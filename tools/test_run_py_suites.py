# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-P1: run_py_suites.py, the PY lane. Each case builds a tiny tools/ tree in a temp dir and runs the real lane on it."""
import importlib.util
import os
import shutil
import tempfile
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("run_py_suites", os.path.join(_dir, "run_py_suites.py"))
R = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(R)

PASSING = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): pass\n    def test_b(self): pass\n"
FAILING = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): self.assertEqual(1, 2)\n"
SKIPPING = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): pass\n    @unittest.skip('why')\n    def test_b(self): pass\n"
EMPTY = "import unittest\nclass T(unittest.TestCase):\n    pass\n"
SLOW = "import time, unittest\nclass T(unittest.TestCase):\n    def test_a(self): time.sleep(30)\n"


class Lane(unittest.TestCase):
    def tree(self, files):
        root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, root, True)
        for rel, text in files.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        return root

    def test_directories_with_test_files_are_found_and_others_are_not(self):
        root = self.tree({"tools/test_a.py": PASSING, "tools/eval/test_b.py": PASSING, "tools/eval/helper.py": "",
                          "tools/lib/util.py": "", "tools/__pycache__/test_c.py": PASSING})
        self.assertEqual(R.suite_dirs(root), ["tools", "tools/eval"])

    def test_counts_are_summed_per_directory(self):
        root = self.tree({"tools/test_a.py": PASSING, "tools/eval/test_b.py": PASSING})
        results = R.run_all(root)
        self.assertEqual([(r["dir"], r["status"], r["tests"]) for r in results], [("tools", "PASS", 2), ("tools/eval", "PASS", 2)])
        text, rc = R.report(results)
        self.assertEqual(rc, 0)
        self.assertIn("PY lane: 4 tests in 2 directories, 0 failed directories", text)

    def test_a_failing_test_fails_the_lane(self):
        root = self.tree({"tools/test_a.py": PASSING, "tools/eval/test_b.py": FAILING})
        results = R.run_all(root)
        self.assertEqual([r["status"] for r in results], ["PASS", "FAIL"])
        text, rc = R.report(results)
        self.assertEqual(rc, 1)
        self.assertIn("1 failed directories", text)
        self.assertIn("AssertionError", text)

    def test_a_directory_that_runs_zero_tests_fails(self):
        results = R.run_all(self.tree({"tools/test_a.py": EMPTY}))
        self.assertEqual((results[0]["status"], results[0]["note"]), ("FAIL", "0 tests ran"))

    def test_an_import_crash_fails(self):
        results = R.run_all(self.tree({"tools/test_a.py": "raise SystemExit(2)\n"}))
        self.assertEqual(results[0]["status"], "FAIL")

    def test_skips_are_counted_and_fail_only_with_no_skips(self):
        root = self.tree({"tools/test_a.py": SKIPPING})
        lenient = R.run_all(root)[0]
        self.assertEqual((lenient["status"], lenient["tests"], lenient["skipped"]), ("PASS", 2, 1))
        strict = R.run_all(root, no_skips=True)[0]
        self.assertEqual((strict["status"], strict["note"]), ("FAIL", "1 skipped (--no-skips)"))

    def test_a_timeout_fails(self):
        result = R.run_all(self.tree({"tools/test_a.py": SLOW}), timeout=3)[0]
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("timeout", result["note"])

    def test_nothing_to_run_is_a_failure(self):
        text, rc = R.report([])
        self.assertEqual(rc, 1)
        self.assertIn("NOTHING RAN", text)

    def test_only_selects_a_directory_by_path_or_name(self):
        root = self.tree({"tools/test_a.py": PASSING, "tools/eval/test_b.py": PASSING})
        self.assertEqual([r["dir"] for r in R.run_all(root, only={"tools/eval"})], ["tools/eval"])
        self.assertEqual([r["dir"] for r in R.run_all(root, only={"eval"})], ["tools/eval"])

    def test_parse_run(self):
        self.assertEqual(R.parse_run("Ran 3 tests in 0.5s\n\nOK (skipped=1)"), (3, 1, 0.5))
        self.assertEqual(R.parse_run("Ran 1 test in 0.1s\n\nOK"), (1, 0, 0.1))
        self.assertEqual(R.parse_run("FAILED (failures=1, skipped=2)\nRan 4 tests in 1.0s")[:2], (4, 2))
        self.assertIsNone(R.parse_run("no summary at all")[0])

    def test_the_real_tools_tree_lists_the_known_suite_directories(self):
        dirs = R.suite_dirs()
        for d in ("tools", "tools/eval", "tools/looprig", "tools/levels"):
            self.assertIn(d, dirs)

    def test_the_selftest_and_list_exit_zero(self):
        self.assertEqual(R.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
