# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tiny job queue: each line of --jobs is "<output file> <command...>"; a worker skips lines whose output exists or
that another worker claimed (atomic mkdir <output>.claim), runs the rest one at a time. Start N workers in parallel.

  python -I tools/eval/td_vectors/run_jobs.py --jobs jobs.txt --log DIR
"""
import argparse
import os
import shlex
import subprocess
import sys


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", required=True)
    ap.add_argument("--log", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.log, exist_ok=True)
    with open(a.jobs, encoding="utf8") as f:
        lines = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    for ln in lines:
        out, cmd = ln.split(" ", 1)
        if os.path.exists(out):
            continue
        claim = out + ".claim"
        try:
            os.makedirs(claim)
        except OSError:
            continue
        name = os.path.basename(out)
        with open(os.path.join(a.log, name + ".log"), "w", encoding="utf8") as lg:
            r = subprocess.run(shlex.split(cmd, posix=False), stdout=lg, stderr=subprocess.STDOUT)
        if r.returncode:
            print("FAILED", out, flush=True)
        else:
            os.rmdir(claim)
    return 0


if __name__ == "__main__":
    sys.exit(main())
