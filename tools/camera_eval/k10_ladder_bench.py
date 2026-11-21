# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""K10-2 / K10-4 (B8): Unattended camera ladder bench and owner summary runner.

Runs (or evaluates stored results for) the full camera ladder in fixed doc 45 s4 /
plan-CE-k3-k10.md order:
  1. K4 measurement switches (10 switches in doc 45 s4 order)
  2. C1 (K5) grain map
  3. C3 (K7) sigma-delta dither + deband
  4. C2 (K6) detail pulse + texture restore
  5. C4 (K8) occlusion / trust filler
  6. C6 (K9) smoothed de-judder / hiccup / R-A / R-B (LAST on the ladder)

Per step, checks:
  - Confident-wrong <= 1 % (> 1 px) and noise tier (> 1.5 px) per tier, synthetic WRONG == 0,
    usable drop <= 0.03 (compatible with r2/ce-kb gate_ladder.py JSON schema);
  - Timing gate (ABAB median delta within budget) and "no category worse > 2 %";
  - Robustness list (no cut/flash/resync/late-fallback silent regressions).

Stop rule (doc 91 line 83/100): a failing step does NOT abort the run; every step is
evaluated and recorded. Produces the K10-4 owner summary Markdown table including the
Q-CE2 partial-flip recommendation (engine + passing helpers ON; failing helpers visibly OFF).
Missing footage or GPU harness MUST log SKIP-NO-FOOTAGE / SKIP-OPT-IN (never a silent PASS).

Usage (python -I):
  python -I k10_ladder_bench.py --results-dir DIR [--out-md SUMMARY.md] [--out-json SUMMARY.json]
  python -I k10_ladder_bench.py --dry-run [--out-md SUMMARY.md]
  python -I k10_ladder_bench.py --selftest
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Exact ladder order (plan-CE-k3-k10.md s2 B8 + doc 45 s4): K4 switches first, then C1 -> C3 -> C2 -> C4 -> C6 last.
LADDER_STEPS = [
    dict(id='K4_01_model_global',     group='K4', keys=['camera_meas_model_global'],               desc='K4 model global prior'),
    dict(id='K4_02_guided_candidate', group='K4', keys=['camera_meas_guided_candidate'],           desc='K4 guided candidate'),
    dict(id='K4_03_narrow_window',    group='K4', keys=['camera_meas_narrow_window'],              desc='K4 narrow window'),
    dict(id='K4_04_e1_skip',          group='K4', keys=['camera_meas_e1_skip'],                    desc='K4 E1 skip'),
    dict(id='K4_05_static_gate',      group='K4', keys=['camera_meas_static_gate'],                desc='K4 static gate'),
    dict(id='K4_06_cut_skip',         group='K4', keys=['camera_meas_cut_skip'],                   desc='K4 cut skip'),
    dict(id='K4_07_fade_normalised',  group='K4', keys=['camera_meas_fade_normalised'],            desc='K4 fade normalised'),
    dict(id='K4_08_flash_bridge',     group='K4', keys=['camera_meas_flash_bridge'],               desc='K4 flash bridge'),
    dict(id='K4_09_temporal_seed',    group='K4', keys=['camera_meas_temporal_seed'],              desc='K4 temporal seed'),
    dict(id='K4_10_tiny_mover_roi',   group='K4', keys=['camera_meas_tiny_mover_roi'],             desc='K4 tiny mover ROI'),
    dict(id='C1_K5_grain',            group='C1', keys=['camera_use_grain'],                       desc='C1 (K5) grain map smoothing'),
    dict(id='C3_K7_dither_deband',    group='C3', keys=['camera_use_dither', 'camera_use_deband'], desc='C3 (K7) sigma-delta + deband'),
    dict(id='C2_K6_detail_texture',   group='C2', keys=['camera_use_detail'],                      desc='C2 (K6) detail pulse + texture restore'),
    dict(id='C4_K8_filler',           group='C4', keys=['camera_use_filler'],                      desc='C4 (K8) occlusion / trust filler'),
    dict(id='C6_K9_dejudder',         group='C6', keys=['camera_use_dejudder'],                    desc='C6 (K9) smoothed de-judder / hiccup / R-A / R-B'),
]

# Compatible with r2/ce-kb gate_ladder.py thresholds.
THRESHOLDS = {
    'master':     {'wrong1': 0.01, 'wrong15': 0.005},
    '1080p':      {'wrong1': 0.01, 'wrong15': 0.005},
    '1080p_low':  {'wrong1': 0.01, 'wrong15': 0.01},
    '1080p_vlow': {'wrong1': 0.01, 'wrong15': 0.01},
}
USABLE_DROP_LIMIT = 0.03
MAX_CATEGORY_REGRESSION = 0.02   # "no category worse > 2 %"


def evaluate_step(step, payload):
    """Evaluate one ladder step from a stored JSON result dict (or None if missing).

    payload schema (optional fields):
      gate:   gate_ladder.py JSON dict (ok, cells, notes, synth_wrong)
      timing: {ok: bool, median_delta_ms: float, budget_ms: float}
      categories: {category_name: relative_delta_float}  (> +0.02 fails "no category worse > 2 %")
      robustness: {ok: bool, failures: [str]}
      gpu_ran: bool (False -> SKIP-OPT-IN for GPU timing row)
    """
    res = dict(
        id=step['id'],
        group=step['group'],
        keys=list(step['keys']),
        desc=step['desc'],
        verdict='SKIP-NO-FOOTAGE',
        reasons=[],
        timing_str='SKIP-OPT-IN',
        gate_str='SKIP-NO-FOOTAGE',
        category_str='SKIP-NO-FOOTAGE',
        robustness_str='SKIP-NO-FOOTAGE',
    )
    if payload is None:
        res['reasons'].append('SKIP-NO-FOOTAGE: no stored step result JSON')
        return res

    any_fail = False
    any_skip = False

    # 1. Confident-wrong + synthetic gate (compatible with r2/ce-kb gate_ladder.py JSON)
    gate = payload.get('gate')
    if gate is None:
        any_skip = True
        res['gate_str'] = 'SKIP-NO-FOOTAGE'
        res['reasons'].append('SKIP-NO-FOOTAGE: gate_ladder result absent')
    else:
        g_ok = bool(gate.get('ok', False)) and int(gate.get('synth_wrong', 0)) == 0
        for c in gate.get('cells', []):
            if c.get('fail'):
                g_ok = False
        res['gate_str'] = 'PASS' if g_ok else 'FAIL'
        if not g_ok:
            any_fail = True
            res['reasons'].append('gate_ladder failed (confident-wrong > limit or synth_wrong > 0 or usable drop > %.2f)' % USABLE_DROP_LIMIT)

    # 2. Timing gate (quiet GPU ABAB)
    timing = payload.get('timing')
    if timing is None or not payload.get('gpu_ran', True):
        any_skip = True
        res['timing_str'] = 'SKIP-OPT-IN'
        res['reasons'].append('SKIP-OPT-IN: GPU quiet timing row not executed')
    else:
        t_ok = bool(timing.get('ok', False))
        delta = timing.get('median_delta_ms', 0.0)
        res['timing_str'] = '%s (%+.2f ms)' % ('PASS' if t_ok else 'FAIL', delta)
        if not t_ok:
            any_fail = True
            res['reasons'].append('timing gate failed (delta %+.2f ms)' % delta)

    # 3. Category regression check ("no category worse > 2 %")
    cats = payload.get('categories')
    if cats is None:
        any_skip = True
        res['category_str'] = 'SKIP-NO-FOOTAGE'
    else:
        worst_cat = None
        worst_val = 0.0
        bad = []
        for k, v in sorted(cats.items()):
            fv = float(v)
            if worst_cat is None or fv > worst_val:
                worst_cat, worst_val = k, fv
            if fv > MAX_CATEGORY_REGRESSION:
                bad.append('%s %+.1f%%' % (k, fv * 100.0))
        if bad:
            any_fail = True
            res['category_str'] = 'FAIL (%s)' % ', '.join(bad)
            res['reasons'].append('category worse > 2%%: %s' % ', '.join(bad))
        else:
            res['category_str'] = 'PASS (max %+.1f%%)' % (worst_val * 100.0)

    # 4. Robustness list
    rob = payload.get('robustness')
    if rob is None:
        any_skip = True
        res['robustness_str'] = 'SKIP-NO-FOOTAGE'
    else:
        r_ok = bool(rob.get('ok', False)) and not rob.get('failures')
        res['robustness_str'] = 'PASS' if r_ok else 'FAIL (%s)' % ', '.join(rob.get('failures', ['failed']))
        if not r_ok:
            any_fail = True
            res['reasons'].append('robustness failed: ' + ', '.join(rob.get('failures', ['failed'])))

    if any_fail:
        res['verdict'] = 'FAIL'
    elif any_skip:
        res['verdict'] = 'SKIP-NO-FOOTAGE' if 'SKIP-NO-FOOTAGE' in (res['gate_str'], res['category_str'], res['robustness_str']) else 'SKIP-OPT-IN'
    else:
        res['verdict'] = 'PASS'
    return res


def run_ladder(results_by_id):
    """Evaluate all ladder steps in fixed order without stopping on failure (doc 91 stop rule)."""
    rows = []
    for step in LADDER_STEPS:
        rows.append(evaluate_step(step, results_by_id.get(step['id'])))
    passed = [r for r in rows if r['verdict'] == 'PASS']
    failed = [r for r in rows if r['verdict'] == 'FAIL']
    skipped = [r for r in rows if r['verdict'].startswith('SKIP-')]
    k4_ok = all(r['verdict'] == 'PASS' for r in rows if r['group'] == 'K4')
    passing_helpers = [k for r in passed if r['group'] != 'K4' for k in r['keys']]
    failing_helpers = [k for r in failed if r['group'] != 'K4' for k in r['keys']]
    skipped_helpers = [k for r in skipped if r['group'] != 'K4' for k in r['keys']]
    return dict(
        rows=rows,
        passed_count=len(passed),
        failed_count=len(failed),
        skipped_count=len(skipped),
        engine_k4_passed=k4_ok,
        q_ce2_flip_on=passing_helpers if k4_ok else [],
        q_ce2_hold_off=failing_helpers + skipped_helpers if k4_ok else [k for r in rows if r['group'] != 'K4' for k in r['keys']],
        overall='PASS' if (not failed and not skipped) else ('FAIL' if failed else 'INCOMPLETE-SKIP'),
    )


def format_owner_summary(summary):
    """K10-4 owner summary Markdown table."""
    L = [
        '# K10-4 Camera Ladder Owner Summary',
        '',
        '| # | step | group | keys | gate (<=1%/1.5px) | timing | category (<=2%) | robustness | verdict |',
        '|---:|---|---|---|---|---|---|---|---|',
    ]
    for idx, r in enumerate(summary['rows'], start=1):
        L.append('| %d | %s | %s | `%s` | %s | %s | %s | %s | **%s** |' % (
            idx, r['id'], r['group'], ', '.join(r['keys']),
            r['gate_str'], r['timing_str'], r['category_str'], r['robustness_str'], r['verdict']))
    L += [
        '',
        'Totals: %d PASS, %d FAIL, %d SKIP (overall: **%s**).' % (
            summary['passed_count'], summary['failed_count'], summary['skipped_count'], summary['overall']),
        '',
        '## Q-CE2 Flip Recommendation (engine + passing helpers ON; failing / unrun helpers visibly OFF)',
        '- Engine K4 gate: **%s**' % ('PASS' if summary['engine_k4_passed'] else 'HOLD OFF'),
        '- Helpers eligible to flip ON: %s' % (', '.join('`%s`' % k for k in summary['q_ce2_flip_on']) or 'none'),
        '- Helpers held visibly OFF: %s' % (', '.join('`%s`' % k for k in summary['q_ce2_hold_off']) or 'none'),
    ]
    notes = [(r['id'], r['reasons']) for r in summary['rows'] if r['reasons']]
    if notes:
        L += ['', '## Notes / Skips / Failures']
        for sid, rs in notes:
            L.append('- `%s`: %s' % (sid, '; '.join(rs)))
    return '\n'.join(L) + '\n'


def selftest():
    # 1. Exact ladder order: K4 (10 steps) -> C1 (K5) -> C3 (K7) -> C2 (K6) -> C4 (K8) -> C6 (K9) last.
    groups = [s['group'] for s in LADDER_STEPS]
    assert groups[:10] == ['K4'] * 10
    assert groups[10:] == ['C1', 'C3', 'C2', 'C4', 'C6'], 'ladder order must be K4 -> C1 -> C3 -> C2 -> C4 -> C6'

    # 2. All-passing ladder -> overall PASS and all helpers eligible to flip ON.
    good_payload = dict(
        gpu_ran=True,
        gate=dict(ok=True, synth_wrong=0, cells=[dict(clip='boat', tier='master', fail=False)]),
        timing=dict(ok=True, median_delta_ms=0.05, budget_ms=0.5),
        categories=dict(pans=-0.01, zooms=0.005, movers=0.015),
        robustness=dict(ok=True, failures=[]),
    )
    full_pass = run_ladder({s['id']: good_payload for s in LADDER_STEPS})
    assert full_pass['overall'] == 'PASS'
    assert full_pass['passed_count'] == len(LADDER_STEPS)
    assert full_pass['engine_k4_passed'] and len(full_pass['q_ce2_hold_off']) == 0

    # 3. Stop rule + Q-CE2 partial pass: C2_K6 fails category > 2 %, C4_K8 and C6_K9 STILL evaluated and pass.
    partial = {s['id']: good_payload for s in LADDER_STEPS}
    partial['C2_K6_detail_texture'] = dict(good_payload, categories=dict(pans=0.025, zooms=0.0))
    res_partial = run_ladder(partial)
    assert res_partial['overall'] == 'FAIL'
    assert res_partial['failed_count'] == 1 and res_partial['passed_count'] == len(LADDER_STEPS) - 1
    assert 'camera_use_detail' in res_partial['q_ce2_hold_off']
    assert 'camera_use_grain' in res_partial['q_ce2_flip_on']
    assert 'camera_use_dejudder' in res_partial['q_ce2_flip_on']

    # 4. Dry-run with missing results MUST mark SKIP-NO-FOOTAGE, NEVER silent PASS.
    dry = run_ladder({})
    assert dry['passed_count'] == 0 and dry['skipped_count'] == len(LADDER_STEPS)
    assert all(r['verdict'] == 'SKIP-NO-FOOTAGE' for r in dry['rows'])
    md = format_owner_summary(dry)
    assert 'SKIP-NO-FOOTAGE' in md and 'K10-4 Camera Ladder Owner Summary' in md

    # 5. Missing GPU timing with CPU gate passing -> SKIP-OPT-IN, never silent PASS.
    cpu_only = dict(good_payload, gpu_ran=False, timing=None)
    res_cpu = run_ladder({'C1_K5_grain': cpu_only})
    c1_row = next(r for r in res_cpu['rows'] if r['id'] == 'C1_K5_grain')
    assert c1_row['verdict'] == 'SKIP-OPT-IN' and c1_row['timing_str'] == 'SKIP-OPT-IN'
    print('k10_ladder_bench selftest: ok (%d ladder steps verified)' % len(LADDER_STEPS))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--results-dir', help='directory holding <step_id>.json per ladder step')
    ap.add_argument('--dry-run', action='store_true', help='emit summary with SKIP-NO-FOOTAGE for missing rows')
    ap.add_argument('--out-md')
    ap.add_argument('--out-json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    results = {}
    if a.results_dir and os.path.isdir(a.results_dir):
        for step in LADDER_STEPS:
            p = os.path.join(a.results_dir, step['id'] + '.json')
            if os.path.isfile(p):
                with open(p, encoding='utf-8') as fp:
                    results[step['id']] = json.load(fp)
    elif not a.dry_run:
        print('SKIP-NO-FOOTAGE: --results-dir missing or not found (%s); use --dry-run to emit SKIP summary' % a.results_dir,
              file=sys.stderr)
        return 2
    summary = run_ladder(results)
    md = format_owner_summary(summary)
    print(md)
    if a.out_md:
        with open(a.out_md, 'w', encoding='utf-8') as fp:
            fp.write(md)
    if a.out_json:
        with open(a.out_json, 'w', encoding='utf-8') as fp:
            json.dump(summary, fp, indent=2)
    return 0 if summary['overall'] == 'PASS' else (0 if a.dry_run else 1)


if __name__ == '__main__':
    sys.exit(main())
