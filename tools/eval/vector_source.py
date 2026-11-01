# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""External motion-vector fields for the temporal Python candidates (MC-LTM in ltm_eval.py, chroma_mv /
chroma_mv_cfl in candidates_chroma.py), so they can be re-run on the camera model's vectors instead of
the OpenCV DIS stand-in.

File contract (one dense field per ordered frame pair, luma-pixel units):
  <dir>/flow_<a>_<b>.npy   float32 / float64 array (H, W, 2): channel 0 = dx, channel 1 = dy, such that
                           frame_b(p + F(p)) ~ frame_a(p)  (the DIS convention used by the candidates)
  a, b = 6-digit zero-padded frame indices: the SOURCE frame index (Frame.index) for the harness chroma
  candidates; the window-local index (0 = first decoded frame) for ltm_eval clips, under <dir>/<clip>/.
Vectors from a lower-resolution field are accepted if H, W divide the luma size: they are upsampled
(bilinear) and scaled by the size ratio.

No silent fallback: with a directory given, a missing pair raises (strict, the default); with
strict=False the pair falls back to DIS and is COUNTED (stats() reports external vs DIS pairs, which
the callers print).  Without a directory the DIS stand-in is used as before.
"""
import os

import numpy as np
from scipy import ndimage


class VectorSource:
    def __init__(self, directory=None, strict=True):
        self.dir = directory
        self.strict = strict
        self.n_ext = 0
        self.n_dis = 0

    @classmethod
    def from_env(cls):
        """VP_EVAL_VECTORS=<dir> selects external vectors; VP_EVAL_VECTORS_STRICT=0 allows counted DIS fallback."""
        d = os.environ.get("VP_EVAL_VECTORS") or None
        return cls(d, os.environ.get("VP_EVAL_VECTORS_STRICT", "1") != "0")

    def path(self, a, b, sub=None):
        base = os.path.join(self.dir, sub) if sub else self.dir
        return os.path.join(base, "flow_%06d_%06d.npy" % (int(a), int(b)))

    def flow(self, a, b, a8, b8, sub=None):
        """Dense flow a -> b (b(p + F(p)) ~ a(p)) at the luma size of a8."""
        if self.dir:
            p = self.path(a, b, sub)
            if os.path.exists(p):
                F = np.load(p).astype(np.float64)
                self.n_ext += 1
                return _fit(F, a8.shape)
            if self.strict:
                raise FileNotFoundError("external vectors missing: %s (set VP_EVAL_VECTORS_STRICT=0 to allow "
                                        "a counted DIS fallback)" % p)
        from candidates_chroma import dis_flow
        self.n_dis += 1
        return dis_flow(a8, b8)

    def stats(self):
        return dict(source=self.dir or "DIS stand-in", external_pairs=self.n_ext, dis_pairs=self.n_dis)


def _fit(F, shape):
    h, w = shape
    if F.shape[:2] == (h, w):
        return F
    fy, fx = h / F.shape[0], w / F.shape[1]
    if abs(fy - round(fy)) > 1e-9 or abs(fx - round(fx)) > 1e-9:
        raise ValueError("vector field %s does not divide the luma size %s" % (F.shape[:2], shape))
    up = np.stack([ndimage.zoom(F[..., k], (fy, fx), order=1, mode="nearest", grid_mode=True)[:h, :w]
                   for k in range(2)], axis=-1)
    up[..., 0] *= fx
    up[..., 1] *= fy
    return up
