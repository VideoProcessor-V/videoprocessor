# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""CC-RELOC2-MUST1 drift guard (lead20/CC-gemini-verify-2.md SHOULD-2, SHOULD-4, MUST-1):
pins that VP-V's relocated copies of Bill's timing helpers stay body-identical to Bill's definitions
in LibplaceboVideoRenderer.cpp, that PerformanceCounterNow and RenderAheadNow have one VP-V definition
matching Bill's QPC error handling, that Bill's 3 comment lines above RenderLocked( stay byte-exact to
a591ad3b5, and that Bill's BetaParametersTests.cpp retains all 4 #ifdef VP_BETA_TEST_DLL VPV-HOOK tags."""
import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LVR = os.path.join(ROOT, "src", "VideoProcessor-Lib", "vprenderer", "LibplaceboVideoRenderer.cpp")
DEJUDDER_H = os.path.join(ROOT, "src", "VP-V", "modules", "motion", "consumers", "DejudderExtrasPresent.h")
DEJUDDER_CPP = os.path.join(ROOT, "src", "VP-V", "modules", "motion", "consumers", "DejudderExtrasPresent.cpp")
PRESENT_H = os.path.join(ROOT, "src", "VP-V", "modules", "output", "presentation", "PresentationHostHelpers.h")
PRESENT_CPP = os.path.join(ROOT, "src", "VP-V", "modules", "output", "presentation", "PresentationHostHelpers.cpp")
BETA_TESTS = os.path.join(ROOT, "src", "VideoProcessor-BetaTests", "BetaParametersTests.cpp")


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _extract_brace_block(text, anchor_rx):
    m = re.search(anchor_rx, text, re.M)
    if not m:
        return None
    i = text.find("{", m.end() - 1)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        ch = text[j]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def _strip_comments_and_ws(code):
    code = re.sub(r"//[^\n]*", "", code)
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r"\s+", " ", code).strip()
    return re.sub(r"\(\s+", "(", code)


class Reloc2TimingHelperDriftTests(unittest.TestCase):
    def test_frame_gpu_timing_scope_matches_bill_alpha_scope(self):
        bill = _extract_brace_block(_read(LVR), r"\bclass\s+AlphaFrameGpuTimingScope\b")
        vpv = _extract_brace_block(_read(DEJUDDER_H), r"\bclass\s+FrameGpuTimingScope\b")
        self.assertIsNotNone(bill, "Bill's AlphaFrameGpuTimingScope not found in LibplaceboVideoRenderer.cpp")
        self.assertIsNotNone(vpv, "Vpv::Dejudder::FrameGpuTimingScope not found in DejudderExtrasPresent.h")
        bill_norm = _strip_comments_and_ws(bill).replace("AlphaFrameGpuTimingScope", "FrameGpuTimingScope")
        vpv_norm = _strip_comments_and_ws(vpv)
        self.assertEqual(bill_norm, vpv_norm, "FrameGpuTimingScope drifted from Bill's AlphaFrameGpuTimingScope")

    def test_timer_suspension_scope_matches_bill_alpha_scope(self):
        bill = _extract_brace_block(_read(LVR), r"\bclass\s+AlphaLibplaceboTimerSuspensionScope\b")
        vpv = _extract_brace_block(_read(DEJUDDER_CPP), r"\bclass\s+LibplaceboTimerSuspensionScope\b")
        self.assertIsNotNone(bill, "Bill's AlphaLibplaceboTimerSuspensionScope not found in LibplaceboVideoRenderer.cpp")
        self.assertIsNotNone(vpv, "LibplaceboTimerSuspensionScope not found in DejudderExtrasPresent.cpp")
        bill_norm = _strip_comments_and_ws(bill).replace("AlphaLibplaceboTimerSuspensionScope", "LibplaceboTimerSuspensionScope")
        vpv_norm = _strip_comments_and_ws(vpv)
        self.assertEqual(bill_norm, vpv_norm, "LibplaceboTimerSuspensionScope drifted from Bill's AlphaLibplaceboTimerSuspensionScope")

    def test_performance_counter_now_matches_bill_and_has_one_vpv_definition(self):
        bill = _extract_brace_block(_read(LVR), r"\bint64_t\s+PerformanceCounterNow\s*\(\s*\)")
        vpv = _extract_brace_block(_read(PRESENT_CPP), r"\bint64_t\s+PerformanceCounterNow\s*\(\s*\)")
        self.assertIsNotNone(bill, "Bill's PerformanceCounterNow not found in LibplaceboVideoRenderer.cpp")
        self.assertIsNotNone(vpv, "Vpv::PresentationHost::PerformanceCounterNow not found in PresentationHostHelpers.cpp")
        self.assertEqual(_strip_comments_and_ws(bill), _strip_comments_and_ws(vpv),
                         "Vpv::PresentationHost::PerformanceCounterNow drifted from Bill's definition (must return 0 on QPC failure)")
        # One VP-V definition only: DejudderExtrasPresent.cpp must not define its own PerformanceCounterNow or RenderAheadNow.
        dj = _read(DEJUDDER_CPP)
        self.assertIsNone(re.search(r"\bint64_t\s+PerformanceCounterNow\s*\(\s*\)\s*\{", dj),
                          "duplicate PerformanceCounterNow definition in DejudderExtrasPresent.cpp")
        self.assertIsNone(re.search(r"\bdouble\s+RenderAheadNow\s*\(\s*\)\s*\{", dj),
                          "duplicate RenderAheadNow definition in DejudderExtrasPresent.cpp")
        self.assertIn("int64_t PerformanceCounterNow();", _read(PRESENT_H))

    def test_bill_render_locked_comment_lines_are_byte_exact_to_a591ad3b5(self):
        expected = [
            "\t// The caller holds renderMutex. Queue-generation validation and rendering",
            "\t// share that lock so a frame removed before a reset cannot cross the reset",
            "\t// boundary while waiting to enter libplacebo.",
            "\tbool RenderLocked(",
        ]
        lines = _read(LVR).splitlines()
        idx = next((i for i, l in enumerate(lines) if l == "\tbool RenderLocked("), None)
        self.assertIsNotNone(idx, "RenderLocked( not found in LibplaceboVideoRenderer.cpp")
        self.assertEqual(lines[idx - 3:idx + 1], expected,
                         "Bill's 3 comment lines directly above RenderLocked( must be byte-exact to a591ad3b5")
        # Also cross-check against git a591ad3b5 when available.
        r = subprocess.run(["git", "-C", ROOT, "show", "a591ad3b5:src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp"],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode == 0:
            bill_lines = r.stdout.splitlines()
            bidx = next((i for i, l in enumerate(bill_lines) if l == "\tbool RenderLocked("), None)
            self.assertIsNotNone(bidx)
            self.assertEqual(lines[idx - 3:idx + 1], bill_lines[bidx - 3:bidx + 1])

    def test_beta_parameters_tests_keeps_all_four_dll_entry_vpv_hook_tags(self):
        text = _read(BETA_TESTS)
        for line in (
            "#ifdef VP_BETA_TEST_DLL // VPV-HOOK",
            "int RunBetaTestMain() { // VPV-HOOK",
            "#else // VPV-HOOK",
            "#endif // VPV-HOOK",
        ):
            self.assertIn(line, text, "missing W1 VPV-HOOK tag in BetaParametersTests.cpp: " + line)


if __name__ == "__main__":
    unittest.main()
