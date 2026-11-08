/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The chain estimator worker (SID-C2b, identity/chain/ChainEstimator.h): the SourceFrameTap sink and its due predicate, the line copy,
// determinism (same lines + times -> byte-identical records), the CPU-share governor's cadence, the bounded newest-wins queue, events and
// discontinuities, clean start / stop / reset (10k race rounds, no deadlock with the monitor's own mutex), replay of a dumped ring,
// the evidence log never writing from the caller's thread. CPU only, synthetic planes, fixed seeds, injected clocks, an in-memory
// evidence folder. The numeric quality of the verdicts is the C1 units' business (ChainUnitTests), not tested here.
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		using namespace ChainFx;
		namespace Tap = VpFrameAnalysis::SourceFrameTap;

		constexpr int kW = 320, kH = 240;

		inline void StartShadow(C::ChainEstimator& e)
		{
			e.SetSynchronous(true);
			e.SetMode(C::EstimatorMode::Shadow);
		}

		inline G::Record Newest(C::ChainEstimator& e)
		{
			G::Record r;
			Assert::IsTrue(e.Held().ReadLatest(0, r), L"a record was published");
			return r;
		}
	}

	TEST_CLASS(ChainEstimatorTests)
	{
	public:
		TEST_METHOD(LineSnapshotEqualsTheSamplerFillAndHashesTheContent)
		{
			const Plane plane = MakePlane(400, 300, 3, 4);
			C::LineRect active;
			active.x = 0; active.y = 0; active.w = 400; active.h = 300;
			const C::LinePlan plan = C::LineSampler::Plan(active, 7);
			Assert::IsTrue(plan.valid);
			C::LineSnapshot snap;
			Assert::IsTrue(C::CaptureLines<uint16_t>(plan, plane.luma.data(), plane.w, 1.0, snap));
			for (int axis = 0; axis < 2; ++axis)
			{
				C::LineSet a, b;
				Assert::IsTrue(C::LineSampler::Fill<uint16_t>(plan, axis, plane.luma.data(), plane.w, 1.0, a));
				Assert::IsTrue(C::ToLineSet(snap, axis, b));
				Assert::AreEqual(a.axis, b.axis);
				Assert::AreEqual(a.axisLen, b.axisLen);
				Assert::AreEqual(a.origin, b.origin);
				Assert::AreEqual(a.length, b.length);
				Assert::AreEqual(a.count, b.count);
				Assert::IsTrue(a.data == b.data, L"the snapshot path gives the sampler's doubles");
				Assert::IsTrue(a.cross == b.cross);
			}
			const C::LinePlan other = C::LineSampler::Plan(active, 8);
			C::LineSnapshot snap2;
			Assert::IsTrue(C::CaptureLines<uint16_t>(other, plane.luma.data(), plane.w, 1.0, snap2));
			Assert::IsTrue(C::SnapshotHash(snap) != C::SnapshotHash(snap2), L"another sequence samples other lines");
			Assert::IsTrue(C::SnapshotHash(snap) == C::SnapshotHash(snap), L"the hash is a pure function");
			Assert::AreEqual(4, C::DetectStepCodes(snap), L"an 8-bit picture carried in 10-bit codes");
			const Plane ten = MakePlane(400, 300, 3, 1);
			C::LineSnapshot snap10;
			Assert::IsTrue(C::CaptureLines<uint16_t>(plan, ten.luma.data(), ten.w, 1.0, snap10));
			Assert::AreEqual(1, C::DetectStepCodes(snap10));
		}

		TEST_METHOD(SameLinesAndTimesGiveByteIdenticalRecords)
		{
			std::vector<Plane> planes;
			for (int i = 0; i < 4; ++i)
				planes.push_back(MakePlane(kW, kH, 10 + static_cast<uint32_t>(i), 4));
			std::vector<G::Packed> runs[2];
			for (int run = 0; run < 2; ++run)
			{
				ManualClock clk;
				C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
				StartShadow(est);
				for (int i = 0; i < 4; ++i)
				{
					clk.Set(1100.0 * i);
					Feed(est, planes[static_cast<size_t>(i)], 100u + 30u * static_cast<uint64_t>(i), 10.0 + 1.1 * i);
					runs[run].push_back(G::Pack(Newest(est)));
				}
				Assert::AreEqual<uint64_t>(4, est.GetCounters().evaluations);
			}
			for (size_t i = 0; i < 4; ++i)
				Assert::IsTrue(SameBytes(runs[0][i], runs[1][i]), L"record bytes are a pure function of the lines and times");
			G::Record last;
			Assert::IsTrue(G::Unpack(runs[0][3], last));
			Assert::AreEqual<uint64_t>(190, last.evalSeq);
			Assert::AreEqual<int64_t>(static_cast<int64_t>(std::llround((10.0 + 1.1 * 3) * 1e6)), last.evalTimeUs);
			Assert::AreEqual<uint64_t>(4, last.publishSerial);
			Assert::IsTrue((last.flags & G::kFlagShadow) != 0, L"shadow mode is flagged in the record");
			Assert::IsTrue(last.maxAgeUs > 0, L"the staleness bound is derived from the evaluation interval");
		}

		TEST_METHOD(AWantedFrameIsOnlyDueAfterTheGovernorInterval)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 1, 4);
			Assert::IsTrue(est.Wants(), L"nothing evaluated yet: due");
			Feed(est, plane, 10, 1.0);
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations);
			Assert::IsFalse(est.Wants(), L"just evaluated: not due");
			clk.Set(999.0);
			Assert::IsFalse(est.Wants());
			clk.Set(1000.0);
			Assert::IsTrue(est.Wants(), L"the acquiring interval is 1 s (zero cost on the manual clock)");
			const C::GovernorPlan plan = est.GetGovernorPlan();
			Assert::AreEqual(1.0, plan.intervalSec, 1e-9);
			Assert::IsFalse(plan.stretched);
		}

		TEST_METHOD(AnExpensiveEvaluationStretchesTheCadenceVisibly)
		{
			TickClock tick;
			tick.step = 2000.0;   // every clock read costs 2 s: the evaluation takes at least 2 s of "wall" time
			C::ChainEstimator est(C::EstimatorSettings(), tick.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 1, 4);
			Feed(est, plane, 10, 1.0);
			const C::GovernorPlan plan = est.GetGovernorPlan();
			Assert::IsTrue(plan.costMs >= 2000.0);
			Assert::IsTrue(plan.stretched, L"cost / 15% share exceeds the 1 s nominal");
			Assert::IsTrue(plan.overBudget);
			Assert::IsTrue(plan.intervalSec >= 2.0 / 0.15 - 1e-6);
			Assert::IsTrue(est.DetailText().find("STRETCHED") != std::string::npos, L"Ctrl+Alt+D says so");
		}

		TEST_METHOD(TheQueueIsBoundedAndTheNewestFrameWins)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());   // a real worker thread
			est.SetMode(C::EstimatorMode::Shadow);
			est.PauseForTest(true);
			const Plane plane = MakePlane(kW, kH, 2, 4);
			Feed(est, plane, 10, 1.0);
			clk.Set(2000.0);
			Feed(est, plane, 70, 2.0);
			const C::EstimatorCounters held = est.GetCounters();
			Assert::AreEqual<uint64_t>(2, held.delivered);
			Assert::AreEqual<uint64_t>(1, held.dropped, L"the older job was replaced, and counted");
			Assert::AreEqual<uint64_t>(0, held.evaluations);
			est.PauseForTest(false);
			Assert::IsTrue(est.WaitIdle(30000));
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations);
			Assert::AreEqual<uint64_t>(70, Newest(est).evalSeq, L"the newest job is the one that ran");
		}

		TEST_METHOD(AnEventIsConsumedByTheFirstEvaluationAfterIt)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 4, 4);
			est.NoteEvent(C::EstimatorEvent::Cut, 120);
			est.NoteEvent(C::EstimatorEvent::Blank, 125);
			Feed(est, plane, 100, 1.0);   // before both events
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags);
			clk.Set(1100.0);
			Feed(est, plane, 130, 2.0);   // after both
			Assert::AreEqual<uint32_t>(C::kEvCut | C::kEvBlank, est.StatusInput().last.flags);
			clk.Set(2200.0);
			Feed(est, plane, 160, 3.0);
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags, L"an event is consumed once");
		}

		TEST_METHOD(RepeatsAndFramesWithoutATimeAreSkippedAndCounted)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 5, 4);
			est.Deliver(View(plane, 5));   // no NoteFrame: no source time
			Assert::AreEqual<uint64_t>(1, est.GetCounters().noClock);
			Assert::AreEqual<uint64_t>(0, est.GetCounters().delivered);
			est.NoteFrame(5, 1.0, 0, 0, 0, 0);
			est.NoteRepeat(5);
			est.Deliver(View(plane, 5));
			Assert::AreEqual<uint64_t>(1, est.GetCounters().repeatsSkipped);
			Assert::AreEqual<uint64_t>(0, est.GetCounters().evaluations);
			Assert::IsTrue(est.Wants(), L"a skipped repeat keeps the due slot");
			Feed(est, plane, 6, 1.0167);
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations);
		}

		TEST_METHOD(ASourceTimeBeyondTheNewestClockEntryIsExtrapolated)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 6, 4);
			est.NoteFrame(10, 1.00, 0, 0, 0, 0);
			est.NoteFrame(11, 1.02, 0, 0, 0, 0);
			est.Deliver(View(plane, 13));   // two frames beyond the newest, 0.02 s per frame
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations);
			const int64_t t = Newest(est).evalTimeUs;
			Assert::IsTrue(t > 1055000 && t < 1065000, L"1.02 + 2 x 0.02 s");
		}

		TEST_METHOD(ADiscontinuityResetsEverythingAndPublishesAResetRecord)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 7, 4);
			Feed(est, plane, 10, 1.0);
			clk.Set(1100.0);
			Feed(est, plane, 70, 2.0);
			const G::Record before = Newest(est);
			est.NoteDiscontinuity("test resync");
			const G::Record after = Newest(est);
			Assert::AreEqual<uint32_t>(before.discontinuities + 1u, after.discontinuities);
			for (int a = 0; a < 2; ++a)
			{
				Assert::IsTrue(after.measured[a].state == G::LatchState::Unknown);
				Assert::IsTrue(after.applied[a].state == G::LatchState::Unknown);
			}
			Assert::AreEqual<uint64_t>(1, est.GetCounters().discontinuities);
			Assert::IsTrue(est.Wants(), L"acquisition restarts at once");
		}

		TEST_METHOD(ADiscontinuityDropsQueuedJobsOfTheOldSource)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			est.SetMode(C::EstimatorMode::Shadow);
			est.PauseForTest(true);
			const Plane plane = MakePlane(kW, kH, 8, 4);
			Feed(est, plane, 10, 1.0);
			est.NoteDiscontinuity("seek");
			est.PauseForTest(false);
			Assert::IsTrue(est.WaitIdle(30000));
			Assert::AreEqual<uint64_t>(0, est.GetCounters().evaluations, L"the old source's frame is never evaluated");
			Assert::AreEqual<uint64_t>(1, est.GetCounters().discontinuities);
			G::Record r;
			Assert::IsFalse(est.Held().ReadLatest(0, r), L"nothing was evaluated, so there is no reset record either");
		}

		TEST_METHOD(AStreamKeyChangeIsADiscontinuityAndIsStamped)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 9, 4);
			est.SetStreamKey(1);
			Feed(est, plane, 10, 1.0);
			Assert::AreEqual<uint64_t>(1, Newest(est).streamKey);
			Assert::AreEqual<uint64_t>(0, est.GetCounters().discontinuities, L"the first key is not a change");
			est.SetStreamKey(2);
			Assert::AreEqual<uint64_t>(1, est.GetCounters().discontinuities);
			clk.Set(1100.0);
			Feed(est, plane, 70, 2.0);
			Assert::AreEqual<uint64_t>(2, Newest(est).streamKey);
		}

		TEST_METHOD(AChangedActiveAreaRestartsTheReadingAtTheFirstFrameOfTheNewArea)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 11, 4);
			est.NoteFrame(10, 1.0, 0, 0, 0, 0);
			est.Deliver(View(plane, 10));
			const uint32_t d0 = Newest(est).discontinuities;
			clk.Set(1100.0);
			est.NoteFrame(70, 2.0, 0, 16, kW, kH - 32);   // a letterbox appeared
			est.Deliver(View(plane, 70));
			const G::Record r = Newest(est);
			Assert::AreEqual<uint32_t>(d0 + 1u, r.discontinuities);
			Assert::AreEqual(kH - 32, static_cast<int>(r.area.h));
			Assert::IsTrue(r.measured[0].state == G::LatchState::Unknown);
		}

		TEST_METHOD(OffStopsTheWorkAndOnStartsClean)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 12, 4);
			Feed(est, plane, 10, 1.0);
			Assert::IsTrue(est.Running());
			est.SetMode(C::EstimatorMode::Off);
			Assert::IsFalse(est.Running());
			Assert::IsFalse(est.Wants());
			G::Record r;
			Assert::IsFalse(est.Held().ReadLatest(0, r), L"a record from before an off period never reads as fresh");
			const uint64_t delivered = est.GetCounters().delivered;
			clk.Set(5000.0);
			Feed(est, plane, 20, 2.0);
			Assert::AreEqual<uint64_t>(delivered, est.GetCounters().delivered, L"off: the sink does nothing");
			est.SetMode(C::EstimatorMode::On);
			clk.Set(9000.0);
			Feed(est, plane, 30, 3.0);
			const G::Record fresh = Newest(est);
			Assert::AreEqual<uint64_t>(1, fresh.publishSerial, L"a clean start");
			Assert::AreEqual<uint32_t>(0u, fresh.discontinuities);
			Assert::IsTrue((fresh.flags & G::kFlagShadow) == 0, L"On is not shadow");
		}

		TEST_METHOD(TheOriginalPathwayForcesTheEstimatorOff)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			est.SetOriginalPathway(true);
			Assert::IsFalse(est.Running());
			Assert::IsFalse(est.Wants());
			Assert::IsTrue(est.StatusLine().find("Original pathway") != std::string::npos);
			est.SetOriginalPathway(false);
			Assert::IsTrue(est.Running());
		}

		TEST_METHOD(TheSinkRegistersWithTheTapAndTheSkeletonSinkIsUnchanged)
		{
			int key = 0;
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			est.SetSynchronous(true);
			int skeletonFrames = 0;
			Tap::Register(&key, [&skeletonFrames](const VpSkeleton::SourceFrameView&) { ++skeletonFrames; });   // the old API: the name ""
			Assert::IsTrue(est.AttachTap(&key));
			Assert::IsFalse(Tap::Registered(&key, "chain"), L"Off: no chain sink");
			est.SetMode(C::EstimatorMode::Shadow);
			Assert::IsTrue(Tap::Registered(&key, "chain"));
			Assert::IsTrue(Tap::Registered(&key, ""));
			const Plane plane = MakePlane(kW, kH, 13, 4);
			est.NoteFrame(10, 1.0, 0, 0, 0, 0);
			Assert::IsTrue(Tap::Wants(&key));
			Assert::IsTrue(Tap::Deliver(&key, View(plane, 10)));
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations);
			est.NoteFrame(11, 1.02, 0, 0, 0, 0);
			Assert::IsTrue(Tap::Deliver(&key, View(plane, 11)), L"the skeleton sink still takes every frame");
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations, L"the chain sink did not want the second one (not due)");
			Assert::AreEqual(2, skeletonFrames);
			Assert::AreEqual<uint64_t>(1, Tap::Delivered(&key, "chain"));
			Assert::AreEqual<uint64_t>(2, Tap::Delivered(&key, ""));
			est.SetMode(C::EstimatorMode::Off);
			Assert::IsFalse(Tap::Registered(&key, "chain"), L"Off unregisters the chain sink");
			Assert::IsTrue(Tap::Registered(&key, ""), L"and leaves the skeleton's alone");
			Tap::Unregister(&key);
			est.DetachTap();
			Assert::IsFalse(Tap::AnyWants());
		}

		TEST_METHOD(ADumpedRingReplaysToTheSameRecordBytes)
		{
			const std::string folder = "mem\\sid-evidence";
			std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
			std::vector<Plane> planes;
			for (int i = 0; i < 5; ++i)
				planes.push_back(MakePlane(kW, kH, 30 + static_cast<uint32_t>(i), 4));
			G::Packed live;
			{
				ManualClock clk;
				C::ChainEstimator est(C::EstimatorSettings(), clk.Fn(), fs, FixedStamp());
				est.SetSynchronous(true);
				est.SetEvidence(folder, C::EvidenceLevel::Lines);
				est.SetMode(C::EstimatorMode::Shadow);
				est.NoteEvent(C::EstimatorEvent::Cut, 95);
				for (int i = 0; i < 5; ++i)
				{
					clk.Set(1100.0 * i);
					Feed(est, planes[static_cast<size_t>(i)], 100u + 60u * static_cast<uint64_t>(i), 5.0 + 1.0 * i);
				}
				Assert::AreEqual<uint64_t>(5, est.GetCounters().evaluations);
				live = G::Pack(Newest(est));
				est.RequestEvidenceDump("replay-test");
			}
			const std::string name = fs->FirstWithSuffix(folder, ".sidl");
			Assert::IsFalse(name.empty(), L"the hotkey wrote the ring");
			std::string header;
			std::vector<C::LineSnapshot> snaps;
			Assert::IsTrue(C::ParseDump(fs->Get(folder, name), header, snaps));
			Assert::IsTrue(header.find("sid-lines-dump") != std::string::npos);
			Assert::AreEqual<size_t>(5, snaps.size());
			Assert::AreEqual<uint32_t>(C::kEvCut, snaps[0].flags, L"the consumed event travels with the lines");

			ManualClock clk2;
			C::ChainEstimator replay(C::EstimatorSettings(), clk2.Fn());
			StartShadow(replay);
			for (const C::LineSnapshot& s : snaps)
				Assert::IsTrue(replay.SubmitLines(s));
			Assert::AreEqual<uint64_t>(5, replay.GetCounters().evaluations);
			Assert::IsTrue(SameBytes(live, G::Pack(Newest(replay))), L"an exact replay of the dumped lines");
		}

		TEST_METHOD(TheEvidenceLogNeverWritesOnTheCallersThread)
		{
			const std::string folder = "mem\\sid-evidence";
			std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn(), fs, FixedStamp());   // a real worker thread
			est.SetEvidence(folder, C::EvidenceLevel::Lines);
			est.SetMode(C::EstimatorMode::Shadow);
			const Plane plane = MakePlane(kW, kH, 14, 4);
			for (int i = 0; i < 3; ++i)
			{
				clk.Set(1100.0 * i);
				Feed(est, plane, 10u + 60u * static_cast<uint64_t>(i), 1.0 + i);
				Assert::IsTrue(est.WaitIdle(30000));
			}
			est.RequestEvidenceDump("hotkey");
			Assert::IsTrue(est.WaitIdle(30000));
			Assert::IsTrue(fs->WriteCount() > 0, L"rows and a dump were written");
			Assert::IsFalse(fs->AnyWriteFrom(std::this_thread::get_id()), L"the render side only copies and notes: the worker does the I/O");
			Assert::IsFalse(fs->FirstWithSuffix(folder, ".sidl").empty());
			Assert::IsFalse(fs->FirstWithSuffix(folder, ".jsonl").empty());
		}

		TEST_METHOD(TheEvidenceLogIsOffInTheOriginalPathwayAndWhenTheEstimatorIsOff)
		{
			const std::string folder = "mem\\sid-evidence";
			std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn(), fs, FixedStamp());
			est.SetSynchronous(true);
			est.SetEvidence(folder, C::EvidenceLevel::Lines);
			const Plane plane = MakePlane(kW, kH, 15, 4);
			Feed(est, plane, 10, 1.0);   // Off: nothing runs
			Assert::AreEqual<size_t>(0, fs->FileCount());
			est.SetMode(C::EstimatorMode::Shadow);
			est.SetOriginalPathway(true);
			clk.Set(5000.0);
			Feed(est, plane, 20, 2.0);
			Assert::AreEqual<size_t>(0, fs->FileCount(), L"Original: no evidence either");
			Assert::IsTrue(est.EvidenceStatusText().find("off") != std::string::npos);
			est.SetOriginalPathway(false);
			clk.Set(9000.0);
			Feed(est, plane, 30, 3.0);
			Assert::IsTrue(fs->FileCount() > 0, L"back on: it logs again");
			std::string notice;
			Assert::IsTrue(est.TakeNotice(notice));
			Assert::IsTrue(notice.find("evidence logging on (shadow)") != std::string::npos, L"a visible one-time notice");
		}

		// ---- SID-C2b round-2 audit fixes ----------------------------------------------------------------------------------------
		TEST_METHOD(AnEventConsumedByADroppedJobMovesToTheJobThatReplacedIt)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());   // a real worker thread
			est.SetMode(C::EstimatorMode::Shadow);
			est.PauseForTest(true);
			const Plane plane = MakePlane(kW, kH, 41, 4);
			est.NoteEvent(C::EstimatorEvent::Cut, 5);
			est.NoteEvent(C::EstimatorEvent::Blank, 6);
			Feed(est, plane, 10, 1.0);   // consumes both events; this job is replaced below
			clk.Set(2000.0);
			Feed(est, plane, 70, 2.0);   // no event of its own
			Assert::AreEqual<uint64_t>(1, est.GetCounters().dropped);
			est.PauseForTest(false);
			Assert::IsTrue(est.WaitIdle(30000));
			Assert::AreEqual<uint64_t>(1, est.GetCounters().evaluations);
			Assert::AreEqual<uint32_t>(C::kEvCut | C::kEvBlank, est.StatusInput().last.flags, L"the first evaluation after the drop carries the cut and the blank");
		}

		TEST_METHOD(ALateEventIsCarriedByTheNextEvaluation)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 42, 4);
			est.NoteFrame(1, 1.0, 0, 0, 0, 0);
			est.NoteFrame(5, 1.4, 0, 0, 0, 0);
			est.NoteFrame(9, 1.8, 0, 0, 0, 0);
			est.Deliver(View(plane, 5));   // evaluated before the monitor's analysis of frame 4 reported its cut
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags);
			est.NoteEvent(C::EstimatorEvent::Cut, 4);   // late: the analysis worker reports it after the evaluation
			clk.Set(1100.0);
			est.Deliver(View(plane, 9));
			Assert::AreEqual<uint32_t>(C::kEvCut, est.StatusInput().last.flags, L"the next evaluation carries it (the contract holds statistically)");
			clk.Set(2200.0);
			est.NoteFrame(13, 2.2, 0, 0, 0, 0);
			est.Deliver(View(plane, 13));
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags, L"and only once");
		}

		// ---- Revert window: ClearWindow events ------------------------------------------------------------------------------------
		TEST_METHOD(AClearWindowEventIsConsumedByTheFirstEvaluationAfterItAndNeitherABlankNorACutClears)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 43, 4);
			est.NoteEvent(C::EstimatorEvent::Cut, 20);
			est.NoteEvent(C::EstimatorEvent::Blank, 21);
			Feed(est, plane, 30, 1.0);
			Assert::AreEqual<uint32_t>(C::kEvCut | C::kEvBlank, est.StatusInput().last.flags);
			Assert::AreEqual<uint64_t>(0, est.GetCounters().windowClears, L"a cut and a black frame clear nothing");
			clk.Set(1100.0);
			est.NoteEvent(C::EstimatorEvent::ClearWindow, 40);
			Feed(est, plane, 35, 2.0);   // before the event's frame
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags);
			Assert::AreEqual<uint64_t>(0, est.GetCounters().windowClears);
			clk.Set(2200.0);
			Feed(est, plane, 50, 3.0);   // after it
			Assert::AreEqual<uint32_t>(C::kEvClearWindow, est.StatusInput().last.flags, L"the clear travels with the evaluation (log flag)");
			Assert::AreEqual<uint64_t>(1, est.GetCounters().windowClears);
			Assert::AreEqual<uint64_t>(0, est.GetCounters().discontinuities, L"a window clear is not a reset: no reset record, the era goes on");
			clk.Set(3300.0);
			Feed(est, plane, 60, 4.0);
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags, L"consumed once");
			Assert::AreEqual<uint64_t>(1, est.GetCounters().windowClears);
		}

		TEST_METHOD(AClearWindowEventReachesAReplayedSnapshotButCutsAndBlanksDoNot)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 44, 4);
			C::LineRect active;
			active.x = 0; active.y = 0; active.w = kW; active.h = kH;
			const C::LinePlan plan = C::LineSampler::Plan(active, 9);
			Assert::IsTrue(plan.valid);
			C::LineSnapshot snap;
			Assert::IsTrue(C::CaptureLines<uint16_t>(plan, plane.luma.data(), plane.w, 1.0, snap));
			snap.seq = 10;
			snap.timeUs = 1000000;
			est.NoteEvent(C::EstimatorEvent::ClearWindow, 4);
			est.NoteEvent(C::EstimatorEvent::Cut, 4);
			est.NoteEvent(C::EstimatorEvent::ClearWindow, 99);   // a later frame: not consumed by this snapshot
			Assert::IsTrue(est.SubmitLines(snap));
			Assert::AreEqual<uint32_t>(C::kEvClearWindow, est.StatusInput().last.flags, L"the clear of an earlier frame is consumed; the replay carries its own cuts");
			Assert::AreEqual<uint64_t>(1, est.GetCounters().windowClears);
			snap.seq = 11;
			snap.timeUs = 2000000;
			snap.flags = 0;
			Assert::IsTrue(est.SubmitLines(snap));
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags, L"the later event is still pending");
			snap.seq = 100;
			snap.timeUs = 3000000;
			Assert::IsTrue(est.SubmitLines(snap));
			Assert::AreEqual<uint32_t>(C::kEvClearWindow, est.StatusInput().last.flags, L"and is consumed once its frame is reached");
			Assert::AreEqual<uint64_t>(2, est.GetCounters().windowClears);
		}

		TEST_METHOD(AnEventPullsTheNextEvaluationForwardWhileConfidentAtMostOncePerSecond)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 43, 4);
			Feed(est, plane, 10, 1.0);
			Assert::IsFalse(est.Wants());
			est.NoteEvent(C::EstimatorEvent::Cut, 11);
			Assert::IsFalse(est.Wants(), L"not confident: the acquiring cadence is already short, nothing is pulled");
			est.ForceLatchedPowerForTest(true);
			clk.Set(100.0);
			est.NoteEvent(C::EstimatorEvent::Cut, 12);
			Assert::IsTrue(est.Wants(), L"confident: a cut makes the next frame due at once");
			Feed(est, plane, 20, 2.0);   // evaluated at 100 ms (the evaluation itself recomputes the power flag)
			Assert::IsFalse(est.Wants());
			est.ForceLatchedPowerForTest(true);
			clk.Set(600.0);
			est.NoteEvent(C::EstimatorEvent::Cut, 21);
			Assert::IsFalse(est.Wants(), L"a second pull inside one second is refused");
		}

		// S1 (b) (round 1): while the monitor holds native-looking frames on the strength of this estimator's reading, the long cadence ends.
		TEST_METHOD(ANativeHeldNoteRestoresTheAcquiringCadenceWhileConfident)
		{
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			StartShadow(est);
			const Plane plane = MakePlane(kW, kH, 44, 4);
			Feed(est, plane, 10, 1.0);   // evaluated at 0 ms
			est.ForceLatchedPowerForTest(true);
			est.ForceNextDueForTest(5000.0);   // as a confident evaluation leaves it: the long interval
			clk.Set(1500.0);
			Assert::IsFalse(est.Wants(), L"fixture: the confident cadence is not due at 1.5 s");
			est.NoteNativeHeld(0u);
			Assert::IsTrue(est.LatchedPowerForTest() && !est.Wants(), L"an empty note changes nothing");
			est.NoteNativeHeld(1u);
			Assert::IsFalse(est.LatchedPowerForTest(), L"a held native-looking frame ends the confident regime");
			Assert::AreEqual(1.0, est.IntervalSec(), 1e-9, L"the acquiring interval (the governor's plan)");
			Assert::IsTrue(est.Wants(), L"the evaluation due at the long interval is pulled forward to one acquiring interval after the last start");
			est.ForceNextDueForTest(5000.0);
			est.NoteNativeHeld(2u);
			Assert::IsFalse(est.Wants(), L"already acquiring: a note pulls nothing forward");
			C::ChainEstimator idle(C::EstimatorSettings(), clk.Fn());   // never started: inert
			idle.ForceLatchedPowerForTest(true);
			idle.NoteNativeHeld(3u);
			Assert::IsTrue(idle.LatchedPowerForTest(), L"an estimator that does not run ignores the note");
		}

		TEST_METHOD(ABackToBackDiscontinuityPublishesAResetRecordAtOrAboveTheMonitorFloor)
		{
			const Plane plane = MakePlane(kW, kH, 44, 4);
			{
				ManualClock clk;
				C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
				StartShadow(est);
				Feed(est, plane, 10, 1.0);
				est.NoteFrame(20, 2.0, 0, 0, 0, 0);   // announced by the render thread, not delivered
				est.NoteFrame(30, 3.0, 0, 0, 0, 0);
				est.NoteDiscontinuity("resync");
				est.NoteDiscontinuity("format change");   // the monitor sends both at a FULL resync
				// The monitor's floor is the row sequence after its last analysed frame: at worst the newest announced frame + 1.
				Assert::IsTrue(Newest(est).evalSeq >= 31u, L"synchronous: the reset record is never below the floor");
			}
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());   // a real worker: the two calls coalesce into one reset
			est.SetMode(C::EstimatorMode::Shadow);
			Feed(est, plane, 10, 1.0);
			Assert::IsTrue(est.WaitIdle(30000));
			est.PauseForTest(true);
			est.NoteFrame(20, 2.0, 0, 0, 0, 0);
			est.NoteFrame(30, 3.0, 0, 0, 0, 0);
			est.NoteDiscontinuity("resync");
			est.NoteDiscontinuity("format change");
			est.PauseForTest(false);
			Assert::IsTrue(est.WaitIdle(30000));
			const G::Record r = Newest(est);
			Assert::AreEqual<uint64_t>(31, r.evalSeq, L"the second call keeps the first call's reset clock: the era's first sequence is one past the newest frame");
			Assert::AreEqual<uint64_t>(1, est.GetCounters().discontinuities);
		}

		TEST_METHOD(AHotkeyPressWhileOffOrAtLevelLogIsRefusedVisiblyNeverKeptForLater)
		{
			const std::string folder = "mem\\sid-evidence";
			std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn(), fs, FixedStamp());
			est.SetSynchronous(true);
			est.SetEvidence(folder, C::EvidenceLevel::Lines);
			est.RequestEvidenceDump("early press");   // the estimator is off
			std::string notice, all;
			while (est.TakeNotice(notice))
				all += notice + "\n";
			Assert::IsTrue(all.find("evidence dump ignored") != std::string::npos, L"a visible notice");
			est.SetMode(C::EstimatorMode::Shadow);
			const Plane plane = MakePlane(kW, kH, 45, 4);
			Feed(est, plane, 10, 1.0);
			Assert::AreEqual<size_t>(0, fs->CountWithSuffix(folder, ".sidl"), L"the early press does not dump an unrelated moment later");

			ManualClock clk2;
			C::ChainEstimator logOnly(C::EstimatorSettings(), clk2.Fn(), fs, FixedStamp());
			logOnly.SetSynchronous(true);
			logOnly.SetEvidence("mem\\other", C::EvidenceLevel::Log);
			logOnly.SetMode(C::EstimatorMode::Shadow);
			Feed(logOnly, plane, 10, 1.0);
			logOnly.RequestEvidenceDump("press at level log");
			all.clear();
			while (logOnly.TakeNotice(notice))
				all += notice + "\n";
			Assert::IsTrue(all.find("level log keeps no lines") != std::string::npos, L"said, not only a log row");
		}

		TEST_METHOD(AStatusLineNamesALostTapAndAStalledEstimator)
		{
			int key = 0;
			ManualClock clk;
			C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
			est.SetSynchronous(true);
			Assert::IsTrue(est.AttachTap(&key));
			est.SetMode(C::EstimatorMode::Shadow);
			const Plane plane = MakePlane(kW, kH, 46, 4);
			Feed(est, plane, 10, 1.0);
			Assert::IsTrue(est.StatusInput().tapAttached);
			Assert::IsFalse(est.StatusInput().tapLost);
			Assert::IsTrue(est.StatusLine().find("stalled") == std::string::npos && est.StatusLine().find("tap lost") == std::string::npos,
				L"a healthy line while frames flow");
			clk.Set(60000.0);   // a minute without a frame
			Assert::IsTrue(est.StatusLine().find("stalled (no frames for 60 s") != std::string::npos, L"a stopped worker is not shown as healthy");
			Tap::UnregisterAll(&key);   // the host tore its registrations down
			Assert::IsTrue(est.StatusInput().tapLost);
			Assert::IsFalse(est.StatusInput().tapAttached);
			Assert::IsTrue(est.StatusLine().find("frame tap lost") != std::string::npos);
			est.DetachTap();
		}
	};

	TEST_CLASS(ChainEstimatorThreadingTests)
	{
	public:
		TEST_METHOD(StartStopAndResetRacesTenThousandRoundsLeaveItCoherent)
		{
			TickClock tick;   // every read moves 1.1 s: every offered frame is due
			C::ChainEstimator est(C::EstimatorSettings(), tick.Fn());
			const Plane plane = MakePlane(kW, kH, 21, 4);
			std::thread toggler([&est]() {
				for (int i = 0; i < 10000; ++i)
				{
					est.SetMode((i & 1) != 0 ? C::EstimatorMode::On : C::EstimatorMode::Off);
					if (i % 7 == 0) est.NoteDiscontinuity("race");
					if (i % 11 == 0) est.SetStreamKey(static_cast<uint64_t>(i));
					if (i % 13 == 0) est.SetEvidence(std::string(), C::EvidenceLevel::Off);
					if (i % 17 == 0) est.SetOriginalPathway((i & 2) != 0);
				}
			});
			std::thread feeder([&est, &plane]() {
				for (int i = 0; i < 600; ++i)
				{
					est.NoteFrame(static_cast<uint64_t>(i), i / 60.0, 0, 0, 0, 0);
					if (est.Wants())
						est.Deliver(View(plane, static_cast<uint64_t>(i)));
					if (i % 50 == 0) est.NoteEvent(C::EstimatorEvent::Cut, static_cast<uint64_t>(i));
					if (i % 97 == 0) est.NoteRepeat(static_cast<uint64_t>(i + 1));
				}
			});
			std::thread reader([&est]() {
				for (int i = 0; i < 3000; ++i)
				{
					G::Record r;
					est.Held().ReadLatest(0, r);
					if (i % 20 == 0)
					{
						const std::string a = est.StatusLine();
						const std::string b = est.DetailText();
						Assert::IsFalse(a.empty() || b.empty());
					}
				}
			});
			toggler.join();
			feeder.join();
			reader.join();
			est.SetOriginalPathway(false);
			est.SetMode(C::EstimatorMode::Off);
			Assert::IsTrue(est.WaitIdle(60000), L"no deadlock: the worker drains and idles");
			G::Record r;
			Assert::IsFalse(est.Held().ReadLatest(0, r), L"off leaves no record behind");
			const C::EstimatorCounters c = est.GetCounters();
			Assert::IsTrue(c.delivered >= c.evaluations, L"every evaluation came from a delivered frame");
			est.SetMode(C::EstimatorMode::Shadow);
			est.NoteFrame(1, 1.0, 0, 0, 0, 0);
			est.Deliver(View(plane, 1));
			Assert::IsTrue(est.WaitIdle(60000));
			Assert::IsTrue(est.Held().ReadLatest(0, r), L"it starts again after the storm");
		}

		TEST_METHOD(TheMonitorsMutexAndTheEstimatorsNeverWaitOnEachOther)
		{
			SourceResolutionMonitor::Monitor m;
			m.SetChainEnabled(true);
			m.SetSourceIdMode(SourceResolutionMonitor::Monitor::SourceIdMode::Shadow);
			std::thread a([&m]() {
				for (int i = 0; i < 2000; ++i)
				{
					if (i % 5 == 0) m.Discontinuity("race");
					m.SetStreamKey(static_cast<uint64_t>(i / 3));
					m.SetSourceIdMode((i % 9) == 0 ? SourceResolutionMonitor::Monitor::SourceIdMode::Off :
						((i % 2) != 0 ? SourceResolutionMonitor::Monitor::SourceIdMode::Shadow : SourceResolutionMonitor::Monitor::SourceIdMode::On));
				}
			});
			std::thread b([&m]() {
				for (int i = 0; i < 2000; ++i)
				{
					m.Read();
					m.GeometryView(static_cast<double>(i));
					if (i % 10 == 0) m.ChainStatusLine();
				}
			});
			std::thread c([&m]() {
				for (int i = 0; i < 500; ++i)
				{
					m.SetChainEvidence(std::string(), (i & 1) != 0 ? "log" : "off");
					m.SetHistoryFolder(std::string());
				}
			});
			a.join();
			b.join();
			c.join();
			Assert::IsTrue(m.ChainEstimatorRef().WaitIdle(30000), L"no deadlock");
		}

		TEST_METHOD(TheMonitorFeedsDiscontinuitiesToTheEstimatorOnlyWhenEnabled)
		{
			SourceResolutionMonitor::Monitor m;
			m.SetSourceIdMode(SourceResolutionMonitor::Monitor::SourceIdMode::Shadow);
			Assert::IsFalse(m.ChainEnabled(), L"default off");
			Assert::IsTrue(m.ChainStatusLine().find("classical only") != std::string::npos);
			m.Discontinuity("before enable");
			Assert::IsTrue(m.ChainEstimatorRef().WaitIdle(5000));
			Assert::AreEqual<uint64_t>(0, m.ChainEstimatorRef().GetCounters().discontinuities);
			Assert::IsFalse(m.GeometryView().confident.fromRecord, L"no record: the delivered geometry stands in, visibly");
			Assert::IsTrue(m.Read().chainNote.empty(), L"disabled: the stats line is the one it was");
			m.SetChainEnabled(true);
			Assert::IsTrue(m.ChainEstimatorRef().Running());
			const SourceResolutionMonitor::Snapshot shown = m.Read();
			Assert::IsTrue(shown.chainNote.find("Source identity") != std::string::npos, L"the chain status rides on the snapshot");
			Assert::IsTrue(SourceResolutionMonitor::Describe(shown, 1.0, false).find(shown.chainNote) != std::string::npos,
				L"and on the existing Describe line");
			m.Discontinuity("after enable");
			Assert::IsTrue(m.ChainEstimatorRef().WaitIdle(5000));
			Assert::AreEqual<uint64_t>(1, m.ChainEstimatorRef().GetCounters().discontinuities);
			m.SetStreamKey(5);
			m.SetStreamKey(6);   // the first key is no change, the second is
			Assert::IsTrue(m.ChainEstimatorRef().WaitIdle(5000));
			Assert::AreEqual<uint64_t>(2, m.ChainEstimatorRef().GetCounters().discontinuities);
			m.SetSourceIdMode(SourceResolutionMonitor::Monitor::SourceIdMode::Off);
			Assert::IsFalse(m.ChainEstimatorRef().Running(), L"consensus off: the estimator follows");
			m.SetChainEnabled(false);
			Assert::IsFalse(m.ChainEnabled());
			Assert::IsTrue(m.Read().chainNote.empty());
			Assert::IsFalse(m.SetChainEvidence(std::string(), "bogus"), L"not a level: refused");
			Assert::IsTrue(m.SetChainEvidence(std::string(), "log+lines"));
		}
	};

	// The monitor's feeds into the estimator and its gate clock (SID-C2b round-2 audit fixes 8 and 9). CPU only, synthetic tiles.
	TEST_CLASS(ChainMonitorFeedTests)
	{
	public:
		typedef SourceResolutionMonitor::Monitor Mon;

		static void AnalyseFlat(Mon& m, double seconds, uint64_t seq)
		{
			const int tile = 128, tiles = 8;
			const std::vector<float> data(static_cast<size_t>(tile) * tile * tiles, 0.5f);   // a flat field: no informative tile
			const std::vector<float> none;
			m.AnalyseTilesForTest(data, tiles, seconds, 3840, 2160, none, none, tiles, tile, 64.0 / 1023.0, seq);
		}

		TEST_METHOD(BlankIsFedOnTheRisingEdgeOnlyNotOnEveryDarkFrame)
		{
			Mon m;
			m.SetChainEnabled(true);
			C::ChainEstimator& est = m.ChainEstimatorRef();
			est.SetSynchronous(true);
			m.SetSourceIdMode(Mon::SourceIdMode::Shadow);
			Assert::IsTrue(est.Running());
			const Plane plane = MakePlane(kW, kH, 47, 4);
			AnalyseFlat(m, 10.0, 2);   // the first flat frame: the Blank edge
			est.NoteFrame(2, 10.0, 0, 0, 0, 0);
			est.NoteFrame(3, 10.1, 0, 0, 0, 0);
			est.Deliver(View(plane, 3));
			Assert::IsTrue((est.StatusInput().last.flags & C::kEvBlank) != 0u, L"the edge reaches the estimator");
			AnalyseFlat(m, 10.5, 4);   // still flat: no new edge
			est.NoteFrame(4, 10.5, 0, 0, 0, 0);
			est.NoteFrame(5, 10.6, 0, 0, 0, 0);
			for (int i = 0; i < 200 && !est.Wants(); ++i)   // the estimator's own (real) clock: due again after its interval
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			Assert::IsTrue(est.Wants(), L"fixture: the next evaluation became due");
			est.Deliver(View(plane, 5));
			Assert::IsTrue(est.GetCounters().evaluations >= 2, L"fixture: a second evaluation ran");
			Assert::AreEqual<uint32_t>(0u, est.StatusInput().last.flags & C::kEvBlank, L"a dark scene flags blank once, not on every evaluation");
		}

		TEST_METHOD(TheChainGateAgeIsMeasuredOnTheRawSourceClockAfterABackwardsSeek)
		{
			Mon m;
			GeoFx::G::HeldState geo;
			m.SetFrameBound(true, 0);
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(Mon::SourceIdMode::On);
			auto record = [](uint64_t seq, double timeSec) {
				return GeoFx::MakeRecord(seq, static_cast<int64_t>(timeSec * 1e6), GeoFx::AppliedAxis(1920, 3840, 5), GeoFx::AppliedAxis(1080, 2160, 5));
			};
			std::string why;
			double fx = 1.0, fy = 1.0;
			geo.Publish(record(2, 99.6));
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 100.0, 2);
			Assert::IsFalse(m.SourceIdGeometryFallback().engaged, L"fresh before the seek");
			geo.Publish(record(3, 39.6));   // the source clock jumped back 60 s; the estimator stamps raw source seconds
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 40.0, 3);
			Assert::IsFalse(m.SourceIdGeometryFallback().engaged, L"the record stays fresh: the age is measured on the raw clock, not the never-backwards one");
		}
	};
}
