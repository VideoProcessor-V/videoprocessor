# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N4 general-not-fixed checker (doc 124 s2.2 N4; owner rule "never hard-code refresh/frame rates, cadences
or resolutions; derive from measured rates").

Usage: python -I tools/check_general_not_fixed.py [--root P] [--since REF] [--write-baseline] [--strict]

A literal ratchet over VP-V production code (src/VP-V, comments stripped, tests skipped):
  rate   23.976 / 29.97 / 59.94 anywhere, or 24 25 30 50 60 120 on a line that talks about
         fps / Hz / refresh / rate / cadence / frame
  size   720 1080 1440 1600 2160 2560 3840 4320 7680 on a line naming width / height / resolution / size / rows / cols / 4k
A hit is waived by "// GENERAL-REF: <why>" on the same or the previous line (declared reference constants,
e.g. ADR-0018 reference 2160 / 23.976, and standard-size tables). Remaining hits are counted per file against
tools/general-not-fixed-baseline.json: a file may not grow, an unlisted file has ceiling 0, baselines only
shrink. --since REF lists hits on lines added since REF as NEW. Exit 0 / 1 violation / 2 error.
"""
import importlib.util
import os
import re
import sys


def _lib():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compliance_lib.py")
    s = importlib.util.spec_from_file_location("compliance_lib", p)
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


L = _lib()
BASELINE = "tools/general-not-fixed-baseline.json"
RATE_SPECIAL = re.compile(r"(?<![\w.])(23\.976|29\.97|59\.94)(?![\w])")
RATE_NUM = re.compile(r"(?<![\w.])(24|25|30|50|60|120)(\.0+)?f?(?![\w.])")
RATE_CTX = re.compile(r"fps|hz|refresh|rate|cadence|frame", re.I)
SIZE_NUM = re.compile(r"(?<![\w.])(720|1080|1440|1600|2160|2560|3840|4320|7680)(?![\w.])")
SIZE_CTX = re.compile(r"width|height|resolution|\bres\b|\bw\b|\bh\b|size|rows|cols|4k", re.I)
TAG = re.compile(r"GENERAL-REF:\s*\S")


def classify_line(line):
    ids = []
    if RATE_SPECIAL.search(line) or (RATE_NUM.search(line) and RATE_CTX.search(line)):
        ids.append("rate")
    if SIZE_NUM.search(line) and SIZE_CTX.search(line):
        ids.append("size")
    return ids


def scan_text(rel, text):
    raw = text.splitlines()
    out = []
    for no, line in enumerate(L.strip_comments(text).splitlines(), 1):
        ids = classify_line(line)
        if not ids:
            continue
        tagged = TAG.search(raw[no - 1] if no <= len(raw) else "") or (no > 1 and TAG.search(raw[no - 2]))
        if tagged:
            continue
        out.append({"file": rel, "line": no, "pattern": "+".join(ids), "text": line.strip()[:140]})
    return out


def scan(root):
    hits = []
    for rel, full in L.iter_files(root, ["src/VP-V"], exempt_prefixes=("src/VP-V/tests/",)):
        hits.extend(scan_text(rel, L.read(full)))
    return hits


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--selftest" in argv:
        ok = classify_line("double fps = 23.976;") == ["rate"] and classify_line("int width = 3840;") == ["size"] \
            and not classify_line("int x = 60;") and len(scan_text("a.h", "auto fps = 24;\n")) == 1 \
            and len(scan_text("a.h", "// GENERAL-REF: ref\nauto fps = 24;\n")) == 0
        return L.selftest_ok("general-not-fixed", ok)
    return L.run_ratchet("N4 general-not-fixed", scan, argv, BASELINE,
                         "a new hard-coded rate/resolution; derive it from the measured value or tag a true reference with // GENERAL-REF: <why>.",
                         per_class=lambda hits: {k: sum(1 for h in hits if k in h["pattern"].split("+")) for k in ("rate", "size")})


if __name__ == "__main__":
    sys.exit(main())
