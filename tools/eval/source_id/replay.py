# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Scores the source-identity evaluation traces (doc 61 section 2.5).

Input folder (written by make_abr_reels.py and then by the C++ SourceIdStreamingEvalTests with VP_SOURCE_ID_EVAL_DIR = this folder):
reels.tsv truth.tsv shots.tsv frames.csv events.csv engages.csv notices.csv.  Writes results.md and results.json next to them.

NR metrics are weak (tools/eval/findings-streaming.md), so nothing here uses an absolute quality threshold: events are scored
against the splice points, quality is compared as a RELATIVE drop against the same stream's own top rung, and the false-alarm
numbers come from the no-switch control reels. Pure standard library.

  python -I tools/eval/source_id/replay.py --dir D:/codex/videoprocessor/eval-scratch/source-id
"""
import argparse
import csv
import json
import os
import statistics
from collections import defaultdict

TOL_S = 1.0   # an event matches a splice when its boundary is within this many seconds (2 analysis intervals)


def rows(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t" if path.endswith(".tsv") else ","))


def rank(values):
    order = sorted(range(len(values)), key=lambda i: values[i])
    r = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2.0 + 1.0
        i = j + 1
    return r


def spearman(a, b):
    if len(a) < 3:
        return None
    ra, rb = rank(a), rank(b)
    ma, mb = statistics.mean(ra), statistics.mean(rb)
    num = sum((x - ma) * (y - mb) for x, y in zip(ra, rb))
    da = sum((x - ma) ** 2 for x in ra) ** 0.5
    db = sum((y - mb) ** 2 for y in rb) ** 0.5
    return num / (da * db) if da > 0 and db > 0 else None


def pct(v, q):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


def fnum(x, nd=2):
    return "n/a" if x is None else ("%.*f" % (nd, x))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    args = ap.parse_args()
    d = args.dir
    reels = {r["id"]: r for r in rows(os.path.join(d, "reels.tsv"))}
    truth = defaultdict(list)
    for r in rows(os.path.join(d, "truth.tsv")):
        truth[r["id"]].append(r)
    events = defaultdict(list)
    for r in rows(os.path.join(d, "events.csv")):
        events[r["reel"]].append(r)
    frames = defaultdict(list)
    for r in rows(os.path.join(d, "frames.csv")):
        frames[r["reel"]].append(r)
    engages = defaultdict(list)
    for r in rows(os.path.join(d, "engages.csv")):
        engages[r["reel"]].append(r)
    notices = defaultdict(list)
    for r in rows(os.path.join(d, "notices.csv")):
        notices[r["reel"]].append(r)
    shots = defaultdict(list)
    for r in rows(os.path.join(d, "shots.tsv")):
        shots[r["id"]].append(r)
    ran = [rid for rid in reels if rid in frames]
    if not ran:
        raise SystemExit("no traces for any reel in %s" % d)

    def duration(rid):
        fr = frames[rid]
        return float(fr[-1]["t"]) if fr else 0.0

    # ---- event detection against the splice points ------------------------------------------------------------------
    stat = {"major": {"truth": 0, "tp": 0, "lat": [], "err": []}, "minor": {"truth": 0, "tp": 0, "lat": [], "err": []}}
    by_dir = {"drop": {"truth": 0, "tp": 0}, "rise": {"truth": 0, "tp": 0}}
    fp_events = 0
    matched_minor_events = 0
    per_reel = []
    for rid in ran:
        fps = float(reels[rid]["fps"])
        used = set()
        res = []
        for tr in truth.get(rid, []):
            t = int(tr["frame"]) / fps
            cl = tr["class"]
            stat[cl]["truth"] += 1
            by_dir[tr["direction"]]["truth"] += 1 if cl == "major" else 0
            best = None
            for i, ev in enumerate(events.get(rid, [])):
                if i in used or ev["direction"] != tr["direction"]:
                    continue
                if abs(float(ev["event_t"]) - t) <= TOL_S:
                    best = i
                    break
            if best is not None:
                used.add(best)
                ev = events[rid][best]
                stat[cl]["tp"] += 1
                if cl == "major":
                    by_dir[tr["direction"]]["tp"] += 1
                stat[cl]["lat"].append(float(ev["detect_t"]) - t)
                stat[cl]["err"].append(float(ev["event_t"]) - t)
                res.append((tr["direction"], cl, "hit", float(ev["detect_t"]) - t))
            else:
                res.append((tr["direction"], cl, "miss", None))
        for i, ev in enumerate(events.get(rid, [])):
            if i in used:
                continue
            fp_events += 1
        per_reel.append((rid, res))
    n_events = sum(len(events.get(r, [])) for r in ran)
    precision = (stat["major"]["tp"] + stat["minor"]["tp"]) / n_events if n_events else None
    recall_major = stat["major"]["tp"] / stat["major"]["truth"] if stat["major"]["truth"] else None
    recall_minor = stat["minor"]["tp"] / stat["minor"]["truth"] if stat["minor"]["truth"] else None

    # ---- controls: false events, flaps, notices ---------------------------------------------------------------------
    controls = [r for r in ran if reels[r]["kind"] == "control"]
    def split_const(rid):
        base, rung = rid.split("_const_")
        h, k = rung.split("p_")
        return base, int(h), int(k)

    top_of = {}   # (stream, raster width) -> the highest rung of that ladder (native raster, highest rate)
    for r in controls:
        if "_const_" in r:
            base, h, k = split_const(r)
            key = (base, reels[r]["width"])
            if key not in top_of or (h, k) > top_of[key][0]:
                top_of[key] = ((h, k), r)
    top_controls = [v[1] for v in top_of.values()]
    ctl_hours = sum(duration(r) for r in controls) / 3600.0
    ctl_events = sum(len(events.get(r, [])) for r in controls)
    ctl_notices = sum(len(notices.get(r, [])) for r in controls)
    settle = 10.0   # the first seconds after a start set the tier (a change from unknown), not a false change

    def changes(rid):
        out = 0
        last = None
        for fr in frames[rid]:
            if float(fr["t"]) < settle:
                last = (fr["tier"], fr["ratioH"], fr["ratioV"])
                continue
            cur = (fr["tier"], fr["ratioH"], fr["ratioV"])
            if last is not None and cur != last:
                out += 1
            last = cur
        return out

    ctl_flaps = sum(changes(r) for r in controls)

    # ---- false engages ---------------------------------------------------------------------------------------------------
    def engaged_lane(rid, lane):
        return [e for e in engages.get(rid, []) if e["lane"] == lane and float(e["to_y"]) < 0.999]

    top_off = sum(1 for r in top_controls if engaged_lane(r, "off"))
    top_on = sum(1 for r in top_controls if engaged_lane(r, "on"))
    ctl_off = sum(1 for r in controls if engaged_lane(r, "off"))
    ctl_on = sum(1 for r in controls if engaged_lane(r, "on"))
    suppressed = [r for r in ran if engaged_lane(r, "off") and not engaged_lane(r, "on")]
    added = [r for r in ran if engaged_lane(r, "on") and not engaged_lane(r, "off")]

    # ---- notices on major drops -------------------------------------------------------------------------------------------
    drop_reels = [r for r in ran if any(t["direction"] == "drop" and t["class"] == "major" for t in truth.get(r, []))]
    noticed = [r for r in drop_reels if notices.get(r)]
    notice_lat = []
    for r in noticed:
        first_drop = min(int(t["frame"]) / float(reels[r]["fps"]) for t in truth[r] if t["direction"] == "drop" and t["class"] == "major")
        notice_lat.append(float(notices[r][0]["t"]) - first_drop)

    # ---- latch time: the consensus tier/ratio after the first major drop -------------------------------------------------
    latch = []
    for r in drop_reels:
        fps = float(reels[r]["fps"])
        t0 = min(int(t["frame"]) / fps for t in truth[r] if t["direction"] == "drop" and t["class"] == "major")
        before = [fr for fr in frames[r] if float(fr["t"]) < t0]
        if not before:
            continue
        tier0, rv0 = int(before[-1]["tier"]), float(before[-1]["ratioV"])
        for fr in frames[r]:
            if float(fr["t"]) < t0:
                continue
            if float(fr["ratioV"]) < rv0 - 0.05 or (int(fr["tier"]) >= 0 and int(fr["tier"]) < tier0):
                latch.append(float(fr["t"]) - t0)
                break

    # ---- relative quality ordering and starved shots ----------------------------------------------------------------------
    order_corr = []
    groups = defaultdict(list)
    for r in controls:
        if "_const_" not in r:
            continue
        base, rung = r.split("_const_")
        q = [float(fr["frame_quality"]) for fr in frames[r] if float(fr["t"]) >= 5.0]
        if q:
            groups[base + "@" + reels[r]["width"]].append((rung, statistics.median(q)))
    rung_rank = {}
    for base, lst in groups.items():
        def key(item):
            rung = item[0]
            h, k = rung.split("p_")
            return (-int(h), -int(k))   # higher raster, then higher rate = better
        lst.sort(key=key, reverse=True)   # ascending in quality
        rho = spearman(list(range(len(lst))), [m for _, m in lst])
        if rho is not None:
            order_corr.append((base, rho))
        rung_rank[base] = lst
    starved_x, starved_y = [], []
    for r in controls:
        if r not in shots:
            continue
        fps = float(reels[r]["fps"])
        per = []
        for s in shots[r]:
            a, b = int(s["start"]) / fps, (int(s["start"]) + int(s["frames"])) / fps
            q = [float(fr["frame_quality"]) for fr in frames[r] if a + 1.0 <= float(fr["t"]) < b]
            if len(q) >= 2 and float(s["complexity_kbps"]) > 0:
                per.append((float(s["kbps"]) / float(s["complexity_kbps"]), statistics.median(q)))
        if len(per) >= 3:
            mx = max(y for _, y in per)
            for x, y in per:
                starved_x.append(x)
                starved_y.append(y / mx if mx > 0 else 0.0)
    starved_rho = spearman(starved_x, starved_y)

    # ---- the report -------------------------------------------------------------------------------------------------------
    out = {
        "reels_run": len(ran), "controls": len(controls), "top_rung_controls": len(top_controls),
        "events": n_events, "false_events": fp_events,
        "precision_all": precision, "recall_major": recall_major, "recall_minor": recall_minor,
        "recall_major_drop": by_dir["drop"]["tp"] / by_dir["drop"]["truth"] if by_dir["drop"]["truth"] else None,
        "recall_major_rise": by_dir["rise"]["tp"] / by_dir["rise"]["truth"] if by_dir["rise"]["truth"] else None,
        "latency_major_median_s": pct(stat["major"]["lat"], 0.5), "latency_major_p95_s": pct(stat["major"]["lat"], 0.95),
        "boundary_error_median_s": pct(stat["major"]["err"], 0.5),
        "control_false_events": ctl_events, "control_hours": ctl_hours,
        "control_false_events_per_hour": ctl_events / ctl_hours if ctl_hours else None,
        "control_flaps": ctl_flaps, "control_flaps_per_hour": ctl_flaps / ctl_hours if ctl_hours else None,
        "control_notices": ctl_notices, "control_notices_per_title_hour": ctl_notices / ctl_hours if ctl_hours else None,
        "major_drop_reels": len(drop_reels), "major_drop_noticed": len(noticed),
        "notice_latency_median_s": pct(notice_lat, 0.5), "notice_latency_p95_s": pct(notice_lat, 0.95),
        "latch_latency_median_s": pct(latch, 0.5), "latch_latency_p95_s": pct(latch, 0.95), "latched": len(latch),
        "false_engage_top_rung_off": top_off, "false_engage_top_rung_on": top_on, "top_rung_controls_n": len(top_controls),
        "controls_engaged_off": ctl_off, "controls_engaged_on": ctl_on,
        "engage_suppressed_by_consensus": suppressed, "engage_added_by_consensus": added,
        "rung_order_spearman": {b: r for b, r in order_corr},
        "rung_order_spearman_min": min((r for _, r in order_corr), default=None),
        "starved_shot_spearman": starved_rho, "starved_shot_n": len(starved_x),
    }
    with open(os.path.join(d, "results.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1, sort_keys=True)

    L = []
    L.append("# Source identity: streaming-suite evaluation\n")
    L.append("Reels run: %d (controls %d, of which top-rung controls %d). Scoring tolerance +-%.1f s around the splice.\n" % (len(ran), len(controls), len(top_controls), TOL_S))
    L.append("## ABR step detection\n")
    L.append("| | truth | detected | recall | latency median / p95 (s) | boundary error median (s) |\n|---|---|---|---|---|---|")
    for cl in ("major", "minor"):
        s = stat[cl]
        L.append("| %s | %d | %d | %s | %s / %s | %s |" % (cl, s["truth"], s["tp"], fnum(s["tp"] / s["truth"] if s["truth"] else None),
                                                       fnum(pct(s["lat"], 0.5)), fnum(pct(s["lat"], 0.95)), fnum(pct(s["err"], 0.5))))
    L.append("\nMajor drops: %s recall, major rises: %s recall. Events %d, matching a splice %d, false %d: precision %s.\n" % (
        fnum(out["recall_major_drop"]), fnum(out["recall_major_rise"]), n_events, n_events - fp_events, fp_events, fnum(precision)))
    L.append("## No-switch controls (false alarms)\n")
    L.append("Controls total %.3f h of stream: false ABR events %d (%s per hour), consensus changes after the first %.0f s %d (%s per hour), one-time notices %d (%s per title-hour).\n" % (
        ctl_hours, ctl_events, fnum(out["control_false_events_per_hour"], 1), settle, ctl_flaps, fnum(out["control_flaps_per_hour"], 1), ctl_notices,
        fnum(out["control_notices_per_title_hour"], 1)))
    L.append("## False engages on clean high-bitrate sources\n")
    L.append("Top-rung controls (native raster, highest rate of the ladder), n=%d: re-base engaged with consensus OFF %d, with consensus ON %d. All controls: off %d, on %d. Engages suppressed by consensus: %d reels; added by consensus: %d reels.\n" % (
        len(top_controls), top_off, top_on, ctl_off, ctl_on, len(suppressed), len(added)))
    if added:
        L.append("Added by consensus: %s\n" % ", ".join(added))
    L.append("## Notices and latch\n")
    L.append("Reels with a major drop: %d; one-time notice fired on %d (latency median %s s, p95 %s s). Consensus latched the lower state on %d reels (median %s s, p95 %s s after the splice).\n" % (
        len(drop_reels), len(noticed), fnum(out["notice_latency_median_s"]), fnum(out["notice_latency_p95_s"]), len(latch),
        fnum(out["latch_latency_median_s"]), fnum(out["latch_latency_p95_s"])))
    L.append("## Relative quality (frame-level signal, medians per rung, normalised only within a stream)\n")
    L.append("Rung ordering vs the frame-level signal, Spearman per stream (>= 0.9 wanted): min %s over %d streams. Starved shots (shot rate / shot complexity vs the shot's relative quality), Spearman %s over %d shots.\n" % (
        fnum(out["rung_order_spearman_min"]), len(order_corr), fnum(starved_rho), len(starved_x)))
    L.append("| stream | rung medians (ascending quality order) |\n|---|---|")
    for base, lst in sorted(rung_rank.items()):
        L.append("| %s | %s |" % (base, ", ".join("%s %.2f" % (r, m) for r, m in lst)))
    L.append("\n## Per reel\n")
    L.append("| reel | kind | events | result |\n|---|---|---|---|")
    for rid, res in per_reel:
        txt = "; ".join("%s %s %s" % (cl, dr, ("hit +%.1fs" % lat) if lat is not None else "MISS") for dr, cl, _, lat in res) or "-"
        L.append("| %s | %s | %d | %s |" % (rid, reels[rid]["kind"], len(events.get(rid, [])), txt))
    with open(os.path.join(d, "results.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("\n".join(L[:22]))


if __name__ == "__main__":
    main()
