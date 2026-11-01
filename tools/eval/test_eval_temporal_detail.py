# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for the TemporalDetail real-encode candidates (candidates_temporal_detail.py) and td_cli.

The td_cli tests skip when tools/td-cli/bin/td_cli.exe is not built or OpenCV is missing.
  python -I -m unittest discover -s tools/eval -p "test_eval_temporal_detail.py" -v
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "td-cli"))

import eval_io as eio  # noqa: E402
import candidates_temporal_detail as ctd  # noqa: E402

try:
    import cv2  # noqa: F401
    HAVE_CV2 = True
except ImportError:
    HAVE_CV2 = False
HAVE_CLI = os.path.isfile(ctd.TD_CLI)


def clip(n=10, w=96, h=64, bd=10, pulse=True, seed=7):
    """Textured pan with fresh grain; every 4th frame keeps the fine band (I/P-like), the rest lose half (B-like)."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    out = []
    k = 1 << (bd - 8)
    for t in range(n):
        x = xx + 0.75 * t
        fine = 9 * np.sin(2 * np.pi * x / 4.5) + 8 * np.sin(2 * np.pi * (0.7 * x + 0.7 * yy) / 6.1)
        mid = 14 * np.sin(2 * np.pi * x / 11.0) + 12 * np.sin(2 * np.pi * yy / 14.0)
        q = 1.0 if (not pulse or t % 4 == 0) else 0.5
        Y = np.round((110 + mid + q * fine + rng.normal(0, 1.5, (h, w))) * k)
        C = np.full((h // 2, w // 2), 128.0 * k)
        out.append(eio.Frame(Y=Y, Cb=C, Cr=C.copy(), bitdepth=bd, fps=24.0, index=100 + t))
    return out


class ExtraMetrics(unittest.TestCase):
    def test_frame_type_rows_and_identity(self):
        fr = clip(12)
        case = dict(frame_types="IBBBPBBBPBBB")
        m = ctd.extra_metrics_temporal_detail(fr, fr, fr, case, {})
        self.assertEqual(m["psnr_B_Y"], float("inf"))
        self.assertIsNone(m["ib_gap_db_Y"])          # inf - inf is not a gap
        self.assertAlmostEqual(m["detail_pulse_Y"], 0.0, places=12)

    def test_ib_gap_reads_the_pulse(self):
        truth = clip(12, pulse=False, seed=8)
        coded = clip(12, pulse=True)
        notes = {}
        m = ctd.extra_metrics_temporal_detail(truth, coded, coded, dict(frame_types="IBBBPBBBPBBB"), notes)
        self.assertGreater(m["ib_gap_db_Y"], 1.0)
        self.assertGreater(m["detail_pulse_Y"], 0.05)

    def test_letterbox_rows(self):
        fr = clip(4)
        out = [f.with_planes(Y=f.Y.copy()) for f in fr]
        m = ctd.extra_metrics_temporal_detail(fr, out, fr, dict(letterbox=dict(bar_rows=20)), {})
        self.assertEqual(m["lb_bar_change_max_Y"], 0.0)
        self.assertEqual(m["lb_band_overshoot_mean_Y"], 0.0)
        out[1].Y[5, 5] += 3.0                       # a changed bar pixel is seen
        out[2].Y[22, 10] = fr[2].Y[18:27, 5:16].min() - 50.0   # an undershoot at the boundary is seen
        m = ctd.extra_metrics_temporal_detail(fr, out, fr, dict(letterbox=dict(bar_rows=20)), {})
        self.assertEqual(m["lb_bar_change_max_Y"], 3.0)
        self.assertGreater(m["lb_band_undershoot_max_Y"], 40.0)

    def test_missing_frame_types_is_na_with_reason(self):
        fr = clip(4)
        notes = {}
        m = ctd.extra_metrics_temporal_detail(fr, fr, fr, {}, notes)
        self.assertIsNone(m["psnr_B_Y"])
        self.assertIn("frame_types", notes["psnr_B_Y"])
        self.assertIsNone(m["grain_keep_vs_input_band1_Y"])  # < 8 frames


@unittest.skipUnless(HAVE_CLI and HAVE_CV2, "td_cli.exe not built or OpenCV missing")
class Cli(unittest.TestCase):
    def run_cand(self, cand, frames):
        cand.reset(frames[0].meta())
        outs = [cand.process(f) for f in frames]
        cand.reset({})
        return outs

    def test_off_is_bit_identical(self):
        fr = clip(6)
        outs = self.run_cand(ctd.CANDIDATES["td_off_model"](), fr)
        for a, b in zip(fr, outs):
            self.assertTrue(np.array_equal(a.Y, b.Y))

    def test_steady_is_deterministic_and_luma_only(self):
        fr = clip(10)
        a = self.run_cand(ctd.CANDIDATES["td_b008"](), fr)
        b = self.run_cand(ctd.CANDIDATES["td_b008"](), fr)
        for x, y, f in zip(a, b, fr):
            self.assertTrue(np.array_equal(x.Y, y.Y))
            self.assertTrue(np.array_equal(x.Cb, f.Cb))

    def test_file_mode_matches_stdio(self):
        fr = clip(8)
        with tempfile.TemporaryDirectory() as d:
            yuv = os.path.join(d, "in.yuv")
            with open(yuv, "wb") as f:
                for x in fr:
                    for p in (x.Y, x.Cb, x.Cr):
                        f.write(np.round(p).astype("<u2").tobytes())
            tdv = os.path.join(d, "v.tdv")
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "td-cli", "td_flow.py"), "--in", yuv,
                            "--w", "96", "--h", "64", "--pix-fmt", "yuv420p10le", "--out", tdv], check=True,
                           capture_output=True)
            out = os.path.join(d, "cap")
            os.makedirs(out)
            subprocess.run([ctd.TD_CLI, "--in", yuv, "--w", "96", "--h", "64", "--pix-fmt", "yuv420p10le", "--vectors",
                            tdv, "--out", out, "--hz", "24", "--lines", "1080", "--start-index", "100"], check=True,
                           capture_output=True)
            cap = __import__("eval_candidates").CaptureCandidate(out)
            self.assertEqual(cap.indices(), [100 + i for i in range(8)])
            st = self.run_cand(ctd.CANDIDATES["td_b008"](), fr)
            for i, s in enumerate(st):
                f, _ = cap.frame(100 + i)
                self.assertTrue(np.array_equal(f.Y, np.clip(np.floor(s.Y + 0.5), 0, 1023)))


if __name__ == "__main__":
    unittest.main()
