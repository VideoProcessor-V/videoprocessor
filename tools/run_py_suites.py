#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-P1: the PY lane. Runs every Python unittest suite under tools/ (each directory that holds test_*.py) with
`python -I -m unittest discover`, one child process per directory, and prints the per-directory counts.

Usage: python -I tools/run_py_suites.py [--root P] [--only DIR[,DIR]] [--no-skips] [--timeout SEC] [--json FILE]
       python -I tools/run_py_suites.py --list | --selftest

A directory FAILS when its run exits non-zero, runs 0 tests, times out, or prints no "Ran N tests" line. With
--no-skips a skipped test also fails it (the lane runner passes it: a skipped test proved nothing, rule 11(d)). The
exit code is 0 only when every directory passes, 1 on any failure, 2 on a usage error. The lane has no by-name
exclusion list: a file that is not a suite is renamed out of test_* (tools/test_py_suite_guard.py fails any
test_*.py that does not import cleanly or defines no TestCase).

Run it after the x64 Release build: tools/test_libplacebo_variants.py loads the packaged renderers and FAILS (never
skips) when the build is missing. VP_BUILD_ROOT overrides the build folder.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
RAN_RE = re.compile(r"^Ran (\d+) tests? in ([\d.]+)s", re.M)
SKIPPED_RE = re.compile(r"skipped=(\d+)")
SKIP_DIRS = {"__pycache__", "testclips", "td-cli", "3rdparty"}


def suite_dirs(root=ROOT):
    """Every directory under <root>/tools that holds a test_*.py, as paths relative to root (forward slashes), sorted."""
    found = []
    for base, dirs, files in os.walk(os.path.join(root, "tools")):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        if any(f.startswith("test_") and f.endswith(".py") for f in files):
            found.append(os.path.relpath(base, root).replace("\\", "/"))
    return sorted(found)


def parse_run(text):
    """(tests, skipped, seconds) from the unittest summary text; tests is None when there is no "Ran N tests" line."""
    m = RAN_RE.search(text)
    if not m:
        return None, 0, 0.0
    skipped = sum(int(x) for x in SKIPPED_RE.findall(text))
    return int(m.group(1)), skipped, float(m.group(2))


def run_dir(root, rel, no_skips=False, timeout=900):
    """Run one directory; returns {dir, status, tests, skipped, rc, note, tail}."""
    cmd = [sys.executable, "-I", "-m", "unittest", "discover", "-s", os.path.join(root, rel), "-p", "test_*.py"]
    t0 = time.time()
    try:
        done = subprocess.run(cmd, cwd=root, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"dir": rel, "status": "FAIL", "tests": None, "skipped": 0, "rc": None, "note": "timeout after %d s" % timeout, "tail": []}
    text = (done.stdout or "") + (done.stderr or "")
    tests, skipped, _secs = parse_run(text)
    status, note = "PASS", "%.0f s" % (time.time() - t0)
    if tests == 0 or "NO TESTS RAN" in text:
        status, note = "FAIL", "0 tests ran"
    elif done.returncode != 0:
        status, note = "FAIL", "rc %d" % done.returncode
    elif tests is None:
        status, note = "FAIL", "no 'Ran N tests' line"
    elif no_skips and skipped:
        status, note = "FAIL", "%d skipped (--no-skips)" % skipped
    return {"dir": rel, "status": status, "tests": tests, "skipped": skipped, "rc": done.returncode, "note": note,
            "tail": text.strip().splitlines()[-12:] if status == "FAIL" else []}


def run_all(root, only=None, no_skips=False, timeout=900, dirs=None):
    results = []
    for rel in (dirs if dirs is not None else suite_dirs(root)):
        if only and rel not in only and os.path.basename(rel) not in only and rel.split("/", 1)[-1] not in only:
            continue
        results.append(run_dir(root, rel, no_skips, timeout))
    return results


def report(results):
    lines = ["PY lane: %d directories" % len(results)]
    for r in results:
        lines.append("%-24s %-4s tests=%s skipped=%d %s" % (r["dir"], r["status"], r["tests"], r["skipped"], r["note"]))
    for r in results:
        if r["status"] != "PASS":
            lines.append("-- %s" % r["dir"])
            lines.extend("   " + x[:220] for x in r["tail"])
    total = sum(r["tests"] or 0 for r in results)
    bad = sum(1 for r in results if r["status"] != "PASS")
    lines.append("PY lane: %d tests in %d directories, %d failed directories%s" % (
        total, len(results), bad, "" if results else " (NOTHING RAN)"))
    return "\n".join(lines) + "\n", (1 if bad or not results else 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description="run every Python unittest suite under tools/")
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--only", help="comma-separated directories (tools/eval, or just eval)")
    ap.add_argument("--no-skips", action="store_true", help="a skipped test fails its directory")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--json", help="also write the results as JSON")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        ok = parse_run("Ran 3 tests in 0.5s\n\nOK (skipped=1)") == (3, 1, 0.5) and parse_run("no summary")[0] is None
        print("selftest run_py_suites %s" % ("ok" if ok else "FAIL"))
        return 0 if ok else 2
    if a.list:
        print("\n".join(suite_dirs(a.root)))
        return 0
    results = run_all(a.root, set(a.only.split(",")) if a.only else None, a.no_skips, a.timeout)
    if a.only and not results:
        print("run_py_suites: --only %s matched no suite directory" % a.only)
        return 2
    text, rc = report(results)
    sys.stdout.write(text)
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(results, fh, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
