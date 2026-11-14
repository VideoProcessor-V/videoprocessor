# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
# Fixture bytes are built with bytes([...]) so no control byte ever sits in this source file.
import importlib.util
import os
import subprocess
import tempfile
import unittest

_d = os.path.dirname(os.path.abspath(__file__))
_s = importlib.util.spec_from_file_location("ctlscan", os.path.join(_d, "ctlscan.py"))
CS = importlib.util.module_from_spec(_s)
_s.loader.exec_module(CS)
_s2 = importlib.util.spec_from_file_location("run_compliance", os.path.join(_d, "run_compliance.py"))
RC = importlib.util.module_from_spec(_s2)
_s2.loader.exec_module(RC)

BAD = {"nul.h": bytes([0]), "vt.h": bytes([11]), "bs.h": bytes([8]), "cr.h": bytes([13])}


def _body(c):
    return b"line1" + c + b"line2" + bytes([13, 10])


def _git(r, *a):
    subprocess.run(["git", "-C", r, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.autocrlf=false"] + list(a),
                   check=True, capture_output=True)


class CtlscanTests(unittest.TestCase):
    def test_each_bad_byte_fails_and_clean_passes(self):
        with tempfile.TemporaryDirectory() as d:
            for n, c in BAD.items():
                p = os.path.join(d, n)
                open(p, "wb").write(_body(c))
                self.assertEqual(CS.main([p]), 1, n)
            p = os.path.join(d, "clean.h")
            open(p, "wb").write(b"\ta\tb" + bytes([13, 10]) + b"\tc" + bytes([13, 10]) + b"tail\n")
            self.assertEqual(CS.main([p]), 0)

    def test_since_scans_exactly_changed_text_files(self):
        with tempfile.TemporaryDirectory() as r:
            _git(r, "init", "-q")
            open(os.path.join(r, "old.h"), "wb").write(_body(bytes([8])))  # pre-existing, outside the diff
            open(os.path.join(r, "a.h"), "wb").write(b"ok\n")
            _git(r, "add", "-A")
            _git(r, "commit", "-qm", "base")
            _git(r, "tag", "base")
            self.assertEqual(CS.main(["--root", r, "--since", "base"]), 0)  # empty diff
            open(os.path.join(r, "a.h"), "wb").write(_body(bytes([11])))
            open(os.path.join(r, "blob.dat"), "wb").write(bytes([0, 1, 2, 3]) * 50)  # binary: skipped
            _git(r, "add", "-A")
            _git(r, "commit", "-qm", "change")
            self.assertEqual(CS.main(["--root", r, "--since", "base"]), 1)
            open(os.path.join(r, "a.h"), "wb").write(b"fixed\n")
            _git(r, "add", "-A")
            _git(r, "commit", "-qm", "fix")
            self.assertEqual(CS.main(["--root", r, "--since", "base"]), 0)  # old.h not in diff, blob.dat binary

    def test_bad_base_is_error(self):
        with tempfile.TemporaryDirectory() as r:
            _git(r, "init", "-q")
            self.assertEqual(CS.main(["--root", r, "--since", "nope"]), 2)

    def test_selftest_and_compliance_wiring(self):
        self.assertEqual(CS.main(["--selftest"]), 0)
        self.assertTrue(any(t[1] == "ctlscan.py" and t[3] for t in RC.TOOLS))

    def test_this_file_and_the_checker_are_clean(self):
        self.assertEqual(CS.main([os.path.join(_d, "ctlscan.py"), os.path.join(_d, "test_ctlscan.py")]), 0)


if __name__ == "__main__":
    unittest.main()
