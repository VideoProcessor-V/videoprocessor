# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""numpy mirror of src/VP-V/modules/output/dither/DitherNeed.h (the dither-need map for the output sigma-delta).

Law (identical names/semantics to the C++; double = the CPU reference, float32 = the shader mirror used by sample_extra):

  sigma_out = sqrt(max(N,0)) * |Sout| / q_out
  a_target  = clamp(a_need - sigma_out, a_base, max(a_max, a_base))
  g_vis     = |Sout| > 1e-6 ? smoothstep(vis_lo, vis_hi, V * q_out / |Sout|) : 0
  g         = (in_picture and not overlay) ? banding * g_vis : 0
  target    = g * (a_target - a_base)
Temporal state (class Map): reset to the target (first frame, discontinuity, grid / LUT fingerprint / colour bits /
a_base change), hysteresis |target - e| <= deadband holds e, else e += alpha (target - e), alpha = 1 - exp(-dt/tau).

Also a minimal mirror of the region-evidence inputs it needs, so the eval can run on pixels: block_inputs_from_luma(y)
-> N (GrainSplit::Compute at scale B/16, luma through ExtendPicture), meanY (StatsBlock), banding (DeriveBlock).
Differences to the C++ GrainSplit/StatsBlock are documented at grain_blocks(): Gaussians use the C++ kernel (radius
max(1, ceil(3 sigma)), normalised, edges replicated), summed in a different (vectorised) order, so results agree to float64
rounding, not bitwise.
"""
import math

import numpy as np

import light_level_ref as llr

F32 = np.float32
VERSION = 1

# Params (C++ DitherNeed::Params defaults)
A_BASE = 0.15
A_NEED = 0.55
A_MAX = 0.5
VIS_LO = 0.25
VIS_HI = 0.75
DEADBAND = 0.02
TAU = 0.25

# DeriveParams banding constants
FLAT_LO, FLAT_HI, SLOPE_LO, SLOPE_HI = 1.0, 12.0, 1.0, 4.0


class Params:
    def __init__(self, a_base=A_BASE, a_need=A_NEED, a_max=A_MAX, vis_lo=VIS_LO, vis_hi=VIS_HI,
                 deadband=DEADBAND, tau=TAU):
        self.a_base, self.a_need, self.a_max = a_base, a_need, a_max
        self.vis_lo, self.vis_hi, self.deadband, self.tau = vis_lo, vis_hi, deadband, tau


def smoothstep(e0, e1, x):
    x = np.asarray(x, dtype=np.float64)
    if not (e1 > e0):
        return np.where(x >= e0, 1.0, 0.0)
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def q_out(color_bits):
    """2^(10 - colorBits): the link code step in 10-bit-scale codes."""
    return math.ldexp(1.0, 10 - min(max(int(color_bits), 1), 16))


def alpha_for(dt, tau=TAU):
    if not (dt > 0.0) or not (tau > 0.0):
        return 1.0
    return 1.0 - math.exp(-dt / tau)


def gate_vis(p, V, Sout, q):
    V = np.asarray(V, dtype=np.float64)
    aS = np.abs(np.asarray(Sout, dtype=np.float64))
    ok = aS > 1e-6
    safe = np.where(ok, aS, 1.0)
    return np.where(ok, smoothstep(p.vis_lo, p.vis_hi, V * q / safe), 0.0)


def target_block(p, N, V, Sout, banding, in_picture, overlay, q):
    """Vectorised over blocks (all arguments broadcast)."""
    N = np.asarray(N, dtype=np.float64)
    aS = np.abs(np.asarray(Sout, dtype=np.float64))
    sigma_out = np.sqrt(np.maximum(N, 0.0)) * aS / q
    hi = max(p.a_max, p.a_base)
    a_target = np.minimum(np.maximum(p.a_need - sigma_out, p.a_base), hi)
    g_vis = gate_vis(p, V, Sout, q)
    on = np.logical_and(np.asarray(in_picture, dtype=bool), np.logical_not(np.asarray(overlay, dtype=bool)))
    g = np.where(on, np.asarray(banding, dtype=np.float64) * g_vis, 0.0)
    return g * (a_target - p.a_base)


def needed_anywhere(lut, q, p=None):
    """lut: dict from light_level_ref.build_lut. max over codes 64..1023 of gVis > 0."""
    p = p or Params()
    g = gate_vis(p, lut["V"][64:], lut["sout"][64:], q)
    return bool(np.max(g) > 0.0)


class Map:
    """State + Update; extra is the per-block extra amplitude (float64, raster order)."""

    def __init__(self):
        self.e = None
        self.valid = False
        self.key = None
        self.result = "off"
        self.missing = 0

    def update(self, p, grid_shape, blocks, dt, discontinuity=False, enabled=True, missing=0, needed=True,
               color_bits=8, lut_fingerprint=""):
        """blocks: dict(N, V, Sout, banding, inPicture, overlay) arrays of grid_shape, or None."""
        n = int(np.prod(grid_shape))
        key = (tuple(grid_shape), lut_fingerprint, color_bits, p.a_base)
        if not enabled:
            self.result = "off"
        elif missing:
            self.result = "unavailable"
        elif not needed:
            self.result = "not needed"
        else:
            self.result = "on"
        self.missing = missing if enabled else 0
        if self.result != "on":
            self.e = np.zeros(grid_shape, dtype=np.float64)
            self.valid = False
            self.key = key
            return self.result
        q = q_out(color_bits)
        target = target_block(p, blocks["N"], blocks["V"], blocks["Sout"], blocks["banding"], blocks["inPicture"],
                              blocks["overlay"], q)
        reset = (not self.valid) or discontinuity or key != self.key or self.e is None or self.e.size != n
        if reset:
            self.e = np.array(target, dtype=np.float64)
        else:
            a = alpha_for(dt, p.tau)
            diff = target - self.e
            hold = np.abs(diff) <= p.deadband
            self.e = np.where(hold, self.e, self.e + a * diff)
        self.valid = True
        self.key = key
        return self.result


def sample_extra(extra, block_px, scale_x, scale_y, off_x, off_y, pos_x, pos_y):
    """float32 bilinear over block centres, indices clamped; same operation order as DitherNeed::SampleExtra.
    extra: (gh, gw) array; pos_x/pos_y scalars or arrays. Returns float32."""
    m = np.asarray(extra, dtype=F32)
    gh, gw = m.shape
    px = np.asarray(pos_x, dtype=F32)
    py = np.asarray(pos_y, dtype=F32)
    half = F32(0.5)
    sx = (px + half) * F32(scale_x) + F32(off_x)
    sy = (py + half) * F32(scale_y) + F32(off_y)
    bp = F32(block_px)
    u = sx / bp - half
    v = sy / bp - half
    fx0 = np.floor(u)
    fy0 = np.floor(v)
    fx = (u - fx0).astype(F32)
    fy = (v - fy0).astype(F32)
    x0 = fx0.astype(np.int64)
    y0 = fy0.astype(np.int64)
    i0 = np.clip(x0, 0, gw - 1)
    i1 = np.clip(x0 + 1, 0, gw - 1)
    j0 = np.clip(y0, 0, gh - 1)
    j1 = np.clip(y0 + 1, 0, gh - 1)
    a, b, c, d = m[j0, i0], m[j0, i1], m[j1, i0], m[j1, i1]
    top = (a + (b - a) * fx).astype(F32)
    bot = (c + (d - c) * fx).astype(F32)
    return (top + (bot - top) * fy).astype(F32)


# ---------------------------------------------------------------------------------------------------------------
# Region-evidence mirror (inputs of the law, computed from pixels)
# ---------------------------------------------------------------------------------------------------------------

def block_size_for(h):
    """RegionEvidence::BlockSizeFor."""
    if h <= 0:
        return 16
    shift = int(math.floor(math.log2(h / 1080.0) + 0.5))
    b = (16 << min(shift, 2)) if shift >= 0 else (16 >> min(-shift, 1))
    return max(8, min(64, b))


def _reflect_into(i, lo, hi):
    i = np.asarray(i)
    i = np.where(i < lo, lo + (lo - 1 - i), np.where(i >= hi, hi - 1 - (i - hi), i))
    return np.clip(i, lo, hi - 1)


def picture_or_frame(picture, w, h):
    """picture = (left, top, right, bottom) or None; clipped; empty = the whole raster."""
    if picture is None:
        return (0, 0, w, h)
    l, t, r, b = max(0, picture[0]), max(0, picture[1]), min(w, picture[2]), min(h, picture[3])
    return (0, 0, w, h) if (r <= l or b <= t) else (l, t, r, b)


def extend_picture(y, picture=None):
    """RegionEvidence::ExtendPicture: bars replaced by the picture mirrored about its edge (identity for a full picture)."""
    h, w = y.shape
    l, t, r, b = picture_or_frame(picture, w, h)
    ys = _reflect_into(np.arange(h), t, b)
    xs = _reflect_into(np.arange(w), l, r)
    return y[np.ix_(ys, xs)]


def _gauss(a, sigma):
    """GrainSplit::Gauss: radius max(1, ceil(3 sigma)), normalised, edges replicated, separable."""
    if not sigma > 0.0:
        return a
    r = max(1, int(math.ceil(3.0 * sigma)))
    k = np.exp(-0.5 * np.arange(-r, r + 1, dtype=np.float64) ** 2 / (sigma * sigma))
    k /= k.sum()
    h, w = a.shape
    pad = np.pad(a, ((0, 0), (r, r)), mode="edge")
    t = sum(k[i + r] * pad[:, r + i:r + i + w] for i in range(-r, r + 1))
    pad = np.pad(t, ((r, r), (0, 0)), mode="edge")
    return sum(k[i + r] * pad[r + i:r + i + h, :] for i in range(-r, r + 1))


def grain_blocks(y, B, sub=4, quiet=4, nb=2, cal=2.2):
    """GrainSplit::Compute's block outputs on a (picture-extended) luma plane at scale B/16:
    returns (d1, d2, N) with d1 = y - G(1 s) y, d2 = G(1 s) y - G(2.5 s) y, N the (bh, bw) grain level.
    Differences to the C++: none in the law. Sub-block energies use the actual pixel spans (clipped at the frame edge,
    empty ones skipped) exactly as the C++; the minimum over blocks clamps at the grid edge (scipy-free); only the
    float64 summation order differs (vectorised)."""
    scale = B / 16.0
    y = np.asarray(y, dtype=np.float64)
    h, w = y.shape
    g1, gb = _gauss(y, 1.0 * scale), _gauss(y, 2.5 * scale)
    d1, d2 = y - g1, g1 - gb
    d = d1 + d2
    bh, bw = -(-h // B), -(-w // B)
    sb = B // sub
    # per sub-block mean of d^2 with the real span (NaN = empty)
    sq = np.full((bh * B, bw * B), np.nan)
    sq[:h, :w] = d * d
    cells = sq.reshape(bh * sub, sb, bw * sub, sb)
    cnt = np.sum(~np.isnan(cells), axis=(1, 3))
    tot = np.nansum(cells, axis=(1, 3))
    e = np.where(cnt > 0, tot / np.maximum(cnt, 1), np.inf)                       # (bh*sub, bw*sub)
    e = e.reshape(bh, sub, bw, sub).transpose(0, 2, 1, 3).reshape(bh, bw, sub * sub)
    e = np.sort(e, axis=2)
    nvalid = np.minimum(np.sum(np.isfinite(e), axis=2), quiet)
    first = np.where(np.isfinite(e[:, :, :quiet]), e[:, :, :quiet], 0.0)
    qb = np.where(nvalid > 0, cal * first.sum(axis=2) / np.maximum(nvalid, 1), 0.0)
    pad = np.pad(qb, nb, mode="edge")
    N = np.full((bh, bw), np.inf)
    for j in range(-nb, nb + 1):
        for i in range(-nb, nb + 1):
            N = np.minimum(N, pad[nb + j:nb + j + bh, nb + i:nb + i + bw])
    return d1, d2, N


def _span(w, h, B, rect, bx, by):
    fx0, fy0 = bx * B, by * B
    fx1, fy1 = min(fx0 + B, w), min(fy0 + B, h)
    x0, y0 = max(fx0, rect[0]), max(fy0, rect[1])
    x1, y1 = min(fx1, rect[2]), min(fy1, rect[3])
    return x0, y0, x1, y1, (fx1 - fx0) * (fy1 - fy0)


def block_inputs_from_luma(y, picture=None):
    """The law's per-block inputs from a luma plane in 10-bit codes (the Python twin of
    RegionEvidence::ComputeFrame + View + GrainView). Returns dict(N, meanY, banding, inPicture, overlay=0, grid)
    with (gh, gw) float64/bool arrays and grid = dict(w, h, B, gw, gh, picture)."""
    y = np.asarray(y, dtype=np.float64)
    h, w = y.shape
    B = block_size_for(h)
    rect = picture_or_frame(picture, w, h)
    ext = extend_picture(y, picture)
    d1, d2, N = grain_blocks(ext, B)
    bh, bw = N.shape
    meanY = np.zeros((bh, bw))
    banding = np.zeros((bh, bw))
    inpic = np.zeros((bh, bw), dtype=bool)
    overlay = np.zeros((bh, bw), dtype=bool)
    l, t, r, b = rect
    cl = lambda a, lo, hi: np.clip(a, lo, hi)
    for by in range(bh):
        for bx in range(bw):
            x0, y0, x1, y1, area = _span(w, h, B, rect, bx, by)
            if x1 <= x0 or y1 <= y0 or area <= 0:
                continue                                      # coverage 0: not in picture, stats zero
            nx, ny = x1 - x0, y1 - y0
            n = nx * ny
            blk = y[y0:y1, x0:x1]
            meanY[by, bx] = blk.sum() / n
            inpic[by, bx] = (n / area) >= 0.5
            e1 = (d1[y0:y1, x0:x1] ** 2).sum() / n
            e2 = (d2[y0:y1, x0:x1] ** 2).sum() / n
            # plane-fit slopes (StatsBlock): least squares on the block's picture span
            cxm, cym = 0.5 * (x0 + x1 - 1), 0.5 * (y0 + y1 - 1)
            xs = (np.arange(x0, x1) - cxm)[None, :]
            ys = (np.arange(y0, y1) - cym)[:, None]
            sxx = ny * nx * (nx * nx - 1) / 12.0
            syy = nx * ny * (ny * ny - 1) / 12.0
            slope_x = (xs * blk).sum() / sxx if nx > 1 else 0.0
            slope_y = (ys * blk).sum() / syy if ny > 1 else 0.0
            if inpic[by, bx]:
                texture = max(e1 + e2 - N[by, bx], 0.0)
                slope = math.sqrt(slope_x * slope_x + slope_y * slope_y)
                ramp = lambda v, lo, hi: min(max((v - lo) / (hi - lo), 0.0), 1.0)
                banding[by, bx] = (1.0 - ramp(texture, FLAT_LO, FLAT_HI)) * (1.0 - ramp(slope, SLOPE_LO, SLOPE_HI))
    grid = dict(w=w, h=h, B=B, gw=bw, gh=bh, picture=rect)
    return dict(N=N, meanY=meanY, banding=banding, inPicture=inpic, overlay=overlay, grid=grid)


def block_lut_inputs(inp, lut):
    """Adds V and Sout at meanY (LightLevel::Fetch) to the block inputs."""
    f = llr.fetch(lut, inp["meanY"])
    out = dict(inp)
    out["V"] = f["V"]
    out["Sout"] = f["sout"]
    return out


def extra_map_for_frame(y, lut, color_bits=8, params=None, picture=None, state=None, dt=0.0, discontinuity=False,
                        lut_fingerprint=""):
    """One frame: luma (10-bit codes) -> per-block extra amplitude (gh, gw) float64 (the first frame of a state resets
    to the target). Returns (extra, grid, inputs). Pass a Map as `state` to carry the temporal smoothing."""
    p = params or Params()
    inp = block_lut_inputs(block_inputs_from_luma(y, picture), lut)
    st = state or Map()
    needed = needed_anywhere(lut, q_out(color_bits), p)
    st.update(p, inp["N"].shape, inp, dt, discontinuity, True, 0, needed, color_bits, lut_fingerprint)
    return st.e.copy(), inp["grid"], inp


def amp_map_full(extra, grid, shape=None, a_base=A_BASE, scale_x=1.0, scale_y=1.0, off_x=0.0, off_y=0.0):
    """Per-pixel amplitude a_base + SampleExtra over an output raster of `shape` (h, w) (default: the source raster),
    float64 array usable as sigma_delta_ref's `amplitude`."""
    h, w = shape if shape is not None else (grid["h"], grid["w"])
    yy, xx = np.indices((h, w))
    ex = sample_extra(extra, grid["B"], scale_x, scale_y, off_x, off_y, xx, yy)
    return float(a_base) + ex.astype(np.float64)
