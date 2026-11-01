# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""One command: run camera_eval on the synthetic scenarios and the real sequences, new defaults vs phase 1.

Usage (python -I):
  python -I run_eval.py --exe bin/camera_eval.exe --out OUT_DIR [--synth-root DIR] [--real-root D:/codex/td-real-vectors-out]
                        [--synth-pairs 40] [--real-frames 72] [--match] [--match-count 40] [--match-clips crowd,tos,sol,boat]

Builds nothing (build_camera_eval.bat first). Generates the synthetic fields under --synth-root if missing (default
OUT_DIR/synth_fields, never inside the repo), fits every scenario and every real sequence/source under both
configurations ('new' = current Config defaults, 'phase1' = --phase1), scores them, and writes OUT_DIR/summary.md.
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

CONFIGS = [('new', []), ('phase1', ['--phase1'])]
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


def run_match(exe, out, real_root, clips, count):
    cs = cases(real_root)
    res = {}
    for clip in clips:
        c = cs['%s_1080p_low' % clip]
        for tag, flags in CONFIGS:
            o = os.path.join(out, tag, 'match_%s_1080p_low.csv' % clip)
            cmd = [exe, 'match', '--yuv', c['coded'].replace('\\', '/'), '--w', str(c['w']), '--h', str(c['h']), '--bitdepth', '10', '--frames', str(c['frames']),
                   '--vec', os.path.join(real_root, 'vec', '%s_1080p_low' % clip, 'nvof'), '--ref', os.path.join(real_root, 'vec', '%s_master' % clip, 'disref'),
                   '--fps', str(c['fps']), '--count', str(count), '--out', o] + flags
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
    ap.add_argument('--synth-root')
    ap.add_argument('--real-root', default='D:/codex/td-real-vectors-out')
    ap.add_argument('--synth-pairs', type=int, default=40)
    ap.add_argument('--real-frames', type=int, default=72)
    ap.add_argument('--match', action='store_true')
    ap.add_argument('--match-count', type=int, default=40)
    ap.add_argument('--match-clips', default='crowd,tos,sol,boat')
    a = ap.parse_args()
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

    synth, real = {}, {}
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
        rows, truths = xcheck_real.evaluate(rd, a.real_root)
        real[tag] = ({(r['seq'], r['src']): r for r in rows}, truths)
        print('fitted', tag, flush=True)

    L = ['# Camera model evaluation: new defaults vs phase 1', '',
         'Synthetic: %d pairs per scenario at 1920x1080, %d fps. WRONG = a usable fit (valid, confidence >= 0.5) on a scene with no dominant camera '
         'motion, or with max corner error > max(1 px, 10 %% of the true corner displacement).' % (a.synth_pairs - 1, SYNTH_FPS), '',
         '## Synthetic scenarios', '',
         '| scenario | dominant | usable new | usable p1 | WRONG new | WRONG p1 | corner err med new | p1 | label prec/rec new | p1 | jitter new | p1 |',
         '|---|---|---|---|---|---|---|---|---|---|---|---|']
    tot = {t: dict(usable=[], wrong=0) for t, _ in CONFIGS}
    for n in synth_fields.SCENARIOS:
        s, p = synth[('new', n)], synth[('phase1', n)]
        L.append('| %s | %d | %s | %s | %d | %d | %s | %s | %s/%s | %s/%s | %s | %s |' % (
            n, s['dominant'], f(s['usable']), f(p['usable']), s['wrong'], p['wrong'], f(s['err_med']), f(p['err_med']),
            f(s['lab_prec']), f(s['lab_rec']), f(p['lab_prec']), f(p['lab_rec']), f(s['jitter']), f(p['jitter'])))
        for t in ('new', 'phase1'):
            tot[t]['wrong'] += synth[(t, n)]['wrong']
    L += ['', 'Total WRONG-confident pairs: new %d, phase 1 %d.' % (tot['new']['wrong'], tot['phase1']['wrong']), '',
          '## Real sequences (truth = master DIS/NVOF fits that agree)', '']
    for clip, (n, m, k) in real['new'][1].items():
        L.append('- %s: region truth on %d/%d pairs, corner truth on %d' % (clip, k, m, n))
    L += ['', 'Region metric (primary): truth = pairs where master DIS and NVOF fits are valid, share >= 50 camera blocks, p90 block difference <= 0.5 px. '
          'WRONG = confident fit whose p90 block error over the true-camera blocks (or over the fit own camera labels) exceeds max(1 px, 10 % of the p90 truth magnitude). '
          '"Extrapolated corner" columns use the max picture-corner error and include non-camera regions.', '',
          '| sequence | source | usable new | usable p1 | region judged new/p1 | WRONG@true-cam new | p1 | WRONG@own-labels new | p1 | extrapolated corner err med new | p1 | master-camera-block err med new | p1 | extrapolated corner WRONG new | p1 |',
          '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    rn, rp = real['new'][0], real['phase1'][0]
    agg = {t: dict(u=[], w=0, j=0, rj=0, rw=0, ro=0) for t in ('new', 'phase1')}
    for k in sorted(rn):
        x, y = rn[k], rp.get(k)
        if y is None:
            continue
        L.append('| %s | %s | %s | %s | %d/%d | %d | %d | %d | %d | %s | %s | %s | %s | %d | %d |' % (k[0], k[1], f(x['usable']), f(y['usable']), x['region_judged'], y['region_judged'],
                 x['region_wrong'], y['region_wrong'], x['region_wrong_own'], y['region_wrong_own'],
                 f(x['err_med']), f(y['err_med']), f(x['cam_err_med']), f(y['cam_err_med']), x['wrong'], y['wrong']))
        for t, r in (('new', x), ('phase1', y)):
            agg[t]['u'].append(r['usable'])
            agg[t]['w'] += r['wrong']
            agg[t]['j'] += r['judged']
            agg[t]['rj'] += r['region_judged']
            agg[t]['rw'] += r['region_wrong']
            agg[t]['ro'] += r['region_wrong_own']
    L += ['', 'Mean usable share over all real candidates: new %s, phase 1 %s. Region metric WRONG-confident (true-camera blocks / own labels): new %d / %d of %d judged, phase 1 %d / %d of %d judged. '
        'Extrapolated corner WRONG-confident: new %d of %d judged, phase 1 %d of %d judged.' % (
        f(mean(agg['new']['u'])), f(mean(agg['phase1']['u'])), agg['new']['rw'], agg['new']['ro'], agg['new']['rj'], agg['phase1']['rw'], agg['phase1']['ro'], agg['phase1']['rj'],
        agg['new']['w'], agg['new']['j'], agg['phase1']['w'], agg['phase1']['j'])]

    if a.match:
        res = run_match(exe, out, a.real_root, a.match_clips.split(','), a.match_count)
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
