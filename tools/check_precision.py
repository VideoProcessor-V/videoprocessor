# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N2 precision ratchet (doc 124 s2.2 N2; principle: 12-bit end to end; sweep refs/119).

Usage: python -I tools/check_precision.py [--root P] [--since REF] [--write-baseline] [--strict]

Greps production code (src/VP-V, src/VideoProcessor-Lib, src/VideoProcessor-VPRenderer; tests skipped,
comments stripped) for sub-12-bit video paths:

  fmt     DXGI R8 / R8G8 / RGBA8 / BGRA8 / R10G10B10A2 / R16_FLOAT / R16G16_FLOAT, TexFormat R8,
          RG16F / RGBA16F / R16F, pl format strings "r8" "rgba8" "rgba16hf" "r16f"
  shift   `>> 2|4|6` on a line that names a sample (luma / chroma / plane / code / word / pixel ...)
  u8      uint8_t pixel buffers (uint8_t* / vector<uint8_t> on a plane / pixel / luma / rgb line)

Each file is keyed in the baseline "sites" map to a refs/119 row and a class: A allowed, O allowed only with
a row ID (Bill-owned), T and R counted (to fix), U unclassified. Gate: per-file counts and per-class totals
only shrink; a file not in the baseline has ceiling 0; --since REF lists hits on added lines as NEW. A new
legitimate site needs a baseline entry with a refs/119 row ID (checker-owner approval); an inline
"// PRECISION-OK <ROW>: <why>" on the same or previous line waives one line.
Seed (refs/119): R 13, T 10, O 8, A 28 = 59 sites. Exit 0 / 1 violation / 2 error.
"""
import argparse
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
BASELINE = "tools/precision-baseline.json"
SCAN_ROOTS = ["src/VP-V", "src/VideoProcessor-Lib", "src/VideoProcessor-VPRenderer"]
FMT = re.compile(r"DXGI_FORMAT_(R8_UNORM|R8G8_UNORM|R8G8B8A8_UNORM|B8G8R8A8_UNORM|R10G10B10A2_UNORM|R16_FLOAT|R16G16_FLOAT)\b"
                 r"|\b(R8_UNORM|TexFormat::R8|RG16F|RGBA16F|R16F)\b|\"(r8|rgba8|rgba16hf|r16f)\"")
SHIFT = re.compile(r">>\s*(2|4|6)\b")
SHIFT_CTX = re.compile(r"luma|chroma|plane|sample|code|word|y10|intensity|p010", re.I)
U8 = re.compile(r"uint8_t\s*\*|vector\s*<\s*(std::)?uint8_t")
U8_CTX = re.compile(r"pixel|plane|luma|chroma", re.I)
TAG = re.compile(r"PRECISION-OK\s+\S+:\s*\S")
CLASSES = ("A", "O", "T", "R", "U")


def classify_line(line):
    ids = []
    if FMT.search(line):
        ids.append("fmt")
    if SHIFT.search(line) and SHIFT_CTX.search(line):
        ids.append("shift")
    if U8.search(line) and U8_CTX.search(line):
        ids.append("u8")
    return ids


def scan_text(rel, text, sites):
    raw = text.splitlines()
    cls = sites.get(rel, {}).get("class", "U")
    out = []
    for no, line in enumerate(L.strip_comments(text).splitlines(), 1):
        ids = classify_line(line)
        if not ids:
            continue
        if TAG.search(raw[no - 1] if no <= len(raw) else "") or (no > 1 and TAG.search(raw[no - 2])):
            continue
        out.append({"file": rel, "line": no, "pattern": "+".join(ids), "class": cls, "text": line.strip()[:140]})
    return out


def scan(root, sites):
    roots = [r for r in SCAN_ROOTS if os.path.isdir(os.path.join(root, r))]
    hits = []
    for rel, full in L.iter_files(root, roots, exempt_prefixes=("src/VP-V/tests/",)):
        hits.extend(scan_text(rel, L.read(full), sites))
    return hits


def per_class(hits):
    c = {k: 0 for k in CLASSES}
    for h in hits:
        c[h["class"]] = c.get(h["class"], 0) + 1
    return c


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if "--selftest" in argv:
        ok = classify_line("DXGI_FORMAT_R8_UNORM,") == ["fmt"] and classify_line("v = luma >> 6;") == ["shift"] \
            and classify_line("std::vector<uint8_t> pixels;") == ["u8"] and not classify_line("int x = y >> 6;") \
            and not scan_text("a.h", "// PRECISION-OK 119#7: bytes\nx = \"r8\";\n", {})
        return L.selftest_ok("precision", ok)
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--root", default=L.repo_root())
    pre.add_argument("--baseline")
    known, _ = pre.parse_known_args(argv)
    base = L.load_json(known.baseline or os.path.join(known.root, BASELINE)) or {}
    sites = base.get("sites", {})

    def extra(old, hits):
        keep = dict(sites)
        for h in hits:
            keep.setdefault(h["file"], {"row": "UNCLASSIFIED", "class": "U"})
        return {"sites": {f: v for f, v in keep.items() if any(h["file"] == f for h in hits)}}

    return L.run_ratchet("N2 precision", lambda root: scan(root, sites), argv, BASELINE,
                         "a new sub-12-bit format/shift/buffer; keep 12-bit (f32/unorm16) or add a baseline site with a refs/119 row ID.",
                         per_class=per_class, extra_fn=extra)


if __name__ == "__main__":
    sys.exit(main())
