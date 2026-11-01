// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Resync Q3: the source-resolution monitor's frame-bound mode (SetFrameBound).
//
// CPU only. Two feeds: the real analysis path (AnalyseTilesForTest on seeded noise tiles, whose
// readings are whatever the detector says - the tests compare, they do not need an engagement), and
// InjectResultForTest, which publishes a scripted analysed result for a frame exactly as the worker
// would (so engage / fade / disengage are exercised without depending on the detector's spectra).
//
// Source time in these tests is a fixture: frame n is at n / 16 s, exact in binary, so every
// duration below is computed without rounding and the 20 s engage wait / 5 s disengage fall on a
// known frame. Nothing here sleeps except the tests that prove the wall clock is ignored.
namespace VideoProcessorTest
{
	namespace
	{
		using SourceResolutionMonitor::Monitor;
		using SourceResolutionMonitor::Snapshot;

		constexpr double kStep = 0.0625;       // seconds per source frame (test fixture, exact in binary)
		constexpr int kW = 1920, kH = 1080;    // the source's size (test fixture)
		constexpr double kBlack = 64.0 / 1023.0;

		double SecondsOf(uint64_t sequence) { return static_cast<double>(sequence) * kStep; }

		// The scripted analysis of the frame `sequence`: native evidence, then a clear 2:1 reading from
		// frame 100, its confidence gone from frame 800.
		Snapshot Scripted(uint64_t sequence)
		{
			Snapshot s;
			s.reading = true;
			s.nativeWidth = kW;
			s.nativeHeight = kH;
			const bool lower = sequence >= 100;
			s.declaredRatio = s.liveRatio = s.frameRatio = lower ? 0.5 : 1.0;
			s.confidence = sequence < 100 ? 0.0 : sequence < 800 ? 0.9 : 0.1;
			s.frames = sequence / 10;
			return s;
		}

		// A clear lower reading, `frames` analysed frames so far.
		Snapshot Lower(unsigned long long frames)
		{
			Snapshot s;
			s.reading = true;
			s.nativeWidth = kW;
			s.nativeHeight = kH;
			s.declaredRatio = s.liveRatio = s.frameRatio = 0.5;
			s.confidence = 0.9;
			s.frames = frames;
			return s;
		}

		std::vector<float> NoiseTiles(uint64_t sequence)
		{
			std::mt19937 rng(static_cast<unsigned>(sequence) * 2654435761u + 1u);
			std::normal_distribution<double> n(0.5, 0.12);
			std::vector<float> tiles(static_cast<size_t>(8) * Monitor::kTile * Monitor::kTile);
			for (float& v : tiles) v = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, n(rng))) * 1023.0) / 1023.0);
			return tiles;
		}

		void AnalyseNoise(Monitor& m, uint64_t sequence)
		{
			m.AnalyseTilesForTest(NoiseTiles(sequence), 8, SecondsOf(sequence), kW, kH, {}, {}, 8, Monitor::kTile, kBlack, sequence);
		}

		// One decision, as the render thread takes it, as text (everything a viewer could see).
		std::string Decide(Monitor& m, uint64_t sequence, double seconds, bool withSnapshot, double* factorOut = nullptr)
		{
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, seconds, sequence);
			const Monitor::Transition tr = m.AutoTransition(seconds);
			if (factorOut) *factorOut = fx;
			char line[640];
			snprintf(line, sizeof(line), "%llu f=%.6f,%.6f tr=%.6f,%.6f>%.6f,%.6f w=%.6f d=%.6f why=%s", static_cast<unsigned long long>(sequence),
				fx, fy, tr.fromX, tr.fromY, tr.toX, tr.toY, tr.weight, tr.duration, why.c_str());
			std::string out = line;
			if (withSnapshot)
			{
				const Snapshot s = m.Read();
				char more[320];
				snprintf(more, sizeof(more), " rd=%d fr=%llu cuts=%llu mom=%llu shots=%d dis=%llu decl=%.6f conf=%.6f live=%.6f ",
					s.reading ? 1 : 0, s.frames, s.cuts, s.moments, s.shots, s.discontinuities, s.declaredRatio, s.confidence, s.liveRatio);
				out += more;
			}
			return out;
		}

		// Frame-bound run with the worker's completion timing randomised: after each submission a random
		// number (0..3) of the oldest frames in flight complete (FIFO, as the worker's queue), and before
		// the decision the lane's bounded wait drains until ReadyFor.
		std::vector<std::string> RunLagged(bool tiles, uint32_t lead, unsigned lagSeed, uint64_t frames, uint64_t every)
		{
			Monitor m;
			m.SetFrameBound(true, lead);
			std::mt19937 rng(lagSeed);
			std::deque<uint64_t> pending;
			auto complete = [&] {
				const uint64_t q = pending.front();
				pending.pop_front();
				if (tiles) AnalyseNoise(m, q);
				else m.InjectResultForTest(Scripted(q), q);
			};
			std::vector<std::string> trace;
			for (uint64_t seq = 1; seq <= frames; ++seq)
			{
				if (seq % every == 0)
				{
					m.NoteSubmittedForTest(seq);
					pending.push_back(seq);
				}
				for (unsigned k = static_cast<unsigned>(rng() % 4); k > 0 && !pending.empty(); --k) complete();
				while (!m.ReadyFor(seq) && !pending.empty()) complete();
				trace.push_back(Decide(m, seq, SecondsOf(seq), tiles));
			}
			Assert::AreEqual(static_cast<unsigned long long>(0), m.HeldDropped(), L"the held queue never overflowed in the script");
			return trace;
		}

		// The scripted run with every result available at its own frame (no lag): frame-bound with
		// D = 0, or the mode off (published at once).
		std::vector<std::string> RunImmediate(bool frameBound, uint64_t frames)
		{
			Monitor m;
			if (frameBound) m.SetFrameBound(true, 0);
			std::vector<std::string> trace;
			for (uint64_t seq = 1; seq <= frames; ++seq)
			{
				if (seq % 10 == 0) m.InjectResultForTest(Scripted(seq), seq);
				trace.push_back(Decide(m, seq, SecondsOf(seq), false));
			}
			return trace;
		}

		void AssertSameTrace(const std::vector<std::string>& a, const std::vector<std::string>& b, const wchar_t* what)
		{
			Assert::AreEqual(a.size(), b.size(), what);
			for (size_t i = 0; i < a.size(); ++i)
				if (a[i] != b[i])
				{
					Logger::WriteMessage(("first difference at index " + std::to_string(i) + "\n  A: " + a[i] + "\n  B: " + b[i] + "\n").c_str());
					Assert::Fail(what);
				}
		}

		bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

		bool AnyLine(const std::vector<std::string>& trace, const char* part)
		{
			for (const std::string& l : trace) if (Contains(l, part)) return true;
			return false;
		}
	}

	TEST_CLASS(SourceResolutionFrameBoundTests)
	{
	public:
		// (a) The real analysis path: the same frames fed three times with different worker timing give
		// the same per-frame decisions AND the same released readings.
		TEST_METHOD(RunTwiceWithRandomisedWorkerTimingGivesTheSameTraceOnTheRealAnalysisPath)
		{
			const std::vector<std::string> one = RunLagged(true, 3, 11, 120, 8);
			const std::vector<std::string> two = RunLagged(true, 3, 97, 120, 8);
			const std::vector<std::string> three = RunLagged(true, 3, 5, 120, 8);
			Assert::AreEqual(static_cast<size_t>(120), one.size());
			AssertSameTrace(one, two, L"run 2 differs from run 1");
			AssertSameTrace(one, three, L"run 3 differs from run 1");
			// Frame 120 sees the analyses of frames <= 117: 8, 16, ... 112 = 14 of them (each Analyse counts one).
			Assert::IsTrue(Contains(one.back(), " fr=14 "), L"the released reading covers exactly the frames <= s - D");
			// Frame 10 sees frames <= 7: nothing analysed yet.
			Assert::IsTrue(Contains(one[9], " fr=0 "), L"nothing released before the first analysed frame is within reach");
		}

		// (a) The scripted engage / release: same trace whatever the completion timing, and it does engage
		// (after the 20 s source wait, crossfaded) and release (after 5 s, confidence gone).
		TEST_METHOD(RunTwiceWithRandomisedWorkerTimingGivesTheSameEngageAndReleaseTrace)
		{
			const std::vector<std::string> one = RunLagged(false, 6, 1, 1000, 10);
			const std::vector<std::string> two = RunLagged(false, 6, 2, 1000, 10);
			const std::vector<std::string> three = RunLagged(false, 6, 3, 1000, 10);
			AssertSameTrace(one, two, L"run 2 differs from run 1");
			AssertSameTrace(one, three, L"run 3 differs from run 1");
			Assert::IsTrue(AnyLine(one, "f=0.500000,0.500000"), L"the re-base engaged");
			Assert::IsTrue(AnyLine(one, "engaged by a crossfade"), L"engaged by the crossfade after the wait");
			Assert::IsTrue(AnyLine(one, "native after 5 s without a cut"), L"released after the disengage wait");
			Assert::IsTrue(Contains(one.back(), "f=1.000000,1.000000"), L"native again at the end");
			// Decided only on frames <= s - 6: the first lower reading (frame 100) is visible from 106.
			for (uint64_t seq = 1; seq < 106; ++seq)
				Assert::IsTrue(Contains(one[static_cast<size_t>(seq - 1)], "f=1.000000,1.000000"), L"native before the lower reading is within reach");
			Assert::IsTrue(Contains(one[105], "confirmed"), L"pending at frame 106, the first frame that may see frame 100");
		}

		// (b) Only source seconds move: the engage wait, the fade and the disengage are exact in them.
		TEST_METHOD(SourceTimeDrivesEngageFadeAndDisengage)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			uint64_t pendingSeq = 0, engagedSeq = 0, releaseSeq = 0;
			bool engaged = false;
			for (uint64_t seq = 1; seq <= 1000; ++seq)
			{
				if (seq % 10 == 0) m.InjectResultForTest(Scripted(seq), seq);
				double factor = 1.0;
				const std::string line = Decide(m, seq, SecondsOf(seq), false, &factor);
				if (pendingSeq == 0 && Contains(line, "confirmed")) pendingSeq = seq;
				if (!engaged && factor < 1.0) { engaged = true; engagedSeq = seq; }
				if (engaged && releaseSeq == 0 && factor >= 1.0) releaseSeq = seq;
			}
			// The reading appears with frame 100 (D = 0): pending since 100 / 16 s; 20 s of SOURCE time later is frame 420.
			Assert::AreEqual(static_cast<unsigned long long>(100), static_cast<unsigned long long>(pendingSeq));
			Assert::AreEqual(static_cast<unsigned long long>(420), static_cast<unsigned long long>(engagedSeq));
			// Confidence gone at frame 800: native after 5 s = 80 frames.
			Assert::AreEqual(static_cast<unsigned long long>(880), static_cast<unsigned long long>(releaseSeq));
		}

		TEST_METHOD(TheFadeRunsOnSourceSeconds)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			uint64_t engagedSeq = 0;
			for (uint64_t seq = 1; seq <= 440 && engagedSeq == 0; ++seq)
			{
				if (seq % 10 == 0) m.InjectResultForTest(Scripted(seq), seq);
				double factor = 1.0;
				Decide(m, seq, SecondsOf(seq), false, &factor);
				if (factor < 1.0) engagedSeq = seq;
			}
			Assert::AreEqual(static_cast<unsigned long long>(420), static_cast<unsigned long long>(engagedSeq));
			const double t = SecondsOf(engagedSeq);
			const Monitor::Transition start = m.AutoTransition(t);
			Assert::IsTrue(start.Active());
			Assert::AreEqual(1.5, start.duration, 1e-12);   // the crossfade, default 1.5 s
			Assert::AreEqual(0.0, start.weight, 1e-9);
			Assert::AreEqual(1.0, start.fromX, 1e-12);
			Assert::AreEqual(0.5, start.toX, 1e-12);
			Assert::AreEqual(0.5, m.AutoTransition(t + 0.75).weight, 1e-9);
			Assert::IsFalse(m.AutoTransition(t + 1.5).Active());
		}

		// (b) And the reverse: real time passing with the source clock frozen changes nothing.
		TEST_METHOD(FrozenSourceTimeIgnoresTheWallClock)
		{
			Monitor m;
			m.SetEngageTiming(0.05, 0.125, 0.05);   // 50 ms: a wall-clock implementation would engage within the loop
			m.SetFrameBound(true, 0);
			m.InjectResultForTest(Lower(1), 1);
			const double frozen = 100.0;
			bool everEngaged = false;
			for (uint64_t seq = 1; seq <= 25; ++seq)
			{
				double factor = 1.0;
				const std::string line = Decide(m, seq, frozen, false, &factor);
				everEngaged = everEngaged || factor < 1.0 || Contains(line, "engaged");
				std::this_thread::sleep_for(std::chrono::milliseconds(10));   // >= 250 ms of real time in all
			}
			Assert::IsFalse(everEngaged, L"real time must not engage anything while the source clock is frozen");
			double factor = 1.0;
			Decide(m, 26, frozen + 0.03125, false, &factor);
			Assert::AreEqual(1.0, factor, 0.0, L"31 ms of source time is under the 50 ms wait");
			Decide(m, 27, frozen + 0.0625, false, &factor);
			Assert::AreEqual(0.5, factor, 1e-12, L"62 ms of source time passed the wait");
			const double t = frozen + 0.0625;
			std::this_thread::sleep_for(std::chrono::milliseconds(200));   // longer than the whole crossfade
			Assert::AreEqual(0.0, m.AutoTransition(t).weight, 1e-12, L"the fade has not moved with real time");
			Assert::IsTrue(m.AutoTransition(t).Active());
			Assert::AreEqual(0.5, m.AutoTransition(t + 0.0625).weight, 1e-9);
			Assert::IsFalse(m.AutoTransition(t + 0.125).Active());
		}

		// (b) A seek: source time stepping backwards is clamped (no negative durations), counted, and the
		// wait goes on from where it was.
		TEST_METHOD(BackwardSourceTimeIsClampedAndCounted)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			m.InjectResultForTest(Lower(1), 1);
			double factor = 1.0;
			Decide(m, 1, 1000.0, false, &factor);   // pending since 1000
			Decide(m, 2, 1010.0, false, &factor);
			Assert::AreEqual(1.0, factor, 0.0);
			Assert::AreEqual(static_cast<unsigned long long>(0), m.ClockStepsBack());
			Decide(m, 3, 910.0, false, &factor);    // a seek back by 100 s
			Assert::AreEqual(static_cast<unsigned long long>(1), m.ClockStepsBack());
			Assert::AreEqual(1.0, factor, 0.0, L"a backward step must not look like a long wait (or an underflow)");
			const Monitor::Transition held = m.AutoTransition(800.0);
			Assert::IsTrue(held.weight >= 0.0 && held.weight <= 1.0);
			Decide(m, 4, 919.0, false, &factor);    // 9 s after the seek: 1019 on the monitor's clock
			Assert::AreEqual(1.0, factor, 0.0, L"19 s of source time have passed since pending");
			Decide(m, 5, 920.0, false, &factor);    // 1020: 20 s since pending
			Assert::AreEqual(0.5, factor, 1e-12);
			Assert::AreEqual(static_cast<unsigned long long>(1), m.ClockStepsBack());
			Assert::IsTrue(Contains(SourceResolutionMonitor::Describe(m.Read(), 0.5, true), "clock steps back 1"));
		}

		// (c) A result is not applied before the render sequence reaches it (frame + D).
		TEST_METHOD(HeldResultsAreNotAppliedBeforeTheSequenceReachesThem)
		{
			Monitor m;
			m.SetFrameBound(true, 10);
			m.InjectResultForTest(Lower(1), 100);
			double factor = 1.0;
			std::string line = Decide(m, 50, SecondsOf(50), false, &factor);
			Assert::AreEqual(1.0, m.Read().declaredRatio, 0.0);
			Assert::AreEqual(static_cast<size_t>(1), m.HeldCount());
			line = Decide(m, 109, SecondsOf(109), false, &factor);   // frames <= 99
			Assert::AreEqual(1.0, m.Read().declaredRatio, 0.0, L"frame 100's analysis is not for frame 109");
			Assert::AreEqual(static_cast<size_t>(1), m.HeldCount());
			Assert::IsFalse(Contains(line, "confirmed"));
			Assert::AreEqual(1.0, factor, 0.0);
			line = Decide(m, 110, SecondsOf(110), false, &factor);   // frames <= 100
			Assert::AreEqual(0.5, m.Read().declaredRatio, 0.0, L"released when the sequence reaches frame + D");
			Assert::AreEqual(static_cast<size_t>(0), m.HeldCount());
			Assert::AreEqual(static_cast<unsigned long long>(1), m.Read().frames);
			Assert::IsTrue(Contains(line, "confirmed"));
		}

		// (c) The held queue is a fixed ring: overflow drops the oldest, counted and shown.
		TEST_METHOD(HeldQueueOverflowDropsTheOldestAndCountsIt)
		{
			Monitor m;
			m.SetFrameBound(true, 10);
			const size_t extra = 4;
			for (size_t i = 0; i < Monitor::kHeldCap + extra; ++i)
				m.InjectResultForTest(Lower(i + 1), 1000 + i);
			Assert::AreEqual(static_cast<size_t>(Monitor::kHeldCap), m.HeldCount());
			Assert::AreEqual(static_cast<unsigned long long>(extra), m.HeldDropped());
			double factor = 1.0;
			Decide(m, 5, SecondsOf(5), false, &factor);   // nothing within reach
			const std::string text = SourceResolutionMonitor::Describe(m.Read(), 1.0, true);
			Assert::IsTrue(Contains(text, "dropped 4"), L"Describe shows the drops");
			Assert::IsTrue(Contains(text, "held 16"), L"Describe shows the queue length");
			// The survivors start at the fifth result (frames = 5): sequence 1004 is reached at 1014.
			Decide(m, 1014, SecondsOf(1014), false, &factor);
			Assert::AreEqual(static_cast<unsigned long long>(5), m.Read().frames, L"the oldest four were the ones dropped");
			Assert::AreEqual(static_cast<size_t>(Monitor::kHeldCap - 1), m.HeldCount());
		}

		// (c) A Discontinuity() starts a new era: a result analysed before it is never applied.
		TEST_METHOD(ResultsFromBeforeADiscontinuityAreDropped)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			m.InjectResultForTest(Lower(1), 10);
			m.Discontinuity("test seek");
			double factor = 1.0;
			Decide(m, 10, SecondsOf(10), false, &factor);
			const Snapshot s = m.Read();
			Assert::IsFalse(s.reading, L"the old era's reading must not show");
			Assert::AreEqual(1.0, s.declaredRatio, 0.0);
			Assert::AreEqual(static_cast<unsigned long long>(1), s.heldStale);
			Assert::AreEqual(1.0, factor, 0.0);
			m.InjectResultForTest(Lower(2), 11);   // the new era's frame
			Decide(m, 11, SecondsOf(11), false, &factor);
			Assert::AreEqual(0.5, m.Read().declaredRatio, 0.0);
		}

		// (d) ReadyFor / AnalysedThrough.
		TEST_METHOD(ReadyForAndAnalysedThroughFollowTheFramesInFlight)
		{
			Monitor m;
			m.SetFrameBound(true, 4);
			const std::vector<float> flat(static_cast<size_t>(8) * Monitor::kTile * Monitor::kTile, 0.5f);
			auto analyse = [&](uint64_t seq) { m.AnalyseTilesForTest(flat, 8, SecondsOf(seq), kW, kH, {}, {}, 8, Monitor::kTile, kBlack, seq); };
			Assert::IsTrue(m.ReadyFor(100), L"nothing in flight");
			Assert::AreEqual(static_cast<unsigned long long>(0), m.AnalysedThrough());
			m.NoteSubmittedForTest(50);
			m.NoteSubmittedForTest(70);
			Assert::IsTrue(m.ReadyFor(2), L"no frame <= s - D exists");
			Assert::IsTrue(m.ReadyFor(53), L"frame 50 is not needed by 53 (needs <= 49)");
			Assert::IsFalse(m.ReadyFor(54), L"needs frames <= 50: 50 is in flight");
			Assert::IsFalse(m.ReadyFor(60));
			analyse(50);
			Assert::AreEqual(static_cast<unsigned long long>(50), m.AnalysedThrough());
			Assert::IsTrue(m.ReadyFor(54));
			Assert::IsTrue(m.ReadyFor(73));
			Assert::IsFalse(m.ReadyFor(74), L"needs frames <= 70: 70 is in flight");
			// Completed on another thread while the lane polls (the bounded wait).
			std::future<void> worker = std::async(std::launch::async, [&] {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				analyse(70);
			});
			bool ready = false;
			for (int i = 0; i < 2000 && !ready; ++i)
			{
				ready = m.ReadyFor(74);
				if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			worker.get();
			Assert::IsTrue(ready);
			Assert::AreEqual(static_cast<unsigned long long>(70), m.AnalysedThrough());
			Assert::AreEqual(static_cast<size_t>(2), m.HeldCount());
			m.NoteWaitMissed();
			m.NoteWaitMissed();
			Assert::AreEqual(static_cast<unsigned long long>(2), m.WaitsMissed());
			Assert::IsTrue(Contains(SourceResolutionMonitor::Describe(m.Read(), 1.0, true), "waits missed 2"));
		}

		// (e) The switch off (the default): the scripted run gives exactly what frame-bound with D = 0 gives
		// when every result is available at its own frame - the clocks and the decisions are the same code
		// - and none of the new state shows.
		TEST_METHOD(SwitchOffMatchesTheBehaviourBeforeOnAScriptedRun)
		{
			const std::vector<std::string> off = RunImmediate(false, 1000);
			const std::vector<std::string> on = RunImmediate(true, 1000);
			AssertSameTrace(off, on, L"frame-bound D = 0 must equal the mode off");
			Assert::IsTrue(AnyLine(off, "engaged by a crossfade"));
			Assert::IsTrue(Contains(off.back(), "f=1.000000,1.000000"));

			Monitor m;
			Assert::IsFalse(m.FrameBound());
			m.InjectResultForTest(Lower(1), 1);
			Assert::AreEqual(0.5, m.Read().declaredRatio, 0.0, L"off: published at once");
			Assert::AreEqual(static_cast<size_t>(0), m.HeldCount());
			double factor = 1.0;
			Decide(m, 1, 50.0, false, &factor);
			Decide(m, 2, 40.0, false, &factor);   // a backward step is not clamped (or counted) when off
			Assert::AreEqual(static_cast<unsigned long long>(0), m.ClockStepsBack());
			Assert::IsFalse(m.Read().frameBound);
			Assert::IsFalse(Contains(SourceResolutionMonitor::Describe(m.Read(), 1.0, true), "frame-bound"));
			m.SetFrameBound(false, 7);   // already off: nothing changes
			Assert::IsFalse(m.FrameBound());
		}
	};

	// Audit 1 (items 2 and 8): the renderer's call order, and the REAL worker thread.
	TEST_CLASS(SourceResolutionFrameBoundThreadedTests)
	{
	public:
		// The renderer calls Discontinuity -> Frame -> AutoFactors in that order. Frame's frame-bound
		// release consumes the reset; AutoFactors must still see it ("native at once").
		TEST_METHOD(ADiscontinuityConsumedByFrameStillResetsAutoFactor)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			uint64_t engagedSeq = 0;
			for (uint64_t seq = 1; seq <= 440 && engagedSeq == 0; ++seq)
			{
				if (seq % 10 == 0) m.InjectResultForTest(Scripted(seq), seq);
				double factor = 1.0;
				Decide(m, seq, SecondsOf(seq), false, &factor);
				if (factor < 1.0) engagedSeq = seq;
			}
			Assert::AreEqual(static_cast<unsigned long long>(420), static_cast<unsigned long long>(engagedSeq));
			m.Discontinuity("test seek");
			m.FrameFrontForTest(421, SecondsOf(421));   // Frame(): the clock and the release, no GPU
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, SecondsOf(421), 421);
			Assert::AreEqual(1.0, fx, 0.0);
			Assert::AreEqual(1.0, fy, 0.0);
			Assert::IsTrue(Contains(why, "native at once"), L"the reset survived Frame()");
			// And it is applied once: the next frame does not reset again.
			m.FrameFrontForTest(422, SecondsOf(422));
			std::string why2;
			m.AutoFactors(kW, kH, 0.5, why2, fx, fy, SecondsOf(422), 422);
			Assert::IsFalse(Contains(why2, "native at once"));
		}

		// One run with a REAL worker thread: every 8th frame's tiles are handed to the worker, the
		// completion is delayed by a random sleep, and the lane's bounded wait (ReadyFor) is honoured
		// before each decision. The trace must equal the single-threaded model, whatever the timing.
		static std::vector<std::string> RunThreaded(unsigned timingSeed, uint32_t lead, uint64_t frames, uint64_t every,
			bool* reproducible)
		{
			Monitor m;
			m.SetFrameBound(true, lead);
			std::mt19937 rng(timingSeed);
			std::vector<std::string> trace;
			for (uint64_t seq = 1; seq <= frames; ++seq)
			{
				if (seq % every == 0)
				{
					m.SubmitJobForTest(NoiseTiles(seq), 8, SecondsOf(seq), kW, kH, 8, Monitor::kTile, kBlack, seq);
					if (rng() % 3 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 4));
					else std::this_thread::yield();
				}
				// The bounded deterministic wait: bounded here by 10 s of wall clock (a failure, not a hang).
				const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (!m.ReadyFor(seq) && std::chrono::steady_clock::now() < until)
					std::this_thread::sleep_for(std::chrono::microseconds(100 + rng() % 400));
				Assert::IsTrue(m.ReadyFor(seq), L"the worker finished within the bound");
				trace.push_back(Decide(m, seq, SecondsOf(seq), true));
			}
			if (reproducible) *reproducible = m.Reproducible();
			return trace;
		}

		TEST_METHOD(RealWorkerThreadGivesTheSameDecisionsRunToRun)
		{
			bool r1 = false, r2 = false, r3 = false;
			const std::vector<std::string> a = RunThreaded(1, 4, 120, 8, &r1);
			const std::vector<std::string> b = RunThreaded(7, 4, 120, 8, &r2);
			const std::vector<std::string> c = RunThreaded(31337, 4, 120, 8, &r3);
			AssertSameTrace(a, b, L"threaded run 2 differs from run 1");
			AssertSameTrace(a, c, L"threaded run 3 differs from run 1");
			Assert::IsTrue(r1 && r2 && r3, L"nothing was dropped or skipped, so the runs are reproducible");
			const std::vector<std::string> model = RunLagged(true, 4, 5, 120, 8);
			AssertSameTrace(a, model, L"the real worker equals the single-threaded model");
			Assert::IsTrue(Contains(a.back(), " fr="), L"readings were released");
		}

		// Timing may lose analyses (a full job queue drops the oldest): never silently. Whatever the
		// worker does, every submitted frame is either analysed or counted as dropped, and a run with
		// a loss says it is not reproducible.
		TEST_METHOD(LostAnalysesAreCountedAndTheRunIsMarkedNotReproducible)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			const uint64_t submitted = 12;
			for (uint64_t seq = 1; seq <= submitted; ++seq)
				m.SubmitJobForTest(NoiseTiles(seq), 8, SecondsOf(seq), kW, kH, 8, Monitor::kTile, kBlack, seq);
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!m.ReadyFor(1000) && std::chrono::steady_clock::now() < until)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			Assert::IsTrue(m.ReadyFor(1000));
			double factor = 1.0;
			Decide(m, 1000, SecondsOf(1000), false, &factor);   // releases everything held
			const unsigned long long analysed = m.Read().frames;
			const unsigned long long dropped = m.JobsDroppedCount();
			Assert::AreEqual(static_cast<unsigned long long>(submitted), analysed + dropped, L"nothing disappears unaccounted");
			const std::string text = SourceResolutionMonitor::Describe(m.Read(), 1.0, true);
			Assert::IsTrue(Contains(text, "copies skipped 0"), L"the skipped-copy counter is shown");
			if (dropped > 0)
			{
				Assert::IsFalse(m.Reproducible());
				Assert::IsTrue(Contains(text, "not reproducible"), L"a loss is announced");
			}
			else
				Assert::IsTrue(m.Reproducible());
		}

		// Audit 2 SHOULD: reproducibility is judged per frame-bound era.
		TEST_METHOD(ANewFrameBoundEraStartsReproducibleAgain)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			m.NoteWaitMissed();
			Assert::IsFalse(m.Reproducible());
			m.SetFrameBound(true, 2);   // a new lead: a fresh era
			Assert::IsTrue(m.Reproducible(), L"the previous era's loss is not this era's");
			Assert::AreEqual(0ull, m.WaitsMissed());
		}

		TEST_METHOD(AMissedWaitMakesTheRunNotReproducible)
		{
			Monitor m;
			m.SetFrameBound(true, 0);
			Assert::IsTrue(m.Reproducible());
			m.NoteWaitMissed();
			Assert::IsFalse(m.Reproducible());
			Assert::IsTrue(Contains(SourceResolutionMonitor::Describe(m.Read(), 1.0, true), "not reproducible"));
		}
	};
}
