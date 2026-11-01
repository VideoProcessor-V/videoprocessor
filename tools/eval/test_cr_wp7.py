# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for cr_wp7_model (contrast recovery model + new WP7 metrics).  python -I -m unittest test_cr_wp7 (from tools/eval)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402

import cr_wp7_model as cw  # noqa: E402
import tonemap_ref as tr  # noqa: E402


def textured(h=96, w=128, level=0.7, amp=0.02, seed=3):
    rng = np.random.default_rng(seed)
    base = level + amp * np.sin(np.arange(w)[None, :] * 0.5) * np.sin(np.arange(h)[:, None] * 0.4)
    return tr.norm_to_code(np.clip(base, 0, 1)) + 0.0 * rng.random((h, w))


class Model(unittest.TestCase):
    def test_zero_strength_is_plain_curve(self):
        f = textured()
        plain = tr.ToneMapper("global", 1000.0, 720).process(f)
        for mode in ("A", "B"):
            m = cw.ContrastRecovery(mode, 1000.0, 720, params=cw.CRParams(s_max=0.0))
            np.testing.assert_allclose(m.process(f), plain, atol=1e-9)

    def test_restores_but_never_boosts_above_source(self):
        f = textured(level=0.75, amp=0.03)
        plain = tr.sdr_to_perceptual_codes(tr.ToneMapper("global", 1000.0, 720).process(f))
        src = f
        for mode in ("A", "B"):
            m = cw.ContrastRecovery(mode, 1000.0, 720, params=cw.CRParams(s_max=1.0, use_band=False, use_halo=False,
                                                                       use_light=False, rate_limit=1.0, ema=1.0))
            out = tr.sdr_to_perceptual_codes(m.process(f))
            sd = lambda a: float(np.std(a[20:-20, 20:-20]))
            self.assertGreater(sd(out), sd(plain) * 1.05, mode)       # recovered
            self.assertLessEqual(sd(out), sd(src) * 1.10, mode)       # not above the source

    def test_light_gate_zero_in_deep_shadow(self):
        f = textured(level=0.02, amp=0.01)          # PQ 0.02 -> far below the 0.3 nit floor after tone mapping
        m = cw.ContrastRecovery("A", 1000.0, 720)
        m.process(f)
        self.assertLess(float(m.last_strength.max()), 1e-6)

    def test_deterministic(self):
        f = textured()
        a = cw.ContrastRecovery("A", 1000.0, 720).process(f)
        b = cw.ContrastRecovery("A", 1000.0, 720).process(f)
        np.testing.assert_array_equal(a, b)

    def test_strength_reset_at_cut(self):
        m = cw.ContrastRecovery("A", 1000.0, 720)
        m.process(textured(level=0.8, amp=0.03))
        s1 = m.last_strength.mean()
        self.assertGreater(s1, 0.01)
        m.process(tr.norm_to_code(np.full((96, 128), 0.01)))    # cut to a dark flat frame: strength falls at once
        self.assertLess(m.last_strength.mean(), s1 * 0.2)


class Metrics(unittest.TestCase):
    def test_zone_grain_keep(self):
        rng = np.random.default_rng(1)
        dp = rng.standard_normal((4, 32, 32))
        dc = dp * 1.04
        edge = [np.zeros((32, 32), bool) for _ in range(4)]
        for e in edge:
            e[:8] = True
        r = cw.zone_grain_keep(dp, dc, dp, edge)
        self.assertAlmostEqual(r["keep_all"], 1.04, places=6)
        self.assertAlmostEqual(r["keep_edge"], 1.04, places=6)
        dc2 = dp.copy()
        dc2[:, :8] *= 0.5                      # grain halved near the edge zone only
        r2 = cw.zone_grain_keep(dp, dc2, dp, edge)
        self.assertAlmostEqual(r2["keep_edge"], 0.5, places=6)
        self.assertLess(r2["keep_all"], 0.95)
        e0 = [np.zeros((32, 32), bool)] * 4
        self.assertIsNone(cw.zone_grain_keep(dp, dc, dp, e0)["keep_edge"])

    def test_step_visibility_ratio(self):
        self.assertAlmostEqual(cw.step_visibility_ratio(1.03, 1.0), 1.03)
        self.assertIsNone(cw.step_visibility_ratio(None, 1.0))
        self.assertIsNone(cw.step_visibility_ratio(1.0, 0.0))

    def test_acceptance(self):
        cr = dict(restore=0.13, boost=0.02, halo=0.00016, keep_all=1.08, keep_edge=1.1, pump=0.1, vis_ratio=None)
        good = dict(restore=0.12, boost=0.01, halo=0.0001, keep_all=1.02, keep_edge=0.97, pump=0.2, vis_ratio=1.01)
        a = cw.cr_acceptance(good, cr)
        self.assertTrue(a["all"], a)
        for k, v in (("restore", 0.10), ("keep_all", 1.06), ("keep_edge", 0.85), ("halo", 0.001), ("pump", 0.6),
                     ("vis_ratio", 1.03), ("boost", 0.05)):
            bad = dict(good)
            bad[k] = v
            a = cw.cr_acceptance(bad, cr)
            self.assertFalse(a["all"], k)
        nopump = dict(good, pump=None)
        self.assertTrue(cw.cr_acceptance(nopump, cr)["all"])         # unknown criteria do not fail


if __name__ == "__main__":
    unittest.main()
