# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Same-bitrate GOP-length variants of one td_real case (separates I-frame distance from bitrate).

Re-encodes the 1080p rung of a master (make_td_encodes.MASTERS) with exactly the td_real x265 settings except keyint /
min-keyint (e.g. 12 = an I-frame every 12 frames, 250 = one I-frame in the window), decodes the full frame and appends
the cases to data.json (ids <clip>_1080p<tier>_gop<N>). The master file is shared with the base case.

  python -I tools/eval/td_vectors/gop_variants.py --clip boat --tier low --gops 12,250 \
         --enc D:/codex/td-real-vectors-out/enc --data D:/codex/td-real-vectors-out/data/data.json
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import make_td_encodes as mte  # noqa: E402


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("failed: %s\n%s" % (" ".join(cmd), r.stderr[-2000:]))
    return r


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", required=True)
    ap.add_argument("--tier", default="low")
    ap.add_argument("--gops", default="12,250")
    ap.add_argument("--enc", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--frames", type=int, default=72)
    a = ap.parse_args(argv)
    os.makedirs(a.enc, exist_ok=True)
    m = next(x for x in mte.MASTERS if x["id"] == a.clip)
    t = mte.TIERS[a.tier]
    with open(a.data, encoding="utf8") as f:
        data = json.load(f)
    base = next(c for c in data["cases"] if c["id"] == "%s_1080p%s" % (a.clip, t["suffix"]))
    w, h = base["w"], base["h"]
    chain = ([m["vf"]] if m["vf"] else []) + ["scale=%d:%d:flags=lanczos" % (w, h), "format=yuv420p10le"]
    n = int(round(m["secs"] * m["fps"]))
    kbps = t["kbps"][1] if t["kbps"] else m["kbps"][1]
    for g in [int(x) for x in a.gops.split(",")]:
        cid = "%s_1080p%s_gop%d" % (a.clip, t["suffix"], g)
        mp4 = os.path.join(a.enc, cid + ".mp4")
        xp = ("bitrate=%d:vbv-maxrate=%d:vbv-bufsize=%d:keyint=%d:min-keyint=%d:scenecut=0:open-gop=0:bframes=3:"
              "b-adapt=0:pbratio=%.2f:pools=4:frame-threads=2:log-level=error" % (kbps, int(kbps * 1.5), int(kbps * 2), g, g,
                                                                              t["pbratio"]))
        if not os.path.isfile(mp4):
            run([mte.FFMPEG, "-hide_banner", "-y", "-ss", "%.3f" % m["start"], "-i", m["path"], "-frames:v", str(n),
                 "-vf", ",".join(chain), "-c:v", "libx265", "-preset", "medium", "-x265-params", xp, "-an", mp4])
        cfile = os.path.join(os.path.dirname(a.data), cid + "_coded.yuv")
        r = run([mte.FFMPEG, "-hide_banner", "-y", "-i", mp4, "-frames:v", str(a.frames), "-vf", "showinfo",
                 "-pix_fmt", "yuv420p10le", "-f", "rawvideo", cfile])
        types = "".join(re.findall(r" type:([IPB]) ", r.stderr))[:a.frames]
        c = dict(base, id=cid, coded=cfile, types=types, gop=g, kbps=round(os.path.getsize(mp4) * 8.0 / m["secs"] / 1000.0))
        data["cases"] = [x for x in data["cases"] if x["id"] != cid] + [c]
        print(cid, types[:40], c["kbps"], flush=True)
    with open(a.data, "w", encoding="utf8", newline="\n") as f:
        json.dump(data, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
