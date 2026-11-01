# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for report.py. Run: python -I -B tools/soak/test_report.py (any cwd)."""

import datetime
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import report  # noqa: E402

MB = 1024 * 1024
TRIAGE_KEYS = ["source", "check", "verdict", "detail", "build_id", "out_dir", "log_path",
               "trigger_wall", "first_events"]


def _fmt(sec_offset, start_local):
    return (start_local + datetime.timedelta(seconds=sec_offset)).strftime("%Y-%m-%d %H:%M:%S")


def make_run(out, hours, interval=60, pb_mb_per_h=0.0, handles_per_h=0.0, vram=True,
             threads=lambda i: 40, events=(), exit_info=None, write_log=True, frame_p99=None,
             instrument=True, extra_prelog=()):
    """Synthetic soak dir. events: iterable of (elapsed_s, body-after-prefix). Returns start_local."""
    os.makedirs(os.path.join(out, "collected"), exist_ok=True)
    start_utc = datetime.datetime(2026, 10, 30, 12, 0, 0, tzinfo=datetime.timezone.utc)
    start_local = start_utc.astimezone().replace(tzinfo=None)
    n = int(hours * 3600 / interval) + 1
    with open(os.path.join(out, "soak.csv"), "w", encoding="utf-8", newline="") as f:
        f.write("utc,elapsed_s,private_bytes,working_set,handles,threads,gdi_objects,user_objects,"
                "gpu_dedicated,gpu_shared,nvidia_util,nvidia_temp,nvidia_clk_gr,nvidia_clk_mem,log_offset\n")
        for i in range(n):
            el = i * interval
            h = el / 3600.0
            ts = (start_utc + datetime.timedelta(seconds=el)).strftime("%Y-%m-%dT%H:%M:%SZ")
            pb = int(500 * MB + pb_mb_per_h * h * MB)
            vd = str(int(900 * MB)) if vram else ""
            f.write("%s,%d,%d,%d,%d,%d,10,5,%s,,,,,,0\n" % (ts, el, pb, pb // 2,
                                                             int(2000 + handles_per_h * h), threads(i), vd))
    lines = list(extra_prelog)
    lines += ["%s | === VP LOG SESSION START ===" % _fmt(0, start_local),
              "%s | VPEV1 t=5000000.000 kind=session build_id=BLD123 commit=abc" % _fmt(0, start_local)]
    if instrument:
        lines.append("%s | VPEV1 t=5000000.000 kind=resync level=full cause=start seq=1 mode=shadow"
                     % _fmt(0, start_local))
    if frame_p99:
        sec = 0
        while sec <= hours * 3600:
            lines.append("%s | VPEV1 t=%.3f kind=frame_stats window_s=60.000 n=3600 p50_ms=16.600 p99_ms=%.3f max_ms=30.0"
                         % (_fmt(sec, start_local), 5000000.0 + sec * 1000.0, frame_p99(sec)))
            sec += 600
    for el, body in sorted(events, key=lambda x: x[0]):
        lines.append("%s | %s" % (_fmt(el, start_local), body.replace("{T}", "%.3f" % (5000000.0 + el * 1000.0))))
    if write_log:
        with open(os.path.join(out, "collected", "vp.log"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    if exit_info is not None:
        with open(os.path.join(out, "soak-exit.json"), "w", encoding="utf-8") as f:
            json.dump(exit_info, f)
    return start_local


OK_EXIT = {"pid": 1, "exit_code": 0, "crash": False, "reason": "hours_elapsed"}


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="test-tooling-wave1-w5-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.out = os.path.join(self.tmp, "run")

    def go(self, *args):
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = report.main(["--out", self.out] + list(args))
        with open(os.path.join(self.out, "report.json"), encoding="utf-8") as f:
            rep = json.load(f)
        return code, rep, {c["id"]: c for c in rep["checks"]}

    def full_pass_run(self, **kw):
        args = dict(hours=3, exit_info=OK_EXIT, frame_p99=lambda s: 17.0)
        args.update(kw)
        return make_run(self.out, **args)

    def test_flat_run_all_pass(self):
        self.full_pass_run()
        code, rep, c = self.go()
        self.assertEqual(rep["overall"], "PASS", rep["checks"])
        self.assertEqual(code, 0)
        for cid in ("private_bytes_slope", "vram_slope", "handles_slope", "thread_flat", "gpu_errors",
                    "drops_repeats", "resync_causes", "fallback_notice", "frame_time_creep", "crash"):
            self.assertEqual(c[cid]["verdict"], "PASS", cid)
        self.assertTrue(any(f.startswith("soak-20261030") and f.endswith(".md") for f in os.listdir(self.out)))
        for p in ("plot-memory.svg", "plot-vram.svg", "plot-handles.svg", "plot-frametime.svg"):
            self.assertTrue(os.path.isfile(os.path.join(self.out, p)), p)
        self.assertFalse(os.path.exists(os.path.join(self.out, "triage")))

    def test_leak_slope_fails_and_seed_keys(self):
        self.full_pass_run(pb_mb_per_h=10)
        code, rep, c = self.go()
        self.assertEqual(c["private_bytes_slope"]["verdict"], "FAIL")
        self.assertEqual(rep["overall"], "FAIL")
        self.assertEqual(code, 1)
        with open(os.path.join(self.out, "triage", "private_bytes_slope.json"), encoding="utf-8") as f:
            seed = json.load(f)
        self.assertEqual(list(seed.keys()), TRIAGE_KEYS)
        self.assertEqual(seed["source"], "soak")
        self.assertEqual(seed["check"], "private_bytes_slope")
        self.assertEqual(seed["verdict"], "FAIL")
        self.assertEqual(seed["build_id"], "BLD123")
        self.assertEqual(seed["out_dir"], os.path.abspath(self.out))
        self.assertTrue(seed["log_path"].endswith("vp.log"))
        self.assertEqual(seed["trigger_wall"], "")
        self.assertEqual(seed["first_events"], [])

    def test_handles_and_vram_and_thread_range(self):
        self.full_pass_run(handles_per_h=200, threads=lambda i: 40 + (i % 20))
        _c, _r, c = self.go()
        self.assertEqual(c["handles_slope"]["verdict"], "FAIL")
        self.assertEqual(c["thread_flat"]["verdict"], "FAIL")

    def test_device_removed_fails(self):
        self.full_pass_run(events=[
            (4000, "ERROR: DXGI_ERROR_DEVICE_REMOVED during Present"),
            (5000, "libplacebo [error]: something broke")])
        code, _r, c = self.go()
        self.assertEqual(c["gpu_errors"]["verdict"], "FAIL")
        self.assertEqual(code, 1)
        with open(os.path.join(self.out, "triage", "gpu_errors.json"), encoding="utf-8") as f:
            seed = json.load(f)
        self.assertEqual(list(seed.keys()), TRIAGE_KEYS)
        self.assertEqual(len(seed["first_events"]), 2)
        self.assertEqual(set(seed["first_events"][0].keys()), {"line_no", "raw"})
        self.assertIn("DEVICE_REMOVED", seed["first_events"][0]["raw"])
        self.assertRegex(seed["trigger_wall"], r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d$")

    def test_drops_steady_vs_excluded(self):
        drop = "VPEV1 t={T} kind=resync level=minor cause=missed_frames seq=1 mode=on"
        # 5 drops inside a +/-10 s window around a source change: excluded
        ev = [(4000, "VPEV1 t={T} kind=source_change w=3840 h=2160 rate_num=24000 rate_den=1001 transfer=pq")]
        ev += [(4000 + k, drop) for k in range(-5, 0)]
        self.full_pass_run(events=ev)
        _c, _r, c = self.go()
        self.assertEqual(c["drops_repeats"]["verdict"], "PASS", c["drops_repeats"]["detail"])
        # 5 steady drops over ~2.5 h > 1/h
        shutil.rmtree(self.out)
        self.full_pass_run(events=[(3000 + 900 * k, drop) for k in range(5)])
        _c, _r, c = self.go()
        self.assertEqual(c["drops_repeats"]["verdict"], "FAIL")
        self.assertEqual(len(c["drops_repeats"]["first_events"]), 5)

    def test_warmup_drops_ignored(self):
        drop = "VPEV1 t={T} kind=repeat cause=late count=1"
        self.full_pass_run(events=[(60 * k, drop) for k in range(1, 20)])
        _c, _r, c = self.go()
        self.assertEqual(c["drops_repeats"]["verdict"], "PASS")

    def test_full_resync_without_cause_fails(self):
        self.full_pass_run(events=[
            (4000, "VPEV1 t={T} kind=resync level=full cause=none seq=7 mode=on"),
            (5000, "VPEV1 t={T} kind=resync level=full cause=renderer_reset seq=8 mode=on")])
        _c, _r, c = self.go()
        self.assertEqual(c["resync_causes"]["verdict"], "FAIL")
        self.assertEqual(len(c["resync_causes"]["first_events"]), 1)

    def test_previous_session_in_same_log_ignored(self):
        old = datetime.datetime(2026, 10, 29, 9, 0, 0)
        pre = ["%s | === VP LOG SESSION START ===" % old.strftime("%Y-%m-%d %H:%M:%S"),
               "%s | ERROR: DXGI_ERROR_DEVICE_REMOVED during Present" % old.strftime("%Y-%m-%d %H:%M:%S"),
               "%s | libplacebo [error]: old failure" % old.strftime("%Y-%m-%d %H:%M:%S")]
        self.full_pass_run(extra_prelog=pre)
        # a collected vp.log.0 that is wholly an older session
        with open(os.path.join(self.out, "collected", "vp.log.0"), "w", encoding="utf-8") as f:
            f.write(os.linesep.join(pre) + os.linesep)
        _c, rep, c = self.go()
        self.assertEqual(c["gpu_errors"]["verdict"], "PASS", c["gpu_errors"]["detail"])
        self.assertNotEqual(rep["overall"], "FAIL", rep["checks"])

    def test_no_resync_instrumentation_is_no_data(self):
        self.full_pass_run(instrument=False)
        code, rep, c = self.go()
        self.assertEqual(c["drops_repeats"]["verdict"], "NO DATA")
        self.assertEqual(c["resync_causes"]["verdict"], "NO DATA")
        self.assertEqual(rep["overall"], "INCOMPLETE")
        shutil.rmtree(self.out)
        self.full_pass_run(instrument=True)
        _c, _r, c = self.go()
        self.assertEqual(c["drops_repeats"]["verdict"], "PASS")
        self.assertEqual(c["resync_causes"]["verdict"], "PASS")

    def test_queue_generation_other_is_uncaused(self):
        self.full_pass_run(events=[
            (4000, "VPEV1 t={T} kind=resync level=full cause=queue_generation_other seq=7 mode=on")])
        _c, _r, c = self.go()
        self.assertEqual(c["resync_causes"]["verdict"], "FAIL")
        self.assertIn("queue_generation_other", c["resync_causes"]["detail"])

    def test_fallback_is_no_data(self):
        self.full_pass_run(events=[(4000, 'VPEV1 t={T} kind=fallback state=engaged id=gpu_unpack reason=x')])
        code, rep, c = self.go()
        self.assertEqual(c["fallback_notice"]["verdict"], "NO DATA")
        self.assertIn("notice", c["fallback_notice"]["detail"])
        self.assertEqual(rep["overall"], "INCOMPLETE")
        self.assertEqual(code, 3)

    def test_no_frame_stats_and_short_run_no_data(self):
        make_run(self.out, hours=3, exit_info=OK_EXIT)
        _c, rep, c = self.go()
        self.assertEqual(c["frame_time_creep"]["verdict"], "NO DATA")
        self.assertEqual(rep["overall"], "INCOMPLETE")
        shutil.rmtree(self.out)
        make_run(self.out, hours=0.4, exit_info=OK_EXIT)  # shorter than the 30 min warm-up
        code, rep, c = self.go()
        for cid in ("private_bytes_slope", "vram_slope", "handles_slope", "thread_flat"):
            self.assertEqual(c[cid]["verdict"], "NO DATA", cid)
        self.assertEqual(code, 3)

    def test_creep_needs_two_hours_and_detects_creep(self):
        make_run(self.out, hours=1.5, exit_info=OK_EXIT, frame_p99=lambda s: 17.0)
        _c, _r, c = self.go()
        self.assertEqual(c["frame_time_creep"]["verdict"], "NO DATA")
        shutil.rmtree(self.out)
        make_run(self.out, hours=4, exit_info=OK_EXIT, frame_p99=lambda s: 17.0 + 4.0 * s / 14400.0)
        _c, _r, c = self.go()
        self.assertEqual(c["frame_time_creep"]["verdict"], "FAIL", c["frame_time_creep"]["detail"])

    def test_missing_inputs_no_data(self):
        os.makedirs(self.out)
        code, rep, c = self.go()
        self.assertEqual(code, 3)
        self.assertNotIn("FAIL", [x["verdict"] for x in rep["checks"]])
        for x in rep["checks"]:
            self.assertEqual(x["verdict"], "NO DATA", x["id"])

    def test_crash_fails(self):
        self.full_pass_run(exit_info={"pid": 1, "exit_code": -1073741819, "crash": True, "reason": "process_exited"})
        code, _r, c = self.go()
        self.assertEqual(c["crash"]["verdict"], "FAIL")
        self.assertEqual(code, 1)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "triage", "crash.json")))

    def test_verdict_fail_beats_incomplete(self):
        make_run(self.out, hours=3, pb_mb_per_h=20, exit_info=OK_EXIT)  # leak FAIL + frame NO DATA
        code, rep, _c = self.go()
        self.assertEqual((rep["overall"], code), ("FAIL", 1))

    def test_thresholds_override(self):
        self.full_pass_run(pb_mb_per_h=10)
        thp = os.path.join(self.tmp, "th.json")
        with open(thp, "w", encoding="utf-8") as f:
            json.dump({"private_bytes_mb_per_h": 50, "bogus": 1}, f)
        code, rep, c = self.go("--thresholds", thp)
        self.assertEqual(c["private_bytes_slope"]["verdict"], "PASS")
        self.assertEqual(code, 0)
        self.assertTrue(any("bogus" in n for n in rep["notes"]))
        self.assertEqual(rep["thresholds"]["private_bytes_mb_per_h"], 50)
        # --warmup-min overrides: warm-up beyond the run -> NO DATA
        code, _rep, c = self.go("--thresholds", thp, "--warmup-min", "500")
        self.assertEqual(c["private_bytes_slope"]["verdict"], "NO DATA")

    def test_default_thresholds_file_matches_defaults(self):
        with open(os.path.join(_HERE, "thresholds.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f), report.DEFAULTS)

    def test_linear_slope(self):
        self.assertAlmostEqual(report.linear_slope([0, 1, 2], [1, 3, 5]), 2.0)
        self.assertIsNone(report.linear_slope([1], [1]))
        self.assertIsNone(report.linear_slope([1, 1], [1, 2]))

    def test_bad_out_dir(self):
        self.assertEqual(report.main(["--out", os.path.join(self.tmp, "nope")]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
