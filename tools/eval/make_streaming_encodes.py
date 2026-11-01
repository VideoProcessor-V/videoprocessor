# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Streaming-realistic encode generator (suite streaming_*.json).

Mimics streaming practice from lossless masters: per-shot VBR under a capped average (shots found with
ffmpeg's scene score; each shot is an independent closed-GOP segment whose bitrate follows its complexity,
clamped so dark / grainy / fast shots are starved), an ABR ladder, and rung switches mid-clip (segments
from different rungs spliced at IDR points and up-scaled to the top raster like a player does).  Presets,
ladders and rules live in streaming_presets.py.  Deterministic: fixed threads, fixed rules, bitexact muxing.

Per case it writes, into --out (scratch, outside git, never committed; checks free space first):
  <clip>/<id>.mkv                the encode (concatenated shot segments)
  <clip>/<W>x<H>_master.mkv      FFV1 truth window (master scaled to the rung raster, centre crop)
  <clip>/<id>_coded.mkv          FFV1 decode of the encode, the same crop
and merges the case into the suite JSON (kind "ffmpeg" sources read by eval_harness.load_source).

  python -I tools/eval/make_streaming_encodes.py --out D:/codex/videoprocessor/eval-scratch/streaming-suite/enc \
      --suite tools/eval/suites/streaming_hevc.json --preset hevc_gop2 --ladder hd [--only tos] \
      [--rungs 0,1,2,3] [--switch congestion] [--uniform] [--crop 960x544|0]
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import streaming_presets as sp  # noqa: E402

MIN_FREE_GB = 15.0
CROP_DEFAULT = (960, 544)

# id, content type, path, start s, seconds, note.  Windows were chosen from ffmpeg scene scans (>= 3 cuts).
SOURCES = [
    dict(id="tos", ct="film24", path=r"D:\nn-corpus\trimmed\tos4k\reference_ffv1.mkv", start=18.5, secs=8.0,
         note="Tears of Steel 4K FFV1 master (live action + VFX, scope; dark interiors)"),
    dict(id="sintel", ct="film24", path=r"D:\nn-corpus\trimmed\sintel4k\reference_ffv1.mkv", start=70.5, secs=7.0,
         note="Sintel 4K FFV1 master (animation, fast cuts)"),
    dict(id="meridian", ct="dark", path=r"D:\nn-corpus\trimmed\meridian4k_existing\reference_ffv1.mkv", start=14.2,
         secs=5.0, note="Meridian 4K FFV1 master (dark low-key, 59.94p)"),
    dict(id="crowd", ct="sports50_60", path=r"D:\hfr\crowd_run_2160p50.y4m", start=1.0, secs=4.0,
         note="crowd_run 2160p50 (sports PROXY: fast motion, dense texture; single shot)"),
    dict(id="grain35", ct="film24", path=r"D:\testclips\eval-2026-10-31\film_grain_24p\A_Married_Woman_Trailer_1964_35mm_Flat_Mono_UHD\A_Married_Woman_Trailer_1964_35mm_Flat_Mono_UHD.webm",
         start=30.0, secs=8.0, note="35mm trailer scan (heavy grain; already a VP9 encode, so a PROXY master)"),
]


def find_ffmpeg():
    for c in (os.environ.get("VP_FFMPEG"), r"D:\tools\ffmpeg-full\bin\ffmpeg.exe"):
        if c and os.path.exists(c):
            return c
    return shutil.which("ffmpeg") or "ffmpeg"


FFMPEG = find_ffmpeg()
FFPROBE = os.path.join(os.path.dirname(FFMPEG), "ffprobe.exe") if os.path.exists(
    os.path.join(os.path.dirname(FFMPEG), "ffprobe.exe")) else (shutil.which("ffprobe") or "ffprobe")
SCALE = "scale=%d:%d:flags=lanczos+accurate_rnd+full_chroma_int"
SCALE_UP = "scale=%d:%d:flags=bicubic+accurate_rnd+full_chroma_int"


def run(cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf8", errors="replace")
    if check and r.returncode != 0:
        raise RuntimeError("command failed (%d): %s\n%s" % (r.returncode, " ".join(cmd), r.stderr[-1500:]))
    return r


def free_gb(path):
    return shutil.disk_usage(path).free / 1e9


def probe_video(path):
    r = run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
             "stream=width,height,r_frame_rate", "-of", "json", path])
    s = json.loads(r.stdout)["streams"][0]
    return s["width"], s["height"], Fraction(s["r_frame_rate"])


def ss_before(frame, fps):
    """-ss value half a frame before `frame` (frame-exact on intra-only windows)."""
    return "%.6f" % max((frame - 0.5) / float(fps), 0.0) if frame else "0"


def crop_for(w, h, crop):
    if not crop:
        return (0, 0, w // 4 * 4, h // 4 * 4)
    cw, ch = min(crop[0], w // 4 * 4), min(crop[1], h // 4 * 4)
    return ((w - cw) // 2 // 4 * 4, (h - ch) // 2 // 4 * 4, cw, ch)


def make_window(src, fps_out, out_dir):
    """Lossless 10-bit window master (native raster) so every later step is frame-exact and cheap."""
    win = os.path.join(out_dir, "window.mkv")
    if os.path.exists(win):
        return win
    run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-ss", "%.3f" % src["start"], "-i", src["path"],
         "-t", "%.3f" % src["secs"], "-an", "-sn", "-vf", "format=yuv420p10le", "-c:v", "ffv1", "-level", "3",
         "-slicecrc", "0", "-g", "1", "-fflags", "+bitexact", "-flags:v", "+bitexact", win])
    return win


def detect_shots(win, fps, n_frames, out_dir, thr=sp.VBR["scene_threshold"]):
    log = os.path.join(out_dir, "scene.log")
    if not os.path.exists(log):
        r = run([FFMPEG, "-hide_banner", "-nostats", "-i", win, "-an", "-vf",
                 "scale=384:-2:flags=fast_bilinear,select='gt(scene,%s)',showinfo" % thr, "-f", "null", "-"])
        with open(log, "w", encoding="utf8") as f:
            f.write(r.stderr)
    with open(log, "r", encoding="utf8") as f:
        cuts = sp.parse_scene_log(f.read(), float(fps))
    return sp.build_shots(cuts, n_frames, float(fps))


def count_frames(path):
    r = run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries",
             "stream=nb_read_packets", "-of", "csv=p=0", path])
    return int(r.stdout.strip().split()[0])


def probe_complexity(win, shots, fps, w, h, out_dir):
    """Per shot: complexity = x264 veryfast CRF kb/s at a fixed analysis width (shared by every rung) and the
    mean luma (8-bit units, 64-px-wide gray decode)."""
    pw = min(sp.VBR["probe_width"], w)
    ph = int(round(h * pw / w / 2.0)) * 2
    res = []
    for i, (s, n) in enumerate(shots):
        tmp = os.path.join(out_dir, "probe_%d.mkv" % i)
        run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-ss", ss_before(s, fps), "-i", win,
             "-frames:v", str(n), "-vf", (SCALE % (pw, ph)) + ",format=yuv420p", "-c:v", "libx264",
             "-preset", "veryfast", "-crf", str(sp.VBR["probe_crf"]), "-threads", "4", tmp])
        kb = os.path.getsize(tmp) * 8.0 / 1000.0 / (n / float(fps))
        os.remove(tmp)
        r = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-ss", ss_before(s, fps), "-i", win,
                            "-frames:v", str(n), "-vf", "scale=64:-2:flags=area,format=gray10le", "-f", "rawvideo", "-"],
                           capture_output=True)
        if r.returncode != 0:
            raise RuntimeError("luma probe failed: " + r.stderr.decode("utf8", "replace")[-400:])
        mean = float(np.frombuffer(r.stdout, dtype="<u2").mean()) / 4.0 if r.stdout else 128.0
        res.append((kb, mean))
    return res


def encode_rung(win, shots, rates, fps, rung, preset, out_path, tmp_dir):
    """Encode each shot as an independent closed-GOP segment at its rate and concatenate (stream copy)."""
    segs = []
    for i, ((s, n), kb) in enumerate(zip(shots, rates)):
        seg = os.path.join(tmp_dir, "seg_%d_%s.mkv" % (i, rung["name"]))
        run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-ss", ss_before(s, fps), "-i", win,
             "-frames:v", str(n), "-an", "-vf", (SCALE % (rung["w"], rung["h"])) + ",format=" + preset["pix_fmt"]]
            + sp.encoder_args(preset, kb, float(fps), n) + ["-fflags", "+bitexact", "-flags:v", "+bitexact", seg])
        segs.append(seg)
    lst = os.path.join(tmp_dir, "concat_%s.txt" % rung["name"])
    with open(lst, "w", encoding="utf8", newline="\n") as f:
        for sgm in segs:
            f.write("file '%s'\n" % sgm.replace("\\", "/"))
    run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy",
         "-fflags", "+bitexact", "-flags:v", "+bitexact", "-map_metadata", "-1", out_path])
    for sgm in segs:
        os.remove(sgm)
    os.remove(lst)


def stream_md5(path):
    r = run([FFMPEG, "-hide_banner", "-loglevel", "error", "-i", path, "-map", "0:v", "-c", "copy", "-f", "md5", "-"])
    return r.stdout.strip().replace("MD5=", "")


def write_ffv1(cmd_in, vf, out, extra_in=()):
    """Decode/convert to a cropped lossless 10-bit file; returns the showinfo stderr (frame types)."""
    r = run([FFMPEG, "-hide_banner", "-y"] + list(extra_in) + ["-i", cmd_in, "-an", "-vf", vf + ",showinfo",
            "-pix_fmt", "yuv420p10le", "-c:v", "ffv1", "-level", "3", "-slicecrc", "0", "-g", "1",
            "-fflags", "+bitexact", "-flags:v", "+bitexact", out])
    return r.stderr


def frame_types(stderr, n):
    t = "".join(re.findall(r" type:([IPB?]) ", stderr))
    return t if len(t) == n else None


def src_spec(path, w, h, fps, n):
    return dict(kind="ffmpeg", path=path.replace("\\", "/"), width=w, height=h, pix_fmt="yuv420p10le", start=0,
                fps=float(fps))


def build_clip(src, a, suite_cases, log=print):
    out_dir = os.path.join(a.out, src["id"])
    os.makedirs(out_dir, exist_ok=True)
    if free_gb(a.out) < MIN_FREE_GB:
        raise RuntimeError("free space on the output drive is under %.0f GB; stop and tell the coordinator" % MIN_FREE_GB)
    preset = sp.PRESETS[a.preset]
    w0, h0, fps = probe_video(src["path"])
    log("%s: source %dx%d @ %.3f fps" % (src["id"], w0, h0, float(fps)))
    win = make_window(src, fps, out_dir)
    n_frames = count_frames(win)
    shots = detect_shots(win, fps, n_frames, out_dir)
    gop = sp.gop_frames(preset["gop_s"], float(fps))
    log("  window %d frames, %d shots %s, GOP %d" % (n_frames, len(shots), shots, gop))
    probe = probe_complexity(win, shots, fps, w0, h0, out_dir)
    cplx = [p[0] for p in probe]
    dweights = [sp.dark_weight(p[1]) for p in probe]
    durs = [n / float(fps) for _, n in shots]
    rungs = sp.ladder_rungs(a.ladder, w0, h0, float(fps))
    want = [int(x) for x in a.rungs.split(",")] if a.rungs else list(range(len(rungs)))
    rungs = [r for r in rungs if r["index"] in want]
    if not rungs:
        log("  no rung fits this master; skipped")
        return
    all_rungs = sp.ladder_rungs(a.ladder, w0, h0, float(fps))
    top = all_rungs[0]
    decoded = {}  # rung name -> (coded path scaled to top raster) for the switch case
    for rung in rungs:
        cid = "%s_%s_%s%s" % (src["id"], a.preset, rung["name"], "_cbr" if a.uniform else "")
        enc = os.path.join(out_dir, cid + ".mkv")
        if a.uniform:
            rates = [rung["kbps"]] * len(shots)
        else:
            rates = sp.allocate_shot_kbps(durs, cplx, rung["kbps"], weights=dweights)
        if not os.path.exists(enc):
            encode_rung(win, shots, rates, fps, rung, preset, enc, out_dir)
        kbps = os.path.getsize(enc) * 8.0 / 1000.0 / (n_frames / float(fps))
        cx, cy, cw, ch = crop_for(rung["w"], rung["h"], a.crop)
        crop_vf = "crop=%d:%d:%d:%d" % (cw, ch, cx, cy)
        master = os.path.join(out_dir, "%dx%d_master.mkv" % (rung["w"], rung["h"]))
        if not os.path.exists(master):
            write_ffv1(win, (SCALE % (rung["w"], rung["h"])) + "," + crop_vf, master)
        coded = os.path.join(out_dir, cid + "_coded.mkv")
        types = frame_types(write_ffv1(enc, crop_vf, coded), n_frames)
        if count_frames(coded) != n_frames:
            raise RuntimeError("%s: decoded %d frames, expected %d" % (cid, count_frames(coded), n_frames))
        info = dict(preset=a.preset, ladder=a.ladder, rung=rung["name"], raster=[rung["w"], rung["h"]],
                    target_kbps=rung["kbps"], measured_kbps=round(kbps, 1), mode="cbr" if a.uniform else "shot_vbr",
                    shots=[dict(start=s, frames=n, complexity_kbps=round(c, 1), mean_luma=round(pr[1], 1), kbps=r)
                           for (s, n), c, pr, r in zip(shots, cplx, probe, rates)],
                    gop_frames=gop, stream_md5=stream_md5(enc), crop=[cx, cy, cw, ch], source=src["id"],
                    ffmpeg=run([FFMPEG, "-version"]).stdout.splitlines()[0])
        suite_cases[cid] = dict(
            id=cid, content_type=src["ct"], clip=src["id"], case="%s_%s%s" % (a.preset, rung["name"], "_cbr" if a.uniform else ""),
            protocol="same", frames=n_frames, gop=gop, minigop=preset.get("bframes", 3) + 1 if preset["kind"] == "x265" else None,
            frame_types=types, grain_probe=False,
            note="%s; %s %s rung %s %dx%d target %d kb/s measured %.0f kb/s, %s, crop %dx%d at (%d,%d)" % (
                src["note"], a.preset, "uniform bitrate" if a.uniform else "per-shot VBR", rung["name"], rung["w"],
                rung["h"], rung["kbps"], kbps, "%d shots" % len(shots), cw, ch, cx, cy),
            master=src_spec(master, cw, ch, fps, n_frames), coded=src_spec(coded, cw, ch, fps, n_frames),
            streaming=info)
        log("  %s: target %d kb/s measured %.0f kb/s  rates %s" % (cid, rung["kbps"], kbps, rates))
        # decoded and up-scaled to the top raster (what a player shows), cropped at the top raster's window
        if a.switch:
            tcx, tcy, tcw, tch = crop_for(top["w"], top["h"], a.crop)
            up = os.path.join(out_dir, cid + "_up.mkv")
            if not os.path.exists(up):
                vf = ((SCALE_UP % (top["w"], top["h"])) + ",") if (rung["w"], rung["h"]) != (top["w"], top["h"]) else ""
                write_ffv1(enc, vf + "crop=%d:%d:%d:%d" % (tcw, tch, tcx, tcy), up)
            decoded[rung["index"]] = up
    if a.switch:
        build_switch(src, a, preset, fps, shots, gop, n_frames, top, all_rungs, decoded, out_dir, suite_cases, log)
    for up in decoded.values():
        if os.path.exists(up):
            os.remove(up)
    if not a.keep_window and os.path.exists(win):
        os.remove(win)


def build_switch(src, a, preset, fps, shots, gop, n_frames, top, all_rungs, decoded, out_dir, suite_cases, log):
    """Splice the per-rung (top-raster) decodes at IDR points per the plan; truth = master at the top raster."""
    plan = sp.SWITCH_PLANS[a.switch]
    fmap = sp.frame_rung_map(shots, gop, plan, n_frames, len(all_rungs))
    missing = sorted(set(fmap) - set(decoded))
    if missing:
        log("  switch plan %s needs rungs %s (not generated); skipped" % (a.switch, missing))
        return
    cid = "%s_%s_switch_%s" % (src["id"], a.preset, a.switch)
    tcx, tcy, tcw, tch = crop_for(top["w"], top["h"], a.crop)
    out = os.path.join(out_dir, cid + "_coded.mkv")
    # build runs of consecutive frames from the same rung; select them with trim filters and concat
    runs, s = [], 0
    for f in range(1, n_frames + 1):
        if f == n_frames or fmap[f] != fmap[s]:
            runs.append((fmap[s], s, f))
            s = f
    inputs, parts = [], []
    for i, (ri, f0, f1) in enumerate(runs):
        inputs += ["-i", decoded[ri]]
        parts.append("[%d:v]trim=start_frame=%d:end_frame=%d,setpts=PTS-STARTPTS[v%d]" % (i, f0, f1, i))
    fc = ";".join(parts) + ";" + "".join("[v%d]" % i for i in range(len(runs))) + "concat=n=%d:v=1:a=0[o]" % len(runs)
    run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y"] + inputs + ["-filter_complex", fc, "-map", "[o]",
         "-pix_fmt", "yuv420p10le", "-c:v", "ffv1", "-level", "3", "-slicecrc", "0", "-g", "1",
         "-fflags", "+bitexact", "-flags:v", "+bitexact", out])
    master = os.path.join(out_dir, "%dx%d_master.mkv" % (top["w"], top["h"]))
    if count_frames(out) != n_frames:
        raise RuntimeError("%s: spliced %d frames, expected %d" % (cid, count_frames(out), n_frames))
    suite_cases[cid] = dict(
        id=cid, content_type=src["ct"], clip=src["id"], case="%s_switch_%s" % (a.preset, a.switch), protocol="same",
        frames=n_frames, gop=gop, minigop=None, frame_types=None, grain_probe=False,
        note="%s; %s rung switching (plan %s: rung per IDR segment %s), lower rungs bicubic-upscaled to %dx%d like a player; "
             "crop %dx%d at (%d,%d)" % (src["note"], a.preset, a.switch, plan, top["w"], top["h"], tcw, tch, tcx, tcy),
        master=src_spec(master, tcw, tch, fps, n_frames), coded=src_spec(out, tcw, tch, fps, n_frames),
        streaming=dict(preset=a.preset, ladder=a.ladder, plan=a.switch, rung_per_frame=fmap, source=src["id"],
                       crop=[tcx, tcy, tcw, tch]))
    log("  %s: %d runs %s" % (cid, len(runs), [(r[0], r[2] - r[1]) for r in runs]))


def merge_suite(path, name, description, cases):
    suite = dict(name=name, description=description, nr_metrics=True, cases=[])
    if os.path.exists(path):
        with open(path, "r", encoding="utf8") as f:
            suite = json.load(f)
    by = {c["id"]: c for c in suite["cases"]}
    by.update(cases)
    suite["cases"] = [by[k] for k in sorted(by)]
    suite["nr_metrics"] = True
    with open(path, "w", encoding="utf8", newline="\n") as f:
        json.dump(suite, f, indent=1, sort_keys=True)
        f.write("\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--suite", required=True, help="suite JSON to create/merge into")
    ap.add_argument("--preset", default="hevc_gop2", choices=sorted(sp.PRESETS))
    ap.add_argument("--ladder", default="hd", choices=sorted(sp.LADDERS))
    ap.add_argument("--only", default=None, help="comma list of source ids")
    ap.add_argument("--rungs", default=None, help="comma list of rung indices (0 = top)")
    ap.add_argument("--switch", default=None, choices=sorted(sp.SWITCH_PLANS))
    ap.add_argument("--uniform", action="store_true", help="control: same bitrate for every shot (fixed-bitrate style)")
    ap.add_argument("--crop", default="%dx%d" % CROP_DEFAULT, help="WxH centre crop of the evaluated window, 0 = none")
    ap.add_argument("--keep-window", action="store_true", help="keep the lossless window master (default: delete at the end of the clip)")
    ap.add_argument("--name", default=None, help="suite name (default: file stem)")
    a = ap.parse_args(argv)
    a.crop = None if a.crop in ("0", "") else tuple(int(v) for v in a.crop.lower().split("x"))
    os.makedirs(a.out, exist_ok=True)
    ids = set(a.only.split(",")) if a.only else None
    cases = {}
    for src in SOURCES:
        if ids is None or src["id"] in ids:
            build_clip(src, a, cases)
    name = a.name or os.path.splitext(os.path.basename(a.suite))[0]
    merge_suite(a.suite, name, "Streaming-realistic encodes (make_streaming_encodes.py): per-shot VBR under a capped "
                "average, ABR ladder rungs and rung switches; pixels stay in scratch, never committed", cases)
    return 0


if __name__ == "__main__":
    sys.exit(main())
