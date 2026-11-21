# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-P3..P8: the --selftest of a checker must FAIL when the checker is gutted. Each case loads the checker, applies one
mutation (a deleted pattern, a function that no longer discriminates) and asserts that its selftest now reports failures;
the unmutated checker reports none. Before these selftests, gutting the same code still printed "selftest ok"."""
import contextlib
import importlib.util
import io
import os
import re
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
NEVER = re.compile(r"(?!x)x")   # matches nothing


def load(*parts):
    path = os.path.join(_dir, *parts)
    spec = importlib.util.spec_from_file_location("mut_" + os.path.basename(path)[:-3], path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fails(module):
    """True when the module's selftest reports a failed check, or crashes (a gutted function that the selftest then trips
    over is a failed selftest too)."""
    try:
        return bool(module.selftest_failures())
    except Exception:
        return True


def pairwise_failures(module):
    """pairwise.selftest asserts; (True when it raised AssertionError)."""
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            module.selftest()
        except AssertionError:
            return True
    return False


class CleanCheckersPass(unittest.TestCase):
    def test_every_selftest_is_clean_on_the_unmutated_checker(self):
        self.assertEqual(load("run_compliance.py").selftest_failures(), [])
        self.assertEqual(load("check_test_tamper.py").selftest_failures(), [])
        self.assertEqual(load("check_launcher_workarounds.py").selftest_failures(), [])
        self.assertEqual(load("check_fallback_coverage.py").selftest_failures(), [])
        self.assertEqual(load("check_precision.py").selftest_failures(), [])
        self.assertFalse(pairwise_failures(load("vpv_tests", "pairwise.py")))

    def test_each_selftest_command_exits_zero(self):
        for name in ("run_compliance", "check_test_tamper", "check_launcher_workarounds", "check_fallback_coverage", "check_precision"):
            with self.subTest(checker=name), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(load(name + ".py").main(["--selftest"]), 0)


class RunComplianceMutations(unittest.TestCase):
    def test_a_run_all_that_always_exits_zero_fails_the_selftest(self):
        m = load("run_compliance.py")
        m.run_all = lambda *a, **k: (0, "compliance: 1 pass, 0 fail, 0 error, 0 skipped (absent); exit 0\n")
        self.assertTrue(fails(m))

    def test_a_run_one_that_always_passes_fails_the_selftest(self):
        m = load("run_compliance.py")
        m.run_one = lambda root, td, name, *a, **k: {"name": name, "status": "PASS", "rc": 0, "note": "", "tail": []}
        self.assertTrue(fails(m))

    def test_a_run_one_that_never_skips_an_absent_script_fails_the_selftest(self):
        m = load("run_compliance.py")
        real = m.run_one

        def no_skip(root, td, name, script, *a, **k):
            r = real(root, td, name, script, *a, **k)
            if r["status"] == "SKIP":
                r = dict(r, status="PASS", rc=0)
            return r
        m.run_one = no_skip
        self.assertTrue(fails(m))

    def test_a_duplicated_tool_name_fails_the_selftest(self):
        m = load("run_compliance.py")
        m.TOOLS = m.TOOLS + [m.TOOLS[0]]
        self.assertIn("TOOLS names are unique", m.selftest_failures())


class TestTamperMutations(unittest.TestCase):
    def test_each_gutted_function_fails_the_selftest(self):
        for attr, replacement in (
                ("classify", lambda p: "other"),
                ("judge", lambda files, message: ([], [], False)),
                ("has_waiver", lambda message: True),
                ("late_waivers", lambda msgs: set()),
                ("parse_ledger", lambda text: []),
                ("gate_check", lambda root, record, sha: (True, "")),
                ("tip_of", lambda rng: "HEAD")):
            with self.subTest(gutted=attr):
                m = load("check_test_tamper.py")
                setattr(m, attr, replacement)
                self.assertTrue(fails(m))

    def test_each_deleted_class_glob_fails_the_selftest(self):
        for attr in ("CHECKER_GLOBS", "BASELINE_GLOBS"):
            with self.subTest(emptied=attr):
                m = load("check_test_tamper.py")
                setattr(m, attr, [])
                self.assertTrue(fails(m))

    def test_a_dropped_allow_rx_or_row_rx_fails_the_selftest(self):
        for attr in ("ALLOW_RX", "PRODUCT_ROW_RX", "WAIVER_RX", "WAIVES_RX", "LEDGER_SHA_RX", "VERDICT_RX"):
            with self.subTest(dropped=attr):
                m = load("check_test_tamper.py")
                setattr(m, attr, NEVER)
                self.assertTrue(fails(m))

    def test_a_negative_rx_that_matches_nothing_fails_the_selftest(self):
        m = load("check_test_tamper.py")
        m.NEGATIVE_RX = NEVER
        self.assertTrue(fails(m))


class LauncherMutations(unittest.TestCase):
    def test_deleting_any_one_pattern_fails_the_selftest(self):
        ids = [pid for pid, _ in load("check_launcher_workarounds.py").PATTERNS]
        self.assertEqual(len(ids), 6)
        for pid in ids:
            with self.subTest(deleted=pid):
                m = load("check_launcher_workarounds.py")
                m.PATTERNS = [(p, rx) for p, rx in m.PATTERNS if p != pid]
                self.assertTrue(fails(m))

    def test_deleting_the_cmd_wrapper_rule_fails_the_selftest(self):
        m = load("check_launcher_workarounds.py")
        m.WRAP_RX = NEVER
        self.assertTrue(fails(m))

    def test_an_evaluate_that_allows_everything_fails_the_selftest(self):
        m = load("check_launcher_workarounds.py")
        m.evaluate = lambda hits, allow, added=None: ([], [])
        self.assertTrue(fails(m))

    def test_an_evaluate_that_ignores_the_reason_and_row_fails_the_selftest(self):
        m = load("check_launcher_workarounds.py")
        real = m.evaluate
        m.evaluate = lambda hits, allow, added=None: real(hits, [dict(e, reason="r", row="x") for e in allow], added)
        self.assertTrue(fails(m))


class FallbackMutations(unittest.TestCase):
    def test_deleting_a_site_class_fails_the_selftest(self):
        for attr in ("LOG_CALL", "CAP", "PLERR"):
            with self.subTest(deleted=attr):
                m = load("check_fallback_coverage.py")
                setattr(m, attr, NEVER)
                self.assertTrue(fails(m))

    def test_a_cover_or_tag_that_matches_nothing_or_everything_fails_the_selftest(self):
        for attr, rx in (("COVER", NEVER), ("COVER", re.compile("")), ("TAG", NEVER), ("TAG", re.compile("")), ("FB_WORDS", NEVER)):
            with self.subTest(mutated=attr, rx=rx.pattern):
                m = load("check_fallback_coverage.py")
                setattr(m, attr, rx)
                self.assertTrue(fails(m))

    def test_a_catalog_hits_that_finds_nothing_fails_the_selftest(self):
        m = load("check_fallback_coverage.py")
        m.catalog_hits = lambda root: []
        self.assertTrue(fails(m))

    def test_a_catalog_hits_that_always_complains_fails_the_selftest(self):
        m = load("check_fallback_coverage.py")
        m.catalog_hits = lambda root: [{"file": "catalog:x:formatter"}]
        self.assertTrue(fails(m))


class PrecisionMutations(unittest.TestCase):
    def test_deleting_a_rule_fails_the_selftest(self):
        for attr in ("FMT", "SHIFT", "SHIFT_CONST_DEF", "SHIFT_CTX", "U8", "U8_CTX", "TAG"):
            with self.subTest(deleted=attr):
                m = load("check_precision.py")
                setattr(m, attr, NEVER)
                self.assertTrue(fails(m))

    def test_a_named_shift_that_never_matches_fails_the_selftest(self):
        m = load("check_precision.py")
        m._named_shift = lambda names: None
        self.assertTrue(fails(m))

    def test_shift_constants_that_finds_nothing_fails_the_selftest(self):
        m = load("check_precision.py")
        m.shift_constants = lambda text: set()
        self.assertTrue(fails(m))

    def test_a_context_that_matches_everything_fails_the_selftest(self):
        for attr in ("SHIFT_CTX", "U8_CTX"):
            with self.subTest(widened=attr):
                m = load("check_precision.py")
                setattr(m, attr, re.compile(""))
                self.assertTrue(fails(m))


class PairwiseMutations(unittest.TestCase):
    def test_a_verify_that_reports_nothing_fails_the_selftest(self):
        m = load("vpv_tests", "pairwise.py")
        m.verify = lambda dims, lines: []
        self.assertTrue(pairwise_failures(m))

    def test_a_verify_that_always_reports_a_miss_fails_the_selftest(self):
        m = load("vpv_tests", "pairwise.py")
        m.verify = lambda dims, lines: [("a", "0", "b", "x")]
        self.assertTrue(pairwise_failures(m))

    def test_a_verify_that_ignores_the_rows_fails_the_selftest(self):
        m = load("vpv_tests", "pairwise.py")
        real = m.verify
        m.verify = lambda dims, lines: real(dims, [])
        self.assertTrue(pairwise_failures(m))


if __name__ == "__main__":
    unittest.main()
