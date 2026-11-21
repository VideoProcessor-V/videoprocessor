// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3a S2 (lead9/brief-S-3a-scheduler.md s4 S2): the one lane-job adapter (core/LaneSpareWork.h) serving the core-first slot AND the gap from
// the same JobDecl, gap eligibility (backfill only in core_first), the static classes before S3, and P10's class following its proof (Q4).
// Pure CPU with a fake runner. S2-T6 (nested Acquire P10g -> P10) waits for L-B2's P10g form; the AnalysisLane registration move waits for the
// hot-file lanes (brief s4 S2 risks).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/AdmissionTable.h>
#include <VP-V/core/JobRegistry.h>
#include <VP-V/core/LaneSpareWork.h>

#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpSkeleton;

namespace SchedulerS2Test
{
	namespace
	{
		// One product's lane state: a job per source frame, finished after `chunksPerJob` Steps.
		struct FakeRunner
		{
			bool laned = true;
			uint32_t predictUs = 400;
			int chunksPerJob = 3;
			int chunk = 0;
			uint64_t pending = 0;               // the frame whose job is open (0 = none)
			std::map<uint64_t, int> published;  // publishes per frame
			std::vector<uint32_t> budgets;      // the budget of each Step
			std::vector<ProductId>* log = nullptr;

			void NewFrame(uint64_t f) { pending = f; chunk = 0; }
			bool Laned(ProductId) const { return laned; }
			uint32_t PredictChunkUs(ProductId) const { return predictUs; }
			StepOutcome Step(ProductId id, uint32_t budgetUs)
			{
				budgets.push_back(budgetUs);
				if (pending == 0) return StepOutcome::WaitingSource;
				if (log) log->push_back(id);
				if (++chunk >= chunksPerJob)
				{
					++published[pending];
					pending = 0;
					return StepOutcome::Published;
				}
				return StepOutcome::Ran;
			}
			int MaxPerFrame() const
			{
				int m = 0;
				for (const auto& p : published) m = p.second > m ? p.second : m;
				return m;
			}
		};

		using Work = BasicLaneSpareWork<FakeRunner>;

		JobDecl DeclOf(ProductId id, JobClass cls, uint32_t order)
		{
			JobDecl d;
			d.product = id;
			d.cls = cls;
			d.graphOrder = order;
			d.splittable = false;
			d.cpu = false;
			return d;
		}

		struct Clock
		{
			uint64_t us = 0;
			std::function<uint64_t()> Fn() { return [this] { us += 100; return us; }; }
		};
	}

	TEST_CLASS(SchedulerS2Tests)
	{
	public:
		// S2-T1: in core_first no Core / Derived / Slice / Unpack job is offered to the gap; shared_gap (R10) offers all of them.
		TEST_METHOD(GapCarriesOnlyBackfillInCoreFirst)
		{
			const JobClass all[] = { JobClass::Unpack, JobClass::Core, JobClass::Derived, JobClass::Slice, JobClass::Fill };
			for (JobClass c : all)
			{
				const JobDecl d = DeclOf(kP11RegionBlockStats, c, 0);
				Assert::AreEqual(c == JobClass::Fill, GapEligible(d, ScheduleMode::CoreFirst));
				Assert::IsTrue(GapEligible(d, ScheduleMode::SharedGap));
			}
			// the lane products of the fixed core set are never gap work in core_first
			const ProductId core[] = { kP1HalfLuma, kP3AnalysisField, kP4SourceStats, kP11RegionBlockStats, kCodingEvidence, kQualityPattern };
			for (ProductId id : core)
			{
				const StaticClass s = StaticClassOf(id);
				Assert::IsTrue(s.cls == JobClass::Core);
				Assert::IsFalse(GapEligible(DeclOf(id, s.cls, 0), ScheduleMode::CoreFirst));
			}
		}

		// S2-T2: one slot Run per source frame publishes each lane product once per frame (computed <= 1).
		TEST_METHOD(EachLaneProductComputedOncePerFrame)
		{
			Clock clock;
			FakeRunner p11, p16;
			p11.predictUs = p16.predictUs = 0;   // PRE-C2 fixture: no device-cost prediction, so each job is genuinely unmeasured until its first run
			Work w11(&p11, DeclOf(kP11RegionBlockStats, JobClass::Core, 4), clock.Fn());
			Work w16(&p16, DeclOf(kCodingEvidence, JobClass::Core, 5), clock.Fn());
			SpareHost host;
			host.Slot().Register(w11.AsSpare());
			host.Slot().Register(w16.AsSpare());
			for (uint64_t f = 1; f <= 8; ++f)
			{
				p11.NewFrame(f);
				p16.NewFrame(f);
				w11.Wake();
				w16.Wake();
				for (int present = 0; present < 3; ++present)
				{
					host.NotePresented(f, present > 0);
					host.SetBudget(20.0);
					host.Fire(SpareTrigger::PostPresentNoExtras);
				}
			}
			Assert::AreEqual(1, p11.MaxPerFrame());
			Assert::AreEqual(1, p16.MaxPerFrame());
			Assert::AreEqual(size_t(8), p11.published.size());
			// frame 1: both unmeasured, so only the first runs and P16 is promoted first in frame 2 (PostPresentSpare S4 rule); its frame-1 job
			// was superseded by frame 2 (the runner's skip, counted there) - never run twice
			Assert::AreEqual(size_t(7), p16.published.size());
			for (uint32_t b : p11.budgets) Assert::AreEqual(uint32_t(Work::kWholeJobBudgetUs), b, L"the slot asks for the whole job");
		}

		// S2-T3: a Derived-ahead job runs right after its inputs in the slot (graph order inside the Core class); Derived-live (P12) has no job.
		TEST_METHOD(DerivedAheadRunsAfterInputsDerivedLiveIsNoJob)
		{
			Clock clock;
			std::vector<ProductId> log;
			FakeRunner p3, p3r, fill;
			p3.predictUs = p3r.predictUs = fill.predictUs = 0;   // PRE-C2 fixture: no device-cost prediction, so each job is genuinely unmeasured until its first run
			p3.log = &log; p3r.log = &log; fill.log = &log;
			p3.chunksPerJob = p3r.chunksPerJob = fill.chunksPerJob = 1;
			Work wFill(&fill, DeclOf(kP5MonitorTiles, JobClass::Fill, 1), clock.Fn());
			Work wP3r(&p3r, DeclOf(kRefinedField, JobClass::Derived, 3), clock.Fn());
			Work wP3(&p3, DeclOf(kP3AnalysisField, JobClass::Core, 2), clock.Fn());
			SpareHost host;
			host.Slot().Register(wFill.AsSpare());
			host.Slot().Register(wP3r.AsSpare());
			host.Slot().Register(wP3.AsSpare());
			for (uint64_t f = 1; f <= 3; ++f)
			{
				p3.NewFrame(f); p3r.NewFrame(f); fill.NewFrame(f);
				wFill.Wake(); wP3r.Wake(); wP3.Wake();
				host.NotePresented(f, false);
				host.SetBudget(20.0);
				host.Fire(SpareTrigger::PostPresentNoExtras);
			}
			// once both are measured (frame 3), the order is P3 -> P3r (graph order inside the slot's Core class); the fill (lower graph index,
			// later class) never runs ahead of them
			Assert::IsTrue(log.size() >= 2u);
			const size_t n = log.size();
			Assert::AreEqual(int(kP3AnalysisField), int(log[n - 2]));
			Assert::AreEqual(int(kRefinedField), int(log[n - 1]));
			for (size_t i = 0; i < n; ++i)
				if (log[i] == kP5MonitorTiles)
					Assert::IsTrue(i > 0, L"fill never first ahead of core");

			const StaticClass p12 = StaticClassOf(kP12LightLevel);
			Assert::IsTrue(p12.cls == JobClass::Derived);
			Assert::IsFalse(p12.jobbed, L"P12 is an Acquire live fill, not a job");
		}

		// PRE-C2 (review MUST): with every runner measured (predictUs 400) and a budget that fits all three, the Fill runs every frame and only
		// after both the Core job (P3) and the Derived job (P3r) of that frame; P3 runs before P3r. Checked per frame.
		TEST_METHOD(FillRunsOnlyAfterCoreAndDerivedWhenMeasured)
		{
			Clock clock;
			std::vector<ProductId> log;
			FakeRunner p3, p3r, fill;
			p3.log = &log; p3r.log = &log; fill.log = &log;
			p3.chunksPerJob = p3r.chunksPerJob = fill.chunksPerJob = 1;
			Work wFill(&fill, DeclOf(kP5MonitorTiles, JobClass::Fill, 1), clock.Fn());
			Work wP3r(&p3r, DeclOf(kRefinedField, JobClass::Derived, 3), clock.Fn());
			Work wP3(&p3, DeclOf(kP3AnalysisField, JobClass::Core, 2), clock.Fn());
			SpareHost host;
			host.Slot().Register(wFill.AsSpare());
			host.Slot().Register(wP3r.AsSpare());
			host.Slot().Register(wP3.AsSpare());
			for (uint64_t f = 1; f <= 4; ++f)
			{
				p3.NewFrame(f); p3r.NewFrame(f); fill.NewFrame(f);
				wFill.Wake(); wP3r.Wake(); wP3.Wake();
				const size_t start = log.size();
				host.NotePresented(f, false);
				host.SetBudget(20.0);
				host.Fire(SpareTrigger::PostPresentNoExtras);
				Assert::AreEqual(size_t(3), log.size() - start, L"all three jobs run in this frame");
				const size_t none = static_cast<size_t>(-1);
				size_t at3 = none, at3r = none, atFill = none;
				for (size_t i = start; i < log.size(); ++i)
				{
					if (log[i] == kP3AnalysisField) at3 = i;
					else if (log[i] == kRefinedField) at3r = i;
					else if (log[i] == kP5MonitorTiles) atFill = i;
				}
				Assert::IsTrue(at3 != none && at3r != none && atFill != none, L"every job ran this frame (the Fill included)");
				Assert::IsTrue(at3 < at3r, L"P3 runs before P3r");
				Assert::IsTrue(atFill > at3 && atFill > at3r, L"the Fill runs after both the Core and the Derived job of this frame");
			}
		}

		// S2-T4: P10 is optional fill until <= 2 ms at 4K is proven, then core (doc 91 C :119; Q4).
		TEST_METHOD(P10ClassFollowsProof)
		{
			Admission::AdmissionFacts f;
			Assert::IsTrue(Admission::ClassOf(kP10GrainSplit, f).cls == JobClass::Fill);
			f.p10Proven = true;
			Assert::IsTrue(Admission::ClassOf(kP10GrainSplit, f).cls == JobClass::Core);
			Assert::IsTrue(Admission::ClassOf(kP10cGrainSplitChroma, f).cls == JobClass::Fill, L"only P10 is promoted");
		}

		// C4 s3a (owner OD:391, 2026-11-11 07:45; 115 #47 / #59): every ProductId below kSkeletonProductEnd has an EXPLICIT class (no default
		// class in StaticClassOf), checked against this test's own expected table, which itself fails on any id it does not list. The TD
		// products are Slice (the TD slice's jobs), never optional fill.
		TEST_METHOD(EveryProductIdHasAnExplicitClassTdIsSlice)
		{
			for (ProductId id = 0; id < kSkeletonProductEnd; ++id)
			{
				const std::wstring at = L"id " + std::to_wstring(id);
				JobClass want = JobClass::Unpack;
				bool jobbed = true;
				switch (id)
				{
				case kP1HalfLuma: case kP2VectorPair: case kP2mMatcherPair: case kP3AnalysisField: case kP4SourceStats:
				case kP7CameraFit: case kP8CameraLabels: case kP11RegionBlockStats: case kQualityPattern: case kCodingEvidence:
				case kCameraFitField:   // CP-A1-D4-CAMLABEL-FIELDCOPY: P7's fit field is published with P7 (core set)
					want = JobClass::Core;
					break;
				case kRefinedField:
					want = JobClass::Derived;
					break;
				case kP12LightLevel:
					want = JobClass::Derived;
					jobbed = false;
					break;
				case kTdBlocks: case kTdMemory: case kTdAnchorPass: case kTdAnchorRefs: case kTdDelta: case kTdMaps: case kTdTrailSummary:
					want = JobClass::Slice;
					break;
				case kP0RebaseDecision: case kP5MonitorTiles: case kP6EntranceAhead: case kP7gCameraGuide: case kP9CameraSmoothed:
				case kP10GrainSplit: case kP10cGrainSplitChroma: case kP13SourceRing: case kSourceIdentity: case kSourceGeometry:
				case kQualityPatternCausal: case kBandMemo: case kEdgeMap: case kFocusMap: case kRoomLight: case kComposedVectors:
					want = JobClass::Fill;
					break;
				default:
					Assert::Fail((at + L": no expected class - class the new id explicitly in StaticClassOf and here").c_str());
				}
				const StaticClass s = StaticClassOf(id);
				Assert::IsTrue(s.classed, (at + L": classed explicitly, never by a default").c_str());
				Assert::IsTrue(s.cls == want, (at + L": class").c_str());
				Assert::AreEqual(jobbed, s.jobbed, (at + L": jobbed").c_str());
				if (want == JobClass::Slice)
					Assert::IsFalse(GapEligible(DeclOf(id, s.cls, 0), ScheduleMode::CoreFirst), (at + L": a TD job is never core_first gap fill").c_str());
			}
			// an id outside the table is visibly unclassed and never a job (never admitted)
			Assert::IsFalse(StaticClassOf(kSkeletonProductEnd).classed);
			Assert::IsFalse(StaticClassOf(kSkeletonProductEnd).jobbed);
			Assert::IsFalse(StaticClassOf(kNoProduct).classed);
		}

		// S2-T5: the Original pathway registers no lane job either.
		TEST_METHOD(OriginalRegistersNoLaneJob)
		{
			Graph g;
			g.valid = true;
			Node n;
			n.id = kP11RegionBlockStats;
			g.nodes.push_back(n);
			Clock clock;
			FakeRunner r;
			Work w(&r, DeclOf(kP11RegionBlockStats, JobClass::Core, 0), clock.Fn());
			SpareHost host;
			Jobs::JobRegistry reg;
			reg.SetOriginal(true);
			Assert::IsFalse(reg.Register(host, g, w.Decl(), w.AsSpare()));
			Assert::AreEqual(size_t(0), host.Slot().WorkCount());
			reg.SetOriginal(false);
			Assert::IsTrue(reg.Register(host, g, w.Decl(), w.AsSpare()));
		}

		// The same object is the gap work (R10): the gap face keeps today's LaneComputeGapWork law (decay on capNoFit), the slot face never decays.
		TEST_METHOD(GapFaceKeepsTodaysLawSlotFaceNeverDecays)
		{
			Clock clock;
			FakeRunner r;
			r.chunksPerJob = 2;
			Work w(&r, DeclOf(kP11RegionBlockStats, JobClass::Core, 0), clock.Fn());
			IGapWork* gap = w.AsGap();
			ISpareWork* spare = w.AsSpare();
			Assert::IsTrue(gap->Class() == GapClass::LaneCompute);
			Assert::IsTrue(spare->Class() == SpareClass::Core);
			Assert::AreEqual(int(kP11RegionBlockStats), int(gap->Id()));
			Assert::AreEqual(int(kP11RegionBlockStats), int(spare->Id()));

			r.NewFrame(1);
			gap->RunChunk(1000);
			Assert::AreEqual(1000u, r.budgets.back(), L"the gap passes its chunk budget");
			Assert::AreEqual(400u, gap->PredictUs(), L"max(measured CPU 100 us, device 400 us)");
			r.predictUs = 0;
			Assert::AreEqual(100u, gap->PredictUs());
			gap->OnCapNoFit();
			Assert::AreEqual(50u, gap->PredictUs(), L"gap law: halves on capNoFit");

			r.predictUs = 3000;
			r.NewFrame(2);
			w.Wake();
			const double ms = spare->Run(0);
			Assert::IsTrue(ms >= 6.0 - 1e-9, L"slot cost = max(CPU, device prediction per step) = 2 x 3 ms");
			const double before = spare->PredictMs();
			r.NewFrame(3);
			Assert::AreEqual(before, spare->PredictMs(), 1e-12, L"R4: nothing decays the slot cost");
		}
	};
}
