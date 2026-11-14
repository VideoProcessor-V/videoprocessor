# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Test / checker / baseline tamper gate (owner rule 19: no metric gaming).

Usage: python -I tools/check_test_tamper.py [--strict] [--range A..B | --base REF] [--root PATH] [--json]

Walks every non-merge commit in a range (default merge-base(HEAD, base ref)..HEAD) and flags each
commit that touches a guarded file class WITHOUT a WAIVER-<ID> trailer line in its message:

  test       src/**/tests/**, *Tests.cpp, *Test.cpp   (a test-ADDING-only change passes with a note:
                                                       only added lines, no removed/edited line)
  checker    tools/check_*.py, tools/run_compliance.py (any change needs a waiver)
  baseline   tools/*baseline*.json, docs/audit/*baseline*.json, *budget*.json (a change that only
                                                       REMOVES lines, i.e. a shrink, passes with a note)
  allowlist  a changed line, in any other file, naming ALLOW_SITES, CoverageAllowList,
             known_false_positive or an ALLOW list/entry (removal-only passes with a note)

Exit 0 clean; 1 with --strict when any commit is flagged; 2 on a checker error (bad range, git failure).
A waiver is a line "WAIVER-<ID>: <why>" (ID = letters, digits, '-', '_', '.') at the start of a line.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

WAIVER_RX = re.compile(r"^WAIVER-[A-Za-z0-9_.\-]+(\s*:.*)?\s*$", re.M)
CHECKER_GLOBS = ["tools/check_*.py", "tools/run_compliance.py", "tools/compliance_lib.py"]
BASELINE_GLOBS = ["tools/*baseline*.json", "docs/audit/*baseline*.json", "docs/audit/*budget*.json",
                  "tools/*budget*.json"]
ALLOW_RX = re.compile(r"ALLOW_SITES|CoverageAllowList|known_false_positive|\bALLOW\s*=|\"allow\w*\"\s*:")


def classify(path):
    if any(fnmatch.fnmatch(path, g) for g in CHECKER_GLOBS):
        return "checker"
    if any(fnmatch.fnmatch(path, g) for g in BASELINE_GLOBS):
        return "baseline"
    if "/tests/" in path or path.endswith(("Tests.cpp", "Test.cpp")) or "/VideoProcessor-Test/" in path:
        return "test"
    return "other"


def has_waiver(message):
    return bool(WAIVER_RX.search(message))


def judge(files, message):
    """files: [{path, status, added:[lines], removed:[lines]}]. Returns (violations, notes, waived)."""
    viol, notes = [], []
    for f in files:
        cls = classify(f["path"])
        add, rem = f["added"], f["removed"]
        if cls == "test":
            if f["status"].startswith("D"):
                viol.append(("test", f["path"], "deleted"))
            elif f["status"].startswith("A") or (add and not rem):
                notes.append("test-adding only: " + f["path"])
            elif add or rem:
                viol.append(("test", f["path"], "%d line(s) removed/edited" % len(rem)))
        elif cls == "checker":
            viol.append(("checker", f["path"], "checker changed"))
        elif cls == "baseline":
            if rem and not add:
                notes.append("baseline shrink: " + f["path"])
            else:
                viol.append(("baseline", f["path"], "baseline changed (+%d/-%d)" % (len(add), len(rem))))
        else:
            if [l for l in add if ALLOW_RX.search(l)]:
                viol.append(("allowlist", f["path"], "allow-list line added/edited"))
            elif [l for l in rem if ALLOW_RX.search(l)]:
                notes.append("allow-list removal: " + f["path"])
    if has_waiver(message):
        if viol:
            notes.append("WAIVER present: %d guarded change(s) waived" % len(viol))
        return [], notes, bool(viol)
    return viol, notes, False


def git(root, *args):
    r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()[:200]))
    return r.stdout


def commit_files(root, sha):
    names = git(root, "diff-tree", "--no-commit-id", "--name-status", "-r", "--no-renames", sha)
    out = []
    for line in names.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        status, path = parts[0], parts[-1]
        diff = git(root, "show", "--format=", "-U0", "--no-renames", sha, "--", path)
        add = [l[1:] for l in diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
        rem = [l[1:] for l in diff.splitlines() if l.startswith("-") and not l.startswith("---")]
        out.append({"path": path, "status": status, "added": add, "removed": rem})
    return out


def default_range(root, base):
    cands = [base] if base else ["staging/batch1", "origin/staging/batch1", "integ/batch1", "origin/v1.4.00-beta"]
    for c in cands:
        try:
            return git(root, "merge-base", "HEAD", c).strip() + "..HEAD"
        except RuntimeError:
            continue
    raise RuntimeError("no base ref found; pass --range or --base")


def scan(root, rng):
    results = []
    for sha in git(root, "rev-list", "--no-merges", "--reverse", rng).split():
        msg = git(root, "show", "-s", "--format=%B", sha)
        viol, notes, waived = judge(commit_files(root, sha), msg)
        subject = msg.strip().splitlines()[0][:80] if msg.strip() else ""
        results.append({"sha": sha[:9], "subject": subject, "violations": viol, "notes": notes, "waived": waived})
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description="Flag commits that touch tests/checkers/baselines/allow-lists without a WAIVER trailer.")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--range")
    ap.add_argument("--base")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        f = [{"path": "tools/check_x.py", "status": "M", "added": ["a"], "removed": []}]
        v, _, _ = judge(f, "x")
        v2, _, w2 = judge(f, "t\n\nWAIVER-T1: ok\n")
        ok = bool(v) and not v2 and w2
        print("selftest " + ("ok" if ok else "FAIL"))
        return 0 if ok else 2
    try:
        rng = a.range or default_range(a.root, a.base)
        res = scan(a.root, rng)
    except RuntimeError as e:
        print("check_test_tamper: error: %s" % e)
        return 2
    bad = [r for r in res if r["violations"]]
    if a.json:
        print(json.dumps({"range": rng, "commits": len(res), "flagged": len(bad), "results": res}, indent=1))
    else:
        print("test tamper: range %s, %d commit(s), %d flagged, %d waived" % (rng, len(res), len(bad), sum(1 for r in res if r["waived"])))
        for r in res:
            for n in r["notes"]:
                print("  note %s %s" % (r["sha"], n))
            for cls, path, why in r["violations"]:
                print("  FLAG %s %s [%s] %s: %s" % (r["sha"], r["subject"], cls, path, why))
        if bad:
            print("FAIL: add a WAIVER-<ID>: <why> trailer (separately gated) or revert the change (owner rule 19).")
    return 1 if (bad and a.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
