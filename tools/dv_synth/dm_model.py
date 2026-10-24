#!/usr/bin/env python3
# Copyright(C) 2026 Bill Slack
# SPDX-License-Identifier: GPL-3.0-only
"""A documented, NON-Dolby reference display-management model for the synthetic DV tests.

Dolby's display management (its tone curve versions, trim maths and L8 handling) is
proprietary. To measure VP's DV handling POLICY against a fixed, written-down "intent",
these tests define the intent as follows (and say so in every report):

  1. Tone curve: ITU-R BT.2390-x EETF (the Hermite knee) on the PQ intensity I of
     BT.2100 ICtCp, from the scene's L1 range [min, max] (falling back to the source
     range) to the target display range [0, target]. Chroma (Ct, Cp) follows the
     intensity ratio (saturation-preserving, as Dolby-style IPT mapping does).
  2. Trim (SMPTE ST 2094-10 published form, L2 / L8 semantics, 12-bit codes, 2048 =
     neutral): on x = I / PQ(target): x' = clamp(x * slope + offset, 0, 1) ^ power;
     chroma is scaled by saturation_gain x (I'/I)^chroma_weight.
     The trim for the target is the L2 block whose target_max_PQ is nearest the target
     in PQ (a colourist's pass for that display class); `blend` interpolates the two
     bracketing trims linearly in PQ.

What this model can and cannot decide: it fixes ONE plausible intent so the policies
(L1 only, the nearest trim, a blend) can be compared on mechanics, monotonicity,
clipping and the size of the trims' effect. It is not Dolby's certified result; the
Apple TV LLDV output stays the final check (docs/dv/LLDV-vs-tunnel-check.md).
"""
import numpy as np

import dvsynth as dv

_M_LMS = np.array([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], dtype=np.float64) / 4096.0
_M_ICTCP = np.array([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], dtype=np.float64) / 4096.0
_M_LMS_INV = np.linalg.inv(_M_LMS)
_M_ICTCP_INV = np.linalg.inv(_M_ICTCP)


def rgb_to_ictcp(rgb):
    return dv.nits_to_pq(np.clip(rgb, 0.0, None) @ _M_LMS.T) @ _M_ICTCP.T


def ictcp_to_rgb(ictcp):
    lms = dv.pq_to_nits(ictcp @ _M_ICTCP_INV.T)
    return lms @ _M_LMS_INV.T


def eetf_bt2390(e, src_min_pq, src_max_pq, dst_min_pq, dst_max_pq):
    """BT.2390 EETF on PQ values (0..1)."""
    e = np.asarray(e, dtype=np.float64)
    span = max(src_max_pq - src_min_pq, 1e-9)
    e1 = np.clip((e - src_min_pq) / span, 0.0, None)
    min_lum = (dst_min_pq - src_min_pq) / span
    max_lum = (dst_max_pq - src_min_pq) / span
    ks = 1.5 * max_lum - 0.5
    if ks >= 1.0:
        e2 = np.minimum(e1, 1.0)
    else:
        t = np.clip((e1 - ks) / (1.0 - ks), 0.0, 1.0)
        t2, t3 = t * t, t * t * t
        p = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1.0 - ks) + (-2 * t3 + 3 * t2) * max_lum
        e2 = np.where(e1 < ks, e1, p)
        e2 = np.where(e1 > 1.0, max_lum, e2)
    b = min_lum
    e3 = e2 + b * (1.0 - e2) ** 4
    return np.clip(e3 * span + src_min_pq, 0.0, 1.0)


def decode_trim(code7):
    t, slope, offset, power, cw, sat = code7[:6]
    return {"target_pq": t / 4095.0, "slope": slope / 4096.0 + 0.5, "offset": offset / 4096.0 - 0.5,
            "power": power / 4096.0 + 0.5, "chroma_weight": cw / 4096.0 - 0.5, "saturation": sat / 4096.0 + 0.5}


NEUTRAL = {"slope": 1.0, "offset": 0.0, "power": 1.0, "chroma_weight": 0.0, "saturation": 1.0}


def select_trim(l2_list, target_nits, mode="nearest"):
    """l2_list: [[target_pq, slope, offset, power, chroma_weight, saturation, ms], ...] codes."""
    if not l2_list:
        return dict(NEUTRAL), "no trims"
    tpq = dv.nits_to_pq(target_nits)
    trims = sorted((decode_trim(c) for c in l2_list), key=lambda d: d["target_pq"])
    if mode == "nearest":
        best = min(trims, key=lambda d: abs(d["target_pq"] - tpq))
        return best, "nearest (%.0f nits)" % dv.pq_to_nits(best["target_pq"])
    lo = [d for d in trims if d["target_pq"] <= tpq]
    hi = [d for d in trims if d["target_pq"] >= tpq]
    if lo and hi:
        a, b = lo[-1], hi[0]
        w = 0.0 if b["target_pq"] == a["target_pq"] else (tpq - a["target_pq"]) / (b["target_pq"] - a["target_pq"])
        return {k: a[k] + (b[k] - a[k]) * w for k in NEUTRAL}, "blend"
    return (hi[0] if hi else lo[-1]), "edge"


def apply_trim(I, Ct, Cp, trim, target_pq):
    x = np.clip(I / target_pq, 0.0, None)
    inside = np.minimum(x, 1.0)
    y = np.clip(inside * trim["slope"] + trim["offset"] * np.clip(inside / (16 / 4095 / target_pq), 0, 1), 0.0, 1.0) ** trim["power"]
    y = np.where(x > 1.0, y + (x - 1.0), y)
    I2 = y * target_pq
    ratio = np.where(I > 1e-6, I2 / np.maximum(I, 1e-6), 1.0)
    gain = trim["saturation"] * np.clip(ratio, 1e-6, 4.0) ** trim["chroma_weight"]
    return I2, Ct * gain, Cp * gain


def reference_dm(rgb2020_nits, l1=None, target_nits=100.0, source=(0.0, 10000.0), l2=None, trim_mode="nearest",
                 apply_trims=True):
    """The modelled intent: rgb (..., 3) linear BT.2020 nits -> target-display linear BT.2020 nits."""
    ictcp = rgb_to_ictcp(rgb2020_nits)
    I, Ct, Cp = ictcp[..., 0], ictcp[..., 1], ictcp[..., 2]
    if l1 is not None:
        smin, smax = l1[0] / 4095.0, l1[1] / 4095.0
    else:
        smin, smax = dv.nits_to_pq(source[0]), dv.nits_to_pq(source[1])
    tpq = float(dv.nits_to_pq(target_nits))
    if smax > tpq:
        I2 = eetf_bt2390(I, smin, smax, 0.0, tpq)
    else:
        I2 = np.minimum(I, tpq)
    ratio = np.where(I > 1e-6, I2 / np.maximum(I, 1e-6), 1.0)
    Ct, Cp = Ct * ratio, Cp * ratio
    if apply_trims and l2:
        trim, _ = select_trim(l2, target_nits, trim_mode)
        I2, Ct, Cp = apply_trim(I2, Ct, Cp, trim, tpq)
    return ictcp_to_rgb(np.stack([I2, Ct, Cp], axis=-1))
