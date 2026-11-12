#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Pure-move check (no build). For each commit, every code line REMOVED from src/ must reappear among the lines
ADDED in that commit, after normalisation: whitespace/comments/blank/brace-only lines dropped, impl-pointer and
`vpv.<group>.<sub>.` qualifiers stripped, `Impl::` out-of-line qualifiers stripped, leading `static` dropped and
(2.7) alias names mapped back to their `obj.field` target. Lines that do not reappear are mismatches, except pure
alias-reference declarations (`T& n = obj.f;`) with --allow-alias-deletion.
Usage: check_pure_move.py <commit>... [--allow-alias-deletion]   (exit 1 on any mismatch)"""
import re, subprocess, sys
from collections import Counter

OBJ = r'(?:dejudderPolicy|vectorEngine|vectorEstimate|renderAheadPresenter)'
PFX = re.compile(r'(?:\bm_impl->|\bimpl\.|\bimpl_\.|\bvpv\.(?:led\.sdrLift|calibration|sourceAnalysis)\.|\b' + OBJ + r'\.(?=\w+\b(?!\s*\()))')
ALIAS_DECL = re.compile(r'^[\w:<>,\*\s]+&\s*(\w+)\s*=\s*' + OBJ + r'\.(\w+);$')
amap = {}

def norm(l):
    l = l.strip()
    if not l or l.startswith('//') or l in ('{', '}', '};') or re.match(r'^struct\s+\w+$', l):
        return None
    l = re.sub(r'\s*//.*$', '', l)
    l = re.sub(r'struct \{ struct \{ (.*?;) \} sourceAnalysis; \} vpv;', r'', l)   # test fake re-pointed to vpv.sourceAnalysis
    l = PFX.sub('', l)
    l = re.sub(r'\b\w+::Impl::', '', l)
    l = re.sub(r'^static\s+', '', l)
    for f, n in amap.items():
        l = re.sub(r'\b' + f + r'\b', n, l)
    l = re.sub(r'\s+', '', l)
    return l or None

def main():
    allow = '--allow-alias-deletion' in sys.argv
    total = 0
    for c in [a for a in sys.argv[1:] if not a.startswith('--')]:
        d = subprocess.run(['git', 'show', '-U0', '--format=', c, '--', 'src/'], capture_output=True, text=True,
                           encoding='utf-8', errors='replace').stdout.split('\n')
        amap.clear()
        for l in d:
            if l.startswith('-'):
                m = ALIAS_DECL.match(re.sub(r'\s*//.*$', '', l[1:]).strip())
                if m: amap[m.group(2)] = m.group(1)
        rem, add = [], Counter()
        for l in d:
            if l.startswith(('+++', '---')) or l[:1] not in ('+', '-'): continue
            n = norm(l[1:])
            if n is None: continue
            if l[0] == '-': rem.append((n, l[1:].strip()))
            else: add[n] += 1
        bad = []
        for n, raw in rem:
            if add[n] > 0: add[n] -= 1
            elif allow and ALIAS_DECL.match(re.sub(r'\s*//.*$', '', raw).strip()): pass
            else: bad.append(raw)
        print(c[:9], 'removed code lines', len(rem), 'mismatches', len(bad))
        for b in bad: print('   MISMATCH', b[:160])
        total += len(bad)
    print('TOTAL mismatches', total)
    return 1 if total else 0

sys.exit(main())
