"""Upscale study metrics (run with the kit's Python: torch + the kit's own metric functions).

Per output frame, against the reference = the kit's ideal resample (trainkit/common.py resample,
Kaiser-windowed sinc) of the 4K master to the picture size VP used:
  psnr, ssim          luma, the kit's C.psnr / C.ssim
  ring_codes          mean |out - ref| in the near-edge band of the reference (the kit's ring term:
                      C.near_edge_mask, strong edges excluded) - ringing and halos
  overshoot_codes     mean amount by which out leaves the reference's local 3x3 [min, max] in that
                      band - halo overshoot specifically (an over-sharp kernel's signature)
  grain_keep          the kit's C.grain_keep_term ratio: high-band RMS of out / ref on grain tiles
                      (1.0 = grain kept, < 0.95 = grain lost)
  hf_err_codes        mean |highpass(out) - highpass(ref)|
  gpu_ms              PlScaleSweep's pl_timer median (sweep rows only)
Inputs come from study.py (plan.json, the renders); writes <work>/scores.csv. Read-only on the kit.
"""
import argparse
import csv
import glob
import json
import os
import re
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--kit", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--d-staging", default="")
    ap.add_argument("--device", default="cuda")
    a = ap.parse_args()
    sys.path.insert(0, a.kit)
    import torch
    import torch.nn.functional as F
    from trainkit import common as C
    from trainkit import models as Mo
    torch.set_grad_enabled(False)
    dev = a.device if (a.device != "cuda" or torch.cuda.is_available()) else "cpu"
    plan = json.load(open(a.plan))
    masters = {m["id"]: m for m in plan["masters"]}
    cases = {c["id"]: c for c in plan["cases"]}
    T = lambda x: torch.from_numpy(np.ascontiguousarray(x, dtype=np.float32))[None, None].to(dev)

    ref_cache = {}

    def reference(master, seg, frame, ow, oh):
        key = (master, seg, frame, ow, oh)
        if key not in ref_cache:
            w, h = masters[master]["size"]
            p = os.path.join(a.work, "src", "_ref", f"{master}_s{seg}_f{frame:03d}_ref_master.y16")
            m = np.fromfile(p, "<u2", count=w * h).reshape(h, w).astype(np.float32) / 65535.0
            ref_cache.clear()
            ref_cache[key] = C.resample(T(m), oh, ow).clamp(0, 1)
        return ref_cache[key]

    def score(y, ref):
        edge = C.near_edge_mask(ref)
        n = edge.sum().clamp_min(1)
        ring = ((edge * (y - ref).abs()).sum() / n).item() / C.CODE
        pad = F.pad(ref, (1, 1, 1, 1), mode="replicate")
        mx = F.max_pool2d(pad, 3, 1)
        mn = -F.max_pool2d(-pad, 3, 1)
        over = ((edge * (F.relu(y - mx) + F.relu(mn - y))).sum() / n).item() / C.CODE
        _, gk = C.grain_keep_term(y, ref)
        return dict(psnr=C.psnr(y, ref), ssim=C.ssim(y, ref), ring_codes=ring, overshoot_codes=over, grain_keep=float(gk),
                    hf_err_codes=(C.highpass(y) - C.highpass(ref)).abs().mean().item() / C.CODE)

    def vp_rect(job_dir):
        text = open(os.path.join(job_dir, "vp.log"), encoding="utf-8", errors="replace").read()
        m = re.findall(r"VP anamorphic layout:.*? picture=([\d.]+),([\d.]+)-([\d.]+),([\d.]+)", text)
        return [int(round(float(v))) for v in m[-1]] if m else None

    def read_r10(png):
        import cv2
        img = cv2.imread(png, cv2.IMREAD_UNCHANGED)
        r10 = (img[..., 2] >> 6).astype(np.float32)
        return (r10 * 876.0 / 1023.0 + 64.0) / 1023.0   # the kit's fixed limited->full inverse

    netD = None
    if a.d_staging:
        dd = torch.load(os.path.join(a.d_staging, "weights", "D_hybrid_refine.pt"), map_location="cpu", weights_only=False)
        netD = Mo.build(dd["spec"])
        netD.load_state_dict(dd["model"])
        netD.eval().to(dev)
        d_bound = dd["config"]["scale_bound"]

    rows = []
    jobs = {j["id"]: j for j in plan["jobs"]}
    for j in plan["jobs"]:
        if j["window"] != [2560, 1600]:
            continue   # stage-1 renders are inputs, not outputs
        res = os.path.join(j["dir"], "result.txt")
        if not os.path.exists(res):
            continue
        rect = vp_rect(j["dir"])
        if not rect:
            continue
        x0, y0, x1, y1 = rect
        for line in open(res):
            f = line.split()
            if len(f) < 7 or f[2] == "-" or f[0] == "warmup":
                continue
            mm = re.match(r"(\w+?)_s(\d+)_f(\d+)$", f[0])
            if not mm:
                continue
            master, seg, frame = mm.group(1), int(mm.group(2)), int(mm.group(3))
            y = T(read_r10(f[2])[y0:y1, x0:x1])
            ref = reference(master, seg, frame, x1 - x0, y1 - y0)
            base = dict(case=j["case"], variant=j["variant"], master=master, seg=seg, frame=frame, size=f"{x1 - x0}x{y1 - y0}")
            rows.append(dict(base, **score(y.clamp(0, 1), ref)))
            if netD is not None and j["variant"].startswith("route4k_"):
                s1 = jobs.get(j["id"].replace("__s2", "__s1"))
                p1 = os.path.join(a.work, "vp_stage1", j["case"], j["variant"], f[0] + ".y16")
                r1 = vp_rect(s1["dir"]) if s1 else None
                if r1 and os.path.exists(p1):
                    src = np.fromfile(p1, "<u2").reshape(2160, 3840).astype(np.float32) / 1023.0
                    src = T(src[r1[1]:r1[3], r1[0]:r1[2]])
                    out = y.clamp(0, 1)
                    grid = Mo.output_grid(tuple(src.shape[-2:]), tuple(out.shape[-2:]), (0, 0, out.shape[-1], out.shape[-2]), device=dev)
                    corr, _ = netD(out, C.scale_bound(out, d_bound), src, grid)
                    rows.append(dict(base, variant=j["variant"] + "+D", **score((out + corr).clamp(0, 1), ref)))
    for s in plan["sweeps"]:
        folder = os.path.join(a.work, "sweep", s["case"], f"{s['filter']}_ar{s['antiring']:.2f}_{s['target']}")
        for p in sorted(glob.glob(os.path.join(folder, "*.y16"))):
            meta = json.load(open(p + ".json"))
            mm = re.match(r"(\w+?)_s(\d+)_f(\d+)\.y16$", os.path.basename(p))
            master, seg, frame = mm.group(1), int(mm.group(2)), int(mm.group(3))
            w, h = meta["width"], meta["height"]
            y = T(np.fromfile(p, "<u2", count=w * h).reshape(h, w).astype(np.float32) / 65535.0)
            if s["target"] == "route4k":   # the route's stand-in: the kit's ideal downscale to the output picture
                mw, mh = masters[master]["size"]
                ow, oh = fit(mw, mh, 2560, 1600)
                y = C.resample(y, oh, ow)
            ref = reference(master, seg, frame, y.shape[-1], y.shape[-2])
            rows.append(dict(case=s["case"], variant=f"sweep_{s['filter']}_ar{s['antiring']:.2f}_{s['target']}", master=master, seg=seg, frame=frame,
                             size=f"{y.shape[-1]}x{y.shape[-2]}", gpu_ms=meta.get("gpu_ms_median"), **score(y.clamp(0, 1), ref)))
    out = os.path.join(a.work, "scores.csv")
    keys = sorted({k for r in rows for k in r})
    with open(out, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=keys)
        wr.writeheader()
        wr.writerows(rows)
    print(f"wrote {out}: {len(rows)} rows")


def fit(w, h, bw, bh):
    s = min(bw / w, bh / h)
    ow, oh = int(round(w * s / 2)) * 2, int(round(h * s / 2)) * 2
    return min(ow, bw), min(oh, bh)


if __name__ == "__main__":
    main()
