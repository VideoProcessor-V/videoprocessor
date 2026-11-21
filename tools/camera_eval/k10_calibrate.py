# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""K10-1 (B7): Logistic confidence calibration for CameraModel::Config::b[9].

Fits the 9 logistic coefficients
    logit = b0 + b1*inlierShare + b2*precision + b3*coverage + b4*temporal
               + b5*conditioning + b6*engineAgreement + b7*inverse + b8*ambiguity
    conf  = 1 / (1 + exp(-logit))
from camera_eval CSV outputs paired with master-derived truth (xcheck_real.py) or a
labelled signal CSV, using L2-regularised Newton-Raphson (IRLS) with configurable
false-positive (confident-wrong) weight so the 1 % confident-wrong gate is respected.

Emits a Before / After Markdown table and a C++ Config::b[9] snippet. Never edits C++
sources automatically (CE reviews and registers the FIX commit with real-set numbers).
When footage or eval output is missing, prints SKIP-NO-FOOTAGE and exits with code 0
only on --selftest (missing inputs in live mode exit 2 with SKIP-NO-FOOTAGE).

Usage (python -I):
  python -I k10_calibrate.py --eval-out OUT_DIR [--tag new] [--real-root D:/codex/td-real-vectors-out]
                             [--fp-weight 4.0] [--l2 0.1] [--out-md TABLE.md] [--out-json OUT.json]
  python -I k10_calibrate.py --selftest
"""
import argparse
import csv
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Current CameraModel::Config::b[9] in GlobalModelFit.h:83 (baseline for Before column).
DEFAULT_B = [-8.5, 4.0, 1.0, 1.5, 0.5, 0.5, 0.5, 1.0, 2.5]
SIGNAL_COLS = [
    'inlierShare',
    'precision',
    'coverage',
    'temporal',
    'conditioning',
    'engineAgreement',
    'inverse',
    'ambiguity',
]


def sigmoid(z):
    if z >= 0.0:
        e = math.exp(-min(z, 60.0))
        return 1.0 / (1.0 + e)
    e = math.exp(max(z, -60.0))
    return e / (1.0 + e)


def predict_prob(b, x):
    z = b[0]
    for i in range(8):
        z += b[i + 1] * x[i]
    return sigmoid(z)


def _solve_linear(A, rhs):
    """Gaussian elimination with partial pivoting (pure Python, deterministic, 9x9)."""
    n = len(rhs)
    M = [list(A[i]) + [rhs[i]] for i in range(n)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(M[r][col]))
        if abs(M[piv][col]) < 1e-12:
            M[col][col] += 1e-9
            piv = col
        M[col], M[piv] = M[piv], M[col]
        inv = 1.0 / M[col][col]
        for j in range(col, n + 1):
            M[col][j] *= inv
        for r in range(n):
            if r != col and M[r][col] != 0.0:
                f = M[r][col]
                for j in range(col, n + 1):
                    M[r][j] -= f * M[col][j]
    return [M[i][n] for i in range(n)]


def fit_logistic(samples, init_b=None, fp_weight=4.0, l2=0.1, max_iter=30, tol=1e-7):
    """Fit b[9] via L2-regularised Newton-Raphson (IRLS).

    samples: list of dicts with keys 'x' (8 floats in [0, 1]), 'y' (1 = accurate camera pair, 0 = wrong/ambiguous).
    fp_weight: relative penalty weight on negative (y == 0) pairs to curb confident-wrong fits.
    Returns fitted list of 9 floats.
    """
    b = list(init_b if init_b is not None else DEFAULT_B)
    prior = list(DEFAULT_B)
    n = len(samples)
    if n == 0:
        return b
    for _ in range(max_iter):
        grad = [0.0] * 9
        H = [[0.0] * 9 for _ in range(9)]
        for s in samples:
            x = [1.0] + list(s['x'])
            y = 1.0 if s['y'] else 0.0
            w = 1.0 if y > 0.5 else float(fp_weight)
            p = predict_prob(b, s['x'])
            err = (p - y) * w
            wt = max(p * (1.0 - p), 1e-6) * w
            for i in range(9):
                grad[i] += err * x[i]
                xi = x[i]
                row = H[i]
                for j in range(9):
                    row[j] += wt * xi * x[j]
        # L2 shrink towards prior (keeps weakly-observed signals stable and invertible).
        for i in range(9):
            reg = l2 if i > 0 else l2 * 0.1
            grad[i] += reg * (b[i] - prior[i])
            H[i][i] += reg
        step = _solve_linear(H, grad)
        max_delta = 0.0
        for i in range(9):
            d = max(-2.0, min(2.0, step[i]))
            b[i] -= d
            max_delta = max(max_delta, abs(d))
        if max_delta < tol:
            break
    return b


def evaluate_coefficients(b, samples, threshold=0.5):
    """Return calibration metrics for coefficients b over labelled samples."""
    total = len(samples)
    if total == 0:
        return dict(total=0, usable=0, usable_share=0.0, judged_usable=0, wrong=0,
                    wrong_rate=0.0, wrong15=0, wrong15_rate=0.0, brier=0.0, logloss=0.0)
    usable = wrong = wrong15 = 0
    brier = logloss = 0.0
    for s in samples:
        p = predict_prob(b, s['x'])
        y = 1.0 if s['y'] else 0.0
        pc = min(1.0 - 1e-12, max(1e-12, p))
        brier += (p - y) ** 2
        logloss += -(y * math.log(pc) + (1.0 - y) * math.log(1.0 - pc))
        if p >= threshold:
            usable += 1
            if not s['y']:
                wrong += 1
                if s.get('far_wrong', True):
                    wrong15 += 1
    return dict(
        total=total,
        usable=usable,
        usable_share=usable / total,
        wrong=wrong,
        wrong_rate=wrong / max(1, usable),
        wrong15=wrong15,
        wrong15_rate=wrong15 / max(1, usable),
        brier=brier / total,
        logloss=logloss / total,
    )


def format_before_after_table(b_before, b_after, m_before, m_after):
    """Format K10-1 Before / After calibration Markdown report."""
    lines = [
        '# K10-1 Camera Confidence Logistic Calibration (Before / After)',
        '',
        '## Metrics',
        '',
        '| metric | before (Config::b) | after (calibrated) | delta |',
        '|---|---:|---:|---:|',
        '| pairs | %d | %d | 0 |' % (m_before['total'], m_after['total']),
        '| usable share (>= 0.5) | %.4f | %.4f | %+.4f |' % (
            m_before['usable_share'], m_after['usable_share'], m_after['usable_share'] - m_before['usable_share']),
        '| confident-wrong (> 1 px) | %d (%.4f) | %d (%.4f) | %+d |' % (
            m_before['wrong'], m_before['wrong_rate'], m_after['wrong'], m_after['wrong_rate'],
            m_after['wrong'] - m_before['wrong']),
        '| confident-wrong (> 1.5 px) | %d (%.4f) | %d (%.4f) | %+d |' % (
            m_before['wrong15'], m_before['wrong15_rate'], m_after['wrong15'], m_after['wrong15_rate'],
            m_after['wrong15'] - m_before['wrong15']),
        '| Brier score | %.5f | %.5f | %+.5f |' % (
            m_before['brier'], m_after['brier'], m_after['brier'] - m_before['brier']),
        '| log-loss | %.5f | %.5f | %+.5f |' % (
            m_before['logloss'], m_after['logloss'], m_after['logloss'] - m_before['logloss']),
        '',
        '## Coefficients',
        '',
        '| index | term | before | after |',
        '|---:|---|---:|---:|',
    ]
    names = ['bias'] + SIGNAL_COLS
    for i, name in enumerate(names):
        lines.append('| b[%d] | %s | %.4f | %.4f |' % (i, name, b_before[i], b_after[i]))
    cpp = ', '.join('%.4f' % v for v in b_after)
    lines += ['', '```cpp', 'double b[9] = { %s };' % cpp, '```']
    return '\n'.join(lines) + '\n'


def load_samples_from_eval(eval_out, tag, real_root):
    """Load labelled signal vectors from camera_eval real CSVs + xcheck_real truth."""
    real_dir = os.path.join(eval_out, tag, 'real')
    truth_dir = os.path.join(eval_out, 'new', 'real')
    if not os.path.isdir(real_dir) or not os.path.isdir(real_root):
        return None
    import xcheck_real
    rows, _ = xcheck_real.evaluate(real_dir, real_root, truth_dir=truth_dir)
    wrong_lookup = {}
    for r in rows:
        key = (r['seq'], r['src'])
        wrong_lookup[key] = {t: p90 for (t, p90, _thr) in r['wrong_pairs']}
    samples = []
    for r in rows:
        seq, src = r['seq'], r['src']
        csv_path = os.path.join(real_dir, '%s_%s.csv' % (seq, src))
        if not os.path.isfile(csv_path):
            continue
        wmap = wrong_lookup.get((seq, src), {})
        with open(csv_path, newline='') as fp:
            for row in csv.DictReader(fp):
                if row.get('valid') != '1':
                    continue
                t = int(row['pair']) - 1
                x = [float(row[c]) for c in SIGNAL_COLS]
                is_wrong = t in wmap
                far_wrong = is_wrong and wmap[t] > xcheck_real.NOISE_TIER_PX
                samples.append(dict(x=x, y=not is_wrong, far_wrong=far_wrong, seq=seq, src=src, pair=t))
    return samples


def selftest():
    # Deterministic synthetic dataset: good camera pairs have high inlierShare/coverage/ambiguity;
    # bad (foreground/water) pairs have moderate inlierShare but lower coverage and ambiguity,
    # so default weights misclassify some bad pairs as confident-wrong.
    samples = []
    for i in range(120):
        # Good pairs
        s = (i % 10) / 100.0
        samples.append(dict(
            x=[0.80 + s, 0.85, 0.85 + 0.5 * s, 0.75, 0.80, 0.70, 0.80, 0.90],
            y=True,
            far_wrong=False,
        ))
    for i in range(60):
        # Tricky water / foreground pairs that fool default weights (high inlierShare/precision/ambiguity -> logit > 0, low coverage/temporal)
        s = (i % 10) / 100.0
        samples.append(dict(
            x=[0.90 + 0.5 * s, 0.90, 0.20 + s, 0.10, 0.70, 0.70, 0.75, 0.90],
            y=False,
            far_wrong=(i % 2 == 0),
        ))
    m_before = evaluate_coefficients(DEFAULT_B, samples)
    b_after = fit_logistic(samples, fp_weight=4.0, l2=0.05)
    m_after = evaluate_coefficients(b_after, samples)
    assert m_before['wrong'] > 0, 'baseline fixture must exercise confident-wrong elimination'
    assert len(b_after) == 9 and all(math.isfinite(v) for v in b_after), 'fitted coefficients must be finite'
    assert m_after['logloss'] < m_before['logloss'], 'calibrated log-loss must improve'
    assert m_after['wrong'] < m_before['wrong'], 'confident-wrong must strictly drop'
    assert m_after['wrong_rate'] <= 0.01, 'synthetic confident-wrong rate must meet <= 1 % gate'
    assert m_after['usable_share'] >= 0.60, 'good pairs must stay usable'
    md = format_before_after_table(DEFAULT_B, b_after, m_before, m_after)
    assert 'Before / After' in md and 'double b[9]' in md
    # Deterministic repeat check
    b_repeat = fit_logistic(samples, fp_weight=4.0, l2=0.05)
    assert b_after == b_repeat, 'calibration must be deterministic'
    print('k10_calibrate selftest: ok (wrong %d -> %d, logloss %.4f -> %.4f)' % (
        m_before['wrong'], m_after['wrong'], m_before['logloss'], m_after['logloss']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--eval-out')
    ap.add_argument('--tag', default='new')
    ap.add_argument('--real-root', default='D:/codex/td-real-vectors-out')
    ap.add_argument('--fp-weight', type=float, default=4.0)
    ap.add_argument('--l2', type=float, default=0.1)
    ap.add_argument('--out-md')
    ap.add_argument('--out-json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    if not a.eval_out or not os.path.isdir(os.path.join(a.eval_out, a.tag, 'real')) or not os.path.isdir(a.real_root):
        print('SKIP-NO-FOOTAGE: k10_calibrate requires --eval-out/<tag>/real and --real-root (%s, %s)' % (
            a.eval_out, a.real_root), file=sys.stderr)
        return 2
    samples = load_samples_from_eval(a.eval_out, a.tag, a.real_root)
    if not samples:
        print('SKIP-NO-FOOTAGE: no valid real-clip CSV samples found', file=sys.stderr)
        return 2
    m_before = evaluate_coefficients(DEFAULT_B, samples)
    b_after = fit_logistic(samples, fp_weight=a.fp_weight, l2=a.l2)
    m_after = evaluate_coefficients(b_after, samples)
    md = format_before_after_table(DEFAULT_B, b_after, m_before, m_after)
    print(md)
    if a.out_md:
        with open(a.out_md, 'w', encoding='utf-8') as fp:
            fp.write(md)
    if a.out_json:
        with open(a.out_json, 'w', encoding='utf-8') as fp:
            json.dump(dict(before=DEFAULT_B, after=b_after, metrics_before=m_before, metrics_after=m_after), fp, indent=2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
