# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-MC-S15a / S15b: the Beta host entry points (plain functions, no TEST_METHOD, so the C++ scanners cannot see them) must
not skip silently. The Beta timing gate honours VP_TEST_REQUIRE_ALL as well as VP_TEST_REQUIRE_GPU and logs SKIP-NO-GPU; the four
report modes (option quality, de-judder quality, dither compare, real footage) log SKIP-NO-FOOTAGE and fail under
VP_TEST_REQUIRE_FOOTAGE / VP_TEST_REQUIRE_ALL. These are source-structure checks that run in the PY lane; the behaviour itself
is exercised on the Beta host in the gate run."""
import os
import re
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(os.path.dirname(_dir), "src", "VP-V", "tests", "beta_gpu", "BetaGpuTests.cpp")


def source():
    with open(SOURCE, encoding="utf-8-sig") as f:
        return f.read()


def function_body(text, name):
    """Text of `void <name>()` up to the next top-level function definition."""
    start = text.index("\nvoid %s()\n" % name)
    nxt = re.search(r"\n(?:void|int|static) \w+\(", text[start + 10:])
    return text[start:start + 10 + (nxt.start() if nxt else len(text))]


class BetaTimingGate(unittest.TestCase):
    def test_require_all_makes_the_gpu_skip_fail(self):
        text = source()
        self.assertIn('bool BetaRequireGpu() { return BetaRequireAll() || BetaEnvIsOne("VP_TEST_REQUIRE_GPU"); }', text)
        self.assertIn('bool BetaRequireAll() { return BetaEnvIsOne("VP_TEST_REQUIRE_ALL"); }', text)
        body = function_body(text, "RunBetaTimingGate")
        self.assertIn("Check(!BetaRequireGpu(),", body)
        self.assertNotIn('"VP_TEST_REQUIRE_GPU") == 0', body)   # the old inline REQUIRE_GPU-only read is gone

    def test_the_gpu_skip_logs_the_fixed_marker_before_it_returns(self):
        body = function_body(source(), "RunBetaTimingGate")
        marker = body.index('"SKIP-NO-GPU Beta timing gate:')
        self.assertLess(marker, body.index("Check(!BetaRequireGpu(),"))
        self.assertLess(body.index("Check(!BetaRequireGpu(),"), body.index("return;"))


class BetaReportModes(unittest.TestCase):
    MODES = (("RunBetaOptionQuality", "Beta option quality"), ("RunBetaDejudderQuality", "Beta de-judder quality"),
             ("RunBetaDitherCompare", "Beta dither compare"), ("RunBetaRealFootage", "Beta real footage"))

    def test_a_missing_input_goes_through_the_marker_helper_and_not_plain_text(self):
        text = source()
        for func, label in self.MODES:
            with self.subTest(report=func):
                body = function_body(text, func)
                self.assertIn('BetaMissingFootage("%s",' % label, body)
                self.assertNotIn("%s: VP_TEST_CLIP not set" % label, body)   # the old plain-text skip lines
                self.assertNotIn("%s: no 3840x2160" % label, body)

    def test_the_helper_logs_the_marker_and_fails_under_require_footage_or_all(self):
        text = source()
        helper = text[text.index("void BetaMissingFootage("):]
        helper = helper[:helper.index("\n\t}\n")]
        self.assertIn('std::cout << "SKIP-NO-FOOTAGE " << report', helper)
        self.assertIn("Check(!BetaRequireFootage(),", helper)
        self.assertLess(helper.index("SKIP-NO-FOOTAGE"), helper.index("Check(!BetaRequireFootage()"))
        self.assertIn('bool BetaRequireFootage() { return BetaRequireAll() || BetaEnvIsOne("VP_TEST_REQUIRE_FOOTAGE"); }', text)

    def test_the_report_modes_keep_their_distinct_exit_code(self):
        path = os.path.join(os.path.dirname(_dir), "src", "VideoProcessor-BetaTests", "BetaParametersTests.cpp")
        with open(path, encoding="utf-8-sig") as f:
            text = f.read()
        self.assertIn("const int kBetaReportModeExitCode = 3;", text)
        self.assertIn('std::cout << "MODE " << name << ": gate not run\\n";', text)


if __name__ == "__main__":
    unittest.main()
