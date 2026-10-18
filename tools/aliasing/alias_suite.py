"""Aliasing and moire fidelity suite for VideoProcessor's full picture chain.

Owner, 2026-10-18: aliasing / moire is core to the fidelity goal; evaluate at the OUTPUT resolution.

Pipeline (docs/agent-reports/aliasing.md):
  prepare   write the test cards (10-bit V210 codes) and plan.tsv for every source x output pair
            and every stage configuration (VP_ALIAS_SOURCES, VP_ALIAS_OUTPUTS: "WxH,WxH,...")
  validate  CPU only: synthetic captures of known-good / known-bad resamplers through the very same
            analysis; the metric must rank them (the suite does not count otherwise)
  run       for each batch: vstest AliasingSuiteCaptures (the shipped renderer DLL, off screen,
            the output size = the window size), then analyse and delete the raw captures
  analyse   captures -> results.json (per run metrics)
  report    results.json -> markdown tables per source x output pair and per stage
  gate      results.json vs alias_thresholds.json (exit 1 on a regression)

Every ratio is derived from the source and output sizes; the only fixed sizes are the test cases.
Reference: ideal band-limited (periodic sinc / Dirichlet) resampling of the exact source codes to
the output picture grid, separable, in float64, cut-off min(source, output) Nyquist per axis.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SOURCES = "3840x2160,1920x1080"
DEFAULT_OUTPUTS = "2560x1600,1920x1200,1920x1080"
AMP = 0.2          # pattern amplitude (normalised luma, about mid grey 0.5)
CAMP = 0.15        # chroma pattern amplitude
GRAIN = 0.04       # grain-like noise sigma
WIN = 32           # analysis window (output pixels)
STEP = 16


# ---------------------------------------------------------------------------- sizes and geometry
def parse_sizes(text):
    out = []
    for part in text.split(","):
        part = part.strip().lower()
        if part:
            w, h = part.split("x")
            out.append((int(w), int(h)))
    return out


def fit_rect(sw, sh, ow, oh):
    """Aspect-preserving fit, centred (letterbox / pillarbox, never a stretch)."""
    scale = min(ow / sw, oh / sh)
    w, h = sw * scale, sh * scale
    return (ow - w) / 2.0, (oh - h) / 2.0, w, h, scale


def pair_name(src, out):
    return "s%dx%d_o%dx%d" % (src[0], src[1], out[0], out[1])


# ---------------------------------------------------------------------------- cards
def to_ycode(y):
    return np.clip(np.rint(64 + 876 * y), 4, 1019).astype(np.uint16)


def to_ccode(c):
    return np.clip(np.rint(512 + 896 * c), 4, 1019).astype(np.uint16)


def grain(shape, rng, sigma_px=0.5):
    n = rng.standard_normal(shape)
    fy = np.fft.fftfreq(shape[0])[:, None]
    fx = np.fft.fftfreq(shape[1])[None, :]
    g = np.exp(-2 * (math.pi * sigma_px) ** 2 * (fx * fx + fy * fy))
    n = np.real(np.fft.ifft2(np.fft.fft2(n) * g))
    return n / n.std()


def fade(n, margin):
    t = np.ones(n)
    r = np.arange(margin)
    w = 0.5 - 0.5 * np.cos(math.pi * (r + 0.5) / margin)
    t[:margin] = w
    t[n - margin:] = w[::-1]
    return t


STILL_TILES = [
    ["zone", "hsweep", "vsweep", "dsweep"],
    ["zone_grain", "hsweep_grain", "zone_cb", "hsweep_cr"],
    ["edge", "flat", "patches", "grain"],
]


def still_card(sw, sh, seed=1):
    """Luma (sh, sw) and chroma (sh, sw/2) planes in normalised units, and the tile layout."""
    rng = np.random.default_rng(seed)
    Y = np.full((sh, sw), 0.5)
    Cb = np.zeros((sh, sw // 2))
    Cr = np.zeros((sh, sw // 2))
    rows, cols = len(STILL_TILES), len(STILL_TILES[0])
    th, tw = sh // rows, sw // cols
    tiles = []
    for r in range(rows):
        for c in range(cols):
            kind = STILL_TILES[r][c]
            x0, y0 = c * tw, r * th
            m = max(4, int(0.08 * min(tw, th)))
            win = fade(th, m)[:, None] * fade(tw, m)[None, :]
            yy, xx = np.mgrid[0:th, 0:tw].astype(float)
            cx, cy = (tw - 1) / 2.0, (th - 1) / 2.0
            R = min(tw, th) / 2.0 - m
            fmax = 0.45                       # 0.9 of the source Nyquist (cycles / source pixel)
            tile_y = np.zeros((th, tw))
            if kind.startswith("zone") and kind not in ("zone_cb",):
                rr = (xx - cx) ** 2 + (yy - cy) ** 2
                tile_y = AMP * np.cos(math.pi * fmax * rr / R)
            elif kind.startswith("hsweep") and kind != "hsweep_cr":
                L = tw - 1
                tile_y = AMP * np.cos(2 * math.pi * (0.02 * xx + (fmax - 0.02) * xx * xx / (2 * L)))
            elif kind == "vsweep":
                L = th - 1
                tile_y = AMP * np.cos(2 * math.pi * (0.02 * yy + (fmax - 0.02) * yy * yy / (2 * L)))
            elif kind == "dsweep":
                u = (xx + yy) / math.sqrt(2)
                L = u.max()
                f1 = fmax * math.sqrt(2)      # per-axis components up to fmax
                tile_y = AMP * np.cos(2 * math.pi * (0.02 * u + (f1 - 0.02) * u * u / (2 * L)))
            if kind.endswith("_grain"):
                tile_y = tile_y + GRAIN * grain((th, tw), rng)
            if kind == "grain":
                tile_y = 1.5 * GRAIN * grain((th, tw), rng)
            if kind in ("zone_cb", "hsweep_cr"):
                # chroma: defined in luma coordinates, sampled co-sited at even luma columns;
                # 0.9 of the chroma Nyquist horizontally (0.225 cycles / luma pixel)
                cxx = xx[:, 0::2]
                cyy = yy[:, 0::2]
                cw = win[:, 0::2]
                if kind == "zone_cb":
                    rr = (cxx - cx) ** 2 + (cyy - cy) ** 2
                    Cb[y0:y0 + th, x0 // 2:x0 // 2 + tw // 2] = CAMP * np.cos(math.pi * (fmax / 2) * rr / R) * cw
                else:
                    L = tw - 1
                    Cr[y0:y0 + th, x0 // 2:x0 // 2 + tw // 2] = CAMP * np.cos(
                        2 * math.pi * (0.01 * cxx + (fmax / 2 - 0.01) * cxx * cxx / (2 * L))) * cw
            if kind == "edge":
                # a hard-edged rectangle (step 0.3) for ringing
                ex0, ex1 = int(0.3 * tw), int(0.7 * tw)
                ey0, ey1 = int(0.3 * th), int(0.7 * th)
                tile_y = np.zeros((th, tw))
                tile_y[ey0:ey1, ex0:ex1] = 0.3
                win = np.ones_like(win)
                tiles_edge = (x0 + ex0, y0 + ey0, x0 + ex1, y0 + ey1)
            if kind == "patches":
                # greys and colours for the output colour fit (hard patches inside a grey margin)
                vals = [(0.2, 0, 0), (0.4, 0, 0), (0.6, 0, 0), (0.8, 0, 0),
                        (0.5, 0.12, 0), (0.5, -0.12, 0), (0.5, 0, 0.12), (0.5, 0, -0.12)]
                pw, ph = (tw - 2 * m) // 4, (th - 2 * m) // 2
                for i, (yv, cbv, crv) in enumerate(vals):
                    px0, py0 = x0 + m + (i % 4) * pw, y0 + m + (i // 4) * ph
                    Y[py0:py0 + ph, px0:px0 + pw] = yv
                    Cb[py0:py0 + ph, px0 // 2:(px0 + pw) // 2] = cbv
                    Cr[py0:py0 + ph, px0 // 2:(px0 + pw) // 2] = crv
                tiles.append(dict(kind=kind, x0=x0, y0=y0, x1=x0 + tw, y1=y0 + th, margin=m))
                continue
            Y[y0:y0 + th, x0:x0 + tw] = 0.5 + tile_y * win
            t = dict(kind=kind, x0=x0, y0=y0, x1=x0 + tw, y1=y0 + th, margin=m)
            if kind == "edge":
                t["rect"] = tiles_edge
            tiles.append(t)
    return Y, Cb, Cr, tiles


def moving_bands(cut):
    """Bands of the moving card as fractions of the pair's cut-off (the output Nyquist, or the
    source Nyquist when upscaling): passband, near the cut-off, and above it (stopband, only
    where the source can carry it: below 0.95 of the source Nyquist)."""
    bands = [("flat", None), ("p0.40", 0.40), ("p0.70", 0.70), ("n0.90", 0.90)]
    for f in (1.15, 1.40):
        if f * cut <= 0.475:
            bands.append(("s%.2f" % f, f))
    bands += [("d0.80", 0.80), ("grain", None)]
    return bands


def moving_card(sw, sh, cut, seed=2):
    """The x-periodic moving card (luma): integer cycles over the width, so the harness's
    wrapping pan is seamless; cut in cycles per source pixel."""
    rng = np.random.default_rng(seed)
    bands = moving_bands(cut)
    bh = sh // len(bands)
    Y = np.full((sh, sw), 0.5)
    x = np.arange(sw)[None, :]
    layout = []
    for i, (name, f) in enumerate(bands):
        y0 = i * bh
        # a different mean level per band: the per-row colour fit needs luma variation
        m = 0.35 + 0.3 * ((i * 3) % len(bands)) / max(len(bands) - 1, 1)
        Y[y0:y0 + bh] = m
        yy = np.arange(bh)[:, None]
        if name[0] in "pns":
            k = round(f * cut * sw)
            Y[y0:y0 + bh] = m + AMP * np.cos(2 * math.pi * k * x / sw) * np.ones((bh, 1))
        elif name[0] == "d":
            k = round(f * cut * sw)
            Y[y0:y0 + bh] = m + AMP * np.cos(2 * math.pi * (k * x / sw + (k / sw) * (y0 + yy)))
        elif name == "grain":
            Y[y0:y0 + bh] = m + 1.5 * GRAIN * grain((bh, sw), rng)   # periodic in x (FFT synthesis)
        layout.append(dict(kind=name, frac=f, y0=y0, y1=y0 + bh))
    return Y, layout


def pair_cut(sw, sh, ow, oh):
    return min(0.5, 0.5 * fit_rect(sw, sh, ow, oh)[4])


# ---------------------------------------------------------------------------- ideal resampling
def ideal_matrix(n_in, positions, cutoff):
    """Periodic band-limited interpolation: out[j] = sum_n s[n] D(p_j - n), D = Dirichlet kernel
    with every DFT bin |k| < cutoff*N and half weight at |k| = cutoff*N (cutoff in cycles/sample)."""
    N = n_in
    kc = cutoff * N
    m = int(math.ceil(kc - 1e-9)) - 1
    half = abs(kc - round(kc)) < 1e-9
    t = positions[:, None] - np.arange(N)[None, :]
    w = 2 * math.pi * t / N
    s2 = np.sin(w / 2)
    small = np.abs(s2) < 1e-12
    D = np.where(small, 2 * m + 1, np.sin((m + 0.5) * w) / np.where(small, 1, s2))
    if half:
        D = D + np.cos((m + 1) * w)
    return D / N


def out_positions(n_out_px, origin, scale, offset=0.0):
    """Source coordinates of output pixel centres j=0..n-1 (relative to the picture origin)."""
    j = np.arange(n_out_px)
    return (j + 0.5 - origin) / scale - 0.5 - offset


class Geometry:
    def __init__(self, sw, sh, ow, oh, x0=None, y0=None, w=None, h=None):
        fx0, fy0, fw, fh, scale = fit_rect(sw, sh, ow, oh)
        self.sw, self.sh, self.ow, self.oh = sw, sh, ow, oh
        self.x0 = fx0 if x0 is None else x0
        self.y0 = fy0 if y0 is None else y0
        self.w = fw if w is None else w
        self.h = fh if h is None else h
        self.sx = self.w / sw
        self.sy = self.h / sh
        # whole output pixels inside the picture
        self.c0 = int(math.ceil(self.x0 - 1e-6))
        self.c1 = int(math.floor(self.x0 + self.w + 1e-6))
        self.r0 = int(math.ceil(self.y0 - 1e-6))
        self.r1 = int(math.floor(self.y0 + self.h + 1e-6))

    def cols(self, dx=0.0):
        return out_positions(self.c1 - self.c0, self.x0 - self.c0, self.sx, dx)

    def rows(self, dy=0.0):
        return out_positions(self.r1 - self.r0, self.y0 - self.r0, self.sy, dy)

    def to_out_x(self, sx):
        return self.x0 - self.c0 + (sx + 0.5) * self.sx - 0.5

    def to_out_y(self, sy):
        return self.y0 - self.r0 + (sy + 0.5) * self.sy - 0.5


def ideal_resample(plane, geo, chroma=False, dx=0.0, dy=0.0):
    """plane: luma (sh, sw) or co-sited chroma (sh, sw/2). Returns the picture-area output grid."""
    sy_pos = geo.rows(dy)
    My = ideal_matrix(plane.shape[0], sy_pos, min(0.5, 0.5 * geo.sy))
    sx_pos = geo.cols(dx)
    if chroma:
        Mx = ideal_matrix(plane.shape[1], sx_pos / 2.0, min(0.5, 0.5 * geo.sx * 2))
    else:
        Mx = ideal_matrix(plane.shape[1], sx_pos, min(0.5, 0.5 * geo.sx))
    return (My @ plane) @ Mx.T


# ---------------------------------------------------------------------------- control resamplers
def kernel_matrix(n_in, positions, scale, kind):
    """Separable practical resamplers for the validation controls."""
    stretch = 1.0 / scale if scale < 1 else 1.0
    if kind == "nearest":
        idx = np.clip(np.floor(positions + 0.5).astype(int), 0, n_in - 1)
        M = np.zeros((len(positions), n_in))
        M[np.arange(len(positions)), idx] = 1
        return M
    if kind == "bilinear_noaa":       # hardware bilinear: no prefilter when downscaling
        stretch, radius = 1.0, 1.0
        f = lambda t: np.clip(1 - np.abs(t), 0, None)
    elif kind in ("sinc", "sharpened"):   # Lanczos-4 windowed sinc, stretched for downscaling
        radius = 4.0
        f = lambda t: np.where(np.abs(t) < radius, np.sinc(t) * np.sinc(t / radius), 0.0)
    else:
        raise ValueError(kind)
    lo = np.floor(positions - radius * stretch).astype(int)
    taps = int(math.ceil(2 * radius * stretch)) + 2
    M = np.zeros((len(positions), n_in))
    for k in range(taps):
        n = lo + k
        t = (positions - n) / stretch
        wgt = f(t)
        nn = np.clip(n, 0, n_in - 1)
        np.add.at(M, (np.arange(len(positions)), nn), wgt)
    M /= M.sum(axis=1, keepdims=True)
    return M


def control_resample(plane, geo, kind, chroma=False):
    p = plane
    if kind == "sharpened":
        # deliberately sharpened pre-pass: unsharp mask, amount 1.5, sigma 1 source pixel
        from scipy.ndimage import gaussian_filter
        p = p + 1.5 * (p - gaussian_filter(p, 1.0, mode="wrap"))
    if kind == "ideal":
        return ideal_resample(p, geo, chroma)
    My = kernel_matrix(p.shape[0], geo.rows(), geo.sy, kind)
    if chroma:
        Mx = kernel_matrix(p.shape[1], geo.cols() / 2.0, geo.sx * 2, kind)
    else:
        Mx = kernel_matrix(p.shape[1], geo.cols(), geo.sx, kind)
    return (My @ p) @ Mx.T


# ---------------------------------------------------------------------------- capture handling
M709 = np.array([[1.0, 0.0, 1.5748], [1.0, -0.187324, -0.468124], [1.0, 1.8556, 0.0]])


def synth_capture(Yo, Cbo, Cro, geo, bits=10, rng=None):
    """A fake R10 capture (limited-range BT.709 RGB codes, black bars) of output planes."""
    rgb = np.einsum("ij,jhw->ihw", M709, np.stack([Yo, Cbo, Cro]))
    cap = np.zeros((3, geo.oh, geo.ow))
    cap[:] = 0.0
    cap[:, geo.r0:geo.r1, geo.c0:geo.c1] = rgb
    codes = 64 + 876 * cap
    if bits == 8:
        rng = rng or np.random.default_rng(5)
        tpdf = rng.random(codes.shape) - rng.random(codes.shape)
        codes = 4 * np.rint(codes / 4 + tpdf)
    return np.clip(np.rint(codes), 0, 1023).transpose(1, 2, 0).astype(np.uint16)


def load_capture(path, ow, oh):
    a = np.fromfile(path, dtype=np.uint16)
    if a.size != ow * oh * 3:
        raise ValueError("%s: %d values, expected %d" % (path, a.size, ow * oh * 3))
    return a.reshape(oh, ow, 3)


def detect_rect(cap):
    """The picture rectangle (sub-pixel) against the bars: 50% crossings of the mean level."""
    lum = cap.astype(float).mean(axis=2)
    def edges(profile):
        lo = np.percentile(profile, 1)
        hi = np.median(profile)
        if hi - lo < 20:
            return 0.0, float(len(profile))
        half = (lo + hi) / 2
        above = np.nonzero(profile > half)[0]
        a, b = above[0], above[-1]
        def cross(i0, i1):
            v0, v1 = profile[i0], profile[i1]
            return i0 + (half - v0) / (v1 - v0) if v1 != v0 else i0
        start = cross(a - 1, a) + 0.5 if a > 0 else 0.0
        end = cross(b, b + 1) + 0.5 if b + 1 < len(profile) else float(len(profile))
        return start, end
    # use the middle half of the other axis (the card's grey border runs all round)
    h, w = lum.shape
    x0, x1 = edges(lum[h // 4:3 * h // 4].mean(axis=0))
    y0, y1 = edges(lum[:, w // 4:3 * w // 4].mean(axis=1))
    return x0, y0, x1 - x0, y1 - y0


def capture_geometry(cap, sw, sh):
    ow, oh = cap.shape[1], cap.shape[0]
    fx0, fy0, fw, fh, _ = fit_rect(sw, sh, ow, oh)
    x0, y0, w, h = detect_rect(cap)
    # the detected rectangle must agree with the aspect-preserving fit (no stretch, no crop)
    ok = abs(w - fw) < 2.5 and abs(h - fh) < 2.5 and abs(x0 - fx0) < 2.5 and abs(y0 - fy0) < 2.5
    return Geometry(sw, sh, ow, oh), dict(detected=[x0, y0, w, h], fit=[fx0, fy0, fw, fh], ok=bool(ok))


def colour_fit(cap, geo, refs, sigma=3):
    """Least-squares affine map from the output RGB codes to the reference Y', Cb, Cr (low-passed),
    so range, matrix and primaries need no assumption."""
    from scipy.ndimage import gaussian_filter
    rgb = cap[geo.r0:geo.r1, geo.c0:geo.c1, :].astype(float)
    b = 6
    A = []
    for c in range(3):
        A.append(gaussian_filter(rgb[:, :, c], sigma)[b:-b:2, b:-b:2].ravel())
    A.append(np.ones_like(A[0]))
    A = np.stack(A, axis=1)
    coefs = []
    for ref in refs:
        t = gaussian_filter(ref, sigma)[b:-b:2, b:-b:2].ravel()
        coefs.append(np.linalg.lstsq(A, t, rcond=None)[0])
    out = []
    for cf in coefs:
        out.append(rgb[:, :, 0] * cf[0] + rgb[:, :, 1] * cf[1] + rgb[:, :, 2] * cf[2] + cf[3])
    return out


def colour_fit_rows(cap, geo, ref):
    """Luma fit on per-row means: shift-invariant for content periodic over the picture width."""
    rgb = cap[geo.r0:geo.r1, geo.c0:geo.c1, :].astype(float)
    A = np.concatenate([rgb.mean(axis=1), np.ones((rgb.shape[0], 1))], axis=1)
    cf = np.linalg.lstsq(A, ref.mean(axis=1), rcond=None)[0]
    return rgb[:, :, 0] * cf[0] + rgb[:, :, 1] * cf[1] + rgb[:, :, 2] * cf[2] + cf[3]


def subpixel_shift(o, r):
    """(dy, dx) of o relative to r by phase correlation with a parabolic peak."""
    F = np.fft.fft2(o - o.mean()) * np.conj(np.fft.fft2(r - r.mean()))
    F /= np.abs(F) + 1e-9
    c = np.real(np.fft.ifft2(F))
    iy, ix = np.unravel_index(np.argmax(c), c.shape)
    def para(a, b, d):
        den = a - 2 * b + d
        return 0.5 * (a - d) / den if abs(den) > 1e-12 else 0.0
    H, W = c.shape
    dy = iy + para(c[(iy - 1) % H, ix], c[iy, ix], c[(iy + 1) % H, ix])
    dx = ix + para(c[iy, (ix - 1) % W], c[iy, ix], c[iy, (ix + 1) % W])
    if dy > H / 2: dy -= H
    if dx > W / 2: dx -= W
    return dy, dx, float(c.max())


# ---------------------------------------------------------------------------- local spectral analysis
_HANN = None


def hann2():
    global _HANN
    if _HANN is None:
        h = np.hanning(WIN + 2)[1:-1]
        _HANN = np.outer(h, h)
    return _HANN


def windows(y0, y1, x0, x1):
    for y in range(int(y0), int(y1) - WIN + 1, STEP):
        for x in range(int(x0), int(x1) - WIN + 1, STEP):
            yield y, x


def cheb_freq():
    f = np.fft.fftfreq(WIN)
    return np.maximum(np.abs(f)[:, None], np.abs(f)[None, :]) / 0.5   # fraction of output Nyquist


def dilate(mask, n=2):
    out = mask.copy()
    for dy in range(-n, n + 1):
        for dx in range(-n, n + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def window_stack(img, y0, y1, x0, x1):
    """All analysis windows of a region as (n, WIN, WIN), mean removed, Hann applied."""
    ys = list(range(int(y0), int(y1) - WIN + 1, STEP))
    xs = list(range(int(x0), int(x1) - WIN + 1, STEP))
    if not ys or not xs:
        return np.zeros((0, WIN, WIN))
    v = np.lib.stride_tricks.sliding_window_view(img, (WIN, WIN))
    st = v[np.ix_(ys, xs)].reshape(-1, WIN, WIN)
    st = st - st.mean(axis=(1, 2), keepdims=True)
    return st * hann2()[None]


def window_energy(img, y0, y1, x0, x1, mask=None):
    P = np.abs(np.fft.fft2(window_stack(img, y0, y1, x0, x1))) ** 2
    return (P[:, mask] if mask is not None else P.reshape(len(P), -1)).sum(axis=1)


def spectra(img, y, x):
    p = img[y:y + WIN, x:x + WIN]
    return np.fft.fft2((p - p.mean()) * hann2())


def noise_psd(img, region):
    y0, y1, x0, x1 = region
    acc, n = np.zeros((WIN, WIN)), 0
    for y, x in windows(y0, y1, x0, x1):
        acc += np.abs(spectra(img, y, x)) ** 2
        n += 1
    return acc / max(n, 1)


def tile_metrics(O, R, region, N):
    """Aliased energy (output energy where the ideal has none), passband gain per frequency,
    in-band uncorrelated error; region in picture-grid pixels."""
    y0, y1, x0, x1 = region
    So = np.fft.fft2(window_stack(O, y0, y1, x0, x1))
    Sr = np.fft.fft2(window_stack(R, y0, y1, x0, x1))
    specs = list(zip(So, Sr))
    if not specs:
        return None
    T = 1e-3 * max(float((np.abs(r) ** 2).max()) for _, r in specs)
    fq = cheb_freq()
    alias = ref = err = 0.0
    gains = []
    for o, r in specs:
        Pr = np.abs(r) ** 2
        sup = dilate(Pr > T, 2)
        # output energy the ideal does not have: the residual outside the ideal's support
        alias += float((np.abs(o - r) ** 2 - N)[~sup].sum())
        ref += float(Pr.sum())
        core = Pr > T
        den = float(Pr[core].sum())
        if den > 0:
            g = float(np.real((o * np.conj(r))[core].sum()) / den)
            fc = float((Pr[core] * fq[core]).sum() / den)
            gains.append((fc, g))
            err += float((np.abs(o - g * r) ** 2 - N)[core].sum())
    return dict(alias=max(alias, 0.0) / ref, err=max(err, 0.0) / ref, gains=gains)


def db(x):
    return 10 * math.log10(max(x, 1e-9))


MTF_BANDS = [(0.2, 0.4), (0.4, 0.6), (0.6, 0.8), (0.8, 0.95)]


def mtf_summary(gains):
    out = {}
    for lo, hi in MTF_BANDS:
        g = [v for f, v in gains if lo <= f < hi]
        out["%.1f-%.2f" % (lo, hi)] = float(np.median(g)) if len(g) >= 3 else None
    return out


def ringing(O, rect_out, geo):
    """Overshoot beyond the plateaus within 8 output pixels from the rectangle's edges, % of the step."""
    x0, y0, x1, y1 = rect_out
    res = []
    yc0, yc1 = int(y0 + 0.25 * (y1 - y0)), int(y0 + 0.75 * (y1 - y0))
    xc0, xc1 = int(x0 + 0.25 * (x1 - x0)), int(x0 + 0.75 * (x1 - x0))
    prof_x = O[yc0:yc1, :].mean(axis=0)
    prof_y = O[:, xc0:xc1].mean(axis=1)
    for prof, a, b in ((prof_x, x0, x1), (prof_y, y0, y1)):
        idx = np.arange(len(prof))
        inside = prof[int(a + 0.3 * (b - a)):int(b - 0.3 * (b - a))]
        lin = float(np.median(inside))
        outside = np.concatenate([prof[max(0, int(a - 40)):int(a - 12)], prof[int(b + 12):int(b + 40)]])
        lout = float(np.median(outside))
        step = lin - lout
        if abs(step) < 1e-6:
            continue
        for e, sgn in ((a, 1), (b, -1)):
            d = (idx - e) * sgn            # > 0 inside
            # values inside the transition lie between the plateaus, so any excursion beyond
            # them (0..8 output pixels either side) is ringing / overshoot
            ins = (d >= 0) & (d <= 8)
            outs = (d <= 0) & (d >= -8)
            over = float(((prof[ins] - lin) / step).max()) if ins.any() else 0.0
            under = float(((lout - prof[outs]) / step).max()) if outs.any() else 0.0
            res.append(max(over, under, 0.0))
    return 100.0 * float(np.mean(res)) if res else None


# ---------------------------------------------------------------------------- still analysis
REFCACHE = None   # a folder for the ideal references (float64 .npy), set by analyse_one


def disk_cached(name, compute):
    """The ideal references cost seconds to minutes; computed once per pair and kept on disk."""
    if not REFCACHE:
        return compute()
    path = os.path.join(REFCACHE, name + ".npy")
    if os.path.exists(path):
        try:
            return np.load(path)
        except Exception:
            pass
    a = np.asarray(compute())
    os.makedirs(REFCACHE, exist_ok=True)
    tmp = path + ".%d.tmp" % os.getpid()
    with open(tmp, "wb") as f:
        np.save(f, a)
    os.replace(tmp, path)
    return a


def still_reference(sw, sh, geo, cache):
    key = (sw, sh, geo.ow, geo.oh, round(geo.x0, 3), round(geo.y0, 3), round(geo.w, 3), round(geo.h, 3))
    if key not in cache:
        Y, Cb, Cr, tiles = still_card(sw, sh)
        Yq = (to_ycode(Y).astype(float) - 64) / 876
        Cbq = (to_ccode(Cb).astype(float) - 512) / 896
        Crq = (to_ccode(Cr).astype(float) - 512) / 896
        ref = disk_cached("still_%dx%d_%dx%d" % (sw, sh, geo.ow, geo.oh), lambda: np.stack(
            [ideal_resample(Yq, geo), ideal_resample(Cbq, geo, True), ideal_resample(Crq, geo, True)]))
        cache[key] = dict(src=(Yq, Cbq, Crq), tiles=tiles, ref=[ref[0], ref[1], ref[2]])
    return cache[key]


def tile_region(t, geo, inset):
    x0, x1 = geo.to_out_x(t["x0"] - 0.5) + 0.5, geo.to_out_x(t["x1"] - 0.5) + 0.5
    y0, y1 = geo.to_out_y(t["y0"] - 0.5) + 0.5, geo.to_out_y(t["y1"] - 0.5) + 0.5
    i = inset * (x1 - x0)
    j = inset * (y1 - y0)
    return y0 + j, y1 - j, x0 + i, x1 - i


def analyse_still(cap, sw, sh, cache, align=True):
    geo, gcheck = capture_geometry(cap, sw, sh)
    st = still_reference(sw, sh, geo, cache)
    refs = st["ref"]
    O = colour_fit(cap, geo, refs)
    shifts = []
    if align:
        # residual sub-pixel misregistration per plane (chroma siting, kernel phase): measured
        # on the edge tile / chroma tiles and applied to the reference, then reported
        newrefs = []
        for i, (o, r) in enumerate(zip(O, refs)):
            kind = ["edge", "zone_cb", "hsweep_cr"][i]
            t = [tt for tt in st["tiles"] if tt["kind"] == kind][0]
            y0, y1, x0, x1 = [int(round(v)) for v in tile_region(t, geo, 0.05)]
            dy, dx, _ = subpixel_shift(o[y0:y1, x0:x1], r[y0:y1, x0:x1])
            if abs(dx) > 0.02 or abs(dy) > 0.02:
                src = st["src"][i]
                r = ideal_resample(src, geo, i > 0, dx=-dx / geo.sx, dy=-dy / geo.sy)
            newrefs.append(r)
            shifts.append([round(dx, 3), round(dy, 3)])
        refs = newrefs
    res = dict(geometry=gcheck, shifts=shifts, tiles={})
    flat = [t for t in st["tiles"] if t["kind"] == "flat"][0]
    freg = tile_region(flat, geo, 0.1)
    N = [noise_psd(o, freg) for o in O]
    res["noise_rms"] = [float(np.sqrt(n.sum() / (hann2() ** 2).sum()) / WIN) for n in N]
    gains = []
    for t in st["tiles"]:
        k = t["kind"]
        if k in ("flat", "patches"):
            continue
        reg = tile_region(t, geo, 0.03)
        if k == "edge":
            x0, y0, x1, y1 = t["rect"]
            ro = (geo.to_out_x(x0 - 0.5) + 0.5, geo.to_out_y(y0 - 0.5) + 0.5,
                  geo.to_out_x(x1 - 0.5) + 0.5, geo.to_out_y(y1 - 0.5) + 0.5)
            res["tiles"][k] = dict(ringing=ringing(O[0], ro, geo), ringing_ideal=ringing(refs[0], ro, geo))
            continue
        ch = 1 if k == "zone_cb" else 2 if k == "hsweep_cr" else 0
        m = tile_metrics(O[ch], refs[ch], reg, N[ch])
        if m is None:
            continue
        res["tiles"][k] = dict(alias_db=db(m["alias"]), err_db=db(m["err"]), mtf=mtf_summary(m["gains"]))
        if ch == 0 and k != "grain":
            gains += m["gains"]
    luma_alias = [res["tiles"][k]["alias_db"] for k in ("zone", "hsweep", "vsweep", "dsweep", "zone_grain", "hsweep_grain")]
    res["summary"] = dict(
        alias_db=db(np.mean([10 ** (v / 10) for v in luma_alias])),
        alias_chroma_db=db(np.mean([10 ** (res["tiles"][k]["alias_db"] / 10) for k in ("zone_cb", "hsweep_cr")])),
        grain_err_db=res["tiles"]["grain"]["err_db"],
        mtf=mtf_summary(gains),
        ringing=res["tiles"]["edge"]["ringing"],
        ringing_ideal=res["tiles"]["edge"]["ringing_ideal"],
        noise_rms=res["noise_rms"][0])
    return res


# ---------------------------------------------------------------------------- motion analysis
def analyse_shift(caps, sw, sh, cache):
    """Synthetic x-periodic pan: per capture, register against the ideal reference of the card
    (sub-pixel shift tau), then per band and window compare the output's local energy with the
    ideal's at the same tau. Shimmer = temporal std of that difference (noise-corrected),
    as a fraction of the source band's own energy."""
    geo, gcheck = capture_geometry(caps[0], sw, sh)
    key = ("mv", sw, sh, geo.ow, geo.oh)
    if key not in cache:
        Y, layout = moving_card(sw, sh, min(0.5, 0.5 * geo.sx))
        Yq = (to_ycode(Y).astype(float) - 64) / 876
        T = disk_cached("moving_T_%dx%d_%dx%d" % (sw, sh, geo.ow, geo.oh),
                        lambda: ideal_matrix(sh, geo.rows(), min(0.5, 0.5 * geo.sy)) @ Yq)
        cache[key] = dict(T=T, That=np.fft.fft(T, axis=1), layout=layout, Yq=Yq)
    mv = cache[key]
    W = sw
    kfreq = np.fft.fftfreq(W) * W
    cut = min(0.5, 0.5 * geo.sx)
    keep = np.abs(kfreq) < cut * W - 1e-9
    halfk = np.abs(np.abs(kfreq) - cut * W) < 1e-9
    wts = keep + 0.5 * halfk
    pos = geo.cols()
    E = np.exp(2j * math.pi * np.outer(kfreq[wts > 0], pos) / W) * wts[wts > 0][:, None] / W
    Th = mv["That"][:, wts > 0]

    if "R0" not in mv:
        mv["R0"] = np.real(Th @ E)
    R0 = mv["R0"]
    periodic = abs((geo.c1 - geo.c0) - W * geo.sx) < 1e-6 and abs(geo.x0 - geo.c0) < 1e-6
    R0hat = np.fft.fft(R0, axis=1)
    fo = np.fft.fftfreq(R0.shape[1])

    def reference(tau, rows=slice(None)):
        if periodic:
            # the card's period is exactly the picture width: an exact band-limited shift
            ph = np.exp(-2j * math.pi * fo * tau * geo.sx)
            return np.real(np.fft.ifft(R0hat[rows] * ph[None, :], axis=1))
        ph = np.exp(-2j * math.pi * kfreq[wts > 0] * tau / W)
        return np.real((Th[rows] * ph[None, :]) @ E)

    per_band = {b["kind"]: [] for b in mv["layout"]}
    taus = []
    src_energy = None
    for cap in caps:
        O = colour_fit_rows(cap, geo, R0)
        # coarse shift: phase correlation in x over the grain band (unique in the period)
        gb = [b for b in mv["layout"] if b["kind"] == "grain"][0]
        r0, r1 = int(geo.to_out_y(gb["y0"])) + 4, int(geo.to_out_y(gb["y1"] - 1)) - 4
        dy, dx, _ = subpixel_shift(O[r0:r1], R0[r0:r1])
        tau = dx / geo.sx
        # refine tau on the passband bands (least squares, golden search +-0.6 source px)
        rows_sel = slice(r0, r1)
        def cost(t):
            return float(((O[rows_sel] - reference(t, rows_sel)) ** 2).mean())
        a, b = tau - 0.6, tau + 0.6
        gr = (math.sqrt(5) - 1) / 2
        c, d = b - gr * (b - a), a + gr * (b - a)
        fc, fd = cost(c), cost(d)
        for _ in range(18):
            if fc < fd:
                b, d, fd = d, c, fc
                c = b - gr * (b - a); fc = cost(c)
            else:
                a, c, fc = c, d, fd
                d = a + gr * (b - a); fd = cost(d)
        tau = (a + b) / 2
        taus.append(tau)
        R = reference(tau)
        for bd in mv["layout"]:
            y0 = geo.to_out_y(bd["y0"]) + 6
            y1 = geo.to_out_y(bd["y1"] - 1) - 6
            per_band[bd["kind"]].append((window_energy(O, y0, y1, 8, R.shape[1] - 8),
                                         window_energy(R, y0, y1, 8, R.shape[1] - 8),
                                         window_energy(O - R, y0, y1, 8, R.shape[1] - 8)))
    # the source band's own window energy (amplitude AMP sinusoid)
    xs = np.arange(WIN)
    s = AMP * np.cos(2 * math.pi * 0.17 * xs)[None, :] * np.ones((WIN, 1))
    S = float((np.abs(np.fft.fft2((s - s.mean()) * hann2())) ** 2).sum())
    flat = per_band["flat"]
    flat_var = float(np.mean(np.var(np.stack([e for e, _, _ in flat]), axis=0))) if len(flat) > 1 else 0.0
    flat_err = float(np.mean([e.mean() for _, _, e in flat])) if flat else 0.0   # dither / noise floor
    out = dict(geometry=gcheck, taus=[round(t, 3) for t in taus], bands={})
    for name, series in per_band.items():
        if name == "flat" or len(series) < 2:
            continue
        D = np.stack([eo - er for eo, er, _ in series])        # (t, windows)
        var = np.var(D, axis=0).mean() - flat_var
        err = float(np.mean([e.mean() for _, _, e in series])) - flat_err
        out["bands"][name] = dict(shimmer=float(math.sqrt(max(var, 0.0)) / S),
                                  excess=float(D.mean() / S),
                                  motion_err=max(err, 0.0) / S)
    def cls(name):
        return {"p": "pass", "n": "near", "s": "stop", "d": "diag"}.get(name[0], "grain")
    groups = {}
    for name, v in out["bands"].items():
        groups.setdefault(cls(name), []).append(v["shimmer"])
    out["summary"] = {g: float(np.sqrt(np.mean(np.square(v)))) for g, v in groups.items()}
    out["summary"]["all"] = float(np.sqrt(np.mean([x ** 2 for v in groups.values() for x in v])))
    # motion error: energy of (output - ideal of the same motion) per band, the dither floor
    # removed, as a fraction of the source band's energy (dB); stop = moving moire / crawl
    eg = {}
    for name, v in out["bands"].items():
        eg.setdefault(cls(name), []).append(v["motion_err"])
    for g, v in eg.items():
        out["summary"]["err_" + g] = db(float(np.mean(v)))
    out["summary"]["err_all"] = db(float(np.mean([x for v in eg.values() for x in v])))
    return out


def analyse_seq(caps, frames, sw, sh, cache, asset):
    """Real pan (clip library): each capture registered to the nearest ideal-resampled source frame
    plus a sub-pixel x shift; in the fine-detail band, the error energy against that motion-matched
    ideal (dB) and the shimmer (temporal std of each window's relative band-energy error)."""
    geo, gcheck = capture_geometry(caps[0], sw, sh)
    # the clip's picture rows (bars are code 64 in the asset)
    act = np.nonzero(frames[0].mean(axis=1) > 70)[0]
    a0, a1 = act[0], act[-1] + 1
    key = ("seq", asset, geo.ow, geo.oh)
    if key not in cache:
        refs = disk_cached("seq_%s_%dx%d" % (os.path.splitext(asset)[0], geo.ow, geo.oh), lambda: np.stack(
            [ideal_resample((f.astype(float) - 64) / 876, geo) for f in frames]))
        cache[key] = refs
    refs = cache[key]
    ry0 = int(math.ceil(geo.to_out_y(a0))) + 8
    ry1 = int(math.floor(geo.to_out_y(a1 - 1))) - 8
    cx0, cx1 = 48, refs[0].shape[1] - 48
    # the fine-detail band: 0.3..0.95 of the highest frequency the source can carry at the output
    # (the output Nyquist, or below it when upscaling); real film has little energy above, where
    # the 8-bit dither would dominate
    lim = min(1.0, 1.0 / geo.sx)
    fq = cheb_freq()
    hb = (fq >= 0.3 * lim) & (fq <= 0.95 * lim)
    D, ER, EE = [], [], []
    picks = []
    fkey = key + ("fft", ry0, ry1)
    if fkey not in cache:   # frame selection on a 2x-decimated grid (the sub-pixel shift is full-res)
        cache[fkey] = [np.conj(np.fft.fft2(R[ry0:ry1:2, cx0:cx1:2] - R[ry0:ry1:2, cx0:cx1:2].mean())) for R in refs]
    rf = cache[fkey]
    for cap in caps:
        O = colour_fit(cap, geo, [refs[0], refs[0] * 0, refs[0] * 0], sigma=10)[0]
        Of = np.fft.fft2(O[ry0:ry1:2, cx0:cx1:2] - O[ry0:ry1:2, cx0:cx1:2].mean())
        best = None
        for i, Rf in enumerate(rf):
            F = Of * Rf
            F /= np.abs(F) + 1e-9
            peak = float(np.real(np.fft.ifft2(F)).max())
            if best is None or peak > best[0]:
                best = (peak, i)
        i = best[1]
        dy, dx, _ = subpixel_shift(O[ry0:ry1, cx0:cx1], refs[i][ry0:ry1, cx0:cx1])
        picks.append((i, round(dx, 2)))
        R = refs[i]
        f = np.fft.fftfreq(R.shape[1])
        Rs = np.real(np.fft.ifft(np.fft.fft(R, axis=1) * np.exp(-2j * math.pi * f * dx)[None, :], axis=1))
        O = colour_fit(cap, geo, [Rs, Rs * 0, Rs * 0], sigma=10)[0]   # refit against the matched frame
        eo = window_energy(O, ry0, ry1, cx0, cx1, hb)
        er = window_energy(Rs, ry0, ry1, cx0, cx1, hb)
        D.append(eo - er)
        ER.append(er)
        EE.append(window_energy(O - Rs, ry0, ry1, cx0, cx1, hb))
    D = np.stack(D)
    ER = np.stack(ER)
    EE = np.stack(EE)
    # shimmer: temporal std of each window's relative high-band energy error (floor: the median
    # window energy, so flat windows do not divide by ~0), averaged over windows
    floor = float(np.median(ER))
    rel = D / (ER + floor)
    return dict(geometry=gcheck, picks=picks,
                summary=dict(shimmer=float(np.std(rel, axis=0).mean()),
                             hf_excess=float(D.sum() / ER.sum()),
                             hf_err_db=db(float(EE.sum() / ER.sum()))))


# ---------------------------------------------------------------------------- stage configurations
SHARPEN = ("[shader.standard]\\nshader_type: standard\\n"
           "[shader.standard.adaptive_sharpen]\\nshader_type: custom\\nlabel: Adaptive sharpen\\n"
           "stage: post_resize\\nglsl_file: adaptive-sharpen.glsl\\n")


def base_extra(keys):
    # the base profile (no input rule): 8-bit output with VP's temporal sigma-delta, as the owner's
    # FL35 link (temporal_dither auto would leave libplacebo's 8-bit dither); a config's own key wins
    base = [("display_bit_depth", "8"), ("temporal_dither", "sigma_delta")]
    own = {k for k, _ in keys}
    lines = ["[vprenderer.Default]"] + ["%s: %s" % kv for kv in base if kv[0] not in own] + ["%s: %s" % kv for kv in keys]
    return "\\n".join(lines) + "\\n"


# name -> (config keys, applies-to: 'all' | 'down' | 'up', shader profiles)
CONFIGS = [
    ("default", [], "all", "-"),
    ("deband_off", [("deband", "off")], "all", "-"),
    ("deband_vp_on", [("deband_vp", "on")], "all", "-"),
    ("detail_pulse_off", [("detail_pulse_stabilise", "off")], "all", "-"),
    ("texture_restore_on", [("texture_restore", "statistical")], "all", "-"),
    ("rebase_1080", [("source_resolution", "1080")], "down4k", "-"),
    ("src_native", [("source_resolution", "native")], "all", "-"),
    ("hybrid_down_ssim", [("downscaler", "mitchell"), ("adaptive_downscale", "on")], "down", "standard.ssim_downscaler"),
    ("down_lanczos", [("downscaler", "lanczos")], "down", "-"),
    ("down_mitchell", [("downscaler", "mitchell")], "down", "-"),
    ("down_gamma", [("downscale_light", "gamma")], "down", "-"),
    ("hybrid_up_fsrcnnx", [("adaptive_upscale", "on")], "up", "standard.cnn,standard.ssim_superres"),
    ("up_ewa_lanczossharp", [("upscaler", "ewa_lanczossharp")], "up", "-"),
    ("up_lanczos", [("upscaler", "lanczos")], "up", "-"),
    ("up_gamma", [("upscale_light", "gamma")], "up", "-"),
    ("motion_on", [("interpolation", "always")], "all", "-"),
    ("sigma_off", [("temporal_dither", "off")], "all", "-"),
    ("dither_libplacebo", [("temporal_dither", "libplacebo")], "all", "-"),
    ("dither_off_all", [("temporal_dither", "off"), ("dithering", "off")], "all", "-"),
    ("all_detail_off", [("deband", "off"), ("deband_vp", "off"), ("detail_pulse_stabilise", "off"),
                        ("texture_restore", "off"), ("source_resolution", "native")], "all", "-"),
    ("all_detail_on", [("deband_vp", "on"), ("detail_pulse_stabilise", "on"), ("texture_restore", "statistical"),
                       ("interpolation", "always")], "all", "-"),
    # known-bad controls inside the chain (validate the capture path)
    ("ctl_down_gpu_noaa", [("downscaler", "gpu")], "down", "-"),
    ("ctl_up_nearest", [("upscaler", "nearest")], "up", "-"),
]
MOTION_CONFIGS = {"default", "deband_vp_on", "detail_pulse_off", "texture_restore_on", "rebase_1080", "hybrid_down_ssim",
                  "hybrid_up_fsrcnnx", "motion_on", "sigma_off", "all_detail_off", "all_detail_on",
                  "ctl_down_gpu_noaa", "ctl_up_nearest", "down_lanczos", "up_ewa_lanczossharp"}
PAN_SPEEDS = [3, 8]             # source pixels per 24p frame (synthetic): 72 and 192 px/s at 24 fps
REAL_PANS = ["pan_120pxs_23976p", "pan_480pxs_23976p"]


# Hybrid scaler blend sweep (owner 2026-10-18): VP's adaptive scalers with their branch shader.
# Downscale: out = faithful + w (SSIM - faithful), w = adaptive_w_base x (1 - edge) x grain term
# (SSimDownscaler after Mitchell, as the cinema configuration); upscale: out = cnn + (1 - w)
# (faithful - cnn), w = adaptive_upscale_w_base x grain term (FSRCNNX doubler branch).
BLEND_WEIGHTS = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
BLEND_PAIRS = [((3840, 2160), (2560, 1600)), ((3840, 2160), (1920, 1080)), ((1920, 1080), (2560, 1600))]
SHADER_SECTIONS = {
    "standard.ssim_downscaler": "[shader.standard.ssim_downscaler]\\nshader_type: custom\\nlabel: SSimDownscaler\\n"
                                "stage: post_resize\\norder: 20\\nglsl_file: SSimDownscaler.glsl\\n",
    # FSRCNNX: the luma doubler the renderer's own CNN test drives (ArtCNN did not engage in the
    # off-screen harness: the adaptive upscale logged prescale ratio 1)
    "standard.cnn": "[shader.standard.cnn]\\nshader_type: custom\\nlabel: FSRCNNX\\nstage: pre_resize\\n"
                    "order: 10\\nglsl_file: FSRCNNX_x2_16-0-4-1.glsl\\n",
}


def shader_extra(profiles):
    return "[shader.standard]\\ntype: multi\\nlabel: Off\\n" + "".join(SHADER_SECTIONS[p] for p in profiles)


def blend_rows():
    rows = []
    for src, out in BLEND_PAIRS:
        pn = pair_name(src, out)
        down = fit_rect(src[0], src[1], out[0], out[1])[4] < 1
        for w in BLEND_WEIGHTS:
            if down:
                keys = [("downscaler", "mitchell"), ("adaptive_downscale", "on"), ("adaptive_w_base", "%.2f" % w)]
                prof = ["standard.ssim_downscaler"]
                name = "hyb_down_w%.2f" % w
            else:
                keys = [("adaptive_upscale", "on"), ("adaptive_upscale_w_base", "%.2f" % w)]
                prof = ["standard.cnn", "standard.ssim_superres"]
                name = "hyb_fsrcnnx_w%.2f" % w
            extra = base_extra(keys) + shader_extra(prof)
            common = (src[0], src[1], out[0], out[1])
            rows.append([pn + "__bstill__" + name, *common, "still_%dx%d.u16" % src, "still", 0, 4.4,
                         "2.6,3.0,3.4,3.8", extra, ",".join(prof)])
            rows.append([pn + "__bshift8__" + name, *common, "moving_%dx%d_%dx%d.u16" % (src + out), "shift", 8, 5.4,
                         ",".join("%.2f" % (2.4 + 0.27 * i) for i in range(10)), extra, ",".join(prof)])
    return rows


SHADER_SECTIONS["standard.ssim_superres"] = ("[shader.standard.ssim_superres]\\nshader_type: custom\\nlabel: SSimSuperRes\\n"
                                             "stage: post_resize\\norder: 25\\nglsl_file: SSimSuperRes.glsl\\n")


def cost_rows(sources, outputs):
    """GPU cost per scaler configuration (no captures: the readback would add GPU work); the
    renderer's D3D11 timestamp render-load window is logged at the end of each run."""
    rows = []
    for src in sources:
        for out in outputs:
            pn = pair_name(src, out)
            s = fit_rect(src[0], src[1], out[0], out[1])[4]
            cfgs = [("default", [], [])]
            if s < 1:
                cfgs += [("sep_lanczos", [("downscaler", "lanczos")], []),
                         ("sep_mitchell", [("downscaler", "mitchell")], []),
                         ("hyb_down_ssim", [("downscaler", "mitchell"), ("adaptive_downscale", "on")], ["standard.ssim_downscaler"])]
            elif s > 1:
                cfgs += [("sep_lanczos", [("upscaler", "lanczos")], []),
                         ("polar_ewa_lanczossharp", [("upscaler", "ewa_lanczossharp")], []),
                         ("hyb_up_fsrcnnx_superres", [("adaptive_upscale", "on")], ["standard.cnn", "standard.ssim_superres"])]
            if src[1] > 1080 and s < 1:
                cfgs.append(("rebase_1080", [("source_resolution", "1080")], []))
            for name, keys, prof in cfgs:
                extra = base_extra(keys) + (shader_extra(prof) if prof else "")
                rows.append([pn + "__cost__" + name, src[0], src[1], out[0], out[1], "still_%dx%d.u16" % src, "still", 0, 6.0,
                             "", extra, ",".join(prof) if prof else "-"])
    return rows


def cost_report(d):
    import re
    lines = ["| source -> output | scaler configuration | GPU ms/frame (avg) | peak | core render ms (avg) | GPU frames |",
             "|---|---|---|---|---|---|"]
    for row in read_plan(d):
        if "__cost__" not in row["name"]:
            continue
        path = os.path.join(d, "captures", row["name"] + ".log")
        if not os.path.exists(path):
            continue
        m = re.search(r"gpu_timing: valid=(\d) frame_ms_avg=([\d.e+-]+) frame_ms_peak=([\d.e+-]+) core_ms_avg=([\d.e+-]+) "
                      r"core_ms_peak=([\d.e+-]+) frames=(\d+)", open(path, errors="replace").read())
        if not m:
            continue
        pair, _, cfg = row["name"].split("__")
        lines.append("| %dx%d -> %dx%d | %s | %.2f | %.2f | %.2f | %s |" % (
            row["sw"], row["sh"], row["ow"], row["oh"], cfg, float(m.group(2)), float(m.group(3)), float(m.group(4)), m.group(6)))
    return "\n".join(lines) + "\n"


def applies(scope, src, out):
    down = out[0] < src[0] or out[1] < src[1]
    s = min(out[0] / src[0], out[1] / src[1])
    if scope == "all":
        return True
    if scope == "down":
        return s < 1
    if scope == "up":
        return s > 1
    if scope == "down4k":
        return src[1] > 1080 and s < 1
    return False


def prepare(args):
    d = args.dir
    os.makedirs(d, exist_ok=True)
    sources = parse_sizes(os.environ.get("VP_ALIAS_SOURCES", DEFAULT_SOURCES))
    outputs = parse_sizes(os.environ.get("VP_ALIAS_OUTPUTS", DEFAULT_OUTPUTS))
    rows = []
    for sw, sh in ([] if args.plan_only else sources):
        Y, Cb, Cr, _ = still_card(sw, sh)
        np.concatenate([to_ycode(Y).ravel(), to_ccode(Cb).ravel(), to_ccode(Cr).ravel()]).tofile(
            os.path.join(d, "still_%dx%d.u16" % (sw, sh)))
        for ow, oh in outputs:
            Ym, _ = moving_card(sw, sh, pair_cut(sw, sh, ow, oh))
            np.concatenate([to_ycode(Ym).ravel(), np.full(sw * sh, 512, np.uint16)]).tofile(
                os.path.join(d, "moving_%dx%d_%dx%d.u16" % (sw, sh, ow, oh)))
    if not args.no_real and not args.plan_only:
        for clip in REAL_PANS:
            path = os.path.join(d, "%s_1920x1080.u16" % clip)
            if not os.path.exists(path):
                make_real_pan(clip, path)
    for src in sources:
        for out in outputs:
            pn = pair_name(src, out)
            for name, keys, scope, shaders in CONFIGS:
                if not applies(scope, src, out):
                    continue
                extra = base_extra(keys) + (shader_extra(shaders.split(",")) if shaders != "-" else "")
                common = (src[0], src[1], out[0], out[1])
                rows.append([pn + "__still__" + name, *common, "still_%dx%d.u16" % src, "still", 0, 4.4,
                             "2.6,3.0,3.4,3.8", extra, shaders])
                if name in MOTION_CONFIGS:
                    for sp in PAN_SPEEDS:
                        rows.append([pn + "__shift%d__" % sp + name, *common, "moving_%dx%d_%dx%d.u16" % (src + out), "shift", sp, 5.4,
                                     ",".join("%.2f" % (2.4 + 0.27 * i) for i in range(10)), extra, shaders])
                    if src == (1920, 1080) and not args.no_real:
                        for clip in REAL_PANS:
                            rows.append([pn + "__real_%s__" % clip.split("_")[1] + name, *common, "%s_1920x1080.u16" % clip,
                                         "seq", 0, 5.4, ",".join("%.2f" % (2.4 + 0.27 * i) for i in range(10)), extra, shaders])
    rows += blend_rows()
    rows += cost_rows(sources, outputs)
    with open(os.path.join(d, "plan.tsv"), "w", newline="\n") as f:
        f.write("# name\tsw\tsh\tow\toh\tasset\tmode\tshift\tseconds\tcaptures\textra\tshaders\n")
        for r in rows:
            f.write("\t".join(str(v) for v in r) + "\n")
    print("plan: %d runs, sources %s, outputs %s -> %s" % (len(rows), sources, outputs, d))


def make_real_pan(clip, path, frames=24, start=96):
    """24 frames of a clip-library pan (1920x800 film inside a 1920x1080 frame, bars code 64)."""
    man = json.load(open(r"D:\testclips\manifest.json"))
    ff = man["ffmpeg"]
    entry = [c for c in man["clips"] if c["id"] == clip][0]
    src = os.path.join(r"D:\testclips", entry["path"])
    w, h = entry["width"], entry["height"]
    cmd = [ff, "-v", "error", "-i", src, "-vf", "select=gte(n\\,%d)" % start, "-frames:v", str(frames),
           "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    y = np.frombuffer(raw, np.uint8).reshape(frames, h, w).astype(np.uint16) * 4   # 8-bit limited -> 10-bit
    out = np.full((frames, 1080, 1920), 64, np.uint16)
    top = (1080 - h) // 2
    out[:, top:top + h, :w] = y
    out.tofile(path)


# ---------------------------------------------------------------------------- validation (controls)
def validate(args):
    sources = parse_sizes(os.environ.get("VP_ALIAS_SOURCES", DEFAULT_SOURCES))
    outputs = parse_sizes(os.environ.get("VP_ALIAS_OUTPUTS", DEFAULT_OUTPUTS))
    kinds = ["ideal", "sinc", "nearest", "bilinear_noaa", "sharpened"]
    results = {}
    ok_all = True
    cache = {}
    for src in sources:
        Y, Cb, Cr, _ = still_card(*src)
        Yq = (to_ycode(Y).astype(float) - 64) / 876
        Cbq = (to_ccode(Cb).astype(float) - 512) / 896
        Crq = (to_ccode(Cr).astype(float) - 512) / 896
        for out in outputs:
            geo = Geometry(src[0], src[1], out[0], out[1])
            pn = pair_name(src, out)
            results[pn] = {}
            for k in kinds:
                for bits in (10, 8):
                    if bits == 8 and k not in ("ideal", "sinc"):
                        continue
                    t0 = time.time()
                    planes = [control_resample(p, geo, k, ch) for p, ch in ((Yq, False), (Cbq, True), (Crq, True))]
                    cap = synth_capture(*planes, geo, bits)
                    r = analyse_still(cap, src[0], src[1], cache)
                    results[pn]["%s_%d" % (k, bits)] = r["summary"]
                    print("%s %-14s %2d-bit alias %6.1f dB  chroma %6.1f dB  grain-err %6.1f dB  ringing %5.1f%%  mtf %s  (%.1fs)" % (
                        pn, k, bits, r["summary"]["alias_db"], r["summary"]["alias_chroma_db"], r["summary"]["grain_err_db"],
                        r["summary"]["ringing"] or -1, {a: (None if b is None else round(b, 2)) for a, b in r["summary"]["mtf"].items()},
                        time.time() - t0), flush=True)
            # the metric must separate them: bad controls >= 6 dB worse than the good reference
            g = results[pn]["sinc_10"]["alias_db"]
            i = results[pn]["ideal_10"]["alias_db"]
            if abs(geo.sx - 1) < 1e-9:
                # 1:1 (e.g. 1080 on 1080, or 1080 letterboxed in 1200): no resampling, every
                # control is the identity and must match the ideal
                # (chroma is still upsampled 2x horizontally from 4:2:2, so the chroma controls differ)
                lum = [results[pn]["%s_10" % k]["alias_db"] for k in ("ideal", "sinc", "nearest", "bilinear_noaa")]
                checks = {"luma_identity_at_floor": max(lum) < -60, "ideal_near_floor": i < -40,
                          "chroma_nearest_worse": results[pn]["nearest_10"]["alias_chroma_db"] >
                          results[pn]["sinc_10"]["alias_chroma_db"] + 6,
                          "sharpened_detected": results[pn]["sharpened_10"]["alias_db"] > -40}
                results[pn]["checks"] = checks
                print(pn, "checks:", checks, flush=True)
                ok_all &= all(checks.values())
                continue
            checks = {
                "ideal_near_floor": i < -40,
                "good_near_ideal": g < -25,
                "nearest_worse": results[pn]["nearest_10"]["alias_db"] > g + 6,
                "bilinear_noaa_worse": results[pn]["bilinear_noaa_10"]["alias_db"] > g + 6 or out[0] > src[0],
                "sharpened_rings": (results[pn]["sharpened_10"]["ringing"] or 0) > (results[pn]["sinc_10"]["ringing"] or 0) + 5,
                "8bit_dither_tolerated": results[pn]["sinc_8"]["alias_db"] < max(g, -40) + 3,
            }
            # motion: the x-periodic card panned to sub-pixel positions, each control resampled
            Ym, _ = moving_card(src[0], src[1], pair_cut(src[0], src[1], out[0], out[1]))
            Ymq = (to_ycode(Ym).astype(float) - 64) / 876
            kx = np.fft.fftfreq(src[0])
            Fm = np.fft.fft(Ymq, axis=1)
            rng = np.random.default_rng(11)
            taus = rng.uniform(0, 40, 8)
            mres = {}
            for k in ("ideal", "sinc", "nearest", "bilinear_noaa"):
                caps = []
                for tau in taus:
                    sh_ = np.real(np.fft.ifft(Fm * np.exp(-2j * math.pi * kx * tau)[None, :], axis=1))
                    yo = control_resample(sh_, geo, k)
                    caps.append(synth_capture(yo, yo * 0, yo * 0, geo, 8))
                m = analyse_shift(caps, src[0], src[1], cache)
                mres[k] = m["summary"]
                print("%s %-14s motion shimmer %s" % (pn, k, {a: round(b, 4) for a, b in m["summary"].items()}), flush=True)
            results[pn]["motion"] = mres
            checks["motion_ideal_small"] = mres["ideal"]["all"] < 0.02 and mres["ideal"]["err_all"] < -40
            if abs(geo.sx - 1) > 1e-9:
                checks["motion_nearest_worse"] = mres["nearest"]["err_all"] > mres["sinc"]["err_all"] + 6
            results[pn]["checks"] = checks
            print(pn, "checks:", checks, flush=True)
            ok_all &= all(checks.values())
    with open(os.path.join(args.dir, "validation.json"), "w") as f:
        json.dump(results, f, indent=1)
    print("VALIDATION", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


# ---------------------------------------------------------------------------- analysis of captures
def read_plan(d):
    rows = []
    for line in open(os.path.join(d, "plan.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        rows.append(dict(name=f[0], sw=int(f[1]), sh=int(f[2]), ow=int(f[3]), oh=int(f[4]), asset=f[5], mode=f[6]))
    return rows


_WCACHE = {}
_WSEQ = {}


def analyse_one(d, row, files):
    """One run's captures -> metrics (a worker process keeps its own reference cache)."""
    global REFCACHE
    REFCACHE = os.environ.get("VP_ALIAS_REFCACHE") or os.path.join(d, "refcache")
    cap_dir = os.path.join(d, "captures")
    caps = [load_capture(os.path.join(cap_dir, f), row["ow"], row["oh"]) for f in files]
    # The owner's link is 8-bit: a capture with 10-bit codes (VP's 10-bit swapchain without the
    # sigma-delta) is rounded to 8-bit codes, as the link would truncate it (the sigma-delta's
    # output is already on 8-bit codes).
    mid = caps[0][caps[0].shape[0] // 2]
    link8 = bool(np.any(mid % 4))
    if link8:
        caps = [(np.rint(c / 4.0) * 4).clip(0, 1020).astype(np.uint16) for c in caps]
    t0 = time.time()
    try:
        if row["mode"] == "still":
            avg = np.mean(np.stack([c.astype(float) for c in caps]), axis=0)
            info = analyse_still(avg, row["sw"], row["sh"], _WCACHE)
            info["codes_mod4"] = [int(v) for v in sorted(set(np.unique(mid[:, 1] % 4)))]
            info["captures"] = len(caps)
        elif row["mode"] == "shift":
            info = analyse_shift(caps, row["sw"], row["sh"], _WCACHE)
        else:
            if row["asset"] not in _WSEQ:
                a = np.fromfile(os.path.join(d, row["asset"]), np.uint16)
                _WSEQ[row["asset"]] = a.reshape(-1, row["sh"], row["sw"])
            info = analyse_seq(caps, _WSEQ[row["asset"]], row["sw"], row["sh"], _WCACHE, row["asset"])
        info["mode"] = row["mode"]
        info["link8_rounded"] = link8
    except Exception as e:  # keep going; the report lists failures
        info = dict(mode=row["mode"], error=repr(e))
    info["analysis_s"] = round(time.time() - t0, 1)
    return row["name"], files, info


def precompute_pair(d, src, out, assets):
    """The ideal references of one source x output pair, into <dir>/refcache."""
    global REFCACHE
    REFCACHE = os.environ.get("VP_ALIAS_REFCACHE") or os.path.join(d, "refcache")
    geo = Geometry(src[0], src[1], out[0], out[1])
    t0 = time.time()
    still_reference(src[0], src[1], geo, {})
    Y, _ = moving_card(src[0], src[1], min(0.5, 0.5 * geo.sx))
    Yq = (to_ycode(Y).astype(float) - 64) / 876
    disk_cached("moving_T_%dx%d_%dx%d" % (src[0], src[1], out[0], out[1]),
                lambda: ideal_matrix(src[1], geo.rows(), min(0.5, 0.5 * geo.sy)) @ Yq)
    for asset in assets:
        frames = np.fromfile(os.path.join(d, asset), np.uint16).reshape(-1, src[1], src[0])
        disk_cached("seq_%s_%dx%d" % (os.path.splitext(asset)[0], out[0], out[1]), lambda: np.stack(
            [ideal_resample((f.astype(float) - 64) / 876, geo) for f in frames]))
    return "%s %.0fs" % (pair_name(src, out), time.time() - t0)


def precompute(args):
    from concurrent.futures import ProcessPoolExecutor
    pairs = {}
    for row in read_plan(args.dir):
        k = ((row["sw"], row["sh"]), (row["ow"], row["oh"]))
        pairs.setdefault(k, set())
        if row["mode"] == "seq":
            pairs[k].add(row["asset"])
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        for r in pool.map(precompute_pair, [args.dir] * len(pairs), [k[0] for k in pairs], [k[1] for k in pairs],
                          [sorted(v) for v in pairs.values()]):
            print("references:", r, flush=True)


def pending_runs(d, results, reanalyse):
    cap_dir = os.path.join(d, "captures")
    if not os.path.isdir(cap_dir):
        return []
    listing = os.listdir(cap_dir)
    out = []
    for row in read_plan(d):
        name = row["name"]
        if name in results and not reanalyse:
            continue
        # only runs whose capture is complete (the .done marker is written after the files)
        if name + ".done" not in listing:
            continue
        files = sorted([f for f in listing if f.startswith(name + "__") and f.endswith(".rgb10")],
                       key=lambda s: int(s.rsplit("__", 1)[1].split(".")[0]))
        if files:
            out.append((row, files))
    return out


def analyse(args, delete=False):
    """Analyse every completed run's captures (a pool of --workers processes); with --watch keep
    going until <dir>/captures/STOP exists and nothing is pending."""
    from concurrent.futures import ProcessPoolExecutor
    d = args.dir
    res_path = os.path.join(d, args.results)
    results = json.load(open(res_path)) if os.path.exists(res_path) else {}
    delete = delete or args.delete
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        while True:
            todo = pending_runs(d, results, args.reanalyse)
            if args.reanalyse:
                args.reanalyse = False
            futures = [pool.submit(analyse_one, d, row, files) for row, files in todo]
            for fut in futures:
                name, files, info = fut.result()
                results[name] = info
                print("%-60s %s (%.1fs)" % (name, json.dumps(info.get("summary", info.get("error")))[:150],
                                            info["analysis_s"]), flush=True)
                if delete:
                    for f in files:
                        os.remove(os.path.join(d, "captures", f))
                with open(res_path, "w") as f:
                    json.dump(results, f)
            if not args.watch:
                break
            if not todo:
                if os.path.exists(os.path.join(d, "captures", "STOP")):
                    break
                time.sleep(20)
    return results


def gpu_tenants(tag):
    """Other agents' GPU work: any process (not a shell or this suite) whose command line contains
    one of the '|'-separated tags."""
    parts = [t for t in tag.split("|") if t]
    q = "Get-CimInstance Win32_Process | Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress"
    try:
        procs = json.loads(subprocess.run(["powershell", "-NoProfile", "-Command", q],
                                          capture_output=True, text=True).stdout or "[]")
    except ValueError:
        return []
    busy = []
    for p in procs:
        cl, name = p.get("CommandLine") or "", (p.get("Name") or "").lower()
        if name.startswith(("powershell", "bash", "sh.exe", "conhost")) or "alias_suite" in cl:
            continue
        if any(t in cl for t in parts):
            busy.append(str(p["ProcessId"]))
    return busy


def wait_for_gpu(tag):
    """GPU etiquette: wait while another agent's GPU work (see gpu_tenants) runs."""
    if not tag:
        return
    waited = 0
    while True:
        busy = gpu_tenants(tag)
        if not busy:
            break
        if waited % 300 == 0:
            print("waiting for the GPU (%s: pids %s, %d s)" % (tag, ",".join(busy), waited), flush=True)
        time.sleep(30)
        waited += 30


def run(args):
    """Batches per source x output pair: captures (vstest), then analysis, raw captures deleted."""
    d = args.dir
    rows = read_plan(d)
    pairs = []
    for r in rows:
        p = r["name"].split("__")[0]
        if p not in pairs:
            pairs.append(p)
    groups = []
    for p in pairs:
        for kind in ("__still__", "__shift", "__real_", "__bstill__", "__bshift", "__cost__"):
            if any(r["name"].startswith(p + kind) for r in rows):
                groups.append(p + kind)
    if args.only:
        groups = [g for g in groups if args.only in g]
    env = dict(os.environ)
    env["VP_ALIAS_DIR"] = os.path.abspath(d)
    env["VP_TEST_GPU"] = "hardware"
    # the renderer finds the bundled shaders (sigma-delta, SSIM, FSRCNNX) beside the test binaries
    env["VP_TEST_SHADER_DIR"] = os.path.abspath(args.renderer_bin or args.bin)
    if args.renderer_bin:
        # another build's renderer DLL (and shaders), driven by this build's test harness
        env["VP_TEST_RENDERER_DIR"] = os.path.abspath(args.renderer_bin)
    if args.adapter:
        env["VP_TEST_ADAPTER"] = args.adapter   # e.g. NVIDIA: the off-screen window would otherwise use the panel's iGPU
    for g in groups:
        wait_for_gpu(args.wait_for)
        env["VP_ALIAS_FILTER"] = g
        trx_dir = os.path.join(d, "trx")
        os.makedirs(trx_dir, exist_ok=True)
        cmd = [args.vstest, os.path.join(args.bin, "VideoProcessor-Test.dll"),
               "/Tests:AliasingSuiteCaptures", "/Logger:trx;LogFileName=%s.trx" % g.strip("_"),
               "/ResultsDirectory:%s" % trx_dir]
        t0 = time.time()
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        print("captures %s: rc %d (%.0fs)" % (g, p.returncode, time.time() - t0), flush=True)
        if not args.no_analyse:
            analyse(args, delete=True)


# ---------------------------------------------------------------------------- report and gate
def cfg_of(name):
    return name.split("__")[2]


def summarise(results):
    """table[pair][config] = metrics (still + motion merged)."""
    table = {}
    for name, r in results.items():
        if "error" in r:
            continue
        pair, kind, cfg = name.split("__")
        t = table.setdefault(pair, {}).setdefault(cfg, {})
        s = r.get("summary", {})
        if r["mode"] == "still":
            t["alias_db"] = s["alias_db"]
            t["alias_chroma_db"] = s["alias_chroma_db"]
            t["grain_err_db"] = s["grain_err_db"]
            t["mtf_0.6"] = s["mtf"].get("0.6-0.80")
            t["mtf_0.8"] = s["mtf"].get("0.8-0.95")
            t["mtf_0.4"] = s["mtf"].get("0.4-0.60")
            t["ringing"] = s["ringing"]
            t["noise"] = s["noise_rms"]
            t["geometry_ok"] = r["geometry"]["ok"]
            t["codes_mod4"] = r.get("codes_mod4")
        elif r["mode"] == "shift":
            t.setdefault("shimmer", []).append(s["all"])
            t.setdefault("motion_err", []).append(s.get("err_all"))
            t.setdefault("motion_err_stop", []).append(s.get("err_stop"))
            t.setdefault("motion_err_near", []).append(s.get("err_near"))
        else:
            t.setdefault("real_shimmer", []).append(s["shimmer"])
            t.setdefault("real_hf_excess", []).append(s["hf_excess"])
            t.setdefault("real_hf_err", []).append(s.get("hf_err_db"))
    for pair in table.values():
        for t in pair.values():
            for k in ("shimmer", "motion_err", "motion_err_stop", "motion_err_near", "real_shimmer", "real_hf_excess", "real_hf_err"):
                if k in t:
                    v = [x for x in t[k] if x is not None]
                    t[k] = float(np.mean(v)) if v else None
    return table


def fmt(v, f="%.1f"):
    return "-" if v is None else f % v


def verdict(t, base):
    """help / hurt / neutral against default on aliasing, shimmer and passband."""
    notes = []
    if t.get("alias_db") is not None and base.get("alias_db") is not None:
        d = t["alias_db"] - base["alias_db"]
        if d > 1.5: notes.append("alias +%.1f dB" % d)
        elif d < -1.5: notes.append("alias %.1f dB" % d)
    if t.get("motion_err") is not None and base.get("motion_err") is not None:
        d = t["motion_err"] - base["motion_err"]
        if abs(d) > 1.5: notes.append("pan err %+.1f dB" % d)
    if t.get("real_shimmer") and base.get("real_shimmer"):
        r = t["real_shimmer"] / base["real_shimmer"]
        if r > 1.25 or r < 0.8: notes.append("real-pan shimmer x%.2f" % r)
    if t.get("mtf_0.6") is not None and base.get("mtf_0.6") is not None:
        d = t["mtf_0.6"] - base["mtf_0.6"]
        if abs(d) > 0.03: notes.append("MTF0.6 %+.2f" % d)
    if t.get("ringing") is not None and base.get("ringing") is not None:
        d = t["ringing"] - base["ringing"]
        if abs(d) > 1.5: notes.append("ringing %+.1f%%" % d)
    return ", ".join(notes) or "neutral"


def report(args):
    results = json.load(open(os.path.join(args.dir, args.results)))
    table = summarise(results)
    lines = []
    for pair in sorted(table, key=lambda p: (p.split("_o")[0], p)):
        rows = table[pair]
        base = rows.get("default", {})
        src, out = pair.split("_o")
        s = [int(v) for v in src[1:].split("x")]
        o = [int(v) for v in out.split("x")]
        x0, y0, w, h, sc = fit_rect(s[0], s[1], o[0], o[1])
        lines.append("\n### %s -> %s (scale x%.3f, picture %gx%g)\n" % (src[1:], out, sc, w, h))
        lines.append("| stage config | alias dB | chroma alias dB | grain err dB | MTF 0.4-0.6 | MTF 0.6-0.8 | MTF 0.8-0.95 | ringing % | pan err dB | pan moire (stop) dB | shimmer | real-pan HF err dB | real-pan shimmer | vs default |")
        lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        order = [c[0] for c in CONFIGS]
        for cfg in sorted(rows, key=lambda c: order.index(c) if c in order else 99):
            t = rows[cfg]
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                cfg, fmt(t.get("alias_db")), fmt(t.get("alias_chroma_db")), fmt(t.get("grain_err_db")),
                fmt(t.get("mtf_0.4"), "%.2f"), fmt(t.get("mtf_0.6"), "%.2f"), fmt(t.get("mtf_0.8"), "%.2f"),
                fmt(t.get("ringing")), fmt(t.get("motion_err")), fmt(t.get("motion_err_stop")), fmt(t.get("shimmer"), "%.4f"),
                fmt(t.get("real_hf_err")), fmt(t.get("real_shimmer"), "%.3f"), "baseline" if cfg == "default" else verdict(t, base)))
    text = "\n".join(lines) + "\n\n### GPU cost per frame (D3D11 timestamps)\n\n" + cost_report(args.dir)
    with open(os.path.join(args.dir, args.results.replace(".json", ".md")), "w") as f:
        f.write(text)
    print(text)


def gate(args):
    """Thresholds (alias_thresholds.json): per pair, the default chain must stay within its
    recorded baseline + margin; the in-chain known-bad controls must stay clearly worse (the
    capture path still sees aliasing)."""
    results = json.load(open(os.path.join(args.dir, args.results)))
    table = summarise(results)
    th = json.load(open(args.thresholds))
    fails = []
    for pair, lim in th["pairs"].items():
        t = table.get(pair, {}).get("default")
        if not t:
            fails.append("%s: no default result" % pair)
            continue
        if t.get("alias_db") is None or t["alias_db"] > lim["alias_db_max"]:
            fails.append("%s: alias %s dB > %.1f" % (pair, fmt(t.get("alias_db")), lim["alias_db_max"]))
        if t.get("alias_chroma_db") is not None and t["alias_chroma_db"] > lim["alias_chroma_db_max"]:
            fails.append("%s: chroma alias %.1f dB > %.1f" % (pair, t["alias_chroma_db"], lim["alias_chroma_db_max"]))
        if t.get("mtf_0.6") is not None and t["mtf_0.6"] < lim["mtf_0.6_min"]:
            fails.append("%s: MTF 0.6-0.8 %.2f < %.2f" % (pair, t["mtf_0.6"], lim["mtf_0.6_min"]))
        if t.get("ringing") is not None and t["ringing"] > lim["ringing_max"]:
            fails.append("%s: ringing %.1f%% > %.1f%%" % (pair, t["ringing"], lim["ringing_max"]))
        if t.get("motion_err") is not None and "motion_err_db_max" in lim and t["motion_err"] > lim["motion_err_db_max"]:
            fails.append("%s: pan error %.1f dB > %.1f" % (pair, t["motion_err"], lim["motion_err_db_max"]))
        if t.get("real_shimmer") is not None and "real_shimmer_max" in lim and t["real_shimmer"] > lim["real_shimmer_max"]:
            fails.append("%s: real-pan shimmer %.3f > %.3f" % (pair, t["real_shimmer"], lim["real_shimmer_max"]))
        if not t.get("geometry_ok", True):
            fails.append("%s: picture rectangle is not the aspect-preserving fit" % pair)
        for ctl in ("ctl_down_gpu_noaa", "ctl_up_nearest"):
            c = table.get(pair, {}).get(ctl)
            if c and c.get("alias_db") is not None and c["alias_db"] < t["alias_db"] + th.get("control_margin_db", 3.0):
                fails.append("%s: control %s (%.1f dB) not clearly worse than default (%.1f dB)" % (pair, ctl, c["alias_db"], t["alias_db"]))
    for f in fails:
        print("FAIL", f)
    print("GATE", "PASS" if not fails else "FAIL (%d)" % len(fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["prepare", "precompute", "validate", "run", "analyse", "report", "gate"])
    ap.add_argument("--dir", default=os.environ.get("VP_ALIAS_DIR", r"D:\codex\alias-suite"))
    ap.add_argument("--bin", default=None, help="x64\\Release folder with VideoProcessor-Test.dll")
    ap.add_argument("--renderer-bin", default=None, help="x64 Release folder whose renderer DLL and shaders run")
    ap.add_argument("--vstest", default=r"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\Extensions\TestPlatform\vstest.console.exe")
    ap.add_argument("--results", default="results.json")
    ap.add_argument("--thresholds", default=os.path.join(HERE, "alias_thresholds.json"))
    ap.add_argument("--only", default=None)
    ap.add_argument("--wait-for", default="pulse-fast|vp-scale-render|VP-NN-TrainKit",
                    help="'|'-separated command-line tags of other agents' GPU work to wait for")
    ap.add_argument("--no-real", action="store_true")
    ap.add_argument("--plan-only", action="store_true", help="prepare: rewrite plan.tsv only (cards untouched)")
    ap.add_argument("--reanalyse", action="store_true")
    ap.add_argument("--no-analyse", action="store_true", help="run: captures only (an analyse --watch runs alongside)")
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--delete", action="store_true", help="analyse: delete the raw captures once analysed")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--adapter", default="NVIDIA", help="run: VP_TEST_ADAPTER substring ('' = the window's adapter)")
    args = ap.parse_args()
    if args.command == "prepare":
        prepare(args)
    elif args.command == "precompute":
        precompute(args)
    elif args.command == "validate":
        sys.exit(validate(args))
    elif args.command == "run":
        run(args)
    elif args.command == "analyse":
        analyse(args)
    elif args.command == "report":
        report(args)
    elif args.command == "gate":
        sys.exit(gate(args))


if __name__ == "__main__":
    main()
