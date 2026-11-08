# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""SID-CAL: calibrate the chain estimator's null / alt tables (doc 78 D3, D9, D10, sections 3, 4, 9).

Reads the calibration dumps written by the harness (VP_SOURCE_ID_CAL_DUMP: one CSV per reel, one row per
evaluation x axis x tested candidate) and writes
  NullTables.generated.h   per size class (K8, K16, Rational, Generic): GaussDist null + alt, exactly the
                           struct of identity/chain/ChainNullTables.h (C++14);
  calibration-report.md    leakage-controlled validation (2-fold CV BY MASTER), <= 60 lines.

Run:  python -I tools/eval/source_id/calibrate_nulls.py --dump <dir> --out <dir>
Test: python -I tools/eval/source_id/calibrate_nulls.py --selftest

Conservative by construction (a table can only make the estimator abstain MORE than the placeholder where
data is thin); see tools/eval/source_id/README.md, section "calibrate_nulls.py". The false-crossing simulation replays
ChainEvidence.h exactly (recent window in seconds, HOLD forget, first-cut clear, E2 shot dwell).
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
MIN_EVALS_CANDIDATE = 4                 # a c1 crossing needs >= 4 informative evaluations (non-binding: the per-shot cap already needs 2 shots)
# The recent window (EvidenceSettings, SID train r2 N3 + the owner-approved revert window): closed shots expire after
# windowMargin x (c1 / windowYieldNats) x the MEASURED mean gap between the axis's evaluations, clamped (seconds).
WINDOW_YIELD_NATS, WINDOW_MARGIN, WINDOW_MIN_SEC, WINDOW_MAX_SEC, RATE_SMOOTHING = 1.4, 1.5, 10.0, 600.0, 0.25
# E2 (owner-approved latch-semantics change): the minimum informative-shot dwell, source seconds (ChainEvidence.h kMinShotDwellSec).
# 0 = the estimator without E2.
MIN_SHOT_DWELL_SEC = 0.0
REQUIRED_V2 = ["reel_id", "title", "eval_seq", "time_s", "axis", "N", "n", "class", "T", "stage", "best_kernel",
               "kernel_margin", "lines_passed", "lines", "power_seen", "floor_codes", "truth_n", "bit_depth",
               "truth_kernel", "truth_sharpen", "shot_id"]
V3_EXTRA = ["T_ev", "informative", "master", "has_truth", "truth_n1", "cut_id"]
REQUIRED = REQUIRED_V2 + V3_EXTRA        # dump v3 contract: all 27 columns, in this order, REQUIRED
AGREE_TOL, AGREE_BONUS = 0.015, 2.0      # EvidenceSettings::agreementTolerance / agreementBonus
POISSON95 = [3.00, 4.74, 6.30, 7.75, 9.15, 10.51, 11.84, 13.15, 14.43, 15.71, 16.96]   # upper 95% limit for k = 0..10


class Policy(object):
    """Fit policy (documented in the README). Counts are EFFECTIVE sizes: distinct (reel, axis, truth shot) groups."""
    def __init__(self):
        self.min_null_hard = 30      # fewer effective null samples (shots) than this: the class keeps the placeholder
        self.min_alt_hard = 20
        self.min_masters = 5         # fewer masters (footage ids) than this on either side: placeholder
        self.full_null = 150         # at or above both full counts: robust fit used as is (plus widening)
        self.full_alt = 75           # below: shrinkage toward the null and a dominance check vs the placeholder
        self.null_widen = 1.25       # the null sd is widened by this factor ...
        self.se_terms = 2.0          # ... and by (1 + se_terms / sqrt(n_eff)) for the estimate's own uncertainty
        self.sd_floor = 0.005
        self.min_separation = 0.25   # alt mean must sit this many null-sds below the null mean, else placeholder
        # Simulation of the estimator (false crossings, evaluations to c1): the same switches as ChainEvidence.
        self.sim_window = True       # the recent window (N3), seconds from the measured evaluation gap
        self.sim_hold = True         # a HOLD evaluation forgets its axis, the first cut after one clears both axes
        self.dwell_sec = MIN_SHOT_DWELL_SEC   # E2: a shot's evidence counts once the shot has lasted this many source seconds


def make_grid(hi):
    """T grid of the dominance check: -2.0 .. max(1.0, hi), at most ~1200 points."""
    hi = max(1.0, hi)
    step = max(0.01, (hi + 2.0) / 1200.0)
    return [-2.0 + step * i for i in range(int((hi + 2.0) / step) + 2)]


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
# Reading dumps (v3 contract: D:\codex\vp-handover\sid\cal-dump-v3-contract.md)
# ----------------------------------------------------------------------------------------------------------
class Row(object):
    __slots__ = ("reel", "master", "seq", "time", "axis", "N", "n", "cls_dump", "T", "Tev", "stage", "lines_passed",
                 "lines", "power", "truth_n", "truth_n1", "bits", "tshot", "cut", "informative", "has_truth")


def _num(s, default=None):
    try:
        v = float(s)
    except (TypeError, ValueError):
        return default
    return v if math.isfinite(v) else default


def _class_index(s):
    t = (s or "").strip().lower().replace("/", "").replace("<=6", "").replace("_", "")
    for i, key in enumerate(("k8", "k16", "rational", "generic")):
        if t == key or t == str(i):
            return i
    return -1


def load_dumps(path):
    """Returns (rows, info). Fails loudly on a v1/v2 dump. info: files, bad rows, dump hash."""
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
            names = rd.fieldnames or []
            missing = [c for c in REQUIRED if c not in names]
            if missing:
                if all(c in names for c in REQUIRED_V2):
                    raise SystemExit("%s: dump v2 - regenerate with v3 writer (missing %s)" % (fn, ",".join(missing)))
                raise SystemExit("%s: missing columns %s" % (fn, ",".join(missing)))
            for rec in rd:
                r = Row()
                r.reel, r.master = rec["reel_id"], rec["master"]
                v = [_num(rec[k]) for k in ("eval_seq", "time_s", "N", "n", "lines_passed", "lines", "truth_n",
                                            "bit_depth", "informative", "has_truth", "truth_n1", "cut_id", "shot_id")]
                if None in v or v[2] <= 0 or v[5] <= 0 or not r.master:
                    bad += 1
                    continue
                (r.seq, r.time, r.N, r.n, r.lines_passed, r.lines, r.truth_n, r.bits, r.informative, r.has_truth,
                 r.truth_n1, r.cut, r.tshot) = (int(v[0]), v[1], int(v[2]), int(v[3]), int(v[4]), int(v[5]),
                                                int(v[6]), int(v[7]), int(v[8]) == 1, int(v[9]) == 1, int(v[10]),
                                                int(v[11]), int(v[12]))
                r.axis = rec["axis"].strip()
                r.stage = rec["stage"].strip().lower()
                r.T, r.Tev = _num(rec["T"]), _num(rec["T_ev"])
                if r.stage == "exact" and r.informative and r.Tev is None:
                    bad += 1
                    continue
                r.cls_dump = _class_index(rec["class"])
                r.power = rec["power_seen"].strip().lower() in ("1", "true", "yes")
                rows.append(r)
    return rows, {"files": len(files), "bad": bad, "hash": h.hexdigest()}


# ----------------------------------------------------------------------------------------------------------
# Samples: truth, informative, per-evaluation clustering exactly as the estimator, NULL / ALT labelling
# ----------------------------------------------------------------------------------------------------------
class Item(object):
    """One informative cluster (what the estimator turns into one hypothesis increment). T = T_ev."""
    __slots__ = ("reel", "master", "seq", "time", "axis", "N", "n", "T", "cls", "truth_n", "truth_n1", "bits", "label",
                 "tshot", "cut")


def gate_ok(r):
    need = max(LINE_GATE_FLOOR, int(math.ceil(LINE_GATE_FRACTION * r.lines - 1e-9)))
    return r.lines_passed >= need


def is_alt(n, truth_n, truth_n1):
    return abs(n - truth_n) <= 1 or (truth_n1 > 0 and abs(n - truth_n1) <= 1)


def build_samples(rows, require_power=False, null_side="both"):
    """Returns (items, stats). Only exact, informative, has_truth rows that pass the line gate count; candidates of one
    evaluation and axis within +-1 px are clustered and the one with the lowest T_ev kept, as ChainEvidence does."""
    stats = {"rows": len(rows), "coarse": 0, "no_truth": 0, "not_informative": 0, "gate_fail": 0, "power_fail": 0,
             "double_drop": 0, "class_mismatch": 0, "exact": 0}
    per_eval = {}
    for r in rows:
        if r.stage != "exact":
            stats["coarse"] += 1
            continue
        stats["exact"] += 1
        if not r.has_truth:
            stats["no_truth"] += 1
        elif not r.informative or r.Tev is None:
            stats["not_informative"] += 1
        elif not gate_ok(r):
            stats["gate_fail"] += 1
        elif require_power and not r.power:
            stats["power_fail"] += 1
        else:
            per_eval.setdefault((r.reel, r.axis, r.seq), []).append(r)
    items = []
    for key in sorted(per_eval):
        grp = sorted(per_eval[key], key=lambda r: (r.n, r.Tev))
        clusters, cluster = [], [grp[0]]
        for r in grp[1:]:
            if r.n - cluster[-1].n <= 1:
                cluster.append(r)
            else:
                clusters.append(cluster)
                cluster = [r]
        clusters.append(cluster)
        for cl in clusters:
            r = min(cl, key=lambda x: (x.Tev, x.n))
            it = Item()
            it.reel, it.master, it.seq, it.time, it.axis = r.reel, r.master, r.seq, r.time, r.axis
            it.N, it.n, it.T, it.truth_n, it.truth_n1 = r.N, r.n, r.Tev, r.truth_n, r.truth_n1
            it.bits, it.tshot, it.cut = r.bits, r.tshot, r.cut
            it.cls = classify_latent(r.n, r.N)
            if r.cls_dump >= 0 and r.cls_dump != it.cls:
                stats["class_mismatch"] += 1
            if r.truth_n == 0:
                it.label = "null"
            elif is_alt(r.n, r.truth_n, r.truth_n1):
                it.label = "alt"
            elif r.truth_n1 > 0:
                stats["double_drop"] += 1      # double-scaled reel: not alt, and a real latent is never a null
                continue
            elif null_side == "below" and r.n > r.truth_n:
                continue   # above the true latent size a descale is lossless: alt-like
            else:
                it.label = "null"
            items.append(it)
    return items, stats


def _is_hold(flags):
    """The dump has no HOLD column. The estimator's HOLD evaluation (its top candidate is mixed) leaves a gated candidate whose
    statistic was never reached (T_ev empty, the line gate passed) and NO informative candidate (the writer clears them). That
    is the proxy; a HOLD-looking evaluation without any informative candidate over-detects only when a wrong, non-top candidate
    is mixed while the top one is merely low-detail (rare)."""
    return bool(flags) and flags[0] and not flags[1]


def build_segments(rows):
    """Truth segments from ALL rows (any stage): maximal runs of consecutive evaluations of a reel with the same
    per-axis (truth_n, truth_n1). Evaluations without truth (has_truth=0 on any axis) are boundaries and never
    simulated. Returns [dict(reel, master, truth {axis: n}, truth1 {axis: n1}, bits {axis: bits},
    evals [(seq, {axis: (time, cut, hold)})])]; hold = the HOLD proxy (see _is_hold)."""
    ev = {}
    hold = {}      # (reel, seq, axis) -> [a gated candidate was never judged (mixed), some candidate was informative]
    for r in rows:
        e = ev.setdefault(r.reel, {}).setdefault(r.seq, {})
        e[r.axis] = (r.time, r.cut, r.truth_n, r.bits, r.has_truth, r.master, r.truth_n1)
        if r.stage == "exact":
            hk = hold.setdefault((r.reel, r.seq, r.axis), [False, False])
            if r.informative and r.Tev is not None:
                hk[1] = True
            elif r.Tev is None and gate_ok(r):
                hk[0] = True
    segs = []
    for reel in sorted(ev):
        cur, cur_key = None, None
        for seq in sorted(ev[reel]):
            e = ev[reel][seq]
            if any(not v[4] for v in e.values()):
                cur, cur_key = None, None
                continue
            key = tuple(sorted((a, v[2], v[6]) for a, v in e.items()))
            if cur is None or key != cur_key:
                cur_key = key
                cur = {"reel": reel, "master": next(iter(e.values()))[5], "truth": dict((a, t) for a, t, _ in key),
                       "truth1": dict((a, t1) for a, _, t1 in key), "bits": {a: v[3] for a, v in e.items()},
                       "evals": []}
                segs.append(cur)
            cur["evals"].append((seq, {a: (v[0], v[1], _is_hold(hold.get((reel, seq, a)))) for a, v in e.items()}))
    return segs


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


def null_sd(xs, floor):
    """Tail-matched null spread: the largest of the MAD sd and the dip-side spread matched at 1, 2 and 2.5 sigma
    (the false-confident risk sits in the dip tail, not at one sigma)."""
    med = median(xs)
    mad = 1.4826 * median([abs(x - med) for x in xs])
    return max(mad, med - quantile(xs, 0.1587), (med - quantile(xs, 0.0228)) / 2.0,
               (med - quantile(xs, 0.0062)) / 2.5, floor)


def alt_sd(xs, floor):
    med = median(xs)
    mad = 1.4826 * median([abs(x - med) for x in xs])
    return max(mad, quantile(xs, 0.8413) - med, floor)


def dominated(fit_null, fit_alt, c, grid):
    """True when the class tables never give a LARGER clamped increment than the placeholder anywhere on the grid."""
    tabs_f = {"null": [fit_null if i == c else PLACEHOLDER["null"][i] for i in range(4)],
              "alt": [fit_alt if i == c else PLACEHOLDER["alt"][i] for i in range(4)]}
    for t in grid:
        if increment(tabs_f, c, t) > increment(PLACEHOLDER, c, t) + 1e-9:
            return False
    return True


def fit_class(null_x, alt_x, null_grp, alt_grp, null_mst, alt_mst, c, policy, grid):
    """null_x/alt_x: T_ev samples; *_grp: (reel, axis, truth shot) keys (effective size = distinct keys); *_mst: masters.
    Returns (null (mean, sd), alt (mean, sd), diag)."""
    ph_n, ph_a = PLACEHOLDER["null"][c], PLACEHOLDER["alt"][c]
    ne, ae = len(set(null_grp)), len(set(alt_grp))
    d = {"n_null": len(null_x), "n_alt": len(alt_x), "eff_null": ne, "eff_alt": ae, "m_null": len(set(null_mst)),
         "m_alt": len(set(alt_mst)), "status": "placeholder", "why": "", "blend": 1.0,
         "max_null_T": max(null_x) if null_x else float("nan")}
    if ne < policy.min_null_hard or ae < policy.min_alt_hard:
        d["why"] = "sparse (n_eff null %d < %d or alt %d < %d)" % (ne, policy.min_null_hard, ae, policy.min_alt_hard)
        return ph_n, ph_a, d
    if d["m_null"] < policy.min_masters or d["m_alt"] < policy.min_masters:
        d["why"] = "too few masters (null %d, alt %d < %d)" % (d["m_null"], d["m_alt"], policy.min_masters)
        return ph_n, ph_a, d
    m0 = median(null_x)
    nsd = null_sd(null_x, policy.sd_floor) * policy.null_widen * (1.0 + policy.se_terms / math.sqrt(ne))
    nul = (m0, max(nsd, policy.sd_floor))
    m1 = median(alt_x)
    asd = alt_sd(alt_x, policy.sd_floor) * (1.0 + policy.se_terms / math.sqrt(ae))
    lam = min(1.0, ae / float(policy.full_alt))          # alt shrunk toward the null in sparse classes
    alt = (nul[0] + lam * (m1 - nul[0]), max(nul[1] + lam * (asd - nul[1]), policy.sd_floor))
    if alt[0] > nul[0] - policy.min_separation * nul[1]:
        d["why"] = "alt not separated from null (alt %.3f null %.3f sd %.3f)" % (alt[0], nul[0], nul[1])
        return ph_n, ph_a, d
    d["status"], d["blend"] = "fitted", 0.0
    if ne < policy.full_null or ae < policy.full_alt:
        d["status"] = "thin"
        fit_n, fit_a = nul, alt
        b = 0.0
        while not dominated(nul, alt, c, grid) and b < 1.0 - 1e-9:
            b = round(b + 0.1, 10)
            f = 1.0 - b   # always blend from the fit, never from an already blended value
            nul = (f * fit_n[0] + b * ph_n[0], f * fit_n[1] + b * ph_n[1])
            alt = (f * fit_a[0] + b * ph_a[0], f * fit_a[1] + b * ph_a[1])
        d["blend"] = b
        if b >= 1.0 - 1e-9:
            d["status"] = "placeholder"
            d["why"] = "thin and no blend dominated by the placeholder"
        else:
            d["why"] = "thin (n_eff %d/%d, %d/%d): shrunk, blend %.1f" % (ne, policy.full_null, ae, policy.full_alt, b)
    d["dominated"] = dominated(nul, alt, c, grid)
    return nul, alt, d


def fit_tables(items, policy):
    nx = [[] for _ in CLASSES]
    ax = [[] for _ in CLASSES]
    ng = [[] for _ in CLASSES]
    ag = [[] for _ in CLASSES]
    nm = [[] for _ in CLASSES]
    am = [[] for _ in CLASSES]
    for it in items:
        k = (it.reel, it.axis, it.tshot)
        if it.label == "null":
            nx[it.cls].append(it.T), ng[it.cls].append(k), nm[it.cls].append(it.master)
        else:
            ax[it.cls].append(it.T), ag[it.cls].append(k), am[it.cls].append(it.master)
    grid = make_grid(max([it.T for it in items] or [1.0]))
    tabs = {"null": [], "alt": []}
    diag = []
    for c in range(4):
        n_, a_, d = fit_class(nx[c], ax[c], ng[c], ag[c], nm[c], am[c], c, policy, grid)
        tabs["null"].append(n_)
        tabs["alt"].append(a_)
        diag.append(d)
    return tabs, diag


# ----------------------------------------------------------------------------------------------------------
# Simulation of ChainEvidence (Add / Total / FindSlot / Own / Summary prior, the RECENT WINDOW of N3, the HOLD forget and the
# first-cut clear, the minimum shot dwell of E2), both axes jointly, per truth segment. A line-by-line port of ChainEvidence.h:
# times are SECONDS from the dump's time_s, the window comes from the MEASURED mean gap between the axis's evaluations.
# ----------------------------------------------------------------------------------------------------------
class _Slot(object):
    """One hypothesis (ChainEvidence::Hypothesis): the closed shots inside the window ([time, capped value]) and the open shot."""
    __slots__ = ("n", "cls", "closed", "shot_sum", "shot_time", "shot_id", "shots", "evals")

    def __init__(self, n, cls):
        self.n, self.cls = n, cls
        self.closed = []
        self.shot_sum, self.shot_time, self.shot_id, self.shots, self.evals = 0.0, None, None, 0, 0


class _Axis(object):
    """ChainEvidence::AxisState (plus the reading's counters and the rising-edge memory of the simulation)."""
    def __init__(self):
        self.slots, self.inf_evals, self.shots, self.last_shot, self.N = [], 0, 0, None, 0
        self.prev = (False, 0)
        self.last_time, self.interval = None, 0.0          # newest evaluation time (s), running mean gap (s; 0 = not measured)
        self.shot_key, self.shot_start, self.shot_met, self.pending = None, 0.0, False, []   # E2: the dwell of the current shot


def _total(s, cap):
    t = clampd(s.shot_sum, -cap, cap)
    for c in s.closed:
        t += c[1]
    return t


def _add(s, inc, shot, time, cap):
    if s.shots == 0 or s.shot_id != shot:
        if s.shots > 0:
            s.closed.append([s.shot_time, clampd(s.shot_sum, -cap, cap)])
        s.shot_sum = 0.0
        s.shot_id = shot
        s.shots += 1
    s.shot_sum += inc
    s.shot_time = time
    s.evals += 1


def _find_slot(st, it):
    near, near_d = None, 0
    for s in st.slots:
        d = abs(s.n - it.n)
        if d > 1:
            continue
        if near is None or d < near_d or (d == near_d and s.n < near.n):
            near, near_d = s, d
    if near is not None:
        return near
    h = _Slot(it.n, it.cls)
    if len(st.slots) < MAX_HYPOTHESES:
        st.slots.append(h)
        return h
    w = 0
    for i in range(1, len(st.slots)):
        a, b = _total(st.slots[i], 1e300), _total(st.slots[w], 1e300)
        if a < b or (a == b and st.slots[i].n > st.slots[w].n):
            w = i
    st.slots[w] = h
    return h


def _window_sec(ax, c1):
    """ChainEvidence::WindowUs: windowMargin x (c1 / windowYieldNats) measured intervals, clamped; 0 = not measured yet."""
    if not ax.interval > 0.0:
        return 0.0
    return clampd(WINDOW_MARGIN * (c1 / WINDOW_YIELD_NATS) * ax.interval, WINDOW_MIN_SEC, WINDOW_MAX_SEC)


def _expire(s, cutoff):
    k = 0
    while k < len(s.closed) and s.closed[k][0] is not None and s.closed[k][0] < cutoff:
        k += 1
    if k:
        del s.closed[:k]
    if s.shots > 0 and s.shot_time is not None and s.shot_time < cutoff:
        if not s.closed:
            s.shots = 0               # no live evidence left: the hypothesis is dropped by the caller
        else:
            s.shot_sum, s.shot_time = 0.0, None


def _note_time(ax, t, c1):
    """ChainEvidence::NoteTime: the running mean of the gaps (rateSmoothing 0.25), then the expiry of old closed shots."""
    if ax.last_time is not None and t > ax.last_time:
        gap = t - ax.last_time
        ax.interval = (1.0 - RATE_SMOOTHING) * ax.interval + RATE_SMOOTHING * gap if ax.interval > 0.0 else gap
    if ax.last_time is None or t > ax.last_time:
        ax.last_time = t
    w = _window_sec(ax, c1)
    if not w > 0.0:
        return
    cutoff = ax.last_time - w
    keep = []
    for s in ax.slots:
        _expire(s, cutoff)
        if s.closed or s.shots > 0:
            keep.append(s)
    ax.slots = keep


def _flush(ax, cap):
    """E2: the held evidence of a shot that has now lasted the dwell is credited, in order and with its own times."""
    pend, ax.pending = ax.pending, []
    for it, inc, cut, tm in pend:
        slot = _find_slot(ax, it)
        slot.n, slot.cls = it.n, it.cls
        _add(slot, inc, cut, tm, cap)


def _track_shot(ax, cut, t, cap, dwell):
    """E2 (ChainEvidence::TrackShot): a shot's evidence counts once the shot has lasted `dwell` source seconds. A shot that ends
    (the next shot's first evaluation) after at least the dwell is credited at that evaluation; a shorter one adds nothing."""
    if not dwell > 0.0:
        return
    if ax.shot_key is None or ax.shot_key != cut:
        if ax.shot_key is not None and not ax.shot_met and t - ax.shot_start >= dwell:
            ax.shot_met = True
            _flush(ax, cap)
        ax.pending = []
        ax.shot_key, ax.shot_start, ax.shot_met = cut, t, False
    elif not ax.shot_met and t - ax.shot_start >= dwell:
        ax.shot_met = True
        _flush(ax, cap)


def _forget(ax):
    """ChainEvidence::ForgetAccumulation (a HOLD evaluation): the rate measurement, the axis length and the shot tracking stay."""
    ax.slots, ax.inf_evals, ax.shots, ax.last_shot, ax.pending = [], 0, 0, None, []


def simulate_segment(seg, items_idx, tables, c1, cap_fraction, reset_on_candidate, policy=None):
    """Replays one truth segment with all its axes together (the two-axis agreement prior included), as ChainEvidence does it:
    the recent window (seconds, from the measured evaluation gap), a HOLD evaluation forgetting its axis and the first cut after
    a HOLD clearing both, and (policy.dwell_sec > 0) the minimum shot dwell of E2. seg["evals"] entries are
    (seq, {axis: (time, cut_id[, hold])}). Events: (axis, ordinal of the evaluation in the segment, time, n, cls, correct) at each
    rising edge of hasBest && llr + prior >= c1 (with >= 4 informative evaluations over >= 2 shots), per axis."""
    pol = policy or Policy()
    cap = cap_fraction * c1
    axes = sorted(seg["truth"])
    st = {a: _Axis() for a in axes}
    events = []
    hold_since_cut, prev_cut = False, None
    for ordinal, (seq, per_axis) in enumerate(seg["evals"], 1):
        cut_now = next((per_axis[a][1] for a in axes if a in per_axis), None)
        if pol.sim_hold and prev_cut is not None and cut_now is not None and cut_now != prev_cut and hold_since_cut:
            st = {a: _Axis() for a in axes}      # ChainEvidence::NoteCut -> Reset(): both axes, the rate measurement included
            hold_since_cut = False
        if cut_now is not None:
            prev_cut = cut_now
        for a in axes:
            if a not in per_axis:
                continue
            v = per_axis[a]
            tm, cut = v[0], v[1]
            hold = pol.sim_hold and len(v) > 2 and v[2]
            its = items_idx.get((seg["reel"], a, seq), ())
            s = st[a]
            if its and s.N and s.N != its[0].N:
                st[a] = s = _Axis()
            if its:
                s.N = its[0].N
            if pol.sim_window:
                _note_time(s, tm, c1)
            _track_shot(s, cut, tm, cap, pol.dwell_sec)
            if hold:
                _forget(s)
                hold_since_cut = True
                continue
            for it in its:
                inc = increment(tables, it.cls, it.T)
                if pol.dwell_sec > 0.0 and not s.shot_met:
                    s.pending.append((it, inc, cut, tm))
                    continue
                slot = _find_slot(s, it)
                slot.n, slot.cls = it.n, it.cls
                _add(slot, inc, cut, tm, cap)
            if its:
                s.inf_evals += 1
                if s.shots == 0 or s.last_shot != cut:
                    s.shots += 1
                    s.last_shot = cut
        own = {}
        for a in axes:
            best, bv = None, 0.0
            for sl in st[a].slots:
                v = _total(sl, cap)
                if best is None or v > bv or (v == bv and sl.n < best.n):
                    best, bv = sl, v
            own[a] = (best, bv)
        fired = []
        for a in axes:
            best, bv = own[a]
            s = st[a]
            if best is None:
                s.prev = (False, 0)
                continue
            prior = 0.0
            for o in axes:
                if o != a and own[o][0] is not None and bv > 0.0 and own[o][1] > 0.0 and s.N and st[o].N:
                    ra, ro = best.n / float(s.N), own[o][0].n / float(st[o].N)
                    prior = AGREE_BONUS if abs(ra - ro) <= AGREE_TOL * ra else -AGREE_BONUS
            above = bv + prior >= c1 and s.inf_evals >= MIN_EVALS_CANDIDATE and s.shots >= 2
            was, was_n = s.prev
            s.prev = (above, best.n)
            if above and (not was or abs(was_n - best.n) > 1):
                t, t1 = seg["truth"][a], seg["truth1"].get(a, 0)
                fired.append((a, ordinal, per_axis[a][0] if a in per_axis else 0.0, best.n, best.cls,
                              t > 0 and is_alt(best.n, t, t1)))
        events.extend(fired)
        if fired and reset_on_candidate:
            st = {a: _Axis() for a in axes}
    return events


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


def split_masters(masters, folds=2, seed="sid-cal"):
    """Deterministic by-master split (a master = one footage id: all its reels, native or upscaled, share a fold)."""
    order = sorted(set(masters), key=lambda t: hashlib.sha1((seed + "|" + t).encode("utf-8")).hexdigest())
    return {t: i % folds for i, t in enumerate(order)}


def seg_hours(seg):
    ts = [min(v[0] for v in e.values()) for _, e in seg["evals"]]
    if len(ts) < 2:
        return 0.0
    gaps = sorted(b - a for a, b in zip(ts, ts[1:]) if b > a)
    step = gaps[len(gaps) // 2] if gaps else 0.0
    return (max(ts) - min(ts) + step) / 3600.0


def poisson_upper95(k):
    return POISSON95[k] if k < len(POISSON95) else k + 2.0 * math.sqrt(k) + 2.0


def evaluate_heldout(test_items, test_segs, tables, ref_tables, c1, policy=None):
    res = {}
    pos = [[] for _ in range(4)]
    neg = [[] for _ in range(4)]
    for it in test_items:
        (pos if it.label == "alt" else neg)[it.cls].append(llr(tables["null"][it.cls], tables["alt"][it.cls], it.T))
    res["pos"], res["neg"] = pos, neg
    idx = {}
    for it in test_items:
        idx.setdefault((it.reel, it.axis, it.seq), []).append(it)
    fa = {}
    for name, tabs in (("cal", tables), ("ph", ref_tables)):
        for cf in (SHOT_CAP_FRACTION, 1.0):
            v = {"events": 0, "hours": 0.0, "by_class": [0] * 4}
            for seg in test_segs:
                if any(t > 0 for t in seg["truth"].values()):
                    continue
                v["hours"] += seg_hours(seg)
                for e in simulate_segment(seg, idx, tabs, c1, cf, True, policy):
                    v["events"] += 1
                    v["by_class"][e[4]] += 1
            fa[(name, cf)] = v
    res["fa"] = fa
    lat = {8: [], 10: []}
    reached = {8: [0, 0], 10: [0, 0]}
    wrong_first = {8: 0, 10: 0}
    wrong_on_alt = 0
    for seg in test_segs:
        if not any(t > 0 for t in seg["truth"].values()):
            continue
        evs = simulate_segment(seg, idx, tables, c1, SHOT_CAP_FRACTION, False, policy)
        for a, t in sorted(seg["truth"].items()):
            if t <= 0:
                continue
            b = 8 if seg["bits"][a] <= 8 else 10
            mine = [e for e in evs if e[0] == a]
            wrong_on_alt += sum(1 for e in mine if not e[5])
            hit = [e for e in mine if e[5]]
            reached[b][1] += 1
            if mine and not mine[0][5]:
                wrong_first[b] += 1
            if hit:
                reached[b][0] += 1
                lat[b].append(hit[0][1])
            else:
                lat[b].append(float("inf"))
    res["lat"], res["reached"], res["wrong_first"], res["wrong_on_alt"] = lat, reached, wrong_first, wrong_on_alt
    return res


def cross_validate(items, segs, all_masters, policy, c1, seed):
    fold_of = split_masters(all_masters, 2, seed)
    if len(set(fold_of.values())) < 2 or len(fold_of) < 4:
        return None, fold_of
    pooled = {"pos": [[] for _ in range(4)], "neg": [[] for _ in range(4)], "lat": {8: [], 10: []},
              "reached": {8: [0, 0], 10: [0, 0]}, "wrong_first": {8: 0, 10: 0}, "wrong_on_alt": 0, "fa": {}, "folds": []}
    for f in (0, 1):
        train = [it for it in items if fold_of[it.master] != f]
        test = [it for it in items if fold_of[it.master] == f]
        tsegs = [s for s in segs if fold_of[s["master"]] == f]
        tabs, diag = fit_tables(train, policy)
        r = evaluate_heldout(test, tsegs, tabs, {"null": PLACEHOLDER["null"], "alt": PLACEHOLDER["alt"]}, c1, policy)
        for c in range(4):
            pooled["pos"][c] += r["pos"][c]
            pooled["neg"][c] += r["neg"][c]
        for b in (8, 10):
            pooled["lat"][b] += r["lat"][b]
            for k in (0, 1):
                pooled["reached"][b][k] += r["reached"][b][k]
            pooled["wrong_first"][b] += r["wrong_first"][b]
        pooled["wrong_on_alt"] += r["wrong_on_alt"]
        for k, v in r["fa"].items():
            p = pooled["fa"].setdefault(k, {"events": 0, "hours": 0.0, "by_class": [0] * 4})
            p["events"] += v["events"]
            p["hours"] += v["hours"]
            for c in range(4):
                p["by_class"][c] += v["by_class"][c]
        pooled["folds"].append((f, sorted(t for t in all_masters if fold_of[t] == f), diag))
    return pooled, fold_of


# ----------------------------------------------------------------------------------------------------------
# Outputs
# ----------------------------------------------------------------------------------------------------------
def render_header(tabs, diag, prov):
    any_fit = any(d["status"] in ("fitted", "thin") for d in diag)
    P = prov["policy"]
    L = ["// Copyright(C) 2026 VP-V contributors", "// SPDX-License-Identifier: GPL-3.0-only", "#pragma once", "",
         "// GENERATED by tools/eval/source_id/calibrate_nulls.py. DO NOT EDIT: rerun the calibration instead.",
         "// Provenance: dump set sha256 %s" % prov["hash"],
         "//             date %s, %d files, %d rows, %d informative clusters from %d masters (n_eff %d shots), cv %s" % (
             prov["date"], prov["files"], prov["rows"], prov["items"], prov["masters"], prov["n_eff"], prov["cv"]),
         "//             policy: min n_eff null/alt %d/%d, full %d/%d, masters >= %d, null widen %.2f, require_power %s" % (
             P.min_null_hard, P.min_alt_hard, P.full_null, P.full_alt, P.min_masters, P.null_widen,
             prov["require_power"])]
    for c in range(4):
        d = diag[c]
        L.append("//   %-12s null %5d rows (n_eff %d, %d masters), alt %5d (n_eff %d, %d masters): %s%s" % (
            CLASS_NAMES[c], d["n_null"], d["eff_null"], d["m_null"], d["n_alt"], d["eff_alt"], d["m_alt"], d["status"],
            (" - " + d["why"]) if d["why"] else ""))
    L += ["#include \"ChainNullTables.h\"", "", "namespace SourceIdentity", "{", "\tnamespace Chain", "\t{",
          "\t\t// Drop-in for NullTables::Placeholder(): ChainEvidence(settings, GeneratedNullTables()).",
          "\t\tinline NullTables GeneratedNullTables()", "\t\t{", "\t\t\tNullTables t;",
          "\t\t\tt.placeholder = %s;" % ("false" if any_fit else "true")]
    for c in range(4):
        L.append("\t\t\tt.nul[%s].mean = %.6f; t.nul[%s].sd = %.6f;" % (CLASS_CPP[c], tabs["null"][c][0], CLASS_CPP[c],
                                                                       tabs["null"][c][1]))
        L.append("\t\t\tt.alt[%s].mean = %.6f; t.alt[%s].sd = %.6f;" % (CLASS_CPP[c], tabs["alt"][c][0], CLASS_CPP[c],
                                                                       tabs["alt"][c][1]))
    L += ["\t\t\treturn t;", "\t\t}", "\t}", "}"]
    return "\n".join(L) + "\n"


def median_inf(v):
    """Median of a list that may hold inf (never reached) without the nan of inf - inf interpolation: inf when the middle is inf."""
    s = sorted(v)
    if not s:
        return float("nan")
    m = len(s) // 2
    lo, hi = (s[m - 1], s[m]) if len(s) % 2 == 0 else (s[m], s[m])
    return float("inf") if hi == float("inf") else (lo + hi) / 2.0


def _med_lat(v):
    if not v:
        return "n/a"
    m = median_inf(v)
    return "inf" if m == float("inf") else "%.0f" % m


def sim_desc(policy):
    p = policy or Policy()
    w = "recent window %.1f x (c1 / %.1f) measured intervals, %.0f-%.0f s" % (WINDOW_MARGIN, WINDOW_YIELD_NATS, WINDOW_MIN_SEC, WINDOW_MAX_SEC) \
        if p.sim_window else "NO recent window"
    return "%s; HOLD %s; shot dwell %s" % (w, "forgets (proxy from the dump)" if p.sim_hold else "not simulated",
                                           ("%.1f s" % p.dwell_sec) if p.dwell_sec > 0 else "off")


def render_report(tabs, diag, prov, cv, fold_of, c1, stats, policy=None):
    L = ["# SID calibration report", ""]
    L.append("Dump set %s, %s, %d files, %d rows (%d bad), %d informative clusters, %d masters, n_eff %d shots. c1 = %.2f nats,"
             " per-shot cap c1/2 = %.2f, shots = the estimator's cut_id." % (
                 prov["hash"][:12], prov["date"], prov["files"], prov["rows"], prov["bad"], prov["items"],
                 prov["masters"], prov["n_eff"], c1, c1 / 2.0))
    L.append("Exact rows %d: dropped %d no truth, %d not informative, %d gate, %d power%s, %d double-scaled off-latent;"
             " %d class mismatches vs dump. T_ev is the statistic used." % (
                 stats["exact"], stats["no_truth"], stats["not_informative"], stats["gate_fail"], stats["power_fail"],
                 "" if prov["require_power"] else " (not applied)", stats["double_drop"], stats["class_mismatch"]))
    L += ["", "## Fitted tables (stage exact; n_eff = distinct reel x axis x shot groups)",
          "| class | null rows/n_eff/masters | alt rows/n_eff/masters | null mean, sd | alt mean, sd | max null T | status |",
          "|---|---|---|---|---|---|---|"]
    for c in range(4):
        d = diag[c]
        mx = d["max_null_T"]
        L.append("| %s | %d/%d/%d | %d/%d/%d | %.3f, %.3f | %.3f, %.3f | %s | %s%s |" % (
            CLASS_NAMES[c], d["n_null"], d["eff_null"], d["m_null"], d["n_alt"], d["eff_alt"], d["m_alt"],
            tabs["null"][c][0], tabs["null"][c][1], tabs["alt"][c][0], tabs["alt"][c][1],
            "n/a" if mx != mx else "%.3f" % mx, d["status"], (": " + d["why"][:50]) if d["why"] else ""))
    L.append("")
    if cv is None:
        L.append("## Cross-validation: NOT POSSIBLE (fewer than 4 masters or one fold empty). Tables are UNVALIDATED.")
    else:
        L.append("## Held-out validation (2-fold CV by master, %d masters; each fold fitted on the other half)" % len(fold_of))
        L += ["| class | held-out null/alt | ROC AUC (LLR) | held-out null LLR P99 / P99.9 (c1 %.1f) |" % c1, "|---|---|---|---|"]
        for c in range(4):
            a = auc(cv["pos"][c], cv["neg"][c])
            nl = cv["neg"][c]
            tail = "n/a" if not nl else "%.2f / %.2f" % (quantile(nl, 0.99), quantile(nl, 0.999))
            L.append("| %s | %d/%d | %s | %s |" % (CLASS_NAMES[c], len(nl), len(cv["pos"][c]),
                                                  "n/a" if a != a else "%.3f" % a, tail))
        L += ["", "False c1 crossings (false confident readings) on native segments, simulated as ChainEvidence runs: %s;"
              " both axes jointly with the agreement prior; reset after each; held-out only. HEADLINE = the live setting"
              " (calibrated tables, per-shot cap c1/2); the other rows are extras:" % sim_desc(policy),
              "| tables, shot cap | entries | hours H | per hour | upper 95% (3/H if 0) | by class k8/k16/rat/gen |",
              "|---|---|---|---|---|---|"]
        order = [("cal", SHOT_CAP_FRACTION), ("cal", 1.0), ("ph", SHOT_CAP_FRACTION), ("ph", 1.0)]
        for key in order:
            v = cv["fa"].get(key)
            if v is None:
                continue
            name, cf = key
            H = v["hours"]
            rate = "n/a" if H <= 0 else "%.3f" % (v["events"] / H)
            ub = "n/a" if H <= 0 else "%.3f /h" % (poisson_upper95(v["events"]) / H)
            L.append("| %s%s, c1*%.1f | %d | %.2f | %s | %s | %s |" % (
                "**HEADLINE** " if key == order[0] else "", "calibrated" if name == "cal" else "placeholder", cf, v["events"], H, rate, ub,
                "/".join(str(x) for x in v["by_class"])))
        L.append("Budget <= 1/h. Zero events in H hours only bounds the rate at 3/H (95%%): that, not 0, is the claim. Wrong-n entries"
                 " on alt segments (calibrated): %d." % cv["wrong_on_alt"])
        L += ["", "Evaluations to c1 on alt segments (per segment and axis, from segment start; inf = never reached):",
              "| bit depth | segment-axes | reached | median evals | wrong n first |", "|---|---|---|---|---|"]
        for b in (8, 10):
            r = cv["reached"][b]
            L.append("| %d | %d | %d | %s | %d |" % (b, r[1], r[0], _med_lat(cv["lat"][b]), cv["wrong_first"][b]))
    L += ["", "Not simulated: the native hypothesis (cannot raise a candidate). HOLD is simulated from a dump proxy (%d of %d axis"
          " evaluations). Power rule %s." % (prov.get("holds", 0), prov.get("axis_evals", 0),
        "applied to every row" if prov["require_power"] else "not applied to candidate rows (the estimator applies it to native only)")]
    return "\n".join(L) + "\n"


def apply_args(policy, args):
    """The simulation switches (and, later, the fit switches) from the command line; absent attributes keep the defaults."""
    if getattr(args, "no_window", False):
        policy.sim_window = False
    if getattr(args, "no_hold", False):
        policy.sim_hold = False
    if getattr(args, "dwell_sec", None) is not None:
        policy.dwell_sec = float(args.dwell_sec)
    return policy


def run(args):
    policy = apply_args(Policy(), args)
    rows, info = load_dumps(args.dump)
    items, stats = build_samples(rows, args.require_power, args.null_side)
    segs = build_segments(rows)
    masters = sorted(set(r.master for r in rows))
    c1 = thresholds()
    tabs, diag = fit_tables(items, policy)
    cv, fold_of = cross_validate(items, segs, masters, policy, c1, args.seed)
    prov = {"hash": info["hash"], "files": info["files"], "rows": len(rows), "bad": info["bad"], "items": len(items),
            "masters": len(set(it.master for it in items)), "n_eff": len(set((it.reel, it.axis, it.tshot) for it in items)),
            "date": args.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
            "cv": "2-fold by master" if cv else "NOT POSSIBLE (unvalidated)", "policy": policy,
            "require_power": args.require_power,
            "holds": sum(1 for s in segs for _, e in s["evals"] for v in e.values() if len(v) > 2 and v[2]),
            "axis_evals": sum(len(e) for s in segs for _, e in s["evals"])}
    os.makedirs(args.out, exist_ok=True)
    hdr = render_header(tabs, diag, prov)
    rep = render_report(tabs, diag, prov, cv, fold_of, c1, stats, policy)
    if len(rep.splitlines()) > 60:
        raise SystemExit("report exceeds 60 lines")
    with open(os.path.join(args.out, "NullTables.generated.h"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(hdr)
    with open(os.path.join(args.out, "calibration-report.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(rep)
    return hdr, rep, tabs, diag, cv, fold_of, items, segs


# ----------------------------------------------------------------------------------------------------------
# Selftest: synthetic v3 dumps (27 columns, quoted ids, title unique per reel, master shared, multi-segment reels,
# has_truth=0 / informative=0 / double-scaled rows, a sparse class)
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
    out = {0: [], 1: [], 2: [], 3: []}
    for n in range(int(N * 0.2), int(N * 0.985)):
        out[classify_latent(n, N)].append(n)
    return out


def make_synthetic(path, seed=7, masters=12, evals=100, v2=False):
    """Writes v3 CSV dumps (or a v2 one: 21 columns). True null law per class; true alt law -0.30 (8-bit) /
    -0.45 (10-bit), sd 0.08. K16 is SPARSE. `T` (diagnostic) is half of T_ev, to prove T_ev is what is read."""
    rng = _Lcg(seed)
    N = 2160
    sizes = _class_sizes(N)
    true_null = {0: (-0.08, 0.04), 1: (-0.07, 0.04), 2: (-0.03, 0.03), 3: (-0.02, 0.03)}
    # (reel_id, master, bits, [segments: (first_seq, last_seq, truth_n, truth_n1, has_truth)])
    plan = []
    for m in range(masters):
        mid = "m%02d" % m
        b = 8 if m % 2 == 0 else 10
        plan.append(("%s_nat" % mid, mid, b, [(0, evals - 1, 0, 0, 1)]))
        plan.append(("%s_half" % mid, mid, b, [(0, evals - 1, 1080, 0, 1)]))
        plan.append(("%s_two3" % mid, mid, 10, [(0, evals - 1, 1440, 0, 1)]))
        plan.append(("%s_gen" % mid, mid, 8, [(0, evals - 1, 1317, 0, 1)]))
    h = evals // 2
    plan.append(("m00_multi", "m00", 8, [(0, h - 1, 0, 0, 1), (h, evals - 1, 1080, 0, 1),
                                          (evals, evals + 19, 0, 0, 0)]))          # last segment: no truth
    plan.append(("m01_double", "m01", 8, [(0, evals - 1, 1080, 720, 1)]))
    cols = REQUIRED_V2 if v2 else REQUIRED
    stats = {"junk_notruth": 0, "junk_noninf": 0, "double_rows": 0}
    for reel, master, bits, segs in plan:
        with open(os.path.join(path, reel + ".csv"), "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh, quoting=csv.QUOTE_NONNUMERIC)
            w.writerow(cols)
            for first, last, truth_n, truth_n1, has_truth in segs:
                for seq in range(first, last + 1):
                    for axis in ("x", "y"):
                        cands = []
                        if truth_n:
                            cands += [truth_n - 1, truth_n, truth_n + 1]
                        if truth_n1:
                            cands += [truth_n1 - 1, truth_n1, truth_n1 + 1]
                        for _ in range(3):
                            pool = 3 if rng.u() < 0.55 else (2 if rng.u() < 0.5 else 0)
                            if rng.u() < 0.0002:
                                pool = 1
                            lst = sizes[pool]
                            n = lst[int(rng.u() * len(lst)) % len(lst)]
                            if truth_n and abs(n - truth_n) <= 1:
                                continue
                            cands.append(n)
                        for n in cands:
                            c = classify_latent(n, N)
                            if truth_n and (abs(n - truth_n) <= 1 or (truth_n1 and abs(n - truth_n1) <= 1)):
                                tev = (-0.30 if bits == 8 else -0.45) + 0.08 * rng.gauss()
                                if truth_n1 and abs(n - truth_n1) <= 1:
                                    stats["double_rows"] += 1
                            else:
                                mu, sd = true_null[c]
                                tev = mu + sd * rng.gauss()
                            passed, inf = 28, 1
                            if not has_truth:
                                tev, stats["junk_notruth"] = -2.0, stats["junk_notruth"] + 1
                            elif rng.u() < 0.08:
                                tev, inf = -2.0, 0       # not informative (HOLD / masked / gate): must never count
                                stats["junk_noninf"] += 1
                                if rng.u() < 0.5:
                                    passed = 4
                            for stage in ("coarse", "exact"):
                                row = [reel, "description of " + reel, seq, float(seq), axis, N, n, CLASS_NAMES[c],
                                       "nan" if stage == "coarse" else 0.5 * tev, stage, "bilinear", 0.1, passed, 32, 1,
                                       1.155, truth_n, bits, "bilinear", 0, seq // 10]
                                if not v2:
                                    row += ["" if stage == "coarse" else tev, 0 if stage == "coarse" else inf, master,
                                            has_truth, truth_n1, seq // 5]
                                w.writerow(row)
    return stats


def selftest():
    import tempfile
    fails = []

    def check(cond, msg):
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            fails.append(msg)

    check(classify_latent(1080, 2160) == 0, "ClassifyLatent: 1080/2160 -> K8")
    check(classify_latent(1440, 2160) == 2, "ClassifyLatent: 1440/2160 (2/3) -> Rational (==2)")
    check(classify_latent(1317, 2160) == 3, "ClassifyLatent: 0.61 N -> generic")
    check(len(REQUIRED) == 27, "dump v3 has 27 required columns")
    check(abs(thresholds() - 14.87) < 0.02, "c1 = %.3f (README ~14.9)" % thresholds())
    check(abs(auc([1, 2, 3], [0, 0.5, 1.5]) - 8.0 / 9.0) < 1e-9, "AUC rank formula")
    check(poisson_upper95(0) == 3.0 and poisson_upper95(2) > 6.0 and poisson_upper95(30) > 30, "Poisson upper bound (3/H at 0)")

    # --- tail-matched null sd: a null with a deep dip tail gets a spread covering it
    rng = _Lcg(3)
    heavy = [(-0.06 if rng.u() < 0.1 else 0.0) + 0.02 * rng.gauss() for _ in range(2000)]
    plain = [0.02 * rng.gauss() for _ in range(2000)]
    med = median(heavy)
    check(null_sd(heavy, 0.005) >= (med - quantile(heavy, 0.0062)) / 2.5 - 1e-12 and null_sd(heavy, 0.005) > 1.4 * null_sd(plain, 0.005),
          "null sd %.4f matches the dip tail (plain %.4f)" % (null_sd(heavy, 0.005), null_sd(plain, 0.005)))

    # --- two-axis agreement prior and n_eff are simulated: handcrafted segment
    tabs = {"null": PLACEHOLDER["null"], "alt": PLACEHOLDER["alt"]}
    lo, hi = -2.0, 0.0
    for _ in range(60):          # T whose generic increment is 1.0 nat
        mid = 0.5 * (lo + hi)
        if llr(tabs["null"][3], tabs["alt"][3], mid) > 1.0:
            lo = mid
        else:
            hi = mid
    t1nat = 0.5 * (lo + hi)
    check(abs(increment(tabs, 3, t1nat) - 1.0) < 1e-3, "helper T for 1 nat found (%.4f)" % t1nat)

    def seg_for(axes, evs=40):
        return {"reel": "r", "master": "m", "truth": {a: 0 for a in axes}, "truth1": {}, "bits": {a: 8 for a in axes},
                "evals": [(i, {a: (float(i), i // 6) for a in axes}) for i in range(evs)]}

    def mk(axis, i, n):
        it = Item()
        it.reel, it.master, it.seq, it.time, it.axis, it.N, it.n, it.T, it.cls = "r", "m", i, float(i), axis, 2160, n, t1nat, 3
        it.truth_n, it.truth_n1, it.bits, it.label, it.tshot, it.cut = 0, 0, 8, "null", 0, i // 6
        return it
    c1 = thresholds()
    idx1 = {("r", "x", i): [mk("x", i, 1317)] for i in range(40)}
    e1 = simulate_segment(seg_for(["x"]), idx1, tabs, c1, 0.5, False)
    idx2 = dict(idx1)
    idx2.update({("r", "y", i): [mk("y", i, 1317)] for i in range(40)})
    e2 = simulate_segment(seg_for(["x", "y"]), idx2, tabs, c1, 0.5, False)
    idx3 = dict(idx1)
    idx3.update({("r", "y", i): [mk("y", i, 1700)] for i in range(40)})
    e3 = simulate_segment(seg_for(["x", "y"]), idx3, tabs, c1, 0.5, False)
    ox = [e[1] for e in e1 if e[0] == "x"]
    jx = [e[1] for e in e2 if e[0] == "x"]
    dx = [e[1] for e in e3 if e[0] == "x"]
    check(ox and jx and dx, "events produced (single %s, joint %s, disagree %s)" % (ox[:1], jx[:1], dx[:1]))
    check(ox and jx and jx[0] < ox[0], "agreement prior (+2) reaches c1 earlier: joint %s < single %s evals" % (jx[:1], ox[:1]))
    check(ox and dx and dx[0] > ox[0], "disagreeing axes (-2) are slower: %s > %s" % (dx[:1], ox[:1]))
    # per-shot cap: with 1-nat increments a 6-eval cut accumulates 6 < c1/2, so c1 needs several shots
    check(ox and ox[0] >= 15, "single-axis c1 needs >= 15 one-nat evaluations (%s)" % ox[:1])

    # --- the simulation is the estimator's: recent window (seconds), HOLD forget + first-cut clear, E2 dwell (ChainEvidence.h)
    def seg_x(n_evals, cut_of, holds=()):
        return {"reel": "r", "master": "m", "truth": {"x": 0}, "truth1": {}, "bits": {"x": 8},
                "evals": [(i, {"x": (float(i), cut_of(i), i in holds)}) for i in range(n_evals)]}

    def idx_x(ids, T=-1.0):          # a decisive dip (increment clamps at +5 nats) at those evaluations
        out = {}
        for i in ids:
            it = mk("x", i, 1317)
            it.T = T
            out[("r", "x", i)] = [it]
        return out
    pol0 = Policy()
    pol_nowin = Policy()
    pol_nowin.sim_window = False
    pol_nohold = Policy()
    pol_nohold.sim_hold = False
    pol_dwell = Policy()
    pol_dwell.dwell_sec = 3.0
    cut2 = lambda i: 0 if i < 2 else (1 if i < 22 else 2)
    ids = [0, 1, 22, 23]
    wl = simulate_segment(seg_x(24, cut2), idx_x(ids), tabs, c1, 0.5, False, pol0)
    wn = simulate_segment(seg_x(24, cut2), idx_x(ids), tabs, c1, 0.5, False, pol_nowin)
    check(not wl and len(wn) == 1, "recent window: a shot 22 s old (window %.1f s at a 1 s gap) has expired; without the window it counts"
          % _window_sec(type("A", (), {"interval": 1.0})(), c1))
    cut_b = lambda i: 0 if i < 2 else 1
    ws = simulate_segment(seg_x(12, cut_b), idx_x([0, 1, 10, 11]), tabs, c1, 0.5, False, pol0)
    check(len(ws) == 1 and ws[0][1] == 12, "recent window: a shot 10 s old is still inside it (event at evaluation %s)" % (ws[0][1] if ws else None))
    ct = lambda i: 0 if i < 2 else (1 if i < 5 else 2)
    hs = simulate_segment(seg_x(7, ct, holds=(2,)), idx_x([0, 1, 3, 4, 5, 6]), tabs, c1, 0.5, False, pol0)
    hn = simulate_segment(seg_x(7, ct, holds=(2,)), idx_x([0, 1, 3, 4, 5, 6]), tabs, c1, 0.5, False, pol_nohold)
    check(not hs and len(hn) == 1, "HOLD forgets its axis and the first cut after it clears both (no event); without the HOLD model the evidence adds up")
    rapid = lambda i: i
    check(not simulate_segment(seg_x(40, rapid), idx_x(range(40)), tabs, c1, 0.5, False, pol_dwell)
          and len(simulate_segment(seg_x(40, rapid), idx_x(range(40)), tabs, c1, 0.5, False, pol_nowin)) >= 1,
          "E2: rapid cuts every 1 s add no evidence with the 3 s dwell (they do without it)")
    five = lambda i: i // 5
    e_off = simulate_segment(seg_x(40, five), idx_x(range(40)), tabs, c1, 0.5, False, pol0)
    e_on = simulate_segment(seg_x(40, five), idx_x(range(40)), tabs, c1, 0.5, False, pol_dwell)
    check(e_off and e_on and e_off[0][1] <= e_on[0][1] <= e_off[0][1] + 3,
          "E2: normal 5 s shots reach c1 within one dwell of the no-dwell time (%s vs %s evaluations)" % (e_on[0][1] if e_on else None, e_off[0][1] if e_off else None))
    one_long = simulate_segment(seg_x(60, lambda i: 0), idx_x(range(60)), tabs, c1, 0.5, False, pol_dwell)
    check(not one_long, "E2: the per-shot cap still holds (one 60 s shot never reaches c1)")

    # --- thin class: shrunk and blended, 0 < blend < 1 case, and sparse/too-few-masters fallbacks
    pol = Policy()
    grid = make_grid(1.0)
    rng = _Lcg(2)
    nx = [-0.05 + 0.08 * rng.gauss() for _ in range(100)]
    axx = [-0.5 + 0.4 * rng.gauss() for _ in range(40)]
    ks = lambda n_: [("r", "x", i) for i in range(n_)]
    ms = lambda n_: ["m%d" % (i % 6) for i in range(n_)]
    tn, ta, dg = fit_class(nx, axx, ks(100), ks(40), ms(100), ms(40), 0, pol, grid)
    check(dg["status"] == "thin" and 0.0 < dg["blend"] < 1.0 and dominated(tn, ta, 0, grid),
          "thin class: partial blend %.1f toward the placeholder, then dominated" % dg["blend"])
    tn1, ta1, dg1 = fit_class(nx[:20], axx, ks(20), ks(40), ms(20), ms(40), 0, pol, grid)
    check(dg1["status"] == "placeholder" and tn1 == PLACEHOLDER["null"][0], "too few effective nulls -> placeholder")
    tn2, ta2, dg2 = fit_class(nx, axx, ks(100), ks(40), ["a"] * 100, ms(40), 0, pol, grid)
    check(dg2["status"] == "placeholder", "one master only -> placeholder")
    one_shot = [("r", "x", 0)] * 100          # 100 rows but ONE shot: n_eff 1
    tn3, ta3, dg3 = fit_class(nx, axx, one_shot, ks(40), ms(100), ms(40), 0, pol, grid)
    check(dg3["status"] == "placeholder" and dg3["eff_null"] == 1, "100 rows of one shot are n_eff 1 -> placeholder")

    # --- v2 dump must be refused
    with tempfile.TemporaryDirectory() as td:
        d2 = os.path.join(td, "v2")
        os.makedirs(d2)
        make_synthetic(d2, masters=2, evals=5, v2=True)
        try:
            load_dumps(d2)
            check(False, "v2 dump refused")
        except SystemExit as e:
            check("dump v2" in str(e), "v2 dump refused: %s" % str(e)[:60])

        dd = os.path.join(td, "dump")
        out = os.path.join(td, "out")
        os.makedirs(dd)
        js = make_synthetic(dd)
        ns = argparse.Namespace(dump=dd, out=out, date="2026-11-07", require_power=False, null_side="both", seed="sid-cal")
        rows, info = load_dumps(dd)
        check(len(rows) > 0 and info["bad"] == 0, "v3 rows load (%d rows, %d bad; quoted ids, 27 columns)" % (len(rows), info["bad"]))
        hdr, rep, tabs, diag, cv, fold_of, items, segs = run(ns)
        _, st = build_samples(rows)
        check(st["no_truth"] > 0 and st["not_informative"] > 0, "has_truth=0 (%d) and informative=0 (%d) exact rows dropped" % (
            st["no_truth"], st["not_informative"]))
        check(min(it.T for it in items) > -1.0, "no junk T_ev (-2.0) reaches the samples (has_truth=0 / informative=0 / gate)")
        check(st["double_drop"] >= 0 and not any(it.label == "null" and it.truth_n1 > 0 and abs(it.n - it.truth_n1) <= 1 for it in items),
              "double-scaled intermediate latent never enters the null (%d rows)" % js["double_rows"])
        check(any(it.label == "alt" and it.truth_n1 > 0 and abs(it.n - it.truth_n1) <= 1 for it in items),
              "double-scaled intermediate latent counts as alt")
        check(any(s["reel"] == "m00_multi" for s in segs) and sum(1 for s in segs if s["reel"] == "m00_multi") == 2,
              "multi-segment reel: native + alt segments, the no-truth tail is not a segment")
        multi_null = [it for it in items if it.reel == "m00_multi" and it.label == "null" and it.seq >= 50]
        multi_alt = [it for it in items if it.reel == "m00_multi" and it.label == "alt"]
        check(multi_alt and all(it.seq >= 50 for it in multi_alt), "multi-segment truth is per row (alt only after the switch)")
        # T_ev is what is used (T was written as half of it)
        d3 = diag[3]
        nm, nsd = tabs["null"][3]
        check(d3["status"] == "fitted" and abs(nm - (-0.02)) < 0.01, "generic null mean %.4f ~ -0.02 (T_ev, not the diluted T)" % nm)
        check(0.03 <= nsd <= 0.03 * 1.25 * 1.4, "generic null sd %.4f within [truth, 1.75 x truth]" % nsd)
        check(diag[2]["status"] in ("fitted", "thin") and tabs["alt"][2][0] < tabs["null"][2][0] - 0.1, "rational class fitted, alt below null")
        # n_eff by shots, not rows
        check(all(d["eff_null"] < d["n_null"] for d in diag if d["n_null"] > 30), "n_eff counts shots, below the row count")
        check(prov_ok(rep), "report states n_eff and masters")
        d16 = diag[1]
        check(d16["n_null"] < 40 and d16["status"] == "placeholder" and tabs["null"][1] == PLACEHOLDER["null"][1]
              and tabs["alt"][1] == PLACEHOLDER["alt"][1], "sparse K16 (%d null rows) keeps the placeholder" % d16["n_null"])
        grid = make_grid(max(it.T for it in items))
        for c in range(4):
            if diag[c]["status"] in ("thin", "placeholder"):
                check(dominated(tabs["null"][c], tabs["alt"][c], c, grid), "class %s never more permissive than placeholder" % CLASSES[c])

        # one fold per master (the title column is unique per reel and must be ignored)
        masters = sorted(set(r.master for r in rows))
        check(len(set(r.reel for r in rows)) > len(masters) and set(fold_of) == set(masters) and set(fold_of.values()) == {0, 1},
              "split is by master (%d masters, %d reels), both folds used" % (len(masters), len(set(r.reel for r in rows))))
        per_master = {}
        for it in items:
            per_master.setdefault(it.master, set()).add(fold_of[it.master])
        check(all(len(v) == 1 for v in per_master.values()), "one fold per master (native and upscaled reels of a footage together)")
        for f, fm, _ in cv["folds"]:
            check(all(fold_of[t] == f for t in fm), "fold %d masters are exactly its own" % f)

        # held-out results
        a = auc(cv["pos"][3], cv["neg"][3])
        check(a > 0.9, "generic held-out AUC %.3f" % a)
        check(auc(cv["pos"][2], cv["neg"][2]) > 0.9, "rational held-out AUC > 0.9")
        fa = cv["fa"][("cal", SHOT_CAP_FRACTION)]
        check(fa["hours"] > 0 and fa["events"] == 0, "no false confident reading on pure-noise natives (%d in %.3f h)" % (fa["events"], fa["hours"]))
        check("3/H" in rep and "upper 95%" in rep and "%.3f /h" % (3.0 / fa["hours"]) in rep, "report prints the 3/H bound (%.1f /h)" % (3.0 / fa["hours"]))
        for b in (8, 10):
            r = cv["reached"][b]
            m = median_inf(cv["lat"][b]) if cv["lat"][b] else float("inf")
            check(r[1] > 0 and r[0] == r[1] and m <= 30, "%d-bit alt segment-axes reach c1 (%d/%d, median %s evals)" % (b, r[0], r[1], "%.0f" % m))
            check(cv["wrong_first"][b] == 0, "%d-bit: no wrong n first" % b)
        check(sum(r_[1] for r_ in cv["reached"].values()) == sum(1 for s in segs for a_, t_ in s["truth"].items() if t_ > 0 and True),
              "every alt segment-axis is a denominator (none vanishes)")

        check(len(rep.splitlines()) <= 60, "report %d lines <= 60" % len(rep.splitlines()))
        check(hdr.startswith("// Copyright(C) 2026 VP-V contributors\n// SPDX-License-Identifier: GPL-3.0-only\n#pragma once"),
              "header: copyright, SPDX, pragma once")
        check("Provenance: dump set sha256 " in hdr and "2026-11-07" in hdr and "masters" in hdr and "n_eff" in hdr,
              "header: provenance (hash, date, masters, n_eff)")
        assigns = re.findall(r"t\.(nul|alt)\[(kClass\w+)\]\.(mean|sd) = (-?\d+\.\d{6});", hdr)
        check(len(assigns) == 16 and len(set((x, y, z) for x, y, z, _ in assigns)) == 16, "header: 16 distinct assignments")
        check(hdr.count("{") == hdr.count("}") and "NullTables t;" in hdr and "return t;" in hdr, "header: braces balanced")
        check(not re.search(r"inline constexpr|if constexpr|std::filesystem|auto \[|_v<", hdr), "header: C++14 only")
        check(all(float(v) > 0 for _, _, z, v in assigns if z == "sd"), "header: all sd positive")
        hdr2 = run(ns)[0]
        check(hdr2 == hdr, "deterministic: second run identical header")
        ns.require_power = True
        check(run(ns)[0].count("t.nul") == 8, "require_power run writes a full table")

    print("SELFTEST " + ("FAILED (%d)" % len(fails) if fails else "PASSED"))
    return 1 if fails else 0


def prov_ok(rep):
    return "n_eff" in rep and "masters" in rep


def main(argv=None):
    ap = argparse.ArgumentParser(description="Calibrate the SID chain NullTables from harness calibration dumps (v3).")
    ap.add_argument("--dump", help="directory of per-reel v3 CSV dumps (VP_SOURCE_ID_CAL_DUMP)")
    ap.add_argument("--out", help="output directory (NullTables.generated.h, calibration-report.md)")
    ap.add_argument("--selftest", action="store_true", help="synthetic self-test, no inputs")
    ap.add_argument("--require-power", action="store_true",
                    help="also drop rows whose power probe did not see its injection (default: the estimator "
                         "applies the power rule to the NATIVE hypothesis only, not to candidate increments)")
    ap.add_argument("--null-side", choices=("both", "below"), default="both",
                    help="null rows on upscaled reels: n more than 1 from truth on both sides (default, the "
                         "spec) or only below the truth (above it a descale is lossless and alt-like)")
    ap.add_argument("--seed", default="sid-cal", help="seed of the deterministic by-master fold split")
    ap.add_argument("--no-window", action="store_true", help="simulate WITHOUT the recent window (the cal1 behaviour; extra row only)")
    ap.add_argument("--no-hold", action="store_true", help="simulate without the HOLD forget / first-cut clear")
    ap.add_argument("--dwell-sec", type=float, default=None,
                    help="E2 minimum shot dwell in source seconds (default: the estimator's kMinShotDwellSec; 0 = without E2)")
    ap.add_argument("--date", default="", help="date for the provenance (default: today, UTC)")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.dump or not a.out:
        ap.error("--dump and --out are required (or --selftest)")
    sys.stdout.write(run(a)[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
