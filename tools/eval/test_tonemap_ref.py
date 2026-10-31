# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for tonemap_ref (round-2 tone-mapping reference) and the artefact_headroom metrics."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import artefact_headroom as ah  # noqa: E402
import tonemap_ref as tr  # noqa: E402


def textured_frame(level_nits, amp_codes=6.0, h=96, w=128, seed=3):
    base = float(tr.norm_to_code(tr.pq_oetf(level_nits)))
    rng = np.random.default_rng(seed)
    tex = ndimage.gaussian_filter(rng.standard_normal((h, w)), 2.0)
    tex *= amp_codes / tex.std()
    return base + tex


class SplineTests(unittest.TestCase):
    def test_pq_roundtrip(self):
        for L in (0.01, 1.0, 100.0, 1000.0, 4000.0):
            self.assertAlmostEqual(float(tr.pq_eotf(tr.pq_oetf(L))), L, delta=L * 1e-9 + 1e-9)

    def test_spline_monotone_continuous_endpoints(self):
        for pk in (600.0, 1000.0, 4000.0):
            tm = tr.SplineTM(pk, 100.0)
            x = np.linspace(tm.in_min, tm.in_max, 4001)
            y = tm(x)
            self.assertTrue(np.all(np.diff(y) > 0))
            self.assertAlmostEqual(float(y[0]), tm.out_min, places=9)
            self.assertAlmostEqual(float(y[-1]), tm.out_max, places=9)
            # continuity of value and slope at the pivot
            d = 1e-7
            self.assertAlmostEqual(float(tm(tm.sp - d)), float(tm(tm.sp + d)), places=6)
            self.assertAlmostEqual(float(tm.slope(tm.sp - d)), float(tm.slope(tm.sp + d)), places=4)
            # numerical slope matches the analytic one
            xs = np.linspace(tm.in_min + 0.01, tm.in_max - 0.01, 50)
            num = (tm(xs + 1e-6) - tm(xs - 1e-6)) / 2e-6
            np.testing.assert_allclose(num, tm.slope(xs), rtol=1e-4, atol=1e-6)

    def test_identity_when_no_compression(self):
        tm = tr.SplineTM(80.0, 100.0)
        self.assertTrue(tm.identity)


class LtmTests(unittest.TestCase):
    def test_ltm_restores_but_never_boosts(self):
        f = textured_frame(600.0)
        src = f - ndimage.gaussian_filter(f, 4)
        outs = {}
        for k in ("global", "ltm", "ltm_nograin"):
            m = tr.ToneMapper(k, 1000.0, 96)
            p = tr.sdr_to_perceptual_codes(m.process(f))
            outs[k] = (p - ndimage.gaussian_filter(p, 4)).std()
        s = src.std()
        self.assertLess(outs["global"], 0.8 * s)            # the global curve compresses highlights
        self.assertGreater(outs["ltm"], outs["global"] * 1.1)
        self.assertLessEqual(outs["ltm"], s * 1.02)         # never beyond the source (PQ) local contrast
        self.assertLessEqual(outs["ltm_nograin"], s * 1.02)

    def test_strength_zero_is_global(self):
        f = textured_frame(300.0)
        g = tr.ToneMapper("global", 1000.0, 96).process(f)
        z = tr.ToneMapper("ltm", 1000.0, 96, strength=0.0).process(f)
        np.testing.assert_allclose(z, g, atol=0.05)  # only the grain split's 2nd-order term differs

    def test_excursions_preserved_and_deterministic(self):
        f = textured_frame(50.0)
        f[:10, :10] = 20.0
        f[-10:, -10:] = 1010.0
        for k in ("global", "cr", "ltm", "ltm_mc"):
            a = tr.ToneMapper(k, 1000.0, 96).process(f)
            b = tr.ToneMapper(k, 1000.0, 96).process(f)
            np.testing.assert_array_equal(a, b)
            self.assertTrue(np.all(a[:10, :10] < 64.0), k)
            self.assertTrue(np.all(a[-10:, -10:] > 940.0), k)
            self.assertTrue(np.all((a >= 4.0) & (a <= 1019.0)), k)  # legal 10-bit signal range

    def test_mc_resets_on_cut(self):
        f1, f2 = textured_frame(5.0), textured_frame(800.0, seed=9)
        m = tr.ToneMapper("ltm_mc", 1000.0, 96)
        ident = lambda pS, S, pB: (pS, pB)
        m.process(f1)
        m.process(f1, ident)
        self.assertEqual(m.cuts, 0)
        m.process(f2, ident)
        self.assertEqual(m.cuts, 1)


class VectorSourceTests(unittest.TestCase):
    def test_external_fields_strict_and_upsampled(self):
        import tempfile
        import vector_source as vsrc
        with tempfile.TemporaryDirectory() as d:
            F = np.zeros((8, 16, 2), np.float32)
            F[..., 0] = 1.5
            F[..., 1] = -0.5
            np.save(os.path.join(d, "flow_000003_000002.npy"), F)
            a8 = np.zeros((16, 32), np.uint8)
            vs = vsrc.VectorSource(d)
            G = vs.flow(3, 2, a8, a8)
            self.assertEqual(G.shape, (16, 32, 2))
            np.testing.assert_allclose(G[..., 0], 3.0)   # half-size field: upsampled and scaled x2
            np.testing.assert_allclose(G[..., 1], -1.0)
            with self.assertRaises(FileNotFoundError):
                vs.flow(4, 3, a8, a8)
            self.assertEqual(vs.stats()["external_pairs"], 1)


class HeadroomMetricTests(unittest.TestCase):
    def test_banding_metrics_see_quantisation(self):
        x = np.linspace(200, 260, 512)[None, :].repeat(128, 0)
        q = np.rint(x / 8.0) * 8.0  # coarse staircase on a smooth ramp
        s = ah.seq_metrics(x[None], q[None], ah.dark_code_for("sdr"))
        self.assertGreater(s["false_flat"], 0.3)
        self.assertGreater(s["band_share"], 0.5)
        z = ah.seq_metrics(x[None], x[None], ah.dark_code_for("sdr"))
        self.assertEqual(z["false_flat"], 0.0)

    def test_blockiness_sees_grid_steps(self):
        rng = np.random.default_rng(1)
        r = 100.0 + ndimage.gaussian_filter(rng.standard_normal((128, 128)), 1.0)
        off = np.kron(rng.integers(-3, 4, (16, 16)), np.ones((8, 8)))
        s = ah.seq_metrics(r[None], (r + off)[None], ah.dark_code_for("sdr"))
        self.assertGreater(s["blockiness"], 1.0)
        self.assertGreater(s["block_share"], 0.2)


if __name__ == "__main__":
    unittest.main()
