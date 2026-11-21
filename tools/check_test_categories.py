#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Fails when a test that uses the GPU has neither a Category attribute nor a manifest entry.

CPU only, seconds. Also fails on a manifest entry that matches no test (stale), an unknown
category, an entry whose category disagrees with a Category attribute on the same test, and a Category
attribute that names no known category (a typo such as Footge would leave the test in the CPU run, TQ-X2).
Footage, Corpus and Bench are excluded categories: their tests run only in a dedicated job (see
tools/gen_test_filters.py --job-env).
Usage: python tools/check_test_categories.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_categories_lib as lib  # noqa: E402


def main():
    root = lib.repo_root()
    manifest = lib.load_manifest()
    problems = []
    used = set()
    total = gpu = 0
    for path in lib.test_sources(root):
        rel = os.path.relpath(path, root).replace('\\', '/')
        for t in lib.scan_file(path):
            total += 1
            cls, method = t['cls'], t['method']
            for i, e in enumerate(manifest['entries']):
                if lib.entry_matches(e, cls, method):
                    used.add(i)
            best = lib.best_entry(manifest['entries'], cls, method)   # the most specific entry wins, not the first (TQ-RG-C4)
            mapped = best['category'] if best else None
            if t['category'] and t['category'] not in lib.KNOWN_CATEGORIES:
                problems.append(f'{rel}: {cls}::{method} has an unknown Category attribute {t["category"]}')
            if t['gpu']:
                gpu += 1
                if not t['category'] and not mapped:
                    problems.append(f'{rel}: {cls}::{method} uses the GPU and has no category (attribute or manifest entry)')
            if t['category'] and mapped and (t['category'] in lib.EXCLUDED_FROM_CPU) != (mapped in lib.EXCLUDED_FROM_CPU):
                problems.append(f'{rel}: {cls}::{method} attribute {t["category"]} disagrees with the manifest ({mapped})')
    for i, e in enumerate(manifest['entries']):
        if e['category'] not in lib.KNOWN_CATEGORIES:
            problems.append(f'manifest entry {e["value"]}: unknown category {e["category"]}')
        if i not in used:
            problems.append(f'manifest entry {e["kind"]} {e["value"]} matches no test')
    print(f'{total} tests scanned, {gpu} use the GPU, {len(manifest["entries"])} manifest entries')
    for p in problems:
        print('FAIL ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except lib.ScanError as e:
        print(f'FAIL {e}')
        sys.exit(2)
