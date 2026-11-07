# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Score camera_eval fits on real clips against master-derived camera truth.

Usage (python -I):  python -I xcheck_real.py CAMF_DIR [REAL_ROOT]   (REAL_ROOT/data/data.json gives the frame sizes)

CAMF_DIR holds <seq>_<source>.camf (seq e.g. crowd_1080p_low, source disref | nvof) from `camera_eval fit`.
PRIMARY metric (region): truth pairs are those where the master DIS and master NVOF fits are both valid, their common
camera labels cover at least 50 blocks and the p90 block difference between the two fits over those labels is at most
0.5 px; the truth field is the mean of the two fits. A confident fit is WRONG when its p90 block error over the
true-camera blocks exceeds max(1 px, 10 % of the p90 truth magnitude there). The same test over the fit's OWN camera
labels is reported as well. SECONDARY metric ("extrapolated corner error, includes non-camera regions"): the max
picture-corner error, which mostly measures how an affine / homography model extrapolates into non-camera corners.
Corner truth for a clip: the pairs where the DIS and NVOF fits of the MASTER agree (corner difference within
max(0.5 px, 5 % of the corner displacement)); truth = the mean of the two fits. Per candidate: usable share
(valid and confidence >= 0.5), judged = usable pairs with truth, corner error vs truth (median / p90 over judged),
WRONG-confident = judged pairs whose error exceeds max(1 px, 10 % of the truth displacement), and the error at the
master-camera blocks: blocks the master DIS fit labels camera (1), mean |displacement of the candidate model minus
displacement of the truth| there, median over judged pairs.
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score_synth import read_camf  # noqa: E402

DEFAULT_ROOT = 'D:/codex/td-real-vectors-out'
CLIPS = ['boat', 'crowd', 'sol', 'tos']
TIERS = ['master', '1080p', '1080p_low', '1080p_vlow']
SOURCES = ['disref', 'nvof']
# NOISE TIER (R1-GATES): confident-wrong is reported at the 1 px metric AND at this larger error floor. A pair that is wrong only
# between 1 and 1.5 px is a near-threshold (noise-tier) miss, not a foreground lock.
NOISE_TIER_PX = 1.5


def frame_sizes(real_root=DEFAULT_ROOT):
    """clip -> (w, h) from REAL_ROOT/data/data.json (key "cases")."""
    cs = json.load(open(os.path.join(real_root, 'data', 'data.json')))['cases']
    return {c['clip']: (c['w'], c['h']) for c in cs}


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


def evaluate(camdir, real_root=DEFAULT_ROOT, truth_dir=None):
    """-> (rows, truths): one row (dict) per candidate, and per-clip (trusted pairs, pairs).

    truth_dir: where the master fits that define the truth are read (default camdir). Pin it to the default-configuration
    fits when camdir holds another configuration, so the metric is not self-referential."""
    rows, truths = [], {}
    dims = frame_sizes(real_root)
    for clip in CLIPS:
        W, H = dims[clip]
        md, mn = load(truth_dir or camdir, clip + '_master', 'disref'), load(truth_dir or camdir, clip + '_master', 'nvof')
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
        # region truth (primary)
        rtruth = {}
        for t, (a, b) in enumerate(zip(md[3], mn[3])):
            cam = (a['label'] == 1) & (b['label'] == 1)
            if not (a['valid'] and b['valid']) or cam.sum() < 50:
                continue
            fa, fb = disp_at(a['h'], gw, gh, blk, W, H), disp_at(b['h'], gw, gh, blk, W, H)
            if np.percentile(np.hypot(fa[0] - fb[0], fa[1] - fb[1])[cam], 90) > 0.5:
                continue
            rtruth[t] = dict(tx=(fa[0] + fb[0]) / 2, ty=(fa[1] + fb[1]) / 2, cam=cam)
        truths[clip] = (len(truth), len(md[3]), len(rtruth))
        for tier in TIERS:
            for src in SOURCES:
                c = load(camdir, '%s_%s' % (clip, tier), src)
                if c is None:
                    continue
                recs = c[3]
                usable = wrong = 0
                rj = rw_true = rw_own = rw15 = rw15_own = 0
                wrong_pairs = []
                errs, cam_errs = [], []
                for t, x in enumerate(recs):
                    u = x['valid'] and x['conf'] >= 0.5
                    usable += u
                    if u and t in rtruth:
                        rt = rtruth[t]
                        gx, gy = disp_at(x['h'], gw, gh, blk, W, H)
                        e = np.hypot(gx - rt['tx'], gy - rt['ty'])
                        thr = max(1.0, 0.1 * np.percentile(np.hypot(rt['tx'], rt['ty'])[rt['cam']], 90))
                        rj += 1
                        p90 = np.percentile(e[rt['cam']], 90)
                        thr15 = max(NOISE_TIER_PX, 0.1 * np.percentile(np.hypot(rt['tx'], rt['ty'])[rt['cam']], 90))
                        if p90 > thr:
                            rw_true += 1
                            wrong_pairs.append((t, float(p90), float(thr)))
                        if p90 > thr15:
                            rw15 += 1
                        own = x['label'] == 1
                        if own.sum() and np.percentile(e[own], 90) > thr:
                            rw_own += 1
                        if own.sum() and np.percentile(e[own], 90) > thr15:
                            rw15_own += 1
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
                                 cam_err_med=float(np.median(cam_errs)) if cam_errs else None, wrong=wrong,
                                 region_judged=rj, region_wrong=rw_true, region_wrong_own=rw_own,
                                 region_wrong15=rw15, region_wrong15_own=rw15_own, wrong_pairs=wrong_pairs))
    return rows, truths


def fmt(x, p=2):
    return '-' if x is None else ('%.*f' % (p, x))


def main():
    rows, truths = evaluate(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else DEFAULT_ROOT)
    for c, (n, m, k) in truths.items():
        print('%s: region truth on %d/%d pairs, corner truth on %d' % (c, k, m, n))
    for r in rows:
        print('   %-18s %-6s usable=%.2f judged=%d err med=%s p90=%s camblk err med=%s | region judged=%d WRONG@true-camera=%d WRONG@own-labels=%d | >1.5px %d/%d | extrapolated corner WRONG=%d' % (
            r['seq'], r['src'], r['usable'], r['judged'], fmt(r['err_med']), fmt(r['err_p90']), fmt(r['cam_err_med']),
            r['region_judged'], r['region_wrong'], r['region_wrong_own'], r['region_wrong15'], r['region_wrong15_own'], r['wrong']))


if __name__ == '__main__':
    main()
