/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// R1: the resync shadow mode (status text, event lines, Off cost path, restarts).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/ResyncShadow.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;

namespace Tests
{
	namespace ResyncR1
	{
		struct Rig
		{
			Shadow shadow;
			std::vector<std::string> log;
			uint64_t seq = 0;
			uint64_t gen = 1;
			uint64_t state = 0x1000;
			uint64_t format = 7;
			uint64_t counter = 100;
			double rate = 0.0;

			Rig()
			{
				RefusalNote::Take();   // the note is process-global
				shadow.SetLogger([this](const std::string& line) { log.push_back(line); });
			}

			EnqueueFacts Next()
			{
				EnqueueFacts f;
				f.sequence = ++seq;
				f.queueGeneration = gen;
				f.stateKey = state;
				f.formatKey = format;
				f.frameCounter = ++counter;
				f.measuredRateHz = rate;
				return f;
			}
			void Push(double now) { shadow.OnEnqueue(Next(), now); }
			void PushGap(double now, const char* text, uint32_t frames)
			{
				EnqueueFacts f = Next();
				f.gapBefore = true;
				f.gapCause = text;
				f.gapFrames = frames;
				shadow.OnEnqueue(f, now);
			}
			std::string Status(double now) const
			{
				std::string s;
				Assert::IsTrue(shadow.StatusLine(s, now));
				return s;
			}
			std::string Events() const
			{
				std::string s;
				Assert::IsTrue(shadow.EventLines(s));
				return s;
			}
		};
	}

	using namespace ResyncR1;

	TEST_CLASS(ResyncShadowTests)
	{
	public:
		TEST_METHOD(OffWritesNothingAndHasNoLines)
		{
			Rig r;
			Assert::IsTrue(r.shadow.GetMode() == Shadow::Mode::Off);
			r.shadow.ArmQueueGeneration(5, "start");
			r.shadow.ArmInPlaceSourceChange(0x99);
			r.shadow.NoteRefusal(RefusalReason::DisplayMode);
			Assert::AreEqual(uint64_t(0), r.shadow.Arms().Read(ArmBox::Queue).version);
			Assert::AreEqual(uint64_t(0), r.shadow.Arms().Read(ArmBox::State).version);
			Assert::IsTrue(RefusalNote::Take() == RefusalReason::None, L"Off must not note a refusal");
			for (int i = 0; i < 100; ++i)
				r.Push(double(i));
			Assert::IsTrue(r.shadow.GetIndicator() == nullptr, L"Off never builds an Indicator");
			Assert::IsTrue(r.log.empty());
			std::string s = "untouched";
			Assert::IsFalse(r.shadow.StatusLine(s, 1.0));
			Assert::IsFalse(r.shadow.EventLines(s));
			Assert::AreEqual(std::string("untouched"), s);
		}

		TEST_METHOD(ShadowThenOffGivesNoLinesAndNoMoreWrites)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(1.0);
			const std::shared_ptr<VpIndicator::Indicator> ind = r.shadow.GetIndicator();
			Assert::IsTrue(ind != nullptr);
			const uint32_t before = ind->Total(VpIndicator::kFull);
			r.shadow.Configure(Shadow::Mode::Off, 8);
			r.PushGap(2.0, "missed frames", 1);
			r.shadow.NoteRefusal(RefusalReason::Encoding);
			Assert::AreEqual(before, ind->Total(VpIndicator::kFull));
			Assert::AreEqual(uint32_t(0), ind->Total(VpIndicator::kContinuity));
			std::string s;
			Assert::IsFalse(r.shadow.StatusLine(s, 3.0));
			Assert::IsFalse(r.shadow.EventLines(s));
			Assert::IsTrue(RefusalNote::Take() == RefusalReason::None);
		}

		TEST_METHOD(StatusTextForStartAndAgeing)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			Assert::AreEqual(std::string("shadow: no FULL yet; minor x0 / 10 min; timing x0"), r.Status(5.0));
			r.Push(10.0);
			Assert::AreEqual(std::string("shadow: FULL 0 s ago (start); minor x0 / 10 min; timing x0"), r.Status(10.0));
			Assert::AreEqual(std::string("shadow: FULL 3 s ago (start); minor x0 / 10 min; timing x0"), r.Status(13.4));
			Assert::AreEqual(size_t(1), r.log.size());
			Assert::AreEqual(std::string("Resync (shadow): FULL at seq 1 (start)"), r.log[0]);
		}

		TEST_METHOD(SourceFormatChangeIsNamed)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(1.0);
			r.Push(2.0);
			r.format = 8;
			r.Push(3.0);
			Assert::AreEqual(std::string("shadow: FULL 3 s ago (source format changed); minor x0 / 10 min; timing x0"),
				r.Status(6.0));
		}

		TEST_METHOD(MinorRunCountsInTheTenMinuteWindowAndLogsOnPowersOfTwo)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(0.0);
			for (int i = 0; i < 12; ++i)
				r.PushGap(1.0 + i, "missed frames", 1);
			Assert::AreEqual(std::string("shadow: FULL 12 s ago (start); minor x12 / 10 min; timing x0"), r.Status(12.0));
			// 1 FULL + minor logs at counts 1, 2, 4, 8.
			Assert::AreEqual(size_t(5), r.log.size());
			Assert::AreEqual(std::string("Resync (shadow): MINOR at seq 2 (missed frames; gap 1)"), r.log[1]);
			Assert::AreEqual(std::string("Resync (shadow): MINOR at seq 3 (missed frames; gap 1)"), r.log[2]);
			Assert::AreEqual(std::string("Resync (shadow): MINOR at seq 5 (missed frames; gap 1)"), r.log[3]);
			Assert::AreEqual(std::string("Resync (shadow): MINOR at seq 9 (missed frames; gap 1)"), r.log[4]);
			// The window slides: minors older than 600 s drop out (minors were at 1..12 s).
			Assert::AreEqual(std::string("shadow: FULL 607 s ago (start); minor x6 / 10 min; timing x0"), r.Status(607.0));
		}

		TEST_METHOD(MinorCountSaturatesAtTheRingSize)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 100);
			r.Push(0.0);
			for (int i = 0; i < 300; ++i)
				r.PushGap(1.0 + 0.01 * i, "missed frames", 1);
			Assert::AreEqual(std::string("shadow: FULL 5 s ago (start); minor x256+ / 10 min; timing x0"), r.Status(5.0));
		}

		TEST_METHOD(AThrowingLoggerDoesNotPropagateAndShowsTheErrorCount)
		{
			Rig r;
			r.shadow.SetLogger([](const std::string&) { throw std::runtime_error("log sink"); });
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(1.0);   // FULL start -> logged -> throws inside the logger
			Assert::AreEqual(uint32_t(1), r.shadow.Errors());
			Assert::AreEqual(uint32_t(1), r.shadow.GetIndicator()->Total(VpIndicator::kFull), L"the verdict is still recorded");
			Assert::AreEqual(std::string("shadow: FULL 0 s ago (start); minor x0 / 10 min; timing x0; shadow error x1"), r.Status(1.0));
			Assert::IsTrue(r.shadow.Active());
		}

		TEST_METHOD(TimingStepIsCountedAndLoggedOnce)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.rate = 24.0;
			r.Push(1.0);
			r.Push(2.0);
			r.rate = 24.05;   // about 2000 ppm: above the 500 ppm default
			r.Push(3.0);
			Assert::AreEqual(std::string("shadow: FULL 2 s ago (start); minor x0 / 10 min; timing x1"), r.Status(3.0));
			Assert::AreEqual(size_t(2), r.log.size());
			Assert::AreEqual(std::string("Resync (shadow): TIMING at seq 3 (frame rate step)"), r.log[1]);
			Assert::AreEqual(uint32_t(1), r.shadow.GetIndicator()->Total(VpIndicator::kTiming));
		}

		TEST_METHOD(StormAddsItsClauseAndLeavesAfterTheWindow)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(0.0);                       // FULL 1: start
			for (int i = 0; i < 2; ++i) {      // FULL 2, 3: counter backwards
				r.counter = 10 - 5 * i;
				r.Push(1.0 + i);
			}
			Assert::AreEqual(std::string(
				"shadow: FULL 0 s ago (frame counter went backwards); minor x0 / 10 min; timing x0"), r.Status(2.0));
			r.counter = 1;
			r.Push(3.0);                       // FULL 4: more than 3 in 10 s
			Assert::AreEqual(std::string(
				"shadow: FULL 0 s ago (frame counter went backwards); minor x0 / 10 min; timing x0; "
				"storm: 4 FULL in 10 s (would hold safe settings)"), r.Status(3.0));
			Assert::AreEqual(std::string(
				"shadow: FULL 20 s ago (frame counter went backwards); minor x0 / 10 min; timing x0"), r.Status(23.0));
			Assert::AreEqual(size_t(4), r.log.size(), L"every FULL is logged");
		}

		TEST_METHOD(CalibrationNeverMakesAStorm)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(0.0);
			for (int i = 0; i < 6; ++i) {
				++r.gen;
				r.shadow.ArmQueueGeneration(r.gen, (i % 2) == 0 ? "calibration patterns" : "calibration patterns ended");
				r.Push(1.0 + i);
			}
			const std::string s = r.Status(8.0);
			Assert::IsTrue(s.find("storm") == std::string::npos, L"calibration is excluded");
			Assert::IsTrue(s.find("calibration patterns ended") != std::string::npos);
		}

		TEST_METHOD(QueueReasonAndRefusalReachTheText)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(0.0);
			++r.gen;
			r.shadow.ArmQueueGeneration(r.gen, "live queue reset");
			r.Push(1.0);
			Assert::AreEqual(std::string("shadow: FULL 0 s ago (seek, skip or capture re-lock); minor x0 / 10 min; timing x0"),
				r.Status(1.0));

			// A refused change is noted, then the rebuilt renderer's first frame says why.
			Rig q;
			q.shadow.Configure(Shadow::Mode::Shadow, 8);
			q.shadow.NoteRefusal(RefusalReason::Encoding);
			q.Push(5.0);
			Assert::AreEqual(std::string(
				"shadow: FULL 0 s ago (start after a source change; encoding change); minor x0 / 10 min; timing x0"),
				q.Status(5.0));
		}

		TEST_METHOD(InPlaceSourceChangeIsArmedByStateKey)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(0.0);
			r.state = 0x2000;
			r.shadow.ArmInPlaceSourceChange(0x2000);
			r.Push(1.0);
			Assert::AreEqual(std::string("shadow: FULL 0 s ago (source change); minor x0 / 10 min; timing x0"), r.Status(1.0));
		}

		TEST_METHOD(EventLinesFormat)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			const std::shared_ptr<VpIndicator::Indicator> ind = r.shadow.GetIndicator();
			const std::string prefix = "era=" + std::to_string(ind->Era()) + " capacity=" +
				std::to_string(ind->Capacity()) + " ancient_below=0 premature_folds=0 clamped=0; ";
			Assert::AreEqual(prefix, r.Events());
			r.Push(0.0);
			r.PushGap(1.0, "missed frames", 2);
			r.rate = 24.0;
			r.Push(2.0);
			r.rate = 25.0;
			r.Push(3.0);
			Assert::AreEqual(prefix +
				"#1 seq=1 full start readers=none | "
				"#2 seq=2 continuity missed_frames gap=2 readers=none | "
				"#3 seq=4 timing rate_step readers=none", r.Events());
			Assert::AreEqual(size_t(64), ind->Capacity());
		}

		TEST_METHOD(EventLinesShowTheRefusalAndTheLastSixtyFour)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.shadow.NoteRefusal(RefusalReason::DisplayMode);
			r.Push(0.0);
			Assert::IsTrue(r.Events().find("#1 seq=1 full start_after_source_change detail=display mode (incl. 59.94/60 re-latch) readers=none")
				!= std::string::npos);
			for (int i = 0; i < 100; ++i)
				r.PushGap(1.0 + i, "missed frames", 1);
			const std::string text = r.Events();
			size_t items = 1;
			for (size_t at = text.find(" | "); at != std::string::npos; at = text.find(" | ", at + 3))
				++items;
			Assert::AreEqual(size_t(64), items);
			Assert::IsTrue(text.find("#101 seq=101 continuity missed_frames gap=1") != std::string::npos);
			Assert::IsTrue(text.find("#1 seq=1 ") == std::string::npos);
		}

		TEST_METHOD(OffToShadowBuildsANewEraAndRestartsWithAFullStart)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(0.0);
			r.PushGap(1.0, "missed frames", 1);
			const uint32_t era1 = r.shadow.GetIndicator()->Era();
			r.shadow.Configure(Shadow::Mode::Off, 8);
			r.Push(2.0);
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			const std::shared_ptr<VpIndicator::Indicator> second = r.shadow.GetIndicator();
			Assert::IsTrue(second->Era() != era1, L"a fresh Indicator, a new era");
			Assert::AreEqual(uint32_t(0), second->Total(VpIndicator::kFull));
			r.Push(4.0);   // same stream, same generation: only the restart makes this a Start
			Assert::AreEqual(uint32_t(1), second->Total(VpIndicator::kFull));
			Assert::AreEqual(std::string("shadow: FULL 0 s ago (start); minor x0 / 10 min; timing x0"), r.Status(4.0));
			Assert::IsTrue(r.Events().find("missed_frames") == std::string::npos);
		}

		TEST_METHOD(SameCapacityKeepsTheIndicatorAndGrowthBuildsANewOne)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 4);
			r.Push(0.0);
			const std::shared_ptr<VpIndicator::Indicator> first = r.shadow.GetIndicator();
			Assert::AreEqual(size_t(64), first->Capacity());
			r.shadow.Configure(Shadow::Mode::Shadow, 4);   // settings applied again, nothing changed
			r.shadow.Configure(Shadow::Mode::Shadow, 16);  // still 64 events
			Assert::IsTrue(r.shadow.GetIndicator() == first);
			r.PushGap(1.0, "missed frames", 1);
			Assert::AreEqual(uint32_t(1), first->Total(VpIndicator::kFull));
			Assert::AreEqual(uint32_t(1), first->Total(VpIndicator::kContinuity));

			r.shadow.Configure(Shadow::Mode::Shadow, 100);   // 100 * 4 -> 512
			const std::shared_ptr<VpIndicator::Indicator> grown = r.shadow.GetIndicator();
			Assert::IsTrue(grown != first);
			Assert::AreEqual(size_t(512), grown->Capacity());
			Assert::IsTrue(grown->Era() != first->Era());
			r.Push(2.0);   // restart: a FULL Start again
			Assert::AreEqual(uint32_t(1), grown->Total(VpIndicator::kFull));
			Assert::AreEqual(std::string("shadow: FULL 0 s ago (start); minor x0 / 10 min; timing x0"), r.Status(2.0));
		}

		TEST_METHOD(ShrinkingFramesInFlightKeepsTheLargerIndicator)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 100);
			const std::shared_ptr<VpIndicator::Indicator> big = r.shadow.GetIndicator();
			r.shadow.Configure(Shadow::Mode::Shadow, 4);
			Assert::IsTrue(r.shadow.GetIndicator() == big);
		}
	};
}
