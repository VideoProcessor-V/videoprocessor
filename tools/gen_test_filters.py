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
             meter and projector tests out of a GPU run)
  --gpuwarp  print the selection of the GpuWarp tests (WARP-capable GPU tests), for a run with
             VP_TEST_GPU=hardware: the owner rule is that these run on WARP (CPU run) and on the real GPU
  --compare  compare the CPU filter's clauses with a canonical filter (set comparison)
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_categories_lib as lib  # noqa: E402


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
    cats = lib.EXCLUDED_FROM_CPU if cats is None else cats
    parts = clauses(manifest, False, cats) + [f'Category={c}' for c in cats]
    return '|'.join(parts)


def gpuwarp_filter(manifest):
    return gpu_filter(manifest, ['GpuWarp'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cpu', action='store_true')
    ap.add_argument('--gpu', action='store_true')
    ap.add_argument('--gpuwarp', action='store_true')
    ap.add_argument('--categories')
    ap.add_argument('--compare')
    a = ap.parse_args()
    if a.categories and not a.gpu:
        ap.error('--categories needs --gpu')
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
    sys.exit(main())
