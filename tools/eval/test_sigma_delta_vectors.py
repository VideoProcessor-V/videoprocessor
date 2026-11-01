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


if __name__ == "__main__":
    unittest.main()
