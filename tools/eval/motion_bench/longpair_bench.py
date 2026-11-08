# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Long-pair rows of the motion bench (R1-LP): hint benefit and I-to-I span accuracy (numpy only; cv2 is not needed).

  python -I longpair_bench.py prep  --root DIR --scenes pan_subpix,zoom --spans 12,24,48 --seed N
  python -I longpair_bench.py gpu   --root DIR ...     (prints / writes GPU-COMMAND.txt; does not run the GPU)
  python -I longpair_bench.py cpu   --root DIR ...     (hint_only, seeded_zero, seeded_hint)
  python -I longpair_bench.py score --root DIR ...     (longpair-summary.csv / .md, scored twice, identical bytes)
  python -I longpair_bench.py --self-test

DIR is a synth_gt.py output root (frames >= max span + 1). Everything lives under DIR/<scene>/longpair/:
  half/f_%06d.y8 (640x360 luma 8 bit), pairs.csv (span,from,to), hint/h_<from>_<to>.bin, hint_gt_<from>_<to>.npy,
  hint_lab_<from>_<to>.npy, out/<mode>/l_<from>_<to>.bin (+ path.txt for GPU modes).
Cell grid: 4x4 half-luma px per cell, 160x90 cells. Vectors: int16 LE S10.5 (value*32), half-luma px, (x,y) interleaved.
.bin = int16 fwd (gh,gw,2), int16 bwd, uint8 fwd cost (gh,gw), uint8 bwd cost.
"""
import argparse
import csv
import hashlib
import io
import json
import os
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import score_flow  # noqa: E402

CELL = 4
MODES_CPU = ('hint_only', 'seeded_zero', 'seeded_hint')
SIGMA_STEP = 0.15
REGION = 16


# ---------------------------------------------------------------- composition of per-pair ground truth
def sample_bilinear(field, qx, qy):
    """field (H,W,C), points qx,qy (H,W); border-clamped bilinear."""
    h, w = field.shape[:2]
    x0 = np.clip(np.floor(qx).astype(np.int64), 0, w - 1)
    y0 = np.clip(np.floor(qy).astype(np.int64), 0, h - 1)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx = (np.clip(qx, 0, w - 1) - x0)[..., None]
    fy = (np.clip(qy, 0, h - 1) - y0)[..., None]
    top = field[y0, x0] * (1 - fx) + field[y0, x1] * fx
    bot = field[y1, x0] * (1 - fx) + field[y1, x1] * fx
    return top * (1 - fy) + bot * fy


def compose_spans(get_flow, get_occ, get_lab, i, spans):
    """Flow of frame i -> i+k at frame-i pixels for every k in spans (one pass).
    Pair t is frame t-1 -> t, so the span i->i+k composes pairs i+1..i+k:
    C_n(p) = C_{n-1}(p) + V_n(p + C_{n-1}(p)) (the same map as V_n(p)+C_{n-1}(p+V_n(p)) written from the far end).
    A pixel is invalid if any step's occ band is nonzero at the (rounded) point or the point leaves the frame.
    Returns {k: (flow (H,W,2) float64, valid (H,W) bool, label (H,W) uint8 of pair i+1)}."""
    v0 = get_flow(i + 1)
    h, w = v0.shape[:2]
    gy, gx = np.mgrid[0:h, 0:w].astype(np.float64)
    c = np.zeros((h, w, 2))
    valid = np.ones((h, w), bool)
    lab = get_lab(i + 1)
    out = {}
    for n in range(i + 1, i + max(spans) + 1):
        qx, qy = gx + c[..., 0], gy + c[..., 1]
        valid &= (qx >= 0) & (qx <= w - 1) & (qy >= 0) & (qy <= h - 1)
        qxc, qyc = np.clip(qx, 0, w - 1), np.clip(qy, 0, h - 1)
        ox = np.minimum(np.floor(qxc + 0.5).astype(np.int64), w - 1)
        oy = np.minimum(np.floor(qyc + 0.5).astype(np.int64), h - 1)
        valid &= get_occ(n)[oy, ox] == 0
        c = c + sample_bilinear(get_flow(n), qxc, qyc)
        valid &= (gx + c[..., 0] >= 0) & (gx + c[..., 0] <= w - 1) & (gy + c[..., 1] >= 0) & (gy + c[..., 1] <= h - 1)
        k = n - i
        if k in spans:
            out[k] = (c.copy(), valid.copy(), lab)
    return out


def cell_mean(flow, valid, cell):
    """Average a (H,W,2) field over cell x cell px blocks; a block is valid only if all its pixels are."""
    h, w = valid.shape
    gh, gw = h // cell, w // cell
    f = flow[:gh * cell, :gw * cell].reshape(gh, cell, gw, cell, 2)
    v = valid[:gh * cell, :gw * cell].reshape(gh, cell, gw, cell)
    ok = v.all(axis=(1, 3))
    m = f.mean(axis=(1, 3))
    m[~ok] = np.nan
    return m, ok


def cell_label(lab, valid, cell):
    h, w = lab.shape
    gh, gw = h // cell, w // cell
    l = lab[:gh * cell, :gw * cell].reshape(gh, cell, gw, cell)
    v = valid[:gh * cell, :gw * cell].reshape(gh, cell, gw, cell)
    n1 = ((l == 1) & v).sum(axis=(1, 3))
    n2 = ((l == 2) & v).sum(axis=(1, 3))
    return np.where(n2 > n1, 2, np.where(n1 > 0, 1, 0)).astype(np.uint8)


# ---------------------------------------------------------------- fixed point, bins
def to_fixed(v):
    return np.clip(np.floor(np.asarray(v, np.float64) * 32.0 + 0.5), -32767, 32767).astype('<i2')


def write_bin(path, fwd, bwd, cfwd, cbwd):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(to_fixed(fwd).tobytes())
        f.write(to_fixed(bwd).tobytes())
        f.write(np.asarray(cfwd, np.uint8).tobytes())
        f.write(np.asarray(cbwd, np.uint8).tobytes())


def read_bin(path, gh, gw):
    b = open(path, 'rb').read()
    n = gh * gw
    if len(b) != n * 10:
        raise ValueError('%s: %d bytes, expected %d' % (path, len(b), n * 10))
    fwd = np.frombuffer(b, '<i2', n * 2, 0).reshape(gh, gw, 2).astype(np.float64) / 32.0
    bwd = np.frombuffer(b, '<i2', n * 2, n * 4).reshape(gh, gw, 2).astype(np.float64) / 32.0
    cf = np.frombuffer(b, np.uint8, n, n * 8).reshape(gh, gw)
    cb = np.frombuffer(b, np.uint8, n, n * 9).reshape(gh, gw)
    return fwd, bwd, cf, cb


def read_hint(path, gh, gw):
    return np.frombuffer(open(path, 'rb').read(), '<i2').reshape(gh, gw, 2).astype(np.float64) / 32.0


# ---------------------------------------------------------------- seeded block search (mirrors the C++ reference)
def seeded_search(a, b, seed, radius=6, cell=CELL):
    """a, b: uint8 half luma (H,W); seed: (gh,gw,2) half px. Returns vec (gh,gw,2) float64, cost uint8 (gh,gw).
    Window 8x8 centred on the 4x4 cell, seed rounded with floor(x+0.5), +-radius integer offsets, SAD against b
    shifted by seed+offset (edge clamp), first-lowest tie-break (dy outer, dx inner), parabolic sub-pel per axis."""
    h, w = a.shape
    gh, gw = h // cell, w // cell
    m = cell // 2
    win = cell + 2 * m
    wy = np.arange(gh)[:, None] * cell - m + np.arange(win)[None, :]  # (gh,win)
    wx = np.arange(gw)[:, None] * cell - m + np.arange(win)[None, :]  # (gw,win)
    ay = np.clip(wy, 0, h - 1)
    ax = np.clip(wx, 0, w - 1)
    aw = a.astype(np.int32)[ay[:, None, :, None], ax[None, :, None, :]]  # (gh,gw,win,win)
    sx = np.floor(seed[..., 0] + 0.5).astype(np.int64)
    sy = np.floor(seed[..., 1] + 0.5).astype(np.int64)
    bi = b.astype(np.int32)
    n = 2 * radius + 1
    costs = np.empty((n, n, gh, gw), np.int32)
    for oy in range(-radius, radius + 1):
        yy = np.clip(wy[:, None, :] + (sy + oy)[:, :, None], 0, h - 1)  # (gh,gw,win)
        for ox in range(-radius, radius + 1):
            xx = np.clip(wx[None, :, :] + (sx + ox)[:, :, None], 0, w - 1)  # (gh,gw,win)
            bw = bi[yy[:, :, :, None], xx[:, :, None, :]]
            costs[oy + radius, ox + radius] = np.abs(aw - bw).sum(axis=(2, 3))
    best = np.argmin(costs.reshape(n * n, gh, gw), axis=0)  # first lowest in scan order
    by, bx = best // n, best % n
    gyi, gxi = np.mgrid[0:gh, 0:gw]
    c0 = costs[by, bx, gyi, gxi].astype(np.float64)
    offx = np.zeros((gh, gw))
    offy = np.zeros((gh, gw))
    cm = costs[by, np.clip(bx - 1, 0, n - 1), gyi, gxi].astype(np.float64)
    cp = costs[by, np.clip(bx + 1, 0, n - 1), gyi, gxi].astype(np.float64)
    den = cm - 2.0 * c0 + cp
    ok = (bx > 0) & (bx < n - 1) & (den > 1e-6)
    offx[ok] = np.clip((cm - cp)[ok] / (2.0 * den[ok]), -0.5, 0.5)
    cm = costs[np.clip(by - 1, 0, n - 1), bx, gyi, gxi].astype(np.float64)
    cp = costs[np.clip(by + 1, 0, n - 1), bx, gyi, gxi].astype(np.float64)
    den = cm - 2.0 * c0 + cp
    ok = (by > 0) & (by < n - 1) & (den > 1e-6)
    offy[ok] = np.clip((cm - cp)[ok] / (2.0 * den[ok]), -0.5, 0.5)
    vec = np.stack([sx + (bx - radius) + offx, sy + (by - radius) + offy], -1)
    cost = np.minimum(255, np.floor(c0 * 255.0 / (64 * 32) + 0.5)).astype(np.uint8)
    return vec, cost


# ---------------------------------------------------------------- helpers
def scene_dir(root, scene):
    return os.path.join(root, scene, 'longpair')


def read_pairs(root, scene):
    with open(os.path.join(scene_dir(root, scene), 'pairs.csv')) as f:
        return [(int(r['span']), int(r['from']), int(r['to'])) for r in csv.DictReader(f)]


def read_half(root, scene, t, w=640, h=360):
    return np.fromfile(os.path.join(scene_dir(root, scene), 'half', 'f_%06d.y8' % t), np.uint8).reshape(h, w)


def to_half8(y10):
    """10-bit luma (H,W) -> 2x2 box mean (sum+2)>>2 -> >>2 with rounding -> 8 bit."""
    h, w = y10.shape
    s = y10.astype(np.int32).reshape(h // 2, 2, w // 2, 2).sum(axis=(1, 3))
    m10 = (s + 2) >> 2
    return np.clip((m10 + 2) >> 2, 0, 255).astype(np.uint8)


def starts_for(n_frames, k, count=3):
    last = n_frames - 1 - k
    if last < 0:
        return []
    return sorted(set(int(round(x)) for x in np.linspace(0, last, count)))


def drift_field(seed, i, k, gh, gw):
    """Per-16x16-source-px-region random walk, k steps of sigma_step per axis: end point std = sigma_step*sqrt(k),
    constant over the region (2x2 cells), independent per region, deterministic from (seed, from, span)."""
    rng = np.random.default_rng([seed, i, k, 7919])
    r = REGION // 8
    rh, rw = -(-gh // r), -(-gw // r)
    reg = (rng.standard_normal((k, rh, rw, 2)) * SIGMA_STEP).sum(axis=0)
    return np.repeat(np.repeat(reg, r, axis=0), r, axis=1)[:gh, :gw]


# ---------------------------------------------------------------- prep
def cmd_prep(a):
    for scene in a.scenes:
        d = os.path.join(a.root, scene)
        meta = json.load(open(os.path.join(d, 'meta.json')))
        W, H, N = meta['W'], meta['H'], meta['N']
        if N < max(a.spans) + 1:
            print('%s: %d frames, need %d (synth_gt --frames %d)' % (scene, N, max(a.spans) + 1, max(a.spans) + 1), file=sys.stderr)
            return 2
        ld = scene_dir(a.root, scene)
        for sub in ('half', 'hint', 'out'):
            os.makedirs(os.path.join(ld, sub), exist_ok=True)
        fb = W * H * 3  # 16-bit yuv420p frame bytes
        seq = np.memmap(os.path.join(d, 'seq.yuv'), np.uint8, 'r')
        gh, gw = H // 2 // CELL, W // 2 // CELL
        cache, occ_c, lab_c = {}, {}, {}

        def get_flow(t):
            if t not in cache:
                cache[t] = score_flow.read_flo(os.path.join(d, 'gt', 'pair_%06d.flo' % t)).astype(np.float64)
            return cache[t]

        def get_occ(t):
            if t not in occ_c:
                occ_c[t] = np.load(os.path.join(d, 'occ', 'pair_%06d.npy' % t))
            return occ_c[t]

        def get_lab(t):
            if t not in lab_c:
                lab_c[t] = np.load(os.path.join(d, 'lab', 'pair_%06d.npy' % t))
            return lab_c[t]
        rows, need, by_start = [], set(), {}
        for k in a.spans:
            for i in starts_for(N, k):
                rows.append((k, i, i + k))
                need.update((i, i + k))
                by_start.setdefault(i, []).append(k)
        for i, ks in sorted(by_start.items()):
            res = compose_spans(get_flow, get_occ, get_lab, i, set(ks))
            for k in ks:
                flow, valid, lab = res[k]
                g, ok = cell_mean(flow, valid, 8)  # 8x8 source px cell == 4x4 half px cell
                g = g / 2.0
                hint = g + drift_field(a.seed, i, k, gh, gw)
                hint[~ok] = 0.0
                with open(os.path.join(ld, 'hint', 'h_%d_%d.bin' % (i, i + k)), 'wb') as f:
                    f.write(to_fixed(hint).tobytes())
                np.save(os.path.join(ld, 'hint_gt_%d_%d.npy' % (i, i + k)), g.astype(np.float32))
                np.save(os.path.join(ld, 'hint_lab_%d_%d.npy' % (i, i + k)), cell_label(lab, valid, 8))
                print('%s span %d from %d: valid cells %.1f%%' % (scene, k, i, 100.0 * ok.mean()), flush=True)
            for t in [t for t in cache if t <= i + 1]:
                cache.pop(t, None)
        for t in sorted(need):
            y = np.frombuffer(seq[t * fb:t * fb + W * H * 2].tobytes(), '<u2').reshape(H, W)
            to_half8(y).tofile(os.path.join(ld, 'half', 'f_%06d.y8' % t))
        with open(os.path.join(ld, 'pairs.csv'), 'w', newline='') as f:
            f.write('span,from,to\n')
            for k, i, j in rows:
                f.write('%d,%d,%d\n' % (k, i, j))
    return 0


# ---------------------------------------------------------------- gpu (command only)
def cmd_gpu(a):
    root = os.path.abspath(a.root)
    lines = ['# R1-LP GPU stage, run by the lead (PowerShell, repo root, x64 Release built)']
    for scene in a.scenes:
        lines.append('$env:VP_LONGPAIR_BENCH_DIR = "%s"; $env:VP_TEST_GPU = "hardware"; '
                     'vstest.console.exe x64\\Release\\VideoProcessor-Test.dll /Tests:LongPairBenchDump /Logger:trx /ResultsDirectory:%s'
                     % (os.path.join(root, scene, 'longpair'), os.path.join(root, 'trx')))
    lines.append('# expects <scene>\\longpair\\out\\{nohint,hint}\\l_<from>_<to>.bin + path.txt (nvof_hinted / nvof_unhinted / seeded_search)')
    txt = '\n'.join(lines)
    print(txt)
    with open(os.path.join(a.root, 'GPU-COMMAND.txt'), 'w') as f:
        f.write(txt + '\n')
    return 0


# ---------------------------------------------------------------- cpu
def cmd_cpu(a):
    for scene in a.scenes:
        ld = scene_dir(a.root, scene)
        pairs = read_pairs(a.root, scene)
        t0 = time.time()
        for k, i, j in pairs:
            ha, hb = read_half(a.root, scene, i), read_half(a.root, scene, j)
            gh, gw = ha.shape[0] // CELL, ha.shape[1] // CELL
            hint = read_hint(os.path.join(ld, 'hint', 'h_%d_%d.bin' % (i, j)), gh, gw)
            z = np.zeros((gh, gw), np.uint8)
            write_bin(os.path.join(ld, 'out', 'hint_only', 'l_%d_%d.bin' % (i, j)), hint, -hint, z, z)
            for mode, seed_f in (('seeded_zero', np.zeros_like(hint)), ('seeded_hint', hint)):
                vf, cf = seeded_search(ha, hb, seed_f, a.radius)
                vb, cb = seeded_search(hb, ha, -seed_f, a.radius)
                write_bin(os.path.join(ld, 'out', mode, 'l_%d_%d.bin' % (i, j)), vf, vb, cf, cb)
            print('%s span %d from %d done (%.0fs)' % (scene, k, i, time.time() - t0), flush=True)
        for mode, p in (('hint_only', 'hint_only'), ('seeded_zero', 'seeded_search'), ('seeded_hint', 'seeded_search')):
            open(os.path.join(ld, 'out', mode, 'path.txt'), 'w').write(p + '\n')
    return 0


# ---------------------------------------------------------------- score
def pair_stats(fwd, gt, lab):
    """Sums over valid cells (gt not NaN): n, epe half, epe src, outliers (>3 src px), camera/object n and src sums."""
    ok = ~np.isnan(gt[..., 0])
    e = np.sqrt(((fwd - np.nan_to_num(gt)) ** 2).sum(-1))
    es = 2.0 * e
    out = dict(n=int(ok.sum()), eh=float(e[ok].sum()), es=float(es[ok].sum()), out=int((es[ok] > 3.0).sum()))
    for nm, v in (('c', 1), ('o', 2)):
        s = ok & (lab == v)
        out[nm + 'n'] = int(s.sum())
        out[nm + 's'] = float(es[s].sum())
    return out


def add_stats(acc, s):
    for k, v in s.items():
        acc[k] = acc.get(k, 0) + v


def _avg(x, m):
    return '%.4f' % (x / m) if m else 'nan'


def row_of(row, scene, span, mode, s):
    n = s.get('n', 0)
    return [row, scene, str(span), mode, str(n), _avg(s.get('eh', 0), n), _avg(s.get('es', 0), n),
            ('%.3f' % (100.0 * s.get('out', 0) / n)) if n else 'nan', _avg(s.get('cs', 0), s.get('cn', 0)), _avg(s.get('os', 0), s.get('on', 0))]


def _diff(x, y):
    return '%.4f' % (float(x) - float(y)) if 'nan' not in (x, y) else 'nan'


def build_summary(root, scenes):
    cell = {}  # (scene, span, mode) -> sums
    for scene in scenes:
        ld = scene_dir(root, scene)
        try:
            pairs = read_pairs(root, scene)
        except OSError:
            continue
        outd = os.path.join(ld, 'out')
        modes = sorted(m for m in os.listdir(outd) if os.path.isdir(os.path.join(outd, m))) if os.path.isdir(outd) else []
        for k, i, j in pairs:
            gt = np.load(os.path.join(ld, 'hint_gt_%d_%d.npy' % (i, j))).astype(np.float64)
            lab = np.load(os.path.join(ld, 'hint_lab_%d_%d.npy' % (i, j)))
            for m in modes:
                p = os.path.join(outd, m, 'l_%d_%d.bin' % (i, j))
                if os.path.exists(p):
                    add_stats(cell.setdefault((scene, k, m), {}), pair_stats(read_bin(p, gt.shape[0], gt.shape[1])[0], gt, lab))
    pooled = {}
    for (scene, k, m), s in cell.items():
        add_stats(pooled.setdefault(('all', k, m), {}), s)
    rows = [row_of('epe', sc, k, m, s) for (sc, k, m), s in sorted(cell.items())]
    rows += [row_of('span_accuracy', sc, k, m, s) for (sc, k, m), s in sorted(pooled.items())]
    for label, am, bm in (('nohint-hint', 'nohint', 'hint'), ('seeded_zero-seeded_hint', 'seeded_zero', 'seeded_hint')):
        for src in (cell, pooled):
            for (sc, k) in sorted({(sc, k) for (sc, k, m) in src}):
                sa, sb = src.get((sc, k, am)), src.get((sc, k, bm))
                if not sa or not sb or not sa['n'] or not sb['n']:
                    continue
                ra, rb = row_of('x', sc, k, am, sa), row_of('x', sc, k, bm, sb)
                r = row_of('hint_benefit', sc, k, label, sb)
                for c in (5, 6, 7, 8, 9):
                    r[c] = _diff(ra[c], rb[c])
                rows.append(r)
    head = ['row', 'scene', 'span', 'mode', 'n', 'epe_half_px', 'epe_src_px', 'outlier_pct(>3 src px)', 'camera_epe', 'object_epe']
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator='\n')
    w.writerow(head)
    w.writerows(rows)
    md = ['# Long-pair rows (R1-LP)', '',
          'camera_epe / object_epe in source px. hint_benefit rows = first mode minus second (positive = hints help), n = valid cells of the second mode. '
          'span_accuracy = pooled over scenes by valid-cell count.', '',
          '| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    md += ['| ' + ' | '.join(r) + ' |' for r in rows]
    return buf.getvalue(), '\n'.join(md) + '\n'


def cmd_score(a):
    c1, m1 = build_summary(a.root, a.scenes)
    c2, m2 = build_summary(a.root, a.scenes)
    if c1.encode() != c2.encode() or m1.encode() != m2.encode():
        print('NOT DETERMINISTIC: two scoring passes differ', file=sys.stderr)
        return 1
    with open(os.path.join(a.root, 'longpair-summary.csv'), 'w', newline='') as f:
        f.write(c1)
    with open(os.path.join(a.root, 'longpair-summary.md'), 'w') as f:
        f.write(m1)
    print(m1)
    print('run-twice identical, sha1 %s' % hashlib.sha1(c1.encode()).hexdigest()[:12])
    return 0


# ---------------------------------------------------------------- self-test
def _texture(rng):
    """continuous texture f(x,y), so any sub-pixel shift is exact."""
    nf = 80
    fx, fy = rng.uniform(-0.3, 0.3, nf), rng.uniform(-0.3, 0.3, nf)
    ph, amp = rng.uniform(0, 2 * np.pi, nf), rng.uniform(0.5, 1.0, nf)

    def f(x, y):
        s = np.zeros_like(x, dtype=np.float64)
        for a_, u, v, p in zip(amp, fx, fy, ph):
            s += a_ * np.sin(2 * np.pi * (u * x + v * y) + p)
        return np.clip(128 + 128 * s / np.sqrt(nf) * 1.0, 0, 255)
    return f


def self_test():
    t0 = time.time()
    ok = True

    def check(name, cond, info=''):
        nonlocal ok
        print('%-30s %s %s' % (name, 'PASS' if cond else 'FAIL', info))
        ok &= bool(cond)
    rng = np.random.default_rng(5)
    # 1. composition equals the analytic similarity flow
    h, w = 180, 320
    c = np.array([w / 2.0, h / 2.0])
    gy, gx = np.mgrid[0:h, 0:w].astype(np.float64)
    worst, nvalid = 0.0, 0
    lab = np.ones((h, w), np.uint8)
    zero_occ = np.zeros((h, w), np.uint8)
    for s, th, v in ((1.0, 0.0, (3.37, -1.62)), (1.012, 0.0, (1.5, 0.0)), (1.0, 0.6, (0.0, 0.0)), (1.004, -0.3, (0.7, 0.4))):
        r = np.deg2rad(th)
        S = s * np.array([[np.cos(r), -np.sin(r)], [np.sin(r), np.cos(r)]])
        bg = np.stack([(S[0, 0] - 1) * (gx - c[0]) + S[0, 1] * (gy - c[1]) + v[0], S[1, 0] * (gx - c[0]) + (S[1, 1] - 1) * (gy - c[1]) + v[1]], -1)
        res = compose_spans(lambda n: bg, lambda n: zero_occ, lambda n: lab, 0, {12, 24, 48})
        for k, (fl, val, _) in res.items():
            p = np.stack([gx - c[0], gy - c[1]], -1)
            q = p.copy()
            for _ in range(k):
                q = q @ S.T + np.array(v)
            e = np.abs(fl - (q - p))[val]
            worst = max(worst, float(e.max()))
            nvalid += int(val.sum())
    check('composition vs analytic', worst < 0.02 and nvalid > 0, 'max err %.4f px, %d valid px' % (worst, nvalid))
    occ = np.zeros((h, w), np.uint8)
    occ[:, :40] = 255
    res = compose_spans(lambda n: np.zeros((h, w, 2)), lambda n: occ, lambda n: lab, 0, {2})
    check('occ band invalidates', not res[2][1][:, :40].any() and res[2][1][:, 40:].all())
    # 2. seeded search
    hh, ww = 96, 160
    tex = _texture(rng)
    yy, xx = np.mgrid[0:hh, 0:ww].astype(np.float64)
    a8 = np.rint(tex(xx, yy)).astype(np.uint8)
    gh, gw = hh // CELL, ww // CELL
    sl = (slice(5, gh - 5), slice(5, gw - 5))
    dx, dy = 10.0, -7.0  # beyond +-6
    b8 = np.rint(tex(xx - dx, yy - dy)).astype(np.uint8)  # A(x) = B(x + d)
    vz, _ = seeded_search(a8, b8, np.zeros((gh, gw, 2)), 6)
    near = np.zeros((gh, gw, 2))
    near[..., 0], near[..., 1] = dx - 0.6, dy + 0.4
    vh, _ = seeded_search(a8, b8, near, 6)
    ez = np.abs(vz[sl] - np.array([dx, dy])).max(axis=-1)
    eh = np.abs(vh[sl] - np.array([dx, dy])).max(axis=-1)
    check('zero seed fails beyond range', np.median(ez) > 1.0, 'median err %.2f' % np.median(ez))
    check('hint seed recovers', np.median(eh) < 0.15 and (eh < 0.5).mean() > 0.95, 'median err %.3f, %.1f%% < 0.5' % (np.median(eh), 100 * (eh < 0.5).mean()))
    # SAD parabolic fit is biased towards the integer (about 0.11 px at 0.3 on this texture, 0.16 on a 0.3,0.3 diagonal)
    b8 = np.rint(tex(xx - 0.3, yy)).astype(np.uint8)
    vs, _ = seeded_search(a8, b8, np.zeros((gh, gw, 2)), 6)
    es = np.abs(vs[sl][..., 0] - 0.3)
    check('parabolic 0.3 px (x shift)', np.median(es) < 0.15 and np.median(np.abs(vs[sl][..., 1])) < 0.15,
          'median err %.3f (SAD-parabola bias), y %.3f' % (np.median(es), np.median(np.abs(vs[sl][..., 1]))))
    cvec, cc = seeded_search(np.full((16, 16), 9, np.uint8), np.full((16, 16), 9, np.uint8), np.zeros((4, 4, 2)), 2)
    check('first-lowest tie-break', (cvec[1, 1] == [-2.0, -2.0]).all() and cc.max() == 0, str(cvec[1, 1]))
    _, cx = seeded_search(np.full((16, 16), 32, np.uint8), np.zeros((16, 16), np.uint8), np.zeros((4, 4, 2)), 0)
    check('cost u8 scale', int(cx[1, 1]) == 255, str(cx[1, 1]))
    # 3. bin round trip, 4. summary arithmetic
    with tempfile.TemporaryDirectory(prefix='r1cam-longpair-') as td:
        f = np.round(rng.uniform(-40, 40, (4, 5, 2)) * 32) / 32
        cf = rng.integers(0, 256, (4, 5), dtype=np.uint8)
        cb = rng.integers(0, 256, (4, 5), dtype=np.uint8)
        p = os.path.join(td, 'x', 'l.bin')
        write_bin(p, f, -f, cf, cb)
        r = read_bin(p, 4, 5)
        check('bin round trip', np.array_equal(r[0], f) and np.array_equal(r[1], -f) and np.array_equal(r[2], cf) and np.array_equal(r[3], cb)
              and os.path.getsize(p) == 4 * 5 * 10)
        gh2, gw2, sc = 90, 160, 't'
        ld = scene_dir(td, sc)
        os.makedirs(os.path.join(ld, 'out', 'nohint'))
        os.makedirs(os.path.join(ld, 'out', 'hint'))
        gt = np.zeros((gh2, gw2, 2), np.float32)
        gt[..., 0] = 2.0
        gt[:10, :] = np.nan
        lb = np.ones((gh2, gw2), np.uint8)
        lb[:, 80:] = 2
        np.save(os.path.join(ld, 'hint_gt_0_12.npy'), gt)
        np.save(os.path.join(ld, 'hint_lab_0_12.npy'), lb)
        open(os.path.join(ld, 'pairs.csv'), 'w').write('span,from,to\n12,0,12\n')
        z = np.zeros((gh2, gw2), np.uint8)
        g0 = np.nan_to_num(gt.astype(np.float64))
        write_bin(os.path.join(ld, 'out', 'nohint', 'l_0_12.bin'), g0 + np.array([0.5, 0.0]), -g0, z, z)
        write_bin(os.path.join(ld, 'out', 'hint', 'l_0_12.bin'), g0 + np.array([0.0, 0.25]), -g0, z, z)
        csv_txt, _ = build_summary(td, [sc])
        rows = {(r[0], r[1], r[3]): r for r in csv.reader(io.StringIO(csv_txt))}
        n = 80 * 160
        r1, r2, rb = rows[('epe', 't', 'nohint')], rows[('epe', 't', 'hint')], rows[('hint_benefit', 't', 'nohint-hint')]
        good = (r1[4] == str(n) and abs(float(r1[5]) - 0.5) < 1e-4 and abs(float(r1[6]) - 1.0) < 1e-4 and abs(float(r2[6]) - 0.5) < 1e-4
                and abs(float(rb[5]) - 0.25) < 1e-4 and abs(float(rb[6]) - 0.5) < 1e-4 and r1[7] == '0.000'
                and abs(float(r1[8]) - 1.0) < 1e-4 and abs(float(r1[9]) - 1.0) < 1e-4)
        check('summary arithmetic', good, 'nohint %s hint %s benefit %s' % (r1[6], r2[6], rb[6]))
        check('summary run-twice bytes', build_summary(td, [sc])[0] == csv_txt)
    print('self-test %s in %.1fs' % ('OK' if ok else 'FAILED', time.time() - t0))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stage', nargs='?', choices=['prep', 'gpu', 'cpu', 'score'])
    ap.add_argument('--root')
    ap.add_argument('--scenes', default='pan_subpix,zoom,rot,pan_occlusion')
    ap.add_argument('--spans', default='12,24,48')
    ap.add_argument('--seed', type=int, default=20261107)
    ap.add_argument('--radius', type=int, default=6)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.stage or not a.root:
        ap.error('stage and --root required')
    a.scenes = [s for s in a.scenes.split(',') if s]
    a.spans = [int(s) for s in a.spans.split(',') if s]
    if os.path.abspath(a.root).startswith(os.path.abspath(os.path.join(HERE, '..', '..', '..')) + os.sep):
        print('refusing to write inside the repository', file=sys.stderr)
        return 2
    return {'prep': cmd_prep, 'gpu': cmd_gpu, 'cpu': cmd_cpu, 'score': cmd_score}[a.stage](a)


if __name__ == '__main__':
    sys.exit(main())
