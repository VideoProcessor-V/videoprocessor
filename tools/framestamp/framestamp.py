# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""vp-stamp/1: Python mirror of src/VP-V/modules/control/diagnostics/FrameStamp.h.

FrameStamp.h is the single source of truth; every constant and formula here is
identical to it (the golden vectors in test_framestamp.py and FrameStampTests.cpp
pin both).  Luma only.  See tools/framestamp/README.md.
"""
import math

MAGIC = 0xA5
KIND_SOURCE = 0
KIND_OUTPUT = 1
CORNER_SOURCE = 0
CORNER_OUTPUT = 1
UNITS_ACROSS = 270.0      # unit u = H / 270
BLOCK_W = 18.0            # in units, including the 1u quiet border
BLOCK_H = 7.0
COLS = 16
ROWS = 5                  # row 0 = reference, rows 1..4 = data
ANCHOR = 2.0              # block anchored 2u from the top and from the side
INNER_LO = 0.25           # decoder samples the inner 50% of each cell
INNER_HI = 0.75
MIN_CONTRAST = 0.25       # of the code range (1 << depth)
SEARCH_STEP = 0.25        # units
DEFAULT_RADIUS = 1.0      # units


def crc16(data):
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def word(kind, aux, index):
    """64-bit payload: magic 8 | kind 2 | aux 6 | index 32 | crc16 over the first 48 bits."""
    head = (MAGIC << 40) | ((kind & 3) << 38) | ((aux & 63) << 32) | (index & 0xFFFFFFFF)
    crc = crc16(head.to_bytes(6, "big"))
    return (head << 16) | crc


def word_hi(w):
    return (w >> 32) & 0xFFFFFFFF


def word_lo(w):
    return w & 0xFFFFFFFF


def levels(bit_depth, full_range):
    """(black, white) code values at the frame's depth and range."""
    if full_range:
        return 0, (1 << bit_depth) - 1
    shift = bit_depth - 8
    return 16 << shift, 235 << shift


def block_origin(w, h, corner):
    """Top-left of the 18x7 block in (fractional) pixels."""
    u = h / UNITS_ACROSS
    if corner == CORNER_OUTPUT:
        return w - ANCHOR * u - BLOCK_W * u, ANCHOR * u
    return ANCHOR * u, ANCHOR * u


def _pix_start(a):
    """First integer pixel whose centre (i + 0.5) is >= a (exact: corrected after the ceil)."""
    i = math.ceil(a - 0.5)
    while i + 0.5 < a:
        i += 1
    while (i - 1) + 0.5 >= a:
        i -= 1
    return i


def _pix_range(a, b):
    """Integer pixels [i0, i1) whose centre (i + 0.5) lies in [a, b)."""
    return _pix_start(a), _pix_start(b)


def block_rect(w, h, corner):
    """Integer pixel bounds (x0, x1, y0, y1) of the painted block, clipped to the frame."""
    u = h / UNITS_ACROSS
    bx, by = block_origin(w, h, corner)
    x0, x1 = _pix_range(bx, bx + BLOCK_W * u)
    y0, y1 = _pix_range(by, by + BLOCK_H * u)
    return max(x0, 0), min(x1, w), max(y0, 0), min(y1, h)


def cell_bit(word_value, col, row):
    if row == 0:
        return 1 if col % 2 == 0 else 0
    idx = (row - 1) * COLS + col
    return (word_value >> (63 - idx)) & 1


def write(plane, bit_depth, full_range, corner, word_value):
    """Paint the stamp into a 2-D luma array (any integer dtype).  Border pixels are dark too."""
    h, w = plane.shape
    black, white = levels(bit_depth, full_range)
    u = h / UNITS_ACROSS
    bx, by = block_origin(w, h, corner)
    x0, x1, y0, y1 = block_rect(w, h, corner)
    if x1 > x0 and y1 > y0:
        plane[y0:y1, x0:x1] = black
    for row in range(ROWS):
        for col in range(COLS):
            if not cell_bit(word_value, col, row):
                continue
            cx0, cx1 = _pix_range(bx + (1 + col) * u, bx + (2 + col) * u)
            cy0, cy1 = _pix_range(by + (1 + row) * u, by + (2 + row) * u)
            cx0, cx1, cy0, cy1 = max(cx0, 0), min(cx1, w), max(cy0, 0), min(cy1, h)
            if cx1 > cx0 and cy1 > cy0:
                plane[cy0:cy1, cx0:cx1] = white


def cpu_reference(x, y, w, h, corner, word_value):
    """The shader's per-pixel formula: 0.0 / 1.0, or -1.0 when (x, y) is not in the stamp block."""
    u = h / UNITS_ACROSS
    bx, by = block_origin(w, h, corner)
    cxm = x + 0.5
    cym = y + 0.5
    if cxm < bx or cym < by or cxm >= bx + BLOCK_W * u or cym >= by + BLOCK_H * u:
        return -1.0
    kx = int(math.floor((cxm - bx) / u))
    if cxm < bx + kx * u:
        kx -= 1
    elif cxm >= bx + (kx + 1) * u:
        kx += 1
    ky = int(math.floor((cym - by) / u))
    if cym < by + ky * u:
        ky -= 1
    elif cym >= by + (ky + 1) * u:
        ky += 1
    col = kx - 1
    row = ky - 1
    if col < 0 or col >= COLS or row < 0 or row >= ROWS:
        return 0.0
    return float(cell_bit(word_value, col, row))


def _cell_mean(plane, a0, a1, b0, b1):
    """Mean of the samples whose centres lie in [a0,a1) x [b0,b1); None if outside the frame."""
    h, w = plane.shape
    ix0, ix1 = _pix_range(a0, a1)
    iy0, iy1 = _pix_range(b0, b1)
    if ix1 <= ix0:
        ix0 = int(math.floor((a0 + a1) * 0.5))
        ix1 = ix0 + 1
    if iy1 <= iy0:
        iy0 = int(math.floor((b0 + b1) * 0.5))
        iy1 = iy0 + 1
    if ix0 < 0 or iy0 < 0 or ix1 > w or iy1 > h:
        return None
    total = int(plane[iy0:iy1, ix0:ix1].sum(dtype="uint64"))
    return total / ((ix1 - ix0) * (iy1 - iy0))


def _try_decode(plane, bit_depth, corner, ox, oy):
    h, w = plane.shape
    u = h / UNITS_ACROSS
    bx, by = block_origin(w, h, corner)
    bx += ox
    by += oy
    means = []
    for row in range(ROWS):
        for col in range(COLS):
            cx = bx + (1 + col) * u
            cy = by + (1 + row) * u
            m = _cell_mean(plane, cx + INNER_LO * u, cx + INNER_HI * u,
                           cy + INNER_LO * u, cy + INNER_HI * u)
            if m is None:
                return None
            means.append(m)
    sum1 = 0.0
    sum0 = 0.0
    for col in range(COLS):
        if col % 2 == 0:
            sum1 += means[col]
        else:
            sum0 += means[col]
    mean1 = sum1 / 8.0
    mean0 = sum0 / 8.0
    contrast = mean1 - mean0
    if contrast < MIN_CONTRAST * (1 << bit_depth):
        return None
    threshold = (mean1 + mean0) * 0.5
    for col in range(COLS):
        if col % 2 == 0:
            if means[col] < threshold:
                return None
        elif means[col] >= threshold:
            return None
    value = 0
    for i in range(64):
        value = (value << 1) | (1 if means[COLS + i] >= threshold else 0)
    if crc16((value >> 16).to_bytes(6, "big")) != (value & 0xFFFF):
        return None
    if (value >> 56) != MAGIC:
        return None
    return {
        "found": True,
        "kind": (value >> 54) & 3,
        "aux": (value >> 48) & 63,
        "index": (value >> 16) & 0xFFFFFFFF,
        "contrast": contrast,
        "word": value,
    }


def search_offsets(radius):
    n = int(round(radius / SEARCH_STEP))
    cand = [(ix, iy) for ix in range(-n, n + 1) for iy in range(-n, n + 1)]
    cand.sort(key=lambda c: (c[0] * c[0] + c[1] * c[1], c[1], c[0]))
    return cand


def decode(plane, bit_depth, corner, radius=DEFAULT_RADIUS):
    """Decode the stamp in a 2-D luma array (any bit depth).  Returns a dict with found=False on failure.

    offset_x / offset_y are the search shift in units (0, 0 when the nominal geometry matched).
    """
    h = plane.shape[0]
    u = h / UNITS_ACROSS
    for ix, iy in search_offsets(radius):
        r = _try_decode(plane, bit_depth, corner, ix * SEARCH_STEP * u, iy * SEARCH_STEP * u)
        if r is not None:
            r["offset_x"] = ix * SEARCH_STEP
            r["offset_y"] = iy * SEARCH_STEP
            return r
    return {"found": False, "kind": 0, "aux": 0, "index": 0, "contrast": 0.0,
            "offset_x": 0.0, "offset_y": 0.0, "word": 0}


def fnv1a64(data):
    """FNV-1a 64 over a bytes-like object."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h
