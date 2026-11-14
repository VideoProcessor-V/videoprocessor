# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
# Fixture bytes are built with bytes([...]) so no control byte ever sits in this source file.
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

_d = os.path.dirname(os.path.abspath(__file__))
_s = importlib.util.spec_from_file_location("ctlscan", os.path.join(_d, "ctlscan.py"))
CS = importlib.util.module_from_spec(_s)
_s.loader.exec_module(CS)
_s2 = importlib.util.spec_from_file_location("run_compliance", os.path.join(_d, "run_compliance.py"))
RC = importlib.util.module_from_spec(_s2)
_s2.loader.exec_module(RC)

LF = bytes([10])
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

    def test_early_nul_cpp_is_not_skipped_as_binary(self):
        # git counts a .cpp with an early NUL as binary (numstat '-'); the scanner must still FAIL it (R14-86 MUST 1).
        with tempfile.TemporaryDirectory() as r:
            _git(r, "init", "-q")
            open(os.path.join(r, "a.h"), "wb").write(b"ok" + LF)
            _git(r, "add", "-A")
            _git(r, "commit", "-qm", "base")
            _git(r, "tag", "base")
            for n in ("x.cpp", "y.inl", "z.patch", "noext"):
                open(os.path.join(r, n), "wb").write(b"a" + bytes([0]) + b"b" + LF + (b"text" + LF) * 20)
            _git(r, "add", "-A")
            _git(r, "commit", "-qm", "change")
            self.assertEqual(CS.main(["--root", r, "--since", "base"]), 1)
            self.assertEqual(CS.main(["--root", r]), 1)
            names = sorted(os.path.basename(f) for f in CS.changed_files(r, "base"))
            self.assertEqual(names, ["noext", "x.cpp", "y.inl", "z.patch"])

    def test_extensionless_binary_is_sniffed_out(self):
        with tempfile.TemporaryDirectory() as r:
            p = os.path.join(r, "blob")
            open(p, "wb").write(bytes(range(256)) * 8)
            self.assertTrue(CS.looks_binary(p))
            open(p, "wb").write(b"hello" + bytes([8]) + b"world" + LF * 50)
            self.assertFalse(CS.looks_binary(p))

    def test_non_ascii_near_a_hit_keeps_the_hit_list_and_exit_code(self):
        with tempfile.TemporaryDirectory() as r:
            p = os.path.join(r, "u.md")
            text = "caf" + chr(233) + " " + chr(0x2192) + " ok"
            open(p, "wb").write(text.encode("utf-8") + bytes([11]) + (chr(252) + chr(0x4e2d)).encode("utf-8") + LF)
            env = dict(os.environ, PYTHONIOENCODING="cp1252")
            res = subprocess.run([sys.executable, "-I", os.path.join(_d, "ctlscan.py"), p], capture_output=True, env=env)
            self.assertEqual(res.returncode, 1)
            self.assertIn(b"ctlscan: 1 hit(s)", res.stdout)
            self.assertIn(b"x0b", res.stdout)

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
