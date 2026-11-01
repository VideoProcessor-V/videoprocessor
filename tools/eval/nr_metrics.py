# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""No-reference quality metrics for captured streams that have no master.

All metrics work on ONE luma plane (float code values of any bit depth; internally converted to 8-bit units
so one threshold means the same thing at 8/10/12 bits) and are resolution-general: block sizes, radii and
tile sizes derive from the plane height, never from a fixed raster.  Deterministic (no randomness, fixed
reduction order).  Every metric returns a value plus a coverage (how much of the picture could be judged),
so "0" is never confused with "nothing to measure".

  band   banding step visibility on smooth regions: fraction of the picture (area) in smooth tiles that are
         a staircase of few flat levels one quantiser step apart, weighted by the step size (invisible below ~0.3 8-bit code, full at >= 1 code) and dark-weighted (shadows).
  block  blockiness: periodic excess of |first difference| at one phase of a block-transform grid
         (periods 8/16/32 searched, any phase, so crops and rung scaling do not matter), smooth pixels only.
  ring   ringing / mosquito halo: rms of the 1-px high-pass residual in a band 2..6 (x H/540) px beside
         strong edges, over the flat-area noise floor, in 8-bit units.
  noise  flat-area noise: robust sigma of the high-pass residual over the flattest 30 % of pixels
         (8-bit units).  Low = grain/noise stripped (or a clean source), not "good" or "bad" by itself.

Validated against the full-reference measures of the generated encodes by nr_validate.py (report in
findings-streaming.md).  `nr_extra` plugs into eval_harness.evaluate_suite(extra=...).
"""
import json
import math
import os

import numpy as np
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
THRESHOLDS_PATH = os.path.join(HERE, "nr_thresholds.json")
METRICS = ("band", "block", "ring", "noise")
BLOCK_PERIODS = (8, 16, 32)


def to_u8(plane, bitdepth):
    """Plane in 8-bit units (float64)."""
    return np.asarray(plane, dtype=np.float64) / float(2 ** (bitdepth - 8))


def _tile(h):
    """Banding tile size from the plane height: h/34 rounded to a multiple of 4 (544 -> 16, 1080 -> 32, 2160 -> 64)."""
    return int(min(max(int(round(h / 34.0 / 4.0)) * 4, 8), 128))


# ---------------------------------------------------------------- noise
def flat_mask(u, frac=0.3):
    """Flattest `frac` of the picture: low local energy of the 1-px residual and a gentle large-scale slope."""
    hp = u - ndi.gaussian_filter(u, 1.0)
    e = ndi.uniform_filter(hp * hp, 7)
    g = ndi.gaussian_filter(u, 2.0)
    gy, gx = np.gradient(g)
    grad = np.hypot(gx, gy)
    ok = grad < 3.0
    if not ok.any():
        return hp, np.zeros(u.shape, bool)
    thr = np.quantile(e[ok], frac)
    return hp, ok & (e <= thr)


def noise_sigma(u):
    """(sigma, coverage): robust (MAD) sigma of the 1-px residual in the flat mask, 8-bit units."""
    hp, m = flat_mask(u)
    if m.sum() < 64:
        return 0.0, 0.0
    return float(1.4826 * np.median(np.abs(hp[m] - np.median(hp[m])))), float(m.mean())


# ---------------------------------------------------------------- banding
def banding(u, bitdepth):
    """(score in 0..1, coverage).  See module doc.  Quantum q = one code of the plane's own bit depth."""
    h, w = u.shape
    t = _tile(h)
    st = max(t // 2, 1)                            # half-overlapping tiles: a step on a tile edge is still seen
    ys, xs = list(range(0, h - t + 1, st)), list(range(0, w - t + 1, st))
    if not ys or not xs:
        return 0.0, 0.0
    q = 1.0 / float(2 ** (bitdepth - 8))          # one native code in 8-bit units
    tot, elig, acc = len(ys) * len(xs), 0, 0.0
    peak_lim = 6.0                                 # a tile spanning more than 6 (8-bit) codes is not a plain ramp
    for j in ys:
        for i in xs:
            b = u[j:j + t, i:i + t]
            lo, hi = float(b.min()), float(b.max())
            rng = hi - lo
            if rng < 0.5 * q or rng > peak_lim:
                continue
            # flat-neighbour fraction on exact values (staircase: long runs, f >= ~0.9; dithered / noisy: f ~0.5)
            fx = (b[:, 1:] == b[:, :-1])
            fy = (b[1:, :] == b[:-1, :])
            f = 0.5 * (fx.mean() + fy.mean())
            lev = np.unique(np.round(b / q)) * q
            if len(lev) < 2:
                continue
            elig += 1
            s = min(max((f - 0.85) / 0.15, 0.0), 1.0)
            step = float(np.median(np.diff(lev)))              # level spacing, 8-bit units
            vis = min(max((step - 0.3) / 0.7, 0.0), 1.0)         # <= 0.3 code invisible, >= 1 8-bit code fully visible
            dark = 1.0 + 0.5 * (1.0 - min(float(b.mean()) / 255.0, 1.0))
            acc += s * vis * dark / 1.5
    return acc / tot, elig / float(tot)


# ---------------------------------------------------------------- blockiness
def _profile(u, axis, edge_limit):
    """Mean |first difference| per position along `axis`, ignoring strong edges (|d| > edge_limit)."""
    d = np.abs(np.diff(u, axis=axis))
    keep = d <= edge_limit
    s = np.where(keep, d, 0.0).sum(axis=1 - axis)
    c = keep.sum(axis=1 - axis)
    return s / np.maximum(c, 1), c


def blockiness(u, edge_limit=12.0):
    """(score >= 0, coverage): max over periods of (peak-phase / median-phase mean |d|) - 1, averaged over
    the two directions.  Coverage = fraction of first differences that were smooth enough to use."""
    scores, cov = [], []
    for axis in (1, 0):
        d = np.abs(np.diff(u, axis=axis))
        cov.append(float((d <= edge_limit).mean()))
        s, c = _profile(u, axis, edge_limit)
        n = len(s)
        best = 0.0
        for p in BLOCK_PERIODS:
            if n < 4 * p:
                continue
            m = n // p * p
            ph = s[:m].reshape(-1, p).mean(axis=0)
            med = float(np.median(ph))
            if med <= 1e-9:
                continue
            best = max(best, float(ph.max()) / med - 1.0)
        scores.append(best)
    return float(np.mean(scores)), float(np.mean(cov))


# ---------------------------------------------------------------- ringing
def edge_zone(u, h=None):
    """(strong-edge mask, ring-zone mask) from radii that scale with the plane height."""
    h = u.shape[0] if h is None else h
    g = ndi.gaussian_filter(u, 1.0)
    gy, gx = np.gradient(g)
    grad = np.hypot(gx, gy)
    edge = grad >= 10.0
    if not edge.any():
        return edge, np.zeros(u.shape, bool), grad
    r_in = max(2, int(round(h / 270.0)))
    r_out = max(6, int(round(h / 90.0)))
    dist = ndi.distance_transform_edt(~edge)
    zone = (dist > r_in) & (dist <= r_out) & (grad < 5.0)
    return edge, zone, grad


def ringing(u, floor_sigma=None):
    """(ring in 8-bit units, coverage = zone fraction)."""
    edge, zone, _ = edge_zone(u)
    cov = float(zone.mean())
    if zone.sum() < 256:
        return 0.0, cov
    hp = u - ndi.gaussian_filter(u, 1.0)
    if floor_sigma is None:
        floor_sigma = noise_sigma(u)[0]
    rms = math.sqrt(float(np.mean(hp[zone] ** 2)))
    return math.sqrt(max(rms * rms - floor_sigma * floor_sigma, 0.0)), cov


# ---------------------------------------------------------------- frame / sequence API
def nr_frame(plane, bitdepth):
    """dict nr_band, nr_block, nr_ring, nr_noise (+ *_cover) for one luma plane."""
    u = to_u8(plane, bitdepth)
    ns, nc = noise_sigma(u)
    b, bc = banding(u, bitdepth)
    k, kc = blockiness(u)
    r, rc = ringing(u, ns)
    return dict(nr_band=b, nr_band_cover=bc, nr_block=k, nr_block_cover=kc, nr_ring=r, nr_ring_cover=rc,
                nr_noise=ns, nr_noise_cover=nc)


def load_thresholds(path=THRESHOLDS_PATH):
    if os.path.exists(path):
        with open(path, "r", encoding="utf8") as f:
            return json.load(f)
    return None


def poor_index(vals, thr):
    """max over metrics of value / poor-threshold; >= 1 means 'poor' (some artefact at/over its threshold).
    Noise is not an artefact score and does not enter."""
    ratios = [vals["nr_" + k] / thr["poor"][k] for k in ("band", "block", "ring") if thr["poor"].get(k, 0) > 0]
    return max(ratios) if ratios else 0.0


def nr_sequence(planes, bitdepth, stride=1):
    """Per-frame NR dicts for every `stride`-th plane (deterministic order)."""
    return [dict(frame=i, **nr_frame(p, bitdepth)) for i, p in enumerate(planes) if i % stride == 0]


def summarize(rows, keys=("nr_band", "nr_block", "nr_ring", "nr_noise")):
    out = {}
    for k in keys:
        v = np.array([r[k] for r in rows], dtype=np.float64)
        out[k] = float(v.mean()) if len(v) else None
        out[k + "_p95"] = float(np.quantile(v, 0.95)) if len(v) else None
    return out


def nr_extra(truth, outs, inputs, case, notes, stride=None):
    """eval_harness.evaluate_suite(extra=...) hook: NR metrics of the candidate/default OUTPUT luma (mean over
    sampled frames) as nr_* rows.  Banding / blockiness / ringing: lower is better; noise is informational."""
    n = len(outs)
    stride = stride or max(1, n // 12)
    rows = nr_sequence([o.Y for o in outs], outs[0].bitdepth, stride)
    m = summarize(rows)
    res = {"nr_band": m["nr_band"], "nr_block": m["nr_block"], "nr_ring": m["nr_ring"], "nr_noise": m["nr_noise"]}
    thr = load_thresholds()
    if thr:
        res["nr_poor_index_p95"] = float(np.quantile([poor_index(r, thr) for r in rows], 0.95))
    return res
