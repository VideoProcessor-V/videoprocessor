# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""--write-budget keeps the hand-written `temporary` blocks of hook-budget.json (lead 12, ConfigEditorWindow false +1)."""
import json
import os
import unittest

import test_check_hook_gating as base

chg = base.chg
BILL = base.BILL
TWO = "\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tint other = 1;   // VPV-HOOK\n"


class PreserveTemporary(base.CheckHookGatingTest):
    def _budget(self):
        return json.load(open(os.path.join(self.root, *chg.BUDGET_PATH.split("/")), encoding="utf-8"))

    def _count(self):
        base._w(self.root, BILL, base._bill(TWO))
        rc, out = base._run(self.root, extra=["--write-budget"])
        self.assertEqual(rc, 0, out)
        return self._budget()["files"][BILL]

    def _prime(self, reverts_to, limit):
        base._w(self.root, BILL, base._bill(TWO))
        base._w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: reverts_to}, "temporary": {
            BILL: {"limit": limit, "reverts_to": reverts_to, "until": ["re-graft"], "note": "x"}}}))
        rc, out = base._run(self.root, extra=["--write-budget"])
        self.assertEqual(rc, 0, out)

    def test_pending_block_is_kept_and_files_holds_reverts_to(self):
        n = self._count()
        self.assertGreaterEqual(n, 2)
        self._prime(n - 1, n)
        d = self._budget()
        self.assertEqual(d["files"][BILL], n - 1, "files must not bake the temporary +1")
        self.assertEqual(d["temporary"][BILL]["limit"], n)
        self.assertEqual(d["temporary"][BILL]["note"], "x")

    def test_block_not_needed_any_more_is_dropped(self):
        n = self._count()
        self._prime(n, n + 1)  # count == reverts_to: the allowance is unused
        d = self._budget()
        self.assertEqual(d["files"][BILL], n)
        self.assertNotIn("temporary", d)

    def test_count_above_the_temporary_limit_still_fails(self):
        n = self._count()
        self.assertGreaterEqual(n, 2)
        self._prime(n - 2, n - 1)  # --write-budget itself (rc 0 asserted in _prime) must keep the block
        d = self._budget()
        self.assertEqual(d["temporary"][BILL]["limit"], n - 1)
        self.assertEqual(d["files"][BILL], n - 2)
        rc, out = base._run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("temporary budget", out)


# only the two new tests, not the inherited suite
def load_tests(loader, tests, pattern):
    s = unittest.TestSuite()
    s.addTests(loader.loadTestsFromNames(["test_check_hook_gating_preserve.PreserveTemporary.test_pending_block_is_kept_and_files_holds_reverts_to",
                                          "test_check_hook_gating_preserve.PreserveTemporary.test_block_not_needed_any_more_is_dropped",
                                          "test_check_hook_gating_preserve.PreserveTemporary.test_count_above_the_temporary_limit_still_fails"]))
    return s


if __name__ == "__main__":
    unittest.main()
