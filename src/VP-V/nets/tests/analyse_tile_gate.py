"""Would a tile gate (skip flat tiles) work for D? Measures, on a real frame, what skipping costs.

Why (efficiency batch, coordinator 2026-10-24): A's gate copies closed tiles bit-exactly; D has no
gate. A D gate would skip 16x16 output tiles the net barely touches. Skipping keeps the hybrid
scaler's output there, so it cannot ADD anything to grain. What it can do is drop D's correction in
those tiles. This script measures, per threshold on the tile's local 3x3 range (D's own bound
input: bound = c0 + c1 * range codes), the tiles skipped, the largest correction lost, and how
much high-frequency (grain-band) energy D itself changes in those tiles.

Read-only on the kit. Input: a luma frame as raw little-endian uint16 (gray16le), e.g.
  ffmpeg -i reference_ffv1.mkv -frames:v 1 -vf format=gray16le -f rawvideo frame.y16
Run with the kit's Python:
  python -I -B analyse_tile_gate.py --kit <kit> --staging <staging> --frame frame.y16 --size 3840x2160 --out-size 2560x1440
"""
import argparse
import os
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kit", required=True)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--frame", required=True)
    ap.add_argument("--size", default="3840x2160")
    ap.add_argument("--out-size", default="2560x1440")
    ap.add_argument("--net", default="D_hybrid_refine")
    a = ap.parse_args()
    sys.path.insert(0, a.kit)
    import torch
    import torch.nn.functional as F
    from trainkit import common as C
    from trainkit import models as Mo
    torch.set_grad_enabled(False)

    w, h = (int(v) for v in a.size.split("x"))
    ow, oh = (int(v) for v in a.out_size.split("x"))
    y = np.fromfile(a.frame, "<u2", count=w * h).reshape(h, w).astype(np.float32) / 65535.0
    src = torch.from_numpy(y)[None, None]
    out = F.interpolate(src, size=(oh, ow), mode="area")   # a stand-in for the hybrid scaler's output
    d = torch.load(os.path.join(a.staging, "weights", a.net + ".pt"), map_location="cpu", weights_only=False)
    net = Mo.build(d["spec"])
    net.load_state_dict(d["model"])
    net.eval()
    grid = Mo.output_grid((h, w), (oh, ow), (0, 0, ow, oh))
    bound = C.scale_bound(out, d["config"]["scale_bound"])
    corr, _ = net(out, bound, src if net.two_input else None, grid if net.two_input else None)
    corr = corr[0, 0].numpy() / C.CODE
    o = out[0, 0].numpy()
    pad = np.pad(o, 1, mode="reflect")
    win = np.stack([pad[dy:dy + oh, dx:dx + ow] for dy in range(3) for dx in range(3)])
    rng = (win.max(0) - win.min(0)) / C.CODE
    hp = lambda img: img - F.avg_pool2d(F.pad(torch.from_numpy(img)[None, None], (1, 1, 1, 1), mode="reflect"), 3, 1)[0, 0].numpy()
    hp_before = hp(o) / C.CODE
    hp_after = hp(o + corr * C.CODE) / C.CODE
    T = 16
    th, tw = oh // T, ow // T
    tile = lambda m: m[: th * T, : tw * T].reshape(th, T, tw, T)
    tile_rng = tile(rng).max((1, 3))
    tile_corr = np.abs(tile(corr)).max((1, 3))
    hp_rms_b = np.sqrt((tile(hp_before) ** 2).mean((1, 3)))
    hp_rms_a = np.sqrt((tile(hp_after) ** 2).mean((1, 3)))
    print(f"{a.net} on {os.path.basename(a.frame)}: {th * tw} tiles of {T}x{T}; max |corr| {np.abs(corr).max():.3f} codes")
    for thr in (1, 2, 4, 8, 16):
        sel = tile_rng < thr
        n = int(sel.sum())
        lost = float(tile_corr[sel].max()) if n else 0.0
        change = float(np.abs(hp_rms_a[sel] / np.maximum(hp_rms_b[sel], 1e-9) - 1).max()) if n else 0.0
        print(f"  skip tiles with max 3x3 range < {thr:2d} codes: {n / sel.size * 100:5.1f}% of tiles; largest correction lost "
              f"{lost:.3f} codes; D's own grain-band RMS change there up to {change * 100:.2f}%")


if __name__ == "__main__":
    main()
