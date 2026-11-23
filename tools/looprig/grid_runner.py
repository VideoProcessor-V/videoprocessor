# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Row-27 Loop Rig Runner & Grid Orchestrator.

Modes:
  --mode hardware   (default) Probes real loop-rig prerequisites (VideoProcessor.exe,
                    StreamRecorder, capture device, display loop, footage). Missing
                    hardware logs explicit SKIP-NO-HARDWARE on EVERY cell (missing
                    clip logs SKIP-NO-FOOTAGE) — NEVER reported as PASS!
  --mode synthetic  (--synthetic) Drives the full resolved grid through a deterministic
                    synthetic loop simulator and end-to-end stamp verifier, with optional
                    fault-injected negative controls (--include-negative-controls).
  --mode plan       (--dry-run) Validates and emits the resolved grid plan (JSON + MD)
                    without executing hardware.

Exit codes (explicitly distinguishing PASS vs FAIL vs ALL/PARTIAL SKIP):
  0  All cells PASS (and all negative controls caught) — or --mode plan succeeded.
  1  One or more cells FAIL (or a negative control was not caught).
  2  CLI / schema / configuration usage error.
  3  SKIP-NO-HARDWARE or SKIP-NO-FOOTAGE (0 FAIL, >= 1 skipped cell; hardware_verified=false).
     CI and callers can NEVER mistake a skipped hardware run for a pass.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import uuid
from fractions import Fraction
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import row27_grid as rg  # noqa: E402
import stamp_verify as sv  # noqa: E402

SCHEMA_REPORT = "vp-row27-grid-report/1"

STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_SKIP_HW = "SKIP-NO-HARDWARE"
STATUS_SKIP_FOOTAGE = "SKIP-NO-FOOTAGE"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_SKIP = 3

DEFAULT_DEVICE_MATCH = r"DeckLink|Mini Recorder 4K|Quad HDMI"
DEFAULT_BUILD_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "x64", "Release"))

NEGATIVE_CONTROL_FAULTS = (
    "drop",
    "duplicate",
    "out_of_order",
    "corrupt_crc",
    "delay_drift",
    "ra_silent_fallback",
    "ra_late_landing",
    "transition_stall",
    "original_mismatch",
)


# ---- Hardware prerequisite probe ---------------------------------------------------------------

def probe_hardware(
    build_dir=DEFAULT_BUILD_DIR,
    recorder_path=None,
    device_match=DEFAULT_DEVICE_MATCH,
    clips_dir=None,
    display_loop_ready=None,
    replay_dir=None,
):
    """Inspect real loop-rig prerequisites and report exact present/missing components.

    Never guesses or silently passes: every missing prerequisite is named explicitly.
    """
    vp_exe = os.path.join(build_dir, "VideoProcessor.exe") if build_dir else ""
    rec_exe = recorder_path or (os.path.join(build_dir, "VideoProcessorStreamRecorder.exe") if build_dir else "")

    checks = {}
    missing = []

    vp_ok = bool(vp_exe and os.path.isfile(vp_exe))
    checks["vp_binary"] = {
        "ok": vp_ok,
        "path": vp_exe,
        "detail": "found" if vp_ok else ("missing VideoProcessor.exe at %s" % (vp_exe or "(unset)")),
    }
    if not vp_ok:
        missing.append("vp_binary (%s)" % (vp_exe or "unset"))

    rec_ok = bool(rec_exe and os.path.isfile(rec_exe))
    checks["recorder_binary"] = {
        "ok": rec_ok,
        "path": rec_exe,
        "detail": "found" if rec_ok else ("missing StreamRecorder at %s" % (rec_exe or "(unset)")),
    }
    if not rec_ok:
        missing.append("recorder_binary (%s)" % (rec_exe or "unset"))

    # Probe capture device via StreamRecorder --list
    dev_ok = False
    dev_name = None
    dev_index = None
    dev_detail = "StreamRecorder unavailable"
    if rec_ok:
        try:
            proc = subprocess.run([rec_exe, "--list"], capture_output=True, text=True, timeout=20)
            rx = re.compile(device_match, re.I)
            for line in (proc.stdout or "").splitlines():
                m = re.match(r"\s*(\d+):\s*(.+)$", line)
                if m and rx.search(m.group(2)):
                    dev_ok = True
                    dev_index = int(m.group(1))
                    dev_name = m.group(2).strip()
                    dev_detail = "device %d: %s" % (dev_index, dev_name)
                    break
            if not dev_ok:
                out_preview = ((proc.stdout or "").strip() or (proc.stderr or "").strip() or "no output")[:160]
                dev_detail = "no capture device matching /%s/ (recorder output: %s)" % (device_match, out_preview)
        except (OSError, subprocess.SubprocessError) as e:
            dev_detail = "recorder probe error: %s" % e
    checks["capture_device"] = {
        "ok": dev_ok,
        "device_index": dev_index,
        "device_name": dev_name,
        "match": device_match,
        "detail": dev_detail,
    }
    if not dev_ok:
        missing.append("capture_device (%s)" % dev_detail)

    # Display loop verification: requires explicit environment flag or caller assertion
    # (or an existing recorded replay directory for post-capture verification).
    if display_loop_ready is None:
        env_flag = os.environ.get("VP_LOOPRIG_DISPLAY_READY", "").strip().lower()
        loop_ok = env_flag in ("1", "true", "yes")
    else:
        loop_ok = bool(display_loop_ready)
    checks["display_loop"] = {
        "ok": loop_ok,
        "detail": "display loop confirmed" if loop_ok else "display loop cable/EDID not confirmed (set VP_LOOPRIG_DISPLAY_READY=1 on the wired rig)",
    }
    if not loop_ok:
        missing.append("display_loop (not confirmed)")

    # Clips directory check (informational globally; enforced per-cell when cell requires a clip)
    clips_ok = bool(clips_dir and os.path.isdir(clips_dir))
    checks["clips_dir"] = {
        "ok": clips_ok,
        "path": clips_dir or "",
        "detail": "found" if clips_ok else ("clips directory not configured or missing: %s" % (clips_dir or "(unset)")),
    }

    replay_ok = bool(replay_dir and os.path.isdir(replay_dir))
    checks["replay_dir"] = {
        "ok": replay_ok,
        "path": replay_dir or "",
        "detail": "replay directory present" if replay_ok else "none",
    }

    hardware_ready = vp_ok and rec_ok and dev_ok and loop_ok
    return {
        "hardware_ready": hardware_ready,
        "replay_ready": replay_ok,
        "missing": missing,
        "checks": checks,
    }


# ---- Deterministic synthetic loop simulator ----------------------------------------------------

def _bill_pixel_hash(cell, frame_seq):
    raw = "bill-ref:%s:%s:%d" % (cell["color_mode"], cell["source_hz_label"], int(frame_seq))
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:16]


def build_bill_baseline(cell, records):
    """Construct the expected Bill Original-pathway reference for a cell."""
    hashes = {}
    vblanks = {}
    for r in records:
        k = int(r["frame_seq"])
        if k not in hashes:
            hashes[k] = _bill_pixel_hash(cell, k)
            vblanks[k] = int(r["display_vblank_idx"])
    return {"pixel_hashes": hashes, "vblanks": vblanks}


def simulate_cell(cell, fault=None, n_source_frames=64):
    """Deterministically simulate end-to-end loop-rig stamp records for any arbitrary grid cell.

    Produces clean records when fault is None, or injects a named defect from
    NEGATIVE_CONTROL_FAULTS to prove the verification engine catches every failure class.
    """
    src_rate = rg.parse_rate(cell["source_rate"])
    disp_rate = rg.parse_rate(cell["display_rate"])
    ratio = disp_rate / src_rate
    disp_period_ns = Fraction(10**9) / disp_rate
    src_period_ns = Fraction(10**9) / src_rate

    track = cell.get("track", "clean")
    trans_rate = rg.parse_rate(cell["transition_to_rate"]) if cell.get("transition_to_rate") else None
    trans_ratio = (disp_rate / trans_rate) if trans_rate else None
    trans_src_period_ns = (Fraction(10**9) / trans_rate) if trans_rate else None

    min_f = max(int(cell.get("tolerances", {}).get("min_frames", 48)), int(n_source_frames))
    if track == "rate_transition":
        n0 = max(24, min_f // 2)
        n1 = max(28, min_f - n0)
    else:
        n0 = min_f
        n1 = 0

    t0_ns = 1_000_000_000  # 1s base epoch
    ra = bool(cell["render_ahead"])
    qdepth = int(cell["resolved_queue_depth"])
    pathway = cell["pathway"]
    dejudder = bool(cell["dejudder"])
    gate_level = ("L%d" % cell["level"]) if ra else "off"
    base_pipe_vblanks = (3 + qdepth) if ra else 3

    dirty_k = (n0 // 3) if track == "dirty" else -1
    records = []

    def emit_segment(k_start, count, seg_idx, seg_src_period_ns, seg_ratio,
                     seg_pts_offset_ns, seg_vb_offset, force_stall=False):
        used_ratio = ratio if force_stall else seg_ratio
        last_vb = seg_vb_offset - 1
        for local_k in range(count):
            k = k_start + local_k
            is_dirty = (track == "dirty" and seg_idx == 0 and local_k == dirty_k)

            # On a dirty track, simulate a brief 1-frame capture hiccup cleanly tagged as dirty
            if is_dirty:
                continue

            pts_ns = int(round(float(seg_pts_offset_ns + local_k * seg_src_period_ns)))
            cap_ns = t0_ns + pts_ns

            vb0 = seg_vb_offset + int(local_k * used_ratio)
            vb1 = seg_vb_offset + int((local_k + 1) * used_ratio)
            if vb1 <= vb0:
                vb1 = vb0 + 1
            if vb0 <= last_vb:
                shift = (last_vb + 1) - vb0
                vb0 += shift
                vb1 += shift

            for vb in range(vb0, vb1):
                is_extra = (vb > vb0) and dejudder
                pres_ns = int(round(float(t0_ns + (base_pipe_vblanks + vb) * disp_period_ns)))
                lead_ns = int(round(float(max(0.25, float(qdepth) if ra else 0.5) * disp_period_ns)))
                sub_ns = max(cap_ns, min(pres_ns, pres_ns - lead_ns))
                ph = _bill_pixel_hash(cell, k) if pathway == "original" else ""
                rec = sv.make_record(
                    frame_seq=k,
                    source_pts_ns=pts_ns,
                    capture_ts_ns=cap_ns,
                    render_submit_ts_ns=sub_ns,
                    present_ts_ns=pres_ns,
                    display_vblank_idx=vb,
                    queue_depth=qdepth,
                    pathway=pathway,
                    mode_id=cell["cell_id"],
                    visual_aux=local_k % 5,
                    planned_vblank_idx=vb if ra else None,
                    is_extra=is_extra,
                    gate_level=gate_level,
                    fallback_reason="",
                    segment_idx=seg_idx,
                    is_dirty_event=(track == "dirty" and seg_idx == 0 and abs(local_k - dirty_k) <= 1),
                    pixel_hash=ph,
                )
                records.append(rec)
                last_vb = vb
        return last_vb + 1

    next_vb = emit_segment(0, n0, 0, src_period_ns, ratio, Fraction(0), 0)
    if track == "rate_transition" and trans_rate is not None:
        pts_off = n0 * src_period_ns
        emit_segment(
            n0, n1, 1, trans_src_period_ns, trans_ratio, pts_off, next_vb,
            force_stall=(fault == "transition_stall"),
        )

    baseline = build_bill_baseline(cell, records) if pathway == "original" else None

    # ---- Fault injection for negative controls -------------------------------------------------
    if fault is None or fault == "transition_stall":
        return records, baseline

    mid_seq = n0 // 2

    if fault == "drop":
        records = [r for r in records if int(r["frame_seq"]) != mid_seq]
    elif fault == "duplicate":
        # Add extra presents for mid_seq exceeding ceil(ratio) and shift subsequent vblanks/presents
        r_f = float(ratio)
        extra_needed = max(1, int(math.ceil(r_f - 1e-9)) + 1)
        out = []
        shift_vb = 0
        inserted = False
        for r in records:
            if int(r["frame_seq"]) == mid_seq and not inserted:
                # Find last record of mid_seq later; instead copy first record + extra_needed times
                out.append(r)
                last_vb_local = int(r["display_vblank_idx"])
                for e in range(1, extra_needed + 1):
                    new_vb = last_vb_local + e
                    new_pres = int(round(float(int(r["present_ts_ns"]) + e * disp_period_ns)))
                    dup = sv.make_record(
                        frame_seq=r["frame_seq"],
                        source_pts_ns=r["source_pts_ns"],
                        capture_ts_ns=r["capture_ts_ns"],
                        render_submit_ts_ns=r["render_submit_ts_ns"],
                        present_ts_ns=new_pres,
                        display_vblank_idx=new_vb,
                        queue_depth=r["queue_depth"],
                        pathway=r["pathway"],
                        mode_id=r["mode_id"],
                        planned_vblank_idx=(new_vb if r["planned_vblank_idx"] is not None else None),
                        is_extra=False,
                        gate_level=r["gate_level"],
                        segment_idx=r["segment_idx"],
                        is_dirty_event=r["is_dirty_event"],
                        pixel_hash=r["pixel_hash"],
                    )
                    out.append(dup)
                shift_vb = extra_needed
                inserted = True
            else:
                if shift_vb > 0:
                    new_vb = int(r["display_vblank_idx"]) + shift_vb
                    new_pres = int(round(float(int(r["present_ts_ns"]) + shift_vb * disp_period_ns)))
                    r2 = sv.make_record(
                        frame_seq=r["frame_seq"],
                        source_pts_ns=r["source_pts_ns"],
                        capture_ts_ns=r["capture_ts_ns"],
                        render_submit_ts_ns=r["render_submit_ts_ns"],
                        present_ts_ns=new_pres,
                        display_vblank_idx=new_vb,
                        queue_depth=r["queue_depth"],
                        pathway=r["pathway"],
                        mode_id=r["mode_id"],
                        planned_vblank_idx=(new_vb if r["planned_vblank_idx"] is not None else None),
                        is_extra=r["is_extra"],
                        gate_level=r["gate_level"],
                        segment_idx=r["segment_idx"],
                        is_dirty_event=r["is_dirty_event"],
                        pixel_hash=r["pixel_hash"],
                    )
                    out.append(r2)
                else:
                    out.append(r)
        records = out
    elif fault == "out_of_order":
        # Swap frame_seq (and matching visual stamp/CRC) of two adjacent source frames
        idx_a = next((i for i, r in enumerate(records) if int(r["frame_seq"]) == mid_seq), None)
        idx_b = next((i for i, r in enumerate(records) if int(r["frame_seq"]) == mid_seq + 1), None)
        if idx_a is not None and idx_b is not None:
            ra_rec, rb_rec = records[idx_a], records[idx_b]
            records[idx_a] = sv.make_record(
                frame_seq=rb_rec["frame_seq"], source_pts_ns=rb_rec["source_pts_ns"],
                capture_ts_ns=ra_rec["capture_ts_ns"], render_submit_ts_ns=ra_rec["render_submit_ts_ns"],
                present_ts_ns=ra_rec["present_ts_ns"], display_vblank_idx=ra_rec["display_vblank_idx"],
                queue_depth=ra_rec["queue_depth"], pathway=ra_rec["pathway"], mode_id=ra_rec["mode_id"],
                planned_vblank_idx=ra_rec["planned_vblank_idx"], is_extra=ra_rec["is_extra"],
                gate_level=ra_rec["gate_level"], segment_idx=ra_rec["segment_idx"],
            )
            records[idx_b] = sv.make_record(
                frame_seq=ra_rec["frame_seq"], source_pts_ns=ra_rec["source_pts_ns"],
                capture_ts_ns=rb_rec["capture_ts_ns"], render_submit_ts_ns=rb_rec["render_submit_ts_ns"],
                present_ts_ns=rb_rec["present_ts_ns"], display_vblank_idx=rb_rec["display_vblank_idx"],
                queue_depth=rb_rec["queue_depth"], pathway=rb_rec["pathway"], mode_id=rb_rec["mode_id"],
                planned_vblank_idx=rb_rec["planned_vblank_idx"], is_extra=rb_rec["is_extra"],
                gate_level=rb_rec["gate_level"], segment_idx=rb_rec["segment_idx"],
            )
    elif fault == "corrupt_crc":
        target = records[len(records) // 2]
        bad_crc = "%04X" % (int(target["record_crc"], 16) ^ 0x00FF)
        target["record_crc"] = bad_crc
    elif fault == "delay_drift":
        # Linearly drift present_ts_ns by +3 display periods across the second half
        out = []
        half = len(records) // 2
        span = max(1, len(records) - half)
        for i, r in enumerate(records):
            extra_ns = int(round(float(3.0 * disp_period_ns * (i - half) / span))) if i >= half else 0
            out.append(sv.make_record(
                frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
                capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
                present_ts_ns=int(r["present_ts_ns"]) + extra_ns,
                display_vblank_idx=r["display_vblank_idx"],
                queue_depth=r["queue_depth"], pathway=r["pathway"], mode_id=r["mode_id"],
                planned_vblank_idx=r["planned_vblank_idx"], is_extra=r["is_extra"],
                gate_level=r["gate_level"], segment_idx=r["segment_idx"],
                is_dirty_event=r["is_dirty_event"], pixel_hash=r["pixel_hash"],
            ))
        records = out
    elif fault == "ra_silent_fallback":
        # Silently collapse queue_depth to 0 with empty fallback_reason on second half
        out = []
        half = len(records) // 2
        for i, r in enumerate(records):
            qd = 0 if i >= half else r["queue_depth"]
            out.append(sv.make_record(
                frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
                capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
                present_ts_ns=r["present_ts_ns"], display_vblank_idx=r["display_vblank_idx"],
                queue_depth=qd, pathway=r["pathway"], mode_id=r["mode_id"],
                planned_vblank_idx=r["planned_vblank_idx"], is_extra=r["is_extra"],
                gate_level=r["gate_level"], fallback_reason="",
                segment_idx=r["segment_idx"], is_dirty_event=r["is_dirty_event"],
                pixel_hash=r["pixel_hash"],
            ))
        records = out
    elif fault == "ra_late_landing":
        # Shift planned_vblank_idx behind display_vblank_idx by 2 vblanks on steady frames
        out = []
        mid = len(records) // 2
        for i, r in enumerate(records):
            pvb = r["planned_vblank_idx"]
            if pvb is not None and mid <= i < mid + 5:
                pvb = int(r["display_vblank_idx"]) - 2
            out.append(sv.make_record(
                frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
                capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
                present_ts_ns=r["present_ts_ns"], display_vblank_idx=r["display_vblank_idx"],
                queue_depth=r["queue_depth"], pathway=r["pathway"], mode_id=r["mode_id"],
                planned_vblank_idx=pvb, is_extra=r["is_extra"],
                gate_level=r["gate_level"], fallback_reason=r["fallback_reason"],
                segment_idx=r["segment_idx"], is_dirty_event=r["is_dirty_event"],
                pixel_hash=r["pixel_hash"],
            ))
        records = out
    elif fault == "original_mismatch":
        # Perturb one frame's pixel_hash relative to Bill baseline
        idx = len(records) // 2
        r = records[idx]
        records[idx] = sv.make_record(
            frame_seq=r["frame_seq"], source_pts_ns=r["source_pts_ns"],
            capture_ts_ns=r["capture_ts_ns"], render_submit_ts_ns=r["render_submit_ts_ns"],
            present_ts_ns=r["present_ts_ns"], display_vblank_idx=r["display_vblank_idx"],
            queue_depth=r["queue_depth"], pathway=r["pathway"], mode_id=r["mode_id"],
            planned_vblank_idx=r["planned_vblank_idx"], is_extra=r["is_extra"],
            gate_level=r["gate_level"], segment_idx=r["segment_idx"],
            is_dirty_event=r["is_dirty_event"], pixel_hash="DEADBEEF00000001",
        )
    else:
        raise ValueError("unknown fault %r" % (fault,))

    return records, baseline


def run_negative_controls(n_source_frames=64):
    """Execute the full suite of fault-injected negative controls.

    Each control injects one specific defect and succeeds ONLY when stamp_verify catches
    that exact check as FAIL.
    """
    defaults = rg.load_grid_doc()["defaults"]
    ra_cell = rg.build_cell({
        "src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 2,
        "render_ahead": True, "queue_depth": 3, "dejudder": True,
        "color_mode": "sdr", "track": "clean",
    }, defaults)
    orig_cell = rg.build_cell({
        "src": "23.976", "disp": "59.94", "pathway": "original", "level": 0,
        "render_ahead": False, "queue_depth": 0, "dejudder": False,
        "color_mode": "sdr", "track": "clean",
    }, defaults)
    trans_cell = rg.build_cell({
        "src": "23.976", "disp": "59.94", "pathway": "vpv", "level": 2,
        "render_ahead": True, "queue_depth": "auto", "dejudder": True,
        "color_mode": "sdr", "track": "rate_transition", "transition_to": "59.94",
    }, defaults)

    cases = [
        ("drop",               ra_cell,    "monotonic_progression"),
        ("duplicate",          ra_cell,    "monotonic_progression"),
        ("out_of_order",       ra_cell,    "monotonic_progression"),
        ("corrupt_crc",        ra_cell,    "stamp_integrity"),
        ("delay_drift",        ra_cell,    "constant_delay"),
        ("ra_silent_fallback", ra_cell,    "render_ahead_proof"),
        ("ra_late_landing",    ra_cell,    "render_ahead_proof"),
        ("transition_stall",   trans_cell, "track_continuity"),
        ("original_mismatch",  orig_cell,  "original_baseline"),
    ]
    results = []
    for fault, cell, expected_check in cases:
        recs, base = simulate_cell(cell, fault=fault, n_source_frames=n_source_frames)
        ver = sv.verify_cell(cell, recs, bill_baseline=base)
        check_failed = (ver["checks"].get(expected_check, {}).get("status") == sv.FAIL)
        caught = (ver["status"] == sv.FAIL) and check_failed
        results.append({
            "control_id": "negctrl_%s" % fault,
            "fault": fault,
            "expected_check": expected_check,
            "verifier_status": ver["status"],
            "check_status": ver["checks"].get(expected_check, {}).get("status", "MISSING"),
            "detail": ver["checks"].get(expected_check, {}).get("detail", ""),
            "status": STATUS_PASS if caught else STATUS_FAIL,
        })
    return results


# ---- Orchestration -----------------------------------------------------------------------------

def _cell_result_entry(cell, status, detail="", skip_reason="", checks=None, metrics=None):
    return {
        "cell_id": cell["cell_id"],
        "index": cell["index"],
        "status": status,
        "skip_reason": skip_reason,
        "detail": detail,
        "config": {
            "source_rate": cell["source_rate"],
            "source_hz_label": cell["source_hz_label"],
            "display_rate": cell["display_rate"],
            "display_hz_label": cell["display_hz_label"],
            "cadence_ratio": cell["cadence_ratio"],
            "cadence_ratio_float": cell["cadence_ratio_float"],
            "cadence_class": cell["cadence_class"],
            "pathway": cell["pathway"],
            "level": cell["level"],
            "render_ahead": cell["render_ahead"],
            "requested_queue_depth": cell["requested_queue_depth"],
            "resolved_queue_depth": cell["resolved_queue_depth"],
            "dejudder": cell["dejudder"],
            "color_mode": cell["color_mode"],
            "track": cell["track"],
            "transition_to_label": cell["transition_to_label"],
        },
        "metrics": metrics or {},
        "checks": checks or {},
    }


def _load_replay_cell_records(replay_dir, cell):
    """Load recorded stamp telemetry from a replay capture folder if present."""
    cdir = os.path.join(replay_dir, cell["cell_id"])
    jsonl = os.path.join(cdir, "stamps.jsonl")
    if os.path.isfile(jsonl):
        return sv.read_jsonl(jsonl), None
    e2e_csv = os.path.join(cdir, "e2e_stamps.csv")
    if os.path.isfile(e2e_csv):
        return sv.read_csv(e2e_csv), None
    stamps_csv = os.path.join(cdir, "stamps.csv")
    sent_csv = os.path.join(cdir, "sent.csv")
    if os.path.isfile(stamps_csv) and os.path.isfile(sent_csv):
        return sv.from_looprig_artifacts(stamps_csv, sent_csv, cell), None
    return None, "missing capture files under %s" % cdir


def execute_grid(
    plan,
    mode="hardware",
    out_dir=None,
    build_dir=DEFAULT_BUILD_DIR,
    recorder_path=None,
    device_match=DEFAULT_DEVICE_MATCH,
    clips_dir=None,
    display_loop_ready=None,
    replay_dir=None,
    include_negative_controls=False,
    synthetic_frames=64,
    write_stamps=False,
    probe_override=None,
):
    """Run the resolved grid in hardware, synthetic, or plan mode and produce report dict."""
    now_utc = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")

    if mode == "plan":
        return {
            "schema": SCHEMA_REPORT,
            "mode": "plan",
            "profile": plan["profile"],
            "timestamp_utc": now_utc,
            "overall_status": "PLAN",
            "hardware_verified": False,
            "exit_code": EXIT_PASS,
            "hardware_probe": None,
            "counts": {
                "total": plan["total_cells"],
                "pass": 0,
                "fail": 0,
                "skip_no_hardware": 0,
                "skip_no_footage": 0,
                "negative_controls_passed": 0,
                "negative_controls_failed": 0,
            },
            "plan": plan,
            "cells": [],
            "negative_controls": [],
        }

    hw_probe = None
    cell_results = []
    neg_results = []

    if mode == "hardware":
        hw_probe = probe_override if probe_override is not None else probe_hardware(
            build_dir=build_dir,
            recorder_path=recorder_path,
            device_match=device_match,
            clips_dir=clips_dir,
            display_loop_ready=display_loop_ready,
            replay_dir=replay_dir,
        )
        missing_str = "; ".join(hw_probe.get("missing", [])) or "unspecified hardware prerequisite"

        for cell in plan["cells"]:
            # If a replay capture directory is explicitly supplied, score recorded captures
            if hw_probe.get("replay_ready") and replay_dir:
                recs, err = _load_replay_cell_records(replay_dir, cell)
                if recs is None:
                    reason = "%s %s: %s" % (STATUS_SKIP_FOOTAGE, cell["cell_id"], err)
                    cell_results.append(_cell_result_entry(
                        cell, STATUS_SKIP_FOOTAGE, detail=reason, skip_reason=err
                    ))
                    continue
                ver = sv.verify_cell(cell, recs)
                detail = "; ".join(
                    "%s=%s" % (k, v["status"]) for k, v in ver["checks"].items() if v["status"] == sv.FAIL
                ) or "all invariants passed"
                cell_results.append(_cell_result_entry(
                    cell,
                    STATUS_PASS if ver["status"] == sv.PASS else STATUS_FAIL,
                    detail=detail,
                    checks=ver["checks"],
                    metrics=ver["metrics"],
                ))
                continue

            # Strict hardware gating: missing hardware -> EVERY cell logs SKIP-NO-HARDWARE (never PASS!)
            if not hw_probe.get("hardware_ready"):
                reason = "%s %s: missing %s" % (STATUS_SKIP_HW, cell["cell_id"], missing_str)
                cell_results.append(_cell_result_entry(
                    cell, STATUS_SKIP_HW, detail=reason, skip_reason=missing_str
                ))
                continue

            # Hardware present: check required clip footage if specified on cell
            req_clip = cell.get("requires_clip")
            if req_clip:
                clip_path = req_clip if os.path.isabs(req_clip) else (
                    os.path.join(clips_dir, req_clip) if clips_dir else req_clip
                )
                if not os.path.isfile(clip_path):
                    reason = "%s %s: missing clip %s" % (STATUS_SKIP_FOOTAGE, cell["cell_id"], clip_path)
                    cell_results.append(_cell_result_entry(
                        cell, STATUS_SKIP_FOOTAGE, detail=reason, skip_reason="missing clip: %s" % clip_path
                    ))
                    continue

            # Hardware & footage present, but live capture driver requires recorded capture / armed rig
            reason = "%s %s: live capture requires wired rig capture output (--replay-dir)" % (
                STATUS_SKIP_HW, cell["cell_id"]
            )
            cell_results.append(_cell_result_entry(
                cell, STATUS_SKIP_HW, detail=reason, skip_reason="live capture not armed"
            ))

    elif mode == "synthetic":
        for cell in plan["cells"]:
            recs, baseline = simulate_cell(cell, fault=None, n_source_frames=synthetic_frames)
            if write_stamps and out_dir:
                sv.write_jsonl(os.path.join(out_dir, "stamps", "%s.jsonl" % cell["cell_id"]), recs)
            ver = sv.verify_cell(cell, recs, bill_baseline=baseline)
            failed_checks = [
                "%s: %s" % (k, v["detail"])
                for k, v in ver["checks"].items()
                if v["status"] == sv.FAIL
            ]
            detail = "; ".join(failed_checks) if failed_checks else (
                "p50=%.2fms spread=%.3fP_d cad_err=%d depth=%s" % (
                    ver["metrics"].get("latency_p50_ms") or 0.0,
                    ver["metrics"].get("latency_spread_periods") or 0.0,
                    ver["metrics"].get("cadence_errors", 0),
                    ver["metrics"].get("observed_queue_depth_p50"),
                )
            )
            cell_results.append(_cell_result_entry(
                cell,
                STATUS_PASS if ver["status"] == sv.PASS else STATUS_FAIL,
                detail=detail,
                checks=ver["checks"],
                metrics=ver["metrics"],
            ))
        if include_negative_controls:
            neg_results = run_negative_controls(n_source_frames=synthetic_frames)
    else:
        raise ValueError("unknown mode %r" % (mode,))

    # Compute comparative Original vs VP-V summary deltas for matching (src, disp) pairs
    orig_by_pair = {}
    for cr in cell_results:
        cfg = cr["config"]
        if cfg["pathway"] == "original" and cr["status"] == STATUS_PASS:
            key = (cfg["source_hz_label"], cfg["display_hz_label"])
            orig_by_pair[key] = cr["metrics"]
    for cr in cell_results:
        cfg = cr["config"]
        key = (cfg["source_hz_label"], cfg["display_hz_label"])
        if cfg["pathway"] == "vpv" and cr["status"] == STATUS_PASS and key in orig_by_pair:
            orig_m = orig_by_pair[key]
            if cr["metrics"].get("latency_p50_ms") is not None and orig_m.get("latency_p50_ms") is not None:
                dp_ms = cr["metrics"].get("display_period_ms") or 1.0
                delta_ms = cr["metrics"]["latency_p50_ms"] - orig_m["latency_p50_ms"]
                cr["metrics"]["vs_original_latency_delta_ms"] = round(delta_ms, 3)
                cr["metrics"]["vs_original_latency_delta_periods"] = round(delta_ms / dp_ms, 3)

    n_pass = sum(1 for c in cell_results if c["status"] == STATUS_PASS)
    n_fail = sum(1 for c in cell_results if c["status"] == STATUS_FAIL)
    n_skip_hw = sum(1 for c in cell_results if c["status"] == STATUS_SKIP_HW)
    n_skip_ft = sum(1 for c in cell_results if c["status"] == STATUS_SKIP_FOOTAGE)
    n_neg_pass = sum(1 for n in neg_results if n["status"] == STATUS_PASS)
    n_neg_fail = sum(1 for n in neg_results if n["status"] == STATUS_FAIL)

    if n_fail > 0 or n_neg_fail > 0:
        overall = STATUS_FAIL
        exit_code = EXIT_FAIL
    elif (n_skip_hw + n_skip_ft) > 0:
        if n_pass == 0:
            overall = STATUS_SKIP_HW if n_skip_hw >= n_skip_ft else STATUS_SKIP_FOOTAGE
        else:
            overall = "PARTIAL-SKIP"
        exit_code = EXIT_SKIP
    elif len(cell_results) == 0:
        overall = STATUS_FAIL
        exit_code = EXIT_FAIL
    else:
        overall = STATUS_PASS
        exit_code = EXIT_PASS

    hardware_verified = (mode == "hardware" and exit_code == EXIT_PASS and n_pass == len(cell_results) and n_pass > 0)

    return {
        "schema": SCHEMA_REPORT,
        "mode": mode,
        "profile": plan["profile"],
        "timestamp_utc": now_utc,
        "overall_status": overall,
        "hardware_verified": hardware_verified,
        "exit_code": exit_code,
        "hardware_probe": hw_probe,
        "counts": {
            "total": len(cell_results),
            "pass": n_pass,
            "fail": n_fail,
            "skip_no_hardware": n_skip_hw,
            "skip_no_footage": n_skip_ft,
            "negative_controls_passed": n_neg_pass,
            "negative_controls_failed": n_neg_fail,
        },
        "cells": cell_results,
        "negative_controls": neg_results,
    }


# ---- Report renderers (Markdown, TRX, JUnit) ---------------------------------------------------

def render_report_md(report):
    c = report["counts"]
    lines = [
        "# Row-27 Loop Rig Report (%s)" % report["overall_status"],
        "",
        "- Mode: `%s`" % report["mode"],
        "- Profile: `%s`" % report["profile"],
        "- Timestamp (UTC): `%s`" % report["timestamp_utc"],
        "- Overall status: **%s** (exit code `%d`)" % (report["overall_status"], report["exit_code"]),
        "- Hardware verified: `%s`" % ("true" if report["hardware_verified"] else "false"),
        "- Totals: total=%d, PASS=%d, FAIL=%d, SKIP-NO-HARDWARE=%d, SKIP-NO-FOOTAGE=%d" % (
            c["total"], c["pass"], c["fail"], c["skip_no_hardware"], c["skip_no_footage"]
        ),
        "",
    ]

    probe = report.get("hardware_probe")
    if probe is not None:
        lines += [
            "## Hardware Probe",
            "",
            "- Hardware ready: `%s`" % ("true" if probe.get("hardware_ready") else "false"),
            "- Missing prerequisites: %s" % (", ".join(probe.get("missing", [])) or "(none)"),
            "",
            "| check | ok | detail |",
            "|---|---|---|",
        ]
        for k, info in sorted(probe.get("checks", {}).items()):
            lines.append("| %s | %s | %s |" % (k, "yes" if info.get("ok") else "NO",
                                               str(info.get("detail", "")).replace("|", "/")))
        lines.append("")

    lines += [
        "## Grid Results",
        "",
        "| cell_id | src->disp | pathway/lvl | RA/depth | track | status | lat p50/p95/max (ms) | cad err | crc err | detail |",
        "|---|---|---|---|---|---|---|---:|---:|---|",
    ]
    for cr in report["cells"]:
        cfg = cr["config"]
        m = cr.get("metrics") or {}
        if m.get("latency_p50_ms") is not None:
            lat_s = "%.2f / %.2f / %.2f" % (m["latency_p50_ms"], m["latency_p95_ms"], m["latency_max_ms"])
        else:
            lat_s = "-"
        ra_s = ("on/L%s" % cfg["requested_queue_depth"]) if cfg["render_ahead"] else "off/0"
        tr_s = cfg["track"] if not cfg.get("transition_to_label") else ("%s->%s" % (cfg["track"], cfg["transition_to_label"]))
        lines.append(
            "| %s | %s->%s | %s/%d | %s | %s | **%s** | %s | %s | %s | %s |" % (
                cr["cell_id"],
                cfg["source_hz_label"], cfg["display_hz_label"],
                cfg["pathway"], cfg["level"],
                ra_s, tr_s, cr["status"], lat_s,
                m.get("cadence_errors", "-"),
                m.get("stamp_crc_errors", "-"),
                str(cr.get("detail", "")).replace("|", "/").replace("\n", " "),
            )
        )

    neg = report.get("negative_controls") or []
    if neg:
        lines += [
            "",
            "## Negative Controls (Fault Injection)",
            "",
            "| control_id | fault | expected_check | verifier | status | detail |",
            "|---|---|---|---|---|---|",
        ]
        for n in neg:
            lines.append("| %s | %s | %s | %s | **%s** | %s |" % (
                n["control_id"], n["fault"], n["expected_check"], n["verifier_status"], n["status"],
                str(n.get("detail", "")).replace("|", "/"),
            ))

    return "\n".join(lines) + "\n"


def write_trx(path, report):
    """Write Visual Studio TRX XML. Skipped cells are NotExecuted with explicit SKIP-* StdOut."""
    now = report["timestamp_utc"]
    entries = []
    for cr in report["cells"]:
        if cr["status"] == STATUS_PASS:
            entries.append(("Row27LoopRig." + cr["cell_id"], "Passed", cr["detail"]))
        elif cr["status"] == STATUS_FAIL:
            entries.append(("Row27LoopRig." + cr["cell_id"], "Failed", cr["detail"]))
        else:
            entries.append(("Row27LoopRig." + cr["cell_id"], "NotExecuted", cr["detail"]))
    for nc in report.get("negative_controls") or []:
        entries.append((
            "Row27LoopRig." + nc["control_id"],
            "Passed" if nc["status"] == STATUS_PASS else "Failed",
            nc["detail"],
        ))

    total = len(entries)
    passed = sum(1 for _, o, _ in entries if o == "Passed")
    failed = sum(1 for _, o, _ in entries if o == "Failed")
    not_exec = sum(1 for _, o, _ in entries if o == "NotExecuted")

    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<TestRun id="%s" name="Row27LoopRig (%s)" xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010">' % (
            uuid.uuid4(), escape(report["mode"])
        ),
        '<Times creation="%s" start="%s" finish="%s" />' % (now, now, now),
        "<TestDefinitions>",
    ]
    for name, _o, _m in entries:
        tid = uuid.uuid5(uuid.NAMESPACE_URL, "row27looprig/" + name)
        lines.append('<UnitTest name="%s" id="%s"><Execution id="%s" /><TestMethod codeBase="grid_runner.py" className="Row27LoopRig" name="%s" /></UnitTest>' % (
            escape(name), tid, tid, escape(name)
        ))
    lines.append("</TestDefinitions>")
    lines.append("<Results>")
    for name, outcome, msg in entries:
        tid = uuid.uuid5(uuid.NAMESPACE_URL, "row27looprig/" + name)
        lines.append('<UnitTestResult testName="%s" outcome="%s" testId="%s" executionId="%s">' % (
            escape(name), outcome, tid, tid
        ))
        if msg:
            if outcome == "Failed":
                lines.append("<Output><ErrorInfo><Message>%s</Message></ErrorInfo></Output>" % escape(msg))
            else:
                lines.append("<Output><StdOut>%s</StdOut></Output>" % escape(msg))
        lines.append("</UnitTestResult>")
    summary_outcome = "Failed" if failed else ("Completed" if not_exec == 0 else "Inconclusive")
    lines += [
        "</Results>",
        '<ResultSummary outcome="%s"><Counters total="%d" executed="%d" passed="%d" failed="%d" notExecuted="%d" /></ResultSummary>' % (
            summary_outcome, total, total - not_exec, passed, failed, not_exec
        ),
        "</TestRun>",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def write_junit(path, report):
    """Write JUnit XML with explicit <skipped type='SKIP-NO-HARDWARE'/> markers."""
    cells = report["cells"]
    negs = report.get("negative_controls") or []
    total = len(cells) + len(negs)
    failures = sum(1 for c in cells if c["status"] == STATUS_FAIL) + sum(1 for n in negs if n["status"] == STATUS_FAIL)
    skipped = sum(1 for c in cells if c["status"] in (STATUS_SKIP_HW, STATUS_SKIP_FOOTAGE))

    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<testsuite name="Row27LoopRig.%s" tests="%d" failures="%d" skipped="%d" timestamp="%s">' % (
            escape(report["profile"]), total, failures, skipped, escape(report["timestamp_utc"])
        ),
    ]
    for cr in cells:
        lines.append('  <testcase classname="Row27LoopRig.%s" name="%s">' % (
            escape(report["profile"]), escape(cr["cell_id"])
        ))
        if cr["status"] == STATUS_FAIL:
            lines.append('    <failure message="%s" />' % escape(cr["detail"]))
        elif cr["status"] in (STATUS_SKIP_HW, STATUS_SKIP_FOOTAGE):
            lines.append('    <skipped type="%s" message="%s" />' % (escape(cr["status"]), escape(cr["detail"])))
        lines.append("  </testcase>")
    for nc in negs:
        lines.append('  <testcase classname="Row27LoopRig.NegativeControls" name="%s">' % escape(nc["control_id"]))
        if nc["status"] == STATUS_FAIL:
            lines.append('    <failure message="%s" />' % escape(nc["detail"]))
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


# ---- CLI ---------------------------------------------------------------------------------------

def build_arg_parser():
    ap = argparse.ArgumentParser(description="Row-27 loop-rig runner, grid generator, and stamp verifier")
    ap.add_argument("--mode", choices=("hardware", "synthetic", "plan"), default="hardware",
                    help="Execution mode: hardware (default), synthetic, or plan")
    ap.add_argument("--dry-run", action="store_true", help="Alias for --mode plan")
    ap.add_argument("--synthetic", action="store_true", help="Alias for --mode synthetic")
    ap.add_argument("--profile", default="row27", choices=("row27", "smoke", "full", "custom"),
                    help="Grid profile (default: row27)")
    ap.add_argument("--grid", default=rg.DEFAULT_GRID_PATH, help="Path to grid JSON spec")
    ap.add_argument("--custom-json", help="Custom grid JSON file (for --profile custom)")
    ap.add_argument("--expand", choices=("pairwise", "cartesian"), help="Override expansion mode")
    ap.add_argument("--source-rates", help="Comma-separated custom source rates (e.g. 23.976,47.952,60)")
    ap.add_argument("--display-rates", help="Comma-separated custom display rates (e.g. 59.94,119.88,120)")
    ap.add_argument("--filter", dest="filter_expr", default="",
                    help="Filter cells (e.g. pathway=vpv,render_ahead=true,track!=dirty or regex)")
    ap.add_argument("--duration", type=float, help="Override cell duration in seconds")
    ap.add_argument("--out", default="row27-loop-out", help="Output directory for report.json / report.md / plan")
    ap.add_argument("--trx", help="Optional TRX output path")
    ap.add_argument("--junit", help="Optional JUnit XML output path")
    ap.add_argument("--build-dir", default=DEFAULT_BUILD_DIR, help="x64 Release build directory (hardware mode)")
    ap.add_argument("--recorder", help="Path to VideoProcessorStreamRecorder.exe (hardware mode)")
    ap.add_argument("--device-match", default=DEFAULT_DEVICE_MATCH, help="Regex matching DeckLink capture device")
    ap.add_argument("--clips-dir", help="Directory holding external test clips (hardware mode)")
    ap.add_argument("--replay-dir", help="Directory holding recorded per-cell captures to score (hardware mode)")
    ap.add_argument("--include-negative-controls", action="store_true",
                    help="Run fault-injected negative controls alongside synthetic mode")
    ap.add_argument("--synthetic-frames", type=int, default=64,
                    help="Source frames per cell in synthetic mode (default: 64)")
    ap.add_argument("--write-stamps", action="store_true",
                    help="Write per-cell vp-e2e-stamp/1 JSONL files under <out>/stamps/")
    ap.add_argument("--quiet", action="store_true", help="Suppress stdout summary")
    return ap


def main(argv=None):
    ap = build_arg_parser()
    args = ap.parse_args(argv)

    mode = args.mode
    if args.dry_run:
        mode = "plan"
    elif args.synthetic:
        mode = "synthetic"

    src_rates = [x.strip() for x in args.source_rates.split(",") if x.strip()] if args.source_rates else None
    disp_rates = [x.strip() for x in args.display_rates.split(",") if x.strip()] if args.display_rates else None
    custom_spec = None
    if args.custom_json:
        try:
            with open(args.custom_json, "r", encoding="utf-8") as f:
                custom_spec = json.load(f)
        except (OSError, ValueError) as e:
            print("error loading --custom-json: %s" % e, file=sys.stderr)
            return EXIT_USAGE

    try:
        plan = rg.resolve_grid(
            profile=args.profile,
            grid_path=args.grid,
            expand=args.expand,
            source_rates=src_rates,
            display_rates=disp_rates,
            filter_expr=args.filter_expr,
            custom_spec=custom_spec,
            duration_s=args.duration,
        )
    except (rg.GridError, OSError, ValueError, KeyError) as e:
        print("grid error: %s" % e, file=sys.stderr)
        return EXIT_USAGE

    os.makedirs(args.out, exist_ok=True)

    if mode == "plan":
        plan_json_path = os.path.join(args.out, "plan.json")
        plan_md_path = os.path.join(args.out, "plan.md")
        with open(plan_json_path, "w", encoding="utf-8") as f:
            json.dump(plan, f, indent=1)
        md = rg.render_plan_md(plan)
        with open(plan_md_path, "w", encoding="utf-8") as f:
            f.write(md)
        if not args.quiet:
            print(md)
            print("Plan written: %s (%d cells)" % (plan_json_path, plan["total_cells"]))
        return EXIT_PASS

    report = execute_grid(
        plan=plan,
        mode=mode,
        out_dir=args.out,
        build_dir=args.build_dir,
        recorder_path=args.recorder,
        device_match=args.device_match,
        clips_dir=args.clips_dir,
        replay_dir=args.replay_dir,
        include_negative_controls=args.include_negative_controls,
        synthetic_frames=args.synthetic_frames,
        write_stamps=args.write_stamps,
    )

    rep_json_path = os.path.join(args.out, "report.json")
    rep_md_path = os.path.join(args.out, "report.md")
    with open(rep_json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    md = render_report_md(report)
    with open(rep_md_path, "w", encoding="utf-8") as f:
        f.write(md)

    if args.trx:
        write_trx(args.trx, report)
    if args.junit:
        write_junit(args.junit, report)

    if not args.quiet:
        c = report["counts"]
        print("Row-27 Loop Rig [%s/%s]: %s (exit %d) | total=%d pass=%d fail=%d skip_hw=%d skip_footage=%d" % (
            report["mode"], report["profile"], report["overall_status"], report["exit_code"],
            c["total"], c["pass"], c["fail"], c["skip_no_hardware"], c["skip_no_footage"]
        ))
        if report["counts"]["skip_no_hardware"] > 0 or report["counts"]["skip_no_footage"] > 0:
            for cr in report["cells"][:5]:
                if cr["status"] in (STATUS_SKIP_HW, STATUS_SKIP_FOOTAGE):
                    print("  " + cr["detail"])
            if len(report["cells"]) > 5:
                print("  ... (%d skipped cells total; see %s)" % (
                    c["skip_no_hardware"] + c["skip_no_footage"], rep_md_path
                ))
        print("Reports: %s, %s" % (rep_json_path, rep_md_path))

    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
