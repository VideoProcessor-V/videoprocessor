# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Python mirror of shared unit U2 (VpUnit::BandFromBase, repair/grain_split/shared/BandFromBase.h).

Same law, same summation order as the C++ reference; pinned by the generated golden
(tools/eval/units/gen_band_from_base_golden.py -> repair/grain_split/tests/BandFromBaseGolden.generated.h).
"""
import math

UNIT_VERSION = 1


def radius(sigma, factor=3.0, min_one=True, eps=0.0):
    r = int(math.ceil(factor * sigma - eps))
    return max(1, r) if min_one else r


def weight(k, sigma, r):
    s = sigma
    total = 0.0
    for i in range(-r, r + 1):
        total += math.exp(-0.5 * i * i / (s * s))
    return math.exp(-0.5 * k * k / (s * s)) / total


def half_sigma(sigma):
    return math.sqrt(sigma * sigma - 0.25) / 2.0


def blur(plane, w, h, sigma):
    """Separable Gaussian, edges replicated, rows across then columns down; plane is a flat list."""
    if not sigma > 0.0:
        return list(plane)
    r = radius(sigma)
    k = []
    total = 0.0
    for i in range(-r, r + 1):
        v = math.exp(-0.5 * i * i / (sigma * sigma))
        total += v
        k.append(v)
    k = [x / total for x in k]

    def get(x, y):
        return plane[min(h - 1, max(0, y)) * w + min(w - 1, max(0, x))]

    t = [0.0] * (w * h)
    for y in range(h):
        pad = [get(x, y) for x in range(-r, w + r)]
        for x in range(w):
            a = 0.0
            for i in range(-r, r + 1):
                a += k[i + r] * pad[x + r + i]
            t[y * w + x] = a
    o = [0.0] * (w * h)
    for y in range(h):
        for i in range(-r, r + 1):
            sy = min(h - 1, max(0, y + i))
            kk = k[i + r]
            for x in range(w):
                o[y * w + x] += kk * t[sy * w + x]
    return o
