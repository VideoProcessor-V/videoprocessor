# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Score camera_eval fits on real clips against master-derived camera truth.

Usage (python -I):  python -I xcheck_real.py CAMF_DIR

CAMF_DIR holds <seq>_<source>.camf (seq e.g. crowd_1080p_low, source disref | nvof) from `camera_eval fit`.
Truth for a clip: the pairs where the DIS and NVOF fits of the MASTER agree (corner difference within
max(0.5 px, 5 % of the corner displacement)); truth = the mean of the two fits. Per candidate: usable share
(valid and confidence >= 0.5), judged = usable pairs with truth, corner error vs truth (median / p90 over judged),
WRONG-confident = judged pairs whose error exceeds max(1 px, 10 % of the truth displacement), and the error at the
master-camera blocks: blocks the master DIS fit labels camera (1), mean |displacement of the candidate model minus
displacement of the truth| there, median over judged pairs.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score_synth import read_camf  # noqa: E402

DIMS = {'boat': (1920, 1012), 'crowd': (1920, 1080), 'tos': (1920, 856), 'sol': (1920, 1080)}
CLIPS = ['boat', 'crowd', 'sol', 'tos']
TIERS = ['master', '1080p', '1080p_low', '1080p_vlow']
SOURCES = ['disref', 'nvof']


def corners(Hm, W, H):
    r = []
    for x, y in [(-W / 2, -H / 2), (W / 2, -H / 2), (-W / 2, H / 2), (W / 2, H / 2)]:
        p = Hm @ [x, y, 1]
        r.append((p[0] / p[2] - x, p[1] / p[2] - y))
    return np.array(r)


def disp_at(Hm, gw, gh, blk, W, H):
    bx, by = np.meshgrid(np.arange(gw), np.arange(gh))
    x = (bx + 0.5) * blk - W / 2
    y = (by + 0.5) * blk - H / 2
    w = Hm[2, 0] * x + Hm[2, 1] * y + Hm[2, 2]
    return (Hm[0, 0] * x + Hm[0, 1] * y + Hm[0, 2]) / w - x, (Hm[1, 0] * x + Hm[1, 1] * y + Hm[1, 2]) / w - y


def load(camdir, seq, src):
    p = os.path.join(camdir, '%s_%s.camf' % (seq, src))
    return read_camf(p) if os.path.isfile(p) else None


def evaluate(camdir):
    """-> (rows, truths): one row (dict) per candidate, and per-clip (trusted pairs, pairs)."""
    rows, truths = [], {}
    for clip in CLIPS:
        W, H = DIMS[clip]
        md, mn = load(camdir, clip + '_master', 'disref'), load(camdir, clip + '_master', 'nvof')
        if md is None or mn is None:
            continue
        gw, gh, blk = md[0], md[1], md[2]
        truth = {}
        for t, (a, b) in enumerate(zip(md[3], mn[3])):
            ca, cb = corners(a['h'], W, H), corners(b['h'], W, H)
            e = np.max(np.hypot(*(ca - cb).T))
            mag = max(np.max(np.hypot(*ca.T)), 1e-9)
            if a['valid'] and b['valid'] and e <= max(0.5, 0.05 * mag):
                truth[t] = dict(c=(ca + cb) / 2, h=(a['h'], b['h']), cam=(a['label'] == 1))
        truths[clip] = (len(truth), len(md[3]))
        for tier in TIERS:
            for src in SOURCES:
                c = load(camdir, '%s_%s' % (clip, tier), src)
                if c is None:
                    continue
                recs = c[3]
                usable = wrong = 0
                errs, cam_errs = [], []
                for t, x in enumerate(recs):
                    u = x['valid'] and x['conf'] >= 0.5
                    usable += u
                    if not (u and t in truth):
                        continue
                    tt = truth[t]
                    e = float(np.max(np.hypot(*(corners(x['h'], W, H) - tt['c']).T)))
                    mag = float(np.max(np.hypot(*tt['c'].T)))
                    errs.append(e)
                    if e > max(1.0, 0.1 * mag):
                        wrong += 1
                    cx, cy = disp_at(x['h'], gw, gh, blk, W, H)
                    d0 = disp_at(tt['h'][0], gw, gh, blk, W, H)
                    d1 = disp_at(tt['h'][1], gw, gh, blk, W, H)
                    tx, ty = 0.5 * (d0[0] + d1[0]), 0.5 * (d0[1] + d1[1])
                    m = tt['cam']
                    if m.any():
                        cam_errs.append(float(np.mean(np.hypot(cx - tx, cy - ty)[m])))
                rows.append(dict(clip=clip, seq='%s_%s' % (clip, tier), src=src, pairs=len(recs), usable=usable / len(recs), judged=len(errs),
                                 err_med=float(np.median(errs)) if errs else None, err_p90=float(np.percentile(errs, 90)) if errs else None,
                                 cam_err_med=float(np.median(cam_errs)) if cam_errs else None, wrong=wrong))
    return rows, truths


def fmt(x, p=2):
    return '-' if x is None else ('%.*f' % (p, x))


def main():
    rows, truths = evaluate(sys.argv[1])
    for c, (n, m) in truths.items():
        print('%s: trusted master truth on %d/%d pairs' % (c, n, m))
    for r in rows:
        print('   %-18s %-6s usable=%.2f judged=%d err med=%s p90=%s camblk err med=%s WRONG=%d' % (
            r['seq'], r['src'], r['usable'], r['judged'], fmt(r['err_med']), fmt(r['err_p90']), fmt(r['cam_err_med']), r['wrong']))


if __name__ == '__main__':
    main()
