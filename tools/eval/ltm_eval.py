# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 Part B: motion-compensated LOCAL tone mapping (HDR -> SDR for the FL35), Python reference.

  python -I tools/eval/ltm_eval.py --scratch D:/codex/videoprocessor/eval-scratch/<dir> \
         --hr-scratch D:/codex/videoprocessor/eval-scratch/<artefact_headroom scratch> \
         --out tools/eval/prototypes/ltm

Candidates (tonemap_ref.ToneMapper; luma only, PQ 10-bit codes in -> SDR 10-bit codes out, 100 nit):
  global       static libplacebo spline, VP defaults, peak detection off (the FL35 setting) = baseline
  dynamic      the same with per-frame scene average / peak, 20-frame EMA, scene reset (DTM stand-in)
  cr           global + libplacebo contrast recovery 0.30 (VP quality high)
  ltm_gauss    LTM with a Gaussian (not edge-aware) base: halo contrast row
  ltm_nograin  LTM without the grain split: grain-rule contrast row
  ltm          LTM: grain split, guided-filter base (the local adaptation map), strength 1
  ltm_half     the same at strength 0.5
  ltm_mc       ltm with the base carried along DIS flow and smoothed over time, reset at cuts
Material: whole frames scaled to 1280 wide (field of view kept: LTM works on picture-scale context;
scales derive from the picture height), 16 frames per clip; one splice clip (8 + 8 frames of two shots)
tests the cut reset.
Metrics (perceptual space = PQ codes of the OUTPUT luminance vs the source PQ codes):
  lc_ratio     mean of eval_metrics.local_contrast_ratio ratio_s1..s3 (out / source local std; global
               < 1 where it compresses; 1 = all local contrast kept); boost = its boost_fraction
               (tiles > +5 % above the source: must stay ~0)
  restore      (lc_ratio - lc_ratio_global) / (1 - lc_ratio_global): share of the lost local contrast
               brought back
  halo         eval_metrics.halo_overshoot(global output, candidate output) (share of range by which the
               candidate leaves the global output's local envelope near edges)
  pump         RMS of the motion-compensated frame-to-frame change of gauss(out - global, ref_h/108)
               (the adaptation layer only) in PQ codes, cut pairs excluded; flicker = flicker_excess
               (global, candidate)
  grain_keep   two-seed probe (8 frames) (eval_metrics.probe_grain added to the PQ input): rms(out_A - out_B) of the
               candidate / that of the global curve (1 = grain tone-mapped exactly as the global curve does)
  btb / wtw    share of source excursions still beyond the SDR limits (synthetic excursion card + real)
  amplification  codec banding / blocking before vs after: Part A decodes (x265-10 CRF 30, aom-10 CRF 40)
               and their masters both tone-mapped by each candidate; artefact_headroom metrics in SDR codes;
               ratio candidate / global of step p99, false-flat, banding + dark error energy, blockiness
  py_ms        Python reference ms per 1280-wide frame (CPU numpy; NOT GPU-representative)
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
import candidates_chroma as cc  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402
import tonemap_ref as tr  # noqa: E402

FF = eio.DEFAULT_FFMPEG
T = r"D:\testclips\eval-2026-10-31"
SOL40 = T + r"\hdr\sol_levante_hdr10_24p_t40\sol_levante_hdr10_24p_t40.mov"
SOL150 = T + r"\hdr\sol_levante_hdr10_24p_t150\sol_levante_hdr10_24p_t150.mov"
NOC120 = T + r"\hdr_film_grain\nocturne_filmgrain_hdr_60p_t120\nocturne_filmgrain_hdr_60p_t120.mov"
NOC480 = T + r"\hdr_film_grain\nocturne_filmgrain_hdr_60p_t480\nocturne_filmgrain_hdr_60p_t480.mov"
CHIM = r"D:\nn-corpus\trimmed\chimera4k\reference_ffv1.mkv"
NF = 16
WID = 1280  # whole frames at 1280 wide (memory / CPU budget on the shared laptop)
# name, [(path, seek, frames)], mastering peak nits, note
CLIPS = [
    ("sol_a", [(SOL40, 1.5, NF)], 1000.0, "Sol Levante HDR10 4:4:4, bright animation"),
    ("sol_b", [(SOL150, 3.0, NF)], 1000.0, "Sol Levante, sky gradients"),
    ("noc_low", [(NOC120, 3.0, NF)], 4000.0, "Nocturne low-key, film grain"),
    ("noc_bright", [(NOC480, 1.5, NF)], 4000.0, "Nocturne bright, film grain"),
    ("chimera", [(CHIM, 78.0, NF)], 4000.0, "Chimera PQ (8-bit master; 4000 nit assumed)"),
    ("splice_cut", [(SOL150, 5.0, NF // 2), (NOC480, 2.0, NF // 2)], 4000.0, "cut: Sol -> Nocturne at frame 8"),
]
KINDS = [("global", {}), ("dynamic", {}), ("cr", {}), ("ltm_gauss", {}), ("ltm_nograin", {}), ("ltm", {}),
         ("ltm_half", {"strength": 0.5}), ("ltm_mc", {})]
AMP_KINDS = ["global", "cr", "ltm", "ltm_mc"]
AMP_SET = [("sol_sky", 1000.0), ("sol_dark", 1000.0), ("nocturne_dark", 4000.0), ("nocturne_mid", 4000.0)]
AMP_ENC = ["x265-10_crf30", "aom-10_crf40"]


def kind_of(k):
    return "ltm" if k == "ltm_half" else k


def decode_clip(scratch, name, parts):
    out = os.path.join(scratch, "ltm_%s_w%d_n%d.npy" % (name, WID, NF))
    if os.path.exists(out):
        return np.load(out)
    seqs = []
    for path, ss, n in parts:
        r = subprocess.run([FF, "-hide_banner", "-loglevel", "error", "-ss", "%.4f" % ss, "-i", path, "-frames:v", str(n),
                            "-vf", "scale=%d:-2:flags=lanczos" % WID, "-pix_fmt", "yuv420p10le", "-f", "rawvideo", "-"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode("utf8", "replace")[-600:])
        a = np.frombuffer(r.stdout, np.uint16)
        h = a.size // n // WID * 2 // 3
        fr = a.reshape(n, -1)[:, :WID * h]  # luma plane only (limited range kept)
        seqs.append(fr.reshape(n, h, WID).astype(np.float64))
    h = min(s.shape[1] for s in seqs)
    seq = np.concatenate([s[:, :h] for s in seqs])
    np.save(out, seq)
    return seq


def to8(Ycode):
    return np.clip(np.rint(tr.code_to_norm(Ycode) * 255.0), 0, 255).astype(np.uint8)


VECTORS = None  # vector_source.VectorSource for --vectors (camera-model fields); None = DIS stand-in


def flows_for(seq, clip=None):
    """flow[t] (t >= 1): current -> previous, so prev(p + F(p)) ~ cur(p).  With --vectors the fields are
    read from <dir>/<clip>/flow_<t>_<t-1>.npy (window-local indices; see vector_source.py)."""
    f8 = [to8(f) for f in seq]
    if VECTORS is not None and clip is not None:
        return [None] + [VECTORS.flow(t, t - 1, f8[t], f8[t - 1], sub=clip) for t in range(1, len(seq))]
    return [None] + [cc.dis_flow(f8[t], f8[t - 1]) for t in range(1, len(seq))]


def warp(plane, F):
    h, w = plane.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    return cc.bilinear(plane, yy + F[..., 1], xx + F[..., 0])


def cut_pairs(seq, flows, thr=0.06):
    cuts = set()
    for t in range(1, len(seq)):
        a = tr.code_to_norm(seq[t])
        b = warp(tr.code_to_norm(seq[t - 1]), flows[t])
        if float(np.mean(np.abs(a - b))) > thr:
            cuts.add(t)
    return cuts


def run_kind(kind, opts, seq, peak, ref_h, flows):
    m = tr.ToneMapper(kind_of(kind), peak, ref_h, **opts)
    outs = []
    t0 = time.perf_counter()
    for t, f in enumerate(seq):
        fn = None
        if kind == "ltm_mc" and t > 0:
            F = flows[t]
            fn = lambda pS, S, pB, F=F: (warp(pS, F), warp(pB, F))
        outs.append(m.process(f, fn))
    ms = (time.perf_counter() - t0) * 1000.0 / len(seq)
    return np.stack(outs), ms, m


def perceptual(sdr):
    return np.stack([tr.sdr_to_perceptual_codes(f) for f in sdr])


def pump(Pc, Pg, flows, cuts, ref_h):
    s = max(1.0, ref_h / 108.0)
    M = [ndimage.gaussian_filter(a - b, s, mode="nearest") for a, b in zip(Pc, Pg)]
    v = []
    for t in range(1, len(M)):
        if t in cuts:
            continue
        d = M[t] - warp(M[t - 1], flows[t])
        bd = 16
        v.append(float(np.sqrt(np.mean(d[bd:-bd, bd:-bd] ** 2))))
    return float(np.mean(v)) if v else None


def excursion_card():
    """Synthetic 10-bit PQ luma card: ramp 64..940 plus below-black (4..63) and above-white (941..1019) patches,
    with mild texture so local operators see detail next to the excursions."""
    h, w = 256, 512
    x = np.linspace(64, 940, w)[None, :].repeat(h, 0)
    rng = np.random.default_rng(7)
    x = x + 6 * ndimage.gaussian_filter(rng.standard_normal((h, w)), 1.0)
    x[20:80, 40:140] = np.linspace(4, 60, 100)[None, :]
    x[20:80, 360:470] = np.linspace(944, 1019, 110)[None, :]
    x[150:230, 200:300] = 30.0
    x[150:230, 330:430] = 1000.0
    return np.stack([x] * 3)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--skip-amp", action="store_true")
    ap.add_argument("--vectors", default=None, help="external vector fields (camera model): <dir>/<clip>/flow_<t>_<t-1>.npy")
    ap.add_argument("--kinds", default="", help="comma list of candidates (default: all)")
    a = ap.parse_args(argv)
    global VECTORS, KINDS
    if a.vectors:
        import vector_source
        VECTORS = vector_source.VectorSource(a.vectors, strict=True)
    if a.kinds:
        want = a.kinds.split(",")
        KINDS = [k for k in KINDS if k[0] in want or k[0] == "global"]
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    only = set(x for x in a.only.split(",") if x)
    rows = []
    for name, parts, peak, note in CLIPS:
        if only and name not in only:
            continue
        seq = decode_clip(a.scratch, name, parts)
        ref_h = seq.shape[1]  # spatial scales follow the picture height
        flows = flows_for(seq, name)
        cuts = cut_pairs(seq, flows)
        mv = [np.mean(np.hypot(F[..., 0], F[..., 1])) for t, F in enumerate(flows) if F is not None and t not in cuts]
        motion = float(np.mean(mv)) if mv else None
        Psrc = seq
        # two-seed grain probe input (first 8 frames)
        ng = min(8, len(seq))
        gA = np.stack([em.probe_grain(seq.shape[1:], 101, t, 10) for t in range(ng)])
        gB = np.stack([em.probe_grain(seq.shape[1:], 202, t, 10) for t in range(ng)])
        def evaluate(kind, opts):
            out, ms, m = run_kind(kind, opts, seq, peak, ref_h, flows)
            bwk = em.btb_wtw(Psrc, out, 10)
            P = perceptual(out).astype(np.float32)
            del out
            oA, _, _ = run_kind(kind, opts, seq[:ng] + gA, peak, ref_h, flows)
            oB, _, _ = run_kind(kind, opts, seq[:ng] + gB, peak, ref_h, flows)
            dG = perceptual(oA) - perceptual(oB)
            return P, ms, m.cuts, bwk, float(np.sqrt(np.mean(dG ** 2)))

        Pg, _, _, _, grms = evaluate("global", {})
        Pg64 = Pg.astype(np.float64)
        lcg = em.local_contrast_ratio(Psrc, Pg64, 10)
        lcg_m = float(np.mean([lcg["ratio_s%d" % k] for k in (1, 2, 3)]))
        for kind, opts in KINDS:
            P, ms, ncut, bwk, drms = evaluate(kind, opts)
            P = P.astype(np.float64)
            lc = em.local_contrast_ratio(Psrc, P, 10)
            lcm = float(np.mean([lc["ratio_s%d" % k] for k in (1, 2, 3)]))
            row = dict(clip=name, candidate=kind, motion_px=motion, src_cuts=len(cuts),
                       lc_ratio=lcm, lc_s1=lc["ratio_s1"], lc_s3=lc["ratio_s3"], boost=lc["boost_fraction"],
                       restore=(lcm - lcg_m) / (1 - lcg_m) if lcg_m < 1 else None,
                       halo=em.halo_overshoot(Pg64, P, 10),
                       pump=pump(P, Pg64, flows, cuts, ref_h) if kind != "global" else 0.0,
                       flicker=em.flicker_excess(Pg64, P, 10),
                       grain_keep=drms / grms if grms else None,
                       btb=bwk["btb_kept"] if bwk else None, wtw=bwk["wtw_kept"] if bwk else None,
                       mc_resets=ncut if kind == "ltm_mc" else None, py_ms=ms, note=note)
            rows.append(row)
            print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
            del P
    # synthetic excursion card
    card = excursion_card()
    card_rows = []
    for kind, opts in KINDS:
        m = tr.ToneMapper(kind_of(kind), 1000.0, 1080, **opts)
        ident = (lambda pS, S, pB: (pS, pB)) if kind == "ltm_mc" else None  # static card: identity flow
        out = np.stack([m.process(f, ident if t else None) for t, f in enumerate(card)])
        bw = em.btb_wtw(card, out, 10)
        card_rows.append(dict(candidate=kind, btb_kept=bw["btb_kept"], wtw_kept=bw["wtw_kept"],
                              clip_detect=bw["clip_detect"], out_min=float(out.min()), out_max=float(out.max())))
        print(json.dumps(card_rows[-1]), flush=True)
    amp = [] if a.skip_amp else amplification(a.hr_scratch)
    if VECTORS is not None:
        print(json.dumps({"vectors": VECTORS.stats()}), flush=True)
    write(rows, card_rows, amp, a.out)
    return 0


def amplification(hr):
    dk = ah.dark_code_for("sdr")
    rows = []
    for name, peak in AMP_SET:
        for raster, ref_h in (("1080p", 1080),):  # 1080p raster only (CPU budget)
            mpath = os.path.join(hr, "%s_%s_yuv420p10le.yuv" % (name, raster))
            if not os.path.exists(mpath):
                print("amplification: missing", mpath, flush=True)
                continue
            M = ah.luma_seq(mpath, "yuv420p10le")
            for enc in AMP_ENC:
                dpath = os.path.join(hr, "%s_%s_%s.yuv" % (name, raster, enc))
                if not os.path.exists(dpath):
                    print("amplification: missing", dpath, flush=True)
                    continue
                D = ah.luma_seq(dpath, "yuv420p10le")
                flows = flows_for(M, "amp_%s_%s_%s" % (name, raster, enc)) if "ltm_mc" in AMP_KINDS else None
                for kind in AMP_KINDS:
                    oM, _, _ = run_kind(kind, {}, M, peak, ref_h, flows)
                    oD, _, _ = run_kind(kind, {}, D, peak, ref_h, flows)
                    s = ah.seq_metrics(oM, oD, dk)
                    fr = [ah.frame_metrics(r, c, dk) for r, c in zip(oM, oD)]
                    n = sum(f["n"] for f in fr)
                    row = dict(master=name, raster=raster, encode=enc, candidate=kind,
                               step_p99=s["step_p99"], false_flat=s["false_flat"], blockiness=s["blockiness"],
                               band_mse=sum(f["band"] for f in fr) / n, dark_mse=sum(f["dark_e"] for f in fr) / n,
                               psnr=s["psnr"])
                    rows.append(row)
                    print(json.dumps({k: (round(v, 5) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
    return rows


def fmt(v, p="%.4g"):
    if v is None:
        return "-"
    if isinstance(v, float):
        return p % v
    return str(v)


def write(rows, card_rows, amp, out):
    cols = ["clip", "candidate", "motion_px", "src_cuts", "lc_ratio", "lc_s1", "lc_s3", "boost", "restore", "halo", "pump",
            "flicker", "grain_keep", "btb", "wtw", "mc_resets", "py_ms", "note"]
    with open(os.path.join(out, "ltm.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in cols])
    acols = ["master", "raster", "encode", "candidate", "step_p99", "false_flat", "blockiness", "band_mse", "dark_mse", "psnr"]
    with open(os.path.join(out, "amplification.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(acols)
        for r in amp:
            w.writerow([fmt(r.get(c)) for c in acols])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Round-2 local tone mapping (HDR -> SDR 100 nit), Python reference", "",
          "Means over %d clips (whole frames at 1280 wide, 16 frames). See ltm_eval.py / tonemap_ref.py." %
          len(set(r["clip"] for r in rows)), "",
          "| candidate | lc ratio (global = lost) | restore | boost | halo | pump (PQ codes) | flicker | grain keep vs global "
          "| BTB / WTW kept (card) | py ms/frame |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    card = {c["candidate"]: c for c in card_rows}
    for kind, _ in KINDS:
        rs = [r for r in rows if r["candidate"] == kind]
        if not rs:
            continue
        mean = lambda k: (float(np.mean([r[k] for r in rs if r.get(k) is not None]))
                          if any(r.get(k) is not None for r in rs) else None)
        c = card.get(kind, {})
        md.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s / %s | %s |" % (
            kind, fmt(mean("lc_ratio"), "%.3f"), fmt(mean("restore"), "%.2f"), fmt(mean("boost"), "%.3f"),
            fmt(mean("halo"), "%.5f"), fmt(mean("pump"), "%.3f"), fmt(mean("flicker"), "%.3f"),
            fmt(mean("grain_keep"), "%.3f"), fmt(c.get("btb_kept"), "%.3f"), fmt(c.get("wtw_kept"), "%.3f"),
            fmt(mean("py_ms"), "%.0f")))
    if amp:
        md += ["", "## Codec artefacts after tone mapping, candidate vs global (all masters x encodes, 1080p raster)", "",
               "step p99 / false-flat: mean of per-case ratios; band / dark MSE: ratio of summed MSE; blockiness: mean",
               "difference candidate - global (BI is itself a difference, so a ratio is meaningless).", "",
               "| candidate | step p99 ratio | false-flat ratio | band MSE ratio | dark MSE ratio | blockiness diff |",
               "|---|---|---|---|---|---|"]
        for kind in AMP_KINDS:
            st, ff, bd, dk, bl = [], [], [0.0, 0.0], [0.0, 0.0], []
            for r in amp:
                if r["candidate"] != kind:
                    continue
                g = [x for x in amp if x["candidate"] == "global" and x["master"] == r["master"]
                     and x["raster"] == r["raster"] and x["encode"] == r["encode"]][0]
                if r.get("step_p99") is not None and g.get("step_p99"):
                    st.append(r["step_p99"] / g["step_p99"])
                if r.get("false_flat") is not None and g.get("false_flat"):
                    ff.append(r["false_flat"] / g["false_flat"])
                bd[0] += r["band_mse"]; bd[1] += g["band_mse"]
                dk[0] += r["dark_mse"]; dk[1] += g["dark_mse"]
                if r.get("blockiness") is not None and g.get("blockiness") is not None:
                    bl.append(r["blockiness"] - g["blockiness"])
            m_ = lambda v: float(np.mean(v)) if v else None
            md.append("| %s | %s | %s | %s | %s | %s |" % (
                kind, fmt(m_(st), "%.3f"), fmt(m_(ff), "%.3f"), fmt(bd[0] / bd[1] if bd[1] else None, "%.3f"),
                fmt(dk[0] / dk[1] if dk[1] else None, "%.3f"), fmt(m_(bl), "%+.4f")))
    md += ["", "## Synthetic excursion card", "", "| candidate | BTB kept | WTW kept | clip at limit | out min | out max |",
           "|---|---|---|---|---|---|"]
    for c in card_rows:
        md.append("| %s | %s | %s | %s | %.1f | %.1f |" % (c["candidate"], fmt(c["btb_kept"], "%.3f"),
                                                          fmt(c["wtw_kept"], "%.3f"), c["clip_detect"], c["out_min"],
                                                          c["out_max"]))
    md += ["", "## Per clip", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in cols) + " |")
    if amp:
        md += ["", "## Amplification per case", "", "| " + " | ".join(acols) + " |", "|" + "---|" * len(acols)]
        for r in amp:
            md.append("| " + " | ".join(fmt(r.get(c)) for c in acols) + " |")
    md.append("")
    with open(os.path.join(out, "ltm.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    sys.exit(main())
