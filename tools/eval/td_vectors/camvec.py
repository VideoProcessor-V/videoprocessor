# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Camera-model-refined vectors: VP's global camera model fitter (tools/td-cli/bin/camfit_cli.exe, RANSAC/IRLS global fit
+ per-block labels, src/VP-V/modules/motion/estimation/global_model) run offline on a consecutive-pair field set; on
blocks the fitter labels CAMERA INLIER in pairs whose fit is usable (confidence >= --min-conf), the vector is replaced
by the fitted homography's exact sub-pixel displacement (forward = H p - p on frame t-1's grid, backward = H^-1 p - p
on frame t's grid; picture-centred source px); everywhere else the input field is kept. Writes <vec>/<seq>/<out>/ in
the vector_source.py contract.

  python -I tools/eval/td_vectors/camvec.py --vec VEC --seq boat_master --src disref --out camref --w 1920 --h 1012 --fps 30
"""
import argparse
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAMFIT = os.path.join(os.path.dirname(os.path.dirname(HERE)), "td-cli", "bin", "camfit_cli.exe")
G = 4


def read_camf(path):
    with open(path, "rb") as f:
        if f.read(4) != b"CAMF":
            raise ValueError("not a .camf file")
        pairs, gw, gh = struct.unpack("<iii", f.read(12))
        block = struct.unpack("<d", f.read(8))[0]
        out = []
        for _ in range(pairs):
            valid, mtype = struct.unpack("<ii", f.read(8))
            conf = struct.unpack("<d", f.read(8))[0]
            h = np.array(struct.unpack("<9d", f.read(72))).reshape(3, 3)
            lab = np.frombuffer(f.read(gw * gh), np.uint8).reshape(gh, gw)
            out.append(dict(valid=valid, type=mtype, conf=conf, h=h, label=lab))
    return gw, gh, block, out


def h_field(H, gh4, gw4, w, h):
    ys, xs = np.meshgrid(np.arange(gh4) * G + 1.5 - h / 2.0, np.arange(gw4) * G + 1.5 - w / 2.0, indexing="ij")
    P = np.stack([xs, ys, np.ones_like(xs)], -1) @ H.T
    q = P[..., :2] / P[..., 2:3]
    return np.stack([q[..., 0] - xs, q[..., 1] - ys], -1).astype(np.float32)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--vec", required=True)
    ap.add_argument("--seq", required=True)
    ap.add_argument("--src", default="disref")
    ap.add_argument("--out", default="camref")
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--fps", type=float, required=True)
    ap.add_argument("--frames", type=int, default=72)
    ap.add_argument("--min-conf", type=float, default=0.5)
    a = ap.parse_args(argv)
    sd = os.path.join(a.vec, a.seq, a.src)
    od = os.path.join(a.vec, a.seq, a.out)
    os.makedirs(od, exist_ok=True)
    camf = os.path.join(a.vec, a.seq, a.src + ".camf")
    if not os.path.isfile(camf):
        r = subprocess.run([CAMFIT, "--vec", sd, "--frames", str(a.frames), "--w", str(a.w), "--h", str(a.h), "--block", "16",
                            "--fps", str(a.fps), "--out", camf], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("camfit_cli failed: " + r.stderr[-1000:])
    gw, gh, block, recs = read_camf(camf)
    used = cover = 0.0
    for t in range(1, len(recs) + 1):
        rec = recs[t - 1]
        for (ia, ib, Hm) in ((t - 1, t, rec["h"]), (t, t - 1, None)):
            F = np.load(os.path.join(sd, "flow_%06d_%06d.npy" % (ia, ib)))
            if rec["valid"] and rec["conf"] >= a.min_conf:
                H = rec["h"] if Hm is not None else np.linalg.inv(rec["h"])
                Fh = h_field(H, F.shape[0], F.shape[1], a.w, a.h)
                by = np.minimum(np.arange(F.shape[0]) * G // int(block), gh - 1)
                bx = np.minimum(np.arange(F.shape[1]) * G // int(block), gw - 1)
                m = rec["label"][np.ix_(by, bx)] == 1
                F = np.where(m[..., None], Fh, F)
                if Hm is not None:
                    used += 1
                    cover += float(m.mean())
            np.save(os.path.join(od, "flow_%06d_%06d.npy" % (ia, ib)), F.astype(np.float32))
    print("camvec %s: %d pairs, camera fit used on %d (mean inlier coverage there %.2f) -> %s" % (
        a.seq, len(recs), used, cover / max(1, used), od))
    return 0


if __name__ == "__main__":
    sys.exit(main())
