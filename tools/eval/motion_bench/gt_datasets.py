# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Ground-truth optical-flow datasets (MPI-Sintel training, KITTI 2012 / 2015 flow) for the offline motion matrix.

INTERNAL non-commercial evaluation only. The datasets are read from a path argument (never copied into the repository);
the derived luma sequences / predictions live under --data and are never committed. Run with the D:\\hfr\\venv python -I.

  gt_datasets.py prep   --data D --gt-root D:\\testclips\\ground-truth [--sintel-frames 12] [--kitti-pairs 0] [--only-set ...]
  gt_datasets.py report --data D --out R                       (summary.md / summary.csv)
Scoring of one sequence is called from offline_matrix.py cpu (truth == 'gt'): score_seq_gt(s, pred_path, contenders).

Sequences use the offline_matrix registry (D/sequences.json, 16-bit LE 4:2:0 holding 10-bit luma, chroma neutral):
  Sintel: scene = one sequence, first --sintel-frames frames, pairs 2..N-1 judged (the camera plan needs a pair of history).
  KITTI : one sequence per image pair, padded to 3 frames [a0, a0, a1]; judged pair 2 = a0 -> a1. A two-frame pair has no
          real history, so engine_camera is not scored on KITTI (the plan has nothing to be built from).
Frames are cropped (top-left) to a multiple of 8 so the 4 px NVOF cells and the 16 px camera blocks tile the raster.
Metrics (pixel-pooled): EPE all (valid GT), noc (valid and not occluded), s0-10 / s10-40 / s40+ (valid GT, by |gt|),
KITTI Fl-all (EPE > 3 px and > 5 % of |gt|, valid GT incl. occluded). Sintel valid = not in the 'invalid' mask.
"""
import argparse
import csv
import glob
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import score_flow  # noqa: E402

CATS = ['all', 'noc', 's0_10', 's10_40', 's40p']
CAT_LABEL = {'all': 'EPE all', 'noc': 'EPE noc', 's0_10': 's0-10', 's10_40': 's10-40', 's40p': 's40+'}
CONTENDERS = ['nvof_seed', 'engine', 'engine_camera', 'raft']


def _cv2():
    import cv2
    return cv2


def luma10(img):
    """uint8 gray (H,W) or BGR (H,W,3) -> uint16 limited-range 10-bit luma (BT.709 weights on the coded values)."""
    if img.ndim == 3:
        b, g, r = (img[..., i].astype(np.float32) for i in (0, 1, 2))
        y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    else:
        y = img.astype(np.float32)
    return np.rint(64.0 + 876.0 * y / 255.0).astype(np.uint16)


def write_yuv(path, lumas):
    H, W = lumas[0].shape
    chroma = np.full((H // 2, W // 2), 512, '<u2').tobytes()
    with open(path, 'wb') as f:
        for y in lumas:
            f.write(np.ascontiguousarray(y, '<u2').tobytes())
            f.write(chroma)
            f.write(chroma)


def crop8(*arrs):
    H, W = arrs[0].shape[:2]
    h, w = H // 8 * 8, W // 8 * 8
    return [a[:h, :w] for a in arrs]


def read_png(path, flags=-1):
    img = _cv2().imread(path, flags)
    if img is None:
        raise IOError('cannot read ' + path)
    return img


def kitti_flow(path):
    """KITTI 16-bit png -> flow (H,W,2) float32, valid (H,W) bool."""
    im = read_png(path, -1)
    u = (im[..., 2].astype(np.float32) - 32768.0) / 64.0
    v = (im[..., 1].astype(np.float32) - 32768.0) / 64.0
    return np.stack([u, v], -1), im[..., 0] > 0


def save_pair_gt(d, t, flow, valid, noc):
    """gt/pair_%06d.flo (invalid -> 1e10 'unknown'), msk/pair_%06d.npz (valid, noc)."""
    os.makedirs(os.path.join(d, 'gt'), exist_ok=True)
    os.makedirs(os.path.join(d, 'msk'), exist_ok=True)
    g = flow.astype(np.float32).copy()
    g[~valid] = 1e10
    score_flow.write_flo(os.path.join(d, 'gt', 'pair_%06d.flo' % t), g)
    np.savez_compressed(os.path.join(d, 'msk', 'pair_%06d.npz' % t), valid=valid, noc=noc)


def prep_sintel(a, reg):
    root = os.path.join(a.gt_root, 'sintel')
    train = os.path.join(root, 'training')
    if not os.path.isdir(train):
        hits = glob.glob(os.path.join(root, '*', 'training'))
        train = hits[0] if hits else train
    scenes = sorted(os.listdir(os.path.join(train, 'flow')))
    for pas in ('clean', 'final'):
        if a.only_set and ('sintel_' + pas) not in a.only_set:
            continue
        for sc in scenes:
            name = 'sintel_%s_%s' % (pas, sc)
            d = os.path.join(a.data, 'seqs', name)
            os.makedirs(d, exist_ok=True)
            n_all = len(glob.glob(os.path.join(train, pas, sc, 'frame_*.png')))
            N = min(a.sintel_frames, n_all)
            lumas = []
            for k in range(1, N + 1):
                lumas.append(crop8(luma10(read_png(os.path.join(train, pas, sc, 'frame_%04d.png' % k), 1)))[0])
            for t in range(1, N):  # pair t = frame t-1 -> t (0-based) = flow/frame_{t:04d}.flo
                f = score_flow.read_flo(os.path.join(train, 'flow', sc, 'frame_%04d.flo' % t)).astype(np.float32)
                occ = read_png(os.path.join(train, 'occlusions', sc, 'frame_%04d.png' % t), 0) > 0
                inv = read_png(os.path.join(train, 'invalid', sc, 'frame_%04d.png' % t), 0) > 0
                f, occ, inv = crop8(f, occ, inv)
                save_pair_gt(d, t, f, ~inv, ~inv & ~occ)
            H, W = lumas[0].shape
            write_yuv(os.path.join(d, 'seq.yuv'), lumas)
            reg.append(dict(name=name, set='sintel_' + pas, dir=d, yuv=os.path.join(d, 'seq.yuv'), W=W, H=H, N=N, fps=24.0, truth='gt'))
            print('sintel', name, N, 'frames', W, 'x', H, flush=True)


def prep_kitti(a, reg):
    for key, imgdir in (('kitti2015', 'image_2'), ('kitti2012', 'image_0')):
        if a.only_set and key not in a.only_set:
            continue
        base = os.path.join(a.gt_root, key, 'training')
        if not os.path.isdir(base):
            hits = glob.glob(os.path.join(a.gt_root, key, '*', 'training'))
            if not hits:
                print('skip', key, '(not extracted)')
                continue
            base = hits[0]
        ids = sorted(os.path.basename(p)[:-7] for p in glob.glob(os.path.join(base, imgdir, '*_10.png')))
        if a.kitti_pairs:
            ids = ids[:a.kitti_pairs]
        for sid in ids:
            name = '%s_%s' % (key, sid)
            d = os.path.join(a.data, 'seqs', name)
            os.makedirs(d, exist_ok=True)
            f0 = luma10(read_png(os.path.join(base, imgdir, sid + '_10.png'), 1))
            f1 = luma10(read_png(os.path.join(base, imgdir, sid + '_11.png'), 1))
            fo, vo = kitti_flow(os.path.join(base, 'flow_occ', sid + '_10.png'))
            fn, vn = kitti_flow(os.path.join(base, 'flow_noc', sid + '_10.png'))
            h = min(f0.shape[0], f1.shape[0], fo.shape[0]) // 8 * 8
            w = min(f0.shape[1], f1.shape[1], fo.shape[1]) // 8 * 8
            f0, f1, fo, vo, vn = f0[:h, :w], f1[:h, :w], fo[:h, :w], vo[:h, :w], vn[:h, :w]
            save_pair_gt(d, 2, fo, vo, vo & vn)
            write_yuv(os.path.join(d, 'seq.yuv'), [f0, f0, f1])
            reg.append(dict(name=name, set=key, dir=d, yuv=os.path.join(d, 'seq.yuv'), W=w, H=h, N=3, fps=24.0, truth='gt'))
        print(key, len(ids), 'pairs', flush=True)


def prep(a):
    os.makedirs(a.data, exist_ok=True)
    reg = []
    prep_sintel(a, reg)
    prep_kitti(a, reg)
    with open(os.path.join(a.data, 'sequences.json'), 'w') as f:
        json.dump(reg, f, indent=1)
    print('registry: %d sequences' % len(reg))
    return 0


# ---------------------------------------------------------------- scoring
def pair_stats(pred, gt, valid, noc, with_fl):
    """-> {cat: [n, sum_epe, n_fl]}."""
    gt = np.where(valid[..., None], gt, 0.0)
    epe = np.sqrt(((pred.astype(np.float64) - gt) ** 2).sum(-1))
    mag = np.sqrt((gt.astype(np.float64) ** 2).sum(-1))
    fl = (epe > 3.0) & (epe > 0.05 * mag)
    sel = {'all': valid, 'noc': noc, 's0_10': valid & (mag < 10), 's10_40': valid & (mag >= 10) & (mag < 40), 's40p': valid & (mag >= 40)}
    out = {}
    for c, m in sel.items():
        out[c] = [int(m.sum()), float(epe[m].sum()), int(fl[m].sum()) if with_fl else 0]
    return out


def score_seq_gt(s, pred_path, contenders, start=2):
    d = s['dir']
    kitti = s['set'].startswith('kitti')
    for c in contenders:
        if kitti and c == 'engine_camera':
            continue
        sd = os.path.join(d, 'scores', c)
        os.makedirs(sd, exist_ok=True)
        for t in range(start, s['N']):
            gt = score_flow.read_flo(os.path.join(d, 'gt', 'pair_%06d.flo' % t)).astype(np.float32)
            m = np.load(os.path.join(d, 'msk', 'pair_%06d.npz' % t))
            gt = np.where(gt > 1e9, 0.0, gt)
            pr = score_flow.read_flo(pred_path(s, c, t)).astype(np.float32)
            with open(os.path.join(sd, 'pair_%06d.json' % t), 'w') as f:
                json.dump(pair_stats(pr, gt, m['valid'], m['noc'], kitti), f)


# ---------------------------------------------------------------- report
def aggregate(seqs, c):
    acc = {k: [0, 0.0, 0] for k in CATS}
    npairs = 0
    for s in seqs:
        for p in glob.glob(os.path.join(s['dir'], 'scores', c, 'pair_*.json')):
            st = json.load(open(p))
            npairs += 1
            for k in CATS:
                for i in range(3):
                    acc[k][i] += st[k][i]
    return acc, npairs


def report(a):
    reg = json.load(open(os.path.join(a.data, 'sequences.json')))
    sets = {}
    for s in reg:
        sets.setdefault(s['set'], []).append(s)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for g in sorted(sets):
        for c in CONTENDERS:
            acc, n = aggregate(sets[g], c)
            if not n or not acc['all'][0]:
                continue
            r = [g, c, n]
            for k in CATS:
                r.append(acc[k][1] / acc[k][0] if acc[k][0] else float('nan'))
            r.append(100.0 * acc['all'][2] / acc['all'][0] if g.startswith('kitti') else float('nan'))
            rows.append(r)
    hdr = ['dataset', 'method', 'pairs'] + [CAT_LABEL[k] for k in CATS] + ['Fl-all %']
    with open(os.path.join(a.out, 'summary.csv'), 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(hdr)
        for r in rows:
            w.writerow([('%.4f' % v if isinstance(v, float) else v) for v in r])
    fm = lambda v: 'n/a' if isinstance(v, float) and v != v else ('%.3f' % v if isinstance(v, float) else str(v))
    md = ['# Ground-truth motion matrix (internal, non-commercial evaluation only)', '',
          'EPE in px (lower is better), pixel-pooled over judged pairs. noc = non-occluded; s0-10 / s10-40 / s40+ = by GT magnitude (valid GT).',
          'Fl-all = % of valid pixels with EPE > 3 px and > 5 % of |gt| (KITTI). Methods: nvof_seed = NVOFA forward vectors (4 px cells); '
          'engine = CPU ReferenceMatcher, plan off; engine_camera = plan on (needs one earlier pair, so n/a on two-frame KITTI pairs); '
          'raft = torchvision raft_large (luma only). Sintel: first N frames per scene, cropped to a multiple of 8.', '',
          '| ' + ' | '.join(hdr) + ' |', '|' + '---|' * len(hdr)]
    for r in rows:
        md.append('| ' + ' | '.join(fm(v) for v in r) + ' |')
    with open(os.path.join(a.out, 'summary.md'), 'w') as fh:
        fh.write('\n'.join(md) + '\n')
    print('\n'.join(md))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stage', choices=['prep', 'report'])
    ap.add_argument('--data', required=True)
    ap.add_argument('--gt-root', default='D:/testclips/ground-truth')
    ap.add_argument('--sintel-frames', type=int, default=12)
    ap.add_argument('--kitti-pairs', type=int, default=0, help='0 = all training pairs')
    ap.add_argument('--only-set', nargs='*', help='sintel_clean sintel_final kitti2015 kitti2012')
    ap.add_argument('--out', help='report output dir')
    a = ap.parse_args(argv)
    return {'prep': prep, 'report': report}[a.stage](a)


if __name__ == '__main__':
    sys.exit(main())
