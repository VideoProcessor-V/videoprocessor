# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""S2 suite evaluator for the dither-need map (real module reference vs the doc-53 prototype).

  python -I dither_need_eval.py [--eval-dir D] [--size-frac 0.25] [--contexts sdr,hdr_pq,hdr_to_sdr]
         [--s2 sdr,hdr_pq_1000,hdr_pq_4000] [--ids a,b] [--max-cases N] [--out DIR] [--tag T]
  python -I dither_need_eval.py --merge [--out DIR]       # merge every dither_need_eval_*.csv in DIR into one report

Configs (all end on the 8-bit limited-range link, amplitude 0.15 base):
  plain    8-bit rounding, no dither (scale reference, 'nd')
  plain+4  stack_runner.finalise 'sd' (sigma-delta, amplitude 0.15)  <- the base every ratio / gate is against
  plain+4p the doc-53 PROTOTYPE amplitude map (amp_map + sd_adaptive copied from deband53_proto.py), reference only
  plain+4n the REAL module: dither_need_ref block law (block_inputs_from_luma on the coded luma, light-level LUT of the
           run context, Map state across the 2 frames) -> sample_extra per pixel -> sigma_delta_ref with array amplitude.

Documented choices for 4n:
  * Inputs (grain N, meanY, banding) come from the SOURCE-side evidence: the coded frames' luma at source resolution
    (10-bit codes), not from the signal after tone mapping. The LUT is that of the context of the SIGNAL the module reads
    (Sdr for sdr, the passthrough context for hdr_passthrough, HdrToSdr of the source peak for hdr_to_sdr), as
    deband53_proto.Ctx builds it.
  * Output raster == source raster: scale 1, offset 0, so SampleExtra runs on the same pixel grid.
  * q_out = 4 (the 8-bit link in 10-bit-scale codes, color_bits 8).
  * Frame 1: discontinuity (state reset to the target); frame 2: dt = FRAME_DT = 1/24 s (the suite's 24p cadence, the
    measured interval constant) with hysteresis and tau smoothing from Map.
  * Link noise: link_noise.link_noise on display PQ codes, shadow < 50 nit, class split on the source-side sigma_out.
    The case-level numbers are pooled over cases as sqrt(n-weighted mean of ratio^2); the worst case is shown too.
Gates (4n vs plain+4): A1 hdr_to_sdr, A2 hdr_passthrough, A3' sdr, A5 link noise (see gate_verdicts); each is judged per
(s2 context, run context) group.
Lldv_* S2 contexts have inputs identical to hdr_pq and are not run (reported as such).
"""
import argparse
import csv
import glob
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FRAME_DT = 1.0 / 24.0
Q_OUT = 4.0
S2_TO_RUN = {
    "sdr": [("sdr", "sdr")],
    "hdr_pq_1000": [("hdr_pq", "hdr_passthrough"), ("hdr_to_sdr", "hdr_to_sdr")],
    "hdr_pq_4000": [("hdr_pq", "hdr_passthrough"), ("hdr_to_sdr", "hdr_to_sdr")],
    "lldv_1000": [("hdr_pq", "hdr_passthrough"), ("hdr_to_sdr", "hdr_to_sdr")],
    "lldv_4000": [("hdr_pq", "hdr_passthrough"), ("hdr_to_sdr", "hdr_to_sdr")],
}
SKIPPED = {"lldv_1000": "hdr_pq_1000", "lldv_4000": "hdr_pq_4000"}
DEFAULT_S2 = ["sdr", "hdr_pq_1000", "hdr_pq_4000"]
CONFIGS = ["plain", "plain+4", "plain+4p", "plain+4n"]
COLS = ["case", "s2_context", "run_context", "config", "band_lt5_mse", "band_5to50_mse", "tband_lt5_mse",
        "tband_5to50_mse", "tex_lt5", "tex_5to50", "grain_rms_ratio", "grain_vs_plain4", "btb_kept", "wtw_kept",
        "link_noise_low", "link_noise_high", "n_low", "n_high", "extra_mean", "secs"]
# Prototype reference numbers (doc 53; geo-mean ratios vs plain+4): (<5 nit per frame, 5-50 nit per frame, 2-frame mean)
PROTO_REF = {("hdr_passthrough", "hdr_pq_1000"): (0.61, 0.68, 1.01),
             ("hdr_to_sdr", "hdr_pq_1000"): (0.33, 0.49, 0.59),
             ("hdr_to_sdr", "hdr_pq_4000"): (0.32, 0.57, 0.60)}
PROTO_GRAIN_HDR_G0P5 = (1.25, 1.37)

_L = {}


def _load(eval_dir):
    if eval_dir and os.path.abspath(eval_dir) != HERE:
        sys.path.insert(0, eval_dir)
    import numpy as np
    from scipy import ndimage
    import dither_need_ref as dn
    import light_level_ref as llr
    import link_noise as ln
    import s2_synth
    import sigma_delta_ref as sdr_
    import stack_runner as sr
    import tonemap_ref as tr
    _L.update(np=np, ndimage=ndimage, dn=dn, llr=llr, ln=ln, s2_synth=s2_synth, sd=sdr_, sr=sr, tr=tr)


# ---------------------------------------------------------------- prototype pieces (copied from deband53_proto.py)
def grain_split(y, scale=1.0, B=32, sub=4, quiet=4, nb=2, cal=2.2, kappa=2.0, floor=1e-3):
    np, ndimage = _L["np"], _L["ndimage"]
    g = lambda a, s: ndimage.gaussian_filter(a, s, mode="nearest", truncate=3.0)
    g1, gb = g(y, 1.0 * scale), g(y, 2.5 * scale)
    d = y - gb
    h, w = y.shape
    bh, bw = -(-h // B), -(-w // B)
    sb = B // sub
    pad = np.pad(d * d, ((0, bh * B - h), (0, bw * B - w)), mode="edge")
    e = pad.reshape(bh, sub, sb, bw, sub, sb).mean(axis=(2, 5))
    e = np.sort(e.transpose(0, 2, 1, 3).reshape(bh, bw, sub * sub), axis=2)
    qb = cal * e[:, :, :quiet].mean(axis=2)
    N = ndimage.minimum_filter(qb, size=2 * nb + 1, mode="nearest")
    Np = np.repeat(np.repeat(N, B, 0), B, 1)[:h, :w]
    e3 = ndimage.uniform_filter(d * d, 3, mode="nearest")
    f = np.clip(1.0 - kappa * Np / np.maximum(e3, floor), 0.0, 1.0)
    clean = gb + f * d
    return clean, y - clean


class Ctx:
    """Per run context: light-level LUT of the SIGNAL the dither-need / prototype read, output slope, steps."""

    def __init__(self, s2ctx, kind, peak):
        np, llr, s2_synth = _L["np"], _L["llr"], _L["s2_synth"]
        base = s2_synth.CONTEXTS[s2ctx]
        self.kind = kind
        if kind == "sdr":
            lc = llr.Context(kind="sdr", target_white=base["target_white"], target_black=base["target_black"])
        elif kind == "hdr_passthrough":
            lc = llr.Context.from_dict({k: v for k, v in base.items() if k != "eotf"})
        else:
            lc = llr.Context(kind="hdr_to_sdr", target_white=base["target_white"], target_black=base["target_black"],
                             src_peak=peak)
        self.lut = llr.build_lut(lc)
        self.q_out = Q_OUT
        self.slope = np.gradient(self.lut["sdr_code"]) if kind == "hdr_to_sdr" else np.ones(llr.NCODES)

    def at(self, arr, code):
        np = _L["np"]
        return np.interp(np.clip(code, 0, 1023), np.arange(_L["llr"].NCODES), arr)


def amp_map(G, y, cx):
    np, ndimage = _L["np"], _L["ndimage"]
    lp = ndimage.gaussian_filter(y, 2.0, mode="nearest")
    sig = np.sqrt(ndimage.gaussian_filter(G * G, 8.0, mode="nearest")) * cx.at(cx.slope, lp) / cx.q_out
    return np.clip(0.55 - sig, 0.15, 0.5)


def sd_adaptive(x10, amp):
    """Prototype quantiser (per-pixel amplitude map; same ign, same residual law)."""
    np, sd = _L["np"], _L["sd"]
    f = np.asarray(x10, np.float64) / 4.0
    lim = sd.limits(8, False)
    T, h, w = f.shape
    yy, xx = np.indices((h, w))
    r = np.zeros((h, w))
    out = np.empty(f.shape)
    for t in range(T):
        a = amp[t]
        d = (sd.ign(xx, yy, t).astype(np.float64) - 0.5) * 2.0 * a
        k = np.minimum(np.maximum(np.floor(f[t] + r + d + 0.5), lim[0]), lim[1])
        b = 0.5 + a
        r = np.minimum(np.maximum(f[t] + r - k, -b), b)
        out[t] = k * 4.0
    return out


# ---------------------------------------------------------------- the real module
def real_amplitude(coded, cx, fingerprint):
    """coded: (T,H,W) source luma (10-bit codes). Returns (amp (T,H,W) float64 = a_base + extra, sigma_out (T,gh,gw),
    B, extra_mean)."""
    np, dn = _L["np"], _L["dn"]
    state = dn.Map()
    amps, sigs, ex_means = [], [], []
    B = None
    for t, y in enumerate(coded):
        extra, grid, inp = dn.extra_map_for_frame(y, cx.lut, color_bits=8, state=state, dt=(0.0 if t == 0 else FRAME_DT),
                                                  discontinuity=(t == 0), lut_fingerprint=fingerprint)
        B = grid["B"]
        amps.append(dn.amp_map_full(extra, grid))
        sigs.append(np.sqrt(np.maximum(inp["N"], 0.0)) * np.abs(inp["Sout"]) / dn.q_out(8))
        ex_means.append(float(np.mean(amps[-1] - dn.A_BASE)))
    return np.stack(amps), np.stack(sigs), B, float(np.mean(ex_means))


def sd_real(x10, amp):
    """sigma_delta_ref.quantise_sequence with a per-frame per-pixel amplitude array (10-bit-scale in/out)."""
    np = _L["np"]
    return _L["sd"].quantise_sequence(np.asarray(x10, np.float64) / 4.0, 8, False, amp).astype(np.float64) * 4.0


# ---------------------------------------------------------------- one case
def run_case(case, s2ctx, size_frac, want_rc, rows, log):
    np, llr, s2_synth, sr, ln = _L["np"], _L["llr"], _L["s2_synth"], _L["sr"], _L["ln"]
    p = dict(case["master"]["params"], size_frac=size_frac)
    made = s2_synth.make_case(p)
    master = np.stack([f.Y for f in made["master"]])
    coded = np.stack([f.Y for f in made["coded"]])
    for rc, kind in S2_TO_RUN[s2ctx]:
        if rc not in want_rc:
            continue
        t0 = time.time()
        peak = made["context"].src_peak
        if kind == "hdr_to_sdr":
            ref, ectx = sr.tone_map("global", master, peak), sr.SDR_EVAL
        else:
            ref, ectx = master, (made["context"] if kind == "hdr_passthrough" else sr.SDR_EVAL)
        elut = llr.build_lut(ectx)
        cx = Ctx(s2ctx, kind, peak)
        xo = sr.tone_map("global", coded, peak) if kind == "hdr_to_sdr" else coded
        amp_p = np.stack([amp_map(grain_split(f)[1], f, cx) for f in coded])
        amp_n, sig_out, B, extra_mean = real_amplitude(coded, cx, "%s/%s" % (s2ctx, kind))
        outs = {"plain": sr.finalise(xo, "nd"), "plain+4": sr.finalise(xo, "sd"),
                "plain+4p": sd_adaptive(xo, amp_p), "plain+4n": sd_real(xo, amp_n)}
        secs = time.time() - t0
        extras = {"plain+4p": float(np.mean(amp_p - 0.15)), "plain+4n": extra_mean}
        for name, out in outs.items():
            plain_same = outs["plain"] if name == "plain" else outs["plain+4"]
            r = sr.evaluate(ref, plain_same, out, ectx, elut)
            bm = sr.lm.shadow_banding(ref.mean(axis=0), out.mean(axis=0), ectx, sr.RANGES, elut)
            r["tband_lt5_mse"], r["tband_5to50_mse"] = bm["lt5"]["band_mse"], bm["5to50"]["band_mse"]
            r.update(case=case["id"], s2_context=s2ctx, run_context=rc, config=name, secs=secs,
                     extra_mean=extras.get(name))
            if name in extras:
                r["grain_vs_plain4"] = sr.grain_stats(ref, outs["plain+4"], out, ectx, elut)[0]
                ln_r = ln.link_noise(ref, out, outs["plain+4"], sig_out, B, ectx, elut)
                r.update(link_noise_low=ln_r["low"], link_noise_high=ln_r["high"], n_low=ln_r["n_low"],
                         n_high=ln_r["n_high"])
            rows.append(r)
        log("%s %s/%s done in %.1f s (extra mean 4n %.4f, 4p %.4f)" % (case["id"], s2ctx, rc, secs, extra_mean, extras["plain+4p"]))


# ---------------------------------------------------------------- reporting
def fnum(v):
    return "" if v is None else (v if isinstance(v, str) else "%.6g" % v)


def write_csv(rows, path):
    with open(path, "w", newline="", encoding="utf8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(COLS)
        for r in rows:
            w.writerow([fnum(r.get(c)) for c in COLS])


def read_csv(path):
    rows = []
    with open(path, newline="", encoding="utf8") as f:
        for r in csv.DictReader(f):
            for k, v in list(r.items()):
                if k in ("case", "s2_context", "run_context", "config"):
                    continue
                r[k] = None if v in ("", None) else float(v)
            rows.append(r)
    return rows


def geomean(v):
    v = [x for x in v if x is not None and x > 0]
    return math.exp(sum(math.log(x) for x in v) / len(v)) if v else None


def case_ratios(rows, group, cfg, metric, base="plain+4"):
    """[(case, cfg/base)] for one (s2_context, run_context) group where both are defined and base > 0."""
    idx = {(r["case"], r["config"]): r for r in rows if (r["s2_context"], r["run_context"]) == group}
    out = []
    for (c, k), r in idx.items():
        if k != cfg:
            continue
        b = idx.get((c, base))
        if b and r.get(metric) is not None and b.get(metric):
            out.append((c, r[metric] / b[metric]))
    return out


def pooled_link(rows, group, cfg="plain+4n"):
    res = {}
    for cls in ("low", "high"):
        num = den = 0.0
        mx = None
        for r in rows:
            if (r["s2_context"], r["run_context"]) == group and r["config"] == cfg and r.get("link_noise_" + cls) is not None:
                n = r.get("n_" + cls) or 0
                num += n * r["link_noise_" + cls] ** 2
                den += n
                mx = max(mx or 0.0, r["link_noise_" + cls])
        res[cls] = (math.sqrt(num / den) if den else None, mx)
    return res


def f3(v):
    return "n/a" if v is None else "%.3f" % v


def groups_of(rows):
    return sorted({(r["s2_context"], r["run_context"]) for r in rows}, key=lambda g: (g[1], g[0]))


def gate_verdicts(rows):
    """List of (gate, group, passed, detail)."""
    import link_noise as ln
    out = []
    for g in groups_of(rows):
        rc = g[1]
        lt5 = case_ratios(rows, g, "plain+4n", "band_lt5_mse")
        b50 = case_ratios(rows, g, "plain+4n", "band_5to50_mse")
        tb = case_ratios(rows, g, "plain+4n", "tband_lt5_mse")
        if rc == "hdr_to_sdr":
            gm, mx = geomean([x for _, x in lt5]), max([x for _, x in lt5], default=None)
            ok = gm is not None and gm <= 0.6 and mx <= 1.02
            out.append(("A1 HDR->SDR (<5 nit geo-mean <= 0.6, no case > 1.02)", g, ok,
                        "geo %s, worst case %s, n %d" % (f3(gm), f3(mx), len(lt5))))
        if rc == "hdr_pq":
            gm, tg = geomean([x for _, x in lt5]), geomean([x for _, x in tb])
            ok = gm is not None and tg is not None and gm <= 0.8 and tg <= 1.02
            out.append(("A2 HDR passthrough (per-frame <5 geo <= 0.8, 2-frame mean <= 1.02)", g, ok,
                        "per-frame geo %s, 2-frame geo %s, n %d" % (f3(gm), f3(tg), len(lt5))))
        if rc == "sdr":
            m1, m2 = max([x for _, x in lt5], default=None), max([x for _, x in b50], default=None)
            ok = (m1 is None or m1 <= 1.02) and (m2 is None or m2 <= 1.02) and bool(lt5 or b50)
            out.append(("A3' SDR (every case <= 1.02, <5 and 5-50 nit)", g, ok,
                        "worst <5 %s, worst 5-50 %s, n %d/%d" % (f3(m1), f3(m2), len(lt5), len(b50))))
        pl = pooled_link(rows, g)
        v = ln.verdict({"low": pl["low"][0], "high": pl["high"][0]})
        out.append(("A5 link noise (low <= 1.40, high <= 1.05)", g, v["ok"],
                    "low %s (max %s), high %s (max %s)" % (f3(pl["low"][0]), f3(pl["low"][1]), f3(pl["high"][0]), f3(pl["high"][1]))))
    return out


def build_md(rows, meta):
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Dither-need map: S2 evaluation (real module vs prototype)", "", meta, "",
          "Ratios are config / plain+4 (same case, same run context), geo-mean over cases where both are defined; < 1 is better. "
          "4n = real module, 4p = doc-53 prototype amplitude map (reference). Skipped contexts: lldv_1000 = hdr_pq_1000 "
          "(identical input), lldv_4000 = hdr_pq_4000 (identical input).", ""]
    gs = groups_of(rows)
    md += ["## Geo-mean ratios vs plain+4", "",
           "| s2 context | run context | config | <5 nit per frame | 5-50 nit per frame | 2-frame mean <5 | cases better (<5) | grain vs plain+4 (min-max) |",
           "|---|---|---|---|---|---|---|---|"]
    for g in gs:
        for cfg in ("plain+4n", "plain+4p"):
            a, b, c = (case_ratios(rows, g, cfg, m) for m in ("band_lt5_mse", "band_5to50_mse", "tband_lt5_mse"))
            gv = [r["grain_vs_plain4"] for r in rows if (r["s2_context"], r["run_context"]) == g and r["config"] == cfg
                  and r.get("grain_vs_plain4") is not None]
            md.append("| %s | %s | %s | %s | %s | %s | %d/%d | %s |" % (
                g[0], g[1], cfg, f3(geomean([x for _, x in a])), f3(geomean([x for _, x in b])), f3(geomean([x for _, x in c])),
                sum(x < 1 for _, x in a), len(a), ("%.3f - %.3f" % (min(gv), max(gv))) if gv else "n/a"))
    md += ["", "## Link noise (high-pass rms of output error vs plain+4; limits low 1.40, high 1.05)", "",
           "| s2 context | run context | config | low (pooled) | low max | high (pooled) | high max | px low / high |",
           "|---|---|---|---|---|---|---|---|"]
    for g in gs:
        for cfg in ("plain+4n", "plain+4p"):
            pl = pooled_link(rows, g, cfg)
            sel = [r for r in rows if (r["s2_context"], r["run_context"]) == g and r["config"] == cfg]
            md.append("| %s | %s | %s | %s | %s | %s | %s | %d / %d |" % (
                g[0], g[1], cfg, f3(pl["low"][0]), f3(pl["low"][1]), f3(pl["high"][0]), f3(pl["high"][1]),
                sum((r.get("n_low") or 0) for r in sel), sum((r.get("n_high") or 0) for r in sel)))
    md += ["", "## GATE VERDICTS (config 4n vs plain+4)", "", "| gate | s2 context | run context | verdict | detail |",
           "|---|---|---|---|---|"]
    for gate, g, ok, detail in gate_verdicts(rows):
        md.append("| %s | %s | %s | %s | %s |" % (gate, g[0], g[1], "PASS" if ok else "FAIL", detail))
    md += ["", "## Real vs prototype", "",
           "| s2 context | run context | 4n <5 / 5-50 / 2-frame | 4p (this run) <5 / 5-50 / 2-frame | prototype reference (doc 53) |",
           "|---|---|---|---|---|"]
    for g in gs:
        def trip(cfg):
            return " / ".join(f3(geomean([x for _, x in case_ratios(rows, g, cfg, m)]))
                              for m in ("band_lt5_mse", "band_5to50_mse", "tband_lt5_mse"))
        ref = PROTO_REF.get((g[1], g[0]))
        md.append("| %s | %s | %s | %s | %s |" % (g[0], g[1], trip("plain+4n"), trip("plain+4p"),
                                                   ("%.2f / %.2f / %.2f" % ref) if ref else "-"))
    md += ["", "Prototype grain vs plain+4 on HDR grain 0.5 cases: %.2f - %.2f. This run, grain 0.5 (g0p5) cases:" % PROTO_GRAIN_HDR_G0P5, ""]
    for g in gs:
        if g[1] == "sdr":
            continue
        vals = {}
        for cfg in ("plain+4n", "plain+4p"):
            gv = [r["grain_vs_plain4"] for r in rows if (r["s2_context"], r["run_context"]) == g and r["config"] == cfg
                  and r["case"].endswith("g0p5") and r.get("grain_vs_plain4") is not None]
            vals[cfg] = ("%.3f - %.3f" % (min(gv), max(gv))) if gv else "n/a"
        md.append("- %s / %s: 4n %s, 4p %s" % (g[0], g[1], vals["plain+4n"], vals["plain+4p"]))
    md += ["", "## Timing (seconds per case-context, wall clock: signal build + amplitude maps + quantisers, metrics excluded)", "",
           "| case | s2 context | run context | secs |", "|---|---|---|---|"]
    seen = set()
    tot = 0.0
    for r in rows:
        k = (r["case"], r["s2_context"], r["run_context"])
        if k not in seen and r.get("secs") is not None:
            seen.add(k)
            tot += r["secs"]
            md.append("| %s | %s | %s | %.1f |" % (k + (r["secs"],)))
    md += ["", "Total %.0f s over %d case-contexts (metrics evaluation not included)." % (tot, len(seen)), ""]
    return "\n".join(md)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eval-dir", default=HERE)
    ap.add_argument("--size-frac", type=float, default=0.25)
    ap.add_argument("--contexts", default="sdr,hdr_pq,hdr_to_sdr")
    ap.add_argument("--ids", default="")
    ap.add_argument("--s2", default="")
    ap.add_argument("--max-cases", type=int, default=0)
    ap.add_argument("--out", default=r"D:\codex\videoprocessor\dither-need-results")
    ap.add_argument("--tag", default="")
    ap.add_argument("--merge", action="store_true")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    _load(a.eval_dir)
    if a.merge:
        rows = []
        files = [f for f in sorted(glob.glob(os.path.join(a.out, "dither_need_eval_*.csv"))) if "merged" not in os.path.basename(f)]
        for f in files:
            rows += read_csv(f)
        write_csv(rows, os.path.join(a.out, "dither_need_eval_merged.csv"))
        with open(os.path.join(a.out, "dither_need_eval_merged.md"), "w", encoding="utf8", newline="\n") as f:
            f.write(build_md(rows, "Merged from: " + ", ".join(os.path.basename(x) for x in files)))
        print("merged %d files, %d rows" % (len(files), len(rows)))
        return rows
    sr = _L["sr"]
    want_rc = [c for c in a.contexts.split(",") if c]
    s2 = [c for c in a.s2.split(",") if c] if a.s2 else DEFAULT_S2
    for c in s2:
        if c in SKIPPED:
            print("%s = %s (identical input), not run" % (c, SKIPPED[c]))
    s2 = [c for c in s2 if c not in SKIPPED]
    suite = sr.load_suite("s2")
    cases = [c for c in suite["cases"] if c["master"]["params"]["context"] in s2]
    if a.ids:
        cases = [c for c in cases if c["id"] in a.ids.split(",")]
    if a.max_cases:
        cases = cases[:a.max_cases]
    tag = a.tag or "%s_%s" % (a.contexts.replace(",", "-"), a.s2.replace(",", "-") or "all")
    rows, lines = [], []

    def log(s):
        lines.append(s)
        print(s, flush=True)

    t0 = time.time()
    for c in cases:
        run_case(c, c["master"]["params"]["context"], a.size_frac, want_rc, rows, log)
    log("rows %d, wall %.0f s" % (len(rows), time.time() - t0))
    base = os.path.join(a.out, "dither_need_eval_" + tag)
    write_csv(rows, base + ".csv")
    with open(base + ".md", "w", encoding="utf8", newline="\n") as f:
        f.write(build_md(rows, "Run: size_frac %g, contexts %s, s2 %s, %d cases, wall %.0f s." % (
            a.size_frac, a.contexts, ",".join(s2), len(cases), time.time() - t0)))
    with open(base + ".log", "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return rows


if __name__ == "__main__":
    main()
