# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Streaming-like encodes for the TemporalDetail real-world measurement (suite td_real).

The G18 corpus has 4 decoded frames per case: too short for temporal metrics, the I/B pulse or grain bands.
This regenerates >= 3 s windows from lossless masters with x265 at streaming bitrates, fixed IBBBP mini-GOP
(bframes 3, b-adapt 0, b-pyramid default), 2 s closed GOP, no scene-cut I-frames, 10-bit Main10, VBV-capped ABR,
for a 2160p rung (master raster) and a 1080p rung (master Lanczos-downscaled to 1920 wide; that downscale is the
rung's truth, as on a streaming ladder). Per case it writes, into --out (scratch, outside git; never committed):

  <id>.mp4                 the encode
  <id>_master.yuv          the truth window, central crop W x H, yuv420p10le
  <id>_coded.yuv           the decoded encode, the same crop (the harness crop applied up front: memory)
and appends the case (with `frame_types` from ffmpeg showinfo, display order) to the suite JSON.

  python -I tools/eval/make_td_encodes.py --out D:/codex/videoprocessor/eval-scratch/test-td-real/enc \
         --suite tools/eval/suites/td_real.json [--only ID]
"""
import argparse
import json
import os
import re
import subprocess
import sys

FFMPEG = os.environ.get("VP_FFMPEG", r"D:\tools\ffmpeg\bin\ffmpeg.exe")
CROP_W, CROP_H = 960, 544

# id, content type, master, start (s), seconds, source fps -> output fps, picture (w, h), kbps 2160 / 1080
MASTERS = [
    dict(id="tos", ct="film24", path=r"D:\nn-corpus\trimmed\tos4k\reference_ffv1.mkv", start=40.0, secs=3.0,
         fps=24.0, vf="", size=(3840, 1714), kbps=(12000, 4500),
         note="Tears of Steel 4K (live action + VFX, scope), FFV1 master"),
    dict(id="sol", ct="film24", path=r"D:\nn-corpus\trimmed\sollevante4k\reference_ffv1.mkv", start=40.0, secs=3.0,
         fps=24.0, vf="", size=(3840, 2160), kbps=(12000, 4500),
         note="Sol Levante (2D animation, 4:2:2 10-bit FFV1 master; chroma to 4:2:0)"),
    dict(id="boat", ct="nature30", path=r"D:\nn-corpus\trimmed\boat4k\reference_ffv1.mkv", start=0.5, secs=3.0,
         fps=30.0, vf="fps=30", size=(4096, 2160), kbps=(14000, 5000),
         note="boat 4K 60p FFV1 master decimated to 30p (nature30 PROXY)"),
    dict(id="crowd", ct="sports50_60", path=r"D:\hfr\crowd_run_2160p50.y4m", start=2.0, secs=3.0,
         fps=50.0, vf="", size=(3840, 2160), kbps=(16000, 6000),
         note="crowd_run 2160p50 y4m (sports PROXY: fast motion, dense texture)"),
]


# Letterboxed variant (active-picture check): the scope master padded with black bars to 16:9 before encoding.
LETTERBOX = [
    dict(id="toslb", ct="film24", path=r"D:\nn-corpus\trimmed\tos4k\reference_ffv1.mkv", start=40.0, secs=3.0,
         fps=24.0, vf="pad=3840:2160:0:222:black", size=(3840, 2160), pad_top=222, kbps=(12000, 4500),
         note="Tears of Steel 4K scope master LETTERBOXED to 3840x2160 (222-row black bars), crop at the top edge"),
]


# No-harm material (D:/testclips/eval-2026-10-31; already streaming encodes, NO masters): the reference is the
# input itself, so only the input-relative gates and the vs-input grain / ghost / flicker rows are meaningful.
TC = r"D:\testclips\eval-2026-10-31"
NOHARM = [
    dict(id="nh_35mm", ct="film24", rel=r"film_grain_24p\A_Married_Woman_Trailer_1964_35mm_Flat_Mono_UHD\A_Married_Woman_Trailer_1964_35mm_Flat_Mono_UHD.webm",
         start=30.0, fps=24.0, size=(3840, 2160), note="35mm trailer scan (heavy grain), VP9 4K"),
    dict(id="nh_16mm", ct="film24", rel=r"film_grain_24p\Kodak_16mm_film_nature_documentary_Nature_of_1808\Kodak_16mm_film_nature_documentary_Nature_of_1808.webm",
         start=60.0, fps=24.0, size=(2890, 2100), note="16mm nature documentary scan (very heavy grain), VP9"),
    dict(id="nh_dark", ct="dark", rel=r"dark_24p\mixkit_dancer-in-the-dark-back-view-1038\mixkit_dancer-in-the-dark-back-view-1038.mp4",
         start=5.0, fps=23.976, size=(1920, 1080), note="dark low-key 24p, H.264 1080p"),
    dict(id="nh_river", ct="nature30", rel=r"nature_30p\mixkit_raft-going-slowly-down-a-river-1218\mixkit_raft-going-slowly-down-a-river-1218.mp4",
         start=5.0, fps=29.97, size=(1920, 1080), note="nature 30p (water, foliage), H.264 1080p"),
    dict(id="nh_goal", ct="sports50_60", rel=r"sports_ball\2022_FIFA_World_Cup_s_first_goal_by_Enner_Va_1cf2\2022_FIFA_World_Cup_s_first_goal_by_Enner_Va_1cf2.webm",
         start=5.0, fps=25.0, size=(1920, 1080), note="football broadcast 25p (real ball sport), VP9 1080p"),
]


def make_noharm_case(m, out):
    n = int(round(3.0 * m["fps"]))
    w, h = m["size"]
    cexpr, cx, cy = crop_expr(w, h)
    f = os.path.join(out, m["id"] + "_input.yuv")
    r = run([FFMPEG, "-hide_banner", "-y", "-ss", "%.3f" % m["start"], "-i", os.path.join(TC, m["rel"]),
             "-frames:v", str(n), "-vf", cexpr + ",showinfo", "-pix_fmt", "yuv420p10le", "-f", "rawvideo", f])
    types = "".join(re.findall(r" type:([IPB?]) ", r.stderr))
    src = dict(kind="raw", width=CROP_W, height=CROP_H, pix_fmt="yuv420p10le", start=0, fps=m["fps"], path=f)
    return dict(id=m["id"], content_type=m["ct"], clip=m["id"], case="noharm_input_ref", protocol="same", frames=n,
                gop=None, minigop=None, frame_types=types if len(types) == n else None, grain_probe=True,
                note="NO MASTER: reference = input (no-harm checks only); %s; crop %dx%d at (%d,%d), %.1f s from %.1f s"
                     % (m["note"], CROP_W, CROP_H, cx, cy, n / m["fps"], m["start"]),
                master=dict(src), coded=dict(src))


# Bitrate tiers: std = the per-master streaming rates above (12-16 / 4-6 Mb/s, x265 default pbratio 1.3);
# low = streaming-typical lower rungs; vlow = lower still with a harsher B-frame QP offset (pbratio 1.6).
TIERS = {"std": dict(suffix="", kbps=None, pbratio=1.3),
         "low": dict(suffix="_low", kbps=(6000, 2500), pbratio=1.3),
         "vlow": dict(suffix="_vlow", kbps=(4000, 1500), pbratio=1.6)}


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError("command failed (%d): %s\n%s" % (r.returncode, " ".join(cmd), r.stderr[-2000:]))
    return r


def crop_expr(w, h, y=None):
    x = (w - CROP_W) // 2 // 4 * 4
    y = (h - CROP_H) // 2 // 4 * 4 if y is None else y
    return "crop=%d:%d:%d:%d" % (CROP_W, CROP_H, x, y), x, y


def make_case(m, rung, out, tier="std"):
    t = TIERS[tier]
    cid = "%s_%s%s" % (m["id"], rung, t["suffix"])
    n = int(round(m["secs"] * m["fps"]))
    kbps = t["kbps"][0 if rung == "2160p" else 1] if t["kbps"] else m["kbps"][0 if rung == "2160p" else 1]
    w, h = m["size"]
    chain = [m["vf"]] if m["vf"] else []
    if rung == "1080p":
        h = int(round(h * 1920 / w / 2)) * 2
        w = 1920
        chain.append("scale=%d:%d:flags=lanczos" % (w, h))
    chain.append("format=yuv420p10le")
    vf = ",".join(chain)
    src = ["-ss", "%.3f" % m["start"], "-i", m["path"], "-frames:v", str(n)]
    mp4 = os.path.join(out, cid + ".mp4")
    gop = int(round(2 * m["fps"]))
    xp = ("bitrate=%d:vbv-maxrate=%d:vbv-bufsize=%d:keyint=%d:min-keyint=%d:scenecut=0:open-gop=0:bframes=3:"
          "b-adapt=0:pbratio=%.2f:pools=4:frame-threads=2:log-level=error" % (kbps, int(kbps * 1.5), int(kbps * 2), gop,
                                                                          gop, t["pbratio"]))
    if not os.path.isfile(mp4):
        run([FFMPEG, "-hide_banner", "-y"] + src + ["-vf", vf, "-c:v", "libx265", "-preset", "medium",
                                                    "-x265-params", xp, "-an", mp4])
    lb = None
    if m.get("pad_top"):  # letterboxed: the crop starts at the top, so it holds the bar and the boundary
        lb = int(round(m["pad_top"] * h / m["size"][1]))
        cexpr, cx, cy = crop_expr(w, h, 0)
    else:
        cexpr, cx, cy = crop_expr(w, h)
    mfile, cfile = os.path.join(out, cid + "_master.yuv"), os.path.join(out, cid + "_coded.yuv")
    if not os.path.isfile(mfile):
        run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y"] + src +
            ["-vf", vf + "," + cexpr, "-pix_fmt", "yuv420p10le", "-f", "rawvideo", mfile])
    r = run([FFMPEG, "-hide_banner", "-y", "-i", mp4, "-vf", cexpr + ",showinfo", "-pix_fmt", "yuv420p10le",
             "-f", "rawvideo", cfile])
    types = "".join(re.findall(r" type:([IPB]) ", r.stderr))
    size = os.path.getsize(mp4)
    bits = size * 8.0 / (n / m["fps"]) / 1000.0
    if len(types) != n:
        raise RuntimeError("%s: %d frame types for %d frames" % (cid, len(types), n))
    base = dict(kind="raw", width=CROP_W, height=CROP_H, pix_fmt="yuv420p10le", start=0, fps=m["fps"])
    extra = dict(letterbox=dict(bar_rows=lb)) if lb else {}
    return dict(id=cid, content_type=m["ct"], clip=m["id"], case="x265_%s_%gk%s" % (rung, kbps / 1000.0, t["suffix"]), **extra,
                protocol="same", frames=n, gop=gop, minigop=4, frame_types=types, grain_probe=True,
                note="%s; %s rung %dx%d, crop %dx%d at (%d,%d); x265 medium ABR %d kb/s (measured %.0f kb/s), "
                     "IBBBP b-adapt 0, pbratio %.2f, keyint %d, scenecut 0" % (m["note"], rung, w, h, CROP_W, CROP_H, cx, cy, kbps,
                                                                 bits, t["pbratio"], gop),
                master=dict(base, path=mfile), coded=dict(base, path=cfile))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--suite", required=True)
    ap.add_argument("--only", default=None)
    ap.add_argument("--noharm", action="store_true", help="build the no-master no-harm suite instead")
    ap.add_argument("--letterbox", action="store_true", help="build the letterboxed suite instead")
    ap.add_argument("--tier", default="std", choices=sorted(TIERS), help="bitrate tier (std = per-master rates)")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    cases = []
    if a.noharm:
        for m in NOHARM:
            c = make_noharm_case(m, a.out)
            print("%s: %d frames" % (c["id"], c["frames"]), flush=True)
            cases.append(c)
        with open(a.suite, "w", encoding="utf8", newline="\n") as f:
            json.dump(dict(name="td_noharm", description="TemporalDetail no-harm checks on eval-2026-10-31 clips "
                           "(no masters: reference = input)", cases=cases), f, indent=1)
            f.write("\n")
        return 0
    for m in (LETTERBOX if a.letterbox else MASTERS):
        for rung in ("2160p", "1080p"):
            cid = "%s_%s%s" % (m["id"], rung, TIERS[a.tier]["suffix"])
            if a.only and a.only not in cid:
                continue
            c = make_case(m, rung, a.out, a.tier)
            print("%s: %d frames, types %s" % (cid, c["frames"], c["frame_types"][:24]), flush=True)
            cases.append(c)
    suite = dict(name=("td_letterbox" if a.letterbox else "td_real") + TIERS[a.tier]["suffix"], description="TemporalDetail on streaming-like x265 encodes (make_td_encodes.py); "
                 "masters and decodes are central %dx%d crops in scratch, never committed" % (CROP_W, CROP_H),
                 cases=cases)
    with open(a.suite, "w", encoding="utf8", newline="\n") as f:
        json.dump(suite, f, indent=1)
        f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
