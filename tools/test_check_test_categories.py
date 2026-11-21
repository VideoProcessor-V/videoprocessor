# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""check_test_categories.py: the excluded Footage / Corpus / Bench categories (TQ-X2) and the unknown-attribute check."""
import contextlib
import importlib.util
import io
import os
import tempfile
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    s = importlib.util.spec_from_file_location(name, os.path.join(_dir, name + ".py"))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


C = _load("check_test_categories")
L = C.lib

SOURCE = r"""
TEST_CLASS(KindTests)
{
public:
	BEGIN_TEST_METHOD_ATTRIBUTE(TheTest)
		TEST_METHOD_ATTRIBUTE(L"Category", L"%s")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(TheTest) { }
};
"""


def run_checker(category, entries=()):
    """(rc, output) of check_test_categories.main on one synthetic test source carrying the category attribute."""
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "KindTests.cpp")
        with open(p, "w", encoding="utf-8") as f:
            f.write(SOURCE % category)
        real_sources, real_manifest, real_root = L.test_sources, L.load_manifest, L.repo_root
        L.repo_root = lambda: d   # relpath needs the source and the root on one drive
        L.test_sources = lambda _root: iter([p])
        L.load_manifest = lambda path=None: {"entries": list(entries)}
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                rc = C.main()
        finally:
            L.test_sources, L.load_manifest, L.repo_root = real_sources, real_manifest, real_root
        return rc, out.getvalue()


class DedicatedCategories(unittest.TestCase):
    def test_footage_corpus_and_bench_attributes_are_accepted(self):
        for kind in ("Footage", "Corpus", "Bench"):
            with self.subTest(kind=kind):
                rc, out = run_checker(kind)
                self.assertEqual(rc, 0, out)

    def test_unknown_attribute_category_fails(self):
        rc, out = run_checker("Footge")
        self.assertEqual(rc, 1)
        self.assertIn("unknown Category attribute Footge", out)

    def test_manifest_entry_of_a_dedicated_kind_is_known(self):
        entry = {"kind": "class", "value": "KindTests", "category": "Footage"}
        rc, out = run_checker("Footage", [entry])
        self.assertEqual(rc, 0, out)

    def test_attribute_and_manifest_disagreeing_on_exclusion_fails(self):
        entry = {"kind": "class", "value": "KindTests", "category": "Corpus"}
        rc, out = run_checker("Cpu", [entry])
        self.assertEqual(rc, 1)
        self.assertIn("disagrees with the manifest", out)

    def test_manifest_entry_with_an_unknown_category_fails(self):
        entry = {"kind": "class", "value": "KindTests", "category": "Benchmark"}
        rc, out = run_checker("Cpu", [entry])
        self.assertEqual(rc, 1)
        self.assertIn("unknown category Benchmark", out)

    def test_repository_manifest_passes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = C.main()
        self.assertEqual(rc, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
