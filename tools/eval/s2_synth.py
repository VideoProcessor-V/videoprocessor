# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""S2 synthetic shadow ramps (eval_synth style; deterministic, seeded, CPU, numpy only).

Entry points
  CONTEXTS                        name -> light_level_ref.Context dict (suite white/black, source peaks)
  ramp_luminance(shape, h, w, lo_nit, hi_nit)   -> (h,w) target luminance in nit; "horizontal" | "vertical" | "radial"
        (interpolation is linear in PQ between lo and hi, i.e. perceptually uniform steps)
  make_case(spec)                 spec = suite case "master"."params" dict -> dict(master=[Frame], coded=[Frame],
        context=Context): master = ideal float 10-bit-scale codes + grain, coded = master quantised to the DELIVERED
        depth with no dither (SDR 8-bit = multiples of 4 on the 10-bit scale; PQ 10-bit).  Frames are limited range.
  suite_dict() / write_suite(path) -> tools/eval/suites/s2_shadow_ramps.json (python -I s2_synth.py [path])

Params: context, shape, grain_codes (std in DELIVERED-depth codes, 0 / 0.5 / 1.5), seed, size_frac (picture height as
a fraction of 2160 p; width = 16/9 x height; default 0.25 -> 960x540, one 4K-crop-equivalent window), frames (default 2),
lo_nit / hi_nit (default 0.01 / 20).  Grain is white Gaussian, a new field per frame (seed, frame).
Contexts: sdr (BT.1886 inverse at the suite white 100 / black 0.05 nit; signal < black clips to BTB-free code 64),
hdr_pq_1000 / hdr_pq_4000 (PQ, BT.2020, limited), lldv_1000 / lldv_4000 (LLDV-like tunnel: same PQ/BT.2020 luma,
tagged tunnel=lldv, mastering peak 1000 / 4000 nit; the evaluation context is hdr_passthrough toward a 600 nit display).
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eval_io as eio  # noqa: E402
import light_level_ref as llr  # noqa: E402
import tonemap_ref as tr  # noqa: E402

SHAPES = ("horizontal", "vertical", "radial")
GRAINS = (0.0, 0.5, 1.5)
GRAIN_SEEDS = {0.0: 20260001, 0.5: 20260002, 1.5: 20260003}  # fixed; per-shape offset added below
SHAPE_OFFSET = {"horizontal": 0, "vertical": 100, "radial": 200}
SUITE_WHITE, SUITE_BLACK, DISPLAY_PEAK = 100.0, 0.05, 600.0
CONTEXTS = {
    "sdr": dict(kind="sdr", white=SUITE_WHITE, black=SUITE_BLACK, eotf="bt1886"),
    "hdr_pq_1000": dict(kind="hdr_passthrough", white=DISPLAY_PEAK, black=SUITE_BLACK, src_peak=1000.0),
    "hdr_pq_4000": dict(kind="hdr_passthrough", white=DISPLAY_PEAK, black=SUITE_BLACK, src_peak=4000.0),
    "lldv_1000": dict(kind="hdr_passthrough", white=DISPLAY_PEAK, black=SUITE_BLACK, src_peak=1000.0),
    "lldv_4000": dict(kind="hdr_passthrough", white=DISPLAY_PEAK, black=SUITE_BLACK, src_peak=4000.0),
}
SDR_CONTEXTS = ("sdr",)


def ramp_luminance(shape, h, w, lo_nit, hi_nit):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    if shape == "horizontal":
        t = xx / max(w - 1, 1)
    elif shape == "vertical":
        t = yy / max(h - 1, 1)
    elif shape == "radial":
        r = np.hypot((xx - (w - 1) / 2.0) / (w / 2.0), (yy - (h - 1) / 2.0) / (h / 2.0))
        t = np.clip(r / np.sqrt(2.0), 0.0, 1.0)
    else:
        raise ValueError("shape %r" % shape)
    lo, hi = float(tr.pq_oetf(lo_nit)), float(tr.pq_oetf(hi_nit))
    return tr.pq_eotf(lo + (hi - lo) * t)


def _sdr_code_of_L(L, ctx):
    g = ctx.gamma
    d = ctx.white ** (1 / g) - ctx.black ** (1 / g)
    a, b = d ** g, ctx.black ** (1 / g) / d
    v = np.clip((np.asarray(L) / a) ** (1 / g) - b, 0.0, 1.0)  # BT.1886 inverse
    return 64.0 + 876.0 * v


def make_case(spec):
    cname = spec["context"]
    ctx = llr.Context.from_dict(CONTEXTS[cname])
    sdr = cname in SDR_CONTEXTS
    h = int(round(2160 * float(spec.get("size_frac", 0.25))))
    w = int(round(h * 16 / 9))
    L = ramp_luminance(spec["shape"], h, w, float(spec.get("lo_nit", 0.01)), float(spec.get("hi_nit", 20.0)))
    ideal = _sdr_code_of_L(L, ctx) if sdr else 64.0 + 876.0 * tr.pq_oetf(L)
    step = 4.0 if sdr else 1.0  # delivered code step on the 10-bit scale
    sigma = float(spec.get("grain_codes", 0.0)) * step
    meta = dict(bitdepth=10, range="limited", matrix="709" if sdr else "2020", transfer="sdr" if sdr else "pq",
                fps=24.0)
    master, coded = [], []
    cb = np.full((h // 2, w // 2), 512.0)
    for t in range(int(spec.get("frames", 2))):
        y = ideal.copy()
        if sigma > 0:
            y = y + sigma * np.random.default_rng([int(spec["seed"]), t]).standard_normal(y.shape)
        y = np.clip(y, 4.0, 1019.0)
        master.append(eio.Frame(y, cb, cb.copy(), index=t, **meta))
        coded.append(eio.Frame(np.clip(np.rint(y / step) * step, 4.0, 1019.0), cb, cb.copy(), index=t, **meta))
    return dict(master=master, coded=coded, context=ctx)


def suite_dict():
    cases = []
    for cname in CONTEXTS:
        for shape in SHAPES:
            for gr in GRAINS:
                params = dict(context=cname, shape=shape, grain_codes=gr, seed=GRAIN_SEEDS[gr] + SHAPE_OFFSET[shape],
                              size_frac=0.25, frames=2, lo_nit=0.01, hi_nit=20.0)
                cases.append({"id": "s2-%s-%s-g%s" % (cname, shape, ("%g" % gr).replace(".", "p")),
                              "content_type": "synthetic_shadow", "clip": shape, "case": "g%g" % gr,
                              "protocol": "shadow", "frames": 2, "light_context": CONTEXTS[cname],
                              "master": {"kind": "synth", "generator": "s2_synth.make_case", "params": params,
                                         "field": "master"},
                              "coded": {"kind": "synth", "generator": "s2_synth.make_case", "params": params,
                                        "field": "coded"}})
    return {"name": "s2_shadow_ramps",
            "description": ("S2 shadow ramps: synthetic 0.01-20 nit gradients (horizontal/vertical/radial) x contexts "
                            "(SDR BT.1886 100/0.05 nit; HDR PQ 1000/4000 nit; LLDV-like PQ/BT.2020 1000/4000) x seeded "
                            "grain 0/0.5/1.5 delivered codes; 960x540 windows (0.25 x 2160p), 2 frames; deterministic. "
                            "master = ideal+grain, coded = quantised to delivered depth (SDR 8-bit, PQ 10-bit), no dither."),
            "cases": cases}


def write_suite(path=None):
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "suites", "s2_shadow_ramps.json")
    with open(path, "w", encoding="utf8", newline="\n") as f:
        f.write(json.dumps(suite_dict(), indent=1) + "\n")
    return path


if __name__ == "__main__":
    print(write_suite(sys.argv[1] if len(sys.argv) > 1 else None))
