# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Offline no-camera vs camera-guided comparison through tools/camera_eval (see README.md for limits).

  python -I offline_adapter.py --exe camera_eval.exe --yuv CODED.yuv --w W --h H --frames N --vec DIR --ref DIR --fps F
                               [--bitdepth 10] [--start 2] [--count 40] --out-dir DIR
  python -I offline_adapter.py --score-flo --pred-off A.flo --pred-on B.flo --gt G.flo [--camera-labels L] --out-dir DIR
  python -I offline_adapter.py --raft --frame0 a.png --frame1 b.png --out raft.flo     (needs torch, torchvision)

The camera_eval `match` command runs the CPU ReferenceMatcher without and with the camera plan in ONE invocation and
writes a per-pair CSV of block-EPE statistics against the reference flow; it does not write .flo files. This wrapper
runs it and summarises the result. When .flo files exist from elsewhere, --score-flo scores them via score_flow.py.
"""
import argparse
import csv
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run_match(a):
    os.makedirs(a.out_dir, exist_ok=True)
    out_csv = os.path.join(a.out_dir, "match.csv")
    cmd = [a.exe, "match", "--yuv", a.yuv, "--w", str(a.w), "--h", str(a.h), "--bitdepth", str(a.bitdepth),
           "--frames", str(a.frames), "--vec", a.vec, "--ref", a.ref, "--fps", str(a.fps),
           "--start", str(a.start), "--count", str(a.count), "--out", out_csv]
    print(" ".join(cmd))
    subprocess.check_call(cmd)
    return out_csv


def summarise_match(match_csv, out_dir):
    """Mean over pairs of every *_mean column (columns are <run>_<set>_<stat>, run = off/on style prefix)."""
    with open(match_csv, newline="") as f:
        rows = list(csv.DictReader(f))
    sums = {}
    for r in rows:
        for k, v in r.items():
            if k.endswith("_mean"):
                try:
                    sums.setdefault(k, []).append(float(v))
                except ValueError:
                    pass
    path = os.path.join(out_dir, "match_summary.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["column", "pairs", "mean_of_pair_means"])
        for k in sorted(sums):
            w.writerow([k, len(sums[k]), "%.6f" % (sum(sums[k]) / len(sums[k]))])
    print("wrote", path)


def score_flo(a):
    import score_flow
    os.makedirs(a.out_dir, exist_ok=True)
    rc = 0
    for tag, pred in (("off", a.pred_off), ("on", a.pred_on)):
        ns = argparse.Namespace(pred=pred, gt=a.gt, camera_labels=a.camera_labels, object_mask=a.object_mask,
                                prev_pred=None, prev_gt=None, out=os.path.join(a.out_dir, "scores_%s.csv" % tag))
        rc |= score_flow.run(ns)
    return rc


def raft_flow(a):
    try:
        import torch
        from torchvision.models.optical_flow import Raft_Large_Weights, raft_large
        from torchvision.io import read_image
    except ImportError as e:
        print("--raft needs torch/torchvision (use the D:\\hfr\\venv python): %s" % e, file=sys.stderr)
        return 2
    import score_flow
    wts = Raft_Large_Weights.DEFAULT  # weights must already be in the torch cache; nothing is fetched on purpose
    model = raft_large(weights=wts, progress=False).eval()
    f0, f1 = read_image(a.frame0)[:3], read_image(a.frame1)[:3]
    h, w = f0.shape[1] // 8 * 8, f0.shape[2] // 8 * 8
    b0, b1 = wts.transforms()(f0[None, :, :h, :w], f1[None, :, :h, :w])
    with torch.no_grad():
        flow = model(b0, b1)[-1][0].permute(1, 2, 0).numpy()
    score_flow.write_flo(a.out, flow)
    print("wrote", a.out)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe")
    ap.add_argument("--yuv")
    ap.add_argument("--vec")
    ap.add_argument("--ref")
    ap.add_argument("--w", type=int)
    ap.add_argument("--h", type=int)
    ap.add_argument("--frames", type=int)
    ap.add_argument("--fps", type=float)
    ap.add_argument("--bitdepth", type=int, default=10)
    ap.add_argument("--start", type=int, default=2)
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--out-dir")
    ap.add_argument("--score-flo", action="store_true")
    ap.add_argument("--pred-off")
    ap.add_argument("--pred-on")
    ap.add_argument("--gt")
    ap.add_argument("--camera-labels")
    ap.add_argument("--object-mask")
    ap.add_argument("--raft", action="store_true")
    ap.add_argument("--frame0")
    ap.add_argument("--frame1")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    sys.path.insert(0, HERE)
    if a.raft:
        if not (a.frame0 and a.frame1 and a.out):
            ap.error("--raft needs --frame0 --frame1 --out")
        return raft_flow(a)
    if not a.out_dir:
        ap.error("--out-dir required")
    if a.score_flo:
        if not (a.pred_off and a.pred_on and a.gt):
            ap.error("--score-flo needs --pred-off --pred-on --gt")
        return score_flo(a)
    for k in ("exe", "yuv", "vec", "ref", "w", "h", "frames", "fps"):
        if getattr(a, k) in (None, ""):
            ap.error("--%s required for match" % k)
    summarise_match(run_match(a), a.out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
