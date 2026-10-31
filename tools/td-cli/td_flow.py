# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Offline block vectors for td_cli (TemporalDetail CPU model driver).

STAND-IN for VP's vector engine (which needs the GPU): OpenCV DIS optical flow (MEDIUM preset, one thread,
deterministic) on 8-bit luma, reduced to TemporalDetail's MotionPair block grid (TemporalDetailMotion.h):

  Backward(seq): vectors of the pair seq -> seq-1 on frame seq's grid; a vector at p = position in seq-1 minus p
  Forward(seq):  vectors of the pair seq-1 -> seq on frame seq-1's grid

Per block (edge `block` px, grid ceil(W/B) x ceil(H/B), block centres at (b + 0.5) B - 0.5): the vector is the
mean dense flow over the block's pixels; trust = clamp(1 - spread / B, 0, 1) with spread = RMS deviation of the
dense flow from that mean inside the block (px). The model's per-hop uncertainty is (1 - trust) B + |Vf + Vb|, so
it reads exactly "in-block motion spread + forward/backward disagreement" in pixels.

Limits of the stand-in (documented in the results): DIS is a dense variational flow, not VP's block matcher with
its own trust; it is smoother across occlusion boundaries, has no camera model, and its sub-pixel accuracy on
noisy/grainy blocks differs from VP's engine. Results measure the model, conditioned on these vectors.

CLI (writes the .tdv file td_cli --vectors reads):
  python -I tools/td-cli/td_flow.py --in clip.y4m|clip.yuv [--w W --h H --pix-fmt yuv420p10le] --out v.tdv
         [--block 16] [--frames N] [--no-forward]
"""
import argparse
import math
import struct
import sys

import numpy as np

_DIS = None


def _cv2():
    try:
        import cv2
    except ImportError as e:  # visible, never silent
        raise RuntimeError("td_flow needs OpenCV (opencv-python-headless) for the DIS stand-in vectors") from e
    cv2.setNumThreads(1)  # deterministic
    return cv2


def to8(Y, bitdepth):
    """Luma codes at `bitdepth` -> uint8 (rounded)."""
    return np.clip(np.floor(np.asarray(Y, np.float64) / (1 << (bitdepth - 8)) + 0.5), 0, 255).astype(np.uint8)


def dense_flow(a8, b8):
    """Dense flow F on a's raster with b(p + F(p)) ~ a(p)."""
    cv2 = _cv2()
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    f = dis.calc(a8, b8, None)
    return f.astype(np.float64)


def block_reduce(flow, block):
    """Dense (H, W, 2) flow -> (vx, vy, trust) float32 arrays of shape (gh, gw)."""
    h, w = flow.shape[:2]
    gw, gh = int(math.ceil(w / block)), int(math.ceil(h / block))
    vx = np.zeros((gh, gw), np.float64)
    vy = np.zeros((gh, gw), np.float64)
    tr = np.zeros((gh, gw), np.float64)
    for by in range(gh):
        y0, y1 = int(round(by * block)), min(h, int(round((by + 1) * block)))
        for bx in range(gw):
            x0, x1 = int(round(bx * block)), min(w, int(round((bx + 1) * block)))
            fx = flow[y0:y1, x0:x1, 0]
            fy = flow[y0:y1, x0:x1, 1]
            mx, my = float(fx.mean()), float(fy.mean())
            spread = math.sqrt(float(np.mean((fx - mx) ** 2 + (fy - my) ** 2)))
            vx[by, bx], vy[by, bx] = mx, my
            tr[by, bx] = min(1.0, max(0.0, 1.0 - spread / block))
    return vx.astype(np.float32), vy.astype(np.float32), tr.astype(np.float32)


def pair_vectors(prev8, cur8, block=16.0, forward=True):
    """-> (backward (vx, vy, trust) on cur's grid, forward (vx, vy, trust) on prev's grid or None)."""
    b = block_reduce(dense_flow(cur8, prev8), block)
    f = block_reduce(dense_flow(prev8, cur8), block) if forward else None
    return b, f


# ------------------------------------------------------------------ file CLI
def _frames(path, w, h, pix_fmt):
    """Yield (luma uint16/uint8 array, bitdepth) from a planar 4:2:0 .yuv or .y4m file."""
    with open(path, "rb") as f:
        bd = {"yuv420p": 8, "yuv420p10le": 10, "yuv420p12le": 12, "yuv420p16le": 16}.get(pix_fmt)
        y4m = path.lower().endswith(".y4m")
        if y4m:
            hdr = f.readline().decode("ascii").split()
            if hdr[0] != "YUV4MPEG2":
                raise ValueError("not a y4m file")
            cs = "420"
            for t in hdr[1:]:
                if t[0] == "W":
                    w = int(t[1:])
                elif t[0] == "H":
                    h = int(t[1:])
                elif t[0] == "C":
                    cs = t[1:]
            bd = 10 if "p10" in cs else 12 if "p12" in cs else 8
        if bd is None:
            raise ValueError("unsupported pix_fmt " + pix_fmt)
        s = 2 if bd > 8 else 1
        n = w * h * 3 // 2 * s
        while True:
            if y4m:
                tag = f.readline()
                if not tag:
                    return
            raw = f.read(n)
            if len(raw) < n:
                return
            a = np.frombuffer(raw[:w * h * s], np.uint16 if s == 2 else np.uint8).reshape(h, w)
            yield a, bd, w, h


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--w", type=int, default=0)
    ap.add_argument("--h", type=int, default=0)
    ap.add_argument("--pix-fmt", default="yuv420p10le")
    ap.add_argument("--out", required=True)
    ap.add_argument("--block", type=float, default=16.0)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--no-forward", action="store_true")
    a = ap.parse_args(argv)
    recs, prev, geom = [], None, None
    for i, (Y, bd, w, h) in enumerate(_frames(a.inp, a.w, a.h, a.pix_fmt)):
        if a.frames and i >= a.frames:
            break
        cur = to8(Y, bd)
        geom = (w, h)
        if prev is None:
            recs.append((None, None))
        else:
            recs.append(pair_vectors(prev, cur, a.block, not a.no_forward))
        prev = cur
    if geom is None:
        raise SystemExit("no frames read from " + a.inp)
    w, h = geom
    gw, gh = int(math.ceil(w / a.block)), int(math.ceil(h / a.block))
    z = np.zeros((gh, gw), np.float32)
    with open(a.out, "wb") as f:
        f.write(b"TDV1" + struct.pack("<iiidii", len(recs), gw, gh, a.block, w, h))
        for b, fw in recs:
            f.write(struct.pack("<ii", int(b is not None), int(fw is not None)))
            for arr in (b or (z, z, z)) + (fw or (z, z, z)):
                f.write(np.ascontiguousarray(arr, np.float32).tobytes())
    print("td_flow: %d frames, grid %dx%d -> %s" % (len(recs), gw, gh, a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
