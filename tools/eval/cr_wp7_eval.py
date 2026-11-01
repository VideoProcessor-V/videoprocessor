# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""WP7: option A (pre-TM) vs option B (post-TM) grain-safe regional contrast recovery vs libplacebo CR 0.30,
HDR -> SDR 100 nit, Python CPU reference.

  python -I tools/eval/cr_wp7_eval.py --scratch D:/codex/videoprocessor/eval-scratch/round2/ltm \
         --hr-scratch D:/codex/videoprocessor/eval-scratch/round2/hr --out tools/eval/prototypes/cr_wp7 [--clips sol_a,noc_low]

Part 1 (the six ltm_eval clips, 1280 wide, 16 frames): restored share vs the plain curve, boost fraction, halo,
two-seed grain probe (overall / near-edge, relative to the plain curve), lag-1, pump (motion-compensated change
of the adaptation layer).  Part 2 (4 HDR codec cases, x265-10 CRF30, 1080p, 8 frames): deband (VP model) THEN
the candidate; masked step visibility, step p99 and band MSE relative to deband + plain curve.
Acceptance: cr_wp7_model.cr_acceptance (plan 47 WP7).
"""
import argparse
import csv
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import artefact_headroom as ah  # noqa: E402
import cr_wp7_model as cw  # noqa: E402
import deband_stack as ds  # noqa: E402
import eval_metrics as em  # noqa: E402
import ltm_eval as le  # noqa: E402
import texture_dither as td  # noqa: E402
import tonemap_ref as tr  # noqa: E402

P = cw.CRParams
def _cr(mode, **kw):
    return lambda pk, h: cw.ContrastRecovery(mode, pk, h, params=P(**kw))


CANDS = [  # label, factory(peak, ref_h)
    ("plain", lambda pk, h: tr.ToneMapper("global", pk, h)),
    ("cr 0.30", lambda pk, h: tr.ToneMapper("cr", pk, h, cr=0.30)),
    ("cr_gs 0.30", lambda pk, h: tr.ToneMapper("cr_gs", pk, h, cr=0.30)),
    ("A 0.25", _cr("A", s_max=0.25)), ("A 0.35", _cr("A", s_max=0.35)), ("A 0.50", _cr("A", s_max=0.50)),
    ("B 0.25", _cr("B", s_max=0.25)), ("B 0.35", _cr("B", s_max=0.35)), ("B 0.50", _cr("B", s_max=0.50)),
    ("A 0.30", _cr("A", s_max=0.30)), ("A 0.40", _cr("A", s_max=0.40)), ("B 0.30", _cr("B", s_max=0.30)),
    ("A 0.40 det1", _cr("A", s_max=0.40, detail_rel=1 / 720.0)), ("A 0.25 det1", _cr("A", s_max=0.25, detail_rel=1 / 720.0)),
    ("A 0.25 unguarded", _cr("A", s_max=0.25, use_light=False, use_band=False, use_halo=False)),
    ("B 0.25 unguarded", _cr("B", s_max=0.25, use_light=False, use_band=False, use_halo=False)),
    ("A 0.25 -light", _cr("A", s_max=0.25, use_light=False)),
    ("A 0.25 -band", _cr("A", s_max=0.25, use_band=False)),
    ("A 0.25 -halo", _cr("A", s_max=0.25, use_halo=False)),
]
HR_CANDS = ["plain", "cr 0.30", "cr_gs 0.30", "A 0.25", "A 0.25 -band", "A 0.25 unguarded", "B 0.25", "B 0.50"]
HR_CASES = [("sol_sky", 1000.0), ("sol_dark", 1000.0), ("nocturne_dark", 4000.0), ("nocturne_mid", 4000.0)]
HR_ENC = "x265-10_crf30"
FACT = dict(CANDS)
ONLY = None   # --cands filter (plain is always run: it is the baseline)


def HR_LIST():
    return [l for l in (list(dict.fromkeys(HR_CANDS + sorted(ONLY or [])))) if not ONLY or l == "plain" or l in ONLY]


def run(label, seq, peak, ref_h):
    m = FACT[label](peak, ref_h)
    t0 = time.perf_counter()
    out = np.stack([m.process(f) for f in seq])
    return out, (time.perf_counter() - t0) * 1000.0 / len(seq)


def part1(scratch, out_dir, clips):
    rows = []
    for name, parts, peak, note in le.CLIPS:
        if clips and name not in clips:
            continue
        seq = le.decode_clip(scratch, name, parts)
        ref_h = seq.shape[1]
        flows = le.flows_for(seq)
        cuts = le.cut_pairs(seq, flows)
        ng = 8
        gA = np.stack([em.probe_grain(seq.shape[1:], 101, t, 10) for t in range(ng)])
        gB = np.stack([em.probe_grain(seq.shape[1:], 202, t, 10) for t in range(ng)])
        edge = [em.near_edge_zone(f, 10)[0] for f in seq[:ng]]
        base = {}
        for label, _ in CANDS:
            if ONLY and label != "plain" and label not in ONLY:
                continue
            out, ms = run(label, seq, peak, ref_h)
            Pc = le.perceptual(out)
            dG = le.perceptual(run(label, seq[:ng] + gA, peak, ref_h)[0]) - le.perceptual(run(label, seq[:ng] + gB, peak, ref_h)[0])
            lc = em.local_contrast_ratio(seq, Pc, 10)
            lcm = float(np.mean([lc["ratio_s%d" % k] for k in (1, 2, 3)]))
            if label == "plain":
                base = dict(P=Pc, dG=dG, lc=lcm)
            zk = cw.zone_grain_keep(gA - gB, dG, base["dG"], edge)
            gp = em.grain_probe(base["dG"], dG, [np.ones(dG.shape[1:], bool)] * ng)
            pump = le.pump(Pc, base["P"], flows, cuts, ref_h) if label != "plain" else 0.0
            row = dict(clip=name, candidate=label, lc_ratio=lcm,
                       restore=(lcm - base["lc"]) / (1 - base["lc"]) if base["lc"] < 1 else None,
                       boost=lc["boost_fraction"], halo=em.halo_overshoot(base["P"], Pc, 10),
                       keep_all=zk["keep_all"], keep_edge=zk["keep_edge"], lag1=gp["lag1"], pump=pump, py_ms=ms)
            rows.append(row)
            print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
    return rows


def part2(hr, clips=None):
    rows = []
    dk = ah.dark_code_for("sdr")
    for name, peak in HR_CASES:
        if clips and name not in clips:
            continue
        M = ah.luma_seq(os.path.join(hr, "%s_1080p_yuv420p10le.yuv" % name), "yuv420p10le", ds.NFR)
        D = ah.luma_seq(os.path.join(hr, "%s_1080p_%s.yuv" % (name, HR_ENC)), "yuv420p10le", ds.NFR)
        Xd = ds.pre_variant("vpdb", D, M, True)      # decode -> VP deband model (pre-TM), then the candidate
        Xm = M
        res = {}
        for label in HR_LIST():
            Mo, _ = run(label, Xm, peak, 1080)
            Do, _ = run(label, Xd, peak, 1080)
            fr = [ah.frame_metrics(r, c, dk) for r, c in zip(Mo, Do)]
            s = ah.summarize(fr)
            n = sum(f["n"] for f in fr)
            vis = np.concatenate([td.frame_tiles(r, c, dk) for r, c in zip(Mo, Do)])
            res[label] = dict(case=name, candidate=label, vis=float(vis[:, 2].mean()) if len(vis) else None,
                              step_p99=s["step_p99"], band_mse=sum(f["band"] for f in fr) / n)
        for label in HR_LIST():
            r = res[label]
            r["vis_ratio"] = cw.step_visibility_ratio(r["vis"], res["plain"]["vis"])
            r["step_ratio"] = r["step_p99"] / res["plain"]["step_p99"]
            r["band_ratio"] = r["band_mse"] / res["plain"]["band_mse"]
            rows.append(r)
            print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()}), flush=True)
    return rows


def write(rows, rows2, out):
    os.makedirs(out, exist_ok=True)
    cols = ["clip", "candidate", "lc_ratio", "restore", "boost", "halo", "keep_all", "keep_edge", "lag1", "pump", "py_ms"]
    f4 = lambda v: "-" if v is None else ("%.4g" % v if isinstance(v, float) else str(v))
    with open(os.path.join(out, "cr_wp7_clips.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([f4(r.get(c)) for c in cols])
    c2 = ["case", "candidate", "vis", "vis_ratio", "step_p99", "step_ratio", "band_mse", "band_ratio"]
    with open(os.path.join(out, "cr_wp7_deband.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(c2)
        for r in rows2:
            w.writerow([f4(r.get(c)) for c in c2])
    labels = [l for l, _ in CANDS]
    clips = sorted(set(r["clip"] for r in rows))
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# WP7 contrast recovery: option A (pre-TM) vs B (post-TM) vs libplacebo CR 0.30 (HDR -> SDR 100 nit)", "",
          "Means over %d clips (worst clip in brackets for grain). restore = share of the plain curve's lost local "
          "contrast returned; boost = tiles > +5 %% above the source; halo vs plain; keep = two-seed grain probe rms "
          "relative to the plain curve (overall / near edge); pump in PQ codes; deband columns = mean over 4 codec "
          "cases of (deband + candidate) / (deband + plain)." % len(clips), "",
          "| candidate | restore | boost | halo | grain keep all (max) | keep edge (min) | lag-1 | pump | step vis | step p99 | band MSE | py ms |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    summ = {}
    for l in labels:
        rs = [r for r in rows if r["candidate"] == l]
        if not rs:
            continue
        mean = lambda k, src=rs: float(np.mean([r[k] for r in src if r.get(k) is not None]))
        d2 = [r for r in rows2 if r["candidate"] == l]
        dv = lambda k: ("%.3f" % float(np.mean([r[k] for r in d2 if r.get(k) is not None]))) if d2 else "-"
        summ[l] = dict(restore=mean("restore"), boost=mean("boost"), halo=mean("halo"), keep_all=mean("keep_all"),
                       keep_edge=mean("keep_edge"), pump=mean("pump"),
                       vis_ratio=float(np.mean([r["vis_ratio"] for r in d2])) if d2 else None)
        md.append("| %s | %.3f | %.3f | %.5f | %.3f (%.3f) | %.3f (%.3f) | %+.3f | %.3f | %s | %s | %s | %.0f |" % (
            l, summ[l]["restore"], summ[l]["boost"], summ[l]["halo"], summ[l]["keep_all"],
            max(r["keep_all"] for r in rs), summ[l]["keep_edge"], min(r["keep_edge"] for r in rs),
            mean("lag1"), summ[l]["pump"], dv("vis_ratio"), dv("step_ratio"), dv("band_ratio"), mean("py_ms")))
    md += ["", "## Acceptance vs libplacebo CR 0.30 (means; plan 47 WP7)", "",
           "| candidate | restore >= 0.9 x CR | grain 0.95-1.05 | edge keep >= 0.90 | boost <= CR | halo <= CR | step vis <= 1.02 | pump < 0.5 | ALL |",
           "|---|---|---|---|---|---|---|---|---|"]
    cr = summ.get("cr 0.30")
    sym = {True: "PASS", False: "FAIL", None: "n/a"}
    if cr:
        for l in labels:
            if l in summ and l not in ("plain", "cr 0.30"):
                a = cw.cr_acceptance(summ[l], cr)
                md.append("| %s | %s (%.3f vs %.3f) | %s | %s | %s | %s | %s | %s | %s |" % (
                    l, sym[a["restore"]], summ[l]["restore"], cr["restore"], sym[a["grain"]], sym[a["grain_edge"]],
                    sym[a["boost"]], sym[a["halo"]], sym[a["step_vis"]], sym[a["pump"]], sym[a["all"]]))
    md += ["", "## Per clip", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(f4(r.get(c)) for c in cols) + " |")
    md += ["", "## Deband then candidate (per case)", "", "| " + " | ".join(c2) + " |", "|" + "---|" * len(c2)]
    for r in rows2:
        md.append("| " + " | ".join(f4(r.get(c)) for c in c2) + " |")
    md.append("")
    with open(os.path.join(out, "cr_wp7.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))
    with open(os.path.join(out, "cr_wp7_summary.json"), "w", encoding="utf8", newline="\n") as f:
        json.dump(summ, f, indent=1, sort_keys=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch")
    ap.add_argument("--hr-scratch")
    ap.add_argument("--out", required=True)
    ap.add_argument("--clips", default="")
    ap.add_argument("--skip-hr", action="store_true")
    ap.add_argument("--hr-only", action="store_true")
    ap.add_argument("--tag", default="")
    ap.add_argument("--cands", default="", help="comma list; plain always runs")
    ap.add_argument("--merge", action="store_true", help="read all rows_*.json in --out and write the tables")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    global ONLY
    ONLY = set(c for c in a.cands.split(",") if c) or None
    if a.merge:
        rows, rows2 = [], []
        for fn in sorted(os.listdir(a.out)):
            if fn.startswith("rows_") and fn.endswith(".json"):
                d = json.load(open(os.path.join(a.out, fn), encoding="utf8"))
                rows += d["clips"]
                rows2 += d["deband"]
        dedupe = lambda rs, key: list({tuple(r[k] for k in key): r for r in reversed(rs)}.values())[::-1]
        write(dedupe(rows, ("clip", "candidate")), dedupe(rows2, ("case", "candidate")), a.out)
        return 0
    clips = set(c for c in a.clips.split(",") if c)
    rows = [] if a.hr_only else part1(a.scratch, a.out, clips)
    rows2 = [] if a.skip_hr else part2(a.hr_scratch, clips)
    with open(os.path.join(a.out, "rows_%s.json" % (a.tag or "x")), "w", encoding="utf8", newline="\n") as f:
        json.dump(dict(clips=rows, deband=rows2), f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
