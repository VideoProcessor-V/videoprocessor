# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Run vecgen.py over the data.json sequences (masters once per clip, decoded encodes per case), sequentially.

  python -I tools/eval/td_vectors/run_vecgen.py --data D:/codex/td-real-vectors-out/data/data.json \
         --vec D:/codex/td-real-vectors-out/vec --source disref --worker 0 --workers 4
Sequence ids: <clip>_master and the case ids (<clip>_1080p<tier>). Output <vec>/<seq>/<source>/flow_*.npy.
For nvof sources the .nvf file is <vec>/<seq>/nvof.nvf.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def sequences(data):
    seqs, seen = [], set()
    for c in data["cases"]:
        mid = c["clip"] + "_master"
        if mid not in seen:
            seen.add(mid)
            seqs.append((mid, c["master"], c["w"], c["h"]))
        seqs.append((c["id"], c["coded"], c["w"], c["h"]))
    return seqs


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--vec", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--worker", type=int, default=0)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--only", default=None, help="comma list of sequence ids")
    a = ap.parse_args(argv)
    with open(a.data, encoding="utf8") as f:
        seqs = sequences(json.load(f))
    if a.only:
        keep = a.only.split(",")
        seqs = [s for s in seqs if s[0] in keep]
    for i, (sid, path, w, h) in enumerate(seqs):
        if i % a.workers != a.worker:
            continue
        out = os.path.join(a.vec, sid, a.source)
        cmd = [sys.executable, "-I", os.path.join(HERE, "vecgen.py"), "--yuv", path, "--w", str(w), "--h", str(h),
               "--source", a.source, "--out", out]
        if a.source.startswith("nvof"):
            cmd += ["--nvf", os.path.join(a.vec, sid, "nvof.nvf")]
        print(sid, flush=True)
        r = subprocess.run(cmd)
        if r.returncode:
            print("FAILED", sid, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
