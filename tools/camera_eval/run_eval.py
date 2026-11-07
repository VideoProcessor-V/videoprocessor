# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""One command: run camera_eval on the synthetic scenarios and the real sequences, two configurations side by side.

Usage (python -I):
  python -I run_eval.py --exe bin/camera_eval.exe --out OUT_DIR [--vs two-layer|phase1] [--synth-root DIR]
                        [--real-root D:/codex/td-real-vectors-out] [--synth-pairs 40] [--real-frames 72] [--match]
                        [--match-count 40] [--match-clips crowd,tos,sol,boat]

Builds nothing (build_camera_eval.bat first). Generates the synthetic fields under --synth-root if missing (default
OUT_DIR/synth_fields, never inside the repo), fits every scenario and every real sequence/source under two
configurations: 'new' (current Config defaults, twoLayer off) and the second one chosen by --vs ('two-layer' = --two-layer,
the default; 'phase1' = --phase1), scores them, and writes OUT_DIR/summary.md. Real-clip truth is ALWAYS taken from the
'new' (default-configuration) master DIS + NVOF fits, for both columns, so the metric is not self-referential. The summary
also reports the fit time (p50 / p95 / max microseconds per Fitter::Fit over all real pairs) per configuration.
--match also runs the matcher comparison (`camera_eval match`) on the *_1080p_low clips and appends it.
Only reads --real-root; never writes there.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score_synth  # noqa: E402
import synth_fields  # noqa: E402
import xcheck_real  # noqa: E402

SYNTH_FPS = 50


def run(cmd):
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit('command failed (%d): %s\n%s' % (p.returncode, ' '.join(cmd), p.stderr[-2000:]))
    return time.time() - t0


def cases(real_root):
    d = json.load(open(os.path.join(real_root, 'data', 'data.json')))['cases']
    return {c['id']: c for c in d}


def real_jobs(real_root):
    cs = cases(real_root)
    jobs = []
    vec = os.path.join(real_root, 'vec')
    for clip in xcheck_real.CLIPS:
        any_case = next(c for c in cs.values() if c['clip'] == clip)
        for tier in xcheck_real.TIERS:
            seq = '%s_%s' % (clip, tier)
            for src in xcheck_real.SOURCES:
                d = os.path.join(vec, seq, src)
                if os.path.isdir(d):
                    jobs.append((seq, src, d, any_case['w'], any_case['h'], any_case['fps']))
    return jobs


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def f(x, p=2):
    return '-' if x is None else ('%.*f' % (p, x))


def fit_times(rd):
    """All fitUs values (microseconds per Fitter::Fit call) of the real-clip CSVs in rd."""
    v = []
    for fn in sorted(os.listdir(rd)):
        if fn.endswith('.csv'):
            v += [float(r['fitUs']) for r in csv.DictReader(open(os.path.join(rd, fn))) if r.get('fitUs')]
    return v


def run_match(exe, out, real_root, clips, count, configs):
    cs = cases(real_root)
    res = {}
    for clip in clips:
        c = cs['%s_1080p_low' % clip]
        for tag, flags in configs:
            o = os.path.join(out, tag, 'match_%s_1080p_low.csv' % clip)
            cmd = [exe, 'match', '--yuv', c['coded'].replace('\\', '/'), '--w', str(c['w']), '--h', str(c['h']), '--bitdepth', '10', '--frames', str(c['frames']),
                   '--vec', os.path.join(real_root, 'vec', '%s_1080p_low' % clip, 'nvof'), '--ref', os.path.join(real_root, 'vec', '%s_master' % clip, 'disref'),
                   '--fps', str(c['fps']), '--count', str(count), '--out', o] + [x for x in flags if x != '--two-layer']
            os.makedirs(os.path.dirname(o), exist_ok=True)
            print('match', clip, tag, '%.0fs' % run(cmd), flush=True)
            res[(clip, tag)] = summarise_match(o)
    return res


def summarise_match(path):
    rows = list(csv.DictReader(open(path)))
    g = lambda k: [float(r[k]) for r in rows if r[k] != '']
    full, guided = sum(g('evalsFull')), sum(g('evalsGuided'))
    out = dict(pairs=len(rows), active=sum(float(r['planActive']) for r in rows) / max(1, len(rows)), evals_ratio=guided / full if full else None,
               ms_full=mean(g('msFull')), ms_guided=mean(g('msGuided')), escalated=mean(g('escalated')), cam_blocks=mean(g('camBlocks')))
    for run_ in ('off', 'on'):
        for s in ('all', 'cam', 'rest'):
            for m in ('mean', 'median', 'gt1', 'gt3'):
                out['%s_%s_%s' % (run_, s, m)] = mean(g('%s_%s_%s' % (run_, s, m)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--vs', choices=['two-layer', 'phase1', 'static'], default='two-layer',
                    help='second configuration: two-layer (Config::twoLayer on; default), phase1 (--phase1) or static (Config::staticCamera on)')
    ap.add_argument('--synth-root')
    ap.add_argument('--real-root', default='D:/codex/td-real-vectors-out')
    ap.add_argument('--synth-pairs', type=int, default=40)
    ap.add_argument('--real-frames', type=int, default=72)
    ap.add_argument('--tl-args', default='', help='extra camera_eval flags for the two-layer configuration, e.g. "--tl-floor 0.5 --tl-veto 0.3"')
    ap.add_argument('--all-args', default='', help='extra camera_eval flags for BOTH configurations, e.g. "--dyn-texture" (dynamic-texture detector on)')
    ap.add_argument('--match', action='store_true')
    ap.add_argument('--match-count', type=int, default=40)
    ap.add_argument('--match-clips', default='crowd,tos,sol,boat')
    a = ap.parse_args()
    if a.vs == 'phase1':
        B, flagsB, L2 = 'phase1', ['--phase1'], 'p1'
    elif a.vs == 'static':
        B, flagsB, L2 = 'static', ['--static-camera'] + a.tl_args.split(), 'static'
    else:
        B, flagsB, L2 = 'two', ['--two-layer'] + a.tl_args.split(), 'two'
    base = a.all_args.split()
    CONFIGS = [('new', list(base)), (B, flagsB + base)]
    exe = os.path.abspath(a.exe)
    out = os.path.abspath(a.out)
    repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    for d in (out, os.path.abspath(a.synth_root) if a.synth_root else out):
        try:
            inside = os.path.commonpath([os.path.normcase(d), os.path.normcase(repo)]) == os.path.normcase(repo)
        except ValueError:   # different drives
            inside = False
        if inside:
            raise SystemExit('refusing --out / --synth-root inside the repo worktree: ' + d)
    synth_root = os.path.abspath(a.synth_root or os.path.join(out, 'synth_fields'))
    os.makedirs(out, exist_ok=True)
    if not os.path.isdir(os.path.join(synth_root, synth_fields.SCENARIOS[-1])):
        synth_fields.generate(synth_root, a.synth_pairs)

    synth, real, fit_us = {}, {}, {}
    jobs = real_jobs(a.real_root)
    for tag, flags in CONFIGS:
        sd = os.path.join(out, tag, 'synth')
        rd = os.path.join(out, tag, 'real')
        os.makedirs(sd, exist_ok=True)
        os.makedirs(rd, exist_ok=True)
        for n in synth_fields.SCENARIOS:
            run([exe, 'fit', '--vec', os.path.join(synth_root, n), '--frames', str(a.synth_pairs), '--w', '1920', '--h', '1080', '--fps', str(SYNTH_FPS),
                 '--out', os.path.join(sd, n + '.camf'), '--csv', os.path.join(sd, n + '.csv')] + flags)
            synth[(tag, n)] = score_synth.score(os.path.join(synth_root, n), os.path.join(sd, n + '.camf'))
        for seq, src, d, w, h, fps in jobs:
            run([exe, 'fit', '--vec', d, '--frames', str(a.real_frames), '--w', str(w), '--h', str(h), '--fps', str(fps),
                 '--out', os.path.join(rd, '%s_%s.camf' % (seq, src)), '--csv', os.path.join(rd, '%s_%s.csv' % (seq, src))] + flags)
        fit_us[tag] = fit_times(rd)
        print('fitted', tag, flush=True)
    for tag, _ in CONFIGS:   # truth pinned to the default-configuration master fits for both columns
        rows, truths = xcheck_real.evaluate(os.path.join(out, tag, 'real'), a.real_root, truth_dir=os.path.join(out, 'new', 'real'))
        real[tag] = ({(r['seq'], r['src']): r for r in rows}, truths)

    title = ('# Camera model evaluation: new defaults vs phase 1' if B == 'phase1'
             else '# Camera model evaluation: staticCamera off (new) vs staticCamera on (static)' if B == 'static'
             else '# Camera model evaluation: twoLayer off (new) vs twoLayer on (two)')
    L = [title, '',
         'Synthetic: %d pairs per scenario at 1920x1080, %d fps. WRONG = a usable fit (valid, confidence >= 0.5) on a scene with no dominant camera '
         'motion, or with max corner error > max(1 px, 10 %% of the true corner displacement).' % (a.synth_pairs - 1, SYNTH_FPS), '',
         '## Synthetic scenarios', '',
         '| scenario | dominant | usable new | usable %s | WRONG new | WRONG %s | corner err med new | %s | label prec/rec new | %s | jitter new | %s |' % ((L2,) * 5),
         '|---|---|---|---|---|---|---|---|---|---|---|---|']
    tot = {t: 0 for t, _ in CONFIGS}
    for n in synth_fields.SCENARIOS:
        s, p = synth[('new', n)], synth[(B, n)]
        L.append('| %s | %d | %s | %s | %d | %d | %s | %s | %s/%s | %s/%s | %s | %s |' % (
            n, s['dominant'], f(s['usable']), f(p['usable']), s['wrong'], p['wrong'], f(s['err_med']), f(p['err_med']),
            f(s['lab_prec']), f(s['lab_rec']), f(p['lab_prec']), f(p['lab_rec']), f(s['jitter']), f(p['jitter'])))
        for t, _ in CONFIGS:
            tot[t] += synth[(t, n)]['wrong']
    L += ['', 'Total WRONG-confident synthetic pairs: new %d, %s %d.' % (tot['new'], L2, tot[B]), '',
          '## Real sequences (truth = default-configuration master DIS/NVOF fits that agree, for both columns)', '']
    for clip, (n, m, k) in real['new'][1].items():
        L.append('- %s: region truth on %d/%d pairs, corner truth on %d' % (clip, k, m, n))
    L += ['', 'Region metric (primary): truth = pairs where master DIS and NVOF fits are valid, share >= 50 camera blocks, p90 block difference <= 0.5 px. '
          'WRONG = confident fit whose p90 block error over the true-camera blocks (or over the fit own camera labels) exceeds max(1 px, 10 % of the p90 truth magnitude). '
          '"Extrapolated corner" columns use the max picture-corner error and include non-camera regions.', '',
          '| sequence | source | usable new | usable %s | region judged new/%s | WRONG@true-cam new | %s | >1.5 px@true-cam new | %s | WRONG@own-labels new | %s | extrapolated corner err med new | %s | master-camera-block err med new | %s | extrapolated corner WRONG new | %s |' % ((L2,) * 8),
          '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    rn, rp = real['new'][0], real[B][0]
    agg = {t: dict(u=[], w=0, j=0, rj=0, rw=0, ro=0, r15=0) for t in ('new', B)}
    clip_agg = {}
    for k in sorted(rn):
        x, y = rn[k], rp.get(k)
        if y is None:
            continue
        L.append('| %s | %s | %s | %s | %d/%d | %d | %d | %d | %d | %d | %d | %s | %s | %s | %s | %d | %d |' % (k[0], k[1], f(x['usable']), f(y['usable']), x['region_judged'], y['region_judged'],
                 x['region_wrong'], y['region_wrong'], x['region_wrong15'], y['region_wrong15'], x['region_wrong_own'], y['region_wrong_own'],
                 f(x['err_med']), f(y['err_med']), f(x['cam_err_med']), f(y['cam_err_med']), x['wrong'], y['wrong']))
        clip = k[0].split('_')[0]
        for t, r in (('new', x), (B, y)):
            agg[t]['u'].append(r['usable'])
            agg[t]['w'] += r['wrong']
            agg[t]['j'] += r['judged']
            agg[t]['rj'] += r['region_judged']
            agg[t]['rw'] += r['region_wrong']
            agg[t]['ro'] += r['region_wrong_own']
            agg[t]['r15'] += r['region_wrong15']
            c = clip_agg.setdefault((clip, t), dict(u=[], rj=0, rw=0, ro=0, r15=0))
            c['u'].append(r['usable'])
            c['rj'] += r['region_judged']
            c['rw'] += r['region_wrong']
            c['ro'] += r['region_wrong_own']
            c['r15'] += r['region_wrong15']
    L += ['', 'Mean usable share over all real candidates: new %s, %s %s. Region metric WRONG-confident (true-camera blocks / own labels): new %d / %d of %d judged, %s %d / %d of %d judged. '
        'NOISE TIER (> 1.5 px, pairs wrong beyond the near-threshold band): new %d, %s %d. '
        'Extrapolated corner WRONG-confident: new %d of %d judged, %s %d of %d judged.' % (
        f(mean(agg['new']['u'])), L2, f(mean(agg[B]['u'])), agg['new']['rw'], agg['new']['ro'], agg['new']['rj'], L2, agg[B]['rw'], agg[B]['ro'], agg[B]['rj'],
        agg['new']['r15'], L2, agg[B]['r15'],
        agg['new']['w'], agg['new']['j'], L2, agg[B]['w'], agg[B]['j']), '',
        '### Per clip (all tiers and sources)', '',
        '| clip | usable new | usable %s | WRONG@true-cam new | %s | >1.5 px new | %s | judged new | %s | WRONG rate new | %s |' % ((L2,) * 5), '|---|---|---|---|---|---|---|---|---|---|---|']
    for clip in xcheck_real.CLIPS:
        cn, cb = clip_agg.get((clip, 'new')), clip_agg.get((clip, B))
        if not cn or not cb:
            continue
        L.append('| %s | %s | %s | %d | %d | %d | %d | %d | %d | %s | %s |' % (clip, f(mean(cn['u'])), f(mean(cb['u'])), cn['rw'], cb['rw'], cn['r15'], cb['r15'], cn['rj'], cb['rj'],
                 f(cn['rw'] / cn['rj'] if cn['rj'] else None, 3), f(cb['rw'] / cb['rj'] if cb['rj'] else None, 3)))

    def pct(v, q):
        return float(np.percentile(v, q)) if v else None

    L += ['', '### Fit time (microseconds per Fitter::Fit over all real pairs)', '', '| config | pairs | p50 | p95 | max |', '|---|---|---|---|---|']
    for tag, _ in CONFIGS:
        v = fit_us[tag]
        L.append('| %s | %d | %s | %s | %s |' % (tag, len(v), f(pct(v, 50), 0), f(pct(v, 95), 0), f(max(v) if v else None, 0)))

    L += ['', '### Noise tier and dynamic-texture verdicts (real clips)', '',
          'Confident-wrong pairs beyond the noise tier (p90 error > 1.5 px) per configuration (sequence/source/pair: p90 error px), and the number of '
          'real pairs the dynamic-texture detector turned Ambiguous (verdictFlags bit 1).', '']
    for tag, _ in CONFIGS:
        rows_ = real[tag][0]
        far = ['%s/%s/t%d:%.2f' % (k[0], k[1], t, p90) for k in sorted(rows_) for (t, p90, thr) in rows_[k]['wrong_pairs'] if p90 > xcheck_real.NOISE_TIER_PX]
        nd = 0
        for fn in sorted(os.listdir(os.path.join(out, tag, 'real'))):
            if fn.endswith('.csv'):
                nd += sum(1 for r in csv.DictReader(open(os.path.join(out, tag, 'real', fn))) if int(r.get('verdictFlags') or 0) & 1)
        L.append('- %s: %d pairs > 1.5 px %s; dynamic-texture Ambiguous pairs: %d' % (tag, len(far), ('(' + ', '.join(far) + ')') if far else '', nd))
    if a.match:
        res = run_match(exe, out, a.real_root, a.match_clips.split(','), a.match_count, CONFIGS)
        L += ['', '## Matcher: plan from the previous pair (narrow window + guided candidate) vs full search', '',
              'Fit input = coded NVOF of *_1080p_low, reference = master DIS. EPE in source px vs the block mean of the reference; "camera" = blocks the '
              'reference fit labels camera. evals = guided / full. Means over pairs of the per-pair statistic (gt1 = share of blocks with EPE > 1 px).', '',
              '| clip | fit config | pairs | plan active | evals guided/full | ms full | ms guided | escalated blocks | EPE full all/cam/rest (mean) | EPE guided all/cam/rest (mean) | guided cam gt1 | full cam gt1 |',
              '|---|---|---|---|---|---|---|---|---|---|---|---|']
        for (clip, tag), r in sorted(res.items()):
            L.append('| %s | %s | %d | %s | %s | %s | %s | %s | %s/%s/%s | %s/%s/%s | %s | %s |' % (
                clip, tag, r['pairs'], f(r['active']), f(r['evals_ratio'], 3), f(r['ms_full'], 0), f(r['ms_guided'], 0), f(r['escalated'], 1),
                f(r['off_all_mean']), f(r['off_cam_mean']), f(r['off_rest_mean']), f(r['on_all_mean']), f(r['on_cam_mean']), f(r['on_rest_mean']),
                f(r['on_cam_gt1'], 3), f(r['off_cam_gt1'], 3)))
    open(os.path.join(out, 'summary.md'), 'w').write('\n'.join(L) + '\n')
    print('\n'.join(L))


if __name__ == '__main__':
    main()
