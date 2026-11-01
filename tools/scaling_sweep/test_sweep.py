# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""CPU-only tests for sweep.py (no GPU, no renders):
python -I -m unittest discover -s tools/scaling_sweep -p "test_*.py" -v"""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep  # noqa: E402


class SweepTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="vp_scaling_sweep_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def args(self, *extra):
        return sweep.parse_args(["plan", "--work", os.path.join(self.dir, "work")] + list(extra))

    def test_refuse_work(self):
        for bad in (r"E:\x", r"f:\scaling", r"D:\nn-corpus", r"D:\nn-corpus\sub", "d:/NN-corpus/x"):
            self.assertIsNotNone(sweep.refuse_work(bad), bad)
        for ok in (r"D:\codex\scaling-sweep", r"D:\nn-corpus2\x", self.dir):
            self.assertIsNone(sweep.refuse_work(ok), ok)
        self.assertEqual(sweep.main(["plan", "--dry-run", "--work", r"E:\nope"]), 2)

    def test_grid_sizes(self):
        self.assertEqual(len(sweep.variants_down()), 2 * 7 + 2)
        self.assertEqual(len(sweep.variants_up(2, "0.2")), 1 + 1 + 5 * 2 * 2 * 2)   # controls + (direct, route) x grid
        self.assertEqual(len(sweep.variants_up(3, "0.2")), 1 + 2 * 2)
        m = sweep.variants_motion("down", "0.2")
        self.assertEqual(len(m), 1 + 3 * 2 * 2)
        self.assertEqual(m[0]["stages"][0][0]["adaptive_motion"], "off")
        for v in m[1:]:
            s = v["stages"][0][0]
            self.assertLess(float(s["adaptive_motion_speed_lo"]), float(s["adaptive_motion_speed_hi"]))
        n = {}
        for g in sweep.geometries(self.args()):
            n[g["sweep"]] = n.get(g["sweep"], 0) + 1
        self.assertEqual(n, {1: 2, 2: 4, 3: 3, 4: 6})

    def test_variant_settings(self):
        up = {v["id"]: v for v in sweep.variants_up(2, "0.35")}
        s, sh = up["d_w0p25_sroff_gamma"]["stages"][0]
        self.assertEqual((s["adaptive_upscale_w_base"], s["adaptive_upscale_superres_scale"], s["upscale_light"]), ("0.25", "off", "gamma"))
        self.assertIn("standard.fsrcnnx", sh)
        route = up["r_w1p0_sron_linear"]
        self.assertEqual(route["kind"], "route")
        self.assertEqual(route["stages"][1][0]["adaptive_w_base"], "0.35")
        ctrl = up["ctrl_ewa"]["stages"][0]
        self.assertEqual((ctrl[0]["adaptive_upscale"], ctrl[0]["upscaler"], ctrl[1]), ("off", "ewa_lanczossharp", []))
        self.assertEqual(sweep.route_size((1920, 1080)), (3840, 2160))
        self.assertEqual(sweep.route_size((1280, 720)), (2560, 1440))

    def test_prepare_cmd_and_refusal(self):
        a = self.args()
        plan = sweep.Plan(a)
        g = plan.geoms[0]
        cmd = sweep.prepare_cmd(a, g, g["pics"][0], "m.mkv", ["s", "r", "k"])
        self.assertLess(cmd.index("-i"), cmd.index("-fps_mode"), "-fps_mode is an output option (ffmpeg 9)")
        self.assertEqual(cmd.count("-fps_mode"), 3)
        self.assertIn("format=yuv420p10le,extractplanes=y", cmd[cmd.index("-filter_complex") + 1])
        self.assertEqual(sorted(sweep.prepared(plan)), sorted(x["id"] for x in plan.geoms))
        a2 = sweep.parse_args(["render", "--work", os.path.join(self.dir, "work"), "--sweeps", "3"])
        self.assertEqual(sweep.main(["render", "--work", os.path.join(self.dir, "work"), "--sweeps", "3"]) if not sweep.Plan(a2).problems else 1, 1)

    def test_camera_model_doubles(self):
        a0, a1 = self.args(), self.args("--camera-model-key", "camera_model")
        n0 = sum(len(sweep.geom_variants(a0, g)) for g in sweep.geometries(a0) if g["sweep"] in (1, 2, 4))
        n1 = sum(len(sweep.geom_variants(a1, g)) for g in sweep.geometries(a1) if g["sweep"] in (1, 2, 4))
        self.assertEqual(n1, 2 * n0)
        g3 = [g for g in sweep.geometries(a1) if g["sweep"] == 3][0]
        self.assertEqual(len(sweep.geom_variants(a1, g3)), len(sweep.geom_variants(a0, g3)))
        v = sweep.geom_variants(a1, sweep.geometries(a1)[0])
        on = [x for x in v if x["id"].endswith("__cm_on")][0]
        self.assertEqual(on["stages"][0][0]["camera_model"], "on")
        self.assertTrue(on["base"].endswith("__cm_on"))
        self.assertIn(on["base"], {x["id"] for x in v})

    def test_cfg_and_job_text(self):
        a = self.args()
        plan = sweep.Plan(a)
        cfg = sweep.cfg_text(dict(adaptive_motion="on", adaptive_motion_speed_lo="0.1"))
        for line in ("adaptive_motion: on", "[vprenderer.Default]", "[shader.standard.ssim_downscaler]", "source_resolution: native"):
            self.assertIn(line, cfg)
        seq = next(j for j in plan.jobs if j["geom"].startswith("s4_down") and j["variant"].startswith("am_"))
        t = sweep.job_text(a, seq, plan.geom(seq["geom"]), plan.work)
        for line in ("source 3840 2160 24 1", "window 2560 1440", "motion vectors", "capture per_frame", "settle 3.0", "require_motion 0.5",
                     "require_hook SSimDownscaler.glsl", "shaders standard.ssim_downscaler"):
            self.assertIn(line, t)
        self.assertEqual(t.count("picture_seq "), 1)
        still = next(j for j in plan.jobs if j["geom"] == "s1_down_1920x1080")
        t = sweep.job_text(a, still, plan.geom(still["geom"]), plan.work)
        self.assertIn("picture warmup", t)
        self.assertEqual(t.count("\npicture "), 1 + 6)
        s2 = next(j for j in plan.jobs if j["stage"] == 2 and j["geom"] == "s2_up_1280x720_2560x1440")
        t = sweep.job_text(a, s2, plan.geom(s2["geom"]), plan.work)
        self.assertIn("source 2560 1440 24000 1001", t)
        self.assertIn("stage1", t)
        s1 = next(j for j in plan.jobs if j["variant"] == s2["variant"] and j["stage"] == 1 and j["geom"] == s2["geom"])
        self.assertEqual(s1["window"], [2560, 1440])
        self.assertFalse(s1["final"])
        self.assertTrue(s2["final"])

    def test_png_to_capture(self):
        r10 = np.array([[0, 1023, 512, 100], [64, 940, 300, 0]], np.uint16)
        rgb = np.stack([r10 << 6, np.zeros_like(r10), np.zeros_like(r10)], -1)
        p = os.path.join(self.dir, "t.png")
        sweep.write_png16(p, rgb)
        np.testing.assert_array_equal(sweep._read_png_py(p), r10 << 6)
        codes = sweep.png_to_luma_codes(p)
        self.assertEqual((int(codes[0, 0]), int(codes[0, 1])), (64, 940))
        g = dict(id="g", window=(4, 2), pics=[dict(tag="a_f001", master="a", frame=1, n=1, offset=0), dict(tag="b_f002", master="b", frame=2, n=1, offset=1)])
        meta = sweep.write_capture_dir(os.path.join(self.dir, "cap"), g, [("a_f001", 0, p), ("b_f002", 0, p), ("warm", 0, p)])
        self.assertEqual([f["source_frame_index"] for f in meta["frames"]], [0, 1])
        self.assertEqual(os.path.getsize(os.path.join(self.dir, "cap", "000001.yuv")), (4 * 2 + 2 * 2 * 1) * 2)
        with open(os.path.join(self.dir, "cap", "capture.json")) as f:
            disk = json.load(f)
        self.assertEqual((disk["schema"], disk["producer"], disk["width"], disk["height"], disk["pix_fmt"]), (1, "VpScaleRender", 4, 2, "yuv420p10le"))
        raw = np.fromfile(os.path.join(self.dir, "cap", "000000.yuv"), "<u2")
        np.testing.assert_array_equal(raw[:8].reshape(2, 4), codes)
        self.assertTrue((raw[8:] == 512).all())

    def test_png_up_filter_and_padding(self):
        w, h = 3, 2
        rows = [(np.arange(w * 3) * 64).astype(">u2"), (np.arange(w * 3) * 3).astype(">u2")]
        raw = bytearray()
        prev = np.zeros(w * 6, int)
        for r in rows:
            cur = np.frombuffer(r.tobytes(), np.uint8).astype(int)
            raw += b"\x02" + ((cur - prev) & 255).astype(np.uint8).tobytes()
            prev = cur
        p = os.path.join(self.dir, "up.png")

        def ch(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        with open(p, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(bytes(raw))) + ch(b"IEND", b""))
        got = sweep._read_png_py(p)
        np.testing.assert_array_equal(got, np.stack([rows[0][0::3], rows[1][0::3]]).astype(np.uint16))
        pad = sweep.pad_codes(np.full((2, 4), 500, "<u2"), (4, 4))
        self.assertEqual((pad[0, 0], pad[1, 0], pad[3, 3]), (64, 500, 64))

    def test_suite_json_shape(self):
        a = self.args()
        g = next(x for x in sweep.geometries(a) if x["id"] == "s4_up_boat")
        c = sweep.suite_json(g, r"D:\w")["cases"][0]
        self.assertEqual((c["protocol"], c["frames"], c["content_type"]), ("same", 24, "film24"))
        for k in ("master", "coded"):
            self.assertEqual((c[k]["kind"], c[k]["pix_fmt"], c[k]["width"], c[k]["height"], c[k]["start"]), ("raw", "yuv420p10le", 2560, 1440, 0))
        self.assertTrue(c["master"]["path"].endswith("ref/s4_up_boat.yuv"))
        g1 = next(x for x in sweep.geometries(a) if x["id"] == "s2_up_1920x1080_win2560x1600")
        s1 = sweep.suite_json(g1, r"D:\w")
        self.assertEqual([c["master"]["start"] for c in s1["cases"]], [0, 1, 2, 3, 4, 5])
        self.assertEqual(s1["cases"][0]["master"]["height"], 1600)
        json.dumps(s1)

    def test_estimate_and_summarise(self):
        est = sweep.Plan(self.args()).estimate()
        self.assertEqual(set(est), {1, 2, 3, 4})
        self.assertEqual(est[2]["jobs"], 252)
        p = os.path.join(self.dir, "r.csv")
        with open(p, "w") as f:
            f.write("content_type,clip,case,metric,candidate,default,delta,trend,flag,note\n"
                    "film24,a,c,ssim_Y,0.9,0.8,0.1,better,PASS,\nfilm24,a,c,grain_rms_ratio_Y,1.2,1.0,0.2,worse,FAIL,\nfilm24,a,c,lpips,NA,NA,NA,NA,NA,\n")
        m, fails, _ = sweep.summarise(p)
        self.assertEqual((m["ssim_Y"], fails, "lpips" in m), (0.9, ["grain_rms_ratio_Y"], False))
        rows = [dict(v="x", m={"ssim_Y": 0.99}, fails=["halo_overshoot_Y"]), dict(v="y", m={"ssim_Y": 0.95}, fails=[])]
        self.assertEqual(sweep.best_variant(rows)["v"], "y")


if __name__ == "__main__":
    unittest.main()
