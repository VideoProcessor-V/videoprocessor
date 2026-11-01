# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Soak report generator (stdlib only).

python -I tools/soak/report.py --out DIR [--thresholds FILE] [--warmup-min 30]

Exit code: 0 PASS, 1 FAIL, 3 INCOMPLETE (some NO DATA and no FAIL), 2 usage error.
"""

import argparse
import csv
import datetime
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(_HERE), "vplog"))
import parse  # noqa: E402

PASS, FAIL, NODATA, INCOMPLETE = "PASS", "FAIL", "NO DATA", "INCOMPLETE"
MB = 1024.0 * 1024.0

DEFAULTS = {
    "warmup_min": 30, "min_samples": 3, "private_bytes_mb_per_h": 2, "vram_mb_per_h": 4,
    "handles_per_h": 50, "thread_range": 8, "drops_per_h": 1, "steady_window_s": 10,
    "p99_creep_pct": 10, "min_hours_creep": 2,
}
CSV_COLUMNS = ["utc", "elapsed_s", "private_bytes", "working_set", "handles", "threads",
               "gdi_objects", "user_objects", "gpu_dedicated", "gpu_shared"]
_DEVREM_RE = re.compile(r"DEVICE_REMOVED|DEVICE_HUNG|DEVICE_RESET|device[ _-]removed|device[ _-]lost",
                        re.I)
_PREFIX_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) \| ")
_BAD_CAUSES = ("", "none", "unknown")


def load_thresholds(path):
    """Defaults overlaid by the JSON file. Returns (thresholds, notes)."""
    th = dict(DEFAULTS)
    notes = []
    if path:
        with open(path, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
        for k, v in data.items():
            if k in DEFAULTS and isinstance(v, (int, float)) and not isinstance(v, bool):
                th[k] = v
            else:
                notes.append("thresholds: ignored key %r" % k)
    return th, notes


def linear_slope(xs, ys):
    """Least-squares slope dy/dx; None when fewer than 2 points or no x spread."""
    n = len(xs)
    if n < 2:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx <= 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def _num(s):
    try:
        if s is None or str(s).strip() == "":
            return None
        return float(s)
    except ValueError:
        return None


def read_csv(path):
    """Returns (rows, problem). rows = list of dicts with floats/None; problem text or ''."""
    if not os.path.isfile(path):
        return [], "soak.csv missing"
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        missing = [c for c in CSV_COLUMNS if c not in cols]
        if missing:
            return [], "soak.csv lacks columns: " + ",".join(missing)
        rows = []
        for r in rd:
            e = _num(r.get("elapsed_s"))
            if e is None:
                continue
            row = {"utc": r.get("utc", ""), "elapsed_s": e}
            for c in CSV_COLUMNS[2:]:
                row[c] = _num(r.get(c))
            rows.append(row)
    if not rows:
        return [], "soak.csv has no data rows"
    return rows, ""


def _run_start(rows, events):
    """Local naive wall time of the run start: first csv utc, else first event wall."""
    if rows:
        try:
            d = datetime.datetime.fromisoformat(rows[0]["utc"].replace("Z", "+00:00"))
            if d.tzinfo is None:
                d = d.replace(tzinfo=datetime.timezone.utc)
            return (d.astimezone().replace(tzinfo=None)
                    - datetime.timedelta(seconds=rows[0]["elapsed_s"]))
        except ValueError:
            pass
    for e in events:
        if e.wall is not None:
            return e.wall
    return None


def load_logs(collected):
    """Parse collected logs, oldest first: vp.log.N ... vp.log.0, then vp.log."""
    out = []
    if not os.path.isdir(collected):
        return out
    rotated = []
    live = False
    for n in os.listdir(collected):
        m = re.match(r"^vp\.log\.(\d+)$", n)
        if m:
            rotated.append((int(m.group(1)), n))
        elif n == "vp.log":
            live = True
    names = [n for _i, n in sorted(rotated, reverse=True)] + (["vp.log"] if live else [])
    for n in names:
        p = os.path.join(collected, n)
        out.append((p, parse.parse_file(p)))
    return out


def assign_times(files, start):
    """Adds ts (seconds since run start, or None) and pre to every event. Returns pre count."""
    pre = 0
    for _p, evs in files:
        anchor = None
        for e in evs:
            ts = None
            if e.wall is not None and start is not None:
                ts = (e.wall - start).total_seconds()
                if e.t_ms is not None:
                    anchor = (ts, e.t_ms)
            elif e.t_ms is not None and anchor is not None:
                ts = anchor[0] + (e.t_ms - anchor[1]) / 1000.0
            e.ts = ts
            e.pre = ts is not None and ts < -2.0
            if e.pre:
                pre += 1
    return pre


def _result(cid, verdict, detail, events=None, path=""):
    return {"id": cid, "verdict": verdict, "detail": detail, "events": events or [], "log_path": path}


def slope_check(cid, label, rows, col, th, thr_key, unit_div, unit, warmup_s, flat_range=False):
    vals = [(r["elapsed_s"], r[col]) for r in rows if r["elapsed_s"] >= warmup_s and r[col] is not None]
    if not rows:
        return _result(cid, NODATA, "%s: no soak.csv samples" % label)
    if len(vals) < th["min_samples"]:
        return _result(cid, NODATA, "%s: %d samples after %g min warm-up (need %d)"
                       % (label, len(vals), warmup_s / 60.0, th["min_samples"]))
    lim = th[thr_key]
    if flat_range:
        rng = max(v for _, v in vals) - min(v for _, v in vals)
        return _result(cid, FAIL if rng > lim else PASS,
                       "%s post-warm-up range %.0f (limit %g, %d samples)" % (label, rng, lim, len(vals)))
    s = linear_slope([x / 3600.0 for x, _ in vals], [v / unit_div for _, v in vals])
    if s is None:
        return _result(cid, NODATA, "%s: samples span no time" % label)
    return _result(cid, FAIL if s > lim else PASS,
                   "%s slope %.3f %s/h (limit %g, %d samples)" % (label, s, unit, lim, len(vals)))


def _all_events(files):
    for p, evs in files:
        for e in evs:
            if not e.pre:
                yield p, e


def _ev(e):
    return {"line_no": e.line_no, "raw": e.raw, "wall": e.wall}


def check_gpu(files):
    if not files:
        return _result("gpu_errors", NODATA, "no collected vp.log")
    hits = []
    seen = set()
    for p, e in _all_events(files):
        if e.kind == "gpu_error":
            hits.append((p, e.line_no, e.raw, e.wall))
            seen.add((p, e.line_no))
    # raw scan: device-removed lines that no legacy adapter matches
    for p, _evs in files:
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                for i, line in enumerate(f, 1):
                    if (p, i) in seen or not _DEVREM_RE.search(line):
                        continue
                    wall = None
                    m = _PREFIX_RE.match(line)
                    if m:
                        wall = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
                    hits.append((p, i, line.rstrip("\r\n"), wall))
        except OSError:
            pass
    if not hits:
        return _result("gpu_errors", PASS, "no device-removed / gpu_error / libplacebo [error] lines")
    hits.sort(key=lambda h: (h[3] is None, h[3] or datetime.datetime.min, h[1]))
    evs = [{"line_no": h[1], "raw": h[2], "wall": h[3]} for h in hits]
    return _result("gpu_errors", FAIL, "%d device-removed / gpu_error events" % len(hits), evs, hits[0][0])


def steady_hours(files, duration_s, warmup_s, window):
    """Hours after warm-up outside +/-window of source_change / full resync events."""
    spans = []
    for _p, e in _all_events(files):
        if e.ts is None:
            continue
        if e.kind == "source_change" or (e.kind == "resync" and e.fields.get("level") == "full"):
            lo, hi = max(e.ts - window, warmup_s), min(e.ts + window, duration_s)
            if hi > lo:
                spans.append((lo, hi))
    spans.sort()
    excluded, cur = 0.0, None
    for lo, hi in spans:
        if cur is None or lo > cur[1]:
            if cur:
                excluded += cur[1] - cur[0]
            cur = [lo, hi]
        else:
            cur[1] = max(cur[1], hi)
    if cur:
        excluded += cur[1] - cur[0]
    return max(0.0, duration_s - warmup_s - excluded) / 3600.0, spans


def check_drops(files, duration_s, warmup_s, th):
    if not files:
        return _result("drops_repeats", NODATA, "no collected vp.log")
    hrs, spans = steady_hours(files, duration_s, warmup_s, th["steady_window_s"])
    if hrs <= 0:
        return _result("drops_repeats", NODATA, "no steady time after %g min warm-up" % (warmup_s / 60.0))
    drops = []
    for p, e in _all_events(files):
        if e.ts is None or e.ts < warmup_s or e.ts > duration_s + 5:
            continue
        if parse.category(e) != "drop":
            continue
        if any(lo <= e.ts <= hi for lo, hi in spans):
            continue
        drops.append((p, e))
    rate = len(drops) / hrs
    ok = rate <= th["drops_per_h"]
    return _result("drops_repeats", PASS if ok else FAIL,
                   "%d drops/repeats in %.2f steady h = %.2f/h (limit %g)"
                   % (len(drops), hrs, rate, th["drops_per_h"]),
                   [] if ok else [_ev(e) for _p, e in drops],
                   "" if ok else drops[0][0])


def check_resync(files):
    if not files:
        return _result("resync_causes", NODATA, "no collected vp.log")
    counts = {}
    bad = []
    for p, e in _all_events(files):
        if e.kind != "resync":
            continue
        cause = e.fields.get("cause", "")
        counts[cause] = counts.get(cause, 0) + 1
        if e.fields.get("level") == "full" and cause in _BAD_CAUSES:
            bad.append((p, e))
    listing = ", ".join("%s=%d" % kv for kv in sorted(counts.items())) or "none"
    if bad:
        return _result("resync_causes", FAIL,
                       "%d full resync(s) with no cause; counts: %s" % (len(bad), listing),
                       [_ev(e) for _p, e in bad], bad[0][0])
    return _result("resync_causes", PASS, "every full resync has a cause; counts: %s" % listing)


def check_fallback(files):
    if not files:
        return _result("fallback_notice", NODATA, "no collected vp.log")
    eng = [e for _p, e in _all_events(files)
           if e.kind == "fallback" and e.fields.get("state") == "engaged"]
    if not eng:
        return _result("fallback_notice", PASS, "no fallback engaged")
    return _result("fallback_notice", NODATA,
                   "%d fallback engagement(s); verifying a visible notice needs a notice event, "
                   "which does not exist yet: NO DATA until it does" % len(eng))


def check_creep(files, duration_s, th):
    stats = []
    for _p, e in _all_events(files):
        if e.kind == "frame_stats" and e.ts is not None:
            v = _num(e.fields.get("p99_ms"))
            if v is not None:
                stats.append((e.ts, v))
    if not stats:
        return _result("frame_time_creep", NODATA, "no frame_stats lines"), stats
    if duration_s < th["min_hours_creep"] * 3600.0:
        return _result("frame_time_creep", NODATA, "run %.2f h shorter than %g h"
                       % (duration_s / 3600.0, th["min_hours_creep"])), stats
    first = [v for t, v in stats if t < 3600.0]
    last = [v for t, v in stats if t >= duration_s - 3600.0]
    if not first or not last or sum(first) <= 0:
        return _result("frame_time_creep", NODATA, "no frame_stats in the first or last hour"), stats
    a, b = sum(first) / len(first), sum(last) / len(last)
    pct = (b - a) / a * 100.0
    return _result("frame_time_creep", FAIL if pct > th["p99_creep_pct"] else PASS,
                   "p99 first hour %.3f ms, last hour %.3f ms: %+.1f%% (limit %g%%)"
                   % (a, b, pct, th["p99_creep_pct"])), stats


def check_crash(out):
    p = os.path.join(out, "soak-exit.json")
    if not os.path.isfile(p):
        return _result("crash", NODATA, "soak-exit.json missing")
    try:
        with open(p, "r", encoding="utf-8-sig") as f:
            d = json.load(f)
    except (OSError, ValueError) as ex:
        return _result("crash", NODATA, "soak-exit.json unreadable: %s" % ex)
    if d.get("crash"):
        return _result("crash", FAIL, "process exited unexpectedly: exit_code=%s reason=%s"
                       % (d.get("exit_code"), d.get("reason", "")))
    return _result("crash", PASS, "no crash (reason=%s)" % d.get("reason", ""))


def svg_plot(path, title, xlabel, ylabel, series):
    """series: list of (x, y). Minimal polyline plot, no libraries."""
    w, h, ml, mr, mt, mb = 640, 320, 70, 20, 30, 45
    xs, ys = [p[0] for p in series], [p[1] for p in series]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    if x1 == x0:
        x1 = x0 + 1
    if y1 == y0:
        y1 = y0 + 1

    def px(x):
        return ml + (x - x0) / (x1 - x0) * (w - ml - mr)

    def py(y):
        return h - mb - (y - y0) / (y1 - y0) * (h - mt - mb)

    def esc(s):
        return s.replace("&", "&amp;").replace("<", "&lt;")

    pts = " ".join("%.1f,%.1f" % (px(x), py(y)) for x, y in series)
    svg = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
        'font-family="sans-serif" font-size="11">' % (w, h, w, h),
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="%d" y="18" font-size="13">%s</text>' % (ml, esc(title)),
        '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="black"/>' % (ml, h - mb, w - mr, h - mb),
        '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="black"/>' % (ml, mt, ml, h - mb),
        '<polyline fill="none" stroke="#1f77b4" stroke-width="1.5" points="%s"/>' % pts,
        '<text x="%d" y="%d" text-anchor="end">%.4g</text>' % (ml - 4, mt + 4, y1),
        '<text x="%d" y="%d" text-anchor="end">%.4g</text>' % (ml - 4, h - mb, y0),
        '<text x="%d" y="%d">%.4g</text>' % (ml, h - mb + 14, x0),
        '<text x="%d" y="%d" text-anchor="end">%.4g</text>' % (w - mr, h - mb + 14, x1),
        '<text x="%d" y="%d" text-anchor="middle">%s</text>' % ((ml + w - mr) // 2, h - 8, esc(xlabel)),
        '<text x="12" y="%d" transform="rotate(-90 12 %d)" text-anchor="middle">%s</text>'
        % (h // 2, h // 2, esc(ylabel)),
        '</svg>']
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(svg) + "\n")


def build_id_of(files):
    for _p, e in _all_events(files):
        if e.kind == "session" and e.fields.get("build_id"):
            return e.fields["build_id"]
    return "unknown"


def run(out, th_path=None, warmup_min=None):
    out = os.path.abspath(out)
    th, notes = load_thresholds(th_path)
    if warmup_min is not None:
        th["warmup_min"] = warmup_min
    warmup_s = th["warmup_min"] * 60.0
    rows, problem = read_csv(os.path.join(out, "soak.csv"))
    collected = os.path.join(out, "collected")
    files = load_logs(collected)
    start = _run_start(rows, [e for _p, evs in files for e in evs])
    pre = assign_times(files, start)
    if pre:
        notes.append("%d event(s) before the run start (earlier session) ignored" % pre)
    if problem:
        notes.append(problem)
    last_ts = max([e.ts for _p, e in _all_events(files) if e.ts is not None] or [0.0])
    duration_s = rows[-1]["elapsed_s"] if rows else last_ts

    results = [
        slope_check("private_bytes_slope", "private bytes", rows, "private_bytes", th,
                    "private_bytes_mb_per_h", MB, "MB", warmup_s),
        slope_check("vram_slope", "VRAM (gpu_dedicated)", rows, "gpu_dedicated", th,
                    "vram_mb_per_h", MB, "MB", warmup_s),
        slope_check("handles_slope", "handles", rows, "handles", th,
                    "handles_per_h", 1.0, "handles", warmup_s),
        slope_check("thread_flat", "threads", rows, "threads", th,
                    "thread_range", 1.0, "", warmup_s, flat_range=True),
        check_gpu(files),
        check_drops(files, duration_s, warmup_s, th),
        check_resync(files),
        check_fallback(files),
    ]
    creep, stats = check_creep(files, duration_s, th)
    results.append(creep)
    results.append(check_crash(out))

    verdicts = [r["verdict"] for r in results]
    if FAIL in verdicts:
        overall = FAIL
    elif NODATA in verdicts:
        overall = INCOMPLETE
    else:
        overall = PASS

    plots = []

    def plot(name, title, ylabel, pts):
        if len(pts) >= 2:
            svg_plot(os.path.join(out, name), title, "elapsed (h)", ylabel, pts)
            plots.append(name)

    plot("plot-memory.svg", "Private bytes", "MB",
         [(r["elapsed_s"] / 3600.0, r["private_bytes"] / MB) for r in rows if r["private_bytes"] is not None])
    plot("plot-vram.svg", "GPU dedicated memory", "MB",
         [(r["elapsed_s"] / 3600.0, r["gpu_dedicated"] / MB) for r in rows if r["gpu_dedicated"] is not None])
    plot("plot-handles.svg", "Handles", "count",
         [(r["elapsed_s"] / 3600.0, r["handles"]) for r in rows if r["handles"] is not None])
    plot("plot-frametime.svg", "Frame time p99", "ms", [(t / 3600.0, v) for t, v in sorted(stats)])

    bid = build_id_of(files)
    vplog = os.path.join(collected, "vp.log")
    seeds = []
    for r in results:
        if r["verdict"] != FAIL:
            continue
        tdir = os.path.join(out, "triage")
        os.makedirs(tdir, exist_ok=True)
        evs = r["events"][:10]
        wall = ""
        if evs and evs[0].get("wall") is not None:
            wall = evs[0]["wall"].isoformat(sep=" ")
        seed = {"source": "soak", "check": r["id"], "verdict": "FAIL", "detail": r["detail"],
                "build_id": bid, "out_dir": out,
                "log_path": r["log_path"] or (vplog if os.path.isfile(vplog) else ""),
                "trigger_wall": wall,
                "first_events": [{"line_no": e["line_no"], "raw": e["raw"]} for e in evs]}
        with open(os.path.join(tdir, r["id"] + ".json"), "w", encoding="utf-8") as f:
            json.dump(seed, f, indent=2)
        seeds.append(r["id"])

    stamp = (rows[0]["utc"][:10].replace("-", "") if rows and len(rows[0]["utc"]) >= 10
             else datetime.date.today().strftime("%Y%m%d"))
    md = ["# Soak report %s" % stamp, "", "Overall: **%s**" % overall, "",
          "Run length: %.2f h, %d samples, warm-up %g min, build %s"
          % (duration_s / 3600.0, len(rows), th["warmup_min"], bid), "",
          "| Check | Verdict | Detail |", "|---|---|---|"]
    for r in results:
        md.append("| %s | %s | %s |" % (r["id"], r["verdict"], r["detail"].replace("|", "/")))
    for r in results:
        if r["verdict"] == FAIL and r["events"]:
            md += ["", "### First offending events: %s" % r["id"], ""]
            for e in r["events"][:10]:
                md.append("- line %d: `%s`" % (e["line_no"], e["raw"].replace("`", "'")))
    if notes:
        md += ["", "### Notes", ""] + ["- " + n for n in notes]
    if plots:
        md += ["", "### Plots", ""] + ["- %s" % p for p in plots]
    with open(os.path.join(out, "soak-%s.md" % stamp), "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")

    rep = {"overall": overall, "thresholds": th, "duration_s": duration_s, "samples": len(rows),
           "build_id": bid, "notes": notes, "plots": plots, "triage": seeds,
           "checks": [{"id": r["id"], "verdict": r["verdict"], "detail": r["detail"],
                       "first_events": [{"line_no": e["line_no"], "raw": e["raw"]}
                                        for e in r["events"][:10]]} for r in results]}
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=2)
    return overall


def main(argv=None):
    ap = argparse.ArgumentParser(description="VideoProcessor soak report")
    ap.add_argument("--out", required=True)
    ap.add_argument("--thresholds")
    ap.add_argument("--warmup-min", type=float)
    a = ap.parse_args(argv)
    if not os.path.isdir(a.out):
        print("report: --out is not a directory: %s" % a.out, file=sys.stderr)
        return 2
    th = a.thresholds
    if th is None and os.path.isfile(os.path.join(_HERE, "thresholds.json")):
        th = os.path.join(_HERE, "thresholds.json")
    overall = run(a.out, th, a.warmup_min)
    print("soak verdict: %s" % overall)
    return {PASS: 0, FAIL: 1}.get(overall, 3)


if __name__ == "__main__":
    sys.exit(main())
