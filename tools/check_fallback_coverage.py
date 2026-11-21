# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N3 fallback-coverage checker (doc 124 s2.2 N3; owner rule "no silent fallbacks": ADR-0001, vp-no-silent-fallbacks).

Usage: python -I tools/check_fallback_coverage.py [--root P] [--since REF] [--write-baseline] [--strict]

Site hits (a fallback that is only logged) in production code under src/VP-V and src/VideoProcessor-Lib:
  log     a Log / DebugLog::Log / LogF / GLog / CaptureLog / Logged call whose statement text says
          fallback, falling back, disabled, unavailable, degraded, capped, clamp or skipped
  cap     a std::min against a binding cap constant (kMax...) , e.g. kMaxLead
  plerr   pl_renderer_get_errors( (libplacebo error flags, refs/119 #51)
Each must sit in the same function as a visible report (FallbackStatus, Engage(, Engaged(, Notices, FallbackNotice,
Heading() ) or carry a "// VPV-FALLBACK <id>" tag on the same or previous line (id = a FallbackCatalog id or a
note why none applies). Catalog hits ("catalog:<id>" pseudo-files): every id of core/FallbackCatalog.h needs a
Ctrl+I/notice formatter row in core/FallbackPlainText.h and one reference in a tests file ("catalog:<id>:formatter",
":test"). Baseline = per-file ceilings of untagged log-only sites + missing catalog rows; it only shrinks. A file
not in the baseline has ceiling 0. --since REF lists hits on added lines as NEW. Exit 0 / 1 / 2.
"""
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
BASELINE = "tools/fallback-coverage-baseline.json"
CATALOG = "src/VP-V/core/FallbackCatalog.h"
PLAIN = "src/VP-V/core/FallbackPlainText.h"
LOG_CALL = re.compile(r"\b(DebugLog::Log|Log|LogF|GLog|CaptureLog|Logged)\s*\(")
FB_WORDS = re.compile(r"fall(?:s|ing)?\s*back|fallback|disabled|unavailable|degraded|capped|clamp|skipped", re.I)
CAP = re.compile(r"\(?\s*std::min\s*\)?\s*\([^;]*\bkMax\w+")
PLERR = re.compile(r"\bpl_renderer_get_errors\s*\(")
COVER = re.compile(r"FallbackStatus|\bEngage\s*\(|\bEngaged\s*\(|\bNotices\b|FallbackNotice|\.Heading\s*\(|FallbackCatalog")
TAG = re.compile(r"VPV-FALLBACK\s+\S")
FUNC_TAIL = re.compile(r"\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(final\s*)?(->\s*[\w:<>\s\*&,]+)?\s*$")


def function_spans(text):
    """Outermost function-like {...} blocks as (start, end) offsets in comment-stripped text."""
    spans, stack = [], []
    for i, c in enumerate(text):
        if c == "{":
            head = text[max(0, i - 160):i]
            is_func = bool(FUNC_TAIL.search(head)) or bool(re.search(r"\)\s*:\s*[\w:]+\s*\([^{;]*\)\s*(,\s*[\w:]+\s*\([^{;]*\)\s*)*$", head))
            stack.append((i, is_func))
        elif c == "}" and stack:
            s, f = stack.pop()
            if f and not any(sf for _, sf in stack):
                spans.append((s, i))
    return spans


def site_lines(stripped):
    """[(line_no, kind, offset)] of fallback-ish sites."""
    out, lines = [], stripped.split("\n")
    off = 0
    for no, line in enumerate(lines, 1):
        stmt = line + " " + (lines[no] if no < len(lines) else "")
        if LOG_CALL.search(line) and FB_WORDS.search(stmt):
            out.append((no, "log", off))
        elif CAP.search(line):
            out.append((no, "cap", off))
        elif PLERR.search(line):
            out.append((no, "plerr", off))
        off += len(line) + 1
    return out


def scan_text(rel, text):
    stripped = L.strip_comments(text)
    raw = text.split("\n")
    spans = function_spans(stripped)
    hits = []
    for no, kind, off in site_lines(stripped):
        if TAG.search(raw[no - 1] if no <= len(raw) else "") or (no > 1 and TAG.search(raw[no - 2])):
            continue
        body = stripped
        for s, e in spans:
            if s <= off <= e:
                body = stripped[s:e]
                tagged = any(TAG.search(raw[k]) for k in range(stripped.count("\n", 0, s), min(len(raw), stripped.count("\n", 0, e) + 1)))
                if tagged:
                    body = "FallbackStatus"
                break
        else:
            body = stripped.split("\n")[no - 1]
        if COVER.search(body):
            continue
        hits.append({"file": rel, "line": no, "pattern": kind, "text": stripped.split("\n")[no - 1].strip()[:140]})
    return hits


def catalog_ids(root):
    try:
        text = L.read(os.path.join(root, CATALOG))
    except OSError:
        raise RuntimeError("cannot read " + CATALOG)
    return sorted(set(re.findall(r"\bEngaged\s*\(\s*\"([^\"]+)\"", L.strip_comments(text))))


def catalog_hits(root):
    ids = catalog_ids(root)
    try:
        plain = L.read(os.path.join(root, PLAIN))
    except OSError:
        raise RuntimeError("cannot read " + PLAIN)
    plain_ids = set(re.findall(r"\{\s*\"([^\"]+)\"\s*,\s*\"", plain))
    tests = []
    for dp, dn, fn in os.walk(os.path.join(root, "src")):
        if any(x in dp for x in ("3rdparty", "x64")):
            continue
        for f in fn:
            if f.endswith(("Tests.cpp", "Test.cpp")) or os.sep + "tests" + os.sep in dp + os.sep:
                if f.endswith((".cpp", ".h")):
                    try:
                        tests.append(L.read(os.path.join(dp, f)))
                    except OSError:
                        pass
    blob = "\n".join(tests)
    out = []
    for i in ids:
        if i not in plain_ids:
            out.append({"file": "catalog:%s:formatter" % i, "line": 0, "pattern": "catalog", "text": "no plain-text/Ctrl+I formatter row"})
        if ('"%s"' % i) not in blob:
            out.append({"file": "catalog:%s:test" % i, "line": 0, "pattern": "catalog", "text": "no test references the id"})
    return out


def scan(root):
    hits = []
    for rel, full in L.iter_files(root, ["src/VP-V", "src/VideoProcessor-Lib"], exempt_prefixes=("src/VP-V/tests/",)):
        hits.extend(scan_text(rel, L.read(full)))
    hits.extend(catalog_hits(root))
    return hits


def selftest_failures():
    """Names of the failed selftest checks (empty when all pass): the three site classes (log, cap, plerr) each flagged,
    covered by a visible report or a tag, and not flagged without their words; and catalog_hits on a synthetic tree
    (a missing formatter row, a missing test reference, and a complete catalog)."""
    import shutil
    import tempfile
    failed = []

    def check(name, cond):
        if not cond:
            failed.append(name)
    wrap = "void F() {\n  %s\n}\n"
    for label, line, expect in (
            ("log bad", 'Log("x falling back to y");', "log"),
            ("log: disabled word", 'DebugLog::Log("engine disabled");', "log"),
            ("cap bad", "n = std::min(n, kMaxLead);", "cap"),
            ("plerr bad", "pl_renderer_get_errors(r);", "plerr")):
        found = scan_text("a.cpp", wrap % line)
        check(label, [h["pattern"] for h in found] == [expect])
    for label, body in (
            ("log covered by Engage", 'Log("x falling back to y");\n  fallbacks.Engage("x", "y");'),
            ("log covered by FallbackStatus", 'Log("x unavailable");\n  FallbackStatus st;'),
            ("cap covered by Engage", 'n = std::min(n, kMaxLead);\n  fallbacks.Engage("cap", "n");'),
            ("plerr covered by Notices", "pl_renderer_get_errors(r);\n  Notices(1);"),
            ("log tagged", '// VPV-FALLBACK x\n  Log("x falling back");'),
            ("cap tagged", "// VPV-FALLBACK kMaxLead: not a fallback\n  n = std::min(n, kMaxLead);"),
            ("plerr tagged", "// VPV-FALLBACK pl: note\n  pl_renderer_get_errors(r);"),
            ("log without a fallback word", 'Log("frame 12 presented");'),
            ("std::min without a kMax cap", "n = std::min(n, limit);")):
        check(label, not scan_text("a.cpp", wrap % body))
    check("log in a comment is no site", not scan_text("a.cpp", wrap % '// Log("x falling back");'))
    root = tempfile.mkdtemp()
    try:
        def put(rel, text):
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
        put(CATALOG, 'static const Entry a = Engaged("alpha");\n')
        put(PLAIN, '{ "alpha", "Alpha fell back" },\n')
        put("src/VP-V/tests/FallbackTests.cpp", 'Assert::IsTrue(Has("alpha"));\n')
        check("catalog ids read", catalog_ids(root) == ["alpha"])
        check("complete catalog has no hit", catalog_hits(root) == [])
        put(PLAIN, '{ "other", "Other" },\n')
        check("missing formatter row", [h["file"] for h in catalog_hits(root)] == ["catalog:alpha:formatter"])
        put(PLAIN, '{ "alpha", "Alpha fell back" },\n')
        put("src/VP-V/tests/FallbackTests.cpp", "// no reference\n")
        check("missing test reference", [h["file"] for h in catalog_hits(root)] == ["catalog:alpha:test"])
        put(CATALOG, 'static const Entry a = Engaged("alpha");\nstatic const Entry b = Engaged("beta");\n')
        put("src/VP-V/tests/FallbackTests.cpp", 'Has("alpha"); Has("beta");\n')
        check("a second id needs its own formatter row", [h["file"] for h in catalog_hits(root)] == ["catalog:beta:formatter"])
    finally:
        shutil.rmtree(root, True)
    return failed


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if "--selftest" in argv:
        return L.selftest_ok("fallback coverage", not selftest_failures())
    return L.run_ratchet("N3 fallback-coverage", scan, argv, BASELINE,
                         "a fallback / binding cap that is only logged; report it through FallbackStatus (Ctrl+I, notice) or tag // VPV-FALLBACK <id>.",
                         per_class=lambda hits: {k: sum(1 for h in hits if h["pattern"] == k) for k in ("log", "cap", "plerr", "catalog")})


if __name__ == "__main__":
    sys.exit(main())
