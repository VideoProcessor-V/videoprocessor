# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Test / checker / baseline tamper gate (owner rule 19: no metric gaming).

Usage: python -I tools/check_test_tamper.py [--strict] [--range A..B | --base REF] [--root PATH] [--json]

Walks every non-merge commit in a range (default merge-base(HEAD, base ref)..HEAD) and flags each
commit that touches a guarded file class WITHOUT a WAIVER-<ID> trailer line in its message:

  test       src/**/tests/**, *Tests.cpp, *Test.cpp   (a test-ADDING-only change passes with a note:
                                                       only added lines, no removed/edited line)
  checker    tools/check_*.py, tools/run_compliance.py (any change needs a waiver)
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
                 "tools/test_ctlscan.py", "tools/test_check_test_tamper.py"]
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


def selftest_failures():
    """Names of the failed selftest checks (empty when all pass): classify() for every class and glob, judge() for every class
    and verdict (test deleted / edited / added, checker, baseline shrink and growth, register owner, allow-list add and
    remove, waiver trailer), late_waivers(), parse_ledger() rejections and gate_check() on a temp gate root."""
    import shutil
    import tempfile
    failed = []

    def check(name, cond):
        if not cond:
            failed.append(name)
    classes = {
        "tools/check_x.py": "checker", "tools/run_compliance.py": "checker", "tools/compliance_lib.py": "checker",
        "tools/ctlscan.py": "checker", "tools/test_ctlscan.py": "checker", "tools/test_check_test_tamper.py": "checker",
        "tools/x-baseline.json": "baseline", "docs/audit/a-baseline.json": "baseline", "docs/audit/b-budget.json": "baseline",
        "tools/timing-budget.json": "baseline", "docs/audit/waiver-ledger.json": "baseline",
        "src/VP-V/m/tests/FooTests.cpp": "test", "src/VideoProcessor-Test/Bar.cpp": "test", "src/x/BazTest.cpp": "test",
        "src/VP-V/core/X.h": "other", "docs/readme.md": "other"}
    for path, want in classes.items():
        check("classify %s -> %s" % (path, want), classify(path) == want)

    def f(path, status="M", added=(), removed=()):
        return {"path": path, "status": status, "added": list(added), "removed": list(removed)}
    t = "src/VP-V/m/tests/FooTests.cpp"
    check("test deleted is flagged", [v[2] for v in judge([f(t, "D")], "m")[0]] == ["deleted"])
    check("test edited is flagged", [v[0] for v in judge([f(t, added=["a"], removed=["b"])], "m")[0]] == ["test"])
    check("test removal only is flagged", len(judge([f(t, removed=["b"])], "m")[0]) == 1)
    check("test-adding only passes with a note", judge([f(t, added=["a"])], "m")[0] == [] and judge([f(t, added=["a"])], "m")[1])
    check("a new test file passes with a note", judge([f(t, "A", added=["a"])], "m")[0] == [])
    check("checker change is flagged", [v[0] for v in judge([f("tools/check_x.py", added=["a"])], "m")[0]] == ["checker"])
    bl = "tools/x-baseline.json"
    check("baseline growth is flagged", [v[0] for v in judge([f(bl, added=["a"])], "m")[0]] == ["baseline"])
    check("baseline edit is flagged", len(judge([f(bl, added=["a"], removed=["b"])], "m")[0]) == 1)
    check("baseline shrink passes with a note", judge([f(bl, removed=["b"])], "m")[0] == [] and judge([f(bl, removed=["b"])], "m")[1])
    row = '    { kPe, "modules/x", "p" },'
    check("a ProductRows row change is flagged", [v[0] for v in judge([f(PRODUCT_CATALOG, added=[row])], "m")[0]] == ["register_owner"])
    check("a ProductRows row removal is flagged", [v[0] for v in judge([f(PRODUCT_CATALOG, removed=[row])], "m")[0]] == ["register_owner"])
    check("another catalog line is not a register change", judge([f(PRODUCT_CATALOG, added=["// c"])], "m")[0] == [])
    check("an allow-list line added is flagged", [v[0] for v in judge([f("src/x.cpp", added=["const ALLOW_SITES = 1;"])], "m")[0]] == ["allowlist"])
    check("an ALLOW entry added is flagged", len(judge([f("tools/x.json", added=['  "allow_x": 1'])], "m")[0]) == 1)
    check("an allow-list removal passes with a note", judge([f("src/x.cpp", removed=["ALLOW_SITES = 1;"])], "m")[0] == [])
    check("an ordinary change passes", judge([f("src/VP-V/core/X.h", added=["int a;"], removed=["int b;"])], "m") == ([], [], False))
    viol, notes, waived = judge([f("tools/check_x.py", added=["a"])], "t\n\nWAIVER-T1: ok\n")
    check("a WAIVER trailer waives and says so", viol == [] and waived and any("WAIVER present" in n for n in notes))
    check("a waiver on a clean commit is not 'waived'", judge([f("src/VP-V/core/X.h", added=["a"])], "t\n\nWAIVER-T1: ok\n")[2] is False)
    check("a WAIVER mid-line is not a trailer", has_waiver("see WAIVER-T1: not a trailer") is False)
    check("a bare WAIVER id counts", has_waiver("x\nWAIVER-ABC_1.2\n") is True)
    check("late waivers need a WAIVER in the waiving commit",
          late_waivers(["fix\nWaives: abcdef1\nWAIVER-X: y", "no waiver\nWaives: 1234567"]) == {"abcdef1"})
    check("tip_of", tip_of("a..b") == "b" and tip_of("a..") == "HEAD" and tip_of("x") == "x")
    sha = "a" * 40
    good = json.dumps({"entries": [{"sha": sha, "waiver_id": "W", "gate_record": "g.md"}]})
    check("a valid ledger parses", len(parse_ledger(good)) == 1 and len(parse_ledger("\ufeff" + good)) == 1)
    for label, text in (("not json", "{"), ("no entries", "{}"), ("entries not a list", '{"entries": 1}'),
                        ("short sha", '{"entries": [{"sha": "abc"}]}'), ("upper-case sha", json.dumps({"entries": [{"sha": "A" * 40}]})),
                        ("duplicate sha", json.dumps({"entries": [{"sha": sha}, {"sha": sha}]})),
                        ("entry not an object", '{"entries": [1]}'),
                        ("gate_record not a string", json.dumps({"entries": [{"sha": sha, "gate_record": 3}]}))):
        try:
            parse_ledger(text)
            check("ledger rejects: " + label, False)
        except RuntimeError:
            pass
    root = tempfile.mkdtemp()
    try:
        def put(rel, text):
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path) or root, exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
        put("g/ok.md", "verdict: GRANT aaaaaaaaa\n")
        put("g/cond.md", "verdict: GRANT-WITH-CONDITIONS aaaaaaaaa\n")
        put("g/empty.md", "   \n")
        put("g/none.md", "verdict: GRANT bbbbbbbbb\n")
        put("g/deny.md", "verdict: DENIED GRANT aaaaaaaaa\n")
        put("g/nogrant.md", "aaaaaaaaa NO-GRANT\n")
        put("g/novert.md", "aaaaaaaaa looked fine\n")
        check("a GRANT line naming the sha passes", gate_check(root, "g/ok.md", sha)[0])
        check("GRANT-WITH-CONDITIONS passes", gate_check(root, "g/cond.md", sha)[0])
        check("the sha prefix match is case-insensitive", gate_check(root, "g/ok.md", "A" * 40)[0])
        check("a missing record waives nothing", gate_check(root, "", sha)[0] is False and gate_check(root, None, sha)[0] is False)
        check("a missing file waives nothing", gate_check(root, "g/absent.md", sha) == (False, "gate file not found"))
        check("an empty file waives nothing", gate_check(root, "g/empty.md", sha) == (False, "gate file is empty"))
        check("a verdict for another sha waives nothing", gate_check(root, "g/none.md", sha)[0] is False)
        check("a DENIED verdict waives nothing", gate_check(root, "g/deny.md", sha)[0] is False)
        check("a NO-GRANT line waives nothing", gate_check(root, "g/nogrant.md", sha)[0] is False)
        check("a line without GRANT waives nothing", gate_check(root, "g/novert.md", sha)[0] is False)
        for esc in ("../x.md", "g/../../x.md", "/abs.md", "C:/abs.md", "\\abs.md"):
            check("gate_record escape refused: " + esc, gate_check(root, esc, sha) == (False, "gate_record escapes the gate root"))
    finally:
        shutil.rmtree(root, True)
    return failed


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
        failed = selftest_failures()
        for name in failed:
            print("selftest FAILED check: " + name)
        print("selftest " + ("ok" if not failed else "FAIL"))
        return 0 if not failed else 2
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
