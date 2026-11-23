# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Before/after vector-error table for a registered FIX (MAP-RGB-LUMA-PROXY, OD 11-15 11:11 (3)).

Usage: python -I tools/vector_error_table.py BEFORE.json AFTER.json [--markdown]

Each input is a JSON object produced by a GPU/replay run of the motion producer on a fixed clip set, one entry per
source format (e.g. "RGB444_8", "RGB444_10_full", "YUV422_10_full", "P010_limited"):

    { "<format>": { "frames": N, "mean_epe": x, "p95_epe": y, "valid_fraction": z }, ... }

mean_epe / p95_epe are end-point errors in pixels against the reference field of the same clip (ground truth or the
YUV420-limited run of the same content); valid_fraction is the share of blocks with a usable vector. The script only
compares two result files; it does not run the GPU. It prints one row per format with before, after and the delta,
and exits 0 always unless an input is unreadable (2) or the format sets differ (1).
"""
import argparse
import json
import sys

FIELDS = ("mean_epe", "p95_epe", "valid_fraction")


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(path + ": expected a JSON object keyed by format")
    return data


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--markdown", action="store_true")
    a = ap.parse_args(argv)
    try:
        before, after = load(a.before), load(a.after)
    except (OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    if set(before) != set(after):
        print("error: format sets differ: before-only %s, after-only %s"
              % (sorted(set(before) - set(after)), sorted(set(after) - set(before))), file=sys.stderr)
        return 1
    sep = " | " if a.markdown else "  "
    head = ["format", "frames"] + [x + "(b>a,d)" for x in FIELDS]
    rows = [head]
    for fmt in sorted(before):
        b, c = before[fmt], after[fmt]
        row = [fmt, str(c.get("frames", b.get("frames", "?")))]
        for k in FIELDS:
            if k in b and k in c:
                row.append("%.4f>%.4f,%+.4f" % (b[k], c[k], c[k] - b[k]))
            else:
                row.append("n/a")
        rows.append(row)
    for i, row in enumerate(rows):
        print((("| " + sep.join(row) + " |") if a.markdown else sep.join(row)))
        if a.markdown and i == 0:
            print("|" + "|".join(" --- " for _ in row) + "|")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
