// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Resync audit-1 fixes: Indicator::AnyCauseOtherThanIn and the pulldown's mixed-interval rule, the
// Shadow enqueue cursor reset on a new Indicator, the storm hold on SOURCE time (reproducible runs),
// and the motion readers re-arming on a new era (FrameSeal re-registration) with the allocation-free
// cause lookup (unknown/ancient = conservative).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FrameSeal.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/modules/input/queue_timing/PulldownResync.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>
#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	using VpIndicator::Indicator;
	using RCause = VpResync::Cause;

	TEST_CLASS(ResyncAuditIndicatorTests)
	{
	public:
		TEST_METHOD(AnyCauseOtherThanInSeesEveryEventInTheInterval)
		{
			Indicator ind(3, 64);
			ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 3);
			Assert::IsFalse(ind.AnyCauseOtherThanIn(VpIndicator::kFull, RCause::LiveQueueReset, 2, 4), L"only the excepted cause");
			ind.Advance(VpIndicator::kContinuity, RCause::DriftDrop, 3, 1);
			Assert::IsFalse(ind.AnyCauseOtherThanIn(VpIndicator::kFull, RCause::LiveQueueReset, 2, 4), L"other counters are not looked at");
			ind.Advance(VpIndicator::kFull, RCause::RendererReset, 4);
			Assert::IsTrue(ind.AnyCauseOtherThanIn(VpIndicator::kFull, RCause::LiveQueueReset, 2, 4), L"a second FULL with another cause");
			Assert::IsFalse(ind.AnyCauseOtherThanIn(VpIndicator::kFull, RCause::LiveQueueReset, 2, 3), L"outside the interval");
			Assert::IsFalse(ind.AnyCauseOtherThanIn(VpIndicator::kFull, RCause::LiveQueueReset, 4, 4), L"empty interval");
			Assert::IsFalse(ind.AnyCauseOtherThanIn(VpIndicator::kCounterCount, RCause::LiveQueueReset, 0, 9), L"bad counter");
		}

		TEST_METHOD(AnIntervalOlderThanTheRingIsConservative)
		{
			Indicator ind(3, 64);
			for (uint64_t i = 0; i < 80; ++i)
				ind.Advance(VpIndicator::kContinuity, RCause::DriftDrop, 10 + i, 1);   // folds the oldest
			Assert::IsTrue(ind.AncientBelow() > 0);
			Assert::IsTrue(ind.AnyCauseOtherThanIn(VpIndicator::kFull, RCause::LiveQueueReset, 0, 5), L"cannot prove only the excepted cause");
		}

		TEST_METHOD(PulldownAbsorbsOnlyWhenEveryFullWasALiveQueueReset)
		{
			const VpResync::PulldownResyncPolicy::ClassFn cls = [](double) { return 0; };
			const VpIndicator::CounterMask f = VpIndicator::Bit(VpIndicator::kFull);
			{
				Indicator ind(3, 64);
				ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 3);
				ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 4);
				const VpResync::PulldownResyncAction a = VpResync::PulldownResyncPolicy::DecideFromIndicator(ind, 2, 4, f, false, 0, 0, cls);
				Assert::IsTrue(a.absorb && !a.reset, L"two seeks: absorb");
			}
			{
				Indicator ind(3, 64);
				ind.Advance(VpIndicator::kFull, RCause::RendererReset, 3);
				ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 4);   // the newest is a seek
				const VpResync::PulldownResyncAction a = VpResync::PulldownResyncPolicy::DecideFromIndicator(ind, 2, 4, f, false, 0, 0, cls);
				Assert::IsTrue(a.reset && !a.absorb, L"a renderer reset before the seek in one interval: reset");
			}
			{
				Indicator ind(3, 64);
				ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 3);
				ind.Advance(VpIndicator::kFull, RCause::RendererReset, 4);   // the newest is the reset
				const VpResync::PulldownResyncAction a = VpResync::PulldownResyncPolicy::DecideFromIndicator(ind, 2, 4, f, false, 0, 0, cls);
				Assert::IsTrue(a.reset && !a.absorb, L"reset");
			}
			{
				Indicator ind(3, 64);
				for (uint64_t i = 0; i < 80; ++i)
					ind.Advance(VpIndicator::kFull, RCause::LiveQueueReset, 10 + i);
				const VpResync::PulldownResyncAction a = VpResync::PulldownResyncPolicy::DecideFromIndicator(ind, 0, 5, f, false, 0, 0, cls);
				Assert::IsTrue(a.reset, L"an interval older than the ring resets");
			}
		}
	};

	TEST_CLASS(ResyncAuditShadowTests)
	{
	public:
		static void Facts(VpResync::Shadow& s, uint64_t& seq, uint64_t gen, double now, double source)
		{
			VpResync::EnqueueFacts f;
			f.sequence = ++seq;
			f.queueGeneration = gen;
			f.stateKey = 0x1000;
			f.formatKey = 7;
			f.frameCounter = 100 + seq;
			s.OnEnqueue(f, now, source);
		}

		// A run of Start + 4 seeks at source seconds 0..4 with an arbitrary wall clock; returns the hold
		// at a series of source times.
		static std::vector<int> Run(double wallBase, double wallStep)
		{
			VpResync::Shadow s;
			s.Configure(VpResync::Shadow::Mode::On, 8);
			s.SetStormSourceTime(true);
			uint64_t seq = 0;
			uint64_t gen = 1;
			Facts(s, seq, gen, wallBase, 0.0);
			for (int i = 0; i < 4; ++i) {
				++gen;
				s.ArmQueueGeneration(gen, "live queue reset");
				Facts(s, seq, gen, wallBase + (i + 1) * wallStep, 1.0 + i);
			}
			std::vector<int> out;
			const double probes[] = { 4.0, 8.0, 13.9, 14.1, 30.0 };
			for (double t : probes) {
				s.AdvanceStormClock(t);
				out.push_back(s.StormHold(t) ? 1 : 0);   // the rendered frame's source time
			}
			return out;
		}

		TEST_METHOD(StormHoldOnSourceTimeIsIndependentOfTheWallClock)
		{
			const std::vector<int> a = Run(100.0, 0.001);
			const std::vector<int> b = Run(5000.0, 777.0);
			Assert::IsTrue(a == b, L"identical whatever the wall clock");
			const std::vector<int> expect = { 1, 1, 1, 0, 0 };
			Assert::IsTrue(a == expect, L"held until 10 s after the last FULL, in source time");
		}

		// Audit 2 MUST 3: the hold is decided for the RENDERED frame. Two storms (source 0..3 and 20..23);
		// the enqueue thread either keeps pace with the render or has already enqueued every frame. The
		// hold seen at each rendered frame must be identical.
		static std::vector<int> TwoStorms(bool enqueueAhead)
		{
			VpResync::Shadow s;
			s.Configure(VpResync::Shadow::Mode::On, 64);
			s.SetStormSourceTime(true);
			const int kFrames = 80;   // 0.5 s per frame: source 0 .. 39.5
			uint64_t seq = 0, gen = 1;
			auto enqueue = [&](int f) {
				const double src = 0.5 * f;
				const bool full = (f >= 2 && f <= 6 && (f % 2) == 0) || (f >= 40 && f <= 46 && (f % 2) == 0);
				if (full) {
					++gen;
					s.ArmQueueGeneration(gen, "live queue reset");
				}
				Facts(s, seq, gen, 9000.0 + f, src);
			};
			std::vector<int> out;
			if (enqueueAhead)
				for (int f = 0; f < kFrames; ++f)
					enqueue(f);
			for (int f = 0; f < kFrames; ++f) {
				if (!enqueueAhead)
					enqueue(f);
				out.push_back(s.StormHold(0.5 * f) ? 1 : 0);
			}
			return out;
		}

		TEST_METHOD(StormHoldIsDecidedAtTheRenderedFrameNotByTheEnqueueThread)
		{
			const std::vector<int> paced = TwoStorms(false);
			const std::vector<int> ahead = TwoStorms(true);
			Assert::IsTrue(paced == ahead, L"enqueue running ahead must not change the hold");
			Assert::AreEqual(0, paced[5], L"not held before the 4th FULL (source 3)");
			Assert::AreEqual(1, paced[6], L"held from the 4th FULL");
			Assert::AreEqual(1, paced[25], L"still held 9.5 s after the last FULL");
			Assert::AreEqual(0, paced[26], L"released 10 s after the last FULL");
			Assert::AreEqual(0, paced[39], L"not held before the second storm's events");
			Assert::AreEqual(1, paced[46], L"the second storm holds at its 4th FULL");
		}

		// Audit 2 SHOULD: a reader the Indicator refused is a visible seal fallback.
		TEST_METHOD(SealFallbackShowsReadersTheIndicatorRefused)
		{
			VpSeal::FrameSeal fs;
			const std::shared_ptr<Indicator> ind = std::make_shared<Indicator>(VpIndicator::NextEra(), 64);
			fs.Seal(ind, 1);
			Assert::IsFalse(fs.Fallback().engaged);
			std::array<VpIndicator::Policy, VpIndicator::kCounterCount> p{};
			Assert::AreEqual(-1, ind->Register("late", VpIndicator::Bit(VpIndicator::kFull), p, nullptr));
			const FallbackCatalog::Decision d = fs.Fallback();
			Assert::IsTrue(d.engaged, L"engaged");
			Assert::IsTrue(d.reason.find("1 resync reader(s) refused") != std::string::npos, L"says why");
		}

		// Audit 2 SHOULD: the enqueue cursor never moves backwards except through ResetCursor.
		TEST_METHOD(CursorCompareExchangeIsMonotonic)
		{
			VpResync::Shadow s;
			s.NoteEnqueued(9);
			s.NoteEnqueued(4);
			Assert::AreEqual(uint64_t(9), s.PulldownCursorSeq());
			s.ResetCursor();
			s.NoteEnqueued(3);
			Assert::AreEqual(uint64_t(3), s.PulldownCursorSeq());
		}

		TEST_METHOD(SourceTimeModeCountsEventsWithoutSourceTime)
		{
			VpResync::Shadow s;
			s.Configure(VpResync::Shadow::Mode::On, 8);
			s.SetStormSourceTime(true);
			uint64_t seq = 0;
			Facts(s, seq, 1, 5.0, -1.0);
			Assert::IsTrue(s.StormClockMissing() > 0, L"counted");
			std::string line;
			Assert::IsTrue(s.StatusLine(line, 5.0));
			Assert::IsTrue(line.find("storm clock: source time") != std::string::npos, L"shown in the status line");
		}

		TEST_METHOD(ConfigureWithANewIndicatorResetsTheEnqueueCursorAtOnce)
		{
			VpResync::Shadow s;
			s.NoteEnqueued(40);
			s.Configure(VpResync::Shadow::Mode::Shadow, 8);
			Assert::AreEqual(uint64_t(0), s.PulldownCursorSeq(), L"before any OnEnqueue");
			s.NoteEnqueued(7);
			s.Configure(VpResync::Shadow::Mode::On, 8);   // same Indicator: keeps the cursor
			Assert::AreEqual(uint64_t(7), s.PulldownCursorSeq());
			s.Configure(VpResync::Shadow::Mode::Shadow, 100000);   // larger ring: a new Indicator
			Assert::AreEqual(uint64_t(0), s.PulldownCursorSeq());
		}
	};

	TEST_CLASS(ResyncAuditMotionReaderTests)
	{
	public:
		struct Rig
		{
			MotionValidity::State state;
			VpMotionResync::Readers readers;
			VpSeal::FrameSeal fs;
			Rig()
			{
				fs.AddProvider("motion", [this](const std::shared_ptr<Indicator>& i) { VpMotionResync::RegisterOn(i, state, readers); });
			}
			static std::shared_ptr<Indicator> NewEra() { return std::make_shared<Indicator>(VpIndicator::NextEra(), 64); }
			void Frames(const std::shared_ptr<Indicator>& i, uint64_t from, uint64_t to)
			{
				for (uint64_t s = from; s <= to; ++s)
					fs.Seal(i, s);
			}
		};

		TEST_METHOD(ANewEraReArmsTheReadersAndNothingActsSpuriously)
		{
			Rig r;
			const std::shared_ptr<Indicator> a = Rig::NewEra();
			r.Frames(a, 1, 4);
			a->Advance(VpIndicator::kFull, RCause::FileDiscontinuity, 5);
			r.Frames(a, 5, 5);
			Assert::AreEqual(1u, r.readers.HistoryActs());
			Assert::IsTrue(r.readers.TakeHistory().cause == MotionValidity::Cause::Seek);
			Assert::IsTrue(r.readers.TakeProducerNote().drop);

			const uint64_t epoch = r.state.Epoch();
			const std::shared_ptr<Indicator> b = Rig::NewEra();   // a new era: the provider registers again
			r.Frames(b, 1, 4);
			Assert::AreEqual(2u, r.readers.Rearms(), L"re-armed for the new Indicator");
			Assert::AreEqual(1u, r.readers.HistoryActs(), L"the new era's first observe is only a latch");
			Assert::AreEqual(1u, r.readers.ProducerActs());
			Assert::IsTrue(r.state.Epoch() == epoch, L"no epoch move, no release");
			const VpMotionResync::HistoryPending h = r.readers.TakeHistory();
			Assert::IsFalse(h.changed || h.release);
			Assert::IsFalse(r.readers.TakeProducerNote().drop);

			b->Advance(VpIndicator::kFull, RCause::LiveQueueReset, 5);
			r.Frames(b, 5, 5);
			Assert::AreEqual(2u, r.readers.HistoryActs(), L"acts once at the carrying frame");
			const VpMotionResync::HistoryPending h2 = r.readers.TakeHistory();
			Assert::IsTrue(h2.changed && h2.cause == MotionValidity::Cause::QueueReset, L"the cause is found in the NEW era");
			const VpMotionResync::ProducerNote n = r.readers.TakeProducerNote();
			Assert::IsTrue(n.drop && n.engageGeneration == VpResync::Drive::FullKey(*b, 5));
		}

		// Audit 2 MUST 1: the provider form binds the State to each new era itself (no manual Bind).
		// Before the fix PairValidAt / Invalidate(SettingsChange) still used the first (dead) Indicator.
		TEST_METHOD(TheProviderRebindsTheStateOnEveryEraWithoutAManualBind)
		{
			Rig r;
			const std::shared_ptr<Indicator> a = Rig::NewEra();
			r.Frames(a, 1, 4);
			Assert::IsTrue(r.state.Driven(), L"bound by the provider");
			const std::shared_ptr<Indicator> b = Rig::NewEra();
			r.Frames(b, 1, 4);
			// A FULL in the NEW era for frame 6: the pair (5, 6) spans it and must be refused.
			b->Advance(VpIndicator::kFull, RCause::LiveQueueReset, 6);
			Assert::IsFalse(r.state.PairValidAt(5, 6), L"reads the new era's stamps");
			Assert::IsTrue(r.state.PairValidAt(6, 7));
			Assert::IsTrue(r.state.StreamGenerationAt(6) == VpResync::Drive::FullKey(*b, 6), L"keyed on the new era");
			// A settings invalidation is written to the NEW Indicator (not lost in the old one).
			const uint32_t before = b->Total(VpIndicator::kSettingsMotion);
			const uint32_t beforeOld = a->Total(VpIndicator::kSettingsMotion);
			r.state.Invalidate(MotionValidity::Cause::SettingsChange);
			Assert::AreEqual(before + 1u, b->Total(VpIndicator::kSettingsMotion));
			Assert::AreEqual(beforeOld, a->Total(VpIndicator::kSettingsMotion));
			// ... and the reader moves the epoch at the seal of the carrying frame.
			const uint64_t epoch = r.state.Epoch();
			r.Frames(b, 5, 5);
			Assert::IsTrue(r.state.Epoch() > epoch, L"settings[motion] acted in the new era");
		}

		TEST_METHOD(AnUnknownCauseMapsConservatively)
		{
			Rig r;
			const std::shared_ptr<Indicator> a = Rig::NewEra();
			r.Frames(a, 1, 4);
			a->Advance(VpIndicator::kFull, RCause::FileDiscontinuity, 5);
			for (uint64_t i = 0; i < 80; ++i)
				a->Advance(VpIndicator::kContinuity, RCause::DriftDrop, 5, 1);   // folds the FULL out of the ring
			r.Frames(a, 5, 5);
			const VpMotionResync::HistoryPending h = r.readers.TakeHistory();
			Assert::IsTrue(h.changed, L"still acts");
			Assert::IsTrue(h.cause == MotionValidity::Cause::Resync && h.release, L"unknown FULL cause: resync, resources released");
		}

		TEST_METHOD(BoundStateStatusTextShowsDeferredRequests)
		{
			MotionValidity::State state;
			Assert::IsTrue(state.StatusText().empty(), L"unbound: no row");
			const std::shared_ptr<Indicator> a = Rig::NewEra();
			state.Bind(a);
			state.Invalidate(MotionValidity::Cause::Seek);
			const std::string text = state.StatusText();
			Assert::IsTrue(text.find("1 request(s) deferred to the seal") != std::string::npos, L"visible");
			state.Unbind();
			Assert::IsFalse(state.Driven());
		}
	};
}
