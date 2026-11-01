# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Side-by-side summary of TemporalDetail real-encode runs (run_prototypes.py --family temporal_detail).

  python -I tools/eval/td_real_summary.py --runs <dir1> <dir2+rerun_dir> ... --out tools/eval/prototypes/temporal_detail_real/summary.md

Each run dir holds one `<candidate>.csv` (harness rows, candidate vs td_off) and `run_log.txt` (per-case model
statistics). Gains are candidate minus default (td_off = the decoded input). Gates: every input-relative grain row,
the near-edge probe, ghost/smear/flicker/halo, BTB/WTW vs input; any FLAG/FAIL is listed.
"""
import argparse
import csv
import glob
import math
import os
import re
import sys

GATES = ["grain_keep_vs_input_Y", "grain_lag1_vs_input_Y"] + ["grain_keep_vs_input_band%d_Y" % k for k in range(1, 5)] + [
    "grain_probe_keep_edge_vs_input_Y", "grain_probe_lag1_edge_vs_input_Y", "grain_probe_keep_flat_vs_input_Y",
    "grain_probe_lag1_flat_vs_input_Y", "ghost_coeff_Y", "smear_Y", "flicker_excess_Y", "halo_overshoot_Y",
    "btb_kept_vs_input_Y", "wtw_kept_vs_input_Y", "gop_ratio_minigop", "gop_ratio_gop", "duplicate_rate_Y"]


def num(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def load_one(run):
    files = [f for f in glob.glob(os.path.join(run, "*.csv")) if os.path.basename(f) != "summary.csv"]
    if len(files) != 1:
        raise SystemExit("%s: expected one candidate csv, found %s" % (run, files))
    name = os.path.splitext(os.path.basename(files[0]))[0]
    rows = {}
    with open(files[0], encoding="utf8") as f:
        for r in csv.DictReader(f):
            rows.setdefault((r["content_type"], r["clip"], r["case"]), {})[r["metric"]] = r
    # run_log "stats" lines: one per candidate reset that produced frames, in case order; with the grain probe a
    # case has three (the scored run first, then the two probe re-runs). A failed case leaves an unknown number,
    # so cases before the first failure are mapped from the front and cases after it from the back.
    entries = []
    log = os.path.join(run, "run_log.txt")
    if os.path.isfile(log):
        for line in open(log, encoding="utf8"):
            m = re.match(r"stats \S+ case-start-index \S+: (.*)", line)
            if m:
                entries.append(dict(re.findall(r"(\w+) mean ([-0-9.e+]+)", m.group(1))))
    keys = list(rows)
    err = [k for k in keys if "error_candidate" in rows[k]]
    per = [3 if "grain_probe_keep_flat_vs_input_Y" in rows[k] else 1 for k in keys]
    stats = {}
    if not err:
        if sum(per) == len(entries):
            pos = 0
            for k, n in zip(keys, per):
                stats[k] = entries[pos]
                pos += n
    elif len(err) == 1:
        e = keys.index(err[0])
        pos = 0
        for k, n in zip(keys[:e], per[:e]):
            stats[k] = entries[pos]
            pos += n
        pos = len(entries)
        for k, n in reversed(list(zip(keys[e + 1:], per[e + 1:]))):
            pos -= n
            stats[k] = entries[pos]
    return name, rows, stats


def load(spec):
    """A run spec is `dir` or `dir+rerun_dir+...`: later dirs replace the cases they contain (re-runs)."""
    name, rows, stats = None, {}, {}
    for d in spec.split("+"):
        n, r, st = load_one(d)
        name = name or "%s (%s)" % (os.path.basename(os.path.normpath(d)), n)
        for k in r:
            rows[k] = r[k]
            stats.pop(k, None)
            if k in st:
                stats[k] = st[k]
    return name, rows, stats


def fmt(v, p=3, sign=False):
    if v is None:
        return "NA"
    return ("%+.*f" if sign else "%.*f") % (p, v)


def sweep_table(dirs, out, title):
    """One row per candidate csv found in `dirs` (several candidates per run dir): means over cases of the gains,
    the I/B gap change, the transfer's activity (stats), and the gate verdicts (worst value and non-PASS count)."""
    rows = []
    for d in dirs:
        entries_by = {}
        log = os.path.join(d, "run_log.txt")
        if os.path.isfile(log):
            for line in open(log, encoding="utf8"):
                m = re.match(r"stats (\S+) case-start-index \S+: (.*)", line)
                if m:
                    entries_by.setdefault(m.group(1), []).append(dict(re.findall(r"(\w+) mean ([-0-9.e+]+)", m.group(2))))
        for f in sorted(glob.glob(os.path.join(d, "*.csv"))):
            name = os.path.splitext(os.path.basename(f))[0]
            if name == "summary":
                continue
            by = {}
            with open(f, encoding="utf8") as fh:
                for r in csv.DictReader(fh):
                    by.setdefault((r["clip"], r["case"]), {})[r["metric"]] = r

            def mean_delta(metric):
                v = [num(c[metric]["delta"]) for c in by.values() if metric in c and num(c[metric]["delta"]) is not None]
                return sum(v) / len(v) if v else None

            def worst(metric, ref):
                v = [num(c[metric]["candidate"]) for c in by.values() if metric in c and num(c[metric]["candidate"]) is not None]
                return max(v, key=lambda x: abs(x - ref)) if v else None
            bad = sum(1 for c in by.values() for g in GATES if g in c and c[g]["flag"] in ("FAIL", "FLAG"))
            errs = sum(1 for c in by.values() for m in c if m.startswith("error_"))
            st = entries_by.get(name, [])
            w = [num(e.get("weight_mean")) for e in st if num(e.get("weight_mean")) is not None]
            act = [num(e.get("active_share")) for e in st if num(e.get("active_share")) is not None]
            rows.append((name, len(by), mean_delta("psnr_B_Y"), mean_delta("psnr_Y"), mean_delta("ssim_B_Y"),
                         mean_delta("lpips_all"), mean_delta("ib_gap_db_Y"), sum(w) / len(w) if w else None,
                         sum(act) / len(act) if act else None, worst("grain_keep_vs_input_Y", 1.0),
                         worst("grain_lag1_vs_input_Y", 0.0),
                         max([worst("grain_keep_vs_input_band%d_Y" % k, 1.0) or 1.0 for k in range(1, 5)],
                             key=lambda x: abs(x - 1.0)),
                         worst("grain_probe_keep_edge_vs_input_Y", 1.0), mean_delta("ghost_coeff_Y"),
                         mean_delta("smear_Y"), bad, errs))
    L = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
         "# " + title, "",
         "One row per candidate; means over cases of candidate - default (td_off). `w` = mean block weight, `act` = share of "
         "blocks with weight > 0.05 (scored run). Worst = the case value farthest from neutral. Gates = non-PASS flags among "
         "the grain/ghost/smear/flicker/halo/BTB-WTW/GOP rows (0 = all PASS).", "",
         "| candidate | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS | d gap | w | act | worst grain keep | worst lag-1 | "
         "worst band | worst probe edge | d ghost | d smear | gates non-PASS | errors |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        L.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %d | %d |" % (
            r[0], r[1], fmt(r[2], 3, True), fmt(r[3], 3, True), fmt(r[4], 5, True), fmt(r[5], 4, True),
            fmt(r[6], 3, True), fmt(r[7], 4), fmt(r[8], 3), fmt(r[9], 4), fmt(r[10], 4), fmt(r[11], 4), fmt(r[12], 4),
            fmt(r[13], 4, True), fmt(r[14], 4, True), r[15], r[16]))
    with open(out, "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("\n".join(L))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", nargs="+", default=None)
    ap.add_argument("--sweep", nargs="+", default=None, help="run dirs with several candidates: one row per candidate")
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default="TemporalDetail on real streaming-like encodes")
    a = ap.parse_args(argv)
    if a.sweep:
        sweep_table(a.sweep, a.out, a.title)
        return 0
    runs = [load(r) for r in a.runs]
    L = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
         "# " + a.title, "",
         "Default = `td_off` (the decoded input). Deltas = candidate - default; dB for PSNR. `gap` = PSNR(I+P) - "
         "PSNR(B) (the I/B pulse), `pulse` = frame-to-frame CV of fine-band energy out/master (breathing). "
         "Gates = input-relative grain (RMS, lag-1, four temporal bands), near-edge two-seed probe, ghost, smear, "
         "flicker, halo, BTB/WTW, GOP ratios: non-PASS flags listed. CPU model ms = td_cli Process() per 960x544 "
         "frame (one thread, double; NOT GPU-representative). Vectors: DIS-flow stand-in. Sections are named after the "
         "run directory; `a+b` merges a re-run. ERROR rows = the case ran out of memory while six runs shared the "
         "laptop (re-run serially where listed with +).", ""]
    for name, rows, stats in runs:
        keys = list(rows)
        L += ["## %s" % name, "",
              "| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | "
              "pulse in -> out | weight | model ms | gates |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        agg = {}
        for i, k in enumerate(keys):
            r = rows.get(k, {})

            def d(m):
                x = r.get(m)
                return num(x["delta"]) if x else None

            def cd(m):
                x = r.get(m)
                return (num(x["default"]), num(x["candidate"])) if x else (None, None)
            bad = ["%s:%s" % (g, r[g]["flag"]) for g in GATES if g in r and r[g]["flag"] in ("FAIL", "FLAG")]
            errs = [m for m in r if m.startswith("error_")]
            gi, go = cd("ib_gap_db_Y")
            pi, po = cd("detail_pulse_Y")
            st = stats.get(k, {})
            w, ms = num(st.get("weight_mean")), num(st.get("ms"))
            vals = dict(B=d("psnr_B_Y"), P=d("psnr_P_Y"), I=d("psnr_I_Y"), all=d("psnr_Y"), sB=d("ssim_B_Y"),
                        lB=d("lpips_B"), lA=d("lpips_all"), gi=gi, go=go, pi=pi, po=po, w=w, ms=ms)
            for kk, v in vals.items():
                if v is not None:
                    agg.setdefault(k[0], {}).setdefault(kk, []).append(v)
                    agg.setdefault("ALL", {}).setdefault(kk, []).append(v)
            gates = "ERROR " + ",".join(errs) if errs else ("PASS" if not bad else " ".join(bad))
            L.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s -> %s | %s -> %s | %s | %s | %s |" % (
                k[0], k[1], k[2], fmt(vals["B"], 3, True), fmt(vals["P"], 3, True), fmt(vals["I"], 3, True),
                fmt(vals["all"], 3, True), fmt(vals["sB"], 5, True), fmt(vals["lB"], 4, True) + " / " + fmt(vals["lA"], 4, True), fmt(gi, 2), fmt(go, 2),
                fmt(pi, 3), fmt(po, 3), fmt(w, 4), fmt(ms, 0), gates))
        L += ["", "Means by content type:", "",
              "| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |",
              "|---|---|---|---|---|---|---|---|---|---|"]
        for ct in sorted(agg, key=lambda c: (c == "ALL", c)):
            g = agg[ct]

            def m(kk):
                v = g.get(kk)
                return sum(v) / len(v) if v else None
            L.append("| %s | %d | %s | %s | %s | %s | %s -> %s | %s -> %s | %s | %s |" % (
                ct, len(g.get("all", [])), fmt(m("B"), 3, True), fmt(m("all"), 3, True), fmt(m("sB"), 5, True),
                fmt(m("lB"), 4, True) + " / " + fmt(m("lA"), 4, True), fmt(m("gi"), 2), fmt(m("go"), 2), fmt(m("pi"), 3), fmt(m("po"), 3),
                fmt(m("w"), 4), fmt(m("ms"), 0)))
        L.append("")
        lbm = ["lb_bar_change_max_Y", "lb_band_change_rms_Y", "lb_band_overshoot_mean_Y", "lb_band_undershoot_max_Y"]
        lbk = [k for k in keys if lbm[0] in rows.get(k, {})]
        if lbk:
            L += ["Active-picture rectangle (letterbox; codes vs the input; candidate (default)): bar change max must "
                  "be 0, band = +-16 rows around the picture/bar boundary, overshoot/undershoot vs the input's 5x5 "
                  "local range.", "", "| clip | case | " + " | ".join(lbm) + " |", "|---|---|" + "---|" * len(lbm)]
            for k in lbk:
                r = rows[k]
                L.append("| %s | %s | %s |" % (k[1], k[2], " | ".join(
                    "%s (%s)" % (fmt(num(r[x]["candidate"]), 4), fmt(num(r[x]["default"]), 4)) for x in lbm)))
            L.append("")
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("\n".join(L))
    return 0


if __name__ == "__main__":
    sys.exit(main())
