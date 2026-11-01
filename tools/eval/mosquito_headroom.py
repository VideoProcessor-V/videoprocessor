# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Mosquito headroom: how much near-edge ringing / mosquito error remains vs the master in streaming-like
encodes, per codec x rate, at 1080p and 2160p.  CPU only; scratch pixels outside the worktree.

  python -I tools/eval/mosquito_headroom.py --scratch D:/codex/videoprocessor/eval-scratch/<dir> \
         --out tools/eval/prototypes/mosquito_headroom

Per master (4: film grain, animation edges, film/CG, high detail) and raster: a 1024x768 window of 12
frames (2160p: native crop of the 4K master; 1080p: Lanczos downscale of the whole 4K master to 1080p,
then the crop) stored as yuv420p10le = the reference.  Encodes (ffmpeg, encoder defaults otherwise):
  H.264 libx264 8-bit, deblocking on (default), preset medium, CRF 23 / 28 (reference = the 8-bit master
        made by the same ffmpeg conversion, so 8-bit quantisation is not counted as codec error)
  HEVC  libx265 10-bit, SAO on (default), preset medium, CRF 24 / 30
  AV1   libaom-av1 10-bit, cpu-used 6, CRF 30 / 40 (SVT-AV1 is not in this ffmpeg build)
  all GOP 48.
Luma metrics (10-bit codes) with the harness near-edge mask (eval_metrics.near_edge_zone of the master):
  ring_rms      RMS of the high-passed error (err - gauss(err, 1)) in the near-edge zone
  ring_share    near-edge high-passed error energy / total error energy (whole window)
  smooth_share  the same restricted to near-edge pixels where the MASTER is locally smooth (local HF energy
                <= 2x its noise level): error there is artefact, not lost detail -> what a perfect
                mosquito repair could remove
  headroom_db   PSNR gain if all of smooth_share were removed: -10 log10(1 - smooth_share)
  kbps_full     bitrate scaled to the full raster (bits/s x full pixels / window pixels)
"""
import argparse
import csv
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402

FF = eio.DEFAULT_FFMPEG
T = r"D:\testclips\eval-2026-10-31"
W, H, N = 1024, 768, 12
MASTERS = [
    # name, path, seek seconds, fps, source WxH, content
    ("nocturne", T + r"\hdr_film_grain\nocturne_filmgrain_hdr_60p_t120\nocturne_filmgrain_hdr_60p_t120.mov", 1.9917, 60.0,
     (3840, 2160), "film grain (synthetic Vision3), live action, PQ"),
    ("sol_levante", T + r"\hdr\sol_levante_hdr10_24p_t150\sol_levante_hdr10_24p_t150.mov", 4.979, 24.0,
     (3840, 2160), "animation: hard edges, flat colour, PQ"),
    ("tos4k", r"D:\nn-corpus\trimmed\tos4k\reference_ffv1.mkv", 99.979, 24.0, (3840, 1714), "film / CG mix, text-like edges"),
    ("park_joy", r"D:\hfr\park_joy_2160p50.y4m", 1.39, 50.0, (3840, 2160), "high detail (trees), camera pan"),
]
ENCODES = [
    ("H.264", "x264 crf23", ["-c:v", "libx264", "-preset", "medium", "-crf", "23", "-g", "48", "-pix_fmt", "yuv420p"], 8),
    ("H.264", "x264 crf28", ["-c:v", "libx264", "-preset", "medium", "-crf", "28", "-g", "48", "-pix_fmt", "yuv420p"], 8),
    ("HEVC", "x265 crf24", ["-c:v", "libx265", "-preset", "medium", "-crf", "24", "-g", "48", "-pix_fmt", "yuv420p10le",
                            "-x265-params", "log-level=error"], 10),
    ("HEVC", "x265 crf30", ["-c:v", "libx265", "-preset", "medium", "-crf", "30", "-g", "48", "-pix_fmt", "yuv420p10le",
                            "-x265-params", "log-level=error"], 10),
    ("AV1", "aom crf30", ["-c:v", "libaom-av1", "-cpu-used", "6", "-crf", "30", "-b:v", "0", "-g", "48", "-row-mt", "1",
                          "-pix_fmt", "yuv420p10le"], 10),
    ("AV1", "aom crf40", ["-c:v", "libaom-av1", "-cpu-used", "6", "-crf", "40", "-b:v", "0", "-g", "48", "-row-mt", "1",
                          "-pix_fmt", "yuv420p10le"], 10),
]
RASTERS = {"2160p": (3840, 2160), "1080p": (1920, 1080)}


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError(" ".join(cmd) + "\n" + r.stderr.decode("utf8", "replace")[-800:])


def prepare(scratch, name, path, ss, fps, src, raster, pix):
    out = os.path.join(scratch, "%s_%s_%s.yuv" % (name, raster, pix))
    if os.path.exists(out):
        return out
    sw, sh = src
    if raster == "2160p":
        vf = "crop=%d:%d:%d:%d" % (W, H, (sw - W) // 2 // 2 * 2, (sh - H) // 2 // 2 * 2)
    else:
        dw, dh = 1920, int(round(sh * 1920 / sw / 2)) * 2
        vf = "scale=%d:%d:flags=lanczos,crop=%d:%d:%d:%d" % (dw, dh, W, H, (dw - W) // 2 // 2 * 2, (dh - H) // 2 // 2 * 2)
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-ss", "%.4f" % ss, "-i", path, "-frames:v", str(N), "-vf", vf,
         "-pix_fmt", pix, "-f", "rawvideo", out])
    return out


def luma_seq(path, pix):
    fr = eio.read_raw_frames(path, W, H, pix, 0, N)
    return np.stack([eio.to_bitdepth(f, 10).Y for f in fr])


def _hp(a):
    return a - ndimage.gaussian_filter(a, 1.0, mode="nearest")


def metrics(ref, cod):
    pk = em.peak(10)
    tot = ring = smooth = 0.0
    zn = 0
    for r, c in zip(ref, cod):
        e = c - r
        zone, _ = em.near_edge_zone(r, 10)
        he = _hp(e)
        hr = _hp(r)
        loc = ndimage.uniform_filter(hr * hr, size=5, mode="nearest")
        lp = ndimage.gaussian_filter(r, 2.0, mode="nearest")
        g = np.hypot(*np.gradient(lp))
        sel = g <= np.quantile(g, 0.3)
        v = (r - ndimage.gaussian_filter(r, 1.0, mode="nearest"))[sel]
        sig = max(1.4826 * float(np.median(np.abs(v - np.median(v)))), 0.25)
        sm = zone & (loc <= 2.0 * sig * sig)
        tot += float((e * e).sum())
        ring += float((he[zone] ** 2).sum())
        smooth += float((he[sm] ** 2).sum())
        zn += int(zone.sum())
    mse = tot / ref.size
    share_s = smooth / tot if tot else 0.0
    return dict(psnr=10 * np.log10(pk * pk / mse) if mse else float("inf"),
                ring_rms=float(np.sqrt(ring / zn)) if zn else None,
                ring_share=ring / tot if tot else None, smooth_share=share_s,
                headroom_db=-10 * np.log10(max(1.0 - share_s, 1e-12)), zone_share=zn / ref.size)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for name, path, ss, fps, src, content in MASTERS:
        for raster, full in RASTERS.items():
            m10 = prepare(a.scratch, name, path, ss, fps, src, raster, "yuv420p10le")
            m8 = prepare(a.scratch, name, path, ss, fps, src, raster, "yuv420p")
            refs = {10: luma_seq(m10, "yuv420p10le"), 8: luma_seq(m8, "yuv420p")}
            for codec, label, args, bd in ENCODES:
                tag = label.replace(" ", "_")
                enc = os.path.join(a.scratch, "%s_%s_%s.mkv" % (name, raster, tag))
                dec = os.path.join(a.scratch, "%s_%s_%s.yuv" % (name, raster, tag))
                src_raw, src_pix = (m8, "yuv420p") if bd == 8 else (m10, "yuv420p10le")
                if not os.path.exists(dec):
                    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", src_pix, "-s", "%dx%d" % (W, H),
                         "-r", "%g" % fps, "-i", src_raw] + args + [enc])
                    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i", enc, "-pix_fmt", "yuv420p10le", "-f", "rawvideo", dec])
                kbps = os.path.getsize(enc) * 8 / (N / fps) / 1000.0 * (full[0] * full[1]) / (W * H)
                met = metrics(refs[bd], luma_seq(dec, "yuv420p10le"))
                row = dict(master=name, content=content, raster=raster, codec=codec, encode=label,
                           kbps_full=kbps, **met)
                rows.append(row)
                print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
    cols = ["master", "raster", "codec", "encode", "kbps_full", "psnr", "ring_rms", "ring_share", "smooth_share", "headroom_db",
            "zone_share", "content"]
    with open(os.path.join(a.out, "headroom.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([("%.4g" % r[c]) if isinstance(r[c], float) else r[c] for c in cols])
    # codec x rate summary (means over masters)
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Mosquito headroom by codec x rate (means over %d masters)" % len(MASTERS), "",
          "Near-edge mask = eval_metrics.near_edge_zone of the master; luma, 10-bit codes. See mosquito_headroom.py.", "",
          "| raster | encode | kbps (full raster) | PSNR Y | ring RMS | near-edge HF share of error | smooth near-edge share | headroom dB (mean / max) |",
          "|---|---|---|---|---|---|---|---|"]
    for raster in RASTERS:
        for codec, label, _, _ in ENCODES:
            rs = [r for r in rows if r["raster"] == raster and r["encode"] == label]
            mean = lambda k: float(np.mean([r[k] for r in rs if r[k] is not None]))
            md.append("| %s | %s (%s) | %.0f | %.2f | %.2f | %.1f %% | %.1f %% | %.2f / %.2f |" % (
                raster, label, codec, mean("kbps_full"), mean("psnr"), mean("ring_rms"), 100 * mean("ring_share"),
                100 * mean("smooth_share"), mean("headroom_db"), max(r["headroom_db"] for r in rs)))
    md += ["", "## Per master", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(("%.4g" % r[c]) if isinstance(r[c], float) else str(r[c]) for c in cols) + " |")
    md.append("")
    with open(os.path.join(a.out, "headroom.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))
    return 0


if __name__ == "__main__":
    sys.exit(main())
