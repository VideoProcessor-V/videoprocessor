# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Banding / shadow texture of TemporalDetail outputs on the real encodes (light_metrics, stack_runner chain).

Per case: master, decoded input ("plain"), TemporalDetail outputs saved by td_run.py --save (one per vector source)
and the texture re-injection ORACLE (deband_stack.pre_variant "oracle": x - fine(x) + fine(master), TD's upper bound).
Chain (stack_runner order): [VP deband model, stage 2] -> [tone mapping, hdr_to_sdr only] -> 8-bit final, "nd" plain
rounding or "sd" the sigma-delta model (sigma_delta_ref, amplitude 0.15). Metrics in display units (light_metrics):
banding step visibility band_<range>_mse / _p99 over all, < 5 nit and 5-50 nit tiles (tiles from the master), shadow
texture kept vs plain, grain rms ratio vs plain (same final step).
Contexts:
  sdr         BT.1886, 100 nit white, display black 0 (the audit-2 black fix sidestepped as the coordinator asked)
  hdr_to_sdr  HDR PROXY: the SDR masters/encodes are re-encoded to PQ (BT.1886 light, SDR white -> 203 nit, BT.2408),
              deband (HDR thresholds) in PQ, then tonemap_ref "global" static curve (src peak 1000 nit, FL35 deploy:
              peak detection off) to 100 nit SDR; reference = the master through the same curve.

  python -I tools/eval/td_vectors/td_banding.py --data data.json --case tos_1080p_low --td DIR \
         --sources disref:master,disref:coded --out OUT.json [--f0 24 --f1 48]
"""
import argparse
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import deband_stack as ds  # noqa: E402
import light_level_ref as llr  # noqa: E402
import stack_runner as sr  # noqa: E402
import tonemap_ref as tr  # noqa: E402
import vecgen as vg  # noqa: E402

SDR_CTX = llr.Context(kind="sdr", target_white=100.0, target_black=0.0)


def sdr_to_pq(x):
    v = np.clip((x - 64.0) / 876.0, 0.0, 1.1)
    L = 100.0 * v ** 2.4 * 2.03
    return 64.0 + 876.0 * tr.pq_oetf(L)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--td", required=True, help="<td>/<case>/<source>/out_<f0>_<f1>.npy")
    ap.add_argument("--sources", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--f0", type=int, default=24)
    ap.add_argument("--f1", type=int, default=48)
    ap.add_argument("--n", type=int, default=12, help="frames used from [f0, f1)")
    a = ap.parse_args(argv)
    with open(a.data, encoding="utf8") as f:
        case = next(c for c in json.load(f)["cases"] if c["id"] == a.case)
    w, h = case["w"], case["h"]
    M = vg.read_luma(case["master"], w, h, a.f1, np.float64)[a.f0:a.f0 + a.n]
    C = vg.read_luma(case["coded"], w, h, a.f1, np.float64)[a.f0:a.f0 + a.n]
    cands = {"plain": C, "oracle": None}
    for s in a.sources.split(","):
        p = os.path.join(a.td, a.case, s.replace(":", "_"), "out_%d_%d.npy" % (a.f0, a.f1))
        cands["td_" + s] = np.load(p).astype(np.float64)[:a.n]
    t0 = time.time()
    rows = []
    for rc in ("sdr", "hdr_to_sdr"):
        hdr = rc == "hdr_to_sdr"
        Mx = sdr_to_pq(M) if hdr else M
        ctx = SDR_CTX
        lut = llr.build_lut(ctx)
        ref = sr.tone_map("global", Mx, 1000.0) if hdr else Mx
        plain_by_final = {}
        for name in ["plain", "oracle"] + [k for k in cands if k.startswith("td_")]:
            if name == "oracle":
                x0 = ds.pre_variant("oracle", sdr_to_pq(C) if hdr else C, Mx, hdr)
            else:
                x0 = sdr_to_pq(cands[name]) if hdr else cands[name]
            for deband in (False, True):
                x = x0
                if deband:
                    thr = (1.5 if hdr else 6.0) + 6.0
                    x = np.stack([ds.deband_vp(f, thr, x.shape[1], t) for t, f in enumerate(x)])
                if hdr:
                    x = sr.tone_map("global", x, 1000.0)
                for final in ("nd", "sd"):
                    out = sr.finalise(x, final)
                    key = (final, deband)
                    if name == "plain":
                        plain_by_final[key] = out
                    r = sr.evaluate(ref, plain_by_final[key], out, ctx, lut)
                    r.update(case=a.case, context=rc, cand=name, deband=deband, final=final)
                    rows.append(r)
            print("  %s %s %s %.0fs" % (a.case, rc, name, time.time() - t0), flush=True)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        json.dump(dict(case=a.case, frames=[a.f0, a.f0 + a.n], rows=rows), f, indent=1)
    print("td_banding %s -> %s (%.0f s)" % (a.case, a.out, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
