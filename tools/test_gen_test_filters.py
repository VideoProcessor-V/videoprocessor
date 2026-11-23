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
import sys
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
            value = clause.split("~", 1)[-1].split("=", 1)[-1]
            self.assertFalse(clause.startswith(("Name~", "Name!~", "Name=", "Name!=")) and L.PLACEHOLDER_RE.match(value), clause)
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


DEDICATED_SOURCE = r"""
TEST_CLASS(FootageTests)
{
public:
	BEGIN_TEST_METHOD_ATTRIBUTE(NeedsRealClip)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(NeedsRealClip) { }
	BEGIN_TEST_METHOD_ATTRIBUTE(NeedsCorpus)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Corpus")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(NeedsCorpus) { }
	BEGIN_TEST_METHOD_ATTRIBUTE(TimesTheWalk)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Bench")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(TimesTheWalk) { }
	TEST_METHOD(PlainCpuTest) { }
};
"""


class DedicatedJobCategories(unittest.TestCase):
    """TQ-X2: Footage, Corpus and Bench are excluded categories that run only in a dedicated job."""

    KINDS = ("Footage", "Corpus", "Bench")

    def test_each_kind_is_excluded_from_the_cpu_run_and_known(self):
        manifest = L.load_manifest()
        cpu = G.cpu_filter(manifest).split("&")
        for kind in self.KINDS:
            self.assertIn(kind, L.EXCLUDED_FROM_CPU)
            self.assertIn(kind, L.KNOWN_CATEGORIES)
            self.assertIn("Category!=" + kind, cpu)

    def test_no_kind_is_in_the_default_gpu_set_or_the_warp_set(self):
        manifest = L.load_manifest()
        gpu = G.gpu_filter(manifest).split("|")
        warp = G.gpuwarp_filter(manifest).split("|")
        for kind in self.KINDS:
            self.assertNotIn(kind, L.GPU_DEFAULT_EXCLUDED)
            self.assertNotIn("Category=" + kind, gpu)
            self.assertNotIn("Category=" + kind, warp)

    def test_each_kind_has_its_own_selection(self):
        manifest = L.load_manifest()
        for kind in self.KINDS:
            self.assertEqual(G.gpu_filter(manifest, [kind]).split("|")[-1], "Category=" + kind)

    def test_attribute_only_methods_are_listed_by_name_for_their_kind(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "FootageTests.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write(DEDICATED_SOURCE)
            real = L.test_sources
            L.test_sources = lambda _root: iter([p])
            try:
                manifest = {"entries": []}
                cpu = G.cpu_filter(manifest).split("&")
                self.assertEqual(G.attribute_names(manifest, ["Footage"]), ["NeedsRealClip"])
                self.assertEqual(G.attribute_names(manifest, ["Corpus"]), ["NeedsCorpus"])
                self.assertEqual(G.attribute_names(manifest, ["Bench"]), ["TimesTheWalk"])
                for name in ("NeedsRealClip", "NeedsCorpus", "TimesTheWalk"):
                    self.assertIn("Name!=" + name, cpu)
                self.assertNotIn("Name!=PlainCpuTest", cpu)
                self.assertEqual(G.gpu_filter(manifest, ["Footage"]).split("|"), ["Name=NeedsRealClip", "Category=Footage"])
            finally:
                L.test_sources = real

    def test_job_env_requires_the_kind_and_opts_the_bench_in(self):
        self.assertEqual(L.JOB_ENV["Footage"], {"VP_TEST_REQUIRE_FOOTAGE": "1"})
        self.assertEqual(L.JOB_ENV["Corpus"], {"VP_TEST_REQUIRE_CORPUS": "1"})
        self.assertEqual(L.JOB_ENV["Bench"], {"VP_TEST_BENCH": "1", "VP_TEST_REQUIRE_BENCH": "1"})
        self.assertEqual(sorted(L.JOB_ENV), sorted(self.KINDS))

    def _run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        real = sys.argv
        sys.argv = ["gen_test_filters.py", *argv]
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                try:
                    rc = G.main()
                except SystemExit as e:
                    rc = e.code
        finally:
            sys.argv = real
        return rc, out.getvalue().strip(), err.getvalue()

    def test_cli_prints_the_job_env_and_rejects_other_categories(self):
        self.assertEqual(self._run_main("--job-env", "Footage"), (0, "VP_TEST_REQUIRE_FOOTAGE=1", ""))
        self.assertEqual(self._run_main("--job-env", "Bench")[:2], (0, "VP_TEST_BENCH=1;VP_TEST_REQUIRE_BENCH=1"))
        rc, _out, err = self._run_main("--job-env", "Slow")
        self.assertEqual(rc, 2)
        self.assertIn("--job-env takes", err)

    def test_cli_selects_a_kind_with_gpu_categories(self):
        rc, out, _err = self._run_main("--gpu", "--categories", "Corpus")
        self.assertEqual(rc, 0)
        self.assertIn("Category=Corpus", out.split("|"))
        self.assertNotIn("Category=GpuHardware", out.split("|"))

    def test_names_only_drops_every_category_term_for_a_queue_line(self):
        self.assertEqual(G.names_only("Name~A|Category=X|FullyQualifiedName~B", "|"), "Name~A|FullyQualifiedName~B")
        self.assertEqual(G.names_only("Name!~A&Category!=X&FullyQualifiedName!~B", "&"), "Name!~A&FullyQualifiedName!~B")
        self.assertEqual(G.names_only("Category=Footage", "|"), "")

    def test_cli_names_only_has_no_category_term_in_any_selection(self):
        for argv, sep in ((("--cpu",), "&"), (("--gpu",), "|"), (("--gpuwarp",), "|"), (("--gpu", "--categories", "Bench"), "|")):
            with self.subTest(argv=argv):
                rc, out, _err = self._run_main(*argv, "--names-only")
                self.assertEqual(rc, 0)
                self.assertTrue(out)
                self.assertFalse([t for t in out.split(sep) if t.startswith("Category")], out)
        # the Bench class entry keeps its name clause, so a Bench queue line can be coverage-checked
        self.assertIn("FullyQualifiedName~ProducerThroughputBench", self._run_main("--gpu", "--categories", "Bench", "--names-only")[1])

    def test_cli_names_only_fails_when_the_category_leaves_no_name_clause(self):
        real_sources, real_manifest = L.test_sources, L.load_manifest
        L.test_sources = lambda _root: iter([])
        L.load_manifest = lambda path=None: {"entries": []}
        try:
            rc, out, err = self._run_main("--gpu", "--categories", "Footage", "--names-only")
            self.assertEqual((rc, out), (2, ""))
            self.assertIn("no name clause", err)
            rc, out, _err = self._run_main("--gpu", "--categories", "Footage")   # without the flag the Category term remains
            self.assertEqual((rc, out), (0, "Category=Footage"))
        finally:
            L.test_sources, L.load_manifest = real_sources, real_manifest


WARP_SOURCE = r"""
TEST_CLASS(WarpTests)
{
public:
	TEST_METHOD(RendersOnWarp) { }
	TEST_METHOD(SlowMatrixCombos) { }
	BEGIN_TEST_METHOD_ATTRIBUTE(TimesOnTheAdapter)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(TimesOnTheAdapter) { }
};
"""


class WarpSelectionExcludesOtherCategories(unittest.TestCase):
    """TQ-RG-C4: a test of a GpuWarp class that is Slow or GpuHardware leaves the GpuWarp selection."""

    def _manifest(self, order):
        entries = {"class": {"kind": "class", "value": "WarpTests", "category": "GpuWarp"},
                   "slow": {"kind": "methodPrefix", "value": "SlowMatrix", "category": "Slow"}}
        return {"entries": [entries[k] for k in order]}

    def _warp(self, manifest):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "WarpTests.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write(WARP_SOURCE)
            real = L.test_sources
            L.test_sources = lambda _root: iter([p])
            try:
                return G.gpuwarp_filter(manifest), G.gpu_filter(manifest, ["Slow"]), G.cpu_filter(manifest)
            finally:
                L.test_sources = real

    def test_slow_and_gpu_hardware_methods_leave_the_warp_selection_in_either_order(self):
        for order in (["class", "slow"], ["slow", "class"]):
            with self.subTest(order=order):
                warp, slow, _cpu = self._warp(self._manifest(order))
                clauses = warp.split("|")
                self.assertIn("FullyQualifiedName~WarpTests&FullyQualifiedName!~WarpTests::SlowMatrixCombos"
                              "&FullyQualifiedName!~WarpTests::TimesOnTheAdapter", clauses)
                self.assertNotIn("FullyQualifiedName~WarpTests", clauses)
                self.assertNotIn("FullyQualifiedName!~WarpTests::RendersOnWarp", warp)

    def test_the_slow_tier_still_selects_the_slow_method_and_the_cpu_run_keeps_the_warp_class(self):
        _warp, slow, cpu = self._warp(self._manifest(["class", "slow"]))
        self.assertIn("Name~SlowMatrix", slow.split("|"))
        self.assertIn("Name!~SlowMatrix", cpu.split("&"))
        self.assertNotIn("FullyQualifiedName!~WarpTests", cpu.split("&"))

    def test_a_class_with_nothing_shadowed_keeps_the_plain_clause(self):
        warp, _slow, _cpu = self._warp({"entries": [{"kind": "class", "value": "OtherTests", "category": "GpuWarp"}]})
        self.assertIn("FullyQualifiedName~OtherTests", warp.split("|"))

    def test_repository_matrix_leaves_the_warp_run_but_stays_in_the_slow_tier(self):
        manifest = L.load_manifest()
        warp = G.gpuwarp_filter(manifest).split("|")
        self.assertIn("FullyQualifiedName~WholeHookChainHookTests&"
                      "FullyQualifiedName!~WholeHookChainHookTests::EfficiencyStageCostsOnThisGpu&"
                      "FullyQualifiedName!~WholeHookChainHookTests::WholeHookChainCombinationMatrix", warp)
        self.assertNotIn("FullyQualifiedName~WholeHookChainHookTests", warp)
        self.assertIn("Name~WholeHookChainCombinationMatrix", G.gpu_filter(manifest, ["Slow"]).split("|"))


class ScanEveryTestClassFile(unittest.TestCase):
    """TQ-RG-C2 / MISS-1: the scan reads every .cpp that declares a TEST_CLASS, not only *Tests.cpp."""

    def _tree(self, files):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        for rel, text in files.items():
            path = os.path.join(d.name, "src", *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
        return d.name

    def test_a_cpp_without_the_tests_suffix_is_scanned_when_it_has_a_test_class(self):
        root = self._tree({
            "a/FooBench.cpp": "TEST_CLASS(FooBench)\n{\n\tTEST_METHOD(Runs) { }\n};\n",
            "a/NotATest.cpp": "int main() { return 0; }\n",
            "a/CommentOnly.cpp": "// TEST_CLASS(Phantom) lives in another file\nint x;\n",
            "a/FooTests.cpp": "TEST_CLASS(FooTests)\n{\n\tTEST_METHOD(Also) { }\n};\n",
            "a/header.h": "TEST_CLASS(InAHeader)\n",
        })
        names = sorted(os.path.basename(p) for p in L.test_sources(root))
        self.assertEqual(names, ["CommentOnly.cpp", "FooBench.cpp", "FooTests.cpp"])
        scanned = [t["method"] for p in L.test_sources(root) for t in L.scan_file(p)]
        self.assertEqual(sorted(scanned), ["Also", "Runs"])   # the comment-only file adds no test

    def test_the_repository_bench_classes_are_scanned_and_registered(self):
        by_class = {}
        for path in L.test_sources(L.repo_root()):
            for t in L.scan_file(path):
                by_class.setdefault(t["cls"], []).append(t)
        for cls in ("LongPairBenchDumpTests", "GrainSplitV3CostBench", "GrainLevelShiftBench", "ProducerThroughputBench"):
            self.assertIn(cls, by_class)
        self.assertEqual(len(by_class["GrainLevelShiftBench"]), 2)
        self.assertEqual(by_class["ProducerThroughputBench"][0]["category"], "Bench")
        cpu = G.cpu_filter(L.load_manifest()).split("&")
        for clause in ("FullyQualifiedName!~LongPairBenchDumpTests", "FullyQualifiedName!~GrainSplitV3CostBench",
                       "FullyQualifiedName!~ProducerThroughputBench"):
            self.assertIn(clause, cpu)
        # the two CPU methods of GrainLevelShiftBench stay in the CPU run
        self.assertNotIn("FullyQualifiedName!~GrainLevelShiftBench", cpu)
        self.assertIn("FullyQualifiedName~GrainSplitV3CostBench", G.gpu_filter(L.load_manifest()).split("|"))
        self.assertIn("FullyQualifiedName~ProducerThroughputBench", G.gpu_filter(L.load_manifest(), ["Bench"]).split("|"))


class ExactMatchAndFix2AdversarialTests(unittest.TestCase):
    """W23-4u (TQ-GENFILTER-EXACT & FIX-2): exact Name=/Name!= boundary and gpuwarp exclusion of Bench/Footage/Corpus."""

    @staticmethod
    def _matches_vstest_clause(clause, method_name):
        if clause.startswith("Name!="):
            return method_name.lower() != clause[len("Name!="):].lower()
        if clause.startswith("Name="):
            return method_name.lower() == clause[len("Name="):].lower()
        if clause.startswith("Name!~"):
            return clause[len("Name!~"):].lower() not in method_name.lower()
        if clause.startswith("Name~"):
            return clause[len("Name~"):].lower() in method_name.lower()
        return True

    def test_exact_name_exclusion_does_not_drop_prefix_sibling_with_substring_negative_control(self):
        src = r"""
TEST_CLASS(CostTests)
{
public:
	BEGIN_TEST_METHOD_ATTRIBUTE(GpuCostLine)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Bench")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(GpuCostLine) { }
	TEST_METHOD(GpuCostLineCases) { }
};
"""
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "CostTests.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write(src)
            real = L.test_sources
            L.test_sources = lambda _root: iter([p])
            try:
                manifest = {"entries": []}
                cpu = G.cpu_filter(manifest).split("&")
                bench = G.gpu_filter(manifest, ["Bench"]).split("|")
                self.assertIn("Name!=GpuCostLine", cpu)
                self.assertNotIn("Name!~GpuCostLine", cpu)
                self.assertIn("Name=GpuCostLine", bench)
                self.assertNotIn("Name~GpuCostLine", bench)

                # In-test negative control: naive substring Name!~GpuCostLine falsely drops sibling GpuCostLineCases!
                self.assertFalse(self._matches_vstest_clause("Name!~GpuCostLine", "GpuCostLineCases"),
                                 "Negative control failed: substring Name!~ must falsely drop prefix-matching sibling")
                self.assertTrue(self._matches_vstest_clause("Name~GpuCostLine", "GpuCostLineCases"),
                                "Negative control failed: substring Name~ must falsely select prefix-matching sibling")

                # Exact-match clause excludes GpuCostLine while keeping GpuCostLineCases in CPU run.
                cpu_name_clauses = [c for c in cpu if c.startswith("Name")]
                self.assertFalse(all(self._matches_vstest_clause(c, "GpuCostLine") for c in cpu_name_clauses))
                self.assertTrue(all(self._matches_vstest_clause(c, "GpuCostLineCases") for c in cpu_name_clauses))
            finally:
                L.test_sources = real

    def test_gpuwarp_filter_excludes_bench_footage_and_corpus_methods_in_warp_class(self):
        src = r"""
TEST_CLASS(WarpMixedTests)
{
public:
	TEST_METHOD(WarpPlain) { }
	BEGIN_TEST_METHOD_ATTRIBUTE(WarpBench)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Bench")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(WarpBench) { }
	BEGIN_TEST_METHOD_ATTRIBUTE(WarpFootage)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(WarpFootage) { }
	BEGIN_TEST_METHOD_ATTRIBUTE(WarpCorpus)
		TEST_METHOD_ATTRIBUTE(L"Category", L"Corpus")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(WarpCorpus) { }
};
"""
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "WarpMixedTests.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write(src)
            real = L.test_sources
            L.test_sources = lambda _root: iter([p])
            try:
                manifest = {"entries": [{"kind": "class", "value": "WarpMixedTests", "category": "GpuWarp"}]}
                warp_clauses = G.gpuwarp_filter(manifest).split("|")
                expected = ("FullyQualifiedName~WarpMixedTests"
                            "&FullyQualifiedName!~WarpMixedTests::WarpBench"
                            "&FullyQualifiedName!~WarpMixedTests::WarpCorpus"
                            "&FullyQualifiedName!~WarpMixedTests::WarpFootage")
                self.assertIn(expected, warp_clauses)
                self.assertNotIn("FullyQualifiedName~WarpMixedTests", warp_clauses)
                self.assertNotIn("WarpPlain", expected)
            finally:
                L.test_sources = real

    def test_repository_dedicated_lanes_and_fix2_corpus_exclusion(self):
        manifest = L.load_manifest()
        warp = G.gpuwarp_filter(manifest)
        self.assertIn("GpuDolbyVisionTunnelUnpackMatchesTheSyntheticCorpus", warp)
        self.assertIn("!~", warp)
        # Verify Bench, Footage, Corpus each have non-empty --names-only filters with zero Category terms.
        for kind in ("Bench", "Footage", "Corpus"):
            text = G.names_only(G.gpu_filter(manifest, [kind]), "|")
            self.assertTrue(text, kind)
            self.assertNotIn("Category=", text)


if __name__ == "__main__":
    unittest.main()
