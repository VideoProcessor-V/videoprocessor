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
            ".yaml", ".toml", ".props", ".targets", ".vcxproj", ".filters", ".sln", ".cmake", ".rc", ".def", ".iss",
            ".inl", ".idl", ".cs", ".inc", ".patch", ".diff", ".csv", ".gitattributes", ".gitignore")
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


def looks_binary(path):
    """Content sniff for extensionless files: real binaries are dense in control bytes, a corrupted text file is not."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(8192)
    except OSError:
        return False
    if head[:2] in (codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE) or not head:
        return True
    ctl = sum(1 for c in head if c < 32 and c not in (9, 10, 13))
    return ctl * 20 > len(head)


def select(root, names, git_binary):
    """Known text extensions are ALWAYS scanned (git calls a .cpp with an early NUL binary: that is the corruption this
    tool exists to find; .patch files are -text). Extensionless files are decided by content. Any other extension
    follows git's binary detection, and in full-tree mode is not scanned at all."""
    out = []
    for n in names:
        low = n.lower()
        ext = os.path.splitext(low)[1]
        if low.endswith(SKIP_EXT):
            continue
        path = os.path.join(root, n)
        if ext in TEXT_EXT:
            out.append(path)
        elif not ext:
            if not looks_binary(path):
                out.append(path)
        elif git_binary is not None and n not in git_binary:
            out.append(path)
    return out


def changed_files(root, base):
    names = [n for n in git(root, "diff", "--name-only", "--diff-filter=ACMR", base + "..HEAD").splitlines() if n]
    return select(root, names, binary_set(root, base) | attr_binary(root, names))


def tracked_files(root):
    return select(root, [n for n in git(root, "ls-files").splitlines() if n], None)


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
        with open(f, "rb") as fh:
            raw = fh.read()
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
             "cr.h": (b"a\rb\r\n", 1), "two.h": (b"a\x00b\x08c\r\n", 2),
             "clean.h": (b"\ta\tb\r\n\tc\r\nlast\n", 0),
             "utf16le.h": (codecs.BOM_UTF16_LE + "ok\r\n".encode("utf-16-le"), 0),
             "utf16be.h": (codecs.BOM_UTF16_BE + "ok\r\n".encode("utf-16-be"), 0)}
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for name, (data, want) in cases.items():
            p = os.path.join(d, name)
            with open(p, "wb") as fh:
                fh.write(data)
            got = scan_file(p, out=lambda _x: None)
            ok = (got == want)
            print("selftest %-10s %s (got=%d want=%d)" % (name, "ok" if ok else "WRONG", got, want))
            bad += 0 if ok else 1
        # looks_binary / select / walk(.bak-) checks
        bin_extless = os.path.join(d, "binblob")
        txt_extless = os.path.join(d, "README")
        bak_file = os.path.join(d, "keep.h.bak-1")
        png_file = os.path.join(d, "img.png")
        with open(bin_extless, "wb") as fh:
            fh.write(bytes([0, 1, 2, 3, 4]) * 100)
        with open(txt_extless, "wb") as fh:
            fh.write(b"plain text\n")
        with open(bak_file, "wb") as fh:
            fh.write(b"a\x00b\n")
        with open(png_file, "wb") as fh:
            fh.write(b"\x89PNG\r\n\x1a\n")
        extra_checks = [
            ("looks_bin_true", looks_binary(bin_extless) is True),
            ("looks_bin_utf16", looks_binary(os.path.join(d, "utf16le.h")) is True),
            ("looks_bin_false", looks_binary(txt_extless) is False),
            ("select", set(select(d, ["nul.h", "img.png", "README", "binblob"], None))
             == {os.path.join(d, "nul.h"), txt_extless}),
            ("walk_bak_skip", bak_file not in list(walk([d])) and png_file not in list(walk([d]))),
        ]
        for label, ok in extra_checks:
            print("selftest %-15s %s" % (label, "ok" if ok else "WRONG"))
            bad += 0 if ok else 1
    return 1 if bad else 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):  # a hit list must never be lost to a console codec error
        try:
            stream.reconfigure(errors="backslashreplace")
        except (AttributeError, ValueError):
            pass
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
    try:
        return scan(files)
    except Exception as e:  # never exit 0 after a crash
        print("ctlscan: ERROR " + ascii(e))
        return 2


if __name__ == "__main__":
    sys.exit(main())
