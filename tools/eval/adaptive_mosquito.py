# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 last chance for the ringing / mosquito repair: an ADAPTIVE, grain-safe, region-wise version
(owner rule: every repair is dynamic and regionally adaptive), standalone and in the HDR->SDR stack.

  python -I tools/eval/adaptive_mosquito.py --hr-scratch <artefact_headroom scratch> \
         --mq-scratch <mosquito_headroom scratch> --scratch <new scratch dir> --out tools/eval/prototypes/adaptive_mosquito

Repair (AdaptiveMosquito.apply, 10-bit codes, deterministic):
  grain split  S = Y - G, G = soft-limited 1-px residual (|G| <= 2.5 sigma_n, tonemap_ref.grain_split rule)
  zones        strong edges of gauss(S, 1) (Sobel/8 > 4 % of peak), band 1..r px around them
               (r = lines / 128, >= 4), edge cores untouched
  evidence     ring = 5x5 mean of hp(S)^2 in the band / floor^2, floor = max(sigma_n, 0.5 code): the ringing
               energy measured near THIS edge against the local noise floor
  strength     w = clip((ring - 2) / 6, 0, 1)                     zero where there is no ringing
                   x clip(edge contrast / (8 % of peak), 0, 1)    (local max-min of S over 2r)
                   x visibility(brightness)                      post-TM visibility weight: relative
                                                                 slope of the delivered curve at the pixel
                                                                 level (1 for SDR / identity)
                   x class factor                                1.0 low-rate class, 0.5 high-rate
  filter       bilateral of S (radius 2, sigma_s 1.5, sigma_r = max(3 floor, 1.5 rms(hp S) locally)), applied
               as Y + w (S_f - S) in the band only: the grain G passes untouched; BTB / WTW pixels untouched
Tests (6 frames per case):
  standalone SDR, low rate: tos4k text-like window and Sintel sky at 1080p, x264 CRF 28 and CRF 35
  standalone HDR-PQ and stack: Sol dark / Nocturne low-key x265-10 CRF 30; placements early (PQ, before the
  curve) / late (on the SDR output, visibility weight 1) / both, tone stage plain and grain-safe CR 0.30
Metrics: ring (near-edge smooth-band ringing energy vs the reference in the same domain, ratio to no
  repair), psnr delta, zone share, mean w, grain gate = two-seed probe through the repair, near-edge and
  flat keep (eval_metrics.grain_probe; +-5 %), lag-1, lc (post-TM), py_ms.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import artefact_headroom as ah  # noqa: E402
import candidates_mosquito as cm  # noqa: E402
import deband_stack as ds  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402
import ringing_stack as rs  # noqa: E402
import tonemap_ref as tr  # noqa: E402

NFR = 6


class AdaptiveMosquito:
    def __init__(self, low_rate=True, visibility=None):
        self.cls = 1.0 if low_rate else 0.5
        self.visibility = visibility  # callable(Y codes) -> weight in [0, 1], or None
        self.stats = []

    def apply(self, Y):
        pk = 1023.0
        e = Y / 1023.0
        S1, G1 = tr.grain_split(e)
        S, G = S1 * 1023.0, G1 * 1023.0
        sig = tr.noise_sigma_pq(e) * 1023.0
        floor = max(sig, 0.5)
        lp = ndimage.gaussian_filter(S, 1.0, mode="nearest")
        g = np.hypot(ndimage.sobel(lp, 0, mode="nearest"), ndimage.sobel(lp, 1, mode="nearest")) / 8.0
        edges = g > 0.04 * pk
        r = max(4, int(round(min(Y.shape) / 128.0)))
        if not edges.any():
            self.stats.append(dict(zone=0.0, w=0.0))
            return Y.copy()
        core = ndimage.binary_dilation(edges, iterations=1)
        band = ndimage.binary_dilation(edges, iterations=r) & ~core
        hp = S - ndimage.gaussian_filter(S, 1.0, mode="nearest")
        ring = ndimage.uniform_filter(hp * hp, size=5, mode="nearest") / (floor * floor)
        contrast = (ndimage.maximum_filter(S, size=2 * r + 1, mode="nearest")
                    - ndimage.minimum_filter(S, size=2 * r + 1, mode="nearest"))
        w = np.clip((ring - 2.0) / 6.0, 0.0, 1.0) * np.clip(contrast / (0.08 * pk), 0.0, 1.0) * self.cls
        if self.visibility is not None:
            w = w * self.visibility(Y)
        w = np.where(band, w, 0.0)
        w = np.where((Y < 64.0) | (Y > 940.0), 0.0, w)
        if not (w > 0).any():
            self.stats.append(dict(zone=float(band.mean()), w=0.0))
            return Y.copy()
        loc_rms = np.sqrt(ndimage.uniform_filter(hp * hp, size=5, mode="nearest"))
        sr = float(max(3.0 * floor, 1.5 * np.median(loc_rms[w > 0])))
        Sf = cm.bilateral(S, 1.5, sr, 2)
        self.stats.append(dict(zone=float(band.mean()), w=float(w[band].mean())))
        return Y + w * (Sf - S)


def apply_seq(rep, seq):
    t0 = time.perf_counter()
    out = np.stack([rep.apply(f) for f in seq])
    return out, (time.perf_counter() - t0) * 1000.0 / len(seq)


def grain_gate(make, X, M):
    """Two-seed probe through the repair on input X; masks from the reference M."""
    gA = np.stack([em.probe_grain(X.shape[1:], 101, t, 10) for t in range(len(X))])
    gB = np.stack([em.probe_grain(X.shape[1:], 202, t, 10) for t in range(len(X))])
    oA, _ = apply_seq(make(), X + gA)
    oB, _ = apply_seq(make(), X + gB)
    zones = [em.near_edge_zone(m, 10) for m in M]
    ge = em.grain_probe(gA - gB, oA - oB, [z[0] for z in zones])
    gf = em.grain_probe(gA - gB, oA - oB, [z[1] for z in zones])
    return (ge or {}).get("keep"), (gf or {}).get("keep"), (ge or {}).get("lag1")


def ensure_x264(scratch, name, master8, crf, fps=24.0):
    dec = os.path.join(scratch, "%s_x264_crf%d.yuv" % (name, crf))
    if not os.path.exists(dec):
        enc = dec[:-4] + ".mkv"
        ah.run([ah.FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s",
                "%dx%d" % (ah.W, ah.H), "-r", "%g" % fps, "-i", master8] + ah.X264 + ["-crf", str(crf), "-pix_fmt",
                                                                                     "yuv420p", enc])
        ah.run([ah.FF, "-hide_banner", "-loglevel", "error", "-y", "-i", enc, "-pix_fmt", "yuv420p10le", "-f", "rawvideo",
                dec])
    return dec


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--mq-scratch", required=True)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    # ---- standalone SDR, low rate
    sdr = [("tos4k_text", os.path.join(a.mq_scratch, "tos4k_1080p_yuv420p.yuv")),
           ("sintel_sky", os.path.join(a.hr_scratch, "sintel_sky_1080p_yuv420p.yuv"))]
    for name, m8 in sdr:
        M = ah.luma_seq(m8, "yuv420p", NFR)
        for crf in (28, 35):
            D = ah.luma_seq(ensure_x264(a.scratch, name, m8, crf), "yuv420p10le", NFR)
            for label, make in (("adaptive", lambda: AdaptiveMosquito(True)),):
                X, ms = apply_seq(make(), D)
                ke, kf, lag = grain_gate(make, D, M)
                r0, r1 = rs.ring_energy(M, D), rs.ring_energy(M, X)
                rows.append(dict(context="SDR standalone", case="%s x264 crf%d" % (name, crf), placement="-",
                                 stage="-", ring_ratio=r1 / r0 if r0 else None, ring_abs=r0,
                                 dpsnr=em.psnr(M, X, 10) - em.psnr(M, D, 10), keep_edge=ke, keep_flat=kf, lag1=lag,
                                 lc_delta=None, py_ms=ms))
                print(json.dumps(rows[-1]), flush=True)
    # ---- HDR standalone (PQ) and stack (post-TM)
    for name, peak in (("sol_dark", 1000.0), ("nocturne_mid", 4000.0)):
        M = ah.luma_seq(os.path.join(a.hr_scratch, "%s_1080p_yuv420p10le.yuv" % name), "yuv420p10le", NFR)
        for enc in ("x265-10_crf30", "aom-10_crf40"):
            D = ah.luma_seq(os.path.join(a.hr_scratch, "%s_1080p_%s.yuv" % (name, enc)), "yuv420p10le", NFR)
            tmv = tr.SplineTM(peak)
            s0 = float(tmv.slope(tmv.sp))
            vis = lambda Y, tmv=tmv, s0=s0: np.clip(tmv.slope(tr.code_to_norm(Y)) / s0, 0.0, 1.0)
            make = lambda vis=vis: AdaptiveMosquito(True, vis)
            E, ms = apply_seq(make(), D)
            ke, kf, lag = grain_gate(make, D, M)
            r0, r1 = rs.ring_energy(M, D), rs.ring_energy(M, E)
            rows.append(dict(context="HDR-PQ standalone", case="%s %s" % (name, enc), placement="early", stage="-",
                             ring_ratio=r1 / r0 if r0 else None, ring_abs=r0,
                             dpsnr=em.psnr(M, E, 10) - em.psnr(M, D, 10), keep_edge=ke, keep_flat=kf, lag1=lag,
                             lc_delta=None, py_ms=ms))
            print(json.dumps(rows[-1]), flush=True)
            for st in ("plain", "crgs 0.30"):
                Mo = ds.stage(st, M, peak)
                base = ds.stage(st, D, peak)
                lc0 = em.local_contrast_ratio(M, ds.perceptual(base), 10)
                lc0 = float(np.mean([lc0["ratio_s%d" % k] for k in (1, 2, 3)]))
                outs = {"early": ds.stage(st, E, peak)}
                outs["late"], _ = apply_seq(AdaptiveMosquito(True), base)
                outs["both"], _ = apply_seq(AdaptiveMosquito(True), outs["early"])
                rb = rs.ring_energy(Mo, base)
                for pl, X in outs.items():
                    lc = em.local_contrast_ratio(M, ds.perceptual(X), 10)
                    rows.append(dict(context="HDR->SDR stack", case="%s %s" % (name, enc), placement=pl, stage=st,
                                     ring_ratio=rs.ring_energy(Mo, X) / rb if rb else None, ring_abs=rb,
                                     dpsnr=em.psnr(Mo, X, 10) - em.psnr(Mo, base, 10), keep_edge=None, keep_flat=None,
                                     lag1=None, lc_delta=float(np.mean([lc["ratio_s%d" % k] for k in (1, 2, 3)])) - lc0,
                                     py_ms=None))
                    print(json.dumps(rows[-1]), flush=True)
    write(rows, a.out)
    return 0


def write(rows, out):
    cols = ["context", "case", "placement", "stage", "ring_ratio", "ring_abs", "dpsnr", "keep_edge", "keep_flat", "lag1",
            "lc_delta", "py_ms"]
    fmt = lambda v: "-" if v is None else ("%.4g" % v if isinstance(v, float) else str(v))
    with open(os.path.join(out, "adaptive_mosquito.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in cols])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Adaptive grain-safe mosquito / ringing repair (see adaptive_mosquito.py)", "",
          "ring_ratio = near-edge smooth-band ringing energy after / before the repair (same domain, reference = master "
          "through the same stage); ring_abs = before (codes^2); dpsnr = PSNR change; keep = two-seed grain probe "
          "through the repair (gate +-5 %); lc_delta = post-TM local-contrast change.", "",
          "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in cols) + " |")
    md.append("")
    with open(os.path.join(out, "adaptive_mosquito.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    sys.exit(main())
