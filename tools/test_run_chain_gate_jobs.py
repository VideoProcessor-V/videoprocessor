# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-X2: run_chain_gate.ps1 -Footage / -Corpus / -Bench select their category and take the env from the generator.

Uses -PrintFilter, which prints the filter (and each job's env) and exits before any power, vstest or GPU step.
"""
import os
import subprocess
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(_dir, "run_chain_gate.ps1")


def print_filter(*switches, names="X", no_harness=True):
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", GATE, "-PrintFilter", "-Names", names]
    if no_harness:
        cmd.append("-NoHarness")
    cmd += list(switches)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return p.returncode, [ln.strip() for ln in p.stdout.splitlines() if ln.strip()], p.stderr


class DedicatedJobSwitches(unittest.TestCase):
    def test_no_switch_selects_no_dedicated_category(self):
        rc, lines, err = print_filter()
        self.assertEqual(rc, 0, err)
        self.assertEqual(lines, ["FullyQualifiedName~X"])

    def test_each_switch_adds_its_category_and_prints_its_env(self):
        expected = {
            "-Footage": ("Category=Footage", "env Footage: VP_TEST_REQUIRE_FOOTAGE=1"),
            "-Corpus": ("Category=Corpus", "env Corpus: VP_TEST_REQUIRE_CORPUS=1"),
            "-Bench": ("Category=Bench", "env Bench: VP_TEST_BENCH=1;VP_TEST_REQUIRE_BENCH=1"),
        }
        for switch, (clause, env_line) in expected.items():
            with self.subTest(switch=switch):
                rc, lines, err = print_filter(switch)
                self.assertEqual(rc, 0, err)
                self.assertIn(clause, lines[0].split("|"))
                self.assertIn("FullyQualifiedName~X", lines[0].split("|"))
                self.assertIn(env_line, lines)

    def test_a_dedicated_switch_does_not_pull_in_the_other_categories(self):
        rc, lines, err = print_filter("-Footage")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("Category=Corpus", lines[0].split("|"))
        self.assertNotIn("Category=Bench", lines[0].split("|"))

    def test_default_hardware_set_never_holds_the_dedicated_categories(self):
        rc, lines, err = print_filter(names="X", no_harness=False)
        self.assertEqual(rc, 0, err)
        clauses = lines[0].split("|")
        for kind in ("Footage", "Corpus", "Bench"):
            self.assertNotIn("Category=" + kind, clauses)

    def test_the_gate_fails_on_a_requested_jobs_skip_markers(self):
        with open(GATE, encoding="utf-8") as f:
            text = f.read()
        for marker in ("SKIP-NO-FOOTAGE *", "SKIP-NO-CORPUS *", "SKIP-OPT-IN *"):
            self.assertIn('"%s"' % marker, text)
        self.assertIn("$noDedicated.Count -gt 0", text)


if __name__ == "__main__":
    unittest.main()
