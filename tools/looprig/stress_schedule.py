# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Seeded resync stress-schedule generator for the loop rig (T6).

Emits, per (scenario group, input mode):
  schedule.txt   ResyncInject::LoadSchedule text ("at 12.5s gap 3", "at #4000 seek") for VP_RESYNC_INJECT=1
  commands.txt   the same events as "T+12.500s resync.inject gap 3" lines for a driver that calls the
                 `resync.inject` module command (manual events appear as "# MANUAL ..." comment lines)
  expected.json  vp-resync-expected/1: what each event must produce; scored by tools/looprig/analyze.py

Groups follow the owner's resync directive (2026-11-01): random, atv_switch, capture, drift, overflow,
seek, storm, dv, replug (manual HDMI re-plug). Every group runs for every input mode; input modes are
sdr, hdr10, lldv, dv_tunnel (dv_tunnel = TV-led Dolby Vision tunnel with raw planes + RPU).

Determinism: only random.Random.random() is used (ints derived by hand), so one seed gives the same
schedule on every Python version.

    python -I tools/looprig/stress_schedule.py --out DIR --seed 1 --duration 3600 [--inputs sdr,dv_tunnel]
        [--groups random,storm] [--manual-atv]
    python -I tools/looprig/stress_schedule.py --seed 1 --duration 3600 --list     (print the plan only)

The tolerances in EXPECT_TABLE are first guesses and are tunable (see README); every rate-dependent
number (frames lost, settle frames) is derived by the analyser from the source rate it measures.
"""
import argparse
import json
import os
import random
import sys

SCHEMA = "vp-resync-expected/1"
GROUPS = ["random", "atv_switch", "capture", "drift", "overflow", "seek", "storm", "dv", "replug"]
INPUTS = ["sdr", "hdr10", "lldv", "dv_tunnel"]
DV_MODES = ["tunnel", "lldv", "hdr10", "sdr"]
INPUT_TO_DV = {"sdr": "sdr", "hdr10": "hdr10", "lldv": "lldv", "dv_tunnel": "tunnel"}
ATV_RATES = ["23.976", "24", "25", "29.97", "50", "59.94", "60"]
ATV_MODES = ["lldv", "sdr"]
DEFAULT_MIN_GAP_S = 12.0
STORM_MIN_NOTICE = 5          # a burst of at least this many resyncs inside STORM_SPAN_S must raise the storm notice
STORM_SPAN_S = 8.0
STORM_QUIET_S = 15.0          # quiet time after a burst (the notice releases after 10 s)

# ResyncInject caps (src/VP-V/modules/input/signal/ResyncInject.h)
CAPS = {"gap": 1000, "minor": 1000, "stall": 60000, "rpu_loss": 100000, "signal_loss": 600000, "overflow": 1000}
RATE_PPM_MAX = 1000000
MAX_ENTRIES = 100000
QUEUE_MAX = 16
NO_ARG = ("seek", "reset", "inplace", "rebuild", "capture_restart", "rpu_return", "tunnel_handoff",
          "signal_return", "full")
NUM_ARG = ("gap", "rate", "stall", "rpu_loss", "signal_loss", "overflow")


class ScheduleError(Exception):
    pass


def validate_command(text):
    """Mirror of ResyncInject::ParseArgs. Returns (kind, value) or raises ScheduleError."""
    tok = text.split()
    if not tok:
        raise ScheduleError("missing kind")
    kind = tok[0]
    rest = tok[1:]
    if len(rest) > 1:
        raise ScheduleError("too many arguments")
    arg = rest[0] if rest else None
    if kind in NO_ARG:
        if arg is not None:
            raise ScheduleError("%s takes no number" % kind)
        return kind, 0
    if kind == "dv_switch":
        if arg not in DV_MODES:
            raise ScheduleError("bad dv target %r" % arg)
        return kind, arg
    if kind == "minor" and arg is None:
        return kind, 0
    if kind in NUM_ARG or kind == "minor":
        if arg is None:
            raise ScheduleError("%s needs a number" % kind)
        digits = arg[1:] if arg.startswith("-") else arg
        if not digits.isdigit() or len(digits) > 9:
            raise ScheduleError("bad number %r" % arg)
        n = int(arg)
        if kind == "rate":
            if abs(n) > RATE_PPM_MAX:
                raise ScheduleError("rate ppm out of range")
        elif n <= 0:
            raise ScheduleError("%s must be a positive integer" % kind)
        elif n > CAPS.get(kind, 10 ** 9):
            raise ScheduleError("%s out of range (max %d)" % (kind, CAPS[kind]))
        return kind, n
    raise ScheduleError("unknown kind %r" % kind)


# ---- expectations ------------------------------------------------------------------------------
# level: the resync level the event must produce (full|minor|timing); "optional" = may or may not
# resync (checked only when the log has a matching line). causes: accepted primary ResyncCause tokens.
# These mappings follow doc 50 section 5 and ResyncCause.h; align them once the emitter is final.
FULL_REOPEN = ["start_after_source_change", "in_place_source_change", "source_format_changed",
               "live_queue_reset", "renderer_reset", "queue_generation_other"]

EXPECT_TABLE = {
    "seek": dict(level="full", causes=["live_queue_reset", "file_discontinuity"], settle_ms=2000),
    "reset": dict(level="full", causes=["renderer_reset", "queue_generation_other", "live_queue_reset"],
                  settle_ms=2000),
    "inplace": dict(level="full", causes=["in_place_source_change", "source_format_changed"], settle_ms=2000),
    "rebuild": dict(level="full", causes=["start_after_source_change", "renderer_reset", "source_format_changed"],
                    settle_ms=3000),
    "capture_restart": dict(level="full", causes=["live_queue_reset", "start_after_source_change", "start"],
                            settle_ms=4000, source_loss_ms=3000),
    "full": dict(level="full", causes=FULL_REOPEN, settle_ms=2000),
    "gap": dict(level="minor", causes=["missed_frames", "gap_other", "timestamp_break"], settle_ms=1000),
    "minor": dict(level="minor", causes=["missed_frames", "timestamp_break", "gap_other", "drift_drop"],
                  settle_ms=1000),
    "stall": dict(level="minor", causes=["overflow_drop", "stale_delivery", "repeat_preserving_drop",
                                         "drift_drop", "stale_delivery_ended"], settle_ms=2000),
    "overflow": dict(level="minor", causes=["overflow_drop", "repeat_preserving_drop"], settle_ms=2000),
    "dv_switch": dict(level="full", causes=FULL_REOPEN, settle_ms=3000, dv_relatch=True),
    "signal_loss": dict(level="full", causes=FULL_REOPEN + ["start"], settle_ms=4000),
    "tunnel_handoff": dict(level="optional", causes=FULL_REOPEN, settle_ms=3000, dv_relatch=True),
    "rpu_loss": dict(level="optional", causes=FULL_REOPEN, settle_ms=2000),
    "rpu_return": dict(level="optional", causes=FULL_REOPEN, settle_ms=3000, dv_relatch=True),
    "signal_return": dict(level="optional", causes=FULL_REOPEN + ["start"], settle_ms=4000),
    "rate": dict(level="timing", causes=["rate_step"], settle_ms=2000),
    "manual_atv_switch": dict(level="full", causes=FULL_REOPEN + ["start"], settle_ms=5000),
    "manual_replug": dict(level="full", causes=FULL_REOPEN + ["start"], settle_ms=6000),
}


def ntsc_ppm():
    """59.94 vs 60 (and 23.976 vs 24 ...): 1e6 * (1 - 1000/1001)."""
    return int(round(1e6 * (1.0 - 1000.0 / 1001.0)))


def expectation(kind, value, dv_after, input_mode):
    t = dict(EXPECT_TABLE[kind])
    exp = {"level": t["level"], "causes": t["causes"], "settle_ms": t["settle_ms"],
           "relock": "yes" if t["level"] == "full" else "na",
           "vectors_invalidated": t["level"] == "full",
           "source_loss_frames": 0, "source_loss_ms": t.get("source_loss_ms", 0),
           "vp_drops": 2 if t["level"] == "full" else 1, "vp_repeats": 3 if t["level"] == "full" else 2,
           "dv_relatch": bool(t.get("dv_relatch") or (t["level"] == "full" and input_mode == "dv_tunnel")),
           "dv_after": dv_after}
    if kind in ("seek",):
        exp["discontinuity"] = True              # the seek itself skips or repeats source frames
    if kind == "gap":
        exp["source_loss_frames"] = int(value)
    elif kind == "stall":
        exp["vp_drops_ms"] = int(value)          # a stalled render loop may drop value ms worth of frames
    elif kind == "overflow":
        exp["vp_drops"] = int(value) + 2
    elif kind == "minor":
        exp["source_loss_frames"] = int(value)
    elif kind == "signal_loss":
        exp["source_loss_ms"] = int(value)
    elif kind == "rate":
        small = abs(int(value)) < 500
        exp.update(level="optional" if small else "timing", relock="na", vectors_invalidated=False,
                   vp_drops=1, vp_repeats=2)
    return exp


# ---- generation --------------------------------------------------------------------------------
class Rng:
    """Deterministic helpers over random.Random.random() only."""

    def __init__(self, seed):
        self.r = random.Random(seed)

    def f(self, lo, hi):
        return lo + (hi - lo) * self.r.random()

    def i(self, lo, hi):          # inclusive
        return lo + min(hi - lo, int(self.r.random() * (hi - lo + 1)))

    def choice(self, seq):
        return seq[self.i(0, len(seq) - 1)]

    def shuffle(self, seq):
        seq = list(seq)
        for k in range(len(seq) - 1, 0, -1):
            j = self.i(0, k)
            seq[k], seq[j] = seq[j], seq[k]
        return seq

    def weighted(self, pairs):
        total = sum(w for _, w in pairs)
        x = self.r.random() * total
        for item, w in pairs:
            x -= w
            if x < 0:
                return item
        return pairs[-1][0]


class Builder:
    def __init__(self, seed, group, input_mode, duration_s, manual_atv=False, min_gap_s=DEFAULT_MIN_GAP_S):
        self.seed = seed
        self.group = group
        self.input_mode = input_mode
        self.duration = float(duration_s)
        self.manual_atv = manual_atv
        self.gap = float(min_gap_s)
        self.rng = Rng((seed * 1000003 + GROUPS.index(group) * 101 + INPUTS.index(input_mode)) & 0xFFFFFFFF)
        self.events = []
        self.dv = INPUT_TO_DV[input_mode]

    def add(self, at_s, kind, value=None, command=None, manual=False, clock="s", **extra):
        if command is None and not manual:
            command = kind if value in (None, 0) and kind != "rate" else "%s %s" % (kind, value)
        if command is not None:
            validate_command(command)
        if kind == "dv_switch":
            self.dv = value
        ev = {"id": "E%04d" % (len(self.events) + 1), "kind": kind, "group": self.group,
              "at_s": round(at_s, 3), "clock": clock, "command": command, "manual": manual,
              "value": value, "storm_id": extra.pop("storm_id", None),
              "expect": expectation(kind, value, self.dv, self.input_mode)}
        if manual:
            ev["note"] = extra.pop("note", "")
        for k, v in extra.items():
            ev["expect"][k] = v
        self.events.append(ev)
        return ev

    def times(self, start=None, lo=None, hi=None):
        t = self.gap if start is None else start
        lo = lo or self.gap
        hi = hi or 4 * self.gap
        while t < self.duration - self.gap:
            yield t
            t += self.rng.f(lo, hi)


def _g_random(b):
    kinds = [("seek", 3), ("reset", 2), ("inplace", 2), ("rebuild", 2), ("gap", 4), ("rate", 2), ("stall", 3),
             ("capture_restart", 1), ("minor", 3), ("full", 2), ("overflow", 2)]
    if b.input_mode != "sdr":
        kinds += [("dv_switch", 3), ("rpu_loss", 2), ("tunnel_handoff", 1)]
    used_rate = False
    resume = 0.0
    for t in b.times():
        if t < resume:
            continue
        k = b.rng.weighted(kinds)
        if k == "gap":
            b.add(t, k, b.rng.i(1, 8))
        elif k == "minor":
            b.add(t, k, b.rng.i(1, 4) if b.rng.f(0, 1) < 0.5 else 0)
        elif k == "rate":
            b.add(t, k, b.rng.choice([-1, 1]) * b.rng.i(100, 2000))
            used_rate = True
        elif k == "stall":
            b.add(t, k, b.rng.i(100, 500))
        elif k == "overflow":
            b.add(t, k, b.rng.i(1, 20))
        elif k == "dv_switch":
            b.add(t, k, b.rng.choice([m for m in DV_MODES if m != b.dv]))
        elif k == "rpu_loss":
            b.add(t, k, b.rng.i(24, 240))
            resume = t + b.rng.f(3, 8)
            b.add(resume, "rpu_return")
            resume += 1.0
        else:
            b.add(t, k)
    if used_rate:
        last = max(e["at_s"] for e in b.events)
        b.add(min(b.duration - 1.0, last + b.gap), "rate", 0)


def _g_atv(b):
    combos = [(r, m) for r in ATV_RATES for m in ATV_MODES]
    t = b.gap
    for r, m in b.rng.shuffle(combos):
        if t > b.duration - b.gap:
            break
        if b.manual_atv:
            b.add(t, "manual_atv_switch", None, manual=True,
                  note="Apple TV: match frame rate %s Hz, dynamic range %s" % (r, m.upper()))
            b.dv = m
        else:
            b.add(t, "rebuild")          # the display-mode change path (doc 50: rebuild == rate switch)
            if b.input_mode != "sdr" and b.dv != m:
                b.add(t + 1.0, "dv_switch", m)
        t += b.rng.f(b.gap, 2 * b.gap)


def _g_capture(b):
    n = 0
    for t in b.times(lo=b.gap + 6, hi=3 * b.gap):
        v = n % 3
        if v == 0:
            b.add(t, "capture_restart")
        elif v == 1:
            ms = b.rng.i(500, 5000)
            b.add(t, "signal_loss", ms)
            b.add(t + ms / 1000.0 + 0.5, "signal_return")      # after the loss ended: a no-op cancel
        else:
            ms = b.rng.i(3000, 8000)
            b.add(t, "signal_loss", ms)
            b.add(t + 0.6 * ms / 1000.0, "signal_return")       # explicit early return cancels the loss
        n += 1


def _g_drift(b):
    start = 0.5 * b.duration if b.duration >= 7200 else 0.25 * b.duration
    ppm = ntsc_ppm()
    steps = [ppm, -ppm]
    last = start
    for t in b.times(start=start, lo=60, hi=240):
        if steps and b.rng.f(0, 1) < 0.4:
            v = steps.pop(0)
        else:
            v = b.rng.choice([-1, 1]) * b.rng.i(20, 500)
        b.add(t, "rate", v)
        last = t
    for v in steps:                                              # make sure both NTSC steps are present
        last += 60
        b.add(last, "rate", v)
    b.add(last + 60, "rate", 0)


def _g_overflow(b):
    n = 0
    for t in b.times(lo=b.gap, hi=2 * b.gap):
        v = n % 3
        if v == 0:
            b.add(t, "stall", b.rng.i(200, 1500))
        elif v == 1:
            b.add(t, "overflow", b.rng.i(5, 200))
        else:
            b.add(t, "gap", b.rng.i(1, 60))
        n += 1


def _g_seek(b):
    # seeks and file discontinuities use the source-sequence clock (a seek moves source time)
    seq = 600
    for _ in range(20):
        b.add(float(seq), "seek", clock="seq")
        seq += b.rng.i(400, 1500)
    for _ in range(3):
        b.add(float(seq), b.rng.choice(["inplace", "reset"]), clock="seq")
        seq += b.rng.i(400, 1500)


def _g_storm(b):
    t = b.gap
    sid = 0
    kinds = ["seek", "reset", "inplace", "full"]
    while t < b.duration - b.gap - STORM_SPAN_S:
        n = 5 if sid % 2 == 0 else 3
        span = b.rng.f(0.6 * STORM_SPAN_S, 0.95 * STORM_SPAN_S)
        for j in range(n):
            ev = b.add(t + span * j / (n - 1), b.rng.choice(kinds), storm_id=sid + 1)
            ev["expect"].update(settle_ms=10000, vp_drops=4, vp_repeats=6, storm_n=n, storm_last=(j == n - 1))
            if j == n - 1 and n >= STORM_MIN_NOTICE:
                ev["expect"]["expect_fallback"] = "resync_storm"
        t += span + STORM_QUIET_S + b.rng.f(0, b.gap)
        sid += 1


def _g_dv(b):
    # directed edges of the complete graph on the four modes: 12 switches, plus RPU loss/return, hand-offs
    cur = b.dv
    todo = {(a, c) for a in DV_MODES for c in DV_MODES if a != c}
    t = b.gap
    steps = 0
    while todo and steps < 200:
        steps += 1
        opts = sorted((a, c) for (a, c) in todo if a == cur)
        nxt = b.rng.choice(opts)[1] if opts else b.rng.choice([m for m in DV_MODES if m != cur])
        todo.discard((cur, nxt))
        b.add(t, "dv_switch", nxt)
        cur = nxt
        t += b.rng.f(b.gap, 2 * b.gap)
        if steps % 4 == 0:
            b.add(t, "rpu_loss", b.rng.i(24, 240))
            t += b.rng.f(3, 8)
            b.add(t, "rpu_return")
            t += b.gap
        if steps % 5 == 0:
            b.add(t, "tunnel_handoff")
            t += b.gap


def _g_replug(b):
    t = b.gap
    k = 0
    while t < b.duration - b.gap and k < 8:
        b.add(t, "manual_replug", None, manual=True, note="unplug and replug the HDMI cable (owner-attended)")
        t += b.rng.f(3 * b.gap, 6 * b.gap)
        k += 1


GROUP_FN = {"random": _g_random, "atv_switch": _g_atv, "capture": _g_capture, "drift": _g_drift,
            "overflow": _g_overflow, "seek": _g_seek, "storm": _g_storm, "dv": _g_dv, "replug": _g_replug}


def generate(seed, group, input_mode, duration_s=3600.0, manual_atv=False, min_gap_s=DEFAULT_MIN_GAP_S):
    if group not in GROUPS:
        raise ScheduleError("unknown group %r" % group)
    if input_mode not in INPUTS:
        raise ScheduleError("unknown input %r" % input_mode)
    if duration_s < 4 * min_gap_s:
        raise ScheduleError("duration too short for the group (need at least 4 x min gap)")
    b = Builder(seed, group, input_mode, duration_s, manual_atv, min_gap_s)
    GROUP_FN[group](b)
    # events are ordered by their own clock; stable sort keeps the file order for equal times
    sec = sorted((e for e in b.events if e["clock"] == "s"), key=lambda e: e["at_s"])
    seq = [e for e in b.events if e["clock"] == "seq"]
    events = sec + seq
    for n, e in enumerate(events, 1):
        e["id"] = "E%04d" % n
    if len(events) > MAX_ENTRIES:
        raise ScheduleError("too many entries")
    return {"schema": SCHEMA, "seed": seed, "group": group, "input": input_mode, "duration_s": duration_s,
            "initial_dv": INPUT_TO_DV[input_mode], "min_gap_s": min_gap_s, "manual_atv": manual_atv,
            "events": events}


def check_schedule(sched):
    """Validate a generated schedule against the ResyncInject rules. Returns a list of problems."""
    probs = []
    last = {"s": -1.0, "seq": -1.0}
    for e in sched["events"]:
        if e["command"] is not None:
            try:
                validate_command(e["command"])
            except ScheduleError as err:
                probs.append("%s: %s" % (e["id"], err))
        if e["at_s"] < last[e["clock"]]:
            probs.append("%s: time goes backwards on the %s clock" % (e["id"], e["clock"]))
        last[e["clock"]] = e["at_s"]
    # the injection queue holds 16: no more than 16 commands released inside 0.5 s
    times = sorted(e["at_s"] for e in sched["events"] if e["clock"] == "s" and e["command"])
    for i in range(len(times)):
        j = i
        while j < len(times) and times[j] - times[i] < 0.5:
            j += 1
        if j - i > QUEUE_MAX:
            probs.append("more than %d commands inside 0.5 s at %.3f s" % (QUEUE_MAX, times[i]))
            break
    return probs


def _time_token(e):
    if e["clock"] == "seq":
        return "#%d" % int(e["at_s"])
    return "%ss" % ("%.3f" % e["at_s"]).rstrip("0").rstrip(".")


def to_schedule_text(sched):
    lines = ["# vp-resync schedule: group=%s input=%s seed=%d" % (sched["group"], sched["input"], sched["seed"]),
             "# arm with VP_RESYNC_INJECT=1; the matching expected.json is scored by tools/looprig/analyze.py",
             "seed %d" % sched["seed"]]
    for e in sched["events"]:
        if e["command"] is None:
            lines.append("# MANUAL at %s: %s" % (_time_token(e), e.get("note", e["kind"])))
        else:
            lines.append("at %s %s" % (_time_token(e), e["command"]))
    return "\n".join(lines) + "\n"


def to_commands_text(sched):
    lines = ["# group=%s input=%s seed=%d; times are source seconds (T+) or source sequence (seq)"
             % (sched["group"], sched["input"], sched["seed"])]
    for e in sched["events"]:
        tok = ("seq %d" % int(e["at_s"])) if e["clock"] == "seq" else "T+%.3fs" % e["at_s"]
        if e["command"] is None:
            lines.append("# MANUAL %s: %s" % (tok, e.get("note", e["kind"])))
        else:
            lines.append("%s resync.inject %s" % (tok, e["command"]))
    return "\n".join(lines) + "\n"


def write_all(sched, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "schedule.txt"), "w", newline="\n") as f:
        f.write(to_schedule_text(sched))
    with open(os.path.join(out_dir, "commands.txt"), "w", newline="\n") as f:
        f.write(to_commands_text(sched))
    with open(os.path.join(out_dir, "expected.json"), "w", newline="\n") as f:
        json.dump(sched, f, indent=1)
        f.write("\n")


def suite(seed, groups, inputs, duration_s, manual_atv=False):
    """Generator of (name, schedule) for every group x input."""
    for g in groups:
        for i in inputs:
            dur = max(duration_s, 3600.0) if g == "seek" else duration_s
            yield "%s_%s" % (g, i), generate(seed, g, i, dur, manual_atv)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="output folder (one sub-folder per group_input)")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--duration", type=float, required=True, help="seconds of source time per schedule")
    ap.add_argument("--groups", default=",".join(GROUPS))
    ap.add_argument("--inputs", default="sdr,dv_tunnel")
    ap.add_argument("--manual-atv", action="store_true", help="Apple TV switches become manual steps")
    ap.add_argument("--list", action="store_true", help="print the plan and write nothing")
    a = ap.parse_args(argv)
    groups = [g for g in a.groups.split(",") if g]
    inputs = [i for i in a.inputs.split(",") if i]
    try:
        plan = list(suite(a.seed, groups, inputs, a.duration, a.manual_atv))
    except ScheduleError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    bad = 0
    for name, sched in plan:
        probs = check_schedule(sched)
        bad += len(probs)
        print("%-24s %4d events  %s" % (name, len(sched["events"]), "; ".join(probs) if probs else "ok"))
        if a.out and not a.list:
            write_all(sched, os.path.join(a.out, name))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
