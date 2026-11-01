// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Q0-A part 1: the B2 enqueue cursor, the G8/D4 storm hold and its notice, the unthrottled VPEV1
// resync lines (all in ResyncShadow.h) and the "resync_storm" FallbackCatalog entry.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;

namespace Tests
{
	namespace ResyncQ0
	{
		struct Rig
		{
			Shadow shadow;
			std::vector<std::string> log;
			std::vector<std::string> lines;
			uint64_t seq = 0;
			uint64_t gen = 1;
			uint64_t counter = 100;
			double rate = 0.0;

			Rig()
			{
				RefusalNote::Take();   // the note is process-global
				shadow.SetLogger([this](const std::string& line) { log.push_back(line); });
			}

			void Push(double now)
			{
				EnqueueFacts f;
				f.sequence = ++seq;
				f.queueGeneration = gen;
				f.stateKey = 0x1000;
				f.formatKey = 7;
				f.frameCounter = ++counter;
				f.measuredRateHz = rate;
				shadow.OnEnqueue(f, now);
			}
			// A FULL (LiveQueueReset) at `now`.
			void Full(double now, const char* reason = "live queue reset")
			{
				++gen;
				shadow.ArmQueueGeneration(gen, reason);
				Push(now);
			}
			void CaptureLines()
			{
				shadow.SetEventSink([this](const std::string& line) { lines.push_back(line); });
			}
		};

		// Start + `resets` resets: Start (t=0) is a counted FULL, then one per second from t=1.
		inline void Storm(Rig& r, int resets)
		{
			r.Push(0.0);
			for (int i = 0; i < resets; ++i)
				r.Full(1.0 + i);
		}
	}

	using namespace ResyncQ0;

	TEST_CLASS(ResyncShadowQ0CursorTests)
	{
	public:
		TEST_METHOD(CursorOnlyGrowsAndResets)
		{
			Shadow s;
			Assert::AreEqual(uint64_t(0), s.PulldownCursorSeq());
			s.NoteEnqueued(5);
			s.NoteEnqueued(3);
			Assert::AreEqual(uint64_t(5), s.PulldownCursorSeq());
			s.NoteEnqueued(9);
			Assert::AreEqual(uint64_t(9), s.PulldownCursorSeq());
			s.ResetCursor();
			Assert::AreEqual(uint64_t(0), s.PulldownCursorSeq());
			s.NoteEnqueued(2);
			Assert::AreEqual(uint64_t(2), s.PulldownCursorSeq());
		}

		TEST_METHOD(RestartResetsTheCursor)
		{
			Shadow s;
			s.NoteEnqueued(40);
			s.Configure(Shadow::Mode::Shadow, 8);   // Off -> Shadow restarts the detector
			EnqueueFacts f;
			f.sequence = 1;
			f.queueGeneration = 1;
			f.frameCounter = 1;
			s.OnEnqueue(f, 1.0);
			Assert::AreEqual(uint64_t(0), s.PulldownCursorSeq());
		}

		TEST_METHOD(TwoThreadRaceNeverGoesBackwardsAndSeesTheFinalValue)
		{
			Shadow s;
			const uint64_t kFinal = 1000000;
			std::atomic<bool> go(false);
			std::atomic<int> violations(0);
			std::atomic<uint64_t> seen(0);
			std::thread writer([&]() {
				while (!go.load())
					std::this_thread::yield();
				for (uint64_t i = 1; i <= kFinal; ++i)
					s.NoteEnqueued(i);
			});
			std::thread reader([&]() {
				while (!go.load())
					std::this_thread::yield();
				uint64_t last = 0;
				for (;;) {
					const uint64_t v = s.PulldownCursorSeq();
					if (v < last)
						violations.fetch_add(1);
					last = v;
					if (v == kFinal)
						break;
					std::this_thread::yield();
				}
				seen.store(last);
			});
			go.store(true);
			writer.join();
			reader.join();
			Assert::AreEqual(0, violations.load());
			Assert::AreEqual(kFinal, seen.load());
			Assert::AreEqual(kFinal, s.PulldownCursorSeq());
		}
	};

	TEST_CLASS(ResyncShadowQ0StormTests)
	{
	public:
		TEST_METHOD(FourFullInTenSecondsEngagesTheHold)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::On, 8);
			Storm(r, 3);   // Start + 3 resets = 4 FULL
			Assert::IsTrue(r.shadow.StormHold(3.0));
			const Shadow::SafeSettings s = r.shadow.StormSafeSettings(3.0);
			Assert::IsTrue(s.laneT0 && s.nativeRebase && s.vectorsPaused);
		}

		TEST_METHOD(ThreeFullDoNotEngage)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::On, 8);
			Storm(r, 2);   // Start + 2 resets = 3 FULL
			Assert::IsFalse(r.shadow.StormHold(2.0));
			const Shadow::SafeSettings s = r.shadow.StormSafeSettings(2.0);
			Assert::IsFalse(s.laneT0 || s.nativeRebase || s.vectorsPaused);
			std::string text;
			Assert::IsFalse(r.shadow.StormNotice(text, 2.0));
		}

		TEST_METHOD(CalibrationCausesAreExcluded)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::On, 8);
			r.Push(0.0);
			for (int i = 0; i < 8; ++i)
				r.Full(1.0 + i, (i % 2) == 0 ? "calibration patterns" : "calibration patterns ended");
			Assert::IsFalse(r.shadow.StormHold(8.5));
		}

		TEST_METHOD(ReleasesTenSecondsAfterTheLastFullAndLaterFullExtendsIt)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::On, 8);
			Storm(r, 3);   // last FULL at t=3
			Assert::IsTrue(r.shadow.StormHold(12.5));
			Assert::IsFalse(r.shadow.StormHold(13.0), L"10 s since the last FULL: released");
			Assert::IsFalse(r.shadow.StormHold(60.0));

			// Held again, then one more FULL at t=8 keeps it held until t=18 even though the window thins.
			Rig q;
			q.shadow.Configure(Shadow::Mode::On, 8);
			Storm(q, 3);
			q.Full(8.0);
			Assert::IsTrue(q.shadow.StormHold(17.5));
			Assert::IsFalse(q.shadow.StormHold(18.0));
			// A lone FULL long after the release does not re-engage (latch was cleared).
			q.Full(40.0);
			Assert::IsFalse(q.shadow.StormHold(40.5));
		}

		TEST_METHOD(ShadowModeNeverHoldsButTheStatusSaysWouldHold)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			Storm(r, 3);
			Assert::IsFalse(r.shadow.StormHold(3.0));
			const Shadow::SafeSettings s = r.shadow.StormSafeSettings(3.0);
			Assert::IsFalse(s.laneT0 || s.nativeRebase || s.vectorsPaused);
			std::string text;
			Assert::IsFalse(r.shadow.StormNotice(text, 3.0));
			std::string status;
			Assert::IsTrue(r.shadow.StatusLine(status, 3.0));
			Assert::IsTrue(status.find("storm: 4 FULL in 10 s (would hold safe settings)") != std::string::npos);
		}

		TEST_METHOD(OffNeverHolds)
		{
			Shadow s;
			Assert::IsFalse(s.StormHold(1.0));
			std::string text;
			Assert::IsFalse(s.StormNotice(text, 1.0));
		}

		TEST_METHOD(NoticeTextCarriesTheFullCount)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::On, 8);
			Storm(r, 4);   // 5 FULL at t=0..4
			std::string text;
			Assert::IsTrue(r.shadow.StormNotice(text, 4.0));
			Assert::AreEqual(std::string("Input unstable: 5 resyncs in 10 s - processing held at safe settings"), text);
		}

		TEST_METHOD(CatalogEntryExists)
		{
			const FallbackCatalog::Decision on = FallbackCatalog::ResyncStorm(true, 4);
			Assert::IsTrue(on.engaged);
			Assert::AreEqual(std::string("resync_storm"), on.id);
			Assert::AreEqual(std::string("Input unstable: 4 resyncs in 10 s - processing held at safe settings"), on.reason);
			const FallbackCatalog::Decision off = FallbackCatalog::ResyncStorm(false, 4);
			Assert::IsFalse(off.engaged);
			Assert::AreEqual(std::string("resync_storm"), off.id);
			FallbackStatus::Registry registry;
			Assert::IsTrue(FallbackCatalog::Sync(registry, on));
			Assert::IsTrue(registry.Engaged("resync_storm"));
			Assert::IsTrue(FallbackCatalog::Sync(registry, off));
			Assert::IsFalse(registry.Engaged("resync_storm"));
		}
	};

	TEST_CLASS(ResyncShadowQ0EventSinkTests)
	{
	public:
		TEST_METHOD(FullMinorAndTimingLinesHaveTheVpevFormat)
		{
			Rig r;
			r.CaptureLines();
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.rate = 24.0;
			r.Push(1.0);   // FULL start at seq 1
			r.Push(2.0);
			r.shadow.OnLateSlip(Cause::DriftDrop, 7, 2, 2.5);   // MINOR
			r.rate = 24.05;
			r.Push(3.0);   // TIMING at seq 3
			Assert::AreEqual(size_t(3), r.lines.size());
			Assert::AreEqual(std::string("VPEV1 t=1000.000 kind=resync level=full cause=start seq=1 mode=shadow"),
				r.lines[0]);
			Assert::AreEqual(std::string("VPEV1 t=2500.000 kind=resync level=minor cause=drift_drop seq=7 gap=2 mode=shadow"),
				r.lines[1]);
			Assert::AreEqual(std::string("VPEV1 t=3000.000 kind=resync level=timing cause=rate_step seq=3 mode=shadow"),
				r.lines[2]);
		}

		TEST_METHOD(OnModeIsNamedInTheLine)
		{
			Rig r;
			r.CaptureLines();
			r.shadow.Configure(Shadow::Mode::On, 8);
			r.Push(1.0);
			Assert::AreEqual(size_t(1), r.lines.size());
			Assert::IsTrue(r.lines[0].find(" mode=on") != std::string::npos);
		}

		TEST_METHOD(NoSinkMeansNothingIsEmitted)
		{
			Rig r;
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(1.0);
			r.shadow.OnLateSlip(Cause::DriftDrop, 7, 2, 2.0);
			Assert::AreEqual(size_t(0), r.lines.size());
			Assert::AreEqual(uint32_t(0), r.shadow.Errors());
			// Setting a sink later only sees later events.
			r.CaptureLines();
			r.shadow.OnLateSlip(Cause::DriftDrop, 8, 1, 3.0);
			Assert::AreEqual(size_t(1), r.lines.size());
		}

		TEST_METHOD(TheSinkIsNotRateLimitedWhileTheHumanLogIs)
		{
			Rig r;
			r.CaptureLines();
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(1.0);   // FULL: one human line, one VPEV1 line
			r.log.clear();
			r.lines.clear();
			for (int i = 0; i < 100; ++i)
				r.shadow.OnLateSlip(Cause::DriftDrop, 10 + i, 1, 2.0 + i * 0.01);
			Assert::AreEqual(size_t(100), r.lines.size());
			size_t human = 0;   // the human log also carries rate-limited VPEV1 copies: count the others
			for (const std::string& l : r.log)
				if (l.compare(0, 5, "VPEV1") != 0)
					++human;
			Assert::AreEqual(size_t(7), human, L"human log: 1, 2, 4, 8, 16, 32, 64");
		}

		TEST_METHOD(AThrowingSinkIsContained)
		{
			Rig r;
			r.shadow.SetEventSink([](const std::string&) { throw 1; });
			r.shadow.Configure(Shadow::Mode::Shadow, 8);
			r.Push(1.0);
			Assert::AreEqual(uint32_t(1), r.shadow.Errors());
			Assert::AreEqual(uint32_t(1), r.shadow.GetIndicator()->Total(VpIndicator::kFull));
		}
	};
}
