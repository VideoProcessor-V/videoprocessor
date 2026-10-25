#!/usr/bin/env python3
"""Tests for the upscale study scripts (stdlib unittest; no renders, no GPU):
python -B tools/upscale_study/test_study.py"""
import csv
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402
import study  # noqa: E402


class StudyTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="vp_upscale_study_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def args(self, *extra):
        tools = os.path.join(self.dir, "tools")
        os.makedirs(tools, exist_ok=True)
        fake = {}
        for name in ("ffmpeg.exe", "VpScaleRender.exe", "renderer.dll", "PlScaleSweep.exe"):
            p = os.path.join(tools, name)
            open(p, "w").close()
            fake[name] = p
        kit = os.path.join(self.dir, "kit")
        os.makedirs(os.path.join(kit, "trainkit"), exist_ok=True)
        open(os.path.join(kit, "trainkit", "common.py"), "w").close()
        return ["--work", os.path.join(self.dir, "work"), "--ffmpeg", fake["ffmpeg.exe"], "--render-tool", fake["VpScaleRender.exe"],
                "--dll", fake["renderer.dll"], "--sweep-tool", fake["PlScaleSweep.exe"], "--kit", kit] + list(extra)

    def test_fit_matches_the_kit(self):
        self.assertEqual(study.fit(4096, 2160, 2560, 1600), (2560, 1350))
        self.assertEqual(study.fit(3840, 2160, 2560, 1600), (2560, 1440))
        self.assertEqual(study.fit(3840, 1714, 1920, 1080), (1920, 856))

    def test_every_vp_setting_is_one_the_renderer_accepts(self):
        for v in study.variants():
            for st in v["stages"]:
                for k, val in st["settings"].items():
                    if k in study.VALID:
                        self.assertIn(val, study.VALID[k], f"{v['id']} {k}")

    def test_plan_writes_valid_jobs(self):
        import argparse
        ap_args = self.args()
        a = argparse.Namespace(work=ap_args[1], ffmpeg=ap_args[3], render_tool=ap_args[5], dll=ap_args[7], sweep_tool=ap_args[9], kit=ap_args[11],
                               kit_python="", d_staging="", adapter="NVIDIA", hold=1.0, dry_run=False)
        plan = study.Plan(a).build()
        # the masters are only on the study PC: their absence is reported, nothing else
        self.assertTrue(all(p.startswith("master missing") for p in plan.problems), plan.problems)
        plan.write()
        self.assertTrue(os.path.exists(os.path.join(a.work, "plan.json")))
        # stage-2 route jobs read the stage-1 export, at 3840x2160, re-base off
        s2 = [j for j in plan.jobs if j["stage"] == 2]
        self.assertTrue(s2 and all(j["source"] == [3840, 2160] and "source_resolution: native" in j["cfg"] for j in s2))
        # player-upscaled deliveries are re-based to the encoded height
        pj = next(j for j in plan.jobs if j["case"] == "p720_2160" and j["stage"] == 1)
        self.assertIn("source_resolution: 720", pj["cfg"])
        self.assertIn("rebase_kernel_match: on", pj["cfg"])
        text = open(os.path.join(pj["dir"], "job.txt")).read()
        self.assertIn("source 3840 2160 24000 1001", text)
        self.assertIn("window 2560 1600", text)
        self.assertIn("picture warmup", text)
        est = plan.estimate()
        self.assertGreater(est["vp_jobs"], 0)
        self.assertEqual(est["scored_frames_per_case"], len(study.MASTERS) * len(study.SEGMENT_STARTS) * len(study.EVAL_FRAMES) + 1, "+ the WTW ramp")
        # The downscale A/B: polar vs separable faithful branch, fbo_float32 on/off, the ramp only there.
        down = [j for j in plan.jobs if j["case"] == "native2160"]
        self.assertEqual(sorted(j["variant"] for j in down), sorted(["down_polar_fbo32on", "down_polar_fbo32off", "down_separable_fbo32on", "down_separable_fbo32off"]))
        self.assertTrue(any("adaptive_faithful: ortho" in j["cfg"] for j in down) and any("adaptive_faithful: auto" in j["cfg"] for j in down))
        self.assertIn("wtwramp", open(os.path.join(down[0]["dir"], "job.txt")).read())
        self.assertNotIn("wtwramp", text)
        self.assertFalse(any(sw["case"] == "native2160" for sw in plan.sweeps))

    def test_dry_run_refuses_read_only_work_folders(self):
        a = self.args()
        a[1] = r"D:\nn-corpus\study"
        self.assertEqual(study.main(["plan"] + a + ["--dry-run"]), 2)

    def test_report_ranks_and_flags_grain_loss(self):
        work = os.path.join(self.dir, "w")
        os.makedirs(work)
        rows = [dict(case="n1080", variant="direct_lanczos", psnr=40, ssim=0.97, ring_codes=1.2, overshoot_codes=0.3, grain_keep=0.97, hf_err_codes=2, gpu_ms=""),
                dict(case="n1080", variant="sweep_x", psnr=41, ssim=0.98, ring_codes=1.0, overshoot_codes=0.2, grain_keep=0.90, hf_err_codes=1.8, gpu_ms=0.5)]
        with open(os.path.join(work, "scores.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
        report.main(["--work", work])
        text = open(os.path.join(work, "report.md")).read()
        self.assertLess(text.index("sweep_x"), text.index("direct_lanczos"), "sorted by PSNR")
        self.assertIn("sweep_x (grain lost)", text)


if __name__ == "__main__":
    unittest.main()
