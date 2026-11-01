# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Full-frame 1080p data for the TemporalDetail real-vector test (no crops).

For each master (tos, sol, boat, crowd; make_td_encodes.MASTERS) writes into --out (scratch, never committed):
  <clip>_1080p_master.yuv            the 1080p rung's truth (master Lanczos-downscaled to 1920 wide, as the encode)
  <clip>_1080p<tier>_coded.yuv       the decoded existing x265 encode (std, _low, _vlow), full frame
  <clip>_1080p<tier>_types.txt       frame types in display order (ffmpeg showinfo)
  data.json                          the case list (paths, raster, fps, types)
The encodes themselves come from make_td_encodes.py (--enc). yuv420p10le throughout.

  python -I tools/eval/td_vectors/prep_data.py --enc D:/codex/videoprocessor/eval-scratch/test-td-real/enc \
         --out D:/codex/td-real-vectors-out/data [--frames 72] [--rung 1080p]
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
    ap.add_argument("--enc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=72)
    ap.add_argument("--rung", default="1080p", choices=["1080p", "2160p"])
    ap.add_argument("--only", default=None)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    cases = []
    for m in mte.MASTERS:
        if a.only and a.only != m["id"]:
            continue
        w, h = m["size"]
        chain = [m["vf"]] if m["vf"] else []
        if a.rung == "1080p":
            h = int(round(h * 1920 / w / 2)) * 2
            w = 1920
            chain.append("scale=%d:%d:flags=lanczos" % (w, h))
        chain.append("format=yuv420p10le")
        n = min(a.frames, int(round(m["secs"] * m["fps"])))
        mfile = os.path.join(a.out, "%s_%s_master.yuv" % (m["id"], a.rung))
        if not os.path.isfile(mfile):
            run([mte.FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-ss", "%.3f" % m["start"], "-i", m["path"],
                 "-frames:v", str(n), "-vf", ",".join(chain), "-pix_fmt", "yuv420p10le", "-f", "rawvideo", mfile])
        for tier, suf in (("std", ""), ("low", "_low"), ("vlow", "_vlow")):
            cid = "%s_%s%s" % (m["id"], a.rung, suf)
            mp4 = os.path.join(a.enc, cid + ".mp4")
            cfile = os.path.join(a.out, cid + "_coded.yuv")
            r = run([mte.FFMPEG, "-hide_banner", "-y", "-i", mp4, "-frames:v", str(n), "-vf", "showinfo",
                     "-pix_fmt", "yuv420p10le", "-f", "rawvideo", cfile])
            types = "".join(re.findall(r" type:([IPB]) ", r.stderr))
            types = types[:n]  # showinfo may print one more line than -frames:v emits
            if len(types) != n:
                raise RuntimeError("%s: %d types for %d frames" % (cid, len(types), n))
            kbps = os.path.getsize(mp4) * 8.0 / (m["secs"]) / 1000.0
            cases.append(dict(id=cid, clip=m["id"], tier=tier, content=m["ct"], w=w, h=h, fps=m["fps"], frames=n,
                              types=types, kbps=round(kbps), master=mfile, coded=cfile))
            print(cid, w, h, n, types[:20], round(kbps), flush=True)
    path = os.path.join(a.out, "data.json")
    old = []
    if os.path.isfile(path):
        with open(path, encoding="utf8") as f:
            old = [c for c in json.load(f)["cases"] if c["id"] not in {x["id"] for x in cases}]
    with open(path, "w", encoding="utf8", newline="\n") as f:
        json.dump(dict(cases=old + cases), f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
