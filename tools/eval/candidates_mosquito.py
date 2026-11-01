# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Python REFERENCE mosquito-noise (edge-ringing) repair candidates for the evaluation harness.

Offline prototypes, NOT product code.  Protocol `same` (output vs master at the coded raster); the
DEFAULT is `RepairNone` (today VP has no mosquito repair).  Luma only in this prototype (chroma is
passed through, so the chroma grain gates hold trivially).

The design (a redesign of the earlier "temporal mosquito filter" idea, so that it obeys the grain rule):

 1. Noise level sigma_n per frame: robust MAD of the fine residual on low-structure pixels.
 2. Grain split: S = edge-preserving structure layer (bilateral, range ~ 2 sigma_n), R = Y - S holds the
    grain (and every detail below grain amplitude).  R is never touched and is added back EXACTLY.
 3. Ringing zones: pixels within `zone_px` of a strong edge of S (excluding the edge core itself) whose
    local high-frequency energy in S exceeds the grain level, i.e. compression-structured oscillation
    that survived the grain split.  Soft weight z in [0, 1].  `zone_px` is a transform-block-related
    distance in CODED pixels (ringing spreads across a transform block), not a picture-size constant.
 4. Motion-compensated temporal filtering of S, ONLY where z > 0: S of t-1 / t+1 warped along DIS flow
    (STAND-IN for VP's vector engine), validated per pixel (low-passed structure match + forward/backward
    flow consistency), averaged with confidence weights.  Mosquito noise changes from frame to frame
    (requantisation) while real structure follows the motion, so the average removes it.
 5. out = Y + z * (S_filtered - S)  ( == S_new + R : the exact grain is back); pixels beyond the
    nominal black/white limits in the input are left untouched (BTB/WTW preserved).
 6. Output grain guard: per tile, the share of the input grain layer that the change removes
    (negative projection of the change on the grain, both high-passed) must stay <= 5 %; otherwise the
    tile's change is scaled back.  (Grain left in S by the soft split is what MC averaging could take.)

Variants: RepairMosquitoMC (the design), RepairMosquitoSpatial (step 4 replaced by a stronger spatial
bilateral of S in the zone: does the temporal part earn its cost?), RepairMosquitoNaiveMC (the original
idea WITHOUT grain split or guard: MC average of Y in the zone; included to show what the grain gates
catch).
"""
import os
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eval_candidates as ec  # noqa: E402
import eval_metrics as em  # noqa: E402
from candidates_chroma import bilinear, dis_flow, to8  # noqa: E402


def peak(frame):
    return float((1 << frame.bitdepth) - 1)


def sobel_mag(a):
    return np.hypot(ndimage.sobel(a, 0, mode="nearest"), ndimage.sobel(a, 1, mode="nearest")) / 8.0


def noise_sigma(Y, floor):
    """Robust grain/noise level: 1.4826 * MAD of (Y - gauss(Y, 1)) on the 30 % least-structured pixels."""
    r = Y - ndimage.gaussian_filter(Y, 1.0, mode="nearest")
    g = sobel_mag(ndimage.gaussian_filter(Y, 2.0, mode="nearest"))
    sel = g <= np.quantile(g, 0.3)
    v = r[sel]
    return max(1.4826 * float(np.median(np.abs(v - np.median(v)))), floor)


def bilateral(a, sigma_s, sigma_r, radius):
    """Plain numpy bilateral filter (fixed loop order -> deterministic)."""
    h, w = a.shape
    p = np.pad(a, radius, mode="edge")
    acc = np.zeros_like(a)
    ws = np.zeros_like(a)
    inv2s, inv2r = 1.0 / (2 * sigma_s * sigma_s), 1.0 / (2 * sigma_r * sigma_r)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            q = p[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            wt = np.exp(-(dy * dy + dx * dx) * inv2s) * np.exp(-((q - a) ** 2) * inv2r)
            acc += wt * q
            ws += wt
    return acc / ws


def ringing_zone(S, sigma_n, pk, edge_frac, zone_px, hf_k):
    """Soft ringing-zone weight z in [0,1] and the edge-core mask."""
    g = sobel_mag(S)
    edges = g > edge_frac * pk
    if not edges.any():
        z = np.zeros_like(S)
        return z, edges
    core = ndimage.binary_dilation(edges, iterations=1)
    near = ndimage.binary_dilation(edges, iterations=zone_px) & ~core
    hf = S - ndimage.gaussian_filter(S, 1.0, mode="nearest")
    e = ndimage.uniform_filter(hf * hf, size=5, mode="nearest")
    z = np.clip((e / (sigma_n * sigma_n) - hf_k) / hf_k, 0.0, 1.0) * near
    return z, core


def grain_guard(d, R, zone, tile, max_loss=0.05):
    """Output grain guard.  d = out - Y (the repair's change), R = the grain layer of the input.  Per tile,
    the share of the input grain the change removes is beta = -<hp(d), hp(R)> / <hp(R), hp(R)> over the
    tile's zone pixels (hp = residual after the harness's grain low-pass).  A change that only removes
    ringing is uncorrelated with the grain (beta ~ 0); one that averages grain away has beta > 0.  Where
    beta > max_loss the tile's change is scaled by max_loss / beta (closed form: beta is linear in d).
    Returns (guarded d, number of tiles scaled)."""
    hd = d - ndimage.gaussian_filter(d, em.GRAIN_SIGMA, mode="nearest")
    hr = R - ndimage.gaussian_filter(R, em.GRAIN_SIGMA, mode="nearest")
    h, w = d.shape
    out = d.copy()
    n_scaled = 0
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            sl = (slice(y, min(h, y + tile)), slice(x, min(x + tile, w)))
            m = zone[sl]
            if m.sum() < 16:
                continue
            rr = float((hr[sl][m] ** 2).sum())
            if rr <= 0:
                continue
            beta = -float((hd[sl][m] * hr[sl][m]).sum()) / rr
            if beta > max_loss:
                out[sl] = d[sl] * (max_loss / beta)
                n_scaled += 1
    return out, n_scaled


class RepairNone(ec.PythonCandidate):
    name = "repair_none"
    description = "default: no mosquito repair (identity)"

    def process(self, frame, future=()):
        return frame.with_planes()


class RepairMosquitoMC(ec.PythonCandidate):
    name = "mosquito_mc"
    description = ("mosquito repair: ringing zones near strong edges, MC temporal filter of the structure "
                   "layer only, exact grain add-back, output grain guard (DIS flow = vector stand-in)")
    lookahead = 1
    mode = "mc"            # mc | spatial | naive

    def __init__(self, split_k=2.0, edge_frac=0.06, zone_px=6, hf_k=1.5, fb_tol_px=0.75, match_k=1.5,
                 match_floor_frac=0.004, guard=True):
        self.split_k, self.edge_frac, self.zone_px, self.hf_k = split_k, edge_frac, zone_px, hf_k
        self.fb_tol, self.match_k, self.match_floor = fb_tol_px, match_k, match_floor_frac
        self.guard = guard
        self.prev = None
        self.stats = []

    def reset(self, meta):
        self.prev = None
        self.stats = []
        self._memo = {}

    def _split(self, frame, sigma_n):
        """Structure layer, memoised per source frame (each frame is split once, also as a neighbour)."""
        key = (frame.index, self.split_k)
        if key not in getattr(self, "_memo", {}):
            if not hasattr(self, "_memo") or len(self._memo) > 3:
                self._memo = {}
            self._memo[key] = bilateral(frame.Y, 1.5, self.split_k * sigma_n, 2)
        return self._memo[key]

    def _sigma(self, frame, floor):
        key = ("sigma", frame.index)
        if not hasattr(self, "_memo"):
            self._memo = {}
        if key not in self._memo:
            self._memo[key] = noise_sigma(frame.Y, floor)
        return self._memo[key]

    def _temporal(self, layer_t, layer_n, Yt, Yn, frame, sigma_n):
        """Warp layer_n onto t; returns (warped, confidence)."""
        t8, n8 = to8(Yt, frame.bitdepth), to8(Yn, frame.bitdepth)
        f_tn = dis_flow(t8, n8)
        f_nt = dis_flow(n8, t8)
        h, w = Yt.shape
        gy, gx = np.mgrid[0:h, 0:w].astype(np.float64)
        u, v = gx + f_tn[..., 0], gy + f_tn[..., 1]
        warped = bilinear(layer_n, v, u)
        back = np.hypot(f_tn[..., 0] + bilinear(f_nt[..., 0], v, u), f_tn[..., 1] + bilinear(f_nt[..., 1], v, u))
        lt = ndimage.gaussian_filter(layer_t, 1.5, mode="nearest")
        ln = bilinear(ndimage.gaussian_filter(layer_n, 1.5, mode="nearest"), v, u)
        tau = self.match_k * sigma_n + self.match_floor * peak(frame)
        inside = (u >= 0) & (u <= w - 1) & (v >= 0) & (v <= h - 1)
        conf = np.exp(-(back / self.fb_tol) ** 2) * np.exp(-((lt - ln) / tau) ** 2) * inside
        return warped, conf

    def process(self, frame, future=()):
        Y = frame.Y
        pk = peak(frame)
        floor = 0.25 * (1 << (frame.bitdepth - 8))          # quarter of an 8-bit code
        sigma_n = self._sigma(frame, floor)
        naive = self.mode == "naive"
        S = Y if naive else self._split(frame, sigma_n)
        z, core = ringing_zone(self._split(frame, sigma_n) if naive else S, sigma_n, pk, self.edge_frac,
                               self.zone_px, self.hf_k)
        neigh = [n for n in (self.prev, future[0] if future else None) if n is not None and n.Y.shape == Y.shape]
        self.prev = frame
        if not z.any():
            self.stats.append(dict(sigma_n=sigma_n, zone=0.0, conf=0.0, guarded=0))
            return frame.with_planes()
        if self.mode == "spatial":
            Sf = bilateral(S, 2.0, 4.0 * sigma_n, 3)
            cm = 0.0
        else:
            num, den, cm = S.copy(), np.ones_like(S), 0.0
            for n in neigh:
                Sn = n.Y if naive else self._split(n, self._sigma(n, floor))
                wv, cf = self._temporal(S, Sn, Y, n.Y, frame, sigma_n)
                num += cf * wv
                den += cf
                cm += float(cf[z > 0].mean()) / max(len(neigh), 1)
            Sf = num / den
        d = z * (Sf - S)
        # BTB/WTW: pixels the input carries beyond the nominal limits are never touched (an average
        # with in-range neighbours would pull them back inside)
        lim = em.nominal_limits(frame.bitdepth, "luma", frame.range)
        if lim is not None:
            d = np.where((Y < lim[0]) | (Y > lim[1]), 0.0, d)
        guarded = 0
        if self.guard and not naive:
            d, guarded = grain_guard(d, Y - S, z > 0, em.tile_size(Y.shape, divisor=16, lo=16, hi=128))
        out = Y + d
        self.stats.append(dict(sigma_n=sigma_n, zone=float((z > 0).mean()), conf=cm, guarded=guarded))
        return frame.with_planes(Y=out)


class RepairMosquitoSpatial(RepairMosquitoMC):
    name = "mosquito_spatial"
    description = "mosquito repair, spatial only: zone bilateral of the structure layer, exact grain add-back, guard"
    lookahead = 0
    mode = "spatial"


class RepairMosquitoNaiveMC(RepairMosquitoMC):
    name = "mosquito_naive_mc"
    description = "CONTRAST ONLY: MC temporal average of Y in the zone, no grain split, no guard (the original idea)"
    mode = "naive"


# ------------------------------------------------------------------ extra metrics (harness `extra` hook)
def edge_zones(master_Y, bitdepth, edge_frac=0.06, band_px=6):
    """From the MASTER: edge cores, the near-edge band, and the band's SMOOTH part (master local
    high-frequency energy <= 2x its flat-area noise level): there any high-frequency difference to the
    master is artefact (ringing / mosquito), not lost detail."""
    g = sobel_mag(master_Y)
    edges = g > edge_frac * em.peak(bitdepth)
    core = ndimage.binary_dilation(edges, iterations=1)
    band = ndimage.binary_dilation(edges, iterations=band_px) & ~core
    hf = _hp(master_Y)
    e = ndimage.uniform_filter(hf * hf, size=5, mode="nearest")
    sig = noise_sigma(master_Y, 0.25 * (1 << (bitdepth - 8)))
    smooth = band & (e <= 2.0 * sig * sig)
    return core, band, smooth


def _hp(a):
    return a - ndimage.gaussian_filter(a, 1.0, mode="nearest")


def extra_metrics_mosquito(truth, outs, inputs, case, notes):
    """Rows (all against the master unless named _vs_input):
    ringing_energy_ratio_Y  high-pass error energy (out - master) / (input - master) on the SMOOTH part
                            of the near-edge band (INFO; < 1 = ringing removed; 1 = untouched);
    ringing_band_share      share of pixels in that smooth band (INFO);
    psnr_band_Y             PSNR in the whole near-edge band ('psnr_' rule -> higher is better);
    edge_keep_vs_input_Y    mean gradient magnitude on the edge cores, out / input (INFO; 1 = edges as
                            sharp as delivered); edge_keep_vs_master_Y the same vs the master (INFO)."""
    bd = truth[0].bitdepth
    num = den = se = cnt = share = 0.0
    g_o = g_i = g_m = 0.0
    nfr = 0
    for t, o, i in zip(truth, outs, inputs):
        core, band, smooth = edge_zones(t.Y, bd)
        if not band.any():
            continue
        nfr += 1
        eo, ei = _hp(o.Y - t.Y), _hp(i.Y - t.Y)
        num += float((eo[smooth] ** 2).sum())
        den += float((ei[smooth] ** 2).sum())
        share += float(smooth.mean())
        d = (o.Y - t.Y)[band]
        se += float((d * d).sum())
        cnt += float(band.sum())
        g_o += float(sobel_mag(o.Y)[core].mean())
        g_i += float(sobel_mag(i.Y)[core].mean())
        g_m += float(sobel_mag(t.Y)[core].mean())
    m = {}
    if cnt:
        m["ringing_energy_ratio_Y"] = num / den if den else None
        if den == 0:
            notes["ringing_energy_ratio_Y"] = "no smooth near-edge pixels in the master"
        m["ringing_band_share"] = share / nfr
        mse = se / cnt
        m["psnr_band_Y"] = float("inf") if mse == 0 else 10 * np.log10(em.peak(bd) ** 2 / mse)
        m["edge_keep_vs_input_Y"] = g_o / g_i if g_i else None
        m["edge_keep_vs_master_Y"] = g_o / g_m if g_m else None
    else:
        for k in ("ringing_energy_ratio_Y", "ringing_band_share", "psnr_band_Y", "edge_keep_vs_input_Y",
                  "edge_keep_vs_master_Y"):
            m[k] = None
            notes[k] = "no strong edges in the reference"
    return m


CANDIDATES = {"repair_none": RepairNone, "mosquito_mc": RepairMosquitoMC, "mosquito_spatial": RepairMosquitoSpatial,
              "mosquito_naive_mc": RepairMosquitoNaiveMC}
