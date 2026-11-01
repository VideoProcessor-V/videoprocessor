# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Scan the owner's recorded-stream folders (VideoProcessor-StreamRecorder output, the folders replayed by
RecordedStreamReplaysThroughTheRenderer via VP_TEST_STREAM_DIR) and report, per recording, the no-reference
quality over time and where it falls to "poor".

READ-ONLY on the recordings (files are only opened for reading); all output goes to --out, which must not be
inside a recording folder.  Never writes pixels.

A recording folder holds frames.csv always; pixels only when the recorder kept them:
  raw_NNNNN.bin   full frames (--raw N; first N frames only)      -> tier "full"  (8BitARGB / 8BitYUV decoded)
  nr.csv          per-frame NR columns written by a future recorder -> tier "full"
  thumbs.y8       240-px point-sampled 8-bit luma per frame        -> tier "thumbnail" (block/ring/band lose
                  meaning at that scale; series reported, never flagged poor)
  (neither)       timing-only recording: tier "timing-only" - cadence/timing summary, no quality possible.

  python -I tools/eval/scan_recordings.py --root D:/testclips/pulldown --out D:/codex/videoprocessor/eval-scratch/streaming-suite/recordings
"""
import argparse
import csv
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nr_metrics as nr  # noqa: E402

TICK = 1e7                    # frames.csv hardware times are 100 ns units
POOR_MIN_SECONDS = 2


def find_recordings(roots, max_depth=3):
    """Folders (under the roots, at most max_depth deep) that contain frames.csv, sorted."""
    found = []
    for root in roots:
        root = os.path.abspath(root)
        base = root.rstrip(os.sep).count(os.sep)
        for dp, dn, fn in os.walk(root):
            if dp.count(os.sep) - base >= max_depth:
                dn[:] = []
            if "frames.csv" in fn:
                found.append(dp)
    return sorted(set(found))


def read_frames_csv(path):
    with open(path, "r", encoding="utf8", newline="") as f:
        return list(csv.DictReader(f))


def timing_summary(rows):
    n = len(rows)
    if n == 0:
        return dict(frames=0)
    t = np.array([float(r["hw_time_100ns"]) for r in rows]) / TICK
    dur = np.array([float(r["hw_duration_100ns"]) for r in rows]) / TICK
    hashes = [r["hash"] for r in rows]
    distinct = sum(1 for i in range(n) if i == 0 or hashes[i] != hashes[i - 1])
    gaps = sum(1 for r in rows if int(r["periods_since_previous"]) > 1)
    no_in = sum(1 for r in rows if int(r["no_input"]) != 0)
    span = float(t[-1] - t[0] + dur[-1]) if n else 0.0
    return dict(frames=n, seconds=round(span, 2), nominal_hz=round(1.0 / float(np.median(dur)), 3) if np.median(dur) > 0 else None,
                distinct_pictures=distinct, picture_rate_hz=round(distinct / span, 3) if span > 0 else None,
                exact_repeat_share=round(1.0 - distinct / n, 4), gaps=gaps, no_input_frames=no_in)


def parse_events(path):
    info = dict(thumb=None, raw=[])
    if not os.path.exists(path):
        return info
    with open(path, "r", encoding="utf8", errors="replace") as f:
        for ln in f:
            m = re.search(r"thumbnails: (\d+)x(\d+)", ln)
            if m:
                info["thumb"] = (int(m.group(1)), int(m.group(2)))
            m = re.search(r"frame (\d+): raw frame saved \((\d+)x(\d+), (\d+) bytes per row, (\w+)\)", ln)
            if m:
                info["raw"].append(dict(frame=int(m.group(1)), w=int(m.group(2)), h=int(m.group(3)),
                                        stride=int(m.group(4)), fmt=m.group(5)))
    return info


def raw_luma(path, w, h, stride, fmt):
    """Luma (float, 8-bit units) of a saved raw frame; None for formats not decoded here."""
    data = np.fromfile(path, dtype=np.uint8)
    if data.size < stride * h:
        return None
    data = data[:stride * h].reshape(h, stride)
    if fmt == "8BitARGB":
        px = data[:, :4 * w].reshape(h, w, 4).astype(np.float64)       # recorder keeps the card's BGRA byte order
        return 0.0722 * px[:, :, 0] + 0.7152 * px[:, :, 1] + 0.2126 * px[:, :, 2]
    if fmt == "8BitYUV":
        return data[:, 1:2 * w:2].astype(np.float64)                   # UYVY: Y at odd bytes
    return None


def series_from_planes(planes, fps, stride=1):
    rows = []
    for i, p in planes:
        if i % stride == 0:
            rows.append(dict(frame=i, t=i / fps if fps else float(i), **nr.nr_frame(p, 8)))
    return rows


def per_second(rows):
    secs = {}
    for r in rows:
        secs.setdefault(int(r["t"]), []).append(r)
    out = []
    for s in sorted(secs):
        g = secs[s]
        d = dict(second=s, frames=len(g))
        for k in ("nr_band", "nr_block", "nr_ring", "nr_noise"):
            d[k] = float(np.median([x[k] for x in g]))
        out.append(d)
    return out


def poor_spans(sec_rows, thr, min_seconds=POOR_MIN_SECONDS):
    """Spans [start_s, end_s] of >= min_seconds consecutive seconds whose poor index is >= 1."""
    spans, cur = [], None
    for d in sec_rows:
        idx = nr.poor_index(d, thr)
        d["poor_index"] = idx
        if idx >= 1.0:
            if cur and d["second"] == cur[1] + 1:
                cur[1] = d["second"]
            else:
                if cur and cur[1] - cur[0] + 1 >= min_seconds:
                    spans.append(tuple(cur))
                cur = [d["second"], d["second"]]
        else:
            if cur and cur[1] - cur[0] + 1 >= min_seconds:
                spans.append(tuple(cur))
            cur = None
    if cur and cur[1] - cur[0] + 1 >= min_seconds:
        spans.append(tuple(cur))
    return spans


def scan_one(folder, thr, stride=4, max_frames=None):
    name = os.path.basename(folder.rstrip(os.sep))
    rows = read_frames_csv(os.path.join(folder, "frames.csv"))
    res = dict(recording=name, folder=folder, **timing_summary(rows))
    ev = parse_events(os.path.join(folder, "events.txt"))
    fps = res.get("picture_rate_hz") or res.get("nominal_hz") or 24.0
    series, tier = [], "timing-only"
    nrcsv = os.path.join(folder, "nr.csv")
    thumbs = os.path.join(folder, "thumbs.y8")
    raws = sorted(f for f in os.listdir(folder) if re.fullmatch(r"raw_\d+\.bin", f))
    if os.path.exists(nrcsv):
        tier = "full"
        with open(nrcsv, "r", encoding="utf8", newline="") as f:
            for r in csv.DictReader(f):
                series.append(dict(frame=int(r["frame"]), t=int(r["frame"]) / fps,
                                   **{k: float(r[k]) for k in ("nr_band", "nr_block", "nr_ring", "nr_noise")}))
    elif raws:
        planes = []
        by_frame = {e["frame"]: e for e in ev["raw"]}
        for fn in raws:
            idx = int(re.search(r"(\d+)", fn).group(1))
            e = by_frame.get(idx)
            if not e:
                continue
            p = raw_luma(os.path.join(folder, fn), e["w"], e["h"], e["stride"], e["fmt"])
            if p is not None:
                planes.append((idx, p))
        if planes:
            tier = "full"
            series = series_from_planes(planes, fps)
            res["note"] = "raw frames are only the first %d frames of the recording" % len(planes)
    elif os.path.exists(thumbs) and ev["thumb"]:
        tw, th = ev["thumb"]
        n = os.path.getsize(thumbs) // (tw * th)
        if max_frames:
            n = min(n, max_frames)
        mm = np.memmap(thumbs, dtype=np.uint8, mode="r", shape=(os.path.getsize(thumbs) // (tw * th), th, tw))
        planes = [(i, np.asarray(mm[i], dtype=np.float64)) for i in range(0, n, stride)]
        tier = "thumbnail"
        series = [dict(frame=i, t=i / fps, **nr.nr_frame(p, 8)) for i, p in planes]
        res["note"] = "thumbnail tier (%dx%d point-sampled luma): block/ring/band lose meaning; shown, never flagged poor" % (tw, th)
    res["tier"] = tier
    res["series_frames"] = len(series)
    if series:
        sec = per_second(series)
        res["metrics_median"] = {k: round(float(np.median([d[k] for d in sec])), 4)
                                 for k in ("nr_band", "nr_block", "nr_ring", "nr_noise")}
        if tier == "full" and thr:
            spans = poor_spans(sec, thr)
            res["poor_spans_s"] = spans
            res["poor_seconds"] = sum(b - a + 1 for a, b in spans)
            res["poor_index_max"] = round(max(d["poor_index"] for d in sec), 3)
        res["_seconds"] = sec
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", action="append", default=None, help="folder(s) to search (default: VP_TEST_STREAM_DIR, "
                    "D:/testclips/pulldown, VP_PULLDOWN_RECORDINGS)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stride", type=int, default=4)
    ap.add_argument("--max-frames", type=int, default=None)
    a = ap.parse_args(argv)
    roots = a.root or [p for p in (os.environ.get("VP_TEST_STREAM_DIR"), r"D:\testclips\pulldown",
                                   os.environ.get("VP_PULLDOWN_RECORDINGS")) if p and os.path.isdir(p)]
    out = os.path.abspath(a.out)
    recs = find_recordings(roots)
    for r in recs:
        if out.startswith(os.path.abspath(r) + os.sep) or out == os.path.abspath(r):
            raise SystemExit("--out must not be inside a recording folder: " + r)
    thr = nr.load_thresholds()
    os.makedirs(out, exist_ok=True)
    results = [scan_one(r, thr, a.stride, a.max_frames) for r in recs]
    lines = ["| recording | tier | frames | seconds | Hz | exact-repeat | gaps | NR median band/block/ring/noise | poor spans (s) |",
             "|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        mm = r.get("metrics_median")
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["recording"], r["tier"], r.get("frames"), r.get("seconds"), r.get("picture_rate_hz"),
            r.get("exact_repeat_share"), r.get("gaps"),
            "/".join("%.3f" % mm[k] for k in ("nr_band", "nr_block", "nr_ring", "nr_noise")) if mm else "no pixels",
            json.dumps(r.get("poor_spans_s")) if "poor_spans_s" in r else "n/a"))
    with open(os.path.join(out, "recordings_summary.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    for r in results:
        sec = r.pop("_seconds", None)
        if sec:
            with open(os.path.join(out, r["recording"] + "_nr_per_second.csv"), "w", encoding="utf8", newline="") as f:
                w = csv.DictWriter(f, fieldnames=sorted({k for d in sec for k in d}))
                w.writeheader()
                w.writerows(sec)
    with open(os.path.join(out, "recordings_summary.json"), "w", encoding="utf8", newline="\n") as f:
        json.dump(results, f, indent=1, sort_keys=True)
        f.write("\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
