# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2: the use cases the owner expects for the ADAPTIVE ringing repair (adaptive_mosquito.AdaptiveMosquito),
each measured in the space the viewer sees (after the upscale / sharpening), CPU, 6 frames:
  (1) upscaling magnification: 1080p windows and 720p ABR rungs upscaled x2 (Lanczos3, eval_io.resample_plane);
      repair BEFORE the upscale vs none
  (2) VP sharpening stand-in after the upscale: unsharp gain k in {0, 0.5, 1.0} (out = up + k (up - gauss(up, 1)));
      VP's kernel-inverse de-blur / sharp branches / CNN doubler cannot run here, the unsharp is their
      linear stand-in.  Question: does a pre-repair let k rise at equal ringing?
  (3) chroma ringing: the repair on Cb / Cr (own edges, chroma resolution), Sol Levante (saturated animation)
      and Sintel, x264 CRF 35 (SDR) / x265-10 CRF 30 (HDR)
  (4) burned-in text / credits: SYNTHETIC credits card (white Arial text on a dark film-grain background,
      slow roll, 8-bit), x264 CRF 28 / 35 / 40 at 1080p
  (5) low ABR rungs: 720p (2/3 scale) x264 CRF 38 of the credits card and the tos4k window, shown x2
Reference = the master through the same chain.  Metrics: ring = near-edge smooth-band ringing energy
(ringing_stack.ring_energy) in output codes^2, ratio with / without repair, ring_rms (absolute, codes,
10-bit; ~4 codes = one 8-bit step), dpsnr, grain gate (two-seed probe through the repair, near edges).

  python -I tools/eval/ringing_usecases.py --hr-scratch <artefact_headroom scratch> --mq-scratch <mosquito scratch> \
         --scratch <new dir> --out tools/eval/prototypes/ringing_usecases
"""
import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import adaptive_mosquito as am  # noqa: E402
import artefact_headroom as ah  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402
import ringing_stack as rs  # noqa: E402

NFR = 6
W, H = ah.W, ah.H


def credits_card(path, n=NFR):
    """Synthetic 8-bit credits: white text rolling 2 px/frame over a dark, grainy background (yuv420p raw)."""
    if os.path.exists(path):
        return path
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(r"C:\Windows\Fonts\arial.ttf", 34)
    lines = ["DIRECTED BY  A. N. EXAMPLE", "Director of Photography", "Original Music", "Visual Effects Supervisor",
             "Production Design", "Edited by  J. Smith", "Sound Design and Mix", "Colourist  K. Lee",
             "Executive Producers", "Associate Producer", "Casting by  M. Rossi", "Costume Design"]
    rng = np.random.default_rng(42)
    with open(path, "wb") as f:
        for t in range(n):
            img = Image.new("L", (W, H), 0)
            d = ImageDraw.Draw(img)
            for i, s in enumerate(lines):
                d.text((W // 2, 60 + 62 * i - 2 * t), s, fill=235, font=font, anchor="mm")
            y = np.asarray(img, np.float64) * (219.0 / 255.0) + 16.0
            y += 26.0 * np.linspace(0, 1, W)[None, :] * 0.3  # faint background gradient
            y += ndimage.gaussian_filter(rng.standard_normal((H, W)), 0.7) * 2.5
            y = np.clip(np.rint(y), 0, 255).astype(np.uint8)
            c = np.full((H // 2, W // 2), 128, np.uint8)
            f.write(y.tobytes() + c.tobytes() + c.tobytes())
    return path


def scaled_master(scratch, name, m8, w, h):
    out = os.path.join(scratch, "%s_%dx%d_yuv420p.yuv" % (name, w, h))
    if not os.path.exists(out):
        ah.run([ah.FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s",
                "%dx%d" % (W, H), "-r", "24", "-i", m8, "-frames:v", str(NFR), "-vf", "scale=%d:%d:flags=lanczos" % (w, h),
                "-pix_fmt", "yuv420p", "-f", "rawvideo", out])
    return out


def encode(scratch, tag, src, w, h, args, pix="yuv420p"):
    dec = os.path.join(scratch, "%s.yuv" % tag)
    if not os.path.exists(dec):
        enc = dec[:-4] + ".mkv"
        ah.run([ah.FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", pix, "-s", "%dx%d" % (w, h),
                "-r", "24", "-i", src, "-frames:v", str(NFR)] + args + ["-pix_fmt", pix, enc])
        ah.run([ah.FF, "-hide_banner", "-loglevel", "error", "-y", "-i", enc, "-pix_fmt", "yuv420p10le", "-f", "rawvideo", dec])
    return dec


def planes(path, w, h, pix):
    fr = eio.read_raw_frames(path, w, h, pix, 0, NFR)
    fr = [eio.to_bitdepth(f, 10) for f in fr]
    return (np.stack([f.Y for f in fr]), np.stack([f.Cb for f in fr]), np.stack([f.Cr for f in fr]))


def up2(seq):
    return np.stack([eio.resample_plane(f, 2 * f.shape[0], 2 * f.shape[1], "lanczos3") for f in seq])


def sharpen(seq, k):
    return seq if k == 0 else np.stack([f + k * (f - ndimage.gaussian_filter(f, 1.0, mode="nearest")) for f in seq])


def repair(seq):
    rep = am.AdaptiveMosquito(True)
    return np.stack([rep.apply(f) for f in seq])


def gate(X, M):
    return am.grain_gate(lambda: am.AdaptiveMosquito(True), X, M)


def row(**kw):
    print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in kw.items()}), flush=True)
    return kw


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--mq-scratch", required=True)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    X264 = ah.X264
    card = credits_card(os.path.join(a.scratch, "credits_1024x768_yuv420p.yuv"))
    tos = os.path.join(a.mq_scratch, "tos4k_1080p_yuv420p.yuv")
    # (4) text / credits at 1080p, and (1)+(2) upscale + sharpening of the same decodes
    w7, h7 = 682, 512
    sets = [("credits", card, W, H, 28), ("credits", card, W, H, 35), ("credits", card, W, H, 40),
            ("tos4k_text", tos, W, H, 35)]
    for name, m8, w, h, crf in sets:
        M = planes(m8, w, h, "yuv420p")[0]
        D = planes(encode(a.scratch, "%s_%dx%d_crf%d" % (name, w, h, crf), m8, w, h, X264 + ["-crf", str(crf)]), w, h,
                   "yuv420p10le")[0]
        R = repair(D)
        ke, kf, _ = gate(D, M)
        r0, r1 = rs.ring_energy(M, D), rs.ring_energy(M, R)
        rows.append(row(usecase="(4) text 1080p" if name == "credits" else "(1) film/text 1080p", case="%s x264 crf%d" % (name, crf),
                        chain="native", k=0.0, ring_ratio=r1 / r0, ring_rms=float(np.sqrt(r0)),
                        dpsnr=em.psnr(M, R, 10) - em.psnr(M, D, 10), keep_edge=ke))
        Mu, Du, Ru = up2(M), up2(D), up2(R)
        for k in (0.0, 0.5, 1.0):
            Ms, Ds, Rs = sharpen(Mu, k), sharpen(Du, k), sharpen(Ru, k)
            r0, r1 = rs.ring_energy(Ms, Ds), rs.ring_energy(Ms, Rs)
            rows.append(row(usecase="(1)(2) x2 upscale + sharpen", case="%s x264 crf%d" % (name, crf), chain="up2", k=k,
                            ring_ratio=r1 / r0, ring_rms=float(np.sqrt(r0)),
                            dpsnr=em.psnr(Ms, Rs, 10) - em.psnr(Ms, Ds, 10), keep_edge=None))
    # (5) low ABR rung: 720p (2/3) CRF 38, shown x2
    for name, m8 in (("credits", card), ("tos4k_text", tos)):
        m7 = scaled_master(a.scratch, name, m8, w7, h7)
        M = planes(m7, w7, h7, "yuv420p")[0]
        D = planes(encode(a.scratch, "%s_%dx%d_crf38" % (name, w7, h7), m7, w7, h7, X264 + ["-crf", "38"]), w7, h7,
                   "yuv420p10le")[0]
        R = repair(D)
        ke, _, _ = gate(D, M)
        for k in (0.0, 1.0):
            Ms, Ds, Rs = sharpen(up2(M), k), sharpen(up2(D), k), sharpen(up2(R), k)
            r0, r1 = rs.ring_energy(Ms, Ds), rs.ring_energy(Ms, Rs)
            rows.append(row(usecase="(5) 720p rung x2", case="%s x264 crf38" % name, chain="up2", k=k, ring_ratio=r1 / r0,
                            ring_rms=float(np.sqrt(r0)), dpsnr=em.psnr(Ms, Rs, 10) - em.psnr(Ms, Ds, 10), keep_edge=ke))
    # (3) chroma ringing
    chroma = [("sintel_sky", os.path.join(a.hr_scratch, "sintel_sky_1080p_yuv420p.yuv"), "yuv420p", "sintel_sky_1080p_x265-8_crf30.yuv"),
              ("sol_sky", os.path.join(a.hr_scratch, "sol_sky_1080p_yuv420p10le.yuv"), "yuv420p10le", "sol_sky_1080p_x265-10_crf30.yuv"),
              ("sol_dark", os.path.join(a.hr_scratch, "sol_dark_1080p_yuv420p10le.yuv"), "yuv420p10le", "sol_dark_1080p_aom-10_crf40.yuv")]
    for name, mp, pix, dec in chroma:
        Mp = planes(mp, W, H, pix)
        Dp = planes(os.path.join(a.hr_scratch, dec), W, H, "yuv420p10le")
        for ci, cname in ((1, "Cb"), (2, "Cr")):
            M, D = Mp[ci], Dp[ci]
            R = repair(D)
            r0, r1 = rs.ring_energy(M, D), rs.ring_energy(M, R)
            ke, _, _ = gate(D, M)
            rows.append(row(usecase="(3) chroma", case="%s %s %s" % (name, dec.split("_1080p_")[1][:-4], cname), chain="native",
                            k=0.0, ring_ratio=r1 / r0 if r0 else None, ring_rms=float(np.sqrt(r0)) if r0 else None,
                            dpsnr=em.psnr(M, R, 10) - em.psnr(M, D, 10), keep_edge=ke))
    cols = ["usecase", "case", "chain", "k", "ring_ratio", "ring_rms", "dpsnr", "keep_edge"]
    fmt = lambda v: "-" if v is None else ("%.4g" % v if isinstance(v, float) else str(v))
    with open(os.path.join(a.out, "ringing_usecases.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in cols])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Adaptive ringing repair: owner use cases (see ringing_usecases.py)", "",
          "ring_ratio = ringing energy with / without the repair in the viewed space; ring_rms = without (10-bit codes, "
          "4 = one 8-bit step); keep_edge = near-edge grain gate of the repair (+-5 %).", "",
          "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in cols) + " |")
    md.append("")
    with open(os.path.join(a.out, "ringing_usecases.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))
    return 0


if __name__ == "__main__":
    sys.exit(main())
