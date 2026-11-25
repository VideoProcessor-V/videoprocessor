# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""One-shot compliance run (doc 124 s2.2): runs every compliance checker present in tools/ and writes ONE summary file.

Usage: python -I tools/run_compliance.py [--root P] [--out FILE] [--since REF] [--only NAME[,NAME]] [--py-lane] [--list]

Each checker runs as its own process (python -I, cwd = root, 600 s limit). A checker whose file is absent in this
tree is reported SKIP (absent), never a pass, and does not change the exit code; the summary lists it so a missing
checker is visible. --since REF is passed to the checkers that take it (N2, N3, N4, N5, check_hook_gating). The summary
file (default tools/../compliance.txt under the root's scratch: --out) holds one line per checker plus the last
lines of each non-zero one. Exit: 0 all present checkers clean, 1 at least one violation, 2 a checker error or
timeout (the worst code wins).

--py-lane adds the PY lane (tools/run_py_suites.py: every Python unittest suite under tools/, TQ-RG-P1). It is opt-in here
because tools/test_libplacebo_variants.py needs the x64 Release build (a missing build FAILS, it is not skipped) and the lane
takes minutes; run_chain_gate.ps1 runs it by default, after the build.
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
    ("bespoke_skips", "check_bespoke_skips.py", [], False),
    ("private_copies", "check_private_copies.py", ["--strict"], False),
    ("test_tamper", "check_test_tamper.py", ["--strict"], False),
    ("producer_chain", "producer_chain.py", ["--check"], False),   # 15d J3 (ii): a stale producer-chain table = exit 1
    ("N2_precision", "check_precision.py", ["--strict"], True),
    ("N3_fallback", "check_fallback_coverage.py", ["--strict"], True),
    ("N4_general", "check_general_not_fixed.py", ["--strict"], True),
    ("N5_public_hygiene", "check_public_hygiene.py", ["--strict"], True),
    ("N6_launchers", "check_launcher_workarounds.py", ["--strict"], False),
    # N7 (check_records.py) is intentionally skipped here: it targets private external drive-D records outside this repo.
    ("N8_readme_vs_code", "check_readme_vs_code.py", ["--strict"], False),
    ("ctlscan", "ctlscan.py", [], True),
]


# The PY lane (opt-in, see the module docstring): the whole lane is one entry with a 3600 s limit (the checkers have 600 s).
PY_LANE = ("py_suites", "run_py_suites.py", [], False)


def run_one(root, tools_dir, name, script, args, since_ok, since, timeout=None):
    if timeout is None:
        timeout = 3600 if script == PY_LANE[1] else 600
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


def selftest_failures():
    """Names of the failed selftest checks (empty when all pass): unique TOOLS names, and run_all() on stub tools in a temp
    root for every outcome (clean, violation rc 1, error rc 2, crash rc 7, absent script = SKIP, --only, --since only to the
    tools that take it) with the exact worst exit code and summary text."""
    import shutil
    import tempfile
    failed = []

    def check(name, cond):
        if not cond:
            failed.append(name)
    check("TOOLS names are unique", len(TOOLS) == len({t[0] for t in TOOLS}))
    root = tempfile.mkdtemp()
    try:
        os.makedirs(os.path.join(root, "tools"))

        def slurp(path):
            with open(path, encoding="utf-8") as fh:
                return fh.read()

        def stub(name, code, record=False):
            body = "import sys\nprint('stub %s')\n" % name
            if record:
                body += "open(sys.argv[0] + '.args', 'w').write(' '.join(sys.argv[1:]))\n"
            with open(os.path.join(root, "tools", name + ".py"), "w", newline="\n") as fh:
                fh.write(body + "sys.exit(%d)\n" % code)
        for name, code in (("ok", 0), ("bad", 1), ("err", 2), ("crash", 7)):
            stub(name, code, record=True)
        tools = {n: (n, n + ".py", [], n == "ok") for n in ("ok", "bad", "err", "crash")}
        tools["gone"] = ("gone", "gone.py", [], False)
        t = lambda *names: [tools[n] for n in names]
        rc, text = run_all(root, t("ok"))
        check("clean run exits 0 with one pass", rc == 0 and "1 pass, 0 fail, 0 error, 0 skipped (absent); exit 0" in text)
        rc, text = run_all(root, t("ok", "bad"))
        check("a violation exits 1 and its tail is listed", rc == 1 and "1 fail" in text and "-- bad" in text and "stub bad" in text)
        rc, text = run_all(root, t("ok", "err"))
        check("a checker error exits 2", rc == 2 and "1 error" in text and "-- err" in text)
        rc, text = run_all(root, t("crash"))
        check("any other exit code is an error (2)", rc == 2 and "ERROR" in text)
        rc, text = run_all(root, t("bad", "err"))
        check("the worst code wins", rc == 2 and "1 fail, 1 error" in text)
        rc, text = run_all(root, t("ok", "gone"))
        check("an absent script is SKIP and does not change the exit", rc == 0 and "SKIP" in text and "1 skipped (absent)" in text)
        results = [run_one(root, os.path.join(root, "tools"), *tools["gone"], None)]
        check("an absent script reports rc None", results[0]["status"] == "SKIP" and results[0]["rc"] is None)
        rc, text = run_all(root, t("ok", "bad"), only={"ok"})
        check("--only runs just the named checker", rc == 0 and "bad" not in text)
        rc, text = run_all(root, t("ok", "bad"), only={"nope"})
        check("--only with no match runs nothing", rc == 0 and "0 pass" in text)
        out = os.path.join(root, "out", "summary.txt")
        run_all(root, t("ok"), out=out)
        check("--out writes the summary", os.path.isfile(out) and "compliance:" in slurp(out))
        run_all(root, t("ok", "bad"), since="abc123")
        ok_args = slurp(os.path.join(root, "tools", "ok.py.args"))
        bad_args = slurp(os.path.join(root, "tools", "bad.py.args"))
        check("--since goes to a tool that takes it, with --root", "--since abc123" in ok_args and "--root" in ok_args)
        check("--since is not passed to a tool that does not", "--since" not in bad_args)
    finally:
        shutil.rmtree(root, True)
    return failed


def main(argv=None):
    ap = argparse.ArgumentParser(description="run every compliance checker, write one summary file")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--out", help="summary file (default: print only)")
    ap.add_argument("--since")
    ap.add_argument("--only", help="comma-separated checker names")
    ap.add_argument("--py-lane", action="store_true", help="also run the PY lane (needs the x64 Release build)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        failed = selftest_failures()
        for name in failed:
            print("selftest FAILED check: " + name)
        print("selftest run_compliance %s" % ("ok" if not failed else "FAIL"))
        return 0 if not failed else 2
    if a.list:
        for t in TOOLS + ([PY_LANE] if a.py_lane else []):
            print("%-17s %s" % (t[0], t[1]))
        return 0
    worst, text = run_all(a.root, TOOLS + [PY_LANE] if a.py_lane else None, a.since, set(a.only.split(",")) if a.only else None, a.out)
    sys.stdout.write(text)
    return worst


if __name__ == "__main__":
    sys.exit(main())
