# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 stack test for the grain-safe SPATIAL mosquito / ringing repair (candidates_mosquito.
RepairMosquitoSpatial, the variant that passed the near-edge grain gate) in two placements:
  early  on the decoded source (signal domain, before texture re-injection and tone mapping)
  late   on the post-tone-mapping SDR output (after the amplifying stages, before dither)
  both
crossed with texture re-injection (none / oracle) and tone stage (plain, crgs 0.30 = grain-safe contrast
recovery, pe = grain-safe pre-emphasis).  HDR masters (4), x265-10 CRF 30, 1080p raster, 6 frames, from
the artefact_headroom cache.  All metrics on the post-TM SDR output (reference = master through the same
stage, so the stage's own look is not counted as error):
  ring     near-edge ringing energy: mean squared 1-px residual of the error in the master's near-edge band
           where the master is locally smooth (mosquito_headroom rule), SDR codes^2
  halo     halo_overshoot vs the plain curve on the same (unrepaired, same texture) input
  lc       local contrast of the output PQ vs the source PQ (LTM-study metric)
  keep_in_edge / keep_in_flat   fine-band energy vs the same stage without any ringing repair
  psnr     vs the master through the same stage
  py_ms    ms per frame of the repair itself (Python; the repair is cost class medium-low: 2 bilaterals)

  python -I tools/eval/ringing_stack.py --hr-scratch <artefact_headroom scratch> --out tools/eval/prototypes/ringing_stack
"""
import argparse
import csv
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import artefact_headroom as ah  # noqa: E402
import candidates_mosquito as cm  # noqa: E402
import deband_stack as ds  # noqa: E402
import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402

NFR = 6
CASES = [("sol_sky", 1000.0), ("sol_dark", 1000.0), ("nocturne_dark", 4000.0), ("nocturne_mid", 4000.0)]
ENC = "x265-10_crf30"
STAGES = ["plain", "crgs 0.30", "pe"]
PLACES = ["off", "early", "late", "both"]
_MS = []


def mosquito(seq):
    rep = cm.RepairMosquitoSpatial()
    rep.reset({})
    out = []
    t0 = time.perf_counter()
    for t, y in enumerate(seq):
        c = np.zeros((y.shape[0] // 2, y.shape[1] // 2))
        f = eio.Frame(Y=y, Cb=c + 512.0, Cr=c + 512.0, bitdepth=10, index=t)
        out.append(rep.process(f).Y)
    _MS.append((time.perf_counter() - t0) * 1000.0 / len(seq))
    return np.stack(out)


def ring_energy(ref, X):
    tot, n = 0.0, 0
    for r, x in zip(ref, X):
        zone, _ = em.near_edge_zone(r, 10)
        hr = r - ndimage.gaussian_filter(r, 1.0, mode="nearest")
        loc = ndimage.uniform_filter(hr * hr, size=5, mode="nearest")
        sig = ah.noise_sigma(r)
        sm = zone & (loc <= 2.0 * sig * sig)
        e = x - r
        he = e - ndimage.gaussian_filter(e, 1.0, mode="nearest")
        tot += float((he[sm] ** 2).sum())
        n += int(sm.sum())
    return tot / n if n else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hr-scratch", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for name, peak in CASES:
        M = ah.luma_seq(os.path.join(a.hr_scratch, "%s_1080p_yuv420p10le.yuv" % name), "yuv420p10le", NFR)
        D = ah.luma_seq(os.path.join(a.hr_scratch, "%s_1080p_%s.yuv" % (name, ENC)), "yuv420p10le", NFR)
        zones = [em.near_edge_zone(m, 10) for m in M]
        edge, flat = [z[0] for z in zones], [z[1] for z in zones]
        src = {"none": D, "oracle": ds.pre_variant("oracle", D, M, True)}
        early = {k: mosquito(v) for k, v in src.items()}
        for st in STAGES:
            Mo = ds.stage(st, M, peak)
            for tex in ("none", "oracle"):
                plain_in = ds.perceptual(ds.stage("plain", src[tex], peak))
                base = ds.stage(st, src[tex], peak)  # no repair
                outs = {"off": base, "early": ds.stage(st, early[tex], peak)}
                outs["late"] = mosquito(base)
                outs["both"] = mosquito(outs["early"])
                for pl in PLACES:
                    X = outs[pl]
                    P = ds.perceptual(X)
                    lc = em.local_contrast_ratio(M, P, 10)
                    row = dict(case=name, stage=st, texture=tex, ringing_repair=pl,
                               ring=ring_energy(Mo, X), halo=em.halo_overshoot(plain_in, P, 10),
                               lc=float(np.mean([lc["ratio_s%d" % k] for k in (1, 2, 3)])),
                               keep_in_edge=ds.energy(X, edge) / max(ds.energy(base, edge), 1e-9),
                               keep_in_flat=ds.energy(X, flat) / max(ds.energy(base, flat), 1e-9),
                               psnr=em.psnr(Mo, X, 10))
                    rows.append(row)
                    print(json.dumps({k: (round(v, 5) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
    write(rows, a.out)
    return 0


def write(rows, out):
    cols = ["case", "stage", "texture", "ringing_repair", "ring", "halo", "lc", "keep_in_edge", "keep_in_flat", "psnr"]
    fmt = lambda v: "-" if v is None else ("%.5g" % v if isinstance(v, float) else str(v))
    with open(os.path.join(out, "ringing_stack.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in cols])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Ringing repair in the stack (post-TM SDR output; 4 HDR masters, x265-10 CRF 30, 1080p)", "",
          "ring = near-edge smooth-band ringing energy, summed over cases relative to (same stage, same texture, "
          "repair off); also relative to plain/none/off in brackets. repair py ms/frame mean: %.0f. See ringing_stack.py." %
          (float(np.mean(_MS)) if _MS else float("nan")), "",
          "| stage | texture | ringing repair | ring vs off (vs plain/none/off) | halo | lc | keep near-edge / flat vs off | PSNR |",
          "|---|---|---|---|---|---|---|---|"]
    ref0 = sum(r["ring"] for r in rows if (r["stage"], r["texture"], r["ringing_repair"]) == ("plain", "none", "off"))
    for st in STAGES:
        for tex in ("none", "oracle"):
            off = sum(r["ring"] for r in rows if (r["stage"], r["texture"], r["ringing_repair"]) == (st, tex, "off"))
            for pl in PLACES:
                rs = [r for r in rows if (r["stage"], r["texture"], r["ringing_repair"]) == (st, tex, pl)]
                ring = sum(r["ring"] for r in rs)
                m = lambda c: float(np.mean([r[c] for r in rs if r.get(c) is not None]))
                md.append("| %s | %s | %s | %.3f (%.3f) | %.5f | %.3f | %.3f / %.3f | %.2f |" % (
                    st, tex, pl, ring / off, ring / ref0, m("halo"), m("lc"), m("keep_in_edge"), m("keep_in_flat"),
                    m("psnr")))
    md += ["", "## All rows", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in cols) + " |")
    md.append("")
    with open(os.path.join(out, "ringing_stack.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    sys.exit(main())
