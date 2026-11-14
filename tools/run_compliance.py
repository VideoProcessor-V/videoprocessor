# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""One-shot compliance run (doc 124 s2.2): runs every compliance checker present in tools/ and writes ONE summary file.

Usage: python -I tools/run_compliance.py [--root P] [--out FILE] [--since REF] [--only NAME[,NAME]] [--list]

Each checker runs as its own process (python -I, cwd = root, 600 s limit). A checker whose file is absent in this
tree is reported SKIP (absent), never a pass, and does not change the exit code; the summary lists it so a missing
checker is visible. --since REF is passed to the checkers that take it (N2, N3, N4, check_hook_gating). The summary
file (default tools/../compliance.txt under the root's scratch: --out) holds one line per checker plus the last
lines of each non-zero one. Exit: 0 all present checkers clean, 1 at least one violation, 2 a checker error or
timeout (the worst code wins).
"""
import argparse
import os
import subprocess
import sys
import time

# (name, script, args, takes --since)
TOOLS = [
    ("hook_gating", "check_hook_gating.py", [], True),
    ("footprint_budget", "vp_footprint.py", ["--budget", "docs/audit/footprint-budget.json"], False),
    ("motion_bypass", "check_motion_bypass.py", [], False),
    ("shared_units", "check_shared_units.py", ["--strict"], False),
    ("master_settings", "check_master_settings.py", ["--strict"], False),
    ("config_placement", "check_config_placement.py", [], False),
    ("module_manifests", "check_module_manifests.py", [], False),
    ("test_categories", "check_test_categories.py", [], False),
    ("private_copies", "check_private_copies.py", [], False),
    ("test_tamper", "check_test_tamper.py", ["--strict"], False),
    ("N2_precision", "check_precision.py", ["--strict"], True),
    ("N3_fallback", "check_fallback_coverage.py", ["--strict"], True),
    ("N4_general", "check_general_not_fixed.py", ["--strict"], True),
    ("N6_launchers", "check_launcher_workarounds.py", ["--strict"], False),
]


def run_one(root, tools_dir, name, script, args, since_ok, since, timeout=600):
    path = os.path.join(tools_dir, script)
    if not os.path.isfile(path):
        return {"name": name, "status": "SKIP", "rc": None, "note": "absent in this tree", "tail": []}
    cmd = [sys.executable, "-I", path, "--root", root] if script not in ("check_module_manifests.py", "check_test_categories.py",
                                                                        "vp_footprint.py", "check_config_placement.py") else [sys.executable, "-I", path]
    if script == "vp_footprint.py":
        cmd += ["--repo", root]
    cmd += list(args)
    if since and since_ok:
        cmd += ["--since", since]
    t0 = time.time()
    try:
        r = subprocess.run(cmd, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        rc, out = r.returncode, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        rc, out = 2, "timeout after %d s" % timeout
    except OSError as e:
        rc, out = 2, "cannot run: %s" % e
    lines = [x for x in out.splitlines() if x.strip()]
    status = "PASS" if rc == 0 else ("FAIL" if rc == 1 else "ERROR")
    return {"name": name, "status": status, "rc": rc, "note": "%.0f s" % (time.time() - t0), "tail": lines[-6:] if rc else lines[-1:]}


def run_all(root, tools=None, since=None, only=None, out=None):
    tools_dir = os.path.join(root, "tools")
    results = []
    for name, script, args, since_ok in (tools or TOOLS):
        if only and name not in only:
            continue
        results.append(run_one(root, tools_dir, name, script, args, since_ok, since))
    worst = 0
    for r in results:
        if r["rc"] is not None:
            worst = max(worst, 2 if r["rc"] not in (0, 1) else r["rc"])
    lines = ["compliance run: root %s%s" % (root, (", since " + since) if since else "")]
    for r in results:
        lines.append("%-17s %-5s rc=%s %s" % (r["name"], r["status"], r["rc"], r["note"]))
    for r in results:
        if r["status"] in ("FAIL", "ERROR"):
            lines.append("-- %s" % r["name"])
            lines.extend("   " + x[:220] for x in r["tail"])
    npass = sum(1 for r in results if r["status"] == "PASS")
    lines.append("compliance: %d pass, %d fail, %d error, %d skipped (absent); exit %d" % (
        npass, sum(1 for r in results if r["status"] == "FAIL"), sum(1 for r in results if r["status"] == "ERROR"),
        sum(1 for r in results if r["status"] == "SKIP"), worst))
    text = "\n".join(lines) + "\n"
    if out:
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    return worst, text


def main(argv=None):
    ap = argparse.ArgumentParser(description="run every compliance checker, write one summary file")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--out", help="summary file (default: print only)")
    ap.add_argument("--since")
    ap.add_argument("--only", help="comma-separated checker names")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        ok = len(TOOLS) == len({t[0] for t in TOOLS})
        print("selftest run_compliance %s" % ("ok" if ok else "FAIL"))
        return 0 if ok else 2
    if a.list:
        for t in TOOLS:
            print("%-17s %s" % (t[0], t[1]))
        return 0
    worst, text = run_all(a.root, None, a.since, set(a.only.split(",")) if a.only else None, a.out)
    sys.stdout.write(text)
    return worst


if __name__ == "__main__":
    sys.exit(main())
