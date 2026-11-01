# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Streaming-practice encode presets, ABR ladders, per-shot VBR allocation and rung-switch plans.

Pure functions and data only (no ffmpeg calls here), so the rules are unit-tested and the generator
(make_streaming_encodes.py) stays a thin driver.  Everything is resolution- and rate-general: a rung is
a nominal WIDTH plus a bitrate at a 16:9 reference raster; the real bitrate is scaled by the master's
actual pixel area and frame rate.  Deterministic: no randomness, fixed iteration orders.

Why: services starve dark / grainy / fast shots with per-shot VBR under a capped average, and players
fall to low ladder rungs during congestion.  A fixed-bitrate encode (make_td_encodes.py) is kinder than
that; this module models the worse, realistic case.
"""
import math
import re

# ---------------------------------------------------------------- encoder presets
# kind: x265 | x264 | svtav1.  gop_s = closed-GOP length in seconds (2-4 s as on streaming services).
PRESETS = {
    "hevc_gop2": dict(
        kind="x265", pix_fmt="yuv420p10le", gop_s=2.0, bframes=3, b_adapt=2, refs=4, aq_mode=3, aq_strength=1.0,
        preset="medium", rc_lookahead=40, vbv_maxrate_ratio=1.5, vbv_buffer_s=2.0, threads=4, frame_threads=2,
        note="HEVC Main10, closed 2 s GOP, 3 B-frames (b-adapt 2, pyramid), AQ mode 3 (dark-biased auto-variance), "
             "no scene-cut keyframes (shots are encoded as separate closed segments), VBV 1.5x peak / 2 s buffer"),
    "hevc_gop4": dict(
        kind="x265", pix_fmt="yuv420p10le", gop_s=4.0, bframes=4, b_adapt=2, refs=4, aq_mode=3, aq_strength=1.0,
        preset="medium", rc_lookahead=60, vbv_maxrate_ratio=1.5, vbv_buffer_s=2.0, threads=4, frame_threads=2,
        note="as hevc_gop2 with a closed 4 s GOP and 4 B-frames (longer-GOP services)"),
    "avc_gop2": dict(
        kind="x264", pix_fmt="yuv420p", gop_s=2.0, bframes=3, refs=4, aq_mode=2, aq_strength=1.0, preset="medium",
        rc_lookahead=40, vbv_maxrate_ratio=1.5, vbv_buffer_s=2.0, threads=4,
        note="H.264 High 8-bit, closed 2 s GOP, 3 B-frames, AQ mode 2, VBV 1.5x / 2 s (the low-tier / legacy "
             "ladder; blockier than HEVC, which validates the blockiness metric)"),
    "av1": dict(
        kind="svtav1", pix_fmt="yuv420p10le", gop_s=2.0, preset=6, film_grain=0, film_grain_denoise=0,
        vbv_maxrate_ratio=1.5, vbv_buffer_s=2.0, fast_decode=0, threads=4,
        note="SVT-AV1 10-bit preset 6, closed 2 s GOP, VBR at the per-shot target (the per-shot cap is the allocation clamp), no film-grain synthesis"),
    "av1_fgs": dict(
        kind="svtav1", pix_fmt="yuv420p10le", gop_s=2.0, preset=6, film_grain=8, film_grain_denoise=1,
        vbv_maxrate_ratio=1.5, vbv_buffer_s=2.0, fast_decode=0, threads=4,
        note="as av1 with film-grain synthesis (strength 8, source denoised then re-synthesised at decode, "
             "the practice of AV1 services); the decoded grain is synthetic, not the master's"),
}

# ---------------------------------------------------------------- ladders
# rung = (name, nominal width, kbps at a 16:9 raster of that width and <= 30 fps).  Rungs wider than the
# master are skipped (never upscaled).  Index 0 is the top rung.
LADDERS = {
    "hd": [("1080p_6000", 1920, 6000), ("1080p_4500", 1920, 4500), ("1080p_3000", 1920, 3000),
           ("720p_1500", 1280, 1500)],
    "uhd": [("2160p_16000", 3840, 16000), ("2160p_12000", 3840, 12000), ("2160p_8000", 3840, 8000),
            ("1080p_4500", 1920, 4500)],
}

# Per-shot VBR rule (relative to the rung's average bitrate R): shot rate r_i is proportional to
# complexity^alpha, clamped to [floor R, cap R], re-normalised so the duration-weighted mean is R
# (never above).  cap < 2 is what starves dark / grainy / fast shots.
# alpha 0.9: close to constant-quality demand, so shots that need more than cap x R are starved relative to it.
# dark_knee / dark_min: a metric-driven allocator (VMAF-like) is nearly blind to banding and shadow detail, so the
# weight of a shot with mean luma below dark_knee (8-bit units) is scaled by max(mean / dark_knee, dark_min).
VBR = dict(alpha=0.9, floor=0.5, cap=1.5, min_shot_s=0.5, scene_threshold=0.25, probe_width=1280, probe_crf=26,
           dark_knee=64.0, dark_min=0.5)

# Rung-switch plans: entries are rung indices into the ladder (0 = top, -1 = lowest), spread evenly over the
# IDR (closed-GOP) switch points of the clip.  Players switch only at segment/IDR boundaries.
SWITCH_PLANS = {
    "congestion": [0, 0, -1, -1, 1, 0],
    "ramp_down": [0, 1, 2, -1],
}

FPS_REF = 30.0
FPS_EXPONENT = 0.5


def rung_raster(master_w, master_h, rung_w):
    """Even-sized raster of a rung for a master of master_w x master_h, or None if the rung would upscale."""
    if rung_w > master_w:
        return None
    w = rung_w
    h = int(round(master_h * w / master_w / 2.0)) * 2
    return w, max(h, 2)


def rung_kbps(base_kbps, rung_w, w, h, fps):
    """Bitrate for the real raster: scale by pixel area vs a 16:9 raster of the nominal width, and by
    (fps / 30)^0.5 above 30 fps.  Integer kb/s, deterministic."""
    area_ref = rung_w * rung_w * 9.0 / 16.0
    k = base_kbps * (w * h) / area_ref
    if fps > FPS_REF:
        k *= (fps / FPS_REF) ** FPS_EXPONENT
    return int(round(k))


def ladder_rungs(ladder, master_w, master_h, fps):
    """-> list of dict(name, w, h, kbps, index) for the rungs that fit (top first), no upscaling."""
    out = []
    for name, rw, kb in LADDERS[ladder]:
        r = rung_raster(master_w, master_h, rw)
        if r is None:
            continue
        out.append(dict(name=name, w=r[0], h=r[1], kbps=rung_kbps(kb, rw, r[0], r[1], fps)))
    for i, d in enumerate(out):
        d["index"] = i
    return out


# ---------------------------------------------------------------- shots
_PTS = re.compile(r"pts_time:\s*([0-9.]+)")


def parse_scene_log(text, fps, t0=0.0):
    """Frame indices (relative to the window start at t0 s) of the scene cuts in an ffmpeg
    select='gt(scene,T)',showinfo log.  Sorted, unique."""
    cuts = set()
    for m in _PTS.finditer(text):
        f = int(round((float(m.group(1)) - t0) * fps))
        if f > 0:
            cuts.add(f)
    return sorted(cuts)


def build_shots(cuts, n_frames, fps, min_shot_s=VBR["min_shot_s"]):
    """Shot list [(start, length)] covering n_frames.  Cuts closer than min_shot_s to the previous shot start
    (or to the end) are dropped, so every segment is long enough for a GOP structure."""
    min_len = max(int(round(min_shot_s * fps)), 2)
    starts = [0]
    for c in cuts:
        if c - starts[-1] >= min_len and n_frames - c >= min_len and 0 < c < n_frames:
            starts.append(c)
    ends = starts[1:] + [n_frames]
    return [(s, e - s) for s, e in zip(starts, ends)]


def gop_frames(gop_s, fps):
    return max(int(round(gop_s * fps)), 2)


def idr_points(shots, gop):
    """Frame indices of every closed-GOP start: each shot start plus every `gop` frames inside the shot."""
    pts = []
    for s, n in shots:
        pts.extend(range(s, s + n, gop))
    return pts


# ---------------------------------------------------------------- per-shot VBR allocation
def dark_weight(mean_luma, knee=VBR["dark_knee"], lo=VBR["dark_min"]):
    """Allocation weight of a shot from its mean luma (8-bit units): 1 at/above the knee, down to `lo` in the dark."""
    return min(max(mean_luma / knee, lo), 1.0)


def allocate_shot_kbps(durations, complexities, avg_kbps, alpha=VBR["alpha"], floor=VBR["floor"],
                       cap=VBR["cap"], weights=None):
    """Per-shot bitrate (kb/s) with a CAPPED AVERAGE: sum(d_i r_i)/sum(d_i) <= avg_kbps and
    floor*avg <= r_i <= cap*avg.  r_i follows complexity^alpha (water-filling when clamped).
    `weights` = optional per-shot multipliers (dark_weight).  Deterministic, monotone in complexity.  If every shot is clamped the mean can fall below avg."""
    n = len(durations)
    if n == 0:
        return []
    if n != len(complexities) or any(d <= 0 for d in durations):
        raise ValueError("durations/complexities mismatch or non-positive duration")
    lo, hi = floor * avg_kbps, cap * avg_kbps
    w = [max(c, 1e-9) ** alpha * (1.0 if weights is None else weights[i]) for i, c in enumerate(complexities)]
    free = list(range(n))
    fixed = {}
    for _ in range(n + 1):
        budget = avg_kbps * sum(durations) - sum(durations[i] * r for i, r in fixed.items())
        wsum = sum(durations[i] * w[i] for i in free)
        if not free or wsum <= 0:
            break
        scale = budget / wsum
        clamped = False
        for i in list(free):
            r = w[i] * scale
            if r < lo or r > hi:
                fixed[i] = min(max(r, lo), hi)
                free.remove(i)
                clamped = True
        if not clamped:
            for i in free:
                fixed[i] = w[i] * scale
            free = []
            break
    rates = [fixed[i] for i in range(n)]
    # integer kb/s, never above the cap on the mean
    rates = [int(math.floor(r + 1e-6)) for r in rates]
    return rates


def weighted_mean(durations, rates):
    return sum(d * r for d, r in zip(durations, rates)) / float(sum(durations))


# ---------------------------------------------------------------- switch plans
def switch_assignment(plan, n_points, n_rungs):
    """Rung index for each of n_points switch points: the plan's entries spread evenly (entry i covers
    points with floor(j * len(plan) / n_points) == i).  Negative entries count from the lowest rung;
    entries beyond the ladder clamp to its lowest rung."""
    out = []
    for j in range(n_points):
        e = plan[min(j * len(plan) // max(n_points, 1), len(plan) - 1)]
        idx = e if e >= 0 else n_rungs + e
        out.append(min(max(idx, 0), n_rungs - 1))
    return out


def frame_rung_map(shots, gop, plan, n_frames, n_rungs):
    """Per-frame rung index: each IDR segment [p_j, p_{j+1}) comes from the rung chosen for point j."""
    pts = idr_points(shots, gop)
    asg = switch_assignment(plan, len(pts), n_rungs)
    out = [0] * n_frames
    for j, p in enumerate(pts):
        q = pts[j + 1] if j + 1 < len(pts) else n_frames
        for f in range(p, q):
            out[f] = asg[j]
    return out


# ---------------------------------------------------------------- ffmpeg argument builders
def x265_params(p, kbps, fps, shot_frames, vbv=True):
    """-x265-params string for one shot segment (closed GOP from the segment start, no scene-cut)."""
    g = gop_frames(p["gop_s"], fps)
    prm = ["keyint=%d" % g, "min-keyint=%d" % g, "scenecut=0", "open-gop=0", "bframes=%d" % p["bframes"],
           "b-adapt=%d" % p["b_adapt"], "ref=%d" % p["refs"], "aq-mode=%d" % p["aq_mode"],
           "aq-strength=%s" % p["aq_strength"], "rc-lookahead=%d" % min(p["rc_lookahead"], max(shot_frames - 1, 0)),
           "bitrate=%d" % kbps, "frame-threads=%d" % p["frame_threads"], "pools=%d" % p["threads"],
           "no-info=1", "log-level=error"]
    if vbv:
        mx = int(round(kbps * p["vbv_maxrate_ratio"]))
        prm += ["vbv-maxrate=%d" % mx, "vbv-bufsize=%d" % int(round(mx * p["vbv_buffer_s"]))]
    return ":".join(prm)


def svtav1_params(p, kbps, fps, vbv=True):
    g = gop_frames(p["gop_s"], fps)
    prm = ["keyint=%d" % g, "scd=0", "irefresh-type=2", "rc=1", "tbr=%d" % kbps, "lp=%d" % p["threads"]]
    # SVT-AV1 only allows a max bitrate (mbr) in CRF mode; VBR (rc=1) uses the per-shot target alone
    if p.get("film_grain"):
        prm += ["film-grain=%d" % p["film_grain"], "film-grain-denoise=%d" % p["film_grain_denoise"]]
    if p.get("fast_decode"):
        prm += ["fast-decode=%d" % p["fast_decode"]]
    return ":".join(prm)


def encoder_args(p, kbps, fps, shot_frames, vbv=True):
    """ffmpeg output-codec arguments for one segment."""
    if p["kind"] == "x265":
        return ["-c:v", "libx265", "-preset", p["preset"], "-pix_fmt", p["pix_fmt"], "-tag:v", "hvc1",
                "-x265-params", x265_params(p, kbps, fps, shot_frames, vbv)]
    if p["kind"] == "x264":
        g = gop_frames(p["gop_s"], fps)
        mx = int(round(kbps * p["vbv_maxrate_ratio"]))
        prm = ["keyint=%d" % g, "min-keyint=%d" % g, "scenecut=0", "bframes=%d" % p["bframes"], "ref=%d" % p["refs"],
               "aq-mode=%d" % p["aq_mode"], "rc-lookahead=%d" % min(p["rc_lookahead"], max(shot_frames - 1, 0)),
               "bitrate=%d" % kbps, "threads=%d" % p["threads"]]
        if vbv:
            prm += ["vbv-maxrate=%d" % mx, "vbv-bufsize=%d" % int(round(mx * p["vbv_buffer_s"]))]
        return ["-c:v", "libx264", "-preset", p["preset"], "-pix_fmt", p["pix_fmt"], "-x264-params", ":".join(prm)]
    if p["kind"] == "svtav1":
        return ["-c:v", "libsvtav1", "-preset", str(p["preset"]), "-pix_fmt", p["pix_fmt"],
                "-svtav1-params", svtav1_params(p, kbps, fps, vbv)]
    raise ValueError("unknown encoder kind " + p["kind"])
