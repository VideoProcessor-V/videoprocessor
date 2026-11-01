# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Loop-rig analyser (T3): scores a captured loop recording against what was sent and what VP logged.

    python -I tools/looprig/analyze.py --scenario film24_to_60 --scenarios tools/looprig/scenarios.json
        --recording rec.y4m|DIR(raw v210)|stamps.csv --sent sent.csv --log vp.log
        [--expected expected.json] [--capture-times times.csv | --capture-t0-ms N] --out DIR

Inputs
  recording   Y4M C422p10 (or StreamRecorder raw_*.bin directory, as tools/levels/check.py reads them), or a
              pre-decoded stamps CSV (header: pos,src_index,src_aux,out_index,out_aux; empty = undecoded).
              One captured frame per output refresh of the loop. The source stamp (top-left, kind 0) says
              which source frame is on screen; the output stamp (top-right, kind 1) is VP's output counter.
  sent.csv    index,aux,pts_num,pts_den[,t_ms]  (tools/framestamp/make_stamped_clip.py, DvLoopback --send)
  vp.log      VPEV1 lines (tools/vplog/parse.py; legacy lines are adapted by the parser)
  expected    vp-resync-expected/1 from tools/looprig/stress_schedule.py (optional; steady runs have none)

Outputs report.json + report.md. Verdict per row PASS / FAIL / NO DATA / INFO; overall FAIL if any FAIL,
INCOMPLETE if any NO DATA (no FAIL), else PASS. Exit codes 0 / 1 / 3; usage errors 2.

Nothing rate-specific is built in: the source rate comes from sent.csv (or the scenario), the output rate
from the scenario, and every allowance is derived from those.
"""
import argparse
import bisect
import csv
import json
import math
import os
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
for sub in ("vplog", "framestamp", "levels"):
    p = os.path.join(TOOLS, sub)
    if p not in sys.path:
        sys.path.insert(0, p)

import parse as vplog  # noqa: E402

PASS, FAIL, NODATA, INFO = "PASS", "FAIL", "NO DATA", "INFO"

DEFAULTS = {
    "phase_tolerance": 0.25,          # display periods of cadence slack beyond the natural pulldown spread
    "max_vp_per_hour": 1.0,           # unexplained VP-caused drops+repeats per steady hour
    "clock_drift_ppm": 0.0,           # measured source-vs-display clock offset: adds allowed corrections
    "event_window_s": 5.0,            # source seconds after an event that belong to it
    "min_segment_frames": 8,          # steady segments shorter than this are not scored for cadence
    "max_undecoded_fraction": 0.005,
    "capture_loss_fatal": True,
    "lock_within_s": 1.0,             # cadence lock after start / after a lost lock
    "relock_within_s": 2.0,
    "start_grace_s": 3.0,             # resyncs with a start cause inside this are the session start
    "latency_budget_periods": {"low_latency": 3, "render_ahead": 8, "games": 2, "competitive": 1, "default": 6},
    "latency_extra_periods": 1.0,
    "required_readers": [],
    "dv_tokens": {"tunnel": ["tunnel", "dv_tunnel", "dvtunnel"], "lldv": ["lldv", "ll"],
                  "hdr10": ["", "none", "0", "hdr10", "off"], "sdr": ["", "none", "0", "sdr", "off"]},
}


class InputError(Exception):
    pass


# ---- loading -----------------------------------------------------------------------------------
def load_scenario(path, name):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    base = dict(DEFAULTS)
    base.update(doc.get("defaults", {}))
    for sc in doc.get("scenarios", []):
        if sc.get("id") == name:
            out = dict(base)
            out.update(sc)
            return out
    raise InputError("scenario %r not in %s" % (name, path))


def load_sent(path):
    rows = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            t = r.get("t_ms") or r.get("qpc_ms") or ""
            rows.append({"index": int(r["index"]), "aux": int(r.get("aux") or 0),
                         "pts": Fraction(int(r["pts_num"]), int(r["pts_den"])),
                         "t_ms": float(t) if t else None})
    if not rows:
        raise InputError("%s: no rows" % path)
    return rows


def src_period(sent, scenario):
    """Source frame duration in seconds (Fraction): scenario src_rate [n,d] if numeric, else sent.csv pts."""
    sr = scenario.get("src_rate")
    if isinstance(sr, (list, tuple)) and len(sr) == 2:
        return Fraction(sr[1], sr[0])
    if len(sent) < 2 or sent[-1]["index"] == sent[0]["index"]:
        raise InputError("cannot derive the source rate from fewer than two sent rows")
    # per index, not per row: rows the source never sent must not skew the period
    return (sent[-1]["pts"] - sent[0]["pts"]) / (sent[-1]["index"] - sent[0]["index"])


def load_stamps_csv(path):
    rows = []

    def val(x):
        return int(x) if x not in ("", None) else None
    with open(path, "r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            rows.append((val(r["src_index"]), val(r.get("src_aux")), val(r.get("out_index")), val(r.get("out_aux"))))
    return rows


def decode_recording(path, scenario):
    """-> (stamp rows [(src, src_aux, out, out_aux)], frames_fn or None)."""
    if path.lower().endswith(".csv"):
        return load_stamps_csv(path), None
    import numpy  # noqa: F401
    import framestamp as fs
    import check as lvl
    import argparse as ap
    ns = ap.Namespace(width=scenario.get("width"), height=scenario.get("height"), row_bytes=None)

    def frames():
        return lvl.iter_frames([path], ns)
    rows = []
    for y, _cb, _cr in frames():
        s = fs.decode(y, 10, fs.CORNER_SOURCE)
        o = fs.decode(y, 10, fs.CORNER_OUTPUT)
        rows.append((s["index"] if s["found"] and s["kind"] == fs.KIND_SOURCE else None,
                     s["aux"] if s["found"] and s["kind"] == fs.KIND_SOURCE else None,
                     o["index"] if o["found"] and o["kind"] == fs.KIND_OUTPUT else None,
                     o["aux"] if o["found"] and o["kind"] == fs.KIND_OUTPUT else None))
    return rows, frames


# ---- run structure -----------------------------------------------------------------------------
def build_runs(src):
    """[(index, first_pos, length)] over decoded positions."""
    runs = []
    for pos, k in enumerate(src):
        if k is None:
            continue
        if runs and runs[-1][0] == k:
            runs[-1][2] += 1
        else:
            runs.append([k, pos, 1])
    return runs


def natural_spread(r):
    """Spread of p - k*r for an ideal pulldown of ratio r (output periods per source frame)."""
    den = Fraction(r).limit_denominator(1000).denominator
    return 1.0 - 1.0 / den


def lattice_errors(points, r, tolerance):
    """points: [(k, first_pos)] of one contiguous segment. Returns (error_count, [distance outside])."""
    if len(points) < 2:
        return 0, []
    k0 = points[0][0]
    d = [p - (k - k0) * r for k, p in points]
    width = natural_spread(r) + tolerance + 1e-6
    sd = sorted(d)
    best_c, best_n, j = sd[0], 0, 0
    for i in range(len(sd)):
        while sd[i] - sd[j] > width:
            j += 1
        if i - j + 1 > best_n:
            best_n, best_c = i - j + 1, sd[j]
    out = []
    for v in d:
        if v < best_c:
            out.append(best_c - v)
        elif v > best_c + width:
            out.append(v - best_c - width)
    return len(out), out


def percentile(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    pos = (len(s) - 1) * q
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


class Windows:
    """Event windows over source indices: [(lo, hi, event_id)]."""

    def __init__(self):
        self.items = []

    def add(self, lo, hi, eid):
        self.items.append((lo, hi, eid))

    def which(self, k):
        for lo, hi, eid in self.items:
            if lo <= k <= hi:
                return eid
        return None


def analyze_sequence(src, r, sent_idx, windows, scenario):
    """Core pacing analysis over decoded source indices per captured position.

    Returns dict: missing [(k, cause, eid)], excess [(k, count, cause, eid)], backwards [(pos, from, to)],
    segments [(first_k, last_k, n_runs, errors, outside)], last_bad index per event, undecoded count.
    cause: source (never sent) | event (inside an event window) | vp
    """
    runs = build_runs(src)
    ceil_r = max(1, int(math.ceil(r - 1e-9)))
    sent_count = {}
    for k in sent_idx:
        sent_count[k] = sent_count.get(k, 0) + 1
    missing, excess, backwards, bad_idx = [], [], [], []
    segments = []
    seg = []
    tol = float(scenario["phase_tolerance"])

    def close():
        if seg:
            pts = [(k, p) for k, p, _ in seg]
            n, out = lattice_errors(pts, r, tol)
            segments.append({"first": seg[0][0], "last": seg[-1][0], "runs": len(seg), "errors": n,
                             "outside": out, "window": windows.which(seg[0][0]) is not None})
            if n:
                bad_idx.append(seg[-1][0])

    def classify(k):
        if sent_idx and k not in sent_count:
            return "source", None
        eid = windows.which(k)
        return ("event", eid) if eid else ("vp", None)

    prev = None
    for run in runs:
        k, pos, length = run
        allowed = ceil_r * max(1, sent_count.get(k, 1))
        if length > allowed:
            cause, eid = classify(k)
            excess.append((k, length - allowed, cause, eid))
            bad_idx.append(k)
            close()
            seg = []
        if prev is not None:
            if k < prev[0]:
                backwards.append((pos, prev[0], k))
                bad_idx.append(k)
                close()
                seg = []
            elif k > prev[0] + 1:
                nb = windows.which(prev[0]) or windows.which(k)
                for m in range(prev[0] + 1, k):
                    cause, eid = classify(m)
                    if cause == "vp" and nb:
                        cause, eid = "event", nb
                    missing.append((m, cause, eid))
                    bad_idx.append(m)
                close()
                seg = []
        seg.append((k, pos, length))
        prev = run
    close()
    undecoded = sum(1 for k in src if k is None)
    return {"missing": missing, "excess": excess, "backwards": backwards, "segments": segments,
            "bad_idx": sorted(set(bad_idx)), "undecoded": undecoded, "runs": len(runs)}


# ---- log handling ------------------------------------------------------------------------------
def int_or_none(x):
    try:
        return int(x)
    except (TypeError, ValueError):
        return None


def assign_indices(events, src_fps):
    """Source index (float) for every event: nearest preceding anchor (seq/index fields) advanced by t."""
    anchors = []
    for i, e in enumerate(events):
        s = None
        if e.kind == "resync":          # resync_settle's seq is the carrying frame, not "now": not an anchor
            s = int_or_none(e.fields.get("seq"))
        elif e.kind == "stamp" and e.fields.get("dir", "") in ("src", "source", "in"):
            s = int_or_none(e.fields.get("index"))
        if s is not None:
            anchors.append((i, e.t_ms, s))
    out = [None] * len(events)
    if not anchors:
        return out
    cur = None
    ai = 0
    for i, e in enumerate(events):
        while ai < len(anchors) and anchors[ai][0] <= i:
            cur = anchors[ai]
            ai += 1
        if cur is None:
            continue
        v = float(cur[2])
        if e.t_ms is not None and cur[1] is not None and i != cur[0]:
            v += (e.t_ms - cur[1]) / 1000.0 * src_fps
        out[i] = v
    first = anchors[0]
    for i in range(first[0]):
        e = events[i]
        v = float(first[2])
        if e.t_ms is not None and first[1] is not None:
            v += (e.t_ms - first[1]) / 1000.0 * src_fps
        out[i] = v
    return out


def pattern_key(text):
    if text is None:
        return ""
    t = str(text).strip().lower()
    parts = [p for p in t.replace("-", ":").replace(",", ":").split(":") if p != ""]
    if parts and all(p.isdigit() for p in parts):
        rots = [tuple(parts[i:] + parts[:i]) for i in range(len(parts))]
        return ":".join(min(rots))
    return t


def sec_to_index(sent, t_s):
    """Index of the first sent frame with pts >= t_s (extrapolated at the source period beyond the end)."""
    pts = [float(r["pts"]) for r in sent]
    i = bisect.bisect_left(pts, t_s)
    if i < len(sent):
        return sent[i]["index"]
    period = (pts[-1] - pts[0]) / max(1, len(pts) - 1)
    return sent[-1]["index"] + int(math.ceil((t_s - pts[-1]) / period))


def event_index(ev, sent):
    if ev["clock"] == "seq":
        return int(ev["at_s"]) + sent[0]["index"] if ev.get("seq_relative") else int(ev["at_s"])
    return sec_to_index(sent, ev["at_s"])


# ---- per-event scoring -------------------------------------------------------------------------
def match_events(expected, log_events, log_idx, sent, scenario, src_fps):
    """Greedy ordered matching of expected events to resync log lines. Returns (matches, unmatched log)."""
    cand = [(i, e) for i, e in enumerate(log_events)
            if e.kind == "resync" and e.fields.get("level") in ("full", "minor", "timing")]
    used = set()
    tol = max(30, int(math.ceil(2 * float(scenario["event_window_s"]) * src_fps)))
    matches = []
    for ev in expected["events"]:
        exp = ev["expect"]
        eidx = event_index(ev, sent)
        loss_f = int(math.ceil(exp.get("source_loss_ms", 0) / 1000.0 * src_fps))
        found = None
        if exp["level"] != "none":
            best = None
            for i, e in cand:
                if i in used or log_idx[i] is None:
                    continue
                d = log_idx[i] - eidx
                if d < -tol or d > tol + loss_f:
                    continue
                score = abs(d) - (1000 if e.fields.get("cause") in exp["causes"] else 0) \
                    - (500 if e.fields.get("injected") == "1" else 0)
                if best is None or score < best[0]:
                    best = (score, i, e)
            if best is not None:
                found = best
                used.add(best[1])
        matches.append((ev, eidx, found))
    unmatched = [(i, e) for i, e in cand if i not in used]
    return matches, unmatched


def settle_for(log_events, log_idx, seq):
    for i, e in enumerate(log_events):
        if e.kind == "resync_settle" and int_or_none(e.fields.get("seq")) == seq:
            return e
    return None


def f_or_none(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def score_events(expected, log_events, log_idx, sent, scenario, src_fps, seqres, windows):
    rows = []
    matches, unmatched = match_events(expected, log_events, log_idx, sent, scenario, src_fps)
    cad = [(i, e) for i, e in enumerate(log_events) if e.kind == "cadence"]
    srcchg = [(i, e) for i, e in enumerate(log_events) if e.kind == "source_change"]
    fallbacks = [(i, e) for i, e in enumerate(log_events) if e.kind == "fallback"]
    required = set(scenario.get("required_readers") or [])
    bad_idx = seqres["bad_idx"]
    win_frames = int(math.ceil(float(scenario["event_window_s"]) * src_fps))
    tokens = scenario["dv_tokens"]
    for ev, eidx, found in matches:
        exp = ev["expect"]
        row = {"id": ev["id"], "kind": ev["kind"], "group": ev.get("group"), "at_s": ev["at_s"],
               "storm_id": ev.get("storm_id"), "manual": ev.get("manual", False), "checks": {}}
        checks = row["checks"]
        li, le = (found[1], found[2]) if found else (None, None)
        center = int(round(log_idx[li])) if li is not None and log_idx[li] is not None else eidx
        loss_frames = exp.get("source_loss_frames", 0) + int(math.ceil(exp.get("source_loss_ms", 0) / 1000.0 * src_fps))
        lo, hi = center - 1, center + win_frames + loss_frames
        row["window"] = [lo, hi]

        # 1. a resync was logged with an accepted cause and level
        if exp["level"] == "optional":
            checks["logged"] = (INFO, "optional: %s" % ("logged as %s" % le.fields.get("cause") if le else "no line"))
        elif exp["level"] == "none":
            checks["logged"] = (PASS, "none expected")
        elif le is None:
            checks["logged"] = (FAIL, "no resync line near index %d" % eidx)
        else:
            okc = le.fields.get("cause") in exp["causes"]
            okl = le.fields.get("level") == exp["level"] or exp["level"] == "optional"
            checks["logged"] = (PASS if okc and okl else FAIL,
                                "cause=%s level=%s (accepted %s/%s)" % (le.fields.get("cause"), le.fields.get("level"),
                                                                       ",".join(exp["causes"]), exp["level"]))
        if le is None and exp["level"] != "optional" and exp["level"] != "none":
            seqv = None
        else:
            seqv = int_or_none(le.fields.get("seq")) if le is not None else None

        # 2. settle time / frames from the log, and from the recording
        st = settle_for(log_events, log_idx, seqv) if seqv is not None else None
        if exp["level"] in ("full", "timing") and le is not None:
            if st is None:
                checks["settle"] = (NODATA, "no resync_settle line for seq %s" % seqv)
            else:
                ms = f_or_none(st.fields.get("settle_ms"))
                if ms is None:
                    checks["settle"] = (NODATA, "resync_settle without settle_ms")
                else:
                    ok = ms <= exp["settle_ms"] and st.fields.get("vectors") != "invalid"
                    checks["settle"] = (PASS if ok else FAIL, "settle %.0f ms (limit %d), frames %s, vectors=%s" % (
                        ms, exp["settle_ms"], st.fields.get("settle_frames"), st.fields.get("vectors")))
        # recording-side settle: last bad index inside the window, relative to the event
        in_win = [k for k in bad_idx if lo <= k <= hi + win_frames]
        rec_settle = (max(in_win) - center) if in_win else 0
        lim_frames = int(math.ceil(exp["settle_ms"] / 1000.0 * src_fps)) + loss_frames
        if exp["level"] in ("full", "timing", "minor"):
            checks["picture_settle"] = (PASS if rec_settle <= lim_frames else FAIL,
                                        "last disturbed source frame %d after the event (limit %d)" % (rec_settle, lim_frames))

        # 3. drops / repeats around the event, from the recording
        miss = [m for m in seqres["missing"] if lo <= m[0] <= hi]
        exc = [x for x in seqres["excess"] if lo <= x[0] <= hi]
        n_drop, n_rep = len(miss), sum(x[1] for x in exc)
        src_lost = sum(1 for m in miss if m[1] == "source")
        vp_drop = n_drop - src_lost
        allow_d = exp["vp_drops"] + loss_frames + int(math.ceil(exp.get("vp_drops_ms", 0) / 1000.0 * src_fps))
        row["drops"], row["repeats"] = n_drop, n_rep
        if exp.get("discontinuity"):
            checks["drops"] = (INFO, "%d source frames skipped by the discontinuity (not scored)" % n_drop)
        else:
            checks["drops"] = (PASS if vp_drop <= allow_d else FAIL, "%d dropped (%d source-side), allowed VP %d" % (
                n_drop, src_lost, allow_d))
        checks["repeats"] = (PASS if n_rep <= exp["vp_repeats"] else FAIL, "%d excess repeats, allowed %d" % (
            n_rep, exp["vp_repeats"]))

        # 4. cadence relock
        lost = [(i, e) for i, e in cad if e.fields.get("state") == "lost" and log_idx[i] is not None
                and lo <= log_idx[i] <= hi]
        if exp["relock"] == "yes" and le is not None:
            if st is not None and st.fields.get("relock") in ("yes", "no", "na"):
                rl = st.fields.get("relock")
                checks["relock"] = (FAIL if rl == "no" else PASS, "resync_settle relock=%s frames=%s" % (
                    rl, st.fields.get("relock_frames")))
            elif lost:
                after = [(i, e) for i, e in cad if e.fields.get("state") == "locked" and log_idx[i] is not None
                         and log_idx[i] >= log_idx[lost[0][0]]]
                lim = log_idx[lost[0][0]] + float(scenario["relock_within_s"]) * src_fps
                ok = any(log_idx[i] <= lim for i, _ in after)
                checks["relock"] = (PASS if ok else FAIL, "cadence lost, %s" % ("relocked in time" if ok else "no relock"))
            elif scenario.get("expect_cadence", {}).get("pattern") in (None, "none"):
                checks["relock"] = (INFO, "no cadence in this scenario")
            else:
                checks["relock"] = (PASS, "cadence stayed locked")

        # 5. consumers invalidated through the one Indicator
        if exp["vectors_invalidated"] and le is not None:
            readers = le.fields.get("readers")
            if readers is None:
                checks["consumers"] = (NODATA, "resync line has no readers field")
            else:
                names = set(x for x in readers.split(",") if x)
                missing_r = required - names
                ok = bool(names) and not missing_r
                checks["consumers"] = (PASS if ok else FAIL, "readers=%s%s" % (readers, (" missing " + ",".join(sorted(missing_r))) if missing_r else ""))
            if st is not None:
                checks["stale_vectors"] = (PASS if st.fields.get("vectors") == "valid" else FAIL,
                                           "after settle vectors=%s" % st.fields.get("vectors"))

        # 6. DV metadata re-latched
        if exp.get("dv_relatch") and le is not None:
            relevant = [e for i, e in srcchg if log_idx[i] is not None and log_idx[i] <= hi]
            if not srcchg:
                checks["dv_relatch"] = (NODATA, "no source_change lines in the log")
            elif not relevant:
                checks["dv_relatch"] = (NODATA, "no source_change before the end of the window")
            else:
                got = (relevant[-1].fields.get("dv") or "").lower()
                acc = [t.lower() for t in tokens.get(exp["dv_after"], [exp["dv_after"]])]
                checks["dv_relatch"] = (PASS if got in acc else FAIL, "dv=%r expected one of %s" % (got, acc))

        # 7. storm notice
        if exp.get("expect_fallback"):
            hits = [e for i, e in fallbacks if exp["expect_fallback"] in e.fields.get("id", "")
                    and e.fields.get("state") == "engaged" and log_idx[i] is not None and log_idx[i] >= center - 1]
            checks["storm_notice"] = (PASS if hits else FAIL, "fallback %s %s" % (
                exp["expect_fallback"], "engaged" if hits else "not engaged (silent)"))
        row["status"] = FAIL if any(c[0] == FAIL for c in checks.values()) else (
            NODATA if any(c[0] == NODATA for c in checks.values()) else PASS)
        if ev.get("manual"):
            checks["manual"] = (INFO, "manual step: %s" % ev.get("note", ""))
        rows.append(row)
        windows.add(lo, hi, ev["id"])
        if abs(eidx - center) > 1:
            windows.add(min(eidx, center) - 1, max(eidx, center) + win_frames + loss_frames, ev["id"])
    return rows, unmatched


# ---- scenario-level rows -----------------------------------------------------------------------
def row(rid, status, detail, **extra):
    d = {"id": rid, "status": status, "detail": detail}
    d.update(extra)
    return d


def pacing_rows(seqres, r, hours, scenario, expected_present):
    rows = []
    seg_ok = [s for s in seqres["segments"] if not s["window"] and s["runs"] >= scenario["min_segment_frames"]]
    outside = [d for s in seg_ok for d in s["outside"]]
    errors = sum(s["errors"] for s in seg_ok)
    if not seg_ok:
        rows.append(row("pacing_cadence", NODATA, "no steady segment of at least %d source frames" %
                        scenario["min_segment_frames"]))
    else:
        p99 = percentile(outside, 0.99) if outside else 0.0
        ok = errors == 0
        rows.append(row("pacing_cadence", PASS if ok else FAIL,
                        "%d cadence errors over %d steady segments, p99 phase error %.3f periods (slack %.2f, ratio %.4f)" % (
                            errors, len(seg_ok), p99, scenario["phase_tolerance"], r),
                        errors=errors, p99_phase_error=round(p99, 4), segments=len(seg_ok)))
    return rows


def drop_rows(seqres, hours, scenario, windows):
    rows = []
    miss, exc = seqres["missing"], seqres["excess"]
    by = {"source": 0, "event": 0, "vp": 0}
    for _, cause, _ in miss:
        by[cause] += 1
    rep = {"source": 0, "event": 0, "vp": 0}
    for _, n, cause, _ in exc:
        rep[cause] += n
    drift_allow = scenario["clock_drift_ppm"] * 1e-6 * 3600.0 * scenario.get("_out_fps", 0.0) * hours
    allowed = int(math.floor(scenario["max_vp_per_hour"] * hours + drift_allow + 1e-9))
    vp_total = by["vp"] + rep["vp"]
    rows.append(row("drops_repeats", PASS if vp_total <= allowed else FAIL,
                    "VP-caused outside events: %d dropped + %d repeated (allowed %d over %.2f h); inside event windows: "
                    "%d dropped + %d repeated; source-caused: %d dropped + %d repeated" % (
                        by["vp"], rep["vp"], allowed, hours, by["event"], rep["event"], by["source"], rep["source"]),
                    vp_dropped=by["vp"], vp_repeated=rep["vp"], event_dropped=by["event"], event_repeated=rep["event"],
                    source_dropped=by["source"], source_repeated=rep["source"], allowed=allowed))
    back = seqres["backwards"]
    out_of_event = [b for b in back if windows.which(b[2]) is None and windows.which(b[1]) is None]
    rows.append(row("order", PASS if not out_of_event else FAIL,
                    "%d backwards jumps outside events (first: %s)" % (len(out_of_event), out_of_event[:3]),
                    backwards_total=len(back)))
    return rows


def capture_rows(rows_stamps, scenario):
    n = len(rows_stamps)
    und = sum(1 for r in rows_stamps if r[0] is None)
    frac = und / n if n else 1.0
    out = [row("undecoded_frames", PASS if frac <= scenario["max_undecoded_fraction"] else FAIL,
               "%d of %d captured frames without a decodable source stamp (limit %.3f%%)" % (
                   und, n, 100 * scenario["max_undecoded_fraction"]), fraction=round(frac, 6))]
    lost, prev = 0, None
    for _, _, o, _ in rows_stamps:
        if o is None:
            continue
        if prev is not None and o > prev + 1:
            lost += o - prev - 1
        prev = o
    status = PASS if lost == 0 else (FAIL if scenario["capture_loss_fatal"] else INFO)
    out.append(row("capture_integrity", status,
                   "%d output-counter values missing in the recording (capture lost frames: a rig fault, not VP)" % lost,
                   capture_lost=lost))
    return out


def latency_rows(seqres, runs, sent, scenario, out_period_s, cap_times, t0_ms):
    mode = scenario.get("latency_mode", "default")
    bud = scenario["latency_budget_periods"]
    periods = bud.get(mode, bud.get("default"))
    limit_ms = (periods + scenario["latency_extra_periods"]) * out_period_s * 1000.0
    have_sent_t = any(r["t_ms"] is not None for r in sent)
    if not have_sent_t or (cap_times is None and t0_ms is None):
        return [row("latency", NODATA, "needs sent.csv t_ms plus --capture-times or --capture-t0-ms", mode=mode)]
    st = {r["index"]: r["t_ms"] for r in sent if r["t_ms"] is not None}
    lat = []
    for k, pos, _ in runs:
        if k not in st:
            continue
        t_out = cap_times[pos] if cap_times is not None and pos < len(cap_times) else (
            t0_ms + pos * out_period_s * 1000.0 if t0_ms is not None else None)
        if t_out is not None:
            lat.append(t_out - st[k])
    if not lat:
        return [row("latency", NODATA, "no frame could be matched in time", mode=mode)]
    med, p99 = percentile(lat, 0.5), percentile(lat, 0.99)
    return [row("latency", PASS if p99 <= limit_ms else FAIL,
                "mode %s: median %.1f ms, p99 %.1f ms, limit %.1f ms (%g + %g periods of %.2f ms)" % (
                    mode, med, p99, limit_ms, periods, scenario["latency_extra_periods"], out_period_s * 1000.0),
                median_ms=round(med, 2), p99_ms=round(p99, 2), limit_ms=round(limit_ms, 2))]


def cadence_rows(log_events, log_idx, seqres, windows, scenario, src_fps, first_idx, last_idx):
    exp = scenario.get("expect_cadence") or {"pattern": "none"}
    want = pattern_key(exp.get("pattern", "none"))
    cad = [(i, e) for i, e in enumerate(log_events) if e.kind == "cadence"]
    rows = []
    locked = [(i, e) for i, e in cad if e.fields.get("state") == "locked"]
    if want in ("none", ""):
        false_locks = [e for i, e in locked if log_idx[i] is not None and windows.which(int(log_idx[i])) is None]
        rows.append(row("cadence_lock", PASS if not false_locks else FAIL,
                        "%d locks logged where no pulldown is expected" % len(false_locks)))
        return rows
    if not cad:
        return [row("cadence_lock", NODATA, "no cadence lines in the log")]
    lim = first_idx + float(scenario["lock_within_s"]) * src_fps
    first = next(((i, e) for i, e in locked if pattern_key(e.fields.get("pattern")) == want), None)
    if first is None:
        rows.append(row("cadence_lock", FAIL, "pattern %s never locked (saw %s)" % (
            exp.get("pattern"), sorted(set(pattern_key(e.fields.get("pattern")) for _, e in locked)) or "no lock")))
    else:
        at = log_idx[first[0]]
        ok = at is None or at <= lim
        rows.append(row("cadence_lock", PASS if ok else FAIL,
                        "locked %s at source frame %s (limit %.0f)" % (exp.get("pattern"), at, lim)))
    wrong = [e for i, e in locked if pattern_key(e.fields.get("pattern")) != want]
    rows.append(row("cadence_false_lock", PASS if not wrong else FAIL,
                    "%d locks on a different pattern" % len(wrong)))
    stray = [e for i, e in cad if e.fields.get("state") == "lost" and log_idx[i] is not None
             and windows.which(int(log_idx[i])) is None]
    rows.append(row("cadence_steady", PASS if not stray else FAIL, "%d lost-lock lines outside event windows" % len(stray)))
    last = cad[-1][1]
    rows.append(row("cadence_final", PASS if last.fields.get("state") == "locked" else FAIL,
                    "last cadence line: %s" % last.fields.get("state")))
    return rows


def unexplained_rows(unmatched, log_idx, scenario, first_idx, src_fps, windows, has_expected):
    bad = []
    minor = 0
    grace = first_idx + float(scenario["start_grace_s"]) * src_fps
    for i, e in unmatched:
        cause = e.fields.get("cause", "")
        lvl = e.fields.get("level")
        if cause.startswith("pulldown_") or lvl == "none":
            continue
        idx = log_idx[i]
        if cause in ("start", "start_after_source_change") and (idx is None or idx <= grace):
            continue
        if lvl == "minor":
            minor += 1
            continue
        if idx is not None and windows.which(int(idx)) is not None and e.fields.get("cause") in (
                "queue_generation_other", "source_format_changed", "start_after_source_change", "in_place_source_change"):
            continue
        bad.append((cause, lvl, idx))
    return [row("unexplained_resyncs", PASS if not bad else FAIL,
                "%d FULL/TIMING resyncs with no cause event (first: %s); %d MINOR unexplained (counted under drops)" % (
                    len(bad), bad[:3], minor), count=len(bad))]


def levels_rows(frames_fn, steady_positions, scenario, size):
    lv = scenario.get("levels")
    if not lv:
        return []
    if frames_fn is None:
        return [row("levels", NODATA, "levels need a raw recording (not a stamps CSV)")]
    import numpy as np  # noqa: F401
    import check as lvl
    import expected as ex
    import patterns
    first = next(iter(frames_fn()), None)
    if first is None:
        return [row("levels", NODATA, "recording has no frames")]
    h, w = first[0].shape
    layout = patterns.build_layout(lv["set"], w, h)
    chain = ex.chain_from_spec(lv["src"], lv.get("dst"))
    want = set(steady_positions[: int(lv.get("frames", 24))])
    if not want:
        return [row("levels", NODATA, "no steady frames to sample")]

    def picked():
        for pos, fr in enumerate(frames_fn()):
            if pos in want:
                yield fr
    rep = lvl.evaluate(layout, chain, picked(), lv.get("thresholds"))
    out = [row("levels", {"PASS": PASS, "FAIL": FAIL}.get(rep["verdict"], NODATA),
               "%s over %d frames (%s)" % (rep["verdict"], rep["frames_read"], lv["set"]), rows=len(rep["rows"]))]
    for r in rep["rows"]:
        if r["id"] in ("btb_reached", "wtw_reached") or r["kind"] == "ramp" or r["status"] != "PASS":
            d = r.get("criterion") or r.get("reason") or ""
            de = r.get("de2000", r.get("de_itp"))
            out.append(row("levels_" + r["id"], {"PASS": PASS, "FAIL": FAIL}.get(r["status"], NODATA),
                           "%s %s" % (d, ("dE %.3f" % de) if de is not None else ""),
                           max_code_error=r.get("max_code_error")))
    return out


# ---- driver ------------------------------------------------------------------------------------
def analyze(scenario, rows_stamps, frames_fn, sent, log_events, expected=None, cap_times=None, t0_ms=None,
            frame_size=None):
    out_rate = scenario["out_rate"]
    out_period = Fraction(out_rate[1], out_rate[0])
    sp = src_period(sent, scenario)
    r = float(sp / out_period)
    src_fps = float(1 / sp)
    scenario = dict(scenario)
    scenario["_out_fps"] = float(1 / out_period)
    first_idx = sent[0]["index"]
    src = [x[0] for x in rows_stamps]
    sent_idx = [s["index"] for s in sent]
    log_idx = assign_indices(log_events, src_fps)
    windows = Windows()
    ev_rows, unmatched = [], []
    # event windows must exist before sequence classification: compute them from the matching first
    seq_pre = analyze_sequence(src, r, sent_idx, Windows(), scenario)
    if expected:
        ev_rows, unmatched = score_events(expected, log_events, log_idx, sent, scenario, src_fps, seq_pre, windows)
        seqres = analyze_sequence(src, r, sent_idx, windows, scenario)
        # re-score with windows known so excess/missing causes line up (windows only affect classification)
        ev_rows2, _ = score_events(expected, log_events, log_idx, sent, scenario, src_fps, seqres, Windows())
        ev_rows = ev_rows2
    else:
        seqres = seq_pre
        unmatched = [(i, e) for i, e in enumerate(log_events)
                     if e.kind == "resync" and e.fields.get("level") in ("full", "minor", "timing")]
    hours = max(0.0, (len(src) * float(out_period)) / 3600.0)
    rows = []
    rows += capture_rows(rows_stamps, scenario)
    rows += pacing_rows(seqres, r, hours, scenario, bool(expected))
    rows += drop_rows(seqres, hours, scenario, windows)
    rows += latency_rows(seqres, build_runs(src), sent, scenario, float(out_period), cap_times, t0_ms)
    rows += cadence_rows(log_events, log_idx, seqres, windows, scenario, src_fps, first_idx, sent[-1]["index"])
    rows += unexplained_rows(unmatched, log_idx, scenario, first_idx, src_fps, windows, bool(expected))
    steady_pos = []
    if frames_fn is not None and scenario.get("levels"):
        steady = [s for s in seqres["segments"] if not s["window"] and s["runs"] >= scenario["min_segment_frames"]]
        wanted = set()
        for s in steady:
            wanted.update(range(s["first"], s["last"] + 1))
        steady_pos = [pos for pos, k in enumerate(src) if k in wanted]
    rows += levels_rows(frames_fn, steady_pos, scenario, frame_size)
    if expected:
        for er in ev_rows:
            rows.append(row("event_" + er["id"], er["status"], "%s %s" % (er["kind"], "; ".join(
                "%s: %s" % (k, v[1]) for k, v in er["checks"].items() if v[0] in (FAIL, NODATA)) or "ok")))
    statuses = [x["status"] for x in rows]
    verdict = "FAIL" if FAIL in statuses else ("INCOMPLETE" if NODATA in statuses or not rows else "PASS")
    events_json = []
    for er in ev_rows:
        e2 = dict(er)
        e2["checks"] = {k: {"status": v[0], "detail": v[1]} for k, v in er["checks"].items()}
        events_json.append(e2)
    summary = {"events_total": len(ev_rows),
               "events_pass": sum(1 for e in ev_rows if e["status"] == PASS),
               "events_fail": sum(1 for e in ev_rows if e["status"] == FAIL),
               "events_no_data": sum(1 for e in ev_rows if e["status"] == NODATA)}
    return {"scenario": scenario.get("id"), "verdict": verdict, "src_fps": src_fps, "ratio_out_per_src": r,
            "captured_frames": len(src), "hours": round(hours, 4), "rows": rows, "events": events_json,
            "summary": summary}


def render_md(rep):
    L = ["# Loop-rig report: %s (%s)" % (rep["scenario"], rep["verdict"]), "",
         "Captured frames: %d (%.3f h), source %.4f fps, output periods per source frame %.4f" % (
             rep["captured_frames"], rep["hours"], rep["src_fps"], rep["ratio_out_per_src"]), "",
         "| row | status | detail |", "|---|---|---|"]
    for r in rep["rows"]:
        L.append("| %s | %s | %s |" % (r["id"], r["status"], r["detail"].replace("|", "/")))
    s = rep["summary"]
    if s["events_total"]:
        L += ["", "Resync events: %d total, %d pass, %d fail, %d no data" % (
            s["events_total"], s["events_pass"], s["events_fail"], s["events_no_data"]), "",
            "| event | kind | status | drops | repeats | failing checks |", "|---|---|---|---|---|---|"]
        for e in rep["events"]:
            bad = "; ".join("%s: %s" % (k, v["detail"]) for k, v in e["checks"].items() if v["status"] in (FAIL, NODATA))
            L.append("| %s | %s | %s | %s | %s | %s |" % (e["id"], e["kind"], e["status"], e.get("drops", ""),
                                                       e.get("repeats", ""), bad.replace("|", "/")))
    return "\n".join(L) + "\n"


def write_report(rep, out):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=1)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write(render_md(rep))


def load_cap_times(path):
    out = {}
    with open(path, "r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["pos"])] = float(r["t_ms"])
    n = max(out) + 1 if out else 0
    return [out.get(i) for i in range(n)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--scenarios", default=os.path.join(HERE, "scenarios.json"))
    ap.add_argument("--recording", required=True)
    ap.add_argument("--sent", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--expected")
    ap.add_argument("--capture-times")
    ap.add_argument("--capture-t0-ms", type=float)
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--out", default="looprig-report")
    a = ap.parse_args(argv)
    try:
        sc = load_scenario(a.scenarios, a.scenario)
        if a.width:
            sc["width"] = a.width
        if a.height:
            sc["height"] = a.height
        sent = load_sent(a.sent)
        stamps, frames_fn = decode_recording(a.recording, sc)
        events = vplog.parse_file(a.log)
        expected = None
        if a.expected:
            with open(a.expected, "r", encoding="utf-8") as f:
                expected = json.load(f)
        cap = load_cap_times(a.capture_times) if a.capture_times else None
        size = (sc.get("width"), sc.get("height"))
        rep = analyze(sc, stamps, frames_fn, sent, events, expected, cap, a.capture_t0_ms, size)
    except (InputError, OSError, ValueError, KeyError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    write_report(rep, a.out)
    print("%s -> %s" % (rep["verdict"], os.path.join(a.out, "report.md")))
    return {"PASS": 0, "FAIL": 1}.get(rep["verdict"], 3)


if __name__ == "__main__":
    sys.exit(main())
