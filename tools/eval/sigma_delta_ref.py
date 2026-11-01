# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""numpy port of src/VP-V/modules/output/dither/SigmaDeltaDomain.h.

Arithmetic mirrors the C++ exactly: Limits / ResidualBound / QuantiseStep are
C++ double (numpy float64, same operation order); Ign is C++ float (numpy
float32 scalars, same operation order, no promotion to double).
"""
import math

import numpy as np

F32 = np.float32


def limits(color_bits, full):
    """SigmaDeltaDomain::Limits -> (min, max) as float codes."""
    bits = min(max(int(color_bits), 8), 16)
    unit = math.ldexp(1.0, bits - 8)
    lo = 0.0 if full else unit
    hi = math.ldexp(1.0, bits) - 1.0 if full else 255.0 * unit - 1.0
    return lo, hi


def residual_bound(amplitude):
    return 0.5 + max(0.0, float(amplitude))


def quantise_step(x, r, d, lim, amplitude):
    """One step, float64 arrays/scalars. Returns (k, r_new)."""
    x = np.asarray(x, dtype=np.float64)
    r = np.asarray(r, dtype=np.float64)
    d = np.asarray(d, dtype=np.float64)
    k = np.minimum(np.maximum(np.floor(x + r + d + 0.5), lim[0]), lim[1])
    b = residual_bound(amplitude)
    return k, np.minimum(np.maximum(x + r - k, -b), b)


def ign(px, py, frame):
    """SigmaDeltaDomain::Ign in float32 (scalars or arrays); returns float32."""
    px = np.asarray(px, dtype=F32)
    py = np.asarray(py, dtype=F32)
    shift = F32(5.588238) * F32(frame & 63)
    px = px + shift
    py = py + shift
    inner = F32(0.06711056) * px + F32(0.00583715) * py
    v = F32(52.9829189) * (inner - np.floor(inner))
    return (v - np.floor(v)).astype(F32)


def dither(px, py, frame, amplitude):
    """(Ign - 0.5) * 2 * amplitude, evaluated in double like the C++ test."""
    return (ign(px, py, frame).astype(np.float64) - 0.5) * 2.0 * float(amplitude)


def quantise_sequence(frames_codes_float, bits=8, full=False, amplitude=0.15,
                      return_residual=False):
    """frames: (T,H,W) or (T,...) float output codes. Per-pixel residual is
    carried across frames; dither uses pixel (x,y)=(col,row) of the last two
    axes (1-D/scalar frames use py=0). Returns integer codes (int32)."""
    f = np.asarray(frames_codes_float, dtype=np.float64)
    lim = limits(bits, full)
    shp = f.shape[1:]
    if len(shp) >= 2:
        yy, xx = np.indices(shp[-2:])
        px = np.broadcast_to(xx, shp)
        py = np.broadcast_to(yy, shp)
    else:
        px = np.arange(int(np.prod(shp)) or 1).reshape(shp) if shp else 0
        py = np.zeros_like(px)
    r = np.zeros(shp, dtype=np.float64)
    out = np.empty(f.shape, dtype=np.int32)
    for t in range(f.shape[0]):
        d = dither(px, py, t, amplitude)
        k, r = quantise_step(f[t], r, d, lim, amplitude)
        out[t] = k.astype(np.int32)
    return (out, r) if return_residual else out


def quantise_round(frames_codes_float, bits=8, full=False):
    """No-dither, no-feedback rounding to the same code limits."""
    lim = limits(bits, full)
    f = np.asarray(frames_codes_float, dtype=np.float64)
    return np.minimum(np.maximum(np.floor(f + 0.5), lim[0]), lim[1]).astype(np.int32)
