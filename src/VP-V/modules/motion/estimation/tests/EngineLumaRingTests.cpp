// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Producer throughput plan (refs\105 s3.5 / s5, phase 1b): the engine-luma ring bookkeeping. CPU only (no D3D11): lookup by
// key (generation, sequence), capacity and oldest-first eviction, the pin API (no caller yet), generation clear, slot indices
// stay below TotalSlots (the textures the owner allocates and declares as VRAM).
#include "pch.h"
#include <VP-V/modules/motion/estimation/EngineLumaRing.h>

#include <set>
#include <string>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(EngineLumaRingTests)
	{
		using Ring = MotionAhead::EngineLumaRing;

	public:
		TEST_METHOD(AFrameIsFoundByItsKeyAndNotByAnotherGeneration)
		{
			Ring ring(3, 0);
			const int a = ring.Acquire(1, 10);
			Assert::AreEqual(a, ring.Find(1, 10));
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(2, 10), L"same sequence, other generation");
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(1, 11));
		}

		TEST_METHOD(TheOldestFrameIsEvictedAndSlotsStayInRange)
		{
			Ring ring(3, 0);
			std::set<int> slots;
			int evicted = Ring::kNoSlot;
			for (uint64_t s = 1; s <= 3; ++s) slots.insert(ring.Acquire(1, s, &evicted));
			Assert::AreEqual(size_t(3), slots.size(), L"three frames, three different textures");
			const int fourth = ring.Acquire(1, 4, &evicted);
			Assert::AreEqual(ring.Find(1, 4), fourth);
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(1, 1), L"frame 1 (oldest) was evicted");
			Assert::AreEqual(evicted, fourth, L"the new frame reuses the evicted frame's texture");
			Assert::IsTrue(ring.Find(1, 2) != Ring::kNoSlot && ring.Find(1, 3) != Ring::kNoSlot);
			for (uint64_t s = 5; s < 40; ++s)
			{
				const int slot = ring.Acquire(1, s);
				Assert::IsTrue(slot >= 0 && static_cast<size_t>(slot) < ring.TotalSlots());
			}
			Assert::AreEqual(size_t(3), ring.Live());
		}

		TEST_METHOD(AcquiringAHeldFrameKeepsItsSlotAndMakesItNewest)
		{
			Ring ring(3, 0);
			const int s1 = ring.Acquire(1, 1);
			ring.Acquire(1, 2);
			ring.Acquire(1, 3);
			Assert::AreEqual(s1, ring.Acquire(1, 1), L"the same slot");
			ring.Acquire(1, 4);   // evicts the oldest: now frame 2
			Assert::AreEqual(s1, ring.Find(1, 1));
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(1, 2));
			Assert::AreEqual(size_t(3), ring.Live());
		}

		TEST_METHOD(APinnedFrameSurvivesEvictionAndUsesAnExtraSlot)
		{
			Ring ring(3, 2);
			const int anchor = ring.Acquire(1, 100);
			Assert::IsTrue(ring.Pin(1, 100));
			for (uint64_t s = 1; s <= 10; ++s)
			{
				const int slot = ring.Acquire(1, s);
				Assert::IsTrue(slot != anchor, L"the pinned frame's texture is never reused");
				Assert::IsTrue(static_cast<size_t>(slot) < ring.TotalSlots());
			}
			Assert::AreEqual(anchor, ring.Find(1, 100));
			Assert::AreEqual(size_t(1), ring.Pinned());
			Assert::AreEqual(size_t(4), ring.Live(), L"3 recent + 1 pinned");
			Assert::IsTrue(ring.Unpin(1, 100));
			Assert::IsFalse(ring.Unpin(1, 100), L"already unpinned");
			ring.Acquire(1, 11);
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(1, 100), L"unpinned: evicted like any other");
		}

		TEST_METHOD(PinsAreLimitedAndOnlyForHeldFrames)
		{
			Ring ring(3, 2);
			ring.Acquire(1, 1); ring.Acquire(1, 2); ring.Acquire(1, 3);
			Assert::IsFalse(ring.Pin(1, 99), L"not held");
			Assert::IsTrue(ring.Pin(1, 1));
			Assert::IsTrue(ring.Pin(1, 2));
			Assert::IsFalse(ring.Pin(1, 3), L"only two pins");
			Ring none(3, 0);
			none.Acquire(1, 1);
			Assert::IsFalse(none.Pin(1, 1), L"the producer's ring has no pin slots yet");
		}

		TEST_METHOD(ANewGenerationClearsOtherGenerationsAndTheirPins)
		{
			Ring ring(3, 2);
			ring.Acquire(1, 1); ring.Acquire(1, 2);
			ring.Pin(1, 1);
			ring.Acquire(2, 7);
			const size_t removed = ring.ClearGenerationsOtherThan(2);
			Assert::AreEqual(size_t(2), removed);
			Assert::AreEqual(size_t(1), ring.Live());
			Assert::AreEqual(size_t(0), ring.Pinned(), L"a pin never outlives its generation");
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(1, 1));
			Assert::IsTrue(ring.Find(2, 7) != Ring::kNoSlot);
			ring.Clear();
			Assert::AreEqual(size_t(0), ring.Live());
		}

		TEST_METHOD(ThreeSlotsHoldThePipelinesPreviousCurrentAndNextFrame)
		{
			Ring ring(3, 0);
			for (uint64_t s = 1; s <= 50; ++s)
			{
				ring.Acquire(1, s);
				if (s >= 3)
				{
					Assert::IsTrue(ring.Find(1, s) != Ring::kNoSlot && ring.Find(1, s - 1) != Ring::kNoSlot && ring.Find(1, s - 2) != Ring::kNoSlot);
				}
			}
		}

		// S2PH1-S3B C4 (register 115 #13 / #35): the walk pair's first frame. Chained and held = its slot; chained but evicted =
		// Lost() (the producer counts and names it); not chained (no previous, a gap, another generation) = never a miss.
		TEST_METHOD(APairStartTheRingLostIsALostMissNotASilentSkip)
		{
			Ring ring(3, 0);
			const int held = ring.Acquire(1, 9);
			const MotionAhead::PairStart hit = MotionAhead::FindPairStart(ring, true, 1, 9, 1, 10);
			Assert::IsTrue(hit.chained && !hit.Lost());
			Assert::AreEqual(held, hit.slot, L"the slot of frame 9 by key");
			for (uint64_t s = 20; s <= 22; ++s) ring.Acquire(1, s);   // three newer frames evict frame 9
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), ring.Find(1, 9), L"setup: frame 9 was evicted");
			const MotionAhead::PairStart lost = MotionAhead::FindPairStart(ring, true, 1, 9, 1, 10);
			Assert::IsTrue(lost.chained, L"the chain still says (9, 10) is a pair");
			Assert::AreEqual(static_cast<int>(Ring::kNoSlot), lost.slot);
			Assert::IsTrue(lost.Lost(), L"a ring miss, for the producer's NoteRingMiss");
			Assert::AreEqual(std::string("the engine-luma ring lost the pair's first frame"), std::string(MotionAhead::PairStartLostText()));
		}

		TEST_METHOD(AnUnchainedFrameIsNeverARingMiss)
		{
			Ring ring(3, 0);   // empty: every Find misses
			const MotionAhead::PairStart noPrevious = MotionAhead::FindPairStart(ring, false, 1, 9, 1, 10);
			const MotionAhead::PairStart gap = MotionAhead::FindPairStart(ring, true, 1, 8, 1, 10);
			const MotionAhead::PairStart otherGeneration = MotionAhead::FindPairStart(ring, true, 1, 9, 2, 10);
			for (const MotionAhead::PairStart& p : { noPrevious, gap, otherGeneration })
			{
				Assert::IsFalse(p.chained);
				Assert::IsFalse(p.Lost(), L"no pair is due: not a miss");
				Assert::AreEqual(static_cast<int>(Ring::kNoSlot), p.slot);
			}
		}
	};
}
