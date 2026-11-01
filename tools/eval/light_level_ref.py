# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Python mirror of the light-level LUT (per 10-bit code, float64, deterministic, single-threaded).

Public API
  Context(kind, ...)         dataclass; kind in {"sdr", "hdr_passthrough", "hdr_to_sdr"}; to_dict()/from_dict()
  build_lut(ctx)             -> dict J, S, V, L, sdr_code : float64 arrays of length 1024 (index = 10-bit code)
  fetch(lut, code10)         -> dict of the same fields at (array of) float codes; index clamp to [0,1023], linear interp
  j_eff(plane_codes10, ctx, tile=None, lut=None) -> float64 map, same shape as the plane:
                                0.5 * J(lowpass(code)) + 0.5 * tile_mean(J(lowpass(code)))
  tile_size_for(h)           -> max(1, round(h / 68))   (about 32 px at 2160p; no fixed pixel size)
  lowpass(plane)             -> separable binomial [1 4 6 4 1]/16, reflect borders (documented, fixed 5 taps)

Per code c: v = (c-64)/876 (limited) or c/1023 (full); v < 0 (BTB) -> J = S = V = 0 (L reported as 0).
  sdr             display EOTF at white Lw, black Lb.  eotf="bt1886": g=2.4, a=(Lw^(1/g)-Lb^(1/g))^g,
                  b=Lb^(1/g)/(Lw^(1/g)-Lb^(1/g)), L=a*max(v+b,0)^g;  eotf="power": L=Lb+(Lw-Lb)*v^g.  L capped at Lw.
  hdr_passthrough PQ EOTF of the signal, then the BT.2390 EETF (src min/peak -> display Lb/Lw, Lw = display peak).
  hdr_to_sdr      tonemap_ref.SplineTM(src_peak, dst_peak=Lw, src_min, dst_min=Lb) on the PQ signal; SDR code
                  = 64 + 876 * (L_out / Lw)^(1/2.4); then the SDR display EOTF above (BT.1886 black lift applies).
J = PQ OETF(L) in [0, 1].  S = dJ/dcode (central difference, +-0.5 code; BTB evaluates at v = 0).
V = |L(c+.5) - L(c-.5)| / (weber * (L(c) + L_floor)),  weber default 0.02,  L_floor = Lb + ambient
(V < 1: the step is below the Weber threshold; denominator floored at 1e-9 nit).  All nit values are Context fields; the dataclass defaults
(white 100, black 0.05, ambient 0, source peak 1000, source min 0) are documented defaults only.
sdr_code: identity-scale code for sdr, tone-mapped code for hdr_to_sdr, 0 for hdr_passthrough (none exists).
"""
from dataclasses import asdict, dataclass

import numpy as np
from scipy import ndimage

import tonemap_ref as tr

NCODES = 1024


@dataclass
class Context:
    kind: str = "sdr"
    range: str = "limited"          # limited | full
    white: float = 100.0            # Lw: SDR display white / display peak (nit)
    black: float = 0.05             # Lb: display black (nit)
    ambient: float = 0.0            # reflected ambient (nit), enters L_floor only
    weber: float = 0.02
    eotf: str = "bt1886"            # sdr only: bt1886 | power
    gamma: float = 2.4
    src_peak: float = 1000.0        # HDR source mastering peak (nit)
    src_min: float = 0.0            # HDR source mastering black (nit)

    def to_dict(self):
        return asdict(self)

    @staticmethod
    def from_dict(d):
        return Context(**d)

    @property
    def l_floor(self):
        return self.black + self.ambient


def _v_of_code(c, rng):
    c = np.asarray(c, np.float64)
    return (c - 64.0) / 876.0 if rng == "limited" else c / 1023.0


def _sdr_eotf(v, ctx):
    v = np.clip(v, 0.0, None)
    Lw, Lb, g = ctx.white, ctx.black, ctx.gamma
    if ctx.eotf == "bt1886":
        d = Lw ** (1 / g) - Lb ** (1 / g)
        a = d ** g
        b = Lb ** (1 / g) / d
        L = a * np.maximum(v + b, 0.0) ** g
    elif ctx.eotf == "power":
        L = Lb + (Lw - Lb) * v ** g
    else:
        raise ValueError("eotf %r" % ctx.eotf)
    return np.minimum(L, Lw)


def _bt2390(e, ctx):
    """BT.2390 EETF on the PQ signal e (0..1): source [src_min, src_peak] -> display [black, white]."""
    smin, smax = float(tr.pq_oetf(ctx.src_min)), float(tr.pq_oetf(ctx.src_peak))
    dmin, dmax = float(tr.pq_oetf(ctx.black)), float(tr.pq_oetf(ctx.white))
    span = smax - smin
    e1 = np.clip((e - smin) / span, 0.0, 1.0)
    min_l, max_l = (dmin - smin) / span, (dmax - smin) / span
    if max_l >= 1.0:
        e2 = e1
    else:
        ks = 1.5 * max_l - 0.5
        t = np.clip((e1 - ks) / (1.0 - ks), 0.0, 1.0)
        p = ((2 * t ** 3 - 3 * t ** 2 + 1) * ks + (t ** 3 - 2 * t ** 2 + t) * (1 - ks)
             + (-2 * t ** 3 + 3 * t ** 2) * max_l)
        e2 = np.where(e1 < ks, e1, p)
    e3 = e2 + min_l * (1.0 - e2) ** 4
    return np.minimum(tr.pq_eotf(e3 * span + smin), ctx.white)


def _spline(ctx):
    return tr.SplineTM(ctx.src_peak, dst_peak=ctx.white, src_min=ctx.src_min, dst_min=ctx.black)


def _luminance(codes, ctx, spline=None):
    """Display luminance (nit) at float 10-bit codes; v < 0 evaluates as v = 0 (callers zero the BTB themselves)."""
    v = _v_of_code(codes, ctx.range)
    v = np.clip(v, 0.0, None if ctx.kind == "sdr" else 1.0)
    if ctx.kind == "sdr":
        return _sdr_eotf(v, ctx)
    if ctx.kind == "hdr_passthrough":
        return _bt2390(v, ctx)
    if ctx.kind == "hdr_to_sdr":
        return _sdr_eotf((_tm_code(v, ctx, spline or _spline(ctx)) - 64.0) / 876.0, ctx)
    raise ValueError("kind %r" % ctx.kind)


def _tm_code(v, ctx, sp):
    l_out = tr.pq_eotf(sp(v))
    return 64.0 + 876.0 * np.clip(l_out / ctx.white, 0.0, 1.0) ** (1 / 2.4)


def build_lut(ctx):
    c = np.arange(NCODES, dtype=np.float64)
    sp = _spline(ctx) if ctx.kind == "hdr_to_sdr" else None
    L = _luminance(c, ctx, sp)
    Lp, Lm = _luminance(c + 0.5, ctx, sp), _luminance(c - 0.5, ctx, sp)
    J = tr.pq_oetf(L)
    S = tr.pq_oetf(Lp) - tr.pq_oetf(Lm)
    V = np.abs(Lp - Lm) / (ctx.weber * np.maximum(L + ctx.l_floor, 1e-9))
    v = np.clip(_v_of_code(c, ctx.range), 0.0, 1.0)
    if ctx.kind == "sdr":
        sdr_code = 64.0 + 876.0 * v
    elif ctx.kind == "hdr_to_sdr":
        sdr_code = _tm_code(v, ctx, sp)
    else:
        sdr_code = np.zeros_like(v)
    btb = _v_of_code(c, ctx.range) < 0
    J, S, V, L = (np.where(btb, 0.0, a) for a in (J, S, V, L))
    return dict(J=J, S=S, V=V, L=L, sdr_code=sdr_code)


def fetch(lut, code10):
    x = np.clip(np.asarray(code10, np.float64), 0.0, NCODES - 1.0)
    i0 = np.minimum(np.floor(x).astype(np.int64), NCODES - 2)
    f = x - i0
    return {k: lut[k][i0] * (1 - f) + lut[k][i0 + 1] * f for k in ("J", "S", "V", "L", "sdr_code")}


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
