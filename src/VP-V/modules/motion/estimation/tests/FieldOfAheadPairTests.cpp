// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// N#3 (Y1b-3): VectorEngine 1 + D (kFieldSlots == 7) verified-field ring tests.
// Verifies:
//   1. FieldOf serves BOTH live slot 0 AND multiple snapshot/ahead slots (NoteSnapshotTaken slot 0 +
//      NoteAheadField slots 1..D) via IMatcherView::LiveField/LiveFieldFrom/SnapFieldAt/SnapFieldFromAt.
//   2. NoteEstimated evicts stale matcherSlot == 0 records without evicting live matcherSlot >= 1
//      records, and AheadFieldSeqs returns ONLY matcher-validated current-epoch sequences in ascending
//      unique order.
//   3. In-test negative controls: epoch invalidation withdraws all pairs from FieldOf and AheadFieldSeqs;
//      mismatched SnapFieldFromAt(slot) or null SnapFieldAt(slot) withdraws that pair from both;
//      older-epoch snapshots and stale-epoch NoteAheadField calls are rejected.
// Pure CPU; built without PCH.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/AheadCeiling.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <array>
#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion;

namespace FieldOfAheadPairTestsNs
{
	struct MultiSlotMatcherView : IMatcherView
	{
		static constexpr int kMaxSlots = 8;
		pl_tex live = nullptr;
		uint64_t liveFrom = 0;
		std::array<pl_tex, kMaxSlots> snapTex{};
		std::array<uint64_t, kMaxSlots> snapFrom{};

		pl_tex LiveField() const override { return live; }
		uint64_t LiveFieldFrom() const override { return liveFrom; }
		bool MatcherGlobalFromEngine() const override { return false; }
		pl_tex SnapField() const override { return snapTex[0]; }
		uint64_t SnapFieldFrom() const override { return snapFrom[0]; }
		pl_tex SnapFieldAt(int slot) const override
		{
			return (slot >= 0 && slot < kMaxSlots) ? snapTex[static_cast<size_t>(slot)] : nullptr;
		}
		uint64_t SnapFieldFromAt(int slot) const override
		{
			return (slot >= 0 && slot < kMaxSlots) ? snapFrom[static_cast<size_t>(slot)] : 0;
		}
	};

	bool RunEstimate(VectorEngine& e, uint64_t a, uint64_t b)
	{
		int dummyMatcher = 0;
		return e.Estimate(a, b, true, 24.0, e.motionValidity.Epoch(), false, dummyMatcher,
			[]() { return true; }, [](const FallbackCatalog::Decision&) {});
	}

	struct FieldFixture
	{
		VectorEngine engine;
		MultiSlotMatcherView view;
		pl_tex_t liveTex{};
		std::array<pl_tex_t, MultiSlotMatcherView::kMaxSlots> snapTexStorage{};

		FieldFixture()
		{
			liveTex.params.w = 120;
			liveTex.params.h = 68;
			for (int i = 0; i < MultiSlotMatcherView::kMaxSlots; ++i)
			{
				snapTexStorage[static_cast<size_t>(i)].params.w = 60 + i * 4;
				snapTexStorage[static_cast<size_t>(i)].params.h = 34 + i * 2;
			}
			engine.BindMatcher(&view);
		}
	};

	TEST_CLASS(FieldOfAheadPairTests)
	{
	public:
		TEST_METHOD(FieldOfAnswersLiveSlotZeroAndMultipleAheadSlotsInAscendingOrder)
		{
			Assert::AreEqual(size_t(1u + VpSkeleton::kAheadCeiling), VectorEngine::kFieldSlots,
				L"derived 1 + D field ring capacity must equal 1 + kAheadCeiling (7)");

			FieldFixture f;
			const uint64_t epoch = f.engine.motionValidity.Epoch();

			// Live pair (5, 6) snapshotted into matcher slot 0, then live advances to (6, 7).
			Assert::IsTrue(RunEstimate(f.engine, 5, 6));
			f.engine.NoteSnapshotTaken(5);
			Assert::IsTrue(RunEstimate(f.engine, 6, 7));
			f.view.live = &f.liveTex;
			f.view.liveFrom = 6;
			f.view.snapTex[0] = &f.snapTexStorage[0];
			f.view.snapFrom[0] = 5;

			// Populate ahead slots 1..5 in non-monotonic order to prove AheadFieldSeqs sorts ascending.
			const uint64_t aheadPairs[5][2] = { { 11, 12 }, { 7, 8 }, { 10, 11 }, { 8, 9 }, { 9, 10 } };
			for (int idx = 0; idx < 5; ++idx)
			{
				const int matcherSlot = idx + 1;
				f.engine.NoteAheadField(aheadPairs[idx][0], aheadPairs[idx][1], epoch, matcherSlot);
				f.view.snapTex[static_cast<size_t>(matcherSlot)] = &f.snapTexStorage[static_cast<size_t>(matcherSlot)];
				f.view.snapFrom[static_cast<size_t>(matcherSlot)] = aheadPairs[idx][0];
			}

			// Verify live slot 0.
			VerifiedField vf;
			Assert::IsTrue(f.engine.FieldOf(7, vf));
			Assert::AreEqual(uint64_t(6), vf.pairFrom);
			Assert::AreEqual(uint64_t(7), vf.pairTo);
			Assert::AreEqual(120, vf.gridW);
			Assert::AreEqual(uint64_t(7), f.engine.LatestVerifiedFieldSeq(),
				L"LatestVerifiedFieldSeq always reports the live pair");

			// Verify snapshot slot 0.
			Assert::IsTrue(f.engine.FieldOf(6, vf));
			Assert::AreEqual(uint64_t(5), vf.pairFrom);
			Assert::AreEqual(60, vf.gridW);

			// Verify all 5 ahead slots return their exact distinct textures.
			for (int idx = 0; idx < 5; ++idx)
			{
				const int matcherSlot = idx + 1;
				const uint64_t from = aheadPairs[idx][0];
				const uint64_t to = aheadPairs[idx][1];
				Assert::IsTrue(f.engine.FieldOf(to, vf), L"ahead slot must be served by FieldOf");
				Assert::AreEqual(from, vf.pairFrom);
				Assert::AreEqual(to, vf.pairTo);
				Assert::AreEqual(60 + matcherSlot * 4, vf.gridW);
			}

			// AheadFieldSeqs returns all 7 valid sequences in strictly ascending unique order.
			uint64_t seqs[16] = {};
			const size_t n = f.engine.AheadFieldSeqs(seqs, 16);
			Assert::AreEqual(size_t(7), n);
			const uint64_t expected[7] = { 6, 7, 8, 9, 10, 11, 12 };
			for (size_t i = 0; i < 7; ++i)
				Assert::AreEqual(expected[i], seqs[i]);

			// Bounded cap truncation check.
			uint64_t smallBuf[3] = {};
			Assert::AreEqual(size_t(3), f.engine.AheadFieldSeqs(smallBuf, 3));
			Assert::AreEqual(uint64_t(6), smallBuf[0]);
			Assert::AreEqual(uint64_t(7), smallBuf[1]);
			Assert::AreEqual(uint64_t(8), smallBuf[2]);

			// In-test negative control: without NoteSnapshotTaken, older live pair is NOT moved into snapshot ring.
			FieldFixture noSnap;
			Assert::IsTrue(RunEstimate(noSnap.engine, 5, 6));
			Assert::IsTrue(RunEstimate(noSnap.engine, 6, 7));
			noSnap.view.live = &noSnap.liveTex;
			noSnap.view.liveFrom = 6;
			noSnap.view.snapTex[0] = &noSnap.snapTexStorage[0];
			noSnap.view.snapFrom[0] = 5;
			Assert::IsTrue(noSnap.engine.FieldOf(7, vf));
			Assert::IsFalse(noSnap.engine.FieldOf(6, vf),
				L"negative control: without NoteSnapshotTaken, older pair is absent");
			uint64_t noSnapSeqs[8] = {};
			Assert::AreEqual(size_t(1), noSnap.engine.AheadFieldSeqs(noSnapSeqs, 8));
			Assert::AreEqual(uint64_t(7), noSnapSeqs[0]);
		}

		TEST_METHOD(NoteEstimatedEvictsStaleSlotZeroWithoutEvictingLiveAheadSlots)
		{
			FieldFixture f;
			const uint64_t epoch = f.engine.motionValidity.Epoch();

			// Fill all 5 ahead slots (matcherSlot 1..5) with live ahead pairs (21..25).
			for (int slot = 1; slot <= 5; ++slot)
			{
				const uint64_t from = 20u + static_cast<uint64_t>(slot - 1);
				const uint64_t to = from + 1u;
				f.engine.NoteAheadField(from, to, epoch, slot);
				f.view.snapTex[static_cast<size_t>(slot)] = &f.snapTexStorage[static_cast<size_t>(slot)];
				f.view.snapFrom[static_cast<size_t>(slot)] = from;
			}

			// Repeatedly overwrite matcherSlot 0 via NoteSnapshotTaken + Estimate (4 cycles).
			// If stale matcherSlot == 0 records were not evicted, these 4 snapshots would push out ahead slots!
			Assert::IsTrue(RunEstimate(f.engine, 100, 101));
			for (uint64_t step = 0; step < 4; ++step)
			{
				f.engine.NoteSnapshotTaken(100 + step);
				Assert::IsTrue(RunEstimate(f.engine, 101 + step, 102 + step));
			}

			// Current live is (104, 105); current matcherSlot 0 holds (103, 104).
			f.view.live = &f.liveTex;
			f.view.liveFrom = 104;
			f.view.snapTex[0] = &f.snapTexStorage[0];
			f.view.snapFrom[0] = 103;

			VerifiedField vf;
			Assert::IsTrue(f.engine.FieldOf(105, vf), L"live pair (104, 105) present");
			Assert::IsTrue(f.engine.FieldOf(104, vf), L"latest slot-0 snapshot (103, 104) present");

			// ALL 5 live matcherSlot >= 1 ahead pairs MUST remain intact!
			for (int slot = 1; slot <= 5; ++slot)
			{
				const uint64_t to = 20u + static_cast<uint64_t>(slot);
				Assert::IsTrue(f.engine.FieldOf(to, vf),
					L"repeated slot-0 snapshots must never evict live matcherSlot >= 1 records");
				Assert::AreEqual(to - 1u, vf.pairFrom);
			}

			// In-test negative control: overwritten stale slot-0 pairs (101, 102, 103) were evicted by NoteEstimated.
			for (uint64_t evictedTo = 101; evictedTo <= 103; ++evictedTo)
			{
				f.view.snapFrom[0] = evictedTo - 1u;
				Assert::IsFalse(f.engine.FieldOf(evictedTo, vf),
					L"negative control: overwritten slot-0 record must be evicted");
			}
			f.view.snapFrom[0] = 103;

			uint64_t seqs[16] = {};
			Assert::AreEqual(size_t(7), f.engine.AheadFieldSeqs(seqs, 16));
			const uint64_t expected[7] = { 21, 22, 23, 24, 25, 104, 105 };
			for (size_t i = 0; i < 7; ++i)
				Assert::AreEqual(expected[i], seqs[i]);
		}

		TEST_METHOD(NegativeControlsEpochInvalidationAndMatcherMismatchWithdrawFieldsAndAheadSeqs)
		{
			FieldFixture f;
			const uint64_t epoch0 = f.engine.motionValidity.Epoch();

			Assert::IsTrue(RunEstimate(f.engine, 5, 6));
			f.engine.NoteSnapshotTaken(5);
			Assert::IsTrue(RunEstimate(f.engine, 6, 7));
			f.engine.NoteAheadField(7, 8, epoch0, 1);
			f.engine.NoteAheadField(8, 9, epoch0, 2);

			f.view.live = &f.liveTex;
			f.view.liveFrom = 6;
			f.view.snapTex[0] = &f.snapTexStorage[0];
			f.view.snapFrom[0] = 5;
			f.view.snapTex[1] = &f.snapTexStorage[1];
			f.view.snapFrom[1] = 7;
			f.view.snapTex[2] = &f.snapTexStorage[2];
			f.view.snapFrom[2] = 8;

			// Positive baseline: all 4 pairs served.
			VerifiedField vf;
			uint64_t seqs[16] = {};
			Assert::AreEqual(size_t(4), f.engine.AheadFieldSeqs(seqs, 16));
			for (uint64_t s = 6; s <= 9; ++s)
				Assert::IsTrue(f.engine.FieldOf(s, vf));

			// Negative control 1: mismatched SnapFieldFromAt(slot) withdraws that pair from FieldOf AND AheadFieldSeqs.
			f.view.snapFrom[1] = 999;
			Assert::IsFalse(f.engine.FieldOf(8, vf),
				L"negative control: mismatched SnapFieldFromAt(1) must reject FieldOf(8)");
			Assert::AreEqual(size_t(3), f.engine.AheadFieldSeqs(seqs, 16));
			Assert::AreEqual(uint64_t(6), seqs[0]);
			Assert::AreEqual(uint64_t(7), seqs[1]);
			Assert::AreEqual(uint64_t(9), seqs[2]);
			f.view.snapFrom[1] = 7;
			Assert::IsTrue(f.engine.FieldOf(8, vf));

			// Negative control 2: null SnapFieldAt(slot) withdraws that pair from FieldOf AND AheadFieldSeqs.
			f.view.snapTex[2] = nullptr;
			Assert::IsFalse(f.engine.FieldOf(9, vf),
				L"negative control: null SnapFieldAt(2) must reject FieldOf(9)");
			Assert::AreEqual(size_t(3), f.engine.AheadFieldSeqs(seqs, 16));
			Assert::AreEqual(uint64_t(6), seqs[0]);
			Assert::AreEqual(uint64_t(7), seqs[1]);
			Assert::AreEqual(uint64_t(8), seqs[2]);
			f.view.snapTex[2] = &f.snapTexStorage[2];
			Assert::IsTrue(f.engine.FieldOf(9, vf));

			// Negative control 3: epoch invalidation withdraws ALL live + ahead pairs from FieldOf and AheadFieldSeqs.
			f.engine.motionValidity.Invalidate(MotionValidity::Cause::Gap);
			for (uint64_t s = 6; s <= 9; ++s)
				Assert::IsFalse(f.engine.FieldOf(s, vf),
					L"negative control: invalidated epoch must withdraw all live and ahead fields");
			Assert::AreEqual(size_t(0), f.engine.AheadFieldSeqs(seqs, 16),
				L"negative control: invalidated epoch must return 0 AheadFieldSeqs");
			Assert::AreEqual(uint64_t(0), f.engine.LatestVerifiedFieldSeq());

			// Negative control 4: snapshot taken before invalidation is NOT carried into the new epoch,
			// and NoteAheadField with stale epoch0 is ignored.
			const uint64_t epoch1 = f.engine.motionValidity.Epoch();
			Assert::IsTrue(epoch1 > epoch0);
			Assert::IsTrue(RunEstimate(f.engine, 20, 21));
			f.engine.NoteSnapshotTaken(20);
			f.engine.motionValidity.Invalidate(MotionValidity::Cause::Gap);   // moves to epoch2
			Assert::IsTrue(RunEstimate(f.engine, 21, 22));                     // live in epoch2
			f.engine.NoteAheadField(22, 23, epoch1, 1);                        // stale epoch1: must be rejected!

			f.view.live = &f.liveTex;
			f.view.liveFrom = 21;
			f.view.snapTex[0] = &f.snapTexStorage[0];
			f.view.snapFrom[0] = 20;
			f.view.snapTex[1] = &f.snapTexStorage[1];
			f.view.snapFrom[1] = 22;

			Assert::IsTrue(f.engine.FieldOf(22, vf), L"new-epoch live pair (21, 22) must be served");
			Assert::IsFalse(f.engine.FieldOf(21, vf),
				L"negative control: older-epoch snapshot must not be carried across invalidation");
			Assert::IsFalse(f.engine.FieldOf(23, vf),
				L"negative control: stale-epoch NoteAheadField must be ignored");
			Assert::AreEqual(size_t(1), f.engine.AheadFieldSeqs(seqs, 16));
			Assert::AreEqual(uint64_t(22), seqs[0]);
		}
	};
}
