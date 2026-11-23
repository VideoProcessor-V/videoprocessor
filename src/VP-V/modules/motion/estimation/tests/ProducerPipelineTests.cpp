// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 16 P1 (refs/105 s3.2 / s5): CPU seam tests for ProducerStages.h (StagingRing<3>, BackQueue,
// OrderedPublishGate): strict publish order, stop mid-pipeline, generation change and restart,
// breakBefore across stages, and on-demand requests answered in submit order.
#include "pch.h"
#include <VP-V/modules/motion/estimation/ProducerStages.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	using namespace MotionAhead::Pipeline;

	TEST_CLASS(ProducerPipelineTests)
	{
	public:
		TEST_METHOD(StagingRingTracksThreeSetsInFifoOrderAndDiscardsOldGenerations)
		{
			StagingRing<3> ring;
			ring.BindEngineThread();
			Assert::AreEqual<size_t>(3, ring.FreeCount());
			Assert::AreEqual<size_t>(0, ring.InFlightCount());
			Assert::AreEqual(StagingRing<3>::kNoSlot, ring.OldestSubmitted());

			const auto t0 = std::chrono::steady_clock::now();
			const int s0 = ring.AcquireFree();
			Assert::AreEqual(0, s0);
			Assert::IsTrue(ring.MarkSubmitted(s0, JobKey{ 1, 0, 1, false, false }, 1, t0));

			const int s1 = ring.AcquireFree();
			Assert::AreEqual(1, s1);
			Assert::IsTrue(ring.MarkSubmitted(s1, JobKey{ 1, 1, 2, false, false }, 1, t0));

			const int s2 = ring.AcquireFree();
			Assert::AreEqual(2, s2);
			Assert::IsTrue(ring.MarkSubmitted(s2, JobKey{ 1, 2, 3, false, false }, 1, t0));

			Assert::AreEqual(StagingRing<3>::kNoSlot, ring.AcquireFree(), L"all three staging sets are busy");
			Assert::AreEqual(s0, ring.OldestSubmitted(), L"FIFO oldest submitted is slot 0");

			// Complete slot 0 -> Ready -> Release -> re-acquire slot 0.
			Assert::IsTrue(ring.MarkReady(s0));
			Assert::AreEqual(s0, ring.OldestReady());
			Assert::AreEqual(s1, ring.OldestSubmitted());
			ring.Release(s0);
			Assert::AreEqual<size_t>(1, ring.FreeCount());

			// New generation 2 marks remaining gen-1 slots discarded without prematurely freeing GPU slots.
			Assert::AreEqual<size_t>(2, ring.MarkDiscardedOtherThan(2));
			Assert::IsTrue(ring.At(s1).discarded);
			Assert::IsTrue(ring.At(s2).discarded);

			const int s3 = ring.AcquireFree();
			Assert::AreEqual(0, s3);
			Assert::IsTrue(ring.MarkSubmitted(s3, JobKey{ 2, 10, 11, false, false }, 1, t0));
			Assert::IsFalse(ring.At(s3).discarded);

			ring.Clear();
			Assert::AreEqual<size_t>(3, ring.FreeCount());
		}

		TEST_METHOD(BackQueuePreservesStrictPublishOrderAcrossTwoThreads)
		{
			struct PairItem { uint64_t from = 0, to = 0; };
			BackQueue<PairItem> q(4);
			OrderedPublishGate gate;
			std::vector<uint64_t> published;
			const uint64_t epoch = q.Epoch();
			q.SetActiveGeneration(1);

			std::thread back([&] {
				BackQueue<PairItem>::Entry e;
				while (q.Pop(e))
				{
					q.MarkBusy(true);
					if (q.IsLive(e.generation, e.epoch) && gate.Accept(e.generation, e.item.from, e.item.to))
						published.push_back(e.item.to);
					q.MarkBusy(false);
				}
			});

			for (uint64_t s = 1; s <= 120; ++s)
				Assert::IsTrue(q.Push(PairItem{ s - 1, s }, 1, epoch));

			q.WaitIdle();
			q.Close();
			back.join();

			Assert::AreEqual<size_t>(120, published.size());
			for (size_t i = 0; i < published.size(); ++i)
				Assert::AreEqual<uint64_t>(static_cast<uint64_t>(i + 1), published[i], L"strictly increasing sequence order");

			// Gate refuses regressed sequence in same generation, accepts newer generation.
			Assert::IsFalse(gate.Accept(1, 118, 119));
			Assert::IsFalse(gate.Accept(1, 119, 120));
			Assert::IsTrue(gate.Accept(2, 0, 1));
			Assert::IsFalse(gate.Accept(1, 120, 121), L"older generation refused");
		}

		TEST_METHOD(StopMidPipelineDiscardsInFlightWithoutDeadlockOrStalePublish)
		{
			BackQueue<uint64_t> q(8);
			const uint64_t epoch = q.Epoch();
			std::mutex gateMutex;
			std::condition_variable gateCv;
			bool enteredFirst = false;
			bool releaseFirst = false;
			std::vector<uint64_t> published;

			std::thread back([&] {
				BackQueue<uint64_t>::Entry e;
				while (q.Pop(e))
				{
					q.MarkBusy(true);
					if (e.item == 1)
					{
						std::unique_lock<std::mutex> lock(gateMutex);
						enteredFirst = true;
						gateCv.notify_all();
						gateCv.wait_for(lock, std::chrono::seconds(5), [&] { return releaseFirst; });
					}
					if (q.IsLive(e.generation, e.epoch))
						published.push_back(e.item);
					q.MarkBusy(false);
				}
			});

			for (uint64_t s = 1; s <= 5; ++s)
				Assert::IsTrue(q.Push(s, 1, epoch));
			{
				std::unique_lock<std::mutex> lock(gateMutex);
				Assert::IsTrue(gateCv.wait_for(lock, std::chrono::seconds(5), [&] { return enteredFirst; }));
			}

			// Stop while item 1 is mid-flight and items 2..5 are queued.
			q.Close();
			{
				std::lock_guard<std::mutex> lock(gateMutex);
				releaseFirst = true;
			}
			gateCv.notify_all();
			back.join();

			Assert::AreEqual<size_t>(0, published.size(), L"in-flight and queued items after Close never publish");
			Assert::IsTrue(q.DiscardedCount() >= 4, L"queued items were discarded on Close");
		}

		TEST_METHOD(GenerationChangeAndRestartDropStaleItemsAndResumeCleanly)
		{
			BackQueue<uint64_t> q(8);
			q.SetActiveGeneration(1);
			const uint64_t epoch1 = q.Epoch();

			Assert::IsTrue(q.Push(10, 1, epoch1));
			Assert::IsTrue(q.Push(11, 1, epoch1));
			// Generation moves to 2: gen 1 items are discarded immediately.
			Assert::AreEqual<size_t>(2, q.SetActiveGeneration(2));
			Assert::IsFalse(q.IsLive(1, epoch1));
			Assert::IsFalse(q.Push(12, 1, epoch1), L"stale generation push refused");
			Assert::IsTrue(q.Push(1, 2, epoch1));

			BackQueue<uint64_t>::Entry e;
			Assert::IsTrue(q.Pop(e));
			Assert::AreEqual<uint64_t>(2, e.generation);
			Assert::AreEqual<uint64_t>(1, e.item);

			// Restart: Close -> Reopen -> new epoch works cleanly.
			q.Close();
			q.Reopen();
			const uint64_t epoch2 = q.Epoch();
			Assert::IsTrue(epoch2 > epoch1);
			Assert::IsTrue(q.Push(50, 3, epoch2));
			Assert::IsTrue(q.Pop(e));
			Assert::AreEqual<uint64_t>(50, e.item);
		}

		TEST_METHOD(BreakBeforeAcrossStagesSkipsDiscontinuityWithoutOutOfOrderPublish)
		{
			struct Work { uint64_t from = 0, to = 0; bool breakBefore = false; };
			BackQueue<Work> q(8);
			OrderedPublishGate gate;
			const uint64_t epoch = q.Epoch();
			q.SetActiveGeneration(1);

			// Simulate ENGINE: frames 1, 2, 3, (breakBefore on 4), 5, 6.
			// Pairs produced: (1,2), (2,3), [no (3,4) due to breakBefore], (4,5), (5,6).
			const std::vector<std::pair<uint64_t, bool>> frames = {
				{ 1, false }, { 2, false }, { 3, false }, { 4, true }, { 5, false }, { 6, false }
			};
			bool havePrev = false;
			uint64_t prev = 0;
			for (const auto& f : frames)
			{
				if (f.second)
					havePrev = false;
				if (havePrev && prev + 1 == f.first)
					Assert::IsTrue(q.Push(Work{ prev, f.first, f.second }, 1, epoch));
				havePrev = true;
				prev = f.first;
			}
			q.Finish();

			std::vector<std::pair<uint64_t, uint64_t>> pairs;
			BackQueue<Work>::Entry e;
			while (q.Pop(e))
			{
				if (gate.Accept(e.generation, e.item.from, e.item.to))
					pairs.push_back(std::make_pair(e.item.from, e.item.to));
			}

			Assert::AreEqual<size_t>(4, pairs.size());
			Assert::AreEqual<uint64_t>(1, pairs[0].first); Assert::AreEqual<uint64_t>(2, pairs[0].second);
			Assert::AreEqual<uint64_t>(2, pairs[1].first); Assert::AreEqual<uint64_t>(3, pairs[1].second);
			Assert::AreEqual<uint64_t>(4, pairs[2].first); Assert::AreEqual<uint64_t>(5, pairs[2].second);
			Assert::AreEqual<uint64_t>(5, pairs[3].first); Assert::AreEqual<uint64_t>(6, pairs[3].second);
		}

		TEST_METHOD(RequestsAndCorePairsRideSameBackQueueInSubmitOrder)
		{
			struct Event { bool isRequest = false; uint64_t from = 0, to = 0; };
			BackQueue<Event> q(8);
			const uint64_t epoch = q.Epoch();
			q.SetActiveGeneration(1);

			Assert::IsTrue(q.Push(Event{ false, 1, 2 }, 1, epoch));
			Assert::IsTrue(q.Push(Event{ true, 2, 3 }, 1, epoch));
			Assert::IsTrue(q.Push(Event{ false, 3, 4 }, 1, epoch));
			// Forced off-chain request of a newer generation rides the same queue without being dropped by gen 1 pin.
			Assert::IsTrue(q.Push(Event{ true, 10, 11 }, 2, epoch, true));
			q.Finish();

			std::vector<Event> out;
			BackQueue<Event>::Entry e;
			while (q.Pop(e))
				out.push_back(e.item);

			Assert::AreEqual<size_t>(4, out.size());
			Assert::IsFalse(out[0].isRequest); Assert::AreEqual<uint64_t>(2, out[0].to);
			Assert::IsTrue(out[1].isRequest);  Assert::AreEqual<uint64_t>(3, out[1].to);
			Assert::IsFalse(out[2].isRequest); Assert::AreEqual<uint64_t>(4, out[2].to);
			Assert::IsTrue(out[3].isRequest);  Assert::AreEqual<uint64_t>(11, out[3].to);
		}
	};
}
