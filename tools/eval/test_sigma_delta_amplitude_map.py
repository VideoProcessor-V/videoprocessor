# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""sigma_delta_ref accepts an amplitude MAP (per pixel / per frame) besides a scalar; scalar results are unchanged."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402

import sigma_delta_ref as s  # noqa: E402


class AmplitudeMap(unittest.TestCase):
    def setUp(self):
        rng = np.random.RandomState(7)
        self.frames = 100.0 + rng.rand(12, 6, 9) * 3.0

    def test_scalar_equals_constant_array(self):
        a = 0.35
        base = s.quantise_sequence(self.frames, 8, False, a)
        per_pixel = s.quantise_sequence(self.frames, 8, False, np.full((6, 9), a))
        per_frame = s.quantise_sequence(self.frames, 8, False, np.full((12, 1, 1), a))
        full = s.quantise_sequence(self.frames, 8, False, np.full(self.frames.shape, a))
        for o in (per_pixel, per_frame, full):
            np.testing.assert_array_equal(base, o)
        r0 = s.quantise_sequence(self.frames, 8, False, a, return_residual=True)[1]
        r1 = s.quantise_sequence(self.frames, 8, False, np.full((6, 9), a), return_residual=True)[1]
        np.testing.assert_array_equal(r0, r1)

    def test_scalar_types_unchanged(self):
        self.assertIsInstance(s.residual_bound(0.25), float)
        self.assertEqual(s.residual_bound(0.25), 0.75)
        self.assertEqual(s.residual_bound(-1.0), 0.5)
        self.assertEqual(s.residual_bound(np.float64(0.25)), 0.75)

    def test_residual_bound_elementwise(self):
        b = s.residual_bound(np.array([-0.2, 0.0, 0.3, 0.5]))
        np.testing.assert_array_equal(b, np.array([0.5, 0.5, 0.8, 1.0]))

    def test_per_pixel_map_differs_where_amplitude_differs(self):
        amp = np.zeros((6, 9))
        amp[:, 5:] = 0.5
        out = s.quantise_sequence(self.frames, 8, False, amp)
        lo = s.quantise_sequence(self.frames, 8, False, 0.0)
        hi = s.quantise_sequence(self.frames, 8, False, 0.5)
        np.testing.assert_array_equal(out[..., :5], lo[..., :5])
        np.testing.assert_array_equal(out[..., 5:], hi[..., 5:])
        self.assertTrue(np.any(lo != hi))

    def test_dither_scales_with_map(self):
        px = np.arange(4.0, dtype=np.float32)
        d1 = s.dither(px, np.zeros(4, np.float32), 3, 0.5)
        d2 = s.dither(px, np.zeros(4, np.float32), 3, np.array([0.5, 0.25, 0.0, 0.5]))
        np.testing.assert_array_equal(d2[[0, 3]], d1[[0, 3]])
        self.assertEqual(d2[1], d1[1] * 0.5)
        self.assertEqual(d2[2], 0.0)

    def test_quantise_step_array_bound(self):
        lim = s.limits(8, False)
        k, r = s.quantise_step(np.array([0.0, 0.0]), np.zeros(2), np.zeros(2), lim, np.array([0.0, 0.5]))
        self.assertEqual(k.tolist(), [1.0, 1.0])
        self.assertEqual(r.tolist(), [-0.5, -1.0])


if __name__ == "__main__":
    unittest.main()
