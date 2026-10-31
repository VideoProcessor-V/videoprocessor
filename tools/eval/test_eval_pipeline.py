# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Pipeline tests (readers, resamplers, candidates, protocols, report, CLI determinism).
Synthetic data only; no GPU, no corpus.
    python -I -m unittest discover -s tools/eval -p "test_*.py" -v
"""
import csv
import io
import json
import math
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import eval_candidates as ec  # noqa: E402
import eval_harness as eh  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_material as emat  # noqa: E402
import eval_metrics as em  # noqa: E402
import eval_protocols as ep  # noqa: E402
import eval_report as er  # noqa: E402
import eval_synth as es  # noqa: E402


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="vp_eval_test_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def p(self, name):
        return os.path.join(self.dir, name)


class Readers(Base):
    def test_raw_10bit_round_trip(self):
        fr = es.tiny_clip(n=3, w=32, h=24, bitdepth=10)
        eio.write_raw(self.p("a.yuv"), fr)
        self.assertEqual(os.path.getsize(self.p("a.yuv")), 3 * eio.frame_bytes(32, 24, "yuv420p10le"))
        back = eio.read_raw_frames(self.p("a.yuv"), 32, 24, "yuv420p10le", start=1, count=2)
        self.assertEqual(len(back), 2)
        for a, b in zip(fr[1:], back):
            self.assertTrue(np.array_equal(a.Y, b.Y) and np.array_equal(a.Cb, b.Cb) and np.array_equal(a.Cr, b.Cr))
            self.assertEqual(b.bitdepth, 10)
        self.assertEqual([b.index for b in back], [1, 2])
        self.assertEqual(eio.raw_frame_count(self.p("a.yuv"), 32, 24, "yuv420p10le"), 3)

    def test_y4m_round_trip_8_and_10_bit(self):
        for bd, tag in ((8, "C420"), (10, "C420p10")):
            fr = es.tiny_clip(n=3, w=32, h=24, bitdepth=bd)
            eio.write_y4m(self.p("c%d.y4m" % bd), fr, 30000, 1001)
            with open(self.p("c%d.y4m" % bd), "rb") as f:
                hdr = f.readline()
            self.assertIn(tag.encode(), hdr)
            d = eio.parse_y4m_header(hdr)
            self.assertEqual((d["width"], d["height"]), (32, 24))
            self.assertAlmostEqual(d["fps"], 30000 / 1001)
            self.assertEqual(eio.y4m_info(self.p("c%d.y4m" % bd))["frames"], 3)
            back = eio.read_y4m_frames(self.p("c%d.y4m" % bd), start=1, count=2)
            for a, b in zip(fr[1:], back):
                self.assertTrue(np.array_equal(a.Y, b.Y) and np.array_equal(a.Cr, b.Cr))
                self.assertEqual(b.bitdepth, bd)

    def test_y4m_header_tags(self):
        d = eio.parse_y4m_header(b"YUV4MPEG2 W1920 H800 F24:1 Ip A2144:2145 C420mpeg2 XYSCSS=420MPEG2\n")
        self.assertEqual(d["pix_fmt"], "yuv420p")
        d = eio.parse_y4m_header("YUV4MPEG2 W3840 H2160 F50:1 Ip A1:1")
        self.assertEqual(d["pix_fmt"], "yuv420p")

    def test_ffmpeg_pipe_window(self):
        if not os.path.exists(eio.DEFAULT_FFMPEG):
            self.skipTest("ffmpeg not installed")
        fr = es.tiny_clip(n=6, w=64, h=48)
        eio.write_y4m(self.p("v.y4m"), fr, 24, 1)
        got = eio.ffmpeg_decode_window(self.p("v.y4m"), 2, 3, 64, 48, "yuv420p", 24.0)
        self.assertEqual(len(got), 3)
        # the pipe decode of a y4m must equal the direct reader (lossless path)
        for g, f in zip(got, fr[2:5]):
            self.assertTrue(np.array_equal(g.Y, f.Y))
        self.assertEqual([g.index for g in got], [2, 3, 4])


class Resample(unittest.TestCase):
    def test_constant_and_identity(self):
        c = np.full((20, 30), 77.0)
        for k in ("bicubic", "mitchell", "lanczos3"):
            self.assertTrue(np.allclose(eio.resample_plane(c, 40, 60, k), 77.0))
            self.assertTrue(np.allclose(eio.resample_plane(c, 10, 15, k), 77.0))
        a = np.random.default_rng(1).random((12, 16))
        self.assertTrue(np.array_equal(eio.resample_plane(a, 12, 16, "lanczos3"), a))

    def test_linear_ramp_preserved_by_catmull_rom(self):
        x = np.tile(np.arange(32, dtype=np.float64), (8, 1))
        up = eio.resample_plane(x, 8, 64, "bicubic")
        centres = (np.arange(64) + 0.5) / 2 - 0.5
        self.assertTrue(np.allclose(up[:, 4:-4], np.tile(centres[4:-4], (8, 1)), atol=1e-9))

    def test_chroma_siting_down_up_round_trip(self):
        yy, xx = np.mgrid[0:64, 0:96].astype(np.float64)
        full = 100 + 50 * np.sin(xx / 30.0) + 20 * np.cos(yy / 25.0)
        sy, sx = eio.chroma_down_shifts(2, 2, "left")
        small = eio.resample_plane(full, 32, 48, "lanczos3", sy, sx)
        sy, sx = eio.chroma_up_shifts(2, 2, "left")
        back = eio.resample_plane(small, 64, 96, "lanczos3", sy, sx)
        self.assertLess(np.abs(back - full)[8:-8, 8:-8].max(), 0.5)
        # a wrong siting would be visibly worse on a horizontal gradient
        sy, sx = eio.chroma_up_shifts(2, 2, "center")
        wrong = eio.resample_plane(small, 64, 96, "lanczos3", sy, sx)
        self.assertGreater(np.abs(wrong - full)[8:-8, 8:-8].max(), np.abs(back - full)[8:-8, 8:-8].max())

    def test_deterministic(self):
        a = np.random.default_rng(3).random((30, 40))
        self.assertEqual(eio.resample_plane(a, 15, 20, "lanczos3").tobytes(), eio.resample_plane(a, 15, 20, "lanczos3").tobytes())


class Candidates(Base):
    def test_builtins_upsample_chroma(self):
        f = es.tiny_clip(n=1, w=64, h=48)[0]
        small = f.with_planes(Cb=f.Cb[::2, ::2], Cr=f.Cr[::2, ::2])
        for name in ("chroma_bicubic", "chroma_lanczos3"):
            c = ec.load_candidate(name)
            o = c.process(small)
            self.assertEqual(o.Cb.shape, o.Y.shape)
            self.assertTrue(np.array_equal(o.Y, small.Y))
        self.assertNotEqual(ec.load_candidate("chroma_bicubic").process(small).Cb.tobytes(),
                            ec.load_candidate("chroma_lanczos3").process(small).Cb.tobytes())
        with self.assertRaises(KeyError):
            ec.load_candidate("nope")

    def test_lookahead_buffering_and_state(self):
        seen = []

        class Peek(ec.PythonCandidate):
            name = "peek"
            lookahead = 2

            def reset(self, meta):
                seen.append(("reset", meta["width"]))

            def process(self, frame, future=()):
                seen.append((frame.index, [x.index for x in future]))
                return frame

        fr = es.tiny_clip(n=4, w=32, h=24)
        outs, ms = ec.run_python_candidate(Peek(), fr)
        self.assertEqual(len(outs), 4)
        self.assertEqual(len(ms), 4)
        self.assertEqual(seen, [("reset", 32), (0, [1, 2]), (1, [2, 3]), (2, [3]), (3, [])])

    def test_capture_candidate_loader(self):
        fr = es.tiny_clip(n=3, w=32, h=24, bitdepth=10)
        frames = []
        for i, f in enumerate(fr):
            name = "%06d.yuv" % i
            with open(self.p(name), "wb") as fh:
                fh.write(eio.encode_planar(f))
            frames.append({"file": name, "source_frame_index": 20 + i, "gpu_ms": 1.5 + i})
        meta = {"schema": 1, "producer": "unit-test", "vp_commit": "abc", "config_hash": "def", "width": 32, "height": 24,
                "pix_fmt": "yuv420p10le", "bitdepth": 10, "range": "limited", "matrix": "709", "transfer": "sdr",
                "fps": 24.0, "frames": frames}
        with open(self.p("capture.json"), "w") as fh:
            json.dump(meta, fh)
        cap = ec.load_candidate("capture:" + self.dir)
        self.assertEqual(cap.indices(), [20, 21, 22])
        f1, g = cap.frame(21)
        self.assertEqual(g, 2.5)
        self.assertEqual(f1.index, 21)
        self.assertTrue(np.array_equal(f1.Y, fr[1].Y))
        self.assertEqual(cap.kind, "capture")


class Protocols(unittest.TestCase):
    def master(self, w=64, h=48, bd=10, seed=4):
        f = es.tiny_clip(n=1, w=w, h=h, seed=seed, bitdepth=bd)[0]
        return f

    def test_chroma444_geometry(self):
        m = self.master()
        inp, truth = ep.chroma444_pair(m, m)
        self.assertEqual(truth.Y.shape, (24, 32))
        self.assertEqual(truth.Cb.shape, (24, 32))
        self.assertTrue(np.array_equal(truth.Cb, m.Cb))
        self.assertEqual(inp.Y.shape, (24, 32))
        self.assertEqual(inp.Cb.shape, (12, 16))
        # genuine 4:4:4 truth: a candidate's output geometry matches it
        out = ec.load_candidate("chroma_lanczos3").process(inp)
        self.assertEqual(out.Cb.shape, truth.Cb.shape)
        self.assertTrue(math.isfinite(em.psnr(truth.Cb, out.Cb, 10)))

    def test_chroma444_odd_height_and_half_rung(self):
        m = self.master(w=64, h=50)
        inp, truth = ep.chroma444_pair(m, m)
        self.assertEqual(truth.Cb.shape, (25, 32))
        self.assertEqual(inp.Cb.shape, (13, 16))
        # coded picture already at the half raster (a 1080p rung of a 4K master): used as the input as is
        half = truth.with_planes(Cb=truth.Cb[::2, ::2][:12], Cr=truth.Cr[::2, ::2][:12], Y=truth.Y)
        half = half.with_planes(Y=truth.Y[:24], Cb=truth.Cb[:24:2, ::2], Cr=truth.Cr[:24:2, ::2])
        m2 = self.master(w=64, h=48)
        i2, t2 = ep.chroma444_pair(m2, half.with_planes(Y=eio.resample_plane(m2.Y, 24, 32, "lanczos3")))
        self.assertEqual(i2.Cb.shape, (12, 16))
        self.assertEqual(t2.Y.shape, (24, 32))

    def test_bit_depth_matched_to_coded(self):
        m = self.master(bd=8)
        coded = eio.to_bitdepth(m, 10)
        inp, truth = ep.chroma444_pair(m, coded)
        self.assertEqual(truth.bitdepth, 10)
        self.assertLessEqual(truth.Y.max(), 1023.0)
        self.assertGreater(truth.Y.max(), 255.0)

    def test_same_protocol_and_mismatch(self):
        m = self.master()
        a, t = ep.same_pair(m, m)
        self.assertEqual(a.Y.shape, t.Y.shape)
        with self.assertRaises(ValueError):
            ep.same_pair(m, self.master(w=32, h=24))

    def test_verify_alignment_finds_offset(self):
        clip = es.tiny_clip(n=8, w=64, h=48, seed=9, speed=4.0)
        r = ep.verify_alignment(clip[5], clip, 10)
        self.assertEqual(r["best_offset"], 15)


class Report(unittest.TestCase):
    def rows(self):
        base = dict(content_type="film24", clip="c", case="k")
        return [dict(base, metric="psnr_Y", candidate="inf", default="inf"),
                dict(base, metric="psnr_Cb", candidate=40.0, default=40.5),
                dict(base, metric="lpips", candidate=None, default=None),
                dict(base, metric="grain_rms_ratio_Y", candidate=0.5, default=0.5),
                dict(base, metric="btb_kept_Y", candidate=None, default=None),
                dict(base, metric="clip_detect_Y", candidate=True, default=False),
                dict(base, metric="cost_ms_per_frame_pyref", candidate=5.0, default=4.0),
                dict(content_type="sports50_60", clip="d", case="k", metric="ssim_Y", candidate=0.9, default=0.9)]

    def test_csv_columns_and_na_explicit(self):
        scored = er.score_rows(self.rows())
        rd = list(csv.reader(io.StringIO(er.to_csv(scored))))
        self.assertEqual(rd[0], er.COLUMNS)
        by = {r[3]: r for r in rd[1:]}
        self.assertEqual(by["psnr_Y"][4:9], ["inf", "inf", "0", "same", "PASS"])
        self.assertEqual(by["psnr_Cb"][8], "FLAG")               # 0.5 dB worse than the default
        self.assertEqual(by["lpips"][4], "NA")
        self.assertEqual(by["lpips"][8], "NA")
        self.assertEqual(by["grain_rms_ratio_Y"][8], "FAIL")
        self.assertIn("default also fails", by["grain_rms_ratio_Y"][9])
        self.assertEqual(by["btb_kept_Y"][8], "NA")
        self.assertEqual(by["clip_detect_Y"][8], "FAIL")
        self.assertEqual(by["cost_ms_per_frame_pyref"][8], "INFO")
        self.assertEqual(by["cost_ms_per_frame_pyref"][7], "worse")

    def test_markdown_summary_per_content_type(self):
        md = er.to_markdown(er.score_rows(self.rows()), "cand", "base")
        self.assertIn("### film24", md)
        self.assertIn("### sports50_60", md)
        self.assertIn("# VP-V evaluation harness: cand vs base (default)", md)
        self.assertIn("| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |", md)
        self.assertIn("NA", md)


class MaterialRules(unittest.TestCase):
    def test_content_type_rules(self):
        self.assertEqual(emat._ctypes("tos", 24.0), ["film24"])
        self.assertEqual(emat._ctypes("tos", 23.976), ["film24"])
        self.assertEqual(emat._ctypes("meridian", 29.97), ["nature30"])
        self.assertEqual(emat._ctypes("x", 59.94), ["sports50_60"])
        self.assertEqual(emat._ctypes("tos_dark_2stops", 23.976), ["dark", "film24"])
        self.assertEqual(emat._ctypes("x", None), [])

    def test_committed_catalog_parses(self):
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "material_catalog.json")
        if not os.path.exists(p):
            self.skipTest("catalog not generated yet")
        with open(p, encoding="utf8") as f:
            c = json.load(f)
        self.assertIn("entries", c)
        self.assertTrue(all("content_types" in e for e in c["entries"]))

    def test_sports_hook_is_not_implemented(self):
        with self.assertRaises(NotImplementedError):
            es.generate_sports_clip({})


class EndToEnd(Base):
    def make_suite(self):
        fr = es.tiny_clip(n=10, w=128, h=96, seed=21, bitdepth=10, speed=3.0)
        rng = np.random.default_rng(5)
        coded = [f.with_planes(Y=np.clip(f.Y + np.rint(rng.normal(0, 4, f.Y.shape)), 0, 1023),
                               Cb=np.clip(f.Cb + np.rint(rng.normal(0, 4, f.Cb.shape)), 0, 1023),
                               Cr=np.clip(f.Cr + np.rint(rng.normal(0, 4, f.Cr.shape)), 0, 1023)) for f in fr]
        eio.write_raw(self.p("master.yuv"), fr)
        eio.write_raw(self.p("coded.yuv"), coded)
        suite = {"name": "unit", "cases": [{
            "content_type": "film24", "clip": "synthetic", "case": "disc", "protocol": "chroma444", "frames": 10,
            "gop": 4, "minigop": 2,
            "master": {"kind": "raw", "path": self.p("master.yuv"), "width": 128, "height": 96, "pix_fmt": "yuv420p10le", "start": 0},
            "coded": {"kind": "raw", "path": self.p("coded.yuv"), "width": 128, "height": 96, "pix_fmt": "yuv420p10le", "start": 0}}]}
        with open(self.p("suite.json"), "w") as f:
            json.dump(suite, f)
        return self.p("suite.json")

    def run_once(self, out, *extra):
        eh.main(["run", "--suite", self.make_suite_path, "--candidate", "chroma_lanczos3", "--baseline", "chroma_bicubic",
                 "--out", out, "--no-cost"] + list(extra))
        with open(os.path.join(out, "results.csv"), "rb") as f:
            c = f.read()
        with open(os.path.join(out, "results.md"), "rb") as f:
            m = f.read()
        return c, m

    def test_run_is_deterministic_and_reports_na(self):
        self.make_suite_path = self.make_suite()
        c1, m1 = self.run_once(self.p("o1"))
        c2, m2 = self.run_once(self.p("o2"))
        self.assertEqual(c1, c2)
        self.assertEqual(m1, m2)
        rows = list(csv.reader(io.StringIO(c1.decode())))
        by = {r[3]: r for r in rows[1:]}
        self.assertEqual(rows[0], er.COLUMNS)
        self.assertNotEqual(by["psnr_Cb"][4], "NA")
        self.assertEqual(by["lpips"][4], "NA")                  # not requested -> explicit NA, never 0
        self.assertIn("grain_hf_ratio_Y", by)
        self.assertFalse(any(r[3].startswith("cost_") for r in rows[1:]))

    def test_temporal_metrics_are_na_with_few_frames(self):
        self.make_suite_path = self.make_suite()
        c, _ = self.run_once(self.p("o3"), "--max-frames", "3")
        by = {r[3]: r for r in list(csv.reader(io.StringIO(c.decode())))[1:]}
        self.assertEqual(by["grain_hf_ratio_Y"][4], "NA")
        self.assertEqual(by["gop_ratio_gop"][4], "NA")
        self.assertNotEqual(by["psnr_Y"][4], "NA")


if __name__ == "__main__":
    unittest.main()
