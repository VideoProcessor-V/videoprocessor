# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tables (markdown) for the TemporalDetail real-vector test from the JSON results under D:/codex/td-real-vectors-out.

  python -I tools/eval/td_vectors/summarize.py --root D:/codex/td-real-vectors-out --out RESULTS.md
"""
import argparse
import glob
import json
import math
import os
import sys
from collections import defaultdict


def db(mse):
    return 10 * math.log10(1023.0 ** 2 / mse) if mse > 0 else float("nan")


def f(v, nd=3, sign=True):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "NA"
    return ("%+.*f" if sign else "%.*f") % (nd, v)


def ad_tables(root):
    out = []
    files = sorted(glob.glob(os.path.join(root, "ad", "*.json")))
    if not files:
        return out
    out.append("## 1. Frame types: quality and fine-band detail vs master (full 1080p frames, frames 2..69)\n")
    out.append("fine band H = x - Gaussian(sigma 1 px). `keep` = E[H(C)^2]/E[H(M)^2] (detail energy kept, can exceed 1 "
               "with coding noise); `hb err` = E[H(M-C)^2] (10-bit codes^2); `grain/incoh` = temporally incoherent master "
               "fine band (grain, noise, new content); `struct err` = E[H(S-C)^2] against the master's 5-frame motion-"
               "compensated structure S.\n")
    out.append("| case | kb/s | PSNR I | PSNR P | PSNR B | gap I-B | gap IP-B | keep I / P / B | hb err I / P / B | grain/incoh B | struct err B |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|")
    rec = []
    seen = set()
    for p in files:
        d = json.load(open(p, encoding="utf8"))
        c = d["case"]
        by = defaultdict(list)
        for r in d["rows"]:
            by[r["type"]].append(r)
        mean = lambda t, k: sum(r[k] for r in by[t]) / len(by[t]) if by[t] else float("nan")
        if c["id"] not in seen:
            seen.add(c["id"])
            ip = by["I"] + by["P"]
            mip = sum(r["mse"] for r in ip) / len(ip)
            out.append("| %s | %d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f / %.2f / %.2f | %.0f / %.0f / %.0f | %.0f | %.0f |" % (
                c["id"], c["kbps"], db(mean("I", "mse")) if by["I"] else float("nan"), db(mean("P", "mse")), db(mean("B", "mse")),
                (db(mean("I", "mse")) - db(mean("B", "mse"))) if by["I"] else float("nan"), db(mip) - db(mean("B", "mse")),
                mean("I", "Ec") / mean("I", "Em") if by["I"] else float("nan"), mean("P", "Ec") / mean("P", "Em"),
                mean("B", "Ec") / mean("B", "Em"),
                mean("I", "Eerr") if by["I"] else float("nan"), mean("P", "Eerr"), mean("B", "Eerr"), mean("B", "incoh"),
                mean("B", "struct_err")))
        for s in d["sources"]:
            for t in ("B", "P"):
                rows = [r for r in by[t] if (s + "|hb") in r]
                if not rows:
                    continue
                mse = sum(r["mse"] for r in rows) / len(rows)
                ip = by["I"] + by["P"]
                gap = db(sum(r["mse"] for r in ip) / len(ip)) - db(sum(r["mse"] for r in by["B"]) / len(by["B"]))
                g = {}
                for k in ("hb", "full", "hb_mref", "hb_align"):
                    if (s + "|" + k) in rows[0]:
                        red = sum(r[s + "|" + k] for r in rows) / len(rows)
                        g[k] = db(mse - red) - db(mse)
                wp = [v for r in rows for kk, v in r.items() if kk.startswith(s + "|warp_psnr")]
                rec.append((c["id"], s, t, g, gap, sum(wp) / len(wp) if wp else float("nan")))
    out.append("")
    out.append("## 2. Recoverable from the decoded I/P references after motion compensation (per-block ORACLE ceilings)\n")
    out.append("References: previous + next anchor (I/P), plus the nearest I frame within 12 frames for B. Per 16 px block the best "
               "reference and strength a in [0,1] are chosen WITH the master, so these are upper bounds. dB = PSNR gain on that "
               "frame type. `% gap` = hb gain / (PSNR(I+P) - PSNR(B)). `warp` = mean PSNR (10-bit peak) of a warped decoded "
               "reference against the master frame (alignment quality). Sources: `X:coded` = vectors from the decoded frames "
               "(engine), `X:master` = vectors from the uncompressed master applied to the decoded frames (near-ideal).\n")
    out.append("| case | vectors | type | hb transfer | % gap | +align (vector-error-free) | full blend | perfect ref (master ref) | warp dB |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for cid, s, t, g, gap, wp in rec:
        out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %.2f |" % (
            cid, s, t, f(g.get("hb")), ("%.0f%%" % (100 * g["hb"] / gap)) if t == "B" and gap > 0.05 and "hb" in g else "-",
            f(g.get("hb_align")), f(g.get("full")), f(g.get("hb_mref")), wp))
    # means by source and type
    agg = defaultdict(lambda: defaultdict(list))
    for cid, s, t, g, gap, wp in rec:
        for k, v in g.items():
            agg[(s, t)][k].append(v)
        agg[(s, t)]["gap"].append(gap)
        agg[(s, t)]["warp"].append(wp)
    out.append("\nMeans over cases:\n")
    out.append("| vectors | type | n | hb transfer | +align | full blend | perfect ref | gap IP-B | warp dB |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for (s, t), m in sorted(agg.items()):
        mm = lambda k: sum(m[k]) / len(m[k]) if m.get(k) else None
        out.append("| %s | %s | %d | %s | %s | %s | %s | %s | %s |" % (s, t, len(m["gap"]), f(mm("hb")), f(mm("hb_align")),
                                                              f(mm("full")), f(mm("hb_mref")), f(mm("gap"), 2, False), f(mm("warp"), 2, False)))
    out.append("")
    return out


def td_tables(root):
    out = []
    files = sorted(glob.glob(os.path.join(root, "td", "*", "*", "*.json")))
    if not files:
        return out
    res = [json.load(open(p, encoding="utf8")) | {"tag": os.path.splitext(os.path.basename(p))[0]} for p in files]
    dflt = [r for r in res if r["tag"] == "default"]
    if dflt:
        out.append("## 3. TemporalDetail CPU model, designed gates (default), full 1080p, 72 frames\n")
        out.append("`grain flat` = grain rms out/in on the master's flat tiles (+-5 % gate); `grain all` = whole picture "
                   "(includes legitimate structure); `ghost` = regression on the previous frame over moving pixels (vs input; "
                   "0 = none); `worse` = share of 16 px blocks whose error vs master rose > 10 %.\n")
        out.append("| case | vectors | dB I | dB P | dB B | dB all | weight B | grain flat | grain all | ghost | worse | model ms |")
        out.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for r in sorted(dflt, key=lambda r: (r["case"], r["source"])):
            ty = r["types"]
            out.append("| %s | %s | %s | %s | %s | %s | %.4f | %s | %s | %s | %.3f | %.0f |" % (
                r["case"], r["source"], f(ty.get("I", {}).get("dpsnr")), f(ty.get("P", {}).get("dpsnr")),
                f(ty.get("B", {}).get("dpsnr")), f(r["dpsnr_all"]), ty.get("B", {}).get("weight", 0), f(r["grain_flat"], 3, False),
                f(r["grain_all"], 3, False), f(r["ghost"], 4), r["worse_share"], r["model_ms"]))
        out.append("")
    for title, pick in (("## 4b. Least-strict gate candidates (low tier, 48 frames; means over clips, per vector source)\n", "r2_"),
                        ("## 6. Chosen least-strict set on every tier (72 frames)\n", "final")):
        rr = [r for r in res if r["tag"].startswith(pick)]
        if not rr:
            continue
        out.append(title)
        out.append("| config | vectors | n | dB I | dB P | dB B | dB all | weight B | grain flat (worst) | grain all | ghost | worse | per case dB B |")
        out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        by = defaultdict(list)
        for r in rr:
            by[(r["tag"], r["source"])].append(r)
        for (tag, src), rs in sorted(by.items()):
            m = lambda fn: sum(fn(r) for r in rs) / len(rs)
            gw = max((r["grain_flat"] or 1 for r in rs), key=lambda g: abs(g - 1))
            out.append("| %s | %s | %d | %s | %s | %s | %s | %.4f | %s (%s) | %s | %s | %.3f | %s |" % (
                tag, src, len(rs), f(m(lambda r: r["types"].get("I", {}).get("dpsnr", 0))), f(m(lambda r: r["types"]["P"]["dpsnr"])),
                f(m(lambda r: r["types"]["B"]["dpsnr"])), f(m(lambda r: r["dpsnr_all"])), m(lambda r: r["types"]["B"]["weight"]),
                f(m(lambda r: r["grain_flat"] or 1), 3, False), f(gw, 3, False), f(m(lambda r: r["grain_all"]), 3, False),
                f(m(lambda r: r["ghost"] or 0), 4), m(lambda r: r["worse_share"]),
                " ".join("%s %s" % (r["case"].replace("_1080p", ""), f(r["types"]["B"]["dpsnr"])) for r in sorted(rs, key=lambda r: r["case"]))))
        out.append("")
    abl = [r for r in res if r["tag"] != "default" and not r["tag"].startswith("r2_") and not r["tag"].startswith("final")]
    if abl:
        out.append("## 4. Gate ablation (master-derived vectors, low tier, 48 frames; means over cases)\n")
        out.append("`all_off` = every validation gate forced open, grain budget 0, output guard off (grain/ghost MEASURED only). "
                   "`only_X` = all off except X enforced. `default_minus_X` = the designed model with only X opened. "
                   "`force1` = weight 1 wherever a reference is selected (projection, budget and guard still apply).\n")
        out.append("| config | n | dB B | dB P | dB all | weight B | grain flat | grain all | ghost | worse | per case dB B |")
        out.append("|---|---|---|---|---|---|---|---|---|---|---|")
        by = defaultdict(list)
        for r in abl:
            by[r["tag"]].append(r)
        order = sorted(by, key=lambda k: (0 if k in ("all_off", "force1") else 1 if k.startswith("only") else 2 if k.startswith("all_off") else 3, k))
        for k in order:
            rs = by[k]
            m = lambda fn: sum(fn(r) for r in rs) / len(rs)
            out.append("| %s | %d | %s | %s | %s | %.4f | %s | %s | %s | %.3f | %s |" % (
                k, len(rs), f(m(lambda r: r["types"]["B"]["dpsnr"])), f(m(lambda r: r["types"]["P"]["dpsnr"])),
                f(m(lambda r: r["dpsnr_all"])), m(lambda r: r["types"]["B"]["weight"]), f(m(lambda r: r["grain_flat"] or 1), 3, False),
                f(m(lambda r: r["grain_all"]), 3, False), f(m(lambda r: r["ghost"] or 0), 4), m(lambda r: r["worse_share"]),
                " ".join("%s %s" % (r["case"].split("_")[0], f(r["types"]["B"]["dpsnr"])) for r in sorted(rs, key=lambda r: r["case"]))))
        out.append("")
    return out


def band_tables(root):
    out = []
    files = sorted(glob.glob(os.path.join(root, "band", "*.json")))
    if not files:
        return out
    out.append("## 5. Banding and shadow texture (light_metrics; display units; tiles from the master)\n")
    out.append("band = mean square of the 3 px-smoothed error (step visibility) over smooth-gradient pixels; p99 = 99th "
               "percentile step. Ratios vs plain (same deband/final): < 1 = less banding. `tex` = shadow texture kept vs plain. "
               "`nd` = plain 8-bit rounding, `sd` = sigma-delta 8-bit model. NA = too few qualifying pixels.\n")
    out.append("| case | context | final | deband | cand | band all | band <5 nit | band 5-50 nit | p99 all | tex <5 | tex 5-50 | grain vs plain |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for p in files:
        d = json.load(open(p, encoding="utf8"))
        rows = d["rows"]
        plain = {(r["context"], r["final"], r["deband"]): r for r in rows if r["cand"] == "plain"}
        for r in rows:
            pl = plain[(r["context"], r["final"], r["deband"])]
            ratio = lambda k: (r[k] / pl[k]) if (r.get(k) is not None and pl.get(k)) else None
            if r["cand"] == "plain":
                vals = ["%.3g" % r[k] if r.get(k) is not None else "NA" for k in ("band_all_mse", "band_lt5_mse", "band_5to50_mse", "band_all_p99")]
            else:
                vals = [f(ratio(k), 3, False) + "x" if ratio(k) is not None else "NA" for k in ("band_all_mse", "band_lt5_mse", "band_5to50_mse", "band_all_p99")]
            out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                d["case"], r["context"], r["final"], "on" if r["deband"] else "off", r["cand"], *vals,
                f(r.get("tex_lt5"), 3, False), f(r.get("tex_5to50"), 3, False), f(r.get("grain_rms_ratio"), 3, False)))
    out.append("")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    lines = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
             "# TemporalDetail with real vectors: tables (generated by tools/eval/td_vectors/summarize.py)", ""]
    lines += ad_tables(a.root) + td_tables(a.root) + band_tables(a.root)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf8", newline="\n") as fo:
        fo.write("\n".join(lines) + "\n")
    print("->", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
