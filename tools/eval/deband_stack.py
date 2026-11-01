# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 STACKING test: texture re-injection (oracle) x deband x tone stage, in VP pipeline order
(re-injection and deband BEFORE tone mapping, in the signal domain), every metric reported both in the
signal domain (PQ codes for HDR, gamma codes for SDR) and, for HDR, on the post-tone-mapping SDR output
the FL35 shows.  Uses the cached artefact_headroom windows (1080p raster, 8 frames).

  D:/hfr/venv/Scripts/python.exe -I tools/eval/deband_stack.py --hr-scratch <artefact_headroom scratch> \
         --out tools/eval/prototypes/deband_stack          (venv python only for LPIPS; any python runs it)

Pre-TM variants (signal domain):
  none        the decode
  vpdb        VP repair/deband, Python MODEL: grain split (gauss 2 px low band), 4-sample ring (hash angle,
              distance 0.5-1 x 16 px x lines/1080) on the low band only, weight 1 below 0.6 x threshold -> 0
              at the threshold, residual unchanged.  Threshold = VpDebander::Strength modelled at bitrate
              quality 0.4: 1.5 x level step (10-bit: 1.5 codes; 8-bit: 6) + 1.5 eight-bit steps (6 codes).
  lpdb        libplacebo deband as Bill uses it (pl_deband_default_params: 1 iteration, threshold 3/1000,
              radius 16 px, grain 4/1000 uniform).  NB Bill's shipped config turns it OFF for HDR sources;
              the HDR rows are a what-if.
  oracle      decode - fine(decode) + fine(master): the master's own 1-px band (TemporalDetail upper bound)
  oracle+vpdb, oracle+lpdb
Tone stages (HDR, post-TM, 100 nit FL35 spline): plain, cr 0.30 (libplacebo as-is), crgs 0.15 / 0.30
  (grain-safe CR), pe (grain-safe pre-emphasis, banding-masked), pe_nomask.
Metrics (reference = the MASTER through the same stage, so a stage's own look is not counted as error):
  psnr, lpips (luma as grey, eval_metrics.lpips_score tiles; None without torch)
  band_mse / dark_mse / false_flat / step_p99   artefact_headroom coarse-error metrics
  vis        mean masked step visibility over smooth 32x32 tiles (texture_dither.V)
  keep_m     fine-band energy vs master (flat = far from strong edges)
  keep_in_flat / keep_in_edge   fine-band energy vs the same stage applied to the plain decode (grain gate
             vs input, flat and near-edge; +-5 %)
  lc         local-contrast ratio vs the SOURCE master: signal domain for pre / SDR; for post-TM, the
             output's PQ (perceptual) vs the master's PQ (same metric as the LTM study)
  halo       halo_overshoot vs the plain curve on the same input (post only)
  grain_pc   two-seed probe through the tone stage on the master (4 frames), rms vs the plain curve
"""
import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import artefact_headroom as ah  # noqa: E402
import eval_metrics as em  # noqa: E402
import texture_dither as td  # noqa: E402
import tonemap_ref as tr  # noqa: E402

NFR = 8
CASES = [  # name, raster, encode, hdr, peak
    ("cosmos_dark", "1080p", "x265-8_crf30", False, None), ("meridian_dark", "1080p", "x265-8_crf30", False, None),
    ("tos4k_dark", "1080p", "x264_crf28", False, None), ("sintel_sky", "1080p", "x265-8_crf30", False, None),
    ("sol_sky", "1080p", "x265-10_crf30", True, 1000.0), ("sol_dark", "1080p", "x265-10_crf30", True, 1000.0),
    ("nocturne_dark", "1080p", "x265-10_crf30", True, 4000.0), ("nocturne_mid", "1080p", "x265-10_crf30", True, 4000.0),
]
PRE_FULL = ["none", "vpdb", "oracle", "oracle+vpdb"]
PRE_PLAIN_ONLY = ["lpdb", "oracle+lpdb"]
STAGES = ["plain", "cr 0.30", "crgs 0.15", "crgs 0.30", "pe", "pe_nomask"]


def _hash_field(shape, seed):
    rng = np.random.default_rng(seed)
    return rng.random(shape), rng.random(shape)


def bilinear(a, yy, xx):
    """Bilinear sample of a at float (yy, xx), edge-replicated (candidates_chroma.bilinear is not in the tree)."""
    return ndimage.map_coordinates(a, [yy, xx], order=1, mode="nearest")


def _ring_avg(L, r, ang):
    h, w = L.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    dx, dy = r * np.cos(ang), r * np.sin(ang)
    return 0.25 * (bilinear(L, yy + dy, xx + dx) + bilinear(L, yy + dy, xx - dx)
                   + bilinear(L, yy - dy, xx - dx) + bilinear(L, yy - dy, xx + dx))


def deband_vp(Y, thr, ref_h, t):
    L = ndimage.gaussian_filter(Y, 2.0, mode="nearest")
    R = Y - L
    u, v = _hash_field(Y.shape, 1000 + 0)  # position hash: identical every frame (no shimmer)
    rad = 16.0 * ref_h / 1080.0 * (0.5 + 0.5 * u)
    avg = _ring_avg(L, rad, v * 2 * np.pi)
    d = np.abs(L - avg)
    wgt = np.clip((thr - d) / (0.4 * thr), 0.0, 1.0)
    return L + wgt * (avg - L) + R


def deband_lp(Y, t):
    u, v = _hash_field(Y.shape, 2000 + t)  # libplacebo's PRNG is per frame
    avg = _ring_avg(Y, 16.0 * u, v * 2 * np.pi)
    thr = 3.0 / 1000.0 * 1023.0
    res = np.where(np.abs(Y - avg) > thr, Y, avg)
    g = np.random.default_rng(3000 + t).random(Y.shape)
    return res + (4.0 / 1000.0 * 1023.0) * (g - 0.5)


def fine(a):
    return a - ndimage.gaussian_filter(a, 1.0, mode="nearest")


def pre_variant(v, D, M, hdr):
    thr = (1.5 if hdr else 6.0) + 6.0
    out = []
    for t, (d, m) in enumerate(zip(D, M)):
        x = d
        if v.startswith("oracle"):
            x = d - fine(d) + fine(m)
        if v.endswith("vpdb"):
            x = deband_vp(x, thr, 1080, t)
        elif v.endswith("lpdb"):
            x = deband_lp(x, t)
        out.append(x)
    return np.stack(out)


def stage(name, X, peak):
    if name == "plain":
        m = tr.ToneMapper("global", peak, 1080)
        return np.stack([m.process(f) for f in X])
    if name.startswith("cr "):
        m = tr.ToneMapper("cr", peak, 1080, cr=float(name.split()[1]))
        return np.stack([m.process(f) for f in X])
    if name.startswith("crgs"):
        m = tr.ToneMapper("cr_gs", peak, 1080, cr=float(name.split()[1]))
        return np.stack([m.process(f) for f in X])
    m = tr.ToneMapper("global", peak, 1080)
    return np.stack([m.process(tr.pre_emphasis(f, m.tm, 1080, mask=(name == "pe"))) for f in X])


def lp_score(ref, out):
    vals = []
    for r, o in zip(ref[:4], out[:4]):
        a = np.clip((r - 64.0) / 876.0, 0, 1)
        b = np.clip((o - 64.0) / 876.0, 0, 1)
        s = em.lpips_score(np.stack([a] * 3), np.stack([b] * 3))
        if s is None:
            return None
        vals.append(s)
    return float(np.mean(vals))


def energy(seq, mask):
    return sum(float(np.sum(fine(f)[m] ** 2)) for f, m in zip(seq, mask))


def metrics(ref, X, inp, dk, flat, edge, src=None, P=None):
    fr = [ah.frame_metrics(r, c, dk) for r, c in zip(ref, X)]
    s = ah.summarize(fr)
    n = sum(f["n"] for f in fr)
    vis = np.concatenate([td.frame_tiles(r, c, dk) for r, c in zip(ref, X)])
    lc = em.local_contrast_ratio(src if src is not None else ref, P if P is not None else X, 10)
    return dict(psnr=s["psnr"], lpips=lp_score(ref, X), band_mse=sum(f["band"] for f in fr) / n,
                dark_mse=sum(f["dark_e"] for f in fr) / n, false_flat=s["false_flat"], step_p99=s["step_p99"],
                vis=float(vis[:, 2].mean()) if len(vis) else None,
                keep_m=energy(X, flat) / max(energy(ref, flat), 1e-9),
                keep_in_flat=energy(X, flat) / max(energy(inp, flat), 1e-9),
                keep_in_edge=energy(X, edge) / max(energy(inp, edge), 1e-9),
                lc=float(np.mean([lc["ratio_s%d" % k] for k in (1, 2, 3)])) if lc else None)


def perceptual(sdr):
    return np.stack([tr.sdr_to_perceptual_codes(f) for f in sdr])


def projection(hr, out):
    """How much of the MASTER's own fine texture / grain survives each deband (energy ratios cannot tell kept
    texture from the dither noise a deband adds).  Input = oracle (decode with the master's fine band), so the
    texture is known; proj = <fine(X), fine(M)> / <fine(M), fine(M)> over far-from-edge pixels (1 = all kept,
    added noise does not count), added = energy of fine(X) orthogonal to fine(M) / energy of fine(M).
    Signal domain (PQ codes for HDR, gamma for SDR), 4 frames."""
    rows = []
    for name, raster, enc, hdr, peak in CASES:
        pix = "yuv420p10le" if hdr else "yuv420p"
        M = ah.luma_seq(os.path.join(hr, "%s_%s_%s.yuv" % (name, raster, pix)), pix, 4)
        D = ah.luma_seq(os.path.join(hr, "%s_%s_%s.yuv" % (name, raster, enc)), "yuv420p10le", 4)
        O = pre_variant("oracle", D, M, hdr)
        flat = [em.near_edge_zone(m, 10)[1] for m in M]
        for v in ("oracle", "oracle+vpdb", "oracle+lpdb"):
            X = O if v == "oracle" else pre_variant(v, D, M, hdr)
            num = den = orth = 0.0
            for x, m, f in zip(X, M, flat):
                fx, fm = fine(x)[f], fine(m)[f]
                num += float(np.dot(fx, fm))
                den += float(np.dot(fm, fm))
            p = num / den
            for x, m, f in zip(X, M, flat):
                fx, fm = fine(x)[f], fine(m)[f]
                orth += float(np.sum((fx - p * fm) ** 2))
            rows.append(dict(case=name, hdr=hdr, variant=v, proj=p, added=orth / den))
            print(json.dumps(rows[-1]), flush=True)
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Deband vs the master's own fine texture / grain (projection; see deband_stack.projection)", "",
          "| domain | variant | master texture kept (proj) | added non-master fine energy |", "|---|---|---|---|"]
    for hdr, dom in ((False, "SDR gamma"), (True, "HDR PQ (pre-TM)")):
        for v in ("oracle", "oracle+vpdb", "oracle+lpdb"):
            rs = [r for r in rows if r["hdr"] == hdr and r["variant"] == v]
            md.append("| %s | %s | %.3f | %.3f |" % (dom, v, np.mean([r["proj"] for r in rs]), np.mean([r["added"] for r in rs])))
    md.append("")
    with open(os.path.join(out, "deband_projection.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--projection-only", action="store_true")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    if a.projection_only:
        projection(a.hr_scratch, a.out)
        return 0
    rows, probe = [], []
    for name, raster, enc, hdr, peak in CASES:
        pix = "yuv420p10le" if hdr else "yuv420p"
        M = ah.luma_seq(os.path.join(a.hr_scratch, "%s_%s_%s.yuv" % (name, raster, pix)), pix, NFR)
        D = ah.luma_seq(os.path.join(a.hr_scratch, "%s_%s_%s.yuv" % (name, raster, enc)), "yuv420p10le", NFR)
        zones = [em.near_edge_zone(m, 10) for m in M]
        edge, flat = [z[0] for z in zones], [z[1] for z in zones]
        dk_sig = ah.dark_code_for("pq" if hdr else "sdr")
        pre = {v: pre_variant(v, D, M, hdr) for v in PRE_FULL + PRE_PLAIN_ONLY}
        for v, X in pre.items():
            row = dict(case=name, enc=enc, ctx="HDR-PQ (pre-TM)" if hdr else "SDR", pre=v, stage="-",
                       **metrics(M, X, D, dk_sig, flat, edge))
            rows.append(row)
            print(json.dumps({k: (round(x, 4) if isinstance(x, float) else x) for k, x in row.items()}), flush=True)
        if not hdr:
            continue
        dk = ah.dark_code_for("sdr")
        plain_cache = {}
        for st in STAGES:
            Mo = stage(st, M, peak)
            Do = stage(st, D, peak)
            for v in (PRE_FULL if True else []) + (PRE_PLAIN_ONLY if st == "plain" else []):
                Xo = Do if v == "none" else stage(st, pre[v], peak)
                Po = perceptual(Xo)
                if st == "plain":
                    plain_cache[v] = Po
                row = dict(case=name, enc=enc, ctx="HDR->SDR (post-TM)", pre=v, stage=st,
                           **metrics(Mo, Xo, Do, dk, flat, edge, src=M, P=Po))
                row["halo"] = em.halo_overshoot(plain_cache[v], Po, 10) if v in plain_cache else None
                rows.append(row)
                print(json.dumps({k: (round(x, 4) if isinstance(x, float) else x) for k, x in row.items()}), flush=True)
            # loss decomposition / grain probe through the stage on the master
            gA = np.stack([em.probe_grain(M.shape[1:], 101, t, 10) for t in range(4)])
            gB = np.stack([em.probe_grain(M.shape[1:], 202, t, 10) for t in range(4)])
            dG = perceptual(stage(st, M[:4] + gA, peak)) - perceptual(stage(st, M[:4] + gB, peak))
            lcM = em.local_contrast_ratio(M, perceptual(Mo), 10)
            probe.append(dict(case=name, stage=st, grain_rms=float(np.sqrt(np.mean(dG ** 2))),
                              lc_master_through_stage=float(np.mean([lcM["ratio_s%d" % k] for k in (1, 2, 3)]))))
            print(json.dumps(probe[-1]), flush=True)
    write(rows, probe, a.out)
    return 0


def write(rows, probe, out):
    cols = ["case", "enc", "ctx", "pre", "stage", "psnr", "lpips", "band_mse", "dark_mse", "false_flat", "step_p99", "vis",
            "keep_m", "keep_in_flat", "keep_in_edge", "lc", "halo"]
    fmt = lambda v: "-" if v is None else ("%.4g" % v if isinstance(v, float) else str(v))
    with open(os.path.join(out, "deband_stack.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in cols])
    with open(os.path.join(out, "stage_probe.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["case", "stage", "grain_rms", "lc_master_through_stage"])
        for r in probe:
            w.writerow([r["case"], r["stage"], fmt(r["grain_rms"]), fmt(r["lc_master_through_stage"])])
    # summary: means over cases per (ctx, pre, stage); MSE as ratio of sums vs (none, same stage)
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Stacking: texture re-injection (oracle) x deband x tone stage", "",
          "Means over cases (SDR: 4 masters; HDR: 4 masters, x265-10 CRF 30, 1080p, 8 frames). band/dark = summed MSE vs "
          "'none' through the same stage. Domain: SDR rows = gamma codes; 'HDR-PQ (pre-TM)' = PQ codes; "
          "'HDR->SDR (post-TM)' = SDR output codes, lc = output PQ vs source PQ. See deband_stack.py.", "",
          "| domain | pre-TM | tone stage | PSNR | LPIPS | band | dark | false-flat | vis | keep vs master | "
          "keep vs input flat / edge | lc | halo |", "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    keys = []
    for r in rows:
        k = (r["ctx"], r["pre"], r["stage"])
        if k not in keys:
            keys.append(k)
    for k in keys:
        rs = [r for r in rows if (r["ctx"], r["pre"], r["stage"]) == k]
        base = [r for r in rows if (r["ctx"], r["pre"], r["stage"]) == (k[0], "none", k[2])]
        m = lambda c: (float(np.mean([r[c] for r in rs if r.get(c) is not None]))
                       if any(r.get(c) is not None for r in rs) else None)
        ratio = lambda c: sum(r[c] for r in rs) / max(sum(r[c] for r in base), 1e-12)
        f = lambda v, p: "-" if v is None else p % v
        md.append("| %s | %s | %s | %s | %s | %.2f | %.2f | %s | %s | %s | %s / %s | %s | %s |" % (
            k[0], k[1], k[2], f(m("psnr"), "%.2f"), f(m("lpips"), "%.4f"), ratio("band_mse"), ratio("dark_mse"),
            f(m("false_flat"), "%.3f"), f(m("vis"), "%.3f"), f(m("keep_m"), "%.3f"), f(m("keep_in_flat"), "%.3f"),
            f(m("keep_in_edge"), "%.3f"), f(m("lc"), "%.3f"), f(m("halo"), "%.5f")))
    md += ["", "## Tone stages on the master (two-seed grain probe rms, local contrast of the master through the stage)", "",
           "| stage | grain rms vs plain | lc (master through stage) |", "|---|---|---|"]
    for st in STAGES:
        ps = [p for p in probe if p["stage"] == st]
        pl = [p for p in probe if p["stage"] == "plain"]
        if ps:
            md.append("| %s | %.3f | %.3f |" % (st, np.mean([x["grain_rms"] / y["grain_rms"] for x, y in zip(ps, pl)]),
                                                np.mean([x["lc_master_through_stage"] for x in ps])))
    md += ["", "## All rows", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in cols) + " |")
    md.append("")
    with open(os.path.join(out, "deband_stack.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    sys.exit(main())
