# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Expected output codes for a named colour chain.

Path: codes -> normalised Y'CbCr -> R'G'B' -> linear light in cd/m2 (EOTF) ->
primaries conversion (3x3 computed from chromaticities, D65 white) -> target
inverse EOTF -> normalised Y'CbCr -> codes.

Conventions
  * Every transfer function is extended as an odd function (sign preserving),
    so BTB (below black) and WTW (whiter than white) codes survive unclipped
    when the chain keeps them (``keeps_btb_wtw``). bt1886 is odd about V = -b.
  * Rounding is round half to even (numpy rint), the same for every component.
  * No tone mapping and no gamut mapping: an SDR source shown in PQ/HLG sits at
    ``ref_white`` nits (BT.2408, 203 by default); an HDR source in a bt1886
    target is an absolute map and is only meaningful for values inside the
    target's range.
"""
import argparse
import math
import sys

import numpy as np

D65 = (0.3127, 0.3290)

PRIMARIES = {
    "bt709": ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060)),
    "bt2020nc": ((0.708, 0.292), (0.170, 0.797), (0.131, 0.046)),
}

# Luma coefficients (Kr, Kb) of the Y'CbCr matrices.
KRKB = {"bt709": (0.2126, 0.0722), "bt2020nc": (0.2627, 0.0593)}

TRANSFERS = ("bt1886", "pq", "hlg")
RANGES = ("limited", "full")

# PQ (SMPTE ST 2084)
_M1 = 2610.0 / 16384.0
_M2 = 2523.0 / 4096.0 * 128.0
_C1 = 3424.0 / 4096.0
_C2 = 2413.0 / 4096.0 * 32.0
_C3 = 2392.0 / 4096.0 * 32.0

# HLG (BT.2100)
_HA = 0.17883277
_HB = 1.0 - 4.0 * _HA
_HC = 0.5 - _HA * math.log(4.0 * _HA)


def _odd(f):
    """Wrap f (defined for x >= 0) as an odd function."""
    def g(x, *a, **k):
        x = np.asarray(x, dtype=np.float64)
        return np.sign(x) * f(np.abs(x), *a, **k)
    return g


def rgb_to_xyz_matrix(matrix):
    """3x3 RGB(linear) -> XYZ with Y(white) = 1, computed from chromaticities."""
    prim = PRIMARIES[matrix]
    cols = []
    for x, y in prim:
        cols.append([x / y, 1.0, (1.0 - x - y) / y])
    p = np.array(cols).T
    wx, wy = D65
    white = np.array([wx / wy, 1.0, (1.0 - wx - wy) / wy])
    s = np.linalg.solve(p, white)
    return p * s


# ---- PQ -------------------------------------------------------------------

def pq_eotf(v):
    """PQ non-linear [0..1+] -> absolute nits (odd extension)."""
    def f(x):
        p = np.power(x, 1.0 / _M2)
        return 10000.0 * np.power(np.maximum(p - _C1, 0.0) / (_C2 - _C3 * p), 1.0 / _M1)
    return _odd(f)(v)


def pq_inverse(nits):
    def f(l):
        y = np.power(l / 10000.0, _M1)
        return np.power((_C1 + _C2 * y) / (1.0 + _C3 * y), _M2)
    return _odd(f)(nits)


# ---- BT.1886 --------------------------------------------------------------

def _bt1886_ab(lw, lb):
    g = 2.4
    a = (lw ** (1 / g) - lb ** (1 / g)) ** g
    b = lb ** (1 / g) / (lw ** (1 / g) - lb ** (1 / g))
    return a, b


def bt1886_eotf(v, lw=100.0, lb=0.0):
    a, b = _bt1886_ab(lw, lb)
    v = np.asarray(v, dtype=np.float64) + b
    return a * np.sign(v) * np.power(np.abs(v), 2.4)


def bt1886_inverse(nits, lw=100.0, lb=0.0):
    a, b = _bt1886_ab(lw, lb)
    l = np.asarray(nits, dtype=np.float64) / a
    return np.sign(l) * np.power(np.abs(l), 1 / 2.4) - b


# ---- HLG ------------------------------------------------------------------

def hlg_oetf_inverse(e):
    """HLG non-linear -> scene linear [0..1] (odd)."""
    def f(x):
        low = x * x / 3.0
        high = (np.exp((np.minimum(x, 10.0) - _HC) / _HA) + _HB) / 12.0
        return np.where(x <= 0.5, low, high)
    return _odd(f)(e)


def hlg_oetf(e):
    def f(x):
        low = np.sqrt(3.0 * x)
        high = _HA * np.log(np.maximum(12.0 * x - _HB, 1e-300)) + _HC
        return np.where(x <= 1.0 / 12.0, low, high)
    return _odd(f)(e)


def hlg_gamma(lw):
    return 1.2 + 0.42 * math.log10(lw / 1000.0)


def hlg_eotf(rgb, lw, luma):
    """rgb: (...,3) non-linear -> display nits. luma: Y row of RGB->XYZ."""
    e = hlg_oetf_inverse(rgb)
    ys = np.sum(e * luma, axis=-1, keepdims=True)
    g = hlg_gamma(lw)
    return lw * np.power(np.abs(ys), g - 1.0) * e


def hlg_inverse(nits, lw, luma):
    ld = np.asarray(nits, dtype=np.float64) / lw
    yd = np.sum(ld * luma, axis=-1, keepdims=True)
    g = hlg_gamma(lw)
    with np.errstate(divide="ignore", invalid="ignore"):
        scale = np.where(np.abs(yd) > 1e-12, np.power(np.abs(yd), (1.0 - g) / g), 0.0)
    return hlg_oetf(ld * scale)


# ---- chain ----------------------------------------------------------------

class Chain:
    def __init__(self, src_transfer="bt1886", src_matrix="bt709", src_range="limited", src_depth=10,
                 dst_transfer=None, dst_matrix=None, dst_range=None, dst_depth=None,
                 src_lw=100.0, src_lb=0.0, dst_lw=100.0, dst_lb=0.0,
                 hlg_lw=1000.0, ref_white=203.0, keeps_btb_wtw=True):
        self.src_transfer = src_transfer
        self.src_matrix = src_matrix
        self.src_range = src_range
        self.src_depth = src_depth
        self.dst_transfer = dst_transfer or src_transfer
        self.dst_matrix = dst_matrix or src_matrix
        self.dst_range = dst_range or src_range
        self.dst_depth = dst_depth or src_depth
        self.src_lw, self.src_lb = src_lw, src_lb
        self.dst_lw, self.dst_lb = dst_lw, dst_lb
        self.hlg_lw = hlg_lw
        self.ref_white = ref_white
        self.keeps_btb_wtw = keeps_btb_wtw
        for t in (self.src_transfer, self.dst_transfer):
            if t not in TRANSFERS:
                raise ValueError("unknown transfer %r" % t)
        for m in (self.src_matrix, self.dst_matrix):
            if m not in KRKB:
                raise ValueError("unknown matrix %r" % m)
        for r in (self.src_range, self.dst_range):
            if r not in RANGES:
                raise ValueError("unknown range %r" % r)
        for d in (self.src_depth, self.dst_depth):
            if d not in (8, 10, 12):
                raise ValueError("unsupported depth %r" % d)

    def is_passthrough(self):
        return (self.src_transfer, self.src_matrix, self.src_range, self.src_depth) == \
               (self.dst_transfer, self.dst_matrix, self.dst_range, self.dst_depth) and \
               (self.src_lw, self.src_lb) == (self.dst_lw, self.dst_lb)

    def describe(self):
        return {k: getattr(self, k) for k in (
            "src_transfer", "src_matrix", "src_range", "src_depth", "dst_transfer", "dst_matrix",
            "dst_range", "dst_depth", "src_lw", "src_lb", "dst_lw", "dst_lb", "hlg_lw",
            "ref_white", "keeps_btb_wtw")}

    def effective_src_lw(self):
        """SDR source shown in an HDR target sits at ref_white."""
        if self.src_transfer == "bt1886" and self.dst_transfer != "bt1886":
            return self.ref_white
        return self.src_lw


def parse_spec(text):
    """'transfer:matrix:range:depth' -> tuple."""
    parts = text.split(":")
    if len(parts) != 4:
        raise ValueError("spec must be transfer:matrix:range:depth, got %r" % text)
    return parts[0], parts[1], parts[2], int(parts[3])


def chain_from_spec(src, dst=None, **kw):
    s = parse_spec(src)
    d = parse_spec(dst) if dst else s
    return Chain(s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], **kw)


def _nominal(depth, rng):
    s = depth - 8
    if rng == "limited":
        return (16 << s, 219 << s, 128 << s, 224 << s)  # y0, yspan, c0, cspan
    full = (1 << depth) - 1
    return (0, full, 1 << (depth - 1), full)


def codes_to_ycc(codes, depth, rng):
    """codes (...,3) Y,Cb,Cr -> normalised Y' in [0,1], Cb/Cr in [-.5,.5] (unclipped)."""
    c = np.asarray(codes, dtype=np.float64)
    y0, ys, c0, cs = _nominal(depth, rng)
    out = np.empty_like(c)
    out[..., 0] = (c[..., 0] - y0) / ys
    out[..., 1] = (c[..., 1] - c0) / cs
    out[..., 2] = (c[..., 2] - c0) / cs
    return out


def ycc_to_codes(ycc, depth, rng, clip_codes=True):
    y0, ys, c0, cs = _nominal(depth, rng)
    out = np.empty_like(ycc)
    out[..., 0] = ycc[..., 0] * ys + y0
    out[..., 1] = ycc[..., 1] * cs + c0
    out[..., 2] = ycc[..., 2] * cs + c0
    out = np.rint(out)  # half to even
    if clip_codes:
        out = np.clip(out, 0, (1 << depth) - 1)
    return out


def ycc_to_rgb(ycc, matrix):
    kr, kb = KRKB[matrix]
    kg = 1.0 - kr - kb
    y, cb, cr = ycc[..., 0], ycc[..., 1], ycc[..., 2]
    r = y + 2.0 * (1.0 - kr) * cr
    b = y + 2.0 * (1.0 - kb) * cb
    g = (y - kr * r - kb * b) / kg
    return np.stack([r, g, b], axis=-1)


def rgb_to_ycc(rgb, matrix):
    kr, kb = KRKB[matrix]
    kg = 1.0 - kr - kb
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = kr * r + kg * g + kb * b
    cb = (b - y) / (2.0 * (1.0 - kb))
    cr = (r - y) / (2.0 * (1.0 - kr))
    return np.stack([y, cb, cr], axis=-1)


def rgb_to_linear(rgb, transfer, matrix, lw=100.0, lb=0.0, hlg_lw=1000.0):
    """Non-linear R'G'B' -> linear light in nits (still in the matrix' primaries)."""
    if transfer == "bt1886":
        return bt1886_eotf(rgb, lw, lb)
    if transfer == "pq":
        return pq_eotf(rgb)
    return hlg_eotf(rgb, hlg_lw, rgb_to_xyz_matrix(matrix)[1])


def linear_to_rgb(lin, transfer, matrix, lw=100.0, lb=0.0, hlg_lw=1000.0):
    if transfer == "bt1886":
        return bt1886_inverse(lin, lw, lb)
    if transfer == "pq":
        return pq_inverse(lin)
    return hlg_inverse(lin, hlg_lw, rgb_to_xyz_matrix(matrix)[1])


def codes_to_xyz(codes, depth, rng, matrix, transfer, lw=100.0, lb=0.0, hlg_lw=1000.0):
    """Codes -> absolute XYZ in nits."""
    rgb = ycc_to_rgb(codes_to_ycc(codes, depth, rng), matrix)
    lin = rgb_to_linear(rgb, transfer, matrix, lw, lb, hlg_lw)
    return lin @ rgb_to_xyz_matrix(matrix).T


def expected_codes(codes, chain):
    """Expected destination codes (...,3) for source codes (...,3)."""
    ycc = codes_to_ycc(codes, chain.src_depth, chain.src_range)
    rgb = ycc_to_rgb(ycc, chain.src_matrix)
    lin = rgb_to_linear(rgb, chain.src_transfer, chain.src_matrix,
                        chain.effective_src_lw(), chain.src_lb, chain.hlg_lw)
    if chain.src_matrix != chain.dst_matrix:
        conv = np.linalg.solve(rgb_to_xyz_matrix(chain.dst_matrix), rgb_to_xyz_matrix(chain.src_matrix))
        lin = lin @ conv.T
    out = linear_to_rgb(lin, chain.dst_transfer, chain.dst_matrix, chain.dst_lw, chain.dst_lb, chain.hlg_lw)
    if not chain.keeps_btb_wtw:
        out = np.clip(out, 0.0, 1.0)
    return ycc_to_codes(rgb_to_ycc(out, chain.dst_matrix), chain.dst_depth, chain.dst_range,
                        clip_codes=True)


def btb_wtw_class(y_code, depth, rng):
    """'BTB' / 'WTW' / '' for a destination Y code (limited range only)."""
    if rng != "limited":
        return ""
    s = depth - 8
    if y_code < (16 << s):
        return "BTB"
    if y_code > (235 << s):
        return "WTW"
    return ""


def table_rows(patches, chain):
    """patches: list of dicts with id and ycbcr (10-bit source codes)."""
    rows = []
    for p in patches:
        src = np.asarray(p["ycbcr"], dtype=np.float64)
        if chain.src_depth != 10:
            src = np.rint(src * (2.0 ** (chain.src_depth - 10)))
        exp = expected_codes(src, chain)
        cls = btb_wtw_class(float(exp[0]), chain.dst_depth, chain.dst_range) if chain.keeps_btb_wtw else ""
        rows.append({"id": p["id"], "src_y": int(src[0]), "src_cb": int(src[1]), "src_cr": int(src[2]),
                     "exp_y": int(exp[0]), "exp_cb": int(exp[1]), "exp_cr": int(exp[2]),
                     "btb_wtw_expected": cls})
    return rows


def format_table(rows, fmt="csv"):
    cols = ["id", "src_y", "src_cb", "src_cr", "exp_y", "exp_cb", "exp_cr", "btb_wtw_expected"]
    if fmt == "csv":
        lines = [",".join(cols)]
        lines += [",".join(str(r[c]) for c in cols) for r in rows]
    else:
        lines = ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
        lines += ["| " + " | ".join(str(r[c]) for c in cols) + " |" for r in rows]
    return "\n".join(lines) + "\n"


def add_chain_args(ap):
    ap.add_argument("--src", default="bt1886:bt709:limited:10", help="transfer:matrix:range:depth")
    ap.add_argument("--dst", default=None, help="same form; default = --src (passthrough)")
    ap.add_argument("--src-lw", type=float, default=100.0)
    ap.add_argument("--src-lb", type=float, default=0.0)
    ap.add_argument("--dst-lw", type=float, default=100.0)
    ap.add_argument("--dst-lb", type=float, default=0.0)
    ap.add_argument("--hlg-lw", type=float, default=1000.0, help="HLG display peak for the system gamma")
    ap.add_argument("--ref-white", type=float, default=203.0, help="SDR white in HDR targets (BT.2408)")
    ap.add_argument("--no-keep-btb-wtw", action="store_true", help="chain clips BTB/WTW")


def chain_from_args(a):
    return chain_from_spec(a.src, a.dst, src_lw=a.src_lw, src_lb=a.src_lb, dst_lw=a.dst_lw,
                           dst_lb=a.dst_lb, hlg_lw=a.hlg_lw, ref_white=a.ref_white,
                           keeps_btb_wtw=not a.no_keep_btb_wtw)


def main(argv=None):
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import patterns
    ap = argparse.ArgumentParser(description="Print the expected output code table of a chain.")
    ap.add_argument("--set", required=True, choices=sorted(patterns.SETS))
    ap.add_argument("--size", required=True, help="WxH (layout only; codes do not depend on it)")
    ap.add_argument("--format", choices=("csv", "md"), default="csv")
    add_chain_args(ap)
    a = ap.parse_args(argv)
    w, h = patterns.parse_size(a.size)
    layout = patterns.build_layout(a.set, w, h)
    sys.stdout.write(format_table(table_rows(layout["patches"], chain_from_args(a)), a.format))
    return 0


if __name__ == "__main__":
    sys.exit(main())
