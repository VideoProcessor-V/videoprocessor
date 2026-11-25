#!/usr/bin/env python3
"""Red Team adversarial tests for tools/motion_ahead/run_50_60p_cost.py (N#6 / Z4).

Covers:
  1. --mode plan prints 50p (20.0 ms) and 60p (16.667 ms) A/B commands and strictly enforces
     DEFAULT_FLIP_ALLOWED is False (no default flips, recommendation only).
  2. --mode parse across synthetic log, CSV, and JSON inputs: dual-rate acceptance gate passes
     ONLY when BOTH 50p and 60p achieve render_ms_saved > 0 AND added_late_presents == 0.
  3. In-test negative controls:
     (a) 50p passes but 60p adds 1 late present -> gate FAILS (strict exit 2).
     (b) 50p and 60p have 0 added late presents but render_ms_saved <= 0 -> gate FAILS.
     (c) Missing 60p (single rate only) -> gate FAILS.
     (d) Missing file, malformed JSON, empty/zero-sample input, and unpaired CLI flags -> explicit error/non-zero.
"""

from __future__ import annotations

import contextlib
import io
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_50_60p_cost as runner  # noqa: E402


def _make_log(render_values, late_presents, slots=0, renders=0):
    lines = [f"frame {i}: render_ms={v:.3f} late_presents={late_presents}" for i, v in enumerate(render_values, start=1)]
    if slots or renders:
        lines.append(f"Motion field ahead ({slots} slots, {renders} renders)")
    return "\n".join(lines) + "\n"


def _make_csv(render_values, late_presents, slots=0, renders=0):
    rows = ["frame,render_ms,late_presents,ahead_slots,ahead_renders"]
    for i, v in enumerate(render_values, start=1):
        rows.append(f"{i},{v:.3f},{late_presents},{slots},{renders}")
    return "\n".join(rows) + "\n"


def _make_json(render_values, late_presents, slots=0, renders=0):
    return json.dumps(
        {
            "render_ms_samples": list(render_values),
            "late_presents": late_presents,
            "motion_field_ahead": {"slots": slots, "renders": renders},
        }
    )


class Run5060pCostTests(unittest.TestCase):
    def test_plan_mode_prints_50p_60p_budgets_and_forbids_default_flip(self):
        self.assertIs(runner.DEFAULT_FLIP_ALLOWED, False)
        self.assertAlmostEqual(runner.BUDGET_50P_MS, 20.0, places=6)
        self.assertAlmostEqual(runner.BUDGET_60P_MS, 1000.0 / 60.0, places=6)

        with tempfile.TemporaryDirectory() as tmp:
            json_out = Path(tmp) / "plan.json"
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = runner.main(
                    [
                        "--mode",
                        "plan",
                        "--clip-50p",
                        "clip50.mkv",
                        "--clip-60p",
                        "clip60.mkv",
                        "--json-out",
                        str(json_out),
                    ]
                )
            self.assertEqual(rc, 0)
            stdout = buf.getvalue()
            self.assertIn("20.000 ms", stdout)
            self.assertIn("16.667 ms", stdout)
            self.assertIn("motion_estimate_ahead_spare=off", stdout)
            self.assertIn("motion_estimate_ahead_spare=on", stdout)
            self.assertIn("default_flip_allowed: False", stdout)

            plan = json.loads(json_out.read_text(encoding="utf-8"))
            self.assertIs(plan["default_flip_allowed"], False)
            self.assertEqual(plan["budgets_ms"]["50p"], 20.0)
            self.assertEqual(plan["budgets_ms"]["60p"], 16.667)
            self.assertEqual(len(plan["runs"]), 4)
            rates_arms = {(r["rate"], r["arm"], r["motion_estimate_ahead_spare"]) for r in plan["runs"]}
            self.assertEqual(
                rates_arms,
                {
                    ("50p", "baseline", "off"),
                    ("50p", "candidate", "on"),
                    ("60p", "baseline", "off"),
                    ("60p", "candidate", "on"),
                },
            )

    def test_parse_passes_dual_rate_gate_across_log_csv_and_json(self):
        formats = (
            ("log", ".log", _make_log),
            ("csv", ".csv", _make_csv),
            ("json", ".json", _make_json),
        )
        for fmt_name, ext, writer in formats:
            with self.subTest(format=fmt_name):
                with tempfile.TemporaryDirectory() as tmp:
                    d = Path(tmp)
                    b50 = d / f"b50{ext}"
                    c50 = d / f"c50{ext}"
                    b60 = d / f"b60{ext}"
                    c60 = d / f"c60{ext}"
                    summary_path = d / "summary.json"

                    b50.write_text(writer([14.0, 14.5, 15.0, 14.2], late_presents=1), encoding="utf-8")
                    c50.write_text(writer([12.0, 12.2, 12.8, 12.1], late_presents=1, slots=3, renders=120), encoding="utf-8")
                    b60.write_text(writer([13.2, 13.5, 13.8, 13.1], late_presents=0), encoding="utf-8")
                    c60.write_text(writer([11.5, 11.7, 12.0, 11.6], late_presents=0, slots=3, renders=180), encoding="utf-8")

                    buf = io.StringIO()
                    with contextlib.redirect_stdout(buf):
                        rc = runner.main(
                            [
                                "--mode",
                                "parse",
                                "--baseline-50p",
                                str(b50),
                                "--candidate-50p",
                                str(c50),
                                "--baseline-60p",
                                str(b60),
                                "--candidate-60p",
                                str(c60),
                                "--json-out",
                                str(summary_path),
                                "--strict",
                            ]
                        )
                    self.assertEqual(rc, 0)
                    summary = json.loads(summary_path.read_text(encoding="utf-8"))
                    self.assertIs(summary["overall_passed"], True)
                    self.assertIs(summary["default_flip_allowed"], False)
                    self.assertEqual(summary["recommendation"], "RECORD_S1_ACCEPTANCE_ROW")
                    self.assertEqual(summary["missing_rates"], [])
                    for rate in ("50p", "60p"):
                        r = summary["rates"][rate]
                        self.assertIs(r["passed"], True)
                        self.assertGreater(r["render_ms_saved"], 0.0)
                        self.assertEqual(r["added_late_presents"], 0)
                        self.assertEqual(r["candidate"]["motion_field_ahead"]["slots"], 3)
                    self.assertIn("| 50p |", buf.getvalue())
                    self.assertIn("| 60p |", buf.getvalue())

    def test_negative_control_60p_added_late_present_fails_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            b50, c50 = d / "b50.log", d / "c50.log"
            b60, c60 = d / "b60.log", d / "c60.log"
            out_json = d / "out.json"

            # 50p passes cleanly (saves ~2 ms, 0 added late presents).
            b50.write_text(_make_log([14.0, 14.2, 14.4], late_presents=0), encoding="utf-8")
            c50.write_text(_make_log([12.0, 12.1, 12.2], late_presents=0, slots=2, renders=60), encoding="utf-8")
            # 60p saves render ms BUT adds 1 late present -> MUST fail overall gate!
            b60.write_text(_make_log([13.5, 13.6, 13.7], late_presents=0), encoding="utf-8")
            c60.write_text(_make_log([11.5, 11.6, 11.7], late_presents=1, slots=2, renders=60), encoding="utf-8")

            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = runner.main(
                    [
                        "--mode",
                        "parse",
                        "--baseline-50p",
                        str(b50),
                        "--candidate-50p",
                        str(c50),
                        "--baseline-60p",
                        str(b60),
                        "--candidate-60p",
                        str(c60),
                        "--json-out",
                        str(out_json),
                        "--strict",
                    ]
                )
            self.assertEqual(rc, 2)
            summary = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertIs(summary["rates"]["50p"]["passed"], True)
            self.assertIs(summary["rates"]["60p"]["passed"], False)
            self.assertEqual(summary["rates"]["60p"]["added_late_presents"], 1)
            self.assertIs(summary["overall_passed"], False)
            self.assertEqual(summary["recommendation"], "KEEP_AUTO_OFF")

    def test_negative_control_non_positive_render_ms_saved_fails_gate(self):
        # Test both exact 0.0 ms saved and negative ms saved (regression) with 0 late presents.
        for label, cand_60 in (("zero_saved", [13.0, 13.0, 13.0]), ("regression", [13.5, 13.6, 13.7])):
            with self.subTest(case=label):
                with tempfile.TemporaryDirectory() as tmp:
                    d = Path(tmp)
                    b50, c50 = d / "b50.csv", d / "c50.csv"
                    b60, c60 = d / "b60.csv", d / "c60.csv"
                    out_json = d / "out.json"

                    b50.write_text(_make_csv([14.0, 14.0, 14.0], late_presents=0), encoding="utf-8")
                    c50.write_text(_make_csv([12.5, 12.5, 12.5], late_presents=0), encoding="utf-8")
                    b60.write_text(_make_csv([13.0, 13.0, 13.0], late_presents=0), encoding="utf-8")
                    c60.write_text(_make_csv(cand_60, late_presents=0), encoding="utf-8")

                    buf = io.StringIO()
                    with contextlib.redirect_stdout(buf):
                        rc = runner.main(
                            [
                                "--mode",
                                "parse",
                                "--baseline-50p",
                                str(b50),
                                "--candidate-50p",
                                str(c50),
                                "--baseline-60p",
                                str(b60),
                                "--candidate-60p",
                                str(c60),
                                "--json-out",
                                str(out_json),
                                "--strict",
                            ]
                        )
                    self.assertEqual(rc, 2)
                    summary = json.loads(out_json.read_text(encoding="utf-8"))
                    self.assertEqual(summary["rates"]["60p"]["added_late_presents"], 0)
                    self.assertLessEqual(summary["rates"]["60p"]["render_ms_saved"], 0.0)
                    self.assertIs(summary["rates"]["60p"]["passed"], False)
                    self.assertIs(summary["overall_passed"], False)
                    self.assertEqual(summary["recommendation"], "KEEP_AUTO_OFF")

    def test_negative_control_missing_rate_and_malformed_or_empty_inputs_fail_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)

            # 1. Only 50p provided (missing 60p): dual-rate gate MUST NOT silently pass.
            b50, c50 = d / "b50.json", d / "c50.json"
            out_json = d / "single_rate.json"
            b50.write_text(_make_json([14.0, 14.2], late_presents=0), encoding="utf-8")
            c50.write_text(_make_json([12.0, 12.1], late_presents=0), encoding="utf-8")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = runner.main(
                    [
                        "--mode",
                        "parse",
                        "--baseline-50p",
                        str(b50),
                        "--candidate-50p",
                        str(c50),
                        "--json-out",
                        str(out_json),
                        "--strict",
                    ]
                )
            self.assertEqual(rc, 2)
            summary = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertIs(summary["overall_passed"], False)
            self.assertEqual(summary["missing_rates"], ["60p"])

            # 2. Empty/unparseable log (0 render_ms samples): MUST fail visibly, never pass.
            empty_log = d / "empty.log"
            empty_log.write_text("unrelated log line with no timings\n", encoding="utf-8")
            out_empty = d / "empty_summary.json"
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = runner.main(
                    [
                        "--mode",
                        "parse",
                        "--baseline-50p",
                        str(empty_log),
                        "--candidate-50p",
                        str(empty_log),
                        "--baseline-60p",
                        str(empty_log),
                        "--candidate-60p",
                        str(empty_log),
                        "--json-out",
                        str(out_empty),
                        "--strict",
                    ]
                )
            self.assertEqual(rc, 2)
            empty_summary = json.loads(out_empty.read_text(encoding="utf-8"))
            self.assertIs(empty_summary["overall_passed"], False)
            self.assertIn("baseline has no render_ms samples", empty_summary["rates"]["50p"]["reasons"])

            # 3. Missing file raises FileNotFoundError explicitly.
            with self.assertRaises(FileNotFoundError):
                runner.parse_run_file(d / "does_not_exist.log")

            # 4. Malformed JSON raises JSONDecodeError; non-object JSON raises ValueError.
            bad_json = d / "bad.json"
            bad_json.write_text("{not valid json", encoding="utf-8")
            with self.assertRaises(json.JSONDecodeError):
                runner.parse_run_file(bad_json)

            list_json = d / "list.json"
            list_json.write_text("[1, 2, 3]", encoding="utf-8")
            with self.assertRaises(ValueError):
                runner.parse_run_file(list_json)

            # 5. CLI parser rejects missing inputs and unpaired baseline/candidate flags with non-zero exit.
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                with self.assertRaises(SystemExit) as ctx_empty:
                    runner.main(["--mode", "parse"])
            self.assertNotEqual(ctx_empty.exception.code, 0)

            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                with self.assertRaises(SystemExit) as ctx_unpaired:
                    runner.main(["--mode", "parse", "--baseline-50p", str(b50)])
            self.assertNotEqual(ctx_unpaired.exception.code, 0)

    def test_negative_control_non_finite_json_render_ms_mean_fails_gate_explicitly(self):
        # Positive control on the exact same summary-style JSON schema: finite render_ms_mean passes.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            b50, c50 = d / "b50.json", d / "c50.json"
            b60, c60 = d / "b60.json", d / "c60.json"
            ok_out = d / "ok.json"
            b50.write_text('{"render_ms_mean": 14.0, "late_presents": 0}', encoding="utf-8")
            c50.write_text('{"render_ms_mean": 12.0, "late_presents": 0}', encoding="utf-8")
            b60.write_text('{"render_ms_mean": 13.5, "late_presents": 0}', encoding="utf-8")
            c60.write_text('{"render_ms_mean": 11.5, "late_presents": 0}', encoding="utf-8")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc_ok = runner.main(
                    [
                        "--mode",
                        "parse",
                        "--baseline-50p",
                        str(b50),
                        "--candidate-50p",
                        str(c50),
                        "--baseline-60p",
                        str(b60),
                        "--candidate-60p",
                        str(c60),
                        "--json-out",
                        str(ok_out),
                        "--strict",
                    ]
                )
            self.assertEqual(rc_ok, 0)
            self.assertIs(json.loads(ok_out.read_text(encoding="utf-8"))["overall_passed"], True)

        # Negative controls: Infinity, -Infinity, and NaN render_ms_mean must fail explicitly and NEVER falsely pass.
        cases = (
            ("baseline_pos_inf", '{"render_ms_mean": Infinity, "late_presents": 0}', '{"render_ms_mean": 12.0, "late_presents": 0}'),
            ("candidate_neg_inf", '{"render_ms_mean": 14.0, "late_presents": 0}', '{"render_ms_mean": -Infinity, "sample_count": 1, "late_presents": 0}'),
            ("baseline_nan", '{"render_ms_mean": NaN, "sample_count": 1, "late_presents": 0}', '{"render_ms_mean": 12.0, "late_presents": 0}'),
        )
        for label, b60_text, c60_text in cases:
            with self.subTest(case=label):
                with tempfile.TemporaryDirectory() as tmp:
                    d = Path(tmp)
                    b50, c50 = d / "b50.json", d / "c50.json"
                    b60, c60 = d / "b60.json", d / "c60.json"
                    out_json = d / "out.json"

                    b50.write_text('{"render_ms_mean": 14.0, "late_presents": 0}', encoding="utf-8")
                    c50.write_text('{"render_ms_mean": 12.0, "late_presents": 0}', encoding="utf-8")
                    b60.write_text(b60_text, encoding="utf-8")
                    c60.write_text(c60_text, encoding="utf-8")

                    buf = io.StringIO()
                    try:
                        with contextlib.redirect_stdout(buf):
                            rc = runner.main(
                                [
                                    "--mode",
                                    "parse",
                                    "--baseline-50p",
                                    str(b50),
                                    "--candidate-50p",
                                    str(c50),
                                    "--baseline-60p",
                                    str(b60),
                                    "--candidate-60p",
                                    str(c60),
                                    "--json-out",
                                    str(out_json),
                                    "--strict",
                                ]
                            )
                    except ValueError:
                        # Explicit parse-time rejection of non-finite JSON constants is valid.
                        continue

                    self.assertEqual(rc, 2, f"{label}: non-finite JSON render_ms_mean must fail strict gate with exit 2")
                    summary = json.loads(out_json.read_text(encoding="utf-8"))
                    self.assertIs(summary["overall_passed"], False)
                    self.assertIs(summary["rates"]["60p"]["passed"], False)
                    self.assertEqual(summary["recommendation"], "KEEP_AUTO_OFF")
                    self.assertTrue(summary["rates"]["60p"]["reasons"])
                    self.assertTrue(math.isfinite(summary["rates"]["60p"]["render_ms_saved"]))
                    self.assertTrue(math.isfinite(summary["rates"]["60p"]["baseline"]["render_ms_mean"]))
                    self.assertTrue(math.isfinite(summary["rates"]["60p"]["candidate"]["render_ms_mean"]))

    def test_csv_motion_field_ahead_note_extracts_slots_and_renders(self):
        csv_status = (
            "frame,render_ms,late_presents,status\n"
            '1,12.000,0,"motion field ahead (slots=4, renders=1)"\n'
            '2,12.200,0,"steady"\n'
        )
        m_status = runner.parse_csv_text(csv_status)
        self.assertEqual(m_status.sample_count, 2)
        self.assertEqual(m_status.ahead_slots, 4)
        self.assertEqual(m_status.ahead_renders, 1)
        self.assertEqual(len(m_status.ahead_lines), 1)

        csv_col = (
            "frame,render_ms,late_presents,motion_field_ahead\n"
            '1,11.800,0,"motion field ahead (slots=4, renders=1)"\n'
        )
        m_col = runner.parse_csv_text(csv_col)
        self.assertEqual(m_col.ahead_slots, 4)
        self.assertEqual(m_col.ahead_renders, 1)
        self.assertEqual(len(m_col.ahead_lines), 1)

        # In-test negative control: CSV status without a motion field ahead note reports 0 slots / 0 renders.
        csv_none = (
            "frame,render_ms,late_presents,status\n"
            '1,12.000,0,"steady"\n'
        )
        m_none = runner.parse_csv_text(csv_none)
        self.assertEqual(m_none.ahead_slots, 0)
        self.assertEqual(m_none.ahead_renders, 0)
        self.assertEqual(m_none.ahead_lines, [])


if __name__ == "__main__":
    unittest.main()
