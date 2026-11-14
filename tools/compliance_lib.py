# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared helpers for the doc 124 compliance checkers N2 / N3 / N4 / N6 (load with importlib; python -I
does not put tools/ on sys.path).

Contract (doc 124 s2.2): exit 0 clean, 1 violation, 2 checker error. A baseline is per-file ceilings that
only shrink. A hit is waived by an inline tag carrying a reason, or an allow-list entry with reason + ROW ID.
Modes: whole-tree ratchet (default) and --since REF (hits on lines added since REF are NEW and blocking,
unless waived).
"""
import argparse
import json
import os
import re
import subprocess
import sys

EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl")
SKIP_DIRS = {"tests", "VideoProcessor-Test", "3rdparty", "x64", "fixtures", ".git", "Debug", "Release"}


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def strip_comments(text):
    """Remove // and /* */ comments, keep line structure; string literals are respected."""
    out, i, n, block, instr = [], 0, len(text), False, None
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if block:
            if two == "*/":
                block = False
                i += 2
            else:
                if c == "\n":
                    out.append("\n")
                i += 1
            continue
        if instr:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == instr or c == "\n":
                instr = None
            i += 1
            continue
        if two == "/*":
            block = True
            i += 2
            continue
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c in "\"'":
            instr = c
        out.append(c)
        i += 1
    return "".join(out)


def iter_files(root, scan_roots, exts=EXTS, skip_dirs=SKIP_DIRS, exempt_prefixes=()):
    for scan in scan_roots:
        base = os.path.join(root, scan)
        for dp, dn, fn in os.walk(base):
            dn[:] = sorted(d for d in dn if d not in skip_dirs)
            for f in sorted(fn):
                if not f.lower().endswith(exts) or f.endswith(("Tests.cpp", "Test.cpp")):
                    continue
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel.startswith(tuple(exempt_prefixes)):
                    continue
                yield rel, full


def read(full):
    with open(full, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def added_lines(root, since):
    """{rel: set(new line numbers)} for the diff since..working tree (HEAD included)."""
    r = subprocess.run(["git", "-C", root, "diff", "-U0", "--no-renames", since, "--"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git diff %s: %s" % (since, r.stderr.strip()[:200]))
    out, cur = {}, None
    for line in r.stdout.splitlines():
        if line.startswith("+++ "):
            cur = line[6:] if line.startswith("+++ b/") else None
            if cur:
                out.setdefault(cur, set())
        elif line.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if m:
                start, cnt = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
                out[cur].update(range(start, start + cnt))
    return out


def load_json(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def save_json(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=1, sort_keys=True)
        fh.write("\n")


def count_by_file(hits):
    c = {}
    for h in hits:
        c[h["file"]] = c.get(h["file"], 0) + 1
    return c


def ratchet(counts, baseline):
    """Compare per-file counts to baseline ceilings. Returns (grown, shrunk): lists of (file, now, ceiling)."""
    ceil = (baseline or {}).get("files", {})
    grown = [(f, n, ceil.get(f, 0)) for f, n in sorted(counts.items()) if n > ceil.get(f, 0)]
    shrunk = [(f, counts.get(f, 0), c) for f, c in sorted(ceil.items()) if counts.get(f, 0) < c]
    return grown, shrunk


def run_ratchet(name, scan, argv, default_baseline, extra_doc="", per_class=None):
    """Generic main. scan(root) -> hits [{file,line,text,...}] (already waiver-filtered).
    per_class(hits) -> {class: count} optional extra baseline section that must also only shrink."""
    ap = argparse.ArgumentParser(description=name)
    ap.add_argument("--root", default=repo_root())
    ap.add_argument("--baseline")
    ap.add_argument("--since", help="also list hits on lines added since REF as NEW (blocking)")
    ap.add_argument("--write-baseline", action="store_true", help="write the baseline; refused if any count grew")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for gate uniformity (always strict)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    bpath = a.baseline or os.path.join(a.root, default_baseline)
    try:
        hits = scan(a.root)
        base = load_json(bpath)
        counts = count_by_file(hits)
        classes = per_class(hits) if per_class else {}
        if a.write_baseline:
            grown, _ = ratchet(counts, base) if base else ([], [])
            cgrown = [k for k, v in classes.items() if base and v > base.get("classes", {}).get(k, v)]
            if grown or cgrown:
                print("%s: refusing to write a baseline that grew: %s" % (name, grown or cgrown))
                return 1
            save_json(bpath, {"tool": name, "total": len(hits), "files": counts, "classes": classes})
            print("%s: baseline written (%d hits, %d files) -> %s" % (name, len(hits), len(counts), bpath))
            return 0
        if base is None:
            print("%s: no baseline at %s (run --write-baseline once)" % (name, bpath))
            return 2
        grown, shrunk = ratchet(counts, base)
        cgrown = [(k, v, base.get("classes", {}).get(k, 0)) for k, v in sorted(classes.items())
                  if v > base.get("classes", {}).get(k, 0)]
        new = []
        if a.since:
            added = added_lines(a.root, a.since)
            new = [h for h in hits if h["line"] in added.get(h["file"], ())]
        total_ceiling = base.get("total", 0)
        print("%s: %d hit(s) in %d file(s), baseline %d, shrunk files %d%s" % (
            name, len(hits), len(counts), total_ceiling, len(shrunk),
            ", classes " + json.dumps(classes, sort_keys=True) if classes else ""))
        for f, n, c in grown:
            print("  GREW %s: %d > ceiling %d" % (f, n, c))
        for k, v, c in cgrown:
            print("  GREW class %s: %d > ceiling %d" % (k, v, c))
        for h in new:
            print("  NEW %s:%d %s" % (h["file"], h["line"], h["text"]))
        bad = bool(grown or cgrown or new)
        if a.json:
            print(json.dumps({"hits": len(hits), "grown": grown, "new": new, "shrunk": shrunk}, indent=1))
        if bad:
            print("FAIL: " + (extra_doc or "ratchet exceeded; fix the hit or add a reasoned waiver (baselines only shrink)."))
        return 1 if bad else 0
    except RuntimeError as e:
        print("%s: error: %s" % (name, e))
        return 2


def selftest_ok(label, cond):
    print("selftest %s %s" % (label, "ok" if cond else "FAIL"))
    return 0 if cond else 2
