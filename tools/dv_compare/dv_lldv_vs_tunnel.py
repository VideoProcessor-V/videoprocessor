#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""LLDV-vs-tunnel check (DV autotest, 2026-10-17).

The same title segment is played twice from the Apple TV while VP records the
raw capture (VP_RAW_CAPTURE_PATH + the raw_capture key):
  (a) LLDV: the Apple TV's certified Dolby display management maps the title
      to the target the EDID declares; VP receives PQ BT.2020 (usually YCbCr
      4:2:2, captured as V210).
  (b) DV tunnel: VP receives the raw 12-bit IPT + DM and maps it itself.

This tool aligns the two recordings by picture content (manual playback:
different start points, pauses, seeks), compares only overlapping frames, and
reports, per frame and per scene:
  - Delta E ITP (ITU-R BT.2124) between the LLDV picture and VP's rendering of
    the tunnel picture to the same target (VP's renders come from the GPU test
    DvAutotestGpuRendersTunnelCorpusForLldvCompare: --tunnel-render);
  - whether the LLDV brightness tracks the DM L1 scene values VP decoded
    (vp-decoded.csv, written by the CPU test RecordedCorpusReplaysThroughTheTunnelPath).

Usage (one command; tools/dv_lldv_vs_tunnel.ps1 wraps the whole chain):
  python dv_lldv_vs_tunnel.py --lldv <recording> --tunnel <recording>
         [--tunnel-render <folder of VP render PNGs>] --out <report folder>
  python dv_lldv_vs_tunnel.py --selftest

Only numpy (and Pillow for PNG renders) are needed. Captured pixels are read
from the recordings and never copied into the report.
"""
import argparse
import csv
import json
import math
import os
import sys

import numpy as np

THUMB_W, THUMB_H = 96, 54

# ---------------------------------------------------------------- corpus I/O


def read_segment(folder):
    """A VP raw-capture segment (capture.txt) or a StreamRecorder folder."""
    seg = {"folder": folder, "frames": []}
    sidecar = os.path.join(folder, "capture.txt")
    if os.path.exists(sidecar):
        keys = {}
        with open(sidecar, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    keys[k.strip()] = v.strip()
        seg.update(keys=keys, width=int(keys["width"]), height=int(keys["height"]),
                   encoding=keys.get("encoding", ""), row_bytes=int(keys.get("bytes_per_row", "0") or 0),
                   rate=float(keys.get("rate_hz", "0") or 0))
        with open(os.path.join(folder, "frames.csv"), encoding="utf-8") as f:
            for row in csv.DictReader(f):
                seg["frames"].append({"file": row["file"], "counter": int(row["counter"])})
        return seg
    events = os.path.join(folder, "events.txt")
    if os.path.exists(events):
        seg["keys"] = {}
        with open(events, encoding="utf-8", errors="replace") as f:
            for line in f:
                if ": mode " in line and "width" not in seg:
                    words = line.split(": mode ", 1)[1].split()
                    for k in range(len(words) - 2):
                        if "x" in words[k] and words[k + 2] == "Hz":
                            w, h = words[k].split("x")
                            if w.isdigit() and h.isdigit():
                                seg.update(width=int(w), height=int(h), rate=float(words[k + 1]),
                                           encoding=words[k + 3] if k + 3 < len(words) else "")
                                break
                if ": raw frame saved (" in line:
                    idx = int(line.split()[1].rstrip(":"))
                    rb = int(line.split("(")[1].split(",")[1].split()[0])
                    seg["row_bytes"] = rb
                    seg["frames"].append({"file": "raw_%05d.bin" % idx, "counter": idx + 1})
        return seg
    return None


def find_segments(root):
    out = []
    for dirpath, _dirs, files in os.walk(root):
        if "capture.txt" in files or ("events.txt" in files and "frames.csv" in files):
            out.append(dirpath)
    return sorted(out)


def load_raw(seg, i):
    with open(os.path.join(seg["folder"], seg["frames"][i]["file"]), "rb") as f:
        return np.frombuffer(f.read(), dtype=np.uint8)


def enc_name(seg):
    e = seg.get("encoding", "").lower()
    aliases = {"8bitargb": "argb", "8bitbgra": "bgra", "10bitrgb": "r210", "10bitrgbx": "r10b",
               "10bitrgbxle": "r10l", "10bityuv": "v210", "8bityuv": "uyvy"}
    for k, v in aliases.items():
        if e == k:
            return v
    for name in ("r210", "r10b", "r10l", "v210", "uyvy", "hdyc", "argb", "bgra", "r12b", "r12l"):
        if name in e:
            return name
    return e


def words_be(raw, h, rb, w):
    rows = raw[: h * rb].reshape(h, rb)[:, : w * 4]
    return rows.reshape(h, w, 4).astype(np.uint32)


def rgb10_fields(seg, raw):
    """10-bit RGB fields (h, w, 3) from r210 / R10b / R10l."""
    w, h, rb = seg["width"], seg["height"], seg["row_bytes"] or ((seg["width"] + 63) // 64) * 256
    b = words_be(raw, h, rb, w)
    e = enc_name(seg)
    if e == "r10l":
        word = (b[..., 3] << 24) | (b[..., 2] << 16) | (b[..., 1] << 8) | b[..., 0]
    else:
        word = (b[..., 0] << 24) | (b[..., 1] << 16) | (b[..., 2] << 8) | b[..., 3]
    if e in ("r10b", "r10l"):
        word >>= 2
    return np.stack([(word >> 20) & 1023, (word >> 10) & 1023, word & 1023], axis=-1)


def rgb8(seg, raw):
    """The RGB8 bytes of an 8-bit RGB signal (tunnel container), placement auto."""
    e = enc_name(seg)
    w, h = seg["width"], seg["height"]
    if e in ("argb", "bgra"):
        rb = seg["row_bytes"] or w * 4
        px = raw[: h * rb].reshape(h, rb)[:, : w * 4].reshape(h, w, 4)
        return px[..., 1:4] if e == "argb" else px[..., 2::-1]
    f = rgb10_fields(seg, raw)
    high8 = not np.any(f & 3)
    return ((f >> 2) if high8 else f).astype(np.uint8)


def tunnel_intensity(seg, raw):
    """The 12-bit I plane of a DV tunnel picture (HDMI 4:2:2 12-bit in RGB8)."""
    px = rgb8(seg, raw).astype(np.uint16)
    return ((px[..., 1] << 4) | (px[..., 2] & 15)).astype(np.float32) / 4095.0


def v210_ycbcr(seg, raw):
    w, h = seg["width"], seg["height"]
    rb = seg["row_bytes"] or ((w + 47) // 48) * 128
    rows = raw[: h * rb].reshape(h, rb)
    words = rows.view("<u4").astype(np.uint32)
    comps = np.stack([words & 1023, (words >> 10) & 1023, (words >> 20) & 1023], axis=-1).reshape(h, -1)
    # 12 components per 4 words: Cb0 Y0 Cr0 Y1 Cb1 Y2 Cr1 Y3 Cb2 Y4 Cr2 Y5
    groups = comps[:, : (comps.shape[1] // 12) * 12].reshape(h, -1, 12)
    y = groups[:, :, [1, 3, 5, 7, 9, 11]].reshape(h, -1)[:, :w]
    cb = groups[:, :, [0, 4, 8]].reshape(h, -1)[:, : w // 2]
    cr = groups[:, :, [2, 6, 10]].reshape(h, -1)[:, : w // 2]
    return y.astype(np.float32), np.repeat(cb, 2, axis=1)[:, :w].astype(np.float32), np.repeat(cr, 2, axis=1)[:, :w].astype(np.float32), 10


def uyvy_ycbcr(seg, raw):
    w, h = seg["width"], seg["height"]
    rb = seg["row_bytes"] or w * 2
    px = raw[: h * rb].reshape(h, rb)[:, : w * 2].reshape(h, w // 2, 4).astype(np.float32)
    y = px[..., [1, 3]].reshape(h, w)
    cb = np.repeat(px[..., 0], 2, axis=1)
    cr = np.repeat(px[..., 2], 2, axis=1)
    return y, cb, cr, 8


def lldv_rgb_pq(seg, raw):
    """Non-linear PQ R'G'B' (0..1) of an LLDV / HDR10 capture, BT.2020."""
    e = enc_name(seg)
    if e in ("v210", "uyvy", "hdyc"):
        y, cb, cr, bits = v210_ycbcr(seg, raw) if e == "v210" else uyvy_ycbcr(seg, raw)
        scale = float(1 << (bits - 8))
        yn = (y - 16 * scale) / (219 * scale)
        pb = (cb - 128 * scale) / (224 * scale)
        pr = (cr - 128 * scale) / (224 * scale)
        kr, kb = 0.2627, 0.0593   # BT.2020 non-constant luminance
        kg = 1 - kr - kb
        r = yn + 2 * (1 - kr) * pr
        b = yn + 2 * (1 - kb) * pb
        g = (yn - kr * r - kb * b) / kg
        return np.clip(np.stack([r, g, b], axis=-1), 0, 1)
    if e in ("r210", "r10b", "r10l"):
        f = rgb10_fields(seg, raw).astype(np.float32)
        return np.clip((f - 64) / 876.0, 0, 1)   # DeckLink 10-bit RGB is limited range
    if e in ("argb", "bgra"):
        return rgb8(seg, raw).astype(np.float32) / 255.0
    raise SystemExit("LLDV recording encoding %s is not supported here (record LLDV as YCbCr 4:2:2, i.e. V210)" % seg.get("encoding"))


# ------------------------------------------------------------ colour maths

M1, M2, C1, C2, C3 = 2610 / 16384, 2523 / 4096 * 128, 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq_eotf(e):
    e = np.clip(e, 0, 1)
    p = np.power(e, 1 / M2)
    return 10000.0 * np.power(np.maximum(p - C1, 0) / (C2 - C3 * p), 1 / M1)


def pq_oetf(nits):
    y = np.clip(nits / 10000.0, 0, 1)
    p = np.power(y, M1)
    return np.power((C1 + C2 * p) / (1 + C3 * p), M2)


RGB2020_TO_LMS = np.array([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], dtype=np.float64) / 4096
LMS_TO_ICTCP = np.array([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], dtype=np.float64) / 4096


def ictcp_from_pq_rgb(rgb_pq):
    lin = pq_eotf(rgb_pq.astype(np.float64))
    lms = lin @ RGB2020_TO_LMS.T
    return pq_oetf(lms) @ LMS_TO_ICTCP.T


def delta_e_itp(a, b):
    """ITU-R BT.2124 Delta E ITP (T = 0.5 Ct)."""
    d = a - b
    return 720.0 * np.sqrt(d[..., 0] ** 2 + (0.5 * d[..., 1]) ** 2 + d[..., 2] ** 2)


def area_resize(img, w, h):
    """Box-filter resize to (h, w) for (H, W[, C]) arrays."""
    H, W = img.shape[:2]
    if h > H or w > W:
        # Upscaling: nearest sample (box bins would be empty).
        yi = np.minimum(np.arange(h) * H // h, H - 1)
        xi = np.minimum(np.arange(w) * W // w, W - 1)
        return img[yi][:, xi].astype(np.float64)
    ys = (np.arange(h + 1) * H // h)
    xs = (np.arange(w + 1) * W // w)
    cs = np.cumsum(np.cumsum(np.pad(img.astype(np.float64), ((1, 0), (1, 0)) + ((0, 0),) * (img.ndim - 2)), 0), 1)
    tot = cs[ys[1:]][:, xs[1:]] - cs[ys[:-1]][:, xs[1:]] - cs[ys[1:]][:, xs[:-1]] + cs[ys[:-1]][:, xs[:-1]]
    area = ((ys[1:] - ys[:-1])[:, None] * (xs[1:] - xs[:-1])[None, :]).astype(np.float64)
    if img.ndim == 3:
        area = area[..., None]
    return tot / area


# ------------------------------------------------------------ alignment

def signature(intensity):
    """A tone-curve-independent structure signature: the rank-normalised
    thumbnail plus its gradients, unit length (two pictures of the same frame
    under different monotone mappings correlate near 1)."""
    t = area_resize(intensity, THUMB_W, THUMB_H)
    flat = t.ravel()
    ranks = np.empty_like(flat)
    ranks[np.argsort(flat, kind="stable")] = np.linspace(-1, 1, flat.size)
    r = ranks.reshape(t.shape)
    gx = np.diff(r, axis=1).ravel()
    gy = np.diff(r, axis=0).ravel()
    v = np.concatenate([r.ravel() * 0.5, gx, gy])
    v -= v.mean()
    n = np.linalg.norm(v)
    return v / n if n > 0 else v


def align(sig_a, sig_b, min_score=0.85, run_min=6, margin=0.01):
    """Pairs (i, j) of matching frames between recordings A and B. Each A frame
    takes its best B frame; a pair is kept when its score clears min_score and
    it either beats the runner-up (outside +-2 frames) by margin, or belongs to
    a run of >= run_min A frames with a constant offset (+-1): playback runs.
    Pauses (A or B repeating a frame) and seeks give several runs."""
    A = np.asarray(sig_a)
    B = np.asarray(sig_b)
    scores = A @ B.T
    best = scores.argmax(axis=1)
    best_score = scores[np.arange(len(A)), best]
    keep = np.zeros(len(A), dtype=bool)
    for i in range(len(A)):
        if best_score[i] < min_score:
            continue
        s = scores[i].copy()
        lo, hi = max(0, best[i] - 2), min(len(B), best[i] + 3)
        s[lo:hi] = -1
        if best_score[i] - s.max() >= margin:
            keep[i] = True
    # runs of constant offset
    offset = best - np.arange(len(A))
    start = 0
    for i in range(1, len(A) + 1):
        if i == len(A) or abs(int(offset[i]) - int(offset[i - 1])) > 1 or best_score[i] < min_score:
            if i - start >= run_min:
                keep[start:i] |= best_score[start:i] >= min_score
            start = i
    return [(int(i), int(best[i]), float(best_score[i])) for i in range(len(A)) if keep[i]]


# ------------------------------------------------------------ the check

def load_decoded_csv(folder):
    path = os.path.join(folder, "vp-decoded.csv")
    if not os.path.exists(path):
        return None
    rows = {}
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            rows[int(row["frame"])] = row
    return rows


def scenes_from_l1(decoded, count):
    """Scene id per tunnel frame: a new scene when the L1 tuple changes."""
    scene, last, out = -1, None, []
    for i in range(count):
        row = decoded.get(i) if decoded else None
        key = (row["L1_min"], row["L1_max"], row["L1_avg"]) if row and row["valid"] == "1" else None
        if key != last:
            scene += 1
            last = key
        out.append(scene)
    return out


def load_render(folder, j, args):
    """VP's render of tunnel frame j as PQ BT.2020 R'G'B' (0..1).

    VP outputs display-referred gamma (output_gamma) at its tone-map target
    (sdr_target_nits / sdr_black_nits, target_primaries); the GPU test renders
    with the target set to the LLDV EDID's luminance range. The rendered-output
    capture PNG holds 10-bit codes as 16-bit ((c << 6) | (c >> 4)). Codes are
    turned into absolute luminance with the pure power law between black and
    peak (an approximation of the display's EOTF; state it in the report), then
    into PQ, so both recordings are compared in the same absolute domain."""
    from PIL import Image  # Pillow, only for the renders
    for name in ("frame_%06d.png" % (j + 1), "frame_%06d.png" % j, "%06d.png" % j):
        p = os.path.join(folder, name)
        if os.path.exists(p):
            img = np.asarray(Image.open(p))
            img = img[..., :3].astype(np.float64)
            code = img / 65535.0 * 1023.0 if img.max() > 255 else img / 255.0 * 1023.0
            if args.render_range == "limited":
                v = np.clip((code - 64.0) / 876.0, 0, 1)
            else:
                v = np.clip(code / 1023.0, 0, 1)
            nits = args.render_black + (args.render_peak - args.render_black) * np.power(v, args.render_gamma)
            if args.render_primaries == "rec709":
                # BT.709 -> BT.2020 linear (ITU-R BT.2087)
                m = np.array([[0.6274, 0.3293, 0.0433], [0.0691, 0.9195, 0.0114], [0.0164, 0.0880, 0.8956]])
                nits = nits @ m.T
            return pq_oetf(nits)
    return None


def run(args):
    lldv_segments = find_segments(args.lldv)
    tunnel_segments = find_segments(args.tunnel)
    if not lldv_segments or not tunnel_segments:
        raise SystemExit("no recording found under --lldv or --tunnel")
    os.makedirs(args.out, exist_ok=True)
    # Use the longest segment of each recording (the title, not the menu).
    lseg = max((read_segment(f) for f in lldv_segments), key=lambda s: len(s["frames"]))
    tseg = max((read_segment(f) for f in tunnel_segments), key=lambda s: len(s["frames"]))
    print("LLDV  : %s (%s %dx%d, %d frames)" % (lseg["folder"], lseg.get("encoding"), lseg["width"], lseg["height"], len(lseg["frames"])))
    print("tunnel: %s (%s %dx%d, %d frames)" % (tseg["folder"], tseg.get("encoding"), tseg["width"], tseg["height"], len(tseg["frames"])))
    lsig, lmean = [], []
    for i in range(len(lseg["frames"])):
        rgb = lldv_rgb_pq(lseg, load_raw(lseg, i))
        y = 0.2627 * rgb[..., 0] + 0.6780 * rgb[..., 1] + 0.0593 * rgb[..., 2]
        lsig.append(signature(y))
        lmean.append(float(y.mean()))
    tsig = [signature(tunnel_intensity(tseg, load_raw(tseg, j))) for j in range(len(tseg["frames"]))]
    pairs = align(lsig, tsig, args.min_score)
    decoded = load_decoded_csv(tseg["folder"])
    scene_of = scenes_from_l1(decoded, len(tseg["frames"]))
    report = {"lldv": lseg["folder"], "tunnel": tseg["folder"], "pairs": len(pairs),
              "lldv_frames": len(lseg["frames"]), "tunnel_frames": len(tseg["frames"]), "scenes": {}}
    per_frame = []
    for i, j, score in pairs:
        entry = {"lldv_frame": i, "tunnel_frame": j, "match": round(score, 4), "scene": scene_of[j],
                 "lldv_mean_pq": round(lmean[i], 5)}
        if decoded and j in decoded and decoded[j]["valid"] == "1":
            entry["l1_avg_pq"] = int(decoded[j]["L1_avg"]) / 4095.0
            entry["l1_max_pq"] = int(decoded[j]["L1_max"]) / 4095.0
        if args.tunnel_render:
            render = load_render(args.tunnel_render, j, args)
            if render is not None:
                lrgb = lldv_rgb_pq(lseg, load_raw(lseg, i))
                gw, gh = args.grid
                a = ictcp_from_pq_rgb(area_resize(lrgb, gw, gh))
                b = ictcp_from_pq_rgb(area_resize(render, gw, gh))
                de = delta_e_itp(a, b)
                entry["delta_e_itp_mean"] = round(float(de.mean()), 3)
                entry["delta_e_itp_p95"] = round(float(np.percentile(de, 95)), 3)
                entry["delta_i_mean"] = round(float((b[..., 0] - a[..., 0]).mean() * 720), 3)
        per_frame.append(entry)
    with open(os.path.join(args.out, "per_frame.csv"), "w", newline="", encoding="utf-8") as f:
        keys = sorted({k for e in per_frame for k in e})
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for e in per_frame:
            w.writerow(e)
    # Per scene: Delta E ITP, and does LLDV brightness follow L1?
    scenes = {}
    for e in per_frame:
        scenes.setdefault(e["scene"], []).append(e)
    lines = []
    l1x, lldvy = [], []
    for s, items in sorted(scenes.items()):
        rec = {"frames": len(items)}
        des = [e["delta_e_itp_mean"] for e in items if "delta_e_itp_mean" in e]
        p95 = [e["delta_e_itp_p95"] for e in items if "delta_e_itp_p95" in e]
        if des:
            rec["delta_e_itp_mean"] = round(float(np.mean(des)), 3)
            rec["delta_e_itp_p95"] = round(float(np.mean(p95)), 3)
            rec["delta_i_mean"] = round(float(np.mean([e["delta_i_mean"] for e in items])), 3)
            rec["pass"] = rec["delta_e_itp_mean"] <= args.pass_mean and rec["delta_e_itp_p95"] <= args.pass_p95
        l1 = [e["l1_avg_pq"] for e in items if "l1_avg_pq" in e]
        if l1:
            rec["l1_avg_pq"] = round(float(np.mean(l1)), 4)
            rec["lldv_mean_pq"] = round(float(np.mean([e["lldv_mean_pq"] for e in items])), 4)
            l1x.append(rec["l1_avg_pq"])
            lldvy.append(rec["lldv_mean_pq"])
        scenes[s] = rec
        report["scenes"][str(s)] = rec
    if len(l1x) >= 3 and np.std(l1x) > 0 and np.std(lldvy) > 0:
        report["l1_tracking_correlation"] = round(float(np.corrcoef(l1x, lldvy)[0, 1]), 3)
    with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    md = ["# LLDV vs DV tunnel check", "",
          "- LLDV recording: `%s` (%d frames)" % (lseg["folder"], len(lseg["frames"])),
          "- Tunnel recording: `%s` (%d frames)" % (tseg["folder"], len(tseg["frames"])),
          "- Matched (overlapping) frames: %d" % len(pairs), ""]
    if "l1_tracking_correlation" in report:
        md.append("- LLDV brightness vs DM L1 average, across scenes: correlation %.3f (near 1: the Apple TV's mapping follows L1)" % report["l1_tracking_correlation"])
    if not args.tunnel_render:
        md.append("- No --tunnel-render folder: the mapping comparison (Delta E ITP) needs VP's renders of the tunnel frames "
                  "(GPU test DvAutotestGpuRendersTunnelCorpusForLldvCompare).")
    md += ["", "| scene | frames | dE ITP mean | dE ITP p95 | dI (VP - LLDV) | L1 avg PQ | LLDV mean PQ | pass |", "|---|---|---|---|---|---|---|---|"]
    for s, rec in sorted(scenes.items()):
        md.append("| %s | %d | %s | %s | %s | %s | %s | %s |" % (s, rec["frames"], rec.get("delta_e_itp_mean", "-"), rec.get("delta_e_itp_p95", "-"),
                  rec.get("delta_i_mean", "-"), rec.get("l1_avg_pq", "-"), rec.get("lldv_mean_pq", "-"),
                  {True: "pass", False: "DEPARTS"}.get(rec.get("pass"), "-")))
    md += ["", "Thresholds: mean dE ITP <= %.1f and p95 <= %.1f per scene. One dE ITP unit is about one just-noticeable "
           "difference; Dolby's DM version and VP's mapping differ by design, so departures say where, not that VP is wrong." % (args.pass_mean, args.pass_p95)]
    with open(os.path.join(args.out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))
    return report


# ------------------------------------------------------------ self-test

def selftest():
    """Synthetic recordings: the same moving scene, (a) as V210 LLDV mapped
    by one curve, (b) as an r210 tunnel I plane under another curve; LLDV
    starts 7 frames later, pauses 5 frames and skips 4. The aligner must find
    exactly the true pairs; Delta E ITP of an identical render is zero."""
    import tempfile
    rng = np.random.default_rng(7)
    w, h, n = 192, 108, 60
    base = rng.random((h + 40, w + 120))
    base = area_resize(area_resize(base, 48, 27), w + 120, h + 40)   # smooth texture

    def scene(k):
        img = base[(k // 3) % 40:(k // 3) % 40 + h, (k * 2) % 120:(k * 2) % 120 + w].copy()
        img[(k * 3) % h: (k * 3) % h + 12, 20:60] = 0.95 if (k // 15) % 2 else 0.1   # moving bar
        return np.clip(img, 0, 1)

    tmp = tempfile.mkdtemp(prefix="dvcmp-")
    tdir, ldir = os.path.join(tmp, "tunnel", "seg01"), os.path.join(tmp, "lldv", "seg01")
    os.makedirs(tdir)
    os.makedirs(ldir)
    trb = ((w + 63) // 64) * 256
    lrb = ((w + 47) // 48) * 128

    def write_seg(d, enc, rb, frames):
        with open(os.path.join(d, "capture.txt"), "w") as f:
            f.write("format=vp-raw-capture-1\nwidth=%d\nheight=%d\nrate_hz=23.976024\nencoding=%s\nbytes_per_row=%d\n" % (w, h, enc, rb))
        with open(os.path.join(d, "frames.csv"), "w") as f:
            f.write("sequence,file,counter,timing_timestamp,capture_timestamp,arrival_qpc,source_discontinuity\n")
            for i, data in enumerate(frames):
                name = "frame_%06d.bin" % (i + 1)
                with open(os.path.join(d, name), "wb") as g:
                    g.write(data)
                f.write("%d,%s,%d,0,0,0,0\n" % (i + 1, name, i + 1))

    tframes = []
    for k in range(n):
        i12 = (np.power(scene(k), 0.8) * 4095).astype(np.uint32)
        c12 = np.full_like(i12, 2048)
        r, g, b = c12 >> 4, i12 >> 4, ((c12 & 15) << 4) | (i12 & 15)
        word = (r << 20) | (g << 10) | b   # low8 placement
        row = np.zeros((h, trb), dtype=np.uint8)
        be = word.astype(">u4").view(np.uint8).reshape(h, w * 4)
        row[:, : w * 4] = be
        tframes.append(row.tobytes())
    order = list(range(7, 30)) + [29] * 5 + list(range(34, 55))   # start late, pause, skip
    lframes = []
    for k in order:
        y10 = (64 + np.power(scene(k), 1.3) * 876).astype(np.uint32)
        out = np.zeros((h, lrb // 4), dtype=np.uint32)
        for gi in range(w // 6):
            ys = y10[:, gi * 6: gi * 6 + 6]
            c = np.full((h,), 512, dtype=np.uint32)
            out[:, gi * 4 + 0] = c | (ys[:, 0] << 10) | (c << 20)
            out[:, gi * 4 + 1] = ys[:, 1] | (c << 10) | (ys[:, 2] << 20)
            out[:, gi * 4 + 2] = c | (ys[:, 3] << 10) | (c << 20)
            out[:, gi * 4 + 3] = ys[:, 4] | (c << 10) | (ys[:, 5] << 20)
        lframes.append(out.astype("<u4").tobytes())
    write_seg(tdir, "r210", trb, tframes)
    write_seg(ldir, "V210", lrb, lframes)
    lseg, tseg = read_segment(ldir), read_segment(tdir)
    lsig = [signature(0.2627 * x[..., 0] + 0.678 * x[..., 1] + 0.0593 * x[..., 2])
            for x in (lldv_rgb_pq(lseg, load_raw(lseg, i)) for i in range(len(order)))]
    tsig = [signature(tunnel_intensity(tseg, load_raw(tseg, j))) for j in range(n)]
    pairs = align(lsig, tsig)
    wrong = [(i, j) for i, j, _ in pairs if j != order[i]]
    coverage = len(pairs) / len(order)
    print("selftest: %d/%d LLDV frames matched, %d wrong" % (len(pairs), len(order), len(wrong)))
    assert not wrong, wrong
    assert coverage >= 0.9, coverage
    a = ictcp_from_pq_rgb(np.full((4, 4, 3), 0.5))
    assert float(delta_e_itp(a, a).max()) == 0.0
    # A known BT.2124 example: PQ 0.5 grey vs 0.51 grey is a few dE ITP.
    d = float(delta_e_itp(ictcp_from_pq_rgb(np.full((1, 1, 3), 0.5)), ictcp_from_pq_rgb(np.full((1, 1, 3), 0.51))).mean())
    assert 5 < d < 9, d
    print("selftest: OK (aligner exact with late start, pause and skip; dE ITP grey step %.2f)" % d)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--lldv")
    p.add_argument("--tunnel")
    p.add_argument("--tunnel-render")
    p.add_argument("--out", default="dv-lldv-vs-tunnel-report")
    p.add_argument("--min-score", type=float, default=0.85)
    p.add_argument("--pass-mean", type=float, default=6.0)
    p.add_argument("--pass-p95", type=float, default=15.0)
    p.add_argument("--grid", type=int, nargs=2, default=(480, 270))
    p.add_argument("--render-gamma", type=float, default=2.4, help="VP output_gamma used for the renders")
    p.add_argument("--render-peak", type=float, default=1000.0, help="sdr_target_nits used for the renders (= the LLDV EDID's maximum)")
    p.add_argument("--render-black", type=float, default=0.005, help="sdr_black_nits used for the renders (= the LLDV EDID's minimum)")
    p.add_argument("--render-range", choices=("full", "limited"), default="full")
    p.add_argument("--render-primaries", choices=("bt2020", "rec709"), default="bt2020")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()
    if args.selftest:
        return selftest()
    if not args.lldv or not args.tunnel:
        p.error("--lldv and --tunnel are required")
    run(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
