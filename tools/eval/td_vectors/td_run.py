# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TemporalDetail CPU model (td_cli --stdio) on full-frame decoded encodes with a chosen vector source, scored on the fly.

Vectors: the 4 px consecutive-pair fields of vecgen.py, reduced to the model's 16 px block grid (TemporalDetailMotion.h:
Backward(seq) = pair seq -> seq-1 on seq's grid, Forward(seq) = pair seq-1 -> seq on seq-1's grid; vector = block mean;
trust = clamp(1 - spread / B, 0, 1), spread = RMS deviation of the 4 px cells from the block mean, as td_flow.py).
Source "X:master" = <vec>/<clip>_master/X (master-derived, the near-ideal ceiling), "X:coded" = <vec>/<case>/X (engine).

Scores per frame type (I/P/B): PSNR in / out vs master; fine-band energy; TD weight; plus
  grain   rms ratio out/in of eval_metrics.grain_residual over the master's flat tiles (+-5 % gate) and over all pixels
          away from master edges ("grain_all", includes legitimate structure change)
  ghost   regression of (out - in) on (in_{t-1} - in) over the input's motion mask (eval_metrics.ghost_coeff, vs input)
  worse   share of 16 px blocks whose SSE vs master rose by > 10 % (local harm), and the SSE added there (harm dB)
Optionally saves the output luma of frames [save0, save1) as float16 (banding evaluation).

  python -I tools/eval/td_vectors/td_run.py --data data.json --case tos_1080p_low --vec VEC --source disref:master \
         --out OUT.json [--gates-off N] [--budget 0.08] [--no-guard] [--force-weight 0] [--save DIR --save0 24 --save1 48]
"""
import argparse
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
sys.path.insert(0, os.path.dirname(HERE))
import eval_metrics as em  # noqa: E402
import vecgen as vg  # noqa: E402

TD_CLI = os.path.join(os.path.dirname(os.path.dirname(HERE)), "td-cli", "bin", "td_cli.exe")
B = 16


def block_vectors(F4, w, h):
    """4 px field (gh4, gw4, 2) -> (vx, vy, trust) on the 16 px grid (ceil(W/16) x ceil(H/16))."""
    gw, gh = -(-w // B), -(-h // B)
    k = B // vg.G
    P = np.pad(F4, ((0, gh * k - F4.shape[0]), (0, gw * k - F4.shape[1]), (0, 0)), mode="edge").astype(np.float64)
    R = P.reshape(gh, k, gw, k, 2)
    mu = R.mean(axis=(1, 3))
    spread = np.sqrt(((R - mu[:, None, :, None, :]) ** 2).sum(-1).mean(axis=(1, 3)))
    tr = np.clip(1.0 - spread / B, 0.0, 1.0)
    return mu[..., 0].astype(np.float32), mu[..., 1].astype(np.float32), tr.astype(np.float32), gw, gh


def pair_bytes(d, a, b, w, h):
    p = os.path.join(d, "flow_%06d_%06d.npy" % (a, b))
    vx, vy, tr, gw, gh = block_vectors(np.load(p), w, h)
    return struct.pack("<iiid", 1, gw, gh, float(B)) + vx.tobytes() + vy.tobytes() + tr.tobytes()


def bsse(x):
    h, w = x.shape
    gh, gw = -(-h // B), -(-w // B)
    P = np.zeros((gh * B, gw * B))
    P[:h, :w] = x
    return P.reshape(gh, B, gw, B).sum(axis=(1, 3))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--vec", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--gates-off", type=int, default=0)
    ap.add_argument("--budget", type=float, default=0.08)
    ap.add_argument("--no-guard", action="store_true")
    ap.add_argument("--no-clean", action="store_true")
    ap.add_argument("--force-weight", type=float, default=0.0)
    ap.add_argument("--strength", type=float, default=1.0)
    ap.add_argument("--boost-max", type=float, default=0.15)
    ap.add_argument("--stronger-margin", type=float, default=0.15)
    ap.add_argument("--unc-px", type=float, default=0.5)
    ap.add_argument("--max-age", type=float, default=2.0)
    ap.add_argument("--type-admission", action="store_true", help="admission from the TRUE frame types (measurement)")
    ap.add_argument("--cli", default=TD_CLI)
    ap.add_argument("--save", default=None)
    ap.add_argument("--save0", type=int, default=24)
    ap.add_argument("--save1", type=int, default=48)
    a = ap.parse_args(argv)
    with open(a.data, encoding="utf8") as f:
        case = next(c for c in json.load(f)["cases"] if c["id"] == a.case)
    w, h = case["w"], case["h"]
    M = vg.read_luma(case["master"], w, h, a.frames)
    C = vg.read_luma(case["coded"], w, h, a.frames)
    n = len(C)
    types = case["types"][:n]
    name, kind = a.source.split(":")
    vd = os.path.join(a.vec, case["clip"] + "_master" if kind == "master" else case["id"], name)
    hdr = "TDCLI1 %d %d 10 %g %d steady %g %d %d %g %g %g %g 0.75 3 %g 0 0 0 0 %g %d" % (
        w, h, case["fps"], h, a.budget, 0 if a.no_clean else 1, 0 if a.no_guard else 1, a.strength, a.max_age, a.boost_max,
        a.stronger_margin, a.unc_px, a.force_weight, a.gates_off)
    hdr += (" 1\n" if a.type_admission else "\n")
    tflag = {"I": 4, "P": 8, "B": 16}
    p = subprocess.Popen([a.cli, "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    p.stdin.write(hdr.encode("ascii"))
    tiles = em.select_flat_tiles(M[:8], 10)
    acc = {}
    g_in = g_out = ga_in = ga_out = 0.0
    gh_num = gh_den = 0.0
    worse_n = blocks_n = 0
    harm = 0.0
    saved = []
    refsrc, dist, qseries = {}, {}, []
    t0 = time.time()
    ms = []
    for t in range(n):
        p.stdin.write(b"FRM1" + struct.pack("<Qi", t, tflag[types[t]]))
        p.stdin.write(C[t].astype(np.float64).tobytes())
        if t > 0:
            p.stdin.write(pair_bytes(vd, t, t - 1, w, h))
            p.stdin.write(pair_bytes(vd, t - 1, t, w, h))
        else:
            p.stdin.write(struct.pack("<i", 0) * 2)
        p.stdin.flush()
        if p.stdout.read(4) != b"OUT1":
            raise RuntimeError("td_cli protocol error")
        out = np.frombuffer(p.stdout.read(8 * w * h), np.float64).reshape(h, w)
        ms.append(struct.unpack("<d", p.stdout.read(8))[0])
        ln = struct.unpack("<i", p.stdout.read(4))[0]
        st = json.loads(p.stdout.read(ln).decode("utf8"))
        # where the transferring blocks take their detail from (true types of the source frames) and the model's q
        for kv in filter(None, st.get("refs", "").split(",")):
            sq, cnt = (int(v) for v in kv.split(":"))
            rk = refsrc.setdefault(types[t], {})
            rk[types[sq]] = rk.get(types[sq], 0) + cnt
            dist[types[t]] = dist.get(types[t], 0.0) + abs(t - sq) * cnt
        qseries.append((types[t], st.get("q_mean", 0.0)))
        out = np.clip(np.floor(out + 0.5), 0, 1023)
        c, m = C[t].astype(np.float64), M[t].astype(np.float64)
        e_in, e_out = (c - m) ** 2, (out - m) ** 2
        Hm = m - ndimage.gaussian_filter(m, 1.0, mode="nearest")
        Ho = out - ndimage.gaussian_filter(out, 1.0, mode="nearest")
        Hc = c - ndimage.gaussian_filter(c, 1.0, mode="nearest")
        d = acc.setdefault(types[t], dict(n=0, se_in=0.0, se_out=0.0, hb_in=0.0, hb_out=0.0, Em=0.0, hbE_in=0.0, hbE_out=0.0,
                                          w=0.0, act=0.0))
        d["n"] += 1
        d["se_in"] += float(e_in.mean()); d["se_out"] += float(e_out.mean())
        d["hb_in"] += float(((Hc - Hm) ** 2).mean()); d["hb_out"] += float(((Ho - Hm) ** 2).mean())
        d["Em"] += float((Hm ** 2).mean()); d["hbE_in"] += float((Hc ** 2).mean()); d["hbE_out"] += float((Ho ** 2).mean())
        d["w"] += st["weight_mean"]; d["act"] += st["active_share"]
        if t >= 1:
            bi, bo = bsse(e_in), bsse(e_out)
            wm = bo > 1.1 * bi + 1e-6 * B * B
            worse_n += int(wm.sum()); blocks_n += wm.size
            harm += float((bo - bi)[wm].sum()) / (w * h)
            mm = em.motion_mask(c, C[t - 1].astype(np.float64), 10)
            if mm.any():
                dd = (C[t - 1].astype(np.float64) - c)[mm]
                gh_num += float(np.sum((out - c)[mm] * dd)); gh_den += float(np.sum(dd * dd))
        for (y, x, s) in tiles:
            ri = em.grain_residual(c[max(0, y - 8):y + s + 8, max(0, x - 8):x + s + 8])
            ro = em.grain_residual(out[max(0, y - 8):y + s + 8, max(0, x - 8):x + s + 8])
            g_in += float((ri ** 2).sum()); g_out += float((ro ** 2).sum())
        ri, ro = em.grain_residual(c), em.grain_residual(out)
        ga_in += float((ri ** 2).sum()); ga_out += float((ro ** 2).sum())
        if a.save and a.save0 <= t < a.save1:
            saved.append(out.astype(np.float16))
        if t % 12 == 0:
            print("  %s %s g%d t=%d %.0fs w=%.4f" % (a.case, a.source, a.gates_off, t, time.time() - t0, st["weight_mean"]),
                  flush=True)
    p.stdin.write(b"END1")
    p.stdin.close()
    p.wait()
    ty = {}
    for k, d in acc.items():
        nn = d["n"]
        ty[k] = dict(n=nn, psnr_in=10 * math.log10(1023 ** 2 / (d["se_in"] / nn)), psnr_out=10 * math.log10(1023 ** 2 / (d["se_out"] / nn)),
                     hb_err_in=d["hb_in"] / nn, hb_err_out=d["hb_out"] / nn, hb_keep_in=d["hbE_in"] / d["Em"],
                     hb_keep_out=d["hbE_out"] / d["Em"], weight=d["w"] / nn, active=d["act"] / nn)
        ty[k]["dpsnr"] = ty[k]["psnr_out"] - ty[k]["psnr_in"]
    tot_in = sum(d["se_in"] for d in acc.values()) / n
    tot_out = sum(d["se_out"] for d in acc.values()) / n
    res = dict(case=a.case, source=a.source, gates_off=a.gates_off, budget=a.budget, guard=not a.no_guard,
               force_weight=a.force_weight, types=ty, dpsnr_all=10 * math.log10(tot_in / tot_out),
               grain_flat=math.sqrt(g_out / g_in) if g_in > 0 else None, grain_all=math.sqrt(ga_out / ga_in),
               ghost=gh_num / gh_den if gh_den > 0 else None, worse_share=worse_n / max(1, blocks_n),
               harm_mse=harm / max(1, n - 1), model_ms=float(np.median(ms)), secs=time.time() - t0,
               max_age=a.max_age, type_admission=a.type_admission, ref_source_types=refsrc,
               ref_mean_distance={k: dist[k] / max(1, sum(refsrc[k].values())) for k in dist}, q_series=qseries)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        json.dump(res, f, indent=1)
    if a.save and saved:
        os.makedirs(a.save, exist_ok=True)
        np.save(os.path.join(a.save, "out_%d_%d.npy" % (a.save0, a.save1)), np.stack(saved))
    print("td_run %s %s gates_off=%d: dB B %+.3f all %+.3f grain %.3f ghost %s (%.0f s)" % (
        a.case, a.source, a.gates_off, ty.get("B", {}).get("dpsnr", float("nan")), res["dpsnr_all"], res["grain_flat"] or -1,
        res["ghost"], res["secs"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
