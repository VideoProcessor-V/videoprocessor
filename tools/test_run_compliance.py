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

    def test_all_registered_tools_exist(self):
        tools_dir = os.path.dirname(os.path.abspath(__file__))
        self.assertTrue(RC.TOOLS)
        for name, script, _, _ in RC.TOOLS:
            path = os.path.join(tools_dir, script)
            self.assertTrue(os.path.isfile(path), f"registered compliance tool '{name}' missing: {path}")

    def test_py_lane_is_opt_in_and_registered(self):
        # TQ-RG-P1: the PY lane (every Python suite) is not in the default checker list; --py-lane adds it.
        self.assertNotIn("py_suites", [t[0] for t in RC.TOOLS])
        self.assertEqual(RC.PY_LANE[:2], ("py_suites", "run_py_suites.py"))
        self.assertTrue(os.path.isfile(os.path.join(os.path.dirname(_p), RC.PY_LANE[1])))
        import contextlib
        import io
        for flag, expected in (([], False), (["--py-lane"], True)):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(RC.main(["--list"] + flag), 0)
            self.assertEqual("py_suites" in out.getvalue(), expected, flag)

    def test_py_lane_has_a_longer_limit_than_a_checker(self):
        import subprocess
        seen = []
        real = subprocess.run
        subprocess.run = lambda *a, **k: (seen.append(k.get("timeout")), real(*a, **k))[1]
        try:
            with tempfile.TemporaryDirectory() as r:
                _tool(r, "run_py_suites.py", 0)
                _tool(r, "a.py", 0)
                RC.run_all(r, [RC.PY_LANE, ("a", "a.py", [], False)])
        finally:
            subprocess.run = real
        self.assertEqual(seen, [3600, 600])


if __name__ == "__main__":
    unittest.main()
