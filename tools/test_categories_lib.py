#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared parsing for the test-category manifest (tools/test-categories.json).

The unit tests are Microsoft native tests: TEST_CLASS(Name) / TEST_METHOD(Name), with an optional
BEGIN_TEST_METHOD_ATTRIBUTE(Name) ... TEST_METHOD_ATTRIBUTE(L"Category", L"X") ... END block before
the method. A method "uses the GPU" when its body references a GPU fixture (the patterns below) or
its name contains "Gpu". The manifest gives the category of tests that carry no attribute, so a
source file (Bill's LibplaceboLutParserTests.cpp) need not be edited.
"""
import json
import os
import re

GPU_PATTERNS = re.compile(r'TestDeviceParams|VP_TEST_GPU|TargetLutGpuFixture')
CLASS_RE = re.compile(r'\bTEST_CLASS\s*\(\s*(\w+)\s*\)')
METHOD_RE = re.compile(r'\bTEST_METHOD\s*\(\s*(\w+)\s*\)')
BEGIN_RE = re.compile(r'\bBEGIN_TEST_METHOD_ATTRIBUTE\s*\(\s*(\w+)\s*\)')
CAT_RE = re.compile(r'TEST_METHOD_ATTRIBUTE\s*\(\s*L"Category"\s*,\s*L"(\w+)"\s*\)')

# Categories that must not run in the CPU CI (a real device, a screen, a meter, a projector).
EXCLUDED_FROM_CPU = ['GpuHardware', 'GpuOnScreen', 'MeterHardware', 'ProjectorHardware']
KNOWN_CATEGORIES = EXCLUDED_FROM_CPU + ['GpuWarp', 'Cpu']


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_manifest(path=None):
    path = path or os.path.join(repo_root(), 'tools', 'test-categories.json')
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def scan_file(path):
    """Return a list of dicts: cls, method, category (attribute or None), gpu (bool)."""
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    events = []  # (pos, kind, name)
    for m in CLASS_RE.finditer(text):
        events.append((m.start(), 'class', m.group(1)))
    for m in BEGIN_RE.finditer(text):
        events.append((m.start(), 'begin', m.group(1)))
    for m in METHOD_RE.finditer(text):
        events.append((m.start(), 'method', m.group(1)))
    events.sort()
    cls = None
    attrs = {}
    out = []
    for i, (pos, kind, name) in enumerate(events):
        end = events[i + 1][0] if i + 1 < len(events) else len(text)
        if kind == 'class':
            cls = name
        elif kind == 'begin':
            cat = CAT_RE.search(text, pos, end)
            if cat:
                attrs[name] = cat.group(1)
        else:
            body = text[pos:end]
            out.append({'cls': cls, 'method': name, 'category': attrs.get(name),
                        'gpu': bool(GPU_PATTERNS.search(body)) or 'Gpu' in name})
    return out


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
