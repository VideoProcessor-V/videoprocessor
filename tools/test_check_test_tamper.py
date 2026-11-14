# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import os
import subprocess
import tempfile
import unittest

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_test_tamper.py")
_s = importlib.util.spec_from_file_location("check_test_tamper", _p)
T = importlib.util.module_from_spec(_s)
_s.loader.exec_module(T)


def F(path, status="M", add=(), rem=()):
    return {"path": path, "status": status, "added": list(add), "removed": list(rem)}


class JudgeTests(unittest.TestCase):
    def test_edited_assertion_flags(self):
        v, _, _ = T.judge([F("src/VP-V/tests/FooTests.cpp", add=["CHECK(2)"], rem=["CHECK(1)"])], "m")
        self.assertEqual(v[0][0], "test")

    def test_add_only_test_passes_with_note(self):
        v, n, _ = T.judge([F("src/VP-V/tests/FooTests.cpp", add=["TEST(x){}"])], "m")
        self.assertEqual(v, [])
        self.assertTrue(n)

    def test_new_test_file_passes(self):
        v, _, _ = T.judge([F("src/VP-V/tests/NewTests.cpp", "A", add=["a"])], "m")
        self.assertEqual(v, [])

    def test_deleted_test_flags(self):
        v, _, _ = T.judge([F("src/VP-V/tests/FooTests.cpp", "D", rem=["a"])], "m")
        self.assertTrue(v)

    def test_checker_needs_waiver(self):
        f = [F("tools/check_foo.py", add=["x"])]
        self.assertTrue(T.judge(f, "m")[0])
        v, _, w = T.judge(f, "m\n\nWAIVER-ABC_1: reason")
        self.assertEqual(v, [])
        self.assertTrue(w)

    def test_baseline_shrink_ok_growth_flags(self):
        self.assertEqual(T.judge([F("tools/x-baseline.json", rem=["1"])], "m")[0], [])
        self.assertTrue(T.judge([F("tools/x-baseline.json", add=["2"], rem=["1"])], "m")[0])

    def test_allowlist_line_in_other_file(self):
        self.assertTrue(T.judge([F("src/a.cpp", add=["// ALLOW_SITES += 1"])], "m")[0])
        self.assertEqual(T.judge([F("src/a.cpp", add=["int x;"])], "m")[0], [])

    def test_waiver_needs_line_start(self):
        self.assertFalse(T.has_waiver("fix mentions WAIVER-X inline"))
        self.assertTrue(T.has_waiver("a\nWAIVER-X"))


class GitTests(unittest.TestCase):
    def _g(self, root, *a):
        subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t"] + list(a),
                       check=True, capture_output=True)

    def test_range_scan(self):
        with tempfile.TemporaryDirectory() as r:
            self._g(r, "init", "-q")
            os.makedirs(os.path.join(r, "tools"))
            p = os.path.join(r, "tools", "check_a.py")
            open(p, "w").write("x=1\n")
            self._g(r, "add", "-A")
            self._g(r, "commit", "-qm", "base")
            base = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            open(p, "w").write("x=2\n")
            self._g(r, "commit", "-qam", "bad")
            open(p, "w").write("x=3\n")
            self._g(r, "commit", "-qam", "ok\n\nWAIVER-T9: why")
            res = T.scan(r, base + "..HEAD")
            self.assertEqual([bool(x["violations"]) for x in res], [True, False])
            self.assertEqual(T.main(["--root", r, "--range", base + "..HEAD", "--strict"]), 1)
            self.assertEqual(T.main(["--root", r, "--range", "nonsense..HEAD", "--strict"]), 2)

    def test_selftest(self):
        self.assertEqual(T.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
