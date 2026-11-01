# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Python REFERENCE chroma-upsampling candidates (4:2:0 -> 4:4:4) for the evaluation harness.

Offline prototypes, NOT product code: they exist to rank ideas before any GPU build.  All of them
follow the harness contract (eval_candidates.PythonCandidate) and are loaded with
    --candidate py:tools/eval/candidates_chroma.py::<Class>
or through tools/eval/run_prototypes.py.

Candidates (the DEFAULT is the harness built-in `chroma_bicubic` = Catmull-Rom, which is what VP passes
to libplacebo's plane_upscaler when `chroma_upscaler` is absent; see docs/VP_RENDERER_MOTION_CHROMA.md):

  ChromaJBU       joint-bilateral (luma-guided) upsampler, +-2 chroma-grid taps, siting-correct.
  ChromaJBURes    residual JBU: Catmull-Rom + (JBU - same-kernel Gaussian), the luma-guidance part only.
  ChromaKrig      KrigBilateral-style: ordinary kriging over the 12 nearest chroma samples with a
                  covariance that mixes spatial distance and luma similarity (mpv shader idea,
                  reimplemented from the idea, no shader code copied).
  ChromaCfL       chroma-from-luma: local linear regression C ~ a*Y + b on the chroma grid
                  (guided-filter style), applied with full-resolution luma, mixed with Catmull-Rom by
                  the local luma/chroma correlation.
  ChromaMV        motion-vector temporal reconstruction: chroma SAMPLES of the previous and next frame
                  are carried along optical flow to sub-pixel positions in the current frame, validated
                  per sample (forward/backward flow consistency, luma match, chroma plausibility) and
                  splatted with the same kernel as the current samples; per pixel the result is mixed by
                  confidence with a spatial candidate (residual JBU by default; JBU or CfL).  OpenCV DIS flow is a documented
                  STAND-IN for VP's vector engine (motion/), not a proposal to use OpenCV.

Chroma siting (Frame.siting, from stream metadata): 'left' (MPEG-2 / H.264 default: horizontally
co-sited with luma column f*j, vertically centred), 'center' (MPEG-1 / JPEG), 'topleft' (co-sited both
ways, e.g. BT.2020 type 2).  All positions below are computed from the siting, never assumed.

Determinism: float64, fixed loop orders, OpenCV single-threaded, no random numbers.
Parameters: defaults were tuned briefly (PSNR Cb/Cr) on TUNING crops (top-left 1024x768 of the Jockey
x264 crf23 and tos4k x265 windows, 3 frames) that differ from the scored centre crops.
"""
import os
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eval_candidates as ec  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402


# ------------------------------------------------------------------ siting geometry
def site_offsets(siting, fy, fx):
    """Position of chroma sample (i, j) in luma coordinates is (fy*i + oy, fx*j + ox)."""
    ox = 0.0 if siting in ("left", "topleft") else (fx - 1) / 2.0
    oy = 0.0 if siting == "topleft" else (fy - 1) / 2.0
    return oy, ox


def factors(frame):
    h, w = frame.Y.shape
    return max(round(h / frame.Cb.shape[0]), 1), max(round(w / frame.Cb.shape[1]), 1)


def grid_coords(n_full, f, o):
    """Chroma-grid coordinate of every luma row/column."""
    return (np.arange(n_full, dtype=np.float64) - o) / f


def upsample_cr(plane, h, w, siting, fy, fx, kernel="bicubic"):
    """Separable kernel upsampling with siting-correct grid shifts (generalises eio.chroma_up_shifts
    to 'topleft').  Output pixel i samples source coordinate (i + 0.5)/f - 0.5 + s = (i - o)/f."""
    oy, ox = site_offsets(siting, fy, fx)
    sy = 0.5 - 0.5 / fy - oy / fy if fy > 1 else 0.0
    sx = 0.5 - 0.5 / fx - ox / fx if fx > 1 else 0.0
    return eio.resample_plane(plane, h, w, kernel, sy, sx)


def luma_at_sites(Y, ch, cw, siting, fy, fx, kernel="bicubic"):
    """Luma low-passed and sampled at the chroma sample positions (anti-aliased Catmull-Rom)."""
    oy, ox = site_offsets(siting, fy, fx)
    h, w = Y.shape
    Yp = np.pad(Y, ((0, ch * fy - h), (0, cw * fx - w)), mode="edge") if (ch * fy > h or cw * fx > w) else Y
    sy = oy - (fy - 1) / 2.0 if fy > 1 else 0.0
    sx = ox - (fx - 1) / 2.0 if fx > 1 else 0.0
    return eio.resample_plane(Yp, ch, cw, kernel, sy, sx)


def peak(frame):
    return float((1 << frame.bitdepth) - 1)


# ------------------------------------------------------------------ (2) joint bilateral
def jbu(Y, Ylo, C, siting, fy, fx, sigma_s, sigma_r, radius=2, fallback=None, eps=1e-3):
    """Joint bilateral upsampling.  For each luma pixel the chroma samples floor(c)-r .. floor(c)+r+1 per
    axis (a window symmetric about the target: +-r taps around the bracketing pair) are weighted by exp(-d^2/2 sigma_s^2) (d in chroma-grid units, from the exact siting)
    times exp(-(Y(p) - Ylo(q))^2 / 2 sigma_r^2).  `fallback` (Catmull-Rom) enters with weight `eps`
    so a pixel whose luma matches no neighbour degrades to the plain upsampler, never to garbage."""
    h, w = Y.shape
    ch, cw = C.shape
    oy, ox = site_offsets(siting, fy, fx)
    cy, cx = grid_coords(h, fy, oy), grid_coords(w, fx, ox)
    iy0, ix0 = np.floor(cy).astype(int), np.floor(cx).astype(int)
    acc = np.zeros((h, w))
    wsum = np.zeros((h, w))
    inv2s = 1.0 / (2 * sigma_s * sigma_s)
    inv2r = 0.0 if sigma_r is None else 1.0 / (2 * sigma_r * sigma_r)     # None: spatial-only kernel
    span = radius + 1.0

    def spatial_w(d):
        # Gaussian x cos^2 taper reaching 0 at |d| = r + 1: the weights are even in d, so the window
        # is symmetric about the target whatever the phase (no centroid bias).
        return np.exp(-d * d * inv2s) * np.where(np.abs(d) < span, np.cos(np.pi * d / (2 * span)) ** 2, 0.0)
    for dy in range(-radius, radius + 2):
        qi = np.clip(iy0 + dy, 0, ch - 1)
        wy = spatial_w(cy - (iy0 + dy))
        for dx in range(-radius, radius + 2):
            qj = np.clip(ix0 + dx, 0, cw - 1)
            wx = spatial_w(cx - (ix0 + dx))
            Yq = Ylo[np.ix_(qi, qj)]
            wgt = np.outer(wy, wx) * (np.exp(-((Y - Yq) ** 2) * inv2r) if inv2r else 1.0)
            acc += wgt * C[np.ix_(qi, qj)]
            wsum += wgt
    if fallback is not None:
        acc += eps * fallback
        wsum += eps
    return acc / np.maximum(wsum, 1e-300)


class ChromaJBU(ec.PythonCandidate):
    name = "chroma_jbu"
    description = "joint-bilateral luma-guided chroma upsampler, +-2 chroma taps, siting-correct"

    residual = False

    def __init__(self, sigma_s=1.2, sigma_r_frac=0.05, radius=2, gain=1.0):
        self.sigma_s, self.sigma_r_frac, self.radius, self.gain = sigma_s, sigma_r_frac, radius, gain

    def spatial(self, frame):
        h, w = frame.Y.shape
        fy, fx = factors(frame)
        ch, cw = frame.Cb.shape
        Ylo = luma_at_sites(frame.Y, ch, cw, frame.siting, fy, fx)
        sr = self.sigma_r_frac * peak(frame)
        out = []
        for C in (frame.Cb, frame.Cr):
            fb = upsample_cr(C, h, w, frame.siting, fy, fx)
            J = jbu(frame.Y, Ylo, C, frame.siting, fy, fx, self.sigma_s, sr, self.radius, fb)
            if self.residual:
                # luma-guidance correction only: (JBU - same kernel without the range term) on top of
                # Catmull-Rom; where luma is flat the two kernels agree and the output IS Catmull-Rom
                G = jbu(frame.Y, Ylo, C, frame.siting, fy, fx, self.sigma_s, None, self.radius, fb)
                J = fb + self.gain * (J - G)
            out.append(J)
        return out

    def process(self, frame, future=()):
        cb, cr = self.spatial(frame)
        return frame.with_planes(Cb=cb, Cr=cr)


class ChromaJBURes(ChromaJBU):
    """Residual JBU: Catmull-Rom + (joint-bilateral - plain Gaussian) with the same taps.  Added after the
    first scored run showed plain JBU (a positive-weight smoother) loses chroma detail and chroma grain
    on clean material; the residual form keeps Catmull-Rom wherever luma gives no edge guidance."""
    name = "chroma_jbu_res"
    description = "residual JBU: Catmull-Rom + luma-guided correction (JBU minus same-kernel Gaussian)"
    residual = True


# ------------------------------------------------------------------ (3) KrigBilateral-style
# the 12 nearest samples: 4x4 window around the cell minus its corners (offsets from floor(c) - 1)
KRIG_OFFS = [(a, b) for a in range(4) for b in range(4) if not ((a in (0, 3)) and (b in (0, 3)))]


def krig(Y, Ylo, C, siting, fy, fx, sigma_s, sigma_r, nugget, row_chunk=64):
    """Ordinary kriging per luma pixel over KRIG_OFFS chroma samples.  Covariance between two points
    a, b: exp(-|pa - pb|^2 / 2 sigma_s^2) * exp(-(Ya - Yb)^2 / 2 sigma_r^2) (+ nugget on the sample
    diagonal: sample noise).  The 13x13 system (12 samples + unbiasedness constraint) depends only on
    the sample window, so it is inverted once per window origin and shared by the luma pixels using it.
    The estimate is clamped to the range of the 4 nearest samples (anti-ringing of negative weights)."""
    h, w = Y.shape
    ch, cw = C[0].shape
    oy, ox = site_offsets(siting, fy, fx)
    cy, cx = grid_coords(h, fy, oy), grid_coords(w, fx, ox)
    by, bx = np.floor(cy).astype(int) - 1, np.floor(cx).astype(int) - 1     # window origin per row/col
    n = len(KRIG_OFFS)
    inv2s, inv2r = 1.0 / (2 * sigma_s * sigma_s), 1.0 / (2 * sigma_r * sigma_r)
    offs = np.array(KRIG_OFFS, dtype=np.float64)
    dpos2 = ((offs[:, None, :] - offs[None, :, :]) ** 2).sum(-1)               # (n, n) spatial, grid units
    ys, xs = np.unique(by), np.unique(bx)
    # gather Ylo / C over all windows: (Ny, Nx, n)
    Yw = np.empty((len(ys), len(xs), n))
    Cw = np.empty((2, len(ys), len(xs), n))
    for k, (a, b) in enumerate(KRIG_OFFS):
        ii = np.clip(ys + a, 0, ch - 1)
        jj = np.clip(xs + b, 0, cw - 1)
        Yw[:, :, k] = Ylo[np.ix_(ii, jj)]
        for c in range(2):
            Cw[c, :, :, k] = C[c][np.ix_(ii, jj)]
    K = np.exp(-dpos2 * inv2s)[None, None] * np.exp(-((Yw[..., :, None] - Yw[..., None, :]) ** 2) * inv2r)
    K = K + nugget * np.eye(n)[None, None]
    A = np.ones(K.shape[:2] + (n + 1, n + 1))
    A[..., :n, :n] = K
    A[..., n, n] = 0.0
    Ainv = np.linalg.inv(A)                                                   # (Ny, Nx, 13, 13)
    yi = np.searchsorted(ys, by)
    xi = np.searchsorted(xs, bx)
    out = np.empty((2, h, w))
    # the 4 nearest samples (offsets (1,1),(1,2),(2,1),(2,2)) -> clamp range
    near = [KRIG_OFFS.index(o) for o in ((1, 1), (1, 2), (2, 1), (2, 2))]
    for r0 in range(0, h, row_chunk):
        r1 = min(h, r0 + row_chunk)
        rows = np.arange(r0, r1)
        Ai = Ainv[yi[rows]][:, xi]                                            # (R, w, 13, 13)
        Yn = Yw[yi[rows]][:, xi]                                              # (R, w, n)
        # target position relative to the window origin, grid units
        ty = (cy[rows] - by[rows])[:, None, None]
        tx = (cx - bx)[None, :, None]
        d2 = (ty - offs[None, None, :, 0]) ** 2 + (tx - offs[None, None, :, 1]) ** 2
        k = np.ones((r1 - r0, w, n + 1))
        k[..., :n] = np.exp(-d2 * inv2s) * np.exp(-((Y[rows][:, :, None] - Yn) ** 2) * inv2r)
        wts = np.einsum("rwij,rwj->rwi", Ai, k)[..., :n]
        for c in range(2):
            Cn = Cw[c][yi[rows]][:, xi]
            est = (wts * Cn).sum(-1)
            lo = Cn[..., near].min(-1)
            hi = Cn[..., near].max(-1)
            out[c, r0:r1] = np.clip(est, lo, hi)
    return out[0], out[1]


class ChromaKrig(ec.PythonCandidate):
    name = "chroma_krig"
    description = "KrigBilateral-style ordinary kriging, 12 samples, spatial x luma covariance (idea reimplemented)"

    def __init__(self, sigma_s=2.0, sigma_r_frac=0.04, nugget=0.5):
        self.sigma_s, self.sigma_r_frac, self.nugget = sigma_s, sigma_r_frac, nugget

    def process(self, frame, future=()):
        fy, fx = factors(frame)
        ch, cw = frame.Cb.shape
        Ylo = luma_at_sites(frame.Y, ch, cw, frame.siting, fy, fx)
        cb, cr = krig(frame.Y, Ylo, (frame.Cb, frame.Cr), frame.siting, fy, fx, self.sigma_s,
                      self.sigma_r_frac * peak(frame), self.nugget)
        return frame.with_planes(Cb=cb, Cr=cr)


# ------------------------------------------------------------------ (4) chroma from luma
def _box(a, r):
    return ndimage.uniform_filter(a, size=2 * r + 1, mode="nearest")


def cfl(Y, Ylo, C, siting, fy, fx, radius, eps, clamp_r=1, mix_power=1.0):
    """Guided-filter chroma-from-luma.  On the chroma grid: a = cov(Ylo, C) / (var(Ylo) + eps),
    b = mean(C) - a mean(Ylo) over a (2r+1)^2 window; a, b box-averaged (guided filter) and upsampled
    with Catmull-Rom; prediction a*Y + b uses the full-resolution luma.  It is mixed with the plain
    Catmull-Rom result by rho^2 (local luma/chroma correlation, ^mix_power) and clamped to the chroma
    range of the (2*clamp_r+1)^2 nearest samples so a failed regression cannot overshoot."""
    h, w = Y.shape
    mY, mC = _box(Ylo, radius), _box(C, radius)
    vY = np.maximum(_box(Ylo * Ylo, radius) - mY * mY, 0.0)
    vC = np.maximum(_box(C * C, radius) - mC * mC, 0.0)
    cov = _box(Ylo * C, radius) - mY * mC
    a = cov / (vY + eps)
    b = mC - a * mY
    rho2 = np.clip(cov * cov / ((vY + eps) * (vC + eps)), 0.0, 1.0) ** mix_power
    a, b = _box(a, radius), _box(b, radius)
    A = upsample_cr(a, h, w, siting, fy, fx)
    B = upsample_cr(b, h, w, siting, fy, fx)
    M = np.clip(upsample_cr(rho2, h, w, siting, fy, fx), 0.0, 1.0)
    base = upsample_cr(C, h, w, siting, fy, fx)
    out = M * (A * Y + B) + (1.0 - M) * base
    if clamp_r is not None:
        lo = ndimage.minimum_filter(C, size=2 * clamp_r + 1, mode="nearest")
        hi = ndimage.maximum_filter(C, size=2 * clamp_r + 1, mode="nearest")
        out = np.clip(out, upsample_cr(lo, h, w, siting, fy, fx, "bicubic"),
                      upsample_cr(hi, h, w, siting, fy, fx, "bicubic"))
    return out


class ChromaCfL(ec.PythonCandidate):
    name = "chroma_cfl"
    description = "chroma-from-luma: guided-filter local linear regression, correlation-mixed with Catmull-Rom"

    def __init__(self, radius=2, eps_frac=0.002, clamp_r=1):
        self.radius, self.eps_frac, self.clamp_r = radius, eps_frac, clamp_r

    def spatial(self, frame):
        h, w = frame.Y.shape
        fy, fx = factors(frame)
        ch, cw = frame.Cb.shape
        Ylo = luma_at_sites(frame.Y, ch, cw, frame.siting, fy, fx)
        eps = (self.eps_frac * peak(frame)) ** 2
        return [cfl(frame.Y, Ylo, C, frame.siting, fy, fx, self.radius, eps, self.clamp_r)
                for C in (frame.Cb, frame.Cr)]

    def process(self, frame, future=()):
        cb, cr = self.spatial(frame)
        return frame.with_planes(Cb=cb, Cr=cr)


# ------------------------------------------------------------------ (5) motion-vector temporal chroma
def _cv2():
    try:
        import cv2
    except ImportError as e:  # visible: the candidate cannot run without its flow stand-in
        raise RuntimeError("ChromaMV needs OpenCV (pip install opencv-python-headless) for the DIS flow "
                           "stand-in: %s" % e)
    cv2.setNumThreads(1)  # deterministic and within the CPU budget
    return cv2


def to8(Y, bitdepth):
    return np.clip(np.rint(Y / float(1 << (bitdepth - 8))), 0, 255).astype(np.uint8)


def dis_flow(a8, b8):
    """Dense flow a -> b (b(p + F(p)) ~ a(p)); OpenCV DIS, MEDIUM preset.  STAND-IN for VP's engine."""
    cv2 = _cv2()
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    return dis.calc(a8, b8, None).astype(np.float64)


def bilinear(plane, yy, xx):
    """Bilinear sample with edge clamping at float positions."""
    h, w = plane.shape
    yy = np.clip(yy, 0, h - 1)
    xx = np.clip(xx, 0, w - 1)
    y0 = np.minimum(np.floor(yy).astype(int), h - 2) if h > 1 else np.zeros(yy.shape, int)
    x0 = np.minimum(np.floor(xx).astype(int), w - 2) if w > 1 else np.zeros(xx.shape, int)
    fy, fx = yy - y0, xx - x0
    y1, x1 = np.minimum(y0 + 1, h - 1), np.minimum(x0 + 1, w - 1)
    return ((1 - fy) * ((1 - fx) * plane[y0, x0] + fx * plane[y0, x1])
            + fy * ((1 - fx) * plane[y1, x0] + fx * plane[y1, x1]))


def splat(shape, v, u, values, conf, Ys, Yt, sigma_px, sigma_r, radius=2):
    """Scatter weighted samples (luma-grid positions v, u; chroma values (k, n); confidence conf; luma
    at the sample Ys) onto the luma grid.  Kernel: Gaussian in luma pixels x luma range weight against
    the target frame's luma Yt.  Returns (acc (k,h,w), wsum (h,w)).  np.bincount sums in input order:
    deterministic."""
    h, w = shape
    acc = np.zeros((values.shape[0], h * w))
    wsum = np.zeros(h * w)
    vb, ub = np.floor(v).astype(int), np.floor(u).astype(int)
    inv2s, inv2r = 1.0 / (2 * sigma_px * sigma_px), 1.0 / (2 * sigma_r * sigma_r)
    for dy in range(-radius + 1, radius + 1):
        py = vb + dy
        oky = (py >= 0) & (py < h)
        for dx in range(-radius + 1, radius + 1):
            px = ub + dx
            ok = oky & (px >= 0) & (px < w) & (conf > 0)
            if not ok.any():
                continue
            pyk, pxk = py[ok], px[ok]
            d2 = (v[ok] - pyk) ** 2 + (u[ok] - pxk) ** 2
            wr = np.exp(-((Yt[pyk, pxk] - Ys[ok]) ** 2) * inv2r)
            wt = conf[ok] * np.exp(-d2 * inv2s) * wr
            idx = pyk * w + pxk
            wsum += np.bincount(idx, weights=wt, minlength=h * w)
            for c in range(values.shape[0]):
                acc[c] += np.bincount(idx, weights=wt * values[c][ok], minlength=h * w)
    return acc.reshape(values.shape[0], h, w), wsum.reshape(h, w)


class ChromaMV(ec.PythonCandidate):
    """Temporal chroma reconstruction along motion.  lookahead 1 (next frame) + the previous input frame
    kept as state; neighbours t-1 and t+1.  Per neighbour sample: landing position from flow n -> t at
    the sample's site, confidence = fb-consistency x luma match x chroma plausibility (vs the spatial
    estimate at the landing point) x alpha.  Output = (Wcur * S + sum_n acc_n) / (Wcur + sum_n W_n) where
    S is the spatial candidate and Wcur the current samples' own splat weight under the same kernel, so
    with no valid neighbour the output is exactly S."""
    name = "chroma_mv"
    description = ("MV temporal chroma: neighbour chroma samples along DIS flow (stand-in for VP vectors), "
                   "validated per sample, confidence-mixed with residual JBU")
    lookahead = 1

    def __init__(self, spatial="jbures", alpha=0.3, sigma_px=0.9, sigma_r_frac=0.05, fb_tol_px=0.5,
                 luma_tol_frac=0.02, chroma_tol_frac=0.04, vectors=None):
        """vectors: None = VP_EVAL_VECTORS environment / DIS stand-in, or a vector_source.VectorSource."""
        from vector_source import VectorSource
        self.vs = vectors if vectors is not None else VectorSource.from_env()
        self.spatial_kind = spatial
        self.sp = {"jbu": ChromaJBU, "jbures": ChromaJBURes, "cfl": ChromaCfL}[spatial]()
        self.alpha, self.sigma_px, self.sigma_r_frac = alpha, sigma_px, sigma_r_frac
        self.fb_tol, self.luma_tol, self.chroma_tol = fb_tol_px, luma_tol_frac, chroma_tol_frac
        if spatial != "jbures":
            self.name = "chroma_mv_" + spatial
            self.description = self.description.replace("with residual JBU", "with " + spatial.upper())
        self.prev = None
        self.stats = []

    def reset(self, meta):
        self.prev = None
        self.stats = []

    def _sites(self, frame):
        fy, fx = factors(frame)
        oy, ox = site_offsets(frame.siting, fy, fx)
        ch, cw = frame.Cb.shape
        ii, jj = np.meshgrid(np.arange(ch, dtype=np.float64), np.arange(cw, dtype=np.float64), indexing="ij")
        return fy * ii + oy, fx * jj + ox

    def process(self, frame, future=()):
        S = np.stack(self.sp.spatial(frame))
        h, w = frame.Y.shape
        fy, fx = factors(frame)
        ch, cw = frame.Cb.shape
        pk = peak(frame)
        sr = self.sigma_r_frac * pk
        sv, su = self._sites(frame)
        Ylo_t = luma_at_sites(frame.Y, ch, cw, frame.siting, fy, fx)
        vals_t = np.stack([frame.Cb.ravel(), frame.Cr.ravel()])
        _, Wcur = splat((h, w), sv.ravel(), su.ravel(), vals_t, np.ones(ch * cw), Ylo_t.ravel(),
                        frame.Y, self.sigma_px, sr)
        neigh = [n for n in (self.prev, future[0] if future else None) if n is not None and n.Y.shape == frame.Y.shape]
        num = Wcur[None] * S
        den = Wcur.copy()
        t8 = to8(frame.Y, frame.bitdepth)
        used = 0.0
        for n in neigh:
            n8 = to8(n.Y, n.bitdepth)
            f_nt = self.vs.flow(n.index, frame.index, n8, t8)   # n -> t (external vectors or DIS)
            f_tn = self.vs.flow(frame.index, n.index, t8, n8)   # t -> n
            fxs = bilinear(f_nt[..., 0], sv, su)
            fys = bilinear(f_nt[..., 1], sv, su)
            u, v = su + fxs, sv + fys
            inside = (u >= 0) & (u <= w - 1) & (v >= 0) & (v <= h - 1)
            bx = bilinear(f_tn[..., 0], v, u)
            byy = bilinear(f_tn[..., 1], v, u)
            fb = np.hypot(fxs + bx, fys + byy)
            Ylo_n = luma_at_sites(n.Y, ch, cw, n.siting, fy, fx)
            dl = Ylo_n - bilinear(ndimage.gaussian_filter(frame.Y, 0.8 * min(fy, fx), mode="nearest"), v, u)
            conf = (np.exp(-(fb / self.fb_tol) ** 2) * np.exp(-(dl / (self.luma_tol * pk)) ** 2) * inside)
            vals = np.stack([n.Cb, n.Cr])
            Sl = np.stack([bilinear(S[0], v, u), bilinear(S[1], v, u)])
            dc = np.hypot(vals[0] - Sl[0], vals[1] - Sl[1])
            conf = conf * np.exp(-(dc / (self.chroma_tol * pk)) ** 2) * self.alpha
            acc, Wn = splat((h, w), v.ravel(), u.ravel(), vals.reshape(2, -1), conf.ravel(), Ylo_n.ravel(),
                            frame.Y, self.sigma_px, sr)
            num += acc
            den += Wn
            used += float(conf.mean() / self.alpha)
        self.stats.append(used / max(len(neigh), 1))
        self.prev = frame
        out = num / np.maximum(den, 1e-300)
        return frame.with_planes(Cb=out[0], Cr=out[1])


class ChromaMVCfL(ChromaMV):
    def __init__(self, **kw):
        ChromaMV.__init__(self, spatial="cfl", **kw)


# ------------------------------------------------------------------ extra metrics (harness `extra` hook)
def edge_band(Y, bitdepth, frac=0.04, grow=2):
    """Luma-edge band of the reference: Sobel magnitude > frac*peak, dilated by `grow` px."""
    g = np.hypot(ndimage.sobel(Y, 0, mode="nearest"), ndimage.sobel(Y, 1, mode="nearest")) / 8.0
    m = g > frac * em.peak(bitdepth)
    return ndimage.binary_dilation(m, iterations=grow) if m.any() else m


def extra_metrics_chroma(truth, outs, inputs, case, notes):
    """Chroma-specific rows: PSNR of Cb/Cr inside the reference luma-edge band (colour bleeding at
    edges; rule 'psnr_' -> higher is better), chroma temporal flicker vs the reference
    ('flicker_excess_' -> lower), and the share of pixels in the edge band (INFO)."""
    bd = truth[0].bitdepth
    m = {}
    masks = [edge_band(t.Y, bd) for t in truth]
    share = float(np.mean([mk.mean() for mk in masks]))
    for n in ("Cb", "Cr"):
        se, cnt = 0.0, 0
        for t, o, mk in zip(truth, outs, masks):
            d = getattr(t, n)[mk] - getattr(o, n)[mk]
            se += float((d * d).sum())
            cnt += int(mk.sum())
        if cnt:
            mse = se / cnt
            m["psnr_edge_" + n] = float("inf") if mse == 0 else 10 * np.log10(em.peak(bd) ** 2 / mse)
        else:
            m["psnr_edge_" + n] = None
            notes["psnr_edge_" + n] = "no luma edges in the reference"
        R = np.stack([getattr(t, n) for t in truth])
        O = np.stack([getattr(o, n) for o in outs])
        m["flicker_excess_" + n] = em.flicker_excess(R, O, bd)
        if m["flicker_excess_" + n] is None:
            notes["flicker_excess_" + n] = "needs >= 3 frames (%d available)" % len(truth)
    m["edge_band_share"] = share
    return m


CANDIDATES = {"chroma_jbu": ChromaJBU, "chroma_jbu_res": ChromaJBURes, "chroma_krig": ChromaKrig, "chroma_cfl": ChromaCfL,
              "chroma_mv": ChromaMV, "chroma_mv_cfl": ChromaMVCfL}
