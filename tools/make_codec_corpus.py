"""Real-codec corpus for the Stage 12 detection gate (planner G18, owner 2026-10-16).

From the uncompressed 4K clips in SOURCE (the real-footage gate's folder), make
short real encodes and decode a few frames of each to raw 10-bit 4:2:0 (the
gate's input format), plus manifest.csv describing every case:

  native     4K coded directly (x265 CRF 20/26/32, x264 CRF 18/23/28, AV1 + film-grain synthesis)
  stream1080 downscaled to 1080 (lanczos), coded at 1080, decoded, upscaled to 4K by the
             player (bicubic, and lanczos for one CRF) - what a 1080 rendition looks like
  master1080 a 1080 master upscaled to 4K (bicubic), then coded at 4K - a "4K" stream of a 1080 title

Usage: python tools/make_codec_corpus.py SOURCE OUT [--ffmpeg PATH] [--frames 32]
Nothing is written inside the repository; OUT holds the encodes and decoded frames.
Resumable: finished cases are kept; a case cut off by an interrupted run is redone.
"""
import argparse, csv, os, subprocess, sys

CLIPS = [
    ("Bosphorus_3840x2160_120fps_420_10bit_YUV.yuv", "raw", 120),
    ("Jockey_3840x2160_120fps_420_10bit_YUV.yuv", "raw", 120),
    ("ReadySetGo_3840x2160_120fps_420_10bit_YUV.yuv", "raw", 120),
    ("YachtRide_3840x2160_120fps_420_10bit_YUV.yuv", "raw", 120),
    ("ShakeNDry_3840x2160_120fps_420_10bit_YUV.yuv", "raw", 120),
    ("park_joy_2160p50.y4m", "y4m", 50),
    ("crowd_run_2160p50.y4m", "y4m", 50),
]
DECODE_FROM = 20      # decoded frames: DECODE_FROM .. +DECODED-1 (past the first I-frame: P/B frames)
DECODED = 4


def cases():
    out = []
    for crf in (20, 26, 32):
        out.append(("native", "x265", crf, None, 1.0))
    for crf in (18, 23, 28):
        out.append(("native", "x264", crf, None, 1.0))
    out.append(("native", "av1fgs", 35, None, 1.0))
    for crf in (20, 26, 32):
        out.append(("stream1080", "x265", crf, "bicubic", 0.5))
    out.append(("stream1080", "x265", 26, "lanczos", 0.5))
    for crf in (18, 23, 28):
        out.append(("stream1080", "x264", crf, "bicubic", 0.5))
    out.append(("stream1080", "av1fgs", 35, "bicubic", 0.5))
    for crf in (20, 26, 32):
        out.append(("master1080", "x265", crf, "bicubic", 0.5))
    return out


def codec_args(codec, crf):
    if codec == "x265":
        return ["-c:v", "libx265", "-preset", "fast", "-crf", str(crf), "-x265-params", "log-level=error"]
    if codec == "x264":
        return ["-c:v", "libx264", "-preset", "fast", "-crf", str(crf)]
    if codec == "av1fgs":
        return ["-c:v", "libsvtav1", "-preset", "8", "-crf", str(crf), "-svtav1-params", "film-grain=8"]
    raise ValueError(codec)


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + r.stderr[-2000:] + "\n")
        raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--frames", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for clip, kind, fps in CLIPS:
        src = os.path.join(a.source, clip)
        if not os.path.exists(src):
            print("missing", clip)
            continue
        base = os.path.splitext(clip)[0].split("_")[0]
        if kind == "raw":
            inp = ["-f", "rawvideo", "-pix_fmt", "yuv420p10le", "-s", "3840x2160", "-r", str(fps), "-ss", "1", "-i", src]
        else:
            inp = ["-ss", "1", "-i", src]
        for scene, codec, crf, upk, truth in cases():
            name = f"{base}_{scene}_{codec}_crf{crf}" + (f"_{upk}" if upk and scene == "stream1080" else "")
            coded = os.path.join(a.out, name + (".mkv"))
            frames = os.path.join(a.out, name + ".yuv")
            complete = DECODED * 3840 * 2160 * 3   # yuv420p10le bytes for the decoded frames
            if os.path.exists(frames) and os.path.getsize(frames) != complete:
                os.remove(frames)   # cut off (an interrupted run): redo
            if os.path.exists(coded) and not os.path.exists(frames):
                os.remove(coded)
            if not os.path.exists(frames):
                pre = []
                if scene == "stream1080":
                    pre = ["-vf", "scale=1920:1080:flags=lanczos"]
                elif scene == "master1080":
                    pre = ["-vf", f"scale=1920:1080:flags=lanczos,scale=3840:2160:flags={upk}"]
                run([a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error"] + inp + pre + ["-frames:v", str(a.frames), "-pix_fmt", "yuv420p10le"] + codec_args(codec, crf) + [coded])
                post = f"select=between(n\\,{DECODE_FROM}\\,{DECODE_FROM + DECODED - 1})"
                if scene == "stream1080":
                    post = f"scale=3840:2160:flags={upk}," + post
                run([a.ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", coded, "-vf", post, "-fps_mode", "passthrough",
                     "-frames:v", str(DECODED), "-pix_fmt", "yuv420p10le", "-f", "rawvideo", frames])
                os.remove(coded)
            size = os.path.getsize(frames)
            rows.append({"file": os.path.basename(frames), "clip": base, "scene": scene, "codec": codec, "crf": crf,
                         "upscaler": upk or "", "truth": truth, "frames": size // (3840 * 2160 * 3)})
            print(name, rows[-1]["frames"], "frames", flush=True)
    with open(os.path.join(a.out, "manifest.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("cases", len(rows))


if __name__ == "__main__":
    main()
