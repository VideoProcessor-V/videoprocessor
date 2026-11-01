# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Consecutive-pair motion fields for the TemporalDetail real-vector test, on a 4 px grid.

Sources (all written in the vector_source.py contract: <dir>/flow_<a>_<b>.npy, float32 (H/4, W/4, 2), luma px,
frame_b(p + F(p)) ~ frame_a(p); pairs (t, t-1) "backward" and (t-1, t) "forward" for t = 1..N-1):
  dis      OpenCV DIS MEDIUM on the 8-bit luma (the earlier stand-in), no refinement
  disref   DIS seed + VP-style block refinement: per 8 px block, quarter-pel search +-1 px around the seed (bicubic
           warp, SSD over a 12 px window), offsets median-smoothed and added to the seed
  nvof     NVIDIA optical flow (VP's engine seed, tools/td-cli/nvof_dump.exe .nvf, slow preset, 4 px grid, quarter-pel)
  nvofref  NVOF seed + the same block refinement (VP: engine seed + matcher refinement)
Run on the DECODED frames these are "engine" vectors; run on the MASTER they are the master-derived (clean-frame)
vectors that are then applied to the decoded frames (the near-ideal ceiling).

  python -I tools/eval/td_vectors/vecgen.py --yuv X.yuv --w 1920 --h 1080 --source disref --out DIR [--nvf X.nvf]
"""
import argparse
import os
import sys
import time

import numpy as np

import cv2

cv2.setNumThreads(1)
G = 4          # stored grid
RB = 8         # refinement block
RWIN = 12      # refinement SSD window
OFFS = [float(v) for v in np.arange(-4, 5) * 0.25]


def read_luma(path, w, h, frames=0, dtype=np.float32):
    fb = w * h * 3 // 2 * 2
    n = os.path.getsize(path) // fb
    if frames:
        n = min(n, frames)
    out = np.empty((n, h, w), dtype)
    with open(path, "rb") as f:
        for i in range(n):
            raw = f.read(fb)
            out[i] = np.frombuffer(raw[:w * h * 2], np.uint16).reshape(h, w)
    return out


def to8(y10):
    return np.clip(np.floor(y10 / 4.0 + 0.5), 0, 255).astype(np.uint8)


def dis(a8, b8):
    d = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    return d.calc(a8, b8, None).astype(np.float32)


def grid_down(F):
    """dense (H, W, 2) -> (ceil(H/G), ceil(W/G), 2) mean per G x G cell."""
    h, w = F.shape[:2]
    gh, gw = -(-h // G), -(-w // G)
    P = np.pad(F, ((0, gh * G - h), (0, gw * G - w), (0, 0)), mode="edge")
    return P.reshape(gh, G, gw, G, 2).mean(axis=(1, 3)).astype(np.float32)


def grid_up(Fg, h, w):
    """grid (gh, gw, 2) at cell centres -> dense (h, w, 2), bilinear."""
    gh, gw = Fg.shape[:2]
    up = cv2.resize(Fg, (gw * G, gh * G), interpolation=cv2.INTER_LINEAR)
    return np.ascontiguousarray(up[:h, :w])


def warp(b, F):
    """b sampled at p + F(p) (bicubic, border replicate)."""
    h, w = b.shape
    xx, yy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(b, xx + F[..., 0], yy + F[..., 1], cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)


def refine(a, b, F):
    """Quarter-pel block refinement of the dense seed F (a's raster, into b) on float planes a, b."""
    h, w = a.shape
    bh, bw = -(-h // RB), -(-w // RB)
    cy = np.minimum(np.arange(bh) * RB + RB // 2, h - 1)
    cx = np.minimum(np.arange(bw) * RB + RB // 2, w - 1)
    best = np.full((bh, bw), np.inf, np.float64)
    bdx = np.zeros((bh, bw), np.float32)
    bdy = np.zeros((bh, bw), np.float32)
    xx, yy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    mx, my = xx + F[..., 0], yy + F[..., 1]
    for dy in OFFS:
        for dx in OFFS:
            wb = cv2.remap(b, mx + dx, my + dy, cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)
            e = cv2.boxFilter((wb - a) ** 2, -1, (RWIN, RWIN), normalize=True, borderType=cv2.BORDER_REPLICATE)
            # small bias toward the seed (ties and flat blocks keep it)
            s = e[np.ix_(cy, cx)].astype(np.float64) * (1.0 + 0.02 * (abs(dx) + abs(dy)))
            m = s < best
            best[m] = s[m]
            bdx[m] = dx
            bdy[m] = dy
    off = np.stack([cv2.medianBlur(bdx, 3), cv2.medianBlur(bdy, 3)], -1)
    up = cv2.resize(off, (bw * RB, bh * RB), interpolation=cv2.INTER_LINEAR)[:h, :w]
    return F + up


def read_nvf(path):
    with open(path, "rb") as f:
        if f.read(4) != b"NVF1":
            raise ValueError("not an .nvf file")
        pairs, w, h, grid, gw, gh = np.frombuffer(f.read(24), np.int32)
        if grid != G:
            raise ValueError("nvf grid %d != %d" % (grid, G))
        n = int(gw) * int(gh)
        out = []
        for _ in range(int(pairs)):
            v = np.frombuffer(f.read(16 * n), np.float32).reshape(4, gh, gw)
            c = np.frombuffer(f.read(2 * n), np.uint8).reshape(2, gh, gw)
            out.append((np.stack([v[0], v[1]], -1), np.stack([v[2], v[3]], -1), c[0], c[1]))
    return int(w), int(h), out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--yuv", required=True)
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--source", required=True, choices=["dis", "disref", "nvof", "nvofref"])
    ap.add_argument("--nvf", default=None)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    Y = read_luma(a.yuv, a.w, a.h, a.frames)
    Y8 = [to8(y) for y in Y]
    nv = None
    if a.source.startswith("nvof"):
        if not a.nvf:
            raise SystemExit("--nvf required for " + a.source)
        nw, nh, nv = read_nvf(a.nvf)
        if (nw, nh) != (a.w, a.h) or len(nv) < len(Y) - 1:
            raise SystemExit("nvf raster/pairs mismatch")
    t0 = time.time()
    for t in range(1, len(Y)):
        for (ia, ib) in ((t, t - 1), (t - 1, t)):
            p = os.path.join(a.out, "flow_%06d_%06d.npy" % (ia, ib))
            if os.path.exists(p):
                continue
            if nv is None:
                F = dis(Y8[ia], Y8[ib])
            else:
                fwd, bwd, _, _ = nv[t - 1]           # forward on t-1's grid (into t), backward on t's grid (into t-1)
                F = grid_up(bwd if ia == t else fwd, a.h, a.w)
            if a.source.endswith("ref"):
                F = refine(Y[ia] / 4.0, Y[ib] / 4.0, F)
            np.save(p, grid_down(F))
        if t % 10 == 0:
            print("  %s t=%d %.0fs" % (a.source, t, time.time() - t0), flush=True)
    print("vecgen %s: %d frames -> %s (%.0f s)" % (a.source, len(Y), a.out, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
