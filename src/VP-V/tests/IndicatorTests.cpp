// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// I0 central Indicator (src/VP-V/core/Indicator.h): derived per-sequence stamps, late events that
// re-stamp queued frames and copies, the seal clamp, ancient folding, implication rules, eras,
// readers (Observe), capacity sizing, the seqlock under concurrent readers, and determinism.
// CPU only.

#include <VP-V/core/Indicator.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace IndicatorTestsNs
{
	using namespace VpIndicator;
	using VpResync::Cause;

	// Brute-force reference: every event ever written, counted per counter for a given seq.
	struct Reference {
		struct Ev { uint64_t from; Counter c; };
		std::vector<Ev> events;

		void Add(uint64_t from, Counter c) { events.push_back({ from, c }); }

		uint32_t Count(uint64_t seq, size_t counter) const
		{
			uint32_t n = 0;
			for (const Ev& e : events)
				if (e.from <= seq && (MovedBy(e.c) & (CounterMask(1u) << counter)) != 0)
					++n;
			return n;
		}
	};

	std::array<Policy, kCounterCount> NoPolicies()
	{
		std::array<Policy, kCounterCount> p;
		p.fill(Policy::Ignore);
		return p;
	}

	// A frame as the queue holds it: the sequence only, no stamp.
	struct FrameCopy {
		uint64_t seq;
	};

	TEST_CLASS(IndicatorTests)
	{
	public:
		TEST_METHOD(RestampAfterALookAheadCopy)
		{
			Indicator ind(NextEra(), 64);
			FrameCopy queued{ 20 };
			FrameCopy copy = queued;   // look-ahead copy taken before the slip
			const Stamp beforeSlip = ind.StampOf(copy.seq);
			const Stamp earlier = ind.StampOf(19);

			ind.Advance(kContinuity, Cause::MissedFrames, 20, 2);

			const Stamp after = ind.StampOf(copy.seq);
			Assert::AreEqual(beforeSlip.v[kContinuity] + 1u, after.v[kContinuity]);
			Assert::IsTrue(ind.StampOf(queued.seq).v[kContinuity] == after.v[kContinuity]);
			Assert::IsTrue(ind.StampOf(19).v[kContinuity] == earlier.v[kContinuity]);   // before the slip: unchanged
			// A pair spanning the slip is not continuous; a pair on one side is.
			Assert::IsFalse(SameFor(ind.StampOf(19), ind.StampOf(20), Bit(kContinuity)));
			Assert::IsTrue(SameFor(ind.StampOf(20), ind.StampOf(21), Bit(kContinuity)));
			Assert::IsTrue(SameFor(ind.StampOf(18), ind.StampOf(19), Bit(kContinuity)));
			Assert::IsFalse(TemporalValid(ind, 19, 21, Bit(kContinuity)));
			Assert::IsTrue(TemporalValid(ind, 19, 21, Bit(kTiming)));
		}

		TEST_METHOD(LateSlipWithQueuedFramesIsClampedAboveTheSeal)
		{
			Indicator ind(NextEra(), 64);
			const uint64_t s1 = 100;
			ind.Seal(s1);
			const Stamp sealed = ind.StampOf(s1);

			const uint64_t effective = ind.Advance(kContinuity, Cause::TimestampBreak, s1, 1);
			Assert::AreEqual(s1 + 1, effective);
			Assert::IsTrue(SameFor(sealed, ind.StampOf(s1), kAllCounters));
			Assert::AreEqual(sealed.v[kContinuity], ind.StampOf(s1).v[kContinuity]);
			Assert::AreEqual(sealed.v[kContinuity] + 1u, ind.StampOf(s1 + 1).v[kContinuity]);   // s2
			Assert::AreEqual(sealed.v[kContinuity] + 1u, ind.StampOf(s1 + 2).v[kContinuity]);   // s3
		}

		TEST_METHOD(ClampReturnsTheEffectiveSeqAndNeverTouchesSealedStamps)
		{
			Indicator ind(NextEra(), 64);
			Assert::IsFalse(ind.HasSealed());
			Assert::AreEqual(uint64_t(0), ind.Advance(kTiming, Cause::RateStep, 0));   // nothing sealed: no clamp
			ind.Seal(50);
			Assert::IsTrue(ind.HasSealed());
			Assert::AreEqual(uint64_t(50), ind.LastSealed());
			const Stamp s40 = ind.StampOf(40);
			const Stamp s50 = ind.StampOf(50);
			Assert::AreEqual(uint64_t(51), ind.Advance(kFull, Cause::LiveQueueReset, 10));
			Assert::AreEqual(uint64_t(60), ind.Advance(kFull, Cause::LiveQueueReset, 60));
			Assert::IsTrue(SameFor(s40, ind.StampOf(40), kAllCounters));
			Assert::IsTrue(SameFor(s50, ind.StampOf(50), kAllCounters));
			ind.Seal(30);   // an older seal never lowers the floor
			Assert::AreEqual(uint64_t(50), ind.LastSealed());
			Assert::AreEqual(uint64_t(51), ind.Advance(kGraph, Cause::GraphRebuild, 0));
		}

		TEST_METHOD(AncientSeqsAreFoldedAndRecentOnesStayExact)
		{
			Indicator ind(NextEra(), 64);
			Assert::AreEqual(size_t(64), ind.Capacity());
			Reference ref;
			const Counter pattern[] = { kContinuity, kTiming, kFull, kRebaseH, kSettingsOutput, kGraph };
			for (uint64_t i = 0; i < 300; ++i) {
				const Counter c = pattern[i % 6];
				ind.Advance(c, Cause::GapOther, 1000 + i);
				ref.Add(1000 + i, c);
			}
			Assert::IsTrue(ind.AncientBelow() > 0);

			const Stamp early = ind.StampOf(1005);
			Assert::IsTrue(early.ancient);
			Assert::IsFalse(SameFor(early, early, kAllCounters));
			Assert::IsFalse(TemporalValid(ind, 1005, 1006, Bit(kTiming)));
			Assert::IsFalse(TemporalValid(ind, 1005, 1400, Bit(kTiming)));

			// Everything at or above the ancient boundary is exact.
			for (uint64_t seq = ind.AncientBelow(); seq < 1000 + 310; seq += 7) {
				const Stamp s = ind.StampOf(seq);
				Assert::IsFalse(s.ancient);
				for (size_t c = 0; c < size_t(kCounterCount); ++c)
					Assert::AreEqual(ref.Count(seq, c), s.v[c]);
			}
			Assert::IsTrue(TemporalValid(ind, 1290, 1299, Bit(kTiming)) == (ref.Count(1290, kTiming) == ref.Count(1299, kTiming)));
		}

		TEST_METHOD(NonMonotonicArrivalsMatchTheReference)
		{
			Indicator ind(NextEra(), 64);
			Reference ref;
			const uint64_t order[] = { 105, 101, 103 };
			const Counter kinds[] = { kContinuity, kFull, kTiming };
			for (int i = 0; i < 3; ++i) {
				ind.Advance(kinds[i], Cause::GapOther, order[i]);
				ref.Add(order[i], kinds[i]);
			}
			for (uint64_t seq = 100; seq <= 106; ++seq) {
				const Stamp s = ind.StampOf(seq);
				for (size_t c = 0; c < size_t(kCounterCount); ++c)
					Assert::AreEqual(ref.Count(seq, c), s.v[c]);
			}
		}

		TEST_METHOD(FullImpliesContinuityAndRebaseButNothingElse)
		{
			Indicator ind(NextEra(), 64);
			const Stamp before = ind.StampOf(10);
			ind.Advance(kFull, Cause::RendererReset, 10);
			const Stamp after = ind.StampOf(10);
			Assert::AreEqual(before.v[kFull] + 1u, after.v[kFull]);
			Assert::AreEqual(before.v[kContinuity] + 1u, after.v[kContinuity]);
			Assert::AreEqual(before.v[kRebaseH] + 1u, after.v[kRebaseH]);
			Assert::AreEqual(before.v[kRebaseV] + 1u, after.v[kRebaseV]);
			Assert::AreEqual(before.v[kTiming], after.v[kTiming]);
			Assert::AreEqual(before.v[kGraph], after.v[kGraph]);
			Assert::AreEqual(before.v[kSettingsMotion], after.v[kSettingsMotion]);
			Assert::AreEqual(before.v[kSettingsRebase], after.v[kSettingsRebase]);
			Assert::AreEqual(before.v[kSettingsAnalysis], after.v[kSettingsAnalysis]);
			Assert::AreEqual(before.v[kSettingsOutput], after.v[kSettingsOutput]);

			const Stamp mid = ind.StampOf(10);
			ind.Advance(kTiming, Cause::RateStep, 10);
			const Stamp t = ind.StampOf(10);
			Assert::AreEqual(mid.v[kTiming] + 1u, t.v[kTiming]);
			Assert::AreEqual(mid.v[kContinuity], t.v[kContinuity]);   // timing NEVER moves continuity
			Assert::AreEqual(mid.v[kFull], t.v[kFull]);

			// Continuity alone does not move FULL.
			ind.Advance(kContinuity, Cause::MissedFrames, 10);
			Assert::AreEqual(t.v[kFull], ind.StampOf(10).v[kFull]);
			Assert::AreEqual(uint32_t(1), ind.Total(kFull));
			Assert::AreEqual(uint32_t(1), ind.Total(kTiming));
			Assert::AreEqual(uint32_t(1), ind.Total(kContinuity));
		}

		TEST_METHOD(ErasMakeStampsIncomparable)
		{
			Indicator a(NextEra(), 64);
			Indicator b(NextEra(), 64);
			Assert::AreNotEqual(a.Era(), b.Era());
			Assert::AreNotEqual(uint32_t(0), a.Era());
			Assert::IsFalse(SameFor(a.StampOf(5), b.StampOf(5), Bit(kTiming)));
			Assert::IsTrue(SameFor(a.StampOf(5), a.StampOf(5), kAllCounters));
			Assert::AreEqual(kAllCounters, Changed(a.StampOf(5), b.StampOf(5)));
		}

		TEST_METHOD(ObserveCallsOnlyRelevantReadersInRegistrationOrder)
		{
			Indicator ind(NextEra(), 64);
			std::vector<int> calls;
			std::vector<CounterMask> relevants;
			auto record = [&](int id) {
				return [&calls, &relevants, id](const Indicator::Change& c) {
					calls.push_back(id);
					relevants.push_back(c.relevant);
				};
			};
			const int a = ind.Register("timing", Bit(kTiming), NoPolicies(), record(0));
			const int b = ind.Register("continuity", Bit(kContinuity), NoPolicies(), record(1));
			const int c = ind.Register("both", Bit(kContinuity) | Bit(kTiming), NoPolicies(), record(2));
			Assert::AreEqual(0, a);
			Assert::AreEqual(1, b);
			Assert::AreEqual(2, c);
			Assert::AreEqual(std::string("continuity"), std::string(ind.ReaderName(1)));

			// First Observe reports everything: all readers are called.
			Assert::AreEqual(kAllCounters, ind.SealAndObserve(10));
			Assert::AreEqual(size_t(3), calls.size());
			Assert::IsTrue(calls[0] == 0 && calls[1] == 1 && calls[2] == 2);
			calls.clear();
			relevants.clear();

			// Nothing changed.
			Assert::AreEqual(CounterMask(0), ind.SealAndObserve(11));
			Assert::AreEqual(size_t(0), calls.size());

			// A continuity event at 12: readers b and c, in order.
			ind.Advance(kContinuity, Cause::MissedFrames, 12);
			Assert::AreEqual(Bit(kContinuity), ind.SealAndObserve(12));
			Assert::AreEqual(size_t(2), calls.size());
			Assert::IsTrue(calls[0] == 1 && calls[1] == 2);
			Assert::AreEqual(Bit(kContinuity), relevants[0]);
			calls.clear();

			// A FULL event moves continuity too (implication) but not timing.
			ind.Advance(kFull, Cause::RendererReset, 13);
			ind.Advance(kTiming, Cause::RateStep, 14);
			Assert::IsTrue(ind.SealAndObserve(14) != 0);
			Assert::AreEqual(size_t(3), calls.size());

			Event ev[8];
			const size_t n = ind.Snapshot(ev, 8);
			Assert::AreEqual(size_t(3), n);
			Assert::AreEqual(uint64_t(1) << 1 | uint64_t(1) << 2, ev[0].actedReaders);                 // continuity @12: b, c
			Assert::AreEqual(uint64_t(1) << 1 | uint64_t(1) << 2, ev[1].actedReaders);                 // FULL @13 implies continuity: b, c
			Assert::AreEqual(uint64_t(1) << 0 | uint64_t(1) << 2, ev[2].actedReaders);                 // timing @14: a, c
			Assert::IsTrue(ev[0].cause == Cause::MissedFrames);
			Assert::IsTrue(ev[1].counter == kFull);
		}

		TEST_METHOD(OutOfOrderObserveIsCountedAndSilent)
		{
			Indicator ind(NextEra(), 64);
			int calls = 0;
			ind.Register("any", kAllCounters, NoPolicies(), [&calls](const Indicator::Change&) { ++calls; });
			ind.SealAndObserve(20);
			Assert::AreEqual(1, calls);
			Assert::AreEqual(CounterMask(0), ind.Observe(19));
			Assert::AreEqual(1, calls);
			Assert::AreEqual(uint32_t(1), ind.OutOfOrderObserves());
			Assert::AreEqual(CounterMask(0), ind.Observe(20));   // equal is in order, nothing changed
			Assert::AreEqual(uint32_t(1), ind.OutOfOrderObserves());
		}

		TEST_METHOD(RegistrationIsBoundedAndSetupOnly)
		{
			Indicator ind(NextEra(), 64);
			for (size_t i = 0; i < Indicator::kMaxReaders; ++i)
				Assert::IsTrue(ind.Register("r", Bit(kFull), NoPolicies(), nullptr) >= 0);
			Assert::AreEqual(-1, ind.Register("extra", Bit(kFull), NoPolicies(), nullptr));
			Assert::AreEqual(uint32_t(1), ind.RejectedReaders());
			Indicator late(NextEra(), 64);
			late.SealAndObserve(1);
			Assert::AreEqual(-1, late.Register("late", Bit(kFull), NoPolicies(), nullptr));
		}

		TEST_METHOD(RequiredCapacityHasAFloorIsPowerOfTwoAndGrows)
		{
			const size_t minCap = Indicator::kMinCapacity;
			Assert::AreEqual(size_t(64), minCap);
			Assert::AreEqual(size_t(64), Indicator::RequiredCapacity(0));
			Assert::AreEqual(size_t(64), Indicator::RequiredCapacity(3));
			Assert::AreEqual(size_t(64), Indicator::RequiredCapacity(16));
			Assert::AreEqual(size_t(128), Indicator::RequiredCapacity(17));
			size_t previous = 0;
			for (size_t frames = 1; frames < 2000; frames += 13) {
				const size_t cap = Indicator::RequiredCapacity(frames);
				Assert::IsTrue(cap >= minCap);
				Assert::IsTrue((cap & (cap - 1)) == 0);
				Assert::IsTrue(cap >= frames * Indicator::kMaxEventsPerFrame);
				Assert::IsTrue(cap >= previous);
				previous = cap;
			}
			// An 8K / 120 Hz-sized queue (hundreds of frames in flight) still sizes sanely.
			const size_t big = Indicator::RequiredCapacity(600);
			Assert::AreEqual(size_t(4096), big);
			Indicator ind(NextEra(), big);
			Assert::AreEqual(size_t(4096), ind.Capacity());
			Indicator rounded(NextEra(), 100);
			Assert::AreEqual(size_t(128), rounded.Capacity());
			Indicator tiny(NextEra(), 1);
			Assert::AreEqual(size_t(64), tiny.Capacity());
		}

		TEST_METHOD(SeqlockStressKeepsInvariantsUnderConcurrentReaders)
		{
			Indicator ind(NextEra(), 64);
			std::atomic<bool> stop(false);
			std::atomic<int> violations(0);

			auto reader = [&]() {
				uint32_t lastContinuity = 0, lastTiming = 0;
				bool haveLast = false;
				Event events[64];
				while (!stop.load(std::memory_order_relaxed)) {
					const Stamp s = ind.StampOf(~uint64_t(0));   // above every event: counts all retained
					if (!s.ancient) {
						if (s.v[kContinuity] < s.v[kFull] || s.v[kRebaseH] < s.v[kFull] || s.v[kRebaseV] < s.v[kFull])
							violations.fetch_add(1);
					}
					// Totals only grow; the folded + retained sum for a fixed high seq never goes down.
					if (haveLast && (s.v[kContinuity] < lastContinuity || s.v[kTiming] < lastTiming))
						violations.fetch_add(1);
					lastContinuity = s.v[kContinuity];
					lastTiming = s.v[kTiming];
					haveLast = true;

					const size_t n = ind.Snapshot(events, 64);
					for (size_t i = 1; i < n; ++i)
						if (events[i].serial != events[i - 1].serial + 1)
							violations.fetch_add(1);
				}
			};

			std::thread r1(reader), r2(reader), r3(reader);
			const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
			uint64_t i = 0;
			while (std::chrono::steady_clock::now() < end) {
				for (int k = 0; k < 200; ++k, ++i) {
					const Counter c = (i % 3 == 0) ? kFull : (i % 3 == 1) ? kContinuity : kTiming;
					ind.Advance(c, Cause::GapOther, 10 + i);
				}
			}
			stop.store(true);
			r1.join();
			r2.join();
			r3.join();
			Assert::AreEqual(0, violations.load());
			Assert::IsTrue(i > 0);
		}

		TEST_METHOD(SameAdvanceSequenceIsDeterministic)
		{
			Indicator a(NextEra(), 64);
			Indicator b(NextEra(), 64);
			uint64_t seed = 12345;
			for (int i = 0; i < 200; ++i) {
				seed = seed * 6364136223846793005ull + 1442695040888963407ull;
				const Counter c = Counter((seed >> 33) % uint64_t(kCounterCount));
				const uint64_t from = 500 + ((seed >> 20) % 40);
				a.Advance(c, Cause::GapOther, from, uint32_t(i), uint8_t(i));
				b.Advance(c, Cause::GapOther, from, uint32_t(i), uint8_t(i));
				if (i % 50 == 0) {
					a.Seal(500 + uint64_t(i) / 4);
					b.Seal(500 + uint64_t(i) / 4);
				}
			}
			for (uint64_t seq = 490; seq < 560; ++seq) {
				const Stamp sa = a.StampOf(seq);
				const Stamp sb = b.StampOf(seq);
				Assert::IsTrue(sa.ancient == sb.ancient);
				Assert::IsTrue(sa.v == sb.v);
			}
			Event ea[64], eb[64];
			const size_t na = a.Snapshot(ea, 64);
			const size_t nb = b.Snapshot(eb, 64);
			Assert::AreEqual(na, nb);
			for (size_t i = 0; i < na; ++i) {
				Assert::AreEqual(ea[i].serial, eb[i].serial);
				Assert::AreEqual(ea[i].fromSeq, eb[i].fromSeq);
				Assert::IsTrue(ea[i].counter == eb[i].counter);
				Assert::AreEqual(ea[i].gapFrames, eb[i].gapFrames);
				Assert::AreEqual(ea[i].detail, eb[i].detail);
			}
			Assert::AreEqual(a.AncientBelow(), b.AncientBelow());
		}

		TEST_METHOD(SnapshotIsNewestLastAndBounded)
		{
			Indicator ind(NextEra(), 64);
			for (uint64_t i = 0; i < 100; ++i)
				ind.Advance(kTiming, Cause::RateStep, i);
			Event ev[10];
			const size_t n = ind.Snapshot(ev, 10);
			Assert::AreEqual(size_t(10), n);
			Assert::AreEqual(uint64_t(100), ev[9].serial);
			Assert::AreEqual(uint64_t(91), ev[0].serial);
			Event all[128];
			Assert::AreEqual(size_t(64), ind.Snapshot(all, 128));
			Assert::AreEqual(uint64_t(37), all[0].serial);
			Assert::AreEqual(uint32_t(100), ind.Total(kTiming));
		}

		TEST_METHOD(UndersizedRingCountsPrematureFolds)
		{
			// Nothing sealed: every folded event still applies to unsealed frames.
			Indicator ind(NextEra(), 64);
			for (uint64_t i = 0; i < 100; ++i)
				ind.Advance(kTiming, Cause::RateStep, 100);
			Assert::AreEqual(uint32_t(36), ind.PrematureFolds());

			// A ring that keeps up with the seal never folds an event that still applies.
			Indicator ok(NextEra(), 64);
			for (uint64_t i = 0; i < 200; ++i) {
				ok.Advance(kTiming, Cause::RateStep, i);
				ok.Seal(i);
			}
			Assert::AreEqual(uint32_t(0), ok.PrematureFolds());

			// Sealed far behind the writers: events at or above the next unsealed seq fold prematurely.
			Indicator behind(NextEra(), 64);
			behind.Seal(50);
			for (uint64_t i = 0; i < 100; ++i)
				behind.Advance(kTiming, Cause::RateStep, 51 + i);
			Assert::AreEqual(uint32_t(36), behind.PrematureFolds());
		}

		TEST_METHOD(ObserveOfAnUnsealedSeqIsSilentAndCounted)
		{
			Indicator ind(NextEra(), 64);
			int calls = 0;
			ind.Register("any", kAllCounters, NoPolicies(), [&calls](const Indicator::Change&) { ++calls; });
			Assert::AreEqual(CounterMask(0), ind.Observe(5));   // nothing sealed
			Assert::AreEqual(uint32_t(1), ind.UnsealedObserves());
			ind.Seal(5);
			Assert::AreEqual(CounterMask(0), ind.Observe(6));   // beyond the seal
			Assert::AreEqual(uint32_t(2), ind.UnsealedObserves());
			Assert::AreEqual(0, calls);
			Assert::AreEqual(kAllCounters, ind.Observe(5));     // sealed: first observe reports everything
			Assert::AreEqual(1, calls);
			Assert::AreEqual(uint32_t(2), ind.UnsealedObserves());
			Indicator j(NextEra(), 64);
			Assert::AreEqual(kAllCounters, j.SealAndObserve(9));
		}

		TEST_METHOD(ReaderCallbackAdvancingAtTheFirstUnsealedSeqLeavesTheSealedStampAlone)
		{
			Indicator ind(NextEra(), 64);
			Stamp insideBefore, insideAfter;
			uint64_t effective = 0;
			ind.Register("reentrant", kAllCounters, NoPolicies(), [&](const Indicator::Change& c) {
				insideBefore = ind.StampOf(c.seq);
				effective = ind.Advance(kContinuity, Cause::MissedFrames, ind.LastSealed() + 1);
				insideAfter = ind.StampOf(c.seq);
			});
			ind.SealAndObserve(10);
			Assert::AreEqual(uint64_t(11), effective);
			Assert::IsTrue(SameFor(insideBefore, insideAfter, kAllCounters));
			Assert::IsTrue(SameFor(insideBefore, ind.StampOf(10), kAllCounters));
			Assert::AreEqual(insideBefore.v[kContinuity] + 1u, ind.StampOf(11).v[kContinuity]);
		}

		TEST_METHOD(OutOfRangeCountersAreRejectedAndBitIsZero)
		{
			Indicator ind(NextEra(), 64);
			Assert::AreEqual(uint64_t(0), ind.Advance(static_cast<Counter>(kCounterCount), Cause::GapOther, 77));
			Assert::AreEqual(uint64_t(0), ind.Advance(static_cast<Counter>(200), Cause::GapOther, 77));
			Assert::AreEqual(uint32_t(2), ind.RejectedAdvances());
			Event ev[4];
			Assert::AreEqual(size_t(0), ind.Snapshot(ev, 4));
			Assert::IsTrue(SameFor(ind.StampOf(0), ind.StampOf(100), kAllCounters));
			Assert::AreEqual(CounterMask(0), Bit(static_cast<Counter>(kCounterCount)));
			Assert::AreEqual(CounterMask(0), Bit(static_cast<Counter>(31)));
			Assert::AreEqual(CounterMask(0), Bit(static_cast<Counter>(255)));
			Assert::AreEqual(CounterMask(1) << (kCounterCount - 1), Bit(kSettingsOutput));
		}

		TEST_METHOD(ClampedAdvancesAreCounted)
		{
			Indicator ind(NextEra(), 64);
			ind.Advance(kTiming, Cause::RateStep, 3);
			Assert::AreEqual(uint32_t(0), ind.ClampedAdvances());   // nothing sealed
			ind.Seal(50);
			ind.Advance(kTiming, Cause::RateStep, 10);               // clamped to 51
			ind.Advance(kTiming, Cause::RateStep, 50);               // clamped to 51
			ind.Advance(kTiming, Cause::RateStep, 51);               // exactly the floor: not clamped
			ind.Advance(kTiming, Cause::RateStep, 90);
			Assert::AreEqual(uint32_t(2), ind.ClampedAdvances());
			Assert::AreEqual(uint32_t(5), ind.Total(kTiming));
		}

		TEST_METHOD(FoldingWithOutOfOrderFromSeqsMatchesTheBruteForceModel)
		{
			Indicator ind(NextEra(), 64);
			Reference ref;
			const Counter pattern[] = { kContinuity, kTiming, kFull, kRebaseV, kSettingsMotion, kGraph, kSettingsOutput };
			for (uint64_t i = 0; i < 300; ++i) {
				const Counter c = pattern[i % 7];
				const uint64_t from = (i % 2 == 0) ? 1000 + i : 900 + i;
				ind.Advance(c, Cause::GapOther, from);
				ref.Add(from, c);
			}
			Assert::AreEqual(uint32_t(300 - 64), ind.PrematureFolds());
			const uint64_t below = ind.AncientBelow();
			Assert::IsTrue(below > 900);
			for (uint64_t seq = 800; seq < 1400; ++seq) {
				const Stamp s = ind.StampOf(seq);
				if (seq < below) {
					Assert::IsTrue(s.ancient);
					continue;
				}
				Assert::IsFalse(s.ancient);
				for (size_t c = 0; c < size_t(kCounterCount); ++c)
					Assert::AreEqual(ref.Count(seq, c), s.v[c]);
			}
		}

		TEST_METHOD(WritersSealerAndReadersKeepInvariantsAndFixedSeqCountsOnlyGrow)
		{
			// Event pattern: writer w, event k -> counter by (w + k) % 3, requested fromSeq 100 + k % 40
			// (the sealer stops at 130, so a clamp never lifts an event above kHigh/kLow's range).
			struct Pat {
				static Counter CounterFor(unsigned w, uint32_t k)
				{
					const unsigned m = (w + k) % 3;
					return m == 0 ? kFull : (m == 1 ? kContinuity : kTiming);
				}
				static uint64_t FromFor(uint32_t k) { return 100 + k % 40; }
			};
			const uint64_t kHigh = 200;   // above every event: never ancient
			const uint64_t kLow = 120;    // inside the event range: may go ancient
			Indicator ind(NextEra(), 64);
			std::atomic<bool> stop(false);
			std::atomic<int> violations(0);

			auto implied = [&ind](Counter c) {   // events that move counter c, read after the stamp
				uint32_t t = ind.Total(c);
				if (c == kContinuity || c == kRebaseH || c == kRebaseV)
					t += ind.Total(kFull);
				return t;
			};

			auto writer = [&](unsigned w) {
				for (uint32_t k = 0; !stop.load(std::memory_order_relaxed); ++k)
					ind.Advance(Pat::CounterFor(w, k), Cause::GapOther, Pat::FromFor(k), k, uint8_t(w));
			};
			auto sealer = [&]() {
				for (uint64_t s = 0; s <= 130 && !stop.load(std::memory_order_relaxed); ++s) {
					ind.Seal(s);
					std::this_thread::sleep_for(std::chrono::milliseconds(8));
				}
			};
			auto reader = [&]() {
				std::array<uint32_t, kCounterCount> lastHigh{}, lastLow{};
				Event ev[64];
				while (!stop.load(std::memory_order_relaxed)) {
					const Stamp hi = ind.StampOf(kHigh);
					if (hi.ancient)
						violations.fetch_add(1);
					for (size_t c = 0; c < size_t(kCounterCount); ++c) {
						if (hi.v[c] < lastHigh[c])
							violations.fetch_add(1);
						lastHigh[c] = hi.v[c];
						if (hi.v[c] > implied(Counter(c)))
							violations.fetch_add(1);
					}
					const Stamp lo = ind.StampOf(kLow);
					if (!lo.ancient) {
						for (size_t c = 0; c < size_t(kCounterCount); ++c) {
							if (lo.v[c] < lastLow[c])
								violations.fetch_add(1);
							lastLow[c] = lo.v[c];
							if (lo.v[c] > implied(Counter(c)))
								violations.fetch_add(1);
						}
					}
					const size_t n = ind.Snapshot(ev, 64);
					for (size_t i = 0; i < n; ++i) {
						if (i > 0 && ev[i].serial != ev[i - 1].serial + 1)
							violations.fetch_add(1);
						if (ev[i].counter != Pat::CounterFor(ev[i].detail, ev[i].gapFrames))
							violations.fetch_add(1);
						if (ev[i].fromSeq < Pat::FromFor(ev[i].gapFrames) || ev[i].fromSeq > 131 + 40)
							violations.fetch_add(1);
					}
				}
			};

			std::thread w0(writer, 0u), w1(writer, 1u), s0(sealer), r0(reader), r1(reader), r2(reader);
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			stop.store(true);
			w0.join(); w1.join(); s0.join(); r0.join(); r1.join(); r2.join();
			Assert::AreEqual(0, violations.load());
			Assert::IsTrue(ind.Total(kFull) + ind.Total(kContinuity) + ind.Total(kTiming) > 0);
		}
	};
}
