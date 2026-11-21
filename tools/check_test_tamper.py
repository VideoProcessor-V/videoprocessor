# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Test / checker / baseline tamper gate (owner rule 19: no metric gaming).

Usage: python -I tools/check_test_tamper.py [--strict] [--range A..B | --base REF] [--root PATH] [--json]

Walks every non-merge commit in a range (default merge-base(HEAD, base ref)..HEAD) and flags each
commit that touches a guarded file class WITHOUT a WAIVER-<ID> trailer line in its message:

  test       src/**/tests/**, *Tests.cpp, *Test.cpp, tools/**/test_*.py
             (a test-ADDING-only change passes with a note: only added lines, no removed/edited line)
  checker    tools/check_*.py, tools/run_compliance.py, tools/vp_footprint.py,
             tools/vpv_tests/pairwise.py (any change needs a waiver)
  selftest   removed/edited assert or selftest lines in any other tools/**/*.py (needs a waiver)
  baseline   tools/*baseline*.json, docs/audit/*baseline*.json, *budget*.json (a change that only
                                                       REMOVES lines, i.e. a shrink, passes with a note)
  register_owner  an added/edited/removed ProductRows() row in src/VP-V/core/ProductCatalog.h (its folder is a register owner)
  allowlist  a changed line, in any other file, naming ALLOW_SITES, CoverageAllowList,
             known_false_positive or an ALLOW list/entry (removal-only passes with a note)

Exit 0 clean; 1 with --strict when any commit is flagged; 2 on a checker error (bad range, git failure).
A waiver is a line "WAIVER-<ID>: <why>" (ID = letters, digits, '-', '_', '.') at the start of a line.
A flagged commit is also waived when the ledger docs/audit/waiver-ledger.json AS COMMITTED AT THE RANGE TIP ({"entries":
[{sha, waiver_id, gate_record}]}) lists its full 40-hex sha (unique; a malformed or duplicate entry is rc 2) AND the
gate_record file, inside --gate-root / VP_GATE_ROOT (no absolute path, no ".."), is non-empty and has a verdict line with
GRANT or GRANT-WITH-CONDITIONS naming the first 9 characters of the sha. The summary prints "ledger-waived N" apart from
trailer waivers. A missing, null, empty or non-naming gate record waives nothing. --ledger FILE is for non-strict runs only.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

WAIVER_RX = re.compile(r"^WAIVER-[A-Za-z0-9_.\-]+(\s*:.*)?\s*$", re.M)
CHECKER_GLOBS = ["tools/check_*.py", "tools/run_compliance.py", "tools/compliance_lib.py", "tools/ctlscan.py",
                 "tools/test_ctlscan.py", "tools/test_check_test_tamper.py",
                 "tools/vp_footprint.py", "tools/vpv_tests/pairwise.py"]
PY_TEST_RX = re.compile(r"^tools/(?:.+/)?test_[^/]+\.py$")
PY_SELFTEST_RX = re.compile(r"\bassert\b|\bassert[A-Z]\w*\s*\(|\bselftest\b")
BASELINE_GLOBS = ["tools/*baseline*.json", "docs/audit/*baseline*.json", "docs/audit/*budget*.json",
                  "tools/*budget*.json", "docs/audit/waiver-ledger.json"]
# Ledger of commits whose waiver was gated but not written in the required trailer form (R14-108). A commit counts as
# ledger-waived only if its sha is listed AND its gate_record file exists under the gate root (VP_GATE_ROOT or --gate-root).
LEDGER_PATH = "docs/audit/waiver-ledger.json"
DEFAULT_GATE_ROOT = "D:/codex/vp-handover"
ALLOW_RX = re.compile(r"ALLOW_SITES|CoverageAllowList|known_false_positive|\bALLOW\s*=|\"allow\w*\"\s*:")


# A ProductRows() row of src/VP-V/core/ProductCatalog.h: { kPn, "<owner folder>", ... }. The folder names the register owner
# (check_private_copies rule d derives its owner folders from it), so any added/edited/removed row moves who may register a
# product: a rule-19 change (S-CHK2, SCHK-REGOWNERS condition).
PRODUCT_ROW_RX = re.compile(r'^\s*\{\s*k[A-Za-z0-9_]+\s*,\s*"[^"]*"\s*,')
PRODUCT_CATALOG = "src/VP-V/core/ProductCatalog.h"


def classify(path):
    if any(fnmatch.fnmatch(path, g) for g in CHECKER_GLOBS):
        return "checker"
    if any(fnmatch.fnmatch(path, g) for g in BASELINE_GLOBS):
        return "baseline"
    if "/tests/" in path or path.endswith(("Tests.cpp", "Test.cpp")) or "/VideoProcessor-Test/" in path:
        return "test"
    if PY_TEST_RX.match(path):
        return "test"
    return "other"


def path_is_catalog(path):
    return path == PRODUCT_CATALOG


def has_waiver(message):
    return bool(WAIVER_RX.search(message))


def judge(files, message):
    """files: [{path, status, added:[lines], removed:[lines]}]. Returns (violations, notes, waived)."""
    viol, notes = [], []
    for f in files:
        cls = classify(f["path"])
        add, rem = f["added"], f["removed"]
        if cls == "test":
            if f["status"].startswith("D"):
                viol.append(("test", f["path"], "deleted"))
            elif f["status"].startswith("A") or (add and not rem):
                notes.append("test-adding only: " + f["path"])
            elif add or rem:
                viol.append(("test", f["path"], "%d line(s) removed/edited" % len(rem)))
        elif cls == "checker":
            viol.append(("checker", f["path"], "checker changed"))
        elif cls == "baseline":
            if rem and not add:
                notes.append("baseline shrink: " + f["path"])
            else:
                viol.append(("baseline", f["path"], "baseline changed (+%d/-%d)" % (len(add), len(rem))))
        else:
            if path_is_catalog(f["path"]) and [l for l in add + rem if PRODUCT_ROW_RX.search(l)]:
                viol.append(("register_owner", f["path"], "ProductRows owner folder changed (moves the N1 rule d register owners)"))
            elif [l for l in add if ALLOW_RX.search(l)]:
                viol.append(("allowlist", f["path"], "allow-list line added/edited"))
            elif [l for l in rem if ALLOW_RX.search(l)]:
                notes.append("allow-list removal: " + f["path"])
            elif f["path"].startswith("tools/") and f["path"].endswith(".py") and [l for l in rem if PY_SELFTEST_RX.search(l)]:
                viol.append(("selftest", f["path"], "python selftest/assert line removed or edited"))
    if has_waiver(message):
        if viol:
            notes.append("WAIVER present: %d guarded change(s) waived" % len(viol))
        return [], notes, bool(viol)
    return viol, notes, False


def git(root, *args):
    r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()[:200]))
    return r.stdout


def commit_files(root, sha):
    names = git(root, "diff-tree", "--no-commit-id", "--name-status", "-r", "--no-renames", sha)
    out = []
    for line in names.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        status, path = parts[0], parts[-1]
        diff = git(root, "show", "--format=", "-U0", "--no-renames", sha, "--", path)
        add = [l[1:] for l in diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
        rem = [l[1:] for l in diff.splitlines() if l.startswith("-") and not l.startswith("---")]
        out.append({"path": path, "status": status, "added": add, "removed": rem})
    return out


def default_range(root, base):
    cands = [base] if base else ["staging/batch1", "origin/staging/batch1", "integ/batch1", "origin/v1.4.00-beta"]
    for c in cands:
        try:
            return git(root, "merge-base", "HEAD", c).strip() + "..HEAD"
        except RuntimeError:
            continue
    raise RuntimeError("no base ref found; pass --range or --base")


WAIVES_RX = re.compile(r"^Waives:\s*([0-9a-f]{7,40})\s*$", re.M)


def late_waivers(msgs):
    """{sha prefixes} named by a "Waives: <sha>" line in a commit that itself carries a WAIVER-<ID> trailer: a missed waiver is
    added by a follow-up commit (no history rewrite). The waiving commit is still judged on its own."""
    out = set()
    for m in msgs:
        if has_waiver(m):
            out.update(WAIVES_RX.findall(m))
    return out


LEDGER_SHA_RX = re.compile(r"^[0-9a-f]{40}$")
VERDICT_RX = re.compile(r"\bGRANT\b")
NEGATIVE_RX = re.compile(r"\b(?:deny|denied|refuse|refused|refusal|hold|pending|withdrawn|no[- ]?grant|not[- ]grant(?:ed)?)\b|\bnogrant", re.I)


def parse_ledger(text):
    """Entries of a ledger text ({"entries": [...]}). Every sha must be exactly 40 lowercase hex characters and unique;
    anything else raises RuntimeError (rc 2). Nothing is silently dropped."""
    try:
        entries = json.loads(text.lstrip(chr(0xFEFF)))["entries"]
    except (ValueError, KeyError, TypeError) as e:
        raise RuntimeError("cannot read %s: %s" % (LEDGER_PATH, e))
    if not isinstance(entries, list):
        raise RuntimeError("%s: entries is not a list" % LEDGER_PATH)
    seen = set()
    for i, e in enumerate(entries):
        if not isinstance(e, dict):
            raise RuntimeError("%s: entry %d is not an object" % (LEDGER_PATH, i))
        sha = e.get("sha")
        if not isinstance(sha, str) or not LEDGER_SHA_RX.match(sha):
            raise RuntimeError("%s: entry %d sha %r is not 40 lowercase hex characters" % (LEDGER_PATH, i, sha))
        if sha in seen:
            raise RuntimeError("%s: duplicate sha %s" % (LEDGER_PATH, sha))
        seen.add(sha)
        rec = e.get("gate_record")
        if rec is not None and not isinstance(rec, str):
            raise RuntimeError("%s: entry %d gate_record is not a string or null" % (LEDGER_PATH, i))
    return entries


def tip_of(rng):
    """The commit a range ends at (the right side of A..B; HEAD when that side is empty)."""
    tip = rng.split("..")[-1] if ".." in rng else rng
    return tip or "HEAD"


def load_ledger(root, tip="HEAD", path=None):
    """Ledger entries. By default read from the committed ledger at the range TIP (git show <tip>:docs/audit/waiver-ledger.json),
    never from the working tree, so an uncommitted edit waives nothing. [] when that commit has no ledger. path: an explicit
    file (non-strict runs only; main refuses it under --strict)."""
    if path:
        try:
            with open(path, encoding="utf-8-sig") as fh:
                return parse_ledger(fh.read())
        except OSError as e:
            raise RuntimeError("cannot read ledger %s: %s" % (path, e))
    try:
        git(root, "cat-file", "-e", "%s:%s" % (tip, LEDGER_PATH))
    except RuntimeError:
        return []
    return parse_ledger(git(root, "show", "%s:%s" % (tip, LEDGER_PATH)))


def ledger_entry(ledger, sha):
    for e in ledger:
        if sha == e["sha"]:
            return e
    return None


def gate_check(gate_root, record, sha):
    """(ok, why). The record must stay inside gate_root, be a non-empty file, and hold a verdict line (GRANT or
    GRANT-WITH-CONDITIONS, and no negative word such as DENY, DENIED, REFUSE, HOLD, PENDING, WITHDRAWN, NO-GRANT or NOT GRANTED, case-insensitive) that names at least the first 9 characters of sha. An existing file alone waives nothing."""
    if not record:
        return False, "no gate_record"
    parts = re.split(r"[\\/]+", record)
    if os.path.isabs(record) or record[:1] in "/\\" or re.match(r"^[A-Za-z]:", record) or ".." in parts:
        return False, "gate_record escapes the gate root"
    root_real = os.path.realpath(gate_root)
    full = os.path.realpath(os.path.join(root_real, record))
    try:
        inside = os.path.commonpath([root_real, full]) == root_real
    except ValueError:
        inside = False
    if not inside:
        return False, "gate_record escapes the gate root"
    if not os.path.isfile(full):
        return False, "gate file not found"
    try:
        with open(full, "rb") as fh:
            text = fh.read(4 * 1024 * 1024).decode("utf-8", "replace")
    except OSError as e:
        return False, "gate file unreadable: %s" % e
    if not text.strip():
        return False, "gate file is empty"
    key = sha[:9].lower()
    for line in text.splitlines():
        low = line.lower()
        if key in low and VERDICT_RX.search(line) and not NEGATIVE_RX.search(line):
            return True, ""
    return False, "gate file has no GRANT verdict line naming %s" % key


def scan(root, rng, ledger=None, gate_root=None):
    ledger = ledger or []
    gate_root = gate_root or os.environ.get("VP_GATE_ROOT") or DEFAULT_GATE_ROOT
    results = []
    shas = git(root, "rev-list", "--no-merges", "--reverse", rng).split()
    msgs = {sha: git(root, "show", "-s", "--format=%B", sha) for sha in shas}
    late = late_waivers(msgs.values())
    for sha in shas:
        msg = msgs[sha]
        if any(sha.startswith(w) for w in late) and not has_waiver(msg):
            msg += "\nWAIVER-LATE: waived by a later 'Waives:' commit"
        viol, notes, waived = judge(commit_files(root, sha), msg)
        subject = msg.strip().splitlines()[0][:80] if msg.strip() else ""
        ledger_waived = False
        if viol:
            ent = ledger_entry(ledger, sha)
            if ent is not None:
                ok, why = gate_check(gate_root, ent.get("gate_record"), sha)
                if ok:
                    notes.append("ledger-waived %s: gate record %s" % (ent.get("waiver_id", "?"), ent["gate_record"]))
                    viol, waived, ledger_waived = [], True, True
                else:
                    notes.append("ledger entry %s has MISSING gate record %r (%s): still flagged" % (ent.get("waiver_id", "?"), ent.get("gate_record"), why))
        results.append({"sha": sha[:9], "subject": subject, "violations": viol, "notes": notes, "waived": waived,
                        "ledger_waived": ledger_waived})
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description="Flag commits that touch tests/checkers/baselines/allow-lists without a WAIVER trailer.")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--range")
    ap.add_argument("--base")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--gate-root", default=None, help="folder that holds the ledger's gate_record files (default VP_GATE_ROOT or %s)" % DEFAULT_GATE_ROOT)
    ap.add_argument("--ledger", default=None, help="ledger file to use instead of <root>/%s" % LEDGER_PATH)
    a = ap.parse_args(argv)
    if a.selftest:
        f = [{"path": "tools/check_x.py", "status": "M", "added": ["a"], "removed": []}]
        v, _, _ = judge(f, "x")
        v2, _, w2 = judge(f, "t\n\nWAIVER-T1: ok\n")
        py_weaken = [{"path": "tools/looprig/test_looprig.py", "status": "M",
                      "added": ["self.assertNotEqual(verdict, 'FAIL')"],
                      "removed": ["self.assertEqual(verdict, 'PASS')"]}]
        vp, _, _ = judge(py_weaken, "weaken py test")
        vp_w, _, wp_w = judge(py_weaken, "weaken py test\n\nWAIVER-T2: ok\n")
        py_add = [{"path": "tools/eval/test_light_eval.py", "status": "M",
                   "added": ["self.assertTrue(r['mid']['value_ok'])"], "removed": []}]
        va, na, _ = judge(py_add, "add py assert")
        st_weaken = [{"path": "tools/eval/source_id/score_identity.py", "status": "M",
                      "added": ["pass"], "removed": ["assert got == want"]}]
        vs, _, _ = judge(st_weaken, "weaken selftest")
        vs_w, _, ws_w = judge(st_weaken, "weaken selftest\n\nWAIVER-T3: ok\n")
        pw_edit = [{"path": "tools/vpv_tests/pairwise.py", "status": "M", "added": ["x = 1"], "removed": []}]
        vpw, _, _ = judge(pw_edit, "edit pairwise")
        ok = (bool(v) and not v2 and w2
              and bool(vp) and vp[0][0] == "test" and not vp_w and wp_w
              and not va and bool(na)
              and bool(vs) and vs[0][0] == "selftest" and not vs_w and ws_w
              and bool(vpw) and vpw[0][0] == "checker")
        print("selftest " + ("ok" if ok else "FAIL"))
        return 0 if ok else 2
    try:
        rng = a.range or default_range(a.root, a.base)
        if a.ledger:
            if a.strict:
                raise RuntimeError("--ledger is refused under --strict: the ledger is read from the range tip commit")
            print("ledger: %s (NOT the committed ledger)" % a.ledger)
        res = scan(a.root, rng, load_ledger(a.root, tip_of(rng), a.ledger), a.gate_root)
    except RuntimeError as e:
        print("check_test_tamper: error: %s" % e)
        return 2
    bad = [r for r in res if r["violations"]]
    if a.json:
        print(json.dumps({"range": rng, "commits": len(res), "flagged": len(bad), "results": res}, indent=1))
    else:
        nled = sum(1 for r in res if r["ledger_waived"])
        print("test tamper: range %s, %d commit(s), %d flagged, %d waived (%d by trailer), ledger-waived %d" % (
            rng, len(res), len(bad), sum(1 for r in res if r["waived"]), sum(1 for r in res if r["waived"] and not r["ledger_waived"]), nled))
        for r in res:
            for n in r["notes"]:
                print("  note %s %s" % (r["sha"], n))
            for cls, path, why in r["violations"]:
                print("  FLAG %s %s [%s] %s: %s" % (r["sha"], r["subject"], cls, path, why))
        if bad:
            print("FAIL: add a WAIVER-<ID>: <why> trailer (separately gated) or revert the change (owner rule 19).")
    return 1 if (bad and a.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
