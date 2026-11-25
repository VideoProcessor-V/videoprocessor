# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N5 public-hygiene checker (doc 124 s2.2 N5).

Usage: python -I tools/check_public_hygiene.py [--root P] [--since REF] [--baseline FILE]
                                               [--write-baseline] [--json] [--strict] [--selftest]

Scans public repo files under src/VP-V/, shaders/, tools/, and docs/ (skipping binary files and
exempting this checker, its test file, and its baseline) for private/machine-local leaks:

  drive-d        local D:\\ or D:/ drive paths (private lab/scratch paths)
  users-path     local user profile paths (C:\\Users\\, C:/Users/, /Users/)
  owner-display  owner display model name (same FL35 rule as tools/master_settings_common.py)

A line is waived by an inline `PUBLIC-HYGIENE-OK <reason>` tag on the same or previous line.
Shrink-only baseline (tools/public-hygiene-baseline.json): records per-file ceilings, per-class
totals, and per-file hit fingerprints (SHA-256 prefix of pattern + stripped line text) so counts
may only shrink and no new leak can be smuggled in by a same-count swap. When no default baseline
exists in --root (e.g. a clean synthetic test tree), zero leaks are allowed. --since REF also
checks added lines against the baseline fingerprints and reports unbaselined hits as NEW.
Exit 0 clean, 1 violation, 2 error.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile


def _lib():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compliance_lib.py")
    s = importlib.util.spec_from_file_location("compliance_lib", p)
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


L = _lib()
BASELINE = "tools/public-hygiene-baseline.json"
SCAN_ROOTS = ("src/VP-V", "shaders", "tools", "docs")
SKIP_DIRS = {"3rdparty", "x64", "Debug", "Release", ".git", "__pycache__", ".vs", "node_modules"}
SELF = {
    "tools/check_public_hygiene.py",
    "tools/test_check_public_hygiene.py",
    "tools/test_n5_n8_compliance.py",
    "tools/public-hygiene-baseline.json",
}
PATTERNS = (
    ("drive-d", re.compile(r"(?<![A-Za-z0-9_])[Dd]:[\\/]")),
    ("users-path", re.compile(r"(?:[Cc]:[\\/]|[\\/])[Uu]sers[\\/]")),
    ("owner-display", re.compile(r"fl\s*-?\s*35", re.I)),   # mirrors tools/master_settings_common.py FL35
)
CLASSES = tuple(pid for pid, _ in PATTERNS)
TAG = re.compile(r"PUBLIC-HYGIENE-OK\s+\S")


def hit_hash(pattern, text):
    """Stable 12-hex-char fingerprint for one (pattern, stripped-line) leak."""
    raw = ("%s\0%s" % (pattern, text)).encode("utf-8", errors="replace")
    return hashlib.sha256(raw).hexdigest()[:12]


def is_binary_file(full):
    try:
        with open(full, "rb") as fh:
            chunk = fh.read(8192)
    except OSError:
        return True
    return b"\x00" in chunk


def classify_line(line):
    return [pid for pid, rx in PATTERNS if rx.search(line)]


def scan_text(rel, text):
    lines = text.splitlines()
    hits = []
    for no, line in enumerate(lines, 1):
        pids = classify_line(line)
        if not pids:
            continue
        if TAG.search(line) or (no > 1 and TAG.search(lines[no - 2])):
            continue
        stripped = line.strip()[:200]
        for pid in pids:
            hits.append({
                "file": rel,
                "line": no,
                "pattern": pid,
                "text": stripped,
                "hash": hit_hash(pid, stripped),
            })
    return hits


def iter_public_files(root):
    for scan_root in SCAN_ROOTS:
        base = os.path.join(root, *scan_root.split("/"))
        if not os.path.isdir(base):
            continue
        for dp, dn, fn in os.walk(base):
            dn[:] = sorted(d for d in dn if d not in SKIP_DIRS)
            for name in sorted(fn):
                full = os.path.join(dp, name)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel in SELF or is_binary_file(full):
                    continue
                yield rel, full


def scan(root):
    hits = []
    for rel, full in iter_public_files(root):
        try:
            text = L.read(full)
        except OSError:
            continue
        hits.extend(scan_text(rel, text))
    return hits


def per_class(hits):
    c = {k: 0 for k in CLASSES}
    for h in hits:
        c[h["pattern"]] = c.get(h["pattern"], 0) + 1
    return c


def hashes_by_file(hits):
    out = {}
    for h in hits:
        out.setdefault(h["file"], []).append(h["hash"])
    return {f: sorted(v) for f, v in sorted(out.items())}


def unbaselined_hits(hits, base_hashes):
    """Hits whose (file, hash) multiset exceeds the seeded baseline multiset for that file."""
    budgets = {f: collections.Counter(hs) for f, hs in (base_hashes or {}).items()}
    extra = []
    for h in hits:
        ctr = budgets.get(h["file"])
        if ctr and ctr[h["hash"]] > 0:
            ctr[h["hash"]] -= 1
            continue
        extra.append(h)
    return extra


def evaluate(hits, baseline, added=None):
    """Compare hits against shrink-only baseline. Returns (grown, shrunk, cgrown, unbaselined, new_since)."""
    counts = L.count_by_file(hits)
    classes = per_class(hits)
    base_files = (baseline or {}).get("files", {})
    base_classes = (baseline or {}).get("classes", {})
    base_hashes = (baseline or {}).get("line_hashes", {})
    grown, shrunk = L.ratchet(counts, {"files": base_files})
    cgrown = [(k, v, base_classes.get(k, 0)) for k, v in sorted(classes.items()) if v > base_classes.get(k, 0)]
    unbaselined = unbaselined_hits(hits, base_hashes)
    new_since = []
    if added is not None:
        new_since = [h for h in unbaselined if h["line"] in added.get(h["file"], ())]
    return grown, shrunk, cgrown, unbaselined, new_since


def build_baseline(hits):
    return {
        "tool": "N5 public-hygiene",
        "total": len(hits),
        "classes": per_class(hits),
        "files": L.count_by_file(hits),
        "line_hashes": hashes_by_file(hits),
    }


def selftest_failures():
    """Names of the failed selftest checks (empty when all pass)."""
    failed = []

    def check(name, cond):
        if not cond:
            failed.append(name)

    positives = [
        ("drive-d", r"path = D:\testclips\clip.mkv"),
        ("drive-d", "path = d:/codex/out"),
        ("users-path", r"C:\Users\alice\Desktop\x.mp4"),
        ("users-path", "C:/Users/bob/Videos/y.mp4"),
        ("users-path", "/Users/charlie/file.txt"),
        ("owner-display", "calibrated for FL35"),
        ("owner-display", "measured on fl-35 projector"),
        ("owner-display", "FL 35 P3 profile"),
    ]
    for pid, line in positives:
        check("pos %s: %s" % (pid, line), pid in classify_line(line))

    negatives = [
        "relative/path/clip.mkv",
        "mode D: auto",
        "std::map<std::string, int> users;",
        "F35 and L35 are unrelated",
        "C:\\Program Files\\VideoProcessor",
    ]
    for line in negatives:
        check("neg: %s" % line, not classify_line(line))

    check("same-line waiver", not scan_text("a.cpp", r'x = "D:\foo"; // PUBLIC-HYGIENE-OK synthetic'))
    check("prev-line waiver", not scan_text("a.cpp", "// PUBLIC-HYGIENE-OK synthetic\nx = \"FL35\";\n"))
    check("waiver without reason fails", len(scan_text("a.cpp", "// PUBLIC-HYGIENE-OK\nx = \"FL35\";\n")) == 1)

    root = tempfile.mkdtemp()
    try:
        def put(rel, content, binary=False):
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            mode = "wb" if binary else "w"
            kw = {} if binary else {"encoding": "utf-8", "newline": "\n"}
            with open(path, mode, **kw) as fh:
                fh.write(content)

        put("src/VP-V/clean.cpp", "// clean source file\nint x = 1;\n")
        put("docs/bin.dat", b"D:\\secret\x00binary", binary=True)
        put("tools/check_public_hygiene.py", "D:\\self_exempt\nFL35\n")
        check("clean tree exits 0 without baseline", main(["--root", root, "--strict"]) == 0)

        put("docs/leak.md", "See C:\\Users\\alice\\notes.txt\n")
        check("unbaselined leak exits 1", main(["--root", root, "--strict"]) == 1)

        bpath = os.path.join(root, BASELINE)
        check("--write-baseline seeds baseline", main(["--root", root, "--write-baseline"]) == 0 and os.path.isfile(bpath))
        check("seeded baseline exits 0", main(["--root", root, "--strict"]) == 0)

        # Same-count swap of a different leak line in the same file must still FAIL.
        put("docs/leak.md", "See C:\\Users\\bob\\other.txt\n")
        check("same-count swap is caught", main(["--root", root, "--strict"]) == 1)
        check("--write-baseline refuses new leak on swap", main(["--root", root, "--write-baseline"]) == 1)

        # Adding a second leak in another file must FAIL and be refused by --write-baseline.
        put("docs/leak.md", "See C:\\Users\\alice\\notes.txt\n")
        put("shaders/bad.glsl", "// tuned on FL35\n")
        check("new file leak fails", main(["--root", root, "--strict"]) == 1)
        check("--write-baseline refuses growth", main(["--root", root, "--write-baseline"]) == 1)

        # Removing the leak shrinks cleanly and --write-baseline accepts the shrink.
        os.remove(os.path.join(root, "shaders", "bad.glsl"))
        put("docs/leak.md", "Clean relative path.\n")
        check("shrunk tree exits 0", main(["--root", root, "--strict"]) == 0)
        check("--write-baseline accepts shrink", main(["--root", root, "--write-baseline"]) == 0)
        check("shrunk baseline total is 0", L.load_json(bpath)["total"] == 0)

        # Explicit missing --baseline returns exit 2.
        check("missing explicit baseline exits 2", main(["--root", root, "--baseline", os.path.join(root, "nope.json")]) == 2)
    finally:
        shutil.rmtree(root, True)
    return failed


def main(argv=None):
    ap = argparse.ArgumentParser(description="N5 public-hygiene checker")
    ap.add_argument("--root", default=L.repo_root())
    ap.add_argument("--since", help="also flag unbaselined hits on lines added since REF as NEW")
    ap.add_argument("--baseline", help="path to shrink-only baseline JSON")
    ap.add_argument("--write-baseline", action="store_true", help="write baseline; refused if any file, class, or hit grew")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for gate uniformity (always strict)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        failed = selftest_failures()
        for name in failed:
            print("selftest FAILED check: " + name)
        return L.selftest_ok("public hygiene", not failed)

    root = os.path.abspath(a.root)
    bpath = a.baseline or os.path.join(root, *BASELINE.split("/"))
    if a.baseline and not a.write_baseline and not os.path.isfile(bpath):
        print("N5 public-hygiene: error: baseline not found at %s" % bpath)
        return 2

    try:
        hits = scan(root)
        base = L.load_json(bpath) if os.path.isfile(bpath) else None
        if os.path.isfile(bpath) and base is None:
            print("N5 public-hygiene: error: invalid JSON baseline at %s" % bpath)
            return 2

        if a.write_baseline:
            if base is not None:
                grown, _shrunk, cgrown, unbaselined, _ = evaluate(hits, base)
                if grown or cgrown or unbaselined:
                    print("N5 public-hygiene: refusing to write baseline that grew (%d grown file(s), %d grown class(es), %d unbaselined hit(s))" % (
                        len(grown), len(cgrown), len(unbaselined)))
                    return 1
            data = build_baseline(hits)
            L.save_json(bpath, data)
            print("N5 public-hygiene: baseline written (%d hits, %d files) -> %s" % (
                data["total"], len(data["files"]), bpath))
            return 0

        added = L.added_lines(root, a.since) if a.since else None
        grown, shrunk, cgrown, unbaselined, new_since = evaluate(hits, base or {}, added)
    except RuntimeError as e:
        print("N5 public-hygiene: error: %s" % e)
        return 2

    classes = per_class(hits)
    counts = L.count_by_file(hits)
    total_ceiling = (base or {}).get("total", 0)
    print("N5 public-hygiene: %d hit(s) in %d file(s), baseline %d, shrunk files %d, classes %s%s" % (
        len(hits), len(counts), total_ceiling, len(shrunk), json.dumps(classes, sort_keys=True),
        " (since " + a.since + ")" if a.since else ""))
    for f, n, c in grown:
        print("  GREW %s: %d > ceiling %d" % (f, n, c))
    for k, v, c in cgrown:
        print("  GREW class %s: %d > ceiling %d" % (k, v, c))
    for h in unbaselined:
        tag = "NEW" if (added is not None and h["line"] in added.get(h["file"], ())) else "LEAK"
        print("  %s %s %s:%d %s" % (tag, h["pattern"], h["file"], h["line"], h["text"]))
    bad = bool(grown or cgrown or unbaselined or new_since)
    if a.json:
        print(json.dumps({
            "hits": len(hits),
            "classes": classes,
            "grown": grown,
            "cgrown": cgrown,
            "shrunk": shrunk,
            "unbaselined": unbaselined,
            "new_since": new_since,
        }, indent=1, sort_keys=True))
    if bad:
        print("FAIL: public-hygiene leak detected (private drive/user path or owner display name); use generic wording/paths or tag PUBLIC-HYGIENE-OK <reason>.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
