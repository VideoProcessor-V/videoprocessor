# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import io
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout

_d = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    s = importlib.util.spec_from_file_location(name, os.path.join(_d, name + ".py"))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


N4 = _load("check_general_not_fixed")
L = _load("compliance_lib")


def _w(root, rel, text):
    full = os.path.join(root, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", newline="\n") as fh:
        fh.write(text)


def _run(root, *args):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = N4.main(["--root", root] + list(args))
    return rc, buf.getvalue()


def _g(root, *a):
    subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t"] + list(a), check=True, capture_output=True)


class ClassifyTests(unittest.TestCase):
    def test_rates_and_sizes(self):
        self.assertEqual(N4.classify_line("double refreshHz = 60.0;"), ["rate"])
        self.assertEqual(N4.classify_line("x = 59.94;"), ["rate"])
        self.assertEqual(N4.classify_line("int frameWidth = 1920;"), [])
        self.assertEqual(N4.classify_line("int outputHeight = 2160;"), ["size"])
        self.assertEqual(N4.classify_line("double seconds = 60.0;"), [])

    def test_comments_ignored_and_tag_waives(self):
        self.assertEqual(N4.scan_text("a.h", "// fps = 24\n"), [])
        self.assertEqual(N4.scan_text("a.h", "// GENERAL-REF: ADR-0018 reference\nconstexpr double kRefFps = 23.976;\n"), [])
        self.assertEqual(len(N4.scan_text("a.h", "// GENERAL-REF:\nconstexpr double kRefFps = 23.976;\n")), 1)
        self.assertEqual(len(N4.scan_text("a.h", "constexpr double kRefFps = 23.976; // not a tag\n")), 1)


class RatchetTests(unittest.TestCase):
    def test_ratchet_and_shrink(self):
        with tempfile.TemporaryDirectory() as r:
            _w(r, "src/VP-V/core/A.h", "double fps = 24;\ndouble hz = 60;\n")
            self.assertEqual(_run(r)[0], 2)  # no baseline
            self.assertEqual(_run(r, "--write-baseline")[0], 0)
            self.assertEqual(_run(r)[0], 0)
            _w(r, "src/VP-V/core/A.h", "double fps = 24;\ndouble hz = 60;\nint outWidth = 3840;\n")
            rc, out = _run(r)
            self.assertEqual(rc, 1)
            self.assertIn("GREW", out)
            self.assertEqual(_run(r, "--write-baseline")[0], 1)  # refuses growth
            _w(r, "src/VP-V/core/A.h", "double hz = 60;\n")
            self.assertEqual(_run(r)[0], 0)
            self.assertEqual(_run(r, "--write-baseline")[0], 0)

    def test_new_file_ceiling_zero_and_tests_skipped(self):
        with tempfile.TemporaryDirectory() as r:
            _w(r, "src/VP-V/core/A.h", "int x;\n")
            _run(r, "--write-baseline")
            _w(r, "src/VP-V/tests/T.cpp", "double fps = 24;\n")
            self.assertEqual(_run(r)[0], 0)
            _w(r, "src/VP-V/core/B.h", "double fps = 24;\n")
            self.assertEqual(_run(r)[0], 1)

    def test_since_lists_new_lines(self):
        with tempfile.TemporaryDirectory() as r:
            _g(r, "init", "-q")
            _w(r, "src/VP-V/core/A.h", "double fps = 24;\n")
            _g(r, "add", "-A")
            _g(r, "commit", "-qm", "b")
            base = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            _run(r, "--write-baseline")
            _w(r, "src/VP-V/core/A.h", "double fps = 24;\nint outputWidth = 3840;\n")
            _g(r, "add", "-A")
            rc, out = _run(r, "--since", base)
            self.assertEqual(rc, 1)
            self.assertIn("NEW src/VP-V/core/A.h:2", out)

    def test_selftest(self):
        self.assertEqual(N4.main(["--selftest"]), 0)


class LibTests(unittest.TestCase):
    def test_strip_comments_keeps_strings(self):
        t = 'a = "x // y"; // c\n/* b\n c */ d\n'
        self.assertEqual(L.strip_comments(t), 'a = "x // y"; \n\n d\n')

    def test_ratchet_function(self):
        g, s = L.ratchet({"a": 3, "b": 1}, {"files": {"a": 2, "b": 2, "c": 1}})
        self.assertEqual(g, [("a", 3, 2)])
        self.assertEqual(sorted(x[0] for x in s), ["b", "c"])


if __name__ == "__main__":
    unittest.main()
