# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TRX filter-coverage checker (AREA-RULES item 11(b), owner decision 2026-11-21).

Usage:
  python -I tools/check_trx_coverage.py --filter "FullyQualifiedName~A|FullyQualifiedName~B" TRX [TRX|DIR ...]
  python -I tools/check_trx_coverage.py --queue-line "<build-queue line>" TRX|DIR ...
  python -I tools/check_trx_coverage.py --queue-file D:/codex/build-queue/queue.txt --lane LANE TRX|DIR ...
  python -I tools/check_trx_coverage.py --selftest
  (add --json for a machine-readable report)

Rule: every filter term of a build-queue line must match at least one EXECUTED test. A zero-match term
is a FAIL; skipped or not-executed tests never count as executed.

Filter: the vstest /TestCaseFilter text. From a queue line it is field 5 (priority|lane|worktree|kind|
filter|gpu|state...; '|' or TAB separated) with ';' turned into '|' exactly as runner.sh does. Grammar:
conditions "Property op Value" (op = '=', '!=', '~', '!~'; a bare value means FullyQualifiedName~value),
joined by '|' (or) and '&' (and, binds tighter), with parentheses; '\\' escapes the next character.
Matching mirrors what the native C++ adapter is SHOWN to match (case-insensitive, like vstest):
  FullyQualifiedName  only the "Ns::Class::Method" form (the TRX TestMethod name attribute). Evidence
                      (build-queue runner.log 2026-11-15): C4-gpu04b, filter
                      FullyQualifiedName~WholeHookChainHookTests.WholeHookChainCombinationMatrix, ended
                      rc=96 (0 tests); C4-gpu04c, the same test as ...WholeHookChainHookTests::Whole...,
                      ended rc=0. A dot-form term is therefore a zero-match, and is named as such.
  Name                the method name (testName).
  ClassName           NO evidence in the queue history of what vstest matches for the native adapter,
                      so a ClassName term never counts as covered (zero-match, rc 1); a negative
                      ClassName term is a usage error. Use FullyQualifiedName~Ns::Class instead.

Per test (TestDefinitions/UnitTest + its UnitTestResult rows):
  executed     outcome Passed or Failed, with no SKIP marker in its output
  skipped      outcome Inconclusive, or any outcome whose output carries a SKIP-* marker
               (SKIP-NO-GPU, SKIP-NO-FOOTAGE, SKIP-OPT-IN, ... : the house "visible skip" convention,
               tests/support/HardwarePrerequisites.h, where a skip reports Passed with the marker)
  notExecuted  any other outcome (Timeout, Error, Aborted, NotExecuted, NotRunnable, Pending, ...; a
               timed-out or errored test proved nothing), or a test definition with no result row

Run level: a ResultSummary outcome other than Completed/Passed/Failed, Counters aborted, error,
timeout, notRunnable or notExecuted above 0, or Counters total != executed is an aborted/partial run.

Input: --trx FILE (repeatable) reads exactly those files; a positional DIR reads every .trx in it
(with a note: the runner reads only the newest), or only the newest with --newest, as runner.sh does
(ls -t | head -1). The output lists every file read.

For each positive filter term: executed / skipped / notExecuted counts over the tests that the WHOLE
filter selects and that the term matches (so an '&' term is credited only where the conjunction holds).
Negative terms ('!=', '!~') only exclude; they are listed, never required to match.

Exit 1 when: any positive term has 0 executed; any selected test is skipped or not executed; a
SKIP-NO-GPU marker appears anywhere in a TRX; the run was aborted or partial; a TRX path is missing,
unparsable or has no Counters line; no TRX was given; or nothing at all was executed. Exit 2 on a
usage error (no filter, bad filter syntax, lane not found). Exit 0 only when every term is covered.
Coverage only: failed tests count as executed and are listed separately ("COVERAGE PASS (n failed:
see the runner rc)"); a coverage PASS is never a green run by itself.
"""
import argparse
import json
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

NS = "{http://microsoft.com/schemas/VisualStudio/TeamTest/2010}"
EXECUTED_OUTCOMES = ("passed", "failed")
RUN_OK_OUTCOMES = ("completed", "passed", "failed")
PARTIAL_COUNTERS = ("aborted", "error", "timeout", "notRunnable", "notExecuted")
SKIP_OUTCOMES = ("inconclusive",)
SKIP_MARKER = re.compile(r"SKIP-[A-Z][A-Z0-9-]*")
COUNTER_KEYS = ("total", "executed", "passed", "failed", "error", "timeout", "aborted", "inconclusive",
                "notRunnable", "notExecuted")
PROPS = {"fullyqualifiedname": "FullyQualifiedName", "name": "Name", "classname": "ClassName"}


class FilterError(Exception):
    pass


# ---------------------------------------------------------------- filter parsing

def _tokens(text):
    out, buf, i = [], [], 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            buf.append(text[i + 1])
            i += 2
            continue
        if c in "|&()":
            if buf:
                out.append(("T", "".join(buf)))
                buf = []
            out.append((c, c))
        else:
            buf.append(c)
        i += 1
    if buf:
        out.append(("T", "".join(buf)))
    return [t for t in out if not (t[0] == "T" and not t[1].strip())]


def _leaf(raw):
    text = raw.strip()
    m = re.match(r"^([A-Za-z]+)\s*(!=|!~|=|~)\s*(.*)$", text)
    if m and m.group(1).lower() in PROPS:
        prop, op, val = PROPS[m.group(1).lower()], m.group(2), m.group(3).strip()
    elif m:
        raise FilterError("unsupported filter property '%s' in '%s'" % (m.group(1), text))
    else:
        prop, op, val = "FullyQualifiedName", "~", text
    if not val:
        raise FilterError("empty value in filter condition '%s'" % text)
    if prop == "ClassName" and op.startswith("!"):
        raise FilterError("negative ClassName term '%s': vstest's ClassName match for the native adapter is "
                          "unproven; use FullyQualifiedName!~Ns::Class" % text)
    return {"leaf": True, "prop": prop, "op": op, "value": val, "text": "%s%s%s" % (prop, op, val)}


def parse_filter(text):
    """Return (ast, leaves). ast nodes: leaf dicts, ("or", [..]) or ("and", [..])."""
    toks = _tokens(text)
    if not toks:
        raise FilterError("empty filter")
    pos = [0]
    leaves = []

    def peek():
        return toks[pos[0]][0] if pos[0] < len(toks) else None

    def take():
        pos[0] += 1
        return toks[pos[0] - 1]

    def p_or():
        items = [p_and()]
        while peek() == "|":
            take()
            items.append(p_and())
        return items[0] if len(items) == 1 else ("or", items)

    def p_and():
        items = [p_atom()]
        while peek() == "&":
            take()
            items.append(p_atom())
        return items[0] if len(items) == 1 else ("and", items)

    def p_atom():
        k = peek()
        if k == "(":
            take()
            node = p_or()
            if peek() != ")":
                raise FilterError("missing ')' in filter")
            take()
            return node
        if k == "T":
            lf = _leaf(take()[1])
            leaves.append(lf)
            return lf
        raise FilterError("unexpected '%s' in filter" % (k or "end"))

    ast = p_or()
    if pos[0] != len(toks):
        raise FilterError("unexpected '%s' in filter" % toks[pos[0]][1])
    return ast, leaves


def _hit(op, value, cand):
    v, c = value.lower(), cand.lower()
    return v == c if op in ("=", "!=") else v in c


def leaf_match(lf, test):
    # Only the forms vstest is shown to match (module docstring: runner.log C4-gpu04b rc=96 vs C4-gpu04c rc=0).
    if lf["prop"] == "ClassName":
        return False   # unproven for the native adapter: never credited (negatives are rejected at parse)
    cand = test["name"] if lf["prop"] == "Name" else test["fqn"]
    hit = _hit(lf["op"], lf["value"], cand)
    return (not hit) if lf["op"].startswith("!") else hit


def dot_form_only(lf, test):
    """True when a positive FullyQualifiedName term would match only the '.'-separated form."""
    return (lf["prop"] == "FullyQualifiedName" and not lf["op"].startswith("!") and "." in lf["value"]
            and not _hit(lf["op"], lf["value"], test["fqn"]) and _hit(lf["op"], lf["value"], test["fqn_dot"]))


def evaluate(node, test):
    if isinstance(node, dict):
        return leaf_match(node, test)
    kind, items = node
    if kind == "or":
        return any(evaluate(n, test) for n in items)
    return all(evaluate(n, test) for n in items)


def filter_from_queue_line(line):
    norm = line.replace("\t", "|").strip()
    fields = norm.split("|")
    if len(fields) < 5:
        raise FilterError("queue line has %d field(s); the filter is field 5" % len(fields))
    flt = fields[4].strip()
    if flt in ("", "-"):
        raise FilterError("queue line has no filter (field 5 is '%s')" % flt)
    return flt.replace(";", "|"), fields[1].strip()


def queue_line_for_lane(path, lane):
    found = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            f = s.replace("\t", "|").split("|")
            if len(f) > 1 and f[1].strip() == lane:
                found = s
    if found is None:
        raise FilterError("lane '%s' not found in %s" % (lane, path))
    return found


# ---------------------------------------------------------------- TRX parsing

def expand_trx(paths, newest=False):
    """Return (files, errors, notes). newest: a directory gives only its newest .trx (runner: ls -t | head -1)."""
    files, errors, notes = [], [], []
    for p in paths:
        if os.path.isdir(p):
            got = [os.path.join(p, n) for n in os.listdir(p) if n.lower().endswith(".trx")]
            got.sort(key=lambda f: (os.path.getmtime(f), f), reverse=True)
            if not got:
                errors.append("no .trx file in directory %s" % p)
            elif newest:
                if len(got) > 1:
                    notes.append("directory %s holds %d .trx; --newest read only %s (as the runner does)"
                                 % (p, len(got), os.path.basename(got[0])))
                got = got[:1]
            elif len(got) > 1:
                notes.append("directory %s holds %d .trx, all read; the runner reads only the newest (%s): "
                             "use --newest or --trx to match it" % (p, len(got), os.path.basename(got[0])))
            files.extend(got)
        elif os.path.isfile(p):
            files.append(p)
        else:
            errors.append("TRX missing: %s" % p)
    return files, errors, notes


def _output_text(elem):
    parts = []
    for tag in ("StdOut", "StdErr", "DebugTrace", "Message", "StackTrace"):
        for e in elem.iter(NS + tag):
            if e.text:
                parts.append(e.text)
    return "\n".join(parts)


def parse_trx(path):
    """Return (tests, counters, skip_no_gpu_count) or raise ValueError."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
        root = ET.fromstring(raw)
    except (OSError, ET.ParseError) as e:
        raise ValueError("TRX unparsable: %s (%s)" % (path, e))
    if root.tag != NS + "TestRun":
        raise ValueError("TRX unparsable: %s (root is %s, not TestRun)" % (path, root.tag))
    cnt = root.find("./%sResultSummary/%sCounters" % (NS, NS))
    if cnt is None:
        raise ValueError("TRX has no Counters line: %s" % path)
    counters = {k: int(cnt.get(k, "0") or 0) for k in COUNTER_KEYS}
    counters["_outcome"] = root.find(NS + "ResultSummary").get("outcome", "")
    text = raw.decode("utf-8", errors="replace")
    nogpu = len(re.findall(r"SKIP-NO-GPU", text))
    tests = {}
    for ut in root.iter(NS + "UnitTest"):
        tm = ut.find(NS + "TestMethod")
        tid = ut.get("id", "")
        name = ut.get("name", "")
        cls = tm.get("className", "") if tm is not None else ""
        fqn = tm.get("name", "") if tm is not None else ""
        if "::" not in fqn:
            fqn = (cls.replace(".", "::") + "::" + name) if cls else name
        tests[tid] = {"id": tid, "name": name, "class": cls or "(no class)", "fqn": fqn,
                      "fqn_dot": fqn.replace("::", "."), "results": [], "trx": path}
    results = root.find(NS + "Results")
    if results is not None:
        for r in results:
            if r.tag != NS + "UnitTestResult":
                continue
            tid = r.get("testId", "")
            t = tests.get(tid)
            if t is None:
                nm = r.get("testName", "")
                t = tests[tid] = {"id": tid, "name": nm, "class": "(no definition)", "fqn": nm,
                                  "fqn_dot": nm, "results": [], "trx": path}
            out = _output_text(r)
            t["results"].append({"outcome": r.get("outcome", ""), "markers": sorted(set(SKIP_MARKER.findall(out)))})
    return list(tests.values()), counters, nogpu


def classify(test):
    """executed / skipped / notExecuted for one test definition (all its result rows)."""
    if not test["results"]:
        return "notExecuted", ["no result row"]
    markers = sorted({m for r in test["results"] for m in r["markers"]})
    outs = [r["outcome"].lower() for r in test["results"]]
    if markers or any(o in SKIP_OUTCOMES for o in outs):
        return "skipped", markers or ["Inconclusive"]
    if all(o in EXECUTED_OUTCOMES for o in outs):
        return "executed", []
    return "notExecuted", sorted({"outcome " + r["outcome"] for r in test["results"]
                                  if r["outcome"].lower() not in EXECUTED_OUTCOMES})


# ---------------------------------------------------------------- the check

def check(filter_text, trx_paths, newest=False):
    rep = {"filter": filter_text, "trx": [], "read": [], "terms": [], "classes": [], "failed_tests": [],
           "problems": [], "notes": []}
    ast, leaves = parse_filter(filter_text)
    files, errs, notes = expand_trx(trx_paths, newest)
    rep["problems"].extend(errs)
    rep["notes"].extend(notes)
    rep["read"] = list(files)
    if not trx_paths:
        rep["problems"].append("no TRX given")
    alltests = []
    for f in files:
        try:
            tests, counters, nogpu = parse_trx(f)
        except ValueError as e:
            rep["problems"].append(str(e))
            continue
        rep["trx"].append({"path": f, "counters": counters, "skip_no_gpu": nogpu, "tests": len(tests)})
        if nogpu:
            rep["problems"].append("SKIP-NO-GPU marker (%d occurrence(s)) in %s" % (nogpu, f))
        oc = counters["_outcome"]
        if oc.lower() not in RUN_OK_OUTCOMES:
            rep["problems"].append("aborted/partial run: ResultSummary outcome '%s' in %s" % (oc, f))
        bad = ["%s=%d" % (k, counters[k]) for k in PARTIAL_COUNTERS if counters[k] > 0]
        if bad:
            rep["problems"].append("aborted/partial run: Counters %s in %s" % (" ".join(bad), f))
        if counters["total"] != counters["executed"]:
            rep["problems"].append("aborted/partial run: Counters total=%d != executed=%d in %s"
                                   % (counters["total"], counters["executed"], f))
        alltests.extend(tests)
    selected, outside = [], 0
    for t in alltests:
        t["state"], t["markers"] = classify(t)
        if evaluate(ast, t):
            selected.append(t)
        else:
            outside += 1
    if outside:
        rep["notes"].append("%d TRX test(s) are outside the filter (not counted)" % outside)
    for lf in leaves:
        neg = lf["op"].startswith("!")
        row = {"term": lf["text"], "negative": neg, "executed": 0, "passed": 0, "failed": 0,
               "skipped": 0, "notExecuted": 0}
        if not neg:
            for t in selected:
                if not leaf_match(lf, t):
                    continue
                row[t["state"]] += 1
                if t["state"] == "executed":
                    if all(r["outcome"].lower() == "passed" for r in t["results"]):
                        row["passed"] += 1
                    else:
                        row["failed"] += 1
            if row["executed"] == 0:
                why = ""
                if lf["prop"] == "ClassName":
                    why = " (ClassName match is unproven for the native adapter; use FullyQualifiedName~Ns::Class)"
                elif any(dot_form_only(lf, t) for t in alltests):
                    why = " (term uses '.' separators; vstest matches the '::' form)"
                rep["problems"].append("term %s: 0 executed%s" % (lf["text"], why))
        rep["terms"].append(row)
    classes = {}
    for t in selected:
        c = classes.setdefault(t["class"], {"class": t["class"], "executed": 0, "total": 0, "skipped": 0,
                                            "notExecuted": 0})
        c["total"] += 1
        c[t["state"]] += 1
        if t["state"] == "skipped":
            rep["problems"].append("skipped: %s::%s %s" % (t["class"], t["name"], ",".join(t["markers"])))
        elif t["state"] == "notExecuted":
            rep["problems"].append("not executed: %s::%s (%s)" % (t["class"], t["name"], ",".join(t["markers"])))
        elif any(r["outcome"].lower() != "passed" for r in t["results"]):
            rep["failed_tests"].append("%s::%s" % (t["class"], t["name"]))
    rep["classes"] = [classes[k] for k in sorted(classes)]
    if files and not any(t["state"] == "executed" for t in selected) and not any(p.startswith("term ") for p in rep["problems"]):
        rep["problems"].append("nothing executed")
    rep["executed_total"] = sum(1 for t in selected if t["state"] == "executed")
    rep["selected_total"] = len(selected)
    rep["verdict"] = "COVERAGE FAIL" if rep["problems"] else "COVERAGE PASS"
    return rep


def render(rep):
    lines = ["FILTER: %s" % rep["filter"]]
    lines.append("READ %d TRX file(s):" % len(rep["read"]))
    for f in rep["read"]:
        lines.append("  %s" % f)
    for t in rep["trx"]:
        c = t["counters"]
        lines.append("TRX: %s" % t["path"])
        lines.append("  Counters: " + " ".join("%s=%d" % (k, c[k]) for k in COUNTER_KEYS) +
                     " SKIP-NO-GPU occurrences=%d" % t["skip_no_gpu"])
        lines.append("  ResultSummary outcome=%s" % (c["_outcome"] or "(none)"))
    lines.append("TERMS (executed = really run; skipped/notExecuted never count):")
    for r in rep["terms"]:
        if r["negative"]:
            lines.append("  [excl] %s (exclusion, not required to match)" % r["term"])
            continue
        ok = "OK  " if r["executed"] > 0 else "FAIL"
        lines.append("  [%s] %s executed=%d (passed=%d failed=%d) skipped=%d notExecuted=%d" %
                     (ok, r["term"], r["executed"], r["passed"], r["failed"], r["skipped"], r["notExecuted"]))
    lines.append("CLASSES (executed/total):")
    for c in rep["classes"]:
        lines.append("  %s %d/%d" % (c["class"], c["executed"], c["total"]) +
                     ("" if c["executed"] == c["total"] else " (skipped=%d notExecuted=%d)" % (c["skipped"], c["notExecuted"])))
    for n in rep["notes"]:
        lines.append("NOTE: %s" % n)
    for p in rep["problems"]:
        lines.append("PROBLEM: %s" % p)
    nf = len(rep["failed_tests"])
    lines.append("TEST FAILURES (not a coverage question; the runner rc judges them): %d" % nf)
    for f in rep["failed_tests"]:
        lines.append("  FAILED %s" % f)
    tag = rep["verdict"] + (" (%d failed: see the runner rc)" % nf if nf else "")
    lines.append("VERDICT: %s executed=%d selected=%d terms=%d problems=%d" %
                 (tag, rep["executed_total"], rep["selected_total"],
                  sum(1 for r in rep["terms"] if not r["negative"]), len(rep["problems"])))
    return "\n".join(lines)


# ---------------------------------------------------------------- selftest

def _fixture(tests, counters=True, outcome="Completed", extra="", total=None):
    """tests: list of (class, method, outcome or None for definition only, stdout).
    outcome: the ResultSummary outcome; extra: extra Counters attributes (e.g. 'aborted="1"');
    total: the Counters total (default: the number of result rows, = executed)."""
    defs, res = [], []
    for i, (cls, meth, toc, out) in enumerate(tests):
        tid = "00000000-0000-0000-0000-%012d" % i
        ns = cls.split(".")[0]
        short = cls.split(".")[-1]
        defs.append('<UnitTest name="%s" id="%s"><TestMethod className="%s" name="%s::%s::%s" /></UnitTest>'
                    % (meth, tid, cls, ns, short, meth))
        if toc is not None:
            body = "<Output><StdOut>%s</StdOut></Output>" % out if out else ""
            res.append('<UnitTestResult testId="%s" testName="%s" outcome="%s">%s</UnitTestResult>'
                       % (tid, meth, toc, body))
    summary = ('<ResultSummary outcome="%s"><Counters total="%d" executed="%d" passed="%d" failed="0" %s/>'
               '</ResultSummary>' % (outcome, len(res) if total is None else total, len(res), len(res), extra)) \
        if counters else ""
    return ('<?xml version="1.0" encoding="utf-8"?>\n<TestRun xmlns="http://microsoft.com/schemas/VisualStudio/'
            'TeamTest/2010"><Results>%s</Results><TestDefinitions>%s</TestDefinitions>%s</TestRun>'
            % ("".join(res), "".join(defs), summary))


def selftest_cases():
    """(name, filter, [fixture text, ...], expected rc, expected substring in the problems or '')."""
    A = ("VideoProcessorTest.CameraAheadFitTests", "FitsAhead", "Passed", "")
    B = ("VideoProcessorTest.PairTimingTests", "PairsOnTime", "Passed", "")
    C = ("VectorEngineTest.VectorEngineTests", "Publishes", "Passed", "")
    return [
        ("all pass", "FullyQualifiedName~CameraAheadFitTests|FullyQualifiedName~PairTimingTests",
         [_fixture([A, B])], 0, ""),
        ("semicolon queue form", "QUEUE:1|lane|D:\\codex\\videoprocessor\\r2-x|build+trx|"
         "FullyQualifiedName~CameraAheadFitTests;FullyQualifiedName~PairTimingTests|no|done", [_fixture([A, B])], 0, ""),
        ("zero-match term", "FullyQualifiedName~CameraModelFitTests|FullyQualifiedName~CameraAheadFitTests",
         [_fixture([A])], 1, "term FullyQualifiedName~CameraModelFitTests: 0 executed"),
        ("inconclusive skip", "FullyQualifiedName~CameraAheadFitTests|FullyQualifiedName~PairTimingTests",
         [_fixture([A, ("VideoProcessorTest.PairTimingTests", "PairsOnTime", "Inconclusive", "")])], 1,
         "term FullyQualifiedName~PairTimingTests: 0 executed"),
        ("visible skip marker on a pass", "FullyQualifiedName~CameraAheadFitTests",
         [_fixture([A, ("VideoProcessorTest.CameraAheadFitTests", "NeedsFootage", "Passed", "SKIP-NO-FOOTAGE x")])], 1,
         "skipped: VideoProcessorTest.CameraAheadFitTests::NeedsFootage SKIP-NO-FOOTAGE"),
        ("SKIP-NO-GPU", "FullyQualifiedName~VectorEngineTests",
         [_fixture([C, ("VectorEngineTest.VectorEngineTests", "OnGpu", "Passed", "SKIP-NO-GPU OnGpu")])], 1,
         "SKIP-NO-GPU marker"),
        ("NotExecuted outcome", "FullyQualifiedName~VectorEngineTests",
         [_fixture([C, ("VectorEngineTest.VectorEngineTests", "Late", "NotExecuted", "")])], 1,
         "not executed: VectorEngineTest.VectorEngineTests::Late"),
        ("definition without a result", "FullyQualifiedName~VectorEngineTests",
         [_fixture([C, ("VectorEngineTest.VectorEngineTests", "Never", None, "")])], 1,
         "not executed: VectorEngineTest.VectorEngineTests::Never"),
        ("multiple TRX files cover the terms", "FullyQualifiedName~CameraAheadFitTests|FullyQualifiedName~VectorEngineTests",
         [_fixture([A]), _fixture([C])], 0, ""),
        ("multiple TRX, one term uncovered", "FullyQualifiedName~CameraAheadFitTests|FullyQualifiedName~VectorEngineTests",
         [_fixture([A]), _fixture([B])], 1, "term FullyQualifiedName~VectorEngineTests: 0 executed"),
        ("and form covered", "FullyQualifiedName~VideoProcessorTest::PairTimingTests&Name=PairsOnTime",
         [_fixture([A, B])], 0, ""),
        ("and form never true", "FullyQualifiedName~VideoProcessorTest::PairTimingTests&Name=FitsAhead",
         [_fixture([A, B])], 1, "term FullyQualifiedName~VideoProcessorTest::PairTimingTests: 0 executed"),
        ("parentheses and exclusion", "(FullyQualifiedName~VideoProcessorTest|Name=Publishes)&FullyQualifiedName!~PairTiming",
         [_fixture([A, B, C])], 0, ""),
        # M1/M2: only the forms vstest matches are credited (runner.log C4-gpu04b rc=96 vs C4-gpu04c rc=0).
        ("dot-form FQN term credited only through another term",
         "FullyQualifiedName~CameraAheadFitTests|FullyQualifiedName~VideoProcessorTest.CameraAheadFitTests.FitsAhead",
         [_fixture([A])], 1, "FullyQualifiedName~VideoProcessorTest.CameraAheadFitTests.FitsAhead: 0 executed "
         "(term uses '.' separators"),
        ("ClassName '::' form credited only through another term",
         "FullyQualifiedName~CameraAheadFitTests|ClassName=VideoProcessorTest::CameraAheadFitTests",
         [_fixture([A])], 1, "term ClassName=VideoProcessorTest::CameraAheadFitTests: 0 executed"),
        ("ClassName dot form alone", "ClassName=VideoProcessorTest.CameraAheadFitTests", [_fixture([A])], 1,
         "ClassName match is unproven"),
        ("'::' FQN form is credited", "FullyQualifiedName~VideoProcessorTest::CameraAheadFitTests::FitsAhead",
         [_fixture([A])], 0, ""),
        # S1: a timed-out, errored or aborted test is not coverage.
        ("timeout is not executed", "FullyQualifiedName~PairTimingTests",
         [_fixture([("VideoProcessorTest.PairTimingTests", "PairsOnTime", "Timeout", "")])], 1,
         "not executed: VideoProcessorTest.PairTimingTests::PairsOnTime (outcome Timeout)"),
        ("error is not executed", "FullyQualifiedName~PairTimingTests",
         [_fixture([("VideoProcessorTest.PairTimingTests", "PairsOnTime", "Error", "")])], 1,
         "term FullyQualifiedName~PairTimingTests: 0 executed"),
        ("aborted test is not executed", "FullyQualifiedName~PairTimingTests",
         [_fixture([B, ("VideoProcessorTest.PairTimingTests", "Cut", "Aborted", "")])], 1, "(outcome Aborted)"),
        # S2: an aborted or partial run is flagged from the summary itself.
        ("aborted ResultSummary", "FullyQualifiedName~PairTimingTests", [_fixture([B], outcome="Aborted")], 1,
         "ResultSummary outcome 'Aborted'"),
        ("aborted counter", "FullyQualifiedName~PairTimingTests", [_fixture([B], extra='aborted="1" ')], 1,
         "Counters aborted=1"),
        ("notExecuted counter", "FullyQualifiedName~PairTimingTests", [_fixture([B], extra='notExecuted="3" ')], 1,
         "Counters notExecuted=3"),
        # Only total != executed marks this run partial (no other counter, outcome Completed, all rows Passed).
        ("total above executed", "FullyQualifiedName~PairTimingTests", [_fixture([B], total=4)], 1,
         "Counters total=4 != executed=1"),
        # S4: the exclusion must really exclude, and matching is case-insensitive.
        ("exclusion removes a not-executed test",
         "FullyQualifiedName~VectorEngineTests&FullyQualifiedName!~Late",
         [_fixture([C, ("VectorEngineTest.VectorEngineTests", "Late", "NotExecuted", "")], extra='notExecuted="0" ')],
         0, ""),
        ("exclusion removes a skipped test", "FullyQualifiedName~VideoProcessorTest&Name!~NeedsFootage",
         [_fixture([A, ("VideoProcessorTest.CameraAheadFitTests", "NeedsFootage", "Inconclusive", "")])], 0, ""),
        ("case-insensitive property and value", "fullyqualifiedname~cameraaheadfittests|NAME=pairsontime",
         [_fixture([A, B])], 0, ""),
        ("case-insensitive term still needs a match", "fullyqualifiedname~cameramodelfittests", [_fixture([A])], 1,
         "0 executed"),
        ("bare value term", "CameraAheadFitTests", [_fixture([A])], 0, ""),
        ("failed test still counts as executed", "FullyQualifiedName~PairTimingTests",
         [_fixture([("VideoProcessorTest.PairTimingTests", "PairsOnTime", "Failed", "")])], 0, ""),
        ("missing TRX", "FullyQualifiedName~PairTimingTests", ["MISSING"], 1, "TRX missing"),
        ("unparsable TRX", "FullyQualifiedName~PairTimingTests", ["<TestRun><Results>"], 1, "TRX unparsable"),
        ("no Counters line", "FullyQualifiedName~PairTimingTests", [_fixture([B], counters=False)], 1, "no Counters line"),
    ]


def run_case(flt, fixtures):
    """Run one case in a temp folder; return (rc, report)."""
    with tempfile.TemporaryDirectory() as d:
        paths = []
        for i, fx in enumerate(fixtures):
            p = os.path.join(d, "run%d.trx" % i)
            if fx == "MISSING":
                paths.append(p)
                continue
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(fx)
            paths.append(p)
        if flt.startswith("QUEUE:"):
            flt = filter_from_queue_line(flt[len("QUEUE:"):])[0]
        rep = check(flt, paths)
        return (1 if rep["problems"] else 0), rep


def newest_dir_cases():
    """S3: a directory with an older TRX that covers a term the newest one does not."""
    out = []
    with tempfile.TemporaryDirectory() as d:
        old, new = os.path.join(d, "old.trx"), os.path.join(d, "new.trx")
        with open(old, "w", encoding="utf-8") as fh:
            fh.write(_fixture([("N.KTests", "M", "Passed", ""), ("N.LTests", "M", "Passed", "")]))
        with open(new, "w", encoding="utf-8") as fh:
            fh.write(_fixture([("N.KTests", "M", "Passed", "")]))
        os.utime(old, (1000000000, 1000000000))
        os.utime(new, (1100000000, 1100000000))
        flt = "FullyQualifiedName~KTests|FullyQualifiedName~LTests"
        rep = check(flt, [d], newest=True)
        out.append(("--newest reads only the newest TRX (term LTests uncovered)",
                    rep["read"] == [new] and any("LTests: 0 executed" in p for p in rep["problems"])))
        rep = check(flt, [d])
        out.append(("directory mode reads all and notes the runner reads only the newest",
                    sorted(rep["read"]) == sorted([old, new]) and not rep["problems"]
                    and any("the runner reads only the newest (new.trx)" in n for n in rep["notes"])))
    return out


def selftest():
    bad = 0
    for name, flt, fx, want_rc, want_sub in selftest_cases():
        rc, rep = run_case(flt, fx)
        ok = rc == want_rc and (not want_sub or any(want_sub in p for p in rep["problems"]))
        if not ok:
            bad += 1
            print("SELFTEST FAIL %s: rc=%d want %d; problems=%s" % (name, rc, want_rc, rep["problems"]))
    bad_filters = ("A|", "(A", "A)", "Owner=x", "Name=", "ClassName!=Ns.K")
    for txt in bad_filters:
        try:
            parse_filter(txt)
            bad += 1
            print("SELFTEST FAIL bad filter accepted: %r" % txt)
        except FilterError:
            pass
    dir_cases = newest_dir_cases()
    for name, ok in dir_cases:
        if not ok:
            bad += 1
            print("SELFTEST FAIL %s" % name)
    n = len(selftest_cases()) + len(bad_filters) + len(dir_cases)
    print("selftest: %d/%d OK" % (n - bad, n))
    return 0 if bad == 0 else 1


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description="TRX filter-coverage checker (AREA-RULES 11(b))")
    ap.add_argument("trx", nargs="*", help="TRX files or directories holding .trx files")
    ap.add_argument("--trx", dest="trx_files", action="append", default=[],
                    help="one TRX file (repeatable); read exactly as given")
    ap.add_argument("--newest", action="store_true",
                    help="a directory gives only its newest .trx, as runner.sh does (ls -t | head -1)")
    ap.add_argument("--filter", help="vstest /TestCaseFilter text")
    ap.add_argument("--queue-line", help="a build-queue line; its field 5 is the filter (';' = '|')")
    ap.add_argument("--queue-file", help="queue.txt; use with --lane (the last line for that lane)")
    ap.add_argument("--lane", help="lane name to look up in --queue-file")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    try:
        if a.filter:
            flt = a.filter
        elif a.queue_line:
            flt = filter_from_queue_line(a.queue_line)[0]
        elif a.queue_file and a.lane:
            flt = filter_from_queue_line(queue_line_for_lane(a.queue_file, a.lane))[0]
        else:
            print("usage error: give --filter, --queue-line, or --queue-file with --lane", file=sys.stderr)
            return 2
        for f in a.trx_files:
            if os.path.isdir(f):
                raise FilterError("--trx takes a file, not a directory: %s" % f)
        rep = check(flt, list(a.trx_files) + list(a.trx), newest=a.newest)
    except FilterError as e:
        print("usage error: %s" % e, file=sys.stderr)
        return 2
    print(json.dumps(rep, indent=1, sort_keys=True) if a.json else render(rep))
    return 1 if rep["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
