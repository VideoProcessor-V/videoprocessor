# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Helper for tools/looprig/run_loop_tier.ps1 (the unattended loop test tier).

    python -I tools/looprig/loop_tier.py detect --recorder StreamRecorder.exe [--match "Mini Recorder 4K"]
        exit 0 and "RIG <n>: <name>" when a matching DeckLink is listed, exit 10 and "SKIP-NO-RIG ..." otherwise.
    python -I tools/looprig/loop_tier.py plan --mode 1080p60|2160p30 [--only a,b] [--attended]
        JSON list of the scenarios the tier runs in that mode (tools/looprig/loop_tier_plan.json).
    python -I tools/looprig/loop_tier.py synth --out DIR --mode 1080p60 [--only a,b] [--seconds 20] [--break ID]
        writes a synthetic capture folder (no rig): one sub-folder per scenario with stamps.csv, sent.csv,
        vp.log and meta.json. --break ID makes that scenario lose frames so it must FAIL.
    python -I tools/looprig/loop_tier.py score --run DIR [--rig-state ran|<SKIP text>]
        scores every scenario folder in DIR with analyze.py, writes DIR/summary.md and DIR/loop-tier.trx.
        Exit 0 all pass (or skipped), 1 any fail or incomplete.

A scenario folder (written by the runner or by synth) holds: stamps.csv (StreamRecorder --stamps), sent.csv
(make_stamped_clip.py), vp.log, meta.json {"scenario", "width", "height"} and optionally expected.json.
No data is a FAIL (fail closed), never a pass.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import uuid
from fractions import Fraction
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import analyze as az  # noqa: E402

PLAN = os.path.join(HERE, "loop_tier_plan.json")
SCENARIOS = os.path.join(HERE, "scenarios.json")


def load_plan():
    with open(PLAN, "r", encoding="utf-8") as f:
        return json.load(f)


def plan_for(mode, only=None, attended=False):
    plan = load_plan()
    if mode not in plan["modes"]:
        raise SystemExit("unknown mode %r (known: %s)" % (mode, ", ".join(plan["modes"])))
    out = []
    for s in plan["scenarios"]:
        if s["mode"] != mode:
            continue
        if s.get("attended") and not attended:
            continue
        if only and s["id"] not in only:
            continue
        out.append(s)
    return out


def cmd_detect(a):
    try:
        p = subprocess.run([a.recorder, "--list"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        print("SKIP-NO-RIG recorder not runnable: %s" % e)
        return 10
    rx = re.compile(a.match, re.I)
    for line in p.stdout.splitlines():
        m = re.match(r"\s*(\d+):\s*(.+)$", line)
        if m and rx.search(m.group(2)):
            print("RIG %s: %s" % (m.group(1), m.group(2).strip()))
            return 0
    print("SKIP-NO-RIG no DeckLink matching %r (recorder said: %s)" % (a.match, (p.stdout.strip() or "nothing")[:200]))
    return 10


def cmd_plan(a):
    only = [x for x in a.only.split(",") if x] if a.only else None
    print(json.dumps(plan_for(a.mode, only, a.attended), indent=1))
    return 0


def scenario_doc(sid):
    return az.load_scenario(SCENARIOS, sid)


def synth_one(folder, entry, seconds, broken):
    sc = scenario_doc(entry.get("scenario", entry["id"]))
    src = Fraction(entry["src_fps"][0], entry["src_fps"][1])
    out = Fraction(sc["out_rate"][0], sc["out_rate"][1])
    ratio = out / src
    n_src = int(seconds * src)
    os.makedirs(folder, exist_ok=True)
    cad = entry.get("cadence")
    plen = sum(int(x) for x in cad.split(":")) if cad else 1
    rows = []
    for k in range(n_src):
        if broken and k % 50 == 25:
            continue  # a source frame that never reaches the output: an unexplained (vp) drop
        n = int((k + 1) * ratio) - int(k * ratio)
        rows += [k] * n
    with open(os.path.join(folder, "stamps.csv"), "w", encoding="utf-8", newline="") as f:
        f.write("frame_index,arrival_qpc,src_found,src_kind,src_aux,src_index,src_contrast,out_found,out_aux,out_index,out_contrast\n")
        for i, k in enumerate(rows):
            f.write("%d,%d,1,1,%d,%d,200.0,1,0,%d,200.0\n" % (i, i, k % plen, k, i))
    with open(os.path.join(folder, "sent.csv"), "w", encoding="utf-8", newline="") as f:
        f.write("index,aux,pts_num,pts_den\n")
        for k in range(n_src):
            f.write("%d,%d,%d,%d\n" % (k, k % plen, k * src.denominator, src.numerator))
    with open(os.path.join(folder, "vp.log"), "w", encoding="utf-8") as f:
        f.write("2026-11-01 10:00:00 | synthetic capture folder (no rig)\n")
    with open(os.path.join(folder, "meta.json"), "w", encoding="utf-8") as f:
        json.dump({"scenario": entry.get("scenario", entry["id"]), "width": sc["size"][0], "height": sc["size"][1], "synthetic": True}, f)


def cmd_synth(a):
    only = [x for x in a.only.split(",") if x] if a.only else None
    entries = [e for e in plan_for(a.mode, only) if not e.get("schedule")]
    if not entries:
        print("no synthetic-capable scenario selected")
        return 2
    for e in entries:
        synth_one(os.path.join(a.out, e["id"]), e, a.seconds, a.break_id == e["id"])
    print("synthetic folders: %s" % ", ".join(e["id"] for e in entries))
    return 0


def score_folder(folder):
    """-> (outcome Passed|Failed|NotExecuted, message)."""
    meta_path = os.path.join(folder, "meta.json")
    if not os.path.isfile(meta_path):
        return "Failed", "no meta.json (the scenario did not run or did not finish)"
    with open(meta_path, "r", encoding="utf-8") as f:
        meta = json.load(f)
    if meta.get("skipped"):
        return "NotExecuted", str(meta["skipped"])
    if meta.get("recorder_error"):
        return "Failed", str(meta["recorder_error"])
    missing = [n for n in ("stamps.csv", "sent.csv", "vp.log") if not os.path.isfile(os.path.join(folder, n))]
    if missing:
        return "Failed", "NO DATA, missing " + ", ".join(missing)
    args = ["--scenario", meta["scenario"], "--recording", os.path.join(folder, "stamps.csv"),
            "--sent", os.path.join(folder, "sent.csv"), "--log", os.path.join(folder, "vp.log"),
            "--width", str(meta["width"]), "--height", str(meta["height"]), "--out", os.path.join(folder, "report")]
    exp = os.path.join(folder, "expected.json")
    if os.path.isfile(exp):
        args += ["--expected", exp]
    rc = az.main(args)
    optional = set(load_plan().get("optional_rows", []))
    try:
        with open(os.path.join(folder, "report", "report.json"), "r", encoding="utf-8") as f:
            rep = json.load(f)
    except (OSError, ValueError):
        return "Failed", "analyzer exit %d and no report.json" % rc
    # A NO DATA row is a failure (fail closed) except the rows the rig cannot measure yet (optional_rows).
    bad = [r for r in rep["rows"] if r["status"] == "FAIL" or (r["status"] == "NO DATA" and r["id"] not in optional)]
    waived = [r["id"] for r in rep["rows"] if r["status"] == "NO DATA" and r["id"] in optional]
    note = ("" if not waived else "no data (waived): " + ",".join(waived))
    if not bad and rc in (0, 3):
        return "Passed", note
    detail = "; ".join("%s=%s: %s" % (r["id"], r["status"], r["detail"]) for r in bad)[:2000]
    return "Failed", ("analyzer exit %d. " % rc) + detail


def write_trx(path, results):
    now = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    n = len(results)
    passed = sum(1 for r in results if r[1] == "Passed")
    failed = sum(1 for r in results if r[1] == "Failed")
    skipped = sum(1 for r in results if r[1] == "NotExecuted")
    L = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<TestRun id="%s" name="LoopRig tier" xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010">' % uuid.uuid4(),
         '<Times creation="%s" start="%s" finish="%s" />' % (now, now, now), "<Results>"]
    for name, outcome, msg in results:
        tid = uuid.uuid5(uuid.NAMESPACE_URL, "looprig/" + name)
        L.append('<UnitTestResult testName="LoopRig.%s" outcome="%s" testId="%s" executionId="%s">' % (escape(name), outcome, tid, tid))
        if msg:
            if outcome == "Failed":
                L.append("<Output><ErrorInfo><Message>%s</Message></ErrorInfo></Output>" % escape(msg))
            else:
                L.append("<Output><StdOut>%s</StdOut></Output>" % escape(msg))
        L.append("</UnitTestResult>")
    L += ["</Results>", '<ResultSummary outcome="%s"><Counters total="%d" executed="%d" passed="%d" failed="%d" notExecuted="%d" /></ResultSummary>' % (
        "Failed" if failed else "Completed", n, n - skipped, passed, failed, skipped), "</TestRun>"]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")


def cmd_score(a):
    results = []
    if a.rig_state != "ran":
        for e in load_plan()["scenarios"]:
            results.append((e["id"], "NotExecuted", a.rig_state))
    else:
        for name in sorted(os.listdir(a.run)):
            if os.path.isdir(os.path.join(a.run, name)) and os.path.isfile(os.path.join(a.run, name, "meta.json")):
                results.append((name,) + score_folder(os.path.join(a.run, name)))
        if not results:
            results.append(("no-scenarios", "Failed", "no scenario folder in %s" % a.run))
    write_trx(os.path.join(a.run, "loop-tier.trx"), results)
    counts = {}
    for _n, o, _m in results:
        counts[o] = counts.get(o, 0) + 1
    L = ["# Loop tier summary", "", "Run: %s" % a.run, "Rig: %s" % a.rig_state, "",
         "Totals: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())), "",
         "| scenario | outcome | detail |", "|---|---|---|"]
    for n, o, m in results:
        L.append("| %s | %s | %s |" % (n, o, m.replace("|", "/").replace("\n", " ")))
    text = "\n".join(L) + "\n"
    with open(os.path.join(a.run, "summary.md"), "w", encoding="utf-8") as f:
        f.write(text)
    print(text)
    return 1 if counts.get("Failed") else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("detect")
    d.add_argument("--recorder", required=True)
    d.add_argument("--match", default="Mini Recorder 4K")
    p = sub.add_parser("plan")
    p.add_argument("--mode", required=True)
    p.add_argument("--only")
    p.add_argument("--attended", action="store_true")
    s = sub.add_parser("synth")
    s.add_argument("--out", required=True)
    s.add_argument("--mode", required=True)
    s.add_argument("--only")
    s.add_argument("--seconds", type=float, default=20)
    s.add_argument("--break", dest="break_id")
    c = sub.add_parser("score")
    c.add_argument("--run", required=True)
    c.add_argument("--rig-state", default="ran")
    a = ap.parse_args(argv)
    return {"detect": cmd_detect, "plan": cmd_plan, "synth": cmd_synth, "score": cmd_score}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
