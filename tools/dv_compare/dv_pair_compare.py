#!/usr/bin/env python3
# Copyright(C) 2026 Victor McPherson
# SPDX-License-Identifier: GPL-3.0-or-later
"""DV comparison capture: pair an LLDV clip with a DV tunnel clip and score them (feat/dv-l8).

The owner records the same scene twice with VP's DV comparison capture (VP_DV_COMPARE_PATH,
see src/VP-V/modules/input/signal/DvCompareCapture.h): once in LLDV mode (the Apple TV's
certified Dolby mapping applied) and once as the DV tunnel (VP maps it). Each clip is a raw
capture session (segNN\\capture.txt, frames.csv, frame_NNNNNN.bin) plus dv-frames.jsonl (per
picture: source counter, decoded DM, the trim level / parameters VP chose, the target nits).

This tool
  1. pairs LLDV frames with tunnel frames by a PICTURE FINGERPRINT that ignores the tone
     curve (a rank-normalised thumbnail + gradients; dv_lldv_vs_tunnel.signature), tolerant
     of a different start, pauses, repeats and drops (dv_lldv_vs_tunnel.align);
  2. scores every pair, in PQ BT.2020 R'G'B' on a grid (default 480x270):
       - PSNR (dB, PQ signal 0..1),
       - Delta E ITP (ITU-R BT.2124) mean and p95,
       - the mean luminance difference in STOPS (log2 of the linear-light mean ratio, + =
         brighter than LLDV);
     for each tunnel picture available:
       - "source": the tunnel's own decode (the DM matrices from dv-frames.jsonl, the graded
         master before any display mapping) - how far the certified mapping moved the picture;
       - every --render NAME=FOLDER: VP's renders of the tunnel frames (e.g. bt2390+trims vs
         the Dolby-style mode), so the modes can be ranked against the certified reference;
  3. reports per pair (pairs.csv), per scene (L1 changes) and per mode (report.json/.md),
     with the trim level VP used.
Captured pixels are only read; nothing from them is copied into the report. Never commit
captures.

Usage:
  python dv_pair_compare.py --lldv <clip> --tunnel <clip> --out <folder>
         [--render bt2390=<folder> --render dolby=<folder> ...] [--grid 480x270]
  python dv_pair_compare.py --selftest
Needs numpy (and Pillow only for PNG renders).
"""
import argparse
import csv
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "dv_synth"))
import dv_lldv_vs_tunnel as base  # noqa: E402

KR, KG, KB = 0.2627, 0.6780, 0.0593   # BT.2020

# ------------------------------------------------------------------ clip I/O


def read_jsonl(path):
    clip, frames = None, {}
    if not os.path.exists(path):
        return clip, frames
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if rec.get("type") == "clip" and clip is None:
                clip = rec
            elif rec.get("type") == "frame":
                frames[int(rec["counter"])] = rec
    return clip, frames


def load_clip(folder):
    """The longest segment of a clip, plus its dv-frames.jsonl (beside the segments or in the
    segment folder)."""
    segs = base.find_segments(folder)
    if not segs:
        raise SystemExit("no capture segment under %s" % folder)
    seg = max((base.read_segment(f) for f in segs), key=lambda s: len(s["frames"]))
    meta = None
    for cand in (os.path.join(seg["folder"], "dv-frames.jsonl"), os.path.join(os.path.dirname(seg["folder"]), "dv-frames.jsonl"),
                 os.path.join(folder, "dv-frames.jsonl")):
        if os.path.exists(cand):
            meta = cand
            break
    seg["clip"], seg["meta"] = read_jsonl(meta) if meta else (None, {})
    return seg


def frame_meta(seg, i):
    return seg["meta"].get(seg["frames"][i]["counter"])


# ------------------------------------------------------------------ pictures


def tunnel_planes(seg, raw):
    """12-bit I and the 4:2:2 chroma of a tunnel picture: (h, w) I, (h, w/2) Ct, (h, w/2) Cp."""
    px = base.rgb8(seg, raw).astype(np.uint16)
    i12 = (px[..., 1] << 4) | (px[..., 2] & 15)
    c12 = (px[..., 0] << 4) | (px[..., 2] >> 4)
    return i12, c12[:, 0::2], c12[:, 1::2]


def tunnel_source_pq(seg, raw, dm):
    """The tunnel picture's own decode (the master, BT.2020 PQ R'G'B') from the DM matrices
    (dv-frames.jsonl); the chroma LSBs carrying the DM packet are noise below 1 code."""
    import dvsynth as dv
    i12, ct, cp = tunnel_planes(seg, raw)
    ct = np.repeat(ct, 2, axis=1)[:, : i12.shape[1]]
    cp = np.repeat(cp, 2, axis=1)[:, : i12.shape[1]]
    codes = np.stack([i12, ct, cp], axis=-1).astype(np.float64)
    if dm and dm.get("nonlinear"):
        ycc = [int(round(v * 8192)) for v in dm["nonlinear"]]
        off = [int(round(v * (1 << 28))) for v in dm["nonlinear_offset"]]
        lms = [int(round(v * 16384)) for v in dm["linear"]]
        rep = dv.Rep("dm", ycc, off, lms, 2)
    else:
        rep = dv.REP_P5
    return base.pq_oetf(np.clip(rep.decode(codes), 0.0, None))


def lum_linear(rgb_pq):
    lin = base.pq_eotf(rgb_pq)
    return KR * lin[..., 0] + KG * lin[..., 1] + KB * lin[..., 2]


def score(a_pq, b_pq, grid):
    """PSNR, dE ITP mean / p95 and the luminance difference in stops (b vs a) on a grid."""
    gw, gh = grid
    a = base.area_resize(a_pq, gw, gh)
    b = base.area_resize(b_pq, gw, gh)
    mse = float(np.mean((a - b) ** 2))
    psnr = float("inf") if mse <= 0 else 10.0 * math.log10(1.0 / mse)
    de = base.delta_e_itp(base.ictcp_from_pq_rgb(a), base.ictcp_from_pq_rgb(b))
    ya, yb = float(lum_linear(a).mean()), float(lum_linear(b).mean())
    stops = math.log2(yb / ya) if ya > 0 and yb > 0 else float("nan")
    return {"psnr_db": round(psnr, 3) if math.isfinite(psnr) else "inf", "de_itp_mean": round(float(de.mean()), 3),
            "de_itp_p95": round(float(np.percentile(de, 95)), 3), "stops": round(stops, 4) if math.isfinite(stops) else "nan"}


def intensity_of_pq(rgb_pq):
    return KR * rgb_pq[..., 0] + KG * rgb_pq[..., 1] + KB * rgb_pq[..., 2]


# ------------------------------------------------------------------ pairing


def pair(lsig, tsig, min_score=0.85):
    return base.align(lsig, tsig, min_score)


def scene_ids(seg):
    """A scene id per frame: a new scene when the DM's L1 changes (no metadata: one scene)."""
    out, last, sid = [], None, -1
    for i in range(len(seg["frames"])):
        m = frame_meta(seg, i)
        key = tuple(m["dm"]["l1"]) if m and m.get("dm") and m["dm"].get("l1") else None
        if key != last or sid < 0:
            sid += 1
            last = key
        out.append(sid)
    return out


# ------------------------------------------------------------------ the run


def run(args):
    lseg, tseg = load_clip(args.lldv), load_clip(args.tunnel)
    grid = tuple(int(v) for v in args.grid.lower().split("x"))
    print("LLDV  : %s (%s %dx%d, %d frames, %d metadata lines)" % (lseg["folder"], lseg.get("encoding"), lseg["width"], lseg["height"],
                                                                  len(lseg["frames"]), len(lseg["meta"])))
    print("tunnel: %s (%s %dx%d, %d frames, %d metadata lines)" % (tseg["folder"], tseg.get("encoding"), tseg["width"], tseg["height"],
                                                                  len(tseg["frames"]), len(tseg["meta"])))
    lpics = [base.lldv_rgb_pq(lseg, base.load_raw(lseg, i)) for i in range(len(lseg["frames"]))]
    lsig = [base.signature(intensity_of_pq(p)) for p in lpics]
    tsig = [base.signature(base.tunnel_intensity(tseg, base.load_raw(tseg, j))) for j in range(len(tseg["frames"]))]
    pairs = pair(lsig, tsig, args.min_score)
    scenes = scene_ids(tseg)
    renders = {}
    for spec in args.render or []:
        name, folder = spec.split("=", 1)
        renders[name] = folder
    rows = []
    for i, j, match in pairs:
        meta = frame_meta(tseg, j)
        dm = meta.get("dm") if meta else None
        trims = meta.get("trims", {}) if meta else {}
        row = {"lldv_frame": i, "tunnel_frame": j, "match": round(match, 4), "scene": scenes[j],
               "trim_level": trims.get("level", "-"), "target_nits": meta.get("target_nits", "-") if meta else "-"}
        src = tunnel_source_pq(tseg, base.load_raw(tseg, j), dm)
        for k, v in score(lpics[i], src, grid).items():
            row["source_" + k] = v
        for name, folder in renders.items():
            r = base.load_render(folder, j, args)
            if r is None:
                continue
            for k, v in score(lpics[i], r, grid).items():
                row[name + "_" + k] = v
        rows.append(row)
    os.makedirs(args.out, exist_ok=True)
    keys = sorted({k for r in rows for k in r})
    with open(os.path.join(args.out, "pairs.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    modes = ["source"] + list(renders)
    report = {"lldv": lseg["folder"], "tunnel": tseg["folder"], "pairs": len(rows), "lldv_frames": len(lpics),
              "tunnel_frames": len(tsig), "modes": {}, "scenes": {},
              "lldv_clip": lseg.get("clip"), "tunnel_clip": tseg.get("clip")}

    def summary(items, mode):
        vals = {}
        for k in ("psnr_db", "de_itp_mean", "de_itp_p95", "stops"):
            xs = [r[mode + "_" + k] for r in items if isinstance(r.get(mode + "_" + k), (int, float))]
            if xs:
                vals[k] = round(float(np.mean(xs)), 3)
        return vals
    for m in modes:
        report["modes"][m] = summary(rows, m)
    for sid in sorted({r["scene"] for r in rows}):
        items = [r for r in rows if r["scene"] == sid]
        report["scenes"][str(sid)] = {"frames": len(items), "trim_level": items[0]["trim_level"],
                                      **{m: summary(items, m) for m in modes}}
    ranked = sorted((m for m in renders if report["modes"].get(m)), key=lambda m: report["modes"][m].get("de_itp_mean", 1e9))
    report["ranking_by_de_itp"] = ranked
    with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    md = ["# DV comparison: LLDV (certified) vs tunnel", "", "- pairs: %d (LLDV %d frames, tunnel %d frames)" % (len(rows), len(lpics), len(tsig)), "",
          "| mode | PSNR dB | dE ITP mean | dE ITP p95 | stops vs LLDV |", "|---|---|---|---|---|"]
    for m in modes:
        s = report["modes"][m]
        md.append("| %s | %s | %s | %s | %s |" % (m, s.get("psnr_db", "-"), s.get("de_itp_mean", "-"), s.get("de_itp_p95", "-"), s.get("stops", "-")))
    if ranked:
        md += ["", "Closest to the certified LLDV (mean dE ITP): " + " < ".join(ranked)]
    md += ["", "`source` = the tunnel's own (unmapped) decode: the difference is the certified mapping itself. "
           "Stops > 0: brighter than LLDV. Pairs are found by picture structure, not timing."]
    with open(os.path.join(args.out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))
    return report


# ------------------------------------------------------------------ self-test


def selftest():
    """(1) metrics: identical -> PSNR inf, dE 0, 0 stops; twice the light -> +1 stop.
    (2) pairing: the same moving scene under two tone curves, the LLDV copy starting 11 frames
        late, pausing 4, dropping 3 and a 2nd start offset: exactly the true pairs.
    (3) the tunnel source decode through a real synthetic tunnel picture (dvsynth: DM packets,
        r210 low8) and the jsonl matrices matches the authored picture within 0.5 % light."""
    import tempfile
    import dvsynth as dv
    a = np.full((8, 8, 3), 0.5)
    s = score(a, a, (4, 4))
    assert s["psnr_db"] == "inf" and s["de_itp_mean"] == 0.0 and s["stops"] == 0.0, s
    lin = base.pq_eotf(a)
    s = score(a, base.pq_oetf(lin * 2.0), (4, 4))
    assert abs(s["stops"] - 1.0) < 1e-6, s
    # (2) pairing robustness.
    rng = np.random.default_rng(3)
    w, h, n = 160, 90, 80
    tex = base.area_resize(base.area_resize(rng.random((h + 60, w + 160)), 40, 22), w + 160, h + 60)

    def pic(k):
        img = tex[(k // 2) % 60:(k // 2) % 60 + h, (k * 2) % 160:(k * 2) % 160 + w].copy()
        img[(k * 5) % (h - 10):(k * 5) % (h - 10) + 10, 30:70] = 0.9 if (k // 10) % 2 else 0.05
        return np.clip(img, 0, 1)
    tsig = [base.signature(np.power(pic(k), 0.7)) for k in range(n)]
    order = list(range(11, 30)) + [29] * 4 + list(range(33, 50)) + list(range(52, 75))
    lsig = [base.signature(np.power(pic(k), 1.6) * 0.8 + 0.02) for k in order]
    pairs = pair(lsig, tsig)
    wrong = [(i, j) for i, j, _ in pairs if j != order[i]]
    assert not wrong, wrong
    assert len(pairs) >= 0.9 * len(order), len(pairs)
    print("selftest: pairing %d/%d LLDV frames, 0 wrong (late start, pause, drops)" % (len(pairs), len(order)))
    # (3) tunnel source decode.
    W, H = 256, 108
    yy, xx = np.mgrid[0:H, 0:W]
    nits = np.stack([5 + 300 * xx / W, 5 + 200 * yy / H, 20 + 0 * xx], axis=-1).astype(np.float64)
    rep = dv.REP_P5
    codes = rep.encode(nits)
    I = np.clip(np.rint(codes[..., 0]), 0, 4095).astype(np.uint16)
    C = np.empty((H, W), dtype=np.uint16)
    C[:, 0::2] = np.clip(np.rint(0.5 * (codes[:, 0::2, 1] + codes[:, 1::2, 1])), 0, 4095)
    C[:, 1::2] = np.clip(np.rint(0.5 * (codes[:, 0::2, 2] + codes[:, 1::2, 2])), 0, 4095)
    dm = {"rep": rep, "source_min_pq": 62, "source_max_pq": 3696, "ext": [dv.ext_l1(0, 3000, 1500)]}
    assert dv.embed(I, C, dv.packets(dv.dm_bytes(dm)))
    data = dv.container(dv.rgb8(I, C), "r210", "low8")
    tmp = tempfile.mkdtemp(prefix="dvpair-")
    seg = {"folder": tmp, "width": W, "height": H, "encoding": "r210", "row_bytes": dv.row_bytes("r210", W), "frames": []}
    jdm = {"nonlinear": [v / 8192.0 for v in rep.ycc], "nonlinear_offset": [v / float(1 << 28) for v in rep.off],
           "linear": [v / 16384.0 for v in rep.lms]}
    got = base.pq_eotf(tunnel_source_pq(seg, np.frombuffer(data, dtype=np.uint8), jdm))
    ratio = float(lum_linear(base.pq_oetf(got)).mean() / lum_linear(base.pq_oetf(nits)).mean())
    assert abs(ratio - 1.0) < 0.005, ratio
    print("selftest: tunnel source decode light ratio %.5f" % ratio)
    print("selftest: OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lldv")
    ap.add_argument("--tunnel")
    ap.add_argument("--out")
    ap.add_argument("--render", action="append", help="NAME=FOLDER of VP renders of the tunnel frames (repeatable)")
    ap.add_argument("--grid", default="480x270")
    ap.add_argument("--min-score", type=float, default=0.85)
    # How VP renders are read (as dv_lldv_vs_tunnel.load_render).
    ap.add_argument("--render-range", default="full", choices=["full", "limited"])
    ap.add_argument("--render-gamma", type=float, default=2.2)
    ap.add_argument("--render-peak", type=float, default=100.0)
    ap.add_argument("--render-black", type=float, default=0.0)
    ap.add_argument("--render-primaries", default="rec2020", choices=["rec2020", "rec709"])
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not (args.lldv and args.tunnel and args.out):
        ap.error("--lldv, --tunnel and --out are required")
    run(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
