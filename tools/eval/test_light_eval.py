# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for light_level_ref, light_metrics, sigma_delta_ref vectors, s2_synth and stack_runner."""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import light_level_ref as llr  # noqa: E402
import light_metrics as lm  # noqa: E402
import s2_synth  # noqa: E402
import sigma_delta_ref as s  # noqa: E402
import stack_runner  # noqa: E402
import tonemap_ref as tr  # noqa: E402


class LightLevelRef(unittest.TestCase):
    def test_hdr_to_sdr_matches_spline(self):
        ctx = llr.Context(kind="hdr_to_sdr", target_white=100.0, target_black=0.0, src_peak=1000.0)
        lut = llr.build_lut(ctx)
        sp = tr.SplineTM(1000.0, dst_peak=100.0, src_min=0.0, dst_min=0.0)
        for c in (64, 100, 200, 400, 600, 800, 940):
            v = (c - 64.0) / 876.0
            l_out = float(tr.pq_eotf(sp(v)))
            want = 64.0 + 876.0 * (l_out / 100.0) ** (1 / 2.4)
            self.assertAlmostEqual(float(lut["sdr_code"][c]), want, places=9)
            self.assertAlmostEqual(float(lut["J"][c]), float(tr.pq_oetf(l_out)), delta=0.01)  # BT.1886 gamma != 2.4 power only via black

    def test_btb_zero_and_monotone(self):
        for kind in ("sdr", "hdr_passthrough", "hdr_to_sdr"):
            lut = llr.build_lut(llr.Context(kind=kind))
            self.assertTrue(np.all(lut["J"][:64] == 0.0))
            self.assertTrue(np.all(np.diff(lut["J"][64:941]) >= -1e-12))

    def test_fetch_and_tile(self):
        lut = llr.build_lut(llr.Context())
        f = llr.fetch(lut, np.array([100.0, 100.5]))
        self.assertAlmostEqual(float(f["J"][1]), 0.5 * (lut["J"][100] + lut["J"][101]))
        self.assertEqual(llr.tile_size_for(2160), 32)

    def test_context_roundtrip(self):
        c = llr.Context(kind="hdr_to_sdr", src_peak=4000.0)
        self.assertEqual(llr.Context.from_dict(c.to_dict()), c)


def ramp(h, w, lo, hi):
    return np.tile(np.linspace(lo, hi, w), (h, 1))


class LightMetrics(unittest.TestCase):
    CTX = llr.Context(kind="sdr", target_white=100.0, target_black=0.05)

    def test_banding_steps_shadow_vs_mids(self):
        # smooth ramp with coarse 8 px steps (a step of 4 codes) in the shadow half only
        h, w = 96, 400
        base = ramp(h, w, 64.0, 600.0)
        banded = base.copy()
        shadow = slice(0, 150)
        banded[:, shadow] = 64.0 + np.floor((base[:, shadow] - 64.0) / 12.0) * 12.0 + 6.0
        ctx = self.CTX
        lut = llr.build_lut(ctx)
        r = lm.shadow_banding(base, banded, ctx, {"dark": (None, 3.0), "mid": (20.0, 80.0)}, lut, tile=8)
        ident = lm.shadow_banding(base, base, ctx, {"dark": (None, 3.0), "mid": (20.0, 80.0)}, lut, tile=8)
        self.assertTrue(r["dark"]["value_ok"])
        self.assertGreater(r["dark"]["band_mse"], 0.0)
        self.assertEqual(ident["dark"]["band_mse"], 0.0)
        if r["mid"]["value_ok"]:
            self.assertLess(r["mid"]["band_mse"], r["dark"]["band_mse"] * 1e-3 + 1e-12)

    def test_banding_na_when_empty(self):
        flat = np.full((64, 64), 900.0)
        r = lm.shadow_banding(flat, flat, self.CTX, {"lt5": (None, 5.0)})
        self.assertIsNone(r["lt5"]["band_mse"])
        self.assertTrue(r["lt5"]["na_reason"])

    def test_texture_kept_identity_and_blur(self):
        rng = np.random.default_rng(7)
        h, w = 160, 160
        m = 90.0 + 3.0 * rng.standard_normal((h, w))
        plain = m.copy()
        r = lm.shadow_texture_kept(m, plain, m, self.CTX, None, 5.0, tile=8)
        self.assertIsNone(r["na_reason"])
        self.assertAlmostEqual(r["ratio"], 1.0, places=9)
        self.assertTrue(r["pass"])
        blur = ndimage.gaussian_filter(m, 1.2)
        rb = lm.shadow_texture_kept(m, plain, blur, self.CTX, None, 5.0, tile=8)
        self.assertLess(rb["ratio"], 0.9)
        self.assertFalse(rb["pass"])


class FinalStepMetrics(unittest.TestCase):
    CTX = stack_runner.SDR_EVAL

    def test_btb_survives_both_finals(self):
        rng = np.random.default_rng(3)
        master = np.stack([np.where(rng.random((48, 48)) < 0.5, 56.0, 120.0)] * 2)  # BTB at -8 ten-bit codes
        for final in ("nd", "sd"):
            out = stack_runner.finalise(master, final)
            kept, n = lm.btb_kept(master, out)
            self.assertGreater(n, 0)
            self.assertEqual(kept, 1.0, final)
        # within half an output step of black is not counted
        self.assertEqual(lm.btb_kept(np.full((4, 4), 63.0), np.full((4, 4), 64.0))[1], 0)

    def test_identical_candidate_and_plain(self):
        rng = np.random.default_rng(11)
        master = np.stack([100.0 + 14.0 * rng.standard_normal((96, 96)) for _ in range(2)])
        lut = llr.build_lut(self.CTX)
        for final in ("nd", "sd"):
            plain = stack_runner.finalise(master, final)
            r = stack_runner.evaluate(master, plain, plain.copy(), self.CTX, lut)
            self.assertAlmostEqual(r["grain_rms_ratio"], 1.0, places=9, msg=final)
            tex = [r[k] for k in ("tex_lt5", "tex_5to50") if r[k] is not None]
            self.assertTrue(tex, r["na"])
            for t in tex:
                self.assertAlmostEqual(t, 1.0, places=9)
            self.assertIsNotNone(r["grain_ratio_master"])


class SigmaDeltaVectors(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(HERE, "sigma_delta_vectors.json"), encoding="utf-8") as f:
            self.v = json.load(f)

    def test_steps(self):
        for c in self.v["cases"]:
            lim = s.limits(c["bits"], c["full"])
            self.assertEqual((c["min"], c["max"]), lim)
            r = np.float64(0.0)
            for t in range(self.v["steps"]):
                d = s.dither(np.float32(c["px"]), np.float32(c["py"]), t, c["amplitude"])
                k, r = s.quantise_step(c["x"], r, d, lim, c["amplitude"])
                self.assertEqual(int(k), c["k"][t])
                self.assertEqual(float(r), float.fromhex(c["r"][t]))

    def test_ign(self):
        for p in self.v["ign"]:
            got = s.ign(np.float32(p["px"]), np.float32(p["py"]), p["frame"])
            self.assertEqual(got.dtype, np.float32)
            self.assertEqual(float(got), float.fromhex(p["value"]))


class S2Synth(unittest.TestCase):
    SPEC = dict(context="sdr", shape="radial", grain_codes=1.5, seed=20260203, size_frac=0.05, frames=2)

    def test_deterministic(self):
        a, b = s2_synth.make_case(self.SPEC), s2_synth.make_case(self.SPEC)
        for k in ("master", "coded"):
            for fa, fb in zip(a[k], b[k]):
                self.assertTrue(np.array_equal(fa.Y, fb.Y))
        self.assertFalse(np.array_equal(a["master"][0].Y, a["master"][1].Y))  # new grain field per frame

    def test_coded_grid(self):
        c = s2_synth.make_case(self.SPEC)
        self.assertTrue(np.all(c["coded"][0].Y % 4 == 0))
        p = dict(self.SPEC, context="hdr_pq_1000")
        self.assertTrue(np.all(s2_synth.make_case(p)["coded"][0].Y == np.rint(s2_synth.make_case(p)["coded"][0].Y)))

    def test_suite_matches_file(self):
        with open(os.path.join(HERE, "suites", "s2_shadow_ramps.json"), encoding="utf8") as f:
            self.assertEqual(json.load(f), json.loads(json.dumps(s2_synth.suite_dict())))


class RunnerSmoke(unittest.TestCase):
    def test_tiny(self):
        with tempfile.TemporaryDirectory(prefix="light-level-map-") as d:
            rows = stack_runner.main(["--suite", "s2", "--out", d, "--size-frac", "0.06", "--max-cases", "1",
                                      "--contexts", "sdr"])
            self.assertTrue(rows)
            for f in ("stack_s2.csv", "stack_s2.md", "run_log.txt"):
                self.assertTrue(os.path.getsize(os.path.join(d, f)) > 0)
            self.assertEqual({r["final"] for r in rows}, {"nd", "sd"})


if __name__ == "__main__":
    unittest.main()
