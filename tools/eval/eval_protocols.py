# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Evaluation protocols: how (master, coded) frame pairs become (candidate input, truth).

chroma444   chroma reconstruction with a TRUE 4:4:4 reference.
            master 4:2:0 (W x H): luma is downscaled 2x with a fixed Lanczos-3 (siting-aligned, see
            below) to (W/2 x H/2); the master's native chroma plane is already (W/2 x H/2).  Luma-half +
            native chroma = genuine 4:4:4 truth at half size.
            input: the same operation on the CODED picture (coded luma downscaled 2x with the same
            filter; coded chroma is native (W/2 x H/2)) and then the chroma is subsampled once more to
            4:2:0 at the half raster with Lanczos-3 and LEFT (MPEG-2) siting: chroma sample j sits on
            luma column 2j, vertically between rows 2j and 2j+1.  The candidate must rebuild the
            (W/2 x H/2) chroma planes from the (W/4 x H/4) ones (+ luma).  If the coded picture is
            already half-size (a 1080p rung of a 4K master) its own 4:2:0 is the input as is.
same        same-geometry: candidate output vs master at the coded raster (repair, tone mapping,
            temporal candidates).  Master bit depth is scaled to the coded depth by exact powers of two.
"""
import numpy as np

import eval_io as eio
import eval_metrics as em

LUMA_DOWN_KERNEL = "lanczos3"
CHROMA_SUB_KERNEL = "lanczos3"


def _even_pad(p, h, w):
    ph, pw = h - p.shape[0], w - p.shape[1]
    return np.pad(p, ((0, ph), (0, pw)), mode="edge") if (ph or pw) else p


def _half_luma(frame):
    """Luma at half raster (chroma-raster of a 4:2:0 picture), co-sited with the chroma samples."""
    ch, cw = frame.Cb.shape
    y = _even_pad(frame.Y, 2 * ch, 2 * cw)
    sy, sx = eio.chroma_down_shifts(2, 2, frame.siting)
    return eio.resample_plane(y, ch, cw, LUMA_DOWN_KERNEL, sy, sx)


def chroma444_pair(master, coded):
    """-> (input_frame 4:2:0 at half raster, truth_frame 4:4:4 at half raster)."""
    master = eio.to_bitdepth(master, coded.bitdepth)
    truth = master.with_planes(Y=_half_luma(master), Cb=master.Cb, Cr=master.Cr)
    th, tw = truth.Y.shape
    if coded.Y.shape == master.Y.shape:
        yh = _half_luma(coded)
        cb, cr = coded.Cb, coded.Cr
        if cb.shape != (th, tw):
            raise ValueError("coded chroma raster %s != half-luma raster %s" % (cb.shape, (th, tw)))
    elif coded.Y.shape == (th, tw):
        # 1080p rung: the coded picture is itself the half-raster 4:2:0 input
        return coded.with_planes(), truth
    else:
        raise ValueError("coded raster %s fits neither the master %s nor its half" % (coded.Y.shape, master.Y.shape))
    sy, sx = eio.chroma_down_shifts(2, 2, coded.siting)
    h2, w2 = (th + 1) // 2, (tw + 1) // 2
    cb2 = eio.resample_plane(_even_pad(cb, 2 * h2, 2 * w2), h2, w2, CHROMA_SUB_KERNEL, sy, sx)
    cr2 = eio.resample_plane(_even_pad(cr, 2 * h2, 2 * w2), h2, w2, CHROMA_SUB_KERNEL, sy, sx)
    inp = coded.with_planes(Y=yh, Cb=cb2, Cr=cr2)
    return inp, truth


def same_pair(master, coded):
    if master.Y.shape != coded.Y.shape:
        raise ValueError("same-geometry protocol: master %s vs coded %s" % (master.Y.shape, coded.Y.shape))
    return coded, eio.to_bitdepth(master, coded.bitdepth)


PROTOCOLS = {"chroma444": chroma444_pair, "same": same_pair}


def verify_alignment(coded_frame, master_frames, first_offset):
    """PSNR of the coded luma against each master frame (consecutive from `first_offset`).
    Returns dict(offsets, psnr, best_offset): the encode frame belongs to the master frame with the
    PSNR peak (a clear, single peak = aligned)."""
    cy = coded_frame.Y
    ps = []
    for m in master_frames:
        m = eio.to_bitdepth(m, coded_frame.bitdepth)
        ps.append(em.psnr(m.Y, cy, coded_frame.bitdepth))
    best = int(np.argmax(np.array(ps)))
    return dict(offsets=list(range(first_offset, first_offset + len(master_frames))),
                psnr=[float(x) for x in ps], best_offset=first_offset + best)
