// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 16 P1 (refs/105 s3.2 / s5): CPU seam tests for ProducerStages.h (StagingRing<3>, BackQueue,
// OrderedPublishGate): strict publish order, stop mid-pipeline, generation change and restart,
// breakBefore across stages, and on-demand requests answered in submit order.
#include "pch.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/ProducerStages.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
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
			struct ProducerStopGuard
			{
				MotionAhead::Producer& producer;
				explicit ProducerStopGuard(MotionAhead::Producer& p) : producer(p) {}
				ProducerStopGuard(const ProducerStopGuard&) = delete;
				ProducerStopGuard& operator=(const ProducerStopGuard&) = delete;
				~ProducerStopGuard() { producer.Stop(); }
			};

			auto waitForFrames = [](const MotionAhead::Producer& prod, unsigned long long targetFrames) {
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < deadline)
				{
					if (prod.GetStats().frames >= targetFrames)
						return true;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				return prod.GetStats().frames >= targetFrames;
			};

			constexpr uint64_t kGen = 1;
			const AnalysisLumaSource dummyLuma{};

			// (a) Real MotionAhead::Producer driven via StartForTest + Submit:
			// frames 1, 2, 3 (breakBefore=false), 4 (breakBefore=true), 5, 6 (breakBefore=false).
			// Published P2 pairs must be (1,2), (2,3), (4,5), (5,6); discontinuity pair (3,4) must NOT publish.
			{
				std::mutex traceMutex;
				std::vector<uint64_t> pairToSeqs;
				std::vector<bool> pairContinuous;

				auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
				seam->job = [](uint64_t, uint64_t) { return true; };
				seam->onPairContinuous = [&](uint64_t toSeq, bool continuous) {
					std::lock_guard<std::mutex> lock(traceMutex);
					pairToSeqs.push_back(toSeq);
					pairContinuous.push_back(continuous);
				};

				MotionAhead::Producer producer;
				ProducerStopGuard stopGuard(producer);
				producer.SetR6AsyncExtras(false);
				producer.SetDepth(6);
				Assert::IsTrue(producer.StartForTest(seam));

				const std::vector<std::pair<uint64_t, bool>> frames = {
					{ 1, false }, { 2, false }, { 3, false }, { 4, true }, { 5, false }, { 6, false }
				};
				for (const auto& f : frames)
					producer.Submit(f.first, kGen, dummyLuma, f.second);

				Assert::IsTrue(waitForFrames(producer, frames.size()), L"Producer must process all 6 submitted frames");

				const MotionAhead::Producer::Stats stats = producer.GetStats();
				Assert::AreEqual<unsigned long long>(6ull, stats.frames);
				Assert::AreEqual<unsigned long long>(4ull, stats.pairs, L"breakBefore on frame 4 must suppress pair (3,4)");
				Assert::AreEqual<unsigned long long>(0ull, stats.dropped);
				Assert::AreEqual<unsigned long long>(0ull, stats.failures);

				const std::shared_ptr<VpMotion::P2Slot> p2 = producer.Slot();
				Assert::IsTrue(p2 != nullptr);
				Assert::AreEqual<uint64_t>(kGen, p2->Generation());
				Assert::IsTrue(p2->Has(1, 2) && p2->Find(1, 2) != nullptr, L"pair (1,2) must be published in P2");
				Assert::IsTrue(p2->Has(2, 3) && p2->Find(2, 3) != nullptr, L"pair (2,3) must be published in P2");
				Assert::IsFalse(p2->Has(3, 4), L"discontinuity pair (3,4) must NOT be published in P2 when frame 4 has breakBefore=true");
				Assert::IsTrue(p2->Find(3, 4) == nullptr, L"discontinuity pair (3,4) lookup must miss in P2");
				Assert::IsTrue(p2->Has(4, 5) && p2->Find(4, 5) != nullptr, L"pair (4,5) must be published in P2 after breakBefore");
				Assert::IsTrue(p2->Has(5, 6) && p2->Find(5, 6) != nullptr, L"pair (5,6) must be published in P2");

				producer.Stop();

				std::lock_guard<std::mutex> lock(traceMutex);
				Assert::AreEqual<size_t>(4, pairToSeqs.size(), L"exactly 4 pairs must run in strictly increasing order");
				Assert::AreEqual<size_t>(4, pairContinuous.size());
				Assert::AreEqual<uint64_t>(2, pairToSeqs[0]);
				Assert::AreEqual<uint64_t>(3, pairToSeqs[1]);
				Assert::AreEqual<uint64_t>(5, pairToSeqs[2]);
				Assert::AreEqual<uint64_t>(6, pairToSeqs[3]);
				Assert::IsFalse(pairContinuous[0], L"first pair (1,2) has no prior pair");
				Assert::IsTrue(pairContinuous[1], L"continuous pair (2,3) must report temporal continuity");
				Assert::IsFalse(pairContinuous[2], L"pair (4,5) after breakBefore must not claim temporal continuity");
				Assert::IsTrue(pairContinuous[3], L"continuous pair (5,6) after re-seed must restore temporal continuity");
			}

			// (b) Same-input negative control (Rule 3): run a second real MotionAhead::Producer on the EXACT SAME
			// 6 frames (1..6, gen 1) with breakBefore=false on frame 4, proving pair (3,4) IS published when the
			// breakBefore flag is absent.
			{
				auto controlSeam = std::make_shared<MotionAhead::Producer::TestEngine>();
				controlSeam->job = [](uint64_t, uint64_t) { return true; };

				MotionAhead::Producer controlProducer;
				ProducerStopGuard controlStopGuard(controlProducer);
				controlProducer.SetR6AsyncExtras(false);
				controlProducer.SetDepth(6);
				Assert::IsTrue(controlProducer.StartForTest(controlSeam));

				for (uint64_t seq = 1; seq <= 6; ++seq)
					controlProducer.Submit(seq, kGen, dummyLuma, false);

				Assert::IsTrue(waitForFrames(controlProducer, 6), L"Negative-control Producer must process all 6 frames");

				const MotionAhead::Producer::Stats ctrlStats = controlProducer.GetStats();
				Assert::AreEqual<unsigned long long>(6ull, ctrlStats.frames);
				Assert::AreEqual<unsigned long long>(
					5ull,
					ctrlStats.pairs,
					L"Negative control: without breakBefore=true, all 5 consecutive pairs including (3,4) must publish");
				Assert::AreEqual<unsigned long long>(0ull, ctrlStats.dropped);

				const std::shared_ptr<VpMotion::P2Slot> ctrlP2 = controlProducer.Slot();
				Assert::IsTrue(ctrlP2 != nullptr);
				Assert::IsTrue(
					ctrlP2->Has(3, 4) && ctrlP2->Find(3, 4) != nullptr,
					L"Negative control: pair (3,4) must be published in P2 when frame 4 has breakBefore=false");
				Assert::IsTrue(ctrlP2->Has(1, 2) && ctrlP2->Has(2, 3) && ctrlP2->Has(4, 5) && ctrlP2->Has(5, 6));

				controlProducer.Stop();
			}
		}

		TEST_METHOD(RequestsAndCorePairsRideSameBackQueueInSubmitOrder)
		{
			using MotionAhead::OnDemand::Mode;
			using MotionAhead::OnDemand::PairFuture;
			using MotionAhead::OnDemand::PairRequest;
			using MotionAhead::OnDemand::PairResult;
			using MotionAhead::OnDemand::Status;

			struct ProducerStopGuard
			{
				MotionAhead::Producer& producer;
				explicit ProducerStopGuard(MotionAhead::Producer& p) : producer(p) {}
				ProducerStopGuard(const ProducerStopGuard&) = delete;
				ProducerStopGuard& operator=(const ProducerStopGuard&) = delete;
				~ProducerStopGuard() { producer.Stop(); }
			};

			struct Gate
			{
				std::mutex m;
				std::condition_variable cv;
				bool entered = false;
				bool open = false;
				void Enter()
				{
					std::unique_lock<std::mutex> lock(m);
					entered = true;
					cv.notify_all();
					cv.wait(lock, [this] { return open; });
				}
				bool WaitEntered()
				{
					std::unique_lock<std::mutex> lock(m);
					return cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; });
				}
				void Open()
				{
					std::lock_guard<std::mutex> lock(m);
					open = true;
					cv.notify_all();
				}
			};

			struct GateOpenGuard
			{
				Gate& gate;
				explicit GateOpenGuard(Gate& g) : gate(g) {}
				GateOpenGuard(const GateOpenGuard&) = delete;
				GateOpenGuard& operator=(const GateOpenGuard&) = delete;
				~GateOpenGuard() { gate.Open(); }
			};

			struct TraceStep
			{
				bool isRequest = false;
				uint64_t from = 0;
				uint64_t to = 0;
				uint64_t generation = 0;
				Mode mode = Mode::Chain;
			};

			auto makeRequest = [](uint64_t from, uint64_t to, uint64_t generation, double budgetMs = 5000.0) {
				PairRequest rq;
				rq.from = from;
				rq.to = to;
				rq.generation = generation;
				rq.budgetMs = budgetMs;
				return rq;
			};

			auto expectedVx = [](uint64_t from, uint64_t to) {
				return static_cast<int16_t>(from * 32 + to);
			};
			auto expectedVy = [](uint64_t generation, uint64_t to) {
				return static_cast<int16_t>(-static_cast<int>(generation * 32 + to));
			};

			auto waitForFrames = [](const MotionAhead::Producer& prod, unsigned long long targetFrames) {
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < deadline)
				{
					if (prod.GetStats().frames >= targetFrames)
						return true;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				return prod.GetStats().frames >= targetFrames;
			};

			Gate gateOnFrame5;
			std::mutex traceMutex;
			std::vector<TraceStep> trace;

			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			seam->job = [&](uint64_t sequence, uint64_t generation) {
				if (sequence == 5)
					gateOnFrame5.Enter();
				std::lock_guard<std::mutex> lock(traceMutex);
				trace.push_back(TraceStep{ false, sequence > 0 ? sequence - 1 : 0, sequence, generation, Mode::Chain });
				return true;
			};
			seam->request = [&](const PairRequest& rq, Mode mode, MotionAhead::Flow& flow) {
				flow.halfW = 64;
				flow.halfH = 32;
				flow.gridW = 16;
				flow.gridH = 8;
				const int16_t vx = expectedVx(rq.from, rq.to);
				const int16_t vy = expectedVy(rq.generation, rq.to);
				flow.forward = { vx, vy };
				flow.backward = { static_cast<int16_t>(-vx), static_cast<int16_t>(-vy) };
				flow.forwardCost = { 4 };
				flow.backwardCost = { 4 };
				std::lock_guard<std::mutex> lock(traceMutex);
				trace.push_back(TraceStep{ true, rq.from, rq.to, rq.generation, mode });
				return true;
			};

			// Declaration order guarantees GateOpenGuard opens gateOnFrame5 BEFORE ProducerStopGuard stops/joins worker.
			MotionAhead::Producer producer;
			ProducerStopGuard stopGuard(producer);
			GateOpenGuard gateGuard(gateOnFrame5);

			producer.SetR6AsyncExtras(false);
			producer.SetDepth(8);
			Assert::IsTrue(producer.StartForTest(seam));

			const AnalysisLumaSource dummyLuma{};

			// Phase 1: Core walk frames 1, 2, 3 (gen 1) -> publishes (1,2) and (2,3) in P2.
			producer.Submit(1, 1, dummyLuma);
			producer.Submit(2, 1, dummyLuma);
			producer.Submit(3, 1, dummyLuma);
			Assert::IsTrue(waitForFrames(producer, 3), L"Phase 1 core frames 1..3 must complete");

			// Phase 2: Interleaved adjacent on-demand Chain request (3 -> 4, gen 1).
			PairFuture futChain34 = producer.Request(makeRequest(3, 4, 1));
			Assert::IsTrue(futChain34.WaitFor(5000.0), L"Adjacent on-demand request (3,4) must resolve");
			const PairResult resChain34 = futChain34.Get();
			Assert::IsTrue(resChain34.status == Status::Ready, L"Adjacent request (3,4) must resolve Ready");
			Assert::IsTrue(resChain34.flow != nullptr && resChain34.flow->forward.size() == 2 && resChain34.flow->backward.size() == 2);
			Assert::AreEqual(static_cast<int>(expectedVx(3, 4)), static_cast<int>(resChain34.flow->forward[0]));
			Assert::AreEqual(static_cast<int>(expectedVy(1, 4)), static_cast<int>(resChain34.flow->forward[1]));
			Assert::AreEqual(static_cast<int>(-expectedVx(3, 4)), static_cast<int>(resChain34.flow->backward[0]));
			Assert::AreEqual(static_cast<int>(-expectedVy(1, 4)), static_cast<int>(resChain34.flow->backward[1]));

			// Phase 3: Interleave core frames 5 and 6 (gen 1) with BOTH an off-chain gap pair (1 -> 3, gen 1)
			// and a forced off-chain newer-generation pair (10 -> 11, gen 2) while worker is paused in frame 5.
			producer.Submit(5, 1, dummyLuma);
			Assert::IsTrue(gateOnFrame5.WaitEntered(), L"Worker must reach gated core frame 5");

			producer.Submit(6, 1, dummyLuma);
			PairFuture futOffChainGap13 = producer.Request(makeRequest(1, 3, 1));
			PairFuture futForcedOffChainGen2 = producer.Request(makeRequest(10, 11, 2));

			gateOnFrame5.Open();

			Assert::IsTrue(waitForFrames(producer, 5), L"All 5 core frames (1,2,3,5,6) must complete");
			Assert::IsTrue(futOffChainGap13.WaitFor(5000.0), L"Off-chain gap request (1,3) must resolve");
			Assert::IsTrue(futForcedOffChainGen2.WaitFor(5000.0), L"Forced off-chain gen-2 request (10,11) must resolve");

			const PairResult resGap13 = futOffChainGap13.Get();
			Assert::IsTrue(resGap13.status == Status::Ready, L"Off-chain gap request (1,3) must resolve Ready");
			Assert::IsTrue(resGap13.flow != nullptr && resGap13.flow->forward.size() == 2 && resGap13.flow->backward.size() == 2);
			Assert::AreEqual(static_cast<int>(expectedVx(1, 3)), static_cast<int>(resGap13.flow->forward[0]));
			Assert::AreEqual(static_cast<int>(expectedVy(1, 3)), static_cast<int>(resGap13.flow->forward[1]));
			Assert::AreEqual(static_cast<int>(-expectedVx(1, 3)), static_cast<int>(resGap13.flow->backward[0]));
			Assert::AreEqual(static_cast<int>(-expectedVy(1, 3)), static_cast<int>(resGap13.flow->backward[1]));

			const PairResult resOffGen2 = futForcedOffChainGen2.Get();
			Assert::IsTrue(resOffGen2.status == Status::Ready, L"Forced off-chain gen-2 request (10,11) must resolve Ready");
			Assert::IsTrue(resOffGen2.flow != nullptr && resOffGen2.flow->forward.size() == 2 && resOffGen2.flow->backward.size() == 2);
			Assert::AreEqual(static_cast<int>(expectedVx(10, 11)), static_cast<int>(resOffGen2.flow->forward[0]));
			Assert::AreEqual(static_cast<int>(expectedVy(2, 11)), static_cast<int>(resOffGen2.flow->forward[1]));
			Assert::AreEqual(static_cast<int>(-expectedVx(10, 11)), static_cast<int>(resOffGen2.flow->backward[0]));
			Assert::AreEqual(static_cast<int>(-expectedVy(2, 11)), static_cast<int>(resOffGen2.flow->backward[1]));

			// Verify real P2 slot contents: all gen-1 core pairs + adjacent Chain request (3,4) are published;
			// off-chain gap (1,3) and forced off-chain gen-2 (10,11) do NOT pollute or wipe the gen-1 P2 slot.
			const std::shared_ptr<VpMotion::P2Slot> p2 = producer.Slot();
			Assert::IsTrue(p2 != nullptr);
			Assert::AreEqual<uint64_t>(1ull, p2->Generation(), L"Forced off-chain gen-2 request must not overwrite gen-1 P2 slot");
			Assert::IsTrue(p2->Has(1, 2) && p2->Find(1, 2) != nullptr, L"Core pair (1,2) must be in P2");
			Assert::IsTrue(p2->Has(2, 3) && p2->Find(2, 3) != nullptr, L"Core pair (2,3) must be in P2");
			const MotionAhead::FlowPtr p2Pair34 = p2->Find(3, 4);
			Assert::IsTrue(p2Pair34 != nullptr && p2Pair34->forward.size() == 2, L"Adjacent on-demand pair (3,4) must be published in P2");
			Assert::AreEqual(static_cast<int>(expectedVx(3, 4)), static_cast<int>(p2Pair34->forward[0]));
			Assert::AreEqual(static_cast<int>(expectedVy(1, 4)), static_cast<int>(p2Pair34->forward[1]));
			Assert::IsTrue(p2->Has(4, 5) && p2->Find(4, 5) != nullptr, L"Core pair (4,5) chaining after request (3,4) must be in P2");
			Assert::IsTrue(p2->Has(5, 6) && p2->Find(5, 6) != nullptr, L"Core pair (5,6) must be in P2");
			Assert::IsFalse(p2->Has(1, 3), L"Off-chain gap pair (1,3) must stay out of adjacency-only P2 slot");
			Assert::IsFalse(p2->Has(10, 11), L"Forced off-chain gen-2 pair (10,11) must stay out of gen-1 P2 slot");

			// Compute-once check: requesting an already-published core pair answers FromSlot immediately with zero engine work.
			PairFuture futHit23 = producer.Request(makeRequest(2, 3, 1));
			Assert::IsTrue(futHit23.Ready(), L"Published pair (2,3) must answer synchronously FromSlot");
			const PairResult resHit23 = futHit23.Get();
			Assert::IsTrue(resHit23.status == Status::FromSlot);
			Assert::IsTrue(resHit23.flow != nullptr && resHit23.flow.get() == p2->Find(2, 3).get());

			// Join the worker completely BEFORE reading stats so Producer::Answer() has finished incrementing
			// requestStats.ready after OnDemand::Fulfil() woke the caller (eliminates under-load race).
			producer.Stop();

			const MotionAhead::Producer::Stats stats = producer.GetStats();
			const MotionAhead::Producer::RequestStats reqStats = producer.GetRequestStats();
			Assert::AreEqual<unsigned long long>(5ull, stats.frames);
			Assert::AreEqual<unsigned long long>(4ull, stats.pairs);
			Assert::AreEqual<unsigned long long>(0ull, stats.dropped);
			Assert::AreEqual<unsigned long long>(0ull, stats.failures);
			Assert::AreEqual<unsigned long long>(3ull, reqStats.ready);
			Assert::AreEqual<unsigned long long>(1ull, reqStats.fromSlot);
			Assert::AreEqual<unsigned long long>(1ull, reqStats.chainRuns);
			Assert::AreEqual<unsigned long long>(2ull, reqStats.offChainRuns);
			Assert::AreEqual<unsigned long long>(1ull, reqStats.forcedOffChain);
			Assert::IsTrue(reqStats.coreAheadOfRequests >= 1ull, L"Core frame 6 must run ahead of waiting on-demand requests");
			Assert::AreEqual<unsigned long long>(0ull, reqStats.refused + reqStats.failed);
			Assert::AreEqual<unsigned long long>(0ull, reqStats.unavailable);

			// Verify exact execution order: core 1,2,3 -> request (3,4) Chain -> core 5,6 -> requests (1,3) and (10,11) OffChain in submit order.
			std::lock_guard<std::mutex> lock(traceMutex);
			Assert::AreEqual<size_t>(8, trace.size());
			Assert::IsFalse(trace[0].isRequest); Assert::AreEqual<uint64_t>(1, trace[0].to);
			Assert::IsFalse(trace[1].isRequest); Assert::AreEqual<uint64_t>(2, trace[1].to);
			Assert::IsFalse(trace[2].isRequest); Assert::AreEqual<uint64_t>(3, trace[2].to);
			Assert::IsTrue(trace[3].isRequest);  Assert::AreEqual<uint64_t>(4, trace[3].to);  Assert::IsTrue(trace[3].mode == Mode::Chain);
			Assert::IsFalse(trace[4].isRequest); Assert::AreEqual<uint64_t>(5, trace[4].to);
			Assert::IsFalse(trace[5].isRequest); Assert::AreEqual<uint64_t>(6, trace[5].to);
			Assert::IsTrue(trace[6].isRequest);  Assert::AreEqual<uint64_t>(3, trace[6].to);  Assert::IsTrue(trace[6].mode == Mode::OffChain);
			Assert::IsTrue(trace[7].isRequest);  Assert::AreEqual<uint64_t>(11, trace[7].to); Assert::IsTrue(trace[7].mode == Mode::OffChain);
		}

		TEST_METHOD(BackQueueIgnoreGenerationSurvivesActiveGenerationPinWithNegativeControl)
		{
			struct WorkItem
			{
				bool isRequest = false;
				uint64_t from = 0;
				uint64_t to = 0;
			};

			BackQueue<WorkItem> q(8);
			const uint64_t epoch = q.Epoch();
			const WorkItem coreGen1{ false, 1, 2 };
			const WorkItem forcedGen2{ true, 10, 11 };

			// (a) Pre-pin queueing: push gen-1 core (ignoreGeneration=false), gen-2 forced (ignoreGeneration=true),
			// and same-input negative control (identical payload + gen 2 + epoch, ignoreGeneration=false).
			Assert::IsTrue(q.Push(coreGen1, /*generation=*/1, epoch, /*ignoreGeneration=*/false));
			Assert::IsTrue(q.Push(forcedGen2, /*generation=*/2, epoch, /*ignoreGeneration=*/true));
			Assert::IsTrue(q.Push(forcedGen2, /*generation=*/2, epoch, /*ignoreGeneration=*/false));
			Assert::AreEqual<size_t>(3, q.Size());

			// Pin active generation to 1: SetActiveGeneration(1) must drop ONLY the gen-2 ignoreGeneration=false
			// control item while keeping both the gen-1 core item and the gen-2 ignoreGeneration=true item.
			Assert::AreEqual<size_t>(
				1,
				q.SetActiveGeneration(1),
				L"SetActiveGeneration(1) must discard only the gen-2 ignoreGeneration=false negative control");
			Assert::AreEqual<uint64_t>(1ull, q.DiscardedCount());
			Assert::AreEqual<size_t>(2, q.Size());

			// IsLive while queue is open: ignoreGeneration=true stays live across active generation pin,
			// whereas the same-input ignoreGeneration=false negative control reports not live.
			Assert::IsTrue(q.IsLive(1, epoch, /*ignoreGeneration=*/false));
			Assert::IsTrue(
				q.IsLive(2, epoch, /*ignoreGeneration=*/true),
				L"gen-2 ignoreGeneration=true entry must remain live under active generation 1");
			Assert::IsFalse(
				q.IsLive(2, epoch, /*ignoreGeneration=*/false),
				L"Negative control: gen-2 ignoreGeneration=false entry must not be live under active generation 1");

			// (b) Post-pin queueing under active generation 1: gen-1 core and gen-2 ignoreGeneration=true
			// both enqueue, while the exact same-input gen-2 ignoreGeneration=false control is rejected by Push().
			const WorkItem coreGen1After{ false, 2, 3 };
			const WorkItem forcedGen2After{ true, 20, 21 };
			Assert::IsTrue(q.Push(coreGen1After, /*generation=*/1, epoch, /*ignoreGeneration=*/false));
			Assert::IsTrue(q.Push(forcedGen2After, /*generation=*/2, epoch, /*ignoreGeneration=*/true));
			Assert::IsFalse(
				q.Push(forcedGen2After, /*generation=*/2, epoch, /*ignoreGeneration=*/false),
				L"Negative control: Push with mismatched generation and ignoreGeneration=false must fail once pinned");

			// Epoch guard still applies even when ignoreGeneration=true.
			Assert::IsFalse(
				q.Push(forcedGen2After, /*generation=*/2, epoch + 1, /*ignoreGeneration=*/true),
				L"ignoreGeneration=true must not bypass epoch check on Push");
			Assert::IsFalse(
				q.IsLive(2, epoch + 1, /*ignoreGeneration=*/true),
				L"ignoreGeneration=true must not report live for a mismatched epoch");

			// Finish() closes to new pushes while allowing Pop() to drain all live items in strict FIFO order.
			q.Finish();

			std::vector<BackQueue<WorkItem>::Entry> drained;
			BackQueue<WorkItem>::Entry e{};
			while (q.Pop(e))
			{
				drained.push_back(e);
				q.MarkBusy(false);
			}

			Assert::AreEqual<size_t>(
				4,
				drained.size(),
				L"Exactly the two gen-1 core items and two gen-2 ignoreGeneration=true items must pop in FIFO order");

			Assert::IsFalse(drained[0].item.isRequest);
			Assert::AreEqual<uint64_t>(1, drained[0].generation);
			Assert::IsFalse(drained[0].ignoreGeneration);
			Assert::AreEqual<uint64_t>(1, drained[0].item.from);
			Assert::AreEqual<uint64_t>(2, drained[0].item.to);

			Assert::IsTrue(drained[1].item.isRequest);
			Assert::AreEqual<uint64_t>(2, drained[1].generation);
			Assert::IsTrue(drained[1].ignoreGeneration);
			Assert::AreEqual<uint64_t>(10, drained[1].item.from);
			Assert::AreEqual<uint64_t>(11, drained[1].item.to);

			Assert::IsFalse(drained[2].item.isRequest);
			Assert::AreEqual<uint64_t>(1, drained[2].generation);
			Assert::IsFalse(drained[2].ignoreGeneration);
			Assert::AreEqual<uint64_t>(2, drained[2].item.from);
			Assert::AreEqual<uint64_t>(3, drained[2].item.to);

			Assert::IsTrue(drained[3].item.isRequest);
			Assert::AreEqual<uint64_t>(2, drained[3].generation);
			Assert::IsTrue(drained[3].ignoreGeneration);
			Assert::AreEqual<uint64_t>(20, drained[3].item.from);
			Assert::AreEqual<uint64_t>(21, drained[3].item.to);

			// (c) Epoch discard still drops ignoreGeneration=true items on Stop/restart.
			q.Reopen();
			const uint64_t epoch2 = q.Epoch();
			Assert::IsTrue(q.Push(forcedGen2, /*generation=*/2, epoch2, /*ignoreGeneration=*/true));
			Assert::AreEqual<size_t>(
				1,
				q.DiscardAllAndBumpEpoch(),
				L"DiscardAllAndBumpEpoch must drop ignoreGeneration=true items on epoch bump");
			Assert::AreEqual<size_t>(0, q.Size());
			Assert::IsFalse(
				q.IsLive(2, epoch2, /*ignoreGeneration=*/true),
				L"ignoreGeneration=true item must not remain live after epoch bump");
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
