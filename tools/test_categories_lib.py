#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared parsing for the test-category manifest (tools/test-categories.json).

The unit tests are Microsoft native tests: TEST_CLASS(Name) / TEST_METHOD(Name), with an optional
BEGIN_TEST_METHOD_ATTRIBUTE(Name) ... TEST_METHOD_ATTRIBUTE(L"Category", L"X") ... END block before
the method. A method "uses the GPU" when its body references a GPU fixture (the patterns below) or
its name has the word "gpu" (any case). The manifest gives the category of tests that carry no attribute, so a
source file (Bill's LibplaceboLutParserTests.cpp) need not be edited.
"""
import json
import os
import re

GPU_PATTERNS = re.compile(r'TestDeviceParams|VP_TEST_GPU|TargetLutGpuFixture|pl_d3d11_create|D3D11CreateDevice|EnumAdapters|CreateDXGIFactory')
CLASS_RE = re.compile(r'\bTEST_CLASS\s*\(\s*(\w+)\s*\)')
METHOD_RE = re.compile(r'\bTEST_METHOD\s*\(\s*(\w+)\s*\)')
BEGIN_RE = re.compile(r'\bBEGIN_TEST_METHOD_ATTRIBUTE\s*\(\s*(\w+)\s*\)')
CAT_RE = re.compile(r'TEST_METHOD_ATTRIBUTE\s*\(\s*L"Category"\s*,\s*L"(\w+)"\s*\)')

# Bill's own tests that a later Bill (upstream) commit made stale, with Bill's code unchanged in
# VP-V on both pathways: kept out of the CPU run (like every excluded category, they stay in the
# complementary --gpu selection) and listed by name in the manifest with the upstream commit; run
# only them with --gpu --categories UpstreamStale.
QUARANTINED = ['UpstreamStale']
# Categories that must not run in the CPU CI (a real device, a screen, a meter, a projector).
# LoopRig: needs the DeckLink loop rig; run by tools/looprig/run_loop_tier.ps1 only. Excluded from the CPU
# run AND from the default --gpu set (GPU_DEFAULT_EXCLUDED); select it with --gpu --categories LoopRig.
EXCLUDED_FROM_CPU = ['GpuHardware', 'GpuOnScreen', 'MeterHardware', 'ProjectorHardware', 'Slow', 'LoopRig'] + QUARANTINED
GPU_DEFAULT_EXCLUDED = [c for c in EXCLUDED_FROM_CPU if c != 'LoopRig']
KNOWN_CATEGORIES = EXCLUDED_FROM_CPU + ['GpuWarp', 'Cpu']


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_manifest(path=None):
    path = path or os.path.join(repo_root(), 'tools', 'test-categories.json')
    with open(path, encoding='utf-8') as f:
        return json.load(f)


WORD_RE = re.compile(r'[A-Z]+(?![a-z])|[A-Z]?[a-z]+|[0-9]+')


def name_says_gpu(name):
    # A whole camel/snake-case word "gpu" in any case (WaitingPulse does not count).
    return any(w.lower() == 'gpu' for w in WORD_RE.findall(name))


class ScanError(Exception):
    """A test source the scanner cannot read reliably (callers print it and exit non-zero)."""


# A #define logical line: the directive and every backslash-continued line after it.
DEFINE_RE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)(\([^)]*\))?((?:[^\n]*\\\r?\n)*[^\n]*)', re.M)
# An ALL-CAPS name (a macro parameter such as NAME), never a real test name: MSTest's Name~ / Name!~ is a
# case-insensitive substring match, so a clause on it would select or drop every test whose name holds it.
PLACEHOLDER_RE = re.compile(r'^[A-Z][A-Z0-9_]*$')


def _split_args(text, start):
    """Top-level comma-separated arguments of the call whose '(' is at text[start]; None if it never closes."""
    depth, args, cur = 0, [], []
    for i in range(start, len(text)):
        ch = text[i]
        if ch == '(':
            depth += 1
            if depth > 1:
                cur.append(ch)
        elif ch == ')':
            depth -= 1
            if depth == 0:
                args.append(''.join(cur).strip())
                return args
            cur.append(ch)
        elif ch == ',' and depth == 1:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    return None


def strip_defines(text):
    """(text with every #define blanked, same length and line breaks; {macro: (param index, category, gpu)}).

    A macro whose body declares TEST_METHOD(<one of its parameters>) is a test-method wrapper: each of its
    invocations is a test named by that argument, with the body's Category attribute and GPU patterns."""
    wrappers = {}
    out = list(text)
    for m in DEFINE_RE.finditer(text):
        name, params, body = m.group(1), m.group(2), m.group(3)
        for k in range(m.start(), m.end()):
            if out[k] not in '\r\n':
                out[k] = ' '
        if not params:
            continue
        plist = [p.strip() for p in params[1:-1].split(',')]
        tm = METHOD_RE.search(body)
        if tm and tm.group(1) in plist:
            cat = CAT_RE.search(body)
            wrappers[name] = (plist.index(tm.group(1)), cat.group(1) if cat else None, bool(GPU_PATTERNS.search(body)))
    return ''.join(out), wrappers


def scan_text(text, path='<text>'):
    """scan_file on source text (see scan_file)."""
    text, wrappers = strip_defines(text)
    events = []  # (pos, kind, name, wrapper spec or None)
    for m in CLASS_RE.finditer(text):
        events.append((m.start(), 'class', m.group(1), None))
    for m in BEGIN_RE.finditer(text):
        events.append((m.start(), 'begin', m.group(1), None))
    for m in METHOD_RE.finditer(text):
        events.append((m.start(), 'method', m.group(1), None))
    for macro, spec in wrappers.items():
        for m in re.finditer(r'\b' + re.escape(macro) + r'\s*\(', text):
            args = _split_args(text, m.end() - 1)
            if args is not None and spec[0] < len(args) and re.fullmatch(r'\w+', args[spec[0]]):
                events.append((m.start(), 'method', args[spec[0]], spec))
    events.sort(key=lambda e: e[0])
    cls = None
    attrs = {}
    out = []
    for i, (pos, kind, name, spec) in enumerate(events):
        end = events[i + 1][0] if i + 1 < len(events) else len(text)
        if kind == 'class':
            cls = name
        elif kind == 'begin':
            cat = CAT_RE.search(text, pos, end)
            if cat:
                attrs[name] = cat.group(1)
        else:
            if PLACEHOLDER_RE.match(name):
                raise ScanError(f'{path}: test method name {name!r} is an ALL-CAPS placeholder (a macro parameter?): '
                                'a Name~ clause on it would match every test whose name contains it, in any case')
            body = text[pos:end]
            category = spec[1] if spec is not None and spec[1] else attrs.get(name)
            gpu = bool(GPU_PATTERNS.search(body)) or name_says_gpu(name) or bool(spec is not None and spec[2])
            out.append({'cls': cls, 'method': name, 'category': category, 'gpu': gpu})
    return out


def scan_file(path):
    """Return a list of dicts: cls, method, category (attribute or None), gpu (bool).

    A #define body is not scanned as code (its TEST_METHOD(NAME) names a parameter, not a test); each invocation of
    a test-method wrapper macro is a test named by its argument. Raises ScanError on an ALL-CAPS placeholder name."""
    with open(path, encoding='utf-8', errors='replace') as f:
        return scan_text(f.read(), path)


def entry_matches(entry, cls, method):
    kind, value = entry['kind'], entry['value']
    if kind == 'class':
        return cls == value
    if kind == 'classPrefix':
        return bool(cls) and cls.startswith(value)
    if kind == 'method':
        return f'{cls}::{method}' == value
    if kind == 'methodPrefix':
        return method.startswith(value)
    raise ValueError(f'unknown entry kind {kind}')


def test_sources(root):
    for base, _dirs, files in os.walk(os.path.join(root, 'src')):
        for name in files:
            if name.endswith('Tests.cpp'):
                yield os.path.join(base, name)
