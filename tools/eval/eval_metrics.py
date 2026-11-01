# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Pure metric functions on numpy arrays (no I/O).

Conventions
  * planes are float64 CODE VALUES; `bitdepth` gives the code range (peak = 2^bd - 1).
  * a single picture is (H, W); a sequence is (T, H, W) in presentation order.
  * 'ref' is the truth/reference, 'out' the candidate or default output of the same geometry.
  * None means "not available" (insufficient data, no such content, optional dependency missing);
    the report prints NA.  Nothing here ever returns a silent 0 for missing data.
  * Everything that depends on picture size (tiles, windows, scales) is derived from the shape.
  * Reductions use float64 and a fixed order, so results are bit-identical between runs.
"""
import math

import numpy as np
from scipy import ndimage

# ------------------------------------------------------------------ helpers


def peak(bitdepth):
    return float((1 << bitdepth) - 1)


def code_scale(bitdepth):
    """Multiplier from 8-bit code units to this bit depth (2^(bd-8))."""
    return 2.0 ** (bitdepth - 8)


def tile_size(shape, divisor=16, lo=16, hi=256):
    """Power-of-two tile edge derived from the smaller picture dimension (~min/divisor)."""
    m = min(shape[-2], shape[-1])
    t = 2 ** int(round(math.log2(max(m / divisor, 1.0))))
    return int(min(max(t, lo), hi))


def tile_grid(shape, tile):
    """Non-overlapping tile origins (y, x) covering the picture (remainders dropped)."""
    h, w = shape[-2], shape[-1]
    return [(y, x) for y in range(0, h - tile + 1, tile) for x in range(0, w - tile + 1, tile)]


def _lowpass(a, sigma):
    return ndimage.gaussian_filter(a, sigma, mode="reflect")


def _seq(a):
    a = np.asarray(a, dtype=np.float64)
    return a[None] if a.ndim == 2 else a


# ------------------------------------------------------------------ 1. PSNR / SSIM


def psnr(ref, out, bitdepth):
    """PSNR in dB of one plane (or sequence).  Identical inputs -> math.inf."""
    d = np.asarray(ref, dtype=np.float64) - np.asarray(out, dtype=np.float64)
    mse = float(np.mean(d * d))
    if mse == 0.0:
        return math.inf
    return 10.0 * math.log10(peak(bitdepth) ** 2 / mse)


def ssim(ref, out, bitdepth, k1=0.01, k2=0.03, sigma=1.5, win=11):
    """Mean SSIM (Wang 2004): Gaussian 11x11 window, sigma 1.5, K1 .01, K2 .03, L = 2^bd - 1.
    Statistics are taken over the 'valid' region (5 px border dropped).  Sequences are averaged."""
    r, o = _seq(ref), _seq(out)
    L = peak(bitdepth)
    c1, c2 = (k1 * L) ** 2, (k2 * L) ** 2
    trunc = ((win - 1) / 2) / sigma
    pad = (win - 1) // 2
    vals = []
    for a, b in zip(r, o):
        g = lambda x: ndimage.gaussian_filter(x, sigma, truncate=trunc, mode="reflect")  # noqa: E731
        mu_a, mu_b = g(a), g(b)
        saa = g(a * a) - mu_a * mu_a
        sbb = g(b * b) - mu_b * mu_b
        sab = g(a * b) - mu_a * mu_b
        m = ((2 * mu_a * mu_b + c1) * (2 * sab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (saa + sbb + c2))
        if m.shape[0] > 2 * pad and m.shape[1] > 2 * pad:
            m = m[pad:-pad, pad:-pad]
        vals.append(float(np.mean(m)))
    return float(np.mean(np.array(vals)))


# ------------------------------------------------------------------ 2. LPIPS (optional)

_LPIPS_CACHE = {}


def ycbcr_to_rgb(Y, Cb, Cr, bitdepth, rng="limited", matrix="709"):
    """Full-resolution (4:4:4) planes -> gamma-coded R'G'B' in [0,1] (no transfer change)."""
    kr, kb = {"709": (0.2126, 0.0722), "2020": (0.2627, 0.0593), "601": (0.299, 0.114)}[matrix]
    kg = 1.0 - kr - kb
    s = code_scale(bitdepth)
    if rng == "limited":
        y = (Y - 16 * s) / (219 * s)
        cb = (Cb - 128 * s) / (224 * s)
        cr = (Cr - 128 * s) / (224 * s)
    else:
        y = Y / peak(bitdepth)
        cb = (Cb - 128 * s) / peak(bitdepth)
        cr = (Cr - 128 * s) / peak(bitdepth)
    r = y + 2 * (1 - kr) * cr
    b = y + 2 * (1 - kb) * cb
    g = (y - kr * r - kb * b) / kg
    return np.clip(np.stack([r, g, b]), 0.0, 1.0)


def lpips_tiles(shape, n=3):
    """Deterministic n x n grid of square crops, edge derived from the picture size (~min/8,
    multiple of 16, >= 64), centred on the grid cells.  Caps LPIPS cost independent of resolution."""
    h, w = shape
    t = max(64, 16 * int(round(min(h, w) / 8 / 16)))
    t = min(t, h, w)
    ys = [int(round((h - t) * (i + 0.5) / n)) for i in range(n)]
    xs = [int(round((w - t) * (i + 0.5) / n)) for i in range(n)]
    return [(y, x, t) for y in ys for x in xs]


def lpips_score(ref_rgb, out_rgb):
    """Mean LPIPS(alex) over deterministic tile crops; CPU only.  rgb: (3,H,W) in [0,1].
    Returns None when torch / lpips (or its cached weights) is unavailable - never downloads."""
    try:
        import torch
        import lpips as _lp
    except Exception:
        return None
    try:
        if "net" not in _LPIPS_CACHE:
            torch.set_num_threads(1)
            hub = torch.hub
            orig = hub.download_url_to_file

            def _no_net(*a, **k):
                raise RuntimeError("download blocked (offline harness)")
            hub.download_url_to_file = _no_net
            try:
                net = _lp.LPIPS(net="alex", verbose=False)
            finally:
                hub.download_url_to_file = orig
            net.eval()
            net.to("cpu")
            _LPIPS_CACHE["net"] = net
        net = _LPIPS_CACHE["net"]
        tot = []
        with torch.no_grad():
            for y, x, t in lpips_tiles(ref_rgb.shape[1:]):
                a = torch.from_numpy(np.ascontiguousarray(ref_rgb[:, y:y + t, x:x + t] * 2 - 1)).float()[None]
                b = torch.from_numpy(np.ascontiguousarray(out_rgb[:, y:y + t, x:x + t] * 2 - 1)).float()[None]
                tot.append(float(net(a, b, normalize=False).item()))
        return float(np.mean(np.array(tot, dtype=np.float64)))
    except Exception:
        return None


# ------------------------------------------------------------------ 3. grain

GRAIN_SIGMA = 2.0          # px: residual = plane - gaussian_lowpass(plane, GRAIN_SIGMA)
GRAIN_TOL = 0.05           # rms ratio must stay within +-5 %
GRAIN_LAG1_TOL = 0.05      # lag-1 temporal correlation may rise by at most this
GRAIN_MID = (0.2, 0.8)     # flat tiles: low-passed mean within this share of the nominal range
GRAIN_MAX_GRADIENT = 0.004  # ... and mean |grad| of the low-passed tile <= this share of peak per px
GRAIN_MAX_TILES = 16


def grain_residual(plane, sigma=GRAIN_SIGMA):
    p = np.asarray(plane, dtype=np.float64)
    return p - _lowpass(p, sigma)


def select_flat_tiles(ref, bitdepth, sigma=GRAIN_SIGMA):
    """Fixed rule, from the REFERENCE only.  ref: (H,W) or (T,H,W) (time-mean is used).
    Tiles (size from picture size) whose low-passed mean is mid-tone (20-80 % of the range)
    and whose mean low-passed |gradient| <= 0.4 % of peak/px; the GRAIN_MAX_TILES flattest
    (ties by tile index).  Returns a list of (y, x, tile); empty when nothing is flat."""
    r = _seq(ref)
    m = np.mean(r, axis=0) if r.shape[0] > 1 else r[0]
    lp = _lowpass(m, sigma)
    gy, gx = np.gradient(lp)
    gm = np.hypot(gy, gx)
    t = tile_size(m.shape, divisor=16, lo=16, hi=128)
    pk = peak(bitdepth)
    cand = []
    for i, (y, x) in enumerate(tile_grid(m.shape, t)):
        mean = lp[y:y + t, x:x + t].mean() / pk
        grad = gm[y:y + t, x:x + t].mean() / pk
        if GRAIN_MID[0] <= mean <= GRAIN_MID[1] and grad <= GRAIN_MAX_GRADIENT:
            cand.append((grad, i, y, x))
    cand.sort()
    return [(y, x, t) for _, _, y, x in cand[:GRAIN_MAX_TILES]]


def _tile_residual_stack(seq, tiles):
    """(T,H,W) -> list of (T,t,t) residual blocks (residual computed on the whole plane first)."""
    res = np.stack([grain_residual(f) for f in seq])
    return [res[:, y:y + t, x:x + t] for (y, x, t) in tiles]


def grain_rms_ratio(ref, out, tiles, tol=GRAIN_TOL):
    """(ratio, passed) with ratio = rms(out residual)/rms(ref residual) over the flat tiles.
    None, None if no tiles or the reference has no residual."""
    if not tiles:
        return None, None
    rb = _tile_residual_stack(_seq(ref), tiles)
    ob = _tile_residual_stack(_seq(out), tiles)
    sr = sum(float(np.sum(b * b)) for b in rb)
    so = sum(float(np.sum(b * b)) for b in ob)
    if sr <= 0:
        return None, None
    ratio = math.sqrt(so / sr)
    return ratio, abs(ratio - 1.0) <= tol


def _high_band_share(blocks):
    """Share of temporal-DFT energy above a quarter of the frame rate (upper half of the band)."""
    hi = tot = 0.0
    for b in blocks:
        b = b - b.mean(axis=0, keepdims=True)
        spec = np.abs(np.fft.rfft(b, axis=0)) ** 2
        n = b.shape[0]
        f = np.fft.rfftfreq(n, d=1.0)           # cycles / frame
        tot += float(spec[1:].sum())
        hi += float(spec[f > 0.25].sum())
    return hi / tot if tot > 0 else None


def grain_temporal_spectrum(ref, out, tiles, min_frames=8):
    """dict(high_ref, high_out, ratio) - high-band/total temporal energy of the grain residual.
    None when fewer than `min_frames` frames or no tiles."""
    r, o = _seq(ref), _seq(out)
    if r.shape[0] < min_frames or not tiles:
        return None
    hr = _high_band_share(_tile_residual_stack(r, tiles))
    ho = _high_band_share(_tile_residual_stack(o, tiles))
    if hr is None or ho is None or hr == 0:
        return None
    return dict(high_ref=hr, high_out=ho, ratio=ho / hr)


def _lag1(blocks):
    vals = []
    for b in blocks:
        b = b - b.mean(axis=0, keepdims=True)
        den = np.sum(b[:-1] * b[:-1], axis=0) + np.sum(b[1:] * b[1:], axis=0)
        num = 2.0 * np.sum(b[:-1] * b[1:], axis=0)
        ok = den > 0
        vals.append(np.where(ok, num / np.where(ok, den, 1.0), 0.0).ravel())
    return float(np.mean(np.concatenate(vals))) if vals else None


def grain_lag1(ref, out, tiles, tol=GRAIN_LAG1_TOL, min_frames=3):
    """dict(ref, out, delta, flag): lag-1 temporal correlation of the residual, averaged per pixel.
    flag True when out - ref > tol (temporal averaging raises it)."""
    r, o = _seq(ref), _seq(out)
    if r.shape[0] < min_frames or not tiles:
        return None
    a = _lag1(_tile_residual_stack(r, tiles))
    b = _lag1(_tile_residual_stack(o, tiles))
    return dict(ref=a, out=b, delta=b - a, flag=(b - a) > tol)


# ------------------------------------------------------------------ 4. temporal stability


def tile_mean_series(seq, tile=None):
    """(T,H,W) -> (T, n_tiles) per-tile means; tile edge derived from the size."""
    s = _seq(seq)
    t = tile or tile_size(s.shape[1:], divisor=8, lo=16, hi=256)
    g = tile_grid(s.shape[1:], t)
    return np.stack([[s[i, y:y + t, x:x + t].mean() for (y, x) in g] for i in range(s.shape[0])])


def flicker_excess(ref, out, bitdepth, min_frames=3):
    """(V_out - V_ref) / (V_ref + c) where V = mean over tiles of var(diff over time of the tile
    mean), in 8-bit-equivalent code units squared; c = 0.01 (0.1 code)^2 guards static references.
    > 0 means the output flickers more than the reference."""
    r, o = _seq(ref), _seq(out)
    if r.shape[0] < min_frames:
        return None
    k = 1.0 / code_scale(bitdepth)
    sr = np.diff(tile_mean_series(r) * k, axis=0)
    so = np.diff(tile_mean_series(o) * k, axis=0)
    vr = float(np.mean(np.var(sr, axis=0)))
    vo = float(np.mean(np.var(so, axis=0)))
    return (vo - vr) / (vr + 0.01)


def tile_energy_series(seq, tile=None, sigma=GRAIN_SIGMA):
    """(T,H,W) -> (T, n_tiles) mean squared detail residual per tile (the 'tile energy')."""
    s = _seq(seq)
    t = tile or tile_size(s.shape[1:], divisor=8, lo=16, hi=256)
    g = tile_grid(s.shape[1:], t)
    res = [grain_residual(f, sigma) ** 2 for f in s]
    return np.stack([[r[y:y + t, x:x + t].mean() for (y, x) in g] for r in res])


def dft_amplitude(series, f):
    """|sum_t s_t exp(-2 pi i f t)| / T per column, f in cycles/frame (direct evaluation)."""
    s = np.asarray(series, dtype=np.float64)
    s = s - s.mean(axis=0, keepdims=True)
    t = np.arange(s.shape[0], dtype=np.float64)
    c = np.cos(2 * np.pi * f * t)[:, None]
    n = np.sin(2 * np.pi * f * t)[:, None]
    return np.hypot((s * c).sum(axis=0), (s * n).sum(axis=0)) / s.shape[0]


def _harmonic_amp(series, f0, max_harm=3):
    tot = 0.0
    for h in range(1, max_harm + 1):
        f = f0 * h
        if f >= 0.5:
            break
        a = dft_amplitude(series, f)
        tot += float(np.sum(a * a))
    return math.sqrt(tot / series.shape[1])


def gop_spectrum(ref, out, gop, minigop=None, min_periods=2, max_harm=3):
    """Spectral lines of the relative tile-energy series at f = 1/gop and 1/minigop (frames) and
    harmonics (rss over `max_harm`).  Returns dict(<name>_ref, <name>_out, <name>_ratio) per period,
    or None when the series is shorter than `min_periods` periods of the longest period asked."""
    r, o = _seq(ref), _seq(out)
    periods = {"gop": gop}
    if minigop:
        periods["minigop"] = minigop
    longest = max(periods.values())
    if not gop or r.shape[0] < min_periods * longest:
        return None
    er = tile_energy_series(r)
    eo = tile_energy_series(o)
    er = er / np.maximum(er.mean(axis=0, keepdims=True), 1e-12)
    eo = eo / np.maximum(eo.mean(axis=0, keepdims=True), 1e-12)
    res = {}
    for name, p in periods.items():
        a, b = _harmonic_amp(er, 1.0 / p, max_harm), _harmonic_amp(eo, 1.0 / p, max_harm)
        res[name + "_ref"], res[name + "_out"] = a, b
        res[name + "_ratio"] = (b / a) if a > 1e-12 else None
    return res


# ------------------------------------------------------------------ 5. motion artefacts

MOTION_SIGMA = 2.0


def motion_mask(ref_t, ref_prev, bitdepth):
    """1 where the (low-passed) reference changes between frames by more than 2 8-bit codes."""
    d = _lowpass(np.abs(np.asarray(ref_t, dtype=np.float64) - ref_prev), MOTION_SIGMA)
    return d > 2.0 * code_scale(bitdepth)


def ghost_coeff(ref, out, bitdepth):
    """Regression coefficient of (out_t - ref_t) on (ref_{t-1} - ref_t) over the motion mask, pooled
    over t >= 1.  1 = previous frame fully blended in, 0 = none.  None if no moving pixels."""
    r, o = _seq(ref), _seq(out)
    num = den = 0.0
    for t in range(1, r.shape[0]):
        m = motion_mask(r[t], r[t - 1], bitdepth)
        if not m.any():
            continue
        d = (r[t - 1] - r[t])[m]
        e = (o[t] - r[t])[m]
        num += float(np.sum(e * d))
        den += float(np.sum(d * d))
    return num / den if den > 0 else None


def smear(ref, out, bitdepth):
    """Gradient energy ratio out/ref inside the (dilated) motion mask, pooled.  < 1 = smeared /
    softened motion.  None if no moving pixels."""
    r, o = _seq(ref), _seq(out)
    so = sr = 0.0
    for t in range(1, r.shape[0]):
        m = ndimage.binary_dilation(motion_mask(r[t], r[t - 1], bitdepth), iterations=2)
        if not m.any():
            continue
        gy, gx = np.gradient(r[t])
        hy, hx = np.gradient(o[t])
        sr += float(np.sum((gy * gy + gx * gx)[m]))
        so += float(np.sum((hy * hy + hx * hx)[m]))
    return so / sr if sr > 0 else None


def duplicate_rate(ref, out, bitdepth, eps=0.25):
    """Share of transitions t-1 -> t where the output repeats (MAD < eps 8-bit codes) while the
    reference changes (MAD >= 2 eps).  None with fewer than two frames."""
    r, o = _seq(ref), _seq(out)
    if r.shape[0] < 2:
        return None
    e = eps * code_scale(bitdepth)
    n = dup = 0
    for t in range(1, r.shape[0]):
        if float(np.mean(np.abs(r[t] - r[t - 1]))) >= 2 * e:
            n += 1
            if float(np.mean(np.abs(o[t] - o[t - 1]))) < e:
                dup += 1
    return dup / n if n else None


# ------------------------------------------------------------------ 6. halo / tone mapping


def halo_overshoot(ref, out, bitdepth):
    """Mean amount (share of nominal range) by which out leaves the reference's local [min,max]
    envelope, in the near-edge band (reference local range > 5 % of peak).  Window derived from
    picture size (odd, ~min/120).  ~0 for identity; > 0 for sharpening / tone-mapping halos."""
    r, o = _seq(ref), _seq(out)
    w = 2 * int(round(min(r.shape[1:]) / 240)) + 1
    w = max(w, 3)
    pk = peak(bitdepth)
    tot = cnt = 0.0
    for a, b in zip(r, o):
        lo = ndimage.minimum_filter(a, size=w, mode="nearest")
        hi = ndimage.maximum_filter(a, size=w, mode="nearest")
        band = ndimage.binary_dilation((hi - lo) > 0.05 * pk, iterations=w // 2)
        if not band.any():
            continue
        over = np.maximum(b - hi, 0.0) + np.maximum(lo - b, 0.0)
        tot += float(over[band].sum())
        cnt += float(band.sum())
    return (tot / cnt) / pk if cnt else None


def local_std(a, sigma):
    m = _lowpass(a, sigma)
    v = _lowpass(a * a, sigma) - m * m
    return np.sqrt(np.maximum(v, 0.0))


def local_contrast_ratio(ref, out, bitdepth, tol=0.05, floor_codes=0.5):
    """dict with `ratio_s{k}` (mean local std out / ref at 3 scales sigma = size/540 * {1,2,4}) and
    `boost_fraction` (share of tiles whose scale-averaged ratio > 1 + tol, tiles with reference local
    std below `floor_codes` 8-bit codes ignored).  The rule is 'restore lost local contrast only,
    no boost': boost_fraction should be ~0.  None when the reference is featureless."""
    r, o = _seq(ref), _seq(out)
    base = max(min(r.shape[1:]) / 540.0, 0.5)
    sig = [base, 2 * base, 4 * base]
    t = tile_size(r.shape[1:], divisor=8, lo=16, hi=256)
    grid = tile_grid(r.shape[1:], t)
    fl = floor_codes * code_scale(bitdepth)
    res = {}
    tile_ratio = np.zeros(len(grid))
    tile_ok = np.ones(len(grid), dtype=bool)
    for k, s in enumerate(sig):
        sr = np.mean([local_std(a, s) for a in r], axis=0)
        so = np.mean([local_std(b, s) for b in o], axis=0)
        if sr.mean() <= 0:
            return None
        res["ratio_s%d" % (k + 1)] = float(so.mean() / sr.mean())
        for i, (y, x) in enumerate(grid):
            a, b = sr[y:y + t, x:x + t].mean(), so[y:y + t, x:x + t].mean()
            if a < fl:
                tile_ok[i] = False
            else:
                tile_ratio[i] += (b / a) / len(sig)
    n = int(tile_ok.sum())
    res["boost_fraction"] = float(np.sum(tile_ratio[tile_ok] > 1.0 + tol) / n) if n else None
    res["sigmas"] = [float(x) for x in sig]
    return res


# ------------------------------------------------------------------ 7. BTB / WTW


def nominal_limits(bitdepth, kind="luma", rng="limited"):
    s = code_scale(bitdepth)
    if rng != "limited":
        return None
    return (16 * s, 235 * s) if kind == "luma" else (16 * s, 240 * s)


def btb_wtw(ref, out, bitdepth, kind="luma", rng="limited"):
    """Below-black / above-white preservation (limited range only; chroma excursions use 16..240).
    dict(btb_n, btb_kept, wtw_n, wtw_kept, clip_detect) with *_kept = share of reference pixels
    beyond the nominal limit that are still beyond it in the output (None if the reference has
    none).  clip_detect True when the output extremum sits exactly at a nominal limit while the
    reference exceeds it.  Returns None for full-range material."""
    lim = nominal_limits(bitdepth, kind, rng)
    if lim is None:
        return None
    lo, hi = lim
    r, o = np.asarray(ref, dtype=np.float64), np.asarray(out, dtype=np.float64)
    below, above = r < lo, r > hi
    res = dict(btb_n=int(below.sum()), wtw_n=int(above.sum()))
    res["btb_kept"] = float(np.mean(o[below] < lo)) if below.any() else None
    res["wtw_kept"] = float(np.mean(o[above] > hi)) if above.any() else None
    clip = False
    if below.any() and float(o.min()) == lo:
        clip = True
    if above.any() and float(o.max()) == hi:
        clip = True
    res["clip_detect"] = clip
    return res


# ------------------------------------------------------------------ 8. cost


def median_ms(samples):
    s = sorted(float(x) for x in samples)
    if not s:
        return None
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def est_3090_ms(gpu_ms, ratio):
    """Laptop GPU ms x ratio (cost_ratios.json: ESTIMATE).  None when there is no GPU timing."""
    return None if gpu_ms is None else gpu_ms * ratio
