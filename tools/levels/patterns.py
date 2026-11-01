# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Level / colour test patterns (10-bit Y'CbCr 4:2:2) with a layout description.

Geometry is fractional (proportional to frame height/width); the frame size is
always an argument. Patch x edges are rounded to even pixels (4:2:2 chroma
siting). Reserved codes 0-3 and 1020-1023 (10-bit) are never generated.

CLI: python -I tools/levels/patterns.py --set levels10 --size WxH --frames N --out DIR [--v210]
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import expected as ex  # noqa: E402

RESERVED = set(range(0, 4)) | set(range(1020, 1024))
BG = (512, 512, 512)
MIN_PATCH_H = 0.06
MIN_PATCH_PX = 4  # so the inner 50% is at least 2 px

LEVELS10 = (4, 16, 32, 48, 64, 128, 512, 876, 940, 980, 1019)
LEVELS8 = (1, 4, 16, 64, 128, 235, 254)
NEAR_BLACK = tuple(range(64, 81, 2))                     # 9 patches
NEAR_WHITE = tuple(range(924, 1020, 8)) + (1019,)        # 13 patches (step 8 + the top code)
RAMP_STEPS = 64
BAR_NAMES = ("white", "yellow", "cyan", "green", "magenta", "red", "blue", "black")
BAR_RGB = ((1, 1, 1), (1, 1, 0), (0, 1, 1), (0, 1, 0), (1, 0, 1), (1, 0, 0), (0, 0, 1), (0, 0, 0))
PRIM_NAMES = ("R", "G", "B", "C", "M", "Y")
PRIM_RGB = ((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 1, 1), (1, 0, 1), (1, 1, 0))


def parse_size(text):
    try:
        w, h = text.lower().split("x")
        w, h = int(w), int(h)
    except ValueError:
        raise ValueError("size must be WxH, got %r" % text)
    if w < 16 or h < 16 or w % 2:
        raise ValueError("size must be at least 16x16 with even width")
    return w, h


def _even(v):
    return 2 * int(round(v / 2.0))


def rect_px(frac, w, h):
    """Fractional rect (x0, y0, x1, y1) -> pixel rect, x edges even."""
    x0, y0, x1, y1 = frac
    return [_even(x0 * w), int(round(y0 * h)), _even(x1 * w), int(round(y1 * h))]


def _grey(code):
    return (code, 512, 512)


def _rgb_codes(rgb, matrix, scale):
    ycc = ex.rgb_to_ycc(np.array(rgb, dtype=np.float64) * scale, matrix)
    return tuple(int(v) for v in ex.ycc_to_codes(ycc, 10, "limited"))


def _grid(items, w, h):
    """Place items (id, ycbcr, extra) in a grid of ~equal cells; 10% margin per cell."""
    n = len(items)
    rows = max(1, int(round(math.sqrt(n * h / float(w)))))
    cols = int(math.ceil(n / float(rows)))
    out = []
    for i, (pid, ycc, extra) in enumerate(items):
        r, c = divmod(i, cols)
        cw, ch = 1.0 / cols, 1.0 / rows
        frac = (c * cw + 0.1 * cw, r * ch + 0.1 * ch, (c + 1) * cw - 0.1 * cw, (r + 1) * ch - 0.1 * ch)
        out.append((pid, ycc, frac, extra))
    return out


def _bars(prefix, scale, w, h, matrix):
    out = []
    for row, (rname, amp) in enumerate((("100", 1.0), ("75", 0.75))):
        y0, y1 = (0.05, 0.45) if row == 0 else (0.55, 0.95)
        for i, (name, rgb) in enumerate(zip(BAR_NAMES, BAR_RGB)):
            frac = (i / 8.0, y0, (i + 1) / 8.0, y1)
            out.append(("%s_%s_%s" % (prefix, rname, name), _rgb_codes(rgb, matrix, amp), frac,
                        {"rgb_nonlinear": [amp * c for c in rgb]}))
    return out


def _spec(set_name, w, h):
    """Returns (items, matrix, transfer). items: (id, ycbcr, frac, extra)."""
    if set_name == "levels10":
        items = _grid([("y10_%04d" % c, _grey(c), {}) for c in LEVELS10], w, h)
        return items, "bt709", "bt1886"
    if set_name == "levels8":
        items = _grid([("y8_%03d" % c, _grey(4 * c), {"code8": c}) for c in LEVELS8], w, h)
        return items, "bt709", "bt1886"
    if set_name == "near_black":
        return _grid([("nb_%04d" % c, _grey(c), {}) for c in NEAR_BLACK], w, h), "bt709", "bt1886"
    if set_name == "near_white":
        return _grid([("nw_%04d" % c, _grey(c), {}) for c in NEAR_WHITE], w, h), "bt709", "bt1886"
    if set_name == "ramp10":
        items = []
        for i in range(RAMP_STEPS):
            code = int(round(4 + i * 1015.0 / (RAMP_STEPS - 1)))
            items.append(("ramp_%02d" % i, _grey(code), (i / float(RAMP_STEPS), 0.35,
                                                         (i + 1) / float(RAMP_STEPS), 0.65),
                          {"group": "ramp", "index": i}))
        return items, "bt709", "bt1886"
    if set_name == "bars709":
        return _bars("b709", 0, w, h, "bt709"), "bt709", "bt1886"
    if set_name == "bars2020":
        return _bars("b2020", 0, w, h, "bt2020nc"), "bt2020nc", "pq"
    if set_name == "primaries2020":
        items = []
        for row, (suffix, amp) in enumerate((("100", 1.0), ("50", 0.5))):
            for i, (name, rgb) in enumerate(zip(PRIM_NAMES, PRIM_RGB)):
                frac = (i / 6.0 + 0.01, 0.05 + row * 0.5, (i + 1) / 6.0 - 0.01, 0.45 + row * 0.5)
                items.append(("p2020_%s_%s" % (name, suffix), _rgb_codes(rgb, "bt2020nc", amp), frac,
                              {"rgb_nonlinear": [amp * c for c in rgb]}))
        return items, "bt2020nc", "pq"
    raise ValueError("unknown set %r" % set_name)


SETS = ("levels10", "levels8", "ramp10", "bars709", "bars2020", "primaries2020", "near_black", "near_white")
SETS = frozenset(SETS)


def build_layout(set_name, w, h):
    if w % 2 or w < 16 or h < 16:
        raise ValueError("width must be even, size at least 16x16")
    items, matrix, transfer = _spec(set_name, w, h)
    patches = []
    for pid, ycc, frac, extra in items:
        for v in ycc:
            if v in RESERVED:
                raise ValueError("reserved code generated in %s" % pid)
        px = rect_px(frac, w, h)
        if px[2] - px[0] < MIN_PATCH_PX or px[3] - px[1] < MIN_PATCH_PX:
            raise ValueError("frame %dx%d too small for set %s (patch %s)" % (w, h, set_name, pid))
        p = {"id": pid, "rect_frac": [round(v, 6) for v in frac], "rect_px": px,
             "ycbcr": list(ycc), "matrix": matrix, "range": "limited", "transfer": transfer}
        p.update(extra)
        patches.append(p)
    return {"set": set_name, "width": w, "height": h, "depth": 10, "chroma": "422",
            "bg_ycbcr": list(BG), "matrix": matrix, "range": "limited", "transfer": transfer,
            "patches": patches}


def render_frame(layout):
    """Returns (Y (H,W), Cb (H,W/2), Cr (H,W/2)) uint16."""
    w, h = layout["width"], layout["height"]
    planes = [np.full((h, w), v, dtype=np.uint16) for v in layout["bg_ycbcr"]]
    for p in layout["patches"]:
        x0, y0, x1, y1 = p["rect_px"]
        for pl, v in zip(planes, p["ycbcr"]):
            pl[y0:y1, x0:x1] = v
    return planes[0], planes[1][:, ::2].copy(), planes[2][:, ::2].copy()


# ---- v210 -------------------------------------------------------------------

def v210_row_bytes(w):
    return ((w + 47) // 48) * 128


def v210_pack(y, cb, cr):
    """Planes (H,W),(H,W/2),(H,W/2) of 10-bit values -> v210 bytes (rows 128-byte aligned)."""
    h, w = y.shape
    wp = ((w + 5) // 6) * 6
    yy = np.pad(y.astype(np.uint32), ((0, 0), (0, wp - w)), mode="edge")
    cbp = np.pad(cb.astype(np.uint32), ((0, 0), (0, wp // 2 - cb.shape[1])), mode="edge")
    crp = np.pad(cr.astype(np.uint32), ((0, 0), (0, wp // 2 - cr.shape[1])), mode="edge")
    g = wp // 6
    yy = yy.reshape(h, g, 6)
    cbp = cbp.reshape(h, g, 3)
    crp = crp.reshape(h, g, 3)
    words = np.stack([
        cbp[..., 0] | (yy[..., 0] << 10) | (crp[..., 0] << 20),
        yy[..., 1] | (cbp[..., 1] << 10) | (yy[..., 2] << 20),
        crp[..., 1] | (yy[..., 3] << 10) | (cbp[..., 2] << 20),
        yy[..., 4] | (crp[..., 2] << 10) | (yy[..., 5] << 20),
    ], axis=-1).reshape(h, g * 4).astype("<u4")
    rb = v210_row_bytes(w)
    out = np.zeros((h, rb), dtype=np.uint8)
    raw = words.view(np.uint8).reshape(h, g * 16)
    out[:, :g * 16] = raw
    return out.tobytes()


def v210_unpack(data, w, h, row_bytes=None):
    """v210 bytes -> (Y (H,W), Cb (H,W/2), Cr (H,W/2)) uint16."""
    rb = row_bytes or v210_row_bytes(w)
    g = (w + 5) // 6
    if len(data) < rb * h or rb < g * 16:
        raise ValueError("v210 buffer too small: %d bytes for %dx%d row_bytes %d" % (len(data), w, h, rb))
    a = np.frombuffer(data, dtype=np.uint8, count=rb * h).reshape(h, rb)[:, :g * 16]
    wd = np.ascontiguousarray(a).view("<u4").reshape(h, g, 4).astype(np.uint32)
    m = 0x3FF
    y = np.empty((h, g, 6), dtype=np.uint16)
    cb = np.empty((h, g, 3), dtype=np.uint16)
    cr = np.empty((h, g, 3), dtype=np.uint16)
    w0, w1, w2, w3 = wd[..., 0], wd[..., 1], wd[..., 2], wd[..., 3]
    cb[..., 0], y[..., 0], cr[..., 0] = w0 & m, (w0 >> 10) & m, (w0 >> 20) & m
    y[..., 1], cb[..., 1], y[..., 2] = w1 & m, (w1 >> 10) & m, (w1 >> 20) & m
    cr[..., 1], y[..., 3], cb[..., 2] = w2 & m, (w2 >> 10) & m, (w2 >> 20) & m
    y[..., 4], cr[..., 2], y[..., 5] = w3 & m, (w3 >> 10) & m, (w3 >> 20) & m
    return (y.reshape(h, g * 6)[:, :w].copy(), cb.reshape(h, g * 3)[:, :w // 2].copy(),
            cr.reshape(h, g * 3)[:, :w // 2].copy())


# ---- Y4M --------------------------------------------------------------------

def y4m_header(w, h, fps="0:0"):
    return ("YUV4MPEG2 W%d H%d F%s Ip A1:1 C422p10\n" % (w, h, fps)).encode("ascii")


def y4m_frame_bytes(y, cb, cr):
    return b"FRAME\n" + y.astype("<u2").tobytes() + cb.astype("<u2").tobytes() + cr.astype("<u2").tobytes()


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate level/colour test patterns.")
    ap.add_argument("--set", required=True, choices=sorted(SETS))
    ap.add_argument("--size", required=True, help="WxH (required)")
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--v210", action="store_true", help="write raw packed v210 instead of Y4M")
    ap.add_argument("--fps", default="0:0", help="Y4M frame rate NUM:DEN (default 0:0 = unspecified)")
    a = ap.parse_args(argv)
    if a.frames < 1:
        ap.error("--frames must be >= 1")
    w, h = parse_size(a.size)
    layout = build_layout(a.set, w, h)
    y, cb, cr = render_frame(layout)
    os.makedirs(a.out, exist_ok=True)
    if a.v210:
        layout["v210_row_bytes"] = v210_row_bytes(w)
        frame = v210_pack(y, cb, cr)
        path = os.path.join(a.out, a.set + ".v210")
        with open(path, "wb") as f:
            for _ in range(a.frames):
                f.write(frame)
    else:
        frame = y4m_frame_bytes(y, cb, cr)
        path = os.path.join(a.out, a.set + ".y4m")
        with open(path, "wb") as f:
            f.write(y4m_header(w, h, a.fps))
            for _ in range(a.frames):
                f.write(frame)
    lpath = os.path.join(a.out, a.set + ".layout.json")
    with open(lpath, "w", encoding="utf-8") as f:
        json.dump(layout, f, indent=1)
    print(path)
    print(lpath)
    return 0


if __name__ == "__main__":
    sys.exit(main())
