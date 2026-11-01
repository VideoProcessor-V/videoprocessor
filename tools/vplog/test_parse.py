# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for parse.py. Run: python -I -B tools/vplog/test_parse.py (any cwd)."""

import datetime
import io
import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import parse  # noqa: E402

FIX = os.path.join(HERE, "fixtures")


def fx(name):
    return os.path.join(FIX, name)


def one(line):
    events = parse.parse_lines([line])
    assert len(events) == 1, events
    return events[0]


class Goldens(unittest.TestCase):
    def test_g1_resync(self):
        e = one("VPEV1 t=1234.500 kind=resync level=minor cause=missed_frames seq=42 gap=3 mode=shadow")
        self.assertEqual(e.kind, "resync")
        self.assertEqual(e.t_ms, 1234.5)
        self.assertEqual(e.source, "vpev")
        self.assertFalse(e.legacy)
        self.assertEqual(e.fields, {"level": "minor", "cause": "missed_frames", "seq": "42",
                                    "gap": "3", "mode": "shadow"})
        self.assertEqual(list(e.fields), ["level", "cause", "seq", "gap", "mode"])
        self.assertEqual(parse.category(e), "drop")

    def test_g2_flag_space(self):
        e = one('VPEV1 t=0.000 kind=flag source=hotkey tag="needs space"')
        self.assertEqual(e.fields, {"source": "hotkey", "tag": "needs space"})
        self.assertEqual(e.t_ms, 0.0)

    def test_g3_escapes(self):
        e = one(r'VPEV1 t=5.250 kind=fallback state=engaged id="capture GPU unpack" '
                r'reason="CPU formatters: \"x\" \\ y"')
        self.assertEqual(e.kind, "fallback")
        self.assertEqual(e.fields["state"], "engaged")
        self.assertEqual(e.fields["id"], "capture GPU unpack")
        self.assertEqual(e.fields["reason"], 'CPU formatters: "x" \\ y')

    def test_g4_empty_value(self):
        e = one('VPEV1 t=1.000 kind=gate from=a to=b reason=""')
        self.assertEqual(e.fields, {"from": "a", "to": "b", "reason": ""})

    def test_g5_double_space_kept(self):
        e = one('VPEV1 t=2.000 kind=flag source=s tag="a  b c"')
        self.assertEqual(e.fields["tag"], "a  b c")

    def test_g6_frame_stats(self):
        e = one("VPEV1 t=60000.125 kind=frame_stats window_s=60.000 n=3600 p50_ms=16.667 "
                "p99_ms=17.250 max_ms=33.400")
        self.assertEqual(e.t_ms, 60000.125)
        self.assertEqual(e.fields, {"window_s": "60.000", "n": "3600", "p50_ms": "16.667",
                                    "p99_ms": "17.250", "max_ms": "33.400"})
        self.assertEqual(parse.category(e), "other")


class Malformed(unittest.TestCase):
    def reason(self, line):
        e = one(line)
        self.assertEqual(e.kind, "malformed", line)
        self.assertEqual(e.source, "vpev")
        return e.fields["reason"]

    def test_each_reason(self):
        cases = {
            "missing_t": "VPEV1 kind=flag source=s",
            "missing_kind": "VPEV1 t=1.000 source=s tag=x",
            "bad_token_order": "VPEV1 kind=flag t=1.000 source=s",
            "unknown_kind": "VPEV1 t=1.000 kind=bogus a=1",
            "bad_key": "VPEV1 t=1.000 kind=flag Tag=x",
            "bad_quoting": 'VPEV1 t=1.000 kind=flag tag="bad \\q"',
            "unbalanced_quote": 'VPEV1 t=1.000 kind=flag tag="open',
            "bad_value": "VPEV1 t=1.000 kind=flag tag=a$b",
            "duplicate_key": "VPEV1 t=1.000 kind=flag tag=a tag=b",
            "bad_t": "VPEV1 t=abc kind=flag source=s",
        }
        for reason, line in cases.items():
            with self.subTest(reason=reason):
                self.assertEqual(self.reason(line), reason)

    def test_key_without_equals(self):
        self.assertEqual(self.reason("VPEV1 t=1.000 kind=flag lonely"), "bad_key")

    def test_quote_followed_by_text(self):
        self.assertEqual(self.reason('VPEV1 t=1.000 kind=flag tag="a"b'), "bad_quoting")

    def test_bare_empty_value(self):
        self.assertEqual(self.reason("VPEV1 t=1.000 kind=flag tag="), "bad_value")

    def test_malformed_keeps_t_when_known(self):
        self.assertEqual(one("VPEV1 t=14.000 kind=bogus a=1").t_ms, 14.0)

    def test_not_vpev1_token(self):
        self.assertEqual(len(parse.parse_lines(["VPEV10 t=1.000 kind=flag"])), 0)

    def test_fixture_counts(self):
        events = parse.parse_file(fx("vpev1_all_kinds.log"))
        mal = [e for e in events if e.kind == "malformed"]
        self.assertEqual(len(mal), 9)
        self.assertEqual(sorted(set(e.fields["reason"] for e in mal)),
                         sorted(["bad_key", "bad_quoting", "bad_token_order", "duplicate_key",
                                 "missing_kind", "missing_t", "unbalanced_quote", "unknown_kind"]))
        self.assertEqual(parse.summarize(events)["malformed"], 9)


class Prefix(unittest.TestCase):
    def test_with_and_without(self):
        body = "VPEV1 t=3.000 kind=flag source=s tag=x"
        a = one(body)
        b = one("2026-10-31 20:01:02 | " + body)
        self.assertIsNone(a.wall)
        self.assertEqual(b.wall, datetime.datetime(2026, 10, 31, 20, 1, 2))
        self.assertEqual(a.fields, b.fields)
        self.assertEqual(b.raw, "2026-10-31 20:01:02 | " + body)
        self.assertEqual(b.line_no, 1)

    def test_all_kinds_fixture(self):
        events = parse.parse_file(fx("vpev1_all_kinds.log"))
        kinds = set(e.kind for e in events)
        self.assertTrue(parse.KINDS <= kinds, parse.KINDS - kinds)
        self.assertEqual(len(events), 27)
        self.assertEqual(events.skipped, 1)  # the comment line
        walls = [e for e in events if e.wall is not None]
        self.assertGreaterEqual(len(walls), 2)
        self.assertGreater(len(events) - len(walls), 10)

    def test_trunc_field(self):
        e = one('VPEV1 t=12.000 kind=flag source=s tag="long" trunc=1')
        self.assertEqual(e.fields["trunc"], "1")


class LegacyAdapters(unittest.TestCase):
    def setUp(self):
        self.real = parse.parse_file(fx("real_renderer_harness.log"))
        self.fmt = parse.parse_file(fx("format_strings_prefixed.log"))

    def of(self, events, kind):
        return [e for e in events if e.kind == kind]

    def test_build_identity_real(self):
        e = self.of(self.real, "session")[0]
        self.assertEqual(e.line_no, 1)
        self.assertTrue(e.legacy)
        self.assertEqual(e.fields["legacy"], "1")
        self.assertEqual(e.fields["module"], "renderer")
        self.assertTrue(e.fields["build_id"].startswith("v1.4.00-beta-"))
        self.assertEqual(len(e.fields["commit"]), 40)

    def test_session_start_and_identity_format(self):
        s = self.of(self.fmt, "session")
        self.assertEqual(s[0].fields["build_id"], "")
        self.assertEqual(s[1].fields["build_id"], "v1.4.00-beta-70d668d")
        self.assertEqual(s[1].fields["module"], "host")
        self.assertEqual(s[1].wall, datetime.datetime(2026, 10, 31, 19, 55, 1))

    def test_queue_generation_real(self):
        e = [x for x in self.of(self.real, "resync")][0]
        self.assertTrue(e.legacy)
        self.assertEqual(e.fields["level"], "full")
        self.assertEqual(e.fields["cause"], "start")
        self.assertEqual(e.fields["generation"], "1")
        self.assertEqual(e.fields["prefill_target"], "4")
        self.assertEqual(parse.category(e), "resync")

    def test_queue_generation_unknown_reason(self):
        e = one("Alpha queue generation armed: generation=2 reason=weird_thing prefill_target=4")
        self.assertEqual(e.fields["cause"], "queue_generation_other")
        self.assertEqual(e.fields["detail"], "weird_thing")

    def test_resync_lines(self):
        r = [e for e in self.of(self.fmt, "resync") if "seq" in e.fields]
        self.assertEqual(len(r), 3)
        self.assertEqual(r[0].fields, {"level": "full", "cause": "start", "seq": "1",
                                       "mode": "shadow", "legacy": "1"})
        self.assertEqual(r[1].fields["cause"], "missed_frames")
        self.assertEqual(r[1].fields["gap"], "3")
        self.assertEqual(r[1].fields["level"], "minor")
        self.assertEqual(parse.category(r[1]), "drop")
        self.assertEqual(r[2].fields["cause"], "rate_step")
        self.assertEqual(r[2].fields["mode"], "on")
        self.assertEqual(parse.category(r[2]), "timing")

    def test_gate_change_and_reset(self):
        g = self.of(self.fmt, "gate")
        self.assertEqual(g[0].fields["from"], "full")
        self.assertEqual(g[0].fields["to"], "degraded")
        self.assertEqual(g[0].fields["reason"], "extra too slow")
        self.assertEqual(g[1].fields, {"from": "degraded", "to": "reset", "reason": "queue epoch",
                                       "legacy": "1"})

    def test_fallbacks_real(self):
        f = self.of(self.real, "fallback")
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].fields["state"], "engaged")
        self.assertEqual(f[0].fields["id"], "refresh rate")
        self.assertTrue(f[0].fields["reason"].startswith("kept 59.950 Hz for 23.976 Hz input"))

    def test_fallbacks_multiple_and_none(self):
        f = self.of(self.fmt, "fallback")
        self.assertEqual([x.fields["id"] for x in f], ["refresh rate", "entrance once"])
        self.assertEqual(f[1].fields["reason"], "nested (inner) reason")
        # the 'none' line produced nothing
        self.assertEqual(len([e for e in self.fmt if e.line_no == 11]), 0)

    def test_fallbacks_embedded_and_nested(self):
        e = parse.parse_lines([
            "Fallbacks engaged: entrance once (B's vectors not ready ahead (no frame after next "
            "(short queue)); B per picture; those pictures take the per-picture stages)"])
        self.assertEqual(len(e), 1)
        self.assertEqual(e[0].fields["id"], "entrance once")
        self.assertTrue(e[0].fields["reason"].endswith("per-picture stages"))
        self.assertEqual(len(parse.parse_lines(
            ["]: presented yes; next frame not ready; Fallbacks engaged: none"])), 0)

    def test_libplacebo_error_real(self):
        events = parse.parse_file(fx("real_libplacebo_error.log"))
        g = [e for e in events if e.kind == "gpu_error"]
        self.assertEqual(len(g), 1)
        self.assertEqual(g[0].fields["api"], "libplacebo")
        self.assertEqual(g[0].fields["code"], "error")
        self.assertEqual(g[0].fields["what"], "User hook tried resizing non-resizable stage!")
        self.assertTrue(g[0].legacy)
        self.assertEqual(parse.category(g[0]), "other")

    def test_pulldown_real(self):
        events = parse.parse_file(fx("real_pulldown.log"))
        self.assertEqual(len(events), 6)
        self.assertTrue(all(e.kind == "cadence" for e in events))
        self.assertEqual(events[0].fields, {"state": "locked", "pattern": "23.976p film in 29.970 Hz (4 new of 5)",
                                            "was": "none", "legacy": "1"})
        lost = events[-1]
        self.assertEqual(lost.fields["state"], "lost")
        self.assertEqual(lost.fields["pattern"], "23.976p film in 29.970 Hz (4 new of 5)")
        self.assertEqual(parse.category(lost), "cadence")

    def test_unmatched_skipped_counted(self):
        self.assertEqual(self.real.skipped, 150 - 3)
        self.assertEqual(parse.summarize(self.real)["skipped"], 147)


class Categories(unittest.TestCase):
    def cat(self, cause, level="minor"):
        return parse.category(one("VPEV1 t=1.000 kind=resync level=%s cause=%s seq=1 mode=on" % (level, cause)))

    def test_table(self):
        for c in ["missed_frames", "overflow_drop", "repeat_preserving_drop", "drift_drop",
                  "kept_run_hole", "stale_delivery", "gap_other"]:
            self.assertEqual(self.cat(c), "drop", c)
        for c in ["pulldown_lock_change", "pulldown_rephase", "pulldown_clock_restart", "pulldown_off"]:
            self.assertEqual(self.cat(c), "cadence", c)
        self.assertEqual(self.cat("rate_step", "timing"), "timing")
        for c in ["start", "start_after_source_change", "in_place_source_change", "renderer_reset",
                  "live_queue_reset", "calibration_start", "calibration_end", "queue_generation_other",
                  "file_discontinuity", "counter_backwards", "timestamp_break_large",
                  "source_format_changed"]:
            self.assertEqual(self.cat(c, "full"), "resync", c)

    def test_unknown_cause_uses_level(self):
        self.assertEqual(self.cat("new_cause", "full"), "resync")
        self.assertEqual(self.cat("new_cause", "minor"), "drop")
        self.assertEqual(self.cat("new_cause", "timing"), "timing")
        self.assertEqual(self.cat("new_cause", "none"), "other")

    def test_non_resync_kinds(self):
        self.assertEqual(parse.category(one("VPEV1 t=1.000 kind=cadence state=locked pattern=3:2")), "cadence")
        self.assertEqual(parse.category(one("VPEV1 t=1.000 kind=repeat cause=x count=2")), "drop")
        self.assertEqual(parse.category(one("VPEV1 t=1.000 kind=gate from=a to=b reason=r")), "other")
        self.assertEqual(parse.category(one("VPEV1 t=1.000 kind=bogus")), "other")


class Summary(unittest.TestCase):
    def test_prefixed_fixture(self):
        events = parse.parse_file(fx("format_strings_prefixed.log"))
        s = parse.summarize(events)
        self.assertEqual(s["total"], 11)
        self.assertEqual(s["counts_by_kind"], {"session": 2, "resync": 4, "gate": 2, "fallback": 2,
                                               "gpu_error": 1})
        self.assertEqual(s["counts_by_category"], {"other": 7, "resync": 2, "drop": 1, "timing": 1})
        self.assertEqual(s["counts_by_cause"], {"start": 2, "missed_frames": 1, "rate_step": 1})
        self.assertEqual(s["first_wall"], "2026-10-31T19:55:01")
        self.assertEqual(s["last_wall"], "2026-10-31T19:55:15")
        self.assertIsNone(s["first_t_ms"])
        self.assertEqual(s["malformed"], 0)
        self.assertEqual(s["skipped"], 2)

    def test_vpev_fixture(self):
        s = parse.summarize(parse.parse_file(fx("vpev1_all_kinds.log")))
        self.assertEqual(s["total"], 27)
        self.assertEqual(s["counts_by_kind"]["malformed"], 9)
        self.assertEqual(s["counts_by_kind"]["flag"], 3)
        self.assertEqual(s["counts_by_kind"]["resync"], 4)
        self.assertEqual(s["first_t_ms"], 0.0)
        self.assertEqual(s["last_t_ms"], 19.0)
        self.assertEqual(s["counts_by_cause"]["pulldown_rephase"], 1)

    def test_plain_list_has_no_skipped(self):
        s = parse.summarize(list(parse.parse_file(fx("real_pulldown.log"))))
        self.assertNotIn("skipped", s)
        self.assertEqual(s["counts_by_kind"], {"cadence": 6})


class Cli(unittest.TestCase):
    def run_cli(self, *args):
        proc = subprocess.run([sys.executable, "-I", "-B", os.path.join(HERE, "parse.py")] + list(args),
                              capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout

    def test_json_smoke(self):
        out = json.loads(self.run_cli(fx("vpev1_all_kinds.log"), "--json"))
        self.assertEqual(len(out), 27)
        self.assertEqual(out[1]["kind"], "resync")
        self.assertEqual(out[1]["category"], "drop")
        self.assertEqual(out[1]["wall"], "2026-10-31T20:00:00")

    def test_summary_json(self):
        out = json.loads(self.run_cli(fx("format_strings_prefixed.log"), "--summary", "--json"))
        self.assertEqual(out["total"], 11)

    def test_kinds_filter_and_text(self):
        out = self.run_cli(fx("vpev1_all_kinds.log"), "--kinds", "frame_stats,stamp")
        self.assertEqual(len(out.strip().splitlines()), 2)

    def test_since_filter(self):
        out = json.loads(self.run_cli(fx("format_strings_prefixed.log"), "--json", "--since", "19:55:10"))
        self.assertTrue(out)
        self.assertTrue(all(e["wall"] >= "2026-10-31T19:55:10" for e in out))
        self.assertEqual(len(out), 6)
        # no wall time -> dropped
        self.assertEqual(json.loads(self.run_cli(fx("real_pulldown.log"), "--json", "--since", "00:00:00")), [])

    def test_since_in_process(self):
        buf = io.StringIO()
        old = sys.stdout
        sys.stdout = buf
        try:
            parse.main([fx("format_strings_prefixed.log"), "--json", "--since", "19:55:13"])
        finally:
            sys.stdout = old
        self.assertEqual(len(json.loads(buf.getvalue())), 3)


if __name__ == "__main__":
    unittest.main(verbosity=1)
