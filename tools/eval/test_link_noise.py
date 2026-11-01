# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import link_noise as ln  # noqa: E402

B = 16
H = W = 128


def noise(seed, rms, shape=(2, H, W)):
    return np.random.default_rng(seed).normal(0.0, rms, shape)


class LinkNoiseTest(unittest.TestCase):
    def test_identical_is_one(self):
        base = noise(1, 1.0)
        r = ln.link_noise_core(base, base.copy(), None, np.full((H // B, W // B), 0.1), B)
        self.assertAlmostEqual(r["low"], 1.0, places=12)
        self.assertIsNone(r["high"])

    def test_added_white_noise_analytic(self):
        base = noise(2, 1.0)
        add = noise(3, 0.8)
        r = ln.link_noise_core(base + add, base, None, np.full((H // B, W // B), 0.1), B)
        self.assertAlmostEqual(r["low"], np.sqrt(1.0 + 0.64), delta=0.02)   # high-pass scales both equally
        self.assertEqual(r["n_low"], 2 * H * W)

    def test_class_split(self):
        base = noise(4, 1.0)
        add = noise(5, 1.0)
        so = np.zeros((H // B, W // B))
        so[:, W // B // 2:] = 0.9                      # right half: high class
        left = np.zeros((H, W), bool)
        left[:, :W // 2] = True
        cand = base + add * left[None]
        r = ln.link_noise_core(cand, base, None, so, B)
        self.assertAlmostEqual(r["high"], 1.0, places=3)   # the 3 px high-pass leaks a little across the class border
        self.assertGreater(r["low"], 1.3)
        self.assertEqual(r["n_low"], r["n_high"])
        v = ln.verdict(r)
        self.assertEqual((v["low"], v["high"], v["ok"]), (r["low"] <= 1.40, True, r["low"] <= 1.40))

    def test_shadow_mask_and_empty_class(self):
        base = noise(6, 1.0)
        cand = base + noise(7, 1.0)
        shadow = np.zeros(base.shape, bool)
        shadow[:, :32, :] = True
        r = ln.link_noise_core(cand, base, shadow, np.full((H // B, W // B), 0.5), B)
        self.assertIsNone(r["low"])
        self.assertEqual(r["n_high"], 2 * 32 * W)
        self.assertGreater(r["high"], 1.2)
        none = ln.link_noise_core(cand, base, np.zeros(base.shape, bool), np.full((H // B, W // B), 0.5), B)
        self.assertIsNone(none["low"])
        self.assertIsNone(none["high"])
        self.assertFalse(ln.verdict(none)["ok"])

    def test_verdict_limits(self):
        v = ln.verdict({"low": 1.40, "high": 1.06})
        self.assertTrue(v["low"])
        self.assertFalse(v["high"])
        self.assertFalse(v["ok"])
        self.assertTrue(ln.verdict({"low": None, "high": 1.0})["ok"])


if __name__ == "__main__":
    unittest.main()
