# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic material.

generate_sports_clip(spec)  HOOK ONLY (not implemented): the planned ball/puck sports generator.
tiny_clip(...)              TEST-ONLY deterministic clip (moving disc on a noisy background); used by
                            the unit tests, never as evaluation material.
"""
import numpy as np

import eval_io as eio

SPORTS_SPEC_FIELDS = {
    "width/height/fps": "raster and frame rate; every other quantity is derived from them",
    "object_diameter_frac": "ball/puck diameter as a fraction of picture height (size in px = frac * height)",
    "object_speed_frac_per_s": "object speed in picture heights per second (px/frame = speed * height / fps)",
    "shutter_deg": "camera shutter angle; motion blur length = speed_px_per_frame * shutter_deg / 360",
    "background_pan_frac_per_s": "global camera pan in picture widths per second (textured background)",
    "background_texture": "texture class (turf-like / court-like / crowd-like) with seeded noise",
    "grain_sigma_codes": "film/sensor grain std in 8-bit codes, seeded, temporally independent",
    "occlusion": "optional occluder crossing the object path (tests occlusion x confidence blending)",
    "seed": "RNG seed (required, determinism)",
    "frames": "number of frames",
}


def generate_sports_clip(spec):
    raise NotImplementedError("synthetic sports generator is not written yet; spec fields: %s"
                              % ", ".join(sorted(SPORTS_SPEC_FIELDS)))


def tiny_clip(n=8, w=96, h=64, seed=1234, grain=3.0, bitdepth=8, speed=3.0, radius=9, still_noise=False,
              fps=24.0):
    """TEST-ONLY.  Moving bright disc on a smooth gradient background with Gaussian grain.
    Returns a list of Frame (4:2:0, limited range).  `still_noise=True` repeats one noise field."""
    rng = np.random.default_rng(seed)
    s = 2.0 ** (bitdepth - 8)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    bg = (80.0 + 60.0 * xx / w + 20.0 * yy / h) * s
    base_noise = rng.standard_normal((h, w))
    frames = []
    for t in range(n):
        noise = base_noise if still_noise else rng.standard_normal((h, w))
        cx = radius + 2 + (speed * t) % (w - 2 * radius - 4)
        cy = h / 2
        disc = ((xx - cx) ** 2 + (yy - cy) ** 2 <= radius ** 2)
        y = np.where(disc, 200.0 * s, bg) + grain * s * noise
        y = np.clip(np.rint(y), 16 * s, 235 * s)
        cb = np.full((h // 2, w // 2), 110.0 * s) + np.where(disc[::2, ::2], 20.0 * s, 0.0)
        cr = np.full((h // 2, w // 2), 140.0 * s) - np.where(disc[::2, ::2], 15.0 * s, 0.0)
        frames.append(eio.Frame(y, cb, cr, bitdepth=bitdepth, fps=fps, index=t))
    return frames
