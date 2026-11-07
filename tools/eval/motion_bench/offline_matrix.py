# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Offline motion matrix: NVOF seed / engine matcher / engine + camera plan / RAFT on synthetic and real sequences.
Run with the D:\\hfr\\venv python -I (numpy, cv2, torch). Driven by run_offline_matrix.ps1 (locks, env, order).

  offline_matrix.py prep   --data D --real-root D:\\codex\\td-real-vectors-out\\data --testclips D:\\testclips --ffmpeg ffmpeg.exe
  offline_matrix.py gpu    --data D --nvof-dump nvof_dump.exe [--only NAME ...]      (GPU: NVOF dumps + RAFT; one chunk per call)
  offline_matrix.py cpu    --data D --camera-eval camera_eval.exe [--only NAME ...]  (labels, matcher, scoring, no GPU)
  offline_matrix.py report --data D                                                  (summary.csv / summary.md in D/results)

Sequence registry: D/sequences.json. Every sequence is yuv420p, 16-bit LE samples holding 10-bit values. Pair t is
frame t-1 -> t; judged pairs are START..N-1 (the engine plan needs one earlier pair). Truth: exact synthetic flow, or for
real clips the RAFT forward flow (reference, NOT ground truth). Real-clip camera labels = camera_eval fit of the RAFT
fields (label 1 camera / 2 object); occlusion band = RAFT forward/backward inconsistency, dilated 5x5.
Contenders: nvof_seed (forward vectors of the NVOFA, 4 px cells, repeated to pixels), engine (CPU ReferenceMatcher, plan
off), engine_camera (plan on; plan built from the fit of the NVOF vectors of the previous pair), raft (reference on real).
"""
import argparse
import csv
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import score_flow  # noqa: E402

START = 2
CONTENDERS = ['nvof_seed', 'engine', 'engine_camera', 'raft']
METRICS = ['all', 'camera', 'object', 'occlusion_boundary', 'smallfast', 'subpixel']
REAL = [  # name, kind, frames
    ('real_tos', 'tos'), ('real_cosmos', 'cosmos')]
REAL_FRAMES = 32


def sh(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.check_call([str(c) for c in cmd], **kw)


def load_reg(data):
    with open(os.path.join(data, 'sequences.json')) as f:
        return json.load(f)


def select(reg, only):
    return [s for s in reg if not only or s['name'] in only]


def pdir(d, *parts):
    p = os.path.join(d, *parts)
    os.makedirs(p, exist_ok=True)
    return p


# ---------------------------------------------------------------- prep
def prep(a):
    reg = []
    synth = os.path.join(a.data, 'synth')
    for name in sorted(os.listdir(synth)):
        mp = os.path.join(synth, name, 'meta.json')
        if os.path.isfile(mp):
            m = json.load(open(mp))
            reg.append(dict(name=name, set='synthetic', dir=os.path.join(synth, name), yuv=os.path.join(synth, name, 'seq.yuv'),
                            W=m['W'], H=m['H'], N=m['N'], fps=m['fps'], truth='synth'))
    # real clip 1: Tears of Steel pan, first frames of the existing 1920x856 master (16-bit 4:2:0)
    W, H = 1920, 856
    d = pdir(a.data, 'real', 'real_tos')
    out = os.path.join(d, 'seq.yuv')
    src = os.path.join(a.real_root, 'tos_1080p_master.yuv')
    if not os.path.isfile(out):
        with open(src, 'rb') as f, open(out, 'wb') as g:
            g.write(f.read(W * H * 3 * REAL_FRAMES))
    reg.append(dict(name='real_tos', set='real_tos', dir=d, yuv=out, W=W, H=H, N=REAL_FRAMES, fps=24.0, truth='raft'))
    # real clip 2: Cosmos Laundromat long take, 1920x800, 32 frames from 10 s
    W, H = 1920, 800
    d = pdir(a.data, 'real', 'real_cosmos')
    out = os.path.join(d, 'seq.yuv')
    if not os.path.isfile(out):
        sh([a.ffmpeg, '-v', 'error', '-y', '-ss', '10', '-i', os.path.join(a.testclips, 'variants', 'cosmos_longtake_23976p_44s.mp4'),
            '-frames:v', str(REAL_FRAMES), '-pix_fmt', 'yuv420p10le', '-f', 'rawvideo', out])
    reg.append(dict(name='real_cosmos', set='real_cosmos', dir=d, yuv=out, W=W, H=H, N=REAL_FRAMES, fps=24000 / 1001.0, truth='raft'))
    with open(os.path.join(a.data, 'sequences.json'), 'w') as f:
        json.dump(reg, f, indent=1)
    print('registry: %d sequences' % len(reg))
    return 0


# ---------------------------------------------------------------- io helpers
def read_luma(path, W, H, t):
    fb = W * H * 3
    with open(path, 'rb') as f:
        f.seek(fb * t)
        return np.frombuffer(f.read(W * H * 2), '<u2').reshape(H, W)


def read_nvf(path):
    with open(path, 'rb') as f:
        if f.read(4) != b'NVF1':
            raise ValueError('not an .nvf')
        pairs, w, h, grid, gw, gh = (int(x) for x in np.frombuffer(f.read(24), np.int32))
        n = gw * gh
        out = []
        for _ in range(pairs):
            v = np.frombuffer(f.read(16 * n), np.float32).reshape(4, gh, gw)
            f.read(2 * n)
            out.append((np.stack([v[0], v[1]], -1), np.stack([v[2], v[3]], -1)))
    return w, h, out


def to_grid4(flow):
    h, w = flow.shape[:2]
    gh, gw = h // 4, w // 4
    return flow[:gh * 4, :gw * 4].reshape(gh, 4, gw, 4, 2).mean((1, 3)).astype(np.float32)


def fpath(d, sub, t, ext):
    return os.path.join(d, sub, 'pair_%06d.%s' % (t, ext))


# ---------------------------------------------------------------- gpu stage
def raft_stage(s, want_bwd):
    import torch
    from torchvision.models.optical_flow import Raft_Large_Weights, raft_large
    wts = Raft_Large_Weights.DEFAULT  # cached weights only; this call never downloads when they are present
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    model = raft_large(weights=wts, progress=False).eval().to(dev)
    W, H = s['W'], s['H']
    ph, pw = (-H) % 8, (-W) % 8

    def prep_img(t):
        y = np.clip((read_luma(s['yuv'], W, H, t).astype(np.float32) - 64.0) / 876.0, 0, 1)
        x = torch.from_numpy(y)[None, None].repeat(1, 3, 1, 1)
        x = torch.nn.functional.pad(x, (0, pw, 0, ph), mode='replicate')
        return x.to(dev) * 2 - 1  # the weights' transform maps [0,1] -> [-1,1]

    cache = {}

    def flow(a, b):
        for k in (a, b):
            if k not in cache:
                cache[k] = prep_img(k)
        with torch.no_grad():
            f = model(cache[a], cache[b])[-1][0].permute(1, 2, 0).cpu().numpy()
        return f[:H, :W]

    pdir(s['dir'], 'raft_fwd')
    if want_bwd:
        pdir(s['dir'], 'raft_bwd')
    t0 = time.time()
    for t in range(1, s['N']):
        cache.pop(t - 2, None)
        p = fpath(s['dir'], 'raft_fwd', t, 'flo')
        if not os.path.exists(p):
            score_flow.write_flo(p, flow(t - 1, t))
        if want_bwd:
            p = fpath(s['dir'], 'raft_bwd', t, 'flo')
            if not os.path.exists(p):
                score_flow.write_flo(p, flow(t, t - 1))
    print('  raft %s: %d pairs %.1f s' % (s['name'], s['N'] - 1, time.time() - t0), flush=True)


def gpu(a):
    for s in select(load_reg(a.data), a.only):
        nvf = os.path.join(s['dir'], 'seq.nvf')
        if not os.path.exists(nvf):
            t0 = time.time()
            sh([a.nvof_dump, '--in', s['yuv'], '--w', s['W'], '--h', s['H'], '--pix-fmt', 'yuv420p10le', '--frames', s['N'], '--out', nvf])
            print('  nvof %s %.1f s' % (s['name'], time.time() - t0), flush=True)
        if not a.skip_raft:
            raft_stage(s, want_bwd=s['truth'] == 'raft')
    return 0


# ---------------------------------------------------------------- cpu stage
def expand4(f, H, W):
    return np.repeat(np.repeat(f, 4, 0), 4, 1)[:H, :W]


def raft_aux(s, cam_exe):
    """Real clips: RAFT npy dir (4 px grid), camera labels from camera_eval fit, occlusion band from fwd/bwd inconsistency."""
    import cv2
    d, W, H, N = s['dir'], s['W'], s['H'], s['N']
    vec = pdir(d, 'raft_npy')
    for t in range(1, N):
        for ia, ib, sub in ((t - 1, t, 'raft_fwd'), (t, t - 1, 'raft_bwd')):
            p = os.path.join(vec, 'flow_%06d_%06d.npy' % (ia, ib))
            if not os.path.exists(p):
                np.save(p, to_grid4(score_flow.read_flo(fpath(d, sub, t, 'flo'))))
    camf = os.path.join(d, 'raft.camf')
    if not os.path.exists(camf):
        sh([cam_exe, 'fit', '--vec', vec, '--frames', N, '--w', W, '--h', H, '--block', 16, '--fps', s['fps'], '--out', camf])
    raw = open(camf, 'rb').read()
    pairs, gw, gh = struct.unpack_from('<iii', raw, 4)
    off = 4 + 12 + 8
    rec = 8 + 8 + 72 + gw * gh
    pdir(d, 'lab'), pdir(d, 'occ')
    gy, gx = np.mgrid[0:H, 0:W].astype(np.float32)
    k5 = np.ones((5, 5), np.uint8)
    shares = []
    for t in range(1, N):
        base = off + (t - 1) * rec
        valid = struct.unpack_from('<i', raw, base)[0]
        lab = np.frombuffer(raw, np.uint8, gw * gh, base + 88).reshape(gh, gw)
        full = np.zeros((H, W), np.uint8)
        full[:gh * 16, :gw * 16] = np.repeat(np.repeat(lab, 16, 0), 16, 1)
        np.save(fpath(d, 'lab', t, 'npy'), full)
        shares.append(float((full == 1).mean()))
        f = score_flow.read_flo(fpath(d, 'raft_fwd', t, 'flo')).astype(np.float32)
        b = score_flow.read_flo(fpath(d, 'raft_bwd', t, 'flo')).astype(np.float32)
        bw = cv2.remap(b, gx + f[..., 0], gy + f[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        d2 = ((f + bw) ** 2).sum(-1)
        occ = d2 > 0.01 * ((f ** 2).sum(-1) + (bw ** 2).sum(-1)) + 0.5
        np.save(fpath(d, 'occ', t, 'npy'), (cv2.dilate(occ.astype(np.uint8), k5) * 255).astype(np.uint8))
    print('  %s camera label share mean %.3f' % (s['name'], sum(shares) / len(shares)))


def dump_nvof(s):
    d, W, H, N = s['dir'], s['W'], s['H'], s['N']
    w, h, pr = read_nvf(os.path.join(d, 'seq.nvf'))
    assert (w, h) == (W, H), 'nvf raster'
    vec = pdir(d, 'nvof_npy')
    pdir(d, 'nvof_flo')
    for t in range(1, N):
        fwd, bwd = pr[t - 1]
        np.save(os.path.join(vec, 'flow_%06d_%06d.npy' % (t - 1, t)), np.ascontiguousarray(fwd, np.float32))
        np.save(os.path.join(vec, 'flow_%06d_%06d.npy' % (t, t - 1)), np.ascontiguousarray(bwd, np.float32))
        p = fpath(d, 'nvof_flo', t, 'flo')
        if not os.path.exists(p):
            score_flow.write_flo(p, expand4(fwd, H, W))
    return vec


def run_match(s, cam_exe, vec, outdir):
    d = pdir(s['dir'], outdir)
    csvp = os.path.join(d, 'match.csv')
    sh([cam_exe, 'match', '--yuv', s['yuv'], '--w', s['W'], '--h', s['H'], '--bitdepth', 10, '--frames', s['N'], '--vec', vec,
        '--ref', vec, '--fps', s['fps'], '--start', START, '--count', s['N'], '--dump-flo', d, '--out', csvp])
    return d


def pred_path(s, c, t):
    d = s['dir']
    if c == 'nvof_seed':
        return fpath(d, 'nvof_flo', t, 'flo')
    if c == 'raft':
        return fpath(d, 'raft_fwd', t, 'flo')
    return os.path.join(d, 'match', 'pair_%06d' % t, 'pred_%s.flo' % ('off' if c == 'engine' else 'on'))


def gt_path(s, t):
    return fpath(s['dir'], 'gt', t, 'flo') if s['truth'] == 'synth' else fpath(s['dir'], 'raft_fwd', t, 'flo')


def hash_dir(d):
    h = hashlib.sha256()
    for root, _, files in sorted(os.walk(d)):
        for fn in sorted(files):
            if fn.endswith('.flo'):
                h.update(open(os.path.join(root, fn), 'rb').read())
    return h.hexdigest()


def score_seq(s):
    d = s['dir']
    small = s['truth'] == 'synth' and os.path.isdir(os.path.join(d, 'small')) and bool(os.listdir(os.path.join(d, 'small')))
    contenders = [c for c in CONTENDERS if not (c == 'raft' and s['truth'] == 'raft')]
    for c in contenders:
        sd = pdir(d, 'scores', c)
        for t in range(START, s['N']):
            ns = argparse.Namespace(pred=pred_path(s, c, t), gt=gt_path(s, t), camera_labels=fpath(d, 'lab', t, 'npy'),
                                    object_mask=fpath(d, 'occ', t, 'npy'),
                                    small_mask=fpath(d, 'small', t, 'npy') if small else None,
                                    prev_pred=pred_path(s, c, t - 1) if t > START else None,
                                    prev_gt=gt_path(s, t - 1) if t > START else None, out=os.path.join(sd, 'pair_%06d.csv' % t))
            if score_flow.run(ns) != 0:
                raise SystemExit('score_flow failed for %s %s %d' % (s['name'], c, t))


def cpu(a):
    for s in select(load_reg(a.data), a.only):
        print('== %s' % s['name'], flush=True)
        if s['truth'] == 'raft':
            raft_aux(s, a.camera_eval)
        vec = dump_nvof(s)
        shutil.rmtree(os.path.join(s['dir'], 'match'), ignore_errors=True)
        run_match(s, a.camera_eval, vec, 'match')
        if a.check_determinism and s['name'] == a.determinism_scene:
            run_match(s, a.camera_eval, vec, 'match_rerun')
            h1, h2 = hash_dir(os.path.join(s['dir'], 'match')), hash_dir(os.path.join(s['dir'], 'match_rerun'))
            ok = h1 == h2
            print('determinism %s: matcher outputs run twice %s (sha256 %s)' % (s['name'], 'IDENTICAL' if ok else 'DIFFER', h1))
            with open(os.path.join(a.data, 'determinism.txt'), 'w') as f:
                f.write('%s matcher dump run twice: %s sha256=%s\n' % (s['name'], 'identical' if ok else 'DIFFERENT', h1))
            shutil.rmtree(os.path.join(s['dir'], 'match_rerun'), ignore_errors=True)
            if not ok:
                return 1
        score_seq(s)
        if not a.keep_flo:  # keep match.csv (cost proxy), drop the dense predictions
            for e in os.listdir(os.path.join(s['dir'], 'match')):
                if e.startswith('pair_'):
                    shutil.rmtree(os.path.join(s['dir'], 'match', e), ignore_errors=True)
    return 0


# ---------------------------------------------------------------- report
def read_scores(path):
    return {r['metric']: r['value'] for r in csv.DictReader(open(path))}


def aggregate(seqs, c):
    """n-weighted pooling over pairs and sequences; tc = plain mean over pairs."""
    acc = {}
    tc = {'tc_pred': [], 'tc_excess': []}
    for s in seqs:
        for t in range(START, s['N']):
            p = os.path.join(s['dir'], 'scores', c, 'pair_%06d.csv' % t)
            if not os.path.exists(p):
                continue
            r = read_scores(p)
            for m in METRICS:
                n = int(r.get(m + '_n', 0))
                if n and r[m + '_epe'] != 'nan':
                    a = acc.setdefault(m, [0, 0.0, 0.0])
                    a[0] += n
                    a[1] += n * float(r[m + '_epe'])
                    a[2] += n * float(r[m + '_outlier_pct'])
            for k in tc:
                if k in r:
                    tc[k].append(float(r[k]))
    out = {}
    for m in METRICS:
        if m in acc:
            n, e, o = acc[m]
            out[m + '_epe'], out[m + '_out'], out[m + '_n'] = e / n, o / n, n
    for k, v in tc.items():
        if v:
            out[k] = sum(v) / len(v)
    return out


def cost(seqs):
    tot = dict(pairs=0, evalsFull=0, evalsGuided=0, msFull=0.0, msGuided=0.0, active=0, narrow=0, escalated=0, accepted=0)
    for s in seqs:
        p = os.path.join(s['dir'], 'match', 'match.csv')
        if not os.path.exists(p):
            continue
        for r in csv.DictReader(open(p)):
            tot['pairs'] += 1
            tot['evalsFull'] += int(r['evalsFull'])
            tot['evalsGuided'] += int(r['evalsGuided'])
            tot['msFull'] += float(r['msFull'])
            tot['msGuided'] += float(r['msGuided'])
            tot['active'] += int(r['planActive'])
            tot['narrow'] += int(r['narrowBlocks'])
            tot['escalated'] += int(r['escalated'])
            tot['accepted'] += int(r['narrowAccepted'])
    return tot


def report(a):
    reg = load_reg(a.data)
    sets = {}
    for s in reg:
        sets.setdefault(s['set'], []).append(s)
    groups = [('synthetic (pooled)', sets.get('synthetic', []))] + [(s['name'] + ' (vs RAFT, not GT)', [s]) for s in reg if s['set'] != 'synthetic']
    groups += [('synth: ' + s['name'], [s]) for s in sets.get('synthetic', [])]
    res = pdir(a.data, 'results')
    cols = ['all_epe', 'camera_epe', 'object_epe', 'occlusion_boundary_epe', 'smallfast_epe', 'subpixel_epe', 'tc_pred', 'tc_excess',
            'all_out', 'camera_out', 'object_out']
    rows = []
    for g, seqs in groups:
        for c in CONTENDERS:
            if c == 'raft' and any(s['truth'] == 'raft' for s in seqs):
                continue
            ag = aggregate(seqs, c)
            if ag:
                rows.append([g, c] + [ag.get(k, '') for k in cols])
    with open(os.path.join(res, 'summary.csv'), 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['set', 'contender'] + cols)
        for r in rows:
            w.writerow([('%.4f' % v if isinstance(v, float) else v) for v in r])
    f = lambda v: '' if v == '' else '%.3f' % v
    md = ['# Offline motion matrix (R1-MB)', '', 'EPE in source px (lower is better); out = % of pixels with EPE > 3 px; tc = temporal inconsistency (px).',
          'Truth: exact synthetic flow; real clips use RAFT raft_large as reference (not ground truth), its camera labels come from the camera fitter.',
          'Scenes run 1280x720 (synthetic) / 1920x856 and 1920x800 (real), pairs 2..N-1.', '',
          '| set | contender | EPE all | camera | object | occl. boundary | small fast | sub-pixel | TC | TC excess | out% all | out% cam | out% obj |',
          '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        md.append('| ' + ' | '.join([r[0], r[1]] + [f(v) for v in r[2:]]) + ' |')
    md += ['', '## Category change, engine_camera vs engine (relative EPE; > +2 % is a default-on blocker)', '',
           '| set | all | camera | object | occl. boundary | small fast | sub-pixel |', '|---|---|---|---|---|---|---|']
    for g, seqs in groups:
        e, ec = aggregate(seqs, 'engine'), aggregate(seqs, 'engine_camera')
        if not e or not ec:
            continue
        cells = []
        for m in ('all', 'camera', 'object', 'occlusion_boundary', 'smallfast', 'subpixel'):
            k = m + '_epe'
            cells.append('%+.1f %%' % (100 * (ec[k] / e[k] - 1)) if k in e and k in ec and e[k] > 0 else '')
        md.append('| ' + g + ' | ' + ' | '.join(cells) + ' |')
    md += ['', '## Cost proxy (CPU ReferenceMatcher, wall ms of this machine; guided = plan on, full = plan off)', '',
           '| set | pairs | plan active | evals full | evals guided | guided/full | ms full | ms guided | escalated / narrow blocks |', '|---|---|---|---|---|---|---|---|---|']
    for g, seqs in groups:
        c = cost(seqs)
        if c['pairs']:
            md.append('| %s | %d | %d | %d | %d | %.3f | %.0f | %.0f | %.2f %% |' % (
                g, c['pairs'], c['active'], c['evalsFull'], c['evalsGuided'], c['evalsGuided'] / max(1, c['evalsFull']),
                c['msFull'], c['msGuided'], 100.0 * c['escalated'] / max(1, c['narrow'])))
    det = os.path.join(a.data, 'determinism.txt')
    if os.path.exists(det):
        md += ['', 'Determinism: ' + open(det).read().strip()]
    with open(os.path.join(res, 'summary.md'), 'w') as fh:
        fh.write('\n'.join(md) + '\n')
    print('\n'.join(md))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stage', choices=['prep', 'gpu', 'cpu', 'report'])
    ap.add_argument('--data', required=True)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--real-root', default='D:/codex/td-real-vectors-out/data')
    ap.add_argument('--testclips', default='D:/testclips')
    ap.add_argument('--ffmpeg', default='D:/tools/ffmpeg/bin/ffmpeg.exe')
    ap.add_argument('--nvof-dump')
    ap.add_argument('--camera-eval')
    ap.add_argument('--skip-raft', action='store_true')
    ap.add_argument('--check-determinism', action='store_true')
    ap.add_argument('--determinism-scene', default='pan_small_fast')
    ap.add_argument('--keep-flo', action='store_true')
    a = ap.parse_args(argv)
    return {'prep': prep, 'gpu': gpu, 'cpu': cpu, 'report': report}[a.stage](a)


if __name__ == '__main__':
    sys.exit(main())
