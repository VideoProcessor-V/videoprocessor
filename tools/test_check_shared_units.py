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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location(
    "check_shared_units", os.path.join(HERE, "check_shared_units.py"))
csu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(csu)


def _w(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def _run(argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = csu.main(argv)
    return rc, buf.getvalue()


def _unit(result, uid):
    return next(c for c in result["computations"] if c["id"] == uid)


class CheckSharedUnitsTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, "src/VP-V/modules/repair/grain_split/Own.h",
           "void a();\ngaussian_blur(x);\n")
        _w(self.root, "src/VP-V/modules/repair/grain_split/shared/BandFromBase.h",
           "gaussian_blur(shared);\n")
        _w(self.root, "src/VP-V/modules/repair/other/Dup.h",
           "int x;\n\ngaussian_blur(y);\nauto h = half_luma;\n")
        _w(self.root, "src/VP-V/modules/repair/tests/T.cpp",
           "gaussian_blur(z);\n")

    def tearDown(self):
        self.td.cleanup()

    def test_scan(self):
        r = csu.scan(self.root)
        g = _unit(r, "U2")
        self.assertEqual([(h["file"], h["line"]) for h in g["hits"]],
                         [("src/VP-V/modules/repair/other/Dup.h", 3)])
        self.assertEqual(g["inside_owner"], 2)
        h = _unit(r, "U3")
        self.assertEqual(h["hits"][0]["line"], 4)
        files = {x["file"] for c in r["computations"] for x in c["hits"]}
        self.assertFalse(any("/tests/" in f for f in files))
        self.assertFalse(any("grain_split" in f for f in files))

    def test_every_unit_registered(self):
        ids = [e["id"] for e in csu.REGISTRY]
        self.assertEqual(ids, ["U%d" % i for i in range(1, 13)])

    def test_default_run_exits_zero_even_with_duplicates(self):
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 0)
        self.assertIn("Total duplicates", out)
        rc, out = _run(["--json", "--root", self.root])
        self.assertEqual(rc, 0)
        d = json.loads(out)
        self.assertEqual(d["total_outside"], 2)

    # ---- roles --------------------------------------------------------------------------------
    def test_roles(self):
        _w(self.root, "src/VP-V/modules/repair/c/Cons.h",
           "VpUnit::BandFromBase::Weight(1, 2.0, 3);  // gaussian_blur\n")
        _w(self.root, "src/VP-V/core/ProductCatalog.h", "gaussian_blur(decl);\n")
        _w(self.root, "src/VP-V/modules/repair/c/Cmt.h", "// gaussian_blur is described here\n")
        _w(self.root, "src/VP-V/modules/drive/led_x/Led.cpp", "gaussian_blur(led);\n")
        _w(self.root, "src/VideoProcessor-Lib/vprenderer/BillFile.h", "gaussian_blur(bill);\n")
        _w(self.root, "src/VideoProcessor-Lib/vprenderer/VpOwn.h", "gaussian_blur(vpown);\n")
        r = csu.scan(self.root, baseline={"units": {}})
        role = {h["file"].rsplit("/", 1)[-1]: h["role"] for h in _unit(r, "U2")["hits"]}
        self.assertEqual(role["Cons.h"], "consumer")
        self.assertEqual(role["ProductCatalog.h"], "declaration")
        self.assertEqual(role["Cmt.h"], "declaration")
        self.assertEqual(role["Led.cpp"], "output_domain")
        self.assertEqual(role["BillFile.h"], "bill")
        self.assertEqual(role["VpOwn.h"], "duplicate")
        self.assertEqual(role["Dup.h"], "duplicate")
        self.assertEqual(_unit(r, "U2")["duplicates"], 2)
        self.assertEqual(r["total_by_role"]["bill"], 1)

    def test_known_false_positive_and_residual(self):
        base = {"units": {"U2": {"blocking": True, "baseline_duplicates": 1, "known_false_positive": [
            {"file": "src/VP-V/modules/repair/other/Dup.h", "pattern": "gaussian_blur",
             "reason": "test fp"}]}}}
        r = csu.scan(self.root, baseline=base)
        u = _unit(r, "U2")
        self.assertEqual(u["roles"]["known_false_positive"], 1)
        self.assertEqual(u["duplicates"], 0)
        self.assertEqual(u["hits"][0]["reason"], "test fp")
        base["units"]["U2"]["known_false_positive"][0]["kind"] = "residual"
        u = _unit(csu.scan(self.root, baseline=base), "U2")
        self.assertEqual(u["duplicates"], 1)
        self.assertEqual(u["hits"][0]["reason"], "test fp")

    def test_widened_signatures_find_real_duplicate_forms(self):
        _w(self.root, "src/VP-V/modules/repair/w/K.h",
           "sum += std::exp(-0.5 * k * k / (s * s));\n"
           "int r = (int)std::ceil(3.0 * sigma);\n"
           "double v = GAUSS_R;\n"
           "double hs = HalfSigma(s);\n"
           "#include <VP-V/modules/repair/grain_split/GrainSplit.h>\n"
           "float w = gp_w(i);\n"
           "float l = laplacian(x);\n"
           "dst[x] = (a + b + c + d + 2u) >> 2;\n"
           "dst[x] = min(255u, sum >> 4);\n")
        r = csu.scan(self.root, baseline={"units": {}})
        lines2 = {h["line"] for h in _unit(r, "U2")["hits"] if h["file"].endswith("K.h")}
        self.assertTrue({1, 2, 3, 4, 5, 6} <= lines2, lines2)
        self.assertIn(7, {h["line"] for h in _unit(r, "U5")["hits"]})
        lines3 = {h["line"] for h in _unit(r, "U3")["hits"] if h["file"].endswith("K.h")}
        self.assertTrue({8, 9} <= lines3, lines3)

    # ---- strict + baseline --------------------------------------------------------------------
    def _baseline(self, blocking, n):
        _w(self.root, "tools/shared_units_baseline.json", json.dumps(
            {"schema": 1, "units": {"U2": {"blocking": blocking, "baseline_duplicates": n,
                                           "known_false_positive": []}}}))

    def test_strict_passes_at_baseline_and_fails_above(self):
        self._baseline(True, 1)
        rc, out = _run(["--strict", "--root", self.root])
        self.assertEqual(rc, 0)
        self.assertIn("STRICT: pass", out)
        self._baseline(True, 0)
        rc, out = _run(["--strict", "--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("STRICT: FAIL U2 1 > baseline 0", out)

    def test_strict_ignores_non_blocking_units(self):
        self._baseline(False, 0)
        rc, _ = _run(["--strict", "--root", self.root])
        self.assertEqual(rc, 0)

    def test_write_baseline_records_and_refuses_to_raise_blocking(self):
        self._baseline(False, 0)
        rc, _ = _run(["--write-baseline", "--root", self.root])
        self.assertEqual(rc, 0)
        d = json.load(open(os.path.join(self.root, "tools", "shared_units_baseline.json")))
        self.assertEqual(d["units"]["U2"]["baseline_duplicates"], 1)
        self.assertFalse(d["units"]["U2"]["blocking"])
        self._baseline(True, 0)
        rc, out = _run(["--write-baseline", "--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("refusing to raise", out)
        d = json.load(open(os.path.join(self.root, "tools", "shared_units_baseline.json")))
        self.assertEqual(d["units"]["U2"]["baseline_duplicates"], 0)

    # ---- the real repository ------------------------------------------------------------------
    def test_repo_baseline_passes_strict_and_only_shrinks(self):
        rc, out = _run(["--strict", "--root", ROOT])
        self.assertEqual(rc, 0, out[-600:])
        cur = csu.load_baseline(ROOT)["units"]
        try:
            head = subprocess.run(
                ["git", "-C", ROOT, "show", "HEAD:tools/shared_units_baseline.json"],
                capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return
        if head.returncode != 0:
            return   # not committed yet, or no git
        old = json.loads(head.stdout)["units"]
        for uid, o in old.items():
            if uid in cur:
                self.assertLessEqual(cur[uid]["baseline_duplicates"], o["baseline_duplicates"],
                                     "baseline of %s rose" % uid)
                if o.get("blocking"):
                    self.assertTrue(cur[uid]["blocking"], "%s lost blocking" % uid)


if __name__ == "__main__":
    unittest.main()
