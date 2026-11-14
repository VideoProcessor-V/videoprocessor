# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Control-byte scan (rule 29 addendum 8; R14-84).

Finds the escape corruption that shell/sed/non-raw string handling causes in Windows paths and regexes:
backslash-b, -v, -f, -a, -cX and -NNN sequences become control bytes, a lone CR appears, or a stray NUL. Tabs, LF and
CRLF are allowed. It cannot see newline or tab corruption (they look like ordinary breaks/tabs): review by eye.

Usage:
  python -I tools/ctlscan.py [--root R] --since BASE   scan exactly the files of
                                                       git diff --name-only --diff-filter=ACMR BASE..HEAD
  python -I tools/ctlscan.py [--root R]                scan every tracked text file (git ls-files)
  python -I tools/ctlscan.py PATH [PATH ...]           scan files / directories (dirs: recursive, *.bak-* skipped)
  python -I tools/ctlscan.py --selftest                run the built-in fixtures

Binary files are skipped (git numstat '-' or a `binary` gitattribute; otherwise a file whose first 8 KiB
holds a NUL is not auto-skipped: a NUL is exactly what this tool reports, so use numstat/gitattributes).
Exit: 1 on any hit, 0 clean, 2 on a git/usage error.
"""
import argparse
import codecs
import os
import re
import subprocess
import sys
import tempfile

PAT = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f]|\r(?!\n)")
TEXT_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hlsl", ".glsl", ".vert", ".frag", ".comp", ".py", ".ps1",
            ".cmd", ".bat", ".sh", ".md", ".txt", ".json", ".cfg", ".ini", ".xml", ".html", ".css", ".js", ".yml",
            ".yaml", ".toml", ".props", ".targets", ".vcxproj", ".filters", ".sln", ".cmake", ".rc", ".def", ".iss")
SKIP_EXT = (".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".dll", ".exe", ".lib", ".pdb", ".zip", ".7z",
            ".onnx", ".bin", ".pdf", ".ttf", ".otf", ".woff", ".woff2", ".mp4", ".mkv", ".obj", ".pyc")


def git(root, *args):
    r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def binary_set(root, base):
    """Paths git treats as binary: numstat '-' for the diff, plus the gitattributes binary flag."""
    out = set()
    rng = [base + "..HEAD"] if base else []
    for line in git(root, "diff", "--numstat", "--diff-filter=ACMR", *rng).splitlines() if base else []:
        p = line.split("\t")
        if len(p) == 3 and p[0] == "-" and p[1] == "-":
            out.add(p[2])
    return out


def attr_binary(root, paths):
    if not paths:
        return set()
    r = subprocess.run(["git", "-C", root, "check-attr", "-z", "binary", "diff", "text", "--stdin", "-z"],
                       input="\0".join(paths) + "\0", capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    items = r.stdout.split("\0")
    bad = set()
    for i in range(0, len(items) - 2, 3):
        path, attr, val = items[i], items[i + 1], items[i + 2]
        if (attr == "binary" and val == "set") or (attr == "diff" and val == "unset") or (attr == "text" and val == "unset"):
            bad.add(path)
    return bad


def changed_files(root, base):
    names = [n for n in git(root, "diff", "--name-only", "--diff-filter=ACMR", base + "..HEAD").splitlines() if n]
    skip = binary_set(root, base) | attr_binary(root, names)
    return [os.path.join(root, n) for n in names if n not in skip and not n.lower().endswith(SKIP_EXT)]


def tracked_files(root):
    names = [n for n in git(root, "ls-files").splitlines() if n.lower().endswith(TEXT_EXT)]
    skip = attr_binary(root, names)
    return [os.path.join(root, n) for n in names if n not in skip and not n.lower().endswith(SKIP_EXT)]


def walk(args):
    for a in args:
        if os.path.isdir(a):
            for d, _, names in os.walk(a):
                for n in sorted(names):
                    if ".bak-" not in n and not n.lower().endswith(SKIP_EXT):
                        yield os.path.join(d, n)
        else:
            yield a


def scan_file(f, out=print):
    hits = 0
    try:
        raw = open(f, "rb").read()
        if raw[:2] in (codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE):
            return 0  # UTF-16 text legitimately holds NULs
        s = raw.decode("utf-8", errors="replace")
    except OSError as e:
        out("%s: unreadable (%s)" % (f, e))
        return 1
    for m in PAT.finditer(s):
        a, b = max(0, m.start() - 40), min(len(s), m.end() + 40)
        out("%s:%d: %s" % (f, s.count("\n", 0, m.start()) + 1, ascii(s[a:b])[1:-1]))
        hits += 1
    return hits


def scan(files, out=print):
    hits = sum(scan_file(f, out) for f in files)
    out("ctlscan: %d hit(s) in %d file(s)" % (hits, len(files)))
    return 1 if hits else 0


def selftest():
    cases = {"nul.h": (b"a\x00b\r\n", 1), "vt.h": (b"a\x0bb\r\n", 1), "bs.h": (b"a\x08b\r\n", 1),
             "cr.h": (b"a\rb\r\n", 1), "clean.h": (b"\ta\tb\r\n\tc\r\nlast\n", 0)}
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for name, (data, want) in cases.items():
            p = os.path.join(d, name)
            with open(p, "wb") as fh:
                fh.write(data)
            got = scan_file(p, out=lambda _x: None)
            ok = (got > 0) == (want > 0)
            print("selftest %-8s %s" % (name, "ok" if ok else "WRONG"))
            bad += 0 if ok else 1
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--root", default=None)
    ap.add_argument("--since", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    try:
        if a.paths:
            return scan(list(walk(a.paths)))
        root = a.root or os.getcwd()
        files = changed_files(root, a.since) if a.since else tracked_files(root)
    except RuntimeError as e:
        print("ctlscan: " + str(e))
        return 2
    return scan(files)


if __name__ == "__main__":
    sys.exit(main())
