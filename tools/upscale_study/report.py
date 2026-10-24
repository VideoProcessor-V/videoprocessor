"""Upscale study report: <work>/scores.csv -> <work>/report.md (stdlib only).

Per case (delivery), one table of every variant's means: PSNR, SSIM, ringing, overshoot,
grain keep, high-band error, GPU ms (sweep). Sorted by PSNR; a variant whose grain keep falls
under 0.95 is marked, since the owner's rule is grain faithful first (vp-grain-faithful).
"""
import argparse
import csv
import os
from collections import defaultdict

METRICS = ["psnr", "ssim", "ring_codes", "overshoot_codes", "grain_keep", "hf_err_codes", "gpu_ms"]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    a = ap.parse_args(argv)
    rows = list(csv.DictReader(open(os.path.join(a.work, "scores.csv"))))
    groups = defaultdict(lambda: defaultdict(list))
    for r in rows:
        g = groups[r["case"]][r["variant"]]
        g.append(r)
    out = ["# Upscale study results", "", f"{len(rows)} scored frames. Reference: the kit's ideal resample of the 4K master to the FL35 picture size.", ""]
    for case in sorted(groups):
        out += [f"## {case}", "", "| variant | n | " + " | ".join(METRICS) + " |", "|---|---|" + "---|" * len(METRICS)]
        table = []
        for variant, rs in groups[case].items():
            means = {}
            for m in METRICS:
                vals = [float(r[m]) for r in rs if r.get(m) not in (None, "", "None")]
                means[m] = sum(vals) / len(vals) if vals else None
            table.append((variant, len(rs), means))
        table.sort(key=lambda t: -(t[2]["psnr"] or 0))
        for variant, n, means in table:
            flag = " (grain lost)" if means["grain_keep"] is not None and means["grain_keep"] < 0.95 else ""
            cells = [("-" if means[m] is None else f"{means[m]:.4f}" if m in ("ssim", "grain_keep") else f"{means[m]:.3f}") for m in METRICS]
            out.append(f"| {variant}{flag} | {n} | " + " | ".join(cells) + " |")
        out.append("")
    path = os.path.join(a.work, "report.md")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
