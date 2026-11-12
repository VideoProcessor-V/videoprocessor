# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("check_motion_bypass", os.path.join(HERE, "check_motion_bypass.py"))
cmb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cmb)

SAD_KERNEL = """\
float best = 1e9;
for (int dy = -4; dy <= 4; ++dy)
    for (int dx = -4; dx <= 4; ++dx) {
        float s = 0.0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                s += abs(cur_luma[y * 16 + x] - prev_luma[(y + dy) * 20 + (x + dx)]);
        best = min(best, s);
    }
"""

# Files of the doc 85 rows marked "not motion" (spatial / hash / timing / shared products): none may hit.
NOT_MOTION_NAMES = ("PulldownDetector", "HalfLuma", "LaneGpuLightKernel", "DitherNeed", "Subtitle", "ChromaCfl",
                    "BasicChroma", "RenderAhead", "RepeatReuse", "AlphaCadence", "InterpolationTimeline",
                    "GrainCameraRefine", "RegionEvidence", "SceneDetector", "LedMeasurement", "SourceResolutionDetector",
                    "ActivePictureEvidence", "FieldRefineRef", "CameraGuidanceGpu")


def _w(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def _run(root):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = cmb.main(["--root", root])
    return rc, buf.getvalue()


class CheckMotionBypassTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name

    def tearDown(self):
        self.td.cleanup()

    def test_clean_on_the_current_tree(self):
        rc, out = _run(ROOT)
        self.assertEqual(rc, 0, out)
        self.assertIn("0 new", out)

    def test_red_a_new_private_sad_search_outside_motion_fails(self):
        _w(self.root, "src/VideoProcessor-Lib/vprenderer/PrivateSearch.glsl", SAD_KERNEL)
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("NEW sad src/VideoProcessor-Lib/vprenderer/PrivateSearch.glsl", out)

    def test_red_other_private_estimators_fail(self):
        cases = {
            "src/VP-V/modules/repair/a/Nv.cpp": "auto s = new NvOpticalFlow::Engine(dev);\n",
            "src/VP-V/modules/repair/a/Nv2.cpp": "NvOFAPICreateInstance(NV_OF_API_VERSION, &api);\n",
            "src/VP-V/modules/repair/a/Hint.cpp": "auto e = std::make_unique<HintedEngine>(dev);\n",
            "src/VP-V/modules/repair/a/Bm.h": "double SearchSadBlock(const float* a, const float* b);\n",
            "src/VP-V/modules/repair/a/Bm2.h": "int BlockMatchPyramid(int levels);\n",
            "src/VP-V/modules/repair/a/Pc.cpp": "auto peak = PhaseCorrelate(a, b);\n",
            "src/VP-V/modules/repair/a/Fft.cpp": "auto cross_power = X * conj(Y);\n",
            "shaders/PrivateSad.hlsl": "float sad(float2 v) { return 0; }\n",
        }
        for rel, text in cases.items():
            with self.subTest(rel=rel):
                with tempfile.TemporaryDirectory() as td:
                    _w(td, rel, text)
                    rc, out = _run(td)
                    self.assertEqual(rc, 1, out)

    def test_an_engine_constant_is_not_a_session_but_an_engine_still_is(self):
        # Coordinator rule 9b (2026-11-08): NvOpticalFlow::Engine::Grid (a static constant) is not an NVOF session; the red cases stay red.
        _w(self.root, "src/VP-V/modules/repair/a/Const.h", "const int g = NvOpticalFlow::Engine::Grid;\nint h = NvOpticalFlow::Engine :: Grid;\n")
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        for text in ("NvOpticalFlow::Engine engine;\n", "std::unique_ptr<NvOpticalFlow::Engine> e;\n", "NvOpticalFlow::HintedEngine h(dev);\n"):
            with self.subTest(text=text):
                with tempfile.TemporaryDirectory() as td:
                    _w(td, "src/VP-V/modules/repair/a/Red.cpp", text)
                    rc, out = _run(td)
                    self.assertEqual(rc, 1, out)

    def test_inside_motion_comments_tests_and_spatial_code_are_not_hits(self):
        _w(self.root, "src/VP-V/modules/motion/estimation/Own.glsl", SAD_KERNEL)
        _w(self.root, "src/VP-V/modules/repair/a/Prose.cpp", "// a BestShift or PhaseCorrelate in a comment\n/* NvOFInit(\n SearchSad */\n")
        _w(self.root, "src/VP-V/modules/repair/a/tests/T.cpp", SAD_KERNEL)
        _w(self.root, "src/VP-V/modules/repair/a/Spatial.cpp", "texture += std::abs(value - previous);\ndifference += std::abs(cells[i] - previousCells[i]);\n")
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)

    def test_an_allow_listed_file_cannot_grow_a_second_search(self):
        entry = cmb.ALLOW[0]
        lines = "\n".join("double v%d = BestShift(a, b, 3);" % i for i in range(entry["max_hits"] + 1)) + "\n"
        _w(self.root, entry["file"], lines)
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("over the allowed ceiling", out)

    def test_allow_list_entries_are_documented_and_the_not_motion_rows_never_hit(self):
        self.assertEqual(len(cmb.ALLOW), 1)
        for e in cmb.ALLOW:
            for key in ("file", "patterns", "max_hits", "row", "reason"):
                self.assertTrue(e.get(key), (e, key))
            self.assertTrue(os.path.isfile(os.path.join(ROOT, *e["file"].split("/"))), e["file"])
            self.assertGreater(len(e["reason"]), 40)
        hits = cmb.scan(ROOT)
        for h in hits:
            self.assertFalse(any(n in h["file"] for n in NOT_MOTION_NAMES), h)
        new, used, stale = cmb.evaluate(hits)
        self.assertEqual(new, [])
        self.assertEqual(stale, [], "an allow-list entry without a hit can be deleted")


if __name__ == "__main__":
    unittest.main()
