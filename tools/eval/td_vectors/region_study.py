# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Where in the picture could temporal detail transfer help? Per-16 px-block gains binned by region class.

Per case (frames [f0, f1), the frames td_run.py --save kept) and per block:
  oracle   per-block ORACLE fine-band transfer gain (available_detail.oracle; refs = prev/next anchor + near I; vectors
           X:master) in SSE units (10-bit codes^2)
  td_*     the TemporalDetail output's SSE reduction vs master for each saved run (td/<case>/<dir>/out_f0_f1.npy)
Region classes (from the MASTER and the master-derived vectors only): light level (BT.1886, 100 nit white: <5, 5-50,
>50 nit), detail class (flat / smooth gradient = banding-prone / textured / edge, from fine-band energy and gradient),
motion magnitude (px/frame), camera-model label (inlier / other / no usable fit), distance to the last I frame.
Outputs per class: area share, oracle-gain share, TD-gain share, gain per area; the gain concentration curve (share of
total oracle gain captured by the best x % of blocks, oracle-ranked = unreachable upper bound) and a fitted region
rule (class combination selected on train clips, scored on held-out clips: area share run vs gain share kept).

  python -I tools/eval/td_vectors/region_study.py --data data.json --cases boat_1080p_low,... --vec VEC --td TD --out X.json
"""
import argparse
import json
import os
import sys

import numpy as np

import cv2

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import available_detail as ad  # noqa: E402
import camvec  # noqa: E402
import vecgen as vg  # noqa: E402

B = 16


def bmean(x):
    h, w = x.shape
    gh, gw = h // B, w // B
    return x[:gh * B, :gw * B].reshape(gh, B, gw, B).mean(axis=(1, 3))


def bsum(x):
    h, w = x.shape
    gh, gw = h // B, w // B
    return x[:gh * B, :gw * B].reshape(gh, B, gw, B).sum(axis=(1, 3))


def oracle_blocks(e, ds):
    best = np.zeros_like(bsum(e))
    for d in ds:
        ed, dd = bsum(e * d), bsum(d * d)
        a = np.clip(ed / np.maximum(dd, 1e-9), 0.0, 1.0)
        best = np.maximum(best, 2 * a * ed - a * a * dd)
    return best


def nits(code10):
    v = np.clip((code10 - 64.0) / 876.0, 0.0, 1.0)
    return 100.0 * v ** 2.4


def case_blocks(case, vec, td, vsrc, f0, f1, tddirs):
    w, h = case["w"], case["h"]
    M = vg.read_luma(case["master"], w, h, f1 + 4)
    C = vg.read_luma(case["coded"], w, h, f1 + 4)
    types = case["types"]
    fl = ad.Flows(os.path.join(vec, case["clip"] + "_master", vsrc), h, w)
    camf = os.path.join(vec, case["clip"] + "_master", vsrc + ".camf")
    cam = camvec.read_camf(camf)[3] if os.path.isfile(camf) else None
    outs = {}
    for dd in tddirs:
        p = os.path.join(td, case["id"], dd, "out_%d_%d.npy" % (f0, f1))
        if os.path.isfile(p):
            outs[dd] = np.load(p).astype(np.float32)
    rows = []
    for t in range(f0, f1):
        m, c = M[t], C[t]
        Hm, Hc = ad.hb(m), ad.hb(c)
        refs = ad.refs_for(types[:len(M)], t) if types[t] != "I" else []
        ds = [ad.hb(vg.warp(C[r], fl.to(t, r))) - Hc for r in refs]
        ora = oracle_blocks(Hm - Hc, ds) if ds else np.zeros_like(bsum(m))
        sse_in = bsum((c - m) ** 2)
        tdg = {k: sse_in - bsum((o[t - f0] - m) ** 2) for k, o in outs.items()}
        L = nits(bmean(m))
        Ehb = bmean(Hm ** 2)
        gx = cv2.Sobel(cv2.GaussianBlur(m, (0, 0), 2.0), cv2.CV_32F, 1, 0) / 8.0
        gy = cv2.Sobel(cv2.GaussianBlur(m, (0, 0), 2.0), cv2.CV_32F, 0, 1) / 8.0
        G = bmean(np.hypot(gx, gy))
        Gmax = np.hypot(gx, gy)[:L.shape[0] * B, :L.shape[1] * B].reshape(L.shape[0], B, L.shape[1], B).max(axis=(1, 3))
        F = fl.pair(t, t - 1)
        mot = np.hypot(bmean(F[..., 0]), bmean(F[..., 1]))
        lab = np.full(L.shape, 9, np.uint8)
        if cam is not None and t - 1 < len(cam):
            rec = cam[t - 1]
            if rec["valid"] and rec["conf"] >= 0.5:
                gl = rec["label"]
                lab = gl[:L.shape[0], :L.shape[1]] if gl.shape[0] >= L.shape[0] else np.pad(gl, ((0, L.shape[0] - gl.shape[0]), (0, L.shape[1] - gl.shape[1])), mode="edge")
        last_i = max([k for k in range(t + 1) if types[k] == "I"] or [0])
        rows.append(dict(t=t, type=types[t], ora=ora, tdg=tdg, L=L, Ehb=Ehb, G=G, Gmax=Gmax, mot=mot, lab=lab, dI=t - last_i,
                         sse_in=sse_in))
        fl.trim(t - 4)
    return rows


def classes(r):
    L, Ehb, G, mot, lab = r["L"], r["Ehb"], r["G"], r["mot"], r["lab"]
    # thresholds (10-bit codes): edge = max smoothed gradient > 40 codes/px; textured = fine-band energy > 4 codes^2;
    # smooth gradient (banding-prone) = mean gradient > 0.25 codes/px without texture; else flat
    light = np.where(L < 5, "lt5", np.where(L < 50, "5to50", "gt50"))
    detail = np.where(r["Gmax"] > 40, "edge", np.where(Ehb > 4.0, "textured", np.where(G > 0.25, "gradient", "flat")))
    motion = np.where(mot < 0.5, "m<0.5", np.where(mot < 2, "m0.5-2", np.where(mot < 8, "m2-8", "m>8")))
    camc = np.where(lab == 9, "nofit", np.where(lab == 1, "cam_inlier", "cam_other"))
    return dict(light=light, detail=detail, motion=motion, camera=camc)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--cases", required=True)
    ap.add_argument("--vec", required=True)
    ap.add_argument("--td", required=True)
    ap.add_argument("--vsrc", default="disref")
    ap.add_argument("--tddirs", default="disref_master_final,disref_coded_final")
    ap.add_argument("--train", default="boat,tos")
    ap.add_argument("--f0", type=int, default=24)
    ap.add_argument("--f1", type=int, default=48)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    with open(a.data, encoding="utf8") as f:
        allc = {c["id"]: c for c in json.load(f)["cases"]}
    tddirs = a.tddirs.split(",")
    res = dict(classes={}, curve={}, rule={}, per_case={})
    store = {}
    for cid in a.cases.split(","):
        rows = case_blocks(allc[cid], a.vec, a.td, a.vsrc, a.f0, a.f1, tddirs)
        store[cid] = rows
        print("region_study %s: %d frames" % (cid, len(rows)), flush=True)
    # class tables pooled over cases
    agg = {}
    for cid, rows in store.items():
        for r in rows:
            cl = classes(r)
            cl["dI"] = np.full(r["L"].shape, "dI<=4" if r["dI"] <= 4 else "dI5-12" if r["dI"] <= 12 else "dI13-24" if r["dI"] <= 24 else "dI>24")
            cl["ftype"] = np.full(r["L"].shape, r["type"])
            for dim, lab in cl.items():
                for v in np.unique(lab):
                    m = lab == v
                    e = agg.setdefault(dim, {}).setdefault(str(v), dict(area=0, ora=0.0, sse=0.0, **{k: 0.0 for k in tddirs}))
                    e["area"] += int(m.sum()); e["ora"] += float(r["ora"][m].sum()); e["sse"] += float(r["sse_in"][m].sum())
                    for k in tddirs:
                        if k in r["tdg"]:
                            e[k] += float(r["tdg"][k][m].sum())
    for dim, d in agg.items():
        A = sum(e["area"] for e in d.values()); O = sum(e["ora"] for e in d.values())
        res["classes"][dim] = {v: dict(area_share=e["area"] / A, oracle_share=e["ora"] / O if O else 0.0,
                                       oracle_db=10 * np.log10(e["sse"] / max(e["sse"] - e["ora"], 1e-9)),
                                       **{k + "_db": 10 * np.log10(e["sse"] / max(e["sse"] - e[k], 1e-9)) for k in tddirs})
                               for v, e in sorted(d.items())}
    # concentration curve (oracle-ranked, all cases)
    g = np.concatenate([r["ora"].ravel() for rows in store.values() for r in rows])
    gs = np.sort(g)[::-1]
    cs = np.cumsum(gs) / max(gs.sum(), 1e-9)
    res["curve"] = {"%d%%" % p: float(cs[max(0, int(len(gs) * p / 100) - 1)]) for p in (5, 10, 20, 30, 50)}
    # region rule: choose the class combination (light x detail x motion x camera) whose blocks carry the most oracle gain
    # per area on TRAIN clips; select combos in that order until 60 % of train gain; score on held-out clips.
    train = set(a.train.split(","))

    def combos(rows_by_case, keep):
        tab = {}
        for cid, rows in rows_by_case.items():
            if (cid.split("_")[0] in train) != keep:
                continue
            for r in rows:
                cl = classes(r)
                key = np.char.add(np.char.add(np.char.add(cl["light"] + "|", cl["detail"]) , "|" + cl["motion"]), "|" + cl["camera"])
                for v in np.unique(key):
                    m = key == v
                    e = tab.setdefault(str(v), [0, 0.0])
                    e[0] += int(m.sum()); e[1] += float(r["ora"][m].sum())
        return tab
    tr, te = combos(store, True), combos(store, False)
    order = sorted(tr, key=lambda k: -tr[k][1] / max(tr[k][0], 1))
    tot = sum(v[1] for v in tr.values()); acc = 0.0; chosen = []
    for k in order:
        if acc >= 0.6 * tot:
            break
        chosen.append(k); acc += tr[k][1]
    A_te = sum(v[0] for v in te.values()); O_te = sum(v[1] for v in te.values())
    res["rule"] = dict(chosen=chosen, train_area=sum(tr[k][0] for k in chosen) / max(1, sum(v[0] for v in tr.values())),
                       train_gain=acc / max(tot, 1e-9),
                       test_area=sum(te.get(k, [0, 0])[0] for k in chosen) / max(1, A_te),
                       test_gain=sum(te.get(k, [0, 0])[1] for k in chosen) / max(O_te, 1e-9))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        json.dump(res, f, indent=1)
    print(json.dumps(res["curve"]), json.dumps({k: v for k, v in res["rule"].items() if k != "chosen"}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
