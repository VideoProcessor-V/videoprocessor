# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Stack runner for the S2 shadow-ramp suite (CPU, numpy/scipy, single-threaded, deterministic).

  python -I tools/eval/stack_runner.py --suite s2 [--out DIR] [--size-frac F] [--max-cases N] [--contexts a,b]

Evaluation contexts (the delivered signal always ends as an 8-bit limited-range link, FL35 style):
  sdr         S2 "sdr" cases; stages 1, 2 on the SDR code signal; no tone mapping (stage 3 not applicable).
  hdr_pq      S2 "hdr_pq_1000" cases, PQ passthrough to a 600 nit display (stage 3 not applicable).
  hdr_to_sdr  the same PQ cases tone mapped to 100 nit SDR: plain = tonemap_ref static spline ("global",
              FL35 deploy settings: peak detection off); reference = the master through the same static curve.
Stand-in stages (signal domain, before tone mapping, in VP order):
  s1  texture re-injection oracle  x - fine(x) + fine(master)   (deband_stack.pre_variant "oracle": TemporalDetail upper bound)
  s2  VP deband model              deband_stack.deband_vp (threshold 1.5 + 6 codes HDR, 6 + 6 SDR; ref height = picture height)
  s3  grain-safe contrast recovery tonemap_ref ToneMapper("cr_gs", cr 0.30)  (hdr_to_sdr only)
  s4  sigma-delta 8-bit final      sigma_delta_ref.quantise_sequence (amplitude 0.15)
Final conversion is ALWAYS 8-bit: variant "sd" = sigma-delta (stage 4; config name gets "+4"), variant "nd" =
plain 8-bit rounding without dither (scale reference).  Metrics are computed in display units (light_metrics),
tiles come from the master.  Texture kept and the grain ratio compare with the plain chain of the same context AND the same final step.
"""
import argparse
import csv
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import deband_stack as ds  # noqa: E402
import eval_metrics as em  # noqa: E402
import light_level_ref as llr  # noqa: E402
import light_metrics as lm  # noqa: E402
import s2_synth  # noqa: E402
import sigma_delta_ref as sd  # noqa: E402
import tonemap_ref as tr  # noqa: E402

SD_AMPLITUDE = 0.15
GRAIN_MIN_RMS = 0.2   # display codes: below this the master has no grain to compare with
TEX_MIN_PROJ = 0.25   # plain must keep at least this projection of the master texture, else the ratio is unreliable
RANGES = dict(lm.SHADOW_RANGES, all=(None, 10000.0))
STAGE_SETS = [(), (1,), (2,), (3,), (1, 2), (1, 2, 3)]
RUN_CONTEXTS = {  # name -> (S2 context, evaluation kind)
    "sdr": ("sdr", "sdr"),
    "hdr_pq": ("hdr_pq_1000", "hdr_passthrough"),
    "hdr_to_sdr": ("hdr_pq_1000", "hdr_to_sdr"),
}
SDR_EVAL = llr.Context(kind="sdr", target_white=s2_synth.SUITE_WHITE, target_black=s2_synth.SUITE_BLACK)
COLUMNS = ["case", "run_context", "config", "final", "band_all_mse", "band_all_p99", "band_lt5_mse", "band_lt5_p99",
           "band_5to50_mse", "band_5to50_p99", "tex_lt5", "tex_5to50", "grain_rms_ratio", "grain_ratio_master", "grain_rms_out",
           "btb_kept", "wtw_kept", "na"]


def load_suite(name):
    with open(os.path.join(HERE, "suites", {"s2": "s2_shadow_ramps.json"}[name]), encoding="utf8") as f:
        return json.load(f)


def select_cases(suite, run_contexts, max_cases):
    want = {RUN_CONTEXTS[c][0] for c in run_contexts}
    cases = [c for c in suite["cases"] if c["master"]["params"]["context"] in want]
    if max_cases and max_cases < len(cases):  # evenly spaced, deterministic
        idx = np.unique(np.rint(np.linspace(0, len(cases) - 1, max_cases)).astype(int))
        cases = [cases[i] for i in idx]
    return cases


def config_name(stages, final):
    return ("+".join(str(s) for s in stages) or "plain") + ("+4" if final == "sd" else "")


def apply_signal_stages(stages, coded, master, hdr):
    x = np.stack(coded)
    if 1 in stages:
        x = ds.pre_variant("oracle", x, master, hdr)
    if 2 in stages:
        thr = (1.5 if hdr else 6.0) + 6.0
        x = np.stack([ds.deband_vp(f, thr, x.shape[1], t) for t, f in enumerate(x)])
    return x


def tone_map(kind, x, peak):
    m = tr.ToneMapper(kind, peak, x.shape[1])
    return np.stack([m.process(f) for f in x])


def finalise(x, final):
    """float 10-bit-scale codes -> 8-bit link (returned on the 10-bit scale, float)."""
    q = sd.quantise_sequence(x / 4.0, 8, False, SD_AMPLITUDE) if final == "sd" else sd.quantise_round(x / 4.0, 8, False)
    return q.astype(np.float64) * 4.0


def grain_stats(master, plain, cand, ctx, lut, tile=None):
    """Grain over shadow (<50 nit) pixels away from edges, residual rms in display codes.
    grain_rms_ratio = rms(cand residual) / rms(plain residual), plain = the plain chain WITH THE SAME FINAL STEP
    (None when the master or plain has no residual above GRAIN_MIN_RMS); grain_ratio_master = cand/master (absolute);
    grain_rms_out = rms of the cand residual.  Returns (ratio_vs_plain, ratio_vs_master, out)."""
    M, P, C = (lm.display_planes(a, ctx, lut) for a in (master, plain, cand))
    reg = lm.region_tiles(master, ctx, None, 50.0, lut, tile)
    sm = sp = so = 0.0
    n = 0
    for m, p, c, rg in zip(M, P, C, reg):
        sel = rg & em.near_edge_zone(m, 10)[1]
        if not sel.any():
            continue
        sm += float(np.sum(em.grain_residual(m)[sel] ** 2))
        sp += float(np.sum(em.grain_residual(p)[sel] ** 2))
        so += float(np.sum(em.grain_residual(c)[sel] ** 2))
        n += int(sel.sum())
    if n < lm.MIN_PX:
        return None, None, None
    out, ref, pl = (float(np.sqrt(v / n)) for v in (so, sm, sp))
    has = ref > GRAIN_MIN_RMS
    return (out / pl if has and pl > GRAIN_MIN_RMS else None), (out / ref if has else None), out


def evaluate(master_eval, plain_eval, cand, ctx, lut):
    """plain_eval = the plain chain with the SAME final step as cand (nd vs plain nd; sd vs plain+4)."""
    na = []
    row = {}
    b = lm.shadow_banding(master_eval, cand, ctx, RANGES, lut)
    for k in ("all", "lt5", "5to50"):
        row["band_%s_mse" % k], row["band_%s_p99" % k] = b[k]["band_mse"], b[k]["step_p99"]
        if b[k]["na_reason"]:
            na.append("band_%s: %s" % (k, b[k]["na_reason"]))
    for k, (lo, hi) in (("lt5", (None, 5.0)), ("5to50", (5.0, 50.0))):
        t = lm.shadow_texture_kept(master_eval, plain_eval, cand, ctx, lo, hi, lut=lut)
        row["tex_" + k] = t["ratio"]
        if t["ratio"] is not None and t["proj_plain"] < TEX_MIN_PROJ:
            row["tex_" + k] = None
            na.append("tex_%s: plain keeps only %.3f of the master texture (< %g)" % (k, t["proj_plain"], TEX_MIN_PROJ))
        if t["na_reason"]:
            na.append("tex_%s: %s" % (k, t["na_reason"]))
    row["grain_rms_ratio"], row["grain_ratio_master"], row["grain_rms_out"] = grain_stats(
        master_eval, plain_eval, cand, ctx, lut)
    bw = em.btb_wtw(master_eval, cand, 10)
    row["btb_kept"] = lm.btb_kept(master_eval, cand)[0]
    row["wtw_kept"] = bw["wtw_kept"]
    row["na"] = "; ".join(na)
    return row


def run_case(case, run_contexts, log):
    p = case["master"]["params"]
    made = s2_synth.make_case(p)
    master = np.stack([f.Y for f in made["master"]])
    coded = [f.Y for f in made["coded"]]
    rows = []
    for rc in run_contexts:
        cname, kind = RUN_CONTEXTS[rc]
        if cname != p["context"]:
            continue
        hdr = kind != "sdr"
        peak = made["context"].src_peak
        if kind == "hdr_to_sdr":
            ref = tone_map("global", master, peak)
            ctx = SDR_EVAL
        else:
            ref, ctx = master, made["context"] if kind == "hdr_passthrough" else SDR_EVAL
        lut = llr.build_lut(ctx)
        plain_by_final = {}
        for stages in STAGE_SETS:
            if 3 in stages and kind != "hdr_to_sdr":
                continue
            x = apply_signal_stages(stages, coded, master, hdr)
            if kind == "hdr_to_sdr":
                x = tone_map("cr_gs" if 3 in stages else "global", x, peak)
            for final in ("nd", "sd"):
                out = finalise(x, final)
                if not stages:
                    plain_by_final[final] = out
                r = evaluate(ref, plain_by_final[final], out, ctx, lut)
                r.update({"case": case["id"], "run_context": rc, "config": config_name(stages, final), "final": final})
                rows.append(r)
        log("  %s %s done" % (case["id"], rc))
    return rows


def fmt(v, nd=4):
    return "" if v is None else ("%.*g" % (nd, v))


def write_csv(rows, path):
    with open(path, "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(COLUMNS)
        for r in rows:
            w.writerow([r[c] if isinstance(r[c], str) else fmt(r[c]) for c in COLUMNS])


def mean(rows, key):
    v = [r[key] for r in rows if r[key] is not None]
    return float(np.mean(v)) if v else None


def write_md(rows, path, args, n_cases, secs, log_lines):
    cols = [("band_all_mse", "band all"), ("band_lt5_mse", "band <5 nit"), ("band_5to50_mse", "band 5-50 nit"),
            ("band_lt5_p99", "step p99 <5"), ("tex_lt5", "texture kept <5"), ("tex_5to50", "texture kept 5-50"),
            ("grain_rms_out", "grain rms out"), ("grain_rms_ratio", "grain ratio vs plain"), ("grain_ratio_master", "grain ratio vs master"), ("btb_kept", "btb"), ("wtw_kept", "wtw")]
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# S2 shadow-ramp stack run", "",
          "Generated by `python -I tools/eval/stack_runner.py --suite s2 --size-frac %g --max-cases %s --contexts %s`." %
          (args.size_frac, args.max_cases, args.contexts), "",
          "**Run size.** %s at size_frac %g (%dx%d px windows, 2 frames); single-threaded CPU %.0f s. %s" % (
              ("%d S2 cases (every case the runner supports: the sdr and hdr_pq_1000 contexts, all grain levels)" % n_cases
               if not args.max_cases else
               "%d S2 cases (evenly spaced subset, deterministic)" % n_cases),
              args.size_frac, int(round(2160 * args.size_frac * 16 / 9)), int(round(2160 * args.size_frac)), secs,
              "The suite's hdr_pq_4000 and lldv_* contexts have no runner context and are not run." if not args.max_cases
              else "Full run: `--max-cases 0`."), "",
          "Stand-ins: s1 texture oracle, s2 VP deband model, s3 cr_gs contrast recovery (HDR->SDR only), s4 sigma-delta. "
          "`nd` = 8-bit rounding, no dither (scale reference); `+4` = sigma-delta final (amplitude 0.15). Every row ends "
          "8-bit. band = mean-square coarse error in display PQ codes (lower is better); texture kept = projection "
          "ratio vs the plain chain with the same final step (nd vs plain nd, +4 vs plain+4; 1 = as plain, gate 0.97); "
          "grain ratio vs plain = cand/plain(same final) residual rms in display codes, vs master = cand/master (blank "
          "when the master has no grain); texture kept is blank when plain keeps < 0.25 of the master texture; btb = share "
          "of master pixels below black by more than half an output code step (2 ten-bit codes) whose output is still "
          "below black; wtw = share of master above-white pixels still above white. Values are means over the cases whose metric is defined; blanks = not applicable. "
          "Synthetic ramps contain no above-white; sigma-delta amplitude, stand-in strengths and the 2-frame length are "
          "deliberately simple, so read the tables as relative ordering only. The SDR coded input is already on the 8-bit "
          "grid, so plain+4 equals plain there (sigma-delta of an exact 8-bit signal is a no-op); texture-kept ratios "
          "are noisy where plain keeps little of the master texture.", ""]
    for rc in RUN_CONTEXTS:
        rs = [r for r in rows if r["run_context"] == rc]
        if not rs:
            continue
        md += ["## %s" % rc, "", "| config | " + " | ".join(c[1] for c in cols) + " |",
               "|---|" + "---|" * len(cols)]
        for name in dict.fromkeys(r["config"] for r in rs):
            g = [r for r in rs if r["config"] == name]
            md.append("| %s | %s |" % (name, " | ".join(fmt(mean(g, c[0]), 3) for c in cols)))
        md.append("")
    md += ["## Banding <5 nit by grain level (mean band mse)", "", "| run context | config | grain 0 | grain 0.5 | grain 1.5 |",
           "|---|---|---|---|---|"]
    for rc in RUN_CONTEXTS:
        rs = [r for r in rows if r["run_context"] == rc]
        for name in dict.fromkeys(r["config"] for r in rs):
            cells = []
            for g in ("g0", "g0p5", "g1p5"):
                sel = [r for r in rs if r["config"] == name and r["case"].endswith("-" + g)]
                cells.append(fmt(mean(sel, "band_lt5_mse"), 3))
            md.append("| %s | %s | %s |" % (rc, name, " | ".join(cells)))
    md += [""]
    cfgs = lambda rs: list(dict.fromkeys(r["config"] for r in rs))
    for band, title in (("band_5to50_mse", "SDR / display shadow banding 5-50 nit"), ("band_lt5_mse", "Shadow banding < 5 nit")):
        md += ["## %s, per case (band mse, candidate vs plain; lower is better)" % title, ""]
        for rc in RUN_CONTEXTS:
            rs = [r for r in rows if r["run_context"] == rc]
            if not rs:
                continue
            cf = cfgs(rs)
            md += ["### %s" % rc, "", "| case | " + " | ".join(cf) + " |", "|---|" + "---|" * len(cf)]
            for case in dict.fromkeys(r["case"] for r in rs):
                cell = {r["config"]: r[band] for r in rs if r["case"] == case}
                md.append("| %s | %s |" % (case, " | ".join(fmt(cell.get(c), 3) for c in cf)))
            md.append("")
    md += ["## Stage-2 result (deband model), band 5-50 nit mse relative to the plain chain with the same final step", "",
           "| run context | config | cases | improved (ratio < 1) | ratio min | ratio mean | ratio max |", "|---|---|---|---|---|---|---|"]
    for rc in RUN_CONTEXTS:
        rs = [r for r in rows if r["run_context"] == rc]
        for name in cfgs(rs):
            if name.split("+")[0] == "plain" or "2" not in name.replace("+4", "").split("+"):
                continue
            ratios = []
            for r in rs:
                if r["config"] != name or r["band_5to50_mse"] is None:
                    continue
                pl = [q for q in rs if q["case"] == r["case"] and q["config"] == ("plain+4" if r["final"] == "sd" else "plain")]
                if pl and pl[0]["band_5to50_mse"]:
                    ratios.append(r["band_5to50_mse"] / pl[0]["band_5to50_mse"])
            if ratios:
                md.append("| %s | %s | %d | %d | %.3g | %.3g | %.3g |" % (rc, name, len(ratios), sum(x < 1 for x in ratios),
                                                                       min(ratios), float(np.mean(ratios)), max(ratios)))
    md += ["", "Per-case values: `stack_s2.csv`; run log: `run_log.txt`.", ""]
    with open(path, "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", default="s2", choices=["s2"])
    ap.add_argument("--out", default=os.path.join(HERE, "prototypes", "stack_s2"))
    ap.add_argument("--size-frac", "--crop", dest="size_frac", type=float, default=0.125,
                    help="picture height as a fraction of 2160 p (default 0.125 = 480x270)")
    ap.add_argument("--max-cases", type=int, default=6, help="evenly spaced subset of the suite (0 = all)")
    ap.add_argument("--contexts", default="sdr,hdr_pq,hdr_to_sdr")
    args = ap.parse_args(argv)
    run_contexts = [c for c in args.contexts.split(",") if c]
    for c in run_contexts:
        if c not in RUN_CONTEXTS:
            ap.error("unknown context %r" % c)
    os.makedirs(args.out, exist_ok=True)
    log_lines = []

    def log(s):
        log_lines.append(s)
        print(s, flush=True)

    t0 = time.process_time()
    suite = load_suite(args.suite)
    cases = select_cases(suite, run_contexts, args.max_cases)
    log("suite %s: %d cases selected, size_frac %g, contexts %s" % (suite["name"], len(cases), args.size_frac, run_contexts))
    rows = []
    for c in cases:
        c = json.loads(json.dumps(c))
        c["master"]["params"]["size_frac"] = args.size_frac
        rows += run_case(c, run_contexts, log)
    secs = time.process_time() - t0
    log("rows %d, cpu %.1f s" % (len(rows), secs))
    write_csv(rows, os.path.join(args.out, "stack_s2.csv"))
    write_md(rows, os.path.join(args.out, "stack_s2.md"), args, len(cases), secs, log_lines)
    with open(os.path.join(args.out, "run_log.txt"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(log_lines) + "\n")
    return rows


if __name__ == "__main__":
    main()
