#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Exports real Dolby Vision profile 8.1 HEVC test files with authored RPUs (test/dv-synthetic,
2026-10-24), for the later hardware step: the Apple TV's certified Dolby engine (LLDV) as the
reference for dv_trims (docs/dv/DV-trims-certified-reference.md).

  python -I export_hevc.py <out-folder> --ffmpeg <ffmpeg.exe> --dovi-tool <dovi_tool.exe> [--frames-per-scene 48]

Software only: libx265 (no hardware encoders), dovi_tool generate + inject-rpu. Writes:
  dv-synth-p81.hevc       the BT.2020 PQ 4:2:0 10-bit base layer with one RPU per frame
  generator.json          the dovi_tool generator config (shots: L1 + L2 trims per scene, L6)
  manifest.json           per scene: frames, picture gain, L1, the L2 trims (codes and decoded),
                          and every patch's rectangle and source light (BT.2020 nits)
The pictures are the corpus's policy scenes (bright 4000-nit / mid 1000 / dim 200 masters of
the same patch layout), so a capture of the Apple TV's LLDV output and VP's tunnel output can
be measured patch by patch with run_dv_synth.py's tools.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dvsynth as dv  # noqa: E402
import dm_model  # noqa: E402

PQ = dv.nits_to_pq_code

# The colourist trims (the 'real' policy set of make_corpus.py): 12-bit codes, 2048 = neutral.
TRIMS = [  # target nits, slope, offset, power, chroma_weight, saturation_gain, ms_weight
    (100, 2048 + 160, 2048 + 40, 2048 - 200, 2048 - 120, 2048 - 160, 2048),
    (600, 2048 + 60, 2048 + 10, 2048 - 80, 2048 - 40, 2048 - 60, 2048),
    (1000, 2048 + 30, 2048, 2048 - 40, 2048 - 20, 2048 - 30, 2048),
]
SCENES = [("bright", 4.0, (0, PQ(4000), PQ(80))), ("mid", 1.0, (0, PQ(1000), PQ(25))), ("dim", 0.2, (0, PQ(200), PQ(5)))]


def ycbcr420_pq(rgb2020_nits):
    """Linear BT.2020 nits -> limited-range 10-bit Y, Cb, Cr (4:2:0) planes (BT.2020 NCL, PQ)."""
    rp = dv.nits_to_pq(np.clip(rgb2020_nits, 0.0, 10000.0))
    kr, kb = 0.2627, 0.0593
    y = kr * rp[..., 0] + (1 - kr - kb) * rp[..., 1] + kb * rp[..., 2]
    cb = (rp[..., 2] - y) / (2 * (1 - kb))
    cr = (rp[..., 0] - y) / (2 * (1 - kr))
    Y = np.clip(np.rint(64 + 876 * y), 4, 1019).astype("<u2")
    sub = lambda c: 0.25 * (c[0::2, 0::2] + c[1::2, 0::2] + c[0::2, 1::2] + c[1::2, 1::2])
    Cb = np.clip(np.rint(512 + 896 * sub(cb)), 4, 1019).astype("<u2")
    Cr = np.clip(np.rint(512 + 896 * sub(cr)), 4, 1019).astype("<u2")
    return Y, Cb, Cr


def l2_block(t):
    target, slope, offset, power, cw, sat, ms = t
    return {"Level2": {"target_max_pq": PQ(target), "trim_slope": slope, "trim_offset": offset, "trim_power": power,
                       "trim_chroma_weight": cw, "trim_saturation_gain": sat, "ms_weight": ms}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--ffmpeg", required=True)
    ap.add_argument("--dovi-tool", required=True)
    ap.add_argument("--frames-per-scene", type=int, default=48)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    w, h, n = a.width, a.height, a.frames_per_scene
    rep = dv.REP_P5
    yuv_path = os.path.join(a.out, "base.yuv")
    manifest = {"schema": "vp-dv-synthetic-hevc-1", "profile": "8.1", "width": w, "height": h, "rate": [24000, 1001],
                "base_layer": "HEVC Main10 4:2:0, BT.2020 NCL, PQ (SMPTE ST 2084), limited range", "scenes": [],
                "trims": [{"target_nits": t[0], "codes": list(t[1:]), "decoded": dm_model.decode_trim([PQ(t[0])] + list(t[1:]))}
                          for t in TRIMS],
                "note": "Synthetic metadata tests mechanics, not colourist intent; L8 is not authored (CM v2.9 L2 only)."}
    shots = []
    with open(yuv_path, "wb") as f:
        for k, (label, gain, l1) in enumerate(SCENES):
            I, C, patches = dv.build_picture(w, h, rep, 0, k, "scene%g" % gain)
            P = C[:, 0::2].astype(np.float64).repeat(2, axis=1)
            T = C[:, 1::2].astype(np.float64).repeat(2, axis=1)
            rgb = rep.decode(np.stack([I.astype(np.float64), P, T], axis=-1))
            Y, Cb, Cr = ycbcr420_pq(rgb)
            frame = Y.tobytes() + Cb.tobytes() + Cr.tobytes()
            for _ in range(n):
                f.write(frame)
            shots.append({"start": k * n, "duration": n,
                          "metadata_blocks": [{"Level1": {"min_pq": l1[0], "max_pq": l1[1], "avg_pq": l1[2]}}] + [l2_block(t) for t in TRIMS]})
            manifest["scenes"].append({
                "label": label, "first_frame": k * n, "frames": n, "gain": gain,
                "l1": {"min_pq": l1[0], "max_pq": l1[1], "avg_pq": l1[2]},
                "patches": [{"name": p["name"], "kind": p["kind"], "rect": p["rect"],
                             "source_rgb2020_nits": [float(v) for v in rep.decode(np.array(p["codes"], dtype=np.float64))]}
                            for p in patches if "codes" in p]})
    gen = {"cm_version": "V29", "profile": "8.1", "length": n * len(SCENES),
           "level6": {"max_display_mastering_luminance": 4000, "min_display_mastering_luminance": 50,
                      "max_content_light_level": 4000, "max_frame_average_light_level": 400},
           "source_min_pq": 62, "source_max_pq": PQ(4000), "shots": shots}
    gen_path = os.path.join(a.out, "generator.json")
    with open(gen_path, "w", newline="\n") as f:
        json.dump(gen, f, indent=1)
    base = os.path.join(a.out, "base.hevc")
    rpu = os.path.join(a.out, "rpu.bin")
    dvh = os.path.join(a.out, "dv-synth-p81.hevc")
    x265 = ("crf=6:hdr10=1:hdr10-opt=0:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
            "master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(40000000,50):max-cll=4000,400:"
            "keyint=48:min-keyint=48:aud=1")
    subprocess.run([a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p10le",
                    "-s", "%dx%d" % (w, h), "-r", "24000/1001", "-i", yuv_path, "-c:v", "libx265", "-preset", "medium",
                    "-x265-params", x265, "-f", "hevc", base], check=True)
    subprocess.run([a.dovi_tool, "generate", "-j", gen_path, "-o", rpu], check=True)
    subprocess.run([a.dovi_tool, "inject-rpu", "-i", base, "--rpu-in", rpu, "-o", dvh], check=True)
    # No container here: ffmpeg 7.x copies the RPUs but writes no DV configuration box (dvcC/dvvC),
    # so players may not engage Dolby Vision. Mux with MP4Box or mkvmerge (docs/dv/DV-trims-certified-reference.md).
    os.remove(yuv_path)
    with open(os.path.join(a.out, "manifest.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=1)
    print("wrote", dvh, "and manifest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
