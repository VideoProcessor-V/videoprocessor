# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-X3: check_bespoke_skips.py flags a test that logs a skip and returns with no SKIP-<KIND> marker."""
import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    s = importlib.util.spec_from_file_location(name, os.path.join(_dir, name + ".py"))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


B = _load("check_bespoke_skips")
RC = _load("run_compliance")

HEAD = "TEST_CLASS(KindTests)\n{\npublic:\n\tTEST_METHOD(TheTest)\n\t{\n"
TAIL = "\t}\n};\n"


def hits(body):
    return B.scan_text("a/KindTests.cpp", HEAD + body + TAIL)


class Detection(unittest.TestCase):
    def test_a_logged_skip_followed_by_return_is_a_hit_in_each_house_shape(self):
        shapes = [
            'if (!x) { Logger::WriteMessage("clip missing: skipped\\n"); return; }\n',
            'if (!x) { Logger::WriteMessage(L"Native replay diagnostic skipped: no file."); return; }\n',
            'if (!x)\n{\n\tLogger::WriteMessage("optical flow engine unavailable: not run\\n");\n\treturn;\n}\n',
            'if (!x)\n{\n\tLogger::WriteMessage("TemporalDetailGpuTiming: VP_TEST_GPU=hardware only; nothing run");\n\treturn;\n}\n',
            'for (;;) { if (!x) { Logger::WriteMessage((std::string(n) + ": no clip, skipped\\n").c_str()); continue; } }\n',
            'Logger::WriteMessage("IntentWalkBenchmark skipped (set VP_TEST_BENCH=1)");\nreturn;\n',
        ]
        for body in shapes:
            with self.subTest(body=body):
                found = hits(body)
                self.assertEqual(len(found), 1, found)
                self.assertEqual(found[0]["file"], "a/KindTests.cpp::KindTests::TheTest")

    def test_a_skip_marker_is_not_a_hit(self):
        for marker in ("SKIP-NO-FOOTAGE", "SKIP-NO-CORPUS", "SKIP-OPT-IN", "SKIP-NO-GPU", "SKIP-NO-HARDWARE"):
            with self.subTest(marker=marker):
                self.assertEqual(hits('if (!x) { Logger::WriteMessage("%s TheTest: skipped\\n"); return; }\n' % marker), [])

    def test_a_log_that_does_not_bail_is_not_a_hit(self):
        self.assertEqual(hits('Logger::WriteMessage("3 reels run, 1 skipped");\nAssert::IsTrue(ok);\n'), [])
        self.assertEqual(hits('Logger::WriteMessage("skipped the slow part");\nRunTheRest();\nreturn;\n'), [])

    def test_a_log_without_skip_words_is_not_a_hit(self):
        self.assertEqual(hits('Logger::WriteMessage("walk 12.5 ms");\nreturn;\n'), [])

    def test_comments_and_strings_are_not_hits(self):
        self.assertEqual(hits('// Logger::WriteMessage("x skipped"); return;\n'), [])
        self.assertEqual(hits('/* Logger::WriteMessage("x skipped"); return; */\n'), [])
        self.assertEqual(hits('const char* s = "Logger::WriteMessage(\\"x skipped\\"); return;";\n'), [])

    def test_a_hit_outside_a_test_method_is_keyed_to_the_file(self):
        text = 'static void Helper()\n{\n\tLogger::WriteMessage("x skipped"); return;\n}\n' + HEAD + TAIL
        found = B.scan_text("a/KindTests.cpp", text)
        self.assertEqual([h["file"] for h in found], ["a/KindTests.cpp::<file>"])
        self.assertEqual(found[0]["line"], 3)

    def test_the_selftest_passes(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(B.main(["--selftest"]), 0)


class Ratchet(unittest.TestCase):
    """The per-method ceilings against a baseline: a new hit fails, a lower count passes, an unlisted method has ceiling 0."""

    def _root(self, body):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        src = os.path.join(d.name, "src", "t")
        os.makedirs(src)
        with open(os.path.join(src, "KindTests.cpp"), "w", encoding="utf-8") as f:
            f.write(HEAD + body + TAIL)
        return d.name

    def _run(self, root, baseline, *extra):
        path = os.path.join(root, "baseline.json")
        if baseline is not None:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"total": sum(baseline.values()), "files": baseline, "classes": {}}, f)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = B.main(["--root", root, "--baseline", path, *extra])
        return rc, out.getvalue()

    BAD = 'if (!x) { Logger::WriteMessage("clip missing: skipped\\n"); return; }\n'
    KEY = "src/t/KindTests.cpp::KindTests::TheTest"

    def test_an_unlisted_hit_fails(self):
        rc, out = self._run(self._root(self.BAD), {})
        self.assertEqual(rc, 1, out)
        self.assertIn("GREW " + self.KEY, out)

    def test_a_listed_hit_passes_and_a_marker_version_passes_with_a_lower_count(self):
        self.assertEqual(self._run(self._root(self.BAD), {self.KEY: 1})[0], 0)
        good = 'if (!x) { Logger::WriteMessage("SKIP-NO-FOOTAGE TheTest: skipped\\n"); return; }\n'
        rc, out = self._run(self._root(good), {self.KEY: 1})
        self.assertEqual(rc, 0, out)
        self.assertIn("shrunk files 1", out)

    def test_a_second_hit_in_a_listed_method_fails(self):
        rc, out = self._run(self._root(self.BAD + self.BAD), {self.KEY: 1})
        self.assertEqual(rc, 1, out)
        self.assertIn("2 > ceiling 1", out)

    def test_write_baseline_refuses_to_grow_and_a_missing_baseline_is_an_error(self):
        root = self._root(self.BAD + self.BAD)
        rc, out = self._run(root, {self.KEY: 1}, "--write-baseline")
        self.assertEqual(rc, 1, out)
        self.assertIn("refusing to write a baseline that grew", out)
        self.assertEqual(self._run(self._root(self.BAD), None)[0], 2)


class Repository(unittest.TestCase):
    def test_the_repository_is_within_its_baseline(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = B.main([])
        self.assertEqual(rc, 0, out.getvalue())

    def test_the_baseline_lists_the_known_rows_and_is_not_empty(self):
        with open(os.path.join(_dir, "bespoke-skips-baseline.json"), encoding="utf-8") as f:
            base = json.load(f)
        self.assertGreater(base["total"], 0)
        self.assertEqual(base["total"], sum(base["files"].values()))
        for key in base["files"]:
            self.assertEqual(key.count("::"), 2, key)

    def test_the_checker_is_registered_in_run_compliance(self):
        names = {t[0]: t[1] for t in RC.TOOLS}
        self.assertEqual(names.get("bespoke_skips"), "check_bespoke_skips.py")
        self.assertTrue(os.path.isfile(os.path.join(_dir, "check_bespoke_skips.py")))


if __name__ == "__main__":
    unittest.main()
