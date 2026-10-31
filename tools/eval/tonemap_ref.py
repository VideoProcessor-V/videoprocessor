# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Python reference tone mapping for evaluation round 2 (luma only, float64, deterministic).

Spaces (all luma planes are 10-bit limited-range CODE values, float64):
  PQ source  e = (Y - 64) / 876, the PQ signal (treated as the PQ code of luminance; luma-only
             approximation: hue / gamut mapping are out of scope for these metrics)
  SDR output V = (L / white) ** (1 / 2.4) (BT.1886 inverse, black 0) -> code 64 + 876 V
  perceptual comparisons use PQ codes of the OUTPUT luminance (pq_oetf(L_out)), i.e. the same
  JND-scaled space as the source, so local contrast and artefact steps compare like for like.

Baseline curve = a reproduction of libplacebo 7.360 `pl_tone_map_spline` with st2094_pick_knee
(src/tone_mapping.c of 3rdparty/libplacebo/source, VP-vendored) at VP's ToneMappingTuning defaults
(knee adaptation 0.4, knee min 0.1, max 0.8, default 0.4, slope tuning 1.5, slope offset 0.2, spline
contrast 0.5).  Static = peak detection off (the FL35 configuration: knee from knee_default, input
max = mastering peak); dynamic = per-frame scene average / peak with EMA smoothing (20 frames) and
scene-change reset (stand-in for libplacebo peak detection).  Contrast recovery = libplacebo's
`contrast_recovery` (0.30 at quality high): mix(T(I), T(low) + (I - low), 0.3) with low = a
lowpass of I (Gaussian sigma 1.75 px stand-in for the 1/3.5 feature map + bicubic upsampling).

Target black = 0 nit (VP sdr_black_nits: 0; libplacebo would infer peak/1000 = 0.1 nit when no target
black is given: about 13 codes brighter at mid-shadow - a documented deviation).  Contrast-recovery
lowpass sigma is fixed at 1.75 px (right for 1280-wide frames shown at 2560; approximate for the
1080p-raster amplification crops).

Excursions (BTB/WTW): every candidate maps the clipped signal [0, PQ(peak)] and then carries the
excursion beyond it linearly with the curve's secant slope over the 64 codes at that end, clamped
to the legal 10-bit signal [4, 1019], so values below black / above the mastering peak stay beyond
the SDR limits (VP extended-range rule; real libplacebo clamps them).

Local tone mapping (LTM) candidates.  Bounds (approximate, first order): the restored local
CONTRAST is never above the source's PQ local contrast and never below the global curve's (gain
>= 0; per pixel the signed detail can of course go below T(S)); near the pivot, where T' peaks
(~0.95), the sum can exceed the source by a few % in theory (not observed: tile ratios <= 0.99).
  structure S = I - G, grain G = soft-limited I - gauss(I, 1) (|G| <= 2.5 sigma_n); with grain split
  off, G = 0
  base B = guided filter of S (self-guided, radius = ref_h / 54 px, eps = 0.06^2 PQ^2; tuned on frames
  outside the scored windows: 0.012^2 restored ~5 %, 0.1^2 more but with more halo and grain gain)
  computed at half resolution -> the local adaptation map
  out = T(S) + s * max(0, 1 - T'(B)) * (gauss(S, 1) - B) + T'(S) * G
i.e. the structure detail between the grain band and the base gets back what the global slope took (up to the source
amplitude in PQ), the grain is tone-mapped exactly as the global curve does (owner rule: subtract
grain -> tone map -> add back).
MC variant: B_t <- (1 - a w) B_t + a w warp(B_{t-1}), w = per-pixel validity (structure warp error
and base disagreement), a = 0.6; flow = OpenCV DIS (stand-in for VP's vector engine) on the source
structure; reset (w = 0) at cuts (mean |S_t - warp(S_{t-1})| > 0.06 PQ or no previous frame).
"""
import numpy as np
from scipy import ndimage

# SMPTE ST 2084
M1 = 2610.0 / 16384.0
M2 = 2523.0 / 4096.0 * 128.0
C1 = 3424.0 / 4096.0
C2 = 2413.0 / 4096.0 * 32.0
C3 = 2392.0 / 4096.0 * 32.0
SDR_GAMMA = 2.4


def pq_eotf(e):
    e = np.clip(np.asarray(e, np.float64), 0.0, 1.0)
    p = e ** (1.0 / M2)
    return 10000.0 * (np.maximum(p - C1, 0.0) / (C2 - C3 * p)) ** (1.0 / M1)


def pq_oetf(L):
    y = np.clip(np.asarray(L, np.float64) / 10000.0, 0.0, 1.0) ** M1
    return ((C1 + C2 * y) / (1.0 + C3 * y)) ** M2


def code_to_norm(Y):
    return (np.asarray(Y, np.float64) - 64.0) / 876.0


def norm_to_code(v):
    return 64.0 + 876.0 * np.asarray(v, np.float64)


def _smoothstep(e0, e1, x):
    if e0 == e1:
        return float(x >= e0)
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


VP_DEFAULTS = dict(knee_adaptation=0.4, knee_minimum=0.1, knee_maximum=0.8, knee_default=0.4,
                   slope_tuning=1.5, slope_offset=0.2, spline_contrast=0.5)


class SplineTM:
    """libplacebo pl_tone_map_spline in PQ space.  src_avg_pq None = no scene metadata (static)."""

    def __init__(self, src_peak, dst_peak=100.0, src_min=0.0, dst_min=0.0, src_avg_pq=None, **const):
        c = dict(VP_DEFAULTS)
        c.update(const)
        self.src_peak, self.dst_peak = float(src_peak), float(dst_peak)
        self.in_min, self.in_max = float(pq_oetf(src_min)), float(pq_oetf(src_peak))
        self.out_min, self.out_max = float(pq_oetf(dst_min)), float(pq_oetf(dst_peak))
        src_min_, src_max_, dst_min_, dst_max_ = self.in_min, self.in_max, self.out_min, self.out_max
        mn, mx, df = c["knee_minimum"], c["knee_maximum"], c["knee_default"]
        mix = lambda a, b, t: a + (b - a) * t
        src_knee = src_avg_pq if src_avg_pq else mix(src_min_, src_max_, df)
        src_knee = min(max(src_knee, mix(src_min_, src_max_, mn)), mix(src_min_, src_max_, mx))
        target = (src_knee - src_min_) / (src_max_ - src_min_)
        adapted = mix(dst_min_, dst_max_, target)
        tuning = 1.0 - _smoothstep(mx, df, target) * _smoothstep(mn, df, target)
        adaptation = mix(c["knee_adaptation"], 1.0, tuning)
        dst_knee = mix(src_knee, adapted, adaptation)
        dst_knee = min(max(dst_knee, mix(dst_min_, dst_max_, mn)), mix(dst_min_, dst_max_, mx))
        self.sp, self.dp = src_knee, dst_knee
        if self.in_max <= self.out_max:  # no compression needed: identity in PQ (clipped)
            self.identity = True
            return
        self.identity = False
        slope = (dst_knee - self.out_min) / (src_knee - self.in_min)
        ratio = self.in_max / self.out_max - 1.0
        ratio = min(max(c["slope_tuning"] * ratio, c["slope_offset"]), 1.0 + c["slope_offset"])
        slope = slope ** ((1.0 - c["spline_contrast"]) * ratio)
        in_min, in_max = self.in_min - src_knee, self.in_max - src_knee
        out_min, out_max = self.out_min - dst_knee, self.out_max - dst_knee
        self.Pa = (out_min - slope * in_min) / (in_min * in_min)
        self.Pb = slope
        t = 2.0 * in_max * in_max
        self.Qa = (slope * in_max - out_max) / (in_max * t)
        self.Qb = -3.0 * (slope * in_max - out_max) / t
        self.Qc = slope

    def __call__(self, x):
        """PQ in -> PQ out (input clipped to [in_min, in_max], as the libplacebo LUT)."""
        x = np.clip(np.asarray(x, np.float64), self.in_min, self.in_max)
        if self.identity:
            return np.minimum(x, self.out_max)
        u = x - self.sp
        return np.where(u > 0, ((self.Qa * u + self.Qb) * u + self.Qc) * u, (self.Pa * u + self.Pb) * u) + self.dp

    def slope(self, x):
        x = np.clip(np.asarray(x, np.float64), self.in_min, self.in_max)
        if self.identity:
            return np.ones_like(x)
        u = x - self.sp
        return np.where(u > 0, (3 * self.Qa * u + 2 * self.Qb) * u + self.Qc, 2 * self.Pa * u + self.Pb)


def pq_out_to_sdr_code(p_out, white=100.0):
    L = pq_eotf(np.clip(p_out, 0.0, 1.0))
    return norm_to_code((L / white) ** (1.0 / SDR_GAMMA))


def gauss(a, s):
    return ndimage.gaussian_filter(a, s, mode="nearest")


def box(a, r):
    return ndimage.uniform_filter(a, size=2 * r + 1, mode="nearest")


def guided_self(p, r, eps, down=2):
    """He et al. guided filter, self-guided, at 1/down resolution (box filters; deterministic)."""
    h, w = p.shape
    q = p[: h - h % down, : w - w % down].reshape(h // down, down, w // down, down).mean(axis=(1, 3)) if down > 1 else p
    rr = max(1, int(round(r / down)))
    m = box(q, rr)
    v = box(q * q, rr) - m * m
    a = v / (v + eps)
    b = m - a * m
    a, b = box(a, rr), box(b, rr)
    if down > 1:
        zy, zx = h / a.shape[0], w / a.shape[1]
        a = ndimage.zoom(a, (zy, zx), order=1, mode="nearest", grid_mode=True)
        b = ndimage.zoom(b, (zy, zx), order=1, mode="nearest", grid_mode=True)
        a, b = a[:h, :w], b[:h, :w]
    return a * p + b


def noise_sigma_pq(I):
    r = I - gauss(I, 1.0)
    g = np.hypot(*np.gradient(gauss(I, 2.0)))
    sel = g <= np.quantile(g, 0.3)
    v = r[sel]
    return max(1.4826 * float(np.median(np.abs(v - np.median(v)))), 0.25 / 876.0)


def grain_split(I, k=2.5):
    """Soft-limited fine residual as the grain layer; returns (S, G)."""
    r = I - gauss(I, 1.0)
    lim = k * noise_sigma_pq(I)
    G = lim * np.tanh(r / lim)
    return I - G, G


class Excursion:
    """Carry values outside [0, PQ(peak)] linearly beyond the mapped range (secant slope over the 64
    codes next to that end, so the steep BT.1886-inverse ends do not explode), then clamp to the
    10-bit legal-signal range [4, 1019]."""

    def __init__(self, tm, white=100.0):
        self.tm, self.white = tm, white
        d = 64.0 / 876.0
        lo, hi = 0.0, tm.in_max
        f = lambda x: code_to_norm(pq_out_to_sdr_code(tm(np.array([x])), white))[0]
        self.s_lo = (f(lo + d) - f(lo)) / d
        self.s_hi = (f(hi) - f(hi - d)) / d

    def split(self, e):
        ec = np.clip(e, 0.0, self.tm.in_max)
        return ec, e - ec

    def apply(self, sdr_code, ex):
        return np.clip(sdr_code + 876.0 * np.where(ex < 0, self.s_lo * ex, self.s_hi * ex), 4.0, 1019.0)


def ltm_structure(S, B, tm, strength, detail_sigma=1.0):
    """Restore only the detail between the grain band and the base: gauss(S, detail_sigma) - B, so
    pixel-scale residual grain left in S keeps the global treatment (grain rule)."""
    gain = strength * np.maximum(0.0, 1.0 - tm.slope(B))
    D = (gauss(S, detail_sigma) - B) if detail_sigma > 0 else (S - B)
    return tm(S) + gain * D


class ToneMapper:
    """Stateful per-sequence tone mapper over 10-bit PQ luma code planes -> 10-bit SDR code planes.

    kind: 'global' | 'dynamic' | 'cr' (contrast recovery) | 'ltm' | 'ltm_nograin' | 'ltm_gauss' | 'ltm_mc'
    ref_h: height of the full picture the plane belongs to (spatial scales are derived from it;
    windows cropped from a 2160p picture use 2160)."""

    def __init__(self, kind, src_peak, ref_h, dst_peak=100.0, strength=1.0, cr=0.30, alpha=0.6,
                 eps=0.06 ** 2, cut_thr=0.06, smoothing=20):
        self.kind, self.src_peak, self.dst_peak = kind, float(src_peak), float(dst_peak)
        self.ref_h, self.strength, self.cr, self.alpha, self.eps = ref_h, strength, cr, alpha, eps
        self.cut_thr, self.smoothing = cut_thr, smoothing
        self.tm = SplineTM(src_peak, dst_peak)
        self.exc = Excursion(self.tm, dst_peak)
        self.reset()

    def reset(self):
        self.prev_S = self.prev_B = None
        self.avg = self.pk = None
        self.cuts = 0
        self.mc_valid = []

    def process(self, Ycode, flow_fn=None):
        e = code_to_norm(Ycode)
        ec, ex = self.exc.split(e)
        tm = self.tm
        k = self.kind
        if k == "global":
            p = tm(ec)
        elif k == "dynamic":
            sel = ec > 0.01
            avg = float(ec[sel].mean()) if sel.any() else float(ec.mean())
            pk = min(float(pq_eotf(ec.max())), self.src_peak)
            if self.avg is None or abs(avg - self.avg) > 0.03:  # scene change: reset smoothing
                self.avg, self.pk = avg, pk
            else:
                a = 1.0 / self.smoothing
                self.avg += a * (avg - self.avg)
                self.pk += a * (pk - self.pk)
            dyn = SplineTM(max(self.pk, self.dst_peak * 1.0001), self.dst_peak, src_avg_pq=self.avg)
            p = dyn(ec)
        elif k == "cr":
            i1 = np.clip(e, 0.0, 1.0)  # libplacebo clamps intensity to [0, 1] only for the detail term
            low = gauss(i1, 1.75)
            p = (1 - self.cr) * tm(ec) + self.cr * (tm(low) + (i1 - low))
            p = np.clip(p, tm.out_min, tm.out_max)
        else:
            if k == "ltm_nograin":
                S, G = ec, np.zeros_like(ec)
            else:
                S, G = grain_split(ec)
            r = max(2, int(round(self.ref_h / 54.0)))
            if k == "ltm_gauss":
                B = gauss(S, r / 2.0)
            else:
                B = guided_self(S, r, self.eps)
            if k == "ltm_mc":
                B = self._mc(S, B, flow_fn)
            p = ltm_structure(S, B, tm, self.strength) + tm.slope(S) * G
            p = np.clip(p, tm.out_min, tm.out_max)
        out = pq_out_to_sdr_code(p, self.dst_peak)
        return self.exc.apply(out, ex)

    def _mc(self, S, B, flow_fn):
        if self.prev_S is None or flow_fn is None:
            self.prev_S, self.prev_B = S, B
            self.mc_valid.append(0.0)
            return B
        Sw, Bw = flow_fn(self.prev_S, S, self.prev_B)
        err = gauss(np.abs(S - Sw), 2.0)
        if float(np.mean(np.abs(S - Sw))) > self.cut_thr:  # cut / flow failure: reset
            self.cuts += 1
            w = np.zeros_like(S)
        else:
            w = np.exp(-(err / 0.02) ** 2) * np.exp(-((B - Bw) / 0.05) ** 2)
        Bn = (1 - self.alpha * w) * B + self.alpha * w * Bw
        self.prev_S, self.prev_B = S, Bn
        self.mc_valid.append(float(w.mean()))
        return Bn


def sdr_to_perceptual_codes(sdr_code, white=100.0):
    """SDR output codes -> 10-bit PQ codes of the output luminance (excursions dropped to the limits)."""
    v = np.clip(code_to_norm(sdr_code), 0.0, 1.0)
    return norm_to_code(pq_oetf(white * v ** SDR_GAMMA))
