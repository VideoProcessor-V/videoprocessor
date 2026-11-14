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

    def _write_ledger(self, r, entries):
        os.makedirs(os.path.join(r, "docs", "audit"), exist_ok=True)
        with open(os.path.join(r, "docs", "audit", "waiver-ledger.json"), "w") as fh:
            json.dump({"entries": entries}, fh)

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
            os.makedirs(os.path.join(gate, "lead1"))
            open(os.path.join(gate, "lead1", "gate.md"), "w").write("GRANT\n")
            rc, out = self._run(argv)
            self.assertEqual(rc, 0)
            self.assertIn("ledger-waived 1", out)
            self.assertIn("0 flagged", out)

    def test_ledger_entry_without_gate_record_or_for_another_sha_waives_nothing(self):
        with tempfile.TemporaryDirectory() as r, tempfile.TemporaryDirectory() as gate:
            rng, bad = self._bad_repo(r)
            open(os.path.join(gate, "g.md"), "w").write("GRANT\n")
            argv = ["--root", r, "--range", rng, "--strict", "--gate-root", gate]
            self._write_ledger(r, [{"sha": bad, "waiver_id": "W", "gate_record": None}])
            self.assertEqual(self._run(argv)[0], 1)
            self._write_ledger(r, [{"sha": "0" * 40, "waiver_id": "W", "gate_record": "g.md"}])
            self.assertEqual(self._run(argv)[0], 1)
            self._write_ledger(r, [{"sha": bad[:7], "waiver_id": "W", "gate_record": "g.md"}])
            self.assertEqual(self._run(argv)[0], 0)  # a 7-character prefix of the sha matches

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
            self.assertEqual(self._run(["--root", r, "--range", rng, "--strict"])[0], 2)
        v, _, _ = T.judge([F("docs/audit/waiver-ledger.json", add=["x"])], "m")
        self.assertEqual(v[0][0], "baseline")
        v, _, _ = T.judge([F("tools/test_check_test_tamper.py", add=["x"])], "m")
        self.assertEqual(v[0][0], "checker")

    def test_selftest(self):
        self.assertEqual(T.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
