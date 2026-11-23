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
# A named constant equal to an axis shift (2/4/6) counts exactly like the literal (rule 19: renaming the
# literal to a constant of the same value is not a fix).
SHIFT_CONST_DEF = re.compile(r"\b(?:constexpr|const)\s+(?:static\s+)?(?:inline\s+)?[\w:]+\s+(\w+)\s*=\s*(?:2|4|6)\s*[;,]")
# CHK-SHIFT-AXISFRAC (102b; lead17 c4-s2a-fix G6): `q6` (a sample on the Q6 analysis axis) and `axis` (so a shift by
# the axis constant kAxisFractionBits names its own context) are sample words too; `q6 >> kAxisFractionBits` is the
# same 16 -> 10 floor as `plane >> 6` (AnalysisAxis10.h Floor10FromQ6 / HistBinFromQ6).
SHIFT_CTX = re.compile(r"luma|chroma|plane|sample|code|word|y10|intensity|p010|q6|axis", re.I)
U8 = re.compile(r"uint8_t\s*\*|vector\s*<\s*(std::)?uint8_t")
U8_CTX = re.compile(r"pixel|plane|luma|chroma", re.I)
TAG = re.compile(r"PRECISION-OK\s+\S+:\s*\S")
CLASSES = ("A", "O", "T", "R", "U")


def shift_constants(text):
    return set(SHIFT_CONST_DEF.findall(text))


_NAMED_CACHE = {}


def _named_shift(names):
    if not names:
        return None
    key = frozenset(names)
    if key not in _NAMED_CACHE:
        _NAMED_CACHE[key] = _build_named_shift(key)
    return _NAMED_CACHE[key]


def _build_named_shift(names):
    return re.compile(r">>\s*(?:[\w:]*::)?(?:" + "|".join(re.escape(n) for n in sorted(names)) + r")\b")


def classify_line(line, names=None):
    ids = []
    if FMT.search(line):
        ids.append("fmt")
    named = _named_shift(names)
    if (SHIFT.search(line) or (named and named.search(line))) and SHIFT_CTX.search(line):
        ids.append("shift")
    if U8.search(line) and U8_CTX.search(line):
        ids.append("u8")
    return ids


def scan_text(rel, text, sites, names=None):
    raw = text.splitlines()
    names = set(names or ()) | shift_constants(text)
    cls = sites.get(rel, {}).get("class", "U")
    out = []
    for no, line in enumerate(L.strip_comments(text).splitlines(), 1):
        ids = classify_line(line, names)
        if not ids:
            continue
        if TAG.search(raw[no - 1] if no <= len(raw) else "") or (no > 1 and TAG.search(raw[no - 2])):
            continue
        out.append({"file": rel, "line": no, "pattern": "+".join(ids), "class": cls, "text": line.strip()[:140]})
    return out


def scan(root, sites):
    roots = [r for r in SCAN_ROOTS if os.path.isdir(os.path.join(root, r))]
    files = [(rel, L.read(full)) for rel, full in L.iter_files(root, roots, exempt_prefixes=("src/VP-V/tests/",))]
    names = set()
    for _, text in files:
        names |= shift_constants(L.strip_comments(text))
    hits = []
    for rel, text in files:
        hits.extend(scan_text(rel, text, sites, names))
    return hits


def per_class(hits):
    c = {k: 0 for k in CLASSES}
    for h in hits:
        c[h["class"]] = c.get(h["class"], 0) + 1
    return c


def selftest_failures():
    """Names of the failed selftest checks (empty when all pass): every format spelling, the literal axis shifts, a named
    shift constant (constexpr/const, plain and namespace-qualified, only for the values 2/4/6), the uint8 pixel-buffer rule
    with its context word, and the PRECISION-OK waiver on the same or the previous line."""
    failed = []

    def check(name, cond):
        if not cond:
            failed.append(name)
    for fmt in ("DXGI_FORMAT_R8_UNORM,", "DXGI_FORMAT_R8G8_UNORM", "DXGI_FORMAT_R8G8B8A8_UNORM", "DXGI_FORMAT_B8G8R8A8_UNORM",
                "DXGI_FORMAT_R10G10B10A2_UNORM", "DXGI_FORMAT_R16_FLOAT", "DXGI_FORMAT_R16G16_FLOAT", "x = R8_UNORM;",
                "TexFormat::R8", "auto f = RGBA16F;", 'fmt = "rgba8";', 'fmt = "r16f";'):
        check("format: " + fmt, classify_line(fmt) == ["fmt"])
    for fmt in ("DXGI_FORMAT_R32_FLOAT", "DXGI_FORMAT_R16_UNORM", "int r8bits = 3;"):
        check("not a format: " + fmt, classify_line(fmt) == [])
    for n in (2, 4, 6):
        check("literal shift %d" % n, classify_line("v = luma >> %d;" % n) == ["shift"])
    check("shift 3 is not an axis shift", classify_line("v = luma >> 3;") == [])
    check("shift 8 is not an axis shift", classify_line("v = luma >> 8;") == [])
    check("a shift without a sample word", classify_line("int x = y >> 6;") == [])
    check("shift context words", all(classify_line("v = %s >> 4;" % w) == ["shift"] for w in ("chroma", "plane", "sample", "code", "word", "y10", "intensity", "p010")))
    defs = shift_constants("constexpr int kAxisShift = 4;\nstatic const uint32_t kOther = 5;\nconstexpr int kSix = 6, kEight = 8;\n")
    check("named shift constants are the 2/4/6 ones", defs == {"kAxisShift", "kSix"})
    check("named shift is flagged like the literal", classify_line("v = luma >> kAxisShift;", {"kAxisShift"}) == ["shift"])
    check("namespace-qualified named shift", classify_line("v = luma >> Ns::kAxisShift;", {"kAxisShift"}) == ["shift"])
    check("a named constant not in the set is not flagged", classify_line("v = luma >> kOther;", {"kAxisShift"}) == [])
    check("named shift without a sample word", classify_line("v = a >> kAxisShift;", {"kAxisShift"}) == [])
    check("named shift needs the names", classify_line("v = luma >> kAxisShift;") == [])
    check("scan_text takes a constant defined in the same file",
          [h["pattern"] for h in scan_text("a.h", "constexpr int kS = 6;\nauto v = luma >> kS;\n", {})] == ["shift"])
    check("scan_text takes a constant from other files", [h["pattern"] for h in scan_text("a.h", "auto v = luma >> kS;\n", {}, {"kS"})] == ["shift"])
    check("a constant of another value is not a shift", not scan_text("a.h", "constexpr int kS = 7;\nauto v = luma >> kS;\n", {}))
    check("u8 pixel buffer", classify_line("std::vector<uint8_t> pixels;") == ["u8"] and classify_line("uint8_t* luma;") == ["u8"])
    check("u8 without a pixel word", classify_line("std::vector<uint8_t> bytes;") == [])
    check("shift and u8 together", classify_line("uint8_t* plane = p >> 4;") == ["shift", "u8"])
    waived = "// PRECISION-OK 119#7: bytes\nx = \"r8\";\n"
    check("waiver on the previous line", not scan_text("a.h", waived, {}))
    check("waiver on the same line", not scan_text("a.h", 'x = "r8"; // PRECISION-OK 119#7: bytes\n', {}))
    check("a waiver needs a reason", len(scan_text("a.h", '// PRECISION-OK 119#7:\nx = "r8";\n', {})) == 1)
    check("a waiver two lines up does not count", len(scan_text("a.h", '// PRECISION-OK 119#7: why\n\nx = "r8";\n', {})) == 1)
    check("comments are stripped", not scan_text("a.h", '// x = "r8";\n', {}))
    check("the class comes from the sites table", scan_text("a.h", 'x = "r8";\n', {"a.h": {"class": "A"}})[0]["class"] == "A")
    check("an unlisted file is class U", scan_text("a.h", 'x = "r8";\n', {})[0]["class"] == "U")
    return failed


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if "--selftest" in argv:
        return L.selftest_ok("precision", not selftest_failures())
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
