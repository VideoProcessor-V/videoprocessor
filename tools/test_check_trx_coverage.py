# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_trx_coverage.py")
_s = importlib.util.spec_from_file_location("check_trx_coverage", _p)
T = importlib.util.module_from_spec(_s)
_s.loader.exec_module(T)


class SelftestCases(unittest.TestCase):
    def test_every_case(self):
        for name, flt, fx, want_rc, want_sub in T.selftest_cases():
            with self.subTest(case=name):
                rc, rep = T.run_case(flt, fx)
                self.assertEqual(rc, want_rc, rep["problems"])
                if want_sub:
                    self.assertTrue(any(want_sub in p for p in rep["problems"]), rep["problems"])

    def test_selftest_entry_point(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(T.selftest(), 0)


class FilterParsing(unittest.TestCase):
    def test_queue_line_semicolons_become_or(self):
        flt, lane = T.filter_from_queue_line(
            "1|camfix|D:\\codex\\videoprocessor\\r2-camfix|build+trx|FullyQualifiedName~A;FullyQualifiedName~B|yes|done|seed=x")
        self.assertEqual(flt, "FullyQualifiedName~A|FullyQualifiedName~B")
        self.assertEqual(lane, "camfix")

    def test_tab_separated_queue_line(self):
        flt, _ = T.filter_from_queue_line("1\tl\tD:\\w\tbuild+trx\tName=X\tno\tqueued")
        self.assertEqual(flt, "Name=X")

    def test_queue_line_without_filter_is_a_usage_error(self):
        with self.assertRaises(T.FilterError):
            T.filter_from_queue_line("1|l|D:\\w|build|-|no|queued")

    def test_and_binds_tighter_than_or(self):
        ast, leaves = T.parse_filter("Name=a|Name=b&Name=c")
        self.assertEqual(ast[0], "or")
        self.assertEqual(ast[1][1][0], "and")
        self.assertEqual(len(leaves), 3)

    def test_escaped_bar_stays_in_the_value(self):
        _, leaves = T.parse_filter("Name~a\\|b")
        self.assertEqual(leaves[0]["value"], "a|b")


class MainEntry(unittest.TestCase):
    def test_json_and_rc(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "r.trx")
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(T._fixture([("N.KTests", "M", "Passed", "")]))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = T.main(["--json", "--filter", "FullyQualifiedName~KTests|FullyQualifiedName~Absent", d])
            rep = json.loads(buf.getvalue())
            self.assertEqual(rc, 1)
            self.assertEqual(rep["verdict"], "COVERAGE FAIL")
            self.assertEqual([r["executed"] for r in rep["terms"]], [1, 0])

    def test_trx_file_option_and_failed_tests_printed_apart(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "r.trx")
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(T._fixture([("N.KTests", "M", "Failed", "")]))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = T.main(["--filter", "FullyQualifiedName~KTests", "--trx", p])
            text = buf.getvalue()
            self.assertEqual(rc, 0)
            self.assertIn("READ 1 TRX file(s):", text)
            self.assertIn("FAILED N.KTests::M", text)
            self.assertIn("VERDICT: COVERAGE PASS (1 failed: see the runner rc)", text)

    def test_trx_option_refuses_a_directory(self):
        with tempfile.TemporaryDirectory() as d:
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(T.main(["--filter", "Name=M", "--trx", d]), 2)

    def test_queue_file_lane_lookup_takes_the_last_line(self):
        with tempfile.TemporaryDirectory() as d:
            q = os.path.join(d, "queue.txt")
            with open(q, "w", encoding="utf-8") as fh:
                fh.write("# comment\n1|ln|D:\\w|build+trx|Name=Old|no|done\n1|ln|D:\\w|build+trx|Name=M|no|done\n")
            p = os.path.join(d, "r.trx")
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(T._fixture([("N.KTests", "M", "Passed", "")]))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(T.main(["--queue-file", q, "--lane", "ln", p]), 0)

    def test_no_filter_is_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(T.main(["x.trx"]), 2)


if __name__ == "__main__":
    unittest.main()
