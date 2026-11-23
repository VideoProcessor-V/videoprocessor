# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""End-to-end frame stamp codec and Row-27 verification engine (vp-e2e-stamp/1).

Integrates directly with tools/framestamp/framestamp.py (vp-stamp/1 64-bit visual stamp
with CRC-16/CCITT-FALSE and magic 0xA5) and extends it to a self-verifying per-present
telemetry record covering source PTS, capture/render/present timestamps, display vblank
index, render-ahead queue depth, pathway, mode_id, visual stamp code, and record CRC.

Every invariant tolerance (constant-delay spread/drift, cadence lattice slack, landing
accuracy, dirty-track recovery, and live rate-transition convergence) is derived from the
exact rational source_period (1 / source_hz) and display_period (1 / display_hz) — never
from hardcoded millisecond literals.
"""
import csv
import json
import math
import os
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
for _sub in ("framestamp", "looprig"):
    _p = os.path.join(TOOLS, _sub)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import framestamp as fs  # noqa: E402
import row27_grid as rg  # noqa: E402

SCHEMA_STAMP = "vp-e2e-stamp/1"
PASS = "PASS"
FAIL = "FAIL"
INFO = "INFO"

CORE_FIELDS = (
    "frame_seq",
    "source_pts_ns",
    "capture_ts_ns",
    "render_submit_ts_ns",
    "present_ts_ns",
    "display_vblank_idx",
    "queue_depth",
    "pathway",
    "mode_id",
    "visual_stamp_code",
)

EXTENDED_FIELDS = (
    "planned_vblank_idx",
    "is_extra",
    "gate_level",
    "fallback_reason",
    "segment_idx",
    "is_dirty_event",
    "pixel_hash",
)

ALL_FIELDS = CORE_FIELDS + EXTENDED_FIELDS + ("record_crc",)


def _canonical_crc_bytes(rec):
    """Deterministic UTF-8 byte serialization of all record fields except record_crc."""
    parts = []
    for k in CORE_FIELDS + EXTENDED_FIELDS:
        v = rec.get(k)
        if isinstance(v, bool):
            s = "1" if v else "0"
        elif v is None:
            s = ""
        else:
            s = str(v)
        parts.append("%s=%s" % (k, s))
    return "|".join(parts).encode("utf-8")


def compute_record_crc(rec):
    """4-hex-char CRC-16/CCITT-FALSE over canonical record payload."""
    return "%04X" % fs.crc16(_canonical_crc_bytes(rec))


def encode_visual_stamp(frame_seq, kind=fs.KIND_SOURCE, aux=0):
    """Encode 64-bit vp-stamp/1 visual stamp code as 16-hex-char string."""
    w = fs.word(kind, aux & 63, int(frame_seq) & 0xFFFFFFFF)
    return "%016X" % w


def decode_visual_stamp(code_hex):
    """Decode and validate a 16-hex-char vp-stamp/1 64-bit visual stamp word."""
    if not isinstance(code_hex, str) or len(code_hex) != 16:
        return {"valid": False, "reason": "visual_stamp_code must be 16 hex chars"}
    try:
        val = int(code_hex, 16)
    except ValueError:
        return {"valid": False, "reason": "visual_stamp_code not hex"}
    magic = (val >> 56) & 0xFF
    if magic != fs.MAGIC:
        return {"valid": False, "reason": "bad magic 0x%02X (expected 0x%02X)" % (magic, fs.MAGIC)}
    crc_calc = fs.crc16((val >> 16).to_bytes(6, "big"))
    crc_got = val & 0xFFFF
    if crc_calc != crc_got:
        return {"valid": False, "reason": "visual stamp CRC mismatch 0x%04X != 0x%04X" % (crc_got, crc_calc)}
    return {
        "valid": True,
        "kind": (val >> 54) & 3,
        "aux": (val >> 48) & 63,
        "index": (val >> 16) & 0xFFFFFFFF,
        "crc": crc_got,
        "word": val,
    }


def make_record(
    frame_seq,
    source_pts_ns,
    capture_ts_ns,
    render_submit_ts_ns,
    present_ts_ns,
    display_vblank_idx,
    queue_depth,
    pathway,
    mode_id,
    visual_stamp_code=None,
    visual_aux=0,
    planned_vblank_idx=None,
    is_extra=False,
    gate_level="off",
    fallback_reason="",
    segment_idx=0,
    is_dirty_event=False,
    pixel_hash="",
):
    """Construct a canonical self-verifying vp-e2e-stamp/1 record with CRC."""
    if visual_stamp_code is None:
        visual_stamp_code = encode_visual_stamp(frame_seq, fs.KIND_SOURCE, visual_aux)
    rec = {
        "frame_seq": int(frame_seq),
        "source_pts_ns": int(source_pts_ns),
        "capture_ts_ns": int(capture_ts_ns),
        "render_submit_ts_ns": int(render_submit_ts_ns),
        "present_ts_ns": int(present_ts_ns),
        "display_vblank_idx": int(display_vblank_idx),
        "queue_depth": int(queue_depth),
        "pathway": str(pathway),
        "mode_id": str(mode_id),
        "visual_stamp_code": str(visual_stamp_code).upper(),
        "planned_vblank_idx": None if planned_vblank_idx is None else int(planned_vblank_idx),
        "is_extra": bool(is_extra),
        "gate_level": str(gate_level),
        "fallback_reason": str(fallback_reason),
        "segment_idx": int(segment_idx),
        "is_dirty_event": bool(is_dirty_event),
        "pixel_hash": str(pixel_hash),
    }
    rec["record_crc"] = compute_record_crc(rec)
    return rec


def verify_record(rec):
    """Validate schema fields, record_crc, and embedded vp-stamp/1 visual_stamp_code.

    Returns (ok: bool, errors: list[str]).
    """
    errors = []
    if not isinstance(rec, dict):
        return False, ["record is not a dict"]
    for k in CORE_FIELDS + ("record_crc",):
        if k not in rec:
            errors.append("missing field %s" % k)
    if errors:
        return False, errors

    # Verify record CRC
    want_crc = compute_record_crc(rec)
    got_crc = str(rec.get("record_crc", "")).upper()
    if got_crc != want_crc:
        errors.append("record_crc mismatch got=%s expected=%s" % (got_crc, want_crc))

    # Verify embedded 64-bit vp-stamp/1 visual stamp code
    vs = decode_visual_stamp(str(rec["visual_stamp_code"]).upper())
    if not vs["valid"]:
        errors.append("visual_stamp_code invalid: %s" % vs["reason"])
    else:
        if vs["index"] != (int(rec["frame_seq"]) & 0xFFFFFFFF):
            errors.append("visual_stamp_code index %d != frame_seq %d" % (vs["index"], int(rec["frame_seq"])))

    if str(rec["pathway"]) not in ("original", "vpv"):
        errors.append("invalid pathway %r" % (rec["pathway"],))
    if int(rec["queue_depth"]) < 0:
        errors.append("negative queue_depth %r" % (rec["queue_depth"],))

    return (len(errors) == 0), errors


def write_jsonl(path, records):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r, sort_keys=False) + "\n")


def read_jsonl(path):
    records = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if s:
                records.append(json.loads(s))
    return records


def write_csv(path, records):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(ALL_FIELDS))
        w.writeheader()
        for r in records:
            row = {}
            for k in ALL_FIELDS:
                v = r.get(k)
                if isinstance(v, bool):
                    row[k] = "1" if v else "0"
                elif v is None:
                    row[k] = ""
                else:
                    row[k] = v
            w.writerow(row)


def read_csv(path):
    records = []
    int_cols = {"frame_seq", "source_pts_ns", "capture_ts_ns", "render_submit_ts_ns",
                "present_ts_ns", "display_vblank_idx", "queue_depth", "segment_idx"}
    opt_int_cols = {"planned_vblank_idx"}
    bool_cols = {"is_extra", "is_dirty_event"}
    with open(path, "r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            out = {}
            for k in ALL_FIELDS:
                raw = r.get(k, "")
                if k in int_cols:
                    out[k] = int(raw)
                elif k in opt_int_cols:
                    out[k] = None if raw in ("", None) else int(raw)
                elif k in bool_cols:
                    out[k] = raw in ("1", "true", "True", True)
                else:
                    out[k] = "" if raw is None else str(raw)
            records.append(out)
    return records


def from_looprig_artifacts(stamps_csv_path, sent_csv_path, cell, t0_ns=0, capture_offset_vblanks=None):
    """Convert existing StreamRecorder stamps.csv + sent.csv into vp-e2e-stamp/1 records.

    Allows real hardware rig captures from tools/looprig/run_loop_tier.ps1 or
    VideoProcessorStreamRecorder.exe --stamps to be scored directly by stamp_verify.py.
    """
    sent_by_idx = {}
    has_wall_t = False
    with open(sent_csv_path, "r", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            idx = int(r["index"])
            pts = Fraction(int(r["pts_num"]), int(r["pts_den"]))
            t_ms = r.get("t_ms") or r.get("qpc_ms")
            if t_ms:
                has_wall_t = True
            sent_by_idx[idx] = {
                "aux": int(r.get("aux") or 0),
                "pts_ns": int(round(float(pts) * 1e9)),
                "t_ns": int(round(float(t_ms) * 1e6)) if t_ms else int(round(float(pts) * 1e9)),
            }

    disp_rate = rg.parse_rate(cell["display_rate"])
    disp_period_ns = float(Fraction(10**9) / disp_rate)
    if capture_offset_vblanks is None:
        if has_wall_t:
            offset_vb = 0
        else:
            offset_vb = (3 + int(cell["resolved_queue_depth"])) if cell.get("render_ahead") else 3
    else:
        offset_vb = int(capture_offset_vblanks)
    seen_seq = set()
    records = []
    with open(stamps_csv_path, "r", encoding="utf-8", newline="") as f:
        for pos, r in enumerate(csv.DictReader(f)):
            raw_src = r.get("src_index", "")
            if raw_src in ("", None):
                # Preserve undecodable capture frame as a corrupted stamp record so verifier flags it
                rec = make_record(
                    frame_seq=-1,
                    source_pts_ns=0,
                    capture_ts_ns=t0_ns,
                    render_submit_ts_ns=t0_ns,
                    present_ts_ns=int(round(t0_ns + (offset_vb + pos) * disp_period_ns)),
                    display_vblank_idx=pos,
                    queue_depth=cell["resolved_queue_depth"],
                    pathway=cell["pathway"],
                    mode_id=cell["cell_id"],
                    visual_stamp_code="0000000000000000",
                )
                records.append(rec)
                continue
            k = int(raw_src)
            aux = int(r.get("src_aux") or 0)
            sinfo = sent_by_idx.get(k, {"aux": aux, "pts_ns": 0, "t_ns": 0})
            present_ns = int(round(t0_ns + (offset_vb + pos) * disp_period_ns))
            cap_ns = sinfo["t_ns"] if has_wall_t else (t0_ns + sinfo["t_ns"])
            sub_ns = min(present_ns, max(cap_ns, present_ns - int(round(0.25 * disp_period_ns))))
            is_extra = (k in seen_seq) and bool(cell.get("dejudder"))
            seen_seq.add(k)
            records.append(make_record(
                frame_seq=k,
                source_pts_ns=sinfo["pts_ns"],
                capture_ts_ns=cap_ns,
                render_submit_ts_ns=sub_ns,
                present_ts_ns=present_ns,
                display_vblank_idx=pos,
                queue_depth=cell["resolved_queue_depth"],
                pathway=cell["pathway"],
                mode_id=cell["cell_id"],
                visual_aux=aux,
                planned_vblank_idx=pos if cell.get("render_ahead") else None,
                is_extra=is_extra,
                gate_level=("L%d" % cell["level"]) if cell.get("render_ahead") else "off",
            ))
    return records


# ---- math helpers (period-derived, never fixed ms) ---------------------------------------------

def percentile(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    pos = (len(s) - 1) * q
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def natural_spread(ratio):
    """Natural quantization spread (in display periods) of p - k*r for an ideal cadence of ratio r."""
    f = Fraction(ratio).limit_denominator(1000)
    return 1.0 - 1.0 / f.denominator


def lattice_errors(points, r, tolerance_periods):
    """Cadence lattice check over [(k, first_vblank_idx)]. Returns (error_count, [outside_periods])."""
    if len(points) < 2:
        return 0, []
    k0 = points[0][0]
    d = [p - (k - k0) * r for k, p in points]
    width = natural_spread(r) + tolerance_periods + 1e-6
    sd = sorted(d)
    best_c, best_n, j = sd[0], 0, 0
    for i in range(len(sd)):
        while sd[i] - sd[j] > width:
            j += 1
        if i - j + 1 > best_n:
            best_n, best_c = i - j + 1, sd[j]
    outside = []
    for v in d:
        if v < best_c:
            outside.append(best_c - v)
        elif v > best_c + width:
            outside.append(v - best_c - width)
    return len(outside), outside


# ---- verification engine -----------------------------------------------------------------------

def verify_cell(cell, records, bill_baseline=None):
    """Verify all end-to-end invariants for a single grid cell over its stamp records.

    Returns a structured verification report dict with:
      - status: "PASS" | "FAIL"
      - checks: dict[check_name -> {"status": PASS|FAIL|INFO, "detail": str, ...}]
      - metrics: summary numeric metrics (latency p50/p95/max, spread, cadence, drops, etc.)
    """
    src_rate = rg.parse_rate(cell["source_rate"])
    disp_rate = rg.parse_rate(cell["display_rate"])
    ratio = disp_rate / src_rate
    disp_period_ns = float(Fraction(10**9) / disp_rate)
    src_period_ns = float(Fraction(10**9) / src_rate)

    trans_rate = rg.parse_rate(cell["transition_to_rate"]) if cell.get("transition_to_rate") else None
    trans_ratio = (disp_rate / trans_rate) if trans_rate else None
    tols = cell.get("tolerances", {})
    phase_tol = float(tols.get("phase_tolerance_periods", 0.25))
    spread_slack = float(tols.get("delay_spread_slack_periods", 0.50))
    drift_limit = float(tols.get("delay_drift_limit_periods", 0.35))
    landing_tol = int(tols.get("landing_tolerance_vblanks", 0))
    max_late_frac = float(tols.get("max_late_landing_fraction", 0.0))
    dirty_recov_periods = int(tols.get("dirty_recovery_source_periods", 8))
    trans_conv_periods = int(tols.get("transition_converge_source_periods", 12))
    min_frames = int(tols.get("min_frames", 16))

    checks = {}
    metrics = {
        "total_records": len(records),
        "unique_source_frames": 0,
        "stamp_crc_errors": 0,
        "dropped_frames": 0,
        "duplicated_frames": 0,
        "out_of_order_frames": 0,
        "latency_p50_ms": None,
        "latency_p95_ms": None,
        "latency_max_ms": None,
        "latency_spread_periods": None,
        "latency_drift_periods": None,
        "cadence_errors": 0,
        "cadence_error_p99_periods": 0.0,
        "observed_queue_depth_p50": None,
        "silent_fallback_frames": 0,
        "late_landings": 0,
        "extras_presented": 0,
        "transition_converge_periods": None,
        "display_period_ms": round(disp_period_ns / 1e6, 4),
        "source_period_ms": round(src_period_ns / 1e6, 4),
    }

    # --- 1. Stamp integrity & CRC ---
    if not records:
        checks["stamp_integrity"] = {"status": FAIL, "detail": "no stamp records captured"}
        return {"status": FAIL, "checks": checks, "metrics": metrics}

    crc_errors = []
    for idx, rec in enumerate(records):
        ok, errs = verify_record(rec)
        if not ok:
            crc_errors.append((idx, "; ".join(errs)))
    metrics["stamp_crc_errors"] = len(crc_errors)
    if crc_errors:
        sample = "; ".join("rec[%d]: %s" % e for e in crc_errors[:3])
        checks["stamp_integrity"] = {
            "status": FAIL,
            "detail": "%d/%d stamp integrity failures (%s)" % (len(crc_errors), len(records), sample),
        }
    elif len(records) < min(min_frames, 4):
        checks["stamp_integrity"] = {
            "status": FAIL,
            "detail": "too few stamp records (%d < minimum %d)" % (len(records), min(min_frames, 4)),
        }
    else:
        checks["stamp_integrity"] = {
            "status": PASS,
            "detail": "all %d records passed CRC-16 and vp-stamp/1 visual code verification" % len(records),
        }

    # Pre-identify transition start record index if rate_transition
    seg1_start_rec = None
    for i, r in enumerate(records):
        if int(r.get("segment_idx", 0)) == 1:
            seg1_start_rec = i
            break

    # --- 2. Monotonic progression (drops, duplicates, out-of-order) ---
    out_of_order = []
    dropped = []
    duplicated = []
    causal_errors = []

    prev_seq = None
    prev_pts = None
    prev_present = None
    prev_vblank = None
    prev_dirty = False
    seq_counts = {}
    seq_base_counts = {}
    seq_seg = {}
    seq_dirty = {}
    seq_first_rec = {}
    seen_vblanks = set()

    for idx, r in enumerate(records):
        seq = int(r["frame_seq"])
        pts = int(r["source_pts_ns"])
        cap = int(r["capture_ts_ns"])
        sub = int(r["render_submit_ts_ns"])
        pres = int(r["present_ts_ns"])
        vb = int(r["display_vblank_idx"])
        dirty = bool(r.get("is_dirty_event", False))
        seg = int(r.get("segment_idx", 0))
        is_extra = bool(r.get("is_extra", False))

        if not (cap <= sub <= pres):
            causal_errors.append((idx, seq, cap, sub, pres))

        if vb in seen_vblanks:
            duplicated.append(("duplicate_vblank", seq, vb))
        seen_vblanks.add(vb)

        if prev_present is not None and pres <= prev_present:
            out_of_order.append(("present_ts", idx, prev_present, pres))
        if prev_vblank is not None and vb <= prev_vblank:
            out_of_order.append(("vblank", idx, prev_vblank, vb))
        if prev_seq is not None:
            if seq < prev_seq:
                out_of_order.append(("frame_seq", idx, prev_seq, seq))
            elif seq > prev_seq + 1:
                if not (dirty or prev_dirty):
                    for missing_k in range(prev_seq + 1, seq):
                        dropped.append(missing_k)
        if prev_pts is not None:
            if seq == prev_seq and pts != prev_pts:
                out_of_order.append(("source_pts_mismatch", idx, prev_pts, pts))
            elif seq > prev_seq and pts <= prev_pts:
                out_of_order.append(("source_pts", idx, prev_pts, pts))

        seq_counts[seq] = seq_counts.get(seq, 0) + 1
        if not is_extra:
            seq_base_counts[seq] = seq_base_counts.get(seq, 0) + 1
        seq_seg.setdefault(seq, seg)
        seq_dirty[seq] = seq_dirty.get(seq, False) or dirty
        if seq not in seq_first_rec:
            seq_first_rec[seq] = r

        prev_seq = seq
        prev_pts = pts
        prev_present = pres
        prev_vblank = vb
        prev_dirty = dirty

    # Detect duplicated frames:
    # (a) Any source frame presented > ceil(active_ratio) times outside dirty/transition windows
    # (b) When dejudder extras are explicitly tagged, >1 base present for a frame when ratio == 1
    #     or total count exceeding mathematical cadence upper bound.
    for seq, cnt in sorted(seq_counts.items()):
        if seq_dirty.get(seq, False):
            continue
        seg = seq_seg.get(seq, 0)
        r_active = float(trans_ratio) if (seg == 1 and trans_ratio is not None) else float(ratio)
        # Allow ceil(r_active - 1e-9) display vblanks per source frame
        max_vblanks = max(1, int(math.ceil(r_active - 1e-9)))
        if cnt > max_vblanks:
            duplicated.append(("excess_presents", seq, cnt, max_vblanks))

    metrics["unique_source_frames"] = len(seq_first_rec)
    metrics["dropped_frames"] = len(dropped)
    metrics["duplicated_frames"] = len(duplicated)
    metrics["out_of_order_frames"] = len(out_of_order)

    mono_issues = []
    if dropped:
        mono_issues.append("%d dropped frames (first: %s)" % (len(dropped), dropped[:3]))
    if duplicated:
        mono_issues.append("%d duplicated frames (first: %s)" % (len(duplicated), duplicated[:3]))
    if out_of_order:
        mono_issues.append("%d out-of-order events (first: %s)" % (len(out_of_order), out_of_order[:3]))
    if causal_errors:
        mono_issues.append("%d non-causal timestamp records" % len(causal_errors))

    checks["monotonic_progression"] = {
        "status": FAIL if mono_issues else PASS,
        "detail": "; ".join(mono_issues) if mono_issues else (
            "monotonic across %d source frames / %d presents (0 drops, 0 duplicates, 0 out-of-order)" % (
                len(seq_first_rec), len(records)
            )
        ),
    }

    # --- 3. Constant-delay gate (derived strictly from 1 / display_hz) ---
    # Evaluate on steady-state first presents per source frame (segment 0 for steady,
    # and post-convergence segment 1 for transitions; exclude dirty windows).
    seg_records = {}
    for seq, r in sorted(seq_first_rec.items()):
        if seq_dirty.get(seq, False):
            continue
        seg = int(r.get("segment_idx", 0))
        seg_records.setdefault(seg, []).append(r)

    # For segment 1 of rate_transition, skip convergence window when measuring steady-state delay
    steady_first_recs = []
    for seg, recs in sorted(seg_records.items()):
        if seg == 1 and len(recs) > trans_conv_periods:
            steady_first_recs.extend(recs[trans_conv_periods:])
        else:
            steady_first_recs.extend(recs)

    if len(steady_first_recs) < 2:
        checks["constant_delay"] = {"status": FAIL, "detail": "fewer than 2 steady frames for constant-delay check"}
    else:
        lats_ns = [int(r["present_ts_ns"]) - int(r["capture_ts_ns"]) for r in steady_first_recs]
        p50_ns = percentile(lats_ns, 0.50)
        p95_ns = percentile(lats_ns, 0.95)
        max_ns = max(lats_ns)
        min_ns = min(lats_ns)
        spread_ns = max_ns - min_ns
        spread_periods = spread_ns / disp_period_ns

        # Compute worst natural spread across active segments
        nat_sp = natural_spread(ratio)
        if trans_ratio is not None:
            nat_sp = max(nat_sp, natural_spread(trans_ratio))
        if cell.get("cadence_class") == "fractional_drift":
            nat_sp = 1.0
        allowed_spread_periods = nat_sp + spread_slack

        # Drift check: compare median of first half vs second half within segment 0 (or largest steady segment)
        seg0 = seg_records.get(0, steady_first_recs)
        seg0_lats = [int(r["present_ts_ns"]) - int(r["capture_ts_ns"]) for r in seg0]
        half = max(1, len(seg0_lats) // 2)
        drift_ns = abs(percentile(seg0_lats[half:], 0.50) - percentile(seg0_lats[:half], 0.50))
        drift_periods = drift_ns / disp_period_ns
        allowed_drift_periods = (0.5 + drift_limit) if cell.get("cadence_class") == "fractional_drift" else drift_limit

        metrics["latency_p50_ms"] = round(p50_ns / 1e6, 3)
        metrics["latency_p95_ms"] = round(p95_ns / 1e6, 3)
        metrics["latency_max_ms"] = round(max_ns / 1e6, 3)
        metrics["latency_spread_periods"] = round(spread_periods, 4)
        metrics["latency_drift_periods"] = round(drift_periods, 4)

        delay_fail = []
        if min_ns < 0:
            delay_fail.append("negative end-to-end latency %.3f ms" % (min_ns / 1e6))
        if spread_periods > allowed_spread_periods + 1e-6:
            delay_fail.append("spread %.3f periods > limit %.3f periods (1 display period = %.3f ms)" % (
                spread_periods, allowed_spread_periods, disp_period_ns / 1e6
            ))
        if drift_periods > allowed_drift_periods + 1e-6:
            delay_fail.append("drift %.3f periods > limit %.3f periods" % (drift_periods, allowed_drift_periods))

        checks["constant_delay"] = {
            "status": FAIL if delay_fail else PASS,
            "detail": "; ".join(delay_fail) if delay_fail else (
                "p50=%.2f ms p95=%.2f ms max=%.2f ms, spread=%.3f<=%.3f P_d, drift=%.3f<=%.3f P_d" % (
                    p50_ns / 1e6, p95_ns / 1e6, max_ns / 1e6,
                    spread_periods, allowed_spread_periods, drift_periods, allowed_drift_periods,
                )
            ),
        }

    # --- 4. Render-ahead proof invariants (Row 27) ---
    steady_all = [r for r in records if not bool(r.get("is_dirty_event", False))]
    depths = [int(r["queue_depth"]) for r in steady_all]
    extras_cnt = sum(1 for r in steady_all if bool(r.get("is_extra", False)))
    metrics["observed_queue_depth_p50"] = round(percentile(depths, 0.50), 2) if depths else None
    metrics["extras_presented"] = extras_cnt

    ra_issues = []
    if cell["pathway"] == "original" or not cell["render_ahead"]:
        bad_depth = [r for r in steady_all if int(r["queue_depth"]) != 0]
        if bad_depth:
            ra_issues.append("render-ahead active (%d frames with queue_depth>0) when disabled/original" % len(bad_depth))
        if not cell.get("dejudder") and extras_cnt > 0:
            ra_issues.append("unexpected %d de-judder extras when dejudder=off" % extras_cnt)
    else:
        want_depth = int(cell["resolved_queue_depth"])
        # Silent fallback check: queue_depth == 0 when render_ahead was requested!
        zero_depth = [r for r in steady_all if int(r["queue_depth"]) == 0]
        silent_zero = [r for r in zero_depth if not str(r.get("fallback_reason", "")).strip()]
        metrics["silent_fallback_frames"] = len(silent_zero)
        if silent_zero:
            ra_issues.append("SILENT fallback to queue_depth=0 on %d frames (requested L=%s / resolved=%d)" % (
                len(silent_zero), cell["requested_queue_depth"], want_depth
            ))
        elif zero_depth:
            ra_issues.append("render-ahead fell back to queue_depth=0 on %d frames (reason=%r)" % (
                len(zero_depth), zero_depth[0].get("fallback_reason")
            ))

        # Check queue depth matches requested target on steady frames
        if depths and abs(percentile(depths, 0.50) - want_depth) > 0.5:
            ra_issues.append("median queue_depth=%.2f != expected=%d" % (percentile(depths, 0.50), want_depth))

        # Landing-check accuracy (exclude transition convergence frames)
        landing_checked = 0
        late_count = 0
        for idx, r in enumerate(steady_all):
            if r.get("planned_vblank_idx") is None:
                continue
            if seg1_start_rec is not None and int(r.get("segment_idx", 0)) == 1:
                # Skip convergence frames right after rate switch
                if idx < seg1_start_rec + trans_conv_periods * 2:
                    continue
            landing_checked += 1
            err = abs(int(r["display_vblank_idx"]) - int(r["planned_vblank_idx"]))
            if err > landing_tol:
                late_count += 1
        metrics["late_landings"] = late_count
        if landing_checked == 0:
            ra_issues.append("no planned_vblank_idx telemetry for render-ahead landing check")
        else:
            late_frac = late_count / float(landing_checked)
            if late_frac > max_late_frac + 1e-9:
                ra_issues.append("%d/%d late landings (%.2f%% > limit %.2f%%, tol=%d vblanks)" % (
                    late_count, landing_checked, 100.0 * late_frac, 100.0 * max_late_frac, landing_tol
                ))

        # De-judder extras verification when ratio > 1 and dejudder enabled
        if cell.get("dejudder") and float(ratio) > 1.0 + rg.RA_CADENCE_TOLERANCE and cell.get("max_extras", 0) > 0:
            if extras_cnt == 0:
                gate_hold = any(str(r.get("gate_level", "")).lower() == "hold" and str(r.get("fallback_reason", "")).strip()
                                for r in steady_all)
                if not gate_hold:
                    ra_issues.append("dejudder=on with ratio=%.3f produced 0 extras without explicit gate hold" % float(ratio))

    checks["render_ahead_proof"] = {
        "status": FAIL if ra_issues else PASS,
        "detail": "; ".join(ra_issues) if ra_issues else (
            "render_ahead=%s depth_p50=%s extras=%d late_landings=%d" % (
                "on" if cell["render_ahead"] else "off",
                metrics["observed_queue_depth_p50"],
                extras_cnt,
                metrics["late_landings"],
            )
        ),
    }

    # --- 5. Cadence / present-interval stability ---
    total_cad_errs = 0
    all_outside = []
    seg_details = []
    for seg_id in sorted({int(r.get("segment_idx", 0)) for r in records}):
        seg_first = []
        seen_k = set()
        for r in records:
            if int(r.get("segment_idx", 0)) != seg_id or bool(r.get("is_dirty_event", False)):
                continue
            k = int(r["frame_seq"])
            if k not in seen_k:
                seen_k.add(k)
                seg_first.append((k, int(r["display_vblank_idx"])))
        if seg_id == 1 and len(seg_first) > trans_conv_periods:
            seg_first = seg_first[trans_conv_periods:]
        if len(seg_first) < 2:
            continue
        r_seg = float(trans_ratio) if (seg_id == 1 and trans_ratio is not None) else float(ratio)
        # Split into contiguous sub-runs (broken by dirty events) so a dirty gap doesn't shift lattice offset
        sub_run = [seg_first[0]]
        for pt in seg_first[1:]:
            if pt[0] == sub_run[-1][0] + 1:
                sub_run.append(pt)
            else:
                if len(sub_run) >= 2:
                    ne, out = lattice_errors(sub_run, r_seg, phase_tol)
                    total_cad_errs += ne
                    all_outside.extend(out)
                sub_run = [pt]
        if len(sub_run) >= 2:
            ne, out = lattice_errors(sub_run, r_seg, phase_tol)
            total_cad_errs += ne
            all_outside.extend(out)
        seg_details.append("seg%d(r=%.4f,pts=%d)" % (seg_id, r_seg, len(seg_first)))

    p99_cad = percentile(all_outside, 0.99) if all_outside else 0.0
    metrics["cadence_errors"] = total_cad_errs
    metrics["cadence_error_p99_periods"] = round(p99_cad, 4)
    checks["cadence_stability"] = {
        "status": FAIL if total_cad_errs > 0 else PASS,
        "detail": "%d cadence errors, p99=%.3f P_d (tol=%.2f P_d) across %s" % (
            total_cad_errs, p99_cad, phase_tol, ", ".join(seg_details) or "no segments"
        ),
    }

    # --- 6. Original == Bill baseline comparison ---
    if cell["pathway"] == "original":
        orig_issues = []
        if any(str(r.get("gate_level", "off")).lower() != "off" for r in records):
            orig_issues.append("non-off VP-V gate_level observed in original pathway")
        if bill_baseline is not None:
            exp_hashes = bill_baseline.get("pixel_hashes", {})
            exp_vblanks = bill_baseline.get("vblanks", {})
            mismatches = 0
            for seq, r in seq_first_rec.items():
                if seq in exp_hashes and r.get("pixel_hash") != exp_hashes[seq]:
                    mismatches += 1
                if seq in exp_vblanks and int(r["display_vblank_idx"]) != int(exp_vblanks[seq]):
                    mismatches += 1
            if not exp_hashes and not exp_vblanks:
                orig_issues.append("empty bill_baseline provided")
            elif mismatches > 0:
                orig_issues.append("%d frame(s) diverged from Bill baseline" % mismatches)
        checks["original_baseline"] = {
            "status": FAIL if orig_issues else PASS,
            "detail": "; ".join(orig_issues) if orig_issues else (
                "Original pathway matches Bill baseline byte-identically" if bill_baseline is not None
                else "Original pathway invariants hold (no VP-V mutations)"
            ),
        }
    else:
        checks["original_baseline"] = {
            "status": INFO,
            "detail": "vpv pathway (level %d)" % cell["level"],
        }

    # --- 7. Clean / dirty / rate-transition continuity ---
    track = cell.get("track", "clean")
    if track == "clean":
        dirty_recs = [r for r in records if bool(r.get("is_dirty_event", False))]
        checks["track_continuity"] = {
            "status": FAIL if dirty_recs else PASS,
            "detail": ("%d unexpected dirty-event markers on clean track" % len(dirty_recs)) if dirty_recs else "clean steady track",
        }
    elif track == "dirty":
        dirty_indices = [i for i, r in enumerate(records) if bool(r.get("is_dirty_event", False))]
        if not dirty_indices:
            checks["track_continuity"] = {"status": FAIL, "detail": "dirty track had 0 injected dirty events"}
        else:
            last_dirty_seq = int(records[dirty_indices[-1]]["frame_seq"])
            # Check that post-recovery tail (after last_dirty_seq + dirty_recov_periods) is spotless
            tail_first = [
                (k, int(r["display_vblank_idx"]))
                for k, r in sorted(seq_first_rec.items())
                if k > last_dirty_seq + dirty_recov_periods
            ]
            if len(tail_first) < 4:
                checks["track_continuity"] = {
                    "status": FAIL,
                    "detail": "insufficient post-dirty recovery tail (%d frames)" % len(tail_first),
                }
            else:
                ne, _ = lattice_errors(tail_first, float(ratio), phase_tol)
                tail_late = sum(
                    1 for r in records
                    if int(r["frame_seq"]) > last_dirty_seq + dirty_recov_periods
                    and r.get("planned_vblank_idx") is not None
                    and abs(int(r["display_vblank_idx"]) - int(r["planned_vblank_idx"])) > landing_tol
                )
                ok = (ne == 0 and tail_late == 0)
                checks["track_continuity"] = {
                    "status": PASS if ok else FAIL,
                    "detail": "dirty events=%d, post-recovery (+%d P_s) tail=%d frames: cadence_err=%d late=%d" % (
                        len(dirty_indices), dirty_recov_periods, len(tail_first), ne, tail_late
                    ),
                }
    elif track == "rate_transition":
        seg0_pts = []
        seg1_pts = []
        seen0, seen1 = set(), set()
        for r in records:
            k = int(r["frame_seq"])
            vb = int(r["display_vblank_idx"])
            seg = int(r.get("segment_idx", 0))
            if seg == 0 and k not in seen0:
                seen0.add(k)
                seg0_pts.append((k, vb))
            elif seg == 1 and k not in seen1:
                seen1.add(k)
                seg1_pts.append((k, vb))
        if not seg0_pts or not seg1_pts or trans_ratio is None:
            checks["track_continuity"] = {
                "status": FAIL,
                "detail": "rate_transition missing pre/post segment (seg0=%d, seg1=%d)" % (len(seg0_pts), len(seg1_pts)),
            }
        else:
            # Find earliest offset m in seg1_pts where the remaining tail locks cleanly to trans_ratio
            r_new = float(trans_ratio)
            converge_idx = None
            min_tail = max(4, min(8, len(seg1_pts) // 2))
            for m in range(0, max(1, len(seg1_pts) - min_tail + 1)):
                tail = seg1_pts[m:]
                ne, _ = lattice_errors(tail, r_new, phase_tol)
                if ne == 0:
                    converge_idx = m
                    break
            if converge_idx is None:
                metrics["transition_converge_periods"] = len(seg1_pts)
                checks["track_continuity"] = {
                    "status": FAIL,
                    "detail": "post-transition stream (%d frames at r=%.4f) never converged to cadence lattice" % (
                        len(seg1_pts), r_new
                    ),
                }
            else:
                metrics["transition_converge_periods"] = converge_idx
                ok = converge_idx <= trans_conv_periods
                checks["track_continuity"] = {
                    "status": PASS if ok else FAIL,
                    "detail": "rate transition %s->%s converged in %d source periods (limit %d P_s)" % (
                        cell["source_hz_label"], cell.get("transition_to_label"),
                        converge_idx, trans_conv_periods,
                    ),
                }
    else:
        checks["track_continuity"] = {"status": FAIL, "detail": "unknown track %r" % (track,)}

    overall = FAIL if any(c["status"] == FAIL for c in checks.values()) else PASS
    return {"status": overall, "checks": checks, "metrics": metrics}
