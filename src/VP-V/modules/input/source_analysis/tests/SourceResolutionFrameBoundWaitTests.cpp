// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// O#6 (SRM-FRAMEBOUND-WAIT): Red Team adversarial CPU tests for frame-bound wait-miss cause
// diagnostics and deterministic-only ready wait.
// Verifies:
//   1. Exact classification and Describe() surfacing for all 3 wait-miss causes (LatePoll, DropOrBacklog,
//      BoundedWaitTimeout) + clean era reset on SetFrameBound.
//   2. Deterministic still run with real worker thread reports WaitsMissed() == 0 and Reproducible() == true,
//      while the exact same frame cadence in live mode (SetDeterministicWait(false)) misses waits visibly.
//   3. Live mode NEVER blocks present on in-flight analysis (both direct Monitor and AnalysisLane::BindLaneReadyProbe),
//      while deterministic opposing control waits and records BoundedWaitTimeout when the bound expires.

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	using SourceResolutionMonitor::Monitor;
	using SourceResolutionMonitor::Snapshot;
	using SourceResolutionMonitor::WaitMissCause;

	constexpr int kW = 1920;
	constexpr int kH = 1080;
	constexpr double kBlackNorm = 64.0 / 1023.0;

	bool Contains(const std::string& haystack, const char* needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	std::vector<float> StillNoiseTiles(unsigned seed)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> dist(0.50, 0.12);
		std::vector<float> tiles(static_cast<size_t>(8) * Monitor::kTile * Monitor::kTile);
		for (float& v : tiles)
		{
			const double c = (std::min)(1.0, (std::max)(0.0, dist(rng)));
			v = static_cast<float>(std::round(c * 1023.0) / 1023.0);
		}
		return tiles;
	}

	void ConfigureMinimalLane(VpSkeleton::AnalysisLane& lane, bool hostDeterministic, uint32_t testWaitUs)
	{
		VpSkeleton::LaneSettings settings;
		settings.mode = VpSkeleton::LaneSettings::Mode::On;
		settings.deterministic = VpSkeleton::LaneSettings::Det::Auto;
		settings.testWaitUs = testWaitUs;

		const VpSkeleton::Registry reg = VpSkeleton::FirstProducts();
		VpSkeleton::ConsumerDecl consumer;
		consumer.id = 1;
		VpSkeleton::AddConsume(consumer, VpSkeleton::kP4SourceStats);
		std::vector<VpSkeleton::ConsumerDecl> consumers{ consumer };

		VpSkeleton::GraphSettings gs;
		gs.dims.width = 64;
		gs.dims.height = 48;
		gs.renderAhead = 0;
		gs.laneLead = 1;
		gs.queueDepth = 4;
		gs.lookAheadWalk = 0;

		VpSkeleton::CardProfile card;
		card.vramBudgetBytes = 1ull << 30;
		card.systemRamFreeBytes = 1ull << 30;
		card.uploadGBps = 10.0;
		card.computeScale = 1.0;

		VpSkeleton::TierInput tier;
		tier.framePeriodMs = 40.0;

		lane.Configure(settings, reg, consumers, gs, card, tier, 1, hostDeterministic);
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(SourceResolutionFrameBoundWaitTests)
	{
	public:
		// Test 1: all 3 wait-miss causes (LatePoll, DropOrBacklog, BoundedWaitTimeout) surfaced in Snapshot & Describe(),
		// and cleared cleanly by SetFrameBound era reset.
		TEST_METHOD(WaitMissCausesAreSurfacedInSnapshotAndDescribe)
		{
			Monitor m;
			m.SetFrameBound(true, 2);
			{
				const Snapshot clean = m.Read();
				Assert::AreEqual<unsigned long long>(0, clean.waitsMissed);
				Assert::IsTrue(clean.lastWaitMissCause == WaitMissCause::None);
				Assert::AreEqual<unsigned long long>(0, clean.waitMissLatePoll);
				Assert::AreEqual<unsigned long long>(0, clean.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(0, clean.waitMissTimeout);
				Assert::IsFalse(Contains(SourceResolutionMonitor::Describe(clean, 1.0, true), "last wait miss"));
			}

			// Advance release point to 10 - 2 = 8.
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.FrameFrontForTest(10, 10.0);
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 10.0, 10);

			// Cause 1: LatePoll (seq 5 <= releasedThrough 8, zero drops/backlog -> framesLate = 8 - 5 + 1 = 4).
			const std::vector<float> tiles = StillNoiseTiles(101u);
			m.AnalyseTilesForTest(tiles, 8, 5.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 5);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(1, s.waitsMissed);
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::LatePoll);
				Assert::AreEqual<uint64_t>(4ull, s.lastWaitMissFramesLate);
				Assert::IsTrue(s.lastWaitMissAnalyseMs >= 0.0);
				Assert::AreEqual<unsigned long long>(1, s.waitMissLatePoll);
				Assert::AreEqual<unsigned long long>(0, s.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(0, s.waitMissTimeout);
				const std::string desc = SourceResolutionMonitor::Describe(s, 1.0, true);
				Assert::IsTrue(Contains(desc, "late poll"), L"Describe must name 'late poll'");
				Assert::IsTrue(Contains(desc, "4 frames late"), L"Describe must name exact frames late");
				Assert::IsTrue(Contains(desc, "analyseMs"), L"Describe must include analyseMs");
				Assert::IsTrue(Contains(desc, "late poll 1, drop/backlog 0, wait timeout 0"));
			}

			// Cause 2: DropOrBacklog (a skipped copy exists in this era; seq 6 <= 8 -> framesLate = 3).
			m.NoteCopySkipped();
			m.AnalyseTilesForTest(tiles, 8, 6.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 6);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(2, s.waitsMissed);
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::DropOrBacklog);
				Assert::AreEqual<uint64_t>(3ull, s.lastWaitMissFramesLate);
				Assert::IsTrue(s.lastWaitMissAnalyseMs >= 0.0);
				Assert::AreEqual<unsigned long long>(1, s.waitMissLatePoll);
				Assert::AreEqual<unsigned long long>(1, s.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(0, s.waitMissTimeout);
				const std::string desc = SourceResolutionMonitor::Describe(s, 1.0, true);
				Assert::IsTrue(Contains(desc, "drop/backlog"), L"Describe must name 'drop/backlog'");
				Assert::IsTrue(Contains(desc, "3 frames late"));
				Assert::IsTrue(Contains(desc, "analyseMs"));
				Assert::IsTrue(Contains(desc, "late poll 1, drop/backlog 1, wait timeout 0"));
			}

			// Cause 3: BoundedWaitTimeout (in-flight seq 11 blocks renderSequence 13 until bound expires).
			m.NoteSubmittedForTest(11);
			m.SetDeterministicWait(true, 15000); // 15 ms explicit deterministic bound
			Assert::IsFalse(m.WaitForReadyDeterministic(13), L"Uncompleted in-flight sequence must time out");
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(3, s.waitsMissed);
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
				Assert::IsTrue(s.lastWaitMissFramesLate >= 1ull);
				Assert::IsTrue(s.lastWaitMissAnalyseMs >= 0.0);
				Assert::AreEqual<unsigned long long>(1, s.waitMissLatePoll);
				Assert::AreEqual<unsigned long long>(1, s.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(1, s.waitMissTimeout);
				const std::string desc = SourceResolutionMonitor::Describe(s, 1.0, true);
				Assert::IsTrue(Contains(desc, "wait timeout"), L"Describe must name 'wait timeout'");
				Assert::IsTrue(Contains(desc, "frames late"));
				Assert::IsTrue(Contains(desc, "analyseMs"));
				Assert::IsTrue(Contains(desc, "late poll 1, drop/backlog 1, wait timeout 1"));
			}

			// Era reset via SetFrameBound clears every wait-miss counter and last-cause field cleanly.
			m.SetFrameBound(true, 4);
			{
				const Snapshot reset = m.Read();
				Assert::AreEqual<unsigned long long>(0, reset.waitsMissed);
				Assert::IsTrue(reset.lastWaitMissCause == WaitMissCause::None);
				Assert::AreEqual<uint64_t>(0ull, reset.lastWaitMissFramesLate);
				Assert::AreEqual(0.0, reset.lastWaitMissAnalyseMs, 1e-12);
				Assert::AreEqual<unsigned long long>(0, reset.waitMissLatePoll);
				Assert::AreEqual<unsigned long long>(0, reset.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(0, reset.waitMissTimeout);
				Assert::IsTrue(m.Reproducible());
				Assert::IsFalse(Contains(SourceResolutionMonitor::Describe(reset, 1.0, true), "last wait miss"));
			}
		}

		// Test 2: Deterministic still run with real worker thread reports zero waits missed and Reproducible()==true;
		// negative control on the EXACT same frame cadence & submission schedule in live mode misses waits visibly.
		TEST_METHOD(DeterministicStillRunReportsZeroWaitsMissedWhileLiveControlMisses)
		{
			const std::vector<float> stillTiles = StillNoiseTiles(20260601u);

			// Deterministic run: waits cleanly on outCv before release points -> 0 waits missed, 0 drops, reproducible.
			{
				Monitor det;
				det.SetFrameBound(true, 4);
				det.SetDeterministicWait(true, 500000); // 500 ms generous deterministic test bound
				std::string why;
				double fx = 1.0, fy = 1.0;
				for (uint64_t seq = 1; seq <= 20; ++seq)
				{
					const double t = static_cast<double>(seq) * 0.5;
					det.FrameFrontForTest(seq, t);
					det.AutoFactors(kW, kH, 0.5, why, fx, fy, t, seq);
					if (seq % 2 == 1)
						det.SubmitJobForTest(stillTiles, 8, t, kW, kH, 8, Monitor::kTile, kBlackNorm, seq);
				}
				const Snapshot s = det.Read();
				Assert::AreEqual<unsigned long long>(0, det.WaitsMissed(), L"Deterministic still run must report 0 waits missed");
				Assert::AreEqual<unsigned long long>(0, det.JobsDroppedCount(), L"Deterministic still run must drop 0 jobs");
				Assert::IsTrue(det.Reproducible(), L"Deterministic still run must remain reproducible");
				Assert::IsTrue(s.reading && s.frames >= 5, L"Deterministic still run must publish analysed frames after startup");
				Assert::IsTrue(det.DeterministicWaitCalls() > 0);
			}

			// Negative control: EXACT same frame cadence & worker submission schedule without deterministic wait
			// (SetDeterministicWait(false)): immediate poll races past worker jobs -> WaitsMissed() > 0, !Reproducible().
			{
				Monitor live;
				live.SetFrameBound(true, 4);
				live.SetDeterministicWait(false);
				std::string why;
				double fx = 1.0, fy = 1.0;
				for (uint64_t seq = 1; seq <= 24; ++seq)
				{
					const double t = static_cast<double>(seq) * 0.5;
					live.FrameFrontForTest(seq, t);
					live.AutoFactors(kW, kH, 0.5, why, fx, fy, t, seq);
					if (seq <= 20 && (seq % 2 == 1))
						live.SubmitJobForTest(stillTiles, 8, t, kW, kH, 8, Monitor::kTile, kBlackNorm, seq);
				}
				// Wait for the background worker to finish analysing the queued jobs so late completions
				// (guaranteed through seq = 19 <= releasedThrough = 20) are deterministically recorded.
				for (int i = 0; i < 4000 && !live.ReadyFor(25); ++i)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				Assert::IsTrue(live.ReadyFor(25), L"Live control worker must finish draining queued jobs");

				const Snapshot s = live.Read();
				Assert::AreEqual<unsigned long long>(0, live.DeterministicWaitYields(),
					L"Live control must never yield/sleep on present");
				Assert::IsTrue(live.WaitsMissed() > 0,
					L"Negative control: unwaited live cadence racing past real worker jobs must record WaitsMissed() > 0");
				Assert::IsFalse(live.Reproducible(),
					L"Negative control: missed waits must visibly mark the run non-reproducible");
				const std::string desc = SourceResolutionMonitor::Describe(s, 1.0, true);
				Assert::IsTrue(Contains(desc, "last wait miss:"), L"Describe must name the exact miss cause");
				Assert::IsTrue(Contains(desc, "late poll") || Contains(desc, "drop/backlog"),
					L"Describe must classify the late worker completion as late poll or drop/backlog");
			}
		}

		// Test 3: Live mode NEVER blocks present on in-flight analysis (direct Monitor + AnalysisLane::BindLaneReadyProbe);
		// opposing deterministic control waits and records BoundedWaitTimeout when the bound expires.
		TEST_METHOD(LiveModeNeverBlocksPresentOnInFlightAnalysis)
		{
			// Part A: direct Monitor FrameFrontForTest + AutoFactors with in-flight sequence 10 never completing.
			{
				Monitor liveMon;
				liveMon.SetFrameBound(true, 4);
				liveMon.SetDeterministicWait(false);
				liveMon.NoteSubmittedForTest(10); // in-flight seq 10 <= 14 - 4 (not ready!)
				Assert::IsFalse(liveMon.ReadyFor(14));

				std::string why;
				double fx = 1.0, fy = 1.0;
				const auto t0 = std::chrono::steady_clock::now();
				liveMon.FrameFrontForTest(14, 14.0);
				liveMon.AutoFactors(kW, kH, 0.5, why, fx, fy, 14.0, 14);
				const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();

				Assert::IsTrue(elapsedMs < 200, L"Live mode must return immediately without blocking present");
				Assert::AreEqual<unsigned long long>(0, liveMon.DeterministicWaitCalls());
				Assert::AreEqual<unsigned long long>(0, liveMon.DeterministicWaitYields());
				Assert::AreEqual<unsigned long long>(0, liveMon.WaitsMissed());
			}

			// Opposing control A: deterministic mode on the exact same in-flight sequence 10 waits (>= 250 ms bound)
			// and times out visibly, proving the live wall-clock margin above catches real blocking waits.
			{
				Monitor detMon;
				detMon.SetFrameBound(true, 4);
				detMon.SetDeterministicWait(true, 250000); // 250 ms explicit bound (> live 200 ms ceiling)
				detMon.NoteSubmittedForTest(10);
				Assert::IsFalse(detMon.ReadyFor(14));

				std::string why;
				double fx = 1.0, fy = 1.0;
				const auto t0 = std::chrono::steady_clock::now();
				detMon.FrameFrontForTest(14, 14.0);
				detMon.AutoFactors(kW, kH, 0.5, why, fx, fy, 14.0, 14);
				const auto detElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();

				Assert::IsTrue(detElapsedMs >= 200,
					L"Opposing control: deterministic mode must actually wait out its bounded timeout");
				Assert::IsTrue(detMon.DeterministicWaitCalls() >= 1);
				Assert::IsTrue(detMon.DeterministicWaitYields() >= 1);
				Assert::AreEqual<unsigned long long>(1, detMon.WaitsMissed());
				Assert::IsTrue(detMon.Read().lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
			}

			// Part B: AnalysisLane bound via BindLaneReadyProbe(lane) — live vs deterministic opposing control.
			{
				Monitor mon;
				mon.SetFrameBound(true, 4);
				mon.NoteSubmittedForTest(10);
				Assert::IsFalse(mon.ReadyFor(14));

				// Live AnalysisLane (hostDeterministic=false, testWaitUs=0): DeterministicWaitUs returns 0 immediately.
				VpIndicator::Indicator liveInd;
				VpSkeleton::AnalysisLane liveLane(&liveInd, []() -> uint64_t { return 0ull; });
				ConfigureMinimalLane(liveLane, false, 0);
				mon.BindLaneReadyProbe(liveLane);
				Assert::AreEqual(0u, liveLane.DeterministicWaitUs(14, 50000),
					L"Live AnalysisLane must NEVER wait on an unready SRM probe");
				Assert::AreEqual<unsigned long long>(0, mon.WaitsMissed());
				mon.UnbindLaneReadyProbe(liveLane);

				// Opposing deterministic AnalysisLane (hostDeterministic=true, testWaitUs=10000): waits and reports timeout.
				VpIndicator::Indicator detInd;
				VpSkeleton::AnalysisLane detLane(&detInd, []() -> uint64_t { return 0ull; });
				ConfigureMinimalLane(detLane, true, 10000);
				mon.BindLaneReadyProbe(detLane);

				const uint32_t firstWait = detLane.DeterministicWaitUs(14, 0);
				Assert::IsTrue(firstWait > 0u,
					L"Opposing control: deterministic AnalysisLane must wait on unready SRM probe");
				int steps = 0;
				while (detLane.DeterministicWaitUs(14, 0) > 0u)
				{
					Assert::IsTrue(++steps < 200, L"Deterministic lane wait must be bounded");
				}
				Assert::AreEqual<uint64_t>(1ull, detLane.ReadyProbeMisses());
				Assert::AreEqual<unsigned long long>(1, mon.WaitsMissed());
				const Snapshot probeSnap = mon.Read();
				Assert::IsTrue(probeSnap.lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
				// Pass A LOW-1: BindLaneReadyProbe tracks last queried seq (14, lead 4 -> through 10) so framesLate > 0.
				Assert::AreEqual<uint64_t>(10ull, probeSnap.lastWaitMissFramesLate,
					L"BindLaneReadyProbe timeout must report exact non-zero framesLate (through - analysedThrough)");

				// Unbind clears the probe cleanly.
				mon.UnbindLaneReadyProbe(detLane);
				Assert::AreEqual(0u, detLane.DeterministicWaitUs(15, 0),
					L"Unbound probe must no longer cause deterministic waits");
			}
		}

		// Pass A HIGH-3 regression test: a frame that timed out in bounded deterministic wait
		// (BoundedWaitTimeout) and later completes in Analyse() must NOT be double-counted or
		// have its cause overwritten to LatePoll; a subsequent non-timed-out late frame still counts.
		TEST_METHOD(BoundedWaitTimeoutFrameFinishingLaterIsNotDoubleCounted)
		{
			Monitor m;
			m.SetFrameBound(true, 2);
			m.SetDeterministicWait(true, 15000);
			m.NoteSubmittedForTest(5);

			std::string why;
			double fx = 1.0, fy = 1.0;
			m.FrameFrontForTest(7, 7.0); // through = 5 -> waits 15 ms -> BoundedWaitTimeout (timedOutThrough = 5)
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 7.0, 7);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(1, m.WaitsMissed());
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
				Assert::AreEqual<unsigned long long>(1, s.waitMissTimeout);
				Assert::AreEqual<unsigned long long>(0, s.waitMissLatePoll);
			}

			// Seq 5 now completes in Analyse() after releasedThrough (5): must NOT double-count or overwrite cause!
			const std::vector<float> tiles = StillNoiseTiles(20260610u);
			m.AnalyseTilesForTest(tiles, 8, 5.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 5);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(1, m.WaitsMissed(),
					L"Timed-out sequence finishing later must NOT double-count WaitsMissed()");
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::BoundedWaitTimeout,
					L"Timed-out sequence finishing later must NOT overwrite cause to LatePoll");
				Assert::AreEqual<unsigned long long>(1, s.waitMissTimeout);
				Assert::AreEqual<unsigned long long>(0, s.waitMissLatePoll);
			}

			// Opposing control: a later sequence (seq 6 > timedOutThrough=5) that never timed out
			// DOES count as LatePoll when arriving after releasedThrough=6.
			m.SetDeterministicWait(false);
			m.FrameFrontForTest(8, 8.0);
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 8.0, 8);
			m.AnalyseTilesForTest(tiles, 8, 6.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 6);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(2, m.WaitsMissed(),
					L"Opposing control: subsequent non-timed-out late sequence must still count");
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::LatePoll);
				Assert::AreEqual<unsigned long long>(1, s.waitMissTimeout);
				Assert::AreEqual<unsigned long long>(1, s.waitMissLatePoll);
			}
		}

		// Pass A MED-1 regression test: after an early drop/skip triggers DropOrBacklog, a subsequent clean
		// late poll with zero new drops/skips must classify as LatePoll, not DropOrBacklog.
		TEST_METHOD(PastDropDoesNotMisclassifySubsequentCleanLatePoll)
		{
			Monitor m;
			m.SetFrameBound(true, 2);
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.FrameFrontForTest(10, 10.0); // releasedThrough = 8
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 10.0, 10);

			const std::vector<float> tiles = StillNoiseTiles(20260611u);

			// First late miss coincides with a copy skip -> DropOrBacklog.
			m.NoteCopySkipped();
			m.AnalyseTilesForTest(tiles, 8, 5.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 5);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(1, s.waitsMissed);
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::DropOrBacklog);
				Assert::AreEqual<unsigned long long>(1, s.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(0, s.waitMissLatePoll);
			}

			// Second late miss has ZERO new drops/skips and no backlog -> must classify as LatePoll!
			m.AnalyseTilesForTest(tiles, 8, 6.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 6);
			{
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(2, s.waitsMissed);
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::LatePoll,
					L"Clean late poll after an earlier drop must classify as LatePoll, not DropOrBacklog");
				Assert::AreEqual<unsigned long long>(1, s.waitMissDropOrBacklog);
				Assert::AreEqual<unsigned long long>(1, s.waitMissLatePoll);
			}
		}

		// Pass A MED-2 regression test: SetDeterministicWait(true, 0) + SetDeterministicSlack(presentSlackUs, guardUs)
		// governs BeginBoundFrame / WaitForReadyDeterministic when boundUs == 0.
		TEST_METHOD(SetDeterministicSlackGovernsZeroBoundWait)
		{
			// Subcase 1: zero slack with boundUs=0 -> immediate BoundMissed without yielding.
			{
				Monitor mZero;
				mZero.SetFrameBound(true, 2);
				mZero.SetDeterministicWait(true, 0);
				mZero.SetDeterministicSlack(0, 0);
				mZero.NoteSubmittedForTest(10);

				Assert::IsFalse(mZero.WaitForReadyDeterministic(12));
				Assert::AreEqual<unsigned long long>(0, mZero.DeterministicWaitYields(),
					L"Zero slack with boundUs=0 must time out immediately without yielding");
				Assert::AreEqual<unsigned long long>(1, mZero.WaitsMissed());
				Assert::IsTrue(mZero.Read().lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
			}

			// Subcase 2 (opposing control): fresh Monitor with configured non-zero slack (25 ms slack, 5 ms guard -> 20 ms budget)
			// yields before timing out cleanly (zero state leakage from subcase 1).
			{
				Monitor mSlack;
				mSlack.SetFrameBound(true, 2);
				mSlack.SetDeterministicWait(true, 0);
				mSlack.SetDeterministicSlack(25000, 5000);
				mSlack.NoteSubmittedForTest(10);

				mSlack.FrameFrontForTest(12, 12.0);
				Assert::IsTrue(mSlack.DeterministicWaitCalls() >= 1);
				Assert::IsTrue(mSlack.DeterministicWaitYields() >= 1,
					L"Configured deterministic slack must drive bounded waiting when boundUs=0");
				Assert::AreEqual<unsigned long long>(1, mSlack.WaitsMissed());
				Assert::IsTrue(mSlack.Read().lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
			}
		}

		// Pass A2 MED-1 (7e66aabb1) regression test: Discontinuity() resets timedOutThrough and haveBoundWaitSeq
		// so a restarted stretch neither swallows late arrivals below the old timedOutThrough nor skips
		// WaitForReadyDeterministic when a prior sequence number recurs.
		TEST_METHOD(DiscontinuityResetsTimedOutThroughAndBoundWaitSequence)
		{
			const std::vector<float> tiles = StillNoiseTiles(20260620u);

			// Part 1: timedOutThrough reset — after BoundedWaitTimeout at seq=100 (timedOutThrough=98) and
			// Discontinuity("seek"), a late arrival at seq=5 (< 98) in the restarted stretch MUST be counted.
			{
				Monitor m;
				m.SetFrameBound(true, 2);
				m.SetDeterministicWait(true, 15000);
				m.NoteSubmittedForTest(98);

				std::string why;
				double fx = 1.0, fy = 1.0;
				m.FrameFrontForTest(100, 100.0); // through = 98 -> BoundedWaitTimeout -> timedOutThrough = 98
				m.AutoFactors(kW, kH, 0.5, why, fx, fy, 100.0, 100);
				Assert::AreEqual<unsigned long long>(1, m.WaitsMissed());
				Assert::IsTrue(m.Read().lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);

				// Finish old seq 98 before the seek (swallowed by timedOutThrough == 98, clearing outstanding).
				m.AnalyseTilesForTest(tiles, 8, 98.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 98);
				Assert::AreEqual<unsigned long long>(1, m.WaitsMissed(),
					L"Pre-seek completion of seq 98 stays deduplicated by timedOutThrough=98");

				// Trigger discontinuity and restart stretch from seq = 1..7 (releasedThrough resets to 0 then advances to 5).
				m.Discontinuity("seek");
				m.FrameFrontForTest(1, 101.0);
				m.AutoFactors(kW, kH, 0.5, why, fx, fy, 101.0, 1);
				m.FrameFrontForTest(7, 102.0);
				m.AutoFactors(kW, kH, 0.5, why, fx, fy, 102.0, 7);
				Assert::AreEqual<unsigned long long>(1, m.WaitsMissed());

				// Late arrival on seq=5 (5 <= releasedThrough=5, and 5 < old timedOutThrough=98) MUST be detected!
				m.AnalyseTilesForTest(tiles, 8, 101.5, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 5);
				const Snapshot s = m.Read();
				Assert::AreEqual<unsigned long long>(2, m.WaitsMissed(),
					L"Discontinuity must reset timedOutThrough so late seq=5 (< old 98) is not swallowed");
				Assert::IsTrue(s.lastWaitMissCause == WaitMissCause::LatePoll);
				Assert::AreEqual<unsigned long long>(1, s.waitMissTimeout);
				Assert::AreEqual<unsigned long long>(1, s.waitMissLatePoll);
			}

			// Part 2 (same-input collision control): if seq=100 recurs immediately after Discontinuity("seek"),
			// colliding with the prior lastBoundWaitSeq==100, AutoFactors MUST still invoke WaitForReadyDeterministic.
			{
				Monitor mRecur;
				mRecur.SetFrameBound(true, 2);
				mRecur.SetDeterministicWait(true, 15000);
				mRecur.NoteSubmittedForTest(98);

				std::string why;
				double fx = 1.0, fy = 1.0;
				mRecur.FrameFrontForTest(100, 100.0); // WaitForReadyDeterministic(100) -> calls=1, waitsMissed=1, lastBoundWaitSeq=100
				mRecur.AutoFactors(kW, kH, 0.5, why, fx, fy, 100.0, 100); // same stretch: deduped (calls still 1)
				Assert::AreEqual<unsigned long long>(1, mRecur.DeterministicWaitCalls());
				Assert::AreEqual<unsigned long long>(1, mRecur.WaitsMissed());

				// Discontinuity -> seq=100 recurs directly on AutoFactors (colliding with prior lastBoundWaitSeq=100).
				mRecur.Discontinuity("seek");
				mRecur.AutoFactors(kW, kH, 0.5, why, fx, fy, 101.0, 100);
				Assert::AreEqual<unsigned long long>(2, mRecur.DeterministicWaitCalls(),
					L"AutoFactors must NOT skip WaitForReadyDeterministic when seq=100 recurs after Discontinuity");
				Assert::AreEqual<unsigned long long>(2, mRecur.WaitsMissed());
				Assert::IsTrue(mRecur.Read().lastWaitMissCause == WaitMissCause::BoundedWaitTimeout);
			}
		}
	};
}
