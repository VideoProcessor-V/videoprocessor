# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Metric unit tests (synthetic data only, no GPU, no corpus):
    python -I -m unittest discover -s tools/eval -p "test_*.py" -v
"""
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import eval_metrics as em  # noqa: E402
import eval_synth as es  # noqa: E402


def noise_clip(t=10, h=96, w=128, seed=7, sigma=3.0, mean=120.0):
    rng = np.random.default_rng(seed)
    return np.clip(mean + sigma * rng.standard_normal((t, h, w)), 0, 255)


def textured_clip(t=10, seed=3):
    """moving structure + grain, so motion masks are non-empty"""
    return np.stack([f.Y for f in es.tiny_clip(n=t, w=128, h=96, seed=seed, speed=4.0)])


class PsnrSsim(unittest.TestCase):
    def test_psnr_identical_is_inf(self):
        a = noise_clip(1)[0]
        self.assertEqual(em.psnr(a, a, 8), math.inf)

    def test_psnr_known_noise_formula(self):
        a = np.full((64, 64), 100.0)
        b = a + 5.0                                   # mse = 25
        self.assertAlmostEqual(em.psnr(a, b, 8), 10 * math.log10(255.0 ** 2 / 25.0), places=9)
        self.assertAlmostEqual(em.psnr(a * 4, b * 4 - 15, 10), 10 * math.log10(1023.0 ** 2 / 25.0), places=9)

    def test_ssim_identical_and_decreasing_with_blur(self):
        a = textured_clip(1)[0]
        self.assertAlmostEqual(em.ssim(a, a, 8), 1.0, places=12)
        s1 = em.ssim(a, ndimage.gaussian_filter(a, 0.8), 8)
        s2 = em.ssim(a, ndimage.gaussian_filter(a, 2.0), 8)
        self.assertLess(s1, 1.0)
        self.assertLess(s2, s1)


class Grain(unittest.TestCase):
    def setUp(self):
        self.ref = noise_clip(12)
        self.tiles = em.select_flat_tiles(self.ref, 8)

    def test_tiles_found_and_reference_derived(self):
        self.assertGreater(len(self.tiles), 0)
        self.assertEqual(self.tiles, em.select_flat_tiles(self.ref, 8))

    def test_identity_passes(self):
        r, ok = em.grain_rms_ratio(self.ref, self.ref, self.tiles)
        self.assertAlmostEqual(r, 1.0, places=12)
        self.assertTrue(ok)

    def test_spatial_blur_fails(self):
        out = np.stack([ndimage.uniform_filter(f, 3) for f in self.ref])
        r, ok = em.grain_rms_ratio(self.ref, out, self.tiles)
        self.assertLess(r, 0.95)
        self.assertFalse(ok)

    def test_temporal_average_fails_and_raises_lag1(self):
        out = self.ref.copy()
        for t in range(1, len(self.ref) - 1):
            out[t] = (self.ref[t - 1] + self.ref[t] + self.ref[t + 1]) / 3.0
        r, ok = em.grain_rms_ratio(self.ref, out, self.tiles)
        self.assertFalse(ok)
        l1 = em.grain_lag1(self.ref, out, self.tiles)
        self.assertGreater(l1["delta"], em.GRAIN_LAG1_TOL)
        self.assertTrue(l1["flag"])
        self.assertLess(em.grain_lag1(self.ref, self.ref, self.tiles)["delta"], 1e-12)

    def test_temporal_spectrum_needs_eight_frames_and_drops_when_averaged(self):
        self.assertIsNone(em.grain_temporal_spectrum(self.ref[:5], self.ref[:5], self.tiles))
        out = self.ref.copy()
        for t in range(1, len(self.ref) - 1):
            out[t] = (self.ref[t - 1] + self.ref[t] + self.ref[t + 1]) / 3.0
        sp = em.grain_temporal_spectrum(self.ref, out, self.tiles)
        self.assertLess(sp["ratio"], 0.9)
        self.assertAlmostEqual(em.grain_temporal_spectrum(self.ref, self.ref, self.tiles)["ratio"], 1.0, places=9)

    def test_no_flat_tiles_is_na(self):
        r, ok = em.grain_rms_ratio(self.ref, self.ref, [])
        self.assertIsNone(r)
        self.assertIsNone(ok)


class Temporal(unittest.TestCase):
    def test_flicker_rises_with_frame_offsets(self):
        ref = noise_clip(10, sigma=1.0)
        out = ref + (np.arange(10) % 2 * 6.0)[:, None, None]
        self.assertAlmostEqual(em.flicker_excess(ref, ref, 8), 0.0, places=12)
        self.assertGreater(em.flicker_excess(ref, out, 8), 5.0)
        self.assertIsNone(em.flicker_excess(ref[:2], out[:2], 8))

    def test_gop_spectrum_peaks_at_metadata_period(self):
        gop = 8                                              # as if from GOP metadata
        t = 4 * gop
        ref = noise_clip(t, sigma=4.0, seed=11)
        gain = 1.0 + 0.5 * np.cos(2 * np.pi * np.arange(t) / gop)   # grain energy pumps with the GOP
        mean = ref.mean(axis=(1, 2), keepdims=True)
        out = mean + (ref - mean) * gain[:, None, None]
        g = em.gop_spectrum(ref, out, gop, minigop=5)
        self.assertGreater(g["gop_ratio"], 3.0)
        # the amplitude at the injected period dominates an unrelated period (1/5)
        eo = em.tile_energy_series(out)
        eo = eo / eo.mean(axis=0, keepdims=True)
        a_gop = em.dft_amplitude(eo, 1.0 / gop).mean()
        a_other = em.dft_amplitude(eo, 1.0 / 5).mean()
        self.assertGreater(a_gop, 3 * a_other)
        self.assertIsNone(em.gop_spectrum(ref[:10], out[:10], gop))      # too short -> NA

    def test_ghost_coeff_recovers_alpha(self):
        ref = textured_clip(8)
        a = 0.3
        out = ref.copy()
        out[1:] = (1 - a) * ref[1:] + a * ref[:-1]
        g = em.ghost_coeff(ref, out, 8)
        self.assertAlmostEqual(g, a, delta=0.05)
        self.assertAlmostEqual(em.ghost_coeff(ref, ref, 8), 0.0, places=12)

    def test_duplicate_detected(self):
        ref = textured_clip(6)
        out = ref.copy()
        out[3] = out[2]
        self.assertAlmostEqual(em.duplicate_rate(ref, out, 8), 1.0 / 5.0, places=9)
        self.assertEqual(em.duplicate_rate(ref, ref, 8), 0.0)

    def test_smear_below_one_on_motion_blur(self):
        ref = textured_clip(6)
        out = np.stack([ndimage.uniform_filter1d(f, 7, axis=1) for f in ref])
        self.assertLess(em.smear(ref, out, 8), 1.0)
        self.assertAlmostEqual(em.smear(ref, ref, 8), 1.0, places=9)


class HaloToneMap(unittest.TestCase):
    def setUp(self):
        self.ref = np.stack([f.Y for f in es.tiny_clip(n=2, w=160, h=96, seed=5, grain=0.5)])

    def test_overshoot_zero_for_identity_positive_for_unsharp(self):
        self.assertEqual(em.halo_overshoot(self.ref, self.ref, 8), 0.0)
        blur = np.stack([ndimage.gaussian_filter(f, 2.0) for f in self.ref])
        sharp = self.ref + 2.5 * (self.ref - blur)
        self.assertGreater(em.halo_overshoot(self.ref, sharp, 8), 0.001)

    def test_contrast_boost_flagged(self):
        lc = em.local_contrast_ratio(self.ref, self.ref, 8)
        self.assertAlmostEqual(lc["ratio_s2"], 1.0, places=9)
        self.assertEqual(lc["boost_fraction"], 0.0)
        m = self.ref.mean()
        boosted = m + 1.5 * (self.ref - m)
        lc = em.local_contrast_ratio(self.ref, boosted, 8)
        self.assertGreater(lc["ratio_s2"], 1.3)
        self.assertGreater(lc["boost_fraction"], 0.5)


class BtbWtw(unittest.TestCase):
    def test_identity_kept_and_clipping_detected(self):
        rng = np.random.default_rng(1)
        ref = np.clip(rng.normal(120, 60, (64, 64)), 0, 255).round()
        self.assertTrue((ref < 16).any() and (ref > 235).any())
        r = em.btb_wtw(ref, ref, 8)
        self.assertEqual((r["btb_kept"], r["wtw_kept"], r["clip_detect"]), (1.0, 1.0, False))
        c = em.btb_wtw(ref, np.clip(ref, 16, 235), 8)
        self.assertEqual((c["btb_kept"], c["wtw_kept"], c["clip_detect"]), (0.0, 0.0, True))

    def test_na_without_excursions_and_full_range(self):
        ref = np.full((8, 8), 100.0)
        r = em.btb_wtw(ref, ref, 8)
        self.assertIsNone(r["btb_kept"])
        self.assertIsNone(r["wtw_kept"])
        self.assertIsNone(em.btb_wtw(ref, ref, 8, rng="full"))

    def test_bit_depth_scaling_and_chroma_limits(self):
        self.assertEqual(em.nominal_limits(10), (64.0, 940.0))
        self.assertEqual(em.nominal_limits(10, "chroma"), (64.0, 960.0))


class Lpips(unittest.TestCase):
    def test_lpips_none_or_sane(self):
        try:
            import torch  # noqa: F401
            import lpips  # noqa: F401
        except Exception:
            self.skipTest("torch/lpips not installed (NA column in reports)")
        rng = np.random.default_rng(2)
        a = np.clip(rng.random((3, 192, 192)) * 0.5 + 0.25, 0, 1)
        v0 = em.lpips_score(a, a)
        if v0 is None:
            self.skipTest("lpips weights unavailable offline")
        self.assertAlmostEqual(v0, 0.0, places=6)
        self.assertGreater(em.lpips_score(a, np.clip(a + rng.normal(0, 0.1, a.shape), 0, 1)), 0.0)


class Determinism(unittest.TestCase):
    def test_tiles_derive_from_size(self):
        self.assertEqual(em.tile_size((1080, 1920)), 64)
        self.assertEqual(em.tile_size((2160, 3840)), 128)
        t = em.lpips_tiles((1080, 1920))
        self.assertEqual(len(t), 9)
        self.assertTrue(all(y + s <= 1080 and x + s <= 1920 for y, x, s in t))


if __name__ == "__main__":
    unittest.main()
