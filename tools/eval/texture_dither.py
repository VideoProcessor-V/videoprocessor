# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 owner hypothesis: is stream banding largely caused by the encoder stripping the fine texture /
grain that dithered the quantisation steps?  Uses the cached artefact_headroom windows (run that first).

  python -I tools/eval/texture_dither.py --hr-scratch D:/codex/videoprocessor/eval-scratch/<dir> \
         --out tools/eval/prototypes/texture_dither

Per window frame, 32x32 tiles; analysed tiles = >= 60 % "smooth" (artefact_headroom.masks of the
reference: no texture above the grain / noise level, i.e. flats, ramps and grainy flats) with some
reference fine-band energy.  Per tile:
  R  texture retention = fine-band (1-px residual) energy of the decode / of the reference
  B  coarse error energy = mean gauss(decode - reference, 3)^2 (the banding / blocking error)
  V  masked step visibility = sqrt(B) / sqrt(sigma_f^2 + 1), sigma_f = fine-band RMS of the decode in the
     tile (contrast-masking proxy: texture / grain over a step hides it; 1 code^2 floor)
Statistics per context x raster x encode (tiles pooled over masters and frames):
  rho_RB, rho_RV  Spearman rank correlation of R with B and with V (negative = banding where texture is lost)
  q1_share        share of all coarse error energy in the quartile of tiles with the LOWEST retention
                  (0.25 = no concentration)
  Oracle (the best TemporalDetail could restore): decode' = decode - fine(decode) + fine(reference) in the
  analysed tiles.  dB_oracle = relative change of the coarse error energy (the step itself; ~0 expected,
  re-injected texture does not remove a step), dV_oracle = 1 - mean V' / mean V (share of the step
  VISIBILITY the restored texture masks).
Contexts as artefact_headroom: SDR (8-bit masters), HDR-PQ, HDR->SDR (static FL35 spline, 100 nit).
"""
import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage, stats  # noqa: E402

import artefact_headroom as ah  # noqa: E402

TILE = 32


def fine(a):
    return a - ndimage.gaussian_filter(a, 1.0, mode="nearest")


def coarse(a):
    return ndimage.gaussian_filter(a, 3.0, mode="nearest")


def frame_tiles(r, c, dark_code):
    """-> (tile rows [R, B, V, Bo, Vo]) for one frame pair; oracle evaluated in the same pass."""
    m = ah.masks(r, dark_code)["smooth"]
    fr, fc = fine(r), fine(c)
    e = coarse(c - r)
    h, w = r.shape
    sel = []
    oracle = c.copy()
    for y in range(0, h - TILE + 1, TILE):
        for x in range(0, w - TILE + 1, TILE):
            sl = (slice(y, y + TILE), slice(x, x + TILE))
            if m[sl].mean() < 0.6 or float(np.mean(fr[sl] ** 2)) <= 0.05:
                continue
            oracle[sl] = c[sl] - fc[sl] + fr[sl]
            sel.append(sl)
    if not sel:
        return np.zeros((0, 5))
    fo = fine(oracle)
    eo = coarse(oracle - r)
    out = []
    for sl in sel:
        er, ec, eo_f = float(np.mean(fr[sl] ** 2)), float(np.mean(fc[sl] ** 2)), float(np.mean(fo[sl] ** 2))
        B, Bo = float(np.mean(e[sl] ** 2)), float(np.mean(eo[sl] ** 2))
        out.append([ec / er, B, np.sqrt(B) / np.sqrt(ec + 1.0), Bo, np.sqrt(Bo) / np.sqrt(eo_f + 1.0)])
    return np.array(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    acc = {}
    for name, path, ss, fps, src, transfer, mpeak, content in ah.MASTERS:
        for raster in ah.RASTERS:
            if raster == "2160p" and src[0] < 3840:
                continue
            hdr = transfer == "pq"
            pix = "yuv420p10le" if hdr else "yuv420p"
            ref = ah.luma_seq(os.path.join(a.hr_scratch, "%s_%s_%s.yuv" % (name, raster, pix)), pix)
            ctxs = [("HDR-PQ", "pq"), ("HDR->SDR", "sdr")] if hdr else [("SDR", "sdr")]
            refs = {ctx: (ah.to_sdr(ref, mpeak) if ctx == "HDR->SDR" else ref) for ctx, _ in ctxs}
            for codec, label, _ in (ah.ENC_HDR if hdr else ah.ENC_SDR):
                dec = ah.luma_seq(os.path.join(a.hr_scratch, "%s_%s_%s.yuv" % (name, raster, label.replace(" ", "_"))),
                                  "yuv420p10le")
                for ctx, space in ctxs:
                    dd = ah.to_sdr(dec, mpeak) if ctx == "HDR->SDR" else dec
                    dk = ah.dark_code_for(space)
                    acc.setdefault((ctx, raster, label), []).extend(
                        frame_tiles(r, c, dk) for r, c in zip(refs[ctx], dd))
            print(name, raster, flush=True)
    rows = []
    for (ctx, raster, label), parts in acc.items():
        T = np.concatenate(parts)
        if len(T) < 20:
            continue
        R, B, V, Bo, Vo = T.T
        q = R <= np.quantile(R, 0.25)
        rows.append(dict(context=ctx, raster=raster, encode=label, tiles=int(len(T)),
                         retention_median=float(np.median(R)),
                         rho_RB=float(stats.spearmanr(R, B)[0]), rho_RV=float(stats.spearmanr(R, V)[0]),
                         q1_share=float(B[q].sum() / B.sum()) if B.sum() > 0 else None,
                         dB_oracle=float(Bo.sum() / B.sum() - 1.0) if B.sum() > 0 else None,
                         dV_oracle=float(1.0 - Vo.mean() / V.mean()) if V.mean() > 0 else None))
        print(json.dumps(rows[-1]), flush=True)
    order = {"SDR": 0, "HDR-PQ": 1, "HDR->SDR": 2}
    rows.sort(key=lambda r: (order[r["context"]], r["raster"] != "2160p", r["encode"]))
    cols = ["context", "raster", "encode", "tiles", "retention_median", "rho_RB", "rho_RV", "q1_share", "dB_oracle",
            "dV_oracle"]
    fmt = lambda v: "-" if v is None else ("%.3f" % v if isinstance(v, float) else str(v))
    with open(os.path.join(a.out, "texture_dither.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r[c]) for c in cols])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Texture loss vs banding (owner hypothesis), per 32x32 smooth tile", "",
          "See texture_dither.py. rho < 0: banding error sits where texture retention is low; q1_share 0.25 = no "
          "concentration; oracle = the master's fine band re-injected into the decode.", "",
          "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r[c]) for c in cols) + " |")
    md.append("")
    with open(os.path.join(a.out, "texture_dither.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))
    return 0


if __name__ == "__main__":
    sys.exit(main())
