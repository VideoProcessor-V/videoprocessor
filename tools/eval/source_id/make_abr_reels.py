# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""ABR reel manifest for the source-identity evaluation (doc 61 section 2.5).

Reads the streaming suites (tools/eval/suites/streaming_{hevc,hevc_uhd,av1}.json) and writes <out>/reels.tsv, <out>/truth.tsv and
<out>/shots.tsv. A reel is a list of SEGMENTS of the suite's decoded rung files ("<clip>_<preset>_<rung>_coded.mkv": the same content
timeline at different rungs, lower rungs already bicubic-upscaled to the top raster like a player does), spliced at IDR points of the
top-rung encode, so a splice is a rendition switch at a segment boundary with no content change. Nothing is re-encoded and no video is
copied: the C++ evaluation (SourceIdStreamingEvalTests, VP_SOURCE_ID_EVAL_DIR) decodes the segments itself.

Reel kinds (kind column):
  control        one rung for the whole clip (no switch)           truth: none
  down_res       top rung -> a lower-resolution rung at an IDR      truth: one MAJOR drop
  up_res         lower-resolution rung -> top rung                  truth: one MAJOR rise
  down_q         top rung -> a same-raster rung at <= half rate     truth: one MAJOR drop
  up_q           the reverse                                        truth: one MAJOR rise
  down_minor     top rung -> the next rung (< 1.6x rate step)       truth: one MINOR drop (reported separately)
  dip            top -> low-resolution for a few seconds -> top     truth: MAJOR drop then MAJOR rise
  congestion     the suite's own switch_congestion case             truth: its rung_per_frame changes (class by the rungs)
Truth is "frame:direction:class" with the frame index in the REEL's own timeline; the evaluation scores events by time.

  python -I tools/eval/source_id/make_abr_reels.py --out D:/codex/videoprocessor/eval-scratch/source-id
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EVAL = os.path.dirname(HERE)
sys.path.insert(0, EVAL)

import streaming_presets as sp  # noqa: E402

SUITES = ("streaming_hevc.json", "streaming_hevc_uhd.json", "streaming_av1.json")
MAJOR_RATE = 1.6   # a same-raster rate step of at least this factor is MAJOR


def rung_info(name):
    """('1080p_6000') -> (height, kbps)."""
    m = re.match(r"(\d+)p_(\d+)", name)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def load_cases(suite_dir):
    cases = []
    for s in SUITES:
        path = os.path.join(suite_dir, s)
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                cases.extend(json.load(f)["cases"])
    return cases


def nearest_idr(idrs, frame, lo=1):
    cand = [i for i in idrs if i >= lo]
    return min(cand, key=lambda i: abs(i - frame)) if cand else None


def clean(text):
    return str(text).replace("\t", " ").replace("\n", " ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--suites", default=os.path.join(EVAL, "suites"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    cases = load_cases(args.suites)

    groups = {}   # (clip, preset, ladder, mode) -> {rung: case}; congestion cases kept apart
    congestion = []
    for c in cases:
        st = c.get("streaming", {})
        if st.get("plan") == "congestion":
            congestion.append(c)
            continue
        if "rung" not in st:
            continue
        groups.setdefault((st["source"], st["preset"], st.get("ladder", ""), st.get("mode", "")), {})[st["rung"]] = c

    reels, truth, shots = [], [], []

    def add(rid, kind, fps, w, h, segments, tr, note=""):
        reels.append((rid, kind, fps, w, h, ";".join("%s@%d-%d" % s for s in segments), note))
        for fr, d, cl in tr:
            truth.append((rid, fr, d, cl))

    for (clip, preset, ladder, mode), by_rung in sorted(groups.items()):
        order = [r[0] for r in sp.LADDERS[ladder] if r[0] in by_rung]
        if not order:
            continue
        top = by_rung[order[0]]
        n = top["frames"]
        fps = top["coded"]["fps"]
        w, h = top["coded"]["width"], top["coded"]["height"]
        idrs = [i for i, ch in enumerate(top.get("frame_types", "")) if ch == "I"]
        base = "%s_%s%s" % (clip, preset, "_cbr" if mode == "cbr" else "")

        def seg(rung, a, b):
            return (by_rung[rung]["coded"]["path"], a, b)

        for rung in order:
            rid = "%s_const_%s" % (base, rung)
            add(rid, "control", fps, w, h, [seg(rung, 0, n)], [], "constant rung %s" % rung)
            for s in by_rung[rung].get("streaming", {}).get("shots", []):
                shots.append((rid, s["start"], s["frames"], s["kbps"], s["complexity_kbps"], s["mean_luma"]))

        th, tk = rung_info(order[0])
        low_res = next((r for r in order[1:] if rung_info(r)[0] != th), None)
        mid = next((r for r in order[1:] if rung_info(r)[0] == th and tk / max(1, rung_info(r)[1]) >= MAJOR_RATE), None)
        minor = next((r for r in order[1:] if rung_info(r)[0] == th and tk / max(1, rung_info(r)[1]) < MAJOR_RATE), None)
        b = nearest_idr(idrs, n // 2)
        if b is None or b < int(fps * 1.5) or n - b < int(fps * 1.5):
            b = n // 2   # no usable IDR near the middle: a mid-clip switch (a stream cut at an arbitrary frame)
        pairs = [("down_res", order[0], low_res, "drop", "major"), ("up_res", low_res, order[0], "rise", "major"),
                 ("down_q", order[0], mid, "drop", "major"), ("up_q", mid, order[0], "rise", "major"),
                 ("down_minor", order[0], minor, "drop", "minor")]
        for kind, a, bb, d, cl in pairs:
            if a is None or bb is None:
                continue
            add("%s_%s_%s_to_%s" % (base, kind, a, bb), kind, fps, w, h, [seg(a, 0, b), seg(bb, b, n)], [(b, d, cl)])
        if low_res is not None:
            b1 = nearest_idr(idrs, n // 3, lo=int(fps))
            b2 = nearest_idr(idrs, 2 * n // 3, lo=int(fps))
            if b1 is None or b2 is None or b2 - b1 < int(fps * 1.5):
                b1, b2 = n // 3, 2 * n // 3
            add("%s_dip_%s" % (base, low_res), "dip", fps, w, h,
                [seg(order[0], 0, b1), seg(low_res, b1, b2), seg(order[0], b2, n)], [(b1, "drop", "major"), (b2, "rise", "major")])

    for c in congestion:
        st = c["streaming"]
        rpf = st["rung_per_frame"]
        names = [r[0] for r in sp.LADDERS[st["ladder"]]]
        tr = []
        for i in range(1, len(rpf)):
            if rpf[i] != rpf[i - 1]:
                ha, ka = rung_info(names[rpf[i - 1]])
                hb, kb = rung_info(names[rpf[i]])
                d = "drop" if rpf[i] > rpf[i - 1] else "rise"
                hi, lo = max(ka, kb), max(1, min(ka, kb))
                cl = "major" if ha != hb or hi / lo >= MAJOR_RATE else "minor"
                tr.append((i, d, cl))
        add(c["id"], "congestion", c["coded"]["fps"], c["coded"]["width"], c["coded"]["height"],
            [(c["coded"]["path"], 0, c["frames"])], tr, "suite congestion plan %s" % st.get("plan"))

    with open(os.path.join(args.out, "reels.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("id\tkind\tfps\twidth\theight\tsegments\tnote\n")
        for r in reels:
            f.write("\t".join(clean(x) for x in r) + "\n")
    with open(os.path.join(args.out, "truth.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("id\tframe\tdirection\tclass\n")
        for r in truth:
            f.write("\t".join(clean(x) for x in r) + "\n")
    with open(os.path.join(args.out, "shots.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("id\tstart\tframes\tkbps\tcomplexity_kbps\tmean_luma\n")
        for r in shots:
            f.write("\t".join(clean(x) for x in r) + "\n")
    print("reels %d  truth events %d  shots %d  -> %s" % (len(reels), len(truth), len(shots), args.out))


if __name__ == "__main__":
    main()
