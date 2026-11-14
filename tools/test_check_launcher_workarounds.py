# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import io
import json
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout

_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_launcher_workarounds.py")
_s = importlib.util.spec_from_file_location("check_launcher_workarounds", _p)
N6 = importlib.util.module_from_spec(_s)
_s.loader.exec_module(N6)


def _g(root, *a):
    subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t"] + list(a), check=True, capture_output=True)


def _w(root, rel, text):
    full = os.path.join(root, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", newline="\n") as fh:
        fh.write(text)


def _run(root, *args):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = N6.main(["--root", root] + list(args))
    return rc, buf.getvalue()


class PatternTests(unittest.TestCase):
    def test_each_idiom(self):
        c = N6.classify_line
        self.assertEqual(c("a.ps1", "powershell -ExecutionPolicy Unrestricted"), ["exec-policy"])
        self.assertEqual(c("a.ps1", "Set-ExecutionPolicy RemoteSigned"), ["exec-policy"])
        self.assertEqual(c("a.ps1", "Unblock-File x.dll"), ["unblock"])
        self.assertEqual(c("a.ps1", "powershell -EncodedCommand AAA"), ["encoded"])
        self.assertEqual(c("a.ps1", "Start-Process powershell -Verb RunAs"), ["nested-ps"])
        self.assertEqual(c("a.cmd", "cmd /c powershell x"), ["nested-ps"])
        self.assertEqual(c("a.bat", "taskkill /F /IM vp.exe"), ["kill-by-name"])
        self.assertEqual(c("a.ps1", "Stop-Process -Name vp"), ["kill-by-name"])
        self.assertEqual(c("a.sh", "git commit --no-verify"), ["no-verify"])
        self.assertEqual(c("w.cmd", "call run.ps1"), ["cmd-wrapper"])
        self.assertEqual(c("w.cmd", "rem call run.ps1"), [])
        self.assertEqual(c("a.py", "x = 1"), [])
        self.assertEqual(c("a.ps1", "Stop-Process -Id $pid"), [])


class TreeTests(unittest.TestCase):
    def _repo(self):
        r = tempfile.mkdtemp()
        _g(r, "init", "-q")
        _w(r, "tools/ok.ps1", "Write-Host hi\n")
        _w(r, "tools/launcher-workarounds-allow.json", json.dumps({"allow": []}))
        _g(r, "add", "-A")
        _g(r, "commit", "-qm", "base")
        return r

    def test_clean_and_new_hit(self):
        r = self._repo()
        self.assertEqual(_run(r)[0], 0)
        _w(r, "tools/bad.ps1", "powershell -ExecutionPolicy Bypass -File x.ps1\n")
        _g(r, "add", "-A")
        rc, out = _run(r)
        self.assertEqual(rc, 1)
        self.assertIn("NEW exec-policy tools/bad.ps1:1", out)

    def test_allow_entry_needs_reason_and_row(self):
        r = self._repo()
        line = "taskkill /IM x.exe"
        _w(r, "tools/k.cmd", line + "\n")
        _g(r, "add", "-A")
        _w(r, "tools/launcher-workarounds-allow.json", json.dumps({"allow": [{"file": "tools/k.cmd", "line": line, "reason": "", "row": ""}]}))
        self.assertEqual(_run(r)[0], 1)
        _w(r, "tools/launcher-workarounds-allow.json", json.dumps({"allow": [{"file": "tools/k.cmd", "line": line, "reason": "why", "row": "R1"}]}))
        self.assertEqual(_run(r)[0], 0)

    def test_since_only_added_lines(self):
        r = self._repo()
        _w(r, "tools/old.ps1", "Unblock-File a\n")
        _g(r, "add", "-A")
        _g(r, "commit", "-qm", "old")
        base = subprocess.run(["git", "-C", r, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        rc, _ = _run(r, "--since", base)
        self.assertEqual(rc, 0)
        _w(r, "tools/old.ps1", "Unblock-File a\nSet-ExecutionPolicy Bypass\n")
        rc, out = _run(r, "--since", base)
        self.assertEqual(rc, 1)
        self.assertEqual(out.count("NEW"), 1)

    def test_missing_allow_list_is_error(self):
        r = self._repo()
        os.remove(os.path.join(r, "tools", "launcher-workarounds-allow.json"))
        self.assertEqual(_run(r)[0], 2)

    def test_selftest(self):
        self.assertEqual(N6.main(["--selftest"]), 0)


if __name__ == "__main__":
    unittest.main()
