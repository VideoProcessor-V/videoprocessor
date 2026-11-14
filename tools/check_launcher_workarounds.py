# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N6 launcher-workaround checker (doc 124 s2.2 N6; P-id H).

Usage: python -I tools/check_launcher_workarounds.py [--root P] [--since REF] [--extra-dir D ...] [--strict]

Scans tracked *.ps1 / *.cmd / *.bat / *.py / *.sh (plus --extra-dir folders, e.g. the agent script
dirs) for the workaround idioms that bypass a block instead of fixing it:

  exec-policy   -ExecutionPolicy Bypass|Unrestricted, Set-ExecutionPolicy
  unblock       Unblock-File
  encoded       -EncodedCommand / -enc
  nested-ps     Start-Process powershell, cmd /c powershell
  cmd-wrapper   a .cmd/.bat line that runs a .ps1 (a new wrapper launching a script)
  kill-by-name  taskkill /IM, Stop-Process -Name
  no-verify     --no-verify

Whole-tree mode: every hit must be on the allow-list (tools/launcher-workarounds-allow.json: entries
{"line": <full stripped line text>, "file": rel, "reason", "row": <102b ROW ID>}); anything else is NEW and
blocks. --since REF restricts the scan to lines added since REF. A stale allow entry (no hit) is reported,
not failing. The 10 pre-existing lines at 6772f5fcb (packaging/*.cmd, build-vp.ps1:72, README run lines)
are the seed. Exit 0 clean, 1 violation, 2 error.
"""
import argparse
import importlib.util
import os
import re
import subprocess
import sys


def _lib():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compliance_lib.py")
    s = importlib.util.spec_from_file_location("compliance_lib", p)
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


L = _lib()
SCRIPT_EXTS = (".ps1", ".cmd", ".bat", ".py", ".sh")
SELF = ("tools/check_launcher_workarounds.py", "tools/test_check_launcher_workarounds.py")
ALLOW_PATH = "tools/launcher-workarounds-allow.json"
_I = re.IGNORECASE
PATTERNS = [
    ("exec-policy", re.compile(r"-ExecutionPolicy\s+(Bypass|Unrestricted)\b|\bSet-ExecutionPolicy\b", _I)),
    ("unblock", re.compile(r"\bUnblock-File\b", _I)),
    ("encoded", re.compile(r"-EncodedCommand\b|\s-enc\s", _I)),
    ("nested-ps", re.compile(r"Start-Process\s+(-FilePath\s+)?[\"']?powershell|cmd(\.exe)?\s+/c\s+[\"']?powershell", _I)),
    ("kill-by-name", re.compile(r"\btaskkill\b.*\s/IM\b|\bStop-Process\b.*\s-Name\b", _I)),
    ("no-verify", re.compile(r"--no-verify\b")),
]
WRAP_RX = re.compile(r"\.ps1\b", _I)


def classify_line(rel, line):
    ids = [pid for pid, rx in PATTERNS if rx.search(line)]
    if rel.lower().endswith((".cmd", ".bat")) and WRAP_RX.search(line) and not line.strip().lower().startswith(("rem ", "::")):
        ids.append("cmd-wrapper")
    return ids


def tracked_scripts(root):
    r = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git ls-files failed: " + r.stderr.strip()[:200])
    return [p for p in r.stdout.splitlines() if p.lower().endswith(SCRIPT_EXTS) and p not in SELF
            and not p.startswith("3rdparty/")]


def scan(root, extra_dirs=()):
    files = [(p, os.path.join(root, p)) for p in tracked_scripts(root)]
    for d in extra_dirs:
        for dp, _dn, fn in os.walk(d):
            for f in fn:
                if f.lower().endswith(SCRIPT_EXTS):
                    full = os.path.join(dp, f)
                    files.append((full.replace(os.sep, "/"), full))
    hits = []
    for rel, full in files:
        try:
            text = L.read(full)
        except OSError:
            continue
        for no, line in enumerate(text.splitlines(), 1):
            for pid in classify_line(rel, line):
                hits.append({"file": rel, "line": no, "pattern": pid, "text": line.strip()[:200]})
    return hits


def evaluate(hits, allow, added=None):
    """Return (new, stale). A hit is allowed when an entry matches its full stripped line text and pattern."""
    keys = {(e["file"], e["line"].strip()): e for e in allow}
    used = set()
    new = []
    for h in hits:
        if added is not None and h["line"] not in added.get(h["file"], ()):
            if (h["file"], h["text"]) in keys:
                used.add((h["file"], h["text"]))
            continue
        k = (h["file"], h["text"])
        if k in keys and keys[k].get("reason") and keys[k].get("row"):
            used.add(k)
            continue
        new.append(h)
    stale = [k for k in keys if k not in used]
    return new, stale


def main(argv=None):
    ap = argparse.ArgumentParser(description="N6 launcher-workaround checker")
    ap.add_argument("--root", default=L.repo_root())
    ap.add_argument("--since")
    ap.add_argument("--extra-dir", action="append", default=[])
    ap.add_argument("--allow")
    ap.add_argument("--write-allow", action="store_true", help="seed the allow-list from current hits (reason/row must be edited in)")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        ok = classify_line("a.ps1", "powershell -ExecutionPolicy Bypass -File x") == ["exec-policy"] \
            and classify_line("a.cmd", "powershell -File b.ps1") == ["cmd-wrapper"] \
            and not classify_line("a.py", "print('hi')")
        return L.selftest_ok("launcher patterns", ok)
    try:
        hits = scan(a.root, a.extra_dir)
        apath = a.allow or os.path.join(a.root, ALLOW_PATH)
        if a.write_allow:
            seen, ent = set(), []
            for h in hits:
                k = (h["file"], h["text"])
                if k not in seen:
                    seen.add(k)
                    ent.append({"file": h["file"], "line": h["text"], "reason": "pre-existing at 6772f5fcb",
                                "row": "102b-PREEXISTING"})
            L.save_json(apath, {"allow": ent})
            hits = ent
            print("allow-list seeded with %d line(s) -> %s (edit reason/row)" % (len(hits), apath))
            return 0
        data = L.load_json(apath)
        if data is None:
            print("N6: no allow-list at %s" % apath)
            return 2
        added = L.added_lines(a.root, a.since) if a.since else None
        new, stale = evaluate(hits, data.get("allow", []), added)
    except RuntimeError as e:
        print("N6: error: %s" % e)
        return 2
    print("N6 launcher workarounds: %d hit(s), %d allow-listed entries, %d new%s" % (
        len(hits), len(data.get("allow", [])), len(new), " (since " + a.since + ")" if a.since else ""))
    for k in stale:
        print("  stale allow entry (may be deleted): %s: %s" % k)
    for h in new:
        print("  NEW %s %s:%d %s" % (h["pattern"], h["file"], h["line"], h["text"]))
    if new:
        print("FAIL: fix the cause instead of bypassing it, or add an allow entry with reason + 102b ROW ID (checker-owner approval).")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
