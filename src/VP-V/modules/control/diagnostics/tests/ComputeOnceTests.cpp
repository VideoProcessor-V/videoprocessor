// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/GpuAttribution.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/diagnostics/ComputeOnceModule.h>
#include <VP-V/modules/control/diagnostics/ComputeOnceReport.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// COMPUTE-ONCE detectors 2 and 3 (iface-L-DET): the unit call counters, the frame scopes, the skeleton Acquire
// fold-in, the GPU pass attribution, the report lines and the cost of the counters while off.
namespace ComputeOnceTestsNs
{
	// Two distinct callers of one "entry point" (noinline: each keeps its own return address).
	__declspec(noinline) void EntryPoint(int64_t seq, uint64_t variant)
	{
		VPV_COMPUTE_ONCE_AT("U1.grain_split", seq, variant);
	}
	__declspec(noinline) void EntryPointScoped(uint64_t variant)
	{
		VPV_COMPUTE_ONCE("U1.grain_split", variant);
	}
	// Distinct bodies with work after the call: no identical-COMDAT folding, no tail call (each keeps its return address).
	volatile int g_sink = 0;
	__declspec(noinline) void CallerA(int64_t seq) { EntryPoint(seq, 0); g_sink = g_sink + 1; }
	__declspec(noinline) void CallerB(int64_t seq) { EntryPoint(seq, 0); g_sink = g_sink + 2; }
	__declspec(noinline) void PrivateCopy(int64_t seq)
	{
		VPV_COMPUTE_ONCE_PRIVATE_AT("U3.half_luma", seq, "matcher");
	}
	__declspec(noinline) void EntryOff(int i)
	{
		VPV_COMPUTE_ONCE_AT("U3.half_luma", i, 0);
	}
	// The CostWhenOff baseline: the same noinline call with no entry point. A plain store keeps the call (an inlinable empty
	// lambda was folded away, so the baseline read 0 ns and the call itself was charged to the detector).
	int g_emptySink = 0;
	__declspec(noinline) void EntryEmpty(int i) { g_emptySink = i; }

	struct EnabledScope
	{
		EnabledScope()
		{
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			GpuAttribution::Global().Reset();
			ComputeOnce::Current() = ComputeOnce::ThreadFrame();
		}
		~EnabledScope()
		{
			ComputeOnce::Global().Reset();
			GpuAttribution::Global().Reset();
			ComputeOnce::Current() = ComputeOnce::ThreadFrame();
			ComputeOnce::SetEnabled(false);
		}
	};

	const ComputeOnce::UnitRow* Find(const std::vector<ComputeOnce::UnitRow>& rows, const std::string& unit, const std::string& variant)
	{
		for (const ComputeOnce::UnitRow& r : rows)
			if (r.unit == unit && r.variant == variant)
				return &r;
		return nullptr;
	}

	bool AnyLineStarts(const std::vector<std::string>& lines, const std::string& prefix)
	{
		for (const std::string& l : lines)
			if (l.compare(0, prefix.size(), prefix) == 0)
				return true;
		return false;
	}

	TEST_CLASS(ComputeOnceTests)
	{
	public:
		TEST_METHOD(OneCallPerFrameIsClean)
		{
			EnabledScope on;
			for (int64_t s = 0; s < 10; ++s)
				CallerA(s);
			const auto rows = ComputeOnce::Global().Rows();
			const ComputeOnce::UnitRow* r = Find(rows, "U1.grain_split", ComputeOnce::detail::HexVariant(0));
			Assert::IsNotNull(r);
			Assert::AreEqual<uint64_t>(10, r->frames);
			Assert::AreEqual<uint64_t>(10, r->calls);
			Assert::AreEqual<uint32_t>(1, r->maxPerFrame);
			Assert::AreEqual<uint64_t>(0, r->dupFrames);
			Assert::AreEqual<size_t>(1, r->callers.size(), L"one caller");
			Assert::IsTrue(ComputeOnceReport::StatusLine().empty(), L"clean: no status line");
		}

		TEST_METHOD(SecondCallOfAFrameIsADuplicateWithBothCallersNamed)
		{
			EnabledScope on;
			for (int64_t s = 0; s < 4; ++s)
			{
				CallerA(s);
				if (s % 2 == 1)
					CallerB(s);
			}
			const auto rows = ComputeOnce::Global().Rows();
			const ComputeOnce::UnitRow* r = Find(rows, "U1.grain_split", ComputeOnce::detail::HexVariant(0));
			Assert::IsNotNull(r);
			Assert::AreEqual<uint64_t>(4, r->frames);
			Assert::AreEqual<uint64_t>(6, r->calls);
			Assert::AreEqual<uint32_t>(2, r->maxPerFrame);
			Assert::AreEqual<uint64_t>(2, r->dupFrames);
			Assert::AreEqual<size_t>(2, r->dupCallers.size(), L"both callers of the duplicate");
			Assert::IsTrue(r->dupCallers[0].address != r->dupCallers[1].address);
			const std::vector<std::string> lines = ComputeOnceReport::Lines();
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once DUPLICATE U1.grain_split variant "));
			bool named = false;
			for (const std::string& l : lines)
				if (l.find("DUPLICATE") != std::string::npos && l.find("CallerA") != std::string::npos && l.find("CallerB") != std::string::npos)
					named = true;
			Assert::IsTrue(named, L"the duplicate line names both callers (PDB symbols)");
			Assert::IsFalse(ComputeOnceReport::StatusLine().empty());
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once summary units 1 duplicates 1 "));
		}

		TEST_METHOD(VariantsAreCountedSeparately)
		{
			EnabledScope on;
			EntryPoint(5, ComputeOnce::Variant(1, 960, 540, 3000));
			EntryPoint(5, ComputeOnce::Variant(1, 960, 540, 6000));
			EntryPoint(5, ComputeOnce::Variant(2, 960, 540, 3000));
			const auto rows = ComputeOnce::Global().Rows();
			Assert::AreEqual<size_t>(3, rows.size());
			for (const ComputeOnce::UnitRow& r : rows)
				Assert::AreEqual<uint32_t>(1, r.maxPerFrame);
			Assert::AreNotEqual<uint64_t>(0, ComputeOnce::Variant(0, 0, 0, 1));
			Assert::AreEqual<uint64_t>(0, ComputeOnce::Variant(0));
		}

		TEST_METHOD(ThreadScopeGivesTheFrameAndNoScopeIsAGap)
		{
			EnabledScope on;
			EntryPointScoped(0);   // no scope on this thread yet
			{
				const ComputeOnce::FrameScope lane(7, "lane", 3);
				EntryPointScoped(0);
				{
					const ComputeOnce::FrameScope inner(8, "lane", 4);
					Assert::AreEqual<int64_t>(8, ComputeOnce::CurrentSeq());
				}
				Assert::AreEqual<int64_t>(7, ComputeOnce::CurrentSeq(), L"the inner scope restores the outer one");
			}
			Assert::AreEqual<int64_t>(-1, ComputeOnce::CurrentSeq(), L"the scope restores 'no frame'");
			ComputeOnce::SetThreadFrame(9, "hub", 4);
			EntryPointScoped(0);
			EntryPointScoped(0);
			const auto gaps = ComputeOnce::Global().Gaps();
			Assert::AreEqual<size_t>(1, gaps.size());
			Assert::AreEqual<uint64_t>(1, gaps[0].calls);
			const auto rows = ComputeOnce::Global().Rows();
			Assert::AreEqual<size_t>(1, rows.size());
			Assert::AreEqual<uint64_t>(2, rows[0].frames, L"frames 7 and 9");
			Assert::AreEqual<uint32_t>(2, rows[0].maxPerFrame, L"two calls under the sticky hub frame 9");
			const std::vector<std::string> lines = ComputeOnceReport::Lines();
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once gap U1.grain_split calls 1 "));
			bool hub = false;
			for (const std::string& l : lines)
				if (l.find("@hub:Begin") != std::string::npos)
					hub = true;
			Assert::IsTrue(hub, L"the hub phase is named in the caller context");
		}

		TEST_METHOD(PrivateCopyIsAHiddenCopyRow)
		{
			EnabledScope on;
			PrivateCopy(1);
			PrivateCopy(2);
			const std::vector<std::string> lines = ComputeOnceReport::Lines();
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once HIDDEN-COPY U3.half_luma variant private:matcher frames 2 calls 2 "));
			Assert::IsTrue(ComputeOnceReport::StatusLine().find("1 hidden copies") != std::string::npos);
		}

		TEST_METHOD(OldFramesArePruned)
		{
			EnabledScope on;
			for (int64_t s = 0; s < 20000; ++s)
				EntryPoint(s, 0);
			Assert::IsTrue(ComputeOnce::Global().LiveCells() <= ComputeOnce::kPruneAbove + 1);
			EntryPoint(19999, 0);   // the newest frame is still known
			const auto rows = ComputeOnce::Global().Rows();
			Assert::AreEqual<uint32_t>(2, rows[0].maxPerFrame);
			Assert::AreEqual<uint64_t>(20000, rows[0].frames);
		}

		TEST_METHOD(AcquireOwnerFillsAreFoldedIn)
		{
			EnabledScope on;
			using namespace VpSkeleton;
			SkeletonMemo m;
			int calls = 0;
			OwnerFill f;
			f.fill = [&calls](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				++calls;
				out.bytes.assign(16, 1);
				return true;
			};
			Assert::IsTrue(m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "repair/grain_split", f) == SkeletonMemo::Registration::Registered);
			m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);
			m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);   // reused: not a fill
			m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu);
			Assert::AreEqual(2, calls);
			const auto rows = ComputeOnce::Global().Rows();
			Assert::AreEqual<size_t>(1, rows.size());
			Assert::IsTrue(rows[0].unit.compare(0, 8, "acquire:") == 0, L"unit acquire:<product>");
			Assert::AreEqual<uint64_t>(2, rows[0].frames);
			Assert::AreEqual<uint32_t>(1, rows[0].maxPerFrame);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuPassesAreAttributedOrListedWithTheirCost)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")   // registry only: no GPU device
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuPassesAreAttributedOrListedWithTheirCost)
		{
			EnabledScope on;
			GpuAttribution::Registry& g = GpuAttribution::Global();
			const char* lib[] = { "color decoding", "ewa_lanczos polar scaling (luma)", "dithering (8 bits)" };
			g.Note(lib, 3, "color decoding, ewa_lanczos polar scaling (luma), dithering (8 bits)", 2000000, nullptr);
			const char* hidden[] = { "VP mystery pass" };
			g.Note(hidden, 1, "VP mystery pass", 500000, nullptr);
			g.Note(hidden, 1, "VP mystery pass", 700000, nullptr);
			const char* watched[] = { "some lane step" };
			g.Note(watched, 1, "some lane step", 100000, "camera_model");
			g.Note(nullptr, 0, nullptr, 100000, nullptr);   // a pass naming nothing
			const auto un = g.Unattributed();
			Assert::AreEqual<size_t>(2, un.size());
			Assert::AreEqual<uint64_t>(5, g.Passes());
			Assert::AreEqual<uint64_t>(3, g.UnattributedPasses());
			bool mystery = false;
			for (const auto& s : un)
				if (s.step == "VP mystery pass")
				{
					mystery = true;
					Assert::AreEqual<uint64_t>(2, s.passes);
					Assert::AreEqual<uint64_t>(1200000, s.ns);
				}
			Assert::IsTrue(mystery);
			const std::vector<std::string> lines = ComputeOnceReport::Lines();
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once gpu UNATTRIBUTED \"VP mystery pass\" passes 2 ms 0.600 "));
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once gpu owner pathway:libplacebo_render passes 1 ms 2.000"));
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once gpu owner camera_model passes 1 "));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ExternalPassesRecordSubmitCountAndResolvedTime)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")   // registry only: no GPU device
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ExternalPassesRecordSubmitCountAndResolvedTime)
		{
			EnabledScope on;
			GpuAttribution::NoteExternal("engine luma", "source_analysis_luma");
			GpuAttribution::NoteExternal("engine luma", "source_analysis_luma");
			GpuAttribution::NoteExternalTimed("engine luma", "source_analysis_luma", 1500000);   // 1.5 ms resolved later
			GpuAttribution::NoteExternalTimed("engine luma", "source_analysis_luma", 2500000);   // 2.5 ms resolved later
			GpuAttribution::NoteExternal("nvof execute", "motion_engine_nvof");
			GpuAttribution::NoteExternalTimed("nvof execute (wall)", "motion_engine_nvof", 3200000);

			const auto owners = GpuAttribution::Global().Owners();
			bool foundLuma = false, foundNvof = false;
			for (const auto& o : owners)
			{
				if (o.owner == "source_analysis_luma")
				{
					foundLuma = true;
					Assert::AreEqual<uint64_t>(2, o.passes, L"NoteExternalTimed does not double-count passes");
					Assert::AreEqual<uint64_t>(4000000, o.ns, L"resolved ns accumulated");
				}
				else if (o.owner == "motion_engine_nvof")
				{
					foundNvof = true;
					Assert::AreEqual<uint64_t>(1, o.passes);
					Assert::AreEqual<uint64_t>(3200000, o.ns);
				}
			}
			Assert::IsTrue(foundLuma);
			Assert::IsTrue(foundNvof);
			const std::vector<std::string> lines = ComputeOnceReport::Lines();
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once gpu owner source_analysis_luma passes 2 ms 2.000"));
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once gpu owner motion_engine_nvof passes 1 ms 3.200"));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuRulesFirstVpOwnerWinsAndPrivateCopiesAreListed)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")   // registry only: no GPU device
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuRulesFirstVpOwnerWinsAndPrivateCopiesAreListed)
		{
			EnabledScope on;
			GpuAttribution::Registry g;
			g.AddRule("VideoProcessor deband", "vp_debander");
			g.AddRule("half luma copy", "dejudder_matcher", GpuAttribution::Kind::PrivateCopy, "U3.half_luma");
			const char* steps[] = { "color decoding", "VideoProcessor deband: grain guard" };
			g.Note(steps, 2, "color decoding, VideoProcessor deband: grain guard", 1000, nullptr);
			const char* copy[] = { "half luma copy" };
			g.Note(copy, 1, "half luma copy", 3000, nullptr);
			const auto owners = g.Owners();
			Assert::AreEqual<size_t>(2, owners.size());
			bool deband = false;
			for (const auto& o : owners)
				if (o.owner == "vp_debander")
					deband = true;
			Assert::IsTrue(deband, L"the VP owner of a mixed pass, not the pathway");
			Assert::AreEqual<size_t>(1, g.PrivateCopies().size());
			Assert::AreEqual<size_t>(0, g.Unattributed().size());
		}

		TEST_METHOD(OffRecordsNothingAndIsSilent)
		{
			ComputeOnce::SetEnabled(false);
			ComputeOnce::Global().Reset();
			GpuAttribution::Global().Reset();
			CallerA(1);
			CallerA(1);
			GpuAttribution::Note(nullptr, 0, "x", 1, nullptr);
			{
				const ComputeOnce::FrameScope s(3, "lane", 1);
				Assert::AreEqual<int64_t>(-1, ComputeOnce::CurrentSeq(), L"no scope while off");
			}
			Assert::AreEqual<size_t>(0, ComputeOnce::Global().Rows().size());
			Assert::AreEqual<uint64_t>(0, GpuAttribution::Global().Passes());
			Assert::IsTrue(ComputeOnceReport::Lines().empty());
			Assert::IsTrue(ComputeOnceReport::StatusLine().empty());
		}

		TEST_METHOD(ModuleShowsTheStatusLineAndAnswersTheReportCommand)
		{
			namespace RE = RendererExtension;
			Vpv::Diagnostics::ComputeOnceModule module;
			RE::Context ctx;
			std::vector<std::string> lines;
			std::string reply;
			ComputeOnce::SetEnabled(false);
			module.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			Assert::IsTrue(lines.empty(), L"off: no status line");
			Assert::IsTrue(module.OnCommand(ctx, "compute_once", "report", reply));
			Assert::IsTrue(reply.find("compute-once: off") == 0);
			Assert::IsFalse(module.OnCommand(ctx, "other", "report", reply));
			EnabledScope on;
			CallerA(1);
			CallerB(1);
			module.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			Assert::AreEqual<size_t>(1, lines.size());
			Assert::IsTrue(lines[0].find("compute-once: 1 duplicate units") == 0);
			module.OnStatus(ctx, RE::StatusKind::Lines, lines, nullptr, nullptr);
			Assert::AreEqual<size_t>(1, lines.size(), L"only the Fallbacks kind");
			Assert::IsTrue(module.OnCommand(ctx, "compute_once", "report", reply));
			Assert::IsTrue(reply.find("Compute-once DUPLICATE U1.grain_split") != std::string::npos);
			Assert::IsTrue(reply.find("Compute-once summary ") != std::string::npos);
		}

		// Near-zero cost when off: one relaxed load and a branch per entry point. Measured against an empty call.
		TEST_METHOD(CostWhenOff)
		{
			ComputeOnce::SetEnabled(false);
			const int n = 20000000;
			auto time = [](void (*fn)(int), int count) {
				const auto t0 = std::chrono::steady_clock::now();
				for (int i = 0; i < count; ++i)
					fn(i);
				return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / count;
			};
			void (*emptyFn)(int) = &EntryEmpty;
			double best = 1e9, bestEmpty = 1e9;
			for (int round = 0; round < 3; ++round)
			{
				best = (std::min)(best, time(&EntryOff, n));
				bestEmpty = (std::min)(bestEmpty, time(emptyFn, n));
			}
			char text[160];
			sprintf_s(text, "compute-once off: %.3f ns per entry call, %.3f ns per empty call, overhead %.3f ns", best, bestEmpty, best - bestEmpty);
			Logger::WriteMessage(text);
			Assert::AreEqual<size_t>(0, ComputeOnce::Global().Rows().size());
			Assert::IsTrue(best - bestEmpty < 3.0, L"off cost under 3 ns per entry point");
		}

		TEST_METHOD(U13IsNotInNotInstrumentedWhileU14Remains)
		{
			const auto& uncounted = ComputeOnceReport::NotInstrumented();
			bool foundU13 = false;
			bool foundU14 = false;
			for (const ComputeOnceReport::Uncounted& u : uncounted)
			{
				if (u.unit && std::string(u.unit) == "U13")
					foundU13 = true;
				if (u.unit && std::string(u.unit) == "U14")
				{
					foundU14 = true;
					Assert::IsNotNull(u.name);
					Assert::AreEqual(std::string("trail_compose"), std::string(u.name), L"U14 entry names trail_compose");
				}
			}
			Assert::IsFalse(foundU13, L"U13 has its own VPV_COMPUTE_ONCE counter and must not be listed in NotInstrumented()");
			// In-test negative control: uninstrumented U14 must still be reported so absence of U13 is not vacuous.
			Assert::IsTrue(foundU14, L"negative control: U14 (trail_compose) remains in NotInstrumented()");

			EnabledScope on;
			const std::vector<std::string> lines = ComputeOnceReport::Lines();
			Assert::IsFalse(AnyLineStarts(lines, "Compute-once not-instrumented U13"),
				L"ComputeOnceReport::Lines() must not emit a not-instrumented row for U13");
			Assert::IsTrue(AnyLineStarts(lines, "Compute-once not-instrumented U14 "),
				L"negative control: ComputeOnceReport::Lines() still emits the not-instrumented row for U14");
			const std::string expectedToken = "not-instrumented " + std::to_string(uncounted.size());
			bool summaryMatches = false;
			for (const std::string& l : lines)
				if (l.compare(0, 21, "Compute-once summary ") == 0 && l.find(expectedToken) != std::string::npos)
					summaryMatches = true;
			Assert::IsTrue(summaryMatches, L"summary line carries exact not-instrumented count");
		}
	};
}
