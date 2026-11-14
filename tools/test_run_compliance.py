# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import os
import tempfile
import unittest

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_compliance.py")
_s = importlib.util.spec_from_file_location("run_compliance", _p)
RC = importlib.util.module_from_spec(_s)
_s.loader.exec_module(RC)


def _tool(root, name, code):
    os.makedirs(os.path.join(root, "tools"), exist_ok=True)
    with open(os.path.join(root, "tools", name), "w", newline="\n") as fh:
        fh.write("import sys\nprint('x')\nsys.exit(%d)\n" % code)


class RunComplianceTests(unittest.TestCase):
    TOOLS = [("a", "a.py", [], False), ("b", "b.py", [], False), ("c", "c.py", [], False), ("gone", "gone.py", [], False)]

    def test_worst_code_wins_and_absent_is_skip(self):
        with tempfile.TemporaryDirectory() as r:
            _tool(r, "a.py", 0)
            _tool(r, "b.py", 1)
            out = os.path.join(r, "out", "compliance.txt")
            rc, text = RC.run_all(r, self.TOOLS[:2] + self.TOOLS[3:], out=out)
            self.assertEqual(rc, 1)
            self.assertIn("gone              SKIP", text)
            self.assertIn("1 pass, 1 fail, 0 error, 1 skipped", text)
            self.assertTrue(os.path.isfile(out))
            _tool(r, "c.py", 2)
            self.assertEqual(RC.run_all(r, self.TOOLS)[0], 2)

    def test_all_clean_is_zero(self):
        with tempfile.TemporaryDirectory() as r:
            _tool(r, "a.py", 0)
            self.assertEqual(RC.run_all(r, self.TOOLS[:1])[0], 0)

    def test_only_filter(self):
        with tempfile.TemporaryDirectory() as r:
            _tool(r, "a.py", 0)
            _tool(r, "b.py", 1)
            self.assertEqual(RC.run_all(r, self.TOOLS[:2], only={"a"})[0], 0)

    def test_selftest(self):
        self.assertEqual(RC.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
