# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Comprehensive offline tests for the Row-27 loop-rig runner, grid, and stamp verifier.

Run from any directory:
    python -I -B tools/looprig/test_row27_runner.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
for _p in (HERE, os.path.join(TOOLS, "framestamp")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import framestamp as fs  # noqa: E402
import grid_runner as gr  # noqa: E402
import row27_grid as rg  # noqa: E402
import stamp_verify as sv  # noqa: E402

PS1 = os.path.join(HERE, "run_row27_loop.ps1")
RUNNER = os.path.join(HERE, "grid_runner.py")
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


def _trx_results(path):
    ns = {"t": "http://microsoft.com/schemas/VisualStudio/TeamTest/2010"}
    root = ET.parse(path).getroot()
    out = {}
    for r in root.findall("t:Results/t:UnitTestResult", ns):
        stdout_el = r.find("t:Output/t:StdOut", ns)
        err_el = r.find("t:Output/t:ErrorInfo/t:Message", ns)
        text = stdout_el.text if stdout_el is not None else (err_el.text if err_el is not None else "")
        out[r.get("testName")] = (r.get("outcome"), text or "")
    return out


class GridMatrixTests(unittest.TestCase):
    def test_parse_rate_exact_rationals_and_arbitrary_rates(self):
        self.assertEqual(rg.parse_rate("23.976"), Fraction(24000, 1001))
        self.assertEqual(rg.parse_rate("29.97"), Fraction(30000, 1001))
        self.assertEqual(rg.parse_rate("47.952"), Fraction(48000, 1001))
        self.assertEqual(rg.parse_rate("59.94"), Fraction(60000, 1001))
        self.assertEqual(rg.parse_rate("119.88"), Fraction(120000, 1001))
        self.assertEqual(rg.parse_rate("48000/1001"), Fraction(48000, 1001))
        self.assertEqual(rg.parse_rate([75, 1]), Fraction(75, 1))
        self.assertEqual(rg.parse_rate(144), Fraction(144, 1))
        self.assertEqual(rg.parse_rate("119.88") / rg.parse_rate("47.952"), Fraction(5, 2))
        for bad in (0, -24, "0/1", "24/0", "abc", "", float("nan"), float("inf"), [24]):
            with self.assertRaises(rg.GridError):
                rg.parse_rate(bad)

    def test_classify_cadence_general_pairs(self):
        self.assertEqual(rg.classify_cadence("24", "48"), "exact_integer")
        self.assertEqual(rg.classify_cadence("25", "50"), "exact_integer")
        self.assertEqual(rg.classify_cadence("59.94", "59.94"), "exact_integer")
        self.assertEqual(rg.classify_cadence("60", "120"), "exact_integer")
        self.assertEqual(rg.classify_cadence("75", "150"), "exact_integer")
        self.assertEqual(rg.classify_cadence("23.976", "59.94"), "pulldown_3_2")
        self.assertEqual(rg.classify_cadence("24", "60"), "pulldown_3_2")
        self.assertEqual(rg.classify_cadence("47.952", "119.88"), "pulldown_3_2")
        self.assertEqual(rg.classify_cadence("23.976", "60"), "fractional_drift")
        self.assertEqual(rg.classify_cadence("24", "59.94"), "fractional_drift")
        self.assertEqual(rg.classify_cadence("59.94", "60"), "fractional_drift")
        self.assertEqual(rg.classify_cadence("29.97", "60"), "fractional_drift")
        self.assertEqual(rg.classify_cadence("25", "60"), "non_integer_judder")
        self.assertEqual(rg.classify_cadence("50", "120"), "non_integer_judder")
        self.assertEqual(rg.classify_cadence("29.97", "50"), "non_integer_judder")

    def test_profiles_row27_smoke_full_cover_all_axes(self):
        p_r27 = rg.resolve_grid("row27")
        cells = p_r27["cells"]
        self.assertGreaterEqual(len(cells), 25)
        self.assertEqual({c["pathway"] for c in cells}, {"original", "vpv"})
        self.assertEqual({c["level"] for c in cells if c["pathway"] == "vpv"}, {0, 1, 2, 3, 4})
        self.assertEqual({c["render_ahead"] for c in cells}, {False, True})
        self.assertTrue({0, 1, 2, 3, 4, "auto"}.issubset({c["requested_queue_depth"] for c in cells}))
        self.assertEqual({c["dejudder"] for c in cells}, {False, True})
        self.assertEqual({c["color_mode"] for c in cells}, {"sdr", "hdr10", "dv_tunnel"})
        self.assertEqual({c["track"] for c in cells}, {"clean", "dirty", "rate_transition"})
        self.assertEqual(
            set(p_r27["cadence_classes"].keys()),
            {"exact_integer", "pulldown_3_2", "fractional_drift", "non_integer_judder"},
        )

        # Smoke profile covers all 4 cadence classes and all tracks
        p_smoke = rg.resolve_grid("smoke")
        self.assertGreaterEqual(p_smoke["total_cells"], 8)
        self.assertEqual(set(p_smoke["cadence_classes"].keys()),
                         {"exact_integer", "pulldown_3_2", "fractional_drift", "non_integer_judder"})

        # Full pairwise is deterministic and covers every valid (src, disp) with disp >= src
        p_full_1 = rg.resolve_grid("full")
        p_full_2 = rg.resolve_grid("full")
        self.assertEqual([c["cell_id"] for c in p_full_1["cells"]], [c["cell_id"] for c in p_full_2["cells"]])
        doc = rg.load_grid_doc()
        src_labels = {rg.format_rate_label(x["rate"]) for x in doc["axes"]["source_rates"]}
        disp_labels = {rg.format_rate_label(x["rate"]) for x in doc["axes"]["display_modes"]}
        seen_pairs = {(c["source_hz_label"], c["display_hz_label"]) for c in p_full_1["cells"]}
        for s in src_labels:
            for d in disp_labels:
                if float(rg.parse_rate(d) / rg.parse_rate(s)) >= 1.0 - 1e-6:
                    self.assertIn((s, d), seen_pairs, "missing pairwise pair %s->%s" % (s, d))

    def test_arbitrary_custom_rates_47_952_to_119_88(self):
        plan = rg.resolve_grid("custom", source_rates=["47.952"], display_rates=["119.88"])
        self.assertGreater(plan["total_cells"], 0)
        for c in plan["cells"]:
            self.assertEqual(c["source_rate"], [48000, 1001])
            self.assertEqual(c["display_rate"], [120000, 1001])
            self.assertEqual(c["cadence_ratio"], [5, 2])
            self.assertEqual(c["cadence_class"], "pulldown_3_2")
            recs, base = gr.simulate_cell(c, n_source_frames=48)
            ver = sv.verify_cell(c, recs, bill_baseline=base)
            self.assertEqual(ver["status"], sv.PASS, ver)

    def test_filter_expressions_and_original_pathway_constraints(self):
        p = rg.resolve_grid("row27", filter_expr="pathway=original")
        self.assertGreater(p["total_cells"], 0)
        for c in p["cells"]:
            self.assertEqual(c["pathway"], "original")
            self.assertFalse(c["render_ahead"])
            self.assertEqual(c["resolved_queue_depth"], 0)
            self.assertFalse(c["dejudder"])
            self.assertEqual(c["level"], 0)

        p2 = rg.resolve_grid("row27", filter_expr="pathway=vpv,track!=dirty,dv_tunnel")
        self.assertGreater(p2["total_cells"], 0)
        for c in p2["cells"]:
            self.assertEqual(c["pathway"], "vpv")
            self.assertNotEqual(c["track"], "dirty")
            self.assertEqual(c["color_mode"], "dv_tunnel")

    def test_queue_depth_zero_rejected_when_render_ahead_on(self):
        defaults = rg.load_grid_doc()["defaults"]
        with self.assertRaises(rg.GridError):
            rg.build_cell({
                "src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 2,
                "render_ahead": True, "queue_depth": 0, "dejudder": True,
                "color_mode": "sdr", "track": "clean",
            }, defaults)


class StampCodecTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="row27-codec-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_record_crc_and_visual_stamp_roundtrip_and_tamper_detection(self):
        rec = sv.make_record(
            frame_seq=42, source_pts_ns=1_751_750_000, capture_ts_ns=2_751_750_000,
            render_submit_ts_ns=2_780_000_000, present_ts_ns=2_801_800_000,
            display_vblank_idx=105, queue_depth=3, pathway="vpv", mode_id="test_cell",
            visual_aux=2, planned_vblank_idx=105, is_extra=False, gate_level="L2",
        )
        ok, errs = sv.verify_record(rec)
        self.assertTrue(ok, errs)
        expected_vs = "%016X" % fs.word(fs.KIND_SOURCE, 2, 42)
        self.assertEqual(rec["visual_stamp_code"], expected_vs)

        # Tampering any core field must fail CRC verification
        for field, bad_val in (
            ("frame_seq", 43),
            ("present_ts_ns", rec["present_ts_ns"] + 1),
            ("queue_depth", 0),
            ("pathway", "original"),
            ("record_crc", "0000"),
            ("visual_stamp_code", "0000000000000000"),
        ):
            bad = dict(rec)
            bad[field] = bad_val
            ok2, errs2 = sv.verify_record(bad)
            self.assertFalse(ok2, "tampering %s was not detected" % field)
            self.assertTrue(errs2)

    def test_jsonl_and_csv_roundtrip(self):
        defaults = rg.load_grid_doc()["defaults"]
        cell = rg.build_cell({
            "src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 2,
            "render_ahead": True, "queue_depth": 3, "dejudder": True,
        }, defaults)
        recs, _ = gr.simulate_cell(cell, n_source_frames=32)

        jp = os.path.join(self.tmp, "stamps.jsonl")
        cp = os.path.join(self.tmp, "stamps.csv")
        sv.write_jsonl(jp, recs)
        sv.write_csv(cp, recs)
        recs_j = sv.read_jsonl(jp)
        recs_c = sv.read_csv(cp)
        self.assertEqual(recs_j, recs)
        self.assertEqual(recs_c, recs)
        self.assertEqual(sv.verify_cell(cell, recs_j)["status"], sv.PASS)
        self.assertEqual(sv.verify_cell(cell, recs_c)["status"], sv.PASS)

    def test_looprig_artifacts_adapter(self):
        defaults = rg.load_grid_doc()["defaults"]
        cell = rg.build_cell({
            "src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 1,
            "render_ahead": True, "queue_depth": 2, "dejudder": True,
        }, defaults)
        src = Fraction(24000, 1001)
        out = Fraction(60000, 1001)
        ratio = out / src
        n_src = 40
        sent_p = os.path.join(self.tmp, "sent.csv")
        stamps_p = os.path.join(self.tmp, "stamps.csv")
        with open(sent_p, "w", encoding="utf-8", newline="") as f:
            f.write("index,aux,pts_num,pts_den\n")
            for k in range(n_src):
                f.write("%d,%d,%d,%d\n" % (k, k % 5, k * src.denominator, src.numerator))
        rows = []
        for k in range(n_src):
            rows.extend([k] * (int((k + 1) * ratio) - int(k * ratio)))
        with open(stamps_p, "w", encoding="utf-8", newline="") as f:
            f.write("frame_index,arrival_qpc,src_found,src_kind,src_aux,src_index,src_contrast,out_found,out_aux,out_index,out_contrast\n")
            for i, k in enumerate(rows):
                f.write("%d,%d,1,1,%d,%d,200.0,1,0,%d,200.0\n" % (i, i, k % 5, k, i))

        recs = sv.from_looprig_artifacts(stamps_p, sent_p, cell, t0_ns=500_000_000)
        ver = sv.verify_cell(cell, recs)
        self.assertEqual(ver["status"], sv.PASS, ver)

        # Undecoded frame in stamps.csv must fail stamp_integrity (never silently ignored)
        bad_stamps = os.path.join(self.tmp, "bad_stamps.csv")
        with open(stamps_p, "r", encoding="utf-8") as fin, open(bad_stamps, "w", encoding="utf-8") as fout:
            lines = fin.readlines()
            lines[10] = "9,9,0,0,,,0.0,1,0,9,200.0\n"
            fout.writelines(lines)
        bad_recs = sv.from_looprig_artifacts(bad_stamps, sent_p, cell, t0_ns=500_000_000)
        bad_ver = sv.verify_cell(cell, bad_recs)
        self.assertEqual(bad_ver["status"], sv.FAIL)
        self.assertEqual(bad_ver["checks"]["stamp_integrity"]["status"], sv.FAIL)


class StampVerifierInvariantTests(unittest.TestCase):
    def _cell(self, **kw):
        defaults = rg.load_grid_doc()["defaults"]
        base = {
            "src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 2,
            "render_ahead": True, "queue_depth": 3, "dejudder": True,
            "color_mode": "sdr", "track": "clean",
        }
        base.update(kw)
        return rg.build_cell(base, defaults)

    def test_clean_pass_across_diverse_rate_pairs(self):
        pairs = [
            ("23.976", "59.94"), ("24.0", "48"), ("25.0", "50"), ("25.0", "60"),
            ("23.976", "60"), ("29.97", "60"), ("50.0", "120"), ("60.0", "60"),
            ("47.952", "119.88"), ("24.0", "120"),
        ]
        for s, d in pairs:
            c = self._cell(src=s, disp=d)
            recs, base = gr.simulate_cell(c, n_source_frames=56)
            ver = sv.verify_cell(c, recs, bill_baseline=base)
            self.assertEqual(ver["status"], sv.PASS, "%s->%s failed: %s" % (s, d, ver))

    def test_single_dropped_frame_fails(self):
        c = self._cell()
        recs, _ = gr.simulate_cell(c, fault="drop")
        ver = sv.verify_cell(c, recs)
        self.assertEqual(ver["status"], sv.FAIL)
        self.assertEqual(ver["checks"]["monotonic_progression"]["status"], sv.FAIL)
        self.assertEqual(ver["metrics"]["dropped_frames"], 1)

    def test_duplicated_frame_fails_both_1to1_and_pulldown(self):
        for s, d in (("60.0", "60"), ("23.976", "59.94")):
            c = self._cell(src=s, disp=d)
            recs, _ = gr.simulate_cell(c, fault="duplicate")
            ver = sv.verify_cell(c, recs)
            self.assertEqual(ver["status"], sv.FAIL, "%s->%s duplicate not caught" % (s, d))
            self.assertEqual(ver["checks"]["monotonic_progression"]["status"], sv.FAIL)
            self.assertGreaterEqual(ver["metrics"]["duplicated_frames"], 1)

    def test_out_of_order_frame_seq_and_present_ts_fail(self):
        c = self._cell()
        recs, _ = gr.simulate_cell(c, fault="out_of_order")
        ver = sv.verify_cell(c, recs)
        self.assertEqual(ver["status"], sv.FAIL)
        self.assertEqual(ver["checks"]["monotonic_progression"]["status"], sv.FAIL)
        self.assertGreaterEqual(ver["metrics"]["out_of_order_frames"], 1)

        # Also test non-monotonic present_ts_ns directly (re-signed with valid CRC)
        recs2, _ = gr.simulate_cell(c)
        i = len(recs2) // 2
        r = recs2[i]
        recs2[i] = sv.make_record(
            frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
            capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
            present_ts_ns=recs2[i - 1]["present_ts_ns"] - 1000,
            display_vblank_idx=r["display_vblank_idx"], queue_depth=r["queue_depth"],
            pathway=r["pathway"], mode_id=r["mode_id"],
            planned_vblank_idx=r["planned_vblank_idx"], is_extra=r["is_extra"], gate_level=r["gate_level"],
        )
        ver2 = sv.verify_cell(c, recs2)
        self.assertEqual(ver2["status"], sv.FAIL)
        self.assertEqual(ver2["checks"]["monotonic_progression"]["status"], sv.FAIL)

    def test_crc_corruption_and_visual_stamp_mismatch_fail(self):
        c = self._cell()
        recs, _ = gr.simulate_cell(c, fault="corrupt_crc")
        ver = sv.verify_cell(c, recs)
        self.assertEqual(ver["status"], sv.FAIL)
        self.assertEqual(ver["checks"]["stamp_integrity"]["status"], sv.FAIL)
        self.assertEqual(ver["metrics"]["stamp_crc_errors"], 1)

        # Valid record_crc over a mismatched visual_stamp_code (wrong embedded frame index)
        recs2, _ = gr.simulate_cell(c)
        r = recs2[10]
        wrong_vs = sv.encode_visual_stamp(r["frame_seq"] + 999)
        recs2[10] = sv.make_record(
            frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
            capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
            present_ts_ns=r["present_ts_ns"], display_vblank_idx=r["display_vblank_idx"],
            queue_depth=r["queue_depth"], pathway=r["pathway"], mode_id=r["mode_id"],
            visual_stamp_code=wrong_vs, planned_vblank_idx=r["planned_vblank_idx"],
            is_extra=r["is_extra"], gate_level=r["gate_level"],
        )
        ver2 = sv.verify_cell(c, recs2)
        self.assertEqual(ver2["status"], sv.FAIL)
        self.assertEqual(ver2["checks"]["stamp_integrity"]["status"], sv.FAIL)

    def test_constant_delay_drift_fails_and_scales_with_display_period(self):
        c = self._cell()
        recs, _ = gr.simulate_cell(c, fault="delay_drift")
        ver = sv.verify_cell(c, recs)
        self.assertEqual(ver["status"], sv.FAIL)
        self.assertEqual(ver["checks"]["constant_delay"]["status"], sv.FAIL)

        # Proof that constant-delay tolerance scales strictly with 1 / display_hz (no magic ms literal!):
        # A 12 ms present jitter spike is 0.288 P_d at 24 Hz (<= 0.50 P_d -> PASS),
        # but 1.440 P_d at 120 Hz (> 0.50 P_d -> FAIL)!
        spike_ns = 12_000_000
        c24 = self._cell(src="24.0", disp="24", render_ahead=False, queue_depth=0, dejudder=False)
        r24, _ = gr.simulate_cell(c24)
        mid24 = len(r24) // 2
        t24 = r24[mid24]
        r24[mid24] = sv.make_record(
            frame_seq=t24["frame_seq"], source_pts_ns=t24["source_pts_ns"],
            capture_ts_ns=t24["capture_ts_ns"], render_submit_ts_ns=t24["render_submit_ts_ns"],
            present_ts_ns=t24["present_ts_ns"] + spike_ns, display_vblank_idx=t24["display_vblank_idx"],
            queue_depth=0, pathway="vpv", mode_id=c24["cell_id"],
        )
        ver24 = sv.verify_cell(c24, r24)
        self.assertEqual(ver24["checks"]["constant_delay"]["status"], sv.PASS, ver24)

        c120 = self._cell(src="60.0", disp="120", render_ahead=False, queue_depth=0, dejudder=False)
        r120, _ = gr.simulate_cell(c120)
        # Apply the exact same +12 ms spike to the first present of one source frame (and keep monotonic timestamps)
        k_target = r120[len(r120) // 2]["frame_seq"]
        for idx, rr in enumerate(r120):
            if rr["frame_seq"] == k_target:
                r120[idx] = sv.make_record(
                    frame_seq=rr["frame_seq"], source_pts_ns=rr["source_pts_ns"],
                    capture_ts_ns=rr["capture_ts_ns"], render_submit_ts_ns=rr["render_submit_ts_ns"],
                    present_ts_ns=rr["present_ts_ns"] + spike_ns, display_vblank_idx=rr["display_vblank_idx"],
                    queue_depth=0, pathway="vpv", mode_id=c120["cell_id"],
                )
                break
        ver120 = sv.verify_cell(c120, r120)
        self.assertEqual(ver120["checks"]["constant_delay"]["status"], sv.FAIL, ver120)

    def test_render_ahead_silent_fallback_and_late_landing_fail(self):
        c = self._cell(render_ahead=True, queue_depth=3)
        recs_sf, _ = gr.simulate_cell(c, fault="ra_silent_fallback")
        ver_sf = sv.verify_cell(c, recs_sf)
        self.assertEqual(ver_sf["status"], sv.FAIL)
        self.assertEqual(ver_sf["checks"]["render_ahead_proof"]["status"], sv.FAIL)
        self.assertGreater(ver_sf["metrics"]["silent_fallback_frames"], 0)
        self.assertIn("SILENT fallback", ver_sf["checks"]["render_ahead_proof"]["detail"])

        recs_ll, _ = gr.simulate_cell(c, fault="ra_late_landing")
        ver_ll = sv.verify_cell(c, recs_ll)
        self.assertEqual(ver_ll["status"], sv.FAIL)
        self.assertEqual(ver_ll["checks"]["render_ahead_proof"]["status"], sv.FAIL)
        self.assertGreater(ver_ll["metrics"]["late_landings"], 0)

    def test_render_ahead_active_when_disabled_or_original_fails(self):
        c = self._cell(pathway="original", render_ahead=False, queue_depth=0, dejudder=False)
        recs, base = gr.simulate_cell(c)
        r = recs[5]
        recs[5] = sv.make_record(
            frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
            capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
            present_ts_ns=r["present_ts_ns"], display_vblank_idx=r["display_vblank_idx"],
            queue_depth=2, pathway="original", mode_id=c["cell_id"], pixel_hash=r["pixel_hash"],
        )
        ver = sv.verify_cell(c, recs, bill_baseline=base)
        self.assertEqual(ver["status"], sv.FAIL)
        self.assertEqual(ver["checks"]["render_ahead_proof"]["status"], sv.FAIL)

    def test_original_baseline_pass_and_mismatch_fail(self):
        c = self._cell(pathway="original", render_ahead=False, queue_depth=0, dejudder=False)
        recs, base = gr.simulate_cell(c)
        self.assertEqual(sv.verify_cell(c, recs, bill_baseline=base)["status"], sv.PASS)

        recs_bad, base_bad = gr.simulate_cell(c, fault="original_mismatch")
        ver_bad = sv.verify_cell(c, recs_bad, bill_baseline=base_bad)
        self.assertEqual(ver_bad["status"], sv.FAIL)
        self.assertEqual(ver_bad["checks"]["original_baseline"]["status"], sv.FAIL)

    def test_dirty_track_recovery_pass_and_lingering_damage_fail(self):
        c = self._cell(track="dirty")
        recs, _ = gr.simulate_cell(c, n_source_frames=64)
        ver = sv.verify_cell(c, recs)
        self.assertEqual(ver["status"], sv.PASS, ver)
        self.assertEqual(ver["checks"]["track_continuity"]["status"], sv.PASS)

        # Inject a late landing in the post-recovery tail -> must FAIL track_continuity
        r = recs[-2]
        recs[-2] = sv.make_record(
            frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
            capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
            present_ts_ns=r["present_ts_ns"], display_vblank_idx=r["display_vblank_idx"],
            queue_depth=r["queue_depth"], pathway=r["pathway"], mode_id=r["mode_id"],
            planned_vblank_idx=r["display_vblank_idx"] - 2, is_extra=r["is_extra"],
            gate_level=r["gate_level"], segment_idx=r["segment_idx"], is_dirty_event=False,
        )
        ver2 = sv.verify_cell(c, recs)
        self.assertEqual(ver2["status"], sv.FAIL)
        self.assertEqual(ver2["checks"]["track_continuity"]["status"], sv.FAIL)

    def test_rate_transition_convergence_pass_and_stall_fail(self):
        c = self._cell(track="rate_transition", transition_to="59.94")
        recs, _ = gr.simulate_cell(c, n_source_frames=72)
        ver = sv.verify_cell(c, recs)
        self.assertEqual(ver["status"], sv.PASS, ver)
        self.assertEqual(ver["checks"]["track_continuity"]["status"], sv.PASS)
        self.assertLessEqual(ver["metrics"]["transition_converge_periods"],
                             c["tolerances"]["transition_converge_source_periods"])

        recs_stall, _ = gr.simulate_cell(c, fault="transition_stall", n_source_frames=72)
        ver_stall = sv.verify_cell(c, recs_stall)
        self.assertEqual(ver_stall["status"], sv.FAIL)
        self.assertEqual(ver_stall["checks"]["track_continuity"]["status"], sv.FAIL)


class HardwareGatingAndOrchestratorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="row27-orch-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_hardware_absent_emits_skip_no_hardware_zero_false_passes(self):
        out_dir = os.path.join(self.tmp, "hw-out")
        trx_path = os.path.join(out_dir, "hw.trx")
        junit_path = os.path.join(out_dir, "hw.junit.xml")
        rc = gr.main([
            "--mode", "hardware", "--profile", "smoke",
            "--build-dir", os.path.join(self.tmp, "no-such-build"),
            "--out", out_dir, "--trx", trx_path, "--junit", junit_path, "--quiet",
        ])
        self.assertEqual(rc, gr.EXIT_SKIP)

        with open(os.path.join(out_dir, "report.json"), "r", encoding="utf-8") as f:
            rep = json.load(f)
        self.assertEqual(rep["schema"], gr.SCHEMA_REPORT)
        self.assertEqual(rep["overall_status"], gr.STATUS_SKIP_HW)
        self.assertFalse(rep["hardware_verified"])
        self.assertEqual(rep["exit_code"], gr.EXIT_SKIP)
        self.assertEqual(rep["counts"]["pass"], 0)
        self.assertEqual(rep["counts"]["fail"], 0)
        self.assertEqual(rep["counts"]["skip_no_hardware"], rep["counts"]["total"])
        self.assertGreater(rep["counts"]["total"], 0)
        for cr in rep["cells"]:
            self.assertEqual(cr["status"], gr.STATUS_SKIP_HW)
            self.assertIn("SKIP-NO-HARDWARE", cr["detail"])
            self.assertTrue(cr["skip_reason"])

        # TRX must report NotExecuted (NEVER Passed!) with SKIP-NO-HARDWARE in StdOut
        trx = _trx_results(trx_path)
        self.assertEqual(len(trx), rep["counts"]["total"])
        for name, (outcome, text) in trx.items():
            self.assertEqual(outcome, "NotExecuted", "%s had outcome %s" % (name, outcome))
            self.assertIn("SKIP-NO-HARDWARE", text)

        # JUnit must emit <skipped type="SKIP-NO-HARDWARE" .../>
        jroot = ET.parse(junit_path).getroot()
        self.assertEqual(int(jroot.get("skipped")), rep["counts"]["total"])
        self.assertEqual(int(jroot.get("failures")), 0)
        skips = jroot.findall(".//skipped")
        self.assertEqual(len(skips), rep["counts"]["total"])
        self.assertTrue(all(s.get("type") == gr.STATUS_SKIP_HW for s in skips))

    def test_hardware_present_missing_clip_emits_skip_no_footage(self):
        custom = {
            "cells": [
                {"src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 2,
                 "render_ahead": True, "queue_depth": 3, "dejudder": True,
                 "requires_clip": "missing_master_clip.y4m"},
            ]
        }
        plan = rg.resolve_grid("custom", custom_spec=custom)
        rep = gr.execute_grid(
            plan=plan,
            mode="hardware",
            clips_dir=self.tmp,
            probe_override={"hardware_ready": True, "replay_ready": False, "missing": [], "checks": {}},
        )
        self.assertEqual(rep["exit_code"], gr.EXIT_SKIP)
        self.assertFalse(rep["hardware_verified"])
        self.assertEqual(rep["counts"]["pass"], 0)
        self.assertEqual(rep["counts"]["skip_no_footage"], 1)
        self.assertEqual(rep["cells"][0]["status"], gr.STATUS_SKIP_FOOTAGE)
        self.assertIn("SKIP-NO-FOOTAGE", rep["cells"][0]["detail"])

    def test_hardware_replay_dir_scores_real_recorded_stamps(self):
        plan = rg.resolve_grid("smoke", filter_expr="src=23.976,track=clean")
        replay_dir = os.path.join(self.tmp, "replay")
        # Record stamps for the first cell only; omit the rest to also test partial SKIP-NO-FOOTAGE
        first_cell = plan["cells"][0]
        recs, _ = gr.simulate_cell(first_cell, n_source_frames=56)
        sv.write_jsonl(os.path.join(replay_dir, first_cell["cell_id"], "stamps.jsonl"), recs)

        out_dir = os.path.join(self.tmp, "replay-out")
        rc = gr.main([
            "--mode", "hardware", "--profile", "smoke", "--filter", "src=23.976,track=clean",
            "--replay-dir", replay_dir, "--out", out_dir, "--quiet",
        ])
        with open(os.path.join(out_dir, "report.json"), "r", encoding="utf-8") as f:
            rep = json.load(f)
        by_id = {c["cell_id"]: c for c in rep["cells"]}
        self.assertEqual(by_id[first_cell["cell_id"]]["status"], gr.STATUS_PASS)
        if len(plan["cells"]) > 1:
            self.assertEqual(rc, gr.EXIT_SKIP)
            self.assertEqual(rep["overall_status"], "PARTIAL-SKIP")
            self.assertFalse(rep["hardware_verified"])
            self.assertGreater(rep["counts"]["skip_no_footage"], 0)

    def test_synthetic_smoke_and_negative_controls_and_reports(self):
        out_dir = os.path.join(self.tmp, "synth-out")
        trx_path = os.path.join(out_dir, "synth.trx")
        junit_path = os.path.join(out_dir, "synth.junit.xml")
        rc = gr.main([
            "--synthetic", "--profile", "smoke", "--include-negative-controls",
            "--write-stamps", "--out", out_dir, "--trx", trx_path, "--junit", junit_path, "--quiet",
        ])
        self.assertEqual(rc, gr.EXIT_PASS)

        with open(os.path.join(out_dir, "report.json"), "r", encoding="utf-8") as f:
            rep = json.load(f)
        self.assertEqual(rep["overall_status"], gr.STATUS_PASS)
        self.assertFalse(rep["hardware_verified"])  # synthetic mode is never marked hardware_verified!
        self.assertEqual(rep["counts"]["pass"], rep["counts"]["total"])
        self.assertEqual(rep["counts"]["fail"], 0)
        self.assertEqual(rep["counts"]["negative_controls_passed"], len(gr.NEGATIVE_CONTROL_FAULTS))
        self.assertEqual(rep["counts"]["negative_controls_failed"], 0)

        # Verify comparative Original vs VP-V delta was computed for matching 23.976->59.94 pair
        vpv_23 = [c for c in rep["cells"]
                  if c["config"]["pathway"] == "vpv" and c["config"]["source_hz_label"] == "23.976"
                  and c["config"]["display_hz_label"] == "59.94"]
        self.assertTrue(vpv_23)
        self.assertIn("vs_original_latency_delta_periods", vpv_23[0]["metrics"])

        # Verify Markdown, stamps JSONL, TRX, and JUnit outputs exist and are valid
        with open(os.path.join(out_dir, "report.md"), "r", encoding="utf-8") as f:
            md = f.read()
        self.assertIn("# Row-27 Loop Rig Report (PASS)", md)
        self.assertIn("## Negative Controls", md)
        stamps_dir = os.path.join(out_dir, "stamps")
        self.assertEqual(len(os.listdir(stamps_dir)), rep["counts"]["total"])
        trx = _trx_results(trx_path)
        self.assertEqual(set(o for o, _ in trx.values()), {"Passed"})
        jroot = ET.parse(junit_path).getroot()
        self.assertEqual(int(jroot.get("failures")), 0)
        self.assertEqual(int(jroot.get("skipped")), 0)

    def test_plan_dry_run_mode(self):
        out_dir = os.path.join(self.tmp, "plan-out")
        rc = gr.main(["--dry-run", "--profile", "row27", "--out", out_dir, "--quiet"])
        self.assertEqual(rc, gr.EXIT_PASS)
        with open(os.path.join(out_dir, "plan.json"), "r", encoding="utf-8") as f:
            plan = json.load(f)
        self.assertEqual(plan["schema"], rg.SCHEMA_PLAN)
        self.assertGreaterEqual(plan["total_cells"], 25)
        self.assertTrue(os.path.isfile(os.path.join(out_dir, "plan.md")))

    def test_cli_subprocess_hardware_exit_code_is_3(self):
        out_dir = os.path.join(self.tmp, "sub-hw")
        proc = subprocess.run(
            [sys.executable, "-I", RUNNER, "--mode", "hardware", "--profile", "smoke",
             "--build-dir", os.path.join(self.tmp, "missing-build"), "--out", out_dir],
            capture_output=True, text=True,
        )
        self.assertEqual(proc.returncode, gr.EXIT_SKIP, proc.stdout + proc.stderr)
        self.assertIn("SKIP-NO-HARDWARE", proc.stdout)

    @unittest.skipUnless(POWERSHELL, "PowerShell not available")
    def test_powershell_wrapper_modes_and_safety(self):
        # 1. Safety guard refuses C:\Videoprocessor*
        r_bad = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PS1,
             "-RunRoot", "C:\\Videoprocessor\\forbidden", "-Mode", "plan"],
            capture_output=True, text=True,
        )
        self.assertEqual(r_bad.returncode, 2)

        # 2. Plan mode succeeds
        r_plan = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PS1,
             "-RunRoot", self.tmp, "-Mode", "plan", "-Profile", "smoke"],
            capture_output=True, text=True,
        )
        self.assertEqual(r_plan.returncode, 0, r_plan.stdout + r_plan.stderr)

        # 3. Hardware mode without rig logs SKIP-NO-HARDWARE and exits 3
        r_hw = subprocess.run(
            [POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PS1,
             "-RunRoot", self.tmp, "-Mode", "hardware", "-Profile", "smoke",
             "-BuildDir", os.path.join(self.tmp, "no-build"), "-EmitTrx", "-EmitJUnit"],
            capture_output=True, text=True,
        )
        self.assertEqual(r_hw.returncode, gr.EXIT_SKIP, r_hw.stdout + r_hw.stderr)
        self.assertIn("SKIP-NO-HARDWARE", r_hw.stdout)


if __name__ == "__main__":
    unittest.main()
