# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "check_shared_units", os.path.join(HERE, "check_shared_units.py"))
csu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(csu)


def _w(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


class CheckSharedUnitsTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, "src/VP-V/modules/repair/grain_split/Own.h",
           "void a();\ngaussian_blur(x);\n")
        _w(self.root, "src/VP-V/modules/repair/other/Dup.h",
           "int x;\n\ngaussian_blur(y);\nauto h = half_luma;\n")
        _w(self.root, "src/VP-V/modules/repair/tests/T.cpp",
           "gaussian_blur(z);\n")

    def tearDown(self):
        self.td.cleanup()

    def test_scan(self):
        r = csu.scan(self.root)
        g = next(c for c in r["computations"] if c["name"] == "gaussian_band_split")
        self.assertEqual([(h["file"], h["line"]) for h in g["hits"]],
                         [("src/VP-V/modules/repair/other/Dup.h", 3)])
        self.assertEqual(g["inside_owner"], 1)
        h = next(c for c in r["computations"] if c["name"] == "half_luma_downsample")
        self.assertEqual(h["hits"][0]["line"], 4)
        files = {x["file"] for c in r["computations"] for x in c["hits"]}
        self.assertFalse(any("/tests/" in f for f in files))
        self.assertFalse(any("grain_split" in f for f in files))

    def test_main_exit_and_json(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(csu.main(["--root", self.root]), 0)
        self.assertIn("non-blocking", buf.getvalue())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(csu.main(["--json", "--root", self.root]), 0)
        d = json.loads(buf.getvalue())
        self.assertEqual(d["total_outside"], 2)


if __name__ == "__main__":
    unittest.main()
