# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for the streaming-realistic evaluation suite: presets / allocation / switch plans (pure), the
no-reference metrics on synthetic pictures, the recordings scanner (read-only), the validation helpers and
the harness hook.  Synthetic data only; no ffmpeg encodes, no GPU.
    python -I -m unittest discover -s tools/eval -p "test_streaming*.py" -v
"""
import csv
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import eval_harness as eh  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_synth as es  # noqa: E402
import make_streaming_encodes as mse  # noqa: E402
import nr_metrics as nr  # noqa: E402
import nr_validate as nv  # noqa: E402
import scan_recordings as sr  # noqa: E402
import streaming_presets as sp  # noqa: E402


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="vp_streaming_test_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def p(self, name):
        return os.path.join(self.dir, name)


# ------------------------------------------------------------------ presets and rules
class Allocation(unittest.TestCase):
    def test_capped_average_and_bounds(self):
        d = [3.0, 2.0, 4.0, 1.0, 2.5]
        c = [1000.0, 4000.0, 400.0, 9000.0, 1500.0]
        r = sp.allocate_shot_kbps(d, c, 4500)
        self.assertLessEqual(sp.weighted_mean(d, r), 4500 + 1e-9)
        self.assertGreater(sp.weighted_mean(d, r), 4500 * 0.97)
        for v in r:
            self.assertTrue(0.5 * 4500 <= v <= 1.5 * 4500)
        self.assertTrue(all(isinstance(v, int) for v in r))

    def test_hard_shots_are_starved_by_the_cap(self):
        # a shot 10x as complex as its neighbours would need >> cap; it gets exactly the cap, not its share
        r = sp.allocate_shot_kbps([2.0, 2.0, 2.0], [1000.0, 1000.0, 10000.0], 3000)
        self.assertEqual(r[2], int(1.5 * 3000))
        self.assertLess(r[0], 3000)

    def test_monotone_and_deterministic(self):
        d = [1.0] * 6
        c = [100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0]
        r = sp.allocate_shot_kbps(d, c, 6000)
        self.assertEqual(r, sorted(r))
        self.assertEqual(r, sp.allocate_shot_kbps(d, c, 6000))

    def test_uniform_complexity_gives_the_average(self):
        r = sp.allocate_shot_kbps([2.0, 5.0], [700.0, 700.0], 3000)
        self.assertEqual(r, [3000, 3000])

    def test_bad_input_raises(self):
        with self.assertRaises(ValueError):
            sp.allocate_shot_kbps([1.0, 2.0], [1.0], 3000)
        self.assertEqual(sp.allocate_shot_kbps([], [], 3000), [])


class Ladders(unittest.TestCase):
    def test_never_upscales_and_even_dimensions(self):
        r = sp.ladder_rungs("uhd", 1920, 800, 24.0)
        self.assertEqual([x["w"] for x in r], [1920])
        self.assertTrue(all(x["h"] % 2 == 0 for x in sp.ladder_rungs("hd", 3840, 1714, 24.0)))
        self.assertEqual(sp.ladder_rungs("hd", 640, 360, 24.0), [])

    def test_general_to_any_master_size(self):
        for w, h in ((7680, 4320), (4096, 2160), (2048, 858), (1280, 720)):
            for x in sp.ladder_rungs("hd", w, h, 24.0):
                self.assertLessEqual(x["w"], w)
                self.assertGreater(x["kbps"], 0)

    def test_bitrate_scales_with_area_and_rate(self):
        base = sp.rung_kbps(4500, 1920, 1920, 1080, 24.0)
        self.assertEqual(base, 4500)
        scope = sp.rung_kbps(4500, 1920, 1920, 800, 24.0)
        self.assertAlmostEqual(scope / 4500.0, 800 / 1080.0, places=2)
        self.assertGreater(sp.rung_kbps(4500, 1920, 1920, 1080, 60.0), 4500 * 1.35)
        self.assertEqual(sp.rung_kbps(4500, 1920, 1920, 1080, 30.0), 4500)

    def test_ladders_are_ordered_top_down(self):
        for name, rungs in sp.LADDERS.items():
            self.assertEqual([k for _, _, k in rungs], sorted([k for _, _, k in rungs], reverse=True), name)


class Shots(unittest.TestCase):
    LOG = ("[Parsed_showinfo_2 @ 0x1] n:   0 pts:  372 pts_time:3.1 pos: 1\n"
           "[Parsed_showinfo_2 @ 0x1] n:   1 pts:  600 pts_time:5.0 pos: 2\n"
           "[Parsed_showinfo_2 @ 0x1] n:   2 pts:  605 pts_time:5.04 pos: 3\n")

    def test_parse_and_build(self):
        cuts = sp.parse_scene_log(self.LOG, 24.0)
        self.assertEqual(cuts, [74, 120, 121])
        shots = sp.build_shots(cuts, 240, 24.0)
        self.assertEqual(shots, [(0, 74), (74, 46), (120, 120)])          # 121 merged (too short), covers 240
        self.assertEqual(sum(n for _, n in shots), 240)

    def test_cut_near_the_end_or_start_is_dropped(self):
        self.assertEqual(sp.build_shots([2, 238], 240, 24.0), [(0, 240)])

    def test_window_offset(self):
        self.assertEqual(sp.parse_scene_log(self.LOG, 24.0, t0=3.0), [2, 48, 49])

    def test_idr_points_and_switch_map(self):
        shots = [(0, 100), (100, 30), (130, 70)]
        pts = sp.idr_points(shots, 48)
        self.assertEqual(pts, [0, 48, 96, 100, 130, 178])
        asg = sp.switch_assignment([0, 0, -1, -1, 1, 0], len(pts), 4)
        self.assertEqual(len(asg), 6)
        self.assertEqual(asg[0], 0)
        m = sp.frame_rung_map(shots, 48, [0, 0, -1, -1, 1, 0], 200, 4)
        self.assertEqual(len(m), 200)
        self.assertEqual(set(m), {0, 1, 3})
        for f in range(1, 200):                                               # switches only at IDR points
            if m[f] != m[f - 1]:
                self.assertIn(f, pts)

    def test_plan_clamps_to_short_ladders(self):
        self.assertEqual(sp.switch_assignment([0, -1, 2], 3, 2), [0, 1, 1])


class EncoderArgs(unittest.TestCase):
    def test_x265_closed_gop_no_scenecut(self):
        a = sp.encoder_args(sp.PRESETS["hevc_gop2"], 3000, 24.0, 100)
        s = " ".join(a)
        for needle in ("libx265", "keyint=48", "min-keyint=48", "scenecut=0", "open-gop=0", "bframes=3",
                       "aq-mode=3", "bitrate=3000", "vbv-maxrate=4500", "vbv-bufsize=9000", "yuv420p10le"):
            self.assertIn(needle, s)
        self.assertIn("keyint=96", " ".join(sp.encoder_args(sp.PRESETS["hevc_gop4"], 3000, 24.0, 300)))

    def test_gop_is_rate_general(self):
        for fps in (23.976, 25.0, 29.97, 50.0, 59.94, 120.0):
            g = sp.gop_frames(2.0, fps)
            self.assertAlmostEqual(g / fps, 2.0, delta=0.5 / fps + 1e-9)

    def test_av1_variants(self):
        plain = " ".join(sp.encoder_args(sp.PRESETS["av1"], 2000, 24.0, 100))
        fgs = " ".join(sp.encoder_args(sp.PRESETS["av1_fgs"], 2000, 24.0, 100))
        self.assertNotIn("film-grain", plain)
        self.assertIn("film-grain=8", fgs)
        self.assertIn("film-grain-denoise=1", fgs)
        self.assertNotIn("mbr=", fgs)                    # SVT-AV1 allows a max bitrate in CRF mode only
        self.assertIn("rc=1", fgs)

    def test_lookahead_never_exceeds_the_shot(self):
        self.assertIn("rc-lookahead=9", sp.x265_params(sp.PRESETS["hevc_gop2"], 1000, 24.0, 10))

    def test_every_preset_documents_itself(self):
        for k, v in sp.PRESETS.items():
            self.assertTrue(v["note"], k)


class GeneratorHelpers(unittest.TestCase):
    def test_crop_is_aligned_and_clamped(self):
        x, y, w, h = mse.crop_for(1920, 1080, (960, 544))
        self.assertEqual((x % 4, y % 4, w % 4, h % 4), (0, 0, 0, 0))
        self.assertEqual((w, h), (960, 544))
        x, y, w, h = mse.crop_for(640, 360, (960, 544))
        self.assertEqual((w, h), (640, 360))
        self.assertEqual(mse.crop_for(1280, 720, None), (0, 0, 1280, 720))

    def test_sources_cover_the_content_types(self):
        self.assertEqual({s["ct"] for s in mse.SOURCES} >= {"film24", "dark", "sports50_60"}, True)

    def test_suite_merge_is_stable(self):
        p = os.path.join(tempfile.mkdtemp(prefix="vp_stream_"), "s.json")
        try:
            mse.merge_suite(p, "t", "d", {"b": {"id": "b"}, "a": {"id": "a"}})
            mse.merge_suite(p, "t", "d", {"c": {"id": "c"}, "a": {"id": "a2"}})
            with open(p) as f:
                s = json.load(f)
            self.assertEqual([c["id"] for c in s["cases"]], ["a2", "b", "c"])
            self.assertTrue(s["nr_metrics"])
        finally:
            shutil.rmtree(os.path.dirname(p), ignore_errors=True)


# ------------------------------------------------------------------ no-reference metrics
def ramp(h, w, lo, hi, bits=10, quant8=True, dither=0.0, seed=1):
    """Horizontal ramp from lo to hi (8-bit units) in a plane of `bits`; quant8 rounds to 8-bit codes."""
    x = np.linspace(lo, hi, w)[None, :].repeat(h, 0)
    if dither:
        rng = np.random.default_rng(seed)
        x = x + dither * (rng.random(x.shape) - rng.random(x.shape))
    if quant8:
        return np.rint(x) * 2 ** (bits - 8)
    return np.rint(x * 2 ** (bits - 8))                   # a properly quantised native-depth ramp


class NoReference(unittest.TestCase):
    def test_banding_flags_a_staircase_not_a_dithered_or_native_ramp(self):
        stair = nr.banding(nr.to_u8(ramp(256, 384, 100, 106), 10), 10)[0]
        dith = nr.banding(nr.to_u8(ramp(256, 384, 100, 106, dither=1.0), 10), 10)[0]
        smooth = nr.banding(nr.to_u8(ramp(256, 384, 100, 106, quant8=False), 10), 10)[0]
        self.assertGreater(stair, 0.02)
        self.assertGreater(stair, 2.0 * dith)
        self.assertGreater(stair, 3.0 * smooth)               # 10-bit steps are a quarter of an 8-bit step

    def test_banding_is_darker_weighted(self):
        dark = nr.banding(nr.to_u8(ramp(256, 384, 20, 26), 10), 10)[0]
        light = nr.banding(nr.to_u8(ramp(256, 384, 200, 206), 10), 10)[0]
        self.assertGreater(dark, light)

    def test_banding_is_resolution_general(self):
        a = nr.banding(nr.to_u8(ramp(256, 384, 100, 106), 10), 10)[0]
        b = nr.banding(nr.to_u8(ramp(512, 768, 100, 106), 10), 10)[0]
        self.assertGreater(b, 0.5 * a)
        self.assertLess(b, 2.0 * a)

    def test_flat_black_is_not_banding(self):
        self.assertEqual(nr.banding(np.full((128, 128), 16.0), 8)[0], 0.0)

    def test_blockiness_finds_the_grid_at_any_phase(self):
        rng = np.random.default_rng(3)
        h, w = 256, 384
        base = 100 + rng.normal(0, 0.6, (h, w))
        blocks = rng.normal(0, 1.5, (h // 8 + 2, w // 8 + 2))
        for phase in (0, 3):
            img = base.copy()
            ys, xs = np.mgrid[0:h, 0:w]
            img += blocks[ys // 8, (xs + phase) // 8]
            self.assertGreater(nr.blockiness(img)[0], 0.25, phase)
        self.assertLess(nr.blockiness(base)[0], 0.12)

    def test_blockiness_16_grid(self):
        rng = np.random.default_rng(4)
        h, w = 256, 512
        ys, xs = np.mgrid[0:h, 0:w]
        img = 100 + rng.normal(0, 0.6, (h, w)) + rng.normal(0, 1.5, (h // 16 + 1, w // 16 + 1))[ys // 16, xs // 16]
        self.assertGreater(nr.blockiness(img)[0], 0.25)

    def test_ringing_over_a_noise_floor(self):
        rng = np.random.default_rng(5)
        h, w = 256, 384
        clean = np.where(np.arange(w)[None, :] < w // 2, 60.0, 180.0).repeat(h, 0) + rng.normal(0, 0.4, (h, w))
        d = np.abs(np.arange(w) - w // 2).astype(float)
        osc = 7.0 * np.cos(2.2 * d) * np.exp(-d / 6.0) * np.sign(np.arange(w) - w // 2 + 0.5)
        ring = clean + osc[None, :]
        r_clean, r_ring = nr.ringing(clean)[0], nr.ringing(ring)[0]
        self.assertGreater(r_ring, 4.0 * max(r_clean, 0.2))

    def test_noise_sigma_tracks_the_added_noise(self):
        rng = np.random.default_rng(6)
        s2 = nr.noise_sigma(100 + rng.normal(0, 2.0, (256, 256)))[0]
        s4 = nr.noise_sigma(100 + rng.normal(0, 4.0, (256, 256)))[0]
        self.assertAlmostEqual(s4 / s2, 2.0, delta=0.25)
        self.assertGreater(s2, 0.8)

    def test_bit_depth_independent(self):
        rng = np.random.default_rng(7)
        a8 = 100 + rng.normal(0, 2.0, (192, 192))
        self.assertAlmostEqual(nr.noise_sigma(nr.to_u8(a8 * 4, 10))[0], nr.noise_sigma(nr.to_u8(a8, 8))[0], places=6)

    def test_deterministic_and_complete(self):
        rng = np.random.default_rng(8)
        pl = 400 + rng.normal(0, 3, (128, 160))
        a, b = nr.nr_frame(pl, 10), nr.nr_frame(pl, 10)
        self.assertEqual(a, b)
        for k in ("band", "block", "ring", "noise"):
            self.assertIn("nr_" + k, a)
            self.assertIn("nr_%s_cover" % k, a)

    def test_poor_index_and_thresholds(self):
        thr = {"poor": {"band": 0.1, "block": 0.5, "ring": 2.0}}
        self.assertAlmostEqual(nr.poor_index(dict(nr_band=0.05, nr_block=0.1, nr_ring=1.0), thr), 0.5)
        self.assertGreaterEqual(nr.poor_index(dict(nr_band=0.05, nr_block=0.6, nr_ring=1.0), thr), 1.0)
        self.assertEqual(nr.poor_index(dict(nr_band=1, nr_block=1, nr_ring=1), {"poor": {}}), 0.0)

    def test_committed_thresholds_parse_if_present(self):
        thr = nr.load_thresholds()
        if thr is not None:
            self.assertTrue(set(thr["poor"]) <= {"band", "block", "ring"})
            self.assertTrue(all(v >= 0 for v in thr["poor"].values()))


# ------------------------------------------------------------------ validation helpers
class Validation(unittest.TestCase):
    def test_auc(self):
        self.assertEqual(nv.auc([5, 6], [1, 2]), 1.0)
        self.assertEqual(nv.auc([1, 2], [5, 6]), 0.0)
        self.assertEqual(nv.auc([1], [1]), 0.5)
        self.assertIsNone(nv.auc([], [1]))

    def test_spearman_and_within_clip(self):
        self.assertAlmostEqual(nv.spearman(list(range(20)), [v * 2 + 1 for v in range(20)]), 1.0)
        self.assertIsNone(nv.spearman([1.0] * 10, list(range(10))))
        rows = []
        for clip, off in (("a", 0.0), ("b", 100.0)):
            for i in range(10):
                rows.append(dict(clip=clip, x=off + i, y=-(off * 5) - i))
        self.assertAlmostEqual(nv.within_clip(rows, "x", "y")[0], -1.0)

    def test_fr_frame_prefers_real_damage(self):
        rng = np.random.default_rng(9)
        ref = 400 + 40 * np.sin(np.arange(192)[None, :] / 20.0) + rng.normal(0, 2, (192, 192))
        blocky = ref + np.repeat(np.repeat(rng.normal(0, 6, (24, 24)), 8, 0), 8, 1)
        mild = ref + rng.normal(0, 1, ref.shape)
        a = nv.fr_frame(ref, blocky, 10, [0, 0])
        b = nv.fr_frame(ref, mild, 10, [0, 0])
        self.assertGreater(a["fr_block"], b["fr_block"] + 0.1)
        self.assertGreater(a["fr_ssim_loss"], b["fr_ssim_loss"])


# ------------------------------------------------------------------ recordings scanner
HEADER = ("index,hw_time_100ns,hw_duration_100ns,stream_time,stream_duration,callback_qpc,periods_since_previous,"
          "buffered,flags,no_input,hash,exact_repeat,callback_us\n")


def write_frames_csv(path, n, dur=416667, repeat_every=5):
    with open(path, "w", newline="") as f:
        f.write(HEADER)
        for i in range(n):
            rep = 1 if repeat_every and i % repeat_every == 4 else 0
            h = 1000 + i - (i // repeat_every if repeat_every else 0)
            f.write("%d,%d,%d,0,0,0,%d,0,0x0,0,%d,%d,0\n" % (i, i * dur, dur, 2 if i == 7 else 1, h, rep))


class Scanner(Base):
    def test_timing_only_recording(self):
        d = self.p("rec")
        os.makedirs(d)
        write_frames_csv(os.path.join(d, "frames.csv"), 50)
        r = sr.scan_one(d, None)
        self.assertEqual(r["tier"], "timing-only")
        self.assertEqual(r["frames"], 50)
        self.assertEqual(r["gaps"], 1)
        self.assertAlmostEqual(r["nominal_hz"], 24.0, places=1)
        self.assertGreater(r["exact_repeat_share"], 0.1)
        self.assertNotIn("metrics_median", r)

    def test_raw_frames_tier_and_poor_spans(self):
        d = self.p("recraw")
        os.makedirs(d)
        write_frames_csv(os.path.join(d, "frames.csv"), 30, dur=1000000, repeat_every=0)
        rng = np.random.default_rng(10)
        ev = ["recorder start"]
        for i in range(3):
            px = np.clip(120 + rng.normal(0, 3, (96, 128, 4)), 0, 255).astype(np.uint8)
            px.tofile(os.path.join(d, "raw_%05d.bin" % i))
            ev.append("frame %d: raw frame saved (128x96, 512 bytes per row, 8BitARGB)" % i)
        with open(os.path.join(d, "events.txt"), "w") as f:
            f.write("\n".join(ev) + "\n")
        r = sr.scan_one(d, {"poor": {"band": 1e-9, "block": 1e9, "ring": 1e9}})
        self.assertEqual(r["tier"], "full")
        self.assertEqual(r["series_frames"], 3)
        self.assertIn("poor_spans_s", r)

    def test_thumbnail_tier_never_flags_poor(self):
        d = self.p("rect")
        os.makedirs(d)
        write_frames_csv(os.path.join(d, "frames.csv"), 20, dur=1000000, repeat_every=0)
        rng = np.random.default_rng(11)
        rng.integers(0, 255, (20, 27, 48), dtype=np.uint8).tofile(os.path.join(d, "thumbs.y8"))
        with open(os.path.join(d, "events.txt"), "w") as f:
            f.write("thumbnails: 48x27 8-bit luma per frame, in frame order\n")
        r = sr.scan_one(d, {"poor": {"band": 1e-9, "block": 1e-9, "ring": 1e-9}}, stride=2)
        self.assertEqual(r["tier"], "thumbnail")
        self.assertNotIn("poor_spans_s", r)

    def test_poor_spans(self):
        thr = {"poor": {"band": 1.0, "block": 1.0, "ring": 1.0}}
        sec = [dict(second=s, nr_band=v, nr_block=0.0, nr_ring=0.0) for s, v in
               enumerate([0.1, 2, 2, 2, 0.1, 2, 0.1, 2, 2])]
        self.assertEqual(sr.poor_spans(sec, thr), [(1, 3), (7, 8)])

    def test_read_only_and_output_must_be_elsewhere(self):
        d = self.p("rec2")
        os.makedirs(d)
        write_frames_csv(os.path.join(d, "frames.csv"), 20)
        def digest():
            with open(os.path.join(d, "frames.csv"), "rb") as fh:
                return hashlib.sha256(fh.read()).hexdigest()
        before = digest()
        out = self.p("out")
        sr.main(["--root", d, "--out", out])
        self.assertEqual(sorted(os.listdir(d)), ["frames.csv"])
        self.assertEqual(digest(), before)
        self.assertTrue(os.path.exists(os.path.join(out, "recordings_summary.md")))
        with self.assertRaises(SystemExit):
            sr.main(["--root", d, "--out", os.path.join(d, "inside")])

    def test_find_recordings(self):
        for n in ("a", "b/c"):
            os.makedirs(self.p(n))
            write_frames_csv(os.path.join(self.p(n), "frames.csv"), 3)
        self.assertEqual(len(sr.find_recordings([self.dir])), 2)


# ------------------------------------------------------------------ harness hook
class HarnessHook(Base):
    def suite(self, name, nr_flag):
        fr = es.tiny_clip(n=6, w=128, h=96, seed=21, bitdepth=10, speed=2.0)
        rng = np.random.default_rng(5)
        coded = [f.with_planes(Y=np.clip(f.Y + np.rint(rng.normal(0, 3, f.Y.shape)), 0, 1023)) for f in fr]
        eio.write_raw(self.p(name + "_m.yuv"), fr)
        eio.write_raw(self.p(name + "_c.yuv"), coded)
        s = {"name": name, "nr_metrics": nr_flag, "cases": [{
            "content_type": "film24", "clip": name, "case": "c", "protocol": "chroma444", "frames": 6, "gop": 4, "minigop": 2,
            "master": {"kind": "raw", "path": self.p(name + "_m.yuv"), "width": 128, "height": 96, "pix_fmt": "yuv420p10le", "start": 0},
            "coded": {"kind": "raw", "path": self.p(name + "_c.yuv"), "width": 128, "height": 96, "pix_fmt": "yuv420p10le", "start": 0}}]}
        with open(self.p(name + ".json"), "w") as f:
            json.dump(s, f)
        return self.p(name + ".json")

    def test_suite_extra_only_for_nr_suites(self):
        self.assertIsNone(eh.suite_extra({"cases": []}))
        self.assertIs(eh.suite_extra({"nr_metrics": True}), nr.nr_extra)

    def test_also_suite_adds_nr_rows_and_is_deterministic(self):
        main = self.suite("main", False)
        also = self.suite("stream", True)
        outs = []
        for k in ("o1", "o2"):
            eh.main(["run", "--suite", main, "--candidate", "chroma_lanczos3", "--baseline", "chroma_bicubic",
                     "--out", self.p(k), "--no-cost", "--also-suite", also])
            with open(os.path.join(self.p(k), "results.csv"), "rb") as f:
                outs.append(f.read())
        self.assertEqual(outs[0], outs[1])
        rows = list(csv.DictReader(outs[0].decode().splitlines()))
        nr_rows = [r for r in rows if r["metric"].startswith("nr_")]
        self.assertTrue(nr_rows)
        self.assertEqual({r["clip"] for r in nr_rows}, {"stream"})      # only the streaming suite gets NR rows
        self.assertTrue(any(r["metric"] == "psnr_Y" and r["clip"] == "main" for r in rows))
        self.assertTrue({"nr_band", "nr_block", "nr_ring", "nr_noise"} <= {r["metric"] for r in nr_rows})

    def test_nr_rules_are_judged(self):
        import eval_report as er
        self.assertEqual(er.rule_for("nr_band")[0], "lower")
        self.assertEqual(er.rule_for("nr_noise")[0], "info")
        self.assertEqual(er.judge("nr_block", 0.1, 0.5)[2], "PASS")


class CommittedSuites(unittest.TestCase):
    def test_streaming_suites_parse(self):
        d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "suites")
        for fn in sorted(os.listdir(d)):
            if not fn.startswith("streaming_"):
                continue
            with open(os.path.join(d, fn)) as f:
                s = json.load(f)
            self.assertTrue(s["nr_metrics"], fn)
            ids = [c["id"] for c in s["cases"]]
            self.assertEqual(ids, sorted(set(ids)), fn)
            for c in s["cases"]:
                self.assertEqual(c["protocol"], "same")
                self.assertIn("streaming", c)
                self.assertIn(c["master"]["kind"], ("ffmpeg", "raw"))


if __name__ == "__main__":
    unittest.main()
