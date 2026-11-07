# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Scores the source-identity evaluation (SourceIdChainEvalTests output) against the reel truth and prints the GO table of
doc 78 section 9.

  python -I tools/eval/source_id/score_identity.py --reels D:/testclips/sourceid/reels.json --eval-dir <VP_SOURCE_ID_EVAL_OUT>
  python -I tools/eval/source_id/score_identity.py --selftest

Inputs : reels.json (truth per segment, events), <eval-dir>/evaluations.csv, events.csv, reels_done.csv (harness output).
Outputs: <out>/summary.md and <out>/gates.json (default out = eval dir).
Lanes  : "off" / "on" = the classical monitor without / with consensus (an engaged re-base factor is the LATCHED state, its n is
         factor x raster); "chain" = the SID-C2 chain estimator when it plugs into the harness seam. The GO table is computed for
         --gate-lane (default: chain when present, else on). Every lane gets the metric tables.

Metric definitions (all per lane; n = original size per axis, compared with the reel truth `truth_n`, exact integers):
  confident-wrong episode   a maximal run of evaluation rows in state LATCHED whose (n_x, n_y) differs from the truth of the segment, rows
                            within --boundary-grace seconds after a truth change are not judged (the latch may still hold the old value)
  per title-hour / per reel episodes / reel-hours ; share of reels with >= 1 episode
  latch latency             seconds from the segment start to the first LATCHED row whose n is exact (segments with a player upscale;
                            a native segment is "latched" when the lane reports LATCHED at the canvas size)
  exact-n accuracy          among segments whose last row is LATCHED: share with exact n
  flaps/h                   a latched value that changes to another value or to unlatched with NO truth change in the preceding
                            --flap-window seconds (default 60), per reel-hour
  UNSURE share by reason    share of rows in state UNKNOWN / ABSTAIN / PROVISIONAL split by the reason column
  false CANDIDATE/h         entries into CANDIDATE whose candidate n differs from the truth (classical lanes: declared ratio vs truth)
  notice false positives    one-time notices with no truth quality/geometry event within [-1 s, +--abr-tol s], per reel-hour
  menu re-base violation    a LATCHED n below the raster persisting > --menu-persist seconds in a native (menu/black) segment
"""
import argparse
import csv
import json
import math
import os
import sys
import tempfile
from collections import defaultdict

LATCH_T = 15.0           # R2 clean: latched correct within this many seconds
DIRTY_T = 10.0
HOURS_PHASE_A = 30.0
TITLES_PHASE_A = 15


def fnum(x, default=None):
    try:
        if x is None or x == "":
            return default
        return float(x)
    except ValueError:
        return default


def pct(vals, p):
    if not vals:
        return None
    v = sorted(vals)
    k = (len(v) - 1) * p
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


# ------------------------------------------------------------------ loading
def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    reels = {}
    for r in doc["reels"]:
        fps = r["fps"][0] / float(r["fps"][1])
        segs = []
        for s in r["segments"]:
            s = dict(s)
            s["t0"] = s["out_start_frame"] / fps
            s["t1"] = (s["out_start_frame"] + s["out_frames"]) / fps
            segs.append(s)
        events = []
        for e in r.get("events", []):
            e = dict(e)
            e["t"] = e["frame"] / fps
            events.append(e)
        reels[r["id"]] = dict(id=r["id"], family=r["family"], master=r["master"], fps=fps, W=r["width"], H=r["height"], frames=r["frames"],
                              duration=r["duration_s"], segs=segs, events=events, flags=r.get("flags", []), title=r.get("title", ""),
                              looped=r.get("looped", False))
    masters = doc.get("masters", {})
    for r in reels.values():
        r["cls"] = masters.get(r["master"], {}).get("cls", "")
    return reels


def load_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def load_eval(eval_dir):
    rows = defaultdict(list)       # (reel, lane) -> rows
    for r in load_csv(os.path.join(eval_dir, "evaluations.csv")):
        r["t"] = fnum(r.get("t"), 0.0)
        rows[(r["reel"], r["lane"])].append(r)
    for v in rows.values():
        v.sort(key=lambda x: (x["t"], fnum(x.get("out_frame"), 0)))
    events = defaultdict(list)
    for e in load_csv(os.path.join(eval_dir, "events.csv")):
        e["t"] = fnum(e.get("t"), 0.0)
        events[(e["reel"], e["lane"])].append(e)
    done = {r["reel"]: r for r in load_csv(os.path.join(eval_dir, "reels_done.csv"))}
    return rows, events, done


def seg_index(reel, t):
    for i, s in enumerate(reel["segs"]):
        if s["t0"] - 1e-9 <= t < s["t1"] - 1e-9:
            return i
    return len(reel["segs"]) - 1


# ------------------------------------------------------------------ per reel / lane
def row_n(row):
    nx, ny = fnum(row.get("n_x")), fnum(row.get("n_y"))
    return (None if nx is None else int(round(nx)), None if ny is None else int(round(ny)))


def truth_n(seg):
    return (seg["truth_n"]["x"], seg["truth_n"]["y"])


def is_native_truth(reel, seg):
    return truth_n(seg) == (reel["W"], reel["H"])


def score_reel_lane(reel, rows, events, cfg):
    st = dict(rows=len(rows), episodes=[], menu_violations=0, flaps=0, cand_entries=0, false_cand=0, seg=[], reasons=defaultdict(int),
              unsure_rows=0, latched_rows=0, ms=[], notices=[], abr=[], geo=[])
    prev_state, prev_n = None, None
    ep = None
    menu_run = {}
    truth_changes = [s["t0"] for i, s in enumerate(reel["segs"]) if i > 0 and (truth_n(s) != truth_n(reel["segs"][i - 1]) or
                                                                              s["active_area_out"] != reel["segs"][i - 1]["active_area_out"] or
                                                                              s["regime"] != reel["segs"][i - 1]["regime"])]
    segs = [dict(idx=i, first_correct=None, last=None, rows=0, hold_rows=0, wrong_rows=0, kernel_last=None, sharp_last=None, n1_last=None,
                 active_last=None, unsure_reason_rows=0, unsure_rows=0) for i in range(len(reel["segs"]))]
    for row in rows:
        t = row["t"]
        si = seg_index(reel, t)
        seg = reel["segs"][si]
        ss = segs[si]
        state = (row.get("state") or "").upper()
        n = row_n(row)
        tn = truth_n(seg)
        ss["rows"] += 1
        ss["last"] = dict(state=state, n=n, row=row)
        ms = fnum(row.get("ms"))
        if ms is not None and ms >= 0:
            st["ms"].append(ms)
        latched = state == "LATCHED"
        in_grace = (t - seg["t0"]) < cfg["boundary_grace"] and si > 0
        wrong = latched and n != tn and not in_grace
        if latched:
            st["latched_rows"] += 1
            if n == tn and ss["first_correct"] is None:
                ss["first_correct"] = t - seg["t0"]
        if state == "HOLD":
            ss["hold_rows"] += 1
        if state in ("UNKNOWN", "ABSTAIN", "PROVISIONAL"):
            st["unsure_rows"] += 1
            reason = (row.get("reason") or "").strip() or "none"
            st["reasons"][reason] += 1
            ss["unsure_rows"] += 1
            if reason != "none":
                ss["unsure_reason_rows"] += 1
        if wrong:
            ss["wrong_rows"] += 1
            if ep is None:
                ep = dict(t0=t, t1=t, seg=si, n=n, truth=tn)
            ep["t1"] = t
        else:
            if ep is not None:
                st["episodes"].append(ep)
                ep = None
        # menu / black: a re-base that persists
        if seg["regime"] in ("menu", "black") or (reel["family"] == "R7" and is_native_truth(reel, seg) and seg["regime"] != "stream"):
            key = si
            if latched and n is not None and n[1] is not None and n[1] < reel["H"]:
                run = menu_run.setdefault(key, [t, t, False])
                run[1] = t
                if not run[2] and run[1] - run[0] > cfg["menu_persist"]:
                    st["menu_violations"] += 1
                    run[2] = True
            else:
                menu_run.pop(key, None)
        # flaps: a latched value that reverts / changes with no truth change in the window
        cur = (latched, n if latched else None)
        if prev_state is not None and prev_state[0] and cur != prev_state:
            recent = [c for c in truth_changes if t - cfg["flap_window"] < c <= t]
            if not recent:
                st["flaps"] += 1
        prev_state = cur
        # candidate entries
        if state == "CANDIDATE" and (prev_n is None or prev_n != "CANDIDATE"):
            st["cand_entries"] += 1
            cand_ok = False
            if n and n[1] is not None and n[1] > 0 and (state == "CANDIDATE") and row.get("lane") == "chain":
                cand_ok = (n == tn)
            else:
                dec = fnum(row.get("declared"))
                cand_ok = dec is not None and abs(dec - tn[1] / float(reel["H"])) < 0.01 and not is_native_truth(reel, seg)
            if not cand_ok:
                st["false_cand"] += 1
        prev_n = state
        # kernel / sharpening / double / active (chain lane columns)
        if latched:
            ss["kernel_last"] = (row.get("kernel") or "").strip() or None
            sa = fnum(row.get("sharpen_amount"))
            ss["sharp_last"] = sa
            n1x, n1y = fnum(row.get("double_n1_x")), fnum(row.get("double_n1_y"))
            ss["n1_last"] = (int(n1x), int(n1y)) if n1x is not None and n1y is not None else None
            ax, ay, aw, ah = (fnum(row.get(k)) for k in ("active_x", "active_y", "active_w", "active_h"))
            ss["active_last"] = (ax, ay, aw, ah) if None not in (ax, ay, aw, ah) else None
    if ep is not None:
        st["episodes"].append(ep)
    st["seg"] = segs
    for e in events:
        typ = e.get("type")
        if typ == "notice":
            st["notices"].append(e["t"])
        elif typ == "abr":
            st["abr"].append(dict(t=e["t"], direction=e.get("a", "")))
        elif typ == "geometry":
            st["geo"].append(dict(t=e["t"], direction=""))
    return st


# ------------------------------------------------------------------ aggregation
def family_pred(*fams):
    return lambda r: r["family"] in fams


def idpred(*prefixes):
    return lambda r: any(r["id"].startswith(p) for p in prefixes)


def aggregate(reels, per, lane, pred, cfg):
    """per: {(reel, lane): stats}. Returns dict of metrics over the reels for which pred(reel) and the lane has rows."""
    sel = [r for r in reels.values() if pred(r) and (r["id"], lane) in per]
    out = dict(reels=len(sel), hours=sum(r["duration"] for r in sel) / 3600.0, titles=len(set(r["master"] for r in sel)),
               classes=len(set(r["cls"] for r in sel)), episodes=0, reels_with_episode=0, flaps=0, cand_entries=0, false_cand=0,
               menu_violations=0, notices_fp=0, notices=0, ms=[], seg_total=0, seg_up=0, seg_up_correct_T=0, lat=[], seg_latched_end=0,
               seg_latched_exact=0, seg_native=0, seg_native_latched_ok=0, reasons=defaultdict(int), unsure_rows=0, rows=0, latched_rows=0,
               kernel_n=0, kernel_ok=0, kernel_cov=0, kernel_wrong=0, sharp_up=0, sharp_hit=0, sharp_off=0, sharp_false=0, dbl_n=0, dbl_ok=0, dbl_cov=0,
               act_n=0, act_ok=0, act_cov=0, act_wrong=0, dirty_segs=0, dirty_ok=0, hold_rows=0, hold_segs=0, wrong_rows=0, sel=[r["id"] for r in sel],
               bad=[])
    for r in sel:
        st = per[(r["id"], lane)]
        out["episodes"] += len(st["episodes"])
        out["reels_with_episode"] += 1 if st["episodes"] else 0
        for ep in st["episodes"]:
            out["bad"].append("%s seg %d: latched n=%s truth=%s t=%.1f-%.1f" % (r["id"], ep["seg"], ep["n"], ep["truth"], ep["t0"], ep["t1"]))
        out["flaps"] += st["flaps"]
        out["cand_entries"] += st["cand_entries"]
        out["false_cand"] += st["false_cand"]
        out["menu_violations"] += st["menu_violations"]
        out["ms"].extend(st["ms"])
        out["rows"] += st["rows"]
        out["latched_rows"] += st["latched_rows"]
        out["unsure_rows"] += st["unsure_rows"]
        out["wrong_rows"] += sum(s["wrong_rows"] for s in st["seg"])
        for k, v in st["reasons"].items():
            out["reasons"][k] += v
        for tn_ in st["notices"]:
            out["notices"] += 1
            near = [e for e in r["events"] if e["kind"] in ("quality", "geometry") and e["t"] - 1.0 <= tn_ <= e["t"] + cfg["abr_tol"]]
            if not near:
                out["notices_fp"] += 1
        for seg, ss in zip(r["segs"], st["seg"]):
            if seg["regime"] != "stream" or ss["rows"] == 0:
                continue
            out["seg_total"] += 1
            native = is_native_truth(r, seg)
            last = ss["last"]
            if native:
                out["seg_native"] += 1
                if ss["wrong_rows"] == 0:
                    out["seg_native_latched_ok"] += 1
            else:
                out["seg_up"] += 1
                if ss["first_correct"] is not None:
                    out["lat"].append(ss["first_correct"])
                    if ss["first_correct"] <= LATCH_T:
                        out["seg_up_correct_T"] += 1
            if last and last["state"] == "LATCHED":
                out["seg_latched_end"] += 1
                if last["n"] == truth_n(seg):
                    out["seg_latched_exact"] += 1
            if seg.get("hold_expected"):
                out["hold_segs"] += 1
                out["hold_rows"] += ss["hold_rows"]
            # dirty outcome: latched correct, or UNSURE with a reason, never wrong
            if not native:
                out["dirty_segs"] += 1
                if ss["wrong_rows"] == 0 and (ss["first_correct"] is not None or (ss["unsure_rows"] > 0 and ss["unsure_reason_rows"] * 2 >= ss["unsure_rows"])):
                    out["dirty_ok"] += 1
            # kernel (10-bit segments with a player kernel)
            if seg.get("kernel") and last and last["state"] == "LATCHED":
                if ss["kernel_last"]:
                    out["kernel_cov"] += 1
                    if seg["bits"] == 10:
                        out["kernel_n"] += 1
                        fam = seg["kernel"]["family"]
                        if ss["kernel_last"].lower() == fam:
                            out["kernel_ok"] += 1
                        else:
                            out["kernel_wrong"] += 1
            # sharpening
            if last and last["state"] == "LATCHED" and ss["sharp_last"] is not None:
                if seg.get("sharpening") and seg["sharpening"]["amount"] >= 0.3:
                    out["sharp_up"] += 1
                    if ss["sharp_last"] > 0:
                        out["sharp_hit"] += 1
                elif not seg.get("sharpening"):
                    out["sharp_off"] += 1
                    if ss["sharp_last"] > 0:
                        out["sharp_false"] += 1
            # double scaling
            if seg.get("double") and last and last["state"] == "LATCHED" and ss["n1_last"] is not None:
                out["dbl_cov"] += 1
                if ss["n1_last"] == (seg["double"]["n1"]["x"], seg["double"]["n1"]["y"]):
                    out["dbl_ok"] += 1
            if seg.get("double"):
                out["dbl_n"] += 1
            # active area
            aa = seg["active_area_out"]
            if (aa["w"], aa["h"]) != (r["W"], r["H"]) and last and last["state"] == "LATCHED" and ss["active_last"] is not None:
                out["act_cov"] += 1
                tol = 2
                if all(abs(a - b) <= tol for a, b in zip(ss["active_last"], (aa["x"], aa["y"], aa["w"], aa["h"]))):
                    out["act_ok"] += 1
                else:
                    out["act_wrong"] += 1
            if (aa["w"], aa["h"]) != (r["W"], r["H"]):
                out["act_n"] += 1
    return out


def abr_scores(reels, per, lane, cfg):
    """Precision / recall of detected rung events (type abr / geometry) against the truth events."""
    tp = det = 0
    truth_geo = truth_geo_hit = truth_q = truth_q_hit = 0
    lat = []
    for r in reels.values():
        st = per.get((r["id"], lane))
        if st is None:
            continue
        detections = [dict(d, used=False) for d in st["abr"] + st["geo"]]
        for e in r["events"]:
            if e["kind"] not in ("geometry", "quality"):
                continue
            hit = None
            for d in detections:
                if d["used"]:
                    continue
                if e["t"] - 1.0 <= d["t"] <= e["t"] + cfg["abr_tol"] and (not d["direction"] or d["direction"] == e["direction"]):
                    hit = d
                    break
            if e["kind"] == "geometry":
                truth_geo += 1
                if hit and hit["t"] <= e["t"] + 2.0:
                    truth_geo_hit += 1
            else:
                truth_q += 1
                if hit:
                    truth_q_hit += 1
            if hit:
                hit["used"] = True
                lat.append(hit["t"] - e["t"])
        det += len(detections)
        tp += sum(1 for d in detections if d["used"])
    return dict(det=det, tp=tp, precision=(tp / float(det)) if det else None, truth_geo=truth_geo, geo_hit=truth_geo_hit,
                truth_q=truth_q, q_hit=truth_q_hit, q_recall=(truth_q_hit / float(truth_q)) if truth_q else None, lat=lat)


# ------------------------------------------------------------------ GO table
def gate(gid, field, crit, value, status, detail=""):
    return dict(id=gid, field=field, criterion=crit, value=value, status=status, detail=detail)


def frac(a, b):
    return (a / float(b)) if b else None


def go_table(reels, per, lane, cfg):
    A = lambda pred: aggregate(reels, per, lane, pred, cfg)
    g = []
    r1 = A(family_pred("R1"))
    h = r1["hours"]
    bound = (3.0 / h) if h > 0 else None
    st = "NA" if r1["reels"] == 0 else ("FAIL" if r1["episodes"] > 0 else ("INSUFFICIENT" if (h < HOURS_PHASE_A or r1["titles"] < TITLES_PHASE_A) else "PASS"))
    g.append(gate("axes.r1.wrong", "Axes - R1 native (phase A smoke gate)", "0 confident-wrong over >= %g h, >= %d titles, >= 1000 shots" % (HOURS_PHASE_A, TITLES_PHASE_A),
                  dict(confident_wrong=r1["episodes"], hours=round(h, 3), titles=r1["titles"], reels_with_wrong=r1["reels_with_episode"],
                       rate_bound_per_h=(round(bound, 2) if bound else None), shots="not measured"), st,
                  "zero events in %.2f h bounds the rate at <= %s per hour (95%%, rule of three)" % (h, ("%.1f" % bound) if bound else "n/a")))
    st = "NA" if r1["reels"] == 0 else ("PASS" if frac(r1["false_cand"], h) is not None and r1["false_cand"] / h <= 1.0 else "FAIL")
    g.append(gate("axes.r1.false_candidate", "Axes - R1 native", "false CANDIDATE <= 1 per hour",
                  dict(per_hour=round(frac(r1["false_cand"], h) or 0.0, 3), count=r1["false_cand"], hours=round(h, 3)), st))
    dk = A(idpred("R8.dark2", "R8.credits", "R8.flat"))
    st = "NA" if dk["reels"] == 0 else ("FAIL" if (dk["flaps"] > 0 or dk["episodes"] > 0) else ("INSUFFICIENT" if dk["hours"] < 1.0 else "PASS"))
    g.append(gate("axes.r1.dark_credits", "Axes - R1 native", "dark / credits >= 1 h: 0 reverts", dict(reverts=dk["flaps"], wrong=dk["episodes"], hours=round(dk["hours"], 3)), st))
    c = A(lambda r: r["id"].startswith("R2.clean"))
    secs = c["hours"] * 3600.0
    ok = frac(c["seg_up_correct_T"], c["seg_up"])
    if c["reels"] == 0:
        st = "NA"
    elif c["episodes"] > 0 or (ok is not None and ok < 0.95):
        st = "FAIL"
    elif c["reels"] < 20 or c["classes"] < 3 or secs < 1200:
        st = "INSUFFICIENT"
    else:
        st = "PASS"
    g.append(gate("axes.r2.clean", "Axes - R2 clean chains", ">= 95%% latched correct <= %g s; 0 wrong (>= 20 clips, >= 3 content types, 20 min)" % LATCH_T,
                  dict(latched_correct_within=(round(ok, 3) if ok is not None else None), wrong=c["episodes"], clips=c["reels"], content_types=c["classes"],
                       seconds=round(secs)), st, "; ".join(c["bad"][:5])))
    d = A(lambda r: r["id"].startswith("R2.dirty") or r["id"].startswith("R2.usm"))
    ok = frac(d["dirty_ok"], d["dirty_segs"])
    lat_ok = frac(sum(1 for x in d["lat"] if x <= DIRTY_T), d["dirty_segs"])
    if d["reels"] == 0:
        st = "NA"
    elif d["episodes"] > 0 or d["flaps"] > 0 or (ok is not None and ok < 0.80):
        st = "FAIL"
    else:
        st = "PASS"
    g.append(gate("axes.dirty", "Axes - dirty chains", ">= 80%% latched or UNSURE with reason; 0 wrong; 0 flaps; 1080 latched <= %g s or UNSURE" % DIRTY_T,
                  dict(latched_or_unsure_with_reason=(round(ok, 3) if ok is not None else None), latched_within_10s=(round(lat_ok, 3) if lat_ok is not None else None),
                       wrong=d["episodes"], flaps=d["flaps"], segments=d["dirty_segs"]), st, "; ".join(d["bad"][:5])))
    m = A(family_pred("R7"))
    if m["reels"] == 0:
        st = "NA"
    else:
        st = "FAIL" if (m["menu_violations"] > 0 or m["episodes"] > 0) else "PASS"
    g.append(gate("axes.r7", "Axes - R7 menus / overlays", "no re-base persisting > %g s on a native menu; 0 wrong under subtitles / PiP (HOLD shown)" % cfg["menu_persist"],
                  dict(menu_violations=m["menu_violations"], wrong=m["episodes"], hold_rows_in_expected_segments=m["hold_rows"], expected_hold_segments=m["hold_segs"]), st,
                  "; ".join(m["bad"][:5])))
    for gid, field, crit in (("axes.r10", "Axes - R10 loop", ">= 90% of informative evaluations propose the true n; latch <= 20 s; 0 wrong; 0 dropped frames"),
                             ("axes.r11", "Axes - R11 Apple TV", "correct n per title; no re-base > 3 s on menus")):
        g.append(gate(gid, field, crit, None, "NA", "no reels in this evaluation (GPU rig / owner capture)"))
    allr = A(lambda r: True)
    k_ok = frac(allr["kernel_ok"], allr["kernel_n"])
    if allr["kernel_cov"] == 0:
        st = "NA"
    else:
        wrong_rate = frac(allr["kernel_wrong"], allr["kernel_n"]) or 0.0
        st = "FAIL" if ((k_ok is not None and k_ok < 0.9) or wrong_rate > 0.02) else "PASS"
    g.append(gate("kernel", "Kernel", ">= 90% correct family when latched on 10-bit; confident-wrong <= 2%",
                  dict(correct=(round(k_ok, 3) if k_ok is not None else None), wrong=allr["kernel_wrong"], latched_with_kernel=allr["kernel_n"]), st,
                  "lane reports no kernel" if allr["kernel_cov"] == 0 else ""))
    s_rec = frac(allr["sharp_hit"], allr["sharp_up"])
    s_fal = frac(allr["sharp_false"], allr["sharp_off"])
    st = "NA" if (allr["sharp_up"] + allr["sharp_off"]) == 0 else ("PASS" if (s_rec is None or s_rec >= 0.8) and (s_fal is None or s_fal <= 0.05) else "FAIL")
    g.append(gate("sharpening", "Sharpening", "recall >= 80% at USM >= 0.3; false <= 5%",
                  dict(recall=(round(s_rec, 3) if s_rec is not None else None), false=(round(s_fal, 3) if s_fal is not None else None)), st,
                  "lane reports no sharpening" if st == "NA" else ""))
    dbl = frac(allr["dbl_ok"], allr["dbl_n"])
    st = "NA" if allr["dbl_cov"] == 0 else ("PASS" if dbl is not None and dbl >= 0.8 else "FAIL")
    g.append(gate("double", "Double scaling", "both notches >= 80%; bottleneck exact when declared",
                  dict(correct=(round(dbl, 3) if dbl is not None else None), segments=allr["dbl_n"], reported=allr["dbl_cov"]), st,
                  "lane reports no double-scale" if st == "NA" else ""))
    st = "NA" if allr["act_cov"] == 0 else ("PASS" if allr["act_wrong"] == 0 else "FAIL")
    g.append(gate("active_area", "Active area", "exact on clean bars, +-2 px soft, 0 wrong",
                  dict(exact=allr["act_ok"], wrong=allr["act_wrong"], segments=allr["act_n"], reported=allr["act_cov"]), st,
                  "lane reports no active area" if st == "NA" else ""))
    g.append(gate("chroma", "Chroma (SID-F)", "siting >= 95%, family >= 80%, false player-upsampled on 4:4:4 <= 5%", None, "NA", "luma-only reels (SID-F)"))
    ab = abr_scores(reels, per, lane, cfg)
    r6 = [r for r in reels.values() if r["family"] == "R6" and (r["id"], lane) in per]
    if not r6:
        st = "NA"
        detail = "no R6 reels"
    else:
        geo_ok = frac(ab["geo_hit"], ab["truth_geo"])
        q_rec = ab["q_recall"]
        prec = ab["precision"]
        bad = []
        if geo_ok is not None and geo_ok < 1.0:
            bad.append("geometry recall < 100%")
        if prec is not None and prec < 0.8:
            bad.append("precision < 0.8")
        if q_rec is not None and q_rec < 0.5:
            bad.append("quality recall < 0.5")
        st = "FAIL" if bad else "PASS"
        detail = "; ".join(bad)
    g.append(gate("abr", "ABR", "resolution rungs 100% as geometry events <= 2 s; quality-only precision >= 0.8, recall >= 0.5",
                  dict(precision=(round(ab["precision"], 3) if ab["precision"] is not None else None), geometry_recall=(round(frac(ab["geo_hit"], ab["truth_geo"]), 3) if ab["truth_geo"] else None),
                       quality_recall=(round(ab["q_recall"], 3) if ab["q_recall"] is not None else None), detections=ab["det"], median_latency_s=(round(pct(ab["lat"], 0.5), 2) if ab["lat"] else None)),
                  st, detail))
    notice_h = allr["hours"]
    nfp = frac(allr["notices_fp"], notice_h)
    st = "NA" if allr["reels"] == 0 else ("PASS" if (nfp or 0.0) <= 0.1 * 1.0 else "FAIL")
    g.append(gate("abr.notice", "ABR notice", "false positives <= 1 per 10 title-hours",
                  dict(per_10_hours=(round((nfp or 0.0) * 10, 3)), notices=allr["notices"], false_positives=allr["notices_fp"], hours=round(notice_h, 3)),
                  "NA" if allr["notices"] == 0 and lane != "on" else st))
    ms = allr["ms"]
    p95 = pct(ms, 0.95)
    st = "NA" if not ms else ("PASS" if p95 <= 50.0 else "FAIL")
    g.append(gate("cost", "Cost", "<= 15% of one core average; p95 <= 50 ms per evaluation; native latched <= 2%",
                  dict(ms_mean=(round(sum(ms) / len(ms), 2) if ms else None), ms_p95=(round(p95, 2) if p95 is not None else None), evaluations=len(ms)), st,
                  "lane 'off'/'on' time includes the classical tile analysis; the chain estimator's own time is the 'chain' lane"))
    return g, ab


# ------------------------------------------------------------------ report
def family_table(reels, per, lane, cfg):
    rows = []
    for fam in sorted(set(r["family"] for r in reels.values())):
        a = aggregate(reels, per, lane, family_pred(fam), cfg)
        if a["reels"] == 0:
            continue
        rows.append((fam, a["reels"], a["hours"], a["episodes"], a["reels_with_episode"], frac(a["episodes"], a["hours"]), pct(a["lat"], 0.5), pct(a["lat"], 0.95),
                     frac(a["seg_latched_exact"], a["seg_latched_end"]), frac(a["flaps"], a["hours"]), frac(a["unsure_rows"], a["rows"])))
    return rows


def fmt(v, nd=2):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%." + str(nd) + "f") % v
    return str(v)


def render(reels, per, lanes, gate_lane, gates, done, cfg, ab):
    L = []
    L.append("# Source identity evaluation")
    L.append("")
    scored = sorted(set(k[0] for k in per))
    hours = sum(reels[r]["duration"] for r in scored if r in reels) / 3600.0
    L.append("Reels scored: %d of %d in the manifest (%.2f reel-hours). Lanes: %s. GO table lane: **%s**." % (len(scored), len(reels), hours, ", ".join(lanes), gate_lane))
    bad_done = [d for d in done.values() if d.get("state") != "ok"]
    if bad_done:
        L.append("")
        L.append("**Harness problems:** " + ", ".join("%s (%s)" % (d["reel"], d["state"]) for d in bad_done[:12]))
    L.append("")
    L.append("## GO table (%s lane)" % gate_lane)
    L.append("")
    L.append("| Field | Criterion | Value | Status |")
    L.append("|---|---|---|---|")
    for gt in gates:
        val = "" if gt["value"] is None else ", ".join("%s=%s" % (k, fmt(v, 3)) for k, v in gt["value"].items())
        L.append("| %s | %s | %s | **%s** |" % (gt["field"], gt["criterion"], val, gt["status"]))
    notes = [gt for gt in gates if gt["detail"]]
    if notes:
        L.append("")
        for gt in notes:
            L.append("- %s: %s" % (gt["id"], gt["detail"]))
    L.append("")
    r1h = aggregate(reels, per, gate_lane, family_pred("R1"), cfg)["hours"]
    L.append("Statistics: the design target is <= 1 confident-wrong per 100 title-hours. Zero events in H hours only bounds the rate at 3/H per hour "
             "(95 percent). This run supports a bound of " + (("%.1f" % (3.0 / r1h)) if r1h > 0 else "n/a") +
             " per hour on the R1 native reels; the reels are about 30 s long, not title-length.")
    for lane in lanes:
        L.append("")
        L.append("## Lane %s by family" % lane)
        L.append("")
        L.append("| Family | Reels | Hours | Confident-wrong | Reels with wrong | Wrong/h | Latency median s | p95 s | Exact-n when latched | Flaps/h | UNSURE share |")
        L.append("|---|---|---|---|---|---|---|---|---|---|---|")
        for row in family_table(reels, per, lane, cfg):
            L.append("| " + " | ".join(fmt(x, 3) if isinstance(x, float) else str(x) if x is not None else "-" for x in row) + " |")
        a = aggregate(reels, per, lane, lambda r: True, cfg)
        if a["reasons"]:
            tot = float(sum(a["reasons"].values()))
            L.append("")
            L.append("UNSURE share by reason (rows in UNKNOWN / ABSTAIN / PROVISIONAL): " + ", ".join("%s %.1f%%" % (k, 100 * v / tot) for k, v in sorted(a["reasons"].items(), key=lambda kv: -kv[1])[:10]))
        if a["bad"]:
            L.append("")
            L.append("Confident-wrong episodes (first 20):")
            for b in a["bad"][:20]:
                L.append("- " + b)
    return "\n".join(L) + "\n"


def score(reels_path, eval_dir, out_dir, cfg, gate_lane=None):
    reels = load_manifest(reels_path)
    rows, events, done = load_eval(eval_dir)
    lanes = sorted(set(k[1] for k in rows), key=lambda x: ["off", "on", "chain"].index(x) if x in ("off", "on", "chain") else 9)
    per = {}
    for (rid, lane), rr in rows.items():
        if rid not in reels:
            continue
        per[(rid, lane)] = score_reel_lane(reels[rid], rr, events.get((rid, lane), []), cfg)
    if not lanes:
        raise SystemExit("no evaluation rows in %s" % eval_dir)
    gl = gate_lane or ("chain" if "chain" in lanes else ("on" if "on" in lanes else lanes[0]))
    gates, ab = go_table(reels, per, gl, cfg)
    os.makedirs(out_dir, exist_ok=True)
    md = render(reels, per, lanes, gl, gates, done, cfg, ab)
    with open(os.path.join(out_dir, "summary.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(md)
    all_lanes = {}
    for lane in lanes:
        gt, _ = go_table(reels, per, lane, cfg)
        all_lanes[lane] = gt
    with open(os.path.join(out_dir, "gates.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(dict(gate_lane=gl, config=cfg, gates=gates, lanes=all_lanes,
                       counts={s: sum(1 for g in gates if g["status"] == s) for s in ("PASS", "FAIL", "INSUFFICIENT", "NA")}), f, indent=1)
    return reels, per, gates, lanes


def default_cfg(args=None):
    return dict(boundary_grace=getattr(args, "boundary_grace", 3.0), flap_window=getattr(args, "flap_window", 60.0),
                abr_tol=getattr(args, "abr_tol", 2.0), menu_persist=getattr(args, "menu_persist", 3.0))


# ------------------------------------------------------------------ self test
def selftest():
    tmp = tempfile.mkdtemp(prefix="sidscore_")
    W, H = 3840, 2160

    def seg(idx, start, frames, n, regime="stream", **kw):
        d = dict(idx=idx, kind=regime, regime=regime, out_start_frame=start, out_frames=frames, truth_n=dict(x=n[0], y=n[1]), native_coded=(n == (W, H)),
                 hold_expected=False, kernel=None, sharpening=None, double=None, active_area_out=dict(x=0, y=0, w=W, h=H), bits=10, dither="none")
        d.update(kw)
        return d

    def reel(rid, fam, segs, events=(), master="m1", cls="nature"):
        n = sum(s["out_frames"] for s in segs)
        return dict(id=rid, family=fam, title="", master=master, fps=[24, 1], width=W, height=H, pix_fmt="gray16le", frames=n, duration_s=n / 24.0,
                    looped=False, flags=[], inputs=[], filter_complex="", segments=segs, events=list(events), truth_csv="")
    up = (1920, 1080)
    kern = dict(name="lanczos3", family="lanczos3", backend="sws", params={})
    doc = dict(schema=1, masters={"m1": dict(cls="nature"), "m2": dict(cls="film"), "m3": dict(cls="sport")}, rungs={}, reels=[
        reel("R2.clean.A", "R2", [seg(0, 0, 720, up, kernel=kern)], master="m1", cls="nature"),
        reel("R1.B", "R1", [seg(0, 0, 720, (W, H))], master="m2"),
        reel("R2.dirty.C", "R2", [seg(0, 0, 720, up)], master="m3"),
        reel("R6.D", "R6", [seg(0, 0, 360, up), seg(1, 360, 360, (1280, 720))], master="m1",
             events=[dict(frame=360, kind="geometry", cls="major", direction="drop", from_seg=0, to_seg=1, t=0)]),
        reel("R2.clean.E", "R2", [seg(0, 0, 720, up)], master="m2"),
        reel("R7.F", "R7", [seg(0, 0, 240, up), seg(1, 240, 240, (W, H), regime="menu"), seg(2, 480, 240, up)], master="m3"),
    ])
    with open(os.path.join(tmp, "reels.json"), "w") as f:
        json.dump(doc, f)
    ev = []

    def add(reel_id, lane, t, state, n, reason="none", **kw):
        r = dict(reel=reel_id, lane=lane, out_frame=int(t * 24), t=t, seg=0, bits=10, fx="", fy="", n_x=n[0] if n else "", n_y=n[1] if n else "", state=state,
                 declared="1.0", conf="", frame_quality="", noise="", classical_quality="", tier="", level="", rec_ratio_h="", rec_ratio_v="", gen="", reason=reason, ms="5",
                 kernel="", sharpen_amount="", double_n1_x="", double_n1_y="", active_x="", active_y="", active_w="", active_h="", llr_x="", llr_y="")
        r.update(kw)
        ev.append(r)
    # A: unknown for 6 s then latched correct with the right kernel
    for i in range(60):
        t = i * 0.5
        if t < 6:
            add("R2.clean.A", "chain", t, "UNKNOWN", None, "no_power")
        else:
            add("R2.clean.A", "chain", t, "LATCHED", up, kernel="lanczos3")
    # B: native reel never latched (UNKNOWN) -> no wrong
    for i in range(60):
        add("R1.B", "chain", i * 0.5, "UNKNOWN", None, "dark")
    # C: dirty reel latched at the wrong n after 5 s -> one confident-wrong episode
    for i in range(60):
        t = i * 0.5
        add("R2.dirty.C", "chain", t, "UNKNOWN" if t < 5 else "LATCHED", None if t < 5 else (1280, 720), "sharpened")
    # D: latches 1080 then follows the truth change to 720 at 15 s (after the grace) and flaps once in the second segment is NOT expected
    for i in range(60):
        t = i * 0.5
        if t < 4:
            add("R6.D", "chain", t, "UNKNOWN", None)
        elif t < 15.5:
            add("R6.D", "chain", t, "LATCHED", up)
        elif t < 20:
            add("R6.D", "chain", t, "LATCHED", up)       # stale for 5 s: judged wrong after the 3 s grace
        else:
            add("R6.D", "chain", t, "LATCHED", (1280, 720))
    # E: latched correct then reverts with no truth change -> one flap
    for i in range(60):
        t = i * 0.5
        add("R2.clean.E", "chain", t, "LATCHED" if (t < 10 or t >= 12) else "UNKNOWN", up if (t < 10 or t >= 12) else None)
    # F: menu segment (10-20 s) still re-based > 3 s -> violation
    for i in range(60):
        t = i * 0.5
        add("R7.F", "chain", t, "LATCHED", up)
    with open(os.path.join(tmp, "evaluations.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(ev[0].keys()))
        w.writeheader()
        w.writerows(ev)
    with open(os.path.join(tmp, "events.csv"), "w", newline="") as f:
        f.write("reel,lane,t,out_frame,seg,type,a,b,detail\n")
        f.write("R6.D,chain,15.2,365,1,geometry,,,\n")
        f.write("R2.clean.A,chain,3.0,72,0,notice,,,x\n")
    with open(os.path.join(tmp, "reels_done.csv"), "w") as f:
        f.write("reel,frames_expected,frames_decoded,wall_s,state\n")
    cfg = default_cfg()
    reels, per, gates, lanes = score(os.path.join(tmp, "reels.json"), tmp, tmp, cfg)
    assert lanes == ["chain"], lanes
    A = per[("R2.clean.A", "chain")]
    assert not A["episodes"] and A["seg"][0]["first_correct"] == 6.0, A["seg"][0]
    assert not per[("R1.B", "chain")]["episodes"]
    C = per[("R2.dirty.C", "chain")]
    assert len(C["episodes"]) == 1 and C["episodes"][0]["n"] == (1280, 720), C["episodes"]
    D = per[("R6.D", "chain")]
    assert len(D["episodes"]) == 1 and D["seg"][1]["first_correct"] is not None and D["seg"][1]["first_correct"] == 5.0, (D["episodes"], D["seg"][1])
    E = per[("R2.clean.E", "chain")]
    assert E["flaps"] == 1, E["flaps"]
    Fm = per[("R7.F", "chain")]
    assert Fm["menu_violations"] >= 1, Fm["menu_violations"]
    by = {g["id"]: g for g in gates}
    assert by["axes.r1.wrong"]["status"] == "INSUFFICIENT" and by["axes.r1.wrong"]["value"]["confident_wrong"] == 0
    assert by["axes.r2.clean"]["status"] == "INSUFFICIENT"            # 2 clips, 1 min: not enough to judge
    assert by["axes.dirty"]["status"] == "FAIL"
    assert by["axes.r7"]["status"] == "FAIL"
    assert by["kernel"]["status"] in ("PASS", "NA") and by["axes.r10"]["status"] == "NA"
    assert by["cost"]["status"] == "PASS" and by["abr"]["status"] in ("PASS", "FAIL")
    assert os.path.exists(os.path.join(tmp, "summary.md")) and os.path.exists(os.path.join(tmp, "gates.json"))
    with open(os.path.join(tmp, "gates.json")) as f:
        js = json.load(f)
    assert js["gate_lane"] == "chain" and js["counts"]["FAIL"] >= 2
    print("score_identity selftest ok (%s)" % tmp)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reels", default="D:/testclips/sourceid/reels.json")
    ap.add_argument("--eval-dir", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--gate-lane", default="")
    ap.add_argument("--boundary-grace", type=float, default=3.0)
    ap.add_argument("--flap-window", type=float, default=60.0)
    ap.add_argument("--abr-tol", type=float, default=2.0)
    ap.add_argument("--menu-persist", type=float, default=3.0)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.eval_dir:
        ap.error("--eval-dir is required")
    cfg = default_cfg(args)
    reels, per, gates, lanes = score(args.reels, args.eval_dir, args.out or args.eval_dir, cfg, args.gate_lane or None)
    counts = {s: sum(1 for g in gates if g["status"] == s) for s in ("PASS", "FAIL", "INSUFFICIENT", "NA")}
    print("scored %d reel/lane pairs; GO table: %s" % (len(per), counts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
