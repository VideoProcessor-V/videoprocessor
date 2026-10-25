#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic Dolby Vision tunnel corpus: content, DM metadata, tunnel packing, capture
containers, truth and an independent reference decode (2026-10-24, test/dv-synthetic).

A THIRD implementation of the HDMI DV tunnel, written in Python/numpy from the
specifications, sharing no code with VP's decoder (CR210DolbyVisionTunnelFormatter),
VP's VPV packer (dv_loopback/DvTunnelPacker.h) or the C++ reference encoder
(VideoProcessor-Test/DvTunnelReferenceEncoder.h):

  - ETSI GS CCM 001 V1.1.1 section 6.2 Table 3/4/5: dm_metadata() bytes and the
    extension blocks (L1/L2/L5 per ETSI; L6/L8/L11 in VP's assumed 16-bit containers,
    the only layout the tunnel is known to use - unverified until a real capture);
  - 6.3 Tables 8-13: 128-byte packets, header, ISO/IEC 13818-1 CRC-32;
  - 6.4.1-6.4.4: packet bit i -> chroma LSB of raster pixel i (Cb on even, Cr on odd
    pixels), scrambled with the parity of chroma bits 11..1 and of the luma sample
    that follows; three copies of 1024 pixels; packet P at pixel P * 3072;
  - HDMI 1.4a 6.5.4 12-bit YCbCr 4:2:2 in an 8-bit RGB 4:4:4 link: R = C[11:4],
    G = Y[11:4], B = C[3:0] Y[3:0];
  - DeckLink 10-bit RGB containers: r210 [00 R10 G10 B10] big endian, R10b
    [R10 G10 B10 xx] big endian, R10l the same little endian; the 8-bit channel at
    the bottom of the 10-bit field ("low8"), at the top ("high8", byte << 2), or at
    the top with the two top bits replicated into the bottom ("high8rep", how an
    8 -> 10 bit scaler fills it); rows padded to 64 pixels (VideoState::BytesPerRow).

The colour model (the reference render) follows the Dolby Vision decode that
libplacebo documents: normalised 12-bit codes v = code / 4095 (full range), nonlinear
LMS' (PQ) = N (v - offset), PQ EOTF, then linear RGB (BT.2020) = LMS->RGB x L x LMS'.
N = YCCtoRGB / 8192, offset = YCCtoRGB_offset / 2^28 (VP's convention, see
docs/VP-dv-tunnel-autotest.md), L = RGBtoLMS / 16384. The HPE LMS <-> BT.2020 matrix
is DERIVED here (Hunt-Pointer-Estevez, D65 normalised, x BT.2020 RGB->XYZ, rows
normalised so D65 white maps to L = M = S), not copied from libplacebo; the
difference from libplacebo's hard-coded constant (< 2e-4) is reported by
`python dvsynth.py --self-check`.

Everything is deterministic (no clock, seeded noise only).
"""
import json
import os
import struct
import sys

import numpy as np

# ---------------------------------------------------------------- PQ (SMPTE ST 2084)
PQ_M1 = 2610.0 / 16384.0
PQ_M2 = 2523.0 / 4096.0 * 128.0
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 4096.0 * 32.0
PQ_C3 = 2392.0 / 4096.0 * 32.0


def pq_to_nits(e):
    e = np.clip(np.asarray(e, dtype=np.float64), 0.0, None)
    p = e ** (1.0 / PQ_M2)
    return 10000.0 * (np.maximum(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p)) ** (1.0 / PQ_M1)


def nits_to_pq(nits):
    y = np.clip(np.asarray(nits, dtype=np.float64), 0.0, None) / 10000.0
    p = y ** PQ_M1
    return ((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p)) ** PQ_M2


def pq_code_to_nits(code):
    return float(pq_to_nits(code / 4095.0))


def nits_to_pq_code(nits):
    return int(round(float(nits_to_pq(nits)) * 4095.0))


# ---------------------------------------------------------------- colour matrices
def rgb_to_xyz_matrix(prim):
    (xr, yr), (xg, yg), (xb, yb), (xw, yw) = prim
    p = np.array([[xr / yr, xg / yg, xb / yb], [1.0, 1.0, 1.0],
                  [(1 - xr - yr) / yr, (1 - xg - yg) / yg, (1 - xb - yb) / yb]])
    w = np.array([xw / yw, 1.0, (1 - xw - yw) / yw])
    return p * np.linalg.solve(p, w)


PRIM_2020 = [(0.708, 0.292), (0.170, 0.797), (0.131, 0.046), (0.3127, 0.3290)]
PRIM_709 = [(0.640, 0.330), (0.300, 0.600), (0.150, 0.060), (0.3127, 0.3290)]
PRIM_P3D65 = [(0.680, 0.320), (0.265, 0.690), (0.150, 0.060), (0.3127, 0.3290)]
M_2020_TO_XYZ = rgb_to_xyz_matrix(PRIM_2020)
HPE_D65 = np.array([[0.4002, 0.7076, -0.0808], [-0.2263, 1.1653, 0.0457], [0.0, 0.0, 0.9182]])
_rgb2lms = HPE_D65 @ M_2020_TO_XYZ
RGB2020_TO_LMS = _rgb2lms / _rgb2lms.sum(axis=1, keepdims=True)
LMS_TO_RGB2020 = np.linalg.inv(RGB2020_TO_LMS)
LIBPLACEBO_LMS2RGB = np.array([[3.06441879, -2.16597676, 0.10155818],
                               [-0.65612108, 1.78554118, -0.12943749],
                               [0.01736321, -0.04725154, 1.03004253]])


def rgb_convert_matrix(src_prim, dst_prim):
    return np.linalg.inv(rgb_to_xyz_matrix(dst_prim)) @ rgb_to_xyz_matrix(src_prim)


# ---------------------------------------------------------------- representation
class Rep:
    """A DV signal representation: the DM's YCCtoRGB, offsets and RGBtoLMS (integers as sent)."""

    def __init__(self, name, ycc, off, lms, color_space):
        self.name = name
        self.ycc = list(ycc)
        self.off = list(off)
        self.lms = list(lms)
        self.color_space = color_space
        self.N = np.array(ycc, dtype=np.float64).reshape(3, 3) / 8192.0
        self.offset = np.array(off, dtype=np.float64) / float(1 << 28)
        self.L = np.array(lms, dtype=np.float64).reshape(3, 3) / 16384.0
        self.Ninv = np.linalg.inv(self.N)
        self.Linv = np.linalg.inv(self.L)

    def encode(self, rgb_nits):
        """Linear BT.2020 RGB (nits, ..., 3) -> unrounded 12-bit codes (..., 3) [I, P, T]."""
        lin = np.asarray(rgb_nits, dtype=np.float64) / 10000.0
        lms = lin @ RGB2020_TO_LMS.T
        lmsx = lms @ self.Linv.T
        pq = nits_to_pq(np.clip(lmsx, 0.0, None) * 10000.0)
        v = pq @ self.Ninv.T + self.offset
        return v * 4095.0

    def decode(self, codes):
        """12-bit codes (..., 3) -> linear BT.2020 RGB in nits (may be negative: out of gamut)."""
        v = np.asarray(codes, dtype=np.float64) / 4095.0
        pq = (v - self.offset) @ self.N.T
        lmsx = pq_to_nits(np.clip(pq, 0.0, None)) / 10000.0
        rgb = (lmsx @ self.L.T) @ LMS_TO_RGB2020.T
        return rgb * 10000.0


# Profile 5 (IPTPQc2): the matrices real profile-5 RPUs carry (dovi_tool fixture profile5.bin).
REP_P5 = Rep("p5-ipt", [8192, 799, 1681, 8192, -933, 1091, 8192, 267, -5545],
             [0, 1 << 27, 1 << 27], [17081, -349, -349, -349, 17081, -349, -349, -349, 17081], 2)
# Profile 8.1 (BT.2020 YCbCr PQ): the matrices of dovi_tool's cmv40_full_rpu.bin.
REP_P8 = Rep("p8-ycc", [9574, 0, 13802, 9574, -1540, -5348, 9574, 17610, 0],
             [16777216, 1 << 27, 1 << 27], [7222, 8771, 390, 2654, 12430, 1300, 0, 422, 15962], 0)
REPS = {"p5": REP_P5, "p8": REP_P8}


def neutral_codes(rep, nits):
    """Codes of a neutral grey (D65) at `nits`, unrounded."""
    return rep.encode(np.array([nits, nits, nits], dtype=np.float64))


# ---------------------------------------------------------------- DM (ETSI Table 3/4/5)
def be16(v):
    return struct.pack(">H", int(v) & 0xFFFF)


def be32(v):
    return struct.pack(">I", int(v) & 0xFFFFFFFF)


def dm_bytes(dm):
    """dm: dict with rep (Rep), scene_refresh, source_min_pq, source_max_pq, bit_depth,
    color_space (override), full_range, ext: list of (level, payload bytes), and optional
    raw overrides. Returns the dm_metadata() body."""
    rep = dm["rep"]
    out = bytearray()
    out += bytes([0, dm.get("scene_refresh", 1) & 0xFF])
    for c in dm.get("ycc", rep.ycc):
        out += be16(c)
    for o in dm.get("off", rep.off):
        out += be32(o)
    for c in dm.get("lms", rep.lms):
        out += be16(c)
    out += bytes([0xFF, 0xFF]) + bytes(8)
    out += bytes([dm.get("bit_depth", 12) & 0xFF, dm.get("color_space", rep.color_space) & 0xFF,
                  dm.get("chroma_format", 1) & 0xFF, dm.get("full_range", 1) & 0xFF])
    out += be16(dm.get("source_min_pq", 62)) + be16(dm.get("source_max_pq", 3696))
    out += bytes([0x00, 0x2A])
    ext = dm.get("ext", [])
    out += bytes([dm.get("num_ext_override", len(ext)) & 0xFF])
    for level, payload in ext:
        out += be32(dm.get("ext_length_override", {}).get(level, len(payload)))
        out += bytes([level & 0xFF]) + bytes(payload)
    return bytes(out)


def ext_l1(mn, mx, avg):
    return (1, be16(mn) + be16(mx) + be16(avg))


def ext_l2(target, slope=2048, offset=2048, power=2048, chroma=2048, sat=2048, ms=2048):
    return (2, b"".join(be16(v) for v in (target, slope, offset, power, chroma, sat, ms)))


def ext_l5(left, right, top, bottom):
    return (5, b"".join(be16(v) for v in (left, right, top, bottom)))


def ext_l6(max_m, min_m, cll, fall):
    return (6, b"".join(be16(v) for v in (max_m, min_m, cll, fall)))


def ext_l8(index, slope=2048, offset=2048, power=2048, chroma=2048, sat=2048, ms=2048, mid_clip=None,
           sat_vector=None, hue_vector=None):
    """L8 in VP's assumed 16-bit containers: index (u8), six 16-bit trims, then the optional
    [mid_contrast, clip_trim] (16 bit each), [6 saturation bytes], [6 hue bytes] (feat/dv-l8:
    the vectors; each needs the fields before it, 128 = neutral)."""
    p = bytes([index & 0xFF]) + b"".join(be16(v) for v in (slope, offset, power, chroma, sat, ms))
    if mid_clip is not None or sat_vector is not None or hue_vector is not None:
        mc = mid_clip if mid_clip is not None else (2048, 2048)
        p += be16(mc[0]) + be16(mc[1])
    if sat_vector is not None or hue_vector is not None:
        p += bytes((v & 0xFF) for v in (sat_vector if sat_vector is not None else [128] * 6))
    if hue_vector is not None:
        p += bytes((v & 0xFF) for v in hue_vector)
    return (8, p)


def ext_l9(source_primary_index):
    """L9: source_primary_index (u8)."""
    return (9, bytes([source_primary_index & 0xFF]))


def ext_l10(index, max_pq, min_pq=62, primary_index=2, packed=False):
    """L10 custom target display (feat/dv-l8): index (u8), target_max_pq, target_min_pq,
    target_primary_index (u8); 16-bit containers (6 bytes) or the RPU's packed 5 bytes."""
    if packed:
        bits = (index & 0xFF) << 32 | (max_pq & 0xFFF) << 20 | (min_pq & 0xFFF) << 8 | (primary_index & 0xFF)
        return (10, bits.to_bytes(5, "big"))
    return (10, bytes([index & 0xFF]) + be16(max_pq) + be16(min_pq) + bytes([primary_index & 0xFF]))


def ext_l11(content_type, whitepoint=0, reference=False, b2=0, b3=0):
    return (11, bytes([content_type & 0xFF, (whitepoint & 15) | (16 if reference else 0), b2 & 0xFF, b3 & 0xFF]))


# ---------------------------------------------------------------- packets (ETSI Tables 8-13)
def _crc_table():
    t = []
    for n in range(256):
        c = n << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) if c & 0x80000000 else (c << 1)
            c &= 0xFFFFFFFF
        t.append(c)
    return t


_CRC = _crc_table()


def crc32_mpeg2(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC[((crc >> 24) ^ b) & 0xFF]
    return crc


assert crc32_mpeg2(b"123456789") == 0x0376E6E7


def packets(body, current_id=0, affected_id=None, no_md=False, eos=False, length_override=None):
    """The 128-byte packets carrying `body`."""
    affected_id = current_id if affected_id is None else affected_id
    length = 0 if no_md else len(body)
    declared = length if length_override is None else length_override
    if no_md or length <= 119:
        count = 1
    else:
        count = 1 + (length - 119 + 120) // 121
    out, at = [], 0
    for k in range(count):
        p = bytearray(128)
        ptype = 0 if count == 1 else (1 if k == 0 else (3 if k == count - 1 else 2))
        p[0] = (ptype << 6) | (1 if no_md else 0)
        p[1] = ((affected_id & 15) << 4) | (current_id & 15)
        p[2] = 1 if eos else 0
        pos = 3
        if k == 0 and not no_md:
            p[3] = (declared >> 8) & 0xFF
            p[4] = declared & 0xFF
            pos = 5
        room = 124 - pos
        chunk = 0 if no_md else min(room, length - at)
        p[pos:pos + chunk] = body[at:at + chunk]
        at += chunk
        crc = crc32_mpeg2(bytes(p[:124]))
        p[124:128] = struct.pack(">I", crc)
        out.append(bytes(p))
    return out


# ---------------------------------------------------------------- picture + embedding
_PARITY12 = np.array([bin(v).count("1") & 1 for v in range(4096)], dtype=np.uint16)


def embed(I, C, pkts, corrupt=None):
    """Writes the packets into the chroma LSBs (in place). I, C: (h, w) uint16 12-bit codes
    (C = the 4:2:2 chroma sample of each pixel: Ct/Cb on even x, Cp/Cr on odd x).
    corrupt: optional set of (packet, copy) whose bits are inverted (a damaged copy).
    Returns False (nothing written) when the raster is too small."""
    h, w = I.shape
    need = len(pkts) * 3072
    if need > h * w:
        return False
    fi, fc = I.reshape(-1), C.reshape(-1)
    for p, pkt in enumerate(pkts):
        bits = np.unpackbits(np.frombuffer(pkt, dtype=np.uint8)).astype(np.uint16)   # MSB first
        for copy in range(3):
            b = bits ^ 1 if corrupt and (p, copy) in corrupt else bits
            idx = np.arange(1024) + p * 3072 + copy * 1024
            c = fc[idx]
            scramble = _PARITY12[c & 0xFFE] ^ _PARITY12[fi[idx] & 0xFFF]
            fc[idx] = (c & 0xFFE) | ((b ^ scramble) & 1)
    return True


def rgb8(I, C):
    """HDMI 1.4a 6.5.4: (h, w, 3) uint8."""
    out = np.empty(I.shape + (3,), dtype=np.uint8)
    out[..., 0] = (C >> 4) & 0xFF
    out[..., 1] = (I >> 4) & 0xFF
    out[..., 2] = ((C & 15) << 4) | (I & 15)
    return out


ENCODING_IDS = {"r210": 6, "R10b": 7, "R10l": 8, "V210": 3}


def row_bytes(encoding, width):
    if encoding == "V210":
        return ((width + 47) // 48) * 128
    return ((width + 63) // 64) * 256


def container(rgb, encoding="r210", placement="low8", pad_bits=0, row_pad_seed=0):
    """An RGB8 picture stored the way a DeckLink stores it. Returns bytes."""
    h, w, _ = rgb.shape
    v = rgb.astype(np.uint32)
    if placement == "low8":
        f = v
    elif placement == "high8":
        f = v << 2
    elif placement == "high8rep":
        f = (v << 2) | (v >> 6)
    else:
        raise ValueError(placement)
    word = (f[..., 0] << 20) | (f[..., 1] << 10) | f[..., 2]
    if encoding != "r210":
        word = (word << 2) | (np.uint32(pad_bits) & 3)
    stride = row_bytes(encoding, w)
    words_per_row = stride // 4
    full = np.zeros((h, words_per_row), dtype=np.uint32)
    if words_per_row > w:
        rng = np.random.default_rng(row_pad_seed)
        full[:, w:] = rng.integers(0, 2 ** 32, size=(h, words_per_row - w), dtype=np.uint64).astype(np.uint32)
    full[:, :w] = word
    dt = ">u4" if encoding in ("r210", "R10b") else "<u4"
    return full.astype(dt).tobytes()


# ---------------------------------------------------------------- V210 (LLDV-like companion)
def v210(Y, Cb, Cr):
    """10-bit 4:2:2 planes (h, w) Y and (h, w/2) Cb, Cr -> V210 bytes (VP stride)."""
    h, w = Y.shape
    stride = row_bytes("V210", w)
    out = np.zeros((h, stride // 4), dtype="<u4")
    groups = w // 6
    for g in range(groups):
        x = g * 6
        c = g * 3
        cb0, cb1, cb2 = Cb[:, c], Cb[:, c + 1], Cb[:, c + 2]
        cr0, cr1, cr2 = Cr[:, c], Cr[:, c + 1], Cr[:, c + 2]
        y0, y1, y2, y3, y4, y5 = (Y[:, x + k].astype(np.uint32) for k in range(6))
        out[:, g * 4 + 0] = cb0.astype(np.uint32) | (y0 << 10) | (cr0.astype(np.uint32) << 20)
        out[:, g * 4 + 1] = y1 | (cb1.astype(np.uint32) << 10) | (y2 << 20)
        out[:, g * 4 + 2] = cr1.astype(np.uint32) | (y3 << 10) | (cb2.astype(np.uint32) << 20)
        out[:, g * 4 + 3] = y4 | (cr2.astype(np.uint32) << 10) | (y5 << 20)
    return out.tobytes()


# ---------------------------------------------------------------- content
class Layout:
    """Patch rectangles in pixels for a raster; all measurement areas avoid the DM rows."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        s = h / 1080.0
        self.s = s
        self.header = (0, int(round(40 * s)))
        self.stamp = (int(round(48 * s)), int(round(96 * s)))
        self.wedge = (int(round(110 * s)), int(round(360 * s)))
        self.colour = (int(round(380 * s)), int(round(620 * s)))
        self.ramp = (int(round(640 * s)), int(round(720 * s)))
        self.chroma_ramp = (int(round(728 * s)), int(round(768 * s)))
        self.extremes = (int(round(780 * s)), int(round(880 * s)))
        self.detail = (int(round(900 * s)), h)


# Wedge levels (nits): spans black to 10000, includes the projector's 48 and the SDR 100.
WEDGE_NITS = [0.0, 0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0,
              10.0, 20.0, 30.0, 48.0, 70.0, 100.0, 150.0, 200.0, 300.0, 400.0, 600.0, 1000.0]
# Colour patches: linear BT.2020 RGB (nits). 709 primaries/secondaries at 50 nits luminance-ish,
# skin tones, P3 and BT.2020 saturated colours, near-black colours, highlights.
_m709 = rgb_convert_matrix(PRIM_709, PRIM_2020)
_mp3 = rgb_convert_matrix(PRIM_P3D65, PRIM_2020)
COLOUR_PATCHES = [
    ("709 red 40", (_m709 @ np.array([40.0, 0, 0])).tolist()),
    ("709 green 40", (_m709 @ np.array([0, 40.0, 0])).tolist()),
    ("709 blue 40", (_m709 @ np.array([0, 0, 40.0])).tolist()),
    ("709 cyan 40", (_m709 @ np.array([0, 40.0, 40.0])).tolist()),
    ("709 magenta 40", (_m709 @ np.array([40.0, 0, 40.0])).tolist()),
    ("709 yellow 40", (_m709 @ np.array([40.0, 40.0, 0])).tolist()),
    ("skin light", (_m709 @ np.array([42.0, 28.0, 21.0])).tolist()),
    ("skin dark", (_m709 @ np.array([12.0, 7.0, 4.5])).tolist()),
    ("P3 red 30", (_mp3 @ np.array([30.0, 0, 0])).tolist()),
    ("P3 green 30", (_mp3 @ np.array([0, 30.0, 0])).tolist()),
    ("2020 red 20", [20.0, 0.0, 0.0]),
    ("2020 green 20", [0.0, 20.0, 0.0]),
    ("dark blue 0.5", (_m709 @ np.array([0.1, 0.2, 0.6])).tolist()),
    ("dark red 0.3", (_m709 @ np.array([0.5, 0.05, 0.05])).tolist()),
    ("sky 200", (_m709 @ np.array([150.0, 190.0, 260.0])).tolist()),
    ("sun 800", (_m709 @ np.array([900.0, 800.0, 600.0])).tolist()),
]
# BTB/WTW-like extremes: I codes (neutral chroma) below the DM's source minimum (62) and
# above the source maximum (3696, ~3940 used as the scene peak elsewhere).
EXTREME_I = [0, 2, 4, 8, 16, 24, 32, 48, 62, 80, 3500, 3600, 3696, 3800, 3900, 4000, 4095]


def build_picture(w, h, rep, frame_stamp, scene=0, variant="patches", seed=1):
    """(I, C, patches): 12-bit code planes and the patch list
    [{name, kind, rect:(x0,y0,x1,y1), codes:[I,P,T]}] (rect excludes a margin)."""
    L = Layout(w, h)
    I = np.zeros((h, w), dtype=np.float64)
    P = np.zeros((h, w), dtype=np.float64)
    T = np.zeros((h, w), dtype=np.float64)
    patches = []
    neutral = lambda nits: neutral_codes(rep, nits)
    bg = neutral(10.0)
    I[:], P[:], T[:] = bg

    def fill(y0, y1, x0, x1, codes, name, kind):
        I[y0:y1, x0:x1], P[y0:y1, x0:x1], T[y0:y1, x0:x1] = codes
        mx, my = max(2, (x1 - x0) // 5), max(2, (y1 - y0) // 5)
        patches.append({"name": name, "kind": kind, "rect": [x0 + mx, y0 + my, x1 - mx, y1 - my],
                        "codes_unrounded": [float(c) for c in codes]})

    # Wedge: 2 rows x 12.
    y0, y1 = L.wedge
    rows = 2
    cols = len(WEDGE_NITS) // rows
    gap = max(2, int(4 * L.s))
    for k, nits in enumerate(WEDGE_NITS):
        r, c = divmod(k, cols)
        ya = y0 + r * (y1 - y0) // rows
        yb = y0 + (r + 1) * (y1 - y0) // rows - gap
        xa = c * w // cols
        xb = (c + 1) * w // cols - gap
        fill(ya, yb, xa, xb, neutral(nits), "grey %g" % nits, "wedge")
    # Colour patches: 2 rows x 8.
    y0, y1 = L.colour
    cols = len(COLOUR_PATCHES) // 2
    for k, (name, rgb) in enumerate(COLOUR_PATCHES):
        r, c = divmod(k, cols)
        ya = y0 + r * (y1 - y0) // 2
        yb = y0 + (r + 1) * (y1 - y0) // 2 - gap
        xa = c * w // cols
        xb = (c + 1) * w // cols - gap
        fill(ya, yb, xa, xb, rep.encode(np.array(rgb)), name, "colour")
    # Intensity ramp: I 0..4095 across the width, neutral chroma of the mid grey.
    y0, y1 = L.ramp
    x = np.arange(w, dtype=np.float64)
    nc = neutral(100.0)
    I[y0:y1, :] = (x * 4095.0 / (w - 1))[None, :]
    P[y0:y1, :], T[y0:y1, :] = nc[1], nc[2]
    patches.append({"name": "I ramp", "kind": "ramp", "rect": [0, y0 + 4, w, y1 - 4], "codes_unrounded": None})
    # Chroma ramp: P (Ct) swept 1024..3072 at a mid I, T neutral.
    y0, y1 = L.chroma_ramp
    I[y0:y1, :] = nc[0]
    P[y0:y1, :] = (1024.0 + x * 2048.0 / (w - 1))[None, :]
    T[y0:y1, :] = nc[2]
    patches.append({"name": "P ramp", "kind": "chroma_ramp", "rect": [0, y0 + 4, w, y1 - 4], "codes_unrounded": None})
    # Extremes.
    y0, y1 = L.extremes
    cols = len(EXTREME_I)
    for k, code in enumerate(EXTREME_I):
        xa = k * w // cols
        xb = (k + 1) * w // cols - gap
        fill(y0, y1, xa, xb, np.array([float(code), nc[1], nc[2]]), "I %d" % code, "extreme")
    # Detail: 1-px line pairs | 2x2 checker | chroma alternation | seeded texture.
    y0, y1 = L.detail
    q = w // 4
    yy, xx = np.mgrid[y0:y1, 0:w]
    lines = np.where((xx[:, :q] & 1) == 0, 1200.0, 2400.0)
    I[y0:y1, :q] = lines
    checker = np.where(((xx[:, q:2 * q] >> 1) + (yy[:, q:2 * q] >> 1)) & 1, 1300.0, 2300.0)
    I[y0:y1, q:2 * q] = checker
    I[y0:y1, 2 * q:3 * q] = nc[0]
    P[y0:y1, 2 * q:3 * q] = np.where(((xx[:, 2 * q:3 * q] >> 1) & 1) == 0, nc[1] + 300, nc[1] - 300)
    rng = np.random.default_rng(seed)
    I[y0:y1, 3 * q:] = 1500.0 + rng.normal(0, 40.0, size=(y1 - y0, w - 3 * q))
    P[y0:y1, 3 * q:] = nc[1] + rng.normal(0, 16.0, size=(y1 - y0, w - 3 * q))
    T[y0:y1, 3 * q:] = nc[2] + rng.normal(0, 16.0, size=(y1 - y0, w - 3 * q))
    patches.append({"name": "detail", "kind": "detail", "rect": [0, y0, w, y1], "codes_unrounded": None})
    if variant == "flat":
        # A single flat picture (scene-only content): the wedge mid grey everywhere below the stamp.
        g = neutral(48.0)
        I[L.wedge[0]:, :], P[L.wedge[0]:, :], T[L.wedge[0]:, :] = g
        patches = [{"name": "flat 48", "kind": "wedge", "rect": [w // 4, L.wedge[0] + 20, 3 * w // 4, h - 20],
                    "codes_unrounded": [float(c) for c in g]}]
    elif variant.startswith("scene"):
        # A scene picture: everything scaled by a scene gain (cuts that change the picture).
        gain = float(variant[5:]) if len(variant) > 5 else 1.0
        rgb = rep.decode(np.stack([I, P, T], axis=-1)) * gain
        e = rep.encode(np.clip(rgb, 0.0, None))
        I, P, T = e[..., 0], e[..., 1], e[..., 2]
        for p in patches:
            if p["codes_unrounded"] is not None:
                c = rep.encode(np.clip(rep.decode(np.array(p["codes_unrounded"])) * gain, 0.0, None))
                p["codes_unrounded"] = [float(v) for v in c]
    # Stamp: 32 cells (magic 0xA5, 16-bit frame index, 8-bit scene).
    word = (0xA5 << 24) | ((frame_stamp & 0xFFFF) << 8) | (scene & 0xFF)
    y0, y1 = L.stamp
    nb = neutral(1.0)
    for bit in range(32):
        on = (word >> (31 - bit)) & 1
        xa, xb = bit * w // 32, (bit + 1) * w // 32 - gap
        I[y0:y1, xa:xb] = 3000.0 if on else 600.0
        P[y0:y1, xa:xb], T[y0:y1, xa:xb] = nb[1], nb[2]
    # 4:2:2: the pair's chroma (Ct/Cb at the even pixel, Cp/Cr at the odd) = the pair mean.
    Pm = 0.5 * (P[:, 0::2] + P[:, 1::2])
    Tm = 0.5 * (T[:, 0::2] + T[:, 1::2])
    Ic = np.clip(np.rint(I), 0, 4095).astype(np.uint16)
    C = np.empty((h, w), dtype=np.uint16)
    C[:, 0::2] = np.clip(np.rint(Pm), 0, 4095).astype(np.uint16)
    C[:, 1::2] = np.clip(np.rint(Tm), 0, 4095).astype(np.uint16)
    for p in patches:
        if p["codes_unrounded"] is not None:
            p["codes"] = [int(np.clip(round(v), 0, 4095)) for v in p["codes_unrounded"]]
    return Ic, C, patches


def stamp_word(frame_stamp, scene):
    return (0xA5 << 24) | ((frame_stamp & 0xFFFF) << 8) | (scene & 0xFF)


# ---------------------------------------------------------------- reference output (no tone mapping)
def encode_output(rgb2020_nits, target_nits, gamma=2.2, primaries="bt2020", black_nits=0.0):
    """Linear BT.2020 nits -> the full-range 10-bit codes of a pure power-law output at
    target_nits peak (what VP outputs with tone_mapping clip when nothing exceeds the
    target and the source range is within it). Returns float codes (unrounded)."""
    rgb = np.asarray(rgb2020_nits, dtype=np.float64)
    if primaries == "bt709":
        rgb = rgb @ rgb_convert_matrix(PRIM_2020, PRIM_709).T
    elif primaries == "p3":
        rgb = rgb @ rgb_convert_matrix(PRIM_2020, PRIM_P3D65).T
    rel = np.clip((rgb - black_nits) / (target_nits - black_nits), 0.0, 1.0)
    return 1023.0 * rel ** (1.0 / gamma)


def decode_output(codes, target_nits, gamma=2.2):
    v = np.clip(np.asarray(codes, dtype=np.float64) / 1023.0, 0.0, 1.0)
    return target_nits * v ** gamma


def ictcp_from_rgb2020(rgb_nits):
    """BT.2100 ICtCp (PQ) of linear BT.2020 nits, for Delta E ITP (ITU-R BT.2124)."""
    m = np.array([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], dtype=np.float64) / 4096.0
    lms = np.clip(np.asarray(rgb_nits, dtype=np.float64), 0.0, None) @ m.T
    pq = nits_to_pq(lms)
    n = np.array([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], dtype=np.float64) / 4096.0
    return pq @ n.T


def delta_e_itp(rgb_a, rgb_b):
    a = ictcp_from_rgb2020(rgb_a)
    b = ictcp_from_rgb2020(rgb_b)
    d = a - b
    return 720.0 * np.sqrt(d[..., 0] ** 2 + (0.5 * d[..., 1]) ** 2 + d[..., 2] ** 2)


def self_check():
    print("derived HPE LMS->BT.2020 minus libplacebo's constant: max |diff| = %.2e" %
          np.abs(LMS_TO_RGB2020 - LIBPLACEBO_LMS2RGB).max())
    for rep in REPS.values():
        rgb = np.array([[0.0, 0, 0], [1, 1, 1], [100, 100, 100], [1000, 10, 50], [5, 80, 3]])
        back = rep.decode(rep.encode(rgb))
        print(rep.name, "round trip (unrounded) max |error| nits:", np.abs(back - rgb).max())
    print("CRC check value 0x%08X" % crc32_mpeg2(b"123456789"))


if __name__ == "__main__":
    if "--self-check" in sys.argv:
        self_check()
