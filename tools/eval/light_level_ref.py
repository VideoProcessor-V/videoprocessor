# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Python mirror of the light-level LUT (per 10-bit code, float64, deterministic, single-threaded).

Mirrors the C++ LightLevel::Build (src/VP-V/modules/input/source_analysis/light_level/LightLevelLut.h);
tests/LightLevelCrossCheckTests.cpp compares the two to 1e-9 on J using vectors from gen_light_level_golden.py.

Public API
  Context(kind, ...)         dataclass; kind in {"sdr", "hdr_passthrough", "hdr_to_sdr"}; to_dict()/from_dict()
  build_lut(ctx)             -> dict J, S, V, L, sdr_code, sout : float64 arrays of length 1024 (index = 10-bit code)
  fetch(lut, code10)         -> dict of the same fields at (array of) float codes; index clamp to [0,1023], linear interp
  j_eff(plane_codes10, ctx, tile=None, lut=None) -> float64 map, same shape as the plane:
                                0.5 * J(lowpass(code)) + 0.5 * tile_mean(J(lowpass(code)))
  tile_size_for(h)           -> max(1, round(h / 68))   (about 32 px at 2160p; no fixed pixel size)
  lowpass(plane)             -> separable binomial [1 4 6 4 1]/16, reflect borders (documented, fixed 5 taps)

Settings (C++ Settings keys): target_white = sdr_target_nits (203), target_black = sdr_black_nits (0),
eotf/gamma = hdr_tone_map_target_gamma (bt1886 | srgb | power, power = gamma), display_white/display_black =
light_level_display_white_nits/_black_nits (None = auto = the target), ambient = light_level_ambient_nits (0),
weber = light_level_weber (0.02).  Context (C++ Context): kind, range (limited | full), src_peak (0 = unknown ->
10000), src_min, display_peak (0 = unknown).  Dynamic peak is not modelled.  The SDR tone shape is off.
Compatibility: the former fields white/black are now target_white/target_black (the old white also served as the
HDR display peak: now display_peak, and the old black as the display black: now display_black).

Per code c: v = (c-64)/876 (limited) or c/1023 (full).  A continuous analytic map L(x) is evaluated at float codes.
  TargetDisplay  = the encoding display (target white/black, eotf); PhysicalDisplay = the same with display white/black.
  Display(v)     nits capped at white: bt1886 with black (g=2.4, a=(Lw^(1/g)-Lb^(1/g))^g, b=Lb^(1/g)/(Lw^(1/g)-Lb^(1/g)),
                 L=a*max(v+b,0)^g), power: Lb+(Lw-Lb)v^g, srgb: Lb+(Lw-Lb)*exact piecewise lin.  Display.Inverse = exact inverse.
  v < 0 (BTB)    L = physical display black, J = PQ_OETF(physical display black), S = V = 0, sdr_code = the input
                 code (any kind).
  sdr            L = PhysicalDisplay(v)
  hdr_passthrough L = max(EETF(PQ_EOTF(v)), physical black); BT.2390 EETF in PQ space, source [src_min, src_peak]
                 -> [physical black, display peak]; identity when the display peak is unknown or >= src peak.
                 (e1 and the Hermite parameter t are clipped to [0, 1]; the C++ must match.)
  hdr_to_sdr     L_out = PQ_EOTF(SplineTM(v)) (src peak, src_min, dst peak = target white, dst min = target black);
                 signal = TargetDisplay.Inverse(clamp(L_out, target black, target white));
                 sdr_code = 64 + 876*signal; L = PhysicalDisplay(signal).
J = PQ OETF(L) in [0, 1].  S = (J(c+.5) - J(c-.5)) / span, one-sided at the ends (lo = c for code 0, hi = c for
code 1023; span = hi - lo).  V = |L(hi) - L(lo)| / span / (weber * max(L(c) + ambient, lowest_step)); L(c) already
includes the physical display black (it is not added again), lowest_step = PQ_EOTF(1/1023) nit (V < 1: below the Weber threshold).
sdr_code: input code except hdr_to_sdr (tone-mapped SDR code).
sout = (sdr_code(hi) - sdr_code(lo)) / span, the same +-0.5 central difference as S, for EVERY code incl. BTB (sdr_code is the
input code there): 1 exactly for sdr / hdr_passthrough; hdr_to_sdr BTB codes 1, the knee code 64 mixed.
"""
from dataclasses import asdict, dataclass
from typing import Optional

import numpy as np
from scipy import ndimage

import tonemap_ref as tr

NCODES = 1024


@dataclass
class Context:
    kind: str = "sdr"
    range: str = "limited"          # limited | full
    target_white: float = 203.0     # sdr_target_nits (encoding white)
    target_black: float = 0.0       # sdr_black_nits
    eotf: str = "bt1886"            # bt1886 | srgb | power
    gamma: float = 2.4              # power only
    display_white: Optional[float] = None   # physical display white; None = target_white
    display_black: Optional[float] = None   # physical display black; None = target_black
    ambient: float = 0.0            # reflected ambient (nit), added to L in the V denominator only
    weber: float = 0.02
    src_peak: float = 0.0           # HDR source mastering peak (nit); 0 = unknown = 10000
    src_min: float = 0.0            # HDR source mastering black (nit)
    display_peak: float = 0.0       # hdr_passthrough display peak (nit); 0 = unknown, no roll-off

    def to_dict(self):
        return asdict(self)

    @staticmethod
    def from_dict(d):
        return Context(**d)

    @property
    def phys_white(self):
        return self.target_white if self.display_white is None else self.display_white

    @property
    def phys_black(self):
        return self.target_black if self.display_black is None else self.display_black

    @property
    def peak(self):
        return self.src_peak if self.src_peak > 0 else 10000.0


def _v_of_code(c, rng):
    c = np.asarray(c, np.float64)
    return (c - 64.0) / 876.0 if rng == "limited" else c / 1023.0


def _srgb_lin(v):
    v = np.maximum(v, 0.0)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def _srgb_inv(lin):
    lin = np.clip(lin, 0.0, 1.0)
    return np.where(lin <= 0.0031308, 12.92 * lin, 1.055 * lin ** (1 / 2.4) - 0.055)


def _display(v, ctx, Lw, Lb):
    """Display EOTF: signal v (0 black, 1 white, may exceed 1) -> nits, capped at white."""
    v = np.asarray(v, np.float64)
    if ctx.eotf == "bt1886":
        g = 2.4
        d = Lw ** (1 / g) - Lb ** (1 / g)
        L = d ** g * np.maximum(v + Lb ** (1 / g) / d, 0.0) ** g
    elif ctx.eotf == "power":
        L = Lb + (Lw - Lb) * np.maximum(v, 0.0) ** ctx.gamma
    elif ctx.eotf == "srgb":
        L = Lb + (Lw - Lb) * _srgb_lin(v)
    else:
        raise ValueError("eotf %r" % ctx.eotf)
    return np.minimum(L, Lw)


def _display_inverse(L, ctx, Lw, Lb):
    L = np.asarray(L, np.float64)
    if ctx.eotf == "bt1886":
        g = 2.4
        d = Lw ** (1 / g) - Lb ** (1 / g)
        return (np.maximum(L, 0.0) / d ** g) ** (1 / g) - Lb ** (1 / g) / d
    if ctx.eotf == "power":
        return (np.maximum(L - Lb, 0.0) / (Lw - Lb)) ** (1 / ctx.gamma)
    if ctx.eotf == "srgb":
        return _srgb_inv((L - Lb) / (Lw - Lb))
    raise ValueError("eotf %r" % ctx.eotf)


def _physical(v, ctx):
    return _display(v, ctx, ctx.phys_white, ctx.phys_black)


def _eetf(nits, ctx):
    """BT.2390 EETF in PQ space: source [src_min, src_peak] -> [physical black, display peak]; identity when off."""
    peak = ctx.peak
    lo, hi = float(tr.pq_oetf(ctx.src_min)), float(tr.pq_oetf(peak))
    rng = hi - lo
    if not (ctx.display_peak > 0.0) or not (rng > 0.0) or ctx.display_peak >= peak:
        return nits
    max_t = (float(tr.pq_oetf(ctx.display_peak)) - lo) / rng
    min_t = (float(tr.pq_oetf(ctx.phys_black)) - lo) / rng
    ks = 1.5 * max_t - 0.5
    e1 = np.clip((tr.pq_oetf(nits) - lo) / rng, 0.0, 1.0)
    t = np.clip((e1 - ks) / (1.0 - ks), 0.0, 1.0)
    t2, t3 = t * t, t * t * t
    p = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_t
    e2 = np.where(e1 >= ks, p, e1)
    e3 = e2 + min_t * (1.0 - e2) ** 4
    return np.minimum(tr.pq_eotf(e3 * rng + lo), ctx.display_peak)


def _spline(ctx):
    return tr.SplineTM(ctx.peak, dst_peak=ctx.target_white, src_min=ctx.src_min, dst_min=ctx.target_black)


def _signal_tm(v, ctx, sp):
    """hdr_to_sdr: tone-mapped SDR signal (0..1) for the PQ signal v."""
    l_out = np.clip(tr.pq_eotf(sp(v)), ctx.target_black, ctx.target_white)
    return _display_inverse(l_out, ctx, ctx.target_white, ctx.target_black)


def _luminance(codes, ctx, spline=None):
    """Analytic luminance (nit), sdr code and BTB mask at float 10-bit codes (BTB -> physical black)."""
    codes = np.asarray(codes, np.float64)
    v = _v_of_code(codes, ctx.range)
    btb = v < 0.0
    sdr_code = codes.copy()
    if ctx.kind == "sdr":
        L = _physical(v, ctx)
    elif ctx.kind == "hdr_passthrough":
        L = np.maximum(_eetf(tr.pq_eotf(np.clip(v, 0.0, 1.0)), ctx), ctx.phys_black)
    elif ctx.kind == "hdr_to_sdr":
        sig = _signal_tm(np.clip(v, 0.0, 1.0), ctx, spline or _spline(ctx))
        sdr_code = np.where(btb, codes, 64.0 + 876.0 * sig)
        L = _physical(sig, ctx)
    else:
        raise ValueError("kind %r" % ctx.kind)
    return np.where(btb, ctx.phys_black, L), sdr_code, btb


def build_lut(ctx):
    c = np.arange(NCODES, dtype=np.float64)
    sp = _spline(ctx) if ctx.kind == "hdr_to_sdr" else None
    L, sdr_code, btb = _luminance(c, ctx, sp)
    lo = np.where(c == 0, c, c - 0.5)
    hi = np.where(c == NCODES - 1, c, c + 0.5)
    Ll, sdr_lo, _ = _luminance(lo, ctx, sp)
    Lh, sdr_hi, _ = _luminance(hi, ctx, sp)
    span = hi - lo
    J = tr.pq_oetf(L)
    sout = (sdr_hi - sdr_lo) / span
    S = (tr.pq_oetf(Lh) - tr.pq_oetf(Ll)) / span
    lowest_step = float(tr.pq_eotf(1.0 / 1023.0))
    V = np.abs(Lh - Ll) / span / (ctx.weber * np.maximum(L + ctx.ambient, lowest_step))
    J = np.where(btb, float(tr.pq_oetf(ctx.phys_black)), J)
    S, V = (np.where(btb, 0.0, a) for a in (S, V))
    return dict(J=J, S=S, V=V, L=L, sdr_code=sdr_code, sout=sout)


def fetch(lut, code10):
    x = np.clip(np.asarray(code10, np.float64), 0.0, NCODES - 1.0)
    i0 = np.minimum(np.floor(x).astype(np.int64), NCODES - 2)
    f = x - i0
    return {k: lut[k][i0] * (1 - f) + lut[k][i0 + 1] * f for k in ("J", "S", "V", "L", "sdr_code", "sout")}


def tile_size_for(h):
    return max(1, int(round(h / 68.0)))


_K5 = np.array([1, 4, 6, 4, 1], np.float64) / 16.0


def lowpass(a):
    a = np.asarray(a, np.float64)
    return ndimage.correlate1d(ndimage.correlate1d(a, _K5, axis=0, mode="reflect"), _K5, axis=1, mode="reflect")


def tile_edges(n, t):
    """Tile boundaries covering n px with ~t px tiles (remainder spread evenly over all tiles)."""
    k = max(1, int(round(n / float(t))))
    return np.rint(np.linspace(0, n, k + 1)).astype(np.int64)


def tile_mean(a, tile):
    out = np.empty_like(a)
    ys, xs = tile_edges(a.shape[0], tile), tile_edges(a.shape[1], tile)
    for y0, y1 in zip(ys[:-1], ys[1:]):
        for x0, x1 in zip(xs[:-1], xs[1:]):
            out[y0:y1, x0:x1] = a[y0:y1, x0:x1].mean()
    return out


def j_eff(plane_codes10, ctx, tile=None, lut=None):
    p = np.asarray(plane_codes10, np.float64)
    lut = lut or build_lut(ctx)
    jp = fetch(lut, lowpass(p))["J"]
    return 0.5 * jp + 0.5 * tile_mean(jp, tile or tile_size_for(p.shape[0]))
