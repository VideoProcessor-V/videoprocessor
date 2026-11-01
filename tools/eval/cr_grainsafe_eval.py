# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 follow-up: GRAIN-SAFE contrast recovery for HDR -> SDR (Python reference, CPU).

  python -I tools/eval/cr_grainsafe_eval.py --scratch D:/codex/videoprocessor/eval-scratch/<ltm dir> \
         --hr-scratch D:/codex/videoprocessor/eval-scratch/<artefact_headroom scratch> --out tools/eval/prototypes/cr_grainsafe

Candidates (tonemap_ref.ToneMapper, 100 nit, VP FL35 curve): global (plain curve), cr 0.3 (libplacebo
contrast recovery as-is), cr_gs 0.15 / 0.30 / 0.45 (grain split -> recovery on the grain-free picture ->
grain added back at the curve slope), cr_gsb 0.30 / 0.45 (as cr_gs, recovered detail only above the
grain band).  Material as ltm_eval.py (same cached clips: whole frames at 1280 wide, 16 frames).
  lc_ratio / restore / halo     as ltm_eval
  grain (two-seed probe, 8 frames, PQ output codes), all RELATIVE TO THE PLAIN CURVE (gate: within +-5 %):
    keep       rms(out_A - out_B) / that of the plain curve        (eval_metrics.grain_probe, whole frame)
    lag1       lag-1 temporal correlation change vs the plain curve (gate: <= +0.05)
    keep_fine  the same on the 1-px residual band of the probe difference
    keep_mid   the same on the gauss(1) - gauss(3) band
  amplification (Part A decodes x265-10 CRF 30, aom-10 CRF 40; 4 HDR masters, 1080p raster): summed
    band / dark MSE (artefact_headroom coarse error) and mean step p99 ratio, candidate / plain curve
  py_ms  Python ms per frame (not GPU-representative).
"""
import argparse
import csv
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import artefact_headroom as ah  # noqa: E402
import eval_metrics as em  # noqa: E402
import ltm_eval as le  # noqa: E402
import tonemap_ref as tr  # noqa: E402

CANDS = [("global", "global", {}), ("cr 0.30", "cr", {"cr": 0.30}),
         ("cr_gs 0.15", "cr_gs", {"cr": 0.15}), ("cr_gs 0.30", "cr_gs", {"cr": 0.30}), ("cr_gs 0.45", "cr_gs", {"cr": 0.45}),
         ("cr_gsb 0.30", "cr_gsb", {"cr": 0.30}), ("cr_gsb 0.45", "cr_gsb", {"cr": 0.45})]


def run(kind, opts, seq, peak, ref_h):
    m = tr.ToneMapper(kind, peak, ref_h, **opts)
    t0 = time.perf_counter()
    out = np.stack([m.process(f) for f in seq])
    return out, (time.perf_counter() - t0) * 1000.0 / len(seq)


def bands(d):
    fine = np.stack([a - ndimage.gaussian_filter(a, 1.0, mode="nearest") for a in d])
    mid = np.stack([ndimage.gaussian_filter(a, 1.0, mode="nearest") - ndimage.gaussian_filter(a, 3.0, mode="nearest")
                    for a in d])
    return fine, mid


def rms(a):
    return float(np.sqrt(np.mean(a * a)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for name, parts, peak, note in le.CLIPS:
        seq = le.decode_clip(a.scratch, name, parts)
        ref_h = seq.shape[1]
        ng = 8
        gA = np.stack([em.probe_grain(seq.shape[1:], 101, t, 10) for t in range(ng)])
        gB = np.stack([em.probe_grain(seq.shape[1:], 202, t, 10) for t in range(ng)])
        base = {}
        for label, kind, opts in CANDS:
            out, ms = run(kind, opts, seq, peak, ref_h)
            P = le.perceptual(out)
            oA, _ = run(kind, opts, seq[:ng] + gA, peak, ref_h)
            oB, _ = run(kind, opts, seq[:ng] + gB, peak, ref_h)
            dG = le.perceptual(oA) - le.perceptual(oB)
            lc = em.local_contrast_ratio(seq, P, 10)
            lcm = float(np.mean([lc["ratio_s%d" % k] for k in (1, 2, 3)]))
            if kind == "global":
                base = dict(P=P, dG=dG, lc=lcm, bands=bands(dG))
            gp = em.grain_probe(base["dG"], dG, [np.ones(dG.shape[1:], bool)] * ng)
            fi, mi = bands(dG)
            row = dict(clip=name, candidate=label, lc_ratio=lcm,
                       restore=(lcm - base["lc"]) / (1 - base["lc"]) if base["lc"] < 1 else None,
                       halo=em.halo_overshoot(base["P"], P, 10), keep=gp["keep"], lag1=gp["lag1"],
                       keep_fine=rms(fi) / rms(base["bands"][0]), keep_mid=rms(mi) / rms(base["bands"][1]),
                       py_ms=ms)
            rows.append(row)
            print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
    amp = amplification(a.hr_scratch)
    write(rows, amp, a.out)
    return 0


def amplification(hr):
    dk = ah.dark_code_for("sdr")
    rows = []
    for name, peak in le.AMP_SET:
        M = ah.luma_seq(os.path.join(hr, "%s_1080p_yuv420p10le.yuv" % name), "yuv420p10le")
        for enc in le.AMP_ENC:
            D = ah.luma_seq(os.path.join(hr, "%s_1080p_%s.yuv" % (name, enc)), "yuv420p10le")
            for label, kind, opts in CANDS:
                oM, _ = run(kind, opts, M, peak, 1080)
                oD, _ = run(kind, opts, D, peak, 1080)
                fr = [ah.frame_metrics(r, c, dk) for r, c in zip(oM, oD)]
                s = ah.summarize(fr)
                n = sum(f["n"] for f in fr)
                rows.append(dict(master=name, encode=enc, candidate=label, step_p99=s["step_p99"],
                                 band_mse=sum(f["band"] for f in fr) / n, dark_mse=sum(f["dark_e"] for f in fr) / n))
                print(json.dumps(rows[-1]), flush=True)
    return rows


def write(rows, amp, out):
    cols = ["clip", "candidate", "lc_ratio", "restore", "halo", "keep", "lag1", "keep_fine", "keep_mid", "py_ms"]
    fmt = lambda v, p="%.4g": "-" if v is None else (p % v if isinstance(v, float) else str(v))
    with open(os.path.join(out, "cr_grainsafe.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in cols])
    ac = ["master", "encode", "candidate", "step_p99", "band_mse", "dark_mse"]
    with open(os.path.join(out, "amplification.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(ac)
        for r in amp:
            w.writerow([fmt(r.get(c)) for c in ac])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Grain-safe contrast recovery (HDR -> SDR 100 nit), Python reference", "",
          "Means over %d clips; grain columns relative to the plain curve (gate: keep within +-5 %%, lag-1 <= +0.05);"
          " codec columns: candidate / plain curve over 4 HDR masters x 2 encodes (1080p). See cr_grainsafe_eval.py." %
          len(set(r["clip"] for r in rows)), "",
          "| candidate | lc ratio | restore | halo | grain keep (max) | lag-1 | keep fine / mid | band MSE | dark MSE | step p99 | py ms |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    ag = [r for r in amp if r["candidate"] == "global"]
    for label, _, _ in CANDS:
        rs = [r for r in rows if r["candidate"] == label]
        m = lambda k: float(np.mean([r[k] for r in rs if r.get(k) is not None]))
        ar = [r for r in amp if r["candidate"] == label]
        bm = sum(r["band_mse"] for r in ar) / sum(r["band_mse"] for r in ag)
        dm = sum(r["dark_mse"] for r in ar) / sum(r["dark_mse"] for r in ag)
        st = float(np.mean([x["step_p99"] / y["step_p99"] for x, y in zip(ar, ag)]))
        md.append("| %s | %.3f | %.2f | %.5f | %.3f (%.3f) | %+.3f | %.3f / %.3f | %.3f | %.3f | %.3f | %.0f |" % (
            label, m("lc_ratio"), m("restore"), m("halo"), m("keep"), max(r["keep"] for r in rs), m("lag1"),
            m("keep_fine"), m("keep_mid"), bm, dm, st, m("py_ms")))
    md += ["", "## Per clip", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in cols) + " |")
    md.append("")
    with open(os.path.join(out, "cr_grainsafe.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    sys.exit(main())
