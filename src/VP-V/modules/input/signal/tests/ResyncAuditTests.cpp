// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Resync audit-1 fixes: Indicator::AnyCauseOtherThanIn and the pulldown's mixed-interval rule, the
// Shadow enqueue cursor reset on a new Indicator, the storm hold on SOURCE time (reproducible runs),
// and the motion readers re-arming on a new era (FrameSeal re-registration) with the allocation-free
// cause lookup (unknown/ancient = conservative).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FrameSeal.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/modules/input/queue_timing/PulldownResync.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>
#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

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
				out.push_back(s.StormHold(wallBase - 1000.0) ? 1 : 0);   // the argument is ignored in source-time mode
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
				fs.AddProvider("motion", [this](const std::shared_ptr<Indicator>& i) { VpMotionResync::RegisterOn(*i, state, readers); });
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
