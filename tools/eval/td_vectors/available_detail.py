# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""How much of a decoded frame's missing detail is available in other decoded frames after motion compensation.

Per case (full-frame 1080p luma, 10-bit codes as float; data.json from prep_data.py) and per vector source (consecutive-
pair fields from vecgen.py, composed hop by hop to the references exactly as TemporalDetail composes them):

  per frame type I / P / B:   PSNR vs master; fine-band energy kept E[H(C)^2]/E[H(M)^2]; fine-band error E[H(M-C)^2]
  references of frame t:      the previous and next anchor (I or P) in display order; for B also the nearest I frame
                              when it is within NEAR_I frames (the best-quality frame of the GOP)
  oracle transfers (per 16 px block, the best reference and the best strength a in [0, 1] chosen WITH the master,
  so they are ceilings, not methods):
    hb      C + a (H(Rw) - H(C))          fine-band (detail) transfer: what TemporalDetail does
    full    C + a (Rw - C)                temporal blend of the whole signal (ceiling of any temporal filter)
    hb_mref C + a (H(Mw) - H(C))          as hb with the MASTER reference warped (a perfect reference frame:
                                          isolates "the detail at t is not in any other frame")
    hb_align  as hb, plus a per-block quarter-pel offset search (+-0.75 px) chosen with the master
              (the vector-error-free ceiling; only with --align)
  decomposition (master only): S = motion-compensated mean of the master over t-2..t+2 (structure estimate);
    incoh = E[H(M - S)^2] / 0.8  (the temporally incoherent fine band: grain/noise + new content; /0.8 corrects the
    5-frame mean for independent noise); struct_err = E[H(S - C)^2] (the decoded frame's error against the structure).

H = x - Gaussian(x, sigma=SIGMA) (fine band). Frames 2 .. N-3 (the decomposition needs +-2). Deterministic.

  python -I tools/eval/td_vectors/available_detail.py --data data.json --case tos_1080p_low --vec VECDIR \
         --sources disref:master,disref:coded --out OUT.json [--align]
A source "X:master" reads <vec>/<clip>_master/X (master-derived vectors applied to the decoded frames), "X:coded"
reads <vec>/<case>/X (vectors from the decoded frames: engine vectors).
"""
import argparse
import json
import os
import sys
import time

import numpy as np

import cv2

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vecgen as vg  # noqa: E402

cv2.setNumThreads(1)
SIGMA = 1.0
BLK = 16
NEAR_I = 12
ALIGN_OFFS = [float(v) for v in np.arange(-3, 4) * 0.25]


def hb(x):
    return x - cv2.GaussianBlur(x, (0, 0), SIGMA, borderType=cv2.BORDER_REPLICATE)


def bsum(x):
    """per-block sums of x (H, W) over BLK x BLK (edge blocks padded with zeros)."""
    h, w = x.shape
    gh, gw = -(-h // BLK), -(-w // BLK)
    P = np.zeros((gh * BLK, gw * BLK), np.float64)
    P[:h, :w] = x
    return P.reshape(gh, BLK, gw, BLK).sum(axis=(1, 3))


def bilinear(F, G):
    """F (H, W, 2) sampled at p + G(p)."""
    h, w = F.shape[:2]
    xx, yy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(F, xx + G[..., 0], yy + G[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


class Flows:
    def __init__(self, d, h, w):
        self.d, self.h, self.w = d, h, w
        self.cache = {}

    def pair(self, a, b):
        k = (a, b)
        if k not in self.cache:
            p = os.path.join(self.d, "flow_%06d_%06d.npy" % (a, b))
            self.cache[k] = vg.grid_up(np.load(p), self.h, self.w)
        return self.cache[k]

    def to(self, t, r):
        """composed field t -> r (frame_r(p + F) ~ frame_t(p))."""
        if t == r:
            return np.zeros((self.h, self.w, 2), np.float32)
        step = -1 if r < t else 1
        F = self.pair(t, t + step).copy()
        cur = t + step
        while cur != r:
            F = F + bilinear(self.pair(cur, cur + step), F)
            cur += step
        return F

    def trim(self, keep):
        for k in [k for k in self.cache if k[0] < keep and k[1] < keep]:
            del self.cache[k]


def oracle(e, ds):
    """e = target error (H, W); ds = candidate directions. Per block: best single d with a in [0, 1].
    Returns (sum of SSE reduction, per-block chosen a mean)."""
    E = bsum(e * e)
    best = np.zeros_like(E)
    amean = np.zeros_like(E)
    for d in ds:
        ed, dd = bsum(e * d), bsum(d * d)
        a = np.clip(ed / np.maximum(dd, 1e-9), 0.0, 1.0)
        red = 2 * a * ed - a * a * dd
        m = red > best
        best[m] = red[m]
        amean[m] = a[m]
    return float(best.sum()), float(amean.mean()), float(E.sum())


def refs_for(types, t):
    n = len(types)
    prev = next((k for k in range(t - 1, -1, -1) if types[k] in "IP"), None)
    nxt = next((k for k in range(t + 1, n) if types[k] in "IP"), None)
    out = [r for r in (prev, nxt) if r is not None]
    if types[t] == "B":
        iw = [k for k in range(n) if types[k] == "I" and abs(k - t) <= NEAR_I and k not in out]
        out += iw[:1]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--vec", required=True)
    ap.add_argument("--sources", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--align", action="store_true")
    ap.add_argument("--frames", type=int, default=0)
    a = ap.parse_args(argv)
    with open(a.data, encoding="utf8") as f:
        case = next(c for c in json.load(f)["cases"] if c["id"] == a.case)
    w, h, types = case["w"], case["h"], case["types"]
    M = vg.read_luma(case["master"], w, h, a.frames)
    C = vg.read_luma(case["coded"], w, h, a.frames)
    n = len(M)
    types = types[:n]
    srcs = []
    for s in a.sources.split(","):
        name, kind = s.split(":")
        d = os.path.join(a.vec, case["clip"] + "_master" if kind == "master" else case["id"], name)
        srcs.append((s, Flows(d, h, w)))
    mflow = next((fl for s, fl in srcs if s.endswith(":master")), srcs[0][1])
    t0 = time.time()
    rows = []
    for t in range(2, n - 2):
        m, c = M[t], C[t]
        Hm, Hc = hb(m), hb(c)
        e_full, e_hb = m - c, Hm - Hc
        N = m.size
        row = dict(t=t, type=types[t], mse=float(np.mean(e_full ** 2)), Em=float(np.mean(Hm ** 2)),
                   Ec=float(np.mean(Hc ** 2)), Eerr=float(np.mean(e_hb ** 2)))
        # decomposition with the master-derived field
        acc = m.astype(np.float64).copy()
        for r in (t - 2, t - 1, t + 1, t + 2):
            acc += vg.warp(M[r], mflow.to(t, r))
        S = (acc / 5.0).astype(np.float32)
        row["incoh"] = float(np.mean(hb(m - S) ** 2) / 0.8)
        row["struct_err"] = float(np.mean(hb(S - c) ** 2))
        refs = refs_for(types, t) if types[t] != "I" else []
        row["refs"] = refs
        for sname, fl in srcs:
            if not refs:
                continue
            Rw, Mw, Hr = [], [], []
            for r in refs:
                F = fl.to(t, r)
                rw = vg.warp(C[r], F)
                Rw.append(rw)
                Hr.append(hb(rw))
                Mw.append(hb(vg.warp(M[r], F)))
                row["%s|warp_psnr_%d" % (sname, r - t)] = float(10 * np.log10(1023.0 ** 2 / max(np.mean((rw - m) ** 2), 1e-9)))
            red, am, _ = oracle(e_hb, [x - Hc for x in Hr])
            row[sname + "|hb"] = red / N
            row[sname + "|hb_a"] = am
            red, _, _ = oracle(e_full, [x - c for x in Rw])
            row[sname + "|full"] = red / N
            red, _, _ = oracle(e_hb, [x - Hc for x in Mw])
            row[sname + "|hb_mref"] = red / N
            if a.align and sname.endswith(":master"):
                ds = []
                for r in refs:
                    F = fl.to(t, r)
                    for dy in ALIGN_OFFS:
                        for dx in ALIGN_OFFS:
                            ds.append(hb(vg.warp(C[r], F + np.array([dx, dy], np.float32))) - Hc)
                red, _, _ = oracle(e_hb, ds)
                row[sname + "|hb_align"] = red / N
        for _, fl in srcs:
            fl.trim(t - 3)
        rows.append(row)
        if t % 10 == 0:
            print("  %s t=%d %.0fs" % (a.case, t, time.time() - t0), flush=True)
    out = dict(case=case, sigma=SIGMA, blk=BLK, sources=[s for s, _ in srcs], rows=rows)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        json.dump(out, f)
    print("available_detail %s -> %s (%.0f s)" % (a.case, a.out, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
