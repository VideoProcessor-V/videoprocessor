// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// QKP slice E (design-queue-keyed-products s9.3): the FlowOf lease MAP keyed (from, to, scale), bounded by the same derived queue window as
// the frame memo (core/QueueWindow.h QueueWindowBoard), by the P2 slot, and by the epochs. VE-T1..T6. CODE-ONLY: written unbuilt.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/QueueWindow.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <memory>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// A pair with backward vectors, costs and the producer's extras, so the cell trust can be formed.
		MotionAhead::FlowPtr FullFlow()
		{
			auto flow = std::make_shared<MotionAhead::Flow>();
			flow->halfW = 16;
			flow->halfH = 8;
			flow->gridW = 4;
			flow->gridH = 2;
			for (int i = 0; i < 8; ++i)
			{
				flow->forward.push_back(static_cast<int16_t>(64 + 32 * i));
				flow->forward.push_back(static_cast<int16_t>(-32));
			}
			flow->backward = flow->forward;
			for (size_t i = 0; i < flow->backward.size(); ++i)
				flow->backward[i] = static_cast<int16_t>(-flow->backward[i] + (i % 3 == 0 ? 40 : 0));
			for (int i = 0; i < 8; ++i) flow->forwardCost.push_back(static_cast<uint8_t>(5 + 9 * i));
			flow->backwardCost.assign(8, 9);
			MotionAhead::FlowGlobals(*flow, true);
			MotionAhead::FlowExtras(*flow);
			return flow;
		}

		// An engine whose slot holds the pairs (k - 1, k) for k = first..last, all kept in the current vectors-valid epoch.
		struct Rig
		{
			VpMotion::VectorEngine engine;
			VpSkeleton::QueueWindowBoard board;
			explicit Rig(bool withBoard = true, uint64_t first = 11, uint64_t last = 15)
			{
				engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				engine.NoteFlowSourceWidth(64);
				for (uint64_t k = first - 1; k <= last; ++k)
					engine.motionValidity.NoteKept(k);
				for (uint64_t k = first; k <= last; ++k)
					engine.p2Slot->Publish(k - 1, k, engine.p2Slot->Generation(), FullFlow());
				if (withBoard)
					engine.SetQueueWindowBoard(&board);
			}
			void Window(uint64_t current, uint16_t back, uint64_t epoch = 1)
			{
				VpSkeleton::QueueWindow w;
				w.current = current;
				w.back = back;
				w.epoch = epoch;
				board.Publish(w);
			}
		};
	}

	TEST_CLASS(VectorLeaseTests)
	{
	public:
		// VE-T1: the two consumer shapes (AheadFlow's camera copy, SharedMotionSource's reader) ask the SAME engine for
		// FlowView::CellTrust, interleaved over two pairs: the single lease recomputed on every alternation; the map computes once per pair.
		TEST_METHOD(InterleavedConsumersComputeOncePerPair)
		{
			Rig rig;
			rig.Window(15, 4);
			const float* planeOf[2] = { nullptr, nullptr };
			for (int round = 0; round < 3; ++round)
				for (uint64_t seq = 12; seq <= 13; ++seq)
					for (int consumer = 0; consumer < 2; ++consumer)   // 0 = AheadFlow-like, 1 = SharedMotionSource-like
					{
						VpMotion::FlowView v;
						Assert::IsTrue(rig.engine.FlowOf(seq, v));
						const float* t = v.CellTrust(1.5f);
						Assert::IsNotNull(t);
						if (consumer == 0) planeOf[seq - 12] = t;
						else Assert::IsTrue(planeOf[seq - 12] == t, L"both consumers read one plane per pair");
					}
			const auto reads = rig.engine.FlowReads();
			Assert::AreEqual(2ull, reads.conversions, L"one conversion per pair");
			Assert::AreEqual(2ull, reads.trustComputes, L"computes == 1 per pair and tolerance");
		}

		// VE-T2: a lease leaves because its frame left the window (frames older than current - back), never because a newer pair was asked.
		TEST_METHOD(LeaseEvictedWhenPairLeavesWindow)
		{
			Rig rig;
			rig.Window(14, 5);
			VpMotion::FlowView v;
			for (uint64_t seq = 12; seq <= 14; ++seq)
				Assert::IsTrue(rig.engine.FlowOf(seq, v));
			Assert::AreEqual(3ull, rig.engine.Leases().live, L"newer pairs do not evict older ones inside the window");
			Assert::AreEqual(0ull, rig.engine.Leases().evictedWindow);
			rig.Window(15, 2);   // floor 13: the pair to 12 leaves
			Assert::IsTrue(rig.engine.FlowOf(15, v));
			Assert::AreEqual(1ull, rig.engine.Leases().evictedWindow);
			Assert::AreEqual(3ull, rig.engine.Leases().live);   // 13, 14, 15
		}

		// VE-T3: an epoch move (the board's invalidation epoch) drops every lease.
		TEST_METHOD(EpochInvalidatesAllLeases)
		{
			Rig rig;
			rig.Window(14, 5, 1);
			VpMotion::FlowView v;
			for (uint64_t seq = 12; seq <= 14; ++seq)
				Assert::IsTrue(rig.engine.FlowOf(seq, v));
			rig.Window(14, 5, 2);
			Assert::IsTrue(rig.engine.FlowOf(13, v));
			Assert::AreEqual(3ull, rig.engine.Leases().droppedEpoch);
			Assert::AreEqual(1ull, rig.engine.Leases().live, L"only the pair just served is leased again");
			Assert::AreEqual(4ull, rig.engine.FlowReads().conversions, L"rebuilt after the invalidation");
		}

		// VE-T4: the bound follows the board's derived back term (no literal): live leases = back + 1 of the five pairs.
		TEST_METHOD(BoundFollowsBoard)
		{
			for (uint16_t back = 1; back <= 3; ++back)
			{
				Rig rig;
				rig.Window(15, back);
				VpMotion::FlowView v;
				for (uint64_t seq = 11; seq <= 15; ++seq)
					Assert::IsTrue(rig.engine.FlowOf(seq, v));
				Assert::AreEqual(static_cast<unsigned long long>(back) + 1ull, rig.engine.Leases().live);
				Assert::AreEqual(static_cast<unsigned long long>(back), rig.engine.Leases().window);
			}
			// The P2 ring binds below a wider board window: the cap is SHOWN with the ring's own numbers.
			Rig wide;
			wide.Window(15, 5);
			Assert::AreEqual(std::string("vector lease window capped by the P2 ring: behind 3 / ahead 13"), wide.engine.LeaseWindowLine());
			Rig narrow;
			narrow.Window(15, 2);
			Assert::AreEqual(std::string(), narrow.engine.LeaseWindowLine());
		}

		// VE-T5: no board: today's single lease, engaged and SHOWN.
		TEST_METHOD(NoBoardKeepsTheSingleLeaseAndShowsIt)
		{
			Rig rig(false);
			VpMotion::FlowView v;
			Assert::IsTrue(rig.engine.FlowOf(12, v));
			Assert::IsTrue(rig.engine.FlowOf(13, v));
			Assert::AreEqual(1ull, rig.engine.Leases().live);
			Assert::AreEqual(1ull, rig.engine.Leases().droppedSingle);
			Assert::AreEqual(std::string("vector lease window not fed"), rig.engine.LeaseWindowLine());
		}

		// VE-T6: a pair pruned from the P2 slot drops its lease; a lease never extends P2 retention.
		TEST_METHOD(LeaseDroppedWhenP2Pruned)
		{
			Rig rig;
			rig.Window(14, 5);
			VpMotion::FlowView v;
			Assert::IsTrue(rig.engine.FlowOf(12, v));
			Assert::IsTrue(rig.engine.FlowOf(13, v));
			rig.engine.P2().Prune(14);   // the pairs ending before 14 leave the slot
			Assert::IsTrue(rig.engine.FlowOf(14, v));
			Assert::AreEqual(2ull, rig.engine.Leases().droppedPruned);
			Assert::AreEqual(1ull, rig.engine.Leases().live);
			VpMotion::FlowView gone;
			Assert::IsFalse(rig.engine.FlowOf(12, gone), L"the lease did not keep the pruned pair servable");
		}
	};
}
