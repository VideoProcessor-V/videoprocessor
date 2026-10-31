/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// R0: the resync detector, the arm box, the refusal note and the storm window.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/ResyncEvent.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;

namespace Tests
{
	namespace ResyncR0
	{
		struct Stream
		{
			Detector det;
			ArmBox arms;
			uint64_t seq = 0;
			uint64_t gen = 1;
			uint64_t state = 0x1000;
			uint64_t format = 7;
			uint64_t counter = 100;
			int64_t ts = 0;
			int64_t tps = 10000000;
			double rate = 0.0;

			Stream() { RefusalNote::Take(); }

			EnqueueFacts Next()
			{
				EnqueueFacts f;
				f.sequence = ++seq;
				f.queueGeneration = gen;
				f.stateKey = state;
				f.formatKey = format;
				f.frameCounter = ++counter;
				f.timestampTicks = ts;
				f.ticksPerSecond = ts != 0 ? tps : 0;
				f.measuredRateHz = rate;
				return f;
			}
			Decision Push() { return det.OnEnqueue(Next(), arms); }
			Decision Push(const EnqueueFacts& f) { return det.OnEnqueue(f, arms); }
			Decision Steady()
			{
				Decision d = Push();   // first frame (Start)
				d = Push();
				return d;
			}
		};

		inline void ExpectOne(const Decision& d, Level l, Cause c)
		{
			Assert::AreEqual(1, static_cast<int>(d.count));
			Assert::IsTrue(d.events[0].level == l);
			Assert::IsTrue(d.events[0].cause == c, L"cause");
		}
		inline void ExpectNone(const Decision& d) { Assert::AreEqual(0, static_cast<int>(d.count)); }
	}

	using namespace ResyncR0;

	TEST_CLASS(ResyncEventTests)
	{
	public:
		TEST_METHOD(RefusedChangeRebuildsWithOneFullAndNoDoubleAction)
		{
			Stream old;
			ExpectOne(old.Push(), Level::Full, Cause::Start);
			ExpectNone(old.Push());
			// The material change is refused: the old detector never sees the new state.
			Stream rebuilt;   // ctor clears the note, so set it after constructing
			RefusalNote::Set(RefusalReason::DisplayMode);
			rebuilt.format = 9;
			Decision d = rebuilt.Push();
			ExpectOne(d, Level::Full, Cause::StartAfterSourceChange);
			Assert::AreEqual(static_cast<int>(RefusalReason::DisplayMode), static_cast<int>(d.events[0].detail));
			ExpectNone(rebuilt.Push());
			Assert::IsTrue(RefusalNote::Take() == RefusalReason::None, L"note consumed");
		}

		TEST_METHOD(InPlaceEotfChangeEmitsOneFullOnFirstNewStateFrame)
		{
			Stream s;
			s.Steady();
			s.arms.ArmState(0x2000, Cause::InPlaceSourceChange);
			ExpectNone(s.Push());   // still the old state key
			ExpectNone(s.Push());
			s.state = 0x2000;
			s.format = 8;           // the format also changed in the same frame
			ExpectOne(s.Push(), Level::Full, Cause::InPlaceSourceChange);
			ExpectNone(s.Push());
		}

		TEST_METHOD(ResetAndLiveQueueResetAreArmedGenerations)
		{
			Stream s;
			s.Steady();
			s.arms.ArmQueue(2, Cause::RendererReset);
			s.gen = 2;
			ExpectOne(s.Push(), Level::Full, Cause::RendererReset);
			s.arms.ArmQueue(3, CauseFromQueueReason("live queue reset"));
			s.gen = 3;
			ExpectOne(s.Push(), Level::Full, Cause::LiveQueueReset);
			s.gen = 4;   // no arm
			ExpectOne(s.Push(), Level::Full, Cause::QueueGenerationOther);
			ExpectNone(s.Push());
		}

		TEST_METHOD(PendingStateArmIsConsumedSilentlyByAGenerationAdvance)
		{
			Stream s;
			s.Steady();
			s.arms.ArmQueue(2, Cause::RendererReset);
			s.arms.ArmState(0x3000, Cause::InPlaceSourceChange);
			s.gen = 2;
			s.state = 0x3000;
			ExpectOne(s.Push(), Level::Full, Cause::RendererReset);
			ExpectNone(s.Push());   // the state arm does not fire a second FULL
		}

		TEST_METHOD(CaptureRelockWithoutModeChangeIsLiveQueueReset)
		{
			Stream s;
			s.Steady();
			s.arms.ArmQueue(2, CauseFromQueueReason("live queue reset"));
			s.gen = 2;
			ExpectOne(s.Push(), Level::Full, Cause::LiveQueueReset);
		}

		TEST_METHOD(FileDiscontinuityCounterBackwardsAndFormatChangeAreFull)
		{
			Stream s;
			s.Steady();
			EnqueueFacts f = s.Next();
			f.sourceDiscontinuity = true;
			f.fileFrame = true;
			ExpectOne(s.Push(f), Level::Full, Cause::FileDiscontinuity);
			f = s.Next();
			f.frameCounter = 5;
			ExpectOne(s.Push(f), Level::Full, Cause::CounterBackwards);
			s.counter = 200;
			s.format = 99;
			ExpectOne(s.Push(), Level::Full, Cause::SourceFormatChanged);
			ExpectNone(s.Push());
		}

		TEST_METHOD(CalibrationIsFullAndIgnoredByTheStorm)
		{
			Stream s;
			s.Steady();
			StormWindow storm;
			s.arms.ArmQueue(2, CauseFromQueueReason("calibration patterns"));
			s.gen = 2;
			Decision d = s.Push();
			ExpectOne(d, Level::Full, Cause::CalibrationStart);
			storm.Note(d.events[0].cause, 1.0);
			s.arms.ArmQueue(3, CauseFromQueueReason("calibration patterns ended"));
			s.gen = 3;
			d = s.Push();
			ExpectOne(d, Level::Full, Cause::CalibrationEnd);
			storm.Note(d.events[0].cause, 2.0);
			for (int i = 0; i < 6; ++i) storm.Note(Cause::CalibrationStart, 3.0 + i);
			Assert::AreEqual(0, storm.FullCount(9.0));
			Assert::IsFalse(storm.Engaged(9.0));
		}

		TEST_METHOD(EveryGapLiteralMapsToItsMinorCause)
		{
			struct Row { const char* text; Cause cause; };
			// Every literal found in LibplaceboVideoRenderer.cpp (gapCause = "...", queue drops,
			// capture-stale causes) plus the two LPR literals that are not in the table.
			const Row rows[] = {
				{ "missed frames", Cause::MissedFrames },
				{ "capture timestamp discontinuity", Cause::TimestampBreak },
				{ "stale capture delivery", Cause::StaleDelivery },
				{ "stale capture delivery ended", Cause::StaleDeliveryEnded },
				{ "queue overflow drop", Cause::OverflowDrop },
				{ "repeat-preserving queue drop", Cause::RepeatPreservingDrop },
				{ "pulldown lock change", Cause::PulldownLockChange },
				{ "pulldown lock change (on a skipped repeat)", Cause::PulldownLockChange },
				{ "pulldown re-phase", Cause::PulldownRephase },
				{ "pulldown film clock restarted (switch toggled)", Cause::PulldownClockRestart },
				{ "pulldown off", Cause::PulldownOff },
				{ "queue resize", Cause::GapOther },     // LPR literal not in the table
				{ "backlog flush", Cause::GapOther },    // LPR literal not in the table
				{ "something unknown", Cause::GapOther },
				{ nullptr, Cause::GapOther },
			};
			for (const Row& r : rows) {
				Stream s;
				s.Steady();
				EnqueueFacts f = s.Next();
				f.gapBefore = true;
				f.gapCause = r.text;
				f.gapFrames = 3;
				Decision d = s.Push(f);
				ExpectOne(d, Level::Minor, r.cause);
				Assert::AreEqual(3u, d.events[0].gapFrames);
				Assert::AreEqual(static_cast<unsigned long long>(f.sequence), static_cast<unsigned long long>(d.events[0].fromSeq));
			}
		}

		TEST_METHOD(QueueReasonLiteralsMap)
		{
			Assert::IsTrue(CauseFromQueueReason("start") == Cause::Start);
			Assert::IsTrue(CauseFromQueueReason("renderer reset") == Cause::RendererReset);
			Assert::IsTrue(CauseFromQueueReason("live queue reset") == Cause::LiveQueueReset);
			Assert::IsTrue(CauseFromQueueReason("calibration patterns") == Cause::CalibrationStart);
			Assert::IsTrue(CauseFromQueueReason("calibration patterns ended") == Cause::CalibrationEnd);
			Assert::IsTrue(CauseFromQueueReason("whatever") == Cause::QueueGenerationOther);
			Assert::IsTrue(CauseFromQueueReason(nullptr) == Cause::QueueGenerationOther);
		}

		TEST_METHOD(TimestampBreakGrading)
		{
			const char* text = "capture timestamp discontinuity";
			// Small forward break -> MINOR.
			{
				Stream s; s.ts = 1000000; s.rate = 0.0;
				s.Push(); s.ts += 400000; s.Push();     // normal delta 40 ms
				s.ts += 900000;                          // forward break, 90 ms
				EnqueueFacts f = s.Next(); f.gapBefore = true; f.gapCause = text;
				ExpectOne(s.Push(f), Level::Minor, Cause::TimestampBreak);
			}
			// Backwards beyond one period -> FULL.
			{
				Stream s; s.ts = 1000000; s.rate = 50.0;
				s.Push(); s.ts += 200000; s.Push();
				s.ts -= 1000000;                         // back 100 ms, period is 20 ms
				EnqueueFacts f = s.Next(); f.gapBefore = true; f.gapCause = text;
				ExpectOne(s.Push(f), Level::Full, Cause::TimestampBreakLarge);
			}
			// Jump over 2 s -> FULL.
			{
				Stream s; s.ts = 1000000; s.rate = 25.0;
				s.Push(); s.ts += 400000; s.Push();
				s.ts += 30000000;                        // 3 s
				EnqueueFacts f = s.Next(); f.gapBefore = true; f.gapCause = text;
				ExpectOne(s.Push(f), Level::Full, Cause::TimestampBreakLarge);
			}
			// Unknown timebase -> MINOR whatever the size.
			{
				Stream s;
				s.Push(); s.Push();
				EnqueueFacts f = s.Next(); f.gapBefore = true; f.gapCause = text;
				f.timestampTicks = 5; f.ticksPerSecond = 0;
				ExpectOne(s.Push(f), Level::Minor, Cause::TimestampBreak);
			}
		}

		TEST_METHOD(OverflowDropOnQueuedSuccessorIsReportedAtItsSequence)
		{
			Stream s;
			s.Steady();
			EnqueueFacts f = s.Next();
			f.overflowSuccessorSeq = 1;   // an earlier, already queued frame
			Decision d = s.Push(f);
			ExpectOne(d, Level::Minor, Cause::OverflowDrop);
			Assert::AreEqual(1ull, static_cast<unsigned long long>(d.events[0].fromSeq));

			f = s.Next();
			f.overflowSuccessorSeq = 2;
			f.gapBefore = true;
			f.gapCause = "missed frames";
			f.gapFrames = 2;
			d = s.Push(f);
			Assert::AreEqual(2, static_cast<int>(d.count));
			Assert::IsTrue(d.events[0].cause == Cause::MissedFrames);
			Assert::IsTrue(d.events[1].cause == Cause::OverflowDrop);
			Assert::AreEqual(2ull, static_cast<unsigned long long>(d.events[1].fromSeq));
		}

		TEST_METHOD(LateEventsCarryTheSlipPoint)
		{
			Detector det;
			Event e = det.Late(Cause::DriftDrop, 42, 1);
			Assert::IsTrue(e.level == Level::Minor && e.cause == Cause::DriftDrop);
			Assert::AreEqual(42ull, static_cast<unsigned long long>(e.fromSeq));
			e = det.Late(Cause::KeptRunHole, 77, 3);
			Assert::IsTrue(e.level == Level::Minor && e.cause == Cause::KeptRunHole);
			Assert::AreEqual(77ull, static_cast<unsigned long long>(e.fromSeq));
			Assert::AreEqual(3u, e.gapFrames);
		}

		TEST_METHOD(RateStepIsTimingOnly)
		{
			// 59.94 -> 60 (about 1000 ppm): Timing RateStep only.
			{
				Stream s; s.rate = 59.94;
				s.Steady();
				s.rate = 60.0;
				ExpectOne(s.Push(), Level::Timing, Cause::RateStep);
				ExpectNone(s.Push());
			}
			// 50 ppm: nothing.
			{
				Stream s; s.rate = 60.0;
				s.Steady();
				s.rate = 60.0 * (1.0 + 50e-6);
				ExpectNone(s.Push());
			}
			// On a FULL frame: no timing event.
			{
				Stream s; s.rate = 59.94;
				s.Steady();
				s.rate = 60.0;
				s.gen = 2;
				ExpectOne(s.Push(), Level::Full, Cause::QueueGenerationOther);
			}
			// Rate going to 0 clears the memory.
			{
				Stream s; s.rate = 60.0;
				s.Steady();
				s.rate = 0.0;
				ExpectNone(s.Push());
				s.rate = 59.94;
				ExpectNone(s.Push());
			}
		}

		TEST_METHOD(StormEngagesOnMoreThanThreeFullInTenSeconds)
		{
			StormWindow w;
			for (int i = 0; i < 3; ++i) w.Note(Cause::RendererReset, 1.0 + i);
			Assert::AreEqual(3, w.FullCount(4.0));
			Assert::IsFalse(w.Engaged(4.0));
			w.Note(Cause::LiveQueueReset, 4.5);
			Assert::IsTrue(w.Engaged(5.0));
			w.Note(Cause::DriftDrop, 5.0);   // MINOR never counts
			Assert::AreEqual(4, w.FullCount(5.0));

			StormWindow spread;
			spread.Note(Cause::Start, 0.0);
			spread.Note(Cause::Start, 3.5);
			spread.Note(Cause::Start, 7.0);
			spread.Note(Cause::Start, 11.0);
			Assert::IsFalse(spread.Engaged(11.0));
			Assert::AreEqual(3, spread.FullCount(11.0));
		}

		TEST_METHOD(ArmBoxNeverTearsAKeyCausePair)
		{
			ArmBox box;
			std::atomic<bool> done(false);
			std::thread writer([&]() {
				for (uint64_t i = 1; i <= 100000; ++i)
					box.ArmState(i, (i & 1) ? Cause::InPlaceSourceChange : Cause::RendererReset);
				done.store(true);
			});
			int torn = 0;
			while (!done.load()) {
				ArmBox::Snapshot s = box.Read(ArmBox::State);
				if (!s.ok) continue;
				const Cause expect = (s.key & 1) ? Cause::InPlaceSourceChange : Cause::RendererReset;
				if (s.key != 0 && s.cause != expect) ++torn;
			}
			writer.join();
			ArmBox::Snapshot last = box.Read(ArmBox::State);
			Assert::IsTrue(last.ok && last.key == 100000 && last.cause == Cause::RendererReset);
			Assert::AreEqual(0, torn);
		}

		TEST_METHOD(SameFactsGiveIdenticalDecisions)
		{
			Stream a, b;
			for (int i = 0; i < 40; ++i) {
				EnqueueFacts fa = a.Next();
				EnqueueFacts fb = b.Next();
				if (i == 10) { fa.gapBefore = fb.gapBefore = true; fa.gapCause = fb.gapCause = "stale capture delivery"; fa.gapFrames = fb.gapFrames = 2; }
				if (i == 20) { a.gen = b.gen = 2; fa.queueGeneration = fb.queueGeneration = 2; }
				if (i == 30) { fa.formatKey = fb.formatKey = 55; a.format = b.format = 55; }
				Decision da = a.Push(fa), db = b.Push(fb);
				Assert::AreEqual(static_cast<int>(da.count), static_cast<int>(db.count));
				for (int k = 0; k < da.count; ++k) {
					Assert::IsTrue(da.events[k].cause == db.events[k].cause);
					Assert::IsTrue(da.events[k].level == db.events[k].level);
					Assert::AreEqual(static_cast<unsigned long long>(da.events[k].fromSeq), static_cast<unsigned long long>(db.events[k].fromSeq));
				}
			}
		}

		TEST_METHOD(EightKHundredTwentyHertzClassifiesTheSame)
		{
			Stream s;
			s.rate = 120.0;
			s.format = 7680ull * 4320ull;
			s.ts = 1000000;
			s.tps = 1000000000;   // 1 GHz timebase
			ExpectOne(s.Push(), Level::Full, Cause::Start);
			s.ts += 8333333;
			ExpectNone(s.Push());
			// small forward break (about 3 periods) -> MINOR
			s.ts += 25000000;
			EnqueueFacts f = s.Next(); f.gapBefore = true; f.gapCause = "capture timestamp discontinuity";
			ExpectOne(s.Push(f), Level::Minor, Cause::TimestampBreak);
			// backwards beyond one 120 Hz period -> FULL
			s.ts -= 100000000;
			f = s.Next(); f.gapBefore = true; f.gapCause = "capture timestamp discontinuity";
			ExpectOne(s.Push(f), Level::Full, Cause::TimestampBreakLarge);
			// format change at 8K -> FULL
			s.format += 1;
			ExpectOne(s.Push(), Level::Full, Cause::SourceFormatChanged);
			// 120 -> 119.88 (1000 ppm) -> timing only
			s.rate = 119.88;
			ExpectOne(s.Push(), Level::Timing, Cause::RateStep);
		}

		TEST_METHOD(VocabularyHelpersAreConsistent)
		{
			Assert::IsTrue(LevelOf(Cause::Start) == Level::Full);
			Assert::IsTrue(LevelOf(Cause::DriftDrop) == Level::Minor);
			Assert::IsTrue(LevelOf(Cause::RateStep) == Level::Timing);
			Assert::IsTrue(LevelOf(Cause::SettingsApply) == Level::None);
			Assert::AreEqual("live_queue_reset", Token(Cause::LiveQueueReset));
			Assert::AreEqual("seek, skip or capture re-lock", Describe(Cause::LiveQueueReset));
			Assert::AreEqual("source format changed", Describe(Cause::SourceFormatChanged));
			Assert::IsTrue(ExcludedFromStorm(Cause::CalibrationStart));
			Assert::IsFalse(ExcludedFromStorm(Cause::Start));
		}
	};
}
