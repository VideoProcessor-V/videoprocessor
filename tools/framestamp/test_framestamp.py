# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for the vp-stamp/1 Python mirror.  Run from any cwd:
    python -I -B tools/framestamp/test_framestamp.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import framestamp as fs  # noqa: E402

# Golden vectors: identical literals are asserted in FrameStampTests.cpp.
GOLDEN_WORDS = [
    ((0, 0, 0), 0xA50000000000AC39),
    ((0, 4, 1), 0xA50400000001351E),
    ((1, 5, 0xFFFFFFFF), 0xA545FFFFFFFF07C9),
    ((1, 63, 0x12345678), 0xA57F12345678606A),
    ((0, 63, 0xDEADBEEF), 0xA53FDEADBEEF0179),
    ((1, 0, 1), 0xA54000000001AD70),
]
GOLDEN_FRAME_HASH = 0x2A42BB6007F3FDB5   # FNV-1a 64, 480x270 10-bit limited, flat 64, source (0, 3, 0x01020304)

HEIGHTS = [270, 720, 1080, 2160, 541, 999]


def width_for(h):
    return (h * 16 + 8) // 9


def make_frame(h, depth=10, full=False, corner=fs.CORNER_SOURCE, w=None, bg=None, word=None):
    w = w or width_for(h)
    black, _ = fs.levels(depth, full)
    plane = np.full((h, w), black if bg is None else bg, dtype=np.uint16)
    word = fs.word(0, 3, 12345) if word is None else word
    fs.write(plane, depth, full, corner, word)
    return plane


def bilinear_up2(p):
    f = p.astype(np.float64)
    h, w = f.shape
    ys = (np.arange(h * 2) + 0.5) / 2 - 0.5
    xs = (np.arange(w * 2) + 0.5) / 2 - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, h - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, w - 1)
    x1 = np.clip(x0 + 1, 0, w - 1)
    fy = np.clip(ys - np.floor(ys), 0, 1)[:, None]
    fx = np.clip(xs - np.floor(xs), 0, 1)[None, :]
    top = f[y0][:, x0] * (1 - fx) + f[y0][:, x1] * fx
    bot = f[y1][:, x0] * (1 - fx) + f[y1][:, x1] * fx
    return np.rint(top * (1 - fy) + bot * fy).astype(np.uint16)


def box_down2(p):
    f = p.astype(np.float64)
    return np.rint((f[0::2, 0::2] + f[1::2, 0::2] + f[0::2, 1::2] + f[1::2, 1::2]) / 4).astype(np.uint16)


class FrameStampTests(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(fs.crc16(b"123456789"), 0x29B1)

    def test_golden_words(self):
        for (kind, aux, index), expected in GOLDEN_WORDS:
            self.assertEqual(fs.word(kind, aux, index), expected)

    def test_golden_frame_hash(self):
        plane = np.full((270, 480), 64, dtype=np.uint16)
        fs.write(plane, 10, False, fs.CORNER_SOURCE, fs.word(0, 3, 0x01020304))
        self.assertEqual(fs.fnv1a64(plane.astype("<u2").tobytes()), GOLDEN_FRAME_HASH)

    def test_round_trip_all_heights(self):
        for h in HEIGHTS:
            for corner, kind in ((fs.CORNER_SOURCE, 0), (fs.CORNER_OUTPUT, 1)):
                w = fs.word(kind, 17, 0xA5C3F00D)
                plane = make_frame(h, corner=corner, word=w)
                r = fs.decode(plane, 10, corner)
                self.assertTrue(r["found"], (h, corner))
                self.assertEqual((r["kind"], r["aux"], r["index"]), (kind, 17, 0xA5C3F00D))
                self.assertEqual((r["offset_x"], r["offset_y"]), (0.0, 0.0))

    def test_depths_and_ranges(self):
        for depth in (8, 10, 12, 16):
            for full in (False, True):
                plane = make_frame(540, depth=depth, full=full)
                r = fs.decode(plane, depth, fs.CORNER_SOURCE)
                self.assertTrue(r["found"], (depth, full))
                self.assertEqual(r["index"], 12345)

    def test_limited_levels(self):
        self.assertEqual(fs.levels(10, False), (64, 940))
        self.assertEqual(fs.levels(8, False), (16, 235))
        self.assertEqual(fs.levels(12, False), (256, 3760))
        self.assertEqual(fs.levels(10, True), (0, 1023))

    def test_field_extremes(self):
        for kind in (0, 1, 2, 3):
            for aux in (0, 63):
                for index in (0, 0xFFFFFFFF):
                    plane = make_frame(540, word=fs.word(kind, aux, index))
                    r = fs.decode(plane, 10, fs.CORNER_SOURCE)
                    self.assertTrue(r["found"])
                    self.assertEqual((r["kind"], r["aux"], r["index"]), (kind, aux, index))

    def test_scaled_1080_to_2160_and_back(self):
        src = make_frame(1080, corner=fs.CORNER_OUTPUT, word=fs.word(1, 9, 777))
        up = bilinear_up2(src)
        self.assertEqual(up.shape[0], 2160)
        r = fs.decode(up, 10, fs.CORNER_OUTPUT)
        self.assertTrue(r["found"])
        self.assertEqual((r["aux"], r["index"]), (9, 777))
        big = make_frame(2160, word=fs.word(0, 2, 4242), w=3840)
        down = box_down2(big)
        r = fs.decode(down, 10, fs.CORNER_SOURCE)
        self.assertTrue(r["found"])
        self.assertEqual((r["aux"], r["index"]), (2, 4242))

    def test_chroma_irrelevant(self):
        # Packed UYVY-style 8-bit: luma at odd bytes, random chroma at even bytes.
        rng = np.random.default_rng(1234)
        luma = make_frame(540, depth=8, w=960)
        packed = np.zeros((540, 960 * 2), dtype=np.uint8)
        packed[:, 1::2] = luma
        packed[:, 0::2] = rng.integers(0, 256, size=(540, 960), dtype=np.uint8)
        r = fs.decode(packed[:, 1::2], 8, fs.CORNER_SOURCE)
        self.assertTrue(r["found"])
        self.assertEqual(r["index"], 12345)

    def test_crc_rejects_flipped_bit(self):
        w = fs.word(0, 3, 12345) ^ (1 << 20)
        plane = make_frame(540, word=w)
        self.assertFalse(fs.decode(plane, 10, fs.CORNER_SOURCE)["found"])

    def test_low_contrast_rejected(self):
        plane = make_frame(540).astype(np.float64)
        plane = np.rint(64 + (plane - 64) * 0.2).astype(np.uint16)   # contrast 175 < 256
        self.assertFalse(fs.decode(plane, 10, fs.CORNER_SOURCE)["found"])

    def test_levels_changed_still_decodes(self):
        plane = make_frame(720).astype(np.float64)
        mapped = np.rint(plane * 0.8 + 40).astype(np.uint16)          # tone-mapped style scale + offset
        r = fs.decode(mapped, 10, fs.CORNER_SOURCE)
        self.assertTrue(r["found"])
        self.assertEqual(r["index"], 12345)

    def test_small_shift_found_by_search(self):
        plane = make_frame(1080)
        shifted = np.roll(plane, (3, 5), axis=(0, 1))
        r = fs.decode(shifted, 10, fs.CORNER_SOURCE)
        self.assertTrue(r["found"])
        self.assertNotEqual((r["offset_x"], r["offset_y"]), (0.0, 0.0))
        self.assertFalse(fs.decode(shifted, 10, fs.CORNER_SOURCE, radius=0.0)["found"])

    def test_plain_picture_has_no_stamp(self):
        plane = np.full((540, 960), 400, dtype=np.uint16)
        self.assertFalse(fs.decode(plane, 10, fs.CORNER_SOURCE)["found"])

    def test_cpu_reference_equals_write(self):
        for h in (270, 541, 720, 999, 1080):
            for corner in (fs.CORNER_SOURCE, fs.CORNER_OUTPUT):
                w = fs.word(1, 21, 0xDEADBEEF)
                width = width_for(h)
                plane = np.zeros((h, width), dtype=np.uint16)
                fs.write(plane, 8, True, corner, w)    # full range 8 bit: 0 / 255
                x0, x1, y0, y1 = fs.block_rect(width, h, corner)
                for y in range(h):
                    for x in range(width):
                        ref = fs.cpu_reference(x, y, width, h, corner, w)
                        if ref < 0:
                            self.assertFalse(x0 <= x < x1 and y0 <= y < y1, (h, x, y))
                        else:
                            self.assertEqual(int(plane[y, x]), 255 if ref == 1.0 else 0, (h, corner, x, y))

    def test_block_unchanged_outside(self):
        plane = np.full((540, 960), 300, dtype=np.uint16)
        fs.write(plane, 10, False, fs.CORNER_SOURCE, fs.word(0, 0, 1))
        x0, x1, y0, y1 = fs.block_rect(960, 540, fs.CORNER_SOURCE)
        mask = np.ones(plane.shape, dtype=bool)
        mask[y0:y1, x0:x1] = False
        self.assertTrue((plane[mask] == 300).all())


class ClipGeneratorTests(unittest.TestCase):
    def _make(self, extra):
        import tempfile
        import make_stamped_clip as mk
        tmp = tempfile.mkdtemp(prefix="vpstamp-")
        mk.main(["--out", tmp, "--frames", "7", "--fps", "24000/1001", "--size", "480x270"] + extra)
        return tmp

    def _read(self, tmp, chroma_rows):
        with open(os.path.join(tmp, "stamped.y4m"), "rb") as f:
            data = f.read()
        nl = data.index(b"\n")
        header = data[:nl].decode()
        pos = nl + 1
        w, h = 480, 270
        ysz, csz = w * h * 2, (w // 2) * chroma_rows * 2
        frames = []
        while pos < len(data):
            self.assertEqual(data[pos:pos + 6], b"FRAME\n")
            pos += 6
            y = np.frombuffer(data[pos:pos + ysz], dtype="<u2").reshape(h, w)
            cb = np.frombuffer(data[pos + ysz:pos + ysz + csz], dtype="<u2").reshape(chroma_rows, w // 2)
            pos += ysz + 2 * csz
            frames.append((y, cb))
        return header, frames

    def test_clip_422_cadence_and_sent_csv(self):
        tmp = self._make(["--cadence", "2:3"])
        header, frames = self._read(tmp, 270)
        self.assertIn("C422p10", header)
        self.assertIn("F24000:1001", header)
        self.assertEqual(len(frames), 7)
        for i, (y, cb) in enumerate(frames):
            r = fs.decode(y, 10, fs.CORNER_SOURCE)
            self.assertTrue(r["found"])
            self.assertEqual((r["kind"], r["aux"], r["index"]), (0, i % 5, i))
            x0, x1, y0, y1 = fs.block_rect(480, 270, fs.CORNER_SOURCE)
            self.assertTrue((cb[y0:y1, x0 // 2:(x1 + 1) // 2] == 512).all())
        with open(os.path.join(tmp, "sent.csv")) as f:
            lines = f.read().splitlines()
        self.assertEqual(lines[0], "index,aux,pts_num,pts_den")
        self.assertEqual(lines[3], "2,2,2002,24000")
        self.assertEqual(lines[6], "5,0,5005,24000")

    def test_clip_420(self):
        tmp = self._make(["--420"])
        header, frames = self._read(tmp, 135)
        self.assertIn("C420p10", header)
        self.assertEqual(len(frames), 7)
        self.assertTrue(fs.decode(frames[3][0], 10, fs.CORNER_SOURCE)["found"])

    def test_fps_and_size_are_required(self):
        import make_stamped_clip as mk
        with self.assertRaises(SystemExit):
            mk.main(["--out", "x", "--frames", "1", "--size", "480x270"])
        with self.assertRaises(SystemExit):
            mk.main(["--out", "x", "--frames", "1", "--fps", "25/1"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
