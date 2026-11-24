# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Asserts the numpy port reproduces sigma_delta_vectors.json (shared with the C++ test
SigmaDeltaHookTests::CrossLanguageFixedVectors) exactly."""
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402

import sigma_delta_ref as s  # noqa: E402

VECTORS = os.path.join(HERE, "sigma_delta_vectors.json")


def load():
    with open(VECTORS, encoding="utf-8") as f:
        return json.load(f)


class SigmaDeltaVectors(unittest.TestCase):
    def test_steps(self):
        v = load()
        for c in v["cases"]:
            lim = s.limits(c["bits"], c["full"])
            self.assertEqual((c["min"], c["max"]), lim)
            r = np.float64(0.0)
            for t in range(v["steps"]):
                d = s.dither(np.float32(c["px"]), np.float32(c["py"]), t, c["amplitude"])
                k, r = s.quantise_step(c["x"], r, d, lim, c["amplitude"])
                self.assertEqual(int(k), c["k"][t])
                self.assertEqual(float(r), float.fromhex(c["r"][t]))

    def test_ign(self):
        for p in load()["ign"]:
            got = s.ign(np.float32(p["px"]), np.float32(p["py"]), p["frame"])
            self.assertEqual(got.dtype, np.float32)
            self.assertEqual(float(got), float.fromhex(p["value"]))

    def test_sequence_matches_steps(self):
        c = load()["cases"][0]
        frames = np.full((16, 1, 4), c["x"])
        out = s.quantise_sequence(frames, c["bits"], c["full"], c["amplitude"])
        self.assertEqual(out.shape, (16, 1, 4))
        # column 3 is pixel (3, 0)
        r = np.float64(0.0)
        lim = s.limits(c["bits"], c["full"])
        for t in range(16):
            k, r = s.quantise_step(c["x"], r, s.dither(np.float32(3), np.float32(0), t, c["amplitude"]), lim, c["amplitude"])
            self.assertEqual(int(out[t, 0, 3]), int(k))

    def test_round(self):
        out = s.quantise_round(np.array([[0.2, 1.4, 254.6, 300.0]]), 8, False)
        self.assertEqual(out.tolist(), [[1, 1, 254, 254]])

    def test_q9b_above_black_never_dips_btb_and_true_btb_preserved(self):
        # Owner Q9b (SD SHOULD-1): limited-range above-black pixels in [black, black+0.5)
        # never dip below reference black (16 at 8-bit, 64 at 10-bit), while true BTB
        # pixels (< black) remain untouched down to timing-code floor (ADR-0002).
        for bits, black, x_above, x_btb, px, py in [
            (8, 16, 16.05, 12.2, 3, 5),
            (10, 64, 64.2, 48.2, 7, 11),
        ]:
            lim = s.limits(bits, False)
            r_prot = np.float64(0.0)
            r_naive = np.float64(0.0)
            naive_dips = 0
            prot_above = 0
            for t in range(32):
                d = s.dither(np.float32(px), np.float32(py), t, 0.5)
                k_prot, r_prot = s.quantise_step(x_above, r_prot, d, lim, 0.5)
                self.assertGreaterEqual(int(k_prot), black)
                if int(k_prot) > black:
                    prot_above += 1
                # Negative control: naive timing-code-only clamp on exact same input dips into BTB.
                k_naive = min(max(np.floor(x_above + r_naive + d + 0.5), lim[0]), lim[1])
                r_naive = min(max(x_above + r_naive - k_naive, -1.0), 1.0)
                if int(k_naive) < black:
                    naive_dips += 1
            self.assertGreater(prot_above, 0, "above-black pixel must still dither upward")
            self.assertGreater(naive_dips, 0, "negative control: naive timing-code clamp must dip into BTB")

            # True BTB pixel remains preserved below reference black (ADR-0002).
            r_btb = np.float64(0.0)
            btb_codes = []
            for t in range(16):
                d = s.dither(np.float32(px), np.float32(py), t, 0.5)
                k_btb, r_btb = s.quantise_step(x_btb, r_btb, d, lim, 0.5)
                btb_codes.append(int(k_btb))
            self.assertTrue(all(int(lim[0]) <= c < black for c in btb_codes))


if __name__ == "__main__":
    unittest.main()
