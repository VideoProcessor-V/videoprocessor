# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Parameterized Row-27 loop-rig grid generator and matrix resolver.

Builds deterministic test matrices over arbitrary (source_rate, display_rate) rational pairs,
pathways (original vs vpv), Levels 0-4, render-ahead off/on with queue depth L=0..4|auto,
de-judder off/on, color modes (sdr/hdr10/dv_tunnel), and tracks (clean/dirty/rate_transition).

Nothing is hardcoded to 23.976/60: every cadence ratio, cadence class, swapchain/queue depth,
and period-scaled tolerance derives from exact rational source and display rates.
"""
import itertools
import json
import math
import os
import re
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GRID_PATH = os.path.join(HERE, "row27_grid.json")

SCHEMA_GRID = "vp-row27-grid/1"
SCHEMA_PLAN = "vp-row27-plan/1"

# Standard NTSC 1000/1001 shorthand decimals mapped to exact rationals so 23.976 -> 59.94
# and arbitrary multiples like 47.952 -> 119.88 yield exact rational ratios (e.g. 5/2).
_NTSC_SHORTHAND = {
    "23.976": Fraction(24000, 1001),
    "29.97": Fraction(30000, 1001),
    "29.970": Fraction(30000, 1001),
    "47.952": Fraction(48000, 1001),
    "59.94": Fraction(60000, 1001),
    "59.940": Fraction(60000, 1001),
    "119.88": Fraction(120000, 1001),
    "119.880": Fraction(120000, 1001),
    "239.76": Fraction(240000, 1001),
}

# RenderAheadPolicy.h constants (single reference mirrored here for planning)
RA_CADENCE_TOLERANCE = 0.05
RA_QUEUED_FRAME_LATENCY = 3
RA_MIN_BUFFERS = 3
RA_MAX_BUFFERS = 6


class GridError(Exception):
    pass


def parse_rate(value):
    """Parse any rate representation into an exact positive Fraction (Hz).

    Accepts:
      - Fraction
      - [num, den] or (num, den)
      - "NUM/DEN" string
      - decimal or integer string / int / float (including NTSC shorthands like 23.976, 47.952, 119.88)
    """
    if isinstance(value, Fraction):
        r = value
    elif isinstance(value, (list, tuple)):
        if len(value) != 2:
            raise GridError("rate pair must have 2 elements, got %r" % (value,))
        try:
            r = Fraction(int(value[0]), int(value[1]))
        except (TypeError, ValueError, ZeroDivisionError) as e:
            raise GridError("invalid rational rate %r: %s" % (value, e))
    elif isinstance(value, bool):
        raise GridError("rate cannot be bool: %r" % (value,))
    elif isinstance(value, (int, float)):
        if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
            raise GridError("non-finite rate: %r" % (value,))
        s = ("%g" % value) if isinstance(value, float) else str(value)
        if s in _NTSC_SHORTHAND:
            r = _NTSC_SHORTHAND[s]
        else:
            r = Fraction(str(value)).limit_denominator(1000000)
    elif isinstance(value, str):
        s = value.strip()
        if not s:
            raise GridError("empty rate string")
        if s in _NTSC_SHORTHAND:
            r = _NTSC_SHORTHAND[s]
        elif "/" in s:
            parts = s.split("/")
            if len(parts) != 2:
                raise GridError("invalid rational rate %r" % (value,))
            try:
                r = Fraction(int(parts[0].strip()), int(parts[1].strip()))
            except (ValueError, ZeroDivisionError) as e:
                raise GridError("invalid rational rate %r: %s" % (value, e))
        else:
            try:
                f = float(s)
            except ValueError as e:
                raise GridError("invalid rate %r: %s" % (value, e))
            if math.isnan(f) or math.isinf(f):
                raise GridError("non-finite rate: %r" % (value,))
            r = Fraction(s).limit_denominator(1000000)
    else:
        raise GridError("unsupported rate type: %r" % (type(value),))

    if r <= 0:
        raise GridError("rate must be positive, got %s" % r)
    return r


def format_rate_label(rate):
    """Compact human-readable label for a Fraction Hz rate."""
    r = parse_rate(rate)
    for label, frac in _NTSC_SHORTHAND.items():
        if r == frac and not label.endswith("0"):
            return label
    if r.denominator == 1:
        return str(r.numerator)
    val = float(r)
    if abs(val - round(val, 3)) < 1e-6:
        return ("%.3f" % val).rstrip("0").rstrip(".")
    return "%d/%d" % (r.numerator, r.denominator)


def classify_cadence(src_rate, disp_rate):
    """Classify the exact (disp_rate / src_rate) relationship for any arbitrary rate pair.

    Returns one of:
      - "exact_integer"      (ratio is an exact integer, e.g. 24->48, 25->50, 59.94->59.94, 60->120)
      - "pulldown_3_2"       (exact 5/2 or 5/4 pulldown ratio, e.g. 23.976->59.94, 47.952->119.88)
      - "fractional_drift"   (non-exact ratio within 0.5% of an integer or 5/2, e.g. 23.976->60, 59.94->60)
      - "non_integer_judder" (general non-integer cadence, e.g. 25->60, 50->120, 29.97->50)
    """
    src = parse_rate(src_rate)
    disp = parse_rate(disp_rate)
    ratio = disp / src
    if ratio.denominator == 1:
        return "exact_integer"
    if ratio in (Fraction(5, 2), Fraction(5, 4)):
        return "pulldown_3_2"
    rf = float(ratio)
    nearest_int = max(1, int(round(rf)))
    if abs(rf - nearest_int) / nearest_int <= 0.005:
        return "fractional_drift"
    if abs(rf - 2.5) / 2.5 <= 0.005 or abs(rf - 1.25) / 1.25 <= 0.005:
        return "fractional_drift"
    return "non_integer_judder"


def max_extras(ratio):
    """Mirror of RenderAhead::MaxExtras(double ratio)."""
    rf = float(ratio)
    if rf <= 1.0 or not math.isfinite(rf):
        return 0
    owned = int(math.ceil(rf - RA_CADENCE_TOLERANCE))
    return min(max(owned - 1, 0), 15)


def swapchain_buffers(ratio):
    """Mirror of RenderAhead::SwapchainBuffers(double ratio)."""
    rf = float(ratio)
    if rf <= 0.0 or not math.isfinite(rf):
        return RA_MAX_BUFFERS
    wanted = int(math.ceil(rf - RA_CADENCE_TOLERANCE)) + 2
    return min(RA_MAX_BUFFERS, max(RA_MIN_BUFFERS, wanted))


def resolve_queue_depth(render_ahead, requested_depth, ratio):
    """Resolve requested queue depth (0..4 or 'auto') into a concrete target depth."""
    if not render_ahead:
        return 0
    if str(requested_depth).lower() == "auto":
        # Match RenderAhead::kQueuedFrameLatency (=3) when ratio > 1, or 1 for 1:1/sub-1 rates
        # when render-ahead is explicitly requested so depth > 0 is always verifiable.
        rf = float(ratio)
        if rf > 1.0 + RA_CADENCE_TOLERANCE:
            return min(4, max(2, min(RA_QUEUED_FRAME_LATENCY, max_extras(rf) + 1)))
        return 1
    d = int(requested_depth)
    if d < 0 or d > 4:
        raise GridError("queue_depth must be 0..4 or 'auto', got %r" % (requested_depth,))
    if d == 0:
        raise GridError("queue_depth=0 is invalid when render_ahead is enabled")
    return d


def default_transition_target(src_rate, disp_rate):
    """Pick a distinct, valid post-transition source rate for a rate_transition track."""
    src = parse_rate(src_rate)
    disp = parse_rate(disp_rate)
    # Choose a contrasting rate matching the display's family (50 Hz vs NTSC vs 60 Hz).
    if float(src) < 40.0:
        if disp.denominator == 1001:
            return Fraction(60000, 1001)
        if disp % 25 == 0 and disp % 60 != 0:
            return Fraction(50, 1)
        return Fraction(60, 1) if disp >= 60 else disp
    else:
        if disp.denominator == 1001:
            return Fraction(24000, 1001)
        if disp % 25 == 0 and disp % 60 != 0:
            return Fraction(25, 1)
        return Fraction(24, 1)


def _slug(text):
    return re.sub(r"[^A-Za-z0-9]+", "_", str(text)).strip("_").lower()


def build_cell(spec, defaults, index=0):
    """Normalize and validate a single grid cell into canonical plan schema."""
    src_rate = parse_rate(spec["src"] if "src" in spec else spec["source_rate"])
    disp_rate = parse_rate(spec["disp"] if "disp" in spec else spec["display_rate"])
    ratio = disp_rate / src_rate
    cadence_class = classify_cadence(src_rate, disp_rate)

    pathway = str(spec.get("pathway", "vpv")).lower()
    if pathway not in ("original", "vpv"):
        raise GridError("invalid pathway %r" % (pathway,))

    level = int(spec.get("level", 0))
    if level < 0 or level > 4:
        raise GridError("level must be 0..4, got %r" % (level,))
    if pathway == "original":
        level = 0

    render_ahead = bool(spec.get("render_ahead", False))
    dejudder = bool(spec.get("dejudder", False))
    requested_depth = spec.get("queue_depth", 0 if not render_ahead else "auto")

    if pathway == "original":
        # Bill baseline pathway: VP-V render-ahead and VP-V de-judder are off.
        render_ahead = False
        dejudder = False
        requested_depth = 0

    resolved_depth = resolve_queue_depth(render_ahead, requested_depth, ratio)

    color_mode = str(spec.get("color_mode", "sdr")).lower()
    if color_mode not in ("sdr", "hdr10", "dv_tunnel"):
        raise GridError("invalid color_mode %r" % (color_mode,))

    track = str(spec.get("track", "clean")).lower()
    if track not in ("clean", "dirty", "rate_transition"):
        raise GridError("invalid track %r" % (track,))

    transition_to = None
    if track == "rate_transition":
        raw_t = spec.get("transition_to") or spec.get("transition_to_rate")
        transition_to = parse_rate(raw_t) if raw_t is not None else default_transition_target(src_rate, disp_rate)
        if transition_to == src_rate:
            transition_to = default_transition_target(src_rate, disp_rate)

    size = list(spec.get("size", [1920, 1080]))
    duration_s = float(spec.get("duration_s", defaults.get("duration_s", 12.0)))
    if duration_s <= 0:
        raise GridError("duration_s must be positive")

    src_lbl = format_rate_label(src_rate)
    disp_lbl = format_rate_label(disp_rate)
    ra_lbl = ("ra_L%s" % requested_depth) if render_ahead else "ra_off"
    dj_lbl = "dj_on" if dejudder else "dj_off"
    pw_lbl = "orig" if pathway == "original" else ("vpv_l%d" % level)
    tr_lbl = track if transition_to is None else ("trans_%s" % format_rate_label(transition_to))
    cell_id = spec.get("cell_id") or _slug("%s_to_%s_%s_%s_%s_%s_%s" % (
        src_lbl, disp_lbl, pw_lbl, ra_lbl, dj_lbl, color_mode, tr_lbl
    ))

    out = {
        "cell_id": cell_id,
        "index": index,
        "source_rate": [src_rate.numerator, src_rate.denominator],
        "source_hz_label": src_lbl,
        "source_hz": round(float(src_rate), 6),
        "display_rate": [disp_rate.numerator, disp_rate.denominator],
        "display_hz_label": disp_lbl,
        "display_hz": round(float(disp_rate), 6),
        "cadence_ratio": [ratio.numerator, ratio.denominator],
        "cadence_ratio_float": round(float(ratio), 6),
        "cadence_class": cadence_class,
        "size": size,
        "pathway": pathway,
        "level": level,
        "render_ahead": render_ahead,
        "requested_queue_depth": requested_depth,
        "resolved_queue_depth": resolved_depth,
        "swapchain_buffers": swapchain_buffers(ratio) if render_ahead else RA_MIN_BUFFERS,
        "max_extras": max_extras(ratio),
        "dejudder": dejudder,
        "color_mode": color_mode,
        "track": track,
        "transition_to_rate": [transition_to.numerator, transition_to.denominator] if transition_to else None,
        "transition_to_label": format_rate_label(transition_to) if transition_to else None,
        "duration_s": duration_s,
        "requires_clip": spec.get("requires_clip"),
        "tolerances": {
            "phase_tolerance_periods": float(spec.get("phase_tolerance_periods",
                                                      defaults.get("phase_tolerance_periods", 0.25))),
            "delay_spread_slack_periods": float(spec.get("delay_spread_slack_periods",
                                                         defaults.get("delay_spread_slack_periods", 0.50))),
            "delay_drift_limit_periods": float(spec.get("delay_drift_limit_periods",
                                                        defaults.get("delay_drift_limit_periods", 0.35))),
            "landing_tolerance_vblanks": int(spec.get("landing_tolerance_vblanks",
                                                      defaults.get("landing_tolerance_vblanks", 0))),
            "max_late_landing_fraction": float(spec.get("max_late_landing_fraction",
                                                        defaults.get("max_late_landing_fraction", 0.0))),
            "dirty_recovery_source_periods": int(spec.get("dirty_recovery_source_periods",
                                                          defaults.get("dirty_recovery_source_periods", 8))),
            "transition_converge_source_periods": int(spec.get("transition_converge_source_periods",
                                                               defaults.get("transition_converge_source_periods", 12))),
            "min_frames": int(spec.get("min_frames", defaults.get("min_frames", 48))),
        },
    }
    return out


def _valid_mode_combo(combo):
    """Validity constraint for pathway/level/render_ahead/queue_depth/dejudder combinations."""
    pathway, level, ra, depth, dj = combo
    if pathway == "original":
        return level == 0 and (not ra) and depth == 0 and (not dj)
    if not ra:
        return depth == 0
    return depth != 0


def generate_pairwise_cells(axes, defaults):
    """Deterministic greedy 2-way covering-array (pairwise) generator across the full grid axes.

    Covers every valid pair of:
      (source_rate, display_mode, pipeline_mode, color_mode, track)
    where pipeline_mode enumerates valid (pathway, level, render_ahead, queue_depth, dejudder)
    combinations (folded into structured tuples so constraints are always respected cleanly).
    """
    src_list = [parse_rate(x["rate"] if isinstance(x, dict) else x) for x in axes["source_rates"]]
    disp_list = [
        (parse_rate(x["rate"] if isinstance(x, dict) else x),
         x.get("size", [1920, 1080]) if isinstance(x, dict) else [1920, 1080])
        for x in axes["display_modes"]
    ]
    pathways = list(axes.get("pathways", ["original", "vpv"]))
    levels = [int(x) for x in axes.get("levels", [0, 1, 2, 3, 4])]
    ras = [bool(x) for x in axes.get("render_ahead", [False, True])]
    depths = list(axes.get("queue_depths", [0, 1, 2, 3, 4, "auto"]))
    djs = [bool(x) for x in axes.get("dejudder", [False, True])]
    colors = list(axes.get("color_modes", ["sdr", "hdr10", "dv_tunnel"]))
    tracks = list(axes.get("tracks", ["clean", "dirty", "rate_transition"]))

    # Construct a representative valid set of pipeline configurations covering every
    # valid (pathway, level), (level, ra), (ra, depth), (ra, dj), (depth, dj) pair.
    pw_tuples = []
    if "original" in pathways:
        pw_tuples.append(("original", 0, False, 0, False))
    if "vpv" in pathways:
        # Pairwise over (level, ra_depth, dj) for vpv
        ra_depths = []
        if False in ras:
            ra_depths.append((False, 0))
        if True in ras:
            for d in depths:
                if d != 0:
                    ra_depths.append((True, d))
        vpv_candidates = [
            ("vpv", lv, ra, dp, dj)
            for lv in levels for (ra, dp) in ra_depths for dj in djs
            if _valid_mode_combo(("vpv", lv, ra, dp, dj))
        ]
        # Greedy pairwise over the 3 inner VPV dimensions (level, (ra,dp), dj)
        pw_tuples.extend(_greedy_pairwise_from_candidates(
            vpv_candidates,
            key_fn=lambda t: (t[1], (t[2], t[3]), t[4]),
        ))

    # Now build deterministic 2-way covering array over the 5 outer parameters:
    # 0: src_rate, 1: disp_mode, 2: pipeline_tuple, 3: color_mode, 4: track
    # We keep disp >= src * 0.5 (avoid nonsensical 120->24 drop-only pairs unless explicitly requested).
    domains = [
        list(range(len(src_list))),
        list(range(len(disp_list))),
        list(range(len(pw_tuples))),
        list(range(len(colors))),
        list(range(len(tracks))),
    ]
    all_pairs = set()
    for a in range(len(domains)):
        for b in range(a + 1, len(domains)):
            for va in domains[a]:
                for vb in domains[b]:
                    if a == 0 and b == 1:
                        # Skip down-conversion where display_hz < source_hz (lossless no-drop cadence requires disp >= src)
                        if float(disp_list[vb][0] / src_list[va]) < 1.0 - 1e-6:
                            continue
                    all_pairs.add((a, va, b, vb))

    chosen = []
    uncovered = set(all_pairs)
    # Deterministic greedy selection: pick the valid tuple covering the most uncovered pairs
    while uncovered:
        # Anchor on the lexicographically first uncovered pair for speed & determinism
        a0, va0, b0, vb0 = min(uncovered)
        best_row = None
        best_gain = -1
        # Build candidate row by greedy per-column choice starting from (a0=va0, b0=vb0)
        row = [None] * len(domains)
        row[a0] = va0
        row[b0] = vb0
        order = [c for c in range(len(domains)) if row[c] is None]
        for col in order:
            best_v = None
            best_c = -1
            for v in domains[col]:
                if col == 0 and row[1] is not None and float(disp_list[row[1]][0] / src_list[v]) < 1.0 - 1e-6:
                    continue
                if col == 1 and row[0] is not None and float(disp_list[v][0] / src_list[row[0]]) < 1.0 - 1e-6:
                    continue
                gain = 0
                for prev_c in range(len(domains)):
                    if row[prev_c] is not None:
                        p = (prev_c, row[prev_c], col, v) if prev_c < col else (col, v, prev_c, row[prev_c])
                        if p in uncovered:
                            gain += 1
                if gain > best_c:
                    best_c = gain
                    best_v = v
            row[col] = best_v if best_v is not None else domains[col][0]
        row_t = tuple(row)
        # Verify gain > 0
        covered_now = set()
        for a in range(len(domains)):
            for b in range(a + 1, len(domains)):
                p = (a, row_t[a], b, row_t[b])
                if p in uncovered:
                    covered_now.add(p)
        if not covered_now:
            # Should never happen, guard against infinite loop
            uncovered.discard((a0, va0, b0, vb0))
            continue
        uncovered -= covered_now
        chosen.append(row_t)

    cells = []
    for idx, (si, di, pi, ci, ti) in enumerate(chosen):
        src_r = src_list[si]
        disp_r, size = disp_list[di]
        pathway, level, ra, depth, dj = pw_tuples[pi]
        spec = {
            "source_rate": [src_r.numerator, src_r.denominator],
            "display_rate": [disp_r.numerator, disp_r.denominator],
            "size": size,
            "pathway": pathway,
            "level": level,
            "render_ahead": ra,
            "queue_depth": depth,
            "dejudder": dj,
            "color_mode": colors[ci],
            "track": tracks[ti],
        }
        cells.append(build_cell(spec, defaults, index=idx))
    return cells


def _greedy_pairwise_from_candidates(candidates, key_fn):
    """Select a minimal deterministic subset of candidates covering all 2-way factor pairs."""
    keys = [key_fn(c) for c in candidates]
    if not keys:
        return []
    ncols = len(keys[0])
    uncovered = set()
    for k in keys:
        for a in range(ncols):
            for b in range(a + 1, ncols):
                uncovered.add((a, k[a], b, k[b]))
    selected = []
    while uncovered:
        best_i = 0
        best_cov = set()
        for i, k in enumerate(keys):
            cov = {
                (a, k[a], b, k[b])
                for a in range(ncols)
                for b in range(a + 1, ncols)
                if (a, k[a], b, k[b]) in uncovered
            }
            if len(cov) > len(best_cov):
                best_cov = cov
                best_i = i
        if not best_cov:
            break
        selected.append(candidates[best_i])
        uncovered -= best_cov
    return selected


def generate_cartesian_cells(axes, defaults):
    """Full valid Cartesian expansion across axes (useful for custom targeted rate grids)."""
    src_list = [parse_rate(x["rate"] if isinstance(x, dict) else x) for x in axes["source_rates"]]
    disp_list = [
        (parse_rate(x["rate"] if isinstance(x, dict) else x),
         x.get("size", [1920, 1080]) if isinstance(x, dict) else [1920, 1080])
        for x in axes["display_modes"]
    ]
    pathways = list(axes.get("pathways", ["original", "vpv"]))
    levels = [int(x) for x in axes.get("levels", [0, 1, 2, 3, 4])]
    ras = [bool(x) for x in axes.get("render_ahead", [False, True])]
    depths = list(axes.get("queue_depths", [0, 1, 2, 3, 4, "auto"]))
    djs = [bool(x) for x in axes.get("dejudder", [False, True])]
    colors = list(axes.get("color_modes", ["sdr"]))
    tracks = list(axes.get("tracks", ["clean"]))

    cells = []
    seen_ids = set()
    idx = 0
    for src_r, (disp_r, size), pw, lv, ra, dp, dj, col, tr in itertools.product(
            src_list, disp_list, pathways, levels, ras, depths, djs, colors, tracks):
        if not _valid_mode_combo((pw, lv, ra, dp, dj)):
            continue
        spec = {
            "source_rate": [src_r.numerator, src_r.denominator],
            "display_rate": [disp_r.numerator, disp_r.denominator],
            "size": size,
            "pathway": pw,
            "level": lv,
            "render_ahead": ra,
            "queue_depth": dp,
            "dejudder": dj,
            "color_mode": col,
            "track": tr,
        }
        cell = build_cell(spec, defaults, index=idx)
        if cell["cell_id"] in seen_ids:
            continue
        seen_ids.add(cell["cell_id"])
        cells.append(cell)
        idx += 1
    return cells


def load_grid_doc(path=DEFAULT_GRID_PATH):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("schema") != SCHEMA_GRID:
        raise GridError("expected schema %r in %s, got %r" % (SCHEMA_GRID, path, doc.get("schema")))
    return doc


def _parse_filter_expr(expr):
    """Parse comma-separated filter terms: 'key=val,key!=val' or bare regex matching cell_id."""
    terms = []
    if not expr:
        return terms
    for raw in expr.split(","):
        tok = raw.strip()
        if not tok:
            continue
        if "!=" in tok:
            k, v = tok.split("!=", 1)
            terms.append(("ne", k.strip(), v.strip()))
        elif "=" in tok:
            k, v = tok.split("=", 1)
            terms.append(("eq", k.strip(), v.strip()))
        else:
            terms.append(("regex", "cell_id", tok))
    return terms


def _cell_field_str(cell, key):
    if key in cell:
        val = cell[key]
    elif key == "src":
        val = cell["source_hz_label"]
    elif key == "disp":
        val = cell["display_hz_label"]
    elif key == "depth":
        val = cell["requested_queue_depth"]
    else:
        return None
    if isinstance(val, bool):
        return "true" if val else "false"
    return str(val)


def filter_cells(cells, filter_expr):
    terms = _parse_filter_expr(filter_expr)
    if not terms:
        return list(cells)
    out = []
    for c in cells:
        keep = True
        for op, k, v in terms:
            fv = _cell_field_str(c, k)
            if op == "eq":
                if fv is None or fv.lower() != v.lower():
                    keep = False
                    break
            elif op == "ne":
                if fv is not None and fv.lower() == v.lower():
                    keep = False
                    break
            elif op == "regex":
                if fv is None or not re.search(v, fv, re.I):
                    keep = False
                    break
        if keep:
            out.append(c)
    return out


def resolve_grid(profile="row27", grid_path=DEFAULT_GRID_PATH, expand=None,
                 source_rates=None, display_rates=None, filter_expr=None,
                 custom_spec=None, duration_s=None):
    """Resolve a profile/custom grid specification into a canonical plan dict."""
    doc = load_grid_doc(grid_path)
    defaults = dict(doc.get("defaults", {}))
    if duration_s is not None:
        defaults["duration_s"] = float(duration_s)

    axes = json.loads(json.dumps(doc.get("axes", {})))
    if source_rates:
        axes["source_rates"] = [
            {"name": format_rate_label(parse_rate(r)),
             "rate": [parse_rate(r).numerator, parse_rate(r).denominator]}
            for r in source_rates
        ]
    if display_rates:
        axes["display_modes"] = [
            {"name": format_rate_label(parse_rate(r)),
             "rate": [parse_rate(r).numerator, parse_rate(r).denominator],
             "size": [1920, 1080]}
            for r in display_rates
        ]

    if profile == "custom":
        if custom_spec is not None:
            if "cells" in custom_spec:
                cells = [build_cell(c, defaults, i) for i, c in enumerate(custom_spec["cells"])]
            elif "axes" in custom_spec:
                merged = dict(axes)
                merged.update(custom_spec["axes"])
                mode = expand or custom_spec.get("expand", "pairwise")
                cells = generate_cartesian_cells(merged, defaults) if mode == "cartesian" else generate_pairwise_cells(merged, defaults)
            else:
                raise GridError("custom spec must define 'cells' or 'axes'")
        else:
            mode = expand or ("cartesian" if (source_rates or display_rates) else "pairwise")
            # For CLI custom rate overrides without custom JSON, keep concise representative axes unless expanded
            if source_rates or display_rates:
                custom_axes = dict(axes)
                custom_axes.setdefault("pathways", ["original", "vpv"])
                custom_axes["levels"] = [0, 2]
                custom_axes["queue_depths"] = [0, "auto"]
                custom_axes["dejudder"] = [False, True]
                custom_axes["color_modes"] = ["sdr"]
                custom_axes["tracks"] = ["clean"]
                cells = (generate_cartesian_cells(custom_axes, defaults)
                         if mode == "cartesian" else generate_pairwise_cells(custom_axes, defaults))
            else:
                cells = generate_pairwise_cells(axes, defaults)
    else:
        profiles = doc.get("profiles", {})
        if profile not in profiles:
            raise GridError("unknown profile %r (known: %s, custom)" % (
                profile, ", ".join(sorted(profiles.keys()))
            ))
        pdoc = profiles[profile]
        if expand in ("pairwise", "cartesian") or "cells" not in pdoc:
            mode = expand or pdoc.get("expand", "pairwise")
            cells = (generate_cartesian_cells(axes, defaults)
                     if mode == "cartesian" else generate_pairwise_cells(axes, defaults))
        else:
            cells = []
            for i, c in enumerate(pdoc["cells"]):
                c2 = dict(c)
                if duration_s is not None:
                    c2["duration_s"] = float(duration_s)
                cells.append(build_cell(c2, defaults, index=i))

    # Deduplicate cell_ids cleanly (append index if duplicate in custom/profile)
    seen = {}
    for i, c in enumerate(cells):
        cid = c["cell_id"]
        if cid in seen:
            seen[cid] += 1
            c["cell_id"] = "%s_%d" % (cid, seen[cid])
        else:
            seen[cid] = 0
        c["index"] = i

    cells = filter_cells(cells, filter_expr)
    for i, c in enumerate(cells):
        c["index"] = i

    cadence_counts = {}
    for c in cells:
        cc = c["cadence_class"]
        cadence_counts[cc] = cadence_counts.get(cc, 0) + 1

    return {
        "schema": SCHEMA_PLAN,
        "profile": profile,
        "expand": expand or ("profile_list" if profile not in ("full", "custom") else "pairwise"),
        "filter": filter_expr or "",
        "total_cells": len(cells),
        "cadence_classes": cadence_counts,
        "cells": cells,
    }


def render_plan_md(plan):
    lines = [
        "# Row-27 Loop Rig Grid Plan",
        "",
        "- Profile: `%s`" % plan["profile"],
        "- Expansion: `%s`" % plan["expand"],
        "- Filter: `%s`" % (plan["filter"] or "(none)"),
        "- Total cells: **%d**" % plan["total_cells"],
        "- Cadence classes: " + ", ".join("%s=%d" % kv for kv in sorted(plan["cadence_classes"].items())),
        "",
        "| # | cell_id | src Hz | disp Hz | ratio | cadence | pathway | lvl | RA | depth | dejudder | color | track |",
        "|---:|---|---:|---:|---:|---|---|---:|---|---:|---|---|---|",
    ]
    for c in plan["cells"]:
        tr = c["track"] if not c["transition_to_label"] else ("%s->%s" % (c["track"], c["transition_to_label"]))
        lines.append(
            "| %d | %s | %s | %s | %.4f | %s | %s | %d | %s | %s (%d) | %s | %s | %s |" % (
                c["index"], c["cell_id"], c["source_hz_label"], c["display_hz_label"],
                c["cadence_ratio_float"], c["cadence_class"], c["pathway"], c["level"],
                "on" if c["render_ahead"] else "off",
                c["requested_queue_depth"], c["resolved_queue_depth"],
                "on" if c["dejudder"] else "off", c["color_mode"], tr,
            )
        )
    return "\n".join(lines) + "\n"
