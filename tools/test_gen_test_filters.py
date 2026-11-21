# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""RG-C1: the test-category scanner and the filter generator on a test-method wrapper macro.

A #define whose body declares TEST_METHOD(NAME) must not register a test named NAME (vstest's Name~ is a
case-insensitive substring match: Name!~NAME dropped every CPU test whose name contains "name"). Its
invocations are the tests, with the body's Category attribute. An ALL-CAPS placeholder name fails loudly.
"""
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


G = _load("gen_test_filters")
L = G.lib   # the one scanner module the generator uses (its ScanError and test_sources)

MACRO_SOURCE = r"""
#define ROW_METHOD(NAME, CLIP, PART) \
	BEGIN_TEST_METHOD_ATTRIBUTE(NAME) \
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware") \
	END_TEST_METHOD_ATTRIBUTE() \
	TEST_METHOD(NAME) \
	{ \
		RunRow(#NAME, CLIP, PART); \
	}
#define PLAIN_CONSTANT 3

namespace Ns
{
	TEST_CLASS(RowTests)
	{
	public:
		ROW_METHOD(Row_Clip_Part1, "clip", (0, 1))
		ROW_METHOD(Row_Clip_Part2, "clip", 1)
		TEST_METHOD(FileNameFormatStaysCpu)
		{
		}
	};
}
"""


class MacroScan(unittest.TestCase):
    def test_define_body_is_not_a_test_and_invocations_are(self):
        tests = L.scan_text(MACRO_SOURCE)
        names = [t["method"] for t in tests]
        self.assertNotIn("NAME", names)
        self.assertEqual(names, ["Row_Clip_Part1", "Row_Clip_Part2", "FileNameFormatStaysCpu"])
        by = {t["method"]: t for t in tests}
        self.assertEqual(by["Row_Clip_Part1"]["category"], "GpuHardware")
        self.assertEqual(by["Row_Clip_Part2"]["category"], "GpuHardware")
        self.assertEqual(by["Row_Clip_Part1"]["cls"], "RowTests")
        self.assertIsNone(by["FileNameFormatStaysCpu"]["category"])

    def test_positions_survive_the_blanking(self):
        stripped, wrappers = L.strip_defines(MACRO_SOURCE)
        self.assertEqual(len(stripped), len(MACRO_SOURCE))
        self.assertEqual(stripped.count("\n"), MACRO_SOURCE.count("\n"))
        self.assertNotIn("TEST_METHOD(NAME)", stripped)
        self.assertEqual(set(wrappers), {"ROW_METHOD"})

    def test_placeholder_name_fails_loudly(self):
        with self.assertRaises(L.ScanError) as cm:
            L.scan_text("TEST_CLASS(C)\n{\n\tTEST_METHOD(NAME)\n\t{\n\t}\n};\n", "x/CTests.cpp")
        self.assertIn("NAME", str(cm.exception))

    def test_real_mixed_case_names_pass(self):
        tests = L.scan_text("TEST_CLASS(C)\n{\n\tTEST_METHOD(D8_Rate24_Part1)\n\t{\n\t}\n\tTEST_METHOD(OnAMenuEnd)\n\t{\n\t}\n};\n")
        self.assertEqual([t["method"] for t in tests], ["D8_Rate24_Part1", "OnAMenuEnd"])

    def test_comments_and_literals_add_no_test(self):
        # Gate SHOULD 1 (gate-A20C5-rgc1-10a31bff6.md): the macro search skips comments and string/char literals.
        src = MACRO_SOURCE.replace(
            "\t\tTEST_METHOD(FileNameFormatStaysCpu)",
            "\t\t// ROW_METHOD(Phantom_LineComment, \"clip\", 1)\n"
            "\t\t/* ROW_METHOD(Phantom_BlockComment, \"clip\", 1)\n\t\t   TEST_METHOD(Phantom_BlockMethod) */\n"
            "\t\tconst char* s = \"TEST_METHOD(Phantom_String) ROW_METHOD(Phantom_StringRow, x, 1)\";\n"
            "\t\tconst char* r = R\"x(TEST_METHOD(Phantom_Raw) \" ROW_METHOD(Phantom_RawRow, x, 1))x\";\n"
            "\t\tconst char q = '(';\n"
            "\t\tconst int big = 1'000'000;\n"
            "\t\tTEST_METHOD(FileNameFormatStaysCpu)")
        tests = L.scan_text(src)
        self.assertEqual([t["method"] for t in tests], ["Row_Clip_Part1", "Row_Clip_Part2", "FileNameFormatStaysCpu"])
        self.assertEqual({t["method"]: t["category"] for t in tests}["Row_Clip_Part1"], "GpuHardware")

    def test_blanking_keeps_length_and_line_breaks(self):
        src = "a // c(\nb /* x\ny */ \"s\\\"t\" 'c' 1'000 R\"d(q)\")d\" z"
        out = L.blank_comments_and_strings(src)
        self.assertEqual(len(out), len(src))
        self.assertEqual(out.count("\n"), src.count("\n"))
        self.assertEqual(out.split(), ["a", "b", "1'000", "z"])


class RepositoryFilters(unittest.TestCase):
    """The generated filters of the repository itself."""

    def test_no_placeholder_clause_and_the_d8_class_is_excluded_from_cpu(self):
        manifest = L.load_manifest()
        cpu = G.cpu_filter(manifest).split("&")
        gpu = G.gpu_filter(manifest).split("|")
        for clause in cpu + gpu:
            value = clause.split("~", 1)[-1]
            self.assertFalse(clause.startswith(("Name~", "Name!~")) and L.PLACEHOLDER_RE.match(value), clause)
        self.assertIn("FullyQualifiedName!~D8SceneJudgeAgreementTests", cpu)
        self.assertIn("FullyQualifiedName~D8SceneJudgeAgreementTests", gpu)

    def test_the_fifteen_d8_rows_are_scanned_as_gpu_hardware(self):
        path = os.path.join(L.repo_root(), "src", "VP-V", "modules", "repair", "texture_restore", "tests",
                            "D8SceneJudgeAgreementTests.cpp")
        rows = [t for t in L.scan_file(path) if t["cls"] == "D8SceneJudgeAgreementTests"]
        self.assertEqual(len(rows), 15)
        self.assertTrue(all(t["method"].startswith("D8_") and t["category"] == "GpuHardware" for t in rows))

    def test_generator_exits_non_zero_on_a_placeholder(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "BadTests.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write("TEST_CLASS(Bad)\n{\n\tTEST_METHOD(NAME)\n\t{\n\t}\n};\n")
            real = L.test_sources
            L.test_sources = lambda _root: iter([p])
            try:
                with self.assertRaises(L.ScanError), contextlib.redirect_stdout(io.StringIO()):
                    G.cpu_filter(L.load_manifest())
            finally:
                L.test_sources = real


if __name__ == "__main__":
    unittest.main()
