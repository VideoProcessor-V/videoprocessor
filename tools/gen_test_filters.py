#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Generates the vstest /TestCaseFilter that keeps GPU/hardware tests out of the CPU run.

The native test adapter cannot be trusted to select by trait alone, so the filter lists the
excluded categories AND the name clauses of every manifest entry in an excluded category.
Usage: python tools/gen_test_filters.py [--cpu] [--gpu] [--compare "<filter>"]
  --cpu      print the CPU filter (default)
  --gpu      print the complementary selection (excluded categories and manifest entries, OR-ed)
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


def attribute_names(manifest):
    """Methods carrying an excluded Category attribute that no excluded manifest entry already covers."""
    names = []
    for path in lib.test_sources(lib.repo_root()):
        for t in lib.scan_file(path):
            if t['category'] not in lib.EXCLUDED_FROM_CPU:
                continue
            if any(e['category'] in lib.EXCLUDED_FROM_CPU and lib.entry_matches(e, t['cls'], t['method'])
                   for e in manifest['entries']):
                continue
            if t['method'] not in names:
                names.append(t['method'])
    return sorted(names)


def clauses(manifest, negate):
    out = []
    for e in manifest['entries']:
        if e['category'] in lib.EXCLUDED_FROM_CPU:
            c = clause(e, negate)
            if c not in out:
                out.append(c)
    bang = '!' if negate else ''
    for n in attribute_names(manifest):
        out.append(f'Name{bang}~{n}')
    return out


def cpu_filter(manifest):
    parts = clauses(manifest, True) + [f'Category!={c}' for c in lib.EXCLUDED_FROM_CPU]
    return '&'.join(parts)


def gpu_filter(manifest):
    parts = clauses(manifest, False) + [f'Category={c}' for c in lib.EXCLUDED_FROM_CPU]
    return '|'.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cpu', action='store_true')
    ap.add_argument('--gpu', action='store_true')
    ap.add_argument('--compare')
    a = ap.parse_args()
    manifest = lib.load_manifest()
    if a.compare:
        have = set(cpu_filter(manifest).split('&'))
        want = set(a.compare.split('&'))
        print('missing from generated:', sorted(want - have) or 'none')
        print('extra in generated:', sorted(have - want) or 'none')
        return 1 if want - have else 0
    print(gpu_filter(manifest) if a.gpu else cpu_filter(manifest))
    return 0


if __name__ == '__main__':
    sys.exit(main())
