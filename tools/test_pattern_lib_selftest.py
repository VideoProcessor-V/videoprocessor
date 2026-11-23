# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-C3: PatternLibSelfTest.cpp (pattern library, calibration_lut) is not a TEST_CLASS and is in no project, so no gate
ever compiled or ran it. run_chain_gate.ps1 now calls run_pattern_lib_tests.ps1 (cl.exe build + run, exit code must be 0);
this suite runs the same script in the PY lane and checks the wiring. A Visual Studio C++ toolset is required: without one
the build fails and so does this test (it is not skipped)."""
import os
import re
import subprocess
import tempfile
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(_dir)
PATTERN_DIR = os.path.join(ROOT, "src", "VP-V", "modules", "output", "calibration_lut", "pattern_lib")
RUNNER = os.path.join(PATTERN_DIR, "run_pattern_lib_tests.ps1")
GATE = os.path.join(_dir, "run_chain_gate.ps1")


class PatternLibSelfTest(unittest.TestCase):
    def test_the_self_test_builds_runs_and_every_check_passes(self):
        with tempfile.TemporaryDirectory() as out:
            done = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", RUNNER, "-OutDir", out],
                                  capture_output=True, text=True, timeout=600)
            text = done.stdout + done.stderr
            self.assertEqual(done.returncode, 0, text[-2000:])
            m = re.search(r"pattern_lib self test: (\d+) checks, (\d+) failed", text)
            self.assertIsNotNone(m, text[-2000:])
            self.assertGreater(int(m.group(1)), 0)
            self.assertEqual(int(m.group(2)), 0)

    def test_the_sources_it_compiles_exist(self):
        for rel in ("ColorMath.cpp", "PatternCodes.cpp", "PatternPlan.cpp", os.path.join("tests", "PatternLibSelfTest.cpp")):
            self.assertTrue(os.path.isfile(os.path.join(PATTERN_DIR, rel)), rel)


class ChainGateWiring(unittest.TestCase):
    def setUp(self):
        with open(GATE, encoding="utf-8") as f:
            self.text = f.read()

    def test_the_gate_calls_the_runner_and_fails_on_a_non_zero_exit(self):
        t = self.text
        self.assertIn("[switch]$NoPatternLibTests", t)
        self.assertIn("if (-not $NoPatternLibTests)", t)
        self.assertIn("run_pattern_lib_tests.ps1", t)
        self.assertIn('throw "pattern library self test failed', t)
        self.assertIn("-ne 0", t[t.index("if (-not $NoPatternLibTests)"):t.index('throw "pattern library self test failed')])

    def test_the_step_runs_before_the_gpu_run_and_after_the_filter_print(self):
        t = self.text
        step = t.index("if (-not $NoPatternLibTests)")
        self.assertGreater(step, t.index("if ($PrintFilter)"))
        self.assertLess(step, t.index("vstest.console.exe"))

    def test_the_runner_script_the_gate_names_exists_at_that_relative_path(self):
        m = re.search(r'\$patternScript = Join-Path \$PSScriptRoot "([^"]+)"', self.text)
        self.assertIsNotNone(m)
        self.assertTrue(os.path.isfile(os.path.normpath(os.path.join(_dir, m.group(1)))), m.group(1))


if __name__ == "__main__":
    unittest.main()
