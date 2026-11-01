# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VP-V common evaluation harness CLI.

  python -I tools/eval/eval_harness.py run --suite tools/eval/suites/sample_chroma.json \
         --candidate chroma_lanczos3 --baseline chroma_bicubic --out <dir> [--max-frames N] [--lpips] [--no-cost]
  python -I tools/eval/eval_harness.py catalog [--out tools/eval/material_catalog.json]
  python -I tools/eval/eval_harness.py align --suite <suite.json> [--case ID]

See tools/eval/README.md.  CPU only; large scratch output belongs outside the worktree.
"""
import argparse
import json
import os
import platform
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import eval_candidates as ec  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402
import eval_protocols as ep  # noqa: E402
import eval_report as er  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS_VERSION = 1


# ------------------------------------------------------------------ loading
def load_source(spec, count, start=None):
    """Frames from a suite source spec.  kind: raw | y4m | ffmpeg.  `start` overrides spec['start']."""
    start = spec.get("start", 0) if start is None else start
    kw = dict(range=spec.get("range", "limited"), matrix=spec.get("matrix", "709"),
              transfer=spec.get("transfer", "sdr"), siting=spec.get("siting", "left"))
    k = spec["kind"]
    if k == "raw":
        fr = eio.read_raw_frames(spec["path"], spec["width"], spec["height"], spec["pix_fmt"], start, count,
                                 fps=spec.get("fps", 24.0), **kw)
    elif k == "y4m":
        fr = eio.read_y4m_frames(spec["path"], start, count, **kw)
    elif k == "ffmpeg":
        fr = eio.ffmpeg_decode_window(spec["path"], start, count, spec["width"], spec["height"], spec["pix_fmt"],
                                      spec["fps"], spec.get("ffmpeg", eio.DEFAULT_FFMPEG), **kw)
    else:
        raise ValueError("unknown source kind " + k)
    if len(fr) < count:
        raise ValueError("%s: only %d of %d frames available at %d" % (spec.get("path"), len(fr), count, start))
    return fr


# ------------------------------------------------------------------ metrics for one (truth, output) sequence
def _stack(frames, name):
    return np.stack([getattr(f, name) for f in frames])


def compute_metrics(truth, out, case, use_lpips=False, inputs=None, notes=None):
    """Flat ordered dict metric -> value (None = NA) for a sequence of truth/output Frames.
    `inputs` = the candidate INPUT frames (same for candidate and default): enables the input-relative
    metrics (*_vs_input) on planes whose input and output geometry match.  `notes` (dict) receives the
    reason for every NA value."""
    notes = {} if notes is None else notes
    for t, o in zip(truth, out):
        for n in ("Y", "Cb", "Cr"):
            if getattr(t, n).shape != getattr(o, n).shape:
                raise ValueError("output plane %s is %s, reference %s" % (n, getattr(o, n).shape, getattr(t, n).shape))
    bd = truth[0].bitdepth
    rng = truth[0].range
    R = {n: _stack(truth, n) for n in ("Y", "Cb", "Cr")}
    O = {n: _stack(out, n) for n in ("Y", "Cb", "Cr")}
    m = {}
    for n in ("Y", "Cb", "Cr"):
        m["psnr_" + n] = em.psnr(R[n], O[n], bd)
    for n in ("Y", "Cb", "Cr"):
        m["ssim_" + n] = em.ssim(R[n], O[n], bd)
    lp = None
    if use_lpips and R["Cb"].shape == R["Y"].shape and O["Cb"].shape == O["Y"].shape:
        vals = []
        for t, o in zip(truth, out):
            a = em.ycbcr_to_rgb(t.Y, t.Cb, t.Cr, bd, rng, t.matrix)
            b = em.ycbcr_to_rgb(o.Y, o.Cb, o.Cr, bd, rng, o.matrix)
            v = em.lpips_score(a, b)
            if v is None:
                vals = None
                break
            vals.append(v)
        lp = float(np.mean(np.array(vals, dtype=np.float64))) if vals else None
    m["lpips"] = lp
    for n in ("Y", "Cb"):
        tiles = em.select_flat_tiles(R[n], bd)
        ratio, _ = em.grain_rms_ratio(R[n], O[n], tiles)
        m["grain_tiles_" + n] = float(len(tiles))
        m["grain_rms_ratio_" + n] = ratio
        sp = em.grain_temporal_spectrum(R[n], O[n], tiles)
        m["grain_hf_ratio_" + n] = sp["ratio"] if sp else None
        l1 = em.grain_lag1(R[n], O[n], tiles)
        m["grain_lag1_delta_" + n] = l1["delta"] if l1 else None
    # input-relative gates: "grain never averaged away" and BTB/WTW preserved relative to what the candidate got
    I = {n: _stack(inputs, n) for n in ("Y", "Cb", "Cr")} if inputs else None
    for n in ("Y", "Cb"):
        k1, k2 = "grain_keep_vs_input_" + n, "grain_lag1_vs_input_" + n
        if I is None or I[n].shape != O[n].shape:
            m[k1] = m[k2] = None
            notes[k1] = notes[k2] = "input/output geometry differs (or no input given)"
            continue
        tiles = em.select_flat_tiles(R[n], bd)
        r, _ = em.grain_rms_ratio(I[n], O[n], tiles)
        m[k1] = r
        l1 = em.grain_lag1(I[n], O[n], tiles)
        m[k2] = l1["delta"] if l1 else None
    for n, planes in (("Y", ("Y",)), ("C", ("Cb", "Cr"))):
        kb, kw = "btb_kept_vs_input_" + n, "wtw_kept_vs_input_" + n
        if I is None or any(I[p].shape != O[p].shape for p in planes):
            m[kb] = m[kw] = None
            notes[kb] = notes[kw] = "input/output geometry differs (or no input given)"
            continue
        bwi = em.btb_wtw(np.concatenate([I[p] for p in planes]), np.concatenate([O[p] for p in planes]), bd,
                         "luma" if n == "Y" else "chroma", rng)
        m[kb] = bwi["btb_kept"] if bwi else None
        m[kw] = bwi["wtw_kept"] if bwi else None
    m["flicker_excess_Y"] = em.flicker_excess(R["Y"], O["Y"], bd)
    g = em.gop_spectrum(R["Y"], O["Y"], case.get("gop"), case.get("minigop")) if case.get("gop") else None
    m["gop_ratio_gop"] = g.get("gop_ratio") if g else None
    m["gop_ratio_minigop"] = g.get("minigop_ratio") if g else None
    m["ghost_coeff_Y"] = em.ghost_coeff(R["Y"], O["Y"], bd)
    m["smear_Y"] = em.smear(R["Y"], O["Y"], bd)
    m["duplicate_rate_Y"] = em.duplicate_rate(R["Y"], O["Y"], bd)
    m["halo_overshoot_Y"] = em.halo_overshoot(R["Y"], O["Y"], bd)
    lc = em.local_contrast_ratio(R["Y"], O["Y"], bd)
    for k in ("ratio_s1", "ratio_s2", "ratio_s3"):
        m["lc_" + k + "_Y"] = lc[k] if lc else None
    m["lc_boost_fraction_Y"] = lc["boost_fraction"] if lc else None
    bw = em.btb_wtw(R["Y"], O["Y"], bd, "luma", rng)
    m["btb_kept_Y"] = bw["btb_kept"] if bw else None
    m["wtw_kept_Y"] = bw["wtw_kept"] if bw else None
    m["clip_detect_Y"] = bw["clip_detect"] if (bw and (bw["btb_n"] or bw["wtw_n"])) else None
    bc = em.btb_wtw(np.concatenate([R["Cb"], R["Cr"]]), np.concatenate([O["Cb"], O["Cr"]]), bd, "chroma", rng)
    m["btb_kept_C"] = bc["btb_kept"] if bc else None
    m["wtw_kept_C"] = bc["wtw_kept"] if bc else None
    m["clip_detect_C"] = bc["clip_detect"] if (bc and (bc["btb_n"] or bc["wtw_n"])) else None
    _na_reasons(m, notes, len(truth), case, rng)
    return m


def _na_reasons(m, notes, n, case, rng):
    """Attach a correct reason to every NA metric that has none yet."""
    gop = case.get("gop")
    longest = max(gop or 0, case.get("minigop") or 0)
    for k, v in m.items():
        if v is not None or k in notes:
            continue
        if k.startswith("grain_hf"):
            notes[k] = "needs >= 8 frames (%d available)" % n if n < 8 else "no flat tiles / no reference grain"
        elif k.startswith("grain_lag1"):
            notes[k] = "needs >= 3 frames (%d available)" % n if n < 3 else "no flat tiles"
        elif k.startswith("flicker"):
            notes[k] = "needs >= 3 frames (%d available)" % n
        elif k.startswith("gop_"):
            if not gop:
                notes[k] = "no GOP metadata"
            elif n < 2 * longest:
                notes[k] = "GOP %d longer than half the window (%d frames)" % (longest, n)
            else:
                notes[k] = "no usable energy series"
        elif k.startswith("grain_rms") or k.startswith("grain_keep"):
            notes[k] = "no flat tiles in the reference"
        elif k.startswith(("btb_", "wtw_", "clip_detect")):
            notes[k] = "candidate input has no excursions beyond nominal limits" if "vs_input" in k else "full-range material" if rng != "limited" else "reference has no excursions beyond nominal limits"
        elif k.startswith(("ghost", "smear")):
            notes[k] = "no moving pixels / needs >= 2 frames"
        elif k.startswith("duplicate"):
            notes[k] = "needs >= 2 frames with reference change"
        elif k.startswith("lc_"):
            notes[k] = "featureless reference"


# ------------------------------------------------------------------ cases
def crop_frame(frame, crop, master_shape):
    """Crop a Frame to case["crop"] = {"x","y","w","h"} given in MASTER luma coordinates (all multiples
    of 4 so every 4:2:0 / half-raster plane stays aligned).  A coded picture on a smaller raster (a
    half-size rung) is cropped at the proportionally scaled window.  Chroma planes are cropped at the
    subsampled window, so chroma siting is unchanged."""
    x, y, w, h = (int(crop[k]) for k in ("x", "y", "w", "h"))
    if any(v % 4 for v in (x, y, w, h)) or w <= 0 or h <= 0:
        raise ValueError("crop %s: x, y, w, h must be positive multiples of 4" % (crop,))
    if x + w > master_shape[1] or y + h > master_shape[0]:
        raise ValueError("crop %s exceeds the master raster %s" % (crop, master_shape))
    H, W = frame.Y.shape
    # integer raster ratio (1 = same raster, 2 = half-size rung; odd master sizes round down)
    ry, rx = max(master_shape[0] // H, 1), max(master_shape[1] // W, 1)
    y0, x0, h0, w0 = y // ry, x // rx, h // ry, w // rx
    fy, fx = max(round(H / frame.Cb.shape[0]), 1), max(round(W / frame.Cb.shape[1]), 1)
    sl = (slice(y0, y0 + h0), slice(x0, x0 + w0))
    cs = (slice(y0 // fy, (y0 + h0) // fy), slice(x0 // fx, (x0 + w0) // fx))
    return frame.with_planes(Y=np.ascontiguousarray(frame.Y[sl]), Cb=np.ascontiguousarray(frame.Cb[cs]),
                             Cr=np.ascontiguousarray(frame.Cr[cs]))


def build_case_inputs(case, max_frames=None):
    """-> (candidate inputs, truth).  Optional case keys: "crop" (see crop_frame), applied to master and
    coded frames before the protocol."""
    n = case.get("frames", 4)
    if max_frames:
        n = min(n, max_frames)
    proto = ep.PROTOCOLS[case.get("protocol", "chroma444")]
    ms = load_source(case["master"], n)
    cs = load_source(case["coded"], n)
    base = case["master"].get("start", 0)
    crop = case.get("crop")
    inputs, truth = [], []
    for i, (mf, cf) in enumerate(zip(ms, cs)):
        if crop:
            mshape = mf.Y.shape
            mf, cf = crop_frame(mf, crop, mshape), crop_frame(cf, crop, mshape)
        mf.index = cf.index = base + i
        a, t = proto(mf, cf)
        a.index = t.index = base + i
        inputs.append(a)
        truth.append(t)
    return inputs, truth


def run_candidate(cand, inputs, truth):
    """-> (output frames, ms per frame list, kind-specific cost dict)"""
    if cand.kind == "python":
        outs, ms = ec.run_python_candidate(cand, inputs)
        return outs, dict(ms=ms, gpu_ms=None)
    outs, gms = [], []
    for t in truth:
        if t.index not in cand.index:
            raise KeyError("capture has no source_frame_index %d" % t.index)
        fr, g = cand.frame(t.index)
        outs.append(fr)
        gms.append(g)
    have = [g for g in gms if g is not None]
    return outs, dict(ms=None, gpu_ms=have if len(have) == len(gms) else None)


def grain_probe_rows(cand, inputs, truth, notes):
    """Near-edge grain gate (case key "grain_probe": true; Python candidates only).  The candidate is run
    twice more, on its inputs plus synthetic probe grain of two fixed seeds (luma only); out_A - out_B
    is the part of the probe grain it passed.  Rows (all *_vs_input: never LIMIT):
      grain_probe_keep_edge_vs_input_Y / grain_probe_lag1_edge_vs_input_Y   near strong edges
      grain_probe_keep_flat_vs_input_Y / grain_probe_lag1_flat_vs_input_Y   far from edges"""
    names = ("grain_probe_keep_edge_vs_input_Y", "grain_probe_lag1_edge_vs_input_Y",
             "grain_probe_keep_flat_vs_input_Y", "grain_probe_lag1_flat_vs_input_Y")
    if getattr(cand, "kind", "python") != "python":
        for k in names:
            notes[k] = "grain probe needs a Python candidate (capture candidates cannot be re-run)"
        return {k: None for k in names}
    bd = inputs[0].bitdepth
    ins = []
    for seed in em.PROBE_SEEDS:
        ins.append([f.with_planes(Y=f.Y + em.probe_grain(f.Y.shape, seed, f.index, bd)) for f in inputs])
    outs = [ec.run_python_candidate(cand, x)[0] for x in ins]
    d_in = np.stack([a.Y - b.Y for a, b in zip(*ins)])
    d_out = np.stack([a.Y - b.Y for a, b in zip(*outs)])
    zones = [em.near_edge_zone(t.Y, bd) for t in truth]
    m = {}
    for tag, idx in (("edge", 0), ("flat", 1)):
        r = em.grain_probe(d_in, d_out, [z[idx] for z in zones])
        kk, kl = "grain_probe_keep_%s_vs_input_Y" % tag, "grain_probe_lag1_%s_vs_input_Y" % tag
        m[kk] = r["keep"] if r else None
        m[kl] = r["lag1"] if r else None
        if r is None:
            notes[kk] = notes[kl] = "no %s pixels in the reference" % ("near-edge" if tag == "edge" else "far-from-edge")
        elif r["lag1"] is None:
            notes[kl] = "needs >= 3 frames (%d available)" % len(truth)
    return m


def load_cost_ratio():
    with open(os.path.join(HERE, "cost_ratios.json"), "r", encoding="utf8") as f:
        return json.load(f)


def evaluate_suite(suite, cand, base, max_frames=None, use_lpips=False, with_cost=True, log=print,
                   cache=None, extra=None):
    """Rows for `cand` vs `base` over the suite's cases.
    cache: optional dict shared across calls with the SAME baseline and options: per case it keeps the
           inputs, truth and the baseline's result, so several candidates are scored without reloading
           or re-running the default (values are identical to uncached runs; cost rows reuse the
           baseline's first timing).
    extra: optional callable(truth, outs, inputs, case, notes) -> dict of additional metric rows,
           applied to candidate and default alike (named so eval_report.RULES judges them; unknown
           prefixes are INFO)."""
    rows = []
    ratio = load_cost_ratio()["gpu_ms_ratio"]
    for case in suite["cases"]:
        ct, clip, cname = case["content_type"], case["clip"], case["case"]
        t0 = time.time()
        key = (case.get("id"), ct, clip, cname, max_frames, bool(use_lpips), base.name)
        hit = cache.get(key) if cache is not None else None
        if hit is None:
            log("case %s / %s / %s: loading" % (ct, clip, cname))
            inputs, truth = build_case_inputs(case, max_frames)
        else:
            log("case %s / %s / %s: cached inputs and default" % (ct, clip, cname))
            inputs, truth = hit["inputs"], hit["truth"]
        res = {}
        notes_by = {"default": dict(hit["notes"])} if hit is not None else {}
        for label, c in (("candidate", cand), ("default", base)):
            if label == "default" and hit is not None:
                res[label] = hit["default"]
                continue
            notes_by[label] = {}
            try:
                outs, cost = run_candidate(c, inputs, truth)
                m = compute_metrics(truth, outs, case, use_lpips, inputs, notes_by[label])
                if extra is not None:
                    m.update(extra(truth, outs, inputs, case, notes_by[label]))
                if case.get("grain_probe"):
                    m.update(grain_probe_rows(c, inputs, truth, notes_by[label]))
                res[label] = (m, cost, None)
            except Exception as e:  # visible, never silent
                res[label] = (None, None, "%s: %s" % (type(e).__name__, e))
                log("  %s FAILED: %s" % (label, res[label][2]))
        # NA reasons: the candidate's own reason wins, the default's fills the rest
        na_notes = dict(notes_by.get("default", {}))
        na_notes.update(notes_by.get("candidate", {}))
        if cache is not None and hit is None:
            cache[key] = dict(inputs=inputs, truth=truth, default=res["default"], notes=dict(notes_by["default"]))
        names = None
        for label in ("candidate", "default"):
            if res[label][0] is not None:
                names = list(res[label][0])
                break
        for k in names or []:
            note = ""
            if k == "lpips" and res["candidate"][0] and res["candidate"][0][k] is None:
                note = "lpips NA (not requested, or torch/lpips unavailable)"
            cv, bv = (res["candidate"][0] or {}).get(k), (res["default"][0] or {}).get(k)
            if not note and (cv is None or bv is None) and k in na_notes:
                note = na_notes[k]
            rows.append(dict(content_type=ct, clip=clip, case=cname, metric=k,
                             candidate=res["candidate"][0][k] if res["candidate"][0] else None,
                             default=res["default"][0][k] if res["default"][0] else None, note=note))
        for label in ("candidate", "default"):
            if res[label][2]:
                rows.append(dict(content_type=ct, clip=clip, case=cname, metric="error_" + label,
                                 candidate=None, default=None, note=res[label][2]))
        if with_cost:
            def cost_vals(label):
                cost = res[label][1]
                if not cost:
                    return None, None, None
                if cost["ms"] is not None:
                    return em.median_ms(cost["ms"]), None, None
                g = em.median_ms(cost["gpu_ms"]) if cost["gpu_ms"] else None
                return None, g, em.est_3090_ms(g, ratio)
            cc, bb = cost_vals("candidate"), cost_vals("default")
            rows.append(dict(content_type=ct, clip=clip, case=cname, metric="cost_ms_per_frame_pyref",
                             candidate=cc[0], default=bb[0], note="py-ref: CPU numpy wall-clock, not GPU-representative"))
            rows.append(dict(content_type=ct, clip=clip, case=cname, metric="cost_gpu_ms_laptop",
                             candidate=cc[1], default=bb[1], note="capture sidecar gpu_ms (laptop GPU)"))
            rows.append(dict(content_type=ct, clip=clip, case=cname, metric="cost_est_3090_ms",
                             candidate=cc[2], default=bb[2], note="ESTIMATE: laptop gpu_ms x %.2f (cost_ratios.json)" % ratio))
        log("  done in %.1f s" % (time.time() - t0))
    return rows


# ------------------------------------------------------------------ CLI
def cmd_run(a):
    with open(a.suite, "r", encoding="utf8") as f:
        suite = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    lines = []

    def log(s):
        print(s, flush=True)
        lines.append(s)
    cand, base = ec.load_candidate(a.candidate), ec.load_candidate(a.baseline)
    log("harness v%d  python %s  numpy %s" % (HARNESS_VERSION, platform.python_version(), np.__version__))
    log("suite %s  candidate %s (%s)  baseline %s (%s)" % (suite.get("name"), cand.name, cand.description,
                                                         base.name, base.description))
    t0 = time.time()
    rows = evaluate_suite(suite, cand, base, a.max_frames, a.lpips, not a.no_cost, log)
    scored = er.score_rows(rows)
    csv_text = er.to_csv(scored)
    extra = ["Suite: `%s` (%d cases). Candidate `%s`, default `%s`. LPIPS %s. Cost: %s." % (
        suite.get("name"), len(suite["cases"]), cand.name, base.name, "requested" if a.lpips else "not requested (NA)",
        "py-ref wall-clock on CPU (not GPU-representative)" if not a.no_cost else "omitted")]
    md = er.to_markdown(scored, cand.name, base.name, extra_lines=extra)
    with open(os.path.join(a.out, "results.csv"), "w", encoding="utf8", newline="") as f:
        f.write(csv_text)
    with open(os.path.join(a.out, "results.md"), "w", encoding="utf8", newline="\n") as f:
        f.write(md)
    counts = {}
    for r in scored:
        counts[r["flag"]] = counts.get(r["flag"], 0) + 1
    log("rows %d  flags %s  total %.1f s" % (len(scored), json.dumps(counts, sort_keys=True), time.time() - t0))
    with open(os.path.join(a.out, "run_log.txt"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return 0


def cmd_catalog(a):
    import eval_material
    cat = eval_material.discover()
    out = a.out or os.path.join(HERE, "material_catalog.json")
    with open(out, "w", encoding="utf8", newline="\n") as f:
        json.dump(cat, f, indent=1, sort_keys=True)
        f.write("\n")
    print("catalog: %d entries -> %s" % (len(cat["entries"]), out))
    return 0


def cmd_align(a):
    with open(a.suite, "r", encoding="utf8") as f:
        suite = json.load(f)
    for case in suite["cases"]:
        if a.case and case["case"] != a.case and case.get("id") != a.case:
            continue
        ms = case["master"]
        s0 = ms.get("start", 0)
        lo = max(0, s0 - a.span)
        n = (s0 - lo) + a.span + 1
        cf = load_source(case["coded"], 1)[0]
        mf = load_source(ms, n, start=lo)
        r = ep.verify_alignment(cf, mf, lo)
        print(json.dumps(dict(clip=case["clip"], case=case["case"], coded_first_frame=case["coded"].get("start", 0),
                              suite_master_start=s0, best_master_offset=r["best_offset"],
                              psnr_by_offset={str(o): ("inf" if p == float("inf") else round(p, 2))
                                              for o, p in zip(r["offsets"], r["psnr"])}), sort_keys=True))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--suite", required=True)
    r.add_argument("--candidate", required=True)
    r.add_argument("--baseline", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--max-frames", type=int, default=None)
    r.add_argument("--lpips", action="store_true")
    r.add_argument("--no-cost", action="store_true", help="omit the (non-deterministic) cost rows")
    r.set_defaults(fn=cmd_run)
    c = sub.add_parser("catalog")
    c.add_argument("--out", default=None)
    c.set_defaults(fn=cmd_catalog)
    al = sub.add_parser("align")
    al.add_argument("--suite", required=True)
    al.add_argument("--case", default=None)
    al.add_argument("--span", type=int, default=6, help="master offsets scanned: start-span .. start+span")
    al.set_defaults(fn=cmd_align)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
