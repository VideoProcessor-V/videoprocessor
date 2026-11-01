# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the no-reference metrics (nr_metrics.py) against full-reference measures on a streaming suite.

For every sampled frame of every case the NR metric is computed on the DECODED picture alone; the full-reference
counterpart uses the master:
  band   fr_band   mean |gauss(err, 3)| (8-bit units) in the reference's smooth gradient mask (staircase error)
  block  fr_block  excess error discontinuity at the 8-px deblocking grid (phase known from the crop) vs inside blocks
  ring   fr_ring   rms of the 1-px high-pass of the error in the reference's ring zone beside strong edges
  noise  fr_noise_loss  1 - sigma(decode)/sigma(ref) over the reference's flat mask
  poor   1 - SSIM (luma) against the NR poor index
Reports pooled and within-clip Spearman correlations and, per metric, the AUC for telling the lowest rung from
the top rung of the same clip; --thresholds writes nr_thresholds.json ("poor" level per artefact).

  python -I tools/eval/nr_validate.py --suite tools/eval/suites/streaming_hevc.json [--suite ...] \
      --out tools/eval/streaming_validation [--stride 8] [--thresholds]
"""
import argparse
import csv
import json
import os
import sys

import numpy as np
from scipy import ndimage as ndi
from scipy import stats

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eval_harness as eh  # noqa: E402
import eval_metrics as em  # noqa: E402
import nr_metrics as nr  # noqa: E402

CHUNK = 16


def fr_frame(ref, dec, bd, crop):
    """Full-reference counterparts for one luma frame pair (floats; None where not measurable)."""
    r, d = nr.to_u8(ref, bd), nr.to_u8(dec, bd)
    err = d - r
    out = {}
    # band: staircase error in smooth gradient areas of the reference
    sig, _ = nr.noise_sigma(r)
    hp_r = r - ndi.gaussian_filter(r, 1.0)
    smooth = ndi.uniform_filter(hp_r * hp_r, 5) <= 2.0 * max(sig, 0.1) ** 2
    gy, gx = np.gradient(ndi.gaussian_filter(r, 2.0))
    g = np.hypot(gx, gy)
    mask = smooth & (g >= 0.0125) & (g <= 0.75)
    out["fr_band"] = float(np.abs(ndi.gaussian_filter(err, 3.0))[mask].mean()) if mask.mean() > 0.005 else None
    # block: error steps on the 8-px grid
    vals = []
    for axis, off in ((1, crop[0]), (0, crop[1])):
        de = np.abs(np.diff(err, axis=axis))
        pos = np.arange(de.shape[axis])
        on = ((pos + 1 + off) % 8) == 0
        m_on = de.take(np.nonzero(on)[0], axis=axis).mean()
        m_off = de.take(np.nonzero(~on)[0], axis=axis).mean()
        vals.append(m_on / max(m_off, 0.02) - 1.0)
    out["fr_block"] = float(max(np.mean(vals), 0.0))
    # ring: error high-pass beside strong edges of the reference
    _, zone, _ = nr.edge_zone(r)
    if zone.sum() >= 256:
        hp_e = err - ndi.gaussian_filter(err, 1.0)
        out["fr_ring"] = float(np.sqrt(np.mean(hp_e[zone] ** 2)))
    else:
        out["fr_ring"] = None
    # noise kept in the reference's flat areas
    hp_r2, fm = nr.flat_mask(r)
    if fm.sum() >= 64:
        hp_d = d - ndi.gaussian_filter(d, 1.0)
        s_r = 1.4826 * np.median(np.abs(hp_r2[fm] - np.median(hp_r2[fm])))
        s_d = 1.4826 * np.median(np.abs(hp_d[fm] - np.median(hp_d[fm])))
        out["fr_noise_loss"] = float(1.0 - s_d / s_r) if s_r > 0.05 else None
    else:
        out["fr_noise_loss"] = None
    out["fr_ssim_loss"] = 1.0 - em.ssim(ref[None], dec[None], bd)
    return out


def case_rows(case, stride, max_frames, log):
    n = case.get("frames", 4)
    if max_frames:
        n = min(n, max_frames)
    crop = [int(v) for v in case.get("streaming", {}).get("crop", [0, 0])[:2]]
    rows = []
    for c0 in range(0, n, CHUNK):
        cnt = min(CHUNK, n - c0)
        ms = eh.load_source(case["master"], cnt, start=c0)
        cs = eh.load_source(case["coded"], cnt, start=c0)
        for k, (m, c) in enumerate(zip(ms, cs)):
            i = c0 + k
            if i % stride:
                continue
            row = dict(case=case["id"], clip=case["clip"], frame=i)
            row.update(nr.nr_frame(c.Y, c.bitdepth))
            row.update(fr_frame(m.Y, c.Y, c.bitdepth, crop))
            rows.append(row)
    log("  %s: %d frames sampled" % (case["id"], len(rows)))
    return rows


def spearman(x, y):
    x, y = np.asarray(x, float), np.asarray(y, float)
    ok = np.isfinite(x) & np.isfinite(y)
    if ok.sum() < 8 or np.ptp(x[ok]) == 0 or np.ptp(y[ok]) == 0:
        return None
    return float(stats.spearmanr(x[ok], y[ok])[0])


def within_clip(rows, kx, ky):
    xs, ys = [], []
    for clip in sorted({r["clip"] for r in rows}):
        sub = [r for r in rows if r["clip"] == clip and r.get(kx) is not None and r.get(ky) is not None]
        if len(sub) < 4:
            continue
        x = np.array([r[kx] for r in sub], float)
        y = np.array([r[ky] for r in sub], float)
        xs.extend(x - x.mean())
        ys.extend(y - y.mean())
    return spearman(xs, ys), len(xs)


def auc(low, top):
    """P(value in `low` > value in `top`) (ties half): how well a metric flags the low rung."""
    low, top = np.asarray(low, float), np.asarray(top, float)
    if len(low) == 0 or len(top) == 0:
        return None
    gt = (low[:, None] > top[None, :]).mean()
    eq = (low[:, None] == top[None, :]).mean()
    return float(gt + 0.5 * eq)


PAIRS = [("band", "nr_band", "fr_band"), ("block", "nr_block", "fr_block"), ("ring", "nr_ring", "fr_ring"),
         ("noise", "nr_noise", "fr_noise_loss")]


def analyse(rows, cases):
    """rows -> (correlation table rows, rung AUC rows, thresholds)."""
    corr = []
    for name, kx, ky in PAIRS:
        sub = [r for r in rows if r.get(ky) is not None]
        pooled = spearman([r[kx] for r in sub], [r[ky] for r in sub])
        wc, n = within_clip(rows, kx, ky)
        corr.append(dict(metric=name, nr=kx, fr=ky, n=len(sub), pooled_spearman=pooled, within_clip_spearman=wc))
    thr = nr.load_thresholds()
    # AUC low vs top rung of the same clip+preset+mode
    groups = {}
    for c in cases:
        s = c.get("streaming", {})
        if "rung" in s:
            groups.setdefault((c["clip"], s["preset"], s["mode"]), []).append(c)
    by_case = {}
    for r in rows:
        by_case.setdefault(r["case"], []).append(r)
    low_rows, top_rows = [], []
    for key, cs in sorted(groups.items()):
        cs = sorted(cs, key=lambda c: -c["streaming"]["target_kbps"])
        if len(cs) < 2:
            continue
        top_rows += by_case.get(cs[0]["id"], [])
        low_rows += by_case.get(cs[-1]["id"], [])
    aucs, poor = [], {}
    for name, kx, _ in PAIRS:
        lo = [r[kx] for r in low_rows]
        tp = [r[kx] for r in top_rows]
        aucs.append(dict(metric=name, auc_low_vs_top=auc(lo, tp), n_low=len(lo), n_top=len(tp),
                         median_top=float(np.median(tp)) if tp else None, median_low=float(np.median(lo)) if lo else None))
        if name != "noise" and lo and tp:
            hi_top, med_low = float(np.quantile(tp, 0.9)), float(np.median(lo))
            poor[name] = round(0.5 * (hi_top + med_low), 6) if med_low > hi_top else round(med_low, 6)
    # composite vs SSIM loss
    if poor:
        th = {"poor": poor}
        pi = [nr.poor_index(r, th) for r in rows]
        comp = dict(metric="poor_index", nr="poor_index", fr="fr_ssim_loss", n=len(rows),
                    pooled_spearman=spearman(pi, [r["fr_ssim_loss"] for r in rows]))
        for r, v in zip(rows, pi):
            r["poor_index"] = v
        comp["within_clip_spearman"] = within_clip(rows, "poor_index", "fr_ssim_loss")[0]
        corr.append(comp)
        aucs.append(dict(metric="poor_index", auc_low_vs_top=auc([r["poor_index"] for r in low_rows],
                                                                  [r["poor_index"] for r in top_rows]),
                         n_low=len(low_rows), n_top=len(top_rows), median_top=None, median_low=None))
    return corr, aucs, poor


def fmt(v):
    return "NA" if v is None else ("%.3f" % v)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", action="append", required=True)
    ap.add_argument("--out", required=True, help="output prefix for _frames.csv (scratch) / _corr.md / _corr.csv")
    ap.add_argument("--stride", type=int, default=8)
    ap.add_argument("--max-frames", type=int, default=None)
    ap.add_argument("--thresholds", action="store_true", help="write nr_thresholds.json from the rung separation")
    ap.add_argument("--skip-switch", action="store_true", default=True)
    a = ap.parse_args(argv)
    cases, rows = [], []
    for sp in a.suite:
        with open(sp, "r", encoding="utf8") as f:
            suite = json.load(f)
        for c in suite["cases"]:
            if "plan" in c.get("streaming", {}):
                continue
            cases.append(c)
            rows += case_rows(c, a.stride, a.max_frames, print)
    corr, aucs, poor = analyse(rows, cases)
    with open(a.out + "_frames.csv", "w", encoding="utf8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=sorted({k for r in rows for k in r}))
        w.writeheader()
        w.writerows(rows)
    lines = ["| metric | NR vs FR | frames | pooled Spearman | within-clip Spearman |", "|---|---|---|---|---|"]
    for c in corr:
        lines.append("| %s | %s vs %s | %d | %s | %s |" % (c["metric"], c["nr"], c["fr"], c["n"], fmt(c["pooled_spearman"]),
                                                          fmt(c["within_clip_spearman"])))
    lines += ["", "| metric | AUC lowest vs top rung | median top | median low | n low / top |", "|---|---|---|---|---|"]
    for q in aucs:
        lines.append("| %s | %s | %s | %s | %d / %d |" % (q["metric"], fmt(q["auc_low_vs_top"]), fmt(q["median_top"]),
                                                          fmt(q["median_low"]), q["n_low"], q["n_top"]))
    lines += ["", "poor thresholds: " + json.dumps(poor, sort_keys=True)]
    with open(a.out + "_corr.md", "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    if a.thresholds and poor:
        with open(nr.THRESHOLDS_PATH, "w", encoding="utf8", newline="\n") as f:
            json.dump({"poor": poor, "method": "midpoint of top-rung p90 and lowest-rung median per artefact "
                       "(nr_validate.py); a metric that does not separate the rungs gets the lowest-rung median",
                       "suites": [os.path.basename(s) for s in a.suite]}, f, indent=1, sort_keys=True)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
