# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Link-noise metric for the dither-need map (own limits; NOT the +-5 % grain rule).

The dither-need map adds sigma-delta dither on the 8-bit link where the grain is too weak to dither it. That added noise
is judged here, per region-evidence block (block size B as dither_need_ref.block_size_for), as the high-pass rms of the
OUTPUT ERROR (output - reference, both in display PQ codes as stack_runner.grain_stats does) for the candidate against
the base (plain+4):

  hp(x)     = x - gaussian_filter(x, 3)
  ratio(c)  = sqrt( sum_c hp(err_cand)^2 / sum_c hp(err_base)^2 )  over the shadow pixels (< 50 nit, region_tiles of
              the reference) of the blocks of class c
  class low : block sigma_out <  0.35   (sigma_out = sqrt(N) |Sout| / q_out, dither_need_ref law; weak grain)
  class high: the rest
Limits: LINK_NOISE_LIMIT_LOW = 1.40 (low class), LINK_NOISE_LIMIT_HIGH = 1.05 (elsewhere).
"""
import numpy as np
from scipy import ndimage

LINK_NOISE_LIMIT_LOW = 1.40
LINK_NOISE_LIMIT_HIGH = 1.05
SIGMA_OUT_SPLIT = 0.35
HP_SIGMA = 3.0
SHADOW_NIT = 50.0


def _seq(a):
    a = np.asarray(a, np.float64)
    return a[None] if a.ndim == 2 else a


def highpass(x):
    return x - ndimage.gaussian_filter(x, HP_SIGMA, mode="nearest")


def class_masks(sigma_out, B, shape, split=SIGMA_OUT_SPLIT):
    """sigma_out: (gh, gw) or (T, gh, gw) block grid -> (low, high) bool masks of shape (T, H, W) (T = 1 for 2-D input)."""
    s = np.asarray(sigma_out, np.float64)
    s = s[None] if s.ndim == 2 else s
    H, W = shape
    px = np.repeat(np.repeat(s, B, axis=1), B, axis=2)[:, :H, :W]
    low = px < split
    return low, ~low


def link_noise_core(err_cand, err_base, shadow, sigma_out, B, split=SIGMA_OUT_SPLIT):
    """Errors (T,H,W) already in comparison units; shadow bool (T,H,W) or None (all). Returns the metric dict."""
    ec, eb = _seq(err_cand), _seq(err_base)
    T, H, W = ec.shape
    low, high = class_masks(sigma_out, B, (H, W), split)
    if low.shape[0] == 1 and T > 1:
        low, high = np.repeat(low, T, 0), np.repeat(high, T, 0)
    sh = np.ones(ec.shape, bool) if shadow is None else np.asarray(shadow, bool)
    sums = {"low": [0.0, 0.0, 0], "high": [0.0, 0.0, 0]}
    for t in range(T):
        hc, hb = highpass(ec[t]), highpass(eb[t])
        for name, cm in (("low", low[t]), ("high", high[t])):
            sel = sh[t] & cm
            n = int(sel.sum())
            if n:
                s = sums[name]
                s[0] += float(np.sum(hc[sel] ** 2))
                s[1] += float(np.sum(hb[sel] ** 2))
                s[2] += n
    out = {}
    for name, (c, b, n) in sums.items():
        out[name] = float(np.sqrt(c / b)) if n and b > 1e-12 else None
        out["n_" + name] = n
    return out


def link_noise(ref, cand, base, sigma_out, B, ctx, lut=None, shadow_nit=SHADOW_NIT):
    """ref / cand / base: (T,H,W) planes in the signal's 10-bit-scale codes (ref = tone-mapped/master signal at the output
    scale); ctx = evaluation light_level_ref.Context. Returns {low, high, n_low, n_high}."""
    import light_level_ref as llr
    import light_metrics as lm
    lut = lut or llr.build_lut(ctx)
    R, C, Bs = (lm.display_planes(a, ctx, lut) for a in (ref, cand, base))
    shadow = lm.region_tiles(ref, ctx, None, shadow_nit, lut)
    return link_noise_core(C - R, Bs - R, shadow, sigma_out, B)


def verdict(ratios, low_limit=LINK_NOISE_LIMIT_LOW, high_limit=LINK_NOISE_LIMIT_HIGH):
    """Compare a link_noise result with the limits. Per class True/False, None when the class is empty; ok = every
    evaluated class passes (False when none is evaluated)."""
    lo, hi = ratios.get("low"), ratios.get("high")
    v = {"low": None if lo is None else bool(lo <= low_limit), "high": None if hi is None else bool(hi <= high_limit)}
    ev = [x for x in v.values() if x is not None]
    v["ok"] = bool(ev) and all(ev)
    return v
