# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Scored table: candidate vs default.

A result row is a dict(content_type, clip, case, metric, candidate, default, note).  `candidate` /
`default` are numbers, bools, 'inf' strings or None (= NA).  judge() adds delta, trend and flag.

flag values: PASS, FLAG (worse than the default beyond tolerance / outside a soft rule), FAIL (a hard
rule of the metric is broken), NA (cannot be judged: missing data), INFO (reported, not gated).
"""
import csv
import io
import math

COLUMNS = ["content_type", "clip", "case", "metric", "candidate", "default", "delta", "trend", "flag", "note"]


def _num(v):
    if v is None:
        return None
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, str):
        return math.inf if v == "inf" else None
    return float(v)


def fmt(v):
    if v is None:
        return "NA"
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, str):
        return v
    if math.isinf(v):
        return "inf"
    return "%.6g" % v


# rule: (kind, tolerance).  kinds:
#   higher / lower   : candidate must not be worse than the default by more than tol
#   band             : candidate itself must be within +-tol of 1 (hard FAIL)
#   max_abs          : candidate itself must be <= tol (hard FAIL)
#   near1            : FLAG if |cand-1| > |default-1| + tol
#   kept             : candidate itself must be >= tol (hard FAIL)  (BTB/WTW preserved)
#   zero             : candidate must be 0 (hard FAIL)
#   no_increase_over_default : FLAG if cand > default + tol
#   info / info_lower : reported, not gated (info_lower: trend uses lower-is-better)
RULES = [
    ("psnr_", ("higher", 0.05)),
    ("ssim_", ("higher", 0.0005)),
    ("lpips", ("lower", 0.002)),
    ("grain_rms_ratio_", ("band", 0.05)),
    ("grain_hf_ratio_", ("near1", 0.10)),
    ("grain_lag1_delta_", ("max_abs", 0.05)),
    ("flicker_excess", ("lower", 0.05)),
    ("gop_", ("near1", 0.10)),
    ("ghost_coeff", ("lower", 0.01)),
    ("smear", ("near1", 0.03)),
    ("duplicate_rate", ("lower", 0.0)),
    ("halo_overshoot", ("lower", 0.002)),
    ("lc_boost_fraction", ("lower", 0.05)),
    ("lc_ratio_", ("max_abs", 1.05)),
    ("btb_kept", ("kept", 0.99)),
    ("wtw_kept", ("kept", 0.99)),
    ("clip_detect", ("zero", 0.0)),
    ("cost_", ("info_lower", 0.0)),
]


def rule_for(metric):
    for prefix, r in RULES:
        if metric.startswith(prefix):
            return r
    return ("info", 0.0)


def judge(metric, cand, base):
    """-> (delta, trend, flag)"""
    c, b = _num(cand), _num(base)
    kind, tol = rule_for(metric)
    delta = None
    if c is not None and b is not None and not (math.isinf(c) and math.isinf(b)):
        delta = c - b
    elif c is not None and b is not None:
        delta = 0.0
    if c is None:
        return None, "NA", "NA"
    if kind == "info":
        return delta, "NA", "INFO"
    if kind == "info_lower":
        return delta, "NA" if delta is None else _trend(delta, 0.0, False), "INFO"
    if kind == "band":
        return delta, _trend_to1(c, b), ("PASS" if abs(c - 1.0) <= tol else "FAIL")
    if kind == "max_abs":
        return delta, _trend(delta, 0.0, False), ("PASS" if c <= tol else "FAIL")
    if kind == "kept":
        return delta, _trend(delta, 0.0, True), ("PASS" if c >= tol else "FAIL")
    if kind == "zero":
        return delta, _trend(delta, 0.0, False), ("PASS" if c == 0.0 else "FAIL")
    if b is None:
        return None, "NA", "NA"
    if kind == "higher":
        if math.isinf(c) and math.isinf(b):
            return 0.0, "same", "PASS"
        return delta, _trend(delta, tol, True), ("PASS" if delta >= -tol else "FLAG")
    if kind == "lower":
        return delta, _trend(delta, tol, False), ("PASS" if delta <= tol else "FLAG")
    if kind == "near1":
        worse = abs(c - 1.0) > abs(b - 1.0) + tol
        return delta, _trend_to1(c, b), ("FLAG" if worse else "PASS")
    return delta, "NA", "INFO"


def _trend(delta, tol, higher_better):
    if delta is None:
        return "NA"
    if abs(delta) <= tol:
        return "same"
    return "better" if (delta > 0) == higher_better else "worse"


def _trend_to1(c, b):
    if b is None:
        return "NA"
    dc, db = abs(c - 1.0), abs(b - 1.0)
    if abs(dc - db) <= 1e-9:
        return "same"
    return "better" if dc < db else "worse"


def score_rows(rows):
    out = []
    for r in rows:
        d, t, f = judge(r["metric"], r["candidate"], r["default"])
        o = dict(r)
        o.update(delta=d, trend=t, flag=f)
        o.setdefault("note", "")
        if f == "FAIL" and rule_for(r["metric"])[0] in ("band", "max_abs", "kept", "zero")                 and judge(r["metric"], r["default"], r["default"])[2] == "FAIL":
            o["note"] = (o["note"] + "; " if o["note"] else "") + "default also fails (input/codec-limited)"
        out.append(o)
    return out


def to_csv(scored):
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(COLUMNS)
    for r in scored:
        w.writerow([r["content_type"], r["clip"], r["case"], r["metric"], fmt(r["candidate"]), fmt(r["default"]),
                    fmt(r["delta"]), r["trend"], r["flag"], r.get("note", "")])
    return buf.getvalue()


def summarise(scored):
    """per content type: flag counts and per-metric means over the cases that have numbers."""
    by_ct = {}
    for r in scored:
        ct = by_ct.setdefault(r["content_type"], dict(counts={}, metrics={}))
        ct["counts"][r["flag"]] = ct["counts"].get(r["flag"], 0) + 1
        m = ct["metrics"].setdefault(r["metric"], dict(c=[], b=[], n=0, na=0, flags={}))
        m["n"] += 1
        c, b = _num(r["candidate"]), _num(r["default"])
        if c is None:
            m["na"] += 1
        elif not math.isinf(c):
            m["c"].append(c)
        if b is not None and not math.isinf(b):
            m["b"].append(b)
        m["flags"][r["flag"]] = m["flags"].get(r["flag"], 0) + 1
    return by_ct


def to_markdown(scored, candidate, baseline, title="VP-V evaluation harness", extra_lines=()):
    lines = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
             "# %s: %s vs %s (default)" % (title, candidate, baseline), ""]
    lines += list(extra_lines)
    if extra_lines:
        lines.append("")
    lines.append("## Per-content-type summary")
    lines.append("")
    summ = summarise(scored)
    for ct in sorted(summ):
        s = summ[ct]
        cnt = ", ".join("%s %d" % (k, s["counts"][k]) for k in sorted(s["counts"]))
        lines.append("### %s  (%s)" % (ct, cnt))
        lines.append("")
        lines.append("| metric | candidate (mean) | default (mean) | delta | n | NA | flags |")
        lines.append("|---|---|---|---|---|---|---|")
        for k in sorted(s["metrics"]):
            m = s["metrics"][k]
            cm = sum(m["c"]) / len(m["c"]) if m["c"] else None
            bm = sum(m["b"]) / len(m["b"]) if m["b"] else None
            dm = (cm - bm) if (cm is not None and bm is not None) else None
            fl = " ".join("%s:%d" % (a, m["flags"][a]) for a in sorted(m["flags"]))
            lines.append("| %s | %s | %s | %s | %d | %d | %s |" % (k, fmt(cm), fmt(bm), fmt(dm), m["n"], m["na"], fl))
        lines.append("")
    lines.append("## All rows")
    lines.append("")
    lines.append("| " + " | ".join(COLUMNS) + " |")
    lines.append("|" + "---|" * len(COLUMNS))
    for r in scored:
        lines.append("| " + " | ".join([r["content_type"], r["clip"], r["case"], r["metric"], fmt(r["candidate"]),
                                          fmt(r["default"]), fmt(r["delta"]), r["trend"], r["flag"],
                                          str(r.get("note", "")).replace("|", "/")]) + " |")
    lines.append("")
    return "\n".join(lines)
