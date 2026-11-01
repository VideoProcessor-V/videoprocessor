# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Write run_jobs.py job lists for the TemporalDetail real-vector test.

  python -I tools/eval/td_vectors/make_jobs.py --kind ad|td|ablate|band --clips boat,tos --vsrc disref --out jobs.txt
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")
OUT = "D:/codex/td-real-vectors-out"
DATA = OUT + "/data/data.json"
VEC = OUT + "/vec"
PY = sys.executable.replace("\\", "/") + " -I "
TIERS = ["", "_low", "_vlow"]
# gate ablation: bit, name (Model::Tuning::GateBits) plus the grain budget and the output guard
GATES = [(1, "corr"), (2, "highband"), (4, "focus"), (8, "stronger"), (16, "harm"), (32, "base"), (64, "unc"),
         (128, "coherence"), (256, "pixelcap"), (512, "evidence"), (1024, "hysteresis"), (2048, "age"), (4096, "retire")]
ALL = 8191


def td_line(case, src, tag, extra, frames=0, save=False):
    o = "%s/td/%s/%s/%s.json" % (OUT, case, src.replace(":", "_"), tag)
    cmd = PY + "%s/td_run.py --data %s --case %s --vec %s --source %s --out %s %s" % (HERE, DATA, case, VEC, src, o, extra)
    if frames:
        cmd += " --frames %d" % frames
    if save:
        cmd += " --save %s/td/%s/%s" % (OUT, case, src.replace(":", "_"))
    return o + " " + cmd


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--kind", required=True)
    ap.add_argument("--clips", required=True)
    ap.add_argument("--vsrc", default="disref")
    ap.add_argument("--tiers", default="std,low,vlow")
    ap.add_argument("--out", required=True)
    ap.add_argument("--final", default="", help="td_run arguments of the chosen set (kind final)")
    a = ap.parse_args(argv)
    tiers = [{"std": "", "low": "_low", "vlow": "_vlow"}[t] for t in a.tiers.split(",")]
    lines = []
    for clip in a.clips.split(","):
        for t in tiers:
            case = "%s_1080p%s" % (clip, t)
            if a.kind == "ad":
                o = "%s/ad/%s_%s.json" % (OUT, case, a.vsrc)
                align = " --align" if a.vsrc == "disref" else ""
                lines.append(o + " " + PY + "%s/available_detail.py --data %s --case %s --vec %s --sources %s:master,%s:coded --out %s%s"
                             % (HERE, DATA, case, VEC, a.vsrc, a.vsrc, o, align))
            elif a.kind == "td":
                for src in ("%s:master" % a.vsrc, "%s:coded" % a.vsrc):
                    lines.append(td_line(case, src, "default", "", save=True))
            elif a.kind == "ablate":
                src = "%s:master" % a.vsrc
                lines.append(td_line(case, src, "all_off", "--gates-off %d --budget 0 --no-guard" % ALL, 48))
                lines.append(td_line(case, src, "force1", "--force-weight 1", 48))
                for bit, name in GATES:
                    lines.append(td_line(case, src, "only_" + name, "--gates-off %d --budget 0 --no-guard" % (ALL & ~bit), 48))
                    lines.append(td_line(case, src, "default_minus_" + name, "--gates-off %d" % bit, 48))
                lines.append(td_line(case, src, "all_off_plus_budget", "--gates-off %d --no-guard" % ALL, 48))
                lines.append(td_line(case, src, "all_off_plus_guard", "--gates-off %d --budget 0" % ALL, 48))
                lines.append(td_line(case, src, "default_minus_budget", "--budget 0", 48))
                lines.append(td_line(case, src, "default_minus_guard", "--no-guard", 48))
            if a.kind == "round2":   # least-strict candidates around "only the motion-uncertainty gate" (round-1 best)
                U = ALL & ~64
                cfgs = [("u", "--gates-off %d --budget 0 --no-guard" % U),
                        ("u_guard", "--gates-off %d --budget 0" % U),
                        ("u_budget", "--gates-off %d --no-guard" % U),
                        ("u_base", "--gates-off %d --budget 0 --no-guard" % (U & ~32)),
                        ("u_coh", "--gates-off %d --budget 0 --no-guard" % (U & ~128)),
                        ("u_base_coh_guard", "--gates-off %d --budget 0" % (U & ~32 & ~128)),
                        ("u025", "--gates-off %d --budget 0 --no-guard --unc-px 0.25" % U),
                        ("u100", "--gates-off %d --budget 0 --no-guard --unc-px 1.0" % U)]
                for src in ("%s:master" % a.vsrc, "%s:coded" % a.vsrc):
                    for tag, extra in cfgs:
                        lines.append(td_line(case, src, "r2_" + tag, extra, 48))
            if a.kind == "final":    # the chosen least-strict set, all tiers, 72 frames, saved for banding
                for src in ("%s:master" % a.vsrc, "%s:coded" % a.vsrc):
                    lines.append(td_line(case, src, "final", a.final, 0, save=True).replace(
                        "--save %s/td/%s/%s" % (OUT, case, src.replace(":", "_")),
                        "--save %s/td/%s/%s_final" % (OUT, case, src.replace(":", "_"))))
    with open(a.out, "a", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(len(lines), "jobs ->", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
