# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Score several Python reference candidates against one default in a single pass.

  python -I tools/eval/run_prototypes.py --family chroma --suite tools/eval/suites/proto_chroma.json \
         --baseline chroma_bicubic --candidates chroma_jbu,chroma_krig,chroma_cfl,chroma_mv \
         --out D:/codex/videoprocessor/eval-scratch/feat-eval-prototypes-chroma [--lpips] [--max-frames N]

Each case is loaded once and the default is run once (eval_harness.evaluate_suite with a shared cache);
every candidate gets the standard harness rows plus the family's extra rows
(candidates_chroma.extra_metrics_chroma / candidates_mosquito.extra_metrics_mosquito).
Writes per candidate `<name>.csv` / `<name>.md` (the harness format), `summary.csv` / `summary.md`
(key metrics, all candidates side by side) and `run_log.txt` (incl. per-case candidate statistics).
"""
import argparse
import json
import math
import os
import platform
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import eval_candidates as ec  # noqa: E402
import eval_harness as eh  # noqa: E402
import eval_report as er  # noqa: E402

FAMILIES = {
    "chroma": ("candidates_chroma", "extra_metrics_chroma",
               ["psnr_Cb", "psnr_Cr", "ssim_Cb", "ssim_Cr", "psnr_edge_Cb", "psnr_edge_Cr", "lpips",
                "flicker_excess_Cb", "flicker_excess_Cr", "grain_rms_ratio_Cb", "btb_kept_C", "wtw_kept_C",
                "clip_detect_C", "cost_ms_per_frame_pyref"]),
    "mosquito": ("candidates_mosquito", "extra_metrics_mosquito",
                 ["grain_probe_keep_edge_vs_input_Y", "grain_probe_lag1_edge_vs_input_Y",
                  "grain_probe_keep_flat_vs_input_Y", "grain_probe_lag1_flat_vs_input_Y",
                  "ringing_energy_ratio_Y", "psnr_band_Y", "psnr_Y", "ssim_Y", "lpips", "halo_overshoot_Y",
                  "edge_keep_vs_input_Y", "grain_keep_vs_input_Y", "grain_lag1_vs_input_Y", "grain_hf_ratio_Y",
                  "grain_rms_ratio_Y", "ghost_coeff_Y", "smear_Y", "flicker_excess_Y", "btb_kept_vs_input_Y",
                  "wtw_kept_vs_input_Y", "cost_ms_per_frame_pyref"]),
}


class StatsProxy:
    """Wraps a Python candidate and records its per-case `stats` list (if it keeps one) at each reset."""
    kind = "python"

    def __init__(self, inner, sink):
        self.inner, self.sink = inner, sink
        self.name, self.description = inner.name, inner.description
        self.lookahead = getattr(inner, "lookahead", 0)
        self.case = None

    def flush(self):
        st = getattr(self.inner, "stats", None)
        if st and self.case is not None:
            self.sink.append((self.name, self.case, st))

    def reset(self, meta):
        self.flush()
        self.case = meta.get("index")
        self.inner.reset(meta)

    def process(self, frame, future=()):
        return self.inner.process(frame, future)


def load(family_mod, spec):
    if spec in getattr(family_mod, "CANDIDATES", {}):
        return family_mod.CANDIDATES[spec]()
    return ec.load_candidate(spec)


def summarise_stats(st):
    if not st:
        return ""
    if isinstance(st[0], dict):
        keys = sorted(st[0])
        return ", ".join("%s mean %.4g" % (k, float(np.mean([s[k] for s in st]))) for k in keys)
    return "mean %.4g" % float(np.mean(st))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--family", required=True, choices=sorted(FAMILIES))
    ap.add_argument("--suite", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidates", required=True, help="comma-separated names")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-frames", type=int, default=None)
    ap.add_argument("--lpips", action="store_true")
    ap.add_argument("--no-cost", action="store_true")
    a = ap.parse_args(argv)

    modname, extra_name, key_metrics = FAMILIES[a.family]
    mod = __import__(modname)
    extra = getattr(mod, extra_name)
    with open(a.suite, "r", encoding="utf8") as f:
        suite = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    lines = []

    def log(s):
        print(s, flush=True)
        lines.append(s)
    stats = []
    base = load(mod, a.baseline)
    log("run_prototypes  harness v%d  python %s  numpy %s" % (eh.HARNESS_VERSION, platform.python_version(), np.__version__))
    log("suite %s (%d cases)  family %s  default %s (%s)" % (suite.get("name"), len(suite["cases"]), a.family,
                                                           base.name, base.description))
    cache = {}
    all_scored = {}
    t_all = time.time()
    for spec in [s.strip() for s in a.candidates.split(",") if s.strip()]:
        cand = StatsProxy(load(mod, spec), stats)
        log("== candidate %s: %s" % (cand.name, cand.description))
        t0 = time.time()
        rows = eh.evaluate_suite(suite, cand, base, a.max_frames, a.lpips, not a.no_cost, log, cache=cache, extra=extra)
        cand.flush()
        scored = er.score_rows(rows)
        all_scored[cand.name] = scored
        extra_lines = ["Suite: `%s` (%d cases). Candidate `%s` (%s), default `%s`. LPIPS %s. Cost: %s." % (
            suite.get("name"), len(suite["cases"]), cand.name, cand.description, base.name,
            "requested (CPU, tile crops)" if a.lpips else "not requested (NA)",
            "py-ref wall-clock on CPU (not GPU-representative)" if not a.no_cost else "omitted")]
        with open(os.path.join(a.out, cand.name + ".csv"), "w", encoding="utf8", newline="") as f:
            f.write(er.to_csv(scored))
        with open(os.path.join(a.out, cand.name + ".md"), "w", encoding="utf8", newline="\n") as f:
            f.write(er.to_markdown(scored, cand.name, base.name, title="VP-V prototype evaluation",
                                   extra_lines=extra_lines))
        counts = {}
        for r in scored:
            counts[r["flag"]] = counts.get(r["flag"], 0) + 1
        log("   %s: rows %d flags %s  %.1f s" % (cand.name, len(scored), json.dumps(counts, sort_keys=True), time.time() - t0))
    for name, case, st in stats:
        log("stats %s case-start-index %s: %s" % (name, case, summarise_stats(st)))
    # side-by-side summary over key metrics
    names = list(all_scored)
    hdr = ["content_type", "clip", "case", "metric", "default"] + names + ["flags"]
    out_rows = []
    first = all_scored[names[0]] if names else []
    for r in first:
        if r["metric"] not in key_metrics:
            continue
        k = (r["content_type"], r["clip"], r["case"], r["metric"])
        vals, flags = [], []
        for n in names:
            m = [x for x in all_scored[n] if (x["content_type"], x["clip"], x["case"], x["metric"]) == k]
            vals.append(er.fmt(m[0]["candidate"]) if m else "NA")
            flags.append("%s:%s" % (n, m[0]["flag"]) if m else "%s:NA" % n)
        out_rows.append(list(k) + [er.fmt(r["default"])] + vals + [" ".join(flags)])
    with open(os.path.join(a.out, "summary.csv"), "w", encoding="utf8", newline="") as f:
        import csv
        w = csv.writer(f, lineterminator="\n")
        w.writerow(hdr)
        w.writerows(out_rows)
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Prototype summary: %s family, default `%s`" % (a.family, base.name), "",
          "Suite `%s`. Key metrics per case; flags are the harness verdicts vs the default "
          "(full rows in `<candidate>.md`)." % suite.get("name"), ""]
    # mean over cases per metric (finite numbers only)
    md.append("## Means over cases")
    md.append("")
    md.append("| metric | default | " + " | ".join(names) + " |")
    md.append("|---|---|" + "---|" * len(names))
    for km in key_metrics:
        rs = [r for r in out_rows if r[3] == km]
        if not rs:
            continue

        def mean(col):
            v = []
            for r in rs:
                try:
                    x = float(r[col])
                except ValueError:
                    continue
                if math.isfinite(x):
                    v.append(x)
            return er.fmt(sum(v) / len(v)) if v else "NA"
        md.append("| %s | %s | %s |" % (km, mean(4), " | ".join(mean(5 + i) for i in range(len(names)))))
    md.append("")
    md.append("## Per case")
    md.append("")
    md.append("| " + " | ".join(hdr) + " |")
    md.append("|" + "---|" * len(hdr))
    for r in out_rows:
        md.append("| " + " | ".join(str(x) for x in r) + " |")
    md.append("")
    with open(os.path.join(a.out, "summary.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))
    log("total %.1f s" % (time.time() - t_all))
    with open(os.path.join(a.out, "run_log.txt"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
