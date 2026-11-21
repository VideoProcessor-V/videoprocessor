# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TRX filter-coverage checker (AREA-RULES item 11(b), owner decision 2026-11-21).

Usage:
  python -I tools/check_trx_coverage.py --filter "FullyQualifiedName~A|FullyQualifiedName~B" TRX [TRX|DIR ...]
  python -I tools/check_trx_coverage.py --queue-line "<build-queue line>" TRX|DIR ...
  python -I tools/check_trx_coverage.py --queue-file D:/codex/build-queue/queue.txt --lane LANE TRX|DIR ...
  python -I tools/check_trx_coverage.py --filter F --trx NATIVE.trx --beta-trx BETA.trx
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

Fork-only tests (GEM-PKG s10g; --beta-trx FILE, repeatable):
  forkOnly     every row Passed and its only SKIP markers are SKIP-FORK-ONLY-<TAG> with TAG in the CLOSED
               FORK_ONLY_TAGS list ({"PL0004"}); any other tag is a plain skip (FAIL)
  A Beta TRX is valid only when BetaSuite::RunBetaSuite, from VideoProcessorBetaTestsHost.dll (its codeBase),
  executed and Passed, with no SKIP marker, its output says "whole chain passed" and has no
  VP_TEST_REPORT_MODE=1 token. The suite's stdout goes to VP_TEST_OUT, so BETA-RAN lines reach the TRX
  only when the Beta host echoes them through Logger::WriteMessage. Only line-start "BETA-RAN <id>" lines in
  that test's output count; BETA-RAN lines anywhere else (a native TRX, another test, an invalid Beta TRX)
  are ignored with a NOTE. A forkOnly native test is "executed on the Beta host" only when a valid Beta TRX
  has "BETA-RAN <its exact FQN>" (case-insensitive full equality); credited tests count as executed and are
  listed with the Beta TRX path. An uncredited forkOnly test in the filter is a FAIL ("fork-only test
  selected on a native lane without Beta-host evidence"). A BETA-RAN id equal to a native test without the
  tag credits nothing. Every other BETA-RAN id under "BetaStage::" (any case) is a Beta pseudo-test (fqn = id,
  executed), so a term like FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests needs at least one; an
  unmatched id outside BetaStage:: credits nothing (NOTE), so Beta lines never cover a native term. A
  pseudo-test satisfies ONLY positive FullyQualifiedName~/= terms whose value starts with BetaStage::; it never
  counts toward any other term (a broad FullyQualifiedName~Sigma, a Name~ term) and is never selected by
  exclusions alone. SKIP-NO-GPU anywhere,
  Beta TRX included, is a FAIL. Each TRX's build lines (VP-BUILD-ID:<hex>, build/commit/sha = <hex>, the
  test DLL codeBase) are printed so a gate can compare the native and Beta builds.

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
# Fork-only (GEM-PKG s10g, agreed with A20-C5 2026-11-21): a native test that needs a libplacebo fork patch
# logs SKIP-FORK-ONLY-<TAG> and returns; it is credited only by a valid Beta TRX's "BETA-RAN <FQN>" line.
# CLOSED allow-list: a new tag is a reviewed change to this line (WAIVER commit), never a pattern.
FORK_ONLY_TAGS = frozenset(["PL0004"])
FORK_ONLY_PREFIX = "SKIP-FORK-ONLY-"
BETA_SUITE_FQN = "betasuite::runbetasuite"
BETA_HOST_DLL = re.compile(r"^videoprocessor-?betatestshost\.dll$", re.I)   # VideoProcessor-BetaTestsHost.vcxproj TargetName
BETA_STAGE_PREFIX = "betastage::"   # Beta-only stage ids (GEM-PKG s10g): the only ids that may become pseudo-tests
TRANSPORT_NOTE = ("the Beta host must echo each BETA-RAN line through Logger::WriteMessage (RunBetaSuite sends the "
                  "suite's stdout to VP_TEST_OUT, not to the TRX)")
BETA_RAN_LINE = re.compile(r"^[ \t]*BETA-RAN[ \t]+(\S+)[ \t]*\r?$", re.M)
BUILD_LINE = re.compile(r"VP-BUILD-ID:[0-9A-Fa-f]{32}|\b(?:build|commit|sha|head)(?:[ \t]+sha)?[ \t]*[:=][ \t]*"
                        r"[0-9a-fA-F]{7,40}\b", re.I)


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


def beta_stage_leaf(lf):
    """A positive FullyQualifiedName~ / = term on the Beta namespace (value starts with BetaStage::, any case)."""
    return (lf["prop"] == "FullyQualifiedName" and lf["op"] in ("~", "=")
            and lf["value"].lower().startswith("betastage::"))


def leaf_match(lf, test):
    # Only the forms vstest is shown to match (module docstring: runner.log C4-gpu04b rc=96 vs C4-gpu04c rc=0).
    if lf["prop"] == "ClassName":
        return False   # unproven for the native adapter: never credited (negatives are rejected at parse)
    if test.get("pseudo") and not lf["op"].startswith("!") and not beta_stage_leaf(lf):
        return False   # gate GR MUST-2: a Beta pseudo-test satisfies only FullyQualifiedName~BetaStage::... terms
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


def build_lines(text):
    """Build identity lines in a TRX (VP-BUILD-ID:<32 hex>, or build/commit/sha/head = <hex>), plus the
    test DLL codeBase folders, so a gate can compare the native and Beta builds."""
    found = []
    for m in BUILD_LINE.finditer(text):
        s = m.group(0).strip()
        if s not in found:
            found.append(s)
    for m in re.finditer(r'codeBase="([^"]*)"', text):
        s = "codeBase=" + m.group(1)
        if s not in found:
            found.append(s)
    return found


def parse_trx(path):
    """Return (tests, counters, skip_no_gpu_count, info) or raise ValueError.
    info: {"build_lines": [...]}; each result row keeps its output text in "out"."""
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
                      "fqn_dot": fqn.replace("::", "."), "results": [], "trx": path,
                      "codebase": tm.get("codeBase", "") if tm is not None else ""}
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
            t["results"].append({"outcome": r.get("outcome", ""), "markers": sorted(set(SKIP_MARKER.findall(out))),
                                 "out": out})
    return list(tests.values()), counters, nogpu, {"build_lines": build_lines(text)}


def fork_only_tags(markers):
    """The SKIP-FORK-ONLY-<TAG> tags among markers."""
    return [m[len(FORK_ONLY_PREFIX):] for m in markers if m.startswith(FORK_ONLY_PREFIX)]


def classify(test):
    """executed / forkOnly / skipped / notExecuted for one test definition (all its result rows).
    forkOnly: every row Passed, and its only SKIP markers are SKIP-FORK-ONLY-<TAG> with TAG in the closed
    FORK_ONLY_TAGS list; any other tag (or any other marker beside it) is a plain skip."""
    if not test["results"]:
        return "notExecuted", ["no result row"]
    markers = sorted({m for r in test["results"] for m in r["markers"]})
    outs = [r["outcome"].lower() for r in test["results"]]
    tags = fork_only_tags(markers)
    if (tags and len(tags) == len(markers) and all(tg in FORK_ONLY_TAGS for tg in tags)
            and all(o == "passed" for o in outs)):
        return "forkOnly", markers
    if markers or any(o in SKIP_OUTCOMES for o in outs):
        return "skipped", markers or ["Inconclusive"]
    if all(o in EXECUTED_OUTCOMES for o in outs):
        return "executed", []
    return "notExecuted", sorted({"outcome " + r["outcome"] for r in test["results"]
                                  if r["outcome"].lower() not in EXECUTED_OUTCOMES})


def beta_evidence(path):
    """Read one --beta-trx. Return {"path", "valid", "why", "ids", "build_lines", "skip_no_gpu", "stray"}
    or raise ValueError (missing / unparsable). Valid = it holds BetaSuite::RunBetaSuite, which executed
    and Passed (every row), from the Beta host DLL (its TestMethod codeBase is VideoProcessorBetaTestsHost.dll,
    so a native class named BetaSuite cannot qualify), with no SKIP marker, "whole chain passed" in its output
    and no VP_TEST_REPORT_MODE=1 token. ids = its line-start "BETA-RAN <id>" lines (only from that test's
    output). TRANSPORT: RunBetaSuite redirects the suite's stdout to VP_TEST_OUT, so a BETA-RAN line reaches
    the TRX only when BetaTestHost.cpp echoes it through Logger::WriteMessage after the suite returns."""
    if not os.path.isfile(path):
        raise ValueError("Beta TRX missing: %s" % path)
    tests, _counters, nogpu, info = parse_trx(path)
    suite = [t for t in tests if t["fqn"].lower() == BETA_SUITE_FQN or t["fqn"].lower().endswith("::" + BETA_SUITE_FQN)]
    ev = {"path": path, "valid": False, "why": "", "ids": [], "build_lines": info["build_lines"],
          "skip_no_gpu": nogpu, "stray": 0}
    for t in tests:
        if t not in suite:
            ev["stray"] += sum(len(BETA_RAN_LINE.findall(r["out"])) for r in t["results"])
    if not suite:
        ev["why"] = "no BetaSuite::RunBetaSuite test (not a Beta TRX)"
        return ev
    s = suite[0]
    out = "\n".join(r["out"] for r in s["results"])
    if not BETA_HOST_DLL.search(os.path.basename(s["codebase"].replace("\\", "/"))):
        ev["why"] = "RunBetaSuite is not from the Beta host DLL (codeBase '%s', want VideoProcessorBetaTestsHost.dll)" \
            % (s["codebase"] or "none")
    elif not s["results"] or any(r["outcome"].lower() != "passed" for r in s["results"]):
        ev["why"] = "RunBetaSuite did not execute and pass (%s)" % (",".join(r["outcome"] for r in s["results"]) or "no result")
    elif any(r["markers"] for r in s["results"]):
        ev["why"] = "RunBetaSuite output carries a SKIP marker (%s)" % ",".join(sorted({m for r in s["results"] for m in r["markers"]}))
    elif re.search(r"VP_TEST_REPORT_MODE\s*=\s*1\b", out):
        ev["why"] = "report-mode run (VP_TEST_REPORT_MODE=1): the gate chain did not run"
    elif "whole chain passed" not in out:
        ev["why"] = "no \"whole chain passed\" in the RunBetaSuite output"
    else:
        ev["valid"] = True
    ids = BETA_RAN_LINE.findall(out)
    if ev["valid"]:
        ev["ids"] = ids
    else:
        ev["stray"] += len(ids)
    return ev


# ---------------------------------------------------------------- the check

def check(filter_text, trx_paths, newest=False, beta_paths=()):
    rep = {"filter": filter_text, "trx": [], "read": [], "terms": [], "classes": [], "failed_tests": [],
           "problems": [], "notes": [], "beta": [], "beta_credited": []}
    ast, leaves = parse_filter(filter_text)
    files, errs, notes = expand_trx(trx_paths, newest)
    rep["problems"].extend(errs)
    rep["notes"].extend(notes)
    rep["read"] = list(files)
    if not trx_paths and not beta_paths:
        rep["problems"].append("no TRX given")
    alltests = []
    for f in files:
        try:
            tests, counters, nogpu, info = parse_trx(f)
        except ValueError as e:
            rep["problems"].append(str(e))
            continue
        rep["trx"].append({"path": f, "counters": counters, "skip_no_gpu": nogpu, "tests": len(tests),
                           "build_lines": info["build_lines"]})
        stray = sum(len(BETA_RAN_LINE.findall(r["out"])) for t in tests for r in t["results"])
        if stray:
            rep["notes"].append("%d BETA-RAN line(s) in the native TRX %s ignored (only a valid --beta-trx is "
                                "Beta-host evidence)" % (stray, f))
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
    # Beta-host evidence: id (lower case) -> Beta TRX path, from valid --beta-trx files only.
    beta_ids = {}
    for bp in beta_paths:
        try:
            ev = beta_evidence(bp)
        except ValueError as e:
            rep["problems"].append(str(e))
            continue
        rep["read"].append(bp)
        rep["beta"].append({"path": bp, "valid": ev["valid"], "why": ev["why"], "ids": ev["ids"],
                            "build_lines": ev["build_lines"], "skip_no_gpu": ev["skip_no_gpu"]})
        if ev["skip_no_gpu"]:
            rep["problems"].append("SKIP-NO-GPU marker (%d occurrence(s)) in the Beta TRX %s" % (ev["skip_no_gpu"], bp))
        if not ev["valid"]:
            rep["notes"].append("INVALID Beta TRX %s: %s; its BETA-RAN lines are ignored" % (bp, ev["why"]))
        if ev["stray"]:
            rep["notes"].append("%d BETA-RAN line(s) in %s outside a valid RunBetaSuite output ignored" % (ev["stray"], bp))
        if ev["valid"] and not ev["ids"]:
            rep["notes"].append("valid Beta TRX %s has no BETA-RAN line in the RunBetaSuite output: %s"
                                % (bp, TRANSPORT_NOTE))
        for i in ev["ids"]:
            beta_ids.setdefault(i.lower(), (i, bp))
    native_fqns = {t["fqn"].lower(): t for t in alltests}
    for t in alltests:
        t["state"], t["markers"] = classify(t)
        if t["state"] == "forkOnly" and t["fqn"].lower() in beta_ids:
            t["state"], t["beta_credit"] = "executed", beta_ids[t["fqn"].lower()][1]
    for key, (fqn, bp) in sorted(beta_ids.items()):
        nt = native_fqns.get(key)
        if nt is not None:
            # Full case-insensitive equality with a native test: only an allow-listed fork-only test is credited.
            if nt.get("beta_credit") is None:
                rep["notes"].append("BETA-RAN %s names a native test without an allow-listed SKIP-FORK-ONLY tag: "
                                    "credits nothing" % nt["fqn"])
            continue
        if not key.startswith(BETA_STAGE_PREFIX):
            # Gate GR MUST-1: an unmatched non-BetaStage id would cover a native term with no native run and no tag.
            rep["notes"].append("BETA-RAN %s names no native test of this run and is not a BetaStage id: credits nothing"
                                % fqn)
            continue
        cls, _, name = fqn.rpartition("::")
        alltests.append({"id": "beta:" + fqn, "name": name or fqn, "class": (cls or fqn) + " (Beta pseudo-test)",
                         "fqn": fqn, "fqn_dot": fqn.replace("::", "."), "trx": bp, "beta_credit": bp,
                         "results": [{"outcome": "Passed", "markers": [], "out": ""}], "pseudo": True})
    selected, outside = [], 0
    for t in alltests:
        if t.get("pseudo"):
            t["state"], t["markers"] = "executed", []
        # A pseudo-test is selected only through a BetaStage:: term it matches (a pure exclusion never selects it).
        if evaluate(ast, t) and (not t.get("pseudo") or any(beta_stage_leaf(lf) and leaf_match(lf, t) for lf in leaves)):
            if t.get("beta_credit"):
                rep["beta_credited"].append({"fqn": t["fqn"], "beta_trx": t["beta_credit"],
                                             "kind": "Beta pseudo-test" if t.get("pseudo") else "native fork-only"})
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
                row["skipped" if t["state"] == "forkOnly" else t["state"]] += 1
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
        c["skipped" if t["state"] == "forkOnly" else t["state"]] += 1
        if t["state"] == "forkOnly":
            rep["problems"].append("fork-only test selected on a native lane without Beta-host evidence: %s (%s)"
                                   % (t["fqn"], ",".join(t["markers"])))
        elif t["state"] == "skipped":
            rep["problems"].append("skipped: %s::%s %s" % (t["class"], t["name"], ",".join(t["markers"])))
        elif t["state"] == "notExecuted":
            rep["problems"].append("not executed: %s::%s (%s)" % (t["class"], t["name"], ",".join(t["markers"])))
        elif any(r["outcome"].lower() != "passed" for r in t["results"]):
            rep["failed_tests"].append("%s::%s" % (t["class"], t["name"]))
    rep["classes"] = [classes[k] for k in sorted(classes)]
    if (files or beta_paths) and not any(t["state"] == "executed" for t in selected) \
            and not any(p.startswith("term ") for p in rep["problems"]):
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
        for b in t["build_lines"] or ["(no build line in this TRX)"]:
            lines.append("  build: %s" % b)
    for b in rep["beta"]:
        lines.append("BETA TRX: %s %s" % (b["path"], "VALID" if b["valid"] else "INVALID (%s)" % b["why"]))
        lines.append("  BETA-RAN ids used: %d (transport: %s)" % (len(b["ids"]), TRANSPORT_NOTE))
        for x in b["build_lines"] or ["(no build line in this TRX)"]:
            lines.append("  build: %s" % x)
    if rep["beta_credited"]:
        lines.append("EXECUTED ON THE BETA HOST (credited by BETA-RAN):")
        for c in rep["beta_credited"]:
            lines.append("  %s [%s] <- %s" % (c["fqn"], c["kind"], c["beta_trx"]))
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

BETA_DLL_FIXTURE = "D:\\w\\x64\\Release\\vprenderer-beta\\VideoProcessorBetaTestsHost.dll"
NATIVE_DLL_FIXTURE = "D:\\w\\x64\\Release\\VideoProcessor-Test.dll"


def _fixture(tests, counters=True, outcome="Completed", extra="", total=None, codebase=""):
    """tests: list of (class, method, outcome or None for definition only, stdout).
    outcome: the ResultSummary outcome; extra: extra Counters attributes (e.g. 'aborted="1"');
    total: the Counters total (default: the number of result rows, = executed); codebase: the TestMethod codeBase."""
    defs, res = [], []
    cb = ' codeBase="%s"' % codebase if codebase else ""
    for i, (cls, meth, toc, out) in enumerate(tests):
        tid = "00000000-0000-0000-0000-%012d" % i
        ns = cls.split(".")[0]
        short = cls.split(".")[-1]
        defs.append('<UnitTest name="%s" id="%s"><TestMethod%s className="%s" name="%s::%s::%s" /></UnitTest>'
                    % (meth, tid, cb, cls, ns, short, meth))
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
        paths, betas = [], []
        for i, fx in enumerate(fixtures):
            p = os.path.join(d, "run%d.trx" % i)
            dest = paths
            if isinstance(fx, tuple):   # ("BETA", text): a --beta-trx file; ("BETA", "MISSING"): a missing one
                dest, fx = betas, fx[1]
            dest.append(p)
            if fx == "MISSING":
                continue
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(fx)
        if flt.startswith("QUEUE:"):
            flt = filter_from_queue_line(flt[len("QUEUE:"):])[0]
        rep = check(flt, paths, beta_paths=betas)
        return (1 if rep["problems"] else 0), rep


def _beta(out, outcome="Passed", codebase=BETA_DLL_FIXTURE):
    """A Beta-host TRX fixture: BetaSuite::RunBetaSuite (from the Beta host DLL by default) with this output."""
    return ("BETA", _fixture([("BetaSuite", "RunBetaSuite", outcome, out)], codebase=codebase))


def fork_only_cases():
    """(name, filter, fixtures, expected rc, problem substring or '', note substring or '')."""
    fq = "VideoProcessorTest::SigmaDeltaHookTests::DitherNeedWarpX"
    mark = "SKIP-FORK-ONLY-PL0004: needs libplacebo fork patch 0004; Beta-host evidence: BETA-RAN " + fq
    D = ("VideoProcessorTest.SigmaDeltaHookTests", "DitherNeedOnCpu", "Passed", "")
    X = ("VideoProcessorTest.SigmaDeltaHookTests", "DitherNeedWarpX", "Passed", mark)
    ok_tail = "\nBeta suite exit code 0; whole chain passed"
    flt = "FullyQualifiedName~VideoProcessorTest::SigmaDeltaHookTests"
    nofq = "fork-only test selected on a native lane without Beta-host evidence: " + fq
    return [
        ("fork-only credited by a valid Beta TRX", flt, [_fixture([D, X]), _beta("BETA-RAN " + fq + ok_tail)], 0, "", ""),
        ("fork-only credit is case-insensitive full equality", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq.lower() + ok_tail)], 0, "", ""),
        ("fork-only without --beta-trx", flt, [_fixture([D, X])], 1, nofq, ""),
        ("fork-only, Beta id is only a prefix of the FQN", flt,
         [_fixture([D, X]), _beta("BETA-RAN VideoProcessorTest::SigmaDeltaHookTests::DitherNeedWarp" + ok_tail)], 1, nofq, ""),
        ("fork-only, mismatched FQN", flt,
         [_fixture([D, X]), _beta("BETA-RAN VideoProcessorTest::SigmaDeltaHookTests::DitherNeedWarpY" + ok_tail)], 1, nofq, ""),
        ("non-allow-listed fork tag is a plain skip", flt,
         [_fixture([D, ("VideoProcessorTest.SigmaDeltaHookTests", "DitherNeedWarpX", "Passed",
                        "SKIP-FORK-ONLY-PL0009: x BETA-RAN " + fq)]), _beta("BETA-RAN " + fq + ok_tail)], 1,
         "skipped: VideoProcessorTest.SigmaDeltaHookTests::DitherNeedWarpX SKIP-FORK-ONLY-PL0009", "credits nothing"),
        ("fork tag beside another SKIP marker is a plain skip", flt,
         [_fixture([D, ("VideoProcessorTest.SigmaDeltaHookTests", "DitherNeedWarpX", "Passed", mark + "\nSKIP-NO-FOOTAGE")]),
          _beta("BETA-RAN " + fq + ok_tail)], 1, "skipped: VideoProcessorTest.SigmaDeltaHookTests::DitherNeedWarpX", ""),
        ("BETA-RAN lines in the native TRX credit nothing", flt,
         [_fixture([D, X, ("VideoProcessorTest.SigmaDeltaHookTests", "Echo", "Passed", "BETA-RAN " + fq)])], 1, nofq,
         "BETA-RAN line(s) in the native TRX"),
        ("a --beta-trx without RunBetaSuite is not a Beta TRX", flt,
         [_fixture([D, X]), ("BETA", _fixture([("Other.OtherTests", "M", "Passed", "BETA-RAN " + fq + ok_tail)]))], 1,
         nofq, "not a Beta TRX"),
        ("report-mode Beta TRX is invalid", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq + ok_tail + " (VP_TEST_REPORT_MODE=1)")], 1, nofq, "report-mode run"),
        ("Beta TRX without whole chain passed is invalid", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq + "\nBeta suite exit code 0; whole chain NOT reached")], 1, nofq,
         "no \"whole chain passed\""),
        ("Beta TRX whose RunBetaSuite failed is invalid", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq + ok_tail, outcome="Failed")], 1, nofq, "did not execute and pass"),
        ("Beta TRX with a SKIP marker is invalid", flt,
         [_fixture([D, X]), _beta("SKIP-NO-CORPUS c\nBETA-RAN " + fq + ok_tail)], 1, nofq, "carries a SKIP marker"),
        ("SKIP-NO-GPU in a Beta TRX fails", flt,
         [_fixture([D, X]), _beta("SKIP-NO-GPU g\nBETA-RAN " + fq + ok_tail)], 1, "in the Beta TRX", ""),
        ("BETA-RAN naming an untagged native test credits nothing", flt,
         [_fixture([D, ("VideoProcessorTest.SigmaDeltaHookTests", "NeedsClip", "Passed", "SKIP-NO-FOOTAGE f")]),
          _beta("BETA-RAN VideoProcessorTest::SigmaDeltaHookTests::NeedsClip" + ok_tail)], 1,
         "skipped: VideoProcessorTest.SigmaDeltaHookTests::NeedsClip SKIP-NO-FOOTAGE", "credits nothing"),
        ("Beta pseudo-test covers a BetaStage term", "FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests",
         [_beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a\nBETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17b"
                + ok_tail)], 0, "", ""),
        ("BetaStage term with no BETA-RAN line is a zero-match", "FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests",
         [_beta("BETA-RAN BetaStage::OtherTests::T1" + ok_tail)], 1,
         "term FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests: 0 executed", ""),
        ("BetaStage ids of an invalid Beta TRX are no pseudo-tests", "FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests",
         [_beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail + " (VP_TEST_REPORT_MODE=1)")], 1,
         "0 executed", "report-mode run"),
        ("missing Beta TRX", flt, [_fixture([D, X]), ("BETA", "MISSING")], 1, "Beta TRX missing", ""),
        # Gate GR MUST-1 probes (gate-A20C5-forkonly-b28cc15ce.md): Beta lines never cover a native term alone.
        ("P1: native class term, no native test, Beta line only", flt,
         [_fixture([("Other.OtherTests", "M", "Passed", "")]), _beta("BETA-RAN " + fq + ok_tail)], 1,
         "term FullyQualifiedName~VideoProcessorTest::SigmaDeltaHookTests: 0 executed",
         "BETA-RAN " + fq + " names no native test of this run and is not a BetaStage id: credits nothing"),
        ("P2: fork-only test absent from the native TRX, Beta line names it", "Name~DitherNeedWarpX",
         [_fixture([D]), _beta("BETA-RAN " + fq + ok_tail)], 1, "term Name~DitherNeedWarpX: 0 executed",
         "is not a BetaStage id: credits nothing"),
        ("BetaStage prefix is case-insensitive", "FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests",
         [_beta("BETA-RAN betastage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail)], 0, "", ""),
        # Gate GR MUST-2: a pseudo-test satisfies only FullyQualifiedName~BetaStage::... terms.
        ("pseudo-test does not satisfy a broad FQN term", "FullyQualifiedName~Sigma",
         [_beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail)], 1,
         "term FullyQualifiedName~Sigma: 0 executed", ""),
        ("pseudo-test does not satisfy a Name~ term", "Name~T17a",
         [_beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail)], 1, "term Name~T17a: 0 executed", ""),
        ("pseudo-test beside a native term counts only for its BetaStage term",
         "FullyQualifiedName~BetaStage::SigmaDeltaCameraGoldenTests|FullyQualifiedName~SigmaDelta",
         [_fixture([("Other.OtherTests", "M", "Passed", "")]),
          _beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail)], 1,
         "term FullyQualifiedName~SigmaDelta: 0 executed", ""),
        ("pure exclusion never selects a pseudo-test", "FullyQualifiedName!~Nothing",
         [_beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail)], 1, "nothing executed", ""),
        ("exact FullyQualifiedName= BetaStage term is covered",
         "FullyQualifiedName=BetaStage::SigmaDeltaCameraGoldenTests::T17a",
         [_beta("BETA-RAN BetaStage::SigmaDeltaCameraGoldenTests::T17a" + ok_tail)], 0, "", ""),
        # SHOULD 1: the report-mode token, with or without parentheses.
        ("P7: report-mode token without parentheses is invalid", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq + ok_tail + "; VP_TEST_REPORT_MODE=1")], 1, nofq, "report-mode run"),
        # SHOULD 3: RunBetaSuite must come from the Beta host DLL.
        ("BetaSuite from a native DLL is not a Beta TRX", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq + ok_tail, codebase=NATIVE_DLL_FIXTURE)], 1, nofq,
         "not from the Beta host DLL"),
        ("BetaSuite with no codeBase is not a Beta TRX", flt,
         [_fixture([D, X]), _beta("BETA-RAN " + fq + ok_tail, codebase="")], 1, nofq, "not from the Beta host DLL"),
        # SHOULD 2: a valid Beta TRX without BETA-RAN lines names the Logger transport requirement.
        ("valid Beta TRX with no BETA-RAN line states the transport rule", flt,
         [_fixture([D, X]), _beta("Beta suite exit code 0; whole chain passed")], 1, nofq,
         "echo each BETA-RAN line through Logger::WriteMessage"),
    ]


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
    fork = fork_only_cases()
    for name, flt, fx, want_rc, want_sub, want_note in fork:
        rc, rep = run_case(flt, fx)
        ok = (rc == want_rc and (not want_sub or any(want_sub in p for p in rep["problems"]))
              and (not want_note or any(want_note in n for n in rep["notes"])))
        if not ok:
            bad += 1
            print("SELFTEST FAIL %s: rc=%d want %d; problems=%s notes=%s" % (name, rc, want_rc, rep["problems"], rep["notes"]))
    n = len(selftest_cases()) + len(bad_filters) + len(dir_cases) + len(fork)
    print("selftest: %d/%d OK" % (n - bad, n))
    return 0 if bad == 0 else 1


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description="TRX filter-coverage checker (AREA-RULES 11(b))")
    ap.add_argument("trx", nargs="*", help="TRX files or directories holding .trx files")
    ap.add_argument("--trx", dest="trx_files", action="append", default=[],
                    help="one TRX file (repeatable); read exactly as given")
    ap.add_argument("--beta-trx", dest="beta_trx", action="append", default=[],
                    help="a Beta-host TRX (BetaSuite::RunBetaSuite, repeatable): its BETA-RAN lines credit "
                         "allow-listed fork-only native tests and become Beta pseudo-tests")
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
        for f in a.trx_files + a.beta_trx:
            if os.path.isdir(f):
                raise FilterError("--trx / --beta-trx take a file, not a directory: %s" % f)
        rep = check(flt, list(a.trx_files) + list(a.trx), newest=a.newest, beta_paths=a.beta_trx)
    except FilterError as e:
        print("usage error: %s" % e, file=sys.stderr)
        return 2
    print(json.dumps(rep, indent=1, sort_keys=True) if a.json else render(rep))
    return 1 if rep["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
