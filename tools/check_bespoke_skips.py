#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-X3: a test must not skip by logging free text and returning.

A native test that logs "... skipped ...", "... not run" or "nothing run" through Logger::WriteMessage and then
`return;`s (or `continue;`s) reports Passed with no SKIP-* marker, so tools/check_trx_coverage.py counts it as an
EXECUTED pass. The house convention is HardwarePrerequisites::RequireOrSkip(Kind, ...), whose marker
(SKIP-NO-HARDWARE / -GPU / -FOOTAGE / -CORPUS / SKIP-OPT-IN) counts as not executed and fails under
VP_TEST_REQUIRE_<KIND>=1.

Usage: python -I tools/check_bespoke_skips.py [--root P] [--baseline F] [--write-baseline] [--selftest]

A hit is a Logger::WriteMessage call (found in code, never in a comment or string) whose text says skip / skipped /
not run / nothing run / cannot run, carries no SKIP-<KIND> marker, and is followed at once by return; or continue;.
Hits are counted per "<file>::<Class>::<Method>" (a hit outside any test method counts for the file's last
method, or "<file>::<file>") against tools/bespoke-skips-baseline.json: a key may not grow, an unlisted key has
ceiling 0, the baseline only shrinks (the area's WAIVER commit that converts a skip to RequireOrSkip lowers it with
--write-baseline). Not detected: a skip through a helper (Suite::Skip, SrcLatch::Skip), or a silent `return;` with no
log line; those are the test rows' own fixes. Exit 0 / 1 violation / 2 error.
"""
import importlib.util
import os
import re
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)
import categories_lib as lib  # noqa: E402


def _compliance():
    s = importlib.util.spec_from_file_location("compliance_lib", os.path.join(_here, "compliance_lib.py"))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


C = _compliance()
BASELINE = "tools/bespoke-skips-baseline.json"
CALL_RE = re.compile(r"\bLogger::WriteMessage\s*\(")
SKIP_WORDS = re.compile(r"\bskip(?:s|ped|ping)?\b|not run\b|nothing run\b|cannot run\b", re.I)
MARKER = re.compile(r"SKIP-[A-Z]")
BAIL_RE = re.compile(r"\s*(?:return|continue)\s*;")


def _call_end(code, open_paren):
    """Index just after the ')' that closes the '(' at open_paren, in comment/string-blanked code; None if it never closes."""
    depth = 0
    for i in range(open_paren, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return None


def scan_text(rel, text):
    """Hits [{file, line, text}] of one test source; file is the "<rel>::<Class>::<Method>" key."""
    tests = lib.scan_text(text, rel)
    code = lib.blank_comments_and_strings(lib.strip_defines(text)[0])
    hits = []
    for m in CALL_RE.finditer(code):
        end = _call_end(code, m.end() - 1)
        if end is None:
            continue
        message = text[m.start():end]
        if not SKIP_WORDS.search(message) or MARKER.search(message):
            continue
        after = code[end:]
        semi = re.match(r"\s*;", after)
        if not semi or not BAIL_RE.match(after[semi.end():]):
            continue
        owner = None
        for t in tests:
            if t["pos"] <= m.start():
                owner = t
        key = "%s::%s::%s" % (rel, owner["cls"], owner["method"]) if owner else "%s::<file>" % rel
        hits.append({"file": key, "line": text.count("\n", 0, m.start()) + 1, "text": " ".join(message.split())[:140]})
    return hits


def scan(root):
    hits = []
    for path in lib.test_sources(root):
        rel = os.path.relpath(path, root).replace("\\", "/")
        with open(path, encoding="utf-8", errors="replace") as f:
            hits.extend(scan_text(rel, f.read()))
    return hits


SELFTEST_BAD = 'TEST_CLASS(C)\n{\n\tTEST_METHOD(Skips)\n\t{\n\t\tif (!x) { Logger::WriteMessage("clip missing: skipped\\n"); return; }\n\t}\n};\n'
SELFTEST_GOOD = ('TEST_CLASS(C)\n{\n\tTEST_METHOD(Marked)\n\t{\n\t\tif (!x) { Logger::WriteMessage("SKIP-NO-FOOTAGE Marked: skipped\\n"); return; }\n\t}\n'
                 '\tTEST_METHOD(Reports)\n\t{\n\t\tLogger::WriteMessage("3 reels run, 1 skipped");\n\t\tAssert::IsTrue(ok);\n\t}\n'
                 '\t// Logger::WriteMessage("commented skipped"); return;\n};\n')


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--selftest" in argv:
        ok = (len(scan_text("a.cpp", SELFTEST_BAD)) == 1 and scan_text("a.cpp", SELFTEST_BAD)[0]["file"] == "a.cpp::C::Skips"
              and not scan_text("a.cpp", SELFTEST_GOOD))
        return C.selftest_ok("bespoke-skips", ok)
    return C.run_ratchet("TQ-X3 bespoke-skips", scan, argv, BASELINE,
                         "a test logs a skip and returns with no SKIP-<KIND> marker; use HardwarePrerequisites::RequireOrSkip(Kind::...) "
                         "(it fails under VP_TEST_REQUIRE_<KIND>=1 and counts as not executed in check_trx_coverage), then lower the baseline.")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except lib.ScanError as e:
        print("FAIL %s" % e)
        sys.exit(2)
