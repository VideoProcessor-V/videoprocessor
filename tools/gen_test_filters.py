#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Generates the vstest /TestCaseFilter that keeps GPU/hardware tests out of the CPU run.

The native test adapter cannot be trusted to select by trait alone, so the filter lists the
excluded categories AND the name clauses of every manifest entry in an excluded category.
Usage: python tools/gen_test_filters.py [--cpu] [--gpu [--categories A,B]] [--gpuwarp] [--compare "<filter>"]
  --cpu      print the CPU filter (default)
  --gpu      print the complementary selection (excluded categories and manifest entries, OR-ed)
  --categories  with --gpu: restrict to these excluded categories (e.g. GpuHardware,GpuOnScreen keeps the
             meter and projector tests out of a GPU run; UpstreamStale selects only the quarantined
             upstream-stale Bill tests, which the CPU run leaves out)
  --job-env CATEGORY  print the environment of that category's dedicated job as a queue-line env= value
             (Footage, Corpus, Bench: a missing clip, corpus or opt-in FAILS in that job). Pair it with
             --gpu --categories CATEGORY for the filter; the categories are in no other lane (TQ-X2)
  --gpuwarp  print the selection of the GpuWarp tests (WARP-capable GPU tests), for a run with
             VP_TEST_GPU=hardware: the owner rule is that these run on WARP (CPU run) and on the real GPU
  --compare  compare the CPU filter's clauses with a canonical filter (set comparison)
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import categories_lib as lib  # noqa: E402


def clause(entry, negate):
    kind, value = entry['kind'], entry['value']
    bang = '!' if negate else ''
    if kind in ('class', 'classPrefix'):
        return f'FullyQualifiedName{bang}~{value}'
    if kind == 'method':
        return f'Name{bang}~{value.split("::")[-1]}'
    return f'Name{bang}~{value}'


def attribute_names(manifest, cats):
    """Methods carrying a Category attribute in cats that no manifest entry of cats already covers."""
    names = []
    for path in lib.test_sources(lib.repo_root()):
        for t in lib.scan_file(path):
            if t['category'] not in cats:
                continue
            if any(e['category'] in cats and lib.entry_matches(e, t['cls'], t['method'])
                   for e in manifest['entries']):
                continue
            if t['method'] not in names:
                names.append(t['method'])
    return sorted(names)


def clauses(manifest, negate, cats=None):
    cats = lib.EXCLUDED_FROM_CPU if cats is None else cats
    out = []
    for e in manifest['entries']:
        if e['category'] in cats:
            c = clause(e, negate)
            if c not in out:
                out.append(c)
    bang = '!' if negate else ''
    for n in attribute_names(manifest, cats):
        out.append(f'Name{bang}~{n}')
    return out


def cpu_filter(manifest):
    parts = clauses(manifest, True) + [f'Category!={c}' for c in lib.EXCLUDED_FROM_CPU]
    return '&'.join(parts)


def gpu_filter(manifest, cats=None):
    cats = lib.GPU_DEFAULT_EXCLUDED if cats is None else cats
    parts = clauses(manifest, False, cats) + [f'Category={c}' for c in cats]
    return '|'.join(parts)


def shadowed_in_warp_classes(manifest):
    """(class, method, entry) for each test inside a class that a GpuWarp class entry selects but whose most specific
    classification is another excluded category (a Slow methodPrefix, a GpuHardware attribute): it must leave the GpuWarp run
    (TQ-RG-C4: WholeHookChainCombinationMatrix is Slow inside the GpuWarp class WholeHookChainHookTests)."""
    entries = manifest['entries']
    warp_classes = [e for e in entries if e['category'] == 'GpuWarp' and e['kind'] in ('class', 'classPrefix')]
    out = []
    for path in lib.test_sources(lib.repo_root()):
        for t in lib.scan_file(path):
            cls, method = t['cls'], t['method']
            if not any(lib.entry_matches(e, cls, method) for e in warp_classes):
                continue
            best = lib.best_entry(entries, cls, method)
            effective = best['category'] if best and best['kind'] in ('method', 'methodPrefix') else t['category']
            if effective in lib.EXCLUDED_FROM_CPU and (cls, method) not in [(c, m) for c, m, _ in out]:
                out.append((cls, method, best))
    return out


def gpuwarp_filter(manifest):
    """The GpuWarp selection: the class entries stay, each ANDed with FullyQualifiedName!~Class::method for every test of the
    class that is classified in another excluded category (the AND binds tighter than the OR of the clauses)."""
    shadow = {}
    for cls, method, _entry in shadowed_in_warp_classes(manifest):
        shadow.setdefault(cls, []).append(method)
    parts = []
    for c in clauses(manifest, False, ['GpuWarp']):
        value = c.split('~', 1)[-1]
        if c.startswith('FullyQualifiedName~'):
            exclusions = [f'FullyQualifiedName!~{cls}::{m}' for cls, methods in shadow.items()
                          if cls == value or cls.startswith(value) for m in methods]
            c = '&'.join([c] + sorted(set(exclusions)))
        parts.append(c)
    parts.append('Category=GpuWarp')
    return '|'.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cpu', action='store_true')
    ap.add_argument('--gpu', action='store_true')
    ap.add_argument('--gpuwarp', action='store_true')
    ap.add_argument('--categories')
    ap.add_argument('--compare')
    ap.add_argument('--job-env')
    a = ap.parse_args()
    if a.categories and not a.gpu:
        ap.error('--categories needs --gpu')
    if a.job_env:
        if a.job_env not in lib.JOB_ENV:
            ap.error(f'--job-env takes {sorted(lib.JOB_ENV)}, not {a.job_env}')
        print(';'.join(f'{k}={v}' for k, v in lib.JOB_ENV[a.job_env].items()))
        return 0
    manifest = lib.load_manifest()
    if a.compare:
        have = set(cpu_filter(manifest).split('&'))
        want = set(a.compare.split('&'))
        print('missing from generated:', sorted(want - have) or 'none')
        print('extra in generated:', sorted(have - want) or 'none')
        return 1 if want - have else 0
    if a.gpuwarp:
        print(gpuwarp_filter(manifest))
    elif a.gpu:
        cats = None
        if a.categories:
            cats = [c for c in a.categories.split(',') if c]
            bad = [c for c in cats if c not in lib.EXCLUDED_FROM_CPU]
            if bad:
                ap.error(f'--categories takes excluded categories {lib.EXCLUDED_FROM_CPU}, not {bad}')
        print(gpu_filter(manifest, cats))
    else:
        print(cpu_filter(manifest))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except lib.ScanError as e:
        # No filter is printed: test_filters.ps1 then throws, so no run uses a filter built on a misread source.
        print(f'gen_test_filters: error: {e}', file=sys.stderr)
        sys.exit(2)
