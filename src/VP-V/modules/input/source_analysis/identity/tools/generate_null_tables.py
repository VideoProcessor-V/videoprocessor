# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Stage S3 offline calibration & gate-scoring generator for bit-depth-stratified NullTables.

Pure Python 3 standard library only (deterministic, zero external dependencies).

Inputs:
  - Dump v4 CSV pairs (<tag>.b_evals.csv + <tag>.b_sizes.csv), optionally paired with a
    JSON truth manifest (--truth-manifest), AND/OR
  - Dump v3 calibration CSVs (cal_dump*.csv with the 27-column v3 contract).

Strict bit-depth stratification:
  - Samples are partitioned strictly into N8, N10, and N12.
  - N8 native samples are NEVER pooled into N10 or N12 (or vice versa).
  - A depth is marked Calibrated ONLY when:
      * native_shots >= min_native_shots (default 1500 per G-PhaseA),
      * sample_hours > 0.0,
      * every size class (K8, K16, Rational, Generic) has finite parameters with sd > 0.
  - Any under-sampled or unpopulated depth emits DepthNullTable::PlaceholderFor(...)
    with calibrated = false and NullTableSourceKind::Placeholder.

Outputs:
  - NullTables.generated.h   C++14 header defining GeneratedDepthNullTables()
  - Markdown & JSON gate reports covering G-PhaseA (per-depth Rule-of-Three 3/H FP bound,
    shot count, confident-wrong count, worst-shot margin) and G-8bit (exact Mann-Whitney U
    8-bit HD AUC, native FPs per depth).

Usage:
  python generate_null_tables.py --dump-dir <dir> --out-dir <dir>
  python generate_null_tables.py --selftest
"""

import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import random
import re
import sys
import tempfile

# ----------------------------------------------------------------------------------------------------------
# Derived contract constants mirrored verbatim from C++ headers (verified by verify_cpp_contract_constants):
#   - ChainNullTables.h : kClassK8..kClassGeneric, NullTables::Placeholder()
#   - ChainCommon.h     : kMaxCandidates (8)
#   - KernelBank.h      : kKernelCount (10)
#   - ChainEvidence.h   : kMinShotDwellSec (3.0) and EvidenceSettings defaults
#   - NullTables.h      : BitDepthClass / DepthNullTableSet fields
# ----------------------------------------------------------------------------------------------------------
CLASSES = ("K8", "K16", "Rational", "Generic")
CLASS_CPP = ("kClassK8", "kClassK16", "kClassRational", "kClassGeneric")
DEPTHS = ("N8", "N10", "N12")
DEPTH_CPP_FIELD = {"N8": "n8", "N10": "n10", "N12": "n12"}
DEPTH_CPP_ENUM = {
    "N8": "BitDepthClass::N8",
    "N10": "BitDepthClass::N10",
    "N12": "BitDepthClass::N12",
}

# NullTables::Placeholder() from ChainNullTables.h:81-96
PLACEHOLDER = {
    "null": [(-0.12, 0.10), (-0.10, 0.08), (-0.04, 0.05), (-0.05, 0.06)],
    "alt": [(-0.30, 0.20)] * 4,
}

# ChainEvidence.h:73-106 / ChainCommon.h:28 / KernelBank.h:42 contract constants
INC_MIN = -3.0                 # ChainEvidence.h: EvidenceSettings::incrementMin
INC_MAX = 5.0                  # ChainEvidence.h: EvidenceSettings::incrementMax
LINE_GATE_FRACTION = 0.25      # ChainEvidence.h: EvidenceSettings::minGateFraction
LINE_GATE_FLOOR = 3            # ChainEvidence.h: EvidenceSettings::minGateLinesFloor
TARGET_RATE_PER_HOUR = 0.01    # ChainEvidence.h: EvidenceSettings::targetRatePerHour
THRESHOLD_INTERVAL_S = 10.0    # ChainEvidence.h: EvidenceSettings::thresholdIntervalSec
CANDIDATE_COUNT = 8            # ChainCommon.h: kMaxCandidates (EvidenceSettings::candidateCount)
KERNEL_COUNT = 10              # KernelBank.h: kKernelCount (EvidenceSettings::kernelCount)
SHOT_CAP_FRACTION = 0.5        # ChainEvidence.h: EvidenceSettings::shotCapFraction
MAX_HYPOTHESES = 16            # ChainEvidence.h: EvidenceSettings::maxHypotheses
MIN_EVALS_CANDIDATE = 4        # ChainEvidence.h: minimum informative evals per candidate
WINDOW_YIELD_NATS = 1.4        # ChainEvidence.h: EvidenceSettings::windowYieldNats
WINDOW_MARGIN = 1.5            # ChainEvidence.h: EvidenceSettings::windowMargin
WINDOW_MIN_SEC = 10.0          # ChainEvidence.h: EvidenceSettings::windowMinSec
WINDOW_MAX_SEC = 600.0         # ChainEvidence.h: EvidenceSettings::windowMaxSec
RATE_SMOOTHING = 0.25          # ChainEvidence.h: EvidenceSettings::rateSmoothing
MIN_SHOT_DWELL_SEC = 3.0       # ChainEvidence.h: kMinShotDwellSec
AGREE_TOL = 0.015              # ChainEvidence.h: EvidenceSettings::agreementTolerance
AGREE_BONUS = 2.0              # ChainEvidence.h: EvidenceSettings::agreementBonus
CONVENTION_TOLERANCE = 0.006   # scaling/geometry/SourceGeometrySet.h: kConventionTolerance

DEFAULT_CV_SEED = "sid-cal"    # Exact default seed from tools/eval/source_id/calibrate_nulls.py:722

# Derived statistical constants (matching calibrate_nulls.py & normal-tail quantiles):
#   - 1 / Phi^-1(0.75) = 1.4826022... (exact Gaussian MAD-to-sigma factor)
#   - Normal quantiles for 1, 2, 2.5 sigma dip tail: Phi(-1)=0.1587, Phi(-2)=0.0228, Phi(-2.5)=0.0062
#   - Upper 1-sigma quantile for alt spread: Phi(+1)=0.8413
#   - Rule of Three 95% CL upper Poisson rate: -ln(0.05) = 2.99573 -> 3.0 / H
MAD_TO_SIGMA = 1.4826
Q_MINUS_1SIG = 0.1587
Q_MINUS_2SIG = 0.0228
Q_MINUS_2P5SIG = 0.0062
Q_PLUS_1SIG = 0.8413
NULL_WIDEN_FACTOR = 1.25      # calibrate_nulls.py Policy::null_widen (conservative tail safety)
SE_UNCERTAINTY_SIGMA = 2.0    # calibrate_nulls.py Policy::se_terms (2-sigma finite-sample uncertainty)
SD_NUMERICAL_FLOOR = 0.005    # calibrate_nulls.py Policy::sd_floor
MIN_ALT_SEPARATION_SIGMA = 0.25  # calibrate_nulls.py Policy::min_separation
DEFAULT_MIN_NATIVE_SHOTS = 1500  # G-PhaseA calibration gate requirement
DEFAULT_MIN_NULL_PER_CLASS = 10
DEFAULT_MIN_ALT_PER_CLASS = 10
G8BIT_MIN_HD_AUC = 0.85          # G-8bit minimum N8 HD (1/2) Mann-Whitney U AUC gate
ALIAS_Q_MAX = 4
ALIAS_TOL = 0.02

REQUIRED_V3_COLS = (
    "reel_id", "title", "eval_seq", "time_s", "axis", "N", "n", "class", "T", "stage",
    "best_kernel", "kernel_margin", "lines_passed", "lines", "power_seen", "floor_codes",
    "truth_n", "bit_depth", "truth_kernel", "truth_sharpen", "shot_id",
    "T_ev", "informative", "master", "has_truth", "truth_n1", "cut_id",
)

REQUIRED_V4_EVAL_COLS = (
    "seq", "t_s", "shot_id", "step_codes", "output_bits", "coded_bits",
    "informative_x", "informative_y", "hold_x", "hold_y",
)

REQUIRED_V4_SIZE_COLS = (
    "seq", "t_s", "shot_id", "axis", "N", "n", "valid", "stat", "lines",
)


# ----------------------------------------------------------------------------------------------------------
# L4: C++ header contract constant drift guard
# ----------------------------------------------------------------------------------------------------------
def _parse_cpp_numeric(text, pattern, label):
    m = re.search(pattern, text)
    if not m:
        raise AssertionError("contract drift guard: failed to locate %s in C++ header" % label)
    return float(m.group(1))


def verify_cpp_contract_constants(chain_dir=None):
    """Parses sibling C++ chain headers to verify zero drift between Python and C++ contract constants.

    Returns a dict of verified constant values when headers are present, or None if chain_dir is absent.
    Raises AssertionError if any constant drifted.
    """
    if chain_dir is None:
        chain_dir = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "chain"))
    ev_path = os.path.join(chain_dir, "ChainEvidence.h")
    cm_path = os.path.join(chain_dir, "ChainCommon.h")
    kb_path = os.path.join(chain_dir, "KernelBank.h")
    nt_path = os.path.join(chain_dir, "ChainNullTables.h")
    if not all(os.path.isfile(p) for p in (ev_path, cm_path, kb_path, nt_path)):
        return None

    with open(ev_path, "r", encoding="utf-8") as fh:
        ev_txt = fh.read()
    with open(cm_path, "r", encoding="utf-8") as fh:
        cm_txt = fh.read()
    with open(kb_path, "r", encoding="utf-8") as fh:
        kb_txt = fh.read()
    with open(nt_path, "r", encoding="utf-8") as fh:
        nt_txt = fh.read()

    num_re = r"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)"
    checks = [
        ("CANDIDATE_COUNT", CANDIDATE_COUNT, cm_txt, r"constexpr\s+int\s+kMaxCandidates\s*=\s*" + num_re + r"\s*;"),
        ("KERNEL_COUNT", KERNEL_COUNT, kb_txt, r"constexpr\s+int\s+kKernelCount\s*=\s*" + num_re + r"\s*;"),
        ("MIN_SHOT_DWELL_SEC", MIN_SHOT_DWELL_SEC, ev_txt, r"constexpr\s+double\s+kMinShotDwellSec\s*=\s*" + num_re + r"\s*;"),
        ("TARGET_RATE_PER_HOUR", TARGET_RATE_PER_HOUR, ev_txt, r"double\s+targetRatePerHour\s*=\s*" + num_re + r"\s*;"),
        ("THRESHOLD_INTERVAL_S", THRESHOLD_INTERVAL_S, ev_txt, r"double\s+thresholdIntervalSec\s*=\s*" + num_re + r"\s*;"),
        ("SHOT_CAP_FRACTION", SHOT_CAP_FRACTION, ev_txt, r"double\s+shotCapFraction\s*=\s*" + num_re + r"\s*;"),
        ("AGREE_BONUS", AGREE_BONUS, ev_txt, r"double\s+agreementBonus\s*=\s*" + num_re + r"\s*;"),
        ("AGREE_TOL", AGREE_TOL, ev_txt, r"double\s+agreementTolerance\s*=\s*" + num_re + r"\s*;"),
        ("LINE_GATE_FRACTION", LINE_GATE_FRACTION, ev_txt, r"double\s+minGateFraction\s*=\s*" + num_re + r"\s*;"),
        ("LINE_GATE_FLOOR", LINE_GATE_FLOOR, ev_txt, r"int\s+minGateLinesFloor\s*=\s*" + num_re + r"\s*;"),
        ("INC_MAX", INC_MAX, ev_txt, r"double\s+incrementMax\s*=\s*" + num_re + r"\s*;"),
        ("INC_MIN", INC_MIN, ev_txt, r"double\s+incrementMin\s*=\s*" + num_re + r"\s*;"),
        ("MAX_HYPOTHESES", MAX_HYPOTHESES, ev_txt, r"int\s+maxHypotheses\s*=\s*" + num_re + r"\s*;"),
        ("WINDOW_YIELD_NATS", WINDOW_YIELD_NATS, ev_txt, r"double\s+windowYieldNats\s*=\s*" + num_re + r"\s*;"),
        ("WINDOW_MARGIN", WINDOW_MARGIN, ev_txt, r"double\s+windowMargin\s*=\s*" + num_re + r"\s*;"),
        ("WINDOW_MIN_SEC", WINDOW_MIN_SEC, ev_txt, r"double\s+windowMinSec\s*=\s*" + num_re + r"\s*;"),
        ("WINDOW_MAX_SEC", WINDOW_MAX_SEC, ev_txt, r"double\s+windowMaxSec\s*=\s*" + num_re + r"\s*;"),
        ("RATE_SMOOTHING", RATE_SMOOTHING, ev_txt, r"double\s+rateSmoothing\s*=\s*" + num_re + r"\s*;"),
    ]
    for c_idx, cpp_cls in enumerate(CLASS_CPP):
        checks.append(("PLACEHOLDER.null[%s].mean" % cpp_cls, PLACEHOLDER["null"][c_idx][0], nt_txt,
                       r"t\.nul\[" + re.escape(cpp_cls) + r"\]\.mean\s*=\s*" + num_re + r"\s*;"))
        checks.append(("PLACEHOLDER.null[%s].sd" % cpp_cls, PLACEHOLDER["null"][c_idx][1], nt_txt,
                       r"t\.nul\[" + re.escape(cpp_cls) + r"\]\.sd\s*=\s*" + num_re + r"\s*;"))
    checks.append(("PLACEHOLDER.alt.mean", PLACEHOLDER["alt"][0][0], nt_txt,
                   r"t\.alt\[c\]\.mean\s*=\s*" + num_re + r"\s*;"))
    checks.append(("PLACEHOLDER.alt.sd", PLACEHOLDER["alt"][0][1], nt_txt,
                   r"t\.alt\[c\]\.sd\s*=\s*" + num_re + r"\s*;"))

    geom_path = os.path.normpath(os.path.join(chain_dir, "..", "..", "..", "..", "scaling", "geometry", "SourceGeometrySet.h"))
    if os.path.isfile(geom_path):
        with open(geom_path, "r", encoding="utf-8") as fh:
            geom_txt = fh.read()
        checks.append(("CONVENTION_TOLERANCE", CONVENTION_TOLERANCE, geom_txt,
                       r"constexpr\s+double\s+kConventionTolerance\s*=\s*" + num_re + r"\s*;"))

    verified = {}
    drifts = []
    for name, py_val, txt, pat in checks:
        cpp_val = _parse_cpp_numeric(txt, pat, name)
        verified[name] = cpp_val
        if abs(float(py_val) - cpp_val) > 1e-12:
            drifts.append("%s: py=%r != cpp=%r" % (name, py_val, cpp_val))
    if drifts:
        raise AssertionError("C++ contract constant drift detected: " + "; ".join(drifts))
    return verified


# ----------------------------------------------------------------------------------------------------------
# Pure math helpers (ports of ChainNullTables.h / NullTables.h / ChainEvidence.h)
# ----------------------------------------------------------------------------------------------------------
def gcd_int(a, b):
    a, b = abs(int(a)), abs(int(b))
    while b:
        a, b = b, a % b
    return a


def clampd(x, lo, hi):
    return lo if x < lo else (hi if x > hi else x)


def classify_latent(n, N, rational_q_max=6, tol=2.0):
    """Exact port of ChainNullTables.h::ClassifyLatent."""
    dn = float(n)
    dN = float(N)
    for k in range(1, 8):
        if abs(dn - dN * k / 8.0) <= tol:
            return 0
    for k in range(1, 16, 2):
        if abs(dn - dN * k / 16.0) <= tol:
            return 1
    for b in range(2, rational_q_max + 1):
        for a in range(1, b):
            if gcd_int(a, b) == 1 and abs(dn - dN * float(a) / float(b)) <= tol:
                return 2
    return 3


def classify_bit_depth_from_step(step_codes, output_bits=0, emp_floor_codes=0.0):
    """Exact port of NullTables.h::ClassifyBitDepthFromStep (including M5/FM-2 empirical floor rule)."""
    if step_codes >= 4:
        return "N8"
    if output_bits == 8:
        return "N8"
    eight_bit_lsb_codes = 4.0
    eight_bit_uniform_floor = eight_bit_lsb_codes / math.sqrt(12.0)
    tol = 5.0 * CONVENTION_TOLERANCE
    if (
        emp_floor_codes is not None
        and math.isfinite(emp_floor_codes)
        and emp_floor_codes > 0.0
        and emp_floor_codes >= eight_bit_uniform_floor * (1.0 - tol)
        and emp_floor_codes < eight_bit_lsb_codes
    ):
        return "N8"
    if output_bits >= 12:
        return "N12"
    if step_codes == 1 or output_bits == 10:
        return "N10"
    return "unknown"


def classify_bit_depth_from_bits(bits):
    """Strict bit-depth stratification for v3 dumps carrying explicit bit_depth."""
    if bits <= 0:
        return "unknown"
    if bits <= 8:
        return "N8"
    if bits == 10:
        return "N10"
    if bits >= 12:
        return "N12"
    return "unknown"


def compute_c1():
    alpha = TARGET_RATE_PER_HOUR * THRESHOLD_INTERVAL_S / 3600.0
    alpha = clampd(alpha, 1e-12, 0.5)
    return -math.log(alpha) + math.log(float(CANDIDATE_COUNT * KERNEL_COUNT))


def llr(params_null, params_alt, stat):
    """Exact port of NullTables::LogLikelihoodRatio."""
    m0, s0 = params_null
    m1, s1 = params_alt
    z0 = (stat - m0) / s0
    z1 = (stat - m1) / s1
    return 0.5 * z0 * z0 - 0.5 * z1 * z1 + math.log(s0 / s1)


def clamped_increment(tables, cls_idx, stat):
    return clampd(llr(tables["null"][cls_idx], tables["alt"][cls_idx], stat), INC_MIN, INC_MAX)


def rule_of_three_bound(sample_hours):
    """Hanley & Lippman-Hand 95% CL upper Poisson rate bound: 3.0 / H."""
    if not math.isfinite(sample_hours) or sample_hours <= 0.0:
        return float("inf")
    return 3.0 / sample_hours


def quantile(xs, q):
    if not xs:
        return float("nan")
    s = sorted(xs)
    p = q * (len(s) - 1)
    lo = int(math.floor(p))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (p - lo)


def median(xs):
    return quantile(xs, 0.5)


def dip_sd(xs, floor=SD_NUMERICAL_FLOOR):
    med = median(xs)
    return max(
        med - quantile(xs, Q_MINUS_1SIG),
        (med - quantile(xs, Q_MINUS_2SIG)) / 2.0,
        (med - quantile(xs, Q_MINUS_2P5SIG)) / 2.5,
        floor,
    )


def null_sd(xs, floor=SD_NUMERICAL_FLOOR):
    med = median(xs)
    mad = MAD_TO_SIGMA * median([abs(x - med) for x in xs])
    return max(mad, dip_sd(xs, floor))


def alt_sd(xs, floor=SD_NUMERICAL_FLOOR):
    med = median(xs)
    mad = MAD_TO_SIGMA * median([abs(x - med) for x in xs])
    return max(mad, quantile(xs, Q_PLUS_1SIG) - med, floor)


def mann_whitney_u_auc(pos, neg):
    """Exact rank-based Mann-Whitney U AUC: P(pos > neg) + 0.5 * P(pos == neg)."""
    if not pos or not neg:
        return float("nan")
    combined = sorted([(v, 1) for v in pos] + [(v, 0) for v in neg], key=lambda p: p[0])
    ranks_pos = 0.0
    i = 0
    n_all = len(combined)
    while i < n_all:
        j = i
        while j + 1 < n_all and combined[j + 1][0] == combined[i][0]:
            j += 1
        avg_rank = 0.5 * (i + j) + 1.0
        pos_in_tie = sum(1 for k in range(i, j + 1) if combined[k][1] == 1)
        ranks_pos += avg_rank * pos_in_tie
        i = j + 1
    n_pos = float(len(pos))
    n_neg = float(len(neg))
    u = ranks_pos - n_pos * (n_pos + 1.0) / 2.0
    return u / (n_pos * n_neg)


def is_alt_size(n, truth_n, truth_n1=0):
    if truth_n <= 0:
        return False
    return abs(n - truth_n) <= 1 or (truth_n1 > 0 and abs(n - truth_n1) <= 1)


def is_alias_size(n, truth_n, q_max=ALIAS_Q_MAX, tol=ALIAS_TOL):
    if truth_n <= 0:
        return False
    ratio = float(n) / float(truth_n)
    for q in range(1, q_max + 1):
        p = int(round(ratio * q))
        if p >= 1 and abs(ratio - float(p) / float(q)) <= tol * float(p) / float(q):
            return True
    return False


def _parse_float(val, default=None):
    if val is None:
        return default
    s = str(val).strip()
    if not s:
        return default
    try:
        v = float(s)
    except (TypeError, ValueError):
        return default
    return v if math.isfinite(v) else default


def _parse_int(val, default=None):
    v = _parse_float(val, None)
    if v is None:
        return default
    return int(round(v))


# ----------------------------------------------------------------------------------------------------------
# Unified Sample & Segment records (shared by Dump v4 and Dump v3 loaders)
# ----------------------------------------------------------------------------------------------------------
class SampleItem(object):
    __slots__ = (
        "reel", "master", "depth", "seq", "time_s", "axis", "N", "n",
        "stat", "cls", "truth_n", "truth_n1", "shot_id", "label",
    )

    def __init__(self, reel, master, depth, seq, time_s, axis, N, n, stat, cls, truth_n, truth_n1, shot_id, label):
        self.reel = reel
        self.master = master
        self.depth = depth
        self.seq = seq
        self.time_s = time_s
        self.axis = axis
        self.N = N
        self.n = n
        self.stat = stat
        self.cls = cls
        self.truth_n = truth_n
        self.truth_n1 = truth_n1
        self.shot_id = shot_id
        self.label = label


def _cluster_and_label(eval_rows, reel, master, depth, truth_n, truth_n1):
    """Clusters +-1 px candidates on one (reel, axis, seq) and labels null vs alt (dropping aliases)."""
    if not eval_rows:
        return []
    grp = sorted(eval_rows, key=lambda r: (r["n"], r["stat"]))
    clusters = []
    cur = [grp[0]]
    for r in grp[1:]:
        if r["n"] - cur[-1]["n"] <= 1:
            cur.append(r)
        else:
            clusters.append(cur)
            cur = [r]
    clusters.append(cur)

    items = []
    for cl in clusters:
        best = min(cl, key=lambda x: (x["stat"], x["n"]))
        cls_idx = classify_latent(best["n"], best["N"])
        if truth_n <= 0:
            label = "null"
        elif is_alt_size(best["n"], truth_n, truth_n1):
            label = "alt"
        elif truth_n1 > 0:
            continue
        elif is_alias_size(best["n"], truth_n):
            continue
        else:
            label = "null"
        items.append(SampleItem(
            reel=reel,
            master=master,
            depth=depth,
            seq=best["seq"],
            time_s=best["time_s"],
            axis=best["axis"],
            N=best["N"],
            n=best["n"],
            stat=best["stat"],
            cls=cls_idx,
            truth_n=truth_n,
            truth_n1=truth_n1,
            shot_id=best["shot_id"],
            label=label,
        ))
    return items


# ----------------------------------------------------------------------------------------------------------
# Dump v4 loader (<tag>.b_evals.csv + <tag>.b_sizes.csv) & Dump v3 loader (cal_dump*.csv)
# ----------------------------------------------------------------------------------------------------------
def load_truth_manifest(path):
    if not path:
        return {}
    with open(path, "r", encoding="utf-8-sig") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError("truth manifest must be a JSON object keyed by reel/tag name")
    return data


def _load_dump_v4_pair(evals_path, sizes_path, tag, manifest):
    meta = manifest.get(tag, {})
    truth_nx = int(meta.get("truth_nx", 0))
    truth_ny = int(meta.get("truth_ny", 0))
    truth_nx1 = int(meta.get("truth_nx1", 0))
    truth_ny1 = int(meta.get("truth_ny1", 0))
    forced_depth = meta.get("depth")
    master = str(meta.get("master", tag))

    eval_meta = {}
    with open(evals_path, "r", newline="", encoding="utf-8-sig") as fh:
        rd = csv.DictReader(fh)
        names = rd.fieldnames or []
        missing = [c for c in REQUIRED_V4_EVAL_COLS if c not in names]
        if missing:
            raise ValueError("%s: missing v4 b_evals columns %s" % (evals_path, ",".join(missing)))
        for rec in rd:
            seq = _parse_int(rec.get("seq"))
            t_s = _parse_float(rec.get("t_s"))
            shot_id = _parse_int(rec.get("shot_id"), 0)
            step_codes = _parse_int(rec.get("step_codes"), 0)
            out_bits = _parse_int(rec.get("output_bits"), 0)
            if seq is None or t_s is None:
                continue
            emp_x = _parse_float(rec.get("emp_floor_x"))
            emp_y = _parse_float(rec.get("emp_floor_y"))
            emp_vx = _parse_int(rec.get("emp_floor_valid_x") if "emp_floor_valid_x" in rec else rec.get("emp_valid_x"), 0)
            emp_vy = _parse_int(rec.get("emp_floor_valid_y") if "emp_floor_valid_y" in rec else rec.get("emp_valid_y"), 0)
            valid_emp = []
            if emp_vx == 1 and emp_x is not None and emp_x > 0.0:
                valid_emp.append(emp_x)
            if emp_vy == 1 and emp_y is not None and emp_y > 0.0:
                valid_emp.append(emp_y)
            emp_floor_codes = min(valid_emp) if valid_emp else 0.0
            null_depth_raw = (rec.get("null_depth_class") or "").strip()
            if forced_depth in DEPTHS:
                depth = forced_depth
            else:
                classified = classify_bit_depth_from_step(step_codes, out_bits, emp_floor_codes)
                if emp_floor_codes > 0.0:
                    depth = classified
                elif null_depth_raw in DEPTHS:
                    depth = null_depth_raw
                else:
                    depth = classified
            eval_meta[seq] = {
                "seq": seq,
                "time_s": t_s,
                "shot_id": shot_id,
                "depth": depth,
                "hold_x": _parse_int(rec.get("hold_x"), 0) == 1,
                "hold_y": _parse_int(rec.get("hold_y"), 0) == 1,
                "inf_x": _parse_int(rec.get("informative_x"), 0) == 1,
                "inf_y": _parse_int(rec.get("informative_y"), 0) == 1,
            }

    per_eval_axis = {}
    with open(sizes_path, "r", newline="", encoding="utf-8-sig") as fh:
        rd = csv.DictReader(fh)
        names = rd.fieldnames or []
        missing = [c for c in REQUIRED_V4_SIZE_COLS if c not in names]
        if missing:
            raise ValueError("%s: missing v4 b_sizes columns %s" % (sizes_path, ",".join(missing)))
        for rec in rd:
            seq = _parse_int(rec.get("seq"))
            valid = _parse_int(rec.get("valid"), 0)
            stat = _parse_float(rec.get("stat"))
            N = _parse_int(rec.get("N"))
            n = _parse_int(rec.get("n"))
            axis_raw = (rec.get("axis") or "").strip().lower()
            axis = "x" if axis_raw in ("x", "0", "h") else ("y" if axis_raw in ("y", "1", "v") else "")
            if seq is None or valid != 1 or stat is None or not axis or N is None or n is None or N <= 0 or n <= 0:
                continue
            em = eval_meta.get(seq)
            if em is None:
                continue
            if (axis == "x" and (em["hold_x"] or not em["inf_x"])) or (axis == "y" and (em["hold_y"] or not em["inf_y"])):
                continue
            per_eval_axis.setdefault((axis, seq), []).append({
                "seq": seq,
                "time_s": em["time_s"],
                "shot_id": em["shot_id"],
                "axis": axis,
                "N": N,
                "n": n,
                "stat": stat,
            })

    items = []
    segs_by_depth = {}
    for seq in sorted(eval_meta):
        em = eval_meta[seq]
        depth = em["depth"]
        seg = segs_by_depth.get(depth)
        if seg is None:
            seg = {
                "reel": tag,
                "master": master,
                "depth": depth,
                "truth": {"x": truth_nx, "y": truth_ny},
                "truth1": {"x": truth_nx1, "y": truth_ny1},
                "evals": [],
            }
            segs_by_depth[depth] = seg
        seg["evals"].append((seq, {
            "x": (em["time_s"], em["shot_id"], em["hold_x"], em["inf_x"]),
            "y": (em["time_s"], em["shot_id"], em["hold_y"], em["inf_y"]),
        }))
        for axis, tn, tn1 in (("x", truth_nx, truth_nx1), ("y", truth_ny, truth_ny1)):
            rows = per_eval_axis.get((axis, seq))
            if rows:
                items.extend(_cluster_and_label(rows, tag, master, depth, tn, tn1))

    return items, list(segs_by_depth.values())


def _load_dump_v3_csv(csv_path):
    items = []
    per_eval = {}
    ev_map = {}
    hold_map = {}
    with open(csv_path, "r", newline="", encoding="utf-8-sig") as fh:
        rd = csv.DictReader(fh)
        names = rd.fieldnames or []
        if not all(c in names for c in REQUIRED_V3_COLS):
            return [], []
        for rec in rd:
            reel = (rec.get("reel_id") or "").strip()
            master = (rec.get("master") or "").strip()
            seq = _parse_int(rec.get("eval_seq"))
            t_s = _parse_float(rec.get("time_s"))
            N = _parse_int(rec.get("N"))
            n = _parse_int(rec.get("n"))
            lines_passed = _parse_int(rec.get("lines_passed"))
            lines = _parse_int(rec.get("lines"))
            truth_n = _parse_int(rec.get("truth_n"))
            truth_n1 = _parse_int(rec.get("truth_n1"), 0)
            bits = _parse_int(rec.get("bit_depth"), 0)
            shot_id = _parse_int(rec.get("shot_id"), 0)
            cut_id = _parse_int(rec.get("cut_id"), shot_id)
            informative = _parse_int(rec.get("informative"), 0) == 1
            has_truth = _parse_int(rec.get("has_truth"), 0) == 1
            stage = (rec.get("stage") or "").strip().lower()
            axis_raw = (rec.get("axis") or "").strip().lower()
            axis = "x" if axis_raw in ("x", "0", "h") else ("y" if axis_raw in ("y", "1", "v") else axis_raw)
            t_ev = _parse_float(rec.get("T_ev"))
            if None in (seq, t_s, N, n, lines_passed, lines, truth_n) or N <= 0 or lines <= 0 or not reel or not master:
                continue
            depth = classify_bit_depth_from_bits(bits)
            ev_map.setdefault((reel, depth), {}).setdefault(seq, {})[axis] = (
                t_s, cut_id, truth_n, truth_n1, has_truth, master
            )
            if stage != "exact":
                continue
            need = max(LINE_GATE_FLOOR, int(math.ceil(LINE_GATE_FRACTION * lines - 1e-9)))
            gate_passed = lines_passed >= need
            hk = hold_map.setdefault((reel, depth, seq, axis), [False, False])
            if informative and t_ev is not None:
                hk[1] = True
            elif t_ev is None and gate_passed:
                hk[0] = True
            if has_truth and informative and t_ev is not None and gate_passed:
                per_eval.setdefault((reel, master, depth, axis, seq, truth_n, truth_n1), []).append({
                    "seq": seq,
                    "time_s": t_s,
                    "shot_id": cut_id,
                    "axis": axis,
                    "N": N,
                    "n": n,
                    "stat": t_ev,
                })

    for key in sorted(per_eval):
        reel, master, depth, axis, seq, tn, tn1 = key
        items.extend(_cluster_and_label(per_eval[key], reel, master, depth, tn, tn1))

    segs = []
    for (reel, depth), by_seq in sorted(ev_map.items()):
        cur = None
        cur_key = None
        for seq in sorted(by_seq):
            e = by_seq[seq]
            if any(not v[4] for v in e.values()):
                cur = None
                cur_key = None
                continue
            key = tuple(sorted((a, v[2], v[3]) for a, v in e.items()))
            if cur is None or key != cur_key:
                cur_key = key
                any_v = next(iter(e.values()))
                cur = {
                    "reel": reel,
                    "master": any_v[5],
                    "depth": depth,
                    "truth": dict((a, t) for a, t, _ in key),
                    "truth1": dict((a, t1) for a, _, t1 in key),
                    "evals": [],
                }
                segs.append(cur)
            per_ax = {}
            for a, v in e.items():
                hf = hold_map.get((reel, depth, seq, a))
                is_hold = bool(hf and hf[0] and not hf[1])
                is_inf = bool(hf and hf[1])
                per_ax[a] = (v[0], v[1], is_hold, is_inf)
            cur["evals"].append((seq, per_ax))
    return items, segs


def load_all_dumps(dump_dirs, truth_manifest_path=None):
    manifest = load_truth_manifest(truth_manifest_path)
    items = []
    segs = []
    v4_pairs = 0
    v3_files = 0
    for d in dump_dirs:
        if not os.path.isdir(d):
            raise FileNotFoundError("dump directory not found: " + d)
        entries = sorted(os.listdir(d))
        eval_files = {}
        size_files = {}
        other_csvs = []
        for fn in entries:
            low = fn.lower()
            if not low.endswith(".csv"):
                continue
            if low.endswith(".b_evals.csv"):
                eval_files[fn[:-len(".b_evals.csv")]] = os.path.join(d, fn)
            elif low.endswith(".b_sizes.csv"):
                size_files[fn[:-len(".b_sizes.csv")]] = os.path.join(d, fn)
            elif low.endswith(".a_frames.csv") or low.endswith(".a_abr.csv"):
                continue
            else:
                other_csvs.append(os.path.join(d, fn))
        for tag in sorted(eval_files):
            if tag not in size_files:
                continue
            it, sg = _load_dump_v4_pair(eval_files[tag], size_files[tag], tag, manifest)
            items.extend(it)
            segs.extend(sg)
            v4_pairs += 1
        for fp in other_csvs:
            it, sg = _load_dump_v3_csv(fp)
            if it or sg:
                items.extend(it)
                segs.extend(sg)
                v3_files += 1
    return items, segs, {"v4_pairs": v4_pairs, "v3_files": v3_files}


# ----------------------------------------------------------------------------------------------------------
# Offline ChainEvidence replay for G-PhaseA (native FP count & worst-shot margin)
# ----------------------------------------------------------------------------------------------------------
class _SimSlot(object):
    __slots__ = ("n", "cls", "closed", "shot_sum", "shot_time", "shot_id", "shots", "evals")

    def __init__(self, n, cls):
        self.n = n
        self.cls = cls
        self.closed = []
        self.shot_sum = 0.0
        self.shot_time = None
        self.shot_id = None
        self.shots = 0
        self.evals = 0


class _SimAxis(object):
    def __init__(self):
        self.slots = []
        self.inf_evals = 0
        self.shots = 0
        self.last_shot = None
        self.N = 0
        self.prev = (False, 0)
        self.last_time = None
        self.interval = 0.0
        self.shot_key = None
        self.shot_start = 0.0
        self.shot_met = False
        self.pending = []


def _slot_total(s, cap):
    t = clampd(s.shot_sum, -cap, cap)
    for _, val in s.closed:
        t += val
    return t


def _slot_add(s, inc, shot, tm, cap):
    if s.shots == 0 or s.shot_id != shot:
        if s.shots > 0:
            s.closed.append([s.shot_time, clampd(s.shot_sum, -cap, cap)])
        s.shot_sum = 0.0
        s.shot_id = shot
        s.shots += 1
    s.shot_sum += inc
    s.shot_time = tm
    s.evals += 1


def _find_slot(ax, item):
    near = None
    near_d = 0
    for s in ax.slots:
        d = abs(s.n - item.n)
        if d > 1:
            continue
        if near is None or d < near_d or (d == near_d and s.n < near.n):
            near = s
            near_d = d
    if near is not None:
        return near
    h = _SimSlot(item.n, item.cls)
    if len(ax.slots) < MAX_HYPOTHESES:
        ax.slots.append(h)
        return h
    worst = 0
    for i in range(1, len(ax.slots)):
        a = _slot_total(ax.slots[i], 1e300)
        b = _slot_total(ax.slots[worst], 1e300)
        if a < b or (a == b and ax.slots[i].n > ax.slots[worst].n):
            worst = i
    ax.slots[worst] = h
    return h


def _window_sec(ax, c1):
    if not ax.interval > 0.0:
        return 0.0
    return clampd(WINDOW_MARGIN * (c1 / WINDOW_YIELD_NATS) * ax.interval, WINDOW_MIN_SEC, WINDOW_MAX_SEC)


def _note_time(ax, tm, c1):
    if ax.last_time is not None and tm > ax.last_time:
        gap = tm - ax.last_time
        ax.interval = (1.0 - RATE_SMOOTHING) * ax.interval + RATE_SMOOTHING * gap if ax.interval > 0.0 else gap
    if ax.last_time is None or tm > ax.last_time:
        ax.last_time = tm
    w = _window_sec(ax, c1)
    if not w > 0.0:
        return
    cutoff = ax.last_time - w
    keep = []
    for s in ax.slots:
        k = 0
        while k < len(s.closed) and s.closed[k][0] is not None and s.closed[k][0] < cutoff:
            k += 1
        if k:
            del s.closed[:k]
        if s.shots > 0 and s.shot_time is not None and s.shot_time < cutoff:
            if not s.closed:
                s.shots = 0
            else:
                s.shot_sum = 0.0
                s.shot_time = None
        if s.closed or s.shots > 0:
            keep.append(s)
    ax.slots = keep


def _flush_pending(ax, cap):
    pend = ax.pending
    ax.pending = []
    for it, inc, shot, tm in pend:
        slot = _find_slot(ax, it)
        slot.n = it.n
        slot.cls = it.cls
        _slot_add(slot, inc, shot, tm, cap)


def _track_shot(ax, shot, tm, cap):
    if MIN_SHOT_DWELL_SEC <= 0.0:
        return
    if ax.shot_key is None or ax.shot_key != shot:
        if ax.shot_key is not None and not ax.shot_met and tm - ax.shot_start >= MIN_SHOT_DWELL_SEC:
            ax.shot_met = True
            _flush_pending(ax, cap)
        ax.pending = []
        ax.shot_key = shot
        ax.shot_start = tm
        ax.shot_met = False
    elif not ax.shot_met and tm - ax.shot_start >= MIN_SHOT_DWELL_SEC:
        ax.shot_met = True
        _flush_pending(ax, cap)


def _axis_informative_non_hold(v):
    hold = bool(v[2])
    inf = True if len(v) < 4 else bool(v[3])
    return inf and not hold


def segment_hours(seg):
    ts = []
    for _, e in seg["evals"]:
        if not e:
            continue
        inf_ts = [v[0] for v in e.values() if _axis_informative_non_hold(v)]
        if inf_ts:
            ts.append(min(inf_ts))
    if len(ts) < 2:
        return 0.0
    gaps = sorted(b - a for a, b in zip(ts, ts[1:]) if b > a)
    step = gaps[len(gaps) // 2] if gaps else 0.0
    return (max(ts) - min(ts) + step) / 3600.0


def simulate_native_segment(seg, items_idx, tables, c1):
    """Replays one native segment; returns (fp_events_count, max_llr_observed)."""
    cap = SHOT_CAP_FRACTION * c1
    axes = sorted(seg["truth"])
    st = {a: _SimAxis() for a in axes}
    fp_events = 0
    max_llr = -1e9
    hold_since_cut = False
    prev_cut = None

    for seq, per_axis in seg["evals"]:
        cut_now = next((per_axis[a][1] for a in axes if a in per_axis), None)
        if prev_cut is not None and cut_now is not None and cut_now != prev_cut and hold_since_cut:
            st = {a: _SimAxis() for a in axes}
            hold_since_cut = False
        if cut_now is not None:
            prev_cut = cut_now

        for a in axes:
            if a not in per_axis:
                continue
            tm, shot, hold = per_axis[a][0], per_axis[a][1], per_axis[a][2]
            its = items_idx.get((seg["reel"], seg["depth"], a, seq), ())
            s = st[a]
            if its and s.N and s.N != its[0].N:
                st[a] = s = _SimAxis()
            if its:
                s.N = its[0].N
            _note_time(s, tm, c1)
            _track_shot(s, shot, tm, cap)
            if hold:
                s.slots, s.inf_evals, s.shots, s.last_shot, s.pending = [], 0, 0, None, []
                hold_since_cut = True
                continue
            for it in its:
                inc = clamped_increment(tables, it.cls, it.stat)
                if MIN_SHOT_DWELL_SEC > 0.0 and not s.shot_met:
                    s.pending.append((it, inc, shot, tm))
                    continue
                slot = _find_slot(s, it)
                slot.n = it.n
                slot.cls = it.cls
                _slot_add(slot, inc, shot, tm, cap)
            if its:
                s.inf_evals += 1
                if s.shots == 0 or s.last_shot != shot:
                    s.shots += 1
                    s.last_shot = shot

        own = {}
        for a in axes:
            best = None
            bv = 0.0
            for sl in st[a].slots:
                v = _slot_total(sl, cap)
                if best is None or v > bv or (v == bv and sl.n < best.n):
                    best = sl
                    bv = v
            own[a] = (best, bv)

        fired = False
        for a in axes:
            best, bv = own[a]
            s = st[a]
            if best is None:
                s.prev = (False, 0)
                continue
            prior = 0.0
            for o in axes:
                if o != a and own[o][0] is not None and bv > 0.0 and own[o][1] > 0.0 and s.N and st[o].N:
                    ra = best.n / float(s.N)
                    ro = own[o][0].n / float(st[o].N)
                    prior = AGREE_BONUS if abs(ra - ro) <= AGREE_TOL * ra else -AGREE_BONUS
            total_llr = bv + prior
            if total_llr > max_llr:
                max_llr = total_llr
            above = total_llr >= c1 and s.inf_evals >= MIN_EVALS_CANDIDATE and s.shots >= 2
            was, was_n = s.prev
            s.prev = (above, best.n)
            if above and (not was or abs(was_n - best.n) > 1):
                fp_events += 1
                fired = True
        if fired:
            st = {a: _SimAxis() for a in axes}
    return fp_events, max_llr


# ----------------------------------------------------------------------------------------------------------
# Leakage-controlled 2-fold held-out cross-validation by master (ported from calibrate_nulls.py:722-818)
# ----------------------------------------------------------------------------------------------------------
def split_masters(masters, folds=2, seed=DEFAULT_CV_SEED):
    """Deterministic by-master split: all reels/segments of one master share a single fold (zero leakage)."""
    order = sorted(set(masters), key=lambda t: hashlib.sha1((seed + "|" + str(t)).encode("utf-8")).hexdigest())
    return {t: i % folds for i, t in enumerate(order)}


def _fit_tables_from_items(train_items, min_null_per_class, min_alt_per_class):
    """Fits per-class null and alt Gaussian parameters strictly from train_items."""
    null_by_cls = [[] for _ in range(4)]
    alt_by_cls = [[] for _ in range(4)]
    null_shots_by_cls = [set() for _ in range(4)]
    alt_shots_by_cls = [set() for _ in range(4)]
    for it in train_items:
        sk = (it.reel, it.axis, it.shot_id)
        if it.label == "null":
            null_by_cls[it.cls].append(it.stat)
            null_shots_by_cls[it.cls].add(sk)
        elif it.label == "alt":
            alt_by_cls[it.cls].append(it.stat)
            alt_shots_by_cls[it.cls].add(sk)

    fitted_null = []
    fitted_alt = []
    class_diag = []
    fit_reasons = []
    all_classes_fitted = True

    for c in range(4):
        nx = null_by_cls[c]
        ax = alt_by_cls[c]
        ne = len(null_shots_by_cls[c])
        ae = len(alt_shots_by_cls[c])
        if len(nx) < min_null_per_class or ne <= 0:
            all_classes_fitted = False
            fit_reasons.append("%s null count %d < %d" % (CLASSES[c], len(nx), min_null_per_class))
            fitted_null.append(PLACEHOLDER["null"][c])
            fitted_alt.append(PLACEHOLDER["alt"][c])
            class_diag.append({"class": CLASSES[c], "n_null": len(nx), "n_alt": len(ax), "status": "insufficient_null"})
            continue

        m0 = median(nx)
        s0 = max(
            null_sd(nx, SD_NUMERICAL_FLOOR) * NULL_WIDEN_FACTOR * (1.0 + SE_UNCERTAINTY_SIGMA / math.sqrt(float(ne))),
            SD_NUMERICAL_FLOOR,
        )
        nul = (m0, s0)

        if len(ax) >= min_alt_per_class and ae > 0:
            m1 = median(ax)
            s1 = max(alt_sd(ax, SD_NUMERICAL_FLOOR) * (1.0 + SE_UNCERTAINTY_SIGMA / math.sqrt(float(ae))), SD_NUMERICAL_FLOOR)
            if m1 < m0 - MIN_ALT_SEPARATION_SIGMA * s0:
                alt = (m1, s1)
                c_status = "fitted"
            else:
                all_classes_fitted = False
                fit_reasons.append(
                    "class %s alt not separated from null (m1=%.4f >= m0-sep=%.4f)"
                    % (CLASSES[c], m1, m0 - MIN_ALT_SEPARATION_SIGMA * s0)
                )
                alt = nul
                c_status = "alt_unseparated_zero_llr"
        else:
            all_classes_fitted = False
            fit_reasons.append(
                "class %s insufficient alt (%d < %d)" % (CLASSES[c], len(ax), min_alt_per_class)
            )
            alt = nul
            c_status = "no_alt_zero_llr"

        fitted_null.append(nul)
        fitted_alt.append(alt)
        class_diag.append({
            "class": CLASSES[c],
            "n_null": len(nx),
            "n_alt": len(ax),
            "eff_null": ne,
            "eff_alt": ae,
            "null_mean": nul[0],
            "null_sd": nul[1],
            "alt_mean": alt[0],
            "alt_sd": alt[1],
            "status": c_status,
        })

    tables = {"null": fitted_null, "alt": fitted_alt}
    if not _valid_table_params(tables):
        all_classes_fitted = False
        fit_reasons.append("invalid_fitted_parameters")
    return tables, class_diag, all_classes_fitted, fit_reasons


def evaluate_heldout(test_items, test_segs, tables, c1):
    """Evaluates native FP replay and LLR discrimination strictly on held-out test_items / test_segs."""
    items_idx = {}
    for it in test_items:
        items_idx.setdefault((it.reel, it.depth, it.axis, it.seq), []).append(it)

    native_segs = [sg for sg in test_segs if all(t <= 0 for t in sg["truth"].values())]
    native_fp = 0
    max_native_llr = -1e9
    sample_hours = 0.0
    for sg in native_segs:
        sample_hours += segment_hours(sg)
        fps, peak_llr = simulate_native_segment(sg, items_idx, tables, c1)
        native_fp += fps
        if peak_llr > max_native_llr:
            max_native_llr = peak_llr
    worst_shot_margin = (c1 - max_native_llr) if max_native_llr > -1e8 else c1

    pos_all = []
    neg_all = []
    pos_hd = []
    neg_hd = []
    for it in test_items:
        val = llr(tables["null"][it.cls], tables["alt"][it.cls], it.stat)
        is_half = it.N > 0 and abs(it.n - 0.5 * it.N) <= 2.0
        if it.label == "alt":
            pos_all.append(val)
            if is_half:
                pos_hd.append(val)
        elif it.label == "null":
            neg_all.append(val)
            if is_half:
                neg_hd.append(val)

    return {
        "native_fp": native_fp,
        "native_segs": len(native_segs),
        "sample_hours": sample_hours,
        "worst_shot_margin": worst_shot_margin,
        "auc_all": mann_whitney_u_auc(pos_all, neg_all),
        "auc_hd": mann_whitney_u_auc(pos_hd, neg_hd),
        "n_pos_hd": len(pos_hd),
        "n_neg_hd": len(neg_hd),
        "pos_all": pos_all,
        "neg_all": neg_all,
        "pos_hd": pos_hd,
        "neg_hd": neg_hd,
    }


# ----------------------------------------------------------------------------------------------------------
# Per-depth strict calibration & gate scoring
# ----------------------------------------------------------------------------------------------------------
def _valid_table_params(tables):
    for c in range(4):
        m0, s0 = tables["null"][c]
        m1, s1 = tables["alt"][c]
        if not (math.isfinite(m0) and math.isfinite(s0) and s0 > 0.0):
            return False
        if not (math.isfinite(m1) and math.isfinite(s1) and s1 > 0.0):
            return False
    return True


def fit_depth_stratum(depth, items, segs, min_native_shots, min_null_per_class, min_alt_per_class,
                      cv_seed=DEFAULT_CV_SEED):
    """Fits a single bit-depth stratum strictly from its own samples with leakage-controlled 2-fold CV by master."""
    c1 = compute_c1()
    depth_items = [it for it in items if it.depth == depth]
    depth_segs = [sg for sg in segs if sg["depth"] == depth]
    native_segs = [sg for sg in depth_segs if all(t <= 0 for t in sg["truth"].values())]

    native_shot_keys = set()
    sample_hours = 0.0
    for sg in native_segs:
        sample_hours += segment_hours(sg)
        for _, per_ax in sg["evals"]:
            for _, v in per_ax.items():
                if _axis_informative_non_hold(v):
                    native_shot_keys.add((sg["reel"], v[1]))
    # Also include any native items whose shot wasn't captured via empty segs
    for it in depth_items:
        if it.truth_n <= 0:
            native_shot_keys.add((it.reel, it.shot_id))
    native_shots = len(native_shot_keys)

    gate_reasons = []
    if native_shots < min_native_shots:
        gate_reasons.append("native_shots %d < %d" % (native_shots, min_native_shots))
    if not (math.isfinite(sample_hours) and sample_hours > 0.0):
        gate_reasons.append("sample_hours %.4f <= 0" % sample_hours)

    # Full-stratum candidate table fit (used for C++ emission ONLY after held-out 2-fold CV passes).
    candidate_tables, class_diag, all_classes_fitted, fit_reasons = _fit_tables_from_items(
        depth_items, min_null_per_class, min_alt_per_class
    )
    gate_reasons.extend(fit_reasons)
    params_valid = _valid_table_params(candidate_tables)

    # Leakage-controlled 2-fold cross-validation grouped by master (H3: NEVER silently fall back to in-sample).
    depth_masters = sorted(set([it.master for it in depth_items] + [sg["master"] for sg in depth_segs]))
    cv_folds = []
    cv_fail_reasons = []
    pooled_pos_all = []
    pooled_neg_all = []
    pooled_pos_hd = []
    pooled_neg_hd = []
    heldout_native_fp = 0
    heldout_worst_shot_margin = c1

    if len(depth_masters) < 2:
        cv_passed = False
        cv_fail_reasons.append("fewer than 2 distinct masters (%d)" % len(depth_masters))
    else:
        fold_of = split_masters(depth_masters, folds=2, seed=cv_seed)
        if len(set(fold_of.values())) < 2:
            cv_passed = False
            cv_fail_reasons.append("master split did not yield 2 disjoint folds")
        else:
            fold_margins = []
            for k in (0, 1):
                test_fold = 1 - k
                train_masters = sorted(m for m in depth_masters if fold_of[m] == k)
                test_masters = sorted(m for m in depth_masters if fold_of[m] == test_fold)
                train_items = [it for it in depth_items if fold_of[it.master] == k]
                test_items = [it for it in depth_items if fold_of[it.master] == test_fold]
                test_segs = [sg for sg in depth_segs if fold_of[sg["master"]] == test_fold]

                fold_tables, _, fold_fit_ok, fold_fit_reasons = _fit_tables_from_items(
                    train_items, min_null_per_class, min_alt_per_class
                )
                fold_params_ok = fold_fit_ok and _valid_table_params(fold_tables)
                eval_tables = fold_tables if fold_params_ok else PLACEHOLDER
                ev = evaluate_heldout(test_items, test_segs, eval_tables, c1)

                fold_reasons = []
                if not fold_params_ok:
                    fold_reasons.append("train fit failed (%s)" % "; ".join(fold_fit_reasons))
                if ev["native_segs"] <= 0:
                    fold_reasons.append("held-out fold %d has 0 native segments" % test_fold)
                if ev["native_fp"] > 0:
                    fold_reasons.append("held-out native_fp %d > 0" % ev["native_fp"])
                fold_passed = len(fold_reasons) == 0
                if not fold_passed:
                    cv_fail_reasons.append("fold %d->%d: %s" % (k, test_fold, "; ".join(fold_reasons)))

                heldout_native_fp += ev["native_fp"]
                if ev["native_segs"] > 0:
                    fold_margins.append(ev["worst_shot_margin"])
                pooled_pos_all.extend(ev["pos_all"])
                pooled_neg_all.extend(ev["neg_all"])
                pooled_pos_hd.extend(ev["pos_hd"])
                pooled_neg_hd.extend(ev["neg_hd"])

                cv_folds.append({
                    "train_fold": k,
                    "test_fold": test_fold,
                    "train_masters": train_masters,
                    "test_masters": test_masters,
                    "fit_ok": fold_params_ok,
                    "fit_reasons": fold_fit_reasons,
                    "native_segs": ev["native_segs"],
                    "sample_hours": ev["sample_hours"],
                    "native_fp": ev["native_fp"],
                    "worst_shot_margin": ev["worst_shot_margin"],
                    "auc_all": ev["auc_all"],
                    "auc_hd": ev["auc_hd"],
                    "n_pos_hd": ev["n_pos_hd"],
                    "n_neg_hd": ev["n_neg_hd"],
                    "passed": fold_passed,
                    "failure_reason": "ok" if fold_passed else "; ".join(fold_reasons),
                })
            if fold_margins:
                heldout_worst_shot_margin = min(fold_margins)
            cv_passed = len(cv_folds) == 2 and all(f["passed"] for f in cv_folds)

    cv_failure_reason = "ok" if cv_passed else "; ".join(cv_fail_reasons)
    if not cv_passed:
        gate_reasons.append("cv_failed: " + cv_failure_reason)

    heldout_auc_all = mann_whitney_u_auc(pooled_pos_all, pooled_neg_all)
    heldout_auc_hd = mann_whitney_u_auc(pooled_pos_hd, pooled_neg_hd)

    # Full-stratum safety check (belt-and-suspenders on emitted tables; NEVER overrides a failing CV).
    full_ev = evaluate_heldout(
        depth_items,
        depth_segs,
        candidate_tables if (all_classes_fitted and params_valid) else PLACEHOLDER,
        c1,
    )
    insample_confident_wrong = full_ev["native_fp"]
    if insample_confident_wrong > 0:
        gate_reasons.append("confident_wrong %d > 0" % insample_confident_wrong)

    calibrated = (
        native_shots >= min_native_shots
        and math.isfinite(sample_hours)
        and sample_hours > 0.0
        and all_classes_fitted
        and params_valid
        and cv_passed
        and heldout_native_fp == 0
        and insample_confident_wrong == 0
    )

    active_tables = candidate_tables if calibrated else PLACEHOLDER
    confident_wrong = max(heldout_native_fp, insample_confident_wrong) if cv_folds else insample_confident_wrong
    worst_shot_margin = heldout_worst_shot_margin if cv_folds else full_ev["worst_shot_margin"]
    r3_bound = rule_of_three_bound(sample_hours)

    return {
        "depth": depth,
        "calibrated": calibrated,
        "source_kind": "calibrated" if calibrated else "placeholder",
        "gate_reason": "ok" if calibrated else "; ".join(gate_reasons),
        "native_shots": native_shots,
        "distinct_masters": len(depth_masters),
        "sample_hours": sample_hours,
        "rule_of_three_bound_per_hour": r3_bound,
        "confident_wrong_count": confident_wrong,
        "heldout_native_fp": heldout_native_fp,
        "insample_confident_wrong_count": insample_confident_wrong,
        "worst_shot_margin": worst_shot_margin,
        "c1": c1,
        "auc_all": heldout_auc_all,
        "auc_hd": heldout_auc_hd,
        "heldout_auc_all": heldout_auc_all,
        "heldout_auc_hd": heldout_auc_hd,
        "n_pos_hd": len(pooled_pos_hd),
        "n_neg_hd": len(pooled_neg_hd),
        "cv_passed": cv_passed,
        "cv_failure_reason": cv_failure_reason,
        "cv_folds": cv_folds,
        "tables": active_tables,
        "classes": class_diag,
        "g_phase_a_passed": bool(
            calibrated and cv_passed and heldout_native_fp == 0 and confident_wrong == 0 and native_shots >= min_native_shots
        ),
    }


def calibrate_all_depths(items, segs, min_native_shots=DEFAULT_MIN_NATIVE_SHOTS,
                         min_null_per_class=DEFAULT_MIN_NULL_PER_CLASS,
                         min_alt_per_class=DEFAULT_MIN_ALT_PER_CLASS,
                         cv_seed=DEFAULT_CV_SEED):
    results = {}
    for d in DEPTHS:
        results[d] = fit_depth_stratum(
            d, items, segs, min_native_shots, min_null_per_class, min_alt_per_class, cv_seed=cv_seed
        )

    unknown_items = sum(1 for it in items if it.depth not in DEPTHS)
    n8 = results["N8"]
    native_fps_by_depth = {d: results[d]["confident_wrong_count"] for d in DEPTHS}
    g8_reasons = []
    if not n8["calibrated"]:
        g8_reasons.append("N8 uncalibrated (%s)" % n8["gate_reason"])
    if not n8["cv_passed"]:
        g8_reasons.append("N8 2-fold CV failed (%s)" % n8["cv_failure_reason"])
    if len(n8["cv_folds"]) != 2:
        g8_reasons.append("missing N8 2-fold held-out HD splits")
    else:
        for f in n8["cv_folds"]:
            if not (f["n_pos_hd"] > 0 and f["n_neg_hd"] > 0 and math.isfinite(f["auc_hd"])):
                g8_reasons.append(
                    "missing N8 HD (1/2) samples on held-out fold %d (pos=%d, neg=%d)"
                    % (f["test_fold"], f["n_pos_hd"], f["n_neg_hd"])
                )
            elif f["auc_hd"] < G8BIT_MIN_HD_AUC:
                g8_reasons.append(
                    "N8 held-out fold %d HD AUC %.4f < %.2f" % (f["test_fold"], f["auc_hd"], G8BIT_MIN_HD_AUC)
                )
    if not (n8["n_pos_hd"] > 0 and n8["n_neg_hd"] > 0 and math.isfinite(n8["heldout_auc_hd"])):
        if not any("missing N8 HD" in r for r in g8_reasons):
            g8_reasons.append("missing N8 HD (1/2) samples (pos=%d, neg=%d)" % (n8["n_pos_hd"], n8["n_neg_hd"]))
    elif n8["heldout_auc_hd"] < G8BIT_MIN_HD_AUC:
        g8_reasons.append("N8 held-out HD AUC %.4f < %.2f" % (n8["heldout_auc_hd"], G8BIT_MIN_HD_AUC))
    for d in DEPTHS:
        if results[d]["confident_wrong_count"] > 0:
            g8_reasons.append("%s native FPs %d > 0" % (d, results[d]["confident_wrong_count"]))
    g8_passed = len(g8_reasons) == 0
    g8_reason = "ok" if g8_passed else "; ".join(g8_reasons)

    report = {
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "min_native_shots": min_native_shots,
        "unknown_depth_items_excluded": unknown_items,
        "G-PhaseA": {
            d: {
                "calibrated": results[d]["calibrated"],
                "source_kind": results[d]["source_kind"],
                "gate_reason": results[d]["gate_reason"],
                "native_shots": results[d]["native_shots"],
                "distinct_masters": results[d]["distinct_masters"],
                "sample_hours": results[d]["sample_hours"],
                "rule_of_three_bound_per_hour": results[d]["rule_of_three_bound_per_hour"],
                "confident_wrong_count": results[d]["confident_wrong_count"],
                "heldout_native_fp": results[d]["heldout_native_fp"],
                "worst_shot_margin": results[d]["worst_shot_margin"],
                "cv_passed": results[d]["cv_passed"],
                "cv_failure_reason": results[d]["cv_failure_reason"],
                "cv_folds": results[d]["cv_folds"],
                "passed": results[d]["g_phase_a_passed"],
            }
            for d in DEPTHS
        },
        "G-8bit": {
            "n8_hd_auc": n8["heldout_auc_hd"],
            "n8_overall_auc": n8["heldout_auc_all"],
            "heldout_auc_hd": n8["heldout_auc_hd"],
            "heldout_auc_hd_folds": [f["auc_hd"] for f in n8["cv_folds"]],
            "n8_hd_pos_samples": n8["n_pos_hd"],
            "n8_hd_neg_samples": n8["n_neg_hd"],
            "native_fps_per_depth": native_fps_by_depth,
            "cv_passed": n8["cv_passed"],
            "cv_failure_reason": n8["cv_failure_reason"],
            "g8_reason": g8_reason,
            "passed": g8_passed,
        },
        "depths": results,
    }
    return report


# ----------------------------------------------------------------------------------------------------------
# C++14 header, Markdown, and JSON writers
# ----------------------------------------------------------------------------------------------------------
def render_cpp_header(report):
    lines = [
        "// Copyright(C) 2026 VP-V contributors",
        "// SPDX-License-Identifier: GPL-3.0-only",
        "// Generated by generate_null_tables.py (Stage S3 bit-depth-stratified NullTables).",
        "// Uncalibrated or under-sampled depths ALWAYS emit DepthNullTable::PlaceholderFor(...).",
        "#pragma once",
        "",
        '#include "NullTables.h"',
        "",
        "namespace SourceIdentity",
        "{",
        "\tnamespace Chain",
        "\t{",
        "\t\tinline DepthNullTableSet GeneratedDepthNullTables()",
        "\t\t{",
        "\t\t\tDepthNullTableSet set = DepthNullTableSet::PlaceholderOnly();",
    ]
    for d in DEPTHS:
        info = report["depths"][d]
        field = DEPTH_CPP_FIELD[d]
        enum_val = DEPTH_CPP_ENUM[d]
        if not info["calibrated"]:
            lines.append("\t\t\t// %s: uncalibrated (%s)" % (d, info["gate_reason"]))
            lines.append("\t\t\tset.%s = DepthNullTable::PlaceholderFor(%s);" % (field, enum_val))
        else:
            lines.append("\t\t\t// %s: calibrated (shots=%d, hours=%.6f, 3/H=%.6f/h)" % (
                d, info["native_shots"], info["sample_hours"], info["rule_of_three_bound_per_hour"]))
            lines.append("\t\t\t{")
            lines.append("\t\t\t\tDepthNullTable& t = set.%s;" % field)
            lines.append("\t\t\t\tt.depthClass = %s;" % enum_val)
            lines.append("\t\t\t\tt.sourceKind = NullTableSourceKind::Calibrated;")
            lines.append("\t\t\t\tt.calibrated = true;")
            lines.append("\t\t\t\tt.sampleHours = %.10g;" % info["sample_hours"])
            lines.append("\t\t\t\tt.nativeShots = %d;" % info["native_shots"])
            lines.append("\t\t\t\tt.tables.placeholder = false;")
            for c in range(4):
                m0, s0 = info["tables"]["null"][c]
                m1, s1 = info["tables"]["alt"][c]
                lines.append("\t\t\t\tt.tables.nul[%s].mean = %.10g; t.tables.nul[%s].sd = %.10g;" % (
                    CLASS_CPP[c], m0, CLASS_CPP[c], s0))
                lines.append("\t\t\t\tt.tables.alt[%s].mean = %.10g; t.tables.alt[%s].sd = %.10g;" % (
                    CLASS_CPP[c], m1, CLASS_CPP[c], s1))
            lines.append("\t\t\t}")
    lines.extend([
        "\t\t\treturn set;",
        "\t\t}",
        "\t}",
        "}",
        "",
    ])
    return "\n".join(lines)


def _fmt_num(v, fmt="%.4f"):
    if v is None or not math.isfinite(v):
        return "inf" if (v is not None and math.isinf(v)) else "nan"
    return fmt % v


def render_markdown_report(report):
    lines = [
        "# Stage S3 NullTables Calibration & Gate Report",
        "",
        "- Generated (UTC): `%s`" % report["generated_utc"],
        "- Calibration gate minimum native shots: `%d`" % report["min_native_shots"],
        "- Unknown-depth items excluded: `%d`" % report["unknown_depth_items_excluded"],
        "",
        "## G-PhaseA (Per-Depth Held-Out 2-Fold CV & Native False-Positive Bounds)",
        "",
        "| Depth | Source | Masters | Native Shots | Hours | Rule-of-Three (3/H) | Held-Out FPs | Worst-Shot Margin | 2-Fold CV | Gate |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |",
    ]
    for d in DEPTHS:
        g = report["G-PhaseA"][d]
        lines.append(
            "| %s | %s | %d | %d | %s | %s/h | %d | %s | %s (%s) | %s (%s) |" % (
                d,
                g["source_kind"],
                g["distinct_masters"],
                g["native_shots"],
                _fmt_num(g["sample_hours"], "%.4f"),
                _fmt_num(g["rule_of_three_bound_per_hour"], "%.4f"),
                g["heldout_native_fp"],
                _fmt_num(g["worst_shot_margin"], "%.3f"),
                "PASS" if g["cv_passed"] else "FAIL",
                g["cv_failure_reason"],
                "PASS" if g["passed"] else "FAIL",
                g["gate_reason"],
            )
        )
    lines.extend([
        "",
        "### Held-Out 2-Fold Cross-Validation Detail (Disjoint By Master)",
        "",
        "| Depth | Train->Test Fold | Test Masters | Held-Out Native Segs | Held-Out FPs | Held-Out HD AUC | Fold Verdict |",
        "| --- | --- | ---: | ---: | ---: | ---: | --- |",
    ])
    for d in DEPTHS:
        g = report["G-PhaseA"][d]
        if not g["cv_folds"]:
            lines.append("| %s | n/a | 0 | 0 | 0 | nan | FAIL (%s) |" % (d, g["cv_failure_reason"]))
            continue
        for f in g["cv_folds"]:
            lines.append(
                "| %s | %d->%d | %d | %d | %d | %s | %s (%s) |" % (
                    d,
                    f["train_fold"],
                    f["test_fold"],
                    len(f["test_masters"]),
                    f["native_segs"],
                    f["native_fp"],
                    _fmt_num(f["auc_hd"], "%.5f"),
                    "PASS" if f["passed"] else "FAIL",
                    f["failure_reason"],
                )
            )
    g8 = report["G-8bit"]
    fold_aucs_str = ", ".join(_fmt_num(x, "%.5f") for x in g8.get("heldout_auc_hd_folds", [])) or "none"
    lines.extend([
        "",
        "## G-8bit (8-Bit Held-Out HD Discrimination & Native FP Safety)",
        "",
        "- N8 held-out HD (1/2) Mann-Whitney U AUC: `%s` (folds: `[%s]`, pos=%d, neg=%d)" % (
            _fmt_num(g8["n8_hd_auc"], "%.5f"), fold_aucs_str, g8["n8_hd_pos_samples"], g8["n8_hd_neg_samples"]),
        "- N8 held-out overall Mann-Whitney U AUC: `%s`" % _fmt_num(g8["n8_overall_auc"], "%.5f"),
        "- N8 2-fold CV: `%s` (%s)" % ("PASS" if g8["cv_passed"] else "FAIL", g8["cv_failure_reason"]),
        "- Native FPs per depth: `N8=%d, N10=%d, N12=%d`" % (
            g8["native_fps_per_depth"]["N8"],
            g8["native_fps_per_depth"]["N10"],
            g8["native_fps_per_depth"]["N12"],
        ),
        "- Gate verdict: `%s` (%s)" % ("PASS" if g8["passed"] else "FAIL", g8.get("g8_reason", "ok")),
        "",
    ])
    return "\n".join(lines)


def _json_safe(obj):
    if isinstance(obj, float):
        if math.isnan(obj):
            return None
        if math.isinf(obj):
            return "inf" if obj > 0 else "-inf"
        return obj
    if isinstance(obj, dict):
        return {k: _json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_json_safe(v) for v in obj]
    return obj


def write_outputs(report, out_header=None, out_md=None, out_json=None):
    if out_header:
        os.makedirs(os.path.dirname(os.path.abspath(out_header)), exist_ok=True)
        with open(out_header, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_cpp_header(report))
    if out_md:
        os.makedirs(os.path.dirname(os.path.abspath(out_md)), exist_ok=True)
        with open(out_md, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_markdown_report(report))
    if out_json:
        os.makedirs(os.path.dirname(os.path.abspath(out_json)), exist_ok=True)
        with open(out_json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(_json_safe(report), fh, indent=2, sort_keys=True)
            fh.write("\n")


# ----------------------------------------------------------------------------------------------------------
# Deterministic end-to-end --selftest (Dump v4 + Dump v3 + negative calibration gate + isolation checks)
# ----------------------------------------------------------------------------------------------------------
def _write_synthetic_v4_pair(directory, tag, step_codes, output_bits, num_shots, evals_per_shot,
                             truth_nx, truth_ny, rng, inf_x=1, inf_y=1, fp_burst_shots=0):
    evals_path = os.path.join(directory, tag + ".b_evals.csv")
    sizes_path = os.path.join(directory, tag + ".b_sizes.csv")
    # Candidate sizes on N=1920 covering all 4 NullTables classes:
    #   960  (1/2 -> K8), 1320 (11/16 -> K16), 1280 (2/3 -> Rational), 1100 (Generic)
    cands_x = [(960, 1920), (1320, 1920), (1280, 1920), (1100, 1920)]
    cands_y = [(540, 1080), (743, 1080), (720, 1080), (618, 1080)]
    with open(evals_path, "w", encoding="utf-8", newline="") as fe, \
         open(sizes_path, "w", encoding="utf-8", newline="") as fs:
        ew = csv.writer(fe)
        sw = csv.writer(fs)
        ew.writerow(list(REQUIRED_V4_EVAL_COLS))
        sw.writerow(list(REQUIRED_V4_SIZE_COLS))
        seq = 1
        t_s = 0.0
        for shot in range(1, num_shots + 1):
            in_burst = fp_burst_shots > 0 and shot <= fp_burst_shots
            for _ in range(evals_per_shot):
                ew.writerow([seq, "%.3f" % t_s, shot, step_codes, output_bits, output_bits, inf_x, inf_y, 0, 0])
                for axis, cands, tn in (("x", cands_x, truth_nx), ("y", cands_y, truth_ny)):
                    for n, N in cands:
                        if in_burst and ((axis == "x" and n == 960) or (axis == "y" and n == 540)):
                            stat = -0.50
                        elif tn > 0 and abs(n - tn) <= 1:
                            stat = rng.gauss(-0.45, 0.04)
                        else:
                            stat = rng.gauss(-0.02, 0.02)
                        sw.writerow([seq, "%.3f" % t_s, shot, axis, N, n, 1, "%.6f" % stat, 32])
                seq += 1
                t_s += 2.0


def _write_synthetic_v3_csv(directory, filename, bit_depth, num_shots, truth_n, rng, master_name=None, reel_name=None):
    path = os.path.join(directory, filename)
    cands = [(960, 1920), (1320, 1920), (1280, 1920), (1100, 1920)]
    master_id = master_name or ("m_v3_%d" % bit_depth)
    reel_id = reel_name or ("v3_reel_%d" % bit_depth)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(list(REQUIRED_V3_COLS))
        seq = 1
        t_s = 0.0
        for shot in range(1, num_shots + 1):
            for _ in range(2):
                for n, N in cands:
                    cls_idx = classify_latent(n, N)
                    if truth_n > 0 and abs(n - truth_n) <= 1:
                        stat = rng.gauss(-0.42, 0.04)
                    else:
                        stat = rng.gauss(-0.02, 0.02)
                    w.writerow([
                        reel_id, "title", seq, "%.3f" % t_s, "x", N, n, CLASSES[cls_idx],
                        "%.6f" % stat, "exact", 0, 0.1, 24, 32, 1, 0.289, truth_n, bit_depth,
                        0, 0, shot, "%.6f" % stat, 1, master_id, 1, 0, shot,
                    ])
                seq += 1
                t_s += 2.0


def run_selftest():
    # L4: Verify Python contract constants match C++ headers with zero drift.
    verified_cpp = verify_cpp_contract_constants()
    assert verified_cpp is not None and len(verified_cpp) >= 20, "C++ contract constant verification did not locate headers"

    # HIGH-1: 8-bit HDMI-loop streams (step_codes == 1, output_bits == 8) MUST classify as N8, never N10.
    assert classify_bit_depth_from_step(1, 8) == "N8", "expected (1, 8) -> N8"
    assert classify_bit_depth_from_step(4, 0) == "N8"
    assert classify_bit_depth_from_step(1, 10) == "N10"
    assert classify_bit_depth_from_step(1, 12) == "N12"
    rng = random.Random(20261123)
    with tempfile.TemporaryDirectory() as tmp:
        v4_dir = os.path.join(tmp, "v4")
        os.makedirs(v4_dir)
        # 1. Well-sampled multi-master N8 native (>= 1500 shots total across 2 disjoint masters,
        #    one per CV fold) + multi-master N8 upscaled reels covering all 4 size classes in both folds.
        _write_synthetic_v4_pair(v4_dir, "n8_native_a", step_codes=4, output_bits=8, num_shots=760, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_native_b", step_codes=4, output_bits=8, num_shots=760, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_half_a", step_codes=4, output_bits=8, num_shots=25, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_half_b", step_codes=4, output_bits=8, num_shots=25, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_k16_a", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1320, truth_ny=743, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_k16_b", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1320, truth_ny=743, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_rat_a", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1280, truth_ny=720, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_rat_b", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1280, truth_ny=720, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_gen_a", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1100, truth_ny=618, rng=rng)
        _write_synthetic_v4_pair(v4_dir, "n8_up_gen_b", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1100, truth_ny=618, rng=rng)
        # 2. Under-sampled N10 native (only 50 shots < 1500) -> MUST NOT calibrate!
        _write_synthetic_v4_pair(v4_dir, "n10_under", step_codes=1, output_bits=10, num_shots=50, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng)
        # 3. Unpopulated N12 (0 shots) -> MUST NOT calibrate!

        manifest_path = os.path.join(tmp, "manifest.json")
        manifest = {
            "n8_native_a": {"truth_nx": 0, "truth_ny": 0, "master": "m_n8_a"},
            "n8_native_b": {"truth_nx": 0, "truth_ny": 0, "master": "m_n8_b"},
            "n8_up_half_a": {"truth_nx": 960, "truth_ny": 540, "master": "m_n8_a"},
            "n8_up_half_b": {"truth_nx": 960, "truth_ny": 540, "master": "m_n8_b"},
            "n8_up_k16_a": {"truth_nx": 1320, "truth_ny": 743, "master": "m_n8_a"},
            "n8_up_k16_b": {"truth_nx": 1320, "truth_ny": 743, "master": "m_n8_b"},
            "n8_up_rat_a": {"truth_nx": 1280, "truth_ny": 720, "master": "m_n8_a"},
            "n8_up_rat_b": {"truth_nx": 1280, "truth_ny": 720, "master": "m_n8_b"},
            "n8_up_gen_a": {"truth_nx": 1100, "truth_ny": 618, "master": "m_n8_a"},
            "n8_up_gen_b": {"truth_nx": 1100, "truth_ny": 618, "master": "m_n8_b"},
            "n10_under": {"truth_nx": 0, "truth_ny": 0, "master": "m_n10"},
        }
        with open(manifest_path, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh)

        items, segs, load_info = load_all_dumps([v4_dir], manifest_path)
        assert load_info["v4_pairs"] == 11, "expected 11 v4 pairs, got %r" % (load_info,)

        report = calibrate_all_depths(items, segs, min_native_shots=DEFAULT_MIN_NATIVE_SHOTS)

        # Verify N8 calibrated cleanly with both held-out CV folds passing
        n8 = report["G-PhaseA"]["N8"]
        assert n8["calibrated"] is True, "N8 must calibrate: %r" % (n8,)
        assert n8["cv_passed"] is True, "N8 2-fold CV must pass: %r" % (n8,)
        assert len(n8["cv_folds"]) == 2
        assert all(f["passed"] and f["native_fp"] == 0 for f in n8["cv_folds"])
        assert n8["source_kind"] == "calibrated"
        assert n8["native_shots"] == 1520, "expected 1520 N8 native shots, got %d" % n8["native_shots"]
        assert n8["confident_wrong_count"] == 0, "expected 0 N8 FPs, got %d" % n8["confident_wrong_count"]
        assert n8["heldout_native_fp"] == 0
        assert n8["passed"] is True
        expected_r3 = 3.0 / n8["sample_hours"]
        assert abs(n8["rule_of_three_bound_per_hour"] - expected_r3) < 1e-12

        # Negative control 1: Under-sampled N10 MUST emit Placeholder, NEVER calibrated
        n10 = report["G-PhaseA"]["N10"]
        assert n10["calibrated"] is False, "under-sampled N10 must NOT calibrate"
        assert n10["cv_passed"] is False, "single-master N10 must fail 2-fold CV visibly"
        assert n10["source_kind"] == "placeholder"
        assert n10["native_shots"] == 50, "strict depth isolation broken for N10: %d" % n10["native_shots"]
        assert n10["passed"] is False

        # Negative control 2: Unpopulated N12 MUST emit Placeholder, NEVER calibrated
        n12 = report["G-PhaseA"]["N12"]
        assert n12["calibrated"] is False, "unpopulated N12 must NOT calibrate"
        assert n12["cv_passed"] is False
        assert n12["source_kind"] == "placeholder"
        assert n12["native_shots"] == 0, "strict depth isolation broken for N12: %d" % n12["native_shots"]
        assert math.isinf(n12["rule_of_three_bound_per_hour"])
        assert n12["passed"] is False

        # Verify G-8bit exact held-out Mann-Whitney U AUC on BOTH folds
        g8 = report["G-8bit"]
        assert math.isfinite(g8["n8_hd_auc"]) and g8["n8_hd_auc"] > 0.99, "bad N8 HD AUC: %r" % (g8,)
        assert len(g8["heldout_auc_hd_folds"]) == 2 and all(a > 0.99 for a in g8["heldout_auc_hd_folds"])
        assert g8["passed"] is True

        # Verify emitted C++ header, Markdown, and JSON
        out_hdr = os.path.join(tmp, "NullTables.generated.h")
        out_md = os.path.join(tmp, "report.md")
        out_json = os.path.join(tmp, "report.json")
        write_outputs(report, out_hdr, out_md, out_json)

        with open(out_hdr, "r", encoding="utf-8") as fh:
            hdr_text = fh.read()
        assert "GeneratedDepthNullTables()" in hdr_text
        assert "NullTableSourceKind::Calibrated" in hdr_text
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N10)" in hdr_text
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N12)" in hdr_text
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N8)" not in hdr_text

        with open(out_json, "r", encoding="utf-8") as fh:
            parsed_json = json.load(fh)
        assert parsed_json["G-PhaseA"]["N8"]["calibrated"] is True
        assert parsed_json["G-PhaseA"]["N8"]["cv_passed"] is True
        assert parsed_json["G-PhaseA"]["N10"]["calibrated"] is False
        assert parsed_json["G-PhaseA"]["N12"]["calibrated"] is False

        with open(out_md, "r", encoding="utf-8") as fh:
            md_text = fh.read()
        assert "G-PhaseA" in md_text and "G-8bit" in md_text and "Held-Out 2-Fold" in md_text

        # Also verify v3 cal-dump parser & strict depth stratification (multi-master per depth,
        # with upscaled alt reels covering all 4 classes in BOTH CV folds for N10 and N12).
        v3_dir = os.path.join(tmp, "v3")
        os.makedirs(v3_dir)
        for bd in (10, 12):
            for suf in ("a", "b"):
                m_name = "m_v3_%d_%s" % (bd, suf)
                _write_synthetic_v3_csv(v3_dir, "cal_dump_%dbit_%s.csv" % (bd, suf), bit_depth=bd, num_shots=30,
                                        truth_n=0, rng=rng, master_name=m_name, reel_name="v3_reel_%d_%s" % (bd, suf))
                for cls_tag, tn in (("half", 960), ("k16", 1320), ("rat", 1280), ("gen", 1100)):
                    _write_synthetic_v3_csv(v3_dir, "cal_dump_%dbit_up_%s_%s.csv" % (bd, cls_tag, suf), bit_depth=bd,
                                            num_shots=15, truth_n=tn, rng=rng, master_name=m_name,
                                            reel_name="v3_up_%d_%s_%s" % (bd, cls_tag, suf))
        v3_items, v3_segs, v3_info = load_all_dumps([v3_dir])
        assert v3_info["v3_files"] == 20
        v3_report = calibrate_all_depths(v3_items, v3_segs, min_native_shots=50)
        assert v3_report["G-PhaseA"]["N10"]["calibrated"] is True
        assert v3_report["G-PhaseA"]["N10"]["cv_passed"] is True
        assert v3_report["G-PhaseA"]["N12"]["calibrated"] is True
        assert v3_report["G-PhaseA"]["N12"]["cv_passed"] is True
        assert v3_report["G-PhaseA"]["N8"]["calibrated"] is False
        assert v3_report["G-PhaseA"]["N8"]["native_shots"] == 0

        # Negative control 3 (HIGH-1 & MEDIUM-1): uninformative v4 rows MUST be excluded AND contribute 0 shots / 0 hours
        uninf_dir = os.path.join(tmp, "uninf")
        os.makedirs(uninf_dir)
        _write_synthetic_v4_pair(uninf_dir, "n8_uninf_both", step_codes=4, output_bits=8, num_shots=5, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng, inf_x=0, inf_y=0)
        uninf_items, uninf_segs, _ = load_all_dumps([uninf_dir])
        assert len(uninf_items) == 0, "uninformative v4 rows leaked into calibration: %d" % len(uninf_items)
        uninf_rep = calibrate_all_depths(uninf_items, uninf_segs, min_native_shots=1)
        assert uninf_rep["G-PhaseA"]["N8"]["native_shots"] == 0, "uninformative-only reel inflated native_shots"
        assert uninf_rep["G-PhaseA"]["N8"]["sample_hours"] == 0.0, "uninformative-only reel inflated sample_hours"
        uninf_half_dir = os.path.join(tmp, "uninf_half")
        os.makedirs(uninf_half_dir)
        _write_synthetic_v4_pair(uninf_half_dir, "n8_uninf_y", step_codes=4, output_bits=8, num_shots=3, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng, inf_x=1, inf_y=0)
        uninf_half_items, _, _ = load_all_dumps([uninf_half_dir])
        assert len(uninf_half_items) > 0 and all(it.axis == "x" for it in uninf_half_items), \
            "informative_y=0 rows leaked into calibration"

        # Negative control 4 (HIGH-2): high-FP native reel MUST revoke Calibrated and emit PlaceholderFor(...)
        fp_dir = os.path.join(tmp, "fp_revocation")
        os.makedirs(fp_dir)
        _write_synthetic_v4_pair(fp_dir, "n8_fp_nat_a", step_codes=4, output_bits=8, num_shots=202, evals_per_shot=3,
                                 truth_nx=0, truth_ny=0, rng=rng, fp_burst_shots=2)
        _write_synthetic_v4_pair(fp_dir, "n8_fp_nat_b", step_codes=4, output_bits=8, num_shots=200, evals_per_shot=3,
                                 truth_nx=0, truth_ny=0, rng=rng, fp_burst_shots=0)
        _write_synthetic_v4_pair(fp_dir, "n8_fp_up_a", step_codes=4, output_bits=8, num_shots=30, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        _write_synthetic_v4_pair(fp_dir, "n8_fp_up_b", step_codes=4, output_bits=8, num_shots=30, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        fp_manifest_path = os.path.join(tmp, "fp_manifest.json")
        with open(fp_manifest_path, "w", encoding="utf-8") as fh:
            json.dump({
                "n8_fp_nat_a": {"truth_nx": 0, "truth_ny": 0, "master": "m_fp_a"},
                "n8_fp_nat_b": {"truth_nx": 0, "truth_ny": 0, "master": "m_fp_b"},
                "n8_fp_up_a": {"truth_nx": 960, "truth_ny": 540, "master": "m_fp_a"},
                "n8_fp_up_b": {"truth_nx": 960, "truth_ny": 540, "master": "m_fp_b"},
            }, fh)
        fp_items, fp_segs, _ = load_all_dumps([fp_dir], fp_manifest_path)
        fp_report = calibrate_all_depths(fp_items, fp_segs, min_native_shots=300)
        fp_n8 = fp_report["G-PhaseA"]["N8"]
        assert fp_n8["confident_wrong_count"] >= 1, "expected native FP in burst negative control: %r" % (fp_n8,)
        assert fp_n8["heldout_native_fp"] >= 1, "expected held-out native FP in burst negative control: %r" % (fp_n8,)
        assert fp_n8["cv_passed"] is False
        assert fp_n8["calibrated"] is False, "failing G-PhaseA stratum MUST revoke calibrated: %r" % (fp_n8,)
        assert fp_n8["source_kind"] == "placeholder"
        assert "confident_wrong" in fp_n8["gate_reason"] or "native_fp" in fp_n8["gate_reason"]
        fp_hdr = render_cpp_header(fp_report)
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N8)" in fp_hdr
        assert "NullTableSourceKind::Calibrated" not in fp_hdr

        # Negative control 5 (MEDIUM-2): missing N8 HD (1/2) samples MUST fail G-8bit (never substitute auc_all)
        no_hd_items = [it for it in items if it.reel not in ("n8_up_half_a", "n8_up_half_b")]
        no_hd_report = calibrate_all_depths(no_hd_items, segs, min_native_shots=DEFAULT_MIN_NATIVE_SHOTS)
        assert no_hd_report["G-8bit"]["n8_hd_pos_samples"] == 0
        assert no_hd_report["G-8bit"]["passed"] is False, "G-8bit must NOT pass without N8 HD samples"
        assert "missing N8 HD" in no_hd_report["G-8bit"]["g8_reason"]

        # Negative control 6 (HIGH-2): clean calibrated N8 + FP-failing N10 MUST fail G-8bit (cross-depth FP check active!)
        cross_dir = os.path.join(tmp, "cross_fp")
        os.makedirs(cross_dir)
        _write_synthetic_v4_pair(cross_dir, "n10_fp_nat_a", step_codes=1, output_bits=10, num_shots=202, evals_per_shot=3,
                                 truth_nx=0, truth_ny=0, rng=rng, fp_burst_shots=2)
        _write_synthetic_v4_pair(cross_dir, "n10_fp_nat_b", step_codes=1, output_bits=10, num_shots=200, evals_per_shot=3,
                                 truth_nx=0, truth_ny=0, rng=rng, fp_burst_shots=0)
        _write_synthetic_v4_pair(cross_dir, "n10_fp_up_a", step_codes=1, output_bits=10, num_shots=30, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        _write_synthetic_v4_pair(cross_dir, "n10_fp_up_b", step_codes=1, output_bits=10, num_shots=30, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        cross_manifest_path = os.path.join(tmp, "cross_manifest.json")
        with open(cross_manifest_path, "w", encoding="utf-8") as fh:
            json.dump({
                "n10_fp_nat_a": {"truth_nx": 0, "truth_ny": 0, "master": "m_n10_fp_a"},
                "n10_fp_nat_b": {"truth_nx": 0, "truth_ny": 0, "master": "m_n10_fp_b"},
                "n10_fp_up_a": {"truth_nx": 960, "truth_ny": 540, "master": "m_n10_fp_a"},
                "n10_fp_up_b": {"truth_nx": 960, "truth_ny": 540, "master": "m_n10_fp_b"},
            }, fh)
        n10_fp_items, n10_fp_segs, _ = load_all_dumps([cross_dir], cross_manifest_path)
        n8_only_items = [it for it in items if it.depth == "N8"]
        n8_only_segs = [sg for sg in segs if sg["depth"] == "N8"]
        cross_report = calibrate_all_depths(n8_only_items + n10_fp_items, n8_only_segs + n10_fp_segs, min_native_shots=300)
        assert cross_report["G-PhaseA"]["N8"]["calibrated"] is True
        assert cross_report["G-PhaseA"]["N10"]["confident_wrong_count"] >= 1
        assert cross_report["G-PhaseA"]["N10"]["calibrated"] is False
        assert cross_report["G-8bit"]["passed"] is False, "G-8bit MUST fail when N10 has native FPs"
        assert "N10 native FPs" in cross_report["G-8bit"]["g8_reason"]

        # Negative control 7 (H3 Red-Team): Overfitted multi-master dataset where IN-SAMPLE validation PASSES
        # (0 in-sample native FPs, in-sample HD AUC > 0.95) yet 2-fold held-out CV by master FAILS visibly!
        # Master A has tight clean native k/8 baseline (~ -0.01, sd 0.008); Master B has a genuinely shifted
        # native k/8 coding-grid distribution (~ -0.18, sd 0.02). Combined in-sample null absorbs both modes
        # (0 in-sample FPs), but training on Master A alone causes held-out Master B to trigger native FPs!
        ovf_dir = os.path.join(tmp, "h3_overfit")
        os.makedirs(ovf_dir)

        def _write_custom_v4(directory, tag, num_shots, evals_per_shot, truth_nx, truth_ny,
                             k8_null_mean, k8_null_sd, alt_mean, alt_sd):
            ev_p = os.path.join(directory, tag + ".b_evals.csv")
            sz_p = os.path.join(directory, tag + ".b_sizes.csv")
            cx = [(960, 1920), (1320, 1920), (1280, 1920), (1100, 1920)]
            cy = [(540, 1080), (743, 1080), (720, 1080), (618, 1080)]
            with open(ev_p, "w", encoding="utf-8", newline="") as fe, \
                 open(sz_p, "w", encoding="utf-8", newline="") as fs:
                ew = csv.writer(fe)
                sw = csv.writer(fs)
                ew.writerow(list(REQUIRED_V4_EVAL_COLS))
                sw.writerow(list(REQUIRED_V4_SIZE_COLS))
                seq_i = 1
                t_sec = 0.0
                for sh in range(1, num_shots + 1):
                    for _ in range(evals_per_shot):
                        ew.writerow([seq_i, "%.3f" % t_sec, sh, 4, 8, 8, 1, 1, 0, 0])
                        for ax_name, clist, tn in (("x", cx, truth_nx), ("y", cy, truth_ny)):
                            for cand_n, cand_N in clist:
                                if tn > 0 and abs(cand_n - tn) <= 1:
                                    st_val = rng.gauss(alt_mean, alt_sd)
                                elif classify_latent(cand_n, cand_N) == 0:
                                    st_val = rng.gauss(k8_null_mean, k8_null_sd)
                                else:
                                    st_val = rng.gauss(-0.02, 0.02)
                                sw.writerow([seq_i, "%.3f" % t_sec, sh, ax_name, cand_N, cand_n, 1, "%.6f" % st_val, 32])
                        seq_i += 1
                        t_sec += 2.0

        _write_custom_v4(ovf_dir, "ovf_nat_a", 200, 3, 0, 0, k8_null_mean=-0.01, k8_null_sd=0.008, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_nat_b", 200, 3, 0, 0, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_a", 25, 2, 960, 540, k8_null_mean=-0.01, k8_null_sd=0.008, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_b", 25, 2, 960, 540, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_k16_a", 20, 2, 1320, 743, k8_null_mean=-0.01, k8_null_sd=0.008, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_k16_b", 20, 2, 1320, 743, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_rat_a", 20, 2, 1280, 720, k8_null_mean=-0.01, k8_null_sd=0.008, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_rat_b", 20, 2, 1280, 720, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_gen_a", 20, 2, 1100, 618, k8_null_mean=-0.01, k8_null_sd=0.008, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(ovf_dir, "ovf_up_gen_b", 20, 2, 1100, 618, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        ovf_manifest = os.path.join(tmp, "ovf_manifest.json")
        with open(ovf_manifest, "w", encoding="utf-8") as fh:
            json.dump({
                "ovf_nat_a": {"truth_nx": 0, "truth_ny": 0, "master": "m_ovf_a"},
                "ovf_nat_b": {"truth_nx": 0, "truth_ny": 0, "master": "m_ovf_b"},
                "ovf_up_a": {"truth_nx": 960, "truth_ny": 540, "master": "m_ovf_a"},
                "ovf_up_b": {"truth_nx": 960, "truth_ny": 540, "master": "m_ovf_b"},
                "ovf_up_k16_a": {"truth_nx": 1320, "truth_ny": 743, "master": "m_ovf_a"},
                "ovf_up_k16_b": {"truth_nx": 1320, "truth_ny": 743, "master": "m_ovf_b"},
                "ovf_up_rat_a": {"truth_nx": 1280, "truth_ny": 720, "master": "m_ovf_a"},
                "ovf_up_rat_b": {"truth_nx": 1280, "truth_ny": 720, "master": "m_ovf_b"},
                "ovf_up_gen_a": {"truth_nx": 1100, "truth_ny": 618, "master": "m_ovf_a"},
                "ovf_up_gen_b": {"truth_nx": 1100, "truth_ny": 618, "master": "m_ovf_b"},
            }, fh)
        ovf_items, ovf_segs, _ = load_all_dumps([ovf_dir], ovf_manifest)

        # In-sample negative control proof: fitting & replaying on the SAME combined data PASSES (0 FPs, AUC > 0.95)!
        ovf_insample_tbls, _, ovf_fit_ok, _ = _fit_tables_from_items(
            ovf_items, DEFAULT_MIN_NULL_PER_CLASS, DEFAULT_MIN_ALT_PER_CLASS
        )
        assert ovf_fit_ok is True, "fixture: combined overfit dataset fits cleanly"
        ovf_insample_ev = evaluate_heldout(ovf_items, ovf_segs, ovf_insample_tbls, compute_c1())
        assert ovf_insample_ev["native_fp"] == 0, \
            "negative control: naive in-sample replay must pass with 0 FPs, got %d" % ovf_insample_ev["native_fp"]
        assert math.isfinite(ovf_insample_ev["auc_hd"]) and ovf_insample_ev["auc_hd"] > 0.95, \
            "negative control: naive in-sample HD AUC must pass (> 0.95), got %r" % (ovf_insample_ev["auc_hd"],)

        # Protected 2-fold held-out CV by master MUST FAIL visibly and emit PlaceholderFor(...)!
        ovf_report = calibrate_all_depths(ovf_items, ovf_segs, min_native_shots=300)
        ovf_n8 = ovf_report["depths"]["N8"]
        assert ovf_n8["insample_confident_wrong_count"] == 0, \
            "negative control: in-sample replay stays 0 FPs while held-out fails"
        assert ovf_n8["heldout_native_fp"] > 0, \
            "H3: held-out 2-fold CV must catch cross-master overfit native FPs: %r" % (ovf_n8,)
        assert ovf_n8["cv_passed"] is False, "H3: overfitted dataset MUST fail cv_passed"
        assert ovf_n8["g_phase_a_passed"] is False, "H3: overfitted dataset MUST fail g_phase_a_passed"
        assert ovf_n8["calibrated"] is False, "H3: overfitted dataset MUST NOT calibrate"
        assert ovf_n8["source_kind"] == "placeholder"
        assert ovf_report["G-8bit"]["passed"] is False, "H3: G-8bit MUST fail when held-out CV fails"
        ovf_hdr = render_cpp_header(ovf_report)
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N8)" in ovf_hdr, \
            "H3: overfitted N8 must emit PlaceholderFor(BitDepthClass::N8)"
        assert "NullTableSourceKind::Calibrated" not in ovf_hdr

        # Negative control 8 (H3 Red-Team): < 2 distinct masters (single-master dataset with >= 1500 native shots,
        # 0 in-sample FPs, and high in-sample AUC) MUST FAIL held-out CV visibly — NEVER silently fall back to in-sample!
        single_dir = os.path.join(tmp, "h3_single_master")
        os.makedirs(single_dir)
        _write_synthetic_v4_pair(single_dir, "sm_nat", step_codes=4, output_bits=8, num_shots=1520, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng)
        _write_synthetic_v4_pair(single_dir, "sm_up_half", step_codes=4, output_bits=8, num_shots=30, evals_per_shot=2,
                                 truth_nx=960, truth_ny=540, rng=rng)
        _write_synthetic_v4_pair(single_dir, "sm_up_k16", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1320, truth_ny=743, rng=rng)
        _write_synthetic_v4_pair(single_dir, "sm_up_rat", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1280, truth_ny=720, rng=rng)
        _write_synthetic_v4_pair(single_dir, "sm_up_gen", step_codes=4, output_bits=8, num_shots=20, evals_per_shot=2,
                                 truth_nx=1100, truth_ny=618, rng=rng)
        sm_manifest = os.path.join(tmp, "sm_manifest.json")
        with open(sm_manifest, "w", encoding="utf-8") as fh:
            json.dump({
                "sm_nat": {"truth_nx": 0, "truth_ny": 0, "master": "m_single_only"},
                "sm_up_half": {"truth_nx": 960, "truth_ny": 540, "master": "m_single_only"},
                "sm_up_k16": {"truth_nx": 1320, "truth_ny": 743, "master": "m_single_only"},
                "sm_up_rat": {"truth_nx": 1280, "truth_ny": 720, "master": "m_single_only"},
                "sm_up_gen": {"truth_nx": 1100, "truth_ny": 618, "master": "m_single_only"},
            }, fh)
        sm_items, sm_segs, _ = load_all_dumps([single_dir], sm_manifest)
        sm_report = calibrate_all_depths(sm_items, sm_segs, min_native_shots=DEFAULT_MIN_NATIVE_SHOTS)
        sm_n8 = sm_report["depths"]["N8"]
        assert sm_n8["native_shots"] >= DEFAULT_MIN_NATIVE_SHOTS, "fixture: single-master has >= 1500 native shots"
        assert sm_n8["insample_confident_wrong_count"] == 0, "fixture: single-master has 0 in-sample FPs"
        assert sm_n8["distinct_masters"] == 1
        assert sm_n8["cv_passed"] is False, "H3: < 2 distinct masters MUST fail held-out CV visibly"
        assert sm_n8["cv_failure_reason"] and sm_n8["cv_failure_reason"] != "ok", \
            "H3: single-master CV failure must report non-empty cv_failure_reason"
        assert sm_n8["calibrated"] is False, "H3: single-master dataset MUST NOT silently fall back to calibrated"
        assert sm_n8["g_phase_a_passed"] is False
        assert sm_n8["source_kind"] == "placeholder"
        assert sm_report["G-8bit"]["passed"] is False
        sm_hdr = render_cpp_header(sm_report)
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N8)" in sm_hdr
        assert "NullTableSourceKind::Calibrated" not in sm_hdr

        # Negative control 9 (NEW-M2 Red-Team): Dump v4 empirical floor depth classification (8-in-10 HDMI loop)
        # with same-input clean 10-bit (0.29) and textured 10-bit (12.0) negative controls.
        m2_dir = os.path.join(tmp, "new_m2_emp_floor")
        os.makedirs(m2_dir)

        def _write_v4_emp_floor(tag, emp_x, emp_y, vx, vy):
            ev_p = os.path.join(m2_dir, tag + ".b_evals.csv")
            sz_p = os.path.join(m2_dir, tag + ".b_sizes.csv")
            cols = list(REQUIRED_V4_EVAL_COLS) + [
                "emp_floor_x", "emp_floor_y", "emp_floor_valid_x", "emp_floor_valid_y"
            ]
            with open(ev_p, "w", encoding="utf-8", newline="") as fe, \
                 open(sz_p, "w", encoding="utf-8", newline="") as fs:
                ew = csv.writer(fe)
                sw = csv.writer(fs)
                ew.writerow(cols)
                sw.writerow(list(REQUIRED_V4_SIZE_COLS))
                for s_i in (1, 2):
                    ew.writerow([s_i, "%.3f" % (s_i * 2.0), 1, 1, 10, 10, 1, 1, 0, 0,
                                 "%.4f" % emp_x, "%.4f" % emp_y, vx, vy])
                    sw.writerow([s_i, "%.3f" % (s_i * 2.0), 1, "x", 1920, 960, 1, "-0.020000", 32])
                    sw.writerow([s_i, "%.3f" % (s_i * 2.0), 1, "y", 1080, 540, 1, "-0.020000", 32])
            return _load_dump_v4_pair(ev_p, sz_p, tag, {})

        loop_items, loop_segs = _write_v4_emp_floor("loop_8in10", 1.30, 1.35, 1, 1)
        assert len(loop_items) > 0 and all(it.depth == "N8" for it in loop_items), \
            "NEW-M2: step=1, output_bits=10, emp_floor=(1.30, 1.35) MUST classify as N8: %r" % ([it.depth for it in loop_items],)
        assert len(loop_segs) == 1 and loop_segs[0]["depth"] == "N8"

        # Same-input negative controls: clean 10-bit floor (0.29) and textured 10-bit (12.0 >= 4.0) stay N10!
        clean10_items, clean10_segs = _write_v4_emp_floor("clean_10bit", 0.29, 0.29, 1, 1)
        assert len(clean10_items) > 0 and all(it.depth == "N10" for it in clean10_items), \
            "NEW-M2 negative control: emp_floor=0.29 must classify as N10"
        assert len(clean10_segs) == 1 and clean10_segs[0]["depth"] == "N10"

        tex10_items, tex10_segs = _write_v4_emp_floor("tex_10bit", 12.0, 12.0, 1, 1)
        assert len(tex10_items) > 0 and all(it.depth == "N10" for it in tex10_items), \
            "NEW-M2 negative control: high-texture emp_floor=12.0 must classify as N10, not N8"
        assert len(tex10_segs) == 1 and tex10_segs[0]["depth"] == "N10"

        # Negative control 10 (TEST-L4 Red-Team): L4 C++ contract constant drift guard MUST raise AssertionError
        # when a C++ header constant drifts from Python's copy!
        real_chain_dir = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "chain"))
        drift_dir = os.path.join(tmp, "l4_drift_chain")
        os.makedirs(drift_dir)
        for hdr_name in ("ChainCommon.h", "ChainEvidence.h", "KernelBank.h", "ChainNullTables.h"):
            with open(os.path.join(real_chain_dir, hdr_name), "r", encoding="utf-8") as src_fh:
                hdr_body = src_fh.read()
            with open(os.path.join(drift_dir, hdr_name), "w", encoding="utf-8", newline="\n") as dst_fh:
                dst_fh.write(hdr_body)
        # Unmodified copy passes cleanly:
        assert verify_cpp_contract_constants(drift_dir) is not None
        # Mutate one constant (kMaxCandidates: 8 -> 9) in ChainCommon.h -> MUST raise AssertionError!
        cm_copy = os.path.join(drift_dir, "ChainCommon.h")
        with open(cm_copy, "r", encoding="utf-8") as fh:
            cm_orig = fh.read()
        cm_mut = re.sub(r"(constexpr\s+int\s+kMaxCandidates\s*=\s*)\d+(\s*;)", r"\g<1>99\g<2>", cm_orig, count=1)
        assert cm_mut != cm_orig, "TEST-L4 fixture: kMaxCandidates mutation must alter ChainCommon.h"
        with open(cm_copy, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(cm_mut)
        drift_caught = False
        try:
            verify_cpp_contract_constants(drift_dir)
        except AssertionError as exc:
            drift_caught = "CANDIDATE_COUNT" in str(exc) and "drift" in str(exc).lower()
        assert drift_caught, "TEST-L4: verify_cpp_contract_constants MUST raise AssertionError on drifted C++ constant"

        # Negative control 11 (CARBON-MED-1 Red-Team: 1-master-alt overfit bypass check):
        # Construct a 2-native-master dataset (>= 1500 native shots, 0 in-sample FPs) where ONLY 1 master
        # has upscaled alt segments (covering all 4 classes so the full-stratum fit succeeds), while the
        # second master has tight clean native ONLY (0 alt segments) — so one CV fold trains without alt.
        # Without MED-1, the alt-less training fold silently collapses alt = nul (LLR == 0), masking held-out
        # overfit FPs on the shifted master and falsely emitting NullTableSourceKind::Calibrated!
        alt1_dir = os.path.join(tmp, "med1_one_master_alt")
        os.makedirs(alt1_dir)
        _write_custom_v4(alt1_dir, "alt1_nat_a", 760, 3, 0, 0, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(alt1_dir, "alt1_nat_b", 760, 3, 0, 0, k8_null_mean=-0.01, k8_null_sd=0.008, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(alt1_dir, "alt1_up_half_a", 25, 2, 960, 540, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(alt1_dir, "alt1_up_k16_a", 20, 2, 1320, 743, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(alt1_dir, "alt1_up_rat_a", 20, 2, 1280, 720, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        _write_custom_v4(alt1_dir, "alt1_up_gen_a", 20, 2, 1100, 618, k8_null_mean=-0.18, k8_null_sd=0.020, alt_mean=-0.50, alt_sd=0.04)
        alt1_manifest = os.path.join(tmp, "alt1_manifest.json")
        with open(alt1_manifest, "w", encoding="utf-8") as fh:
            json.dump({
                "alt1_nat_a": {"truth_nx": 0, "truth_ny": 0, "master": "m_alt1_a"},
                "alt1_nat_b": {"truth_nx": 0, "truth_ny": 0, "master": "m_alt1_b"},
                "alt1_up_half_a": {"truth_nx": 960, "truth_ny": 540, "master": "m_alt1_a"},
                "alt1_up_k16_a": {"truth_nx": 1320, "truth_ny": 743, "master": "m_alt1_a"},
                "alt1_up_rat_a": {"truth_nx": 1280, "truth_ny": 720, "master": "m_alt1_a"},
                "alt1_up_gen_a": {"truth_nx": 1100, "truth_ny": 618, "master": "m_alt1_a"},
            }, fh)
        alt1_items, alt1_segs, _ = load_all_dumps([alt1_dir], alt1_manifest)

        # Same-input negative control: full-stratum fit HAS valid separated alt across all 4 classes & 0 in-sample FPs!
        alt1_full_tbls, alt1_full_diag, alt1_full_ok, _ = _fit_tables_from_items(
            alt1_items, DEFAULT_MIN_NULL_PER_CLASS, DEFAULT_MIN_ALT_PER_CLASS
        )
        assert alt1_full_ok is True and all(d["status"] == "fitted" for d in alt1_full_diag), \
            "MED-1 NC11 fixture: full-stratum fit must succeed across all 4 classes"
        alt1_insample_ev = evaluate_heldout(alt1_items, alt1_segs, alt1_full_tbls, compute_c1())
        assert alt1_insample_ev["native_fp"] == 0, \
            "MED-1 NC11 negative control: combined full-stratum replay has 0 in-sample FPs"

        # Protected 2-fold CV MUST reject the alt-less training fold and refuse calibration!
        alt1_report = calibrate_all_depths(alt1_items, alt1_segs, min_native_shots=DEFAULT_MIN_NATIVE_SHOTS)
        alt1_n8 = alt1_report["depths"]["N8"]
        assert alt1_n8["native_shots"] >= DEFAULT_MIN_NATIVE_SHOTS, "MED-1 NC11 fixture: >= 1500 native shots"
        assert alt1_n8["distinct_masters"] == 2
        assert alt1_n8["insample_confident_wrong_count"] == 0
        assert len(alt1_n8["cv_folds"]) == 2
        assert sum(1 for f in alt1_n8["cv_folds"] if not f["fit_ok"]) == 1, \
            "MED-1 NC11: exactly 1 CV fold (training on alt-less master) MUST fail fit_ok: %r" % (alt1_n8["cv_folds"],)
        assert alt1_n8["cv_passed"] is False, "MED-1 NC11: 1-master-alt dataset MUST fail cv_passed"
        assert alt1_n8["calibrated"] is False, "MED-1 NC11: 1-master-alt dataset MUST NOT calibrate"
        assert alt1_n8["g_phase_a_passed"] is False
        assert alt1_n8["source_kind"] == "placeholder"
        alt1_hdr = render_cpp_header(alt1_report)
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N8)" in alt1_hdr, \
            "MED-1 NC11: must emit PlaceholderFor(BitDepthClass::N8)"
        assert "NullTableSourceKind::Calibrated" not in alt1_hdr, \
            "MED-1 NC11: must NEVER emit NullTableSourceKind::Calibrated"

        # Negative control 12 (CARBON-MED-1 Red-Team: native-only dead LLR == 0 table check):
        # Construct a 2-native-master N10 dataset with 0 upscaled alt items (>= 1500 native shots, 0 native FPs).
        # Naive alt = nul gives LLR == 0 identically (dead detector, 0 FPs trivially). Protected fit_depth_stratum
        # / calibrate_all_depths MUST reject calibration and emit PlaceholderFor(BitDepthClass::N10)!
        natonly_dir = os.path.join(tmp, "med1_native_only_dead_llr")
        os.makedirs(natonly_dir)
        _write_synthetic_v4_pair(natonly_dir, "natonly_a", step_codes=1, output_bits=10, num_shots=760, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng)
        _write_synthetic_v4_pair(natonly_dir, "natonly_b", step_codes=1, output_bits=10, num_shots=760, evals_per_shot=2,
                                 truth_nx=0, truth_ny=0, rng=rng)
        natonly_manifest = os.path.join(tmp, "natonly_manifest.json")
        with open(natonly_manifest, "w", encoding="utf-8") as fh:
            json.dump({
                "natonly_a": {"truth_nx": 0, "truth_ny": 0, "master": "m_natonly_a"},
                "natonly_b": {"truth_nx": 0, "truth_ny": 0, "master": "m_natonly_b"},
            }, fh)
        natonly_items, natonly_segs, _ = load_all_dumps([natonly_dir], natonly_manifest)
        assert sum(1 for it in natonly_items if it.label == "alt") == 0, "MED-1 NC12 fixture: 0 alt items"

        # Same-input negative control: naive alt = nul table produces dead LLR == 0 even on a strong -0.50 upscale
        # while scoring 0 native FPs on natonly_segs!
        dead_tbls, _, dead_all_fitted, dead_fit_reasons = _fit_tables_from_items(
            natonly_items, DEFAULT_MIN_NULL_PER_CLASS, DEFAULT_MIN_ALT_PER_CLASS
        )
        naive_zero_llr_tbls = {"null": list(dead_tbls["null"]), "alt": list(dead_tbls["null"])}
        assert all(llr(naive_zero_llr_tbls["null"][c], naive_zero_llr_tbls["alt"][c], -0.50) == 0.0 for c in range(4)), \
            "MED-1 NC12 negative control: naive alt=nul yields dead LLR == 0.0 on strong -0.50 upscale"
        assert evaluate_heldout(natonly_items, natonly_segs, naive_zero_llr_tbls, compute_c1())["native_fp"] == 0, \
            "MED-1 NC12 negative control: naive dead LLR==0 table trivially achieves 0 native FPs"

        # Protected _fit_tables_from_items / calibrate_all_depths MUST reject dead native-only tables!
        assert dead_all_fitted is False, \
            "MED-1 NC12: native-only dataset (0 alt items) MUST set all_classes_fitted = False"
        assert len(dead_fit_reasons) > 0, "MED-1 NC12: must report explicit fit failure reasons"
        natonly_report = calibrate_all_depths(natonly_items, natonly_segs, min_native_shots=DEFAULT_MIN_NATIVE_SHOTS)
        natonly_n10 = natonly_report["depths"]["N10"]
        assert natonly_n10["native_shots"] >= DEFAULT_MIN_NATIVE_SHOTS, "MED-1 NC12 fixture: >= 1500 native shots"
        assert natonly_n10["distinct_masters"] == 2
        assert natonly_n10["heldout_native_fp"] == 0
        assert natonly_n10["insample_confident_wrong_count"] == 0
        assert natonly_n10["cv_passed"] is False, "MED-1 NC12: native-only dataset MUST fail cv_passed"
        assert natonly_n10["calibrated"] is False, "MED-1 NC12: native-only dataset MUST NOT calibrate"
        assert natonly_n10["g_phase_a_passed"] is False
        assert natonly_n10["source_kind"] == "placeholder"
        natonly_hdr = render_cpp_header(natonly_report)
        assert "DepthNullTable::PlaceholderFor(BitDepthClass::N10)" in natonly_hdr, \
            "MED-1 NC12: must emit PlaceholderFor(BitDepthClass::N10)"
        assert "NullTableSourceKind::Calibrated" not in natonly_hdr, \
            "MED-1 NC12: must NEVER emit NullTableSourceKind::Calibrated"

    print("generate_null_tables.py --selftest: PASS (2-fold held-out CV, H3 overfit & single-master, NEW-M2 emp_floor, TEST-L4 drift, CARBON-MED-1 1-master-alt & native-only dead-LLR negative controls, v4+v3 stratification, G-PhaseA, G-8bit verified)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Stage S3 bit-depth-stratified NullTables generator & gate scorer.")
    parser.add_argument("--dump-dir", action="append", default=[], help="Directory containing Dump v4 (*.b_evals.csv + *.b_sizes.csv) and/or v3 (cal_dump*.csv) files.")
    parser.add_argument("--truth-manifest", default=None, help="Optional JSON manifest mapping Dump v4 tag -> {truth_nx, truth_ny, depth, master}.")
    parser.add_argument("--out-dir", default=None, help="Output directory for NullTables.generated.h, calibration-gate-report.md, calibration-gate-report.json.")
    parser.add_argument("--out-header", default=None, help="Explicit output path for NullTables.generated.h.")
    parser.add_argument("--out-md", default=None, help="Explicit output path for Markdown gate report.")
    parser.add_argument("--out-json", default=None, help="Explicit output path for JSON gate report.")
    parser.add_argument("--min-native-shots", type=int, default=DEFAULT_MIN_NATIVE_SHOTS, help="Minimum native shots required per depth to mark Calibrated (default: 1500).")
    parser.add_argument("--min-null-per-class", type=int, default=DEFAULT_MIN_NULL_PER_CLASS, help="Minimum null samples per size class (default: 10).")
    parser.add_argument("--min-alt-per-class", type=int, default=DEFAULT_MIN_ALT_PER_CLASS, help="Minimum alt samples per size class to fit alt (default: 10).")
    parser.add_argument("--selftest", action="store_true", help="Run built-in deterministic synthetic self-test and exit.")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    verify_cpp_contract_constants()

    if not args.dump_dir:
        parser.error("provide at least one --dump-dir or run --selftest")

    items, segs, _ = load_all_dumps(args.dump_dir, args.truth_manifest)
    report = calibrate_all_depths(
        items,
        segs,
        min_native_shots=args.min_native_shots,
        min_null_per_class=args.min_null_per_class,
        min_alt_per_class=args.min_alt_per_class,
    )

    out_header = args.out_header or (os.path.join(args.out_dir, "NullTables.generated.h") if args.out_dir else None)
    out_md = args.out_md or (os.path.join(args.out_dir, "calibration-gate-report.md") if args.out_dir else None)
    out_json = args.out_json or (os.path.join(args.out_dir, "calibration-gate-report.json") if args.out_dir else None)
    write_outputs(report, out_header, out_md, out_json)

    print(render_markdown_report(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
