# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""SID-CAL: calibrate the chain estimator's null / alt tables (doc 78 D3, D9, D10, sections 3, 4, 9).

Reads the calibration dumps written by the harness (VP_SOURCE_ID_CAL_DUMP: one CSV per reel, one row per
evaluation x axis x tested candidate) and writes
  NullTables.generated.h   per size class (K8, K16, Rational, Generic): GaussDist null + alt, exactly the
                           struct of identity/chain/ChainNullTables.h (C++14);
  calibration-report.md    leakage-controlled validation (2-fold CV BY TITLE), <= 60 lines.

Run:  python -I tools/eval/source_id/calibrate_nulls.py --dump <dir> --out <dir>
Test: python -I tools/eval/source_id/calibrate_nulls.py --selftest

Conservative by construction (a table can only make the estimator abstain MORE than the placeholder where
data is thin); see tools/eval/source_id/README.md, section "calibrate_nulls.py".
"""
import argparse
import csv
import datetime
import hashlib
import math
import os
import re
import sys

# ----------------------------------------------------------------------------------------------------------
# Constants mirrored from the C++ sources (identity/chain). Change them only together with the headers.
# ----------------------------------------------------------------------------------------------------------
CLASSES = ("K8", "K16", "Rational", "Generic")
CLASS_CPP = ("kClassK8", "kClassK16", "kClassRational", "kClassGeneric")
CLASS_NAMES = ("k/8", "k/16", "rational<=6", "generic")
PLACEHOLDER = {   # NullTables::Placeholder()
    "null": [(-0.12, 0.10), (-0.10, 0.08), (-0.04, 0.05), (-0.05, 0.06)],
    "alt": [(-0.30, 0.20)] * 4,
}
INC_MIN, INC_MAX = -3.0, 5.0            # EvidenceSettings::incrementMin / incrementMax
LINE_GATE_FRACTION, LINE_GATE_FLOOR = 0.25, 3   # minGateFraction, minGateLinesFloor
TARGET_RATE_PER_HOUR, THRESHOLD_INTERVAL_S = 0.01, 10.0
CANDIDATE_COUNT, KERNEL_COUNT = 8, 10   # multiplicity of c1
SHOT_CAP_FRACTION = 0.5
MAX_HYPOTHESES = 16
MIN_EVALS_CANDIDATE = 4                 # README: CANDIDATE needs >= 4 informative evaluations
REQUIRED = ["reel_id", "title", "eval_seq", "time_s", "axis", "N", "n", "class", "T", "stage", "best_kernel",
            "kernel_margin", "lines_passed", "lines", "power_seen", "floor_codes", "truth_n", "bit_depth",
            "truth_kernel", "truth_sharpen"]

# Fit policy (documented in the README; every number is a parameter of Policy).
class Policy(object):
    def __init__(self):
        self.min_null_hard = 40      # fewer null samples than this: the class keeps the placeholder
        self.min_alt_hard = 30
        self.min_titles = 3          # fewer titles than this on either side: placeholder
        self.full_null = 300         # at or above both full counts: robust fit used as is (plus widening)
        self.full_alt = 150          # below: shrinkage toward the null and a dominance check vs the placeholder
        self.null_widen = 1.25       # the null sd is widened by this factor ...
        self.se_terms = 2.0          # ... and by (1 + se_terms / sqrt(n)) for the estimate's own uncertainty
        self.sd_floor = 0.005
        self.min_separation = 0.25   # alt mean must sit this many null-sds below the null mean, else placeholder
        self.grid = [(-2.0 + 0.01 * i) for i in range(206)]   # T grid of the dominance check (-2.0 .. 0.05)


# ----------------------------------------------------------------------------------------------------------
# Ports of ChainNullTables.h
# ----------------------------------------------------------------------------------------------------------
def gcd_int(a, b):
    a, b = abs(a), abs(b)
    while b:
        a, b = b, a % b
    return a


def classify_latent(n, N, rational_q_max=6, tol=2.0):
    """Port of ClassifyLatent: K8, K16, Rational, Generic (index into CLASSES), first fit wins."""
    for k in range(1, 8):
        if abs(n - N * k / 8.0) <= tol:
            return 0
    for k in range(1, 16, 2):
        if abs(n - N * k / 16.0) <= tol:
            return 1
    for b in range(2, rational_q_max + 1):
        for a in range(1, b):
            if gcd_int(a, b) == 1 and abs(n - N * a / float(b)) <= tol:
                return 2
    return 3


def llr(params_null, params_alt, stat):
    """NullTables::LogLikelihoodRatio for one class."""
    m0, s0 = params_null
    m1, s1 = params_alt
    z0 = (stat - m0) / s0
    z1 = (stat - m1) / s1
    return 0.5 * z0 * z0 - 0.5 * z1 * z1 + math.log(s0 / s1)


def clampd(x, lo, hi):
    return lo if x < lo else (hi if x > hi else x)


def increment(tables, c, stat):
    return clampd(llr(tables["null"][c], tables["alt"][c], stat), INC_MIN, INC_MAX)


def thresholds():
    alpha = TARGET_RATE_PER_HOUR * THRESHOLD_INTERVAL_S / 3600.0
    return -math.log(alpha) + math.log(CANDIDATE_COUNT * KERNEL_COUNT)


# ----------------------------------------------------------------------------------------------------------
# Reading dumps
# ----------------------------------------------------------------------------------------------------------
class Row(object):
    __slots__ = ("reel", "title", "seq", "time", "axis", "N", "n", "cls_dump", "T", "stage", "lines_passed",
                 "lines", "power", "floor", "truth_n", "bits", "shot")


def _num(s, default=None):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def _class_index(s):
    t = (s or "").strip().lower().replace("/", "").replace("<=6", "").replace("_", "")
    for i, key in enumerate(("k8", "k16", "rational", "generic")):
        if t == key or t == str(i):
            return i
    return -1


def load_dumps(path):
    """Returns (rows, info). info: files, bad rows, dump hash. Fails loudly on missing columns."""
    files = sorted(f for f in os.listdir(path) if f.lower().endswith(".csv"))
    if not files:
        raise SystemExit("no *.csv in " + path)
    h = hashlib.sha256()
    rows, bad = [], 0
    for fn in files:
        fp = os.path.join(path, fn)
        with open(fp, "rb") as fh:
            h.update(fn.encode("utf-8") + b"\0" + hashlib.sha256(fh.read()).digest())
        with open(fp, "r", newline="", encoding="utf-8-sig") as fh:
            rd = csv.DictReader(fh)
            missing = [c for c in REQUIRED if c not in (rd.fieldnames or [])]
            if missing:
                raise SystemExit("%s: missing columns %s" % (fn, ",".join(missing)))
            has_shot = "shot_id" in (rd.fieldnames or [])
            for rec in rd:
                r = Row()
                r.reel, r.title = rec["reel_id"], rec["title"]
                seq, tm, N, n = _num(rec["eval_seq"]), _num(rec["time_s"]), _num(rec["N"]), _num(rec["n"])
                T, lp, ln = _num(rec["T"]), _num(rec["lines_passed"]), _num(rec["lines"])
                tn, bits = _num(rec["truth_n"]), _num(rec["bit_depth"])
                if None in (seq, tm, N, n, T, lp, ln, tn, bits) or not math.isfinite(T) or N <= 0 or ln <= 0:
                    bad += 1
                    continue
                r.seq, r.time, r.axis, r.N, r.n, r.T = int(seq), tm, rec["axis"].strip(), int(N), int(n), T
                r.cls_dump = _class_index(rec["class"])
                r.stage = rec["stage"].strip().lower()
                r.lines_passed, r.lines = int(lp), int(ln)
                r.power = rec["power_seen"].strip().lower() in ("1", "true", "yes")
                r.floor = _num(rec["floor_codes"], 0.0)
                r.truth_n, r.bits = int(tn), int(bits)
                r.shot = int(_num(rec["shot_id"], -1)) if has_shot else -1
                rows.append(r)
    return rows, {"files": len(files), "bad": bad, "hash": h.hexdigest()}


# ----------------------------------------------------------------------------------------------------------
# Samples: gate, per-evaluation clustering exactly as the estimator, NULL / ALT labelling
# ----------------------------------------------------------------------------------------------------------
class Item(object):
    """One informative cluster (what the estimator turns into one hypothesis increment)."""
    __slots__ = ("reel", "title", "seq", "time", "axis", "N", "n", "T", "cls", "truth_n", "bits", "label", "shot")


def gate_ok(r):
    need = max(LINE_GATE_FLOOR, int(math.ceil(LINE_GATE_FRACTION * r.lines - 1e-9)))
    return r.lines_passed >= need


def build_samples(rows, require_power=False, null_side="both"):
    """Returns (items, evals, stats).
    items: informative clusters (exact stage, gate passed). evals: {(reel, axis): sorted [(seq, time, shot)]}
    from ALL rows (so evaluations without any informative candidate still count). The estimator clusters
    candidates within +-1 px of one evaluation and keeps the best (lowest T = largest increment): so do we.
    """
    stats = {"rows": len(rows), "coarse": 0, "gate_fail": 0, "power_fail": 0, "class_mismatch": 0, "exact": 0}
    evals = {}
    per_eval = {}
    for r in rows:
        evals.setdefault((r.reel, r.axis), {})[r.seq] = (r.time, r.shot)
        if r.stage != "exact":
            stats["coarse"] += 1
            continue
        stats["exact"] += 1
        if not gate_ok(r):
            stats["gate_fail"] += 1
            continue
        if require_power and not r.power:
            stats["power_fail"] += 1
            continue
        per_eval.setdefault((r.reel, r.axis, r.seq), []).append(r)
    items = []
    for key in sorted(per_eval):
        grp = sorted(per_eval[key], key=lambda r: (r.n, r.T))
        cluster, clusters = [grp[0]], []
        for r in grp[1:]:
            if r.n - cluster[-1].n <= 1:
                cluster.append(r)
            else:
                clusters.append(cluster)
                cluster = [r]
        clusters.append(cluster)
        for cl in clusters:
            r = min(cl, key=lambda x: (x.T, x.n))
            it = Item()
            it.reel, it.title, it.seq, it.time, it.axis = r.reel, r.title, r.seq, r.time, r.axis
            it.N, it.n, it.T, it.truth_n, it.bits, it.shot = r.N, r.n, r.T, r.truth_n, r.bits, r.shot
            it.cls = classify_latent(r.n, r.N)
            if r.cls_dump >= 0 and r.cls_dump != it.cls:
                stats["class_mismatch"] += 1
            if r.truth_n == 0:
                it.label = "null"
            elif abs(r.n - r.truth_n) <= 1:
                it.label = "alt"
            elif null_side == "below" and r.n > r.truth_n:
                continue   # candidates ABOVE the true latent size are alt-like (a band-limited picture survives a descale to any larger n)
            else:
                it.label = "null"
            items.append(it)
    ev = {k: [(s,) + v for s, v in sorted(d.items())] for k, d in evals.items()}
    return items, ev, stats


# ----------------------------------------------------------------------------------------------------------
# Robust fit
# ----------------------------------------------------------------------------------------------------------
def quantile(xs, q):
    s = sorted(xs)
    if not s:
        return float("nan")
    p = q * (len(s) - 1)
    lo = int(math.floor(p))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (p - lo)


def median(xs):
    return quantile(xs, 0.5)


def robust_gauss(xs):
    """(median, MAD-sd, dip-side one-sigma = median - P16, upper-side one-sigma = P84 - median)."""
    med = median(xs)
    mad = median([abs(x - med) for x in xs])
    return med, 1.4826 * mad, med - quantile(xs, 0.1587), quantile(xs, 0.8413) - med


def dominated(fit_null, fit_alt, c, policy):
    """True when the candidate class tables never give a LARGER clamped increment than the placeholder
    anywhere on the T grid (i.e. are not more permissive in claiming an upscale)."""
    tabs_f = {"null": [fit_null if i == c else PLACEHOLDER["null"][i] for i in range(4)],
              "alt": [fit_alt if i == c else PLACEHOLDER["alt"][i] for i in range(4)]}
    for t in policy.grid:
        if increment(tabs_f, c, t) > increment(PLACEHOLDER, c, t) + 1e-9:
            return False
    return True


def fit_class(null_x, alt_x, null_titles, alt_titles, c, policy):
    """Returns (null (mean, sd), alt (mean, sd), diag dict)."""
    ph_n, ph_a = PLACEHOLDER["null"][c], PLACEHOLDER["alt"][c]
    d = {"n_null": len(null_x), "n_alt": len(alt_x), "titles_null": len(set(null_titles)),
         "titles_alt": len(set(alt_titles)), "status": "placeholder", "why": "", "blend": 1.0}
    if len(null_x) < policy.min_null_hard or len(alt_x) < policy.min_alt_hard:
        d["why"] = "sparse (null %d < %d or alt %d < %d)" % (len(null_x), policy.min_null_hard, len(alt_x),
                                                              policy.min_alt_hard)
        return ph_n, ph_a, d
    if d["titles_null"] < policy.min_titles or d["titles_alt"] < policy.min_titles:
        d["why"] = "too few titles (null %d, alt %d < %d)" % (d["titles_null"], d["titles_alt"], policy.min_titles)
        return ph_n, ph_a, d
    m0, sd_mad, lo0, _ = robust_gauss(null_x)
    nsd = max(sd_mad, lo0, policy.sd_floor) * policy.null_widen * (1.0 + policy.se_terms / math.sqrt(len(null_x)))
    nul = (m0, max(nsd, policy.sd_floor))
    m1, asd_mad, _, up1 = robust_gauss(alt_x)
    asd = max(asd_mad, up1, policy.sd_floor) * (1.0 + policy.se_terms / math.sqrt(len(alt_x)))
    lam = min(1.0, len(alt_x) / float(policy.full_alt))          # alt shrunk toward the null in sparse classes
    alt = (nul[0] + lam * (m1 - nul[0]), max(nul[1] + lam * (asd - nul[1]), policy.sd_floor))
    if alt[0] > nul[0] - policy.min_separation * nul[1]:
        d["why"] = "alt not separated from null (alt %.3f null %.3f sd %.3f)" % (alt[0], nul[0], nul[1])
        return ph_n, ph_a, d
    thin = len(null_x) < policy.full_null or len(alt_x) < policy.full_alt
    d["status"] = "fitted"
    d["blend"] = 0.0
    if thin:
        d["status"] = "thin"
        fit_n, fit_a = nul, alt
        b = 0.0
        while not dominated(nul, alt, c, policy) and b < 1.0 - 1e-9:
            b = round(b + 0.1, 10)
            f = 1.0 - b   # always blend from the fit, never from an already blended value
            nul = (f * fit_n[0] + b * ph_n[0], f * fit_n[1] + b * ph_n[1])
            alt = (f * fit_a[0] + b * ph_a[0], f * fit_a[1] + b * ph_a[1])
        d["blend"] = b
        if b >= 1.0 - 1e-9:
            d["status"] = "placeholder"
            d["why"] = "thin and no blend dominated by the placeholder"
        else:
            d["why"] = "thin (null %d/%d, alt %d/%d): shrunk, blend toward placeholder %.1f" % (
                len(null_x), policy.full_null, len(alt_x), policy.full_alt, b)
    d["dominated"] = dominated(nul, alt, c, policy)
    return nul, alt, d


def fit_tables(items, policy, require_power_null=False):
    nx = [[] for _ in CLASSES]
    ax = [[] for _ in CLASSES]
    nt = [[] for _ in CLASSES]
    at = [[] for _ in CLASSES]
    for it in items:
        if it.label == "null":
            nx[it.cls].append(it.T)
            nt[it.cls].append(it.title)
        else:
            ax[it.cls].append(it.T)
            at[it.cls].append(it.title)
    tabs = {"null": [], "alt": []}
    diag = []
    for c in range(4):
        n_, a_, d = fit_class(nx[c], ax[c], nt[c], at[c], c, policy)
        tabs["null"].append(n_)
        tabs["alt"].append(a_)
        diag.append(d)
    return tabs, diag


# ----------------------------------------------------------------------------------------------------------
# Simulation of the estimator's evidence accumulation (ChainEvidence::Add / Total, per axis)
# ----------------------------------------------------------------------------------------------------------
class _Slot(object):
    __slots__ = ("n", "llr", "shot_sum", "shot_id", "shots", "evals")

    def __init__(self, n):
        self.n, self.llr, self.shot_sum, self.shot_id, self.shots, self.evals = n, 0.0, 0.0, None, 0, 0


def _total(s, cap):
    return s.llr + clampd(s.shot_sum, -cap, cap)


def _add(s, inc, shot, cap):
    if s.shots == 0 or s.shot_id != shot:
        s.llr += clampd(s.shot_sum, -cap, cap)
        s.shot_sum = 0.0
        s.shot_id = shot
        s.shots += 1
    s.shot_sum += inc
    s.evals += 1


def simulate_axis(evlist, items_by_seq, tables, c1, cap_fraction, shot_seconds, stop_on_truth=False,
                  reset_on_candidate=False):
    """Replays one reel axis. evlist: [(seq, time, shot)]. Returns list of events
    (ordinal_of_evaluation, time, n, cls, correct) for each hypothesis that reaches c1 (>= 4 evals)."""
    cap = cap_fraction * c1
    slots = []
    events = []
    for ordinal, (seq, tm, shot) in enumerate(evlist, 1):
        sid = shot if shot >= 0 else int(tm / shot_seconds)
        for it in items_by_seq.get(seq, ()):
            slot = None
            for s in slots:
                if abs(s.n - it.n) <= 1:
                    slot = s
                    break
            if slot is None:
                if len(slots) >= MAX_HYPOTHESES:
                    w = min(range(len(slots)), key=lambda i: _total(slots[i], cap))
                    slots.pop(w)
                slot = _Slot(it.n)
                slots.append(slot)
            slot.n = it.n
            _add(slot, increment(tables, it.cls, it.T), sid, cap)
            if _total(slot, cap) >= c1 and slot.evals >= MIN_EVALS_CANDIDATE:
                correct = it.truth_n != 0 and abs(it.n - it.truth_n) <= 1
                events.append((ordinal, tm, it.n, it.cls, correct))
                if reset_on_candidate:
                    slots = []
                    break
                if stop_on_truth and correct:
                    return events
    return events


def group_reels(items, evals):
    """{(reel, axis): (evlist, {seq: [items]}, meta)} where meta = (title, truth_n, bits)."""
    by = {}
    for it in items:
        by.setdefault((it.reel, it.axis), {}).setdefault(it.seq, []).append(it)
    out = {}
    meta_by_reel = {}
    for it in items:
        meta_by_reel[it.reel] = (it.title, it.truth_n, it.bits)
    for key, evl in evals.items():
        out[key] = (evl, by.get(key, {}), meta_by_reel.get(key[0]))
    return out


# ----------------------------------------------------------------------------------------------------------
# Metrics
# ----------------------------------------------------------------------------------------------------------
def auc(pos, neg):
    """P(pos > neg) + 0.5 ties (Mann-Whitney, rank based)."""
    if not pos or not neg:
        return float("nan")
    allv = sorted([(v, 1) for v in pos] + [(v, 0) for v in neg])
    ranks_pos, i = 0.0, 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        avg = (i + j) / 2.0 + 1.0
        ranks_pos += avg * sum(1 for k in range(i, j + 1) if allv[k][1] == 1)
        i = j + 1
    return (ranks_pos - len(pos) * (len(pos) + 1) / 2.0) / (len(pos) * len(neg))


def split_titles(titles, folds=2, seed="sid-cal"):
    """Deterministic by-title split: sort by sha1(seed+title), alternate. Returns {title: fold}."""
    order = sorted(set(titles), key=lambda t: hashlib.sha1((seed + "|" + t).encode("utf-8")).hexdigest())
    return {t: i % folds for i, t in enumerate(order)}


def reel_hours(evl):
    if len(evl) < 2:
        return 0.0
    ts = [e[1] for e in evl]
    gaps = sorted(b - a for a, b in zip(ts, ts[1:]) if b > a)
    step = gaps[len(gaps) // 2] if gaps else 0.0
    return (max(ts) - min(ts) + step) / 3600.0


def evaluate_heldout(test_items, test_evals, tables, ref_tables, c1, shot_seconds):
    """Held-out metrics for one fold with `tables` (and the placeholder as reference for the FA rate)."""
    res = {"auc": [[] for _ in range(4)], "auc_neg": [[] for _ in range(4)]}
    pos = [[] for _ in range(4)]
    neg = [[] for _ in range(4)]
    for it in test_items:
        v = llr(tables["null"][it.cls], tables["alt"][it.cls], it.T)
        (pos if it.label == "alt" else neg)[it.cls].append(v)
    res["pos"], res["neg"] = pos, neg
    reels = group_reels(test_items, test_evals)
    fa = {}
    for name, tabs in (("cal", tables), ("ph", ref_tables)):
        for cf in (SHOT_CAP_FRACTION, 1.0):
            ev_count, hours, by_class = 0, 0.0, [0] * 4
            wrong_alt = 0
            for (reel, axis), (evl, byseq, meta) in sorted(reels.items()):
                if meta is None or meta[1] != 0:
                    if meta is not None and name == "cal" and cf == SHOT_CAP_FRACTION:
                        for e in simulate_axis(evl, byseq, tabs, c1, cf, shot_seconds, reset_on_candidate=True):
                            if not e[4]:
                                wrong_alt += 1
                    continue
                hours += reel_hours(evl) / 2.0   # per axis; both axes run in the same title time
                for e in simulate_axis(evl, byseq, tabs, c1, cf, shot_seconds, reset_on_candidate=True):
                    ev_count += 1
                    by_class[e[3]] += 1
            fa[(name, cf)] = {"events": ev_count, "hours": hours, "by_class": by_class, "wrong_on_alt": wrong_alt}
    res["fa"] = fa
    lat = {8: [], 10: []}
    reached = {8: [0, 0], 10: [0, 0]}
    wrong_first = {8: 0, 10: 0}
    for (reel, axis), (evl, byseq, meta) in sorted(reels.items()):
        if meta is None or meta[1] == 0:
            continue
        b = 8 if meta[2] <= 8 else 10
        evs = simulate_axis(evl, byseq, tables, c1, SHOT_CAP_FRACTION, shot_seconds, stop_on_truth=True)
        hit = [e for e in evs if e[4]]
        reached[b][1] += 1
        if evs and not evs[0][4]:
            wrong_first[b] += 1
        if hit:
            reached[b][0] += 1
            lat[b].append(hit[0][0])
        else:
            lat[b].append(float("inf"))
    res["lat"], res["reached"], res["wrong_first"] = lat, reached, wrong_first
    return res


def cross_validate(items, evals, policy, c1, shot_seconds, seed):
    titles = [it.title for it in items]
    fold_of = split_titles(titles, 2, seed)
    if len(set(fold_of.values())) < 2 or len(fold_of) < 4:
        return None, fold_of
    pooled = {"pos": [[] for _ in range(4)], "neg": [[] for _ in range(4)], "events": {}, "lat": {8: [], 10: []},
              "reached": {8: [0, 0], 10: [0, 0]}, "wrong_first": {8: 0, 10: 0}, "fa": {}, "folds": []}
    for f in (0, 1):
        train = [it for it in items if fold_of[it.title] != f]
        test = [it for it in items if fold_of[it.title] == f]
        test_reels = set(it.reel for it in test)
        test_evals = {k: v for k, v in evals.items() if k[0] in test_reels}
        tabs, diag = fit_tables(train, policy)
        r = evaluate_heldout(test, test_evals, tabs, {"null": PLACEHOLDER["null"], "alt": PLACEHOLDER["alt"]}, c1,
                             shot_seconds)
        for c in range(4):
            pooled["pos"][c] += r["pos"][c]
            pooled["neg"][c] += r["neg"][c]
        for b in (8, 10):
            pooled["lat"][b] += r["lat"][b]
            pooled["reached"][b][0] += r["reached"][b][0]
            pooled["reached"][b][1] += r["reached"][b][1]
            pooled["wrong_first"][b] += r["wrong_first"][b]
        for k, v in r["fa"].items():
            p = pooled["fa"].setdefault(k, {"events": 0, "hours": 0.0, "by_class": [0] * 4, "wrong_on_alt": 0})
            p["events"] += v["events"]
            p["hours"] += v["hours"]
            p["wrong_on_alt"] += v["wrong_on_alt"]
            for c in range(4):
                p["by_class"][c] += v["by_class"][c]
        pooled["folds"].append((f, sorted(set(t for t in titles if fold_of[t] == f)), diag))
    return pooled, fold_of


# ----------------------------------------------------------------------------------------------------------
# Outputs
# ----------------------------------------------------------------------------------------------------------
def render_header(tabs, diag, prov):
    any_fit = any(d["status"] in ("fitted", "thin") for d in diag)
    L = []
    L.append("// Copyright(C) 2026 VP-V contributors")
    L.append("// SPDX-License-Identifier: GPL-3.0-only")
    L.append("#pragma once")
    L.append("")
    L.append("// GENERATED by tools/eval/source_id/calibrate_nulls.py. DO NOT EDIT: rerun the calibration instead.")
    L.append("// Provenance: dump set sha256 %s" % prov["hash"])
    L.append("//             date %s, %d files, %d rows (%d exact, %d dropped by gate/power/coarse), cv %s" % (
        prov["date"], prov["files"], prov["rows"], prov["exact"], prov["rows"] - prov["used_rows"], prov["cv"]))
    L.append("//             policy: min null/alt %d/%d, full %d/%d, titles >= %d, null widen %.2f, require_power %s" % (
        prov["policy"].min_null_hard, prov["policy"].min_alt_hard, prov["policy"].full_null,
        prov["policy"].full_alt, prov["policy"].min_titles, prov["policy"].null_widen, prov["require_power"]))
    for c in range(4):
        d = diag[c]
        L.append("//   %-12s null %5d (%d titles), alt %5d (%d titles): %s%s" % (
            CLASS_NAMES[c], d["n_null"], d["titles_null"], d["n_alt"], d["titles_alt"], d["status"],
            (" - " + d["why"]) if d["why"] else ""))
    L.append("#include \"ChainNullTables.h\"")
    L.append("")
    L.append("namespace SourceIdentity")
    L.append("{")
    L.append("\tnamespace Chain")
    L.append("\t{")
    L.append("\t\t// Drop-in for NullTables::Placeholder(): ChainEvidence(settings, GeneratedNullTables()).")
    L.append("\t\tinline NullTables GeneratedNullTables()")
    L.append("\t\t{")
    L.append("\t\t\tNullTables t;")
    L.append("\t\t\tt.placeholder = %s;" % ("false" if any_fit else "true"))
    for c in range(4):
        L.append("\t\t\tt.nul[%s].mean = %.6f; t.nul[%s].sd = %.6f;" % (CLASS_CPP[c], tabs["null"][c][0], CLASS_CPP[c],
                                                                       tabs["null"][c][1]))
        L.append("\t\t\tt.alt[%s].mean = %.6f; t.alt[%s].sd = %.6f;" % (CLASS_CPP[c], tabs["alt"][c][0], CLASS_CPP[c],
                                                                       tabs["alt"][c][1]))
    L.append("\t\t\treturn t;")
    L.append("\t\t}")
    L.append("\t}")
    L.append("}")
    return "\n".join(L) + "\n"


def _med_lat(v):
    if not v:
        return "n/a"
    m = median(v)
    return "inf" if m == float("inf") else "%.0f" % m


def render_report(tabs, diag, prov, cv, fold_of, c1, shot_seconds, stats):
    L = []
    L.append("# SID calibration report")
    L.append("")
    L.append("Dump set %s, %s, %d files, %d rows (%d bad), %d informative clusters. c1 = %.2f nats, per-shot cap c1/2 = %.2f,"
             " shot = %s." % (prov["hash"][:12], prov["date"], prov["files"], prov["rows"], prov["bad"],
                              prov["items"], c1, c1 / 2.0,
                              "shot_id column" if prov["has_shot"] else "%.1f s of source time (no shot_id in dump)" % shot_seconds))
    L.append("Rows: %d exact, %d coarse (ignored), %d failed the line gate, %d failed power%s, %d class mismatches vs dump." % (
        stats["exact"], stats["coarse"], stats["gate_fail"], stats["power_fail"],
        "" if prov["require_power"] else " (not applied)", stats["class_mismatch"]))
    L.append("")
    L.append("## Fitted tables (stage exact)")
    L.append("| class | null n/titles | alt n/titles | null mean, sd | alt mean, sd | status |")
    L.append("|---|---|---|---|---|---|")
    for c in range(4):
        d = diag[c]
        L.append("| %s | %d/%d | %d/%d | %.3f, %.3f | %.3f, %.3f | %s%s |" % (
            CLASS_NAMES[c], d["n_null"], d["titles_null"], d["n_alt"], d["titles_alt"], tabs["null"][c][0],
            tabs["null"][c][1], tabs["alt"][c][0], tabs["alt"][c][1], d["status"],
            (": " + d["why"][:60]) if d["why"] else ""))
    L.append("")
    if cv is None:
        L.append("## Cross-validation: NOT POSSIBLE (fewer than 4 titles or one fold empty). Tables are UNVALIDATED.")
    else:
        L.append("## Held-out validation (2-fold CV by title, %d titles; folds fit on the other half)" % len(fold_of))
        L.append("| class | held-out null | held-out alt | ROC AUC (LLR) |")
        L.append("|---|---|---|---|")
        for c in range(4):
            a = auc(cv["pos"][c], cv["neg"][c])
            L.append("| %s | %d | %d | %s |" % (CLASS_NAMES[c], len(cv["neg"][c]), len(cv["pos"][c]),
                                                 "n/a" if a != a else "%.3f" % a))
        L.append("")
        L.append("False CANDIDATE entries on native reels (any axis; state reset after each entry; held-out only):")
        L.append("| tables, shot cap | entries | hours | per hour | by class k8/k16/rat/gen |")
        L.append("|---|---|---|---|---|")
        for (name, cf), v in sorted(cv["fa"].items(), key=lambda kv: (kv[0][0], kv[0][1])):
            rate = (v["events"] / v["hours"]) if v["hours"] > 0 else float("nan")
            L.append("| %s, c1*%.1f | %d | %.2f | %s | %s |" % (
                "calibrated" if name == "cal" else "placeholder", cf, v["events"], v["hours"],
                "n/a" if rate != rate else "%.3f" % rate, "/".join(str(x) for x in v["by_class"])))
        w = cv["fa"].get(("cal", SHOT_CAP_FRACTION), {}).get("wrong_on_alt", 0)
        L.append("Budget <= 1 per hour. Wrong-n CANDIDATE entries on alt reels (calibrated, cap c1/2): %d." % w)
        L.append("")
        L.append("Evaluations to c1 on alt reels (per reel and axis, from reel start; inf = never reached):")
        L.append("| bit depth | reel-axes | reached | median evals | wrong n first |")
        L.append("|---|---|---|---|---|")
        for b in (8, 10):
            r = cv["reached"][b]
            L.append("| %d | %d | %d | %s | %d |" % (b, r[1], r[0], _med_lat(cv["lat"][b]), cv["wrong_first"][b]))
    L.append("")
    L.append("Notes: native hypothesis not simulated (it cannot raise a candidate); two-axis prior and HOLD not simulated;"
             " power rule %s." % ("required on every row" if prov["require_power"] else
                                  "not applied to candidate rows (the estimator does not apply it there)"))
    return "\n".join(L) + "\n"


def run(args):
    policy = Policy()
    rows, info = load_dumps(args.dump)
    items, evals, stats = build_samples(rows, args.require_power, args.null_side)
    c1 = thresholds()
    tabs, diag = fit_tables(items, policy)
    cv, fold_of = cross_validate(items, evals, policy, c1, args.shot_seconds, args.seed)
    prov = {"hash": info["hash"], "files": info["files"], "rows": len(rows), "bad": info["bad"],
            "exact": stats["exact"], "used_rows": len(items),   # clusters, not rows: the header says dropped = rows - clusters
            "items": len(items), "date": args.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
            "cv": "2-fold by title" if cv else "NOT POSSIBLE (unvalidated)", "policy": policy,
            "require_power": args.require_power, "has_shot": any(r.shot >= 0 for r in rows)}
    os.makedirs(args.out, exist_ok=True)
    hdr = render_header(tabs, diag, prov)
    rep = render_report(tabs, diag, prov, cv, fold_of, c1, args.shot_seconds, stats)
    if len(rep.splitlines()) > 60:
        raise SystemExit("report exceeds 60 lines")
    with open(os.path.join(args.out, "NullTables.generated.h"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(hdr)
    with open(os.path.join(args.out, "calibration-report.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(rep)
    return hdr, rep, tabs, diag, cv, fold_of, items


# ----------------------------------------------------------------------------------------------------------
# Selftest: synthetic dumps (natives N(0, s)-like, alts shifted, a sparse class, a leaking title)
# ----------------------------------------------------------------------------------------------------------
class _Lcg(object):
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def u(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return (self.s >> 8) / float(1 << 24)

    def gauss(self):
        return math.sqrt(-2.0 * math.log(max(self.u(), 1e-12))) * math.cos(2.0 * math.pi * self.u())


def _class_sizes(N):
    """Candidate sizes per class on an N axis (used to place synthetic candidates)."""
    out = {0: [], 1: [], 2: [], 3: []}
    for n in range(int(N * 0.2), int(N * 0.985)):
        out[classify_latent(n, N)].append(n)
    return out


def make_synthetic(path, seed=7):
    """Writes CSV dumps. True null law per class (mean, sd); true alt law -0.30 (8-bit) / -0.45 (10-bit), sd 0.08.
    Class K16 is SPARSE (a handful of rows only); one title ('leak') has a native AND an upscaled reel."""
    rng = _Lcg(seed)
    N = 2160
    sizes = _class_sizes(N)
    true_null = {0: (-0.08, 0.04), 1: (-0.07, 0.04), 2: (-0.03, 0.03), 3: (-0.02, 0.03)}
    plan = []   # (reel_id, title, truth_n, bits)
    for t in range(8):
        title = "title%d" % t
        plan.append(("%s_nat" % title, title, 0, 8 if t % 2 == 0 else 10))
        plan.append(("%s_half" % title, title, 1080, 8 if t % 2 == 0 else 10))      # k/8 class
        plan.append(("%s_two3" % title, title, 1440, 10))                          # rational class
        plan.append(("%s_gen" % title, title, 1317, 8))                            # generic class (0.61 N)
    plan.append(("leak_nat", "leak", 0, 8))
    plan.append(("leak_up", "leak", 1080, 8))
    junk_rows = 0
    with open(os.path.join(path, "_marker"), "w") as fh:
        fh.write("synthetic")
    for reel, title, truth_n, bits in plan:
        with open(os.path.join(path, reel + ".csv"), "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(REQUIRED)
            for seq in range(160):
                tm = float(seq)
                for axis in ("x", "y"):
                    cands = []
                    if truth_n:
                        cands += [truth_n - 1, truth_n, truth_n + 1]
                    for _ in range(3):
                        pool = 3 if rng.u() < 0.55 else (2 if rng.u() < 0.5 else 0)
                        if rng.u() < 0.0002:
                            pool = 1                       # K16 appears a few times in the whole set: sparse
                        lst = sizes[pool]
                        n = lst[int(rng.u() * len(lst)) % len(lst)]
                        if truth_n and abs(n - truth_n) <= 1:
                            continue
                        cands.append(n)
                    for n in cands:
                        c = classify_latent(n, N)
                        if truth_n and abs(n - truth_n) <= 1:
                            mu = -0.30 if bits == 8 else -0.45
                            T = mu + 0.08 * rng.gauss()
                        else:
                            mu, sd = true_null[c]
                            T = mu + sd * rng.gauss()
                        lines, passed = 32, 28
                        if rng.u() < 0.1:
                            passed = 4                      # fails the 8-of-32 gate: its (junk) T must never count
                            T = -2.0
                            junk_rows += 1
                        for stage in ("coarse", "exact"):
                            w.writerow([reel, title, seq, "%.3f" % tm, axis, N, n, CLASS_NAMES[c], "%.5f" % T, stage,
                                        "bilinear", "0.1", passed, lines, 1, "1.155", truth_n, bits, "bilinear", 0])
    os.remove(os.path.join(path, "_marker"))
    return junk_rows


def selftest():
    import tempfile
    fails = []

    def check(cond, msg):
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            fails.append(msg)

    # --- port of ClassifyLatent against known cases of the C++ header
    check(classify_latent(1080, 2160) == 0, "ClassifyLatent: 1080/2160 -> K8")
    check(classify_latent(1440, 2160) == 2 or classify_latent(1440, 2160) == 0, "ClassifyLatent: 2/3 not generic")
    check(classify_latent(1317, 2160) == 3, "ClassifyLatent: 0.61 N -> generic")
    check(abs(thresholds() - 14.87) < 0.02, "c1 = %.3f (README ~14.9)" % thresholds())

    # --- AUC sanity
    check(abs(auc([1, 2, 3], [0, 0.5, 1.5]) - 8.0 / 9.0) < 1e-9, "AUC rank formula")

    with tempfile.TemporaryDirectory() as td:
        dd = os.path.join(td, "dump")
        out = os.path.join(td, "out")
        os.makedirs(dd)
        junk = make_synthetic(dd)
        ns = argparse.Namespace(dump=dd, out=out, date="2026-11-07", require_power=False, null_side="both",
                                shot_seconds=3.0, seed="sid-cal")
        hdr, rep, tabs, diag, cv, fold_of, items = run(ns)

        # gate: junk T=-2 never reaches the samples
        check(min(it.T for it in items) > -1.0, "gate drops the junk rows (%d junk exact rows written)" % junk)
        check(all(it.cls in (0, 1, 2, 3) for it in items), "classes recomputed from n, N")

        # fit on the dense generic class: truth null (-0.02, 0.03), alt shrunk fit of -0.30/-0.45 pooled
        d = diag[3]
        check(d["status"] == "fitted", "generic class fitted: %s" % d["status"])
        nm, nsd = tabs["null"][3]
        check(abs(nm - (-0.02)) < 0.01, "generic null mean %.4f ~ -0.02" % nm)
        check(0.03 <= nsd <= 0.03 * 1.25 * 1.3, "generic null sd %.4f is the truth 0.03 widened (<= 1.63x)" % nsd)
        # rational class has alt from the 2/3 reels
        check(diag[2]["status"] in ("fitted", "thin"), "rational class has data: %s" % diag[2]["status"])
        check(tabs["alt"][2][0] < tabs["null"][2][0] - 0.1, "rational alt sits below its null (%.3f vs %.3f)" % (
            tabs["alt"][2][0], tabs["null"][2][0]))

        # sparse class K16: placeholder or placeholder-dominated, and its table is never more permissive
        d16 = diag[1]
        check(d16["n_null"] < Policy().min_null_hard, "K16 is sparse (%d null rows)" % d16["n_null"])
        check(d16["status"] == "placeholder" and tabs["null"][1] == PLACEHOLDER["null"][1]
              and tabs["alt"][1] == PLACEHOLDER["alt"][1], "sparse class keeps the placeholder")
        # no class is more permissive than the placeholder where thin
        pol = Policy()
        for c in range(4):
            if diag[c]["status"] in ("thin", "placeholder"):
                check(dominated(tabs["null"][c], tabs["alt"][c], c, pol), "class %s never more permissive than placeholder" % CLASSES[c])

        # thin class unit test: a fitted-but-thin class is blended until dominated
        xs_null = [-0.01 + 0.01 * ((i * 37) % 11 - 5) / 5.0 for i in range(60)]
        xs_alt = [-0.6 + 0.02 * ((i * 17) % 7 - 3) for i in range(40)]
        tn, ta, dg = fit_class(xs_null, xs_alt, ["a", "b", "c"] * 20, ["a", "b", "c", "d"] * 10, 3, pol)
        check(dg["status"] in ("thin", "placeholder") and dominated(tn, ta, 3, pol),
              "thin class (60 null / 40 alt) is shrunk and dominated by the placeholder (blend %.1f)" % dg["blend"])
        tn2, ta2, dg2 = fit_class(xs_null[:20], xs_alt, ["a", "b", "c"] * 7, ["a", "b", "c", "d"] * 10, 3, pol)
        check(dg2["status"] == "placeholder" and tn2 == PLACEHOLDER["null"][3], "too few nulls -> placeholder")
        tn3, ta3, dg3 = fit_class(xs_null, xs_alt, ["a"] * 60, ["a", "b", "c", "d"] * 10, 3, pol)
        check(dg3["status"] == "placeholder", "one title only -> placeholder")

        # by-title split: every title in exactly one fold; leak title reels together; both folds used
        titles = sorted(set(it.title for it in items))
        check(set(fold_of) == set(titles) and set(fold_of.values()) == {0, 1}, "split covers all titles in both folds")
        folds_of_reel = {}
        for it in items:
            folds_of_reel.setdefault(it.reel, set()).add(fold_of[it.title])
        check(all(len(v) == 1 for v in folds_of_reel.values()), "no reel straddles folds")
        check(fold_of["leak"] in (0, 1) and {fold_of[it.title] for it in items if it.title == "leak"} == {fold_of["leak"]},
              "leaking title (native + upscaled reel of one master) stays in ONE fold")
        for f, ftitles, _ in cv["folds"]:
            check(all(fold_of[t] == f for t in ftitles), "fold %d titles are exactly its own" % f)

        # CV results
        a = auc(cv["pos"][3], cv["neg"][3])
        check(a != a or a > 0.9, "generic held-out AUC %s" % ("n/a" if a != a else "%.3f" % a))
        a2 = auc(cv["pos"][2], cv["neg"][2])
        check(a2 > 0.9, "rational held-out AUC %.3f" % a2)
        fa = cv["fa"][("cal", SHOT_CAP_FRACTION)]
        check(fa["hours"] > 0 and fa["events"] == 0, "no false CANDIDATE on pure-noise natives (%d in %.2f h)" % (
            fa["events"], fa["hours"]))
        for b in (8, 10):
            r = cv["reached"][b]
            m = median(cv["lat"][b]) if cv["lat"][b] else float("inf")
            check(r[1] > 0 and r[0] == r[1] and m <= 30, "%d-bit alt reels reach c1 (%d/%d, median %s evals)" % (
                b, r[0], r[1], "%.0f" % m))
            check(cv["wrong_first"][b] == 0, "%d-bit: no wrong n first" % b)

        # outputs
        check(len(rep.splitlines()) <= 60, "report %d lines <= 60" % len(rep.splitlines()))
        check(hdr.startswith("// Copyright(C) 2026 VP-V contributors\n// SPDX-License-Identifier: GPL-3.0-only\n#pragma once"),
              "header: copyright, SPDX, pragma once")
        check("Provenance: dump set sha256 " in hdr and "2026-11-07" in hdr, "header: provenance (hash, date)")
        assigns = re.findall(r"t\.(nul|alt)\[(kClass\w+)\]\.(mean|sd) = (-?\d+\.\d{6});", hdr)
        check(len(assigns) == 16 and len(set((a_, b_, c_) for a_, b_, c_, _ in assigns)) == 16, "header: 16 distinct assignments")
        check(hdr.count("{") == hdr.count("}") and "NullTables t;" in hdr and "return t;" in hdr, "header: braces balanced")
        check(not re.search(r"inline constexpr|if constexpr|std::filesystem|auto \[|_v<", hdr), "header: C++14 only")
        check(all(float(v) > 0 for a_, b_, c_, v in assigns if c_ == "sd"), "header: all sd positive")
        hdr2, *_ = run(ns)
        check(hdr2 == hdr, "deterministic: second run identical header")
        # require-power run still works and drops nothing here (power_seen = 1 everywhere)
        ns.require_power = True
        hdr3, *_ = run(ns)
        check(hdr3.count("t.nul") == 8, "require_power run writes a full table")

    print("SELFTEST " + ("FAILED (%d)" % len(fails) if fails else "PASSED"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Calibrate the SID chain NullTables from harness calibration dumps.")
    ap.add_argument("--dump", help="directory of per-reel CSV dumps (VP_SOURCE_ID_CAL_DUMP)")
    ap.add_argument("--out", help="output directory (NullTables.generated.h, calibration-report.md)")
    ap.add_argument("--selftest", action="store_true", help="synthetic self-test, no inputs")
    ap.add_argument("--require-power", action="store_true",
                    help="also drop rows whose power probe did not see its injection (default: the estimator "
                         "applies the power rule to the NATIVE hypothesis only, not to candidate increments)")
    ap.add_argument("--null-side", choices=("both", "below"), default="both",
                    help="null rows on upscaled reels: n more than 1 from truth on both sides (default, the "
                         "spec) or only below the truth (above it a descale is lossless and alt-like)")
    ap.add_argument("--shot-seconds", type=float, default=3.0,
                    help="shot length (s) used for the per-shot cap when the dump has no shot_id column")
    ap.add_argument("--seed", default="sid-cal", help="seed of the deterministic by-title fold split")
    ap.add_argument("--date", default="", help="date for the provenance (default: today, UTC)")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.dump or not a.out:
        ap.error("--dump and --out are required (or --selftest)")
    hdr, rep, *_ = run(a)
    sys.stdout.write(rep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
