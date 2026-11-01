# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""WP7 (plan 47 s4): grain-safe, regional, light-aware CONTRAST RECOVERY, Python CPU model (luma, PQ 10-bit codes in
-> SDR 10-bit codes out, 100 nit), plus the new acceptance metrics.

Model (ContrastRecovery):
  S, G = tonemap_ref.grain_split(clip(e))             shared clean base / grain layer (grain_split unit)
  low  = gauss(S, ref_h/240)                          the adaptation level (3 px at 720p; libplacebo's 1.75 px is too fine to match its restoration)
  Dc   = S - low                                      clean detail band (no grain); the boosted increment is soft-limited (halo cap)
  g_full = clip(1 / slope_TM(low), 1, gmax)           gain that would return the curve's lost slope exactly
                                                      (out detail amplitude = slope * g <= source: boost <= source)
  s(b) = s_max * k_light(J_eff) * band_gate * (1 - halo_guard)        per pixel, reduced to tiles, smoothed in time
  g    = 1 + s * (g_full - 1)
  mode 'A' (pre-TM, source resolution, after deband):  p = TM(ec) + [TM(S + soft_limit((g-1) Dc)) - TM(S)]
  mode 'B' (post-TM output hook):                      p = TM(ec) + soft_limit((g-1) * (TM(S) - gauss(TM(S), sigma)))
  s = 0  ->  p = TM(ec) exactly (off = plain curve).  The grain G rides the curve exactly as in the plain path.
Light dependence: k_light = smoothstep(J_lo, J_hi, J_eff) on the display-referred level after tone mapping (the
light-level LUT, context hdr_to_sdr): 0 below ~L_floor (amplification there lifts noise / codec error), 1 above.
Get-out-of-the-way: where slope(low) ~ 1 (no loss) g_full = 1 and nothing changes; tiles whose loss is below
need_eps are skipped in a GPU port.

New metrics (unit-tested in test_cr_wp7.py): zone_grain_keep, step_visibility_ratio, cr_acceptance.
"""
from dataclasses import dataclass, replace

import numpy as np
from scipy import ndimage

import eval_metrics as em
import light_level_ref as llr
import tonemap_ref as tr


def smoothstep(e0, e1, x):
    t = np.clip((np.asarray(x, np.float64) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


@dataclass
class CRParams:
    s_max: float = 0.25
    sigma_rel: float = 1.0 / 240.0   # adaptation low-pass sigma = sigma_rel * picture height (3 px at 720p; 1.75 px = libplacebo's is too fine)
    detail_rel: float = 0.0     # >0: boost only gauss(S, detail_rel*h) - low (detail above the grain band; 1/720 = 1 px at 720p)
    gmax: float = 20.0          # cap on g_full (A is ineffective where the curve is nearly flat; the cap only guards 1/0)
    d_cap: float = 0.10         # soft limit of the boosted detail increment, normalised PQ (88 codes): halo cap
    j_lo_nit: float = 0.1       # k_light ramps over display luminance [j_lo, j_hi] after tone mapping
    j_hi_nit: float = 1.0
    halo_r0: float = 0.15       # halo guard: local range (normalised PQ) where the gain starts to fall ...
    halo_r1: float = 0.40       # ... and where it is zero
    band_lo: float = 1.0        # band gate: local detail rms (10-bit codes) below which nothing is boosted ...
    band_hi: float = 4.0        # ... and above which it is fully boosted (steps deband removed stay removed)
    use_light: bool = True
    use_band: bool = True
    use_halo: bool = True
    ema: float = 0.35           # region_smooth: per-frame blend toward the target ...
    rate_limit: float = 0.08    # ... limited to this strength change per frame
    cut_thr: float = 0.06       # mean abs frame change (normalised) that resets the smoothing


def soft_limit(d, c):
    return c * np.tanh(d / c)


def tile_reduce(a, tile):
    """Mean per ~tile px tile (light_level_ref.tile_edges grid) -> small array."""
    ys, xs = llr.tile_edges(a.shape[0], tile), llr.tile_edges(a.shape[1], tile)
    r = np.add.reduceat(np.add.reduceat(a, ys[:-1], axis=0), xs[:-1], axis=1)
    return r / np.outer(np.diff(ys), np.diff(xs))


def tile_upsample(t, shape):
    """Bilinear, edge-clamped, back to the full raster (what a GPU would sample)."""
    ty = np.interp((np.arange(shape[0]) + 0.5) * t.shape[0] / shape[0] - 0.5, np.arange(t.shape[0]), np.arange(t.shape[0]))
    tx = np.interp((np.arange(shape[1]) + 0.5) * t.shape[1] / shape[1] - 0.5, np.arange(t.shape[1]), np.arange(t.shape[1]))
    rows = np.stack([np.interp(ty, np.arange(t.shape[0]), t[:, j]) for j in range(t.shape[1])], axis=1)
    return np.stack([np.interp(tx, np.arange(t.shape[1]), rows[i]) for i in range(rows.shape[0])], axis=0)


class ContrastRecovery:
    """Stateful per-sequence model; mode 'A' | 'B'.  Same process(Ycode)->SDR code interface as tonemap_ref.ToneMapper."""

    def __init__(self, mode, src_peak, ref_h, dst_peak=100.0, params=None):
        assert mode in ("A", "B")
        self.mode, self.src_peak, self.ref_h, self.dst_peak = mode, float(src_peak), ref_h, dst_peak
        self.p = params or CRParams()
        self.tm = tr.SplineTM(src_peak, dst_peak)
        self.exc = tr.Excursion(self.tm, dst_peak)
        self.ctx = llr.Context(kind="hdr_to_sdr", src_peak=self.src_peak, target_white=dst_peak, eotf="power",
                               gamma=tr.SDR_GAMMA)
        self.lut = llr.build_lut(self.ctx)
        self.reset()

    def reset(self):
        self.s_prev = None
        self.e_prev = None
        self.last_strength = None

    # ---- per-pixel evidence -> strength map ------------------------------------------------------------
    def strength_target(self, Ycode, S, low, Dc):
        p = self.p
        k = np.ones_like(S)
        if p.use_light:
            J = llr.j_eff(Ycode, self.ctx, lut=self.lut)
            k = smoothstep(float(tr.pq_oetf(p.j_lo_nit)), float(tr.pq_oetf(p.j_hi_nit)), J)
        w = np.ones_like(S)
        if p.use_band:
            lr = np.sqrt(tr.gauss(Dc * Dc, 4.0)) * 876.0
            w = np.clip((lr - p.band_lo) / (p.band_hi - p.band_lo), 0.0, 1.0)
        hg = np.zeros_like(S)
        if p.use_halo:
            win = max(3, 2 * int(round(min(S.shape) / 240.0)) + 1)
            rng = ndimage.maximum_filter(S, size=win, mode="nearest") - ndimage.minimum_filter(S, size=win, mode="nearest")
            hg = smoothstep(p.halo_r0, p.halo_r1, ndimage.gaussian_filter(rng, win / 2.0, mode="nearest"))
        return p.s_max * k * w * (1.0 - hg)

    def smooth_strength(self, target, e):
        """region_smooth on tile strengths: EMA + rate limit; reset at discontinuities."""
        p = self.p
        tile = llr.tile_size_for(target.shape[0])
        tt = tile_reduce(target, tile)
        cut = False
        if self.e_prev is not None:
            cut = float(np.mean(np.abs(e - self.e_prev))) > p.cut_thr
        if self.s_prev is None or cut:
            cur = tt
        else:
            cur = self.s_prev + np.clip(p.ema * (tt - self.s_prev), -p.rate_limit, p.rate_limit)
        self.s_prev, self.e_prev = cur, e
        return tile_upsample(cur, target.shape)

    # ---- main ------------------------------------------------------------------------------------------
    def process(self, Ycode):
        p, tm = self.p, self.tm
        e = tr.code_to_norm(Ycode)
        ec, ex = self.exc.split(e)
        i1 = np.clip(e, 0.0, 1.0)
        S, G = tr.grain_split(i1)
        low = tr.gauss(S, p.sigma_rel * self.ref_h)
        Dc = (tr.gauss(S, p.detail_rel * self.ref_h) if p.detail_rel > 0 else S) - low
        s = self.smooth_strength(self.strength_target(Ycode, S, low, Dc), e)
        self.last_strength = s
        gfull = np.clip(1.0 / np.maximum(tm.slope(low), 1e-3), 1.0, p.gmax)
        g = 1.0 + s * (gfull - 1.0)
        base = tm(ec)
        if self.mode == "A":
            Sb = S + soft_limit((g - 1.0) * Dc, p.d_cap)
            out = base + (tm(Sb) - tm(S))
        else:
            q = tm(S)
            Dq = (tr.gauss(q, p.detail_rel * self.ref_h) if p.detail_rel > 0 else q) - tr.gauss(q, p.sigma_rel * self.ref_h)
            out = base + soft_limit((g - 1.0) * Dq, p.d_cap)
        out = np.clip(out, tm.out_min, tm.out_max)
        return self.exc.apply(tr.pq_out_to_sdr_code(out, self.dst_peak), ex)


# ---------------------------------------------------------------------------------------------- new metrics

def zone_grain_keep(d_in, d_out_cand, d_out_plain, masks_edge, masks_all=None):
    """Two-seed probe grain kept by a candidate RELATIVE TO THE PLAIN CURVE, overall and in the near-edge zone.
    d_* = out_A - out_B (T,H,W).  keep_all = rms(cand)/rms(plain) over masks_all (default whole frame),
    keep_edge the same over the near-edge masks.  Acceptance: keep_all in [0.95, 1.05], keep_edge >= 0.90.
    d_in is unused except for shape/validation (the plain curve is the reference, not the input)."""
    dc, dp = np.asarray(d_out_cand, np.float64), np.asarray(d_out_plain, np.float64)
    assert dc.shape == dp.shape == np.asarray(d_in).shape
    allm = np.ones(dc.shape, bool) if masks_all is None else np.stack(masks_all)
    edge = np.stack(masks_edge)

    def r(m):
        den = float(np.sum(dp[m] ** 2))
        return float(np.sqrt(np.sum(dc[m] ** 2) / den)) if m.any() and den > 0 else None
    return dict(keep_all=r(allm), keep_edge=r(edge))


def step_visibility_ratio(vis_cand, vis_plain):
    """Mean masked step visibility (texture_dither.V, smooth tiles) of a candidate / the plain curve's.  > 1.02
    fails (deband steps made more visible)."""
    if vis_cand is None or vis_plain is None or vis_plain <= 0:
        return None
    return float(vis_cand) / float(vis_plain)


GATES = dict(restore_vs_cr=0.9, grain_lo=0.95, grain_hi=1.05, grain_edge=0.90, vis_ratio=1.02, pump=0.5)


def cr_acceptance(cand, cr, gates=GATES):
    """Plan 47 WP7 acceptance for one candidate against libplacebo CR 0.3.  cand / cr are dicts with restore,
    boost, halo, keep_all, keep_edge, pump, vis_ratio (vis_ratio None for cr is fine).  Returns {criterion: bool|None}
    plus 'all' (None entries ignored; False if any False)."""
    def ok(c, v):
        return None if v is None else bool(c)
    r = {
        "restore": ok(cand["restore"] is not None and cr["restore"] is not None and
                      cand["restore"] >= gates["restore_vs_cr"] * cr["restore"], cand["restore"] if cr["restore"] is not None else None),
        "grain": ok(cand["keep_all"] is not None and gates["grain_lo"] <= cand["keep_all"] <= gates["grain_hi"], cand["keep_all"]),
        "grain_edge": ok(cand["keep_edge"] is not None and cand["keep_edge"] >= gates["grain_edge"], cand["keep_edge"]),
        "boost": ok(cand["boost"] is not None and cand["boost"] <= max(cr["boost"], 0.0) + 1e-9, cand["boost"]) if cr["boost"] is not None else None,
        "halo": ok(cand["halo"] is not None and cand["halo"] <= cr["halo"] + 1e-12, cand["halo"]) if cr["halo"] is not None else None,
        "step_vis": ok(cand["vis_ratio"] is not None and cand["vis_ratio"] <= gates["vis_ratio"], cand["vis_ratio"]),
        "pump": ok(cand["pump"] is not None and cand["pump"] < gates["pump"], cand["pump"]),
    }
    vals = [v for v in r.values() if v is not None]
    r["all"] = bool(vals) and all(vals)
    return r
