# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Offline tests for tools/looprig. Run: python -I -B tools/looprig/test_looprig.py (from any cwd).

Synthetic recordings with known drops, repeats, cadence errors and resync events must be scored correctly.
"""
import copy
import json
import os
import sys
import tempfile
import unittest
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import analyze as az  # noqa: E402
import stress_schedule as ss  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(HERE), "vplog"))
import parse as vplog  # noqa: E402


# ---- helpers -----------------------------------------------------------------------------------
def make_src(n_src, ratio, drop=(), repeat=(), start=0):
    """Captured-position source indices for ratio output periods per source frame."""
    ratio = Fraction(ratio)
    out = []
    for k in range(start, start + n_src):
        if k in drop:
            continue
        n = int((k + 1) * ratio) - int(k * ratio)
        n += 1 if k in repeat else 0
        out += [k] * n
    return out


def stamps(src):
    return [(k, 0, i, 0) for i, k in enumerate(src)]


def sent_rows(n, num, den, start=0, skip=()):
    return [{"index": k, "aux": 0, "pts": Fraction(k * den, num), "t_ms": None} for k in range(start, start + n)
            if k not in skip]


def scenario(**kw):
    sc = dict(az.DEFAULTS)
    sc.update({"id": "t", "out_rate": [60, 1], "required_readers": [], "expect_cadence": {"pattern": "none"}})
    sc.update(kw)
    return sc


def log(lines):
    text = []
    for i, l in enumerate(lines):
        text.append("2026-11-01 10:00:%02d | %s" % (i % 60, l))
    return vplog.parse_lines(text)


def resync_line(t_ms, seq, cause="live_queue_reset", level="full", readers="render-ahead,timeline", injected=1):
    r = ' readers="%s"' % readers if readers is not None else ""
    return "VPEV1 t=%.3f kind=resync level=%s cause=%s seq=%d mode=on injected=%d%s" % (
        t_ms, level, cause, seq, injected, r)


def settle_line(t_ms, seq, settle_ms=500, frames=30, drops=0, repeats=0, relock="yes", vectors="valid"):
    return ("VPEV1 t=%.3f kind=resync_settle seq=%d settle_ms=%d settle_frames=%d drops=%d repeats=%d "
            "relock=%s relock_frames=2 vectors=%s" % (t_ms, seq, settle_ms, frames, drops, repeats, relock, vectors))


def exp_event(eid, kind, at_s, value=None, dv="sdr", input_mode="sdr", **over):
    ev = {"id": eid, "kind": kind, "at_s": at_s, "clock": "s", "group": "random", "command": kind,
          "manual": False, "value": value, "storm_id": None,
          "expect": ss.expectation(kind, value, dv, input_mode)}
    ev["expect"].update(over)
    return ev


def run(src, sent, events=(), expected=None, **sc_kw):
    sc = scenario(**sc_kw)
    return az.analyze(sc, stamps(src), None, sent, log(list(events)), expected)


def rowd(rep, rid):
    for r in rep["rows"]:
        if r["id"] == rid:
            return r
    raise KeyError(rid)


# ---- pacing, drops, repeats --------------------------------------------------------------------
class Pacing(unittest.TestCase):
    def test_clean_native_passes(self):
        rep = run(make_src(600, 1), sent_rows(600, 60, 1))
        self.assertEqual(rowd(rep, "pacing_cadence")["status"], "PASS")
        self.assertEqual(rowd(rep, "drops_repeats")["status"], "PASS")
        self.assertEqual(rep["verdict"], "INCOMPLETE")      # latency has no data; nothing else fails
        self.assertNotIn("FAIL", [r["status"] for r in rep["rows"]])

    def test_clean_24_in_60_pulldown_passes(self):
        sc = dict(out_rate=[60000, 1001])
        src = make_src(480, Fraction(5, 2))
        rep = run(src, sent_rows(480, 24000, 1001), **sc)
        self.assertEqual(rowd(rep, "pacing_cadence")["status"], "PASS", rowd(rep, "pacing_cadence")["detail"])
        self.assertAlmostEqual(rep["ratio_out_per_src"], 2.5, places=4)

    def test_vp_drop_is_counted_as_vp(self):
        src = make_src(600, 1, drop={200})
        rep = run(src, sent_rows(600, 60, 1))
        d = rowd(rep, "drops_repeats")
        self.assertEqual(d["status"], "FAIL")
        self.assertEqual((d["vp_dropped"], d["source_dropped"]), (1, 0))

    def test_source_drop_is_not_vp(self):
        src = make_src(600, 1, drop={200})
        rep = run(src, sent_rows(600, 60, 1, skip={200}))
        d = rowd(rep, "drops_repeats")
        self.assertEqual(d["status"], "PASS")
        self.assertEqual((d["vp_dropped"], d["source_dropped"]), (0, 1))

    def test_extra_repeat_fails_and_counts(self):
        src = make_src(600, 1, repeat={300})
        rep = run(src, sent_rows(600, 60, 1))
        d = rowd(rep, "drops_repeats")
        self.assertEqual((d["status"], d["vp_repeated"]), ("FAIL", 1))

    def test_repeat_allowance_scales_with_hours(self):
        # 3 hours of steady play at 60 fps would allow 3 VP-caused corrections at 1/h; one drop passes
        src = make_src(60 * 3600 * 3 // 60, 1, drop={5000})       # 3 h at 1 capture per 1/60 s -> 648000 frames /60
        rep = run(src, sent_rows(len(src) + 1, 60, 1), max_vp_per_hour=1.0)
        d = rowd(rep, "drops_repeats")
        self.assertEqual(d["allowed"], int(len(src) / 60 / 3600))

    def test_cadence_error_detected(self):
        # 2:3 pulldown that turns into 3:3 for a stretch
        seq = []
        for k in range(200):
            seq += [k] * (3 if 80 <= k < 120 else (2 if k % 2 == 0 else 3))
        rep = run(seq, sent_rows(200, 24000, 1001), out_rate=[60000, 1001])
        p = rowd(rep, "pacing_cadence")
        self.assertEqual(p["status"], "FAIL")
        self.assertGreater(p["errors"], 0)

    def test_backwards_jump_flagged(self):
        src = make_src(300, 1) + make_src(100, 1, start=50)
        rep = run(src, sent_rows(400, 60, 1))
        self.assertEqual(rowd(rep, "order")["status"], "FAIL")

    def test_capture_loss_detected(self):
        src = make_src(300, 1)
        st = stamps(src)
        st = st[:100] + st[103:]                    # output counter jumps by 3: capture lost frames
        rep = az.analyze(scenario(), st, None, sent_rows(300, 60, 1), log([]))
        c = rowd(rep, "capture_integrity")
        self.assertEqual((c["status"], c["capture_lost"]), ("FAIL", 3))

    def test_undecoded_fraction(self):
        src = make_src(300, 1)
        src[10] = None
        src[11] = None
        rep = run(src, sent_rows(300, 60, 1))
        self.assertEqual(rowd(rep, "undecoded_frames")["status"], "FAIL")

    def test_rate_is_derived_not_fixed(self):
        # a 25 Hz source into a 50 Hz output (2 periods each) scores clean with no rate in the code
        rep = run(make_src(300, 2), sent_rows(300, 25, 1), out_rate=[50, 1])
        self.assertEqual(rowd(rep, "pacing_cadence")["status"], "PASS")
        self.assertAlmostEqual(rep["src_fps"], 25.0)


# ---- latency -----------------------------------------------------------------------------------
class Latency(unittest.TestCase):
    def sent_with_t(self, n, period_ms=1000.0 / 60):
        rows = sent_rows(n, 60, 1)
        for r in rows:
            r["t_ms"] = 5000.0 + r["index"] * period_ms
        return rows

    def test_latency_pass_and_fail(self):
        src = make_src(300, 1)
        period = 1000.0 / 60
        sc = scenario(latency_mode="low_latency")
        ok = az.analyze(sc, stamps(src), None, self.sent_with_t(300), log([]), capture_t0=None) \
            if False else az.analyze(sc, stamps(src), None, self.sent_with_t(300), log([]), None, None,
                                     5000.0 + 2 * period)
        self.assertEqual(rowd(ok, "latency")["status"], "PASS")
        late = az.analyze(sc, stamps(src), None, self.sent_with_t(300), log([]), None, None, 5000.0 + 6 * period)
        self.assertEqual(rowd(late, "latency")["status"], "FAIL")
        self.assertAlmostEqual(rowd(ok, "latency")["median_ms"], 2 * period, places=1)

    def test_latency_no_data_without_clock(self):
        rep = run(make_src(100, 1), sent_rows(100, 60, 1))
        self.assertEqual(rowd(rep, "latency")["status"], "NO DATA")


# ---- cadence lock ------------------------------------------------------------------------------
class CadenceLock(unittest.TestCase):
    def base(self, events, pattern="2:3"):
        src = make_src(600, Fraction(5, 2))
        anchor = resync_line(0.0, 0, cause="start", readers=None, injected=0)
        return az.analyze(scenario(out_rate=[60000, 1001], expect_cadence={"pattern": pattern}),
                          stamps(src), None, sent_rows(600, 24000, 1001), log([anchor] + events))

    def test_locks_in_time(self):
        rep = self.base(["VPEV1 t=500.000 kind=cadence state=locked pattern=2:3"])
        self.assertEqual(rowd(rep, "cadence_lock")["status"], "PASS")
        self.assertEqual(rowd(rep, "cadence_final")["status"], "PASS")

    def test_late_lock_fails(self):
        rep = self.base(["VPEV1 t=9000.000 kind=cadence state=locked pattern=2:3"])
        self.assertEqual(rowd(rep, "cadence_lock")["status"], "FAIL")

    def test_never_locks_fails(self):
        rep = self.base(["VPEV1 t=500.000 kind=cadence state=lost pattern=none"])
        self.assertEqual(rowd(rep, "cadence_lock")["status"], "FAIL")

    def test_rotation_is_same_pattern(self):
        rep = self.base(["VPEV1 t=500.000 kind=cadence state=locked pattern=3:2"])
        self.assertEqual(rowd(rep, "cadence_lock")["status"], "PASS")

    def test_false_lock_on_nature_content(self):
        rep = self.base(["VPEV1 t=500.000 kind=cadence state=locked pattern=2:3"], pattern="none")
        self.assertEqual(rowd(rep, "cadence_lock")["status"], "FAIL")

    def test_lost_outside_events_fails(self):
        rep = self.base(["VPEV1 t=500.000 kind=cadence state=locked pattern=2:3",
                         "VPEV1 t=5000.000 kind=cadence state=lost pattern=2:3"])
        self.assertEqual(rowd(rep, "cadence_steady")["status"], "FAIL")
        self.assertEqual(rowd(rep, "cadence_final")["status"], "FAIL")


# ---- per-resync-event scoring ------------------------------------------------------------------
class ResyncEvents(unittest.TestCase):
    def setUp(self):
        # 60 fps source, 60 Hz output; a seek at 5 s = source frame 300; a rebuild at 15 s = frame 900
        self.sent = sent_rows(1500, 60, 1)
        self.src = make_src(1500, 1, drop={301})
        self.exp = {"schema": ss.SCHEMA, "events": [exp_event("E1", "seek", 5.0)]}
        self.log = [resync_line(5000.0, 300), settle_line(5500.0, 300, drops=1)]

    def event(self, rep, eid="E1"):
        return next(e for e in rep["events"] if e["id"] == eid)

    def go(self, log=None, src=None, expected=None, **kw):
        return run(src if src is not None else self.src, self.sent, log if log is not None else self.log,
                   expected or self.exp, **kw)

    def test_good_event_passes_all_checks(self):
        rep = self.go()
        ev = self.event(rep)
        self.assertEqual(ev["status"], "PASS", json.dumps(ev["checks"]))
        self.assertEqual(rep["summary"]["events_pass"], 1)
        self.assertEqual(ev["drops"], 1)
        self.assertEqual(rowd(rep, "drops_repeats")["status"], "PASS")         # the drop is event-caused
        self.assertEqual(rowd(rep, "unexplained_resyncs")["status"], "PASS")

    def test_slow_settle_fails(self):
        rep = self.go(log=[resync_line(5000.0, 300), settle_line(9000.0, 300, settle_ms=4000)])
        self.assertEqual(self.event(rep)["checks"]["settle"]["status"], "FAIL")

    def test_wrong_cause_fails(self):
        rep = self.go(log=[resync_line(5000.0, 300, cause="renderer_reset"), settle_line(5500.0, 300)])
        # seek accepts live_queue_reset/file_discontinuity only
        self.assertEqual(self.event(rep)["checks"]["logged"]["status"], "FAIL")

    def test_no_resync_line_fails(self):
        rep = self.go(log=[])
        self.assertEqual(self.event(rep)["checks"]["logged"]["status"], "FAIL")
        self.assertEqual(rep["verdict"], "FAIL")

    def test_too_many_drops_in_window_fail(self):
        exp = copy.deepcopy(self.exp)
        exp["events"][0]["kind"] = "reset"                      # no discontinuity allowance
        exp["events"][0]["expect"] = ss.expectation("reset", None, "sdr", "sdr")
        src = make_src(1500, 1, drop={301, 302, 303, 304, 305, 306})
        rep = self.go(log=[resync_line(5000.0, 300, cause="renderer_reset"), settle_line(5500.0, 300)], src=src,
                      expected=exp)
        self.assertEqual(self.event(rep)["checks"]["drops"]["status"], "FAIL")

    def test_extra_repeats_in_window_fail(self):
        exp = copy.deepcopy(self.exp)
        src = make_src(1500, 1, repeat={301, 302, 303, 304, 305, 306})
        rep = self.go(src=src, expected=exp)
        self.assertEqual(self.event(rep)["checks"]["repeats"]["status"], "FAIL")

    def test_relock_no_fails(self):
        rep = self.go(log=[resync_line(5000.0, 300), settle_line(5500.0, 300, relock="no")])
        self.assertEqual(self.event(rep)["checks"]["relock"]["status"], "FAIL")

    def test_unsettled_vectors_fail(self):
        rep = self.go(log=[resync_line(5000.0, 300), settle_line(5500.0, 300, vectors="invalid")])
        self.assertEqual(self.event(rep)["checks"]["stale_vectors"]["status"], "FAIL")
        self.assertEqual(self.event(rep)["checks"]["settle"]["status"], "FAIL")

    def test_consumers_missing_reader_fails(self):
        rep = self.go(log=[resync_line(5000.0, 300, readers="timeline"), settle_line(5500.0, 300)],
                      required_readers=["render-ahead", "timeline"])
        c = self.event(rep)["checks"]["consumers"]
        self.assertEqual(c["status"], "FAIL")
        self.assertIn("render-ahead", c["detail"])

    def test_consumers_field_absent_is_no_data(self):
        rep = self.go(log=[resync_line(5000.0, 300, readers=None), settle_line(5500.0, 300)])
        self.assertEqual(self.event(rep)["checks"]["consumers"]["status"], "NO DATA")
        self.assertEqual(rep["verdict"], "INCOMPLETE")

    def test_missing_settle_line_is_no_data(self):
        rep = self.go(log=[resync_line(5000.0, 300)])
        self.assertEqual(self.event(rep)["checks"]["settle"]["status"], "NO DATA")

    def test_unexplained_full_resync_fails(self):
        extra = resync_line(9000.0, 540, cause="renderer_reset", injected=0)
        rep = self.go(log=self.log + [extra])
        u = rowd(rep, "unexplained_resyncs")
        self.assertEqual((u["status"], u["count"]), ("FAIL", 1))

    def test_start_resync_is_not_unexplained(self):
        start = resync_line(0.0, 0, cause="start", injected=0)
        rep = self.go(log=[start] + self.log)
        self.assertEqual(rowd(rep, "unexplained_resyncs")["status"], "PASS")

    def test_matching_prefers_the_nearer_event(self):
        exp = {"schema": ss.SCHEMA, "events": [exp_event("E1", "seek", 5.0), exp_event("E2", "seek", 15.0)]}
        log_ = [resync_line(5000.0, 300), settle_line(5500.0, 300, drops=1),
                resync_line(15000.0, 900), settle_line(15500.0, 900)]
        rep = self.go(log=log_, expected=exp)
        self.assertEqual(rep["summary"]["events_pass"], 2, json.dumps(rep["events"])[:600])

    def test_gap_source_loss_is_allowed(self):
        exp = {"schema": ss.SCHEMA, "events": [exp_event("E1", "gap", 5.0, 3)]}
        src = make_src(1500, 1, drop={300, 301, 302})
        sent = sent_rows(1500, 60, 1, skip={300, 301, 302})
        log_ = [resync_line(5000.0, 300, cause="missed_frames", level="minor", readers=None)]
        rep = run(src, sent, log_, exp)
        self.assertEqual(self.event(rep)["status"], "PASS", json.dumps(self.event(rep)["checks"]))

    def test_storm_notice_required(self):
        exp = {"schema": ss.SCHEMA, "events": [exp_event("E1", "seek", 5.0, expect_fallback="resync_storm")]}
        rep = self.go(expected=exp)
        self.assertEqual(self.event(rep)["checks"]["storm_notice"]["status"], "FAIL")
        note = "VPEV1 t=5200.000 kind=fallback state=engaged id=resync_storm reason=\"5 in 8 s\""
        rep = self.go(log=self.log + [note], expected=exp)
        self.assertEqual(self.event(rep)["checks"]["storm_notice"]["status"], "PASS")

    def test_dv_relatch(self):
        exp = {"schema": ss.SCHEMA, "events": [exp_event("E1", "dv_switch", 5.0, "lldv", dv="lldv", input_mode="dv_tunnel")]}
        log_ = [resync_line(5000.0, 300, cause="source_format_changed"), settle_line(5500.0, 300),
                "VPEV1 t=5100.000 kind=source_change w=1920 h=1080 rate_num=60 rate_den=1 transfer=pq dv=lldv"]
        rep = self.go(log=log_, expected=exp)
        self.assertEqual(self.event(rep)["checks"]["dv_relatch"]["status"], "PASS")
        bad = [log_[0], log_[1], log_[2].replace("dv=lldv", "dv=tunnel")]
        rep = self.go(log=bad, expected=exp)
        self.assertEqual(self.event(rep)["checks"]["dv_relatch"]["status"], "FAIL")
        rep = self.go(log=log_[:2], expected=exp)
        self.assertEqual(self.event(rep)["checks"]["dv_relatch"]["status"], "NO DATA")

    def test_forward_seek_skip_not_charged_to_vp(self):
        src = make_src(300, 1) + make_src(1000, 1, start=360)    # seek skips 60 source frames
        sent = sent_rows(1400, 60, 1)
        log_ = [resync_line(5000.0, 360), settle_line(5500.0, 360)]
        rep = run(src, sent, log_, self.exp)
        self.assertEqual(rowd(rep, "drops_repeats")["status"], "PASS", rowd(rep, "drops_repeats")["detail"])
        self.assertEqual(self.event(rep)["checks"]["drops"]["status"], "INFO")

    def test_index_from_time_when_no_seq_anchor_between(self):
        # cadence line carries no seq; it is placed by its t_ms relative to the nearest resync anchor
        events = log([resync_line(5000.0, 300), "VPEV1 t=6000.000 kind=cadence state=lost pattern=2:3"])
        idx = az.assign_indices(events, 60.0)
        self.assertAlmostEqual(idx[1], 360.0)


# ---- levels through the analyser (small raw-free path) -----------------------------------------
class Pipeline(unittest.TestCase):
    def test_y4m_end_to_end(self):
        import numpy as np
        import framestamp as fs
        w, h = 480, 270
        n = 40
        src_seq = make_src(n, 1, drop={20})
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "rec.y4m")
            with open(path, "wb") as f:
                f.write(("YUV4MPEG2 W%d H%d F60:1 Ip A1:1 C422p10\n" % (w, h)).encode())
                for pos, k in enumerate(src_seq):
                    y = np.full((h, w), 512, dtype=np.uint16)
                    fs.write(y, 10, False, fs.CORNER_SOURCE, fs.word(fs.KIND_SOURCE, 0, k))
                    fs.write(y, 10, False, fs.CORNER_OUTPUT, fs.word(fs.KIND_OUTPUT, k & 63, pos))
                    cb = np.full((h, w // 2), 512, dtype=np.uint16)
                    f.write(b"FRAME\n" + y.astype("<u2").tobytes() + cb.tobytes() + cb.tobytes())
            sent = os.path.join(d, "sent.csv")
            with open(sent, "w") as f:
                f.write("index,aux,pts_num,pts_den\n")
                for k in range(n):
                    f.write("%d,0,%d,60\n" % (k, k))
            logp = os.path.join(d, "vp.log")
            open(logp, "w").write("")
            scen = os.path.join(d, "scenarios.json")
            json.dump({"defaults": {"min_segment_frames": 4}, "scenarios": [{"id": "x", "out_rate": [60, 1]}]},
                      open(scen, "w"))
            outd = os.path.join(d, "out")
            code = az.main(["--scenario", "x", "--scenarios", scen, "--recording", path, "--sent", sent,
                            "--log", logp, "--out", outd])
            rep = json.load(open(os.path.join(outd, "report.json")))
            self.assertEqual(code, 1)
            r = {x["id"]: x for x in rep["rows"]}
            self.assertEqual(r["drops_repeats"]["vp_dropped"], 1)
            self.assertEqual(r["capture_integrity"]["capture_lost"], 0)
            self.assertEqual(r["undecoded_frames"]["status"], "PASS")
            self.assertTrue(os.path.isfile(os.path.join(outd, "report.md")))

    def test_scenarios_file_loads(self):
        doc = json.load(open(os.path.join(HERE, "scenarios.json")))
        ids = [s["id"] for s in doc["scenarios"]]
        self.assertEqual(len(ids), len(set(ids)))
        for s in doc["scenarios"]:
            sc = az.load_scenario(os.path.join(HERE, "scenarios.json"), s["id"])
            self.assertIn("out_rate", sc)
        self.assertTrue(any(s.get("levels") for s in doc["scenarios"]))

    def test_levels_row_with_synthetic_frames(self):
        import numpy as np
        import patterns
        layout = patterns.build_layout("levels10", 640, 360)
        fr = patterns.render_frame(layout)
        frames = lambda: iter([fr] * 3)          # noqa: E731
        rows = az.levels_rows(frames, [0, 1, 2], {"levels": {"set": "levels10", "src": "bt1886:bt709:limited:10"}},
                              None)
        self.assertEqual(rows[0]["status"], "PASS", rows)
        names = [r["id"] for r in rows]
        self.assertIn("levels_btb_reached", names)
        self.assertIn("levels_wtw_reached", names)
        bad = (np.minimum(fr[0], 64),) + tuple(fr[1:])           # BTB clipped at nominal black
        rows = az.levels_rows(lambda: iter([bad] * 3), [0, 1, 2],
                              {"levels": {"set": "levels10", "src": "bt1886:bt709:limited:10"}}, None)
        self.assertEqual(rows[0]["status"], "FAIL")


# ---- stress schedule ---------------------------------------------------------------------------
class StressSchedule(unittest.TestCase):
    def test_deterministic_per_seed(self):
        a = ss.generate(7, "random", "dv_tunnel", 3600)
        b = ss.generate(7, "random", "dv_tunnel", 3600)
        c = ss.generate(8, "random", "dv_tunnel", 3600)
        self.assertEqual(json.dumps(a), json.dumps(b))
        self.assertNotEqual(json.dumps(a), json.dumps(c))

    def test_every_group_and_input_is_valid(self):
        for g in ss.GROUPS:
            for i in ss.INPUTS:
                s = ss.generate(3, g, i, 3600)
                self.assertTrue(s["events"], (g, i))
                self.assertEqual(ss.check_schedule(s), [], (g, i))

    def test_owner_directive_groups_all_present_for_dv_tunnel(self):
        plan = dict(ss.suite(1, ss.GROUPS, ["dv_tunnel"], 3600))
        self.assertEqual(set(plan), {"%s_dv_tunnel" % g for g in ss.GROUPS})
        kinds = {e["kind"] for s in plan.values() for e in s["events"]}
        for need in ("seek", "reset", "inplace", "rebuild", "gap", "rate", "stall", "capture_restart", "dv_switch",
                     "rpu_loss", "rpu_return", "tunnel_handoff", "signal_loss", "signal_return", "overflow",
                     "minor", "full", "manual_replug"):
            self.assertIn(need, kinds)
        for s in plan.values():
            for e in s["events"]:
                if e["expect"]["level"] == "full":
                    self.assertTrue(e["expect"]["dv_relatch"], e["id"])

    def test_dv_group_covers_all_twelve_transitions(self):
        s = ss.generate(5, "dv", "lldv", 7200)
        cur = s["initial_dv"]
        seen = set()
        for e in s["events"]:
            if e["kind"] == "dv_switch":
                seen.add((cur, e["value"]))
                cur = e["value"]
        self.assertEqual(len(seen), 12)
        self.assertTrue(all(e["expect"]["dv_relatch"] for e in s["events"] if e["kind"] == "dv_switch"))
        # dv_after tracks the walk
        dvs = [e["expect"]["dv_after"] for e in s["events"] if e["kind"] == "dv_switch"]
        self.assertEqual(dvs, [e["value"] for e in s["events"] if e["kind"] == "dv_switch"])

    def test_atv_switch_covers_every_rate_and_mode_manual(self):
        s = ss.generate(1, "atv_switch", "sdr", 4000, manual_atv=True)
        notes = [e["note"] for e in s["events"]]
        for r in ss.ATV_RATES:
            for m in ss.ATV_MODES:
                self.assertTrue(any(("%s Hz" % r) in n and m.upper() in n for n in notes), (r, m))
        self.assertTrue(all(e["command"] is None for e in s["events"]))

    def test_drift_includes_ntsc_ratio_and_returns_to_zero(self):
        s = ss.generate(2, "drift", "sdr", 3600)
        vals = [e["value"] for e in s["events"]]
        self.assertIn(ss.ntsc_ppm(), vals)
        self.assertIn(-ss.ntsc_ppm(), vals)
        self.assertEqual(vals[-1], 0)
        self.assertEqual(ss.ntsc_ppm(), 999)

    def test_storm_bursts_fit_the_window_and_raise_notice(self):
        s = ss.generate(4, "storm", "sdr", 3600)
        bursts = {}
        for e in s["events"]:
            bursts.setdefault(e["storm_id"], []).append(e)
        self.assertGreater(len(bursts), 3)
        for sid, evs in bursts.items():
            self.assertLessEqual(evs[-1]["at_s"] - evs[0]["at_s"], ss.STORM_SPAN_S)
            self.assertEqual(any("expect_fallback" in e["expect"] for e in evs), len(evs) >= ss.STORM_MIN_NOTICE)

    def test_seek_group_uses_the_sequence_clock(self):
        s = ss.generate(1, "seek", "sdr", 3600)
        self.assertTrue(all(e["clock"] == "seq" for e in s["events"]))
        self.assertIn("at #", ss.to_schedule_text(s))
        self.assertEqual(sum(1 for e in s["events"] if e["kind"] == "seek"), 20)

    def test_command_validator_matches_the_cpp_rules(self):
        good = ["seek", "gap 3", "rate -999", "rate 0", "stall 300", "dv_switch lldv", "rpu_loss 10", "rpu_return",
                "tunnel_handoff", "signal_loss 2500", "signal_return", "minor", "minor 4", "full", "overflow 7"]
        for c in good:
            ss.validate_command(c)
        bad = ["", "seek 3", "gap", "gap 0", "gap 1001", "gap -1", "rate 1000001", "dv_switch", "dv_switch dolby",
               "stall 60001", "overflow 1001", "signal_loss 600001", "rpu_loss 100001", "bogus", "gap 1 2", "gap 1x",
               "minor 1001", "full 3"]
        for c in bad:
            with self.assertRaises(ss.ScheduleError, msg=c):
                ss.validate_command(c)

    def test_schedule_text_lines_parse(self):
        import re
        s = ss.generate(9, "random", "dv_tunnel", 3600)
        text = ss.to_schedule_text(s)
        times = []
        for line in text.splitlines():
            if not line or line.startswith("#") or line.startswith("seed "):
                continue
            m = re.match(r"^at (\d+(?:\.\d+)?)s (.+)$", line)
            self.assertIsNotNone(m, line)
            ss.validate_command(m.group(2))
            times.append(float(m.group(1)))
        self.assertEqual(times, sorted(times))
        self.assertIn("seed 9", text)

    def test_files_written_and_expected_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            code = ss.main(["--out", d, "--seed", "3", "--duration", "1800", "--groups", "dv,storm",
                            "--inputs", "dv_tunnel"])
            self.assertEqual(code, 0)
            sub = os.path.join(d, "dv_dv_tunnel")
            self.assertEqual(sorted(os.listdir(sub)), ["commands.txt", "expected.json", "schedule.txt"])
            doc = json.load(open(os.path.join(sub, "expected.json")))
            self.assertEqual(doc["schema"], ss.SCHEMA)
            self.assertIn("resync.inject", open(os.path.join(sub, "commands.txt")).read())

    def test_short_duration_rejected(self):
        with self.assertRaises(ss.ScheduleError):
            ss.generate(1, "random", "sdr", 20)

    def test_generated_expected_scores_end_to_end(self):
        """A schedule from the generator, replayed as a perfect synthetic run, scores PASS for every event."""
        s = ss.generate(11, "random", "sdr", 600)
        fps = 60.0
        sent = sent_rows(int(600 * fps) + 600, 60, 1)
        lines = []
        t_ms = 0.0
        lost = set()
        for e in s["events"]:
            idx = int(round(e["at_s"] * fps))
            t_ms = e["at_s"] * 1000.0
            exp = e["expect"]
            if exp["level"] in ("optional",):
                continue
            cause = exp["causes"][0]
            lines.append(resync_line(t_ms, idx, cause=cause, level=exp["level"]))
            if exp["level"] in ("full", "timing"):
                lines.append(settle_line(t_ms + 400, idx))
            if e["kind"] == "gap":
                lost.update(range(idx, idx + int(e["value"])))
            if e["kind"] == "minor" and e["value"]:
                lost.update(range(idx, idx + int(e["value"])))
        sent = [r for r in sent if r["index"] not in lost]
        src = [r["index"] for r in sent if r["index"] < 600 * fps]
        rep = run(src, sent, lines, s)
        self.assertEqual(rep["summary"]["events_fail"], 0, [e for e in rep["events"] if e["status"] == "FAIL"][:2])
        self.assertNotIn("FAIL", [r["status"] for r in rep["rows"]],
                         [r for r in rep["rows"] if r["status"] == "FAIL"][:3])


if __name__ == "__main__":
    unittest.main()
