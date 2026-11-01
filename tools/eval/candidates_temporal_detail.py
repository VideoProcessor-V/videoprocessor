# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TemporalDetail (repair.temporal_detail) on real encodes: harness candidates and extra rows.

The candidate drives the C++ CPU reference model through tools/td-cli/bin/td_cli.exe --stdio (one process =
one Model per case; the grain probe's re-runs get fresh processes). Vectors: OpenCV DIS flow reduced to the
model's block grid (tools/td-cli/td_flow.py), a documented STAND-IN for VP's GPU vector engine.

Luma only (chroma passes through). Output luma is the model's unrounded value (the GPU prepass works at render
precision, not at the coded bit depth). planeLines = max(height, 1080): every raster <= 2160 lines has Scale 1,
so a crop of a 1080p or 2160p picture runs at the model's true scale.

Candidates: td_off (default: the module off = input, byte-identical by the model's contract, verified by
`td_off_model`), td_b008 (default budget 0.08), td_b005, td_b0085, td_nobudget (measurement only),
td_b008_boost0 (decomposition: fallback boost off, transfer only), td_b008_trust1 (sensitivity: the same DIS
vectors with trust 1 and no forward pair, i.e. no motion uncertainty; separates the stand-in's confidence from the
model's own gates).

Extra rows (`extra_metrics_temporal_detail`, applied to candidate and default alike; case key `frame_types`
= one character I/P/B per window frame, from the encoder's frame-type log):
  psnr_I_Y / psnr_P_Y / psnr_B_Y      PSNR pooled over the frames of that type (vs master)
  ssim_B_Y / lpips_B                  mean over B-frames (LPIPS only when requested and available)
  lpips_all                           mean over all frames, chroma upsampled (Catmull-Rom) for 4:2:0
  ib_gap_db_Y                         psnr over I+P frames minus psnr over B frames (the I/B pulse, lower = less)
  detail_pulse_Y                      coefficient of variation over frames of fine-band energy out/master
                                      (fine band = Y - G(1 px) Y); breathing, lower = steadier
  grain_keep_vs_input_band1..4_Y      flat-tile grain residual energy out/INPUT per quarter of the temporal
                                      spectrum (owner bound 0.95..1.05: the "temporal bands" gate)
  lb_* (case key letterbox.bar_rows)  active-picture check: max bar-pixel change vs input, +-16-row boundary band
                                      change RMS, overshoot/undershoot vs the input's 5x5 local range (INFO rows)
"""
import json
import math
import os
import struct
import subprocess
import sys
import time

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "td-cli"))

import eval_candidates as ec  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402

TD_CLI = os.environ.get("VP_TD_CLI", os.path.join(HERE, "..", "td-cli", "bin", "td_cli.exe"))


class TemporalDetailCLI(ec.PythonCandidate):
    """TemporalDetail CPU model via td_cli --stdio, DIS stand-in vectors."""

    def __init__(self, name, budget=0.08, mode="steady", clean=True, guard=True, forward=True,
                 strength=1.0, max_age=2.0, boost_max=0.15, block=16.0, trust_one=False, stronger_margin=0.15,
                 base_noise=0.75, base_grain_sigmas=3.0, unc_px=0.5, active="auto", force_weight=0.0):
        self.name = name
        self.trust_one = trust_one
        self.stronger_margin, self.base_noise, self.base_grain_sigmas, self.unc_px = (stronger_margin, base_noise,
                                                                                      base_grain_sigmas, unc_px)
        self.active = active
        self.force_weight = force_weight
        self.rect = (0, 0, 0, 0)
        self.budget, self.mode, self.clean, self.guard, self.forward = budget, mode, clean, guard, forward
        self.strength, self.max_age, self.boost_max, self.block = strength, max_age, boost_max, block
        self.description = ("TemporalDetail CPU model (td_cli), mode %s, grain budget %s, cleaner %s, guard %s; "
                            "DIS-flow stand-in vectors%s%s" % (mode, budget, "on" if clean else "off",
                                                               "on" if guard else "off",
                                                               " (+forward)" if forward else "",
                                                               ", trust forced to 1 (vector-confidence sensitivity)"
                                                               if trust_one else ""))
        self.proc = None
        self.stats = []

    # -- process lifetime
    def _close(self):
        if self.proc is not None:
            try:
                self.proc.stdin.write(b"END1")
                self.proc.stdin.close()
                self.proc.wait(timeout=60)
            except Exception:
                self.proc.kill()
            self.proc = None

    def reset(self, meta):
        self._close()
        self.meta = dict(meta)
        self.prev8, self.prev_index, self.seq = None, None, 0
        self.stats = []

    def __del__(self):
        try:
            self._close()
        except Exception:
            pass

    def _start(self, frame):
        import td_flow  # noqa: F401  (fails visibly without OpenCV)
        h, w = frame.Y.shape
        self.w, self.h, self.bd = w, h, frame.bitdepth
        if not os.path.isfile(TD_CLI):
            raise RuntimeError("td_cli.exe not built: run tools/td-cli/build_td_cli.bat (%s)" % TD_CLI)
        self.proc = subprocess.Popen([TD_CLI, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.rect = detect_active(frame) if self.active == "auto" else (0, 0, 0, 0)
        vals = (w, h, frame.bitdepth, frame.fps, max(h, 1080), self.mode, self.budget, int(self.clean), int(self.guard),
                self.strength, self.max_age, self.boost_max, self.stronger_margin, self.base_noise,
                self.base_grain_sigmas, self.unc_px) + tuple(self.rect)
        hdr = "TDCLI1 %d %d %d %.9g %.9g %s %.9g %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d" % vals
        hdr += (" %.9g\n" % self.force_weight) if self.force_weight > 0 else "\n"   # 22nd field only when used
        self.proc.stdin.write(hdr.encode("ascii"))

    def _pair(self, rec):
        if rec is None:
            return struct.pack("<i", 0)
        vx, vy, tr = rec
        gh, gw = vx.shape
        return (struct.pack("<iiid", 1, gw, gh, float(self.block)) + np.ascontiguousarray(vx, np.float32).tobytes() +
                np.ascontiguousarray(vy, np.float32).tobytes() + np.ascontiguousarray(tr, np.float32).tobytes())

    def process(self, frame, future=()):
        import td_flow
        if self.proc is None:
            self._start(frame)
        cur8 = td_flow.to8(frame.Y, frame.bitdepth)
        flags = 0
        b = f = None
        if self.prev8 is not None and frame.index == self.prev_index + 1:
            b, f = td_flow.pair_vectors(self.prev8, cur8, self.block, self.forward)
            if self.trust_one:  # sensitivity: the stand-in's confidence removed (same vectors, no uncertainty)
                b = (b[0], b[1], np.ones_like(b[2]))
                f = None
        elif self.prev8 is not None:
            flags |= 1  # discontinuity: treat as a cut
        t0 = time.perf_counter()
        msg = (b"FRM1" + struct.pack("<Qi", self.seq, flags) + np.ascontiguousarray(frame.Y, np.float64).tobytes() +
               self._pair(b) + self._pair(f))
        self.proc.stdin.write(msg)
        self.proc.stdin.flush()
        out = self.proc.stdout
        tag = out.read(4)
        if tag != b"OUT1":
            raise RuntimeError("td_cli: bad reply %r (exit %s)" % (tag, self.proc.poll()))
        n = self.w * self.h
        Y = np.frombuffer(out.read(8 * n), np.float64).reshape(self.h, self.w).copy()
        ms = struct.unpack("<d", out.read(8))[0]
        ln = struct.unpack("<i", out.read(4))[0]
        st = json.loads(out.read(ln).decode("ascii"))
        st["pipe_ms"] = (time.perf_counter() - t0) * 1000.0
        st["status_ok"] = 1.0 if st.pop("status", "") == "ok" else 0.0  # stats stay numeric (run_prototypes means)
        self.stats.append(st)
        self.prev8, self.prev_index = cur8, frame.index
        self.seq += 1
        return frame.with_planes(Y=Y)


def detect_active(frame, max_std8=4.0, max_mean8=24.0, min_run=4):
    """Active-picture rectangle from the first frame: leading/trailing rows (columns) that are flat (std <=
    max_std8, so codec ringing next to the bar still counts) and dark (mean <= max_mean8), 8-bit-equivalent codes,
    at least min_run long. A deterministic STAND-IN for VP's crop detector; a dark flat picture edge misread as a
    bar is left unprocessed (the safe direction). -> (x0, y0, x1, y1); (0, 0, 0, 0) = whole plane."""
    Y = np.asarray(frame.Y, np.float64) / (1 << (frame.bitdepth - 8))
    h, w = Y.shape

    def flat(a):
        return (a.std(axis=1) <= max_std8) & (a.mean(axis=1) <= max_mean8)

    def runs(mask):
        lead = 0
        while lead < len(mask) and mask[lead]:
            lead += 1
        trail = 0
        while trail < len(mask) - lead and mask[len(mask) - 1 - trail]:
            trail += 1
        return (lead if lead >= min_run else 0), (trail if trail >= min_run else 0)
    t, b = runs(flat(Y))
    l, r = runs(flat(Y.T))
    if (t, b, l, r) == (0, 0, 0, 0) or t + b >= h - 2 or l + r >= w - 2:
        return (0, 0, 0, 0)
    return (l, t, w - r, h - b)


class PassThrough(ec.PythonCandidate):
    name = "td_off"
    description = "default: temporal_detail = off (output = input)"

    def process(self, frame, future=()):
        return frame


CANDIDATES = {
    "td_off": PassThrough,
    "td_off_model": lambda: TemporalDetailCLI("td_off_model", mode="off"),
    "td_b008": lambda: TemporalDetailCLI("td_b008", budget=0.08),
    "td_b005": lambda: TemporalDetailCLI("td_b005", budget=0.05),
    "td_b0085": lambda: TemporalDetailCLI("td_b0085", budget=0.085),
    "td_nobudget": lambda: TemporalDetailCLI("td_nobudget", budget=0.0),
    "td_b008_trust1": lambda: TemporalDetailCLI("td_b008_trust1", budget=0.08, forward=False, trust_one=True),
    "td_b008_boost0": lambda: TemporalDetailCLI("td_b008_boost0", budget=0.08, boost_max=0.0),
    "td_b008_noactive": lambda: TemporalDetailCLI("td_b008_noactive", budget=0.08, active="off"),
}


# ------------------------------------------------------------------ extra rows
def _pooled_psnr(R, O, idx, bd):
    if not idx:
        return None
    mse = float(np.mean([(np.mean((O[i] - R[i]) ** 2)) for i in idx]))
    pk = float((1 << bd) - 1)
    return float("inf") if mse <= 0 else 10.0 * math.log10(pk * pk / mse)


def _rgb444(f):
    """R'G'B' for LPIPS: 4:2:0 chroma upsampled with the default Catmull-Rom (identical for candidate and default,
    which share the input chroma: luma-only candidates)."""
    h, w = f.Y.shape
    cb, cr = f.Cb, f.Cr
    if cb.shape != f.Y.shape:
        fy, fx = round(h / cb.shape[0]), round(w / cb.shape[1])
        sy, sx = eio.chroma_up_shifts(fy, fx, f.siting)
        cb = eio.resample_plane(cb, h, w, "bicubic", sy, sx)
        cr = eio.resample_plane(cr, h, w, "bicubic", sy, sx)
    return em.ycbcr_to_rgb(f.Y, cb, cr, f.bitdepth, f.range, f.matrix)


def _fine_energy(a):
    a = np.asarray(a, np.float64)
    d = a - ndimage.gaussian_filter(a, 1.0, mode="nearest")
    return float(np.mean(d * d))


def _band_energies(blocks, nb=4):
    """Temporal-DFT energy of flat-tile residual blocks in nb equal bands of (0, 0.5] cyc/frame."""
    e = np.zeros(nb)
    for b in blocks:
        b = b - b.mean(axis=0, keepdims=True)
        spec = np.abs(np.fft.rfft(b, axis=0)) ** 2
        f = np.fft.rfftfreq(b.shape[0], d=1.0)
        for k in range(nb):
            lo, hi = 0.5 * k / nb, 0.5 * (k + 1) / nb
            sel = (f > lo) & (f <= hi) if k else (f > 0) & (f <= hi)
            e[k] += float(spec[sel].sum())
    return e


def extra_metrics_temporal_detail(truth, outs, inputs, case, notes):
    bd = truth[0].bitdepth
    R = [t.Y for t in truth]
    O = [o.Y for o in outs]
    m = {}
    # per-frame LPIPS on R'G'B' with upsampled chroma (the harness row is NA for 4:2:0 material)
    lp = None
    if case.get("_lpips"):
        lp = []
        for t, o in zip(truth, outs):
            v = em.lpips_score(_rgb444(t), _rgb444(o))
            if v is None:
                lp = None
                break
            lp.append(v)
    m["lpips_all"] = float(np.mean(lp)) if lp else None
    if m["lpips_all"] is None:
        notes["lpips_all"] = "lpips not requested / unavailable"
    ft = case.get("frame_types")
    types = list(ft[:len(R)]) if ft else None
    if types is None or len(types) < len(R):
        for k in ("psnr_I_Y", "psnr_P_Y", "psnr_B_Y", "ssim_B_Y", "lpips_B", "ib_gap_db_Y"):
            m[k] = None
            notes[k] = "no frame_types in the case"
    else:
        idx = {c: [i for i, t in enumerate(types) if t == c] for c in "IPB"}
        for c in "IPB":
            m["psnr_%s_Y" % c] = _pooled_psnr(R, O, idx[c], bd)
            if not idx[c]:
                notes["psnr_%s_Y" % c] = "no %s-frames in the window" % c
        if idx["B"]:
            m["ssim_B_Y"] = float(np.mean([em.ssim(R[i], O[i], bd) for i in idx["B"]]))
        else:
            m["ssim_B_Y"] = None
            notes["ssim_B_Y"] = "no B-frames in the window"
        m["lpips_B"] = None
        if lp is not None and idx["B"]:
            m["lpips_B"] = float(np.mean([lp[i] for i in idx["B"]]))
        if m["lpips_B"] is None:
            notes["lpips_B"] = "lpips not requested / unavailable"
        ref = idx["I"] + idx["P"]
        a, b = _pooled_psnr(R, O, ref, bd), _pooled_psnr(R, O, idx["B"], bd)
        m["ib_gap_db_Y"] = (a - b) if (a is not None and b is not None and math.isfinite(a) and math.isfinite(b)) else None
        if m["ib_gap_db_Y"] is None:
            notes["ib_gap_db_Y"] = "needs I/P and B frames"
    r = np.array([_fine_energy(o) / max(_fine_energy(t), 1e-9) for o, t in zip(O, R)])
    m["detail_pulse_Y"] = float(np.std(r) / np.mean(r)) if len(r) >= 3 and np.mean(r) > 0 else None
    if m["detail_pulse_Y"] is None:
        notes["detail_pulse_Y"] = "needs >= 3 frames"
    # active-picture rectangle (case key letterbox.bar_rows = top bar height in the crop): bar pixels must be
    # unchanged vs the INPUT, and no halo/undershoot in the +-16 px band around the picture/bar boundary
    lb = (case.get("letterbox") or {}).get("bar_rows")
    if lb:
        Iy = np.stack([f.Y for f in inputs])
        Oy = np.stack(O)
        band = slice(max(0, lb - 16), lb + 16)
        m["lb_bar_change_max_Y"] = float(np.max(np.abs(Oy[:, :lb] - Iy[:, :lb])))
        m["lb_band_change_rms_Y"] = float(np.sqrt(np.mean((Oy[:, band] - Iy[:, band]) ** 2)))
        hi = np.stack([ndimage.maximum_filter(f, size=5, mode="nearest") for f in Iy])[:, band]
        lo = np.stack([ndimage.minimum_filter(f, size=5, mode="nearest") for f in Iy])[:, band]
        ob = Oy[:, band]
        m["lb_band_overshoot_mean_Y"] = float(np.mean(np.maximum(0.0, ob - hi) + np.maximum(0.0, lo - ob)))
        m["lb_band_undershoot_max_Y"] = float(np.max(np.maximum(0.0, lo - ob)))
    # grain temporal bands vs the candidate's input, on the reference-chosen flat tiles
    I = np.stack([f.Y for f in inputs])
    Os = np.stack(O)
    tiles = em.select_flat_tiles(np.stack(R), bd)
    names = ["grain_keep_vs_input_band%d_Y" % (k + 1) for k in range(4)]
    if len(R) < 8 or not tiles:
        for k in names:
            m[k] = None
            notes[k] = "needs >= 8 frames and flat tiles"
    else:
        ei = _band_energies(em._tile_residual_stack(I, tiles))
        eo = _band_energies(em._tile_residual_stack(Os, tiles))
        for k, n in enumerate(names):
            m[n] = math.sqrt(eo[k] / ei[k]) if ei[k] > 0 else None
    return m


# ------------------------------------------------------------------ gate re-tune on real B-frames (sweep)
# Gate thresholds: stronger margin m (reference-stronger gate zero at 1 + m, full at 1 + 3 m; default 0.15),
# base-match allowance scale k (noise 0.75 k codes + 3 k grain sigmas; default 1), motion-uncertainty limit u
# (px; default 0.5). Names: td_s<m>_b<k>_u<u>[_boost]; boost off unless _boost (transfer only = the main candidate).
def _sweep(m, k, u, boost=False):
    tag = "td_s%g_b%g_u%g%s" % (m, k, u, "_boost" if boost else "")
    return tag, (lambda: TemporalDetailCLI(tag, budget=0.08, boost_max=0.15 if boost else 0.0, stronger_margin=m,
                                           base_noise=0.75 * k, base_grain_sigmas=3.0 * k, unc_px=u))


for _m in (0.15, 0.08, 0.05, 0.02):
    for _k in (1.0, 1.5, 2.0, 3.0):
        for _u in (0.5, 0.75, 1.0):
            for _b in (False, True):
                _t, _f = _sweep(_m, _k, _u, _b)
                CANDIDATES[_t] = _f
