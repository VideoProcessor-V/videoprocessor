# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""SK-MV before/after summary (doc 83, owner decision 2026-11-08 #1).

Reads the csv files written by MotionUnificationBenchTests (one per tree: label before / after) and writes a markdown table of the
per-mode means: vector error (source px; epe_half_px in the matcher's half-size px), trusted share, output PSNR / SSIM against the real
middle frame and the hold floor, NVOF pair ms and the Estimate wall ms. Means compare the SAME positions only (a position missing a
metric in either tree is left out of both means). Skipped / failed / no-pair rows are counted and printed per tree, so a half-empty
run cannot look like a clean one. Numbers only; no film frame is read or written.

  python -I mv_compare.py --before before.csv --after after.csv --out summary.md
  python -I mv_compare.py --self-test
"""
import argparse
import csv
import collections
import os
import sys
import tempfile

COUNT_METRICS = ('no_pair', 'estimate_failed')


def load(path):
    """(clip, kind, mode, metric) -> {position: value}"""
    acc = collections.defaultdict(dict)
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                acc[(r['clip'], r['kind'], r['mode'], r['metric'])][int(float(r['position']))] = float(r['value'])
            except (KeyError, ValueError):
                continue
    return acc


def mean(v):
    return sum(v) / len(v) if v else float('nan')


ROWS = [('shift', 'epe_px', 'vector error, trusted blocks (source px)'), ('shift', 'epe_half_px', 'vector error (half-size px, the field unit)'),
        ('shift', 'trusted_share', 'trusted share'), ('shift', 'estimate_ms', 'estimate wall (ms)'), ('shift', 'pair_ms', 'NVOF pair (ms, producer)'),
        ('shift', 'engine_used', 'optical-flow input used (1 = yes)'),
        ('interp', 'psnr_db', 'output PSNR vs the real middle frame (dB)'), ('interp', 'ssim', 'output SSIM'),
        ('interp', 'hold_psnr_db', 'floor: hold A, PSNR (dB)'), ('interp', 'hold_ssim', 'floor: hold A, SSIM'),
        ('interp', 'estimate_ms', 'estimate wall (ms)'), ('interp', 'pair_ms', 'NVOF pair (ms, producer)'),
        ('interp', 'engine_used', 'optical-flow input used (1 = yes)')]


STEADY_MODES = ('a_steady_lane', 'b_lowlatency', 'c_warm_only')
STEADY_ROWS = [('width', 'width'), ('height', 'height'), ('fps', 'rate (fps)'), ('render_ms_mean', 'render path GPU ms (mean, drained)'),
               ('render_ms_p95', 'render path GPU ms (p95)'), ('lane_pair_ms_mean', 'lane: NVOF pair ms (mean)'),
               ('nvof_occupancy', 'NVOF occupancy (pair ms / wall)'), ('pair_latency_ms_mean', 'pair latency after hand-over (ms)'),
               ('vram_lane_mb', 'VRAM: producer engine + ring (MB over start)'), ('vram_render_mb', 'VRAM: matcher on the render path (MB)'),
               ('ring_bytes_mb', 'engine ring textures by formula (MB)'), ('render_estimates', 'render estimates measured'), ('lane_pairs', 'lane pairs measured')]


def steady_section(before, after, clip):
    out = []
    for metric, text in STEADY_ROWS:
        for mode in STEADY_MODES:
            b = before.get((clip, 'steady', mode, metric), {})
            a = after.get((clip, 'steady', mode, metric), {})
            if not a and not b:
                continue
            vb = b.get(0, float('nan'))
            va = a.get(0, float('nan'))
            d = va - vb if (vb == vb and va == va) else float('nan')
            out.append('| %s | %s | %.4f | %.4f | %+.4f |' % (text, mode, vb, va, d))
    if not out:
        return []
    return ['', 'Steady state per mode (one run each; 1080p60 and 4K30 are separate clips = separate sections):', '',
            '| measure | mode | before | after | change |', '|---|---|---|---|---|'] + out


def counts(acc, clip):
    out = []
    for kind in ('shift', 'interp'):
        for mode in ('nopair', 'pair'):
            c = {m: len(acc.get((clip, kind, mode, m), {})) for m in COUNT_METRICS}
            ok = len(acc.get((clip, kind, mode, 'estimate_ms'), {}))
            out.append((kind, mode, ok, c['no_pair'], c['estimate_failed']))
    return out


def report(before, after):
    clips = sorted({k[0] for k in list(before) + list(after)})
    out = ['# SK-MV single NVOF source: before / after', '',
           'before = the de-judder opens its own NVOF session when no pair came ahead (mode nopair); after = no second session, the matcher runs (mode nopair).',
           'Mode pair is the producer pair through the same path in both trees (it should not move). Means use the positions present in BOTH trees.', '']
    for clip in clips:
        out += ['## ' + clip, '', 'Run completeness (positions with a result / no_pair rows / estimate_failed rows):', '',
                '| kind | mode | before ok/no_pair/failed | after ok/no_pair/failed |', '|---|---|---|---|']
        cb, ca = counts(before, clip), counts(after, clip)
        for (kind, mode, okb, npb, fb), (_, _, oka, npa, fa) in zip(cb, ca):
            out.append('| %s | %s | %d / %d / %d | %d / %d / %d |' % (kind, mode, okb, npb, fb, oka, npa, fa))
        out += ['', '| measure | mode | positions | before | after | change |', '|---|---|---|---|---|---|']
        for kind, metric, text in ROWS:
            for mode in ('nopair', 'pair'):
                b = before.get((clip, kind, mode, metric), {})
                a = after.get((clip, kind, mode, metric), {})
                common = sorted(set(b) & set(a))
                if not common and not b and not a:
                    continue
                mb = mean([b[p] for p in common])
                ma = mean([a[p] for p in common])
                d = ma - mb if (mb == mb and ma == ma) else float('nan')
                out.append('| %s (%s) | %s | %d (before %d, after %d) | %.4f | %.4f | %+.4f |' % (text, kind, mode, len(common), len(b), len(a), mb, ma, d))
        out += steady_section(before, after, clip)
        out.append('')
    out += ['Plausibility checks: vector error must stay well below the shift magnitude (8-32 px); hold-A PSNR is the floor the warp must beat;',
            'the pair mode must be unchanged between trees (same code path); engine_used must be 1 for mode pair; nopair/before pair_ms is absent by design.']
    return '\n'.join(out) + '\n'


def self_test():
    with tempfile.TemporaryDirectory() as d:
        for label, epe, extra in (('before', 0.4, True), ('after', 0.9, False)):
            with open(os.path.join(d, label + '.csv'), 'w', newline='') as f:
                f.write('label,clip,kind,mode,position,metric,value\n')
                f.write('%s,c.y4m,shift,nopair,0,epe_px,%s\n' % (label, epe))
                f.write('%s,c.y4m,shift,nopair,0,estimate_ms,5\n' % label)
                f.write('%s,c.y4m,interp,nopair,0,psnr_db,30\n' % label)
                if extra:   # a position only the before tree has: left out of both means
                    f.write('%s,c.y4m,shift,nopair,12,epe_px,9.0\n' % label)
                else:
                    f.write('%s,c.y4m,shift,pair,12,no_pair,1\n' % label)
        text = report(load(os.path.join(d, 'before.csv')), load(os.path.join(d, 'after.csv')))
        assert '| 1 (before 2, after 1) | 0.4000 | 0.9000 | +0.5000 |' in text, text
        assert '| shift | pair | 0 / 0 / 0 | 0 / 1 / 0 |' in text, text
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
