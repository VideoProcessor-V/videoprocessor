# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Merge per-run VP_MOTION_BENCH csv files into summary.csv + summary.md.

  python -I summarize_bench.py --in DIR --out DIR [--scores DIR]

Run files are named <clip>__<config>.csv in --in. Optional <clip>__<config>.scores.csv (score_flow output) in --scores
(default --in) fills the epe_* columns.
"""
import argparse
import csv
import os
import sys

SUMMARY_COLS = ["clip", "config", "pairs", "narrow_pct", "escalated_pct", "skipped_pct", "gpu_ms_mean",
                "epe_all", "epe_camera", "epe_object"]


def _f(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return 0.0


def summarize_run(path):
    n = narrow = full = esc = skip = 0
    gpu = 0.0
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            n += 1
            narrow += _f(r.get("narrow"))
            full += _f(r.get("full"))
            esc += _f(r.get("escalated"))
            skip += _f(r.get("skipped"))
            gpu += _f(r.get("gpu_ms"))
    tot = narrow + full + skip

    def pct(v):
        return "%.3f" % (100.0 * v / tot) if tot > 0 else ""

    return n, pct(narrow), pct(esc), pct(skip), ("%.4f" % (gpu / n) if n else "")


def read_scores(path):
    out = {}
    if os.path.isfile(path):
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                out[r["metric"]] = r["value"]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="indir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--scores")
    a = ap.parse_args(argv)
    sdir = a.scores or a.indir
    rows = []
    for name in sorted(os.listdir(a.indir)):
        if not name.endswith(".csv") or name.endswith(".scores.csv") or name.startswith("summary"):
            continue
        stem = name[:-4]
        clip, _, config = stem.partition("__")
        n, np_, ep, sp, gm = summarize_run(os.path.join(a.indir, name))
        sc = read_scores(os.path.join(sdir, stem + ".scores.csv"))
        rows.append([clip, config or "unknown", n, np_, ep, sp, gm,
                     sc.get("all_epe", ""), sc.get("camera_epe", ""), sc.get("object_epe", "")])
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "summary.csv"), "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(SUMMARY_COLS)
        w.writerows(rows)
    with open(os.path.join(a.out, "summary.md"), "w") as f:
        f.write("# Motion bench summary\n\n| " + " | ".join(SUMMARY_COLS) + " |\n|" + "---|" * len(SUMMARY_COLS) + "\n")
        for r in rows:
            f.write("| " + " | ".join(str(x) for x in r) + " |\n")
    print("%d runs -> %s" % (len(rows), a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
