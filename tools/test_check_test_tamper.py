# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import tempfile
import unittest

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_test_tamper.py")
_s = importlib.util.spec_from_file_location("check_test_tamper", _p)
T = importlib.util.module_from_spec(_s)
_s.loader.exec_module(T)


def F(path, status="M", add=(), rem=()):
    return {"path": path, "status": status, "added": list(add), "removed": list(rem)}


class JudgeTests(unittest.TestCase):
    def test_edited_assertion_flags(self):
        v, _, _ = T.judge([F("src/VP-V/tests/FooTests.cpp", add=["CHECK(2)"], rem=["CHECK(1)"])], "m")
        self.assertEqual(v[0][0], "test")

    def test_add_only_test_passes_with_note(self):
        v, n, _ = T.judge([F("src/VP-V/tests/FooTests.cpp", add=["TEST(x){}"])], "m")
        self.assertEqual(v, [])
        self.assertTrue(n)

    def test_new_test_file_passes(self):
        v, _, _ = T.judge([F("src/VP-V/tests/NewTests.cpp", "A", add=["a"])], "m")
        self.assertEqual(v, [])

    def test_deleted_test_flags(self):
        v, _, _ = T.judge([F("src/VP-V/tests/FooTests.cpp", "D", rem=["a"])], "m")
        self.assertTrue(v)

    def test_checker_needs_waiver(self):
        f = [F("tools/check_foo.py", add=["x"])]
        self.assertTrue(T.judge(f, "m")[0])
        v, _, w = T.judge(f, "m\n\nWAIVER-ABC_1: reason")
        self.assertEqual(v, [])
        self.assertTrue(w)

    def test_baseline_shrink_ok_growth_flags(self):
        self.assertEqual(T.judge([F("tools/x-baseline.json", rem=["1"])], "m")[0], [])
        self.assertTrue(T.judge([F("tools/x-baseline.json", add=["2"], rem=["1"])], "m")[0])

    def test_allowlist_line_in_other_file(self):
        self.assertTrue(T.judge([F("src/a.cpp", add=["// ALLOW_SITES += 1"])], "m")[0])
        self.assertEqual(T.judge([F("src/a.cpp", add=["int x;"])], "m")[0], [])

    def test_product_rows_folder_change_is_a_rule_19_change(self):
        row = '        { kP10GrainSplit, "repair/grain_split", kExpectedGrainSplitUnit, "grain split pass" },'
        moved = '        { kP10GrainSplit, "repair/elsewhere", kExpectedGrainSplitUnit, "grain split pass" },'
        cat = "src/VP-V/core/ProductCatalog.h"
        v, _, _ = T.judge([F(cat, add=[moved], rem=[row])], "m")
        self.assertEqual(v[0][0], "register_owner")
        v, _, _ = T.judge([F(cat, add=[moved])], "m")
        self.assertEqual(v[0][0], "register_owner")
        v, _, w = T.judge([F(cat, add=[moved], rem=[row])], "m" + chr(10) + chr(10) + "WAIVER-X: r")
        self.assertEqual((v, w), ([], True))
        v, _, _ = T.judge([F(cat, add=["inline int Other();"])], "m")
        self.assertEqual(v, [])
        v, _, _ = T.judge([F("src/VP-V/core/Other.h", add=[moved])], "m")
        self.assertEqual(v, [])

    def test_waiver_needs_line_start(self):
        self.assertFalse(T.has_waiver("fix mentions WAIVER-X inline"))
        self.assertTrue(T.has_waiver("a\nWAIVER-X"))


class GitTests(unittest.TestCase):
    def _g(self, root, *a):
        subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t"] + list(a),
                       check=True, capture_output=True)

    def test_range_scan(self):
        with tempfile.TemporaryDirectory() as r:
            self._g(r, "init", "-q")
            os.makedirs(os.path.join(r, "tools"))
            p = os.path.join(r, "tools", "check_a.py")
            open(p, "w").write("x=1\n")
            self._g(r, "add", "-A")
            self._g(r, "commit", "-qm", "base")
            base = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            open(p, "w").write("x=2\n")
            self._g(r, "commit", "-qam", "bad")
            open(p, "w").write("x=3\n")
            self._g(r, "commit", "-qam", "ok\n\nWAIVER-T9: why")
            res = T.scan(r, base + "..HEAD")
            self.assertEqual([bool(x["violations"]) for x in res], [True, False])
            self.assertEqual(T.main(["--root", r, "--range", base + "..HEAD", "--strict"]), 1)
            self.assertEqual(T.main(["--root", r, "--range", "nonsense..HEAD", "--strict"]), 2)

    def test_follow_up_waives_an_earlier_commit_without_history_rewrite(self):
        with tempfile.TemporaryDirectory() as r:
            self._g(r, "init", "-q")
            os.makedirs(os.path.join(r, "tools"))
            p = os.path.join(r, "tools", "check_a.py")
            open(p, "w").write("x=1\n")
            self._g(r, "add", "-A")
            self._g(r, "commit", "-qm", "base")
            base = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            open(p, "w").write("x=2\n")
            self._g(r, "commit", "-qam", "bad")
            bad = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            rng = base + "..HEAD"
            self.assertEqual(T.main(["--root", r, "--range", rng, "--strict"]), 1)
            self._g(r, "commit", "--allow-empty", "-qm", "late waiver\n\nWAIVER-T9: why\nWaives: " + bad[:9])
            self.assertEqual(T.main(["--root", r, "--range", rng, "--strict"]), 0)
            # a Waives: line without a WAIVER trailer, or naming another commit, waives nothing
            self._g(r, "commit", "--allow-empty", "-qm", "noise\n\nWaives: " + bad[:9])
            self.assertEqual(T.main(["--root", r, "--range", rng, "--strict"]), 0)
            self.assertEqual(T.late_waivers(["x\nWaives: abcdef1"]), set())

    def _bad_repo(self, r):
        self._g(r, "init", "-q")
        os.makedirs(os.path.join(r, "tools"))
        p = os.path.join(r, "tools", "check_a.py")
        open(p, "w").write("x=1\n")
        self._g(r, "add", "-A")
        self._g(r, "commit", "-qm", "base")
        base = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        open(p, "w").write("x=2\n")
        self._g(r, "commit", "-qam", "bad one")
        bad = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        return base + "..HEAD", bad

    def _write_ledger(self, r, entries, commit=True):
        """Write the ledger; commit it (with a trailer, so the ledger commit itself is not flagged) unless commit=False."""
        os.makedirs(os.path.join(r, "docs", "audit"), exist_ok=True)
        with open(os.path.join(r, "docs", "audit", "waiver-ledger.json"), "w") as fh:
            json.dump({"entries": entries}, fh)
        if commit:
            self._g(r, "add", "-A")
            self._g(r, "commit", "-qm", "ledger\n\nWAIVER-T-LEDGER: test ledger commit")

    def _grant(self, gate, rel, sha, verdict="GRANT"):
        os.makedirs(os.path.dirname(os.path.join(gate, rel)), exist_ok=True)
        with open(os.path.join(gate, rel), "w") as fh:
            fh.write("sha: %s verdict: %s\n" % (sha, verdict))

    def _run(self, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = T.main(argv)
        return rc, buf.getvalue()

    def test_ledger_waives_only_with_an_existing_gate_record(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            self.assertEqual(self._run(argv)[0], 1)  # no ledger at all
            entry = {"sha": bad, "waiver_id": "WAIVER-T-LEDGER", "gate_record": "lead1/gate.md"}
            self._write_ledger(r, [entry])
            rc, out = self._run(argv)
            self.assertEqual(rc, 1)  # listed, gate record file absent: still flagged
            self.assertIn("MISSING gate record", out)
            self.assertIn("ledger-waived 0", out)
            self._grant(gate, "lead1/gate.md", bad, "GRANT-WITH-CONDITIONS")
            rc, out = self._run(argv)
            self.assertEqual(rc, 0)
            self.assertIn("ledger-waived 1", out)
            self.assertIn("(1 by trailer)", out)  # the ledger commit itself carries a trailer
            self.assertEqual(self._run(["--root", r, "--range", rng, "--gate-root", gate, "--json"])[0], 0)

    def test_ledger_entry_without_gate_record_or_for_another_sha_waives_nothing(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            self._grant(gate, "g.md", bad)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": None}])
            self.assertEqual(self._run(argv)[0], 1)
            self._write_ledger(r, [{"sha": "0" * 40, "waiver_id": "W", "gate_record": "g.md"}])
            self.assertEqual(self._run(argv)[0], 1)

    def test_m1_ledger_is_read_from_the_tip_commit_not_the_working_tree(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            self._grant(gate, "g.md", bad)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            # an uncommitted ledger (never passed the baseline guard) waives nothing
            self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": "g.md"}], commit=False)
            rc, out = self._run(argv)
            self.assertEqual(rc, 1)
            self.assertIn("ledger-waived 0", out)
            # --ledger FILE is refused under --strict (rc 2) and flagged as not committed otherwise
            led = os.path.join(r, "docs", "audit", "waiver-ledger.json")
            self.assertEqual(self._run(argv + ["--ledger", led])[0], 2)
            rc, out = self._run(["--root", r, "--range", rng, "--gate-root", gate, "--ledger", led])
            self.assertIn("NOT the committed ledger", out)

    def test_m2_sha_must_be_forty_lowercase_hex_and_unique(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            self._grant(gate, "g.md", bad)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            for sha in (bad[:7], bad[:12], bad.upper(), 1234567, None, ""):
                self._write_ledger(r, [{"sha": sha, "waiver_id": "W", "gate_record": "g.md"}])
                self.assertEqual(self._run(argv)[0], 2, repr(sha))  # a prefix no longer waives; the entry is an error
            good = {"sha": bad, "waiver_id": "W", "gate_record": "g.md"}
            self._write_ledger(r, [good, dict(good)])
            self.assertEqual(self._run(argv)[0], 2)  # duplicate
            self._write_ledger(r, ["not an object"])
            self.assertEqual(self._run(argv)[0], 2)
            self._write_ledger(r, [good])
            self.assertEqual(self._run(argv)[0], 0)

    def test_m3_gate_record_must_stay_inside_the_gate_root(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as base:
            rng, bad = self._bad_repo(r)
            gate = os.path.join(base, "gate")
            os.makedirs(gate)
            self._grant(base, "outside.md", bad)  # a valid verdict file, one level above the gate root
            self._grant(gate, "sub/in.md", bad)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            outside = os.path.join(base, "outside.md")
            for rec in ("../outside.md", "sub/../../outside.md", outside, outside.replace(os.sep, "/"), "/outside.md"):
                self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": rec}])
                rc, out = self._run(argv)
                self.assertEqual(rc, 1, rec)
                self.assertIn("escapes the gate root", out)
            self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": "sub/in.md"}])
            self.assertEqual(self._run(argv)[0], 0)

    def test_m4_empty_gate_file_waives_nothing(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": "g.md"}])
            for body in ("", "  \n\t\n"):
                open(os.path.join(gate, "g.md"), "w").write(body)
                rc, out = self._run(argv)
                self.assertEqual(rc, 1)
                self.assertIn("gate file is empty", out)

    def test_m5_gate_file_must_hold_a_grant_verdict_line_naming_the_sha(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": "g.md"}])
            g = os.path.join(gate, "g.md")
            cases = [
                ("some text\nGRANT\n", 1),  # exists, non-empty, but does not name the sha
                ("sha: %s verdict: REFUSE\n" % bad, 1),  # names it, wrong verdict
                ("%s notes only\nGRANT elsewhere\n" % bad, 1),  # sha and GRANT on different lines
                ("sha: %s verdict: GRANT-WITH-CONDITIONS REFUSE\n" % bad, 1),
                ("sha: %s verdict: NOGRANT\n" % bad, 1),  # word boundary
                ("sha: %s verdict: GRANT\n" % bad[:8], 1),  # fewer than 9 characters
                ("| %s | row | GRANT |\n" % bad[:9], 0),  # 9 characters is enough
                ("sha: %s verdict: GRANT-WITH-CONDITIONS\n" % bad, 0),
            ]
            for body, rc_want in cases:
                open(g, "w").write(body)
                rc, out = self._run(argv)
                self.assertEqual(rc, rc_want, body)
                if rc_want:
                    self.assertIn("MISSING gate record", out)

    def test_trailer_waiver_is_counted_apart_from_ledger_waiver(self):
        with tempfile.TemporaryDirectory() as r:
            rng, _ = self._bad_repo(r)
            p = os.path.join(r, "tools", "check_a.py")
            open(p, "w").write("x=3\n")
            self._g(r, "commit", "-qam", "ok\n\nWAIVER-T9: why")
            rc, out = self._run(["--root", r, "--range", rng, "--strict"])
            self.assertEqual(rc, 1)
            self.assertIn("1 waived (1 by trailer), ledger-waived 0", out)

    def test_unreadable_ledger_is_an_error_and_the_ledger_file_is_guarded(self):
        with tempfile.TemporaryDirectory() as r:
            rng, _ = self._bad_repo(r)
            os.makedirs(os.path.join(r, "docs", "audit"))
            open(os.path.join(r, "docs", "audit", "waiver-ledger.json"), "w").write("{not json")
            self._g(r, "add", "-A")
            self._g(r, "commit", "-qm", "bad ledger\n\nWAIVER-T-L: x")
            self.assertEqual(self._run(["--root", r, "--range", rng, "--strict"])[0], 2)
        v, _, _ = T.judge([F("docs/audit/waiver-ledger.json", add=["x"])], "m")
        self.assertEqual(v[0][0], "baseline")
        v, _, _ = T.judge([F("tools/test_check_test_tamper.py", add=["x"])], "m")
        self.assertEqual(v[0][0], "checker")

    def test_selftest(self):
        self.assertEqual(T.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
