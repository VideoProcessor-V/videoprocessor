# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Triage of the per-axis confident-wrong episodes of a source-identity evaluation (SID-CAL2).

  python -I tools/eval/source_id/triage_wrong.py --reels D:/testclips/sourceid/reels.json --eval-dir <eval dir> --out <triage.md>
  python -I tools/eval/source_id/triage_wrong.py --selftest

Reads the evaluation exactly as score_identity.py does (read only), takes every per-axis CONFIDENT-wrong episode of the chain lane (state_x / state_y
CONFIDENT with measured_n_<axis> different from that axis's truth) and writes one table row per episode: reel, axis, wrong n, truth n, N, bit depth, encode / bitrate,
kernel truth vs reported, dwell (seconds the axis was CONFIDENT in the episode), the peak llr, the power state, and a class:

  a  alias / harmonic of the truth: n = truth x p/q with q <= 4 (2x, 3/2, 4/3, 3/4, 1/2 ...), within 2%
  b  neighbour: |n - truth| within 5% of the truth, or one rung (1/4, 1/3, 5/12, 1/2, 2/3, 1 of N) away from the truth's rung
  c  kernel confusion: n within 1 px of the truth (the jitter the exact comparison counts as wrong) while the reported kernel family differs from the
     truth family (n is right, the kernel call drove it)
  d  other

The kernel families are compared without their tap / order suffix (lanczos3 and lanczos are the same family). The summary lines count the classes.
"""
import argparse
import csv
import json
import os
import re
import sys
import tempfile
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score_identity as S  # noqa: E402

LADDER = (0.25, 1.0 / 3.0, 5.0 / 12.0, 0.5, 2.0 / 3.0, 1.0)     # the rung ratios of the reels (make_identity_reels.py)


def family(name):
    return re.sub(r"\d+$", "", (name or "").strip().lower())


def classify(n, truth, N, kernel_truth=None, kernel_reported=None):
    """Returns (class letter, detail)."""
    d = abs(n - truth)
    if d <= 1:
        if kernel_truth and kernel_reported and family(kernel_truth) != family(kernel_reported):
            return "c", "n within 1 px, kernel %s reported for %s" % (kernel_reported, kernel_truth)
        return "b", "within 1 px of the truth"
    ratio = n / float(truth)
    if abs(ratio - 1.0) <= 0.05:
        return "b", "%.1f%% from the truth" % (100.0 * (ratio - 1.0))
    for q in range(1, 5):
        p = int(round(ratio * q))
        if p >= 1 and abs(ratio - p / float(q)) <= 0.02 * p / float(q):
            return "a", "n = truth x %d/%d" % (p, q)
    ti = [i for i, r in enumerate(LADDER) if abs(truth - r * N) <= 0.03 * truth]
    if ti:
        for j in (ti[0] - 1, ti[0] + 1):
            if 0 <= j < len(LADDER) and abs(n - LADDER[j] * N) <= 0.03 * n:
                return "b", "one rung %s (truth rung %.3f, n rung %.3f of N)" % ("down" if j < ti[0] else "up", LADDER[ti[0]], LADDER[j])
    return "d", "n/truth = %.4f, n/N = %.4f" % (ratio, n / float(N))


def episodes(reels, rows, cfg):
    """[(reel, episode dict, chain rows of the reel)] of the per-axis episodes."""
    out = []
    for (rid, lane), rr in sorted(rows.items()):
        if lane != "chain" or rid not in reels:
            continue
        st = S.score_reel_lane(reels[rid], rr, [], cfg)
        for ep in st["axis_episodes"]:
            out.append((rid, ep, rr))
    return out


def describe(reel_raw, reel, rid, ep, rr):
    seg = reel["segs"][ep["seg"]]
    raw_seg = reel_raw["segments"][ep["seg"]]
    ax = ep["axis"]
    N = reel["W"] if ax == "x" else reel["H"]
    inside = [r for r in rr if ep["t0"] - 1e-9 <= r["t"] <= ep["t1"] + 1e-9 and (r.get("state_" + ax) or "").upper() == "CONFIDENT"]
    ts = sorted(r["t"] for r in rr)
    gaps = sorted(b - a for a, b in zip(ts, ts[1:]) if b > a)
    step = gaps[len(gaps) // 2] if gaps else 0.5
    kernels = Counter((r.get("kernel") or "").strip() for r in inside if (r.get("kernel") or "").strip())
    llrs = [S.fnum(r.get("llr_" + ax)) for r in inside]
    llrs = [v for v in llrs if v is not None]
    powers = Counter((r.get("power_" + ax) or "").strip() for r in inside)
    ktruth = (raw_seg.get("kernel") or {}).get("family") if raw_seg.get("kernel") else None
    krep = kernels.most_common(1)[0][0] if kernels else None
    rung = raw_seg.get("rung") or {}
    enc = "%s %s%s" % (rung.get("codec", "?"), rung.get("rate", ["?", "?"])[0] if rung.get("rate") else "?",
                       ("=" + str(rung["rate"][1])) if rung.get("rate") and len(rung["rate"]) > 1 else "")
    if rung.get("kbps") is not None:
        enc += " (%.0f kbps)" % rung["kbps"]
    truth = seg["truth_n"][ax]
    cls, detail = classify(ep["n"], truth, N, ktruth, krep)
    return dict(reel=rid, axis=ax, n=ep["n"], truth=truth, N=N, bits=seg.get("bits"), enc=enc, ktruth=ktruth or "none (native)", krep=krep or "-",
                dwell=(ep["t1"] - ep["t0"]) + step, t0=ep["t0"], llr=(max(llrs) if llrs else None),
                power="/".join("%s x%d" % kv for kv in powers.most_common(2)) or "-", cls=cls, detail=detail)


def render(items):
    L = ["# Per-axis confident-wrong triage", "",
         "| reel | axis | wrong n | truth n | N | bits | encode | kernel truth / reported | dwell s | t0 s | peak llr | power | class | why |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for it in items:
        L.append("| %s | %s | %d | %d | %d | %s | %s | %s / %s | %.1f | %.1f | %s | %s | %s | %s |" % (
            it["reel"], it["axis"], it["n"], it["truth"], it["N"], it["bits"], it["enc"], it["ktruth"], it["krep"], it["dwell"], it["t0"],
            "-" if it["llr"] is None else "%.1f" % it["llr"], it["power"], it["cls"], it["detail"]))
    cnt = Counter(it["cls"] for it in items)
    fam = Counter((it["reel"].split(".")[0], it["cls"]) for it in items)
    L += ["", "Summary: %d episodes; classes a (alias / harmonic) %d, b (neighbour / rung) %d, c (kernel confusion) %d, d (other) %d." % (
        len(items), cnt["a"], cnt["b"], cnt["c"], cnt["d"]),
        "By family: " + ", ".join("%s %s %d" % (f, c, n) for (f, c), n in sorted(fam.items())) + ".",
        "By bit depth: " + ", ".join("%s-bit %d" % (b, sum(1 for it in items if it["bits"] == b)) for b in sorted(set(it["bits"] for it in items), key=str)) + "."]
    return "\n".join(L) + "\n"


def run(reels_path, eval_dir, out_path):
    reels = S.load_manifest(reels_path)
    with open(reels_path, "r", encoding="utf-8") as f:
        raw = dict((r["id"], r) for r in json.load(f)["reels"])
    rows, _events, _done = S.load_eval(eval_dir)
    cfg = S.default_cfg()
    items = [describe(raw[rid], reels[rid], rid, ep, rr) for rid, ep, rr in episodes(reels, rows, cfg)]
    items.sort(key=lambda it: (it["reel"], it["axis"], it["t0"]))
    md = render(items)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(md)
    return items, md


def selftest():
    assert classify(2160, 1080, 2160)[0] == "a" and classify(1440, 1080, 2160)[0] == "a"       # 2x, 4/3
    assert classify(810, 1080, 2160)[0] == "a" and classify(720, 1080, 2160)[0] == "a"         # 3/4, 2/3
    assert classify(1080, 1080, 2160, "lanczos3", "bicubic")[0] == "c"
    assert classify(1080, 1080, 2160, "lanczos3", "lanczos")[0] == "b"                         # same family, 0 px
    assert classify(1081, 1080, 2160)[0] == "b" and classify(1100, 1080, 2160)[0] == "b"
    assert classify(945, 2160, 2160)[0] == "d"                                                  # 7/16 of a native: nothing of the above
    assert classify(1350, 2160, 2160)[0] == "d" and classify(1440, 2160, 2160)[0] == "a"       # 5/8 of a native: d; 2/3: a
    assert family("spline36") == "spline" and family("Lanczos") == "lanczos"
    assert classify(1375, 1920, 3840)[0] == "d" and classify(2560, 1920, 3840)[0] == "a"       # 4/3 of the truth
    assert classify(1600, 1920, 3840)[0] == "b"                                                  # 5/12 of N: the rung below 1/2 (and not a q <= 4 ratio)
    print("triage_wrong selftest ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reels", default="D:/testclips/sourceid/reels.json")
    ap.add_argument("--eval-dir", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.eval_dir or not a.out:
        ap.error("--eval-dir and --out are required")
    items, md = run(a.reels, a.eval_dir, a.out)
    print(md.split("Summary:")[1].strip().split("\n")[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
