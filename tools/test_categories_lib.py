#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared parsing for the test-category manifest (tools/test-categories.json).

The unit tests are Microsoft native tests: TEST_CLASS(Name) / TEST_METHOD(Name), with an optional
BEGIN_TEST_METHOD_ATTRIBUTE(Name) ... TEST_METHOD_ATTRIBUTE(L"Category", L"X") ... END block before
the method. A method "uses the GPU" when its body references a GPU fixture (the patterns below) or
its name has the word "gpu" (any case). The manifest gives the category of tests that carry no attribute, so a
source file (Bill's LibplaceboLutParserTests.cpp) need not be edited.

Rules for test authors:
- A test-method name must not be ALL CAPS (^[A-Z][A-Z0-9_]*$, e.g. TEST_METHOD(HDR)): the scan raises
  ScanError, gen_test_filters exits 2 with no filter and check_test_categories FAILs. Such a name looks
  like a macro parameter, and vstest's Name~ / Name!~ is a case-insensitive substring match, so a clause
  on it would select or drop every test whose name holds it. Use mixed case (HdrOutputStaysTen).
- Test macros are found in code only: comments and string / character literals are blanked first, so a
  commented-out TEST_METHOD or a wrapper call inside a comment or string is not a test.
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
# Categories that must not run in the CPU CI (a real device, a screen, a meter, a projector, real footage,
# a corpus, a benchmark).
# LoopRig: needs the DeckLink loop rig; run by tools/looprig/run_loop_tier.ps1 only.
# Footage / Corpus / Bench (TQ-X2): tests whose input (real clips, a generated corpus) or opt-in (a timing
# benchmark) is absent in the CPU and WARP lanes, where HardwarePrerequisites::RequireOrSkip would log a
# SKIP-NO-FOOTAGE / SKIP-NO-CORPUS / SKIP-OPT-IN marker and PASS. They run only in a dedicated job that
# sets JOB_ENV, so a missing clip or corpus FAILS there (VP_TEST_REQUIRE_<KIND>=1); in every other lane they
# are not selected at all. Like LoopRig they are excluded from the CPU run AND from the default --gpu set
# (DEDICATED_JOB_ONLY); select one with --gpu --categories Footage (and print its env with --job-env Footage).
# Category -> the environment its dedicated job must set (queue-line env= field, or the chain gate switch).
# Bench is a timing benchmark that is opt-in by VP_TEST_BENCH=1: the job opts in and requires it.
JOB_ENV = {
    'Footage': {'VP_TEST_REQUIRE_FOOTAGE': '1'},
    'Corpus': {'VP_TEST_REQUIRE_CORPUS': '1'},
    'Bench': {'VP_TEST_BENCH': '1', 'VP_TEST_REQUIRE_BENCH': '1'},
}
DEDICATED_JOB_ONLY = ['LoopRig'] + list(JOB_ENV)
EXCLUDED_FROM_CPU = ['GpuHardware', 'GpuOnScreen', 'MeterHardware', 'ProjectorHardware', 'Slow'] + DEDICATED_JOB_ONLY + QUARANTINED
GPU_DEFAULT_EXCLUDED = [c for c in EXCLUDED_FROM_CPU if c not in DEDICATED_JOB_ONLY]
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


def blank_comments_and_strings(text):
    """text with every // and /* */ comment and every string / character literal (incl. raw R"d(...)d")
    blanked to spaces, same length and line breaks. Used only to FIND test macros (TEST_CLASS, TEST_METHOD,
    BEGIN_TEST_METHOD_ATTRIBUTE, wrapper calls), so a comment such as "D8_ROW_METHOD(Foo, ...)" or a string
    holding "TEST_METHOD(X)" adds no phantom test; attribute values and GPU patterns are still read from the
    unblanked text. A ' between digits (C++14 digit separator, 1'000) is not a literal."""
    out = list(text)
    n, i = len(text), 0

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] not in '\r\n':
                out[k] = ' '

    while i < n:
        ch = text[i]
        if ch == '/' and text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif ch == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif ch == '"':
            raw = re.match(r'R"([^ ()\\\t\r\n]{0,16})\(', text[i - 1:i + 19]) if i > 0 and text[i - 1] == 'R' else None
            if raw:
                close = ')' + raw.group(1) + '"'
                j = text.find(close, i)
                j = n if j < 0 else j + len(close)
            else:
                j = i + 1
                while j < n and text[j] != '"' and text[j] != '\n':
                    j += 2 if text[j] == '\\' else 1
                j = min(j + 1, n)
            blank(i - 1 if raw else i, j)
            i = j
        elif ch == "'":
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] in "_.'"):
                k -= 1
            if k + 1 < i and text[k + 1].isdigit():   # digit separator inside a number
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            blank(i, j)
            i = j
        else:
            i += 1
    return ''.join(out)


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
    code = blank_comments_and_strings(text)   # macro search only: comments and literals add no test
    events = []  # (pos, kind, name, wrapper spec or None)
    for m in CLASS_RE.finditer(code):
        events.append((m.start(), 'class', m.group(1), None))
    for m in BEGIN_RE.finditer(code):
        events.append((m.start(), 'begin', m.group(1), None))
    for m in METHOD_RE.finditer(code):
        events.append((m.start(), 'method', m.group(1), None))
    for macro, spec in wrappers.items():
        for m in re.finditer(r'\b' + re.escape(macro) + r'\s*\(', code):
            args = _split_args(code, m.end() - 1)
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


TEST_CLASS_TEXT = re.compile(r'\bTEST_CLASS\s*\(')


def test_sources(root):
    """Every .cpp under src that declares a TEST_CLASS: the *Tests.cpp files and the ones that do not follow the
    name (LongPairBenchDump.cpp, GrainSplitV3CostBench.cpp, GrainLevelShiftBench.cpp, ProducerThroughputBench.cpp:
    TQ-RG-C2 / MISS-1). A file whose only TEST_CLASS is in a comment is read and yields no test."""
    for base, _dirs, files in os.walk(os.path.join(root, 'src')):
        for name in sorted(files):
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(base, name)
            if name.endswith('Tests.cpp'):
                yield path
                continue
            with open(path, encoding='utf-8', errors='replace') as f:
                if TEST_CLASS_TEXT.search(f.read()):
                    yield path
