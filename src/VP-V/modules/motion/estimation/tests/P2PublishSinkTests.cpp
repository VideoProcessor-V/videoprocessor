// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// N#3 (Y1b-3): P2Slot observer + VectorEngine::SetP2PublishSink contract tests.
// Verifies:
//   1. P2Slot::Publish invokes observer AFTER store_.Put (and outside store_'s mutex) with exact
//      (from, to, generation), while stale-generation Publish returns false and does NOT invoke observer.
//   2. VectorEngine::SetP2PublishSink populates epoch and globalUsable (both usable and unusable
//      negative controls proving globalUsable is a fact, never a publish gate).
//   3. SetP2PublishSink({}) blocks until an in-flight BACK-thread observer finishes (teardown drain
//      safety), and destroying VectorEngine stops further observer calls on a retained P2Slot.
// Pure CPU; built without PCH.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace P2PublishSinkTestsNs
{
	MotionAhead::FlowPtr FlowWithGlobal(bool set, float z, float w = 1.0f)
	{
		std::shared_ptr<MotionAhead::Flow> flow = std::make_shared<MotionAhead::Flow>();
		flow->globalSet = set;
		flow->global[0][2] = z;
		flow->global[0][3] = w;
		return flow;
	}

	TEST_CLASS(P2PublishSinkTests)
	{
	public:
		TEST_METHOD(P2SlotPublishInvokesObserverAfterPutAndRefusesStaleGeneration)
		{
			VpMotion::P2Slot slot(16);
			std::vector<VpMotion::P2Published> seen;
			bool alreadyInStoreInsideObserver = false;

			slot.SetObserver([&](const VpMotion::P2Published& p, const MotionAhead::FlowPtr& f) {
				Assert::IsTrue(f != nullptr, L"observer receives published FlowPtr");
				// Calling slot.Has inside the observer also proves observer runs outside store_'s mutex.
				alreadyInStoreInsideObserver = slot.Has(p.from, p.to);
				seen.push_back(p);
			});

			const uint64_t gen0 = slot.Generation();
			Assert::IsTrue(slot.Publish(10, 11, gen0, FlowWithGlobal(true, 0.1f, 1.0f)));
			Assert::IsTrue(alreadyInStoreInsideObserver, L"pair must already be in P2Slot before observer runs");
			Assert::AreEqual(size_t(1), seen.size());
			Assert::AreEqual(uint64_t(10), seen[0].from);
			Assert::AreEqual(uint64_t(11), seen[0].to);
			Assert::AreEqual(gen0, seen[0].generation);

			// In-test negative control: stale generation Publish returns false and NEVER invokes observer.
			slot.SetGeneration(gen0 + 3);
			Assert::IsFalse(slot.Publish(11, 12, gen0 + 2, FlowWithGlobal(true, 0.1f, 1.0f)),
				L"negative control: stale generation must be refused by P2Slot::Publish");
			Assert::IsFalse(slot.Has(11, 12), L"refused pair must not enter store");
			Assert::AreEqual(size_t(1), seen.size(), L"refused pair must not invoke observer");

			// Positive contrast at matching new generation succeeds and notifies observer.
			Assert::IsTrue(slot.Publish(11, 12, gen0 + 3, FlowWithGlobal(true, 0.1f, 1.0f)));
			Assert::AreEqual(size_t(2), seen.size());
			Assert::AreEqual(gen0 + 3, seen[1].generation);

			// Cleared observer is never invoked again even on accepted Publish.
			slot.SetObserver(VpMotion::P2Slot::Observer());
			Assert::IsTrue(slot.Publish(12, 13, gen0 + 3, FlowWithGlobal(true, 0.1f, 1.0f)));
			Assert::IsTrue(slot.Has(12, 13));
			Assert::AreEqual(size_t(2), seen.size());
		}

		TEST_METHOD(VectorEngineSinkPopulatesEpochAndGlobalUsableAsFactsNotGate)
		{
			VpMotion::VectorEngine engine;
			std::vector<VpMotion::P2Published> seen;
			engine.SetP2PublishSink([&seen](const VpMotion::P2Published& p) {
				seen.push_back(p);
			});

			const uint64_t gen = engine.p2Slot->Generation();
			Assert::AreEqual(gen, engine.P2SlotGeneration());
			const uint64_t epoch0 = engine.motionValidity.Epoch();

			// Usable global: globalSet == true, z < 0.9, w > 0.5.
			Assert::IsTrue(engine.p2Slot->Publish(10, 11, gen, FlowWithGlobal(true, 0.2f, 1.0f)));
			// Negative controls (unusable global: z >= 0.9, w <= 0.5, globalSet == false) MUST still publish!
			Assert::IsTrue(engine.p2Slot->Publish(11, 12, gen, FlowWithGlobal(true, 0.95f, 1.0f)));
			Assert::IsTrue(engine.p2Slot->Publish(12, 13, gen, FlowWithGlobal(true, 0.2f, 0.0f)));
			Assert::IsTrue(engine.p2Slot->Publish(13, 14, gen, FlowWithGlobal(false, 0.1f, 1.0f)));

			Assert::AreEqual(size_t(4), seen.size(),
				L"globalUsable is a fact, never a publish gate: all 4 pairs must reach sink");
			Assert::IsTrue(seen[0].globalUsable, L"clear valid global must be marked usable");
			Assert::IsFalse(seen[1].globalUsable, L"negative control: ambiguous global (z >= 0.9) is unusable");
			Assert::IsFalse(seen[2].globalUsable, L"negative control: invalid weight (w <= 0.5) is unusable");
			Assert::IsFalse(seen[3].globalUsable, L"negative control: unset global (globalSet == false) is unusable");
			for (size_t i = 0; i < 4; ++i)
				Assert::AreEqual(epoch0, seen[i].epoch, L"sink must stamp current motionValidity epoch");

			// Epoch advance is reflected on subsequent publishes.
			engine.motionValidity.Invalidate(MotionValidity::Cause::Gap);
			const uint64_t epoch1 = engine.motionValidity.Epoch();
			Assert::IsTrue(epoch1 > epoch0);
			Assert::IsTrue(engine.p2Slot->Publish(14, 15, gen, FlowWithGlobal(true, 0.15f, 1.0f)));
			Assert::AreEqual(size_t(5), seen.size());
			Assert::AreEqual(epoch1, seen[4].epoch, L"sink must observe advanced validity epoch");
			Assert::IsTrue(seen[4].globalUsable);
		}

		TEST_METHOD(ClearSinkBlocksUntilInFlightBackThreadObserverFinishesAndDestroyedEngineStopsCalls)
		{
			// 1. Synchronized teardown drain proof: SetP2PublishSink({}) waits for in-flight BACK-thread observer.
			{
				VpMotion::VectorEngine engine;
				std::mutex mu;
				std::condition_variable cv;
				bool observerEntered = false;
				bool allowObserverExit = false;
				bool observerFinished = false;
				std::atomic<bool> clearReturned{ false };
				std::atomic<int> callCount{ 0 };

				engine.SetP2PublishSink([&](const VpMotion::P2Published&) {
					callCount.fetch_add(1, std::memory_order_relaxed);
					{
						std::lock_guard<std::mutex> lock(mu);
						observerEntered = true;
					}
					cv.notify_all();
					std::unique_lock<std::mutex> lock(mu);
					cv.wait(lock, [&]() { return allowObserverExit; });
					observerFinished = true;
				});

				const uint64_t gen = engine.p2Slot->Generation();
				std::thread backThread([&]() {
					Assert::IsTrue(engine.p2Slot->Publish(1, 2, gen, FlowWithGlobal(true, 0.1f, 1.0f)));
				});

				// Wait until BACK thread is genuinely inside the sink observer.
				{
					std::unique_lock<std::mutex> lock(mu);
					Assert::IsTrue(cv.wait_for(lock, std::chrono::seconds(5), [&]() { return observerEntered; }),
						L"BACK thread must enter observer");
				}

				// Launch teardown thread calling SetP2PublishSink({}) while observer is blocked in flight.
				std::thread teardownThread([&]() {
					engine.SetP2PublishSink({});
					clearReturned.store(true, std::memory_order_release);
				});

				// Give teardown thread time to attempt acquiring observerMutex_; it MUST stay blocked!
				std::this_thread::sleep_for(std::chrono::milliseconds(30));
				Assert::IsFalse(clearReturned.load(std::memory_order_acquire),
					L"SetP2PublishSink({}) must block while an in-flight BACK-thread observer is still running");

				// Release the observer; both BACK thread and teardown thread must now complete cleanly.
				{
					std::lock_guard<std::mutex> lock(mu);
					allowObserverExit = true;
				}
				cv.notify_all();

				backThread.join();
				teardownThread.join();
				Assert::IsTrue(observerFinished);
				Assert::IsTrue(clearReturned.load(std::memory_order_acquire));

				// Negative control: subsequent Publish after SetP2PublishSink({}) never invokes cleared observer.
				Assert::IsTrue(engine.p2Slot->Publish(2, 3, gen, FlowWithGlobal(true, 0.1f, 1.0f)));
				Assert::AreEqual(1, callCount.load(std::memory_order_relaxed));
			}

			// 2. Destroyed VectorEngine clears its P2Slot observer so a retained slot cannot call into a dead engine.
			{
				std::shared_ptr<VpMotion::P2Slot> retainedSlot;
				std::atomic<int> calls{ 0 };
				{
					VpMotion::VectorEngine engine;
					retainedSlot = engine.p2Slot;
					engine.SetP2PublishSink([&calls](const VpMotion::P2Published&) {
						calls.fetch_add(1, std::memory_order_relaxed);
					});
					Assert::IsTrue(retainedSlot->Publish(1, 2, retainedSlot->Generation(), FlowWithGlobal(true, 0.1f, 1.0f)));
					Assert::AreEqual(1, calls.load(std::memory_order_relaxed));
				}
				// Engine is now destroyed while producer still holds retainedSlot.
				Assert::IsTrue(retainedSlot->Publish(2, 3, retainedSlot->Generation(), FlowWithGlobal(true, 0.1f, 1.0f)));
				Assert::AreEqual(1, calls.load(std::memory_order_relaxed),
					L"~VectorEngine must clear P2Slot observer so no call reaches a destroyed engine");
			}
		}
	};
}
