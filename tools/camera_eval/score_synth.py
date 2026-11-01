# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Score camera_eval fits on the synthetic scenarios of synth_fields.py against their exact camera truth.

Usage (python -I):  python -I score_synth.py SYNTH_ROOT CAMF_DIR   (CAMF_DIR/<scenario>.camf, from `camera_eval fit`)

Per scenario: usable share (valid and confidence >= 0.5), WRONG-confident count (a usable fit on a scene with no
dominant camera motion, or whose max corner error exceeds max(1 px, 10 % of the true corner displacement)),
corner error (median / p90 over all pairs; median / max over usable pairs), label precision / recall of the
camera label (1) against the true camera mask, and jitter (std of the pair-to-pair change of the corner error).
"""
import json
import os
import struct
import sys

import numpy as np

W, H = 1920, 1080


def read_camf(path):
    """-> (gridW, gridH, blockPx, [dict(valid, type, conf, h[3x3], label[gridH, gridW])])"""
    with open(path, 'rb') as f:
        if f.read(4) != b'CAMF':
            raise ValueError('not a .camf: ' + path)
        n, gw, gh = struct.unpack('<iii', f.read(12))
        blk = struct.unpack('<d', f.read(8))[0]
        recs = []
        for _ in range(n):
            v, ty = struct.unpack('<ii', f.read(8))
            conf = struct.unpack('<d', f.read(8))[0]
            h = np.array(struct.unpack('<9d', f.read(72))).reshape(3, 3)
            lab = np.frombuffer(f.read(gw * gh), np.uint8).reshape(gh, gw)
            recs.append(dict(valid=bool(v), type=ty, conf=conf, h=h, label=lab))
    return gw, gh, blk, recs


def corners(Hm, w=W, h=H):
    r = []
    for x, y in [(-w / 2, -h / 2), (w / 2, -h / 2), (-w / 2, h / 2), (w / 2, h / 2)]:
        p = Hm @ [x, y, 1]
        r.append((p[0] / p[2] - x, p[1] / p[2] - y))
    return np.array(r)


def score(scen_dir, camf_path):
    tr = json.load(open(os.path.join(scen_dir, 'truth.json')))
    gw, gh, blk, recs = read_camf(camf_path)
    errs, conf, lp, lr, cerr = [], [], [], [], []
    wrong = usable = 0
    for t, r in enumerate(recs, start=1):
        ct = corners(np.array(tr['H'][t - 1]).reshape(3, 3))
        e = float(np.max(np.hypot(*(corners(r['h']) - ct).T)))
        mag = float(np.max(np.hypot(*ct.T)))
        ok = e <= max(1.0, 0.1 * mag)
        u = r['valid'] and r['conf'] >= 0.5
        usable += u
        conf.append(r['conf'])
        errs.append(e)
        if u:
            cerr.append(e)
            if (not tr['dominant']) or not ok:
                wrong += 1
        tl = np.load(os.path.join(scen_dir, 'truthlab_%06d.npy' % t))[:gh, :gw]
        lab = r['label'] == 1
        if tl.any() and lab.any():
            lp.append((lab & tl).sum() / lab.sum())
            lr.append((lab & tl).sum() / tl.sum())
    return dict(dominant=tr['dominant'], pairs=len(recs), meanconf=float(np.mean(conf)), usable=usable / len(recs), wrong=int(wrong),
                err_med=float(np.median(errs)), err_p90=float(np.percentile(errs, 90)),
                uerr_med=float(np.median(cerr)) if cerr else None, uerr_max=float(np.max(cerr)) if cerr else None,
                lab_prec=float(np.mean(lp)) if lp else None, lab_rec=float(np.mean(lr)) if lr else None,
                jitter=float(np.std(np.diff(errs))) if len(errs) > 2 else 0.0)


def fmt(x, p=2):
    return '-' if x is None else ('%.*f' % (p, x))


def main():
    root, camdir = sys.argv[1], sys.argv[2]
    for n in sorted(os.listdir(root)):
        p = os.path.join(camdir, n + '.camf')
        if not os.path.isfile(p):
            continue
        s = score(os.path.join(root, n), p)
        print('%-13s dom=%d usable=%.2f WRONG=%d err med=%.2f p90=%.2f usable med/max=%s/%s prec=%s rec=%s jitter=%.2f' % (
            n, s['dominant'], s['usable'], s['wrong'], s['err_med'], s['err_p90'], fmt(s['uerr_med']), fmt(s['uerr_max']),
            fmt(s['lab_prec']), fmt(s['lab_rec']), s['jitter']))


if __name__ == '__main__':
    main()
