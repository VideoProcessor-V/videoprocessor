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

		TEST_METHOD(WaitIdleBlocksImmediatelyAfterPopBeforeConsumerFinishes)
		{
			struct ThreadJoinGuard
			{
				BackQueue<int>& queue;
				std::thread& worker;
				ThreadJoinGuard(BackQueue<int>& qRef, std::thread& tRef) : queue(qRef), worker(tRef) {}
				ThreadJoinGuard(const ThreadJoinGuard&) = delete;
				ThreadJoinGuard& operator=(const ThreadJoinGuard&) = delete;
				~ThreadJoinGuard()
				{
					if (worker.joinable())
					{
						queue.Close();
						worker.join();
					}
				}
			};

			BackQueue<int> q(4);
			const uint64_t epoch = q.Epoch();
			constexpr uint64_t kGen = 1;
			constexpr int kPayload = 42;

			// (a) Direct deterministic proof (S3-H3): Pop() must atomically mark busy_ = true under mutex_
			// before unlocking, so WaitIdle() blocks even when the consumer has NOT called MarkBusy(true).
			Assert::IsTrue(q.Push(kPayload, kGen, epoch));
			BackQueue<int>::Entry e{};
			Assert::IsTrue(q.Pop(e));
			Assert::AreEqual(kPayload, e.item);

			{
				std::mutex cvMutex;
				std::condition_variable cv;
				bool waiterStarted = false;
				std::atomic<bool> waitIdleReturned{ false };

				std::thread waiter;
				ThreadJoinGuard guard(q, waiter);
				waiter = std::thread([&] {
					{
						std::lock_guard<std::mutex> lock(cvMutex);
						waiterStarted = true;
					}
					cv.notify_all();
					q.WaitIdle();
					waitIdleReturned.store(true, std::memory_order_release);
					{
						std::lock_guard<std::mutex> lock(cvMutex);
					}
					cv.notify_all();
				});

				bool startedOk = false;
				bool returnedEarlyWhileBusy = false;
				bool returnedAfterMarkIdle = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					startedOk = cv.wait_for(lock, std::chrono::seconds(5), [&] { return waiterStarted; });
					if (startedOk)
					{
						// On pre-fix code, WaitIdle() observes queue_.empty() && !busy_ and returns in microseconds.
						// On fixed code, WaitIdle() stays blocked for the full window until MarkBusy(false).
						returnedEarlyWhileBusy = cv.wait_for(
							lock,
							std::chrono::milliseconds(50),
							[&] { return waitIdleReturned.load(std::memory_order_acquire); });
					}
				}

				q.MarkBusy(false);
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					returnedAfterMarkIdle = cv.wait_for(
						lock,
						std::chrono::seconds(2),
						[&] { return waitIdleReturned.load(std::memory_order_acquire); });
				}

				if (!returnedAfterMarkIdle)
					q.Close();
				waiter.join();

				Assert::IsTrue(startedOk, L"WaitIdle worker thread must start");
				Assert::IsFalse(
					returnedEarlyWhileBusy,
					L"WaitIdle must not return early after Pop() before consumer completes with MarkBusy(false)");
				Assert::IsTrue(
					returnedAfterMarkIdle,
					L"WaitIdle must wake promptly once consumer calls MarkBusy(false)");
			}

			// (b) Same-input negative control (Rule 3): using the exact same BackQueue<int> and (kPayload, kGen, epoch),
			// once busy_ is cleared immediately after Pop() (matching the unprotected pre-fix busy_ == false state),
			// WaitIdle() returns immediately without blocking — proving the hazard is real and WaitIdle is not stuck.
			Assert::IsTrue(q.Push(kPayload, kGen, epoch));
			BackQueue<int>::Entry negEntry{};
			Assert::IsTrue(q.Pop(negEntry));
			Assert::AreEqual(kPayload, negEntry.item);
			q.MarkBusy(false);

			{
				std::mutex negMutex;
				std::condition_variable negCv;
				std::atomic<bool> unprotectedReturned{ false };

				std::thread negWaiter;
				ThreadJoinGuard negGuard(q, negWaiter);
				negWaiter = std::thread([&] {
					q.WaitIdle();
					unprotectedReturned.store(true, std::memory_order_release);
					{
						std::lock_guard<std::mutex> lock(negMutex);
					}
					negCv.notify_all();
				});

				bool unprotectedReturnedPromptly = false;
				{
					std::unique_lock<std::mutex> lock(negMutex);
					unprotectedReturnedPromptly = negCv.wait_for(
						lock,
						std::chrono::seconds(2),
						[&] { return unprotectedReturned.load(std::memory_order_acquire); });
				}

				if (!unprotectedReturnedPromptly)
					q.Close();
				negWaiter.join();

				Assert::IsTrue(
					unprotectedReturnedPromptly,
					L"Negative control: with busy_ cleared right after Pop() (unprotected pre-fix state), WaitIdle must return promptly");
			}

			// (c) Multi-item handoff: WaitIdle stays blocked across MarkBusy(false) while a second item is queued,
			// remains blocked after Pop() of the second item (no MarkBusy(true) call), and wakes only after final MarkBusy(false).
			Assert::IsTrue(q.Push(kPayload, kGen, epoch));
			Assert::IsTrue(q.Push(kPayload + 1, kGen, epoch));
			BackQueue<int>::Entry e1{};
			Assert::IsTrue(q.Pop(e1));
			Assert::AreEqual(kPayload, e1.item);

			{
				std::mutex multiMutex;
				std::condition_variable multiCv;
				bool multiStarted = false;
				std::atomic<bool> multiReturned{ false };

				std::thread multiWaiter;
				ThreadJoinGuard multiGuard(q, multiWaiter);
				multiWaiter = std::thread([&] {
					{
						std::lock_guard<std::mutex> lock(multiMutex);
						multiStarted = true;
					}
					multiCv.notify_all();
					q.WaitIdle();
					multiReturned.store(true, std::memory_order_release);
					{
						std::lock_guard<std::mutex> lock(multiMutex);
					}
					multiCv.notify_all();
				});

				bool multiStartedOk = false;
				bool returnedWhileSecondQueued = false;
				bool returnedWhileSecondBusy = false;
				bool returnedAfterSecondDone = false;

				{
					std::unique_lock<std::mutex> lock(multiMutex);
					multiStartedOk = multiCv.wait_for(lock, std::chrono::seconds(5), [&] { return multiStarted; });
				}

				q.MarkBusy(false);
				{
					std::unique_lock<std::mutex> lock(multiMutex);
					returnedWhileSecondQueued = multiCv.wait_for(
						lock,
						std::chrono::milliseconds(30),
						[&] { return multiReturned.load(std::memory_order_acquire); });
				}

				BackQueue<int>::Entry e2{};
				const bool poppedSecond = q.Pop(e2);
				{
					std::unique_lock<std::mutex> lock(multiMutex);
					returnedWhileSecondBusy = multiCv.wait_for(
						lock,
						std::chrono::milliseconds(30),
						[&] { return multiReturned.load(std::memory_order_acquire); });
				}

				q.MarkBusy(false);
				{
					std::unique_lock<std::mutex> lock(multiMutex);
					returnedAfterSecondDone = multiCv.wait_for(
						lock,
						std::chrono::seconds(2),
						[&] { return multiReturned.load(std::memory_order_acquire); });
				}

				if (!returnedAfterSecondDone)
					q.Close();
				multiWaiter.join();

				Assert::IsTrue(multiStartedOk, L"Multi-item WaitIdle worker thread must start");
				Assert::IsTrue(poppedSecond, L"Second queued item must pop cleanly");
				Assert::AreEqual(kPayload + 1, e2.item);
				Assert::IsFalse(returnedWhileSecondQueued, L"WaitIdle must stay blocked while second item is still queued");
				Assert::IsFalse(returnedWhileSecondBusy, L"WaitIdle must stay blocked after popping second item before MarkBusy(false)");
				Assert::IsTrue(returnedAfterSecondDone, L"WaitIdle must wake once second item finishes with MarkBusy(false)");
			}
		}

		TEST_METHOD(WaitIdleWakesPromptlyWhenStaleDrainEmptiesBackQueue)
		{
			struct ThreadJoinGuard
			{
				BackQueue<int>& queue;
				std::thread& worker;
				ThreadJoinGuard(BackQueue<int>& qRef, std::thread& tRef) : queue(qRef), worker(tRef) {}
				ThreadJoinGuard(const ThreadJoinGuard&) = delete;
				ThreadJoinGuard& operator=(const ThreadJoinGuard&) = delete;
				~ThreadJoinGuard()
				{
					if (worker.joinable())
					{
						queue.Close();
						worker.join();
					}
				}
			};

			// (a) SetActiveGeneration drains the last stale-generation item while WaitIdle() is blocked.
			{
				BackQueue<int> q(4);
				const uint64_t epoch = q.Epoch();
				Assert::IsTrue(q.Push(100, /*generation=*/1, epoch));

				std::mutex cvMutex;
				std::condition_variable cv;
				bool started = false;
				std::atomic<bool> woke{ false };

				std::thread waiter;
				ThreadJoinGuard guard(q, waiter);
				waiter = std::thread([&] {
					{
						std::lock_guard<std::mutex> lock(cvMutex);
						started = true;
					}
					cv.notify_all();
					q.WaitIdle();
					woke.store(true, std::memory_order_release);
					{
						std::lock_guard<std::mutex> lock(cvMutex);
					}
					cv.notify_all();
				});

				bool startedOk = false;
				bool wokeBeforeDrain = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					startedOk = cv.wait_for(lock, std::chrono::seconds(5), [&] { return started; });
					if (startedOk)
					{
						wokeBeforeDrain = cv.wait_for(
							lock,
							std::chrono::milliseconds(30),
							[&] { return woke.load(std::memory_order_acquire); });
					}
				}

				const size_t dropped = q.SetActiveGeneration(2);

				bool wokeAfterGenDrain = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					wokeAfterGenDrain = cv.wait_for(
						lock,
						std::chrono::seconds(2),
						[&] { return woke.load(std::memory_order_acquire); });
				}

				if (!wokeAfterGenDrain)
					q.Close();
				waiter.join();

				Assert::IsTrue(startedOk, L"Generation-drain WaitIdle worker thread must start");
				Assert::IsFalse(wokeBeforeDrain, L"WaitIdle must block while stale gen-1 item is queued");
				Assert::AreEqual<size_t>(1, dropped, L"SetActiveGeneration must drop the queued gen-1 item");
				Assert::IsTrue(
					wokeAfterGenDrain,
					L"WaitIdle must wake promptly without deadlock when SetActiveGeneration drains the queue");
			}

			// (b) DiscardAllAndBumpEpoch drains the queued item while WaitIdle() is blocked.
			{
				BackQueue<int> q(4);
				Assert::IsTrue(q.Push(200, /*generation=*/1, q.Epoch()));

				std::mutex cvMutex;
				std::condition_variable cv;
				bool started = false;
				std::atomic<bool> woke{ false };

				std::thread waiter;
				ThreadJoinGuard guard(q, waiter);
				waiter = std::thread([&] {
					{
						std::lock_guard<std::mutex> lock(cvMutex);
						started = true;
					}
					cv.notify_all();
					q.WaitIdle();
					woke.store(true, std::memory_order_release);
					{
						std::lock_guard<std::mutex> lock(cvMutex);
					}
					cv.notify_all();
				});

				bool startedOk = false;
				bool wokeBeforeDiscard = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					startedOk = cv.wait_for(lock, std::chrono::seconds(5), [&] { return started; });
					if (startedOk)
					{
						wokeBeforeDiscard = cv.wait_for(
							lock,
							std::chrono::milliseconds(30),
							[&] { return woke.load(std::memory_order_acquire); });
					}
				}

				const size_t dropped = q.DiscardAllAndBumpEpoch();

				bool wokeAfterDiscard = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					wokeAfterDiscard = cv.wait_for(
						lock,
						std::chrono::seconds(2),
						[&] { return woke.load(std::memory_order_acquire); });
				}

				if (!wokeAfterDiscard)
					q.Close();
				waiter.join();

				Assert::IsTrue(startedOk, L"Epoch-discard WaitIdle worker thread must start");
				Assert::IsFalse(wokeBeforeDiscard, L"WaitIdle must block while item is queued before epoch bump");
				Assert::AreEqual<size_t>(1, dropped, L"DiscardAllAndBumpEpoch must drop the queued item");
				Assert::IsTrue(
					wokeAfterDiscard,
					L"WaitIdle must wake promptly without deadlock when DiscardAllAndBumpEpoch drains the queue");
			}

			// (c) Busy-protection check during stale drain: emptying the queue via SetActiveGeneration must NOT
			// unblock WaitIdle() while an already-popped item is still owned by the consumer until MarkBusy(false).
			{
				BackQueue<int> q(4);
				const uint64_t epoch = q.Epoch();
				Assert::IsTrue(q.Push(301, /*generation=*/1, epoch));
				Assert::IsTrue(q.Push(302, /*generation=*/1, epoch));

				BackQueue<int>::Entry e{};
				Assert::IsTrue(q.Pop(e));
				Assert::AreEqual(301, e.item);

				std::mutex cvMutex;
				std::condition_variable cv;
				bool started = false;
				std::atomic<bool> woke{ false };

				std::thread waiter;
				ThreadJoinGuard guard(q, waiter);
				waiter = std::thread([&] {
					{
						std::lock_guard<std::mutex> lock(cvMutex);
						started = true;
					}
					cv.notify_all();
					q.WaitIdle();
					woke.store(true, std::memory_order_release);
					{
						std::lock_guard<std::mutex> lock(cvMutex);
					}
					cv.notify_all();
				});

				bool startedOk = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					startedOk = cv.wait_for(lock, std::chrono::seconds(5), [&] { return started; });
				}

				const size_t dropped = q.SetActiveGeneration(2);

				bool wokeWhilePoppedBusy = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					wokeWhilePoppedBusy = cv.wait_for(
						lock,
						std::chrono::milliseconds(50),
						[&] { return woke.load(std::memory_order_acquire); });
				}

				q.MarkBusy(false);

				bool wokeAfterConsumerFinished = false;
				{
					std::unique_lock<std::mutex> lock(cvMutex);
					wokeAfterConsumerFinished = cv.wait_for(
						lock,
						std::chrono::seconds(2),
						[&] { return woke.load(std::memory_order_acquire); });
				}

				if (!wokeAfterConsumerFinished)
					q.Close();
				waiter.join();

				Assert::IsTrue(startedOk, L"Busy-drain WaitIdle worker thread must start");
				Assert::AreEqual<size_t>(1, dropped, L"SetActiveGeneration must drop remaining stale queued item");
				Assert::IsFalse(
					wokeWhilePoppedBusy,
					L"Stale drain emptying queue must not wake WaitIdle while an already-popped item is still busy");
				Assert::IsTrue(
					wokeAfterConsumerFinished,
					L"WaitIdle must wake promptly once consumer finishes the popped item after stale drain");
			}
		}
	};
}
