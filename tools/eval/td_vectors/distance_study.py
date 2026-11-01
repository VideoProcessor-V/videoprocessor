# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Is it the I-frame distance? Per-reference recoverable detail vs distance and reference type.

For every non-I frame t (frames 2 .. N-3) of a case, each candidate reference r on its own:
  prev / next anchor (I or P), the anchor before the previous one, and the previous / next I frame of the window at ANY
  distance. For each (t, r) the per-16 px-block ORACLE gains (strength a in [0, 1] chosen with the master; ceilings):
    hb    fine-band transfer C + a (H(Rw) - H(C))       full  C + a (Rw - C)
  with three motion fields t -> r:
    chain_master  master-derived consecutive fields composed hop by hop (what TemporalDetail does, near-ideal vectors)
    chain_coded   decoded-frame (engine) consecutive fields composed
    direct_master one direct DIS + quarter-pel refinement field between the MASTER frames t and r (I references and the
                  previous anchor only; no chaining error at long distance)
Also the reference's own quality (PSNR vs master, fine-band energy kept) and the warped reference's PSNR against the
master frame t (alignment + content change).

  python -I tools/eval/td_vectors/distance_study.py --data data.json --case boat_1080p_low --vec VEC --vsrc disref --out X.json
"""
import argparse
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import available_detail as ad  # noqa: E402
import vecgen as vg  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--vec", required=True)
    ap.add_argument("--vsrc", default="disref")
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-direct", action="store_true")
    a = ap.parse_args(argv)
    with open(a.data, encoding="utf8") as f:
        case = next(c for c in json.load(f)["cases"] if c["id"] == a.case)
    w, h = case["w"], case["h"]
    M = vg.read_luma(case["master"], w, h)
    C = vg.read_luma(case["coded"], w, h)
    n = min(len(M), len(C), len(case["types"]))
    types = case["types"][:n]
    fl = dict(chain_master=ad.Flows(os.path.join(a.vec, case["clip"] + "_master", a.vsrc), h, w),
              chain_coded=ad.Flows(os.path.join(a.vec, case["id"], a.vsrc), h, w))
    anchors = [k for k in range(n) if types[k] in "IP"]
    iframes = [k for k in range(n) if types[k] == "I"]
    refq = {}
    for k in anchors:
        Hm, Hc = ad.hb(M[k]), ad.hb(C[k])
        refq[k] = dict(psnr=float(10 * np.log10(1023.0 ** 2 / np.mean((C[k] - M[k]) ** 2))),
                       keep=float(np.mean(Hc ** 2) / np.mean(Hm ** 2)))
    rows = []
    t0 = time.time()
    for t in range(2, n - 2):
        if types[t] == "I":
            continue
        m, c = M[t], C[t]
        Hm, Hc = ad.hb(m), ad.hb(c)
        e_hb, e_full = Hm - Hc, m - c
        prev = [k for k in anchors if k < t]
        nxt = [k for k in anchors if k > t]
        cand = {}
        if prev:
            cand[prev[-1]] = "prev_anchor"
        if nxt:
            cand.setdefault(nxt[0], "next_anchor")
        if len(prev) >= 2:
            cand.setdefault(prev[-2], "prev2_anchor")
        pi = [k for k in iframes if k < t]
        ni = [k for k in iframes if k > t]
        if pi:
            cand[pi[-1]] = cand.get(pi[-1], "") + "+prev_I" if pi[-1] in cand else "prev_I"
        if ni:
            cand[ni[0]] = cand.get(ni[0], "") + "+next_I" if ni[0] in cand else "next_I"
        mse = float(np.mean(e_full ** 2))
        for r, role in sorted(cand.items()):
            row = dict(t=t, type=types[t], r=r, rtype=types[r], role=role, d=r - t, mse=mse, ref_psnr=refq[r]["psnr"],
                       ref_keep=refq[r]["keep"])
            fields = {k: f.to(t, r) for k, f in fl.items()}
            if not a.no_direct and ("I" in role or role == "prev_anchor"):
                F0 = vg.dis(vg.to8(M[t]), vg.to8(M[r]))
                fields["direct_master"] = vg.refine(M[t] / 4.0, M[r] / 4.0, F0)
            for k, F in fields.items():
                rw = vg.warp(C[r], F)
                red, am, _ = ad.oracle(e_hb, [ad.hb(rw) - Hc])
                row[k + "|hb"] = red / m.size
                red, _, _ = ad.oracle(e_full, [rw - c])
                row[k + "|full"] = red / m.size
                row[k + "|warp"] = float(10 * np.log10(1023.0 ** 2 / max(np.mean((vg.warp(M[r], F) - m) ** 2), 1e-9)))
            rows.append(row)
        for f in fl.values():
            f.cache.clear()
        if t % 8 == 0:
            print("  %s t=%d %.0fs" % (a.case, t, time.time() - t0), flush=True)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        json.dump(dict(case=case, refq=refq, rows=rows), f)
    print("distance_study %s -> %s (%.0f s)" % (a.case, a.out, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
