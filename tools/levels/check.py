# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Check captured frames against the expected codes of a chain.

Inputs: a Y4M (C422p10) file, StreamRecorder raw_*.bin v210 files (with
--width/--height/--row-bytes, or sizes from the sibling events.txt), or a
StreamRecorder output directory (raw frames listed in events.txt).

Verdict per row: PASS / FAIL / NO DATA. Overall: FAIL if any FAIL, INCOMPLETE if
any NO DATA (and no FAIL), else PASS. Exit code 0 / 1 / 3; usage errors 2.
"""
import argparse
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deltae  # noqa: E402
import expected as ex  # noqa: E402
import patterns  # noqa: E402

PASS, FAIL, NODATA = "PASS", "FAIL", "NO DATA"

DEFAULT_THRESHOLDS = {
    "max_code_error": 1.0,      # codes, passthrough chains
    "max_de2000": 1.0,          # SDR processed chains
    "max_de_itp": 1.0,          # HDR processed chains
    "ramp_tolerance": 0.5,      # codes a ramp step may fall (sigma-delta noise)
    "btb_wtw_margin": 0.5,      # codes beyond nominal black / white that must be reached
}

EVENT_RE = re.compile(r"frame (\d+): raw frame saved \((\d+)x(\d+), (\d+) bytes per row, (\w+)\)")


class InputError(Exception):
    pass


# ---- readers ----------------------------------------------------------------

def read_y4m(path, max_frames=None):
    with open(path, "rb") as f:
        header = f.readline().decode("ascii", "replace").split()
        if not header or header[0] != "YUV4MPEG2":
            raise InputError("%s: not a Y4M file" % path)
        w = h = None
        cs = "420"
        for t in header[1:]:
            if t[0] == "W":
                w = int(t[1:])
            elif t[0] == "H":
                h = int(t[1:])
            elif t[0] == "C":
                cs = t[1:]
        if cs != "422p10" or not w or not h:
            raise InputError("%s: need C422p10 with W/H, got C%s" % (path, cs))
        ysz, csz = w * h * 2, (w // 2) * h * 2
        n = 0
        while max_frames is None or n < max_frames:
            line = f.readline()
            if not line:
                break
            if not line.startswith(b"FRAME"):
                raise InputError("%s: bad frame marker" % path)
            buf = f.read(ysz + 2 * csz)
            if len(buf) < ysz + 2 * csz:
                break  # truncated final frame: not used
            y = np.frombuffer(buf, "<u2", w * h).reshape(h, w)
            cb = np.frombuffer(buf, "<u2", (w // 2) * h, ysz).reshape(h, w // 2)
            cr = np.frombuffer(buf, "<u2", (w // 2) * h, ysz + csz).reshape(h, w // 2)
            n += 1
            yield y, cb, cr


def parse_events(path):
    out = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = EVENT_RE.search(line)
            if m:
                out[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5))
    return out


def _raw_list(inputs, a):
    """-> list of (path, w, h, row_bytes)."""
    items = []
    for p in inputs:
        if os.path.isdir(p):
            ev = os.path.join(p, "events.txt")
            if not os.path.isfile(ev):
                raise InputError("%s: no events.txt (give --width/--height on the raw files)" % p)
            info = parse_events(ev)
            for idx in sorted(info):
                w, h, rb, fmt = info[idx]
                if fmt != "10BitYUV":
                    raise InputError("frame %d is %s, only 10BitYUV (v210) is supported" % (idx, fmt))
                f = os.path.join(p, "raw_%05d.bin" % idx)
                if os.path.isfile(f):
                    items.append((f, w, h, rb))
        else:
            w, h, rb = a.width, a.height, a.row_bytes
            if not (w and h):
                ev = os.path.join(os.path.dirname(os.path.abspath(p)), "events.txt")
                m = re.search(r"raw_(\d+)\.bin$", os.path.basename(p))
                if m and os.path.isfile(ev):
                    info = parse_events(ev).get(int(m.group(1)))
                    if info:
                        w, h, rb = info[0], info[1], info[2]
                        if info[3] != "10BitYUV":
                            raise InputError("%s is %s, only 10BitYUV (v210) is supported" % (p, info[3]))
            if not (w and h):
                raise InputError("%s: give --width and --height (and --row-bytes)" % p)
            items.append((p, w, h, rb or patterns.v210_row_bytes(w)))
    return items


def iter_frames(inputs, a, max_frames=None):
    n = 0
    raws = []
    for p in inputs:
        if os.path.isfile(p) and p.lower().endswith(".y4m"):
            for fr in read_y4m(p, None if max_frames is None else max_frames - n):
                yield fr
                n += 1
                if max_frames is not None and n >= max_frames:
                    return
        else:
            raws.append(p)
    if raws:
        for path, w, h, rb in _raw_list(raws, a):
            if max_frames is not None and n >= max_frames:
                return
            with open(path, "rb") as f:
                data = f.read()
            if len(data) < rb * h:
                continue  # incomplete frame: no data from it
            yield patterns.v210_unpack(data, w, h, rb)
            n += 1


# ---- evaluation -------------------------------------------------------------

def inner_region(frac, w, h):
    x0, y0, x1, y1 = patterns.rect_px(frac, w, h)
    cx = ((x1 - x0) // 4) & ~1
    cy = (y1 - y0) // 4
    xa, xb, ya, yb = x0 + cx, x1 - cx, y0 + cy, y1 - cy
    return xa, xb, ya, yb


def sample_frame(layout, frame):
    """-> dict id -> np.array(3) mean codes, or None when the region is empty."""
    y, cb, cr = frame
    h, w = y.shape
    out = {}
    for p in layout["patches"]:
        xa, xb, ya, yb = inner_region(p["rect_frac"], w, h)
        if xb > w or yb > h or xb - xa < 2 or yb - ya < 1:
            out[p["id"]] = None
            continue
        out[p["id"]] = np.array([float(y[ya:yb, xa:xb].mean()),
                                 float(cb[ya:yb, xa // 2:xb // 2].mean()),
                                 float(cr[ya:yb, xa // 2:xb // 2].mean())])
    return out


def _de(chain, meas, exp):
    kw = dict(depth=chain.dst_depth, rng=chain.dst_range, matrix=chain.dst_matrix,
              transfer=chain.dst_transfer, lw=chain.dst_lw, lb=chain.dst_lb, hlg_lw=chain.hlg_lw)
    xm = ex.codes_to_xyz(meas, **kw)
    xe = ex.codes_to_xyz(exp, **kw)
    if chain.dst_transfer == "bt1886":
        return "de2000", deltae.delta_e_2000_xyz(xe, xm, chain.dst_lw)
    return "de_itp", deltae.delta_e_itp_xyz(xe, xm)


def evaluate(layout, chain, frames, thresholds=None, max_frames=None):
    th = dict(DEFAULT_THRESHOLDS)
    th.update(thresholds or {})
    if chain.dst_depth != 10:
        raise InputError("captured frames are 10-bit: the destination depth must be 10")
    sums, counts = {}, {}
    n = 0
    for fr in frames:
        if max_frames is not None and n >= max_frames:
            break
        n += 1
        for pid, v in sample_frame(layout, fr).items():
            if v is None:
                continue
            sums[pid] = sums.get(pid, 0.0) + v
            counts[pid] = counts.get(pid, 0) + 1
    rows = []
    exp_rows = {r["id"]: r for r in ex.table_rows(layout["patches"], chain)}
    meas = {}
    passthrough = chain.is_passthrough()
    for p in layout["patches"]:
        pid = p["id"]
        e = exp_rows[pid]
        exp = np.array([e["exp_y"], e["exp_cb"], e["exp_cr"]], dtype=np.float64)
        row = {"id": pid, "kind": "patch", "expected": [int(v) for v in exp],
               "btb_wtw_expected": e["btb_wtw_expected"], "frames": counts.get(pid, 0)}
        if not counts.get(pid):
            row.update(status=NODATA, reason="no frames" if n == 0 else "patch outside frame")
            rows.append(row)
            continue
        m = sums[pid] / counts[pid]
        meas[pid] = m
        err = float(np.max(np.abs(m - exp)))
        dname, dval = _de(chain, m, exp)
        row.update(measured=[round(float(v), 3) for v in m], max_code_error=round(err, 3),
                   **{dname: round(float(dval), 4)})
        if passthrough:
            row["status"] = PASS if err <= th["max_code_error"] else FAIL
            row["criterion"] = "max code error <= %g" % th["max_code_error"]
        else:
            lim = th["max_de2000"] if dname == "de2000" else th["max_de_itp"]
            row["status"] = PASS if dval <= lim else FAIL
            row["criterion"] = "%s <= %g" % (dname, lim)
        rows.append(row)

    ramp = sorted((p for p in layout["patches"] if p.get("group") == "ramp"), key=lambda p: p["index"])
    notes = []
    if ramp:
        row = {"id": "ramp_monotonic", "kind": "ramp"}
        if any(p["id"] not in meas for p in ramp):
            row.update(status=NODATA, reason="ramp segments without data")
        else:
            ys = [float(meas[p["id"]][0]) for p in ramp]
            steps = np.diff(ys)
            worst = float(steps.min())
            ok = worst >= -th["ramp_tolerance"] and ys[-1] > ys[0]
            row.update(status=PASS if ok else FAIL, min_step=round(worst, 3),
                       first=round(ys[0], 3), last=round(ys[-1], 3),
                       criterion="no step below -%g and last > first" % th["ramp_tolerance"])
        rows.append(row)

    if chain.keeps_btb_wtw:
        s = chain.dst_depth - 8
        for cls, label in (("BTB", "btb_reached"), ("WTW", "wtw_reached")):
            sel = [p for p in layout["patches"] if exp_rows[p["id"]]["btb_wtw_expected"] == cls]
            if not sel:
                notes.append("%s: this set has no patches expected %s; row omitted (not a pass)" %
                             (label, "below nominal black" if cls == "BTB" else "above nominal white"))
                continue
            row = {"id": label, "kind": "btb_wtw"}
            if any(p["id"] not in meas for p in sel):
                row.update(status=NODATA, reason="patches without data")
            else:
                bad = []
                for p in sel:
                    y = float(meas[p["id"]][0])
                    if cls == "BTB":
                        reached = y < (16 << s) - th["btb_wtw_margin"]
                    else:
                        reached = y > (235 << s) + th["btb_wtw_margin"]
                    if not reached:
                        bad.append(p["id"])
                row.update(status=FAIL if bad else PASS, failed_patches=bad, checked=len(sel),
                           criterion="measured Y beyond %s (%d)" %
                           ("nominal black" if cls == "BTB" else "nominal white",
                            (16 << s) if cls == "BTB" else (235 << s)))
            rows.append(row)

    statuses = [r["status"] for r in rows]
    if FAIL in statuses:
        verdict = "FAIL"
    elif NODATA in statuses or not rows:
        verdict = "INCOMPLETE"
    else:
        verdict = "PASS"
    return {"verdict": verdict, "set": layout.get("set"), "frames_read": n,
            "frame_size": None, "chain": chain.describe(), "passthrough": passthrough,
            "thresholds": th, "rows": rows, "notes": notes}


def render_md(rep):
    L = ["# Level check: %s" % rep["verdict"], "",
         "Set: %s, frames read: %d, passthrough: %s" % (rep["set"], rep["frames_read"], rep["passthrough"]),
         "Chain: `%s`" % json.dumps(rep["chain"]), "",
         "| row | status | expected (Y,Cb,Cr) | measured | max code err | dE | note |",
         "|---|---|---|---|---|---|---|"]
    for r in rep["rows"]:
        de = r.get("de2000", r.get("de_itp", ""))
        if r["kind"] == "patch":
            L.append("| %s | %s | %s | %s | %s | %s | %s |" % (
                r["id"], r["status"], ",".join(map(str, r["expected"])),
                ",".join(map(str, r.get("measured", []))), r.get("max_code_error", ""), de,
                r.get("reason", r.get("btb_wtw_expected", ""))))
        else:
            extra = r.get("reason") or r.get("criterion", "")
            if r.get("failed_patches"):
                extra += "; failed: " + ",".join(r["failed_patches"])
            L.append("| %s | %s | | | | | %s |" % (r["id"], r["status"], extra))
    if rep["notes"]:
        L += ["", "Notes:"] + ["- " + n for n in rep["notes"]]
    return "\n".join(L) + "\n"


def write_report(rep, out):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=1)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write(render_md(rep))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+", help="Y4M file, raw_*.bin files, or a StreamRecorder directory")
    ap.add_argument("--layout", required=True, help="layout JSON written by patterns.py")
    ap.add_argument("--frames", type=int, default=None, help="max frames to average (default: all)")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--row-bytes", type=int)
    ap.add_argument("--thresholds", help="JSON file overriding thresholds")
    ap.add_argument("--out", default="levels-report")
    ex.add_chain_args(ap)
    a = ap.parse_args(argv)
    try:
        with open(a.layout, "r", encoding="utf-8") as f:
            layout = json.load(f)
        th = {}
        if a.thresholds:
            with open(a.thresholds, "r", encoding="utf-8") as f:
                th = json.load(f)
            bad = set(th) - set(DEFAULT_THRESHOLDS)
            if bad:
                raise InputError("unknown thresholds: %s" % ", ".join(sorted(bad)))
        chain = ex.chain_from_args(a)
        frames = iter_frames(a.inputs, a, a.frames)
        rep = evaluate(layout, chain, frames, th, a.frames)
    except (InputError, OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    write_report(rep, a.out)
    print("%s (%d frames) -> %s" % (rep["verdict"], rep["frames_read"], os.path.join(a.out, "report.md")))
    return {"PASS": 0, "FAIL": 1}.get(rep["verdict"], 3)


if __name__ == "__main__":
    sys.exit(main())
