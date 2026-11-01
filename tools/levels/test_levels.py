# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for tools/levels. Run: python -I -B tools/levels/test_levels.py (from any cwd)."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import check  # noqa: E402
import deltae  # noqa: E402
import expected as ex  # noqa: E402
import patterns  # noqa: E402

SIZES = ((640, 360), (1920, 1080), (3840, 2160), (1280, 720), (720, 576))


def passthrough():
    return ex.chain_from_spec("bt1886:bt709:limited:10")


def frame_with(layout, mutate=None):
    y, cb, cr = patterns.render_frame(layout)
    y, cb, cr = y.astype(np.int32), cb.astype(np.int32), cr.astype(np.int32)
    if mutate:
        mutate(y, cb, cr, layout)
    return y, cb, cr


def run_eval(set_name, mutate=None, nframes=2, chain=None, size=(960, 540), th=None):
    layout = patterns.build_layout(set_name, *size)
    frames = [frame_with(layout, mutate) for _ in range(nframes)]
    return check.evaluate(layout, chain or passthrough(), iter(frames), th), layout


def row(rep, rid):
    return [r for r in rep["rows"] if r["id"] == rid][0]


class Sharma(unittest.TestCase):
    def test_pairs(self):
        self.assertGreaterEqual(len(deltae.SHARMA_PAIRS), 30)
        for i, (l1, l2, want) in enumerate(deltae.SHARMA_PAIRS, 1):
            got = deltae.ciede2000(l1, l2)
            self.assertAlmostEqual(got, want, places=4, msg="pair %d" % i)

    def test_symmetric(self):
        a, b = (50, 2.5, 0), (73, 25, -18)
        self.assertAlmostEqual(deltae.ciede2000(a, b), deltae.ciede2000(b, a), places=9)

    def test_xyz_white_is_lab_100(self):
        lab = deltae.xyz_to_lab(deltae.d65_white_xyz(100.0), deltae.d65_white_xyz(100.0))
        self.assertAlmostEqual(float(lab[0]), 100.0, places=6)
        self.assertAlmostEqual(float(lab[1]), 0.0, places=6)


class Itp(unittest.TestCase):
    def test_identical_zero_and_positive(self):
        w = ex.rgb_to_xyz_matrix("bt2020nc") @ np.array([100.0, 100.0, 100.0])
        self.assertEqual(deltae.delta_e_itp_xyz(w, w), 0.0)
        self.assertGreater(deltae.delta_e_itp_xyz(w, w * 1.1), 0.0)
        self.assertAlmostEqual(deltae.delta_e_itp_xyz(w, w * 1.1), deltae.delta_e_itp_xyz(w * 1.1, w), places=9)

    def test_neutral_has_no_chroma(self):
        w = ex.rgb_to_xyz_matrix("bt2020nc") @ np.array([100.0, 100.0, 100.0])
        i, t, p = deltae.itp_from_xyz(w)
        self.assertAlmostEqual(float(t), 0.0, places=3)
        self.assertAlmostEqual(float(p), 0.0, places=3)


class Expected(unittest.TestCase):
    def test_matrix_computed_from_chromaticities(self):
        m = ex.rgb_to_xyz_matrix("bt709")
        want = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
        self.assertTrue(np.allclose(m, want, atol=1e-3))

    def test_identity_chain_codes(self):
        for tr in ex.TRANSFERS:
            for mat in ("bt709", "bt2020nc"):
                for rng in ex.RANGES:
                    ch = ex.chain_from_spec("%s:%s:%s:10" % (tr, mat, rng))
                    ys = [4, 16, 32, 48, 64, 128, 512, 876, 940, 980, 1019]
                    codes = np.array([[y, 512, 512] for y in ys], dtype=np.float64)
                    out = ex.expected_codes(codes, ch)
                    if rng == "full":  # codes below 0 / above max cannot occur; 4 and 1019 are in range
                        pass
                    self.assertTrue(np.array_equal(out, codes), "%s %s %s: %s" % (tr, mat, rng, out[:, 0]))

    def test_identity_with_colour(self):
        ch = ex.chain_from_spec("pq:bt2020nc:limited:10")
        codes = np.array([[300, 100, 900], [700, 960, 64], [400, 512, 512]], dtype=np.float64)
        self.assertTrue(np.array_equal(ex.expected_codes(codes, ch), codes))

    def test_bt709_limited_full_round_trip(self):
        a = ex.chain_from_spec("bt1886:bt709:limited:10", "bt1886:bt709:full:10")
        b = ex.chain_from_spec("bt1886:bt709:full:10", "bt1886:bt709:limited:10")
        y = np.arange(64, 941, dtype=np.float64)
        codes = np.stack([y, np.full_like(y, 512), np.full_like(y, 512)], axis=-1)
        back = ex.expected_codes(ex.expected_codes(codes, a), b)
        self.assertTrue(np.array_equal(back, codes))
        full = ex.expected_codes(np.array([[64, 512, 512], [940, 512, 512]], dtype=np.float64), a)
        self.assertEqual(int(full[0, 0]), 0)
        self.assertEqual(int(full[1, 0]), 1023)

    def test_pq_203_nits_code(self):
        # 203 nits -> PQ 0.5807; 10-bit narrow = 64 + 876 * 0.5807 = ~572.7 (full range ~594).
        v = float(ex.pq_inverse(203.0))
        self.assertAlmostEqual(v, 0.5807, places=3)
        ch = ex.chain_from_spec("bt1886:bt709:limited:10", "pq:bt709:limited:10")
        out = ex.expected_codes(np.array([[940, 512, 512]], dtype=np.float64), ch)
        self.assertLessEqual(abs(float(out[0, 0]) - (64 + 876 * v)), 1.0)
        self.assertLessEqual(abs(float(out[0, 0]) - 573), 1.0)
        self.assertEqual(int(out[0, 1]), 512)

    def test_sdr_white_709_to_2020_pq_stays_neutral(self):
        ch = ex.chain_from_spec("bt1886:bt709:limited:10", "pq:bt2020nc:limited:10")
        out = ex.expected_codes(np.array([[940, 512, 512], [502, 512, 512]], dtype=np.float64), ch)
        self.assertTrue(np.all(np.abs(out[:, 1:] - 512) <= 1))
        self.assertLessEqual(abs(float(out[0, 0]) - 573), 1.0)

    def test_transfer_inverses(self):
        v = np.array([-0.05, 0.0, 0.01, 0.3, 0.5, 0.9, 1.0, 1.09])
        for f, g in ((ex.pq_eotf, ex.pq_inverse), (ex.bt1886_eotf, ex.bt1886_inverse),
                     (lambda x: ex.bt1886_eotf(x, 100, 0.05), lambda x: ex.bt1886_inverse(x, 100, 0.05))):
            self.assertTrue(np.allclose(g(f(v)), v, atol=1e-6))
        rgb = np.stack([v, v * 0.9, v * 0.8], axis=-1)
        luma = ex.rgb_to_xyz_matrix("bt2020nc")[1]
        back = ex.hlg_inverse(ex.hlg_eotf(rgb, 1000.0, luma), 1000.0, luma)
        self.assertTrue(np.allclose(back, rgb, atol=1e-6))

    def test_btb_wtw_column_and_clipping(self):
        ch = passthrough()
        rows = ex.table_rows(patterns.build_layout("levels10", 960, 540)["patches"], ch)
        cls = {r["src_y"]: r["btb_wtw_expected"] for r in rows}
        self.assertEqual(cls[4], "BTB")
        self.assertEqual(cls[48], "BTB")
        self.assertEqual(cls[64], "")
        self.assertEqual(cls[940], "")
        self.assertEqual(cls[980], "WTW")
        clip = ex.chain_from_spec("bt1886:bt709:limited:10", keeps_btb_wtw=False)
        out = ex.expected_codes(np.array([[4, 512, 512], [1019, 512, 512]], dtype=np.float64), clip)
        self.assertEqual(int(out[0, 0]), 64)
        self.assertEqual(int(out[1, 0]), 940)

    def test_table_formats(self):
        rows = ex.table_rows(patterns.build_layout("levels10", 960, 540)["patches"], passthrough())
        self.assertTrue(ex.format_table(rows, "csv").startswith("id,src_y"))
        self.assertIn("| id |", ex.format_table(rows, "md"))


class Patterns(unittest.TestCase):
    def test_reserved_codes_never_generated(self):
        for s in sorted(patterns.SETS):
            for size in SIZES:
                lay = patterns.build_layout(s, *size)
                for p in lay["patches"]:
                    for v in p["ycbcr"]:
                        self.assertNotIn(v, patterns.RESERVED, "%s %s" % (s, p["id"]))
                if s in ("levels10", "levels8", "ramp10"):
                    y, cb, cr = patterns.render_frame(lay)
                    for pl in (y, cb, cr):
                        self.assertFalse(np.isin(pl, list(patterns.RESERVED)).any())

    def test_alignment_and_height(self):
        for s in sorted(patterns.SETS):
            for size in SIZES:
                lay = patterns.build_layout(s, *size)
                for p in lay["patches"]:
                    x0, y0, x1, y1 = p["rect_px"]
                    self.assertEqual(x0 % 2, 0, p["id"])
                    self.assertEqual(x1 % 2, 0, p["id"])
                    self.assertGreaterEqual((y1 - y0) / float(size[1]), patterns.MIN_PATCH_H - 0.005,
                                            "%s %s" % (s, p["id"]))
                    self.assertGreaterEqual(p["rect_frac"][3] - p["rect_frac"][1], patterns.MIN_PATCH_H)
                    self.assertTrue(0 <= x0 < x1 <= size[0] and 0 <= y0 < y1 <= size[1])

    def test_levels_content(self):
        l10 = patterns.build_layout("levels10", 960, 540)
        self.assertEqual([p["ycbcr"][0] for p in l10["patches"]], list(patterns.LEVELS10))
        l8 = patterns.build_layout("levels8", 960, 540)
        self.assertEqual([p["ycbcr"][0] for p in l8["patches"]], [4 * c for c in patterns.LEVELS8])
        self.assertNotIn(0, patterns.LEVELS8)
        self.assertNotIn(255, patterns.LEVELS8)
        self.assertGreaterEqual(len(patterns.build_layout("near_black", 960, 540)["patches"]), 8)
        self.assertGreaterEqual(len(patterns.build_layout("near_white", 960, 540)["patches"]), 8)

    def test_ramp_monotonic_unique(self):
        ys = [p["ycbcr"][0] for p in patterns.build_layout("ramp10", 1920, 1080)["patches"]]
        self.assertEqual(ys[0], 4)
        self.assertEqual(ys[-1], 1019)
        self.assertTrue(all(b > a for a, b in zip(ys, ys[1:])))

    def test_too_small_frame_rejected(self):
        with self.assertRaises(ValueError):
            patterns.build_layout("ramp10", 64, 64)

    def test_v210_round_trip(self):
        rng = np.random.default_rng(1)
        for w, h in ((96, 4), (100, 3), (1920, 2), (48, 2)):
            y = rng.integers(0, 1024, (h, w), dtype=np.uint16)
            cb = rng.integers(0, 1024, (h, w // 2), dtype=np.uint16)
            cr = rng.integers(0, 1024, (h, w // 2), dtype=np.uint16)
            data = patterns.v210_pack(y, cb, cr)
            self.assertEqual(len(data), patterns.v210_row_bytes(w) * h)
            y2, cb2, cr2 = patterns.v210_unpack(data, w, h)
            self.assertTrue(np.array_equal(y, y2))
            self.assertTrue(np.array_equal(cb, cb2))
            self.assertTrue(np.array_equal(cr, cr2))

    def test_v210_word_layout(self):
        y = np.arange(1, 7, dtype=np.uint16).reshape(1, 6)
        cb = np.array([[100, 101, 102]], dtype=np.uint16)
        cr = np.array([[200, 201, 202]], dtype=np.uint16)
        w = np.frombuffer(patterns.v210_pack(y, cb, cr), "<u4")[:4]
        self.assertEqual(int(w[0]), 100 | (1 << 10) | (200 << 20))
        self.assertEqual(int(w[3]), 5 | (202 << 10) | (6 << 20))


class SyntheticCapture(unittest.TestCase):
    def test_exact_passes(self):
        rep, _ = run_eval("levels10")
        self.assertEqual(rep["verdict"], "PASS", json.dumps(rep["rows"])[:400])
        self.assertEqual(row(rep, "btb_reached")["status"], "PASS")
        self.assertEqual(row(rep, "wtw_reached")["status"], "PASS")

    def test_one_code_offset_passes(self):
        def m(y, cb, cr, lay):
            y += 1
        rep, _ = run_eval("levels10", m)
        self.assertEqual(rep["verdict"], "PASS")

    def test_two_code_offset_fails(self):
        def m(y, cb, cr, lay):
            y += 2
        rep, _ = run_eval("levels10", m)
        self.assertEqual(rep["verdict"], "FAIL")

    def test_sigma_delta_mean_averaged(self):
        # alternating +1/+0 frames average to 0.5: passes
        layout = patterns.build_layout("levels10", 960, 540)
        f0 = frame_with(layout)
        f1 = frame_with(layout, lambda y, cb, cr, l: y.__iadd__(1))
        rep = check.evaluate(layout, passthrough(), iter([f0, f1, f0, f1]))
        self.assertEqual(rep["verdict"], "PASS")

    def test_clipped_btb_fails(self):
        def m(y, cb, cr, lay):
            for p in lay["patches"]:
                if p["ycbcr"][0] < 64:
                    x0, y0, x1, y1 = p["rect_px"]
                    y[y0:y1, x0:x1] = 64
        rep, _ = run_eval("levels10", m)
        self.assertEqual(rep["verdict"], "FAIL")
        self.assertEqual(row(rep, "btb_reached")["status"], "FAIL")
        self.assertEqual(row(rep, "wtw_reached")["status"], "PASS")

    def test_clipped_wtw_fails(self):
        def m(y, cb, cr, lay):
            for p in lay["patches"]:
                if p["ycbcr"][0] > 940:
                    x0, y0, x1, y1 = p["rect_px"]
                    y[y0:y1, x0:x1] = 940
        rep, _ = run_eval("levels10", m)
        self.assertEqual(row(rep, "wtw_reached")["status"], "FAIL")
        self.assertEqual(rep["verdict"], "FAIL")

    def test_ramp_passes_and_nonmonotonic_fails(self):
        rep, _ = run_eval("ramp10")
        self.assertEqual(rep["verdict"], "PASS")
        self.assertEqual(row(rep, "ramp_monotonic")["status"], "PASS")

        def m(y, cb, cr, lay):
            a, b = lay["patches"][30]["rect_px"], lay["patches"][31]["rect_px"]
            ca = y[a[1]:a[3], a[0]:a[2]].copy()
            y[a[1]:a[3], a[0]:a[2]] = y[b[1]:b[3], b[0]:b[2]]
            y[b[1]:b[3], b[0]:b[2]] = ca
        rep, _ = run_eval("ramp10", m, size=(1920, 1080))
        self.assertEqual(row(rep, "ramp_monotonic")["status"], "FAIL")
        self.assertEqual(rep["verdict"], "FAIL")

    def test_no_frames_is_no_data(self):
        layout = patterns.build_layout("levels10", 960, 540)
        rep = check.evaluate(layout, passthrough(), iter([]))
        self.assertEqual(rep["verdict"], "INCOMPLETE")
        self.assertTrue(all(r["status"] == "NO DATA" for r in rep["rows"]))

    def test_frame_too_small_for_patch_is_no_data(self):
        layout = patterns.build_layout("levels10", 960, 540)
        tiny = (np.zeros((2, 2), np.int32), np.zeros((2, 1), np.int32), np.zeros((2, 1), np.int32))
        rep = check.evaluate(layout, passthrough(), iter([tiny]))
        self.assertEqual(rep["verdict"], "INCOMPLETE")

    def test_other_size_capture(self):
        # layout made at one size, capture at another: fractions rescale
        layout = patterns.build_layout("levels10", 960, 540)
        big = patterns.build_layout("levels10", 1920, 1080)
        rep = check.evaluate(layout, passthrough(), iter([frame_with(big)]))
        self.assertEqual(rep["verdict"], "PASS")

    def test_processed_chain_uses_delta_e(self):
        ch = ex.chain_from_spec("bt1886:bt709:limited:10", "pq:bt2020nc:limited:10")
        layout = patterns.build_layout("bars709", 960, 540)
        exp = {r["id"]: r for r in ex.table_rows(layout["patches"], ch)}

        def m(y, cb, cr, lay):
            for p in lay["patches"]:
                x0, y0, x1, y1 = p["rect_px"]
                e = exp[p["id"]]
                y[y0:y1, x0:x1] = e["exp_y"]
                cb[y0:y1, x0 // 2:x1 // 2] = e["exp_cb"]
                cr[y0:y1, x0 // 2:x1 // 2] = e["exp_cr"]
        rep = check.evaluate(layout, ch, iter([frame_with(layout, m)]))
        self.assertFalse(rep["passthrough"])
        self.assertIn("de_itp", rep["rows"][0])
        self.assertNotEqual(rep["verdict"], "FAIL", json.dumps([r for r in rep["rows"] if r["status"] == "FAIL"])[:300])

    def test_threshold_override(self):
        def m(y, cb, cr, lay):
            y += 2
        rep, _ = run_eval("levels10", m, th={"max_code_error": 2.0})
        self.assertEqual(rep["verdict"], "PASS")


class EndToEnd(unittest.TestCase):
    def test_y4m_cli(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(patterns.main(["--set", "levels10", "--size", "960x540", "--frames", "3", "--out", d]), 0)
            out = os.path.join(d, "rep")
            rc = check.main([os.path.join(d, "levels10.y4m"), "--layout", os.path.join(d, "levels10.layout.json"),
                             "--out", out, "--frames", "2"])
            self.assertEqual(rc, 0)
            with open(os.path.join(out, "report.json")) as f:
                rep = json.load(f)
            self.assertEqual(rep["frames_read"], 2)
            self.assertTrue(os.path.isfile(os.path.join(out, "report.md")))

    def test_v210_cli_and_recorder_dir(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(patterns.main(["--set", "levels10", "--size", "960x540", "--frames", "2", "--v210",
                                            "--out", d]), 0)
            lay = os.path.join(d, "levels10.layout.json")
            raw = os.path.join(d, "levels10.v210")
            # split into recorder-style raw frames + events.txt
            rb = patterns.v210_row_bytes(960)
            with open(raw, "rb") as fh:
                data = fh.read()
            fs = rb * 540
            rec = os.path.join(d, "stream")
            os.makedirs(rec)
            with open(os.path.join(rec, "events.txt"), "w") as ev:
                for i in range(2):
                    with open(os.path.join(rec, "raw_%05d.bin" % i), "wb") as f:
                        f.write(data[i * fs:(i + 1) * fs])
                    ev.write("frame %d: raw frame saved (960x540, %d bytes per row, 10BitYUV)\n" % (i, rb))
            self.assertEqual(check.main([rec, "--layout", lay, "--out", os.path.join(d, "r1")]), 0)
            self.assertEqual(check.main([os.path.join(rec, "raw_00000.bin"), "--layout", lay, "--width", "960",
                                         "--height", "540", "--row-bytes", str(rb), "--out", os.path.join(d, "r2")]), 0)
            # a truncated raw file gives no data -> INCOMPLETE (3)
            with open(os.path.join(rec, "raw_00001.bin"), "wb") as f:
                f.write(data[:100])
            os.remove(os.path.join(rec, "raw_00000.bin"))
            self.assertEqual(check.main([rec, "--layout", lay, "--out", os.path.join(d, "r3")]), 3)

    def test_expected_cli_and_usage_errors(self):
        self.assertEqual(ex.main(["--set", "levels10", "--size", "960x540"]), 0)
        with tempfile.TemporaryDirectory() as d:
            patterns.main(["--set", "levels10", "--size", "960x540", "--out", d])
            self.assertEqual(check.main([os.path.join(d, "nothing.y4m"), "--layout",
                                         os.path.join(d, "levels10.layout.json")]), 2)
        with self.assertRaises(SystemExit):
            patterns.main(["--set", "levels10", "--out", "x"])  # size is required


if __name__ == "__main__":
    unittest.main(verbosity=1)
