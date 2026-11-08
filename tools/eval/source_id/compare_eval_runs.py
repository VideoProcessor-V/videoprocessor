# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Compares two source-identity evaluation runs of the SAME reels (SID-OPT: did a cost change alter any result, and what did it do to the time?).

  python -I tools/eval/source_id/compare_eval_runs.py <before-eval-dir> <after-eval-dir> [--reels a,b] [--out report.txt] [--expect identical|any]
  python -I tools/eval/source_id/compare_eval_runs.py --selftest

An eval dir is a run_identity_eval.ps1 output folder: reels/<reel>/evaluations.csv (harness rows, lane "chain" is read) and, when the run was made
with VP_SOURCE_ID_CAL_DUMP=<eval dir>/caldump, caldump/<reel>.csv (one row per evaluation x axis x tested candidate).

Per reel it reports
  chain rows        evaluations.csv rows of lane "chain" matched by out_frame. A row is IDENTICAL when the CORE fields (n_x n_y measured_n_x
                    measured_n_y llr_x llr_y kernel state state_x state_y) are equal as text (the harness prints them with %f / %d, so equal text =
                    equal at the printed precision), and FULLY IDENTICAL when every column except the wall-clock column `ms` is equal.
  caldump rows      matched by (eval_seq, axis, candidate index within the evaluation and axis); IDENTICAL when all 27 columns are equal as text
                    (n, class, T, stage, best_kernel, margins, lines, floor, T_ev, informative ... : T is printed with 7 significant digits).
  time              ms median / p95 of the distinct evaluations (consecutive rows repeating the same ms are one evaluation), before vs after, and
                    the ratio. ms is the wall time of the evaluation that ran in the frame (steady clock, the governor's own timer for the
                    verifier), so it only compares runs made on a comparable load (same affinity, nothing else heavy running).
A reel missing from either side is listed as UNCOMPARED (it is not a pass).

Exit code: 0 = every compared row identical (and nothing uncompared unless --expect any); 1 = a difference with --expect identical (the default);
3 = nothing differed but some reel could not be compared; 2 = bad arguments / unreadable input. With --expect any the exit code is 0 unless an input is
unreadable (use it for a result-changing step: the report still lists every difference).
"""
import argparse
import csv
import io
import os
import statistics
import sys
import tempfile

CORE_FIELDS = ["n_x", "n_y", "measured_n_x", "measured_n_y", "llr_x", "llr_y", "kernel", "state", "state_x", "state_y"]
IGNORED_FULL = {"ms", "lane", "reel"}


def percentile(values, q):
    """Linear-interpolated percentile (q in 0..1) of a non-empty list."""
    v = sorted(values)
    if len(v) == 1:
        return v[0]
    pos = q * (len(v) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


def read_csv(path):
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def chain_rows(path):
    rows = [r for r in read_csv(path) if r.get("lane") == "chain"]
    out = {}
    order = []
    for r in rows:
        key = r.get("out_frame")
        if key not in out:
            order.append(key)
        out[key] = r
    return order, out


def distinct_ms(order, rows):
    vals = []
    prev = None
    for k in order:
        txt = rows[k].get("ms", "")
        try:
            v = float(txt)
        except ValueError:
            continue
        if v < 0:
            continue
        if txt != prev:
            vals.append(v)
        prev = txt
    return vals


def caldump_keyed(path):
    rows = read_csv(path)
    seen = {}
    keyed = {}
    order = []
    for r in rows:
        base = (r.get("eval_seq"), r.get("axis"))
        idx = seen.get(base, 0)
        seen[base] = idx + 1
        key = base + (idx,)
        keyed[key] = r
        order.append(key)
    return order, keyed


class Counter(object):
    def __init__(self):
        self.compared = 0
        self.core_same = 0
        self.full_same = 0
        self.by_field = {}
        self.examples = []

    def note(self, key, field, a, b):
        self.by_field[field] = self.by_field.get(field, 0) + 1
        if len(self.examples) < 6:
            self.examples.append("%s %s: before=%s after=%s" % (key, field, a, b))


def compare_chain(before_path, after_path):
    bo, br = chain_rows(before_path)
    ao, ar = chain_rows(after_path)
    c = Counter()
    only_before = [k for k in bo if k not in ar]
    only_after = [k for k in ao if k not in br]
    for k in ao:
        if k not in br:
            continue
        b, a = br[k], ar[k]
        c.compared += 1
        core_ok = True
        for f in CORE_FIELDS:
            if b.get(f) != a.get(f):
                core_ok = False
                c.note("out_frame %s" % k, f, b.get(f), a.get(f))
        full_ok = core_ok
        for f in b:
            if f in IGNORED_FULL or f in CORE_FIELDS:
                continue
            if b.get(f) != a.get(f):
                full_ok = False
                c.note("out_frame %s" % k, f, b.get(f), a.get(f))
        c.core_same += 1 if core_ok else 0
        c.full_same += 1 if full_ok else 0
    return c, only_before, only_after, distinct_ms(bo, br), distinct_ms(ao, ar)


def compare_caldump(before_path, after_path):
    bo, bk = caldump_keyed(before_path)
    ao, ak = caldump_keyed(after_path)
    c = Counter()
    only_before = [k for k in bo if k not in ak]
    only_after = [k for k in ao if k not in bk]
    for k in ao:
        if k not in bk:
            continue
        b, a = bk[k], ak[k]
        c.compared += 1
        same = True
        for f in b:
            if b.get(f) != a.get(f):
                same = False
                c.note("eval_seq %s axis %s #%s" % k, f, b.get(f), a.get(f))
        c.core_same += 1 if same else 0
        c.full_same += 1 if same else 0
    return c, only_before, only_after


def fmt_stats(vals):
    if not vals:
        return "n/a"
    return "n=%d median %.1f ms p95 %.1f ms" % (len(vals), statistics.median(vals), percentile(vals, 0.95))


def run(before, after, only, expect):
    lines = []
    diffs = 0
    uncompared = 0
    broken = 0
    breels = os.path.join(before, "reels")
    areels = os.path.join(after, "reels")
    if not os.path.isdir(areels):
        sys.stderr.write("no reels folder in the AFTER dir: %s\n" % areels)
        return 2, ["no reels folder in the AFTER dir: %s" % areels]
    names = sorted(d for d in os.listdir(areels) if os.path.isdir(os.path.join(areels, d)) and not d.endswith(".tmp"))
    if only:
        names = [n for n in names if any(o in n for o in only)]
    lines.append("compare_eval_runs: BEFORE %s  AFTER %s" % (before, after))
    all_b, all_a = [], []
    for name in names:
        bp = os.path.join(breels, name, "evaluations.csv")
        ap = os.path.join(areels, name, "evaluations.csv")
        lines.append("")
        lines.append("== %s" % name)
        if not os.path.isfile(ap):
            lines.append("  UNCOMPARED: no AFTER evaluations.csv (the reel did not finish)")
            uncompared += 1
            continue
        if not os.path.isfile(bp):
            lines.append("  UNCOMPARED: no BEFORE evaluations.csv at %s" % bp)
            uncompared += 1
            continue
        try:
            c, ob, oa, msb, msa = compare_chain(bp, ap)
        except Exception as e:  # unreadable input
            lines.append("  ERROR reading evaluations.csv: %s" % e)
            broken += 1
            continue
        all_b.extend(msb)
        all_a.extend(msa)
        bad = c.compared - c.full_same
        lines.append("  chain rows compared %d: core fields (n llr kernel state) identical %d, all columns identical %d%s"
                     % (c.compared, c.core_same, c.full_same, "" if bad == 0 else "   <-- %d rows differ" % bad))
        if ob or oa:
            lines.append("  rows only in BEFORE %d, only in AFTER %d (different frame set)" % (len(ob), len(oa)))
            diffs += 1
        if c.by_field:
            lines.append("  differing columns: " + ", ".join("%s x%d" % (f, n) for f, n in sorted(c.by_field.items())))
            for ex in c.examples:
                lines.append("    " + ex)
        diffs += bad
        lines.append("  time BEFORE: %s" % fmt_stats(msb))
        lines.append("  time AFTER : %s" % fmt_stats(msa))
        if msb and msa:
            mb, ma = statistics.median(msb), statistics.median(msa)
            pb, pa = percentile(msb, 0.95), percentile(msa, 0.95)
            lines.append("  ratio after/before: median %.2f (x%.1f faster) p95 %.2f (x%.1f faster)"
                         % (ma / mb if mb else 0.0, mb / ma if ma else 0.0, pa / pb if pb else 0.0, pb / pa if pa else 0.0))
        cb = os.path.join(before, "caldump", name + ".csv")
        ca = os.path.join(after, "caldump", name + ".csv")
        if os.path.isfile(cb) and os.path.isfile(ca):
            try:
                cc, cob, coa = compare_caldump(cb, ca)
                badc = cc.compared - cc.full_same
                lines.append("  caldump rows compared %d: identical %d%s" % (cc.compared, cc.full_same, "" if badc == 0 else "   <-- %d rows differ" % badc))
                if cob or coa:
                    lines.append("  caldump rows only in BEFORE %d, only in AFTER %d" % (len(cob), len(coa)))
                    diffs += 1
                if cc.by_field:
                    lines.append("  differing caldump columns: " + ", ".join("%s x%d" % (f, n) for f, n in sorted(cc.by_field.items())))
                    for ex in cc.examples:
                        lines.append("    " + ex)
                diffs += badc
            except Exception as e:
                lines.append("  ERROR reading caldump: %s" % e)
                broken += 1
        else:
            lines.append("  caldump: not compared (%s)" % ("missing BEFORE %s" % cb if not os.path.isfile(cb) else "missing AFTER %s" % ca))
            uncompared += 1
    lines.append("")
    lines.append("== all compared reels together")
    lines.append("  time BEFORE: %s" % fmt_stats(all_b))
    lines.append("  time AFTER : %s" % fmt_stats(all_a))
    if all_b and all_a:
        lines.append("  median x%.2f, p95 x%.2f (before / after)" % (statistics.median(all_b) / statistics.median(all_a),
                                                                    percentile(all_b, 0.95) / percentile(all_a, 0.95)))
    verdict = "IDENTICAL" if diffs == 0 and broken == 0 else "DIFFERENCES"
    lines.append("verdict: %s (differing rows/blocks %d, uncompared items %d, unreadable %d)" % (verdict, diffs, uncompared, broken))
    if broken:
        return 2, lines
    if expect == "any":
        return 0, lines
    if diffs:
        return 1, lines
    if uncompared:
        return 3, lines
    return 0, lines


def _write(path, text):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def selftest():
    hdr = "reel,lane,out_frame,t,n_x,n_y,state,ms,kernel,measured_n_x,measured_n_y,state_x,state_y,llr_x,llr_y,gen\n"
    cal = "reel_id,eval_seq,axis,n,T,stage\n"
    with tempfile.TemporaryDirectory() as root:
        for side, ms_scale, bump in (("before", 1.0, False), ("after", 0.4, False), ("after2", 0.4, True)):
            for reel in ("R.a", "R.b"):
                rows = hdr
                for i in range(20):
                    ms = (600 + 10 * i) * ms_scale
                    n = 1000 + (5 if (bump and reel == "R.a" and i == 7) else 0)
                    rows += "%s,chain,%d,%d,%d,1080,CONFIDENT,%.4f,bicubic,%d,1080,CONFIDENT,CONFIDENT,1.5,2.5,0\n" % (reel, i * 12, i * 0.5, n, ms, n)
                    rows += "%s,on,%d,%d,0,0,UNKNOWN,-1,,,,,,,,0\n" % (reel, i * 12, i * 0.5)
                _write(os.path.join(root, side, "reels", reel, "evaluations.csv"), rows)
                c = cal
                for i in range(10):
                    c += "%s,%d,x,%d,-0.1%d,exact\n" % (reel, i, 1000, i)
                _write(os.path.join(root, side, "caldump", reel + ".csv"), c)
        code, lines = run(os.path.join(root, "before"), os.path.join(root, "after"), [], "identical")
        assert code == 0, "\n".join(lines)
        assert any("x2.5" in l or "x2.4" in l or "x2.6" in l for l in lines), "\n".join(lines)
        code2, lines2 = run(os.path.join(root, "before"), os.path.join(root, "after2"), [], "identical")
        assert code2 == 1, "\n".join(lines2)
        assert any("n_x" in l for l in lines2)
        code3, _ = run(os.path.join(root, "before"), os.path.join(root, "after2"), [], "any")
        assert code3 == 0
        os.remove(os.path.join(root, "before", "reels", "R.b", "evaluations.csv"))
        code4, lines4 = run(os.path.join(root, "before"), os.path.join(root, "after"), [], "identical")
        assert code4 == 3, "\n".join(lines4)
    assert abs(percentile([1, 2, 3, 4, 5], 0.5) - 3) < 1e-9 and abs(percentile([1.0, 2.0], 0.95) - 1.95) < 1e-9
    print("selftest ok")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("before", nargs="?")
    ap.add_argument("after", nargs="?")
    ap.add_argument("--reels", default="", help="comma list of reel-id substrings (default: every reel folder of the AFTER dir)")
    ap.add_argument("--out", default="", help="also write the report to this file")
    ap.add_argument("--expect", choices=("identical", "any"), default="identical")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.before or not a.after:
        ap.print_usage(sys.stderr)
        return 2
    only = [x for x in a.reels.split(",") if x]
    code, lines = run(a.before, a.after, only, a.expect)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if a.out:
        _write(a.out, text)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
