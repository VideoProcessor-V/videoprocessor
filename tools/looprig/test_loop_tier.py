# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Offline tests for the loop tier runner (no rig). Run: python -I -B tools/looprig/test_loop_tier.py

Covers the helper (plan, synth, score, TRX), the SKIP-NO-RIG detection path, the test-category wiring
and, when PowerShell is present, run_loop_tier.ps1 in -DryRunSynthetic and SKIP-NO-RIG modes.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)

import loop_tier as lt  # noqa: E402

PS1 = os.path.join(HERE, "run_loop_tier.ps1")
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


def trx_outcomes(path):
    ns = {"t": "http://microsoft.com/schemas/VisualStudio/TeamTest/2010"}
    root = ET.parse(path).getroot()
    return {r.get("testName"): r.get("outcome") for r in root.findall("t:Results/t:UnitTestResult", ns)}


class HelperTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="looptier-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_plan_selects_by_mode_and_hides_attended(self):
        ids = [e["id"] for e in lt.plan_for("1080p60")]
        self.assertIn("film24_to_60", ids)
        self.assertNotIn("resync_atv_switch", ids)
        self.assertIn("resync_atv_switch", [e["id"] for e in lt.plan_for("1080p60", attended=True)])
        self.assertEqual([e["id"] for e in lt.plan_for("2160p30")], ["film24_to_30"])

    def test_every_plan_scenario_exists_in_scenarios_json(self):
        with open(os.path.join(HERE, "scenarios.json"), "r", encoding="utf-8") as f:
            known = {s["id"] for s in json.load(f)["scenarios"]}
        for e in lt.load_plan()["scenarios"]:
            self.assertIn(e.get("scenario", e["id"]), known)

    def test_clean_synthetic_capture_passes_and_broken_fails(self):
        self.assertEqual(lt.main(["synth", "--out", self.tmp, "--mode", "1080p60", "--break", "sport60_native"]), 0)
        self.assertNotEqual(lt.main(["score", "--run", self.tmp]), 0)
        out = trx_outcomes(os.path.join(self.tmp, "loop-tier.trx"))
        self.assertEqual(out["LoopRig.film24_to_60"], "Passed")
        self.assertEqual(out["LoopRig.sport60_native"], "Failed")
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "summary.md")))

    def test_clean_capture_exits_zero(self):
        lt.main(["synth", "--out", self.tmp, "--mode", "1080p60"])
        self.assertEqual(lt.main(["score", "--run", self.tmp]), 0)

    def test_missing_data_fails_closed(self):
        d = os.path.join(self.tmp, "film24_to_60")
        os.makedirs(d)
        with open(os.path.join(d, "meta.json"), "w", encoding="utf-8") as f:
            json.dump({"scenario": "film24_to_60", "width": 1920, "height": 1080}, f)
        self.assertEqual(lt.main(["score", "--run", self.tmp]), 1)
        self.assertEqual(trx_outcomes(os.path.join(self.tmp, "loop-tier.trx"))["LoopRig.film24_to_60"], "Failed")

    def test_recorder_error_fails(self):
        lt.main(["synth", "--out", self.tmp, "--mode", "1080p60", "--only", "film24_to_60"])
        mp = os.path.join(self.tmp, "film24_to_60", "meta.json")
        with open(mp, "r", encoding="utf-8") as f:
            m = json.load(f)
        m["recorder_error"] = "StreamRecorder exit 1"
        with open(mp, "w", encoding="utf-8") as f:
            json.dump(m, f)
        self.assertEqual(lt.main(["score", "--run", self.tmp]), 1)

    def test_skip_no_rig_writes_notexecuted_and_exits_zero(self):
        self.assertEqual(lt.main(["score", "--run", self.tmp, "--rig-state", "SKIP-NO-RIG test"]), 0)
        out = trx_outcomes(os.path.join(self.tmp, "loop-tier.trx"))
        self.assertTrue(out and set(out.values()) == {"NotExecuted"})

    def test_detect_without_recorder_is_skip(self):
        self.assertEqual(lt.main(["detect", "--recorder", os.path.join(self.tmp, "nope.exe")]), 10)

    @unittest.skipUnless(os.name == "nt", "fake recorder is a .cmd")
    def test_detect_matches_device_name(self):
        rec = os.path.join(self.tmp, "rec.cmd")
        with open(rec, "w") as f:
            f.write("@echo 0: DeckLink Quad HDMI Recorder (2)\r\n@echo 1: DeckLink Mini Recorder 4K\r\n")
        self.assertEqual(lt.main(["detect", "--recorder", rec]), 0)
        with open(rec, "w") as f:
            f.write("@echo 0: DeckLink Quad HDMI Recorder (2)\r\n")
        self.assertEqual(lt.main(["detect", "--recorder", rec]), 10)


class CategoryTests(unittest.TestCase):
    def run_gen(self, *args):
        return subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_test_filters.py")] + list(args),
                              capture_output=True, text=True).stdout.strip()

    def test_looprig_excluded_from_cpu_and_default_gpu_only_selectable(self):
        self.assertIn("Category!=LoopRig", self.run_gen("--cpu").split("&"))
        self.assertNotIn("Category=LoopRig", self.run_gen("--gpu").split("|"))
        self.assertEqual(self.run_gen("--gpu", "--categories", "LoopRig"), "Category=LoopRig")


@unittest.skipUnless(POWERSHELL, "PowerShell not available")
class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="looptier-run-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def ps(self, *args):
        return subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PS1, "-RunRoot", self.tmp] + list(args),
                              capture_output=True, text=True)

    def only_run(self):
        runs = [os.path.join(self.tmp, d) for d in os.listdir(self.tmp)]
        self.assertEqual(len(runs), 1)
        return runs[0]

    def test_dry_run_synthetic_pipeline(self):
        r = self.ps("-DryRunSynthetic")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = trx_outcomes(os.path.join(self.only_run(), "loop-tier.trx"))
        self.assertEqual(set(out.values()), {"Passed"})

    def test_no_rig_is_skip_exit_zero_and_leaves_no_marker(self):
        r = self.ps("-BuildDir", os.path.join(self.tmp, "no-build"), "-GpuQuietPath", os.path.join(self.tmp, "GPU-QUIET"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("SKIP-NO-RIG", r.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "GPU-QUIET")))

    def test_run_root_in_production_folder_refused(self):
        r = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PS1, "-RunRoot", "C:\\Videoprocessor\\x", "-DryRunSynthetic"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
