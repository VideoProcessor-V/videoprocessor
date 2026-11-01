# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Candidate contract.

(a) Python candidate - a class with
        name, description, lookahead (int, default 0)
        reset(meta: dict)                    called once per case before the first frame
        process(frame, future=()) -> Frame   called in source order; `future` holds up to `lookahead`
                                             following input Frames (the harness buffers them), so a
                                             stateful / temporal candidate sees exactly the frames it
                                             declared.  Returns the output for `frame` (same index).
(b) Capture candidate - a directory of per-frame files `NNNNNN.yuv` (planar, format from the sidecar)
    plus `capture.json`:
        {"schema": 1, "producer": "VpScaleRender|raw-capture|...", "vp_commit": "...", "config_hash": "...",
         "width": W, "height": H, "pix_fmt": "yuv420p10le", "bitdepth": 10, "range": "limited",
         "matrix": "709", "transfer": "sdr", "fps": 24.0,
         "frames": [{"file": "000000.yuv", "source_frame_index": 20, "gpu_ms": 1.7}, ...]}
    gpu_ms is optional.  source_frame_index maps the capture to the case's source frame so the harness
    aligns it with the reference by index, never by order.

Specs accepted by `load_candidate`: a built-in name, `capture:<dir>`, or `py:<file.py>::<ClassName>`.
"""
import importlib.util
import json
import os
import time

import eval_io as eio


class PythonCandidate:
    name = "unnamed"
    description = ""
    lookahead = 0
    kind = "python"

    def reset(self, meta):
        pass

    def process(self, frame, future=()):
        raise NotImplementedError


class ChromaUpsample(PythonCandidate):
    """Upsample 4:2:0 chroma to the luma raster with a fixed separable kernel (chroma siting aware).
    Luma is passed through unchanged."""

    def __init__(self, kernel, name, description):
        self.kernel = kernel
        self.name = name
        self.description = description

    def process(self, frame, future=()):
        h, w = frame.Y.shape
        fy = h / frame.Cb.shape[0]
        fx = w / frame.Cb.shape[1]
        sy, sx = eio.chroma_up_shifts(round(fy), round(fx), frame.siting)
        cb = eio.resample_plane(frame.Cb, h, w, self.kernel, sy, sx)
        cr = eio.resample_plane(frame.Cr, h, w, self.kernel, sy, sx)
        return frame.with_planes(Cb=cb, Cr=cr)


BUILTIN = {
    "chroma_bicubic": lambda: ChromaUpsample("bicubic", "chroma_bicubic",
                                             "default: Catmull-Rom (a=-0.5) chroma upsampling, left siting"),
    "chroma_lanczos3": lambda: ChromaUpsample("lanczos3", "chroma_lanczos3",
                                              "candidate: Lanczos-3 chroma upsampling, left siting"),
}


class CaptureCandidate:
    kind = "capture"

    def __init__(self, directory):
        self.dir = directory
        with open(os.path.join(directory, "capture.json"), "r", encoding="utf8") as f:
            self.meta = json.load(f)
        self.name = "capture:" + os.path.basename(os.path.normpath(directory))
        self.description = "capture directory (producer %s, vp %s)" % (self.meta.get("producer", "?"),
                                                                       self.meta.get("vp_commit", "?"))
        self.index = {int(e["source_frame_index"]): e for e in self.meta["frames"]}

    def indices(self):
        return sorted(self.index)

    def frame(self, source_index):
        e = self.index[source_index]
        m = self.meta
        path = os.path.join(self.dir, e["file"])
        fr = eio.read_raw_frames(path, m["width"], m["height"], m["pix_fmt"], 0, 1,
                                 range=m.get("range", "limited"), matrix=m.get("matrix", "709"),
                                 transfer=m.get("transfer", "sdr"), fps=m.get("fps", 24.0))
        if not fr:
            raise ValueError("short capture file " + path)
        fr[0].index = source_index
        return fr[0], e.get("gpu_ms")


def load_candidate(spec):
    if spec in BUILTIN:
        return BUILTIN[spec]()
    if spec.startswith("capture:"):
        return CaptureCandidate(spec[len("capture:"):])
    if spec.startswith("py:"):
        path, cls = spec[3:].rsplit("::", 1)
        s = importlib.util.spec_from_file_location("eval_user_candidate", path)
        mod = importlib.util.module_from_spec(s)
        s.loader.exec_module(mod)
        return getattr(mod, cls)()
    raise KeyError("unknown candidate %r (built-ins: %s)" % (spec, ", ".join(sorted(BUILTIN))))


def run_python_candidate(cand, frames, meta=None):
    """Feed `frames` in source order with look-ahead buffering.  Returns (outputs, ms_per_frame list)."""
    cand.reset(meta or (frames[0].meta() if frames else {}))
    outs, ms = [], []
    n = len(frames)
    for i, fr in enumerate(frames):
        fut = tuple(frames[i + 1:i + 1 + cand.lookahead]) if cand.lookahead else ()
        t0 = time.perf_counter()
        o = cand.process(fr, fut)
        ms.append((time.perf_counter() - t0) * 1000.0)
        o.index = fr.index
        outs.append(o)
    return outs, ms
