# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Writes a Y4M clip (10-bit, C422p10 or C420p10) whose every frame carries a vp-stamp/1 source
stamp, plus sent.csv (index, aux, pts_num, pts_den).  No defaults for --fps and --size.

    python -I tools/framestamp/make_stamped_clip.py --out DIR --frames 120 --fps 24000/1001 \
        --size 3840x2160 --cadence 2:3 [--420] [--pattern levels --set levels10]

aux = frame index mod the cadence pattern length (sum of the --cadence parts; no --cadence = 0).
Stamp luma is limited range (64/940); chroma under the block is set neutral (512).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import framestamp as fs  # noqa: E402

DEPTH = 10
NEUTRAL = 1 << (DEPTH - 1)
GREY_LUMA = 512


def parse_size(text):
    try:
        w, h = text.lower().split("x")
        w, h = int(w), int(h)
    except ValueError:
        raise argparse.ArgumentTypeError("size must be WxH, got %r" % text)
    if w < 64 or h < 64 or w % 2 or h % 2:
        raise argparse.ArgumentTypeError("size must be even and at least 64x64")
    return w, h


def parse_fps(text):
    try:
        n, d = text.split("/")
        n, d = int(n), int(d)
    except ValueError:
        raise argparse.ArgumentTypeError("fps must be NUM/DEN, got %r" % text)
    if n <= 0 or d <= 0:
        raise argparse.ArgumentTypeError("fps must be positive")
    return n, d


def parse_cadence(text):
    try:
        parts = [int(p) for p in text.split(":")]
    except ValueError:
        raise argparse.ArgumentTypeError("cadence must look like 2:3")
    if not parts or any(p <= 0 for p in parts):
        raise argparse.ArgumentTypeError("cadence parts must be positive")
    if sum(parts) > 64:
        raise argparse.ArgumentTypeError("cadence length must be at most 64 (aux is 6 bits)")
    return parts


def base_planes(w, h, chroma420, pattern, set_name):
    """(Y, Cb, Cr) uint16 for the base picture."""
    if pattern == "levels":
        levels_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "levels")
        if os.path.isfile(os.path.join(levels_dir, "patterns.py")):
            sys.path.insert(0, levels_dir)
            import patterns  # noqa: E402
            y, cb, cr = patterns.render_frame(patterns.build_layout(set_name, w, h))
            if chroma420:
                cb, cr = cb[::2, :].copy(), cr[::2, :].copy()
            return y, cb, cr
        sys.stderr.write("tools/levels/patterns.py not found: using the flat grey pattern\n")
    cw = w // 2
    ch = h // 2 if chroma420 else h
    return (np.full((h, w), GREY_LUMA, dtype=np.uint16), np.full((ch, cw), NEUTRAL, dtype=np.uint16),
            np.full((ch, cw), NEUTRAL, dtype=np.uint16))


def neutral_chroma_under_block(cb, cr, w, h, corner, chroma420):
    x0, x1, y0, y1 = fs.block_rect(w, h, corner)
    cx0, cx1 = x0 // 2, (x1 + 1) // 2
    cy0, cy1 = (y0 // 2, (y1 + 1) // 2) if chroma420 else (y0, y1)
    cb[cy0:cy1, cx0:cx1] = NEUTRAL
    cr[cy0:cy1, cx0:cx1] = NEUTRAL


def main(argv=None):
    ap = argparse.ArgumentParser(description="Write a stamped Y4M test clip.")
    ap.add_argument("--out", required=True, help="output folder (created)")
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--fps", type=parse_fps, required=True, help="NUM/DEN, e.g. 24000/1001 (required)")
    ap.add_argument("--size", type=parse_size, required=True, help="WxH (required)")
    ap.add_argument("--cadence", type=parse_cadence, default=None, help="pulldown pattern such as 2:3; aux cycles over its length")
    ap.add_argument("--420", dest="chroma420", action="store_true", help="C420p10 instead of C422p10")
    ap.add_argument("--pattern", choices=("flat", "levels"), default="flat")
    ap.add_argument("--set", dest="set_name", default="levels10", help="tools/levels/patterns.py set for --pattern levels")
    ap.add_argument("--name", default="stamped", help="base name of the .y4m file")
    a = ap.parse_args(argv)
    if a.frames < 1:
        ap.error("--frames must be >= 1")
    w, h = a.size
    num, den = a.fps
    period = sum(a.cadence) if a.cadence else 1
    os.makedirs(a.out, exist_ok=True)
    y0, cb0, cr0 = base_planes(w, h, a.chroma420, a.pattern, a.set_name)
    cb0, cr0 = cb0.copy(), cr0.copy()
    neutral_chroma_under_block(cb0, cr0, w, h, fs.CORNER_SOURCE, a.chroma420)
    path = os.path.join(a.out, a.name + ".y4m")
    header = "YUV4MPEG2 W%d H%d F%d:%d Ip A1:1 %s\n" % (w, h, num, den, "C420p10" if a.chroma420 else "C422p10")
    with open(path, "wb") as f, open(os.path.join(a.out, "sent.csv"), "w", newline="\n") as sent:
        f.write(header.encode("ascii"))
        sent.write("index,aux,pts_num,pts_den\n")
        cbb, crb = cb0.astype("<u2").tobytes(), cr0.astype("<u2").tobytes()
        for index in range(a.frames):
            aux = index % period
            y = y0.copy()
            fs.write(y, DEPTH, False, fs.CORNER_SOURCE, fs.word(fs.KIND_SOURCE, aux, index))
            f.write(b"FRAME\n" + y.astype("<u2").tobytes() + cbb + crb)
            sent.write("%d,%d,%d,%d\n" % (index, aux, index * den, num))
    print(path)
    print(os.path.join(a.out, "sent.csv"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
