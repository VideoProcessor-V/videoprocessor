# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Owner-priority tables for the TemporalDetail real-vector test: GOP structure, I-frame distance, GOP-length variants,
true vs inferred frame types and reference window, camera model, regions. Reads D:/codex/td-real-vectors-out.

  python -I tools/eval/td_vectors/summarize_priority.py --root D:/codex/td-real-vectors-out --out PRIORITY.md
"""
import argparse
import glob
import json
import math
import os
import re
import sys
from collections import defaultdict


def db(mse):
    return 10 * math.log10(1023.0 ** 2 / mse) if mse > 0 else float("nan")


def gdb(mse, red):
    return 10 * math.log10(mse / max(mse - red, 1e-9))


def f(v, nd=3, sign=True):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "NA"
    return ("%+.*f" if sign else "%.*f") % (nd, v)


def runs(types):
    """B-run lengths between anchors and GOP lengths (frames between I)."""
    br = [len(m) for m in re.findall(r"B+", types)]
    iw = [i for i, c in enumerate(types) if c == "I"]
    gops = [b - a for a, b in zip(iw, iw[1:])]
    return br, gops


def gop_table(root):
    data = json.load(open(os.path.join(root, "data", "data.json"), encoding="utf8"))["cases"]
    out = ["## A. GOP structure of the test encodes (x265 medium: bframes 3, b-adapt 0, b-pyramid on, ref 3 (preset), "
           "closed GOP, scenecut 0; first 72 decoded frames)\n",
           "| case | fps | kb/s | GOP (frames / s) | I frames in window | B-run lengths | anchor spacing | PSNR I / P / B (dB) |",
           "|---|---|---|---|---|---|---|---|"]
    for c in data:
        br, gops = runs(c["types"])
        keyint = c.get("gop") or int(round(2 * c["fps"]))
        q = {}
        adp = os.path.join(root, "ad", c["id"] + "_disref.json")
        tdp = glob.glob(os.path.join(root, "td", c["id"], "disref_master", "default.json"))
        if os.path.isfile(adp):
            rows = json.load(open(adp, encoding="utf8"))["rows"]
            for t in "IPB":
                rr = [r["mse"] for r in rows if r["type"] == t]
                q[t] = db(sum(rr) / len(rr)) if rr else float("nan")
        elif tdp:
            ty = json.load(open(tdp[0], encoding="utf8"))["types"]
            q = {t: ty[t]["psnr_in"] if t in ty else float("nan") for t in "IPB"}
        hist = defaultdict(int)
        for b in br:
            hist[b] += 1
        out.append("| %s | %g | %d | %d / %.1f | %d | %s | %s | %s |" % (
            c["id"], c["fps"], c["kbps"], keyint, keyint / c["fps"], c["types"].count("I"),
            " ".join("%dx%d" % (k, v) for k, v in sorted(hist.items())), "%d" % (max(br) + 1 if br else 0),
            " / ".join(f(q.get(t), 2, False) for t in "IPB")))
    out.append("")
    return out


def dist_tables(root):
    out = []
    files = sorted(glob.glob(os.path.join(root, "dist", "*.json")))
    if not files:
        return out
    out.append("## B. Gain vs distance to the reference: nearest P vs the GOP's I frame (per-block ORACLE ceilings, B and P "
               "frames, each reference on its own)\n")
    out.append("dB = PSNR gain on the frame if that single decoded reference's fine band is transferred with the best per-block "
               "strength (chosen with the master). `chain` = consecutive fields composed hop by hop (as TD); `direct` = one "
               "field between the master frames (no chaining error). ref PSNR / keep = the reference's own quality and fine-band "
               "energy kept. warp = warped master reference vs master frame (alignment + content change), 10-bit peak.\n")
    out.append("| case | frame | role | n | mean |d| | ref PSNR | ref keep | hb chain master | hb chain engine | hb direct master | full chain master | warp chain / direct |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    binrows = defaultdict(lambda: defaultdict(list))
    bidi = defaultdict(lambda: defaultdict(list))
    for p in files:
        d = json.load(open(p, encoding="utf8"))
        cid = d["case"]["id"]
        groups = defaultdict(list)
        for r in d["rows"]:
            role = r["role"].replace("+", " ").split()
            key = "I" if any("_I" in x for x in role) else role[0]
            groups[(r["type"], key)].append(r)
            if "_I" in r["role"] or "anchor" in r["role"]:
                ad = abs(r["d"])
                b = "1-3" if ad <= 3 else "4-8" if ad <= 8 else "9-16" if ad <= 16 else "17-32" if ad <= 32 else ">32"
                binrows[(r["rtype"], b)]["hb"].append(gdb(r["mse"], r["chain_master|hb"]))
                if "direct_master|hb" in r:
                    binrows[(r["rtype"], b)]["direct"].append(gdb(r["mse"], r["direct_master|hb"]))
                binrows[(r["rtype"], b)]["coded"].append(gdb(r["mse"], r["chain_coded|hb"]))
        # past-only vs bidirectional per frame (oracle: best single reference)
        per_t = defaultdict(list)
        for r in d["rows"]:
            per_t[r["t"]].append(r)
        for t, rs in per_t.items():
            past = [r["chain_master|hb"] for r in rs if r["d"] < 0 and ("anchor" in r["role"])]
            near = [r["chain_master|hb"] for r in rs if r["role"].startswith(("prev_anchor", "next_anchor")) or "prev_anchor" in r["role"] or "next_anchor" in r["role"]]
            allr = [r["chain_master|hb"] for r in rs]
            if past and near:
                bidi[cid]["past"].append(gdb(rs[0]["mse"], max(past)))
                bidi[cid]["bidi"].append(gdb(rs[0]["mse"], max(near)))
                bidi[cid]["all"].append(gdb(rs[0]["mse"], max(allr)))
                bidi[cid]["type"].append(rs[0]["type"])
        for (ft, key), rs in sorted(groups.items()):
            m = lambda k: sum(gdb(r["mse"], r[k]) for r in rs if k in r) / max(1, sum(1 for r in rs if k in r)) if any(k in r for r in rs) else None
            w = lambda k: sum(r[k] for r in rs if k in r) / max(1, sum(1 for r in rs if k in r)) if any(k in r for r in rs) else None
            out.append("| %s | %s | %s | %d | %.1f | %.2f | %.2f | %s | %s | %s | %s | %s / %s |" % (
                cid, ft, key, len(rs), sum(abs(r["d"]) for r in rs) / len(rs), sum(r["ref_psnr"] for r in rs) / len(rs),
                sum(r["ref_keep"] for r in rs) / len(rs), f(m("chain_master|hb")), f(m("chain_coded|hb")),
                f(m("direct_master|hb")), f(m("chain_master|full")), f(w("chain_master|warp"), 2, False),
                f(w("direct_master|warp"), 2, False)))
    out.append("\nBy reference type and distance (all cases pooled; mean dB per (frame, reference)):\n")
    out.append("| ref type | distance | n | hb chain master | hb direct master | hb chain engine |")
    out.append("|---|---|---|---|---|---|")
    order = ["1-3", "4-8", "9-16", "17-32", ">32"]
    for (rt, b), v in sorted(binrows.items(), key=lambda kv: (kv[0][0], order.index(kv[0][1]))):
        mm = lambda k: sum(v[k]) / len(v[k]) if v[k] else None
        out.append("| %s | %s | %d | %s | %s | %s |" % (rt, b, len(v["hb"]), f(mm("hb")), f(mm("direct")), f(mm("coded"))))
    out.append("\nPast-only (low latency) vs bidirectional (best of previous / next anchor) vs any listed reference incl. the "
               "far I frame (oracle, master-derived chained vectors, mean dB over B and P frames):\n")
    out.append("| case | n | past anchors only | prev + next anchor | + I frame at any distance |")
    out.append("|---|---|---|---|---|")
    for cid, v in sorted(bidi.items()):
        n = len(v["past"])
        out.append("| %s | %d | %s | %s | %s |" % (cid, n, f(sum(v["past"]) / n), f(sum(v["bidi"]) / n), f(sum(v["all"]) / n)))
    out.append("")
    return out


def infer_types(qs):
    """Frame-type inference from the model's per-frame detail energy only: anchor = q above both neighbours' mean by 2 %;
    I = anchor with q >= 1.10 x the median of its +-8 frames. Returns (anchor accuracy, I recall, I precision)."""
    t = [x[0] for x in qs]
    q = [x[1] for x in qs]
    n = len(q)
    ok = 0
    ip = it = ic = 0
    for i in range(1, n - 1):
        anchor = q[i] > 1.02 * 0.5 * (q[i - 1] + q[i + 1])
        win = sorted(q[max(0, i - 8):i + 9])
        isI = anchor and q[i] >= 1.10 * win[len(win) // 2]
        ok += int(anchor == (t[i] in "IP"))
        ip += int(isI)
        it += int(t[i] == "I")
        ic += int(isI and t[i] == "I")
    return ok / max(1, n - 2), (ic / it if it else None), (ic / ip if ip else None)


def td_k_tables(root):
    out = []
    files = sorted(glob.glob(os.path.join(root, "td", "*", "*", "k_*.json")))
    files += sorted(glob.glob(os.path.join(root, "td", "*gop*", "*", "*.json")))
    files += sorted(glob.glob(os.path.join(root, "td", "*", "camref_*", "final.json")))
    if not files:
        return out
    res = [json.load(open(p, encoding="utf8")) | {"tag": os.path.splitext(os.path.basename(p))[0]} for p in files]
    out.append("## C. TemporalDetail CPU model: true vs inferred frame types, reference window (max age), GOP variants, camera model\n")
    out.append("Base = the least-strict set (only motion uncertainty + base match + coherence + the output guard; evidence, "
               "stronger, high-band, focus, correlation, harm open; grain budget off). `true` = admission from the TRUE coded "
               "frame types (I always becomes the reference, B never) - live VP over HDMI has no frame types. `ma` = max "
               "reference age in seconds (default 2). ref I/P/B = share of transferring blocks on B frames whose detail comes "
               "from an I / P / B source frame; dist = mean frames to that source. infer = frame-type inference from the "
               "model's own per-frame detail energy (anchor accuracy, I recall / precision).\n")
    out.append("| case | vectors | config | dB I | dB P | dB B | dB all | grain flat | ghost | worse | ref I/P/B (B frames) | dist | infer anchor acc / I rec / I prec |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in sorted(res, key=lambda r: (r["case"], r["source"], r["tag"])):
        ty = r["types"]
        rs = r.get("ref_source_types", {}).get("B", {})
        tot = sum(rs.values()) or 0
        share = "/".join("%.2f" % (rs.get(k, 0) / tot) for k in "IPB") if tot else "-"
        dist = r.get("ref_mean_distance", {}).get("B")
        inf = infer_types(r["q_series"]) if r.get("q_series") and any(x[1] for x in r["q_series"]) else None
        out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %.3f | %s | %s | %s |" % (
            r["case"], r["source"], r["tag"], f(ty.get("I", {}).get("dpsnr")), f(ty.get("P", {}).get("dpsnr")),
            f(ty.get("B", {}).get("dpsnr")), f(r["dpsnr_all"]), f(r["grain_flat"], 3, False), f(r["ghost"], 4), r["worse_share"],
            share, f(dist, 1, False), ("%.2f / %s / %s" % (inf[0], f(inf[1], 2, False), f(inf[2], 2, False))) if inf else "-"))
    out.append("")
    return out


def cam_tables(root):
    out = []
    files = sorted(glob.glob(os.path.join(root, "ad", "*_camref.json")))
    if not files:
        return out
    out.append("## D. Camera model (VP's global fit, offline): oracle ceilings with vs without it (low tier, B frames)\n")
    out.append("`camref` = refined DIS vectors with camera-inlier blocks replaced by the fitted homography (usable fits only, "
               "confidence >= 0.5); `disref` = without.\n")
    out.append("| case | vectors | hb transfer | full blend | warp dB |")
    out.append("|---|---|---|---|---|")
    for p in files:
        cid = os.path.basename(p).replace("_camref.json", "")
        for fp in (os.path.join(root, "ad", cid + "_disref.json"), p):
            if not os.path.isfile(fp):
                continue
            d = json.load(open(fp, encoding="utf8"))
            for s in d["sources"]:
                rows = [r for r in d["rows"] if r["type"] == "B" and (s + "|hb") in r]
                mse = sum(r["mse"] for r in rows) / len(rows)
                g = lambda k: gdb(mse, sum(r[s + "|" + k] for r in rows) / len(rows))
                wp = [v for r in rows for kk, v in r.items() if kk.startswith(s + "|warp_psnr")]
                out.append("| %s | %s | %s | %s | %.2f |" % (cid, s, f(g("hb")), f(g("full")), sum(wp) / len(wp)))
    out.append("")
    return out


def region_tables(root):
    p = os.path.join(root, "region", "region_low.json")
    if not os.path.isfile(p):
        return []
    d = json.load(open(p, encoding="utf8"))
    out = ["## E. Where in the picture (low tier, frames 24-47, 16 px blocks; oracle = per-block fine-band transfer ceiling "
           "with master vectors; td = the least-strict TD run)\n",
           "| dimension | class | area share | oracle-gain share | oracle dB in class | TD dB (master vectors) | TD dB (engine vectors) |",
           "|---|---|---|---|---|---|---|"]
    for dim, cl in d["classes"].items():
        for v, e in cl.items():
            out.append("| %s | %s | %.3f | %.3f | %s | %s | %s |" % (dim, v, e["area_share"], e["oracle_share"], f(e["oracle_db"]),
                                                             f(e.get("disref_master_final_db")), f(e.get("disref_coded_final_db"))))
    out.append("\nGain concentration (oracle-ranked blocks, unreachable upper bound): " +
               ", ".join("best %s of blocks -> %.0f%% of the gain" % (k, 100 * v) for k, v in d["curve"].items()))
    r = d["rule"]
    out.append("\nRegion rule (class combinations light|detail|motion|camera chosen on boat+tos, held out sol+crowd): train "
               "%.0f%% of area -> %.0f%% of gain; held-out %.0f%% of area -> %.0f%% of gain. Combos: %s\n" % (
                   100 * r["train_area"], 100 * r["train_gain"], 100 * r["test_area"], 100 * r["test_gain"], ", ".join(r["chosen"][:8])))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    lines = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
             "# TemporalDetail with real vectors: owner-priority tables (generated by tools/eval/td_vectors/summarize_priority.py)", ""]
    lines += gop_table(a.root) + dist_tables(a.root) + td_k_tables(a.root) + cam_tables(a.root) + region_tables(a.root)
    with open(a.out, "w", encoding="utf8", newline="\n") as fo:
        fo.write("\n".join(lines) + "\n")
    print("->", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
