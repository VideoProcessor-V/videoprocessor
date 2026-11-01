# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shadow metrics in the context's DISPLAY units (light_level_ref LUT).  float64, deterministic.

Display units: every plane (10-bit code values of the evaluated signal) is mapped through the context's LUT to
display luminance and then to PQ codes of that luminance, Jd = 64 + 876 * PQ_OETF(L).  A candidate's error is
therefore perceptual, whatever the delivered transfer.  Tiles/regions come from the MASTER only (J_eff).

Public API (planes: (H,W) or a sequence (T,H,W); pass master, candidate [, plain] of identical shape)
  display_planes(planes, ctx, lut=None)            -> Jd planes (same shape)
  region_tiles(master, ctx, lo_nit, hi_nit, lut=None, tile=None) -> bool mask (H,W) of pixels whose tile has
        PQ(lo_nit) <= J_eff < PQ(hi_nit)  (lo_nit=None: from 0)
  shadow_banding(master, cand, ctx, ranges=SHADOW_RANGES, lut=None, tile=None)
        -> {range_name: {"band_mse", "step_p99", "px", "value_ok", "na_reason"}}
        band_mse = mean square of the coarse error (3 px Gaussian of cand-master, as artefact_headroom.frame_metrics)
        over artefact_headroom.masks(...)["grad"] inside the region; step_p99 = 99th percentile of |coarse error|
        there (as artefact_headroom.summarize).  Values are None + na_reason when < MIN_PX (=100) qualifying pixels.
        Default ranges: "lt5" (J_eff < PQ(5 nit)) and "5to50" (PQ(5) <= J_eff < PQ(50)).
  shadow_texture_kept(master, plain, cand, ctx, lo_nit=None, hi_nit=5.0, gate=0.97, lut=None, tile=None)
        -> {"proj_plain", "proj_cand", "ratio", "gate", "pass", "px", "na_reason"}
        proj(x) = <fine(x), fine(master)> / <fine(master), fine(master)> over region pixels away from strong
        master edges (deband_stack.fine, eval_metrics.near_edge_zone, as deband_stack.projection);
        ratio = proj_cand / proj_plain (>= gate passes).  NA (ratio None, reason set) when nothing qualifies.
"""
import numpy as np
from scipy import ndimage

import artefact_headroom as ah
import deband_stack as ds
import eval_metrics as em
import light_level_ref as llr
import tonemap_ref as tr

MIN_PX = 100
SHADOW_RANGES = {"lt5": (None, 5.0), "5to50": (5.0, 50.0)}


def _seq(a):
    a = np.asarray(a, np.float64)
    return a[None] if a.ndim == 2 else a


def display_planes(planes, ctx, lut=None):
    lut = lut or llr.build_lut(ctx)
    return 64.0 + 876.0 * llr.fetch(lut, _seq(planes))["J"]


def region_tiles(master, ctx, lo_nit, hi_nit, lut=None, tile=None):
    """(T,H,W) bool: pixels whose J_eff tile is in [PQ(lo), PQ(hi)) (per frame, from the master)."""
    lut = lut or llr.build_lut(ctx)
    lo = -1.0 if lo_nit is None else float(tr.pq_oetf(lo_nit))
    hi = float(tr.pq_oetf(hi_nit))
    return np.stack([(lambda j: (j >= lo) & (j < hi))(llr.j_eff(m, ctx, tile, lut)) for m in _seq(master)])


def btb_kept(master, out, black=64.0, half_step=2.0):
    """BTB preservation for an 8-bit output: a master pixel counts as below-black only if it is below the black code
    by more than half an OUTPUT code step (half_step, 10-bit units; 2 for an 8-bit output = 2 ten-bit codes), so
    master values within rounding of black are not expected to survive.  kept = share of those pixels whose output is
    below the black code at all.  Returns (kept or None when no such pixel, count)."""
    r, o = np.asarray(master, np.float64), np.asarray(out, np.float64)
    sel = r < black - half_step
    n = int(sel.sum())
    return (float(np.mean(o[sel] < black)) if n else None), n


def shadow_banding(master, cand, ctx, ranges=SHADOW_RANGES, lut=None, tile=None):
    lut = lut or llr.build_lut(ctx)
    R, C = display_planes(master, ctx, lut), display_planes(cand, ctx, lut)
    out = {}
    pre = [(r, c, ah.masks(r, float(64.0 + 876.0 * tr.pq_oetf(5.0)))["grad"], ndimage.gaussian_filter(c - r, 3.0, mode="nearest"))
           for r, c in zip(R, C)]
    for name, (lo, hi) in ranges.items():
        reg = region_tiles(master, ctx, lo, hi, lut, tile)
        steps, sq = [], 0.0
        ntiles = int(reg.sum())
        for (r, c, grad, le), rg in zip(pre, reg):
            m = grad & rg
            steps.append(np.abs(le[m]))
            sq += float((le[m] ** 2).sum())
        s = np.concatenate(steps)
        res = dict(band_mse=None, step_p99=None, px=int(s.size), value_ok=False, na_reason=None)
        if ntiles == 0:
            res["na_reason"] = "no tiles with J_eff in the range"
        elif s.size < MIN_PX:
            res["na_reason"] = "only %d smooth-gradient pixels in range (< %d)" % (s.size, MIN_PX)
        else:
            res.update(band_mse=sq / s.size, step_p99=float(np.quantile(s, 0.99)), value_ok=True)
        out[name] = res
    return out


def shadow_texture_kept(master, plain, cand, ctx, lo_nit=None, hi_nit=5.0, gate=0.97, lut=None, tile=None):
    lut = lut or llr.build_lut(ctx)
    M, P, C = (display_planes(a, ctx, lut) for a in (master, plain, cand))
    reg = region_tiles(master, ctx, lo_nit, hi_nit, lut, tile)
    num_p = num_c = den = 0.0
    px = 0
    for m, p, c, rg in zip(M, P, C, reg):
        sel = rg & em.near_edge_zone(m, 10)[1]
        if not sel.any():
            continue
        fm = ds.fine(m)[sel]
        num_p += float(np.dot(ds.fine(p)[sel], fm))
        num_c += float(np.dot(ds.fine(c)[sel], fm))
        den += float(np.dot(fm, fm))
        px += int(sel.sum())
    res = dict(proj_plain=None, proj_cand=None, ratio=None, gate=gate, passed=None, px=px, na_reason=None)
    if px < MIN_PX:
        res["na_reason"] = "only %d region pixels away from edges (< %d)" % (px, MIN_PX)
    elif den <= 1e-9:
        res["na_reason"] = "master has no fine band in the region"
    elif num_p <= 0:
        res["na_reason"] = "plain keeps none of the master texture (ratio undefined)"
    else:
        res.update(proj_plain=num_p / den, proj_cand=num_c / den, ratio=num_c / num_p)
        res["passed"] = bool(res["ratio"] >= gate)
    res["pass"] = res.pop("passed")
    return res
