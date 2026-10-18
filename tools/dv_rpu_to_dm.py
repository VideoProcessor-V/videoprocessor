#!/usr/bin/env python3
# Copyright(C) 2026 Bill Slack
# SPDX-License-Identifier: GPL-3.0-only
"""Real RPU metadata -> per-frame DM field lines (DV autotest Tier 2, 2026-10-17).

Input: dovi_tool's full export (``dovi_tool export -i X.bin -d all=X.json``), i.e.
libdovi's parse of a real RPU. Output: one line per frame with the vdr_dm_data
fields VP's tunnel DM carries, read by DvRpuAnchorTests, which serialises them as
an HDMI tunnel DM (ETSI GS CCM 001 Table 3/4/5), sends them through the loop and
checks VP's decode against these (dovi_tool's) values.

Line format (space separated key=value; repeated keys for repeated blocks):
  frame=N ids=affected,current refresh=R ycc=9 values off=3 values lms=9 values
  eotf=E params=p0,p1,p2 depth=D cs=C chroma=F full=R minpq=A maxpq=B diag=G
  L1=min,max,avg  L2=target,slope,offset,power,chroma,sat,ms  L3=min,max,avg
  L4=anchor_pq,anchor_power  L5=l,r,t,b  L6=maxm,minm,cll,fall
  L8=index,slope,offset,power,chroma,sat,ms[,mid,clip[,6 sat[,6 hue]]]
  L9=index[,8 primaries]  L10=index,maxpq,minpq,prim_index[,8 primaries]
  L11=type,whitepoint,reference,byte2,byte3  L254=dm_mode,dm_version

Usage: python dv_rpu_to_dm.py X.json [--frames N] > X.dm.txt
"""
import json
import sys


def blocks(dm):
    for group in ("cmv29_metadata", "cmv40_metadata"):
        meta = dm.get(group) or {}
        for block in meta.get("ext_metadata_blocks", []) or []:
            for name, fields in block.items():
                yield name, fields


def line_for(index, frame):
    dm = frame["vdr_dm_data"]
    out = ["frame=%d" % index,
           "ids=%d,%d" % (dm["affected_dm_metadata_id"], dm["current_dm_metadata_id"]),
           "refresh=%d" % dm["scene_refresh_flag"],
           "ycc=" + ",".join(str(dm["ycc_to_rgb_coef%d" % k]) for k in range(9)),
           "off=" + ",".join(str(dm["ycc_to_rgb_offset%d" % k]) for k in range(3)),
           "lms=" + ",".join(str(dm["rgb_to_lms_coef%d" % k]) for k in range(9)),
           "eotf=%d" % dm["signal_eotf"],
           "params=%d,%d,%d" % (dm["signal_eotf_param0"], dm["signal_eotf_param1"], dm["signal_eotf_param2"]),
           "depth=%d" % dm["signal_bit_depth"], "cs=%d" % dm["signal_color_space"],
           "chroma=%d" % dm["signal_chroma_format"], "full=%d" % dm["signal_full_range_flag"],
           "minpq=%d" % dm["source_min_pq"], "maxpq=%d" % dm["source_max_pq"], "diag=%d" % dm["source_diagonal"]]
    for name, f in blocks(dm):
        if name == "Level1":
            out.append("L1=%d,%d,%d" % (f["min_pq"], f["max_pq"], f["avg_pq"]))
        elif name == "Level2":
            out.append("L2=%d,%d,%d,%d,%d,%d,%d" % (f["target_max_pq"], f["trim_slope"], f["trim_offset"], f["trim_power"],
                                                    f["trim_chroma_weight"], f["trim_saturation_gain"], f["ms_weight"]))
        elif name == "Level3":
            out.append("L3=%d,%d,%d" % (f["min_pq_offset"], f["max_pq_offset"], f["avg_pq_offset"]))
        elif name == "Level4":
            out.append("L4=%d,%d" % (f["anchor_pq"], f["anchor_power"]))
        elif name == "Level5":
            out.append("L5=%d,%d,%d,%d" % (f["active_area_left_offset"], f["active_area_right_offset"],
                                           f["active_area_top_offset"], f["active_area_bottom_offset"]))
        elif name == "Level6":
            out.append("L6=%d,%d,%d,%d" % (f["max_display_mastering_luminance"], f["min_display_mastering_luminance"],
                                           f["max_content_light_level"], f["max_frame_average_light_level"]))
        elif name == "Level8":
            v = [f["target_display_index"], f["trim_slope"], f["trim_offset"], f["trim_power"], f["trim_chroma_weight"],
                 f["trim_saturation_gain"], f["ms_weight"]]
            if "target_mid_contrast" in f:
                v += [f["target_mid_contrast"], f.get("clip_trim", 2048)]
                if "saturation_vector_field0" in f:
                    v += [f["saturation_vector_field%d" % k] for k in range(6)]
                    if "hue_vector_field0" in f:
                        v += [f["hue_vector_field%d" % k] for k in range(6)]
            out.append("L8=" + ",".join(str(x) for x in v))
        elif name == "Level9":
            v = [f["source_primary_index"]]
            if "source_primary_red_x" in f:
                v += [f["source_primary_%s_%s" % (c, a)] for c in ("red", "green", "blue", "white") for a in ("x", "y")]
            out.append("L9=" + ",".join(str(x) for x in v))
        elif name == "Level10":
            v = [f["target_display_index"], f["target_max_pq"], f["target_min_pq"], f["target_primary_index"]]
            if "target_primary_red_x" in f:
                v += [f["target_primary_%s_%s" % (c, a)] for c in ("red", "green", "blue", "white") for a in ("x", "y")]
            out.append("L10=" + ",".join(str(x) for x in v))
        elif name == "Level11":
            out.append("L11=%d,%d,%d,%d,%d" % (f["content_type"], f["whitepoint"], 1 if f["reference_mode_flag"] else 0,
                                               f.get("reserved_byte2", 0), f.get("reserved_byte3", 0)))
        elif name == "Level254":
            out.append("L254=%d,%d" % (f["dm_mode"], f["dm_version_index"]))
    return " ".join(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    frames = None
    if "--frames" in sys.argv:
        frames = int(sys.argv[sys.argv.index("--frames") + 1])
    with open(sys.argv[1], encoding="utf-8") as f:
        data = json.load(f)
    for index, frame in enumerate(data):
        if frames is not None and index >= frames:
            break
        if not frame.get("vdr_dm_data"):
            continue
        print(line_for(index, frame))
    return 0


if __name__ == "__main__":
    sys.exit(main())
