# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""SK-MV before/after summary (doc 83, owner decision 2026-11-08 #1).

Reads the csv files written by MotionUnificationBenchTests (one per tree: label before / after) and writes a markdown table of the
per-mode means: vector error (px), trusted share, output PSNR / SSIM against the real middle frame and the hold floor, NVOF pair ms
and the Estimate wall ms. Numbers only; no film frame is read or written.

  python -I mv_compare.py --before before.csv --after after.csv --out summary.md
  python -I mv_compare.py --self-test
"""
import argparse
import csv
import collections
import os
import sys
import tempfile


def load(path):
    acc = collections.defaultdict(list)   # (clip, kind, mode, metric) -> [values]
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                acc[(r['clip'], r['kind'], r['mode'], r['metric'])].append(float(r['value']))
            except (KeyError, ValueError):
                continue
    return acc


def mean(v):
    return sum(v) / len(v) if v else float('nan')


ROWS = [('shift', 'epe_px', 'vector error, trusted blocks (px)'), ('shift', 'trusted_share', 'trusted share'),
        ('shift', 'estimate_ms', 'estimate wall (ms)'), ('shift', 'pair_ms', 'NVOF pair (ms, producer)'),
        ('interp', 'psnr_db', 'output PSNR vs the real middle frame (dB)'), ('interp', 'ssim', 'output SSIM'),
        ('interp', 'hold_psnr_db', 'floor: hold A, PSNR (dB)'), ('interp', 'hold_ssim', 'floor: hold A, SSIM'),
        ('interp', 'estimate_ms', 'estimate wall (ms)'), ('interp', 'pair_ms', 'NVOF pair (ms, producer)')]


def report(before, after):
    clips = sorted({k[0] for k in list(before) + list(after)})
    out = ['# SK-MV single NVOF source: before / after', '',
           'before = the de-judder opens its own NVOF session when no pair came ahead (mode nopair); after = no second session, the matcher runs (mode nopair).',
           'Mode pair is the producer pair through the same path in both trees (it should not move).', '']
    for clip in clips:
        out += ['## ' + clip, '', '| measure | mode | before | after | change |', '|---|---|---|---|---|']
        for kind, metric, text in ROWS:
            for mode in ('nopair', 'pair'):
                b = mean(before.get((clip, kind, mode, metric), []))
                a = mean(after.get((clip, kind, mode, metric), []))
                if b != b and a != a:
                    continue
                d = a - b if (a == a and b == b) else float('nan')
                out.append('| %s (%s) | %s | %.4f | %.4f | %+.4f |' % (text, kind, mode, b, a, d))
        out.append('')
    out += ['Plausibility checks: vector error must stay well below the shift magnitude (8-32 px); hold-A PSNR is the floor the warp must beat;',
            'the pair mode must be unchanged between trees (same code path); nopair/before pair_ms is absent by design.']
    return '\n'.join(out) + '\n'


def self_test():
    with tempfile.TemporaryDirectory() as d:
        for label, epe in (('before', 0.4), ('after', 0.9)):
            with open(os.path.join(d, label + '.csv'), 'w', newline='') as f:
                f.write('label,clip,kind,mode,position,metric,value\n')
                f.write('%s,c.y4m,shift,nopair,0,epe_px,%s\n' % (label, epe))
                f.write('%s,c.y4m,interp,nopair,0,psnr_db,30\n' % label)
        text = report(load(os.path.join(d, 'before.csv')), load(os.path.join(d, 'after.csv')))
        assert '| 0.4000 | 0.9000 | +0.5000 |' in text, text
    print('self-test ok')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--before')
    ap.add_argument('--after')
    ap.add_argument('--out')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not (a.before and a.after and a.out):
        ap.error('--before, --after and --out are required')
    text = report(load(a.before), load(a.after))
    with open(a.out, 'w', encoding='utf-8') as f:
        f.write(text)
    print('wrote', a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
