# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_precision.py")
_s = importlib.util.spec_from_file_location("check_precision", _p)
N2 = importlib.util.module_from_spec(_s)
_s.loader.exec_module(N2)


def _w(root, rel, text):
    full = os.path.join(root, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", newline="\n") as fh:
        fh.write(text)


def _run(root, *args):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = N2.main(["--root", root] + list(args))
    return rc, buf.getvalue()


class ClassifyTests(unittest.TestCase):
    def test_formats(self):
        for t in ("DXGI_FORMAT_R8G8B8A8_UNORM", "DXGI_FORMAT_R16_FLOAT", "TexFormat::R8", "x = \"rgba16hf\";", "RG16F"):
            self.assertEqual(N2.classify_line(t), ["fmt"], t)
        self.assertEqual(N2.classify_line("DXGI_FORMAT_R16_UNORM"), [])
        self.assertEqual(N2.classify_line("DXGI_FORMAT_R32_FLOAT"), [])

    def test_shifts_need_sample_context(self):
        self.assertEqual(N2.classify_line("auto y = lumaWord >> 6;"), ["shift"])
        self.assertEqual(N2.classify_line("int n = flags >> 4;"), [])
        self.assertEqual(N2.classify_line("x = luma >> 3;"), [])

    def test_named_shift_constant_counts_like_the_literal(self):
        # the 11-14 evasion: `>> 6` renamed to a constant equal to 6 must still be a hit
        src = "constexpr int kAxisFractionBits = 6;\nauto y = luma_word >> kAxisFractionBits;\n"
        hits = N2.scan_text("a.cpp", src, {})
        self.assertEqual([h["pattern"] for h in hits], ["shift"])
        self.assertEqual(N2.classify_line("v = luma >> kQ", {"kQ"}), ["shift"])
        self.assertEqual(N2.classify_line("v = luma >> ns::kQ", {"kQ"}), ["shift"])
        self.assertEqual(N2.classify_line("v = luma >> kQuad", {"kQ"}), [])

    def test_named_constant_defined_in_another_file(self):
        with tempfile.TemporaryDirectory() as r:
            _w(r, "src/VP-V/a/Axis.h", "constexpr int kAxisFractionBits = 6;\n")
            _w(r, "src/VP-V/a/B.cpp", "auto y = luma;\n")
            self.assertEqual(_run(r, "--write-baseline")[0], 0)
            _w(r, "src/VP-V/a/B.cpp", "auto y = luma >> kAxisFractionBits;\n")
            rc, out = _run(r)
            self.assertEqual(rc, 1)
            self.assertIn("GREW src/VP-V/a/B.cpp", out)

    def test_named_constants_of_other_values_or_context_are_not_hits(self):
        src = "constexpr int kMode = 3;\nconstexpr int kBits = 6;\nauto a = luma >> kMode;\nauto b = flags >> kBits;\n"
        self.assertEqual(N2.scan_text("a.cpp", src, {}), [])

    def test_code_word_is_shift_context(self):
        # the context pattern once held a backspace byte, so "code" never matched
        self.assertEqual(N2.classify_line("v = (code >> 2) & 1;"), ["shift"])
        self.assertEqual(N2.classify_line("v = evenCode >> 6;"), ["shift"])
        self.assertEqual(N2.classify_line("v = flags >> 2;"), [])
        self.assertNotIn(bytes([8]), open(N2.__file__, "rb").read())

    def test_axis_fraction_shift_on_a_q6_sample_is_a_hit(self):
        # CHK-SHIFT-AXISFRAC (lead17 c4-s2a-fix G6): the S-2a floor `q6 >> kAxisFractionBits` was invisible because
        # neither `q6` nor the axis constant was a sample word
        src = ("constexpr int kAxisFractionBits = 6;\n"
               "inline uint32_t Floor10FromQ6(uint32_t q6) { return q6 >> kAxisFractionBits; }\n"
               "inline uint32_t Bin(uint32_t s) { const uint32_t bin = s >> kAxisFractionBits; return bin; }\n"
               "inline uint32_t Ns(uint32_t s) { return s >> AnalysisDomain::kAxisFractionBits; }\n")
        hits = N2.scan_text("a.h", src, {})
        self.assertEqual([(h["line"], h["pattern"]) for h in hits], [(2, "shift"), (3, "shift"), (4, "shift")])
        self.assertEqual(N2.classify_line("v = q6 >> 6;"), ["shift"])
        self.assertEqual(N2.classify_line("v = valueQ6 >> 4;"), ["shift"])
        self.assertEqual(N2.classify_line("v = axisCode >> 2;"), ["shift"])
        # the other widths and the left shift stay quiet; a plain flag shift is still not a sample
        self.assertEqual(N2.classify_line("v = q6 >> 3;"), [])
        self.assertEqual(N2.classify_line("v = q6 << kAxisFractionBits;", {"kAxisFractionBits"}), [])
        self.assertEqual(N2.classify_line("v = flags >> 4;"), [])

    def test_axis_fraction_shift_planted_in_a_tree_grows_the_file(self):
        with tempfile.TemporaryDirectory() as r:
            _w(r, "src/VP-V/a/AnalysisAxis10.h", "constexpr int kAxisFractionBits = 6;\n")
            _w(r, "src/VP-V/a/Use.h", "inline uint32_t F(uint32_t q6) { return q6; }\n")
            self.assertEqual(_run(r, "--write-baseline")[0], 0)
            _w(r, "src/VP-V/a/Use.h", "inline uint32_t F(uint32_t q6) { return q6 >> kAxisFractionBits; }\n")
            rc, out = _run(r)
            self.assertEqual(rc, 1)
            self.assertIn("GREW src/VP-V/a/Use.h", out)

    def test_u8(self):
        self.assertEqual(N2.classify_line("std::vector<uint8_t> pixelBuf;"), ["u8"])
        self.assertEqual(N2.classify_line("std::vector<uint8_t> blob;"), [])

    def test_waiver_tag_needs_row_and_reason(self):
        self.assertEqual(N2.scan_text("a.h", "// PRECISION-OK 119#7: overlay\nx = \"r8\";\n", {}), [])
        self.assertEqual(len(N2.scan_text("a.h", "// PRECISION-OK\nx = \"r8\";\n", {})), 1)

    def test_comments_ignored(self):
        self.assertEqual(N2.scan_text("a.h", "// DXGI_FORMAT_R8_UNORM\n", {}), [])


class RatchetTests(unittest.TestCase):
    def test_class_ratchet_and_new_file(self):
        with tempfile.TemporaryDirectory() as r:
            _w(r, "src/VP-V/a/A.h", "DXGI_FORMAT_R8_UNORM\n")
            self.assertEqual(_run(r, "--write-baseline")[0], 0)
            b = json.load(open(os.path.join(r, "tools/precision-baseline.json")))
            self.assertEqual(b["sites"]["src/VP-V/a/A.h"]["class"], "U")
            self.assertEqual(_run(r)[0], 0)
            _w(r, "src/VP-V/a/B.h", "DXGI_FORMAT_R8_UNORM\n")
            rc, out = _run(r)
            self.assertEqual(rc, 1)
            self.assertIn("GREW src/VP-V/a/B.h", out)
            _w(r, "src/VP-V/a/A.h", "int ok;\n")
            os.remove(os.path.join(r, "src/VP-V/a/B.h"))
            self.assertEqual(_run(r)[0], 0)
            self.assertEqual(_run(r, "--write-baseline")[0], 0)
            b = json.load(open(os.path.join(r, "tools/precision-baseline.json")))
            self.assertEqual(b["total"], 0)

    def test_selftest(self):
        self.assertEqual(N2.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
