// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Q0-A part 1: the B2 enqueue cursor, the G8/D4 storm hold and its notice, the unthrottled VPEV1
// resync lines (all in ResyncShadow.h) and the "resync_storm" FallbackCatalog entry.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FrameSeal.h>
#include <VP-V/modules/input/signal/DvResyncReader.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>
#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

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

	// Audit 2 MUST 2: an era swap (Shadow::Configure building a larger Indicator) while frames are queued
	// but not sealed. The old era's events for those frames must reach the new era, and a writer still
	// holding the old Indicator must not write into the dead era.
	TEST_CLASS(ResyncStressEraSwapTests)
	{
	public:
		static VpIndicator::CounterMask At(const VpIndicator::Indicator& ind, uint64_t seq)
		{
			return VpIndicator::Changed(ind.StampOf(seq - 1), ind.StampOf(seq));
		}
		static EnqueueFacts Facts(uint64_t seq, uint64_t gen, uint64_t counter, bool gap = false)
		{
			EnqueueFacts f;
			f.sequence = seq;
			f.queueGeneration = gen;
			f.stateKey = 0x1000;
			f.formatKey = 7;
			f.frameCounter = counter;
			if (gap) {
				f.gapBefore = true;
				f.gapCause = "missed frames";
				f.gapFrames = 1;
			}
			return f;
		}

		TEST_METHOD(EventsOnQueuedFramesSurviveAnEraSwapAndTheReadersActOnThem)
		{
			RefusalNote::Take();
			Shadow shadow;
			shadow.Configure(Shadow::Mode::On, 8);
			VpSeal::FrameSeal fs;
			Drive drive;
			MotionValidity::State motion;
			VpMotionResync::Readers mreaders;
			fs.AddProvider("drive", [&drive](const std::shared_ptr<VpIndicator::Indicator>& i) { drive.RegisterOn(i); });
			fs.AddProvider("motion", [&](const std::shared_ptr<VpIndicator::Indicator>& i) { VpMotionResync::RegisterOn(i, motion, mreaders); });
			fs.Subscribe("drive", [&drive](const std::shared_ptr<VpIndicator::Indicator>& i, uint64_t seq, VpIndicator::CounterMask c) { drive.OnObserved(i, seq, c); });

			// Queue depth 10: frames 1..12 enqueued, 1..2 sealed, 3..12 queued.
			for (uint64_t f = 1; f <= 12; ++f)
				shadow.OnEnqueue(Facts(f, 1, 100 + f), 1.0, 0.04 * double(f));
			fs.Seal(shadow.GetIndicator(), 1);
			fs.Seal(shadow.GetIndicator(), 2);
			for (size_t i = 0; i < Drive::kReaders; ++i)
				drive.Take(static_cast<Reader>(i));
			mreaders.TakeHistory();
			mreaders.TakeProducerNote();
			const std::shared_ptr<VpIndicator::Indicator> old = shadow.GetIndicator();
			// Events for future (queued) frames.
			shadow.OnLateSlip(Cause::DriftDrop, 6, 1, 1.0, 0.24);
			motion.Invalidate(MotionValidity::Cause::SettingsChange);   // settings[motion] at LastSealed + 1 = 3
			old->Advance(VpIndicator::kFull, Cause::LiveQueueReset, 9);
			old->Advance(VpIndicator::kRebaseH, Cause::RebaseDecision, 11);

			shadow.Configure(Shadow::Mode::On, 256);   // a larger ring: a new era
			const std::shared_ptr<VpIndicator::Indicator> ind = shadow.GetIndicator();
			Assert::IsTrue(ind != old, L"a new era");
			Assert::IsTrue(old->HandedOver());
			Assert::AreEqual(uint64_t(2), ind->LastSealed(), L"the seal floor is inherited");
			Assert::IsTrue((At(*ind, 3) & VpIndicator::Bit(VpIndicator::kSettingsMotion)) != 0, L"settings carried");
			Assert::IsTrue((At(*ind, 6) & VpIndicator::Bit(VpIndicator::kContinuity)) != 0, L"late slip carried");
			Assert::IsTrue((At(*ind, 9) & VpIndicator::Bit(VpIndicator::kFull)) != 0, L"FULL carried");
			Assert::IsTrue((At(*ind, 11) & VpIndicator::Bit(VpIndicator::kRebaseH)) != 0, L"rebase carried");
			Assert::AreEqual(VpIndicator::CounterMask(0), At(*ind, 5), L"nothing invented");
			Assert::IsFalse(motion.PairValidAt(5, 6), L"pairs spanning a carried event are refused before the rebind");
			Assert::IsFalse(motion.PairValidAt(8, 9));
			Assert::IsTrue(motion.PairValidAt(6, 7));

			// A writer still holding the dead era is forwarded (clamped above the old seal).
			old->Advance(VpIndicator::kContinuity, Cause::MissedFrames, 7, 1);
			Assert::IsTrue((At(*ind, 7) & VpIndicator::Bit(VpIndicator::kContinuity)) != 0, L"forwarded");
			Assert::AreEqual(uint32_t(1), old->ForwardedAdvances());

			// Render the queue in the new era: every reader acts at the carrying frame.
			const uint64_t epoch0 = motion.Epoch();
			unsigned contActs = 0, fullActs = 0;
			for (uint64_t f = 3; f <= 12; ++f) {
				fs.Seal(ind, f);
				const VpIndicator::CounterMask m = drive.Take(Reader::RenderAhead);
				if (f == 6 || f == 7)
					contActs += (m & VpIndicator::Bit(VpIndicator::kContinuity)) != 0 ? 1u : 0u;
				if (f == 9)
					fullActs += (m & VpIndicator::Bit(VpIndicator::kFull)) != 0 ? 1u : 0u;
			}
			Assert::AreEqual(2u, contActs, L"render-ahead relatched at both slips");
			Assert::AreEqual(1u, fullActs, L"render-ahead reset at the carried FULL");
			Assert::IsTrue(motion.Epoch() >= epoch0 + 3, L"motion epoch moved at 6, 7 and 9");
		}

		// Audit 3 MUST: a carried event on a frame that is never sealed in the new era (dropped from the
		// queue) must still act at the first sealed frame: the readers latch from the inherited seal, not
		// from the frame before the observed one.
		TEST_METHOD(ACarriedEventOnADroppedFrameStillActsAfterTheSwap)
		{
			RefusalNote::Take();
			Shadow shadow;
			shadow.Configure(Shadow::Mode::On, 8);
			VpSeal::FrameSeal fs;
			Drive drive;
			MotionValidity::State motion;
			VpMotionResync::Readers mreaders;
			VpDvResync::Reader dv;
			fs.AddProvider("drive", [&drive](const std::shared_ptr<VpIndicator::Indicator>& i) { drive.RegisterOn(i); });
			fs.AddProvider("motion", [&](const std::shared_ptr<VpIndicator::Indicator>& i) { VpMotionResync::RegisterOn(i, motion, mreaders); });
			fs.AddProvider("dv", [&dv](const std::shared_ptr<VpIndicator::Indicator>& i) { dv.RegisterOn(i); });
			fs.Subscribe("drive", [&drive](const std::shared_ptr<VpIndicator::Indicator>& i, uint64_t seq, VpIndicator::CounterMask c) { drive.OnObserved(i, seq, c); });
			for (uint64_t f = 1; f <= 8; ++f)
				shadow.OnEnqueue(Facts(f, 1, 100 + f), 1.0, 0.04 * double(f));
			for (uint64_t f = 1; f <= 4; ++f)
				fs.Seal(shadow.GetIndicator(), f);
			drive.Take(Reader::RenderAhead);
			mreaders.TakeHistory();
			dv.Take();
			const uint64_t relatches0 = dv.Relatches();
			// A FULL on frame 5 (the first unsealed frame), then the ring grows, then frame 5 is dropped.
			shadow.GetIndicator()->Advance(VpIndicator::kFull, Cause::InPlaceSourceChange, 5);
			shadow.Configure(Shadow::Mode::On, 256);
			const std::shared_ptr<VpIndicator::Indicator> ind = shadow.GetIndicator();
			Assert::AreEqual(uint64_t(4), ind->LatchBaseline());
			fs.Seal(ind, 6);   // frame 5 never sealed
			Assert::IsTrue((drive.Take(Reader::RenderAhead) & VpIndicator::Bit(VpIndicator::kFull)) != 0, L"render-ahead resets at 6");
			const VpMotionResync::HistoryPending h = mreaders.TakeHistory();
			Assert::IsTrue(h.changed && h.cause == MotionValidity::Cause::SourceFormatChange, L"motion history acts with the carried cause");
			Assert::AreEqual(relatches0 + 1, dv.Relatches(), L"DV relatches at 6");
			// Pairs reaching back across the swap are refused (ancient), the baseline frame is not.
			Assert::IsFalse(motion.PairValidAt(2, 3), L"older than the inherited seal: ancient");
			Assert::IsTrue(motion.PairValidAt(6, 7));
		}

		// A writer thread that keeps a STALE pointer to the first era (and one that reloads it) writes
		// events on future frames while the render thread swaps eras several times. Every event must be in
		// the newest era exactly once, whatever the interleaving.
		TEST_METHOD(ConcurrentWritersAcrossRepeatedEraSwapsLoseNothing)
		{
			for (int round = 0; round < 20; ++round) {
				Shadow shadow;
				shadow.Configure(Shadow::Mode::On, 8);
				const std::shared_ptr<VpIndicator::Indicator> first = shadow.GetIndicator();
				for (uint64_t f = 1; f <= 10; ++f)
					first->SealAndObserve(f);   // sealed through 10: events land on 11+
				MotionValidity::State motion;
				motion.Bind(first);   // bound to the first era and never rebound here
				const int kSlips = 20, kSettings = 20;
				std::atomic<int> go(0);
				std::thread stale([&] {
					while (go.load() == 0) std::this_thread::yield();
					for (int i = 0; i < kSlips; i += 2) {
						first->Advance(VpIndicator::kContinuity, Cause::MissedFrames, 11 + 2 * uint64_t(i), 1);
						std::this_thread::yield();
					}
				});
				std::thread fresh([&] {
					while (go.load() == 0) std::this_thread::yield();
					for (int i = 1; i < kSlips; i += 2) {
						shadow.GetIndicator()->Advance(VpIndicator::kContinuity, Cause::MissedFrames, 11 + 2 * uint64_t(i), 1);
						std::this_thread::yield();
					}
				});
				std::thread settings([&] {
					while (go.load() == 0) std::this_thread::yield();
					for (int i = 0; i < kSettings; ++i) {
						motion.Invalidate(MotionValidity::Cause::SettingsChange);
						std::this_thread::yield();
					}
				});
				go.store(1);
				const size_t sizes[] = { 32, 64, 128, 256 };
				for (size_t n : sizes) {
					std::this_thread::yield();
					shadow.Configure(Shadow::Mode::On, n);
				}
				stale.join();
				fresh.join();
				settings.join();
				const std::shared_ptr<VpIndicator::Indicator> last = shadow.GetIndicator();
				Assert::IsTrue(last != first);
				Assert::IsTrue(VpIndicator::Indicator::Latest(first) == last, L"the handover chain ends at the current era");
				for (int i = 0; i < kSlips; ++i) {
					const uint64_t s = 11 + 2 * uint64_t(i);
					Assert::IsTrue((At(*last, s) & VpIndicator::Bit(VpIndicator::kContinuity)) != 0, L"every slip reached the newest era");
				}
				Assert::AreEqual(uint32_t(kSlips), last->Total(VpIndicator::kContinuity), L"each slip exactly once");
				Assert::AreEqual(uint32_t(kSettings), last->Total(VpIndicator::kSettingsMotion), L"each settings advance exactly once");
				Assert::AreEqual(uint32_t(0), last->PrematureFolds());
				Assert::AreEqual(uint64_t(10), last->LastSealed(), L"the seal floor followed every handover");
			}
		}

		// The real enqueue path (the detector under the queue mutex, here one enqueue thread) races the
		// render thread's swaps with a queue of >= 8 frames: every gap MINOR stays on its frame.
		TEST_METHOD(EnqueueRacingSwapsKeepsEveryGapOnItsFrame)
		{
			for (int round = 0; round < 20; ++round) {
				RefusalNote::Take();
				Shadow shadow;
				shadow.Configure(Shadow::Mode::On, 8);
				const std::shared_ptr<VpIndicator::Indicator> first = shadow.GetIndicator();
				uint64_t counter = 100;
				for (uint64_t f = 1; f <= 10; ++f)
					shadow.OnEnqueue(Facts(f, 1, ++counter), 1.0, 0.04 * double(f));
				first->SealAndObserve(2);   // 3..10 queued
				std::atomic<int> go(0);
				std::thread enqueue([&] {
					while (go.load() == 0) std::this_thread::yield();
					uint64_t c = counter;
					for (uint64_t f = 11; f <= 80; ++f) {
						const bool gap = (f % 5) == 0;
						c += gap ? 2 : 1;
						shadow.OnEnqueue(Facts(f, 1, c, gap), 1.0, 0.04 * double(f));
					}
				});
				go.store(1);
				const size_t sizes[] = { 32, 64, 128, 256, 512 };
				for (size_t n : sizes) {
					std::this_thread::yield();
					shadow.Configure(Shadow::Mode::On, n);
				}
				enqueue.join();
				const std::shared_ptr<VpIndicator::Indicator> last = shadow.GetIndicator();
				Assert::AreEqual(uint32_t(0), shadow.Errors());
				for (uint64_t f = 15; f <= 80; f += 5)
					Assert::IsTrue((At(*last, f) & VpIndicator::Bit(VpIndicator::kContinuity)) != 0, L"the gap MINOR is on its frame in the newest era");
			}
		}
	};
}
