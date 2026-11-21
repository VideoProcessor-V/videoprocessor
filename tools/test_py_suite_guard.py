# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-P2 guard (PY lane): every tools/**/test_*.py must import without SystemExit (or any exception) and define at
least one unittest.TestCase. A manual script or a library named test_* (argparse at import, 0 tests) crashes or empties
`unittest discover`; rename it out of test_* (eval_*, *_lib) instead of excluding it by name in the runner."""
import importlib.util
import os
import tempfile
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
SKIP_DIRS = {"__pycache__", "testclips", "td-cli", "3rdparty"}


def suite_files(root=_dir):
    out = []
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        out.extend(os.path.join(base, f) for f in sorted(files) if f.startswith("test_") and f.endswith(".py"))
    return out


def problem(path, index=0):
    """Why this test_*.py is not a suite (None when it imports cleanly and defines a TestCase)."""
    spec = importlib.util.spec_from_file_location("guarded_suite_%d" % index, path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except SystemExit as e:
        return "SystemExit(%r) at import (a script that parses arguments at import?)" % (e.code,)
    except BaseException as e:   # an import error is as fatal to discovery as a SystemExit
        return "%s at import: %s" % (type(e).__name__, e)
    cases = [v for v in vars(module).values()
             if isinstance(v, type) and issubclass(v, unittest.TestCase) and v is not unittest.TestCase]
    return None if cases else "defines no unittest.TestCase"


class EveryTestFileIsASuite(unittest.TestCase):
    def test_every_test_py_imports_and_defines_a_test_case(self):
        files = suite_files()
        self.assertGreater(len(files), 30)
        bad = []
        for i, path in enumerate(files):
            why = problem(path, i)
            if why:
                bad.append("%s: %s" % (os.path.relpath(path, _dir).replace("\\", "/"), why))
        self.assertEqual(bad, [], "\n".join(bad))

    def test_the_known_non_suites_are_renamed_out_of_test_star(self):
        names = {os.path.basename(p) for p in suite_files()}
        for gone in ("test_categories_lib.py", "test_subtitle_bbox_corpus.py", "test_subtitle_near_bar_recording.py",
                     "test_subtitle_worker_recording.py", "test_subtitle_gap_diagnostics.py", "test_subtitle_large_font.py",
                     "test_subtitle_recorded_output.py", "test_subtitle_recording.py", "test_subtitle_rendered_loops.py"):
            self.assertNotIn(gone, names)
        for kept in ("test_libplacebo_variants.py", "test_check_test_categories.py", "test_check_module_manifests.py",
                     "test_check_pure_move.py"):
            self.assertIn(kept, names)


class GuardLogic(unittest.TestCase):
    def _problem(self, text):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "test_x.py")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return problem(path, 999)

    def test_a_clean_suite_has_no_problem(self):
        self.assertIsNone(self._problem("import unittest\nclass T(unittest.TestCase):\n    def test_a(self): pass\n"))

    def test_a_subclass_of_an_imported_case_counts(self):
        self.assertIsNone(self._problem("from unittest import TestCase\nclass T(TestCase): pass\n"))

    def test_argparse_at_import_is_a_problem(self):
        why = self._problem("import argparse\np = argparse.ArgumentParser()\np.add_argument('--video', required=True)\n"
                            "p.parse_args(['--none'])\n")
        self.assertIn("SystemExit", why)

    def test_a_file_without_a_test_case_is_a_problem(self):
        self.assertEqual(self._problem("def test_loose(): pass\n"), "defines no unittest.TestCase")
        self.assertEqual(self._problem("import unittest\nTestCase = unittest.TestCase\n"), "defines no unittest.TestCase")

    def test_an_import_error_is_a_problem(self):
        self.assertIn("ModuleNotFoundError", self._problem("import no_such_module_anywhere\n"))


if __name__ == "__main__":
    unittest.main()
