// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// N#4 (Z1): Dejudder::AheadSchedule multi-slot ring arbiter + physical slot tracking tests.
// Verifies:
//   1. Multi-slot depth (SetCapacity(3)): TryAheadSlot fills physical slots 0, 1, 2; Consume pops front
//      returning physical slot 0 while remaining entries KEEP physical slots 1 and 2; next TryAheadSlot
//      reuses freed physical slot 0.
//   2. In-test negative controls: non-chained pair rejected (PushResult::NotChained, steps not run,
//      held slots untouched); full ring rejected (PushResult::Full, steps not run); epoch mismatch on
//      Consume returns false, slot=-1, increments EpochMisses() and Discarded(EpochMoved), and clears
//      the chain; Discard(Resync) / Discard(PictureChanged) clear held count and count discards.
//   3. Depth-1 backward compatibility: legacy 2-arg Consume without NoteEpoch + epoch-checked 3-arg Consume.
// Pure CPU; built without PCH.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/consumers/DejudderAhead.h>
#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>

#include <cstdint>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DejudderAheadRingTestsNs
{
	using Dejudder::AheadSchedule;
	using VpMotion::LaneAhead::DiscardCause;
	using VpMotion::LaneAhead::PushResult;

	struct StepRecorder
	{
		int snapshots = 0;
		int uploads = 0;
		std::vector<std::pair<uint64_t, uint64_t>> estimates;

		AheadSchedule::Steps Steps()
		{
			AheadSchedule::Steps s;
			s.snapshot = [this]() { ++snapshots; return true; };
			s.uploadAfter = [this]() { ++uploads; return true; };
			s.estimate = [this](uint64_t a, uint64_t b) {
				estimates.push_back(std::make_pair(a, b));
				return true;
			};
			return s;
		}
	};

	AheadSchedule::Conditions Cond(uint64_t source, uint64_t next, uint64_t after, uint64_t estimated)
	{
		AheadSchedule::Conditions c;
		c.enabled = true;
		c.renderedWarp = true;
		c.active = true;
		c.source = source;
		c.next = next;
		c.after = after;
		c.afterGap = false;
		c.estimated = estimated;
		c.layoutMatches = true;
		return c;
	}

	TEST_CLASS(DejudderAheadRingTests)
	{
	public:
		TEST_METHOD(MultiSlotRingPreservesPhysicalSlotsAcrossFrontConsumeAndReusesFreedSlot)
		{
			AheadSchedule sched;
			sched.SetCapacity(3);
			sched.NoteEpoch(10);
			Assert::AreEqual(3, sched.Capacity());
			Assert::AreEqual(0, sched.Held());
			Assert::AreEqual(0, sched.FreeSlot());

			StepRecorder rec;
			int slot0 = -1, slot1 = -1, slot2 = -1;

			// Fill physical slots 0, 1, 2 with chained pairs (11,12), (12,13), (13,14).
			Assert::IsTrue(sched.TryAheadSlot(Cond(10, 11, 12, 10), rec.Steps(), &slot0));
			Assert::AreEqual(0, slot0);
			Assert::AreEqual(1, sched.Held());
			Assert::AreEqual(0, sched.SlotAt(0));
			Assert::AreEqual(uint64_t(11), sched.EntryAt(0).from);
			Assert::AreEqual(uint64_t(12), sched.EntryAt(0).to);

			Assert::IsTrue(sched.TryAheadSlot(Cond(10, 12, 13, 11), rec.Steps(), &slot1));
			Assert::AreEqual(1, slot1);
			Assert::AreEqual(2, sched.Held());
			Assert::AreEqual(1, sched.SlotAt(1));

			Assert::IsTrue(sched.TryAheadSlot(Cond(10, 13, 14, 12), rec.Steps(), &slot2));
			Assert::AreEqual(2, slot2);
			Assert::AreEqual(3, sched.Held());
			Assert::AreEqual(2, sched.SlotAt(2));
			Assert::AreEqual(-1, sched.FreeSlot(), L"full ring has no free physical slot");

			// Negative control: pushing a 4th chained pair into a full ring is rejected without running steps.
			const int snapBeforeFull = rec.snapshots;
			const int uploadBeforeFull = rec.uploads;
			const size_t estBeforeFull = rec.estimates.size();
			int fullSlot = 99;
			Assert::IsFalse(sched.TryAheadSlot(Cond(10, 14, 15, 13), rec.Steps(), &fullSlot));
			Assert::AreEqual(-1, fullSlot);
			Assert::AreEqual(snapBeforeFull, rec.snapshots, L"full ring must not run snapshot step");
			Assert::AreEqual(uploadBeforeFull, rec.uploads, L"full ring must not run upload step");
			Assert::AreEqual(estBeforeFull, rec.estimates.size(), L"full ring must not run estimate step");
			Assert::AreEqual(1ull, sched.Ring().Pushed(PushResult::Full));
			Assert::AreEqual(3, sched.Held());

			// Consume front pair (11, 12): returns physical slot 0 while remaining entries KEEP physical slots 1 and 2!
			int consumedSlot = -1;
			Assert::IsTrue(sched.Consume(11, 12, 10, &consumedSlot));
			Assert::AreEqual(0, consumedSlot);
			Assert::AreEqual(uint64_t(10), sched.ConsumedEpoch());
			Assert::AreEqual(1ull, sched.Hits());
			Assert::AreEqual(2, sched.Held());

			// Physical slot stability invariant across front-consume.
			Assert::AreEqual(uint64_t(12), sched.EntryAt(0).from);
			Assert::AreEqual(uint64_t(13), sched.EntryAt(0).to);
			Assert::AreEqual(1, sched.SlotAt(0), L"remaining entry (12, 13) must keep physical slot 1");
			Assert::AreEqual(uint64_t(13), sched.EntryAt(1).from);
			Assert::AreEqual(uint64_t(14), sched.EntryAt(1).to);
			Assert::AreEqual(2, sched.SlotAt(1), L"remaining entry (13, 14) must keep physical slot 2");
			Assert::AreEqual(0, sched.FreeSlot(), L"freed physical slot 0 must be offered next");

			// Next TryAheadSlot pushes (14, 15) into reused physical slot 0!
			int reusedSlot = -1;
			Assert::IsTrue(sched.TryAheadSlot(Cond(11, 14, 15, 13), rec.Steps(), &reusedSlot));
			Assert::AreEqual(0, reusedSlot, L"freed physical slot 0 must be reused");
			Assert::AreEqual(3, sched.Held());
			Assert::AreEqual(1, sched.SlotAt(0));
			Assert::AreEqual(2, sched.SlotAt(1));
			Assert::AreEqual(0, sched.SlotAt(2));

			// Drain remaining entries in chain order and verify returned physical slots are 1, 2, 0.
			int sA = -1, sB = -1, sC = -1;
			Assert::IsTrue(sched.Consume(12, 13, 10, &sA));
			Assert::AreEqual(1, sA);
			Assert::IsTrue(sched.Consume(13, 14, 10, &sB));
			Assert::AreEqual(2, sB);
			Assert::IsTrue(sched.Consume(14, 15, 10, &sC));
			Assert::AreEqual(0, sC);
			Assert::AreEqual(0, sched.Held());
			Assert::AreEqual(4ull, sched.Hits());
		}

		TEST_METHOD(NegativeControlsNonChainedEpochMismatchAndDiscardCauses)
		{
			AheadSchedule sched;
			sched.SetCapacity(3);
			sched.NoteEpoch(7);
			StepRecorder rec;

			// Seed ring with (11, 12) in physical slot 0.
			int slot0 = -1;
			Assert::IsTrue(sched.TryAheadSlot(Cond(10, 11, 12, 10), rec.Steps(), &slot0));
			Assert::AreEqual(0, slot0);
			Assert::AreEqual(uint64_t(12), sched.Ring().BackTo());

			// Negative control 1: non-chained pair (from == 13 != BackTo() == 12) rejected with PushResult::NotChained,
			// steps NOT executed, and held entry (11, 12) untouched.
			const int snapsBefore = rec.snapshots;
			const int uploadsBefore = rec.uploads;
			const size_t estsBefore = rec.estimates.size();
			int badSlot = 77;
			Assert::IsFalse(sched.TryAheadSlot(Cond(10, 13, 14, 11), rec.Steps(), &badSlot));
			Assert::AreEqual(-1, badSlot);
			Assert::AreEqual(snapsBefore, rec.snapshots, L"non-chained pair must not run snapshot step");
			Assert::AreEqual(uploadsBefore, rec.uploads, L"non-chained pair must not run upload step");
			Assert::AreEqual(estsBefore, rec.estimates.size(), L"non-chained pair must not run estimate step");
			Assert::AreEqual(1ull, sched.Ring().Pushed(PushResult::NotChained));
			Assert::AreEqual(1, sched.Held(), L"held entry must remain intact after non-chained rejection");
			Assert::AreEqual(uint64_t(11), sched.EntryAt(0).from);
			Assert::AreEqual(0, sched.SlotAt(0));

			// Positive contrast: properly chained (12, 13) succeeds into physical slot 1.
			int slot1 = -1;
			Assert::IsTrue(sched.TryAheadSlot(Cond(10, 12, 13, 11), rec.Steps(), &slot1));
			Assert::AreEqual(1, slot1);
			Assert::AreEqual(2, sched.Held());

			// Negative control 2: epoch mismatch on Consume returns false, slot=-1, increments EpochMisses()
			// and Discarded(EpochMoved), and clears the entire chain.
			int mismatchSlot = 55;
			Assert::IsFalse(sched.Consume(11, 12, 8 /* wrong epoch */, &mismatchSlot));
			Assert::AreEqual(-1, mismatchSlot);
			Assert::AreEqual(1ull, sched.EpochMisses());
			Assert::AreEqual(2ull, sched.Discarded(DiscardCause::EpochMoved),
				L"all held entries in the chain must be discarded as EpochMoved");
			Assert::AreEqual(0, sched.Held(), L"epoch mismatch must clear the chain");
			Assert::AreEqual(uint64_t(0), sched.ConsumedEpoch());

			// Positive control: matching epoch succeeds on the exact same pair.
			sched.NoteEpoch(8);
			Assert::IsTrue(sched.TryAheadSlot(Cond(10, 11, 12, 10), rec.Steps(), &slot0));
			int matchSlot = -1;
			Assert::IsTrue(sched.Consume(11, 12, 8, &matchSlot));
			Assert::AreEqual(0, matchSlot);
			Assert::AreEqual(uint64_t(8), sched.ConsumedEpoch());

			// Negative control 3: Discard(Resync) and Discard(PictureChanged) clear held entries and increment exact counters.
			Assert::IsTrue(sched.TryAheadSlot(Cond(20, 21, 22, 20), rec.Steps(), &slot0));
			Assert::IsTrue(sched.TryAheadSlot(Cond(20, 22, 23, 21), rec.Steps(), &slot1));
			Assert::AreEqual(2, sched.Held());
			sched.Discard(DiscardCause::Resync);
			Assert::AreEqual(0, sched.Held());
			Assert::AreEqual(2ull, sched.Discarded(DiscardCause::Resync));

			Assert::IsTrue(sched.TryAheadSlot(Cond(30, 31, 32, 30), rec.Steps(), &slot0));
			Assert::AreEqual(1, sched.Held());
			sched.Discard(DiscardCause::PictureChanged);
			Assert::AreEqual(0, sched.Held());
			Assert::AreEqual(1ull, sched.Discarded(DiscardCause::PictureChanged));
		}

		TEST_METHOD(DepthOneBackwardCompatibilityTwoArgAndEpochCheckedThreeArgConsume)
		{
			// 1. Legacy single-pair usage without NoteEpoch (epochNow == 0): 2-arg Consume hits, 3-arg Consume rejects.
			{
				AheadSchedule sched;
				Assert::AreEqual(1, sched.Capacity(), L"default capacity preserves depth-1 single-pair mode");
				StepRecorder rec;

				Assert::IsTrue(sched.TryAhead(Cond(10, 11, 12, 10), rec.Steps()));
				Assert::IsTrue(sched.Pending());
				Assert::AreEqual(1, sched.Held());
				Assert::IsTrue(sched.Consume(11, 12), L"legacy 2-arg Consume hits without NoteEpoch");
				Assert::AreEqual(1ull, sched.Hits());
				Assert::IsFalse(sched.Pending());
				Assert::AreEqual(0, sched.Held());

				// Negative control: legacy un-epoched entry never hits epoch-checked 3-arg Consume.
				Assert::IsTrue(sched.TryAhead(Cond(11, 12, 13, 11), rec.Steps()));
				int slot = 9;
				Assert::IsFalse(sched.Consume(12, 13, 1, &slot),
					L"negative control: entry made without NoteEpoch must miss epoch-checked Consume");
				Assert::AreEqual(-1, slot);
				Assert::AreEqual(1ull, sched.EpochMisses());
				Assert::AreEqual(1ull, sched.Misses());
				Assert::IsFalse(sched.Pending());

				// Negative control: wrong next frame misses 2-arg Consume and clears pending entry.
				Assert::IsTrue(sched.TryAhead(Cond(12, 13, 14, 12), rec.Steps()));
				Assert::IsFalse(sched.Consume(13, 15));
				Assert::AreEqual(2ull, sched.Misses());
				Assert::IsFalse(sched.Pending());
			}

			// 2. Depth-1 with NoteEpoch: BOTH 3-arg epoch-checked Consume AND 2-arg Consume succeed.
			{
				AheadSchedule sched;
				sched.NoteEpoch(42);
				StepRecorder rec;

				Assert::IsTrue(sched.TryAhead(Cond(20, 21, 22, 20), rec.Steps()));
				Assert::AreEqual(1, sched.Held());
				int slot = -1;
				Assert::IsTrue(sched.Consume(21, 22, 42, &slot));
				Assert::AreEqual(0, slot);
				Assert::AreEqual(uint64_t(42), sched.ConsumedEpoch());
				Assert::AreEqual(1ull, sched.Hits());

				Assert::IsTrue(sched.TryAhead(Cond(21, 22, 23, 21), rec.Steps()));
				Assert::IsTrue(sched.Consume(22, 23), L"2-arg Consume also remains compatible when epoch was noted");
				Assert::AreEqual(2ull, sched.Hits());
				Assert::AreEqual(0, sched.Held());
			}
		}
	};
}
