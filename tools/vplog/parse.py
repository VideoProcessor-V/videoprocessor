# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VideoProcessor log parser: structured "vp-event/1" (VPEV1) lines plus legacy adapters.

Library use (other tools do sys.path.insert(<tools/vplog>) then ``import parse``):

    events = parse.parse_file(path)      # list of Event; ``events.skipped`` = unmatched lines
    parse.category(event)                # drop | cadence | resync | timing | other
    parse.summarize(events)              # dict, see README

CLI: python -I tools/vplog/parse.py LOG [--json] [--summary] [--since HH:MM:SS] [--kinds a,b]
"""

import argparse
import dataclasses
import datetime
import json
import re
import sys
from typing import Dict, Iterable, List, Optional, Tuple

# ----------------------------------------------------------------------------------------------
# Event model

KINDS = frozenset(["session", "resync", "fallback", "gate", "gpu_error", "source_change",
                   "cadence", "repeat", "frame_stats", "stamp", "flag", "resync_settle"])

_PREFIX_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) \| ")
_KEY_RE = re.compile(r"^[a-z0-9_]+$")
_BARE_RE = re.compile(r"^[A-Za-z0-9_.:+/-]+$")
_T_RE = re.compile(r"^\d+(\.\d+)?$")


@dataclasses.dataclass
class Event:
    line_no: int
    wall: Optional[datetime.datetime]
    t_ms: Optional[float]
    kind: str
    fields: Dict[str, str]
    source: str          # "vpev" or "legacy"
    raw: str

    @property
    def legacy(self) -> bool:
        return self.source == "legacy"


class EventList(list):
    """list of Event; ``skipped`` counts the ordinary (non-event) lines that were not matched."""
    skipped = 0


# ----------------------------------------------------------------------------------------------
# VPEV1 grammar

class _Bad(Exception):
    def __init__(self, reason: str):
        Exception.__init__(self, reason)
        self.reason = reason


def _tokenize(body: str) -> List[Tuple[str, str]]:
    pairs: List[Tuple[str, str]] = []
    i, n = 0, len(body)
    while True:
        while i < n and body[i] in " \t":
            i += 1
        if i >= n:
            return pairs
        start = i
        while i < n and body[i] not in "= \t":
            i += 1
        if i >= n or body[i] != "=":
            raise _Bad("bad_key")
        key = body[start:i]
        if not _KEY_RE.match(key):
            raise _Bad("bad_key")
        i += 1  # '='
        if i < n and body[i] == '"':
            i += 1
            chars: List[str] = []
            while True:
                if i >= n:
                    raise _Bad("unbalanced_quote")
                c = body[i]
                if c == "\\":
                    if i + 1 >= n:
                        raise _Bad("unbalanced_quote")
                    nxt = body[i + 1]
                    if nxt not in '"\\':
                        raise _Bad("bad_quoting")
                    chars.append(nxt)
                    i += 2
                elif c == '"':
                    i += 1
                    break
                else:
                    chars.append(c)
                    i += 1
            if i < n and body[i] not in " \t":
                raise _Bad("bad_quoting")
            pairs.append((key, "".join(chars)))
        else:
            start = i
            while i < n and body[i] not in " \t":
                i += 1
            value = body[start:i]
            if '"' in value:
                raise _Bad("bad_quoting")
            if not _BARE_RE.match(value):
                raise _Bad("bad_value")
            pairs.append((key, value))


def _parse_vpev(body: str) -> Tuple[Optional[float], str, Dict[str, str]]:
    """body = text after the VPEV1 token. Returns (t_ms, kind, fields); kind 'malformed' on error."""
    t_ms: Optional[float] = None
    try:
        pairs = _tokenize(body)
        keys = [k for k, _ in pairs]
        if not pairs or pairs[0][0] != "t":
            raise _Bad("bad_token_order" if "t" in keys else "missing_t")
        if not _T_RE.match(pairs[0][1]):
            raise _Bad("bad_t")
        t_ms = float(pairs[0][1])
        if len(pairs) < 2 or pairs[1][0] != "kind":
            raise _Bad("bad_token_order" if "kind" in keys else "missing_kind")
        kind = pairs[1][1]
        if kind not in KINDS:
            raise _Bad("unknown_kind")
        fields: Dict[str, str] = {}
        for k, v in pairs[2:]:
            if k in fields or k in ("t", "kind"):
                raise _Bad("duplicate_key")
            fields[k] = v
        return t_ms, kind, fields
    except _Bad as bad:
        return t_ms, "malformed", {"reason": bad.reason}


# ----------------------------------------------------------------------------------------------
# Resync vocabulary (src/VP-V/core/ResyncCause.h: Token / Describe / LevelOf)

# token -> (Describe text, level)
_CAUSES = {
    "start": ("start", "full"),
    "start_after_source_change": ("start after a source change", "full"),
    "in_place_source_change": ("source change", "full"),
    "renderer_reset": ("renderer reset", "full"),
    "live_queue_reset": ("seek, skip or capture re-lock", "full"),
    "calibration_start": ("calibration patterns started", "full"),
    "calibration_end": ("calibration patterns ended", "full"),
    "queue_generation_other": ("queue reset", "full"),
    "file_discontinuity": ("file discontinuity", "full"),
    "counter_backwards": ("frame counter went backwards", "full"),
    "timestamp_break_large": ("large timestamp break", "full"),
    "source_format_changed": ("source format changed", "full"),
    "missed_frames": ("missed frames", "minor"),
    "timestamp_break": ("timestamp break", "minor"),
    "stale_delivery": ("stale capture delivery", "minor"),
    "stale_delivery_ended": ("stale capture delivery ended", "minor"),
    "overflow_drop": ("queue overflow drop", "minor"),
    "repeat_preserving_drop": ("repeat-preserving drop", "minor"),
    "pulldown_lock_change": ("pulldown lock change", "minor"),
    "pulldown_rephase": ("pulldown re-phase", "minor"),
    "pulldown_clock_restart": ("pulldown clock restart", "minor"),
    "pulldown_off": ("pulldown off", "minor"),
    "gap_other": ("gap (other)", "minor"),
    "drift_drop": ("drift drop", "minor"),
    "kept_run_hole": ("kept-run hole", "minor"),
    "rate_step": ("frame rate step", "timing"),
    "settings_apply": ("settings applied", "none"),
    "rebase_decision": ("re-base decision", "none"),
    "graph_rebuild": ("graph rebuilt", "none"),
}
_DESCRIBE_TO_TOKEN = {text: tok for tok, (text, _) in _CAUSES.items()}

_DROP_CAUSES = frozenset(["missed_frames", "overflow_drop", "repeat_preserving_drop", "drift_drop",
                          "kept_run_hole", "stale_delivery", "gap_other",
                          "timestamp_break", "stale_delivery_ended"])
_RESYNC_CAUSES = frozenset(["start", "start_after_source_change", "in_place_source_change",
                            "renderer_reset", "live_queue_reset", "calibration_start",
                            "calibration_end", "queue_generation_other", "file_discontinuity",
                            "counter_backwards", "timestamp_break_large", "source_format_changed"])


def _cause_category(cause: str, level: str) -> str:
    if cause.startswith("pulldown_"):
        return "cadence"
    if cause == "rate_step":
        return "timing"
    if cause in _DROP_CAUSES:
        return "drop"
    if cause in _RESYNC_CAUSES:
        return "resync"
    if cause in _CAUSES or cause.startswith("settings_") or cause in ("none", "unknown"):
        return "other"
    # unknown token: fall back on the level field
    return {"full": "resync", "minor": "drop", "timing": "timing"}.get(level, "other")


def category(event: Event) -> str:
    """drop | cadence | resync | timing | other (table in README)."""
    if event.kind == "resync":
        return _cause_category(event.fields.get("cause", ""), event.fields.get("level", ""))
    if event.kind == "cadence":
        return "cadence"
    if event.kind == "repeat":
        return "drop"
    return "other"


# ----------------------------------------------------------------------------------------------
# Legacy adapters (regexes over the existing free-text lines)

_KV_RE = re.compile(r"(\w+)=(\S+)")
_RESYNC_LEGACY = re.compile(r"^Resync \((shadow|on)\): (FULL|MINOR|TIMING) at seq (\d+) \((.*)\)\s*$")
_GATE_RESET = re.compile(r"^Render-ahead gate: reset \((.*?); was (\w+)\)\s*$")
_GATE_CHANGE = re.compile(r"^Render-ahead gate: (.+?) -> (.+?) \((.*)\)\s*$")
_QUEUE_GEN = re.compile(r"^Alpha queue generation armed: (.*)$")
_BUILD_ID = re.compile(r"^VP build identity: (.*)$")
_FALLBACKS = re.compile(r"(?:^|[\s;=\]:])Fallbacks engaged: (.+?)\s*$")
_LP_ERROR = re.compile(r"^libplacebo \[(error|fatal)\]: (.*)$")
_PULLDOWN = re.compile(r"^\s*Pulldown: (.+?) \(was (.+)\)\s*$")


def _split_top(text: str) -> List[str]:
    """Split on ';' at parenthesis depth 0."""
    parts, depth, cur = [], 0, []
    for c in text:
        if c == "(":
            depth += 1
        elif c == ")":
            depth = max(0, depth - 1)
        if c == ";" and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
    parts.append("".join(cur).strip())
    return [p for p in parts if p]


def _legacy(msg: str) -> List[Tuple[str, Dict[str, str]]]:
    out: List[Tuple[str, Dict[str, str]]] = []

    def add(kind: str, fields: Dict[str, str]) -> None:
        fields["legacy"] = "1"
        out.append((kind, fields))

    m = msg.strip()
    if m == "=== VP LOG SESSION START ===":
        add("session", {"build_id": ""})
        return out
    mm = _BUILD_ID.match(m)
    if mm:
        kv = dict(_KV_RE.findall(mm.group(1)))
        commit = kv.get("commit", "")
        add("session", {"build_id": kv.get("build", ""), "commit": commit.rsplit("/", 1)[-1],
                        **{k: kv[k] for k in ("module", "branch", "dirty") if k in kv}})
        return out
    mm = _QUEUE_GEN.match(m)
    if mm:
        kv = dict(_KV_RE.findall(mm.group(1)))
        reason = kv.pop("reason", "")
        cause = reason if reason in _CAUSES else "queue_generation_other"
        fields = {"level": "full", "cause": cause}
        if cause != reason:
            fields["detail"] = reason
        fields.update(kv)
        add("resync", fields)
        return out
    mm = _RESYNC_LEGACY.match(m)
    if mm:
        mode, label, seq, text = mm.groups()
        parts = [p.strip() for p in text.split(";")]
        cause = _DESCRIBE_TO_TOKEN.get(parts[0], "unknown")
        fields = {"level": label.lower(), "cause": cause, "seq": seq, "mode": mode}
        for p in parts[1:]:
            g = re.match(r"^gap (\d+)$", p)
            if g:
                fields["gap"] = g.group(1)
            else:
                fields["detail"] = p
        if cause == "unknown":
            fields["detail"] = text
        add("resync", fields)
        return out
    mm = _GATE_RESET.match(m)
    if mm:
        add("gate", {"from": mm.group(2), "to": "reset", "reason": mm.group(1)})
        return out
    mm = _GATE_CHANGE.match(m)
    if mm:
        add("gate", {"from": mm.group(1), "to": mm.group(2), "reason": mm.group(3).split(";")[0].strip()})
        return out
    mm = _LP_ERROR.match(m)
    if mm:
        add("gpu_error", {"api": "libplacebo", "code": mm.group(1), "what": mm.group(2)})
        return out
    mm = _PULLDOWN.match(msg)
    if mm:
        now, was = mm.group(1), mm.group(2)
        if now == "lost":
            add("cadence", {"state": "lost", "pattern": was, "was": was})
        else:
            add("cadence", {"state": "locked", "pattern": now, "was": was})
        return out
    mm = _FALLBACKS.search(msg)
    if mm:
        text = mm.group(1)
        if text.strip() != "none":
            for entry in _split_top(text):
                idx = entry.find(" (")
                if idx > 0 and entry.endswith(")"):
                    add("fallback", {"state": "engaged", "id": entry[:idx], "reason": entry[idx + 2:-1]})
                else:
                    add("fallback", {"state": "engaged", "id": entry})
        return out
    return out


# ----------------------------------------------------------------------------------------------
# Public parsing API

def parse_lines(lines: Iterable[str]) -> List[Event]:
    """Parse log lines into events. Returns an EventList; its ``skipped`` attribute counts the
    non-empty lines that were neither VPEV1 nor a legacy match."""
    events = EventList()
    for line_no, line in enumerate(lines, 1):
        raw = line.rstrip("\r\n")
        wall: Optional[datetime.datetime] = None
        msg = raw
        pm = _PREFIX_RE.match(raw)
        if pm:
            try:
                wall = datetime.datetime.strptime(pm.group(1), "%Y-%m-%d %H:%M:%S")
            except ValueError:
                wall = None
            msg = raw[pm.end():]
        if msg.startswith("VPEV1") and (len(msg) == 5 or msg[5] in " \t"):
            t_ms, kind, fields = _parse_vpev(msg[5:].rstrip())
            events.append(Event(line_no, wall, t_ms, kind, fields, "vpev", raw))
            continue
        found = _legacy(msg)
        if not found:
            if raw.strip():
                events.skipped += 1
            continue
        for kind, fields in found:
            events.append(Event(line_no, wall, None, kind, fields, "legacy", raw))
    return events


def parse_file(path) -> List[Event]:
    with open(path, "r", encoding="utf-8", errors="replace", newline=None) as handle:
        return parse_lines(handle)


def _bump(table: Dict[str, int], key: str) -> None:
    table[key] = table.get(key, 0) + 1


def summarize(events: Iterable[Event], skipped: Optional[int] = None) -> dict:
    """Counts and time span. ``skipped`` (optional) adds a "skipped" key; when ``events`` is the
    EventList returned by parse_lines/parse_file it is taken from that if not given."""
    if skipped is None:
        skipped = getattr(events, "skipped", None)
    events = list(events)
    kinds: Dict[str, int] = {}
    cats: Dict[str, int] = {}
    causes: Dict[str, int] = {}
    walls = [e.wall for e in events if e.wall is not None]
    ts = [e.t_ms for e in events if e.t_ms is not None]
    malformed = 0
    for e in events:
        _bump(kinds, e.kind)
        _bump(cats, category(e))
        if e.kind == "malformed":
            malformed += 1
        elif "cause" in e.fields:
            _bump(causes, e.fields["cause"])
    result = {
        "counts_by_kind": kinds,
        "counts_by_category": cats,
        "counts_by_cause": causes,
        "first_wall": walls[0].isoformat() if walls else None,
        "last_wall": walls[-1].isoformat() if walls else None,
        "first_t_ms": ts[0] if ts else None,
        "last_t_ms": ts[-1] if ts else None,
        "malformed": malformed,
        "total": len(events),
    }
    if skipped is not None:
        result["skipped"] = skipped
    return result


# ----------------------------------------------------------------------------------------------
# CLI

def _event_dict(e: Event) -> dict:
    return {"line_no": e.line_no, "wall": e.wall.isoformat() if e.wall else None, "t_ms": e.t_ms,
            "kind": e.kind, "category": category(e), "fields": e.fields, "source": e.source,
            "legacy": e.legacy, "raw": e.raw}


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Parse a VideoProcessor log into events.")
    ap.add_argument("log")
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("--summary", action="store_true", help="summary instead of the event list")
    ap.add_argument("--since", help="HH:MM:SS: keep events whose wall time of day is at or after "
                                    "it (events without a wall time are dropped)")
    ap.add_argument("--kinds", help="comma-separated kinds to keep")
    args = ap.parse_args(argv)

    events = parse_file(args.log)
    skipped = events.skipped
    if args.since:
        try:
            since = datetime.datetime.strptime(args.since, "%H:%M:%S").time()
        except ValueError:
            ap.error("--since must be HH:MM:SS")
        events = [e for e in events if e.wall is not None and e.wall.time() >= since]
    if args.kinds:
        wanted = set(k.strip() for k in args.kinds.split(",") if k.strip())
        events = [e for e in events if e.kind in wanted]

    out = sys.stdout
    if args.summary:
        summary = summarize(events, skipped)
        if args.json:
            json.dump(summary, out, indent=2, sort_keys=True)
            out.write("\n")
        else:
            for key, value in summary.items():
                out.write("%s: %s\n" % (key, json.dumps(value, sort_keys=True)))
    elif args.json:
        json.dump([_event_dict(e) for e in events], out, indent=1)
        out.write("\n")
    else:
        for e in events:
            fields = " ".join("%s=%s" % (k, json.dumps(v) if not _BARE_RE.match(v) else v)
                              for k, v in e.fields.items())
            out.write("%d\t%s\t%s\t%s\t%s\t%s\n" % (
                e.line_no, e.wall.isoformat(sep=" ") if e.wall else "-",
                "-" if e.t_ms is None else "%.3f" % e.t_ms, e.kind, category(e), fields))
    return 0


if __name__ == "__main__":
    sys.exit(main())
