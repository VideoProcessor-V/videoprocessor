# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Frame container, raw / y4m / ffmpeg-pipe readers and deterministic separable resamplers.

All planes are float64 arrays of CODE VALUES (not normalised).  No PIL, no GPU.
Resamplers are separable weight matrices (scipy.sparse) applied in a fixed order,
so the same input always gives the same bytes.
"""
import os
import subprocess
from dataclasses import dataclass, replace

import numpy as np
import scipy.sparse as sp

DEFAULT_FFMPEG = r"D:\tools\ffmpeg\bin\ffmpeg.exe"


@dataclass
class Frame:
    Y: np.ndarray
    Cb: np.ndarray
    Cr: np.ndarray
    bitdepth: int = 8
    range: str = "limited"          # limited | full
    matrix: str = "709"             # 709 | 2020 | 601
    transfer: str = "sdr"           # sdr | pq
    fps: float = 24.0
    index: int = 0                  # source-frame index of the picture this frame represents
    siting: str = "left"            # chroma siting: left (MPEG-2) | center

    def planes(self):
        return (("Y", self.Y), ("Cb", self.Cb), ("Cr", self.Cr))

    def meta(self):
        return dict(bitdepth=self.bitdepth, range=self.range, matrix=self.matrix, transfer=self.transfer,
                    fps=self.fps, index=self.index, siting=self.siting, width=self.Y.shape[1], height=self.Y.shape[0],
                    chroma_width=self.Cb.shape[1], chroma_height=self.Cb.shape[0])

    def with_planes(self, Y=None, Cb=None, Cr=None, **kw):
        return replace(self, Y=self.Y if Y is None else Y, Cb=self.Cb if Cb is None else Cb,
                       Cr=self.Cr if Cr is None else Cr, **kw)


# ---------------------------------------------------------------- formats
# pix_fmt -> (bitdepth, chroma subsampling factors (fy, fx))
PIX_FMTS = {
    "yuv420p": (8, (2, 2)), "yuv420p10le": (10, (2, 2)), "yuv420p12le": (12, (2, 2)), "yuv420p16le": (16, (2, 2)),
    "yuv444p": (8, (1, 1)), "yuv444p10le": (10, (1, 1)), "yuv444p12le": (12, (1, 1)), "yuv444p16le": (16, (1, 1)),
}


def chroma_shape(h, w, sub):
    return ((h + sub[0] - 1) // sub[0], (w + sub[1] - 1) // sub[1])


def frame_bytes(w, h, pix_fmt):
    bd, sub = PIX_FMTS[pix_fmt]
    ch, cw = chroma_shape(h, w, sub)
    return (w * h + 2 * ch * cw) * (1 if bd == 8 else 2)


def _decode_planar(buf, w, h, pix_fmt, **meta):
    bd, sub = PIX_FMTS[pix_fmt]
    ch, cw = chroma_shape(h, w, sub)
    dt = np.uint8 if bd == 8 else np.dtype("<u2")
    a = np.frombuffer(buf, dtype=dt)
    n0, n1 = w * h, ch * cw
    if a.size != n0 + 2 * n1:
        raise ValueError("buffer is %d samples, expected %d" % (a.size, n0 + 2 * n1))
    Y = a[:n0].reshape(h, w).astype(np.float64)
    Cb = a[n0:n0 + n1].reshape(ch, cw).astype(np.float64)
    Cr = a[n0 + n1:].reshape(ch, cw).astype(np.float64)
    return Frame(Y, Cb, Cr, bitdepth=bd, **meta)


def pix_fmt_of(frame):
    sub = (frame.Y.shape[0] // frame.Cb.shape[0], frame.Y.shape[1] // frame.Cb.shape[1])
    for k, v in PIX_FMTS.items():
        if v == (frame.bitdepth, sub):
            return k
    raise ValueError("no planar pix_fmt for bitdepth %d subsampling %s" % (frame.bitdepth, sub))


def encode_planar(frame, pix_fmt=None):
    """Frame -> bytes in a planar pix_fmt (rounded, clipped); used by tests and capture writers."""
    pix_fmt = pix_fmt or pix_fmt_of(frame)
    bd = PIX_FMTS[pix_fmt][0]
    dt = np.uint8 if bd == 8 else np.dtype("<u2")
    top = (1 << bd) - 1
    out = b""
    for _, p in frame.planes():
        out += np.clip(np.rint(p), 0, top).astype(dt).tobytes()
    return out


def read_raw_frames(path, w, h, pix_fmt, start=0, count=1, **meta):
    """Raw planar file, no header.  start/count are frame indices."""
    fb = frame_bytes(w, h, pix_fmt)
    out = []
    with open(path, "rb") as f:
        f.seek(start * fb)
        for i in range(count):
            buf = f.read(fb)
            if len(buf) != fb:
                break
            out.append(_decode_planar(buf, w, h, pix_fmt, index=start + i, **meta))
    return out


def raw_frame_count(path, w, h, pix_fmt):
    return os.path.getsize(path) // frame_bytes(w, h, pix_fmt)


_Y4M_FMT = {"420": "yuv420p", "420jpeg": "yuv420p", "420mpeg2": "yuv420p", "420paldv": "yuv420p",
            "420p10": "yuv420p10le", "420p12": "yuv420p12le", "420p16": "yuv420p16le",
            "444": "yuv444p", "444p10": "yuv444p10le", "444p12": "yuv444p12le", "444p16": "yuv444p16le"}


def parse_y4m_header(line):
    """dict(width,height,fps,pix_fmt,interlace) from the first y4m line (bytes or str)."""
    if isinstance(line, bytes):
        line = line.decode("ascii", "replace")
    tok = line.strip().split()
    if not tok or tok[0] != "YUV4MPEG2":
        raise ValueError("not a y4m header")
    d = dict(width=0, height=0, fps=0.0, pix_fmt="yuv420p", interlace="p")
    for t in tok[1:]:
        k, v = t[0], t[1:]
        if k == "W":
            d["width"] = int(v)
        elif k == "H":
            d["height"] = int(v)
        elif k == "F":
            a, b = v.split(":")
            d["fps"] = int(a) / int(b)
        elif k == "I":
            d["interlace"] = v
        elif k == "C":
            d["pix_fmt"] = _Y4M_FMT[v]
    if not d["width"] or not d["height"]:
        raise ValueError("y4m header lacks W/H")
    return d


def y4m_info(path):
    with open(path, "rb") as f:
        d = parse_y4m_header(f.readline())
        hdr_len = f.tell()
    fb = frame_bytes(d["width"], d["height"], d["pix_fmt"])
    d["frames"] = (os.path.getsize(path) - hdr_len) // (fb + 6)
    return d


def read_y4m_frames(path, start=0, count=1, **meta):
    with open(path, "rb") as f:
        d = parse_y4m_header(f.readline())
        fb = frame_bytes(d["width"], d["height"], d["pix_fmt"])
        # plain "FRAME\n" markers (no frame parameters) are assumed: fixed stride, seekable
        f.seek(start * (fb + 6), os.SEEK_CUR)
        out = []
        for i in range(count):
            mark = f.readline()
            if not mark.startswith(b"FRAME"):
                break
            buf = f.read(fb)
            if len(buf) != fb:
                break
            out.append(_decode_planar(buf, d["width"], d["height"], d["pix_fmt"], fps=d["fps"], index=start + i, **meta))
    return out


def write_y4m(path, frames, fps_num=24, fps_den=1):
    f0 = frames[0]
    tag = {"yuv420p": "420", "yuv420p10le": "420p10", "yuv444p": "444", "yuv444p10le": "444p10"}[pix_fmt_of(f0)]
    with open(path, "wb") as f:
        f.write(("YUV4MPEG2 W%d H%d F%d:%d Ip A1:1 C%s\n" % (f0.Y.shape[1], f0.Y.shape[0], fps_num, fps_den, tag)).encode())
        for fr in frames:
            f.write(b"FRAME\n")
            f.write(encode_planar(fr))


def write_raw(path, frames):
    with open(path, "wb") as f:
        for fr in frames:
            f.write(encode_planar(fr))


def ffmpeg_decode_window(path, start_frame, count, width, height, pix_fmt, fps, ffmpeg=DEFAULT_FFMPEG, **meta):
    """Decode `count` frames from `start_frame` through an ffmpeg rawvideo pipe (explicit pix_fmt).

    Seeks with -ss half a frame before the wanted frame (frames with pts < ss are dropped).  Exact for intra-only masters (FFV1) and for the encodes of
    this corpus, which share the master's timeline; callers verify alignment with
    eval_protocols.verify_alignment.
    """
    ss = (start_frame - 0.5) / fps if start_frame else 0.0
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-ss", "%.6f" % ss, "-i", path, "-frames:v", str(count),
           "-an", "-sn", "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError("ffmpeg failed: " + r.stderr.decode("utf8", "replace")[-500:])
    fb = frame_bytes(width, height, pix_fmt)
    out = []
    for i in range(len(r.stdout) // fb):
        out.append(_decode_planar(r.stdout[i * fb:(i + 1) * fb], width, height, pix_fmt, fps=fps,
                                  index=start_frame + i, **meta))
    return out


def to_bitdepth(frame, bd):
    """Exact code scaling by powers of two between bit depths (8 -> 10 is x4)."""
    if bd == frame.bitdepth:
        return frame
    s = 2.0 ** (bd - frame.bitdepth)
    return frame.with_planes(frame.Y * s, frame.Cb * s, frame.Cr * s, bitdepth=bd)


# ---------------------------------------------------------------- resampling
def _cubic(x, B, C):
    x = np.abs(x)
    x2, x3 = x * x, x * x * x
    r = np.zeros_like(x)
    m1 = x < 1
    m2 = (x >= 1) & (x < 2)
    r[m1] = ((12 - 9 * B - 6 * C) * x3[m1] + (-18 + 12 * B + 6 * C) * x2[m1] + (6 - 2 * B)) / 6
    r[m2] = ((-B - 6 * C) * x3[m2] + (6 * B + 30 * C) * x2[m2] + (-12 * B - 48 * C) * x[m2] + (8 * B + 24 * C)) / 6
    return r


def _lanczos3(x):
    x = np.asarray(x, dtype=np.float64)
    return np.where(np.abs(x) < 3, np.sinc(x) * np.sinc(x / 3), 0.0)


KERNELS = {
    # name: (function, support in source pixels at unit scale)
    "bicubic": (lambda x: _cubic(x, 0.0, 0.5), 2.0),     # Catmull-Rom (a = -0.5): the harness's "bicubic"
    "catmull-rom": (lambda x: _cubic(x, 0.0, 0.5), 2.0),
    "mitchell": (lambda x: _cubic(x, 1 / 3, 1 / 3), 2.0),
    "lanczos3": (_lanczos3, 3.0),
}


def _weights(n_in, n_out, kernel, shift):
    """Sparse (n_out x n_in) resampling matrix.  Output pixel i samples source coordinate
    (i + 0.5) * n_in / n_out - 0.5 + shift; the kernel is stretched by the scale when downsizing
    (anti-aliasing); edges are clamped (replicated); rows are normalised to sum 1."""
    fn, sup = KERNELS[kernel]
    scale = n_in / n_out
    stretch = max(scale, 1.0)
    rows, cols, vals = [], [], []
    for i in range(n_out):
        c = (i + 0.5) * scale - 0.5 + shift
        lo = int(np.floor(c - sup * stretch)) + 1
        hi = int(np.ceil(c + sup * stretch)) - 1
        idx = np.arange(lo, hi + 1)
        w = fn((idx - c) / stretch)
        s = w.sum()
        if s == 0:
            idx = np.array([int(np.clip(round(c), 0, n_in - 1))])
            w = np.array([1.0])
            s = 1.0
        w = w / s
        cl = np.clip(idx, 0, n_in - 1)
        rows.extend([i] * len(cl))
        cols.extend(cl.tolist())
        vals.extend(w.tolist())
    return sp.csr_matrix((vals, (rows, cols)), shape=(n_out, n_in))


_WCACHE = {}


def _wcached(n_in, n_out, kernel, shift):
    k = (n_in, n_out, kernel, round(shift, 9))
    if k not in _WCACHE:
        if len(_WCACHE) > 64:
            _WCACHE.clear()
        _WCACHE[k] = _weights(n_in, n_out, kernel, shift)
    return _WCACHE[k]


def resample_plane(plane, out_h, out_w, kernel="bicubic", shift_y=0.0, shift_x=0.0):
    """Separable resample (vertical, then horizontal) of a 2-D float64 plane.  `shift_*` are
    source-pixel offsets of the sampling grid (chroma siting); 0 = centre-aligned."""
    out = np.asarray(plane, dtype=np.float64)
    h, w = out.shape
    if out_h != h or shift_y:
        out = _wcached(h, out_h, kernel, shift_y) @ out
    if out_w != w or shift_x:
        out = (_wcached(w, out_w, kernel, shift_x) @ out.T).T
    return np.ascontiguousarray(out)


def chroma_up_shifts(f_y, f_x, siting):
    """Source-grid shifts for upsampling a chroma plane by integer factors (f_y, f_x).
    left (MPEG-2): chroma co-sited with luma column 2j horizontally, centred vertically."""
    sx = (0.5 - 0.5 / f_x) if (siting == "left" and f_x > 1) else 0.0
    return 0.0, sx


def chroma_down_shifts(f_y, f_x, siting):
    """Shifts for subsampling a full-resolution plane to chroma resolution (inverse of the above)."""
    sx = (0.5 - f_x / 2.0) if (siting == "left" and f_x > 1) else 0.0
    return 0.0, sx
