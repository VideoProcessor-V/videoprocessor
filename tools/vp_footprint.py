# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VP-V footprint in Bill's files.

Measures how much VP-V code lives in Bill's (upstream) files, by comparing a
Bill ref with a head ref.  Pure git + Python stdlib.

  python -I tools/vp_footprint.py [--bill a591ad3b] [--head HEAD]
         [--files PATH...] [--json] [--append CSV] [--budget JSON]
         [--top N] [--selftest]

Hook lines: an added line that ends with `// VPV-HOOK`, or lies between a line
that is exactly `// VPV-HOOK` and a following `// VPV-HOOK-END`, or any line of
a file whose first line is `// VPV-HOOK-FILE`.  Stray = adds - hook adds.
"""
import argparse
import csv
import datetime
import fnmatch
import json
import re
import subprocess
import sys

VPV_PREFIX = "src/VP-V/"
PROJECT_EXT = (".vcxproj", ".filters", ".sln", ".props", ".targets")
HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
DIFF_ARGS = ["-w", "--diff-algorithm=histogram", "--no-color", "--no-ext-diff"]


def git(args, repo=None, text=True):
    cmd = ["git"] + (["-C", repo] if repo else []) + args
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), out.stderr.decode("utf-8", "replace")))
    return out.stdout.decode("utf-8", "replace") if text else out.stdout


def bucket_of(path):
    low = path.lower()
    if low.endswith(PROJECT_EXT):
        return "project"
    parts = low.split("/")
    base = parts[-1]
    if any(("test" in p) for p in parts[:-1]) or "tests" in base or base.startswith("test"):
        return "tests"
    return "production"


def parse_diff(text):
    """Return {path: {'adds': n, 'dels': n, 'hunks': n, 'added_lines': [lineno,...]}}."""
    files = {}
    cur = None
    new_line = 0
    for raw in text.splitlines():
        if raw.startswith("diff --git "):
            cur = None
            continue
        if raw.startswith("+++ "):
            p = raw[4:]
            if p == "/dev/null":
                cur = None
            else:
                p = p[2:] if p.startswith("b/") else p
                cur = files.setdefault(p.strip('"'), {"adds": 0, "dels": 0, "hunks": 0, "added_lines": []})
            continue
        if raw.startswith("--- "):
            continue
        if cur is None:
            continue
        m = HUNK_RE.match(raw)
        if m:
            cur["hunks"] += 1
            new_line = int(m.group(3))
            continue
        if raw.startswith("+"):
            cur["adds"] += 1
            cur["added_lines"].append(new_line)
            new_line += 1
        elif raw.startswith("-"):
            cur["dels"] += 1
    return files


def hook_line_set(content):
    """Set of 1-based line numbers in `content` that are hook lines."""
    lines = content.splitlines()
    out = set()
    if lines and lines[0].strip().startswith("// VPV-HOOK-FILE"):
        return set(range(1, len(lines) + 1))
    inblock = False
    for i, ln in enumerate(lines, 1):
        s = ln.strip()
        if s.startswith("// VPV-HOOK-END"):
            out.add(i)
            inblock = False
        elif s == "// VPV-HOOK":
            out.add(i)
            inblock = True
        elif inblock or s.endswith("// VPV-HOOK"):
            out.add(i)
    return out


def measure(bill, head, only_files=None, repo=None):
    bill_files = set(git(["ls-tree", "-r", "--name-only", bill], repo).splitlines())
    head_files = set(git(["ls-tree", "-r", "--name-only", head], repo).splitlines())
    bill_prod = {f for f in bill_files if not f.startswith(VPV_PREFIX)}
    diff = parse_diff(git(["diff"] + DIFF_ARGS + ["-U0", bill, head], repo))
    marked = set()
    try:
        marked = {l.split(":", 1)[1] for l in git(["grep", "-l", "VPV-HOOK", head], repo).splitlines() if ":" in l}
    except RuntimeError:
        pass
    rows = []
    for path, d in diff.items():
        if path not in bill_prod or path not in head_files:
            continue
        hook = 0
        if path in marked and d["added_lines"]:
            hs = hook_line_set(git(["show", "%s:%s" % (head, path)], repo))
            hook = sum(1 for n in d["added_lines"] if n in hs)
        rows.append({"path": path, "bucket": bucket_of(path), "adds": d["adds"], "dels": d["dels"],
                     "hunks": d["hunks"], "hook": hook, "stray": d["adds"] - hook})
    # misplaced files
    bill_dirs = {"/".join(f.split("/")[:2]) for f in bill_files if f.startswith("src/") and f.count("/") >= 2}
    misplaced, hookfiles = [], []
    cand = [f for f in sorted(head_files - bill_files)
            if not f.startswith(VPV_PREFIX) and "/".join(f.split("/")[:2]) in bill_dirs]
    counts, hookset = {}, set()
    if cand:
        for ln in git(["grep", "-c", "", head, "--"] + cand, repo).splitlines():
            name, _, n = ln.rpartition(":")
            counts[name.split(":", 1)[1]] = int(n)
        try:
            hookset = {l.split(":", 1)[1] for l in git(["grep", "-l", "-e", "^// VPV-HOOK-FILE", head, "--"] + cand, repo).splitlines()}
        except RuntimeError:
            pass
    for f in cand:
        n = counts.get(f, 0)
        (hookfiles if f in hookset else misplaced).append({"path": f, "lines": n})
    rows.sort(key=lambda r: (-r["stray"], r["path"]))
    if only_files:
        pats = [p.replace("\\", "/") for p in only_files]
        shown = [r for r in rows if any(r["path"] == p or fnmatch.fnmatch(r["path"], p) for p in pats)]
        have = {r["path"] for r in shown}
        for p in pats:
            if p not in have and p in bill_prod:
                shown.append({"path": p, "bucket": bucket_of(p), "adds": 0, "dels": 0, "hunks": 0, "hook": 0, "stray": 0})
    else:
        shown = rows
    totals = {}
    for b in ("production", "tests", "project"):
        sel = [r for r in rows if r["bucket"] == b]
        totals[b] = {k: sum(r[k] for r in sel) for k in ("adds", "dels", "hunks", "hook", "stray")}
        totals[b]["files"] = len(sel)
    return {"bill": bill, "head": head, "files": shown, "totals": totals,
            "misplaced": {"count": len(misplaced), "lines": sum(m["lines"] for m in misplaced), "files": misplaced},
            "hook_files": {"count": len(hookfiles), "lines": sum(m["lines"] for m in hookfiles), "files": hookfiles}}


def render(res, top):
    out = ["Footprint: bill=%s head=%s" % (res["bill"], res["head"]),
           "%-6s %-5s %-5s %-6s %-5s  %s" % ("stray", "adds", "dels", "hunks", "hook", "file (bucket)")]
    for r in res["files"][:top]:
        out.append("%-6d %-5d %-5d %-6d %-5d  %s (%s)" % (r["stray"], r["adds"], r["dels"], r["hunks"], r["hook"], r["path"], r["bucket"]))
    out.append("Totals per bucket:")
    for b, t in res["totals"].items():
        out.append("  %-10s files=%d adds=%d dels=%d hunks=%d hook=%d stray=%d" % (b, t["files"], t["adds"], t["dels"], t["hunks"], t["hook"], t["stray"]))
    m, h = res["misplaced"], res["hook_files"]
    out.append("Misplaced VP files in Bill folders: %d files, %d lines" % (m["count"], m["lines"]))
    for f in m["files"][:top]:
        out.append("  %s (%d)" % (f["path"], f["lines"]))
    out.append("Hook files (VPV-HOOK-FILE): %d files, %d lines" % (h["count"], h["lines"]))
    return "\n".join(out)


def selftest():
    diff = ("diff --git a/src/A.cpp b/src/A.cpp\n--- a/src/A.cpp\n+++ b/src/A.cpp\n"
            "@@ -3,0 +4,2 @@ x\n+one\n+two // VPV-HOOK\n@@ -10 +12 @@\n-old\n+new\n"
            "diff --git a/src/B.h b/src/B.h\n--- /dev/null\n+++ b/src/B.h\n@@ -0,0 +1 @@\n+z\n")
    p = parse_diff(diff)
    assert p["src/A.cpp"]["adds"] == 3 and p["src/A.cpp"]["dels"] == 1 and p["src/A.cpp"]["hunks"] == 2, p
    assert p["src/A.cpp"]["added_lines"] == [4, 5, 12], p
    hs = hook_line_set("a\n// VPV-HOOK\nb\nc\n// VPV-HOOK-END\nd // VPV-HOOK\ne\n")
    assert hs == {2, 3, 4, 5, 6}, hs
    assert hook_line_set("// VPV-HOOK-FILE\nx\n") == {1, 2}
    assert bucket_of("src/X/Foo.vcxproj") == "project" and bucket_of("src/VideoProcessor-Test/a.cpp") == "tests"
    assert bucket_of("src/VideoProcessor-Lib/Foo.cpp") == "production"
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bill", default="a591ad3b")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--files", nargs="*")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--append")
    ap.add_argument("--budget")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--repo")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    res = measure(a.bill, a.head, a.files, a.repo)
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        print(render(res, a.top))
    if a.append:
        sha = git(["rev-parse", "--short", a.head], a.repo).strip()
        new = True
        try:
            new = open(a.append).read() == ""
        except OSError:
            pass
        with open(a.append, "a", newline="") as fh:
            w = csv.writer(fh)
            if new:
                w.writerow(["date", "bill", "head", "prod_adds", "prod_hook", "prod_stray", "prod_hunks",
                            "tests_adds", "tests_stray", "project_adds", "project_hook", "project_stray",
                            "misplaced_files", "misplaced_lines", "hookfile_lines"])
            t = res["totals"]
            w.writerow([datetime.date.today().isoformat(), a.bill, sha, t["production"]["adds"], t["production"]["hook"],
                        t["production"]["stray"], t["production"]["hunks"], t["tests"]["adds"], t["tests"]["stray"],
                        t["project"]["adds"], t["project"]["hook"], t["project"]["stray"],
                        res["misplaced"]["count"], res["misplaced"]["lines"], res["hook_files"]["lines"]])
    if a.budget:
        budget = json.load(open(a.budget))
        default = budget.get("default")
        bad = []
        for r in res["files"]:
            lim = budget.get("files", {}).get(r["path"], default)
            if lim is not None and r["stray"] > lim:
                bad.append("%s stray %d > budget %d" % (r["path"], r["stray"], lim))
        if bad:
            print("BUDGET EXCEEDED:\n" + "\n".join(bad), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
