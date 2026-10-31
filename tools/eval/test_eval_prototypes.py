# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for the Python reference prototypes (candidates_chroma.py, candidates_mosquito.py), the
harness crop / cache / extra-metric hooks and run_prototypes.py.  Synthetic data only; CPU; the
optical-flow tests skip when OpenCV is missing.
    python -I -m unittest discover -s tools/eval -p "test_*.py" -v
"""
import csv
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import candidates_chroma as cc  # noqa: E402
import candidates_mosquito as cm  # noqa: E402
import eval_candidates as ec  # noqa: E402
import eval_harness as eh  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402
import eval_synth as es  # noqa: E402

try:
    import cv2  # noqa: F401
    HAVE_CV2 = True
except ImportError:
    HAVE_CV2 = False

SPATIAL = [cc.ChromaJBU, cc.ChromaKrig, cc.ChromaCfL]


def frame420(Y, Cb, Cr, siting="left", bitdepth=10, index=0):
    return eio.Frame(np.asarray(Y, float), np.asarray(Cb, float), np.asarray(Cr, float), bitdepth=bitdepth,
                     siting=siting, index=index)


def subsample(full, siting):
    h, w = full.shape
    sy, sx = eio.chroma_down_shifts(2, 2, siting)
    return eio.resample_plane(full, h // 2, w // 2, "lanczos3", sy, sx)


class Siting(unittest.TestCase):
    def test_upsample_matches_harness_shifts(self):
        rng = np.random.default_rng(0)
        c = rng.normal(500, 50, (16, 20))
        for siting in ("left", "center"):
            sy, sx = eio.chroma_up_shifts(2, 2, siting)
            ref = eio.resample_plane(c, 32, 40, "bicubic", sy, sx)
            np.testing.assert_allclose(cc.upsample_cr(c, 32, 40, siting, 2, 2), ref, atol=1e-12)

    def test_site_positions(self):
        self.assertEqual(cc.site_offsets("left", 2, 2), (0.5, 0.0))
        self.assertEqual(cc.site_offsets("center", 2, 2), (0.5, 0.5))
        self.assertEqual(cc.site_offsets("topleft", 2, 2), (0.0, 0.0))

    def test_luma_at_sites_of_a_ramp_is_the_ramp_at_the_site(self):
        x = np.tile(np.arange(64, dtype=float), (32, 1)) * 3.0
        for siting in ("left", "center", "topleft"):
            oy, ox = cc.site_offsets(siting, 2, 2)
            lo = cc.luma_at_sites(x, 16, 32, siting, 2, 2)
            np.testing.assert_allclose(lo[:, 4:-4], (2 * np.arange(32) + ox)[None, 4:-4] * 3.0 + 0 * lo[:, 4:-4], atol=1e-9)


class ChromaSpatial(unittest.TestCase):
    def test_constant_chroma_stays_constant(self):
        rng = np.random.default_rng(1)
        Y = rng.normal(500, 80, (32, 48))
        for siting in ("left", "center", "topleft"):
            f = frame420(Y, np.full((16, 24), 600.0), np.full((16, 24), 420.0), siting)
            for C in SPATIAL:
                o = C().process(f)
                np.testing.assert_allclose(o.Cb, 600.0, atol=1e-6, err_msg=C.__name__ + siting)
                np.testing.assert_allclose(o.Cr, 420.0, atol=1e-6, err_msg=C.__name__ + siting)
                np.testing.assert_array_equal(o.Y, Y)

    def test_linear_ramp_with_flat_luma(self):
        jj = np.tile(np.arange(24, dtype=float), (16, 1))
        Cb = 400 + 7.0 * jj
        f = frame420(np.full((32, 48), 500.0), Cb, Cb.copy(), "left")
        truth = 400 + 7.0 * np.tile(np.arange(48, dtype=float) / 2.0, (32, 1))   # left siting: chroma j at luma 2j
        for C in (cc.ChromaJBU, cc.ChromaCfL):
            o = C().process(f)
            np.testing.assert_allclose(o.Cb[:, 8:-8], truth[:, 8:-8], atol=1e-6, err_msg=C.__name__)
        o = cc.ChromaKrig().process(f)
        self.assertLess(np.abs(o.Cb[:, 8:-8] - truth[:, 8:-8]).max(), 1.0)

    def test_luma_guided_upsamplers_reduce_bleeding_at_a_shared_edge(self):
        xx = np.tile(np.arange(64), (32, 1))
        edge = 33
        Y = np.where(xx >= edge, 800.0, 200.0)
        Cfull = np.where(xx >= edge, 700.0, 350.0)
        f = frame420(Y, subsample(Cfull, "left"), subsample(Cfull, "left"), "left")
        base = ec.load_candidate("chroma_bicubic").process(f)
        e_base = em.psnr(Cfull, base.Cb, 10)
        for C in (cc.ChromaJBU, cc.ChromaKrig, cc.ChromaCfL):
            self.assertGreater(em.psnr(Cfull, C().process(f).Cb, 10), e_base, C.__name__)

    def test_deterministic(self):
        rng = np.random.default_rng(3)
        f = frame420(rng.normal(500, 60, (32, 40)), rng.normal(512, 30, (16, 20)), rng.normal(512, 30, (16, 20)))
        for C in SPATIAL:
            a, b = C().process(f), C().process(f)
            self.assertEqual(a.Cb.tobytes(), b.Cb.tobytes())

    def test_odd_luma_raster(self):
        rng = np.random.default_rng(4)
        f = frame420(rng.normal(500, 60, (31, 41)), rng.normal(512, 30, (16, 21)), rng.normal(512, 30, (16, 21)))
        for C in SPATIAL:
            o = C().process(f)
            self.assertEqual(o.Cb.shape, (31, 41))
            self.assertTrue(np.isfinite(o.Cb).all())


@unittest.skipUnless(HAVE_CV2, "OpenCV not installed")
class ChromaTemporal(unittest.TestCase):
    def clip(self):
        fr = es.tiny_clip(n=4, w=96, h=64, seed=7, bitdepth=10, speed=1.5)
        return [f.with_planes(Cb=subsample(np.kron(f.Cb, np.ones((2, 2))), "left"),
                              Cr=subsample(np.kron(f.Cr, np.ones((2, 2))), "left")) for f in fr]

    def test_without_neighbours_equals_spatial(self):
        f = self.clip()[0]
        mv = cc.ChromaMV()
        mv.reset({})
        o = mv.process(f, ())
        cb, cr = cc.ChromaJBURes().spatial(f)
        np.testing.assert_allclose(o.Cb, cb, atol=1e-9)
        np.testing.assert_allclose(o.Cr, cr, atol=1e-9)

    def test_sequence_runs_deterministic_and_bounded(self):
        fr = self.clip()
        runs = []
        for _ in range(2):
            outs, _ = ec.run_python_candidate(cc.ChromaMV(), fr)
            runs.append(b"".join(o.Cb.tobytes() + o.Cr.tobytes() for o in outs))
            for o, f in zip(outs, fr):
                self.assertEqual(o.Cb.shape, f.Y.shape)
                self.assertGreaterEqual(o.Cb.min(), f.Cb.min() - 1e-6 - 64)
                self.assertLessEqual(o.Cb.max(), f.Cb.max() + 1e-6 + 64)
        self.assertEqual(runs[0], runs[1])

    def test_static_scene_neighbours_are_trusted(self):
        f = self.clip()[1]
        mv = cc.ChromaMV()
        mv.reset({})
        mv.process(f, (f,))
        mv.process(f, (f,))
        self.assertGreater(mv.stats[-1], 0.5)          # identical neighbours: high mean confidence


class Mosquito(unittest.TestCase):
    def edge_clip(self, n=4, ring=30.0, grain=4.0, seed=11):
        rng = np.random.default_rng(seed)
        yy, xx = np.mgrid[0:96, 0:128]
        masters, coded = [], []
        for t in range(n):
            m = np.where(xx >= 64, 700.0, 300.0) + rng.normal(0, grain, xx.shape)
            osc = ring * np.sin(xx * 2.3 + t * 1.9) * np.exp(-np.abs(xx - 63.5) / 3.0) * (np.abs(xx - 63.5) > 1.5)
            c = frame420(m + osc, np.full((48, 64), 512.0), np.full((48, 64), 512.0), index=t)
            masters.append(frame420(m, c.Cb, c.Cr, index=t))
            coded.append(c)
        return masters, coded

    def test_none_is_identity(self):
        _, coded = self.edge_clip(2)
        o = cm.RepairNone().process(coded[0])
        self.assertEqual(o.Y.tobytes(), coded[0].Y.tobytes())

    def test_flat_grain_only_is_untouched(self):
        rng = np.random.default_rng(2)
        f = frame420(500 + rng.normal(0, 5, (64, 80)), np.full((32, 40), 512.0), np.full((32, 40), 512.0))
        for C in (cm.RepairMosquitoMC, cm.RepairMosquitoSpatial):
            c = C()
            c.reset({})
            o = c.process(f, (f,) if c.lookahead else ())
            self.assertEqual(o.Y.tobytes(), f.Y.tobytes(), C.__name__)

    def test_changes_only_inside_zone_and_chroma_untouched(self):
        masters, coded = self.edge_clip()
        for C in (cm.RepairMosquitoMC, cm.RepairMosquitoSpatial):
            outs, _ = ec.run_python_candidate(C(), coded) if HAVE_CV2 or not C.lookahead else (None, None)
            if outs is None:
                continue
            for o, c in zip(outs, coded):
                d = np.abs(o.Y - c.Y)
                self.assertEqual(float(d[:, :48].max()), 0.0)       # far from the edge: bit-exact
                self.assertEqual(float(d[:, 80:].max()), 0.0)
                self.assertEqual(o.Cb.tobytes(), c.Cb.tobytes())

    def test_grain_guard_scales_grain_removal(self):
        rng = np.random.default_rng(5)
        R = rng.normal(0, 4, (64, 64))
        hr = R - __import__("scipy.ndimage", fromlist=["x"]).gaussian_filter(R, em.GRAIN_SIGMA, mode="nearest")
        d = -0.5 * hr                                    # removes half of the grain
        zone = np.ones_like(R, bool)
        g, n = cm.grain_guard(d, R, zone, 32)
        self.assertEqual(n, 4)
        np.testing.assert_allclose(g, d * (0.05 / 0.5), rtol=0.05)
        g2, n2 = cm.grain_guard(np.zeros_like(R), R, zone, 32)
        self.assertEqual(n2, 0)

    @unittest.skipUnless(HAVE_CV2, "OpenCV not installed")
    def test_mc_reduces_flickering_ringing_and_is_deterministic(self):
        masters, coded = self.edge_clip(n=5)
        o1, _ = ec.run_python_candidate(cm.RepairMosquitoMC(), coded)
        o2, _ = ec.run_python_candidate(cm.RepairMosquitoMC(), coded)
        self.assertEqual(b"".join(o.Y.tobytes() for o in o1), b"".join(o.Y.tobytes() for o in o2))
        ex = cm.extra_metrics_mosquito(masters, o1, coded, {}, {})
        self.assertLess(ex["ringing_energy_ratio_Y"], 1.0)
        self.assertGreater(ex["edge_keep_vs_input_Y"], 0.95)

    def test_pixels_beyond_nominal_limits_untouched(self):
        masters, coded = self.edge_clip(n=3)
        hot = [c.with_planes(Y=np.where(c.Y > 690, c.Y + 300.0, c.Y)) for c in coded]   # bright side above 940
        for C in (cm.RepairMosquitoSpatial,) + ((cm.RepairMosquitoMC,) if HAVE_CV2 else ()):
            outs, _ = ec.run_python_candidate(C(), hot)
            for o, c in zip(outs, hot):
                beyond = c.Y > 940
                self.assertTrue(beyond.any())
                np.testing.assert_array_equal(o.Y[beyond], c.Y[beyond])

    def test_extra_metrics_identity(self):
        masters, coded = self.edge_clip(2)
        ex = cm.extra_metrics_mosquito(masters, coded, coded, {}, {})
        self.assertAlmostEqual(ex["ringing_energy_ratio_Y"], 1.0)
        self.assertAlmostEqual(ex["edge_keep_vs_input_Y"], 1.0)


class HarnessHooks(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="vp_eval_proto_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def p(self, n):
        return os.path.join(self.dir, n)

    def test_crop_frame(self):
        f = frame420(np.arange(64 * 96, dtype=float).reshape(64, 96), np.zeros((32, 48)), np.zeros((32, 48)))
        c = eh.crop_frame(f, {"x": 8, "y": 4, "w": 40, "h": 24}, (64, 96))
        self.assertEqual(c.Y.shape, (24, 40))
        self.assertEqual(c.Cb.shape, (12, 20))
        self.assertEqual(c.Y[0, 0], f.Y[4, 8])
        half = frame420(np.zeros((32, 48)), np.zeros((16, 24)), np.zeros((16, 24)))
        h = eh.crop_frame(half, {"x": 8, "y": 4, "w": 40, "h": 24}, (64, 96))
        self.assertEqual(h.Y.shape, (12, 20))
        with self.assertRaises(ValueError):
            eh.crop_frame(f, {"x": 2, "y": 0, "w": 40, "h": 24}, (64, 96))
        with self.assertRaises(ValueError):
            eh.crop_frame(f, {"x": 64, "y": 0, "w": 40, "h": 24}, (64, 96))

    def make_suite(self, protocol="chroma444"):
        fr = es.tiny_clip(n=6, w=128, h=96, seed=21, bitdepth=10, speed=2.0)
        eio.write_raw(self.p("m.yuv"), fr)
        eio.write_raw(self.p("c.yuv"), fr)
        src = lambda n: {"kind": "raw", "path": self.p(n), "width": 128, "height": 96, "pix_fmt": "yuv420p10le", "start": 0}
        suite = {"name": "u", "cases": [{"id": "u1", "content_type": "film24", "clip": "s", "case": "c", "protocol": protocol,
                                         "frames": 6, "gop": 4, "minigop": 2, "crop": {"x": 16, "y": 8, "w": 96, "h": 80},
                                         "master": src("m.yuv"), "coded": src("c.yuv")}]}
        with open(self.p("s.json"), "w") as f:
            json.dump(suite, f)
        return suite

    def test_cache_gives_identical_rows(self):
        suite = self.make_suite()
        base = ec.load_candidate("chroma_bicubic")
        silent = lambda s: None
        plain = eh.evaluate_suite(suite, cc.ChromaCfL(), base, with_cost=False, log=silent, extra=cc.extra_metrics_chroma)
        cache = {}
        eh.evaluate_suite(suite, cc.ChromaJBU(), base, with_cost=False, log=silent, cache=cache, extra=cc.extra_metrics_chroma)
        cached = eh.evaluate_suite(suite, cc.ChromaCfL(), base, with_cost=False, log=silent, cache=cache, extra=cc.extra_metrics_chroma)
        self.assertEqual(plain, cached)
        names = [r["metric"] for r in plain]
        self.assertIn("psnr_edge_Cb", names)
        self.assertIn("flicker_excess_Cr", names)

    def test_run_prototypes_writes_tables(self):
        self.make_suite("same")
        import run_prototypes
        out = self.p("out")
        run_prototypes.main(["--family", "mosquito", "--suite", self.p("s.json"), "--baseline", "repair_none",
                             "--candidates", "mosquito_spatial", "--out", out, "--no-cost"])
        for n in ("mosquito_spatial.csv", "mosquito_spatial.md", "summary.csv", "summary.md", "run_log.txt"):
            self.assertTrue(os.path.exists(os.path.join(out, n)), n)
        with open(os.path.join(out, "mosquito_spatial.csv"), encoding="utf8") as f:
            rows = list(csv.reader(io.StringIO(f.read())))
        by = {r[3]: r for r in rows[1:]}
        self.assertEqual(by["grain_keep_vs_input_Y"][8] in ("PASS", "NA"), True)
        self.assertIn("ringing_energy_ratio_Y", by)


if __name__ == "__main__":
    unittest.main()
