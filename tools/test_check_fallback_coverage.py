# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_fallback_coverage.py")
_s = importlib.util.spec_from_file_location("check_fallback_coverage", _p)
N3 = importlib.util.module_from_spec(_s)
_s.loader.exec_module(N3)


def _w(root, rel, text):
    full = os.path.join(root, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", newline="\n") as fh:
        fh.write(text)


def _run(root, *args):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = N3.main(["--root", root] + list(args))
    return rc, buf.getvalue()


CATALOG = 'inline D a() { return Engaged("alpha", "r"); }\ninline D b() { return Engaged("beta", "r"); }\n'
PLAIN = 'static const E t[] = {\n { "alpha", "Alpha sentence" },\n};\n'


class SiteTests(unittest.TestCase):
    def test_log_only_flags_and_report_covers(self):
        self.assertEqual(len(N3.scan_text("a.cpp", 'void F() {\n Log("x: falling back to y");\n}\n')), 1)
        self.assertEqual(N3.scan_text("a.cpp", 'void F() {\n Log("x: falling back");\n g.Engage("x", "r");\n}\n'), [])

    def test_tag_waives(self):
        self.assertEqual(N3.scan_text("a.cpp", 'void F() {\n // VPV-FALLBACK alpha\n Log("disabled");\n}\n'), [])
        self.assertEqual(len(N3.scan_text("a.cpp", 'void F() {\n // VPV-FALLBACK\n Log("disabled");\n}\n')), 1)

    def test_cap_and_plerr(self):
        self.assertEqual(N3.scan_text("a.cpp", 'int F() {\n return (std::min)(x, kMaxLead);\n}\n')[0]["pattern"], "cap")
        self.assertEqual(N3.scan_text("a.cpp", 'void F() {\n pl_renderer_get_errors(rr);\n}\n')[0]["pattern"], "plerr")

    def test_other_function_does_not_cover(self):
        t = 'void A() {\n Log("fell back, disabled");\n}\nvoid B() {\n g.Engage("x","y");\n}\n'
        self.assertEqual(len(N3.scan_text("a.cpp", t)), 1)

    def test_ordinary_log_and_comment_ignored(self):
        self.assertEqual(N3.scan_text("a.cpp", 'void F() {\n Log("started");\n // Log("disabled");\n}\n'), [])


class CatalogTests(unittest.TestCase):
    def test_catalog_gaps_and_ratchet(self):
        with tempfile.TemporaryDirectory() as r:
            _w(r, N3.CATALOG, CATALOG)
            _w(r, N3.PLAIN, PLAIN)
            _w(r, "src/VP-V/tests/FallbackCatalogTests.cpp", 'CHECK(Id() == "alpha");\n')
            files = sorted(h["file"] for h in N3.catalog_hits(r))
            self.assertEqual(files, ["catalog:beta:formatter", "catalog:beta:test"])
            self.assertEqual(_run(r, "--write-baseline")[0], 0)
            self.assertEqual(_run(r)[0], 0)
            _w(r, N3.CATALOG, CATALOG + 'inline D c() { return Engaged("gamma", "r"); }\n')
            rc, out = _run(r)
            self.assertEqual(rc, 1)
            self.assertIn("catalog:gamma:formatter", out)

    def test_selftest(self):
        self.assertEqual(N3.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
