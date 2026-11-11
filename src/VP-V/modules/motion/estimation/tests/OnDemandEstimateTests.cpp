// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W2-C5b (refs\93-prep-C5b.md section 6): the on-demand estimate entry. The CPU tests pin the request queue, the
// once-only answer, the chain-mode choice and the producer's worker rules through its test engine seam (no D3D11 device,
// no optical-flow session): a request is taken before queued look-ahead jobs, an abandoned or late request does no
// engine work, Stop answers every waiting request, adjacent pairs are published once into the P2 slot and gap pairs are
// returned only.
#include "pch.h"
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/OnDemandEstimate.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <dxgi1_2.h>
#include <cstdio>
#include <string>

#include <atomic>
#include <limits>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpMotion::OnDemand;

		PairRequest Pair(uint64_t from, uint64_t to, uint64_t generation = 1, double budgetMs = 1000.0)
		{
			PairRequest r;
			r.from = from;
			r.to = to;
			r.generation = generation;
			r.budgetMs = budgetMs;
			return r;
		}

		// A gate the seam's job callback waits on, so the test controls what the worker is doing.
		struct Gate
		{
			std::mutex m;
			std::condition_variable cv;
			bool open = false;
			bool entered = false;
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

		// What the seam saw, in order: job sequences as positive numbers, requests as the negative of their `to`.
		struct Trace
		{
			std::mutex m;
			std::vector<long long> order;
			std::vector<Mode> modes;
			void Job(uint64_t s) { std::lock_guard<std::mutex> lock(m); order.push_back(static_cast<long long>(s)); }
			void Req(uint64_t to, Mode mode)
			{
				std::lock_guard<std::mutex> lock(m);
				order.push_back(-static_cast<long long>(to));
				modes.push_back(mode);
			}
			std::vector<long long> Order() { std::lock_guard<std::mutex> lock(m); return order; }
			std::vector<Mode> Modes() { std::lock_guard<std::mutex> lock(m); return modes; }
		};

		std::shared_ptr<MotionAhead::Producer::TestEngine> Seam(Trace& trace, Gate* gate = nullptr, uint64_t gateAt = 0,
			std::atomic<bool>* failRequests = nullptr)
		{
			auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
			seam->job = [&trace, gate, gateAt](uint64_t sequence, uint64_t) {
				if (gate && sequence == gateAt) gate->Enter();
				trace.Job(sequence);
				return true;
			};
			seam->request = [&trace, failRequests](const PairRequest& rq, Mode mode, MotionAhead::Flow& flow) {
				trace.Req(rq.to, mode);
				flow.halfW = static_cast<int>(rq.to);   // marks which request made it
				return !(failRequests && failRequests->load());
			};
			return seam;
		}

		// Commit 4: a fake host for the dequeue hook (MotionAhead::IssueOnDemandAtDequeue).
		struct FakeSource
		{
			bool valid = true;
			bool IsValid() const { return valid; }
			operator AnalysisLumaSource() const { return AnalysisLumaSource(); }
		};
		struct FakePrepared { FakeSource source; };
		struct FakeState { bool dolbyVisionTunnel = false; };
		struct FakeIdentity { uint64_t sourceFormatGeneration = 0; };
		struct FakeQueued
		{
			int frame = 0;
			std::shared_ptr<FakeState> state = std::make_shared<FakeState>();
			int converted = 0, analysis = 0;
			FakeIdentity activePictureIdentity;
			uint64_t sourceSequence = 0;
		};
		struct FakeSettings { std::string opticalFlowOnDemand = "auto"; };
		struct FakeHost
		{
			VpMotion::VectorEngine vectorEngine;
			std::shared_ptr<MotionAhead::Producer>& motionProducer = vectorEngine.motionProducer;
			MotionValidity::State& motionValidity = vectorEngine.motionValidity;
			FakeSettings activeSettings;
			std::atomic<bool> lowLatencyQueueActive{ false };
			static bool prepareValid;
			static FakePrepared PrepareFrameAnalysis(int, const FakeState&, int, int, int, uint64_t, bool, bool)
			{
				FakePrepared p;
				p.source.valid = prepareValid;
				return p;
			}
		};
		bool FakeHost::prepareValid = true;
		void Dequeue(FakeHost& host, uint64_t from, uint64_t to)
		{
			FakeQueued next;
			next.sourceSequence = to;
			const std::shared_ptr<FakeState> state = std::make_shared<FakeState>();
			MotionAhead::IssueOnDemandAtDequeue(&host, 0, state, 0, 0, 0, uint64_t(0), from, next, 1.0, 1.0);   // 1 Hz: a deadline of ~0.9 s (no timing flake)
		}

		// Waits until the worker has run `n` seam calls (bounded).
		bool WaitCalls(Trace& trace, size_t n)
		{
			for (int i = 0; i < 500; ++i)
			{
				if (trace.Order().size() >= n) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return false;
		}
	}

	TEST_CLASS(OnDemandEstimateTests)
	{
	public:
		TEST_METHOD(QueueRefusesTheOldestBeyondItsBoundAndFailAllAnswersEveryWaitingRequest)
		{
			Queue q;
			unsigned long long refused = 0;
			const Clock::time_point now = Clock::now();
			PairFuture a = q.Push(Pair(1, 2), now, &refused);
			PairFuture b = q.Push(Pair(2, 3), now, &refused);
			Assert::AreEqual(size_t(2), q.Size());
			Assert::IsFalse(a.Ready(), L"queued: not answered yet");
			PairFuture c = q.Push(Pair(3, 4), now, &refused);
			Assert::AreEqual(size_t(Queue::kMaxPending), q.Size(), L"bounded");
			Assert::AreEqual(1ull, refused);
			Assert::IsTrue(a.Ready(), L"the oldest was pushed out");
			Assert::IsTrue(a.Get().status == Status::Refused);
			Pending p;
			Assert::IsTrue(q.Pop(p));
			Assert::AreEqual<uint64_t>(3, p.request.to, L"oldest first: b is next");
			Assert::AreEqual(size_t(1), q.FailAll(Status::Unavailable));
			Assert::IsTrue(q.Empty());
			Assert::IsTrue(c.Ready() && c.Get().status == Status::Unavailable, L"never a broken promise");
			Assert::IsFalse(b.Ready(), L"popped: the worker answers it");
			PairResult r;
			r.status = Status::Ready;
			Assert::IsTrue(Fulfil(*p.state, r));
			Assert::IsFalse(Fulfil(*p.state, r), L"answered once only");
			Assert::IsTrue(b.Get().status == Status::Ready);
		}

		TEST_METHOD(AFutureHasADeadlineAndAnInvalidHandleAnswersUnavailable)
		{
			PairFuture none;
			Assert::IsFalse(none.Valid());
			Assert::IsFalse(none.WaitFor(1.0));
			Assert::IsTrue(none.Get().status == Status::Unavailable);
			none.Abandon();   // harmless
			PairFuture answered = PairFuture::Answered(Status::NoBudget);
			Assert::IsTrue(answered.Valid() && answered.Ready() && answered.WaitFor(0.0));
			Assert::IsTrue(answered.Get().status == Status::NoBudget);
			Queue q;
			const Clock::time_point now = Clock::now();
			PairFuture waiting = q.Push(Pair(1, 2, 1, 5.0), now);
			Assert::IsFalse(waiting.WaitFor(2.0), L"not answered in time: false, the result untouched");
			waiting.Abandon();
			Pending p;
			Assert::IsTrue(q.Pop(p));
			Assert::IsTrue(p.state->abandoned.load(), L"the worker sees the asker gave up");
			Assert::IsTrue(p.state->deadline == now + std::chrono::microseconds(5000), L"deadline = issue + budget");
		}

		TEST_METHOD(ChooseModeKeepsTheWalkChainUntouchedWhenItRunsAhead)
		{
			ChainState c;
			Assert::IsTrue(ChooseMode(c, Pair(4, 5)) == Mode::Adopt, L"empty chain");
			c.havePrevious = true;
			c.previousSequence = 4;
			c.previousGeneration = 1;
			c.slotGeneration = 1;
			Assert::IsTrue(ChooseMode(c, Pair(4, 5)) == Mode::Chain, L"the chain holds `from`: upload `to` only");
			Assert::IsTrue(ChooseMode(c, Pair(4, 6)) == Mode::Chain, L"a gap pair from the held frame too");
			PairRequest broken = Pair(4, 5);
			broken.chainOk = false;
			Assert::IsTrue(ChooseMode(c, broken) == Mode::Adopt, L"an unseen break: never pair against the held frame");
			Assert::IsTrue(ChooseMode(c, Pair(2, 3)) == Mode::OffChain, L"the walk is ahead: separate textures");
			Assert::IsTrue(ChooseMode(c, Pair(7, 8)) == Mode::Adopt, L"the chain is behind: take it over");
			Assert::IsTrue(ChooseMode(c, Pair(4, 5, 2)) == Mode::Adopt, L"another generation");
			c.slotGeneration = 2;
			Assert::IsTrue(ChooseMode(c, Pair(4, 5)) == Mode::Adopt, L"the slot moved on");
			Assert::IsTrue(Pair(4, 5).Adjacent() && !Pair(4, 6).Adjacent());
		}

		TEST_METHOD(ARequestWithoutAProducerBudgetOrPairIsAnsweredAtOnceWithNoSideEffects)
		{
			MotionAhead::Producer producer;
			PairFuture f = producer.Request(Pair(1, 2));
			Assert::IsTrue(f.Ready() && f.Get().status == Status::Unavailable, L"not running");
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			Assert::IsTrue(producer.Request(Pair(1, 2, 1, 0.0)).Get().status == Status::NoBudget);
			Assert::IsTrue(producer.Request(Pair(2, 2)).Get().status == Status::Refused);
			Assert::IsTrue(producer.Request(Pair(3, 2)).Get().status == Status::Refused);
			producer.Stop();
			Assert::IsTrue(trace.Order().empty(), L"no engine work for any of them");
			const MotionAhead::Producer::RequestStats s = producer.GetRequestStats();
			Assert::AreEqual(3ull, s.requests, L"StartForTest cleared the earlier one");
			Assert::AreEqual(1ull, s.noBudget);
			Assert::AreEqual(2ull, s.refused);
			Assert::AreEqual(0ull, s.ready);
		}

		TEST_METHOD(ARequestIsTakenBeforeQueuedLookAheadJobs)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Assert::IsTrue(producer.StartForTest(Seam(trace, &gate, 1)));
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered(), L"the worker is busy on job 1");
			producer.Submit(2, 1, luma);
			producer.Submit(3, 1, luma);
			PairFuture f = producer.Request(Pair(10, 11));
			gate.Open();
			Assert::IsTrue(f.WaitFor(5000.0));
			const PairResult r = f.Get();
			Assert::IsTrue(r.status == Status::Ready);
			Assert::IsTrue(r.flow && r.flow->halfW == 11);
			Assert::IsTrue(WaitCalls(trace, 4));
			const std::vector<long long> order = trace.Order();
			Assert::AreEqual(1ll, order[0]);
			Assert::AreEqual(-11ll, order[1], L"the request ran before jobs 2 and 3");
			Assert::AreEqual(2ll, order[2]);
			producer.Stop();
		}

		TEST_METHOD(AnAbandonedOrLateRequestDoesNoEngineWork)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Assert::IsTrue(producer.StartForTest(Seam(trace, &gate, 1)));
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered());
			PairFuture abandoned = producer.Request(Pair(4, 5));
			PairFuture late = producer.Request(Pair(6, 7, 1, 0.001));
			abandoned.Abandon();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));   // the 1 us budget has passed
			gate.Open();
			Assert::IsTrue(abandoned.WaitFor(5000.0) && late.WaitFor(5000.0));
			Assert::IsTrue(abandoned.Get().status == Status::Late);
			Assert::IsTrue(late.Get().status == Status::Late);
			producer.Stop();
			for (long long v : trace.Order())
				Assert::IsTrue(v > 0, L"no request reached the engine");
			Assert::AreEqual(2ull, producer.GetRequestStats().late);
		}

		TEST_METHOD(StopAnswersEveryWaitingRequestUnavailable)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Assert::IsTrue(producer.StartForTest(Seam(trace, &gate, 1)));
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered());
			PairFuture f = producer.Request(Pair(4, 5));
			std::thread stopper([&producer] { producer.Stop(); });
			Assert::IsTrue(f.WaitFor(5000.0), L"answered before the worker is joined");
			gate.Open();
			stopper.join();
			Assert::IsTrue(f.Get().status == Status::Unavailable);
			Assert::IsTrue(producer.Request(Pair(5, 6)).Get().status == Status::Unavailable, L"stopped");
		}

		TEST_METHOD(TheQueueBoundRefusesTheOldestVisibly)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Assert::IsTrue(producer.StartForTest(Seam(trace, &gate, 1)));
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered());
			PairFuture a = producer.Request(Pair(4, 5));
			PairFuture b = producer.Request(Pair(5, 6));
			PairFuture c = producer.Request(Pair(6, 7));
			Assert::IsTrue(a.Ready() && a.Get().status == Status::Refused);
			gate.Open();
			Assert::IsTrue(b.WaitFor(5000.0) && c.WaitFor(5000.0));
			Assert::IsTrue(b.Get().status == Status::Ready && c.Get().status == Status::Ready);
			producer.Stop();
			Assert::AreEqual(1ull, producer.GetRequestStats().refused);
		}

		TEST_METHOD(AdjacentPairsArePublishedOnceAndGapPairsAreReturnedOnly)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			PairFuture f = producer.Request(Pair(4, 5));
			Assert::IsTrue(f.WaitFor(5000.0) && f.Get().status == Status::Ready);
			Assert::IsTrue(producer.Take(4, 5) != nullptr, L"published into P2");
			PairFuture again = producer.Request(Pair(4, 5));
			Assert::IsTrue(again.Ready(), L"compute once: answered at issue");
			const PairResult hit = again.Get();
			Assert::IsTrue(hit.status == Status::FromSlot && hit.flow && hit.flow->halfW == 5);
			PairFuture gap = producer.Request(Pair(5, 7));
			Assert::IsTrue(gap.WaitFor(5000.0));
			const PairResult g = gap.Get();
			Assert::IsTrue(g.status == Status::Ready && g.flow && g.flow->halfW == 7);
			Assert::IsTrue(producer.Take(5, 7) == nullptr, L"P2 stays adjacency-only");
			producer.Stop();
			Assert::AreEqual(size_t(2), trace.Order().size(), L"two engine runs, not three");
			const MotionAhead::Producer::RequestStats s = producer.GetRequestStats();
			Assert::AreEqual(2ull, s.ready);
			Assert::AreEqual(1ull, s.fromSlot);
		}

		TEST_METHOD(RequestsFollowTheChainAndLeaveAWalkAheadUntouched)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			AnalysisLumaSource luma;
			for (uint64_t s = 1; s <= 3; ++s) producer.Submit(s, 1, luma);
			Assert::IsTrue(WaitCalls(trace, 3));
			auto run = [&producer](PairRequest rq) {
				PairFuture f = producer.Request(rq);
				return f.WaitFor(5000.0) ? f.Get().status : Status::Pending;
			};
			Assert::IsTrue(run(Pair(3, 4)) == Status::Ready);   // Chain: the walk's last frame is `from`
			Assert::IsTrue(run(Pair(1, 3)) == Status::Ready);   // OffChain: the chain holds 4
			Assert::IsTrue(run(Pair(4, 5)) == Status::Ready);   // still Chain: the off-chain run left it
			Assert::IsTrue(run(Pair(9, 10)) == Status::Ready);  // Adopt: the chain is behind
			Assert::IsTrue(run(Pair(10, 11)) == Status::Ready); // Chain again after the adoption
			producer.Submit(12, 1, luma);                        // the walk pairs on from the request's frame
			Assert::IsTrue(WaitCalls(trace, 9));
			producer.Stop();
			const std::vector<Mode> modes = trace.Modes();
			Assert::AreEqual(size_t(5), modes.size());
			Assert::IsTrue(modes[0] == Mode::Chain);
			Assert::IsTrue(modes[1] == Mode::OffChain);
			Assert::IsTrue(modes[2] == Mode::Chain);
			Assert::IsTrue(modes[3] == Mode::Adopt);
			Assert::IsTrue(modes[4] == Mode::Chain);
			Assert::IsTrue(producer.Take(11, 12) != nullptr, L"the walk's next job pairs against the request's `to`");
			const MotionAhead::Producer::RequestStats s = producer.GetRequestStats();
			Assert::AreEqual(3ull, s.chainRuns);
			Assert::AreEqual(1ull, s.adoptRuns);
			Assert::AreEqual(1ull, s.offChainRuns);
		}

		TEST_METHOD(AFailedEngineRunAnswersFailedAndCountsAsAFailedJob)
		{
			MotionAhead::Producer producer;
			Trace trace;
			std::atomic<bool> fail{ true };
			Assert::IsTrue(producer.StartForTest(Seam(trace, nullptr, 0, &fail)));
			PairFuture f = producer.Request(Pair(4, 5));
			Assert::IsTrue(f.WaitFor(5000.0));
			Assert::IsTrue(f.Get().status == Status::Failed);
			Assert::IsTrue(producer.Take(4, 5) == nullptr);
			fail = false;
			PairFuture next = producer.Request(Pair(5, 6));
			Assert::IsTrue(next.WaitFor(5000.0) && next.Get().status == Status::Ready, L"one failure does not stop the producer");
			producer.Stop();
			Assert::AreEqual(1ull, producer.GetStats().failures);
			Assert::AreEqual(1ull, producer.GetRequestStats().failed);
		}

		// Audit MUST 1: the stream's generation only moves forward and the walk owns its changes.
		TEST_METHOD(AStaleGenerationRequestIsLateAndLeavesTheWalkIntact)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			AnalysisLumaSource luma;
			for (uint64_t s = 1; s <= 3; ++s) producer.Submit(s, 2, luma);
			Assert::IsTrue(WaitCalls(trace, 3));
			PairFuture f = producer.Request(Pair(2, 3, 1));   // issued before the stream moved to generation 2
			Assert::IsTrue(f.WaitFor(5000.0) && f.Get().status == Status::Late);
			producer.Submit(4, 2, luma);
			Assert::IsTrue(WaitCalls(trace, 4));
			producer.Stop();
			for (long long v : trace.Order())
				Assert::IsTrue(v > 0, L"no engine work for the stale request");
			Assert::IsTrue(producer.Take(2, 3) != nullptr, L"the walk's pairs stay stored");
			Assert::IsTrue(producer.Take(3, 4) != nullptr, L"the walk's chain was not reset");
			const MotionAhead::Producer::RequestStats s = producer.GetRequestStats();
			Assert::AreEqual(1ull, s.staleGeneration);
			Assert::AreEqual(1ull, s.late);
		}

		TEST_METHOD(ANewerGenerationRequestRunsOffTheChainWhileTheWalkIsPresent)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			AnalysisLumaSource luma;
			for (uint64_t s = 1; s <= 3; ++s) producer.Submit(s, 1, luma);
			Assert::IsTrue(WaitCalls(trace, 3));
			PairFuture f = producer.Request(Pair(5, 6, 2));
			Assert::IsTrue(f.WaitFor(5000.0));
			const PairResult r = f.Get();
			Assert::IsTrue(r.status == Status::Ready && r.flow && r.flow->halfW == 6);
			producer.Submit(4, 1, luma);
			Assert::IsTrue(WaitCalls(trace, 5));
			producer.Stop();
			const std::vector<Mode> modes = trace.Modes();
			Assert::AreEqual(size_t(1), modes.size());
			Assert::IsTrue(modes[0] == Mode::OffChain);
			Assert::IsTrue(producer.Take(5, 6) == nullptr, L"not published: the slot has not reached generation 2");
			Assert::IsTrue(producer.Take(2, 3) != nullptr, L"the slot kept generation 1");
			Assert::IsTrue(producer.Take(3, 4) != nullptr, L"the walk's chain continues");
			Assert::AreEqual(1ull, producer.GetRequestStats().forcedOffChain);
		}

		TEST_METHOD(ANewerGenerationRequestWithoutAWalkStartsTheGeneration)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			auto run = [&producer](PairRequest rq) {
				PairFuture f = producer.Request(rq);
				return f.WaitFor(5000.0) ? f.Get().status : Status::Pending;
			};
			Assert::IsTrue(run(Pair(1, 2, 1)) == Status::Ready);
			Assert::IsTrue(run(Pair(5, 6, 3)) == Status::Ready);
			Assert::IsTrue(run(Pair(2, 3, 1)) == Status::Late, L"the generation never moves backwards");
			producer.Stop();
			Assert::IsTrue(producer.Take(5, 6) != nullptr, L"published under the new generation");
			const MotionAhead::Producer::RequestStats s = producer.GetRequestStats();
			Assert::AreEqual(0ull, s.forcedOffChain);
			Assert::AreEqual(1ull, s.staleGeneration);
			Assert::AreEqual(size_t(2), trace.Order().size());
		}

		// Audit MUST 2: at most kMaxRequestStreak requests in a row while a job waits; dropped jobs are counted.
		TEST_METHOD(AStreakOfRequestsLetsAWaitingLookAheadJobRun)
		{
			Assert::AreEqual(2, static_cast<int>(MotionAhead::Producer::kMaxRequestStreak), L"the order below assumes a streak of two");
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Gate requestGate;
			auto seam = Seam(trace, &gate, 1);
			auto inner = seam->request;
			seam->request = [inner, &requestGate](const PairRequest& rq, Mode mode, MotionAhead::Flow& flow) {
				if (rq.to == 11) requestGate.Enter();
				return inner(rq, mode, flow);
			};
			Assert::IsTrue(producer.StartForTest(seam));
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered());
			producer.Submit(2, 1, luma);
			PairFuture a = producer.Request(Pair(10, 11));
			PairFuture b = producer.Request(Pair(11, 12));
			gate.Open();
			Assert::IsTrue(requestGate.WaitEntered(), L"the first request is running");
			PairFuture c = producer.Request(Pair(12, 13));   // waiting behind b while job 2 waits
			requestGate.Open();
			Assert::IsTrue(a.WaitFor(5000.0) && b.WaitFor(5000.0) && c.WaitFor(5000.0));
			Assert::IsTrue(WaitCalls(trace, 5));
			producer.Stop();
			const std::vector<long long> order = trace.Order();
			Assert::AreEqual(size_t(5), order.size());
			Assert::AreEqual(1ll, order[0]);
			Assert::AreEqual(-11ll, order[1]);
			Assert::AreEqual(-12ll, order[2]);
			Assert::AreEqual(2ll, order[3], L"the job runs after two requests although a third waits");
			Assert::AreEqual(-13ll, order[4]);
			Assert::AreEqual(1ull, producer.GetRequestStats().fairnessJobs);
		}

		TEST_METHOD(LookAheadJobsDroppedWhileRequestsAreServedAreCounted)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Assert::IsTrue(producer.StartForTest(Seam(trace, &gate, 1)));
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered());
			PairFuture f = producer.Request(Pair(10, 11));
			const uint64_t bound = 6;   // the default look-ahead bound (maxPendingJobs)
			for (uint64_t s = 2; s <= bound + 2; ++s) producer.Submit(s, 1, luma);   // one more than the bound
			gate.Open();
			Assert::IsTrue(f.WaitFor(5000.0));
			producer.Stop();
			Assert::AreEqual(1ull, producer.GetStats().dropped);
			Assert::AreEqual(1ull, producer.GetRequestStats().droppedBehindRequests);
		}

		// Re-check S1: a late job of an older generation never wipes a pair a no-walk request published under a newer one.
		TEST_METHOD(AnOlderGenerationJobAfterARequestStartedANewOneIsSkipped)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			PairFuture f = producer.Request(Pair(5, 6, 3));
			Assert::IsTrue(f.WaitFor(5000.0) && f.Get().status == Status::Ready);
			AnalysisLumaSource luma;
			producer.Submit(2, 2, luma);   // a job of generation 2 arriving late
			producer.Submit(7, 3, luma);   // the stream's generation 3 continues
			Assert::IsTrue(WaitCalls(trace, 2));
			producer.Stop();
			const std::vector<long long> order = trace.Order();
			Assert::AreEqual(size_t(2), order.size(), L"the older job did no work");
			Assert::AreEqual(7ll, order[1]);
			Assert::IsTrue(producer.Take(5, 6) != nullptr, L"the request's pair survives");
			Assert::AreEqual(1ull, producer.GetRequestStats().staleJobs);
		}

		// Re-check S3: drops after a request finished (the worker idle in between) are not counted as behind requests.
		TEST_METHOD(DropsAfterAServedRequestAreNotCountedAsBehindRequests)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Gate gate;
			Assert::IsTrue(producer.StartForTest(Seam(trace, &gate, 1)));
			PairFuture f = producer.Request(Pair(10, 11));
			Assert::IsTrue(f.WaitFor(5000.0) && f.Get().status == Status::Ready);
			AnalysisLumaSource luma;
			producer.Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered());
			for (uint64_t s = 2; s <= 8; ++s) producer.Submit(s, 1, luma);   // one more than the default bound of 6
			gate.Open();
			producer.Stop();
			Assert::AreEqual(1ull, producer.GetStats().dropped);
			Assert::AreEqual(0ull, producer.GetRequestStats().droppedBehindRequests);
		}

		// Audit SHOULD 4: a walk pair lost because an Adopt moved the chain past the walk is counted.
		TEST_METHOD(AnAdoptAheadOfTheWalkCountsTheWalkPairItCosts)
		{
			MotionAhead::Producer producer;
			Trace trace;
			Assert::IsTrue(producer.StartForTest(Seam(trace)));
			AnalysisLumaSource luma;
			for (uint64_t s = 1; s <= 3; ++s) producer.Submit(s, 1, luma);
			Assert::IsTrue(WaitCalls(trace, 3));
			PairFuture f = producer.Request(Pair(8, 9));
			Assert::IsTrue(f.WaitFor(5000.0) && f.Get().status == Status::Ready);
			producer.Submit(4, 1, luma);
			producer.Submit(5, 1, luma);
			Assert::IsTrue(WaitCalls(trace, 6));
			producer.Stop();
			Assert::IsTrue(trace.Modes()[0] == Mode::Adopt);
			Assert::IsTrue(producer.Take(3, 4) == nullptr, L"the chain held 9: no pair into 4");
			Assert::IsTrue(producer.Take(4, 5) != nullptr, L"the walk re-seeds");
			Assert::AreEqual(1ull, producer.GetRequestStats().adoptLostPairs);
		}

		// Audit MUST 3: the VectorEngine entry - every outcome is counted once and maps to one NoPairCause.
		TEST_METHOD(VectorEngineRequestPairAnswersWithoutAProducerOrBudget)
		{
			using FallbackCatalog::NoPairCause;
			VpMotion::VectorEngine engine;
			NoPairCause cause = NoPairCause::None;
			Assert::IsTrue(engine.AwaitPair(engine.RequestPair(Pair(1, 2)), 100.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable, L"no producer");
			auto idle = std::make_shared<MotionAhead::Producer>();   // published, never started
			engine.PublishProducer(idle);
			Assert::IsTrue(engine.AwaitPair(engine.RequestPair(Pair(1, 2)), 100.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable, L"not running");
			Assert::IsTrue(engine.AwaitPair(engine.RequestPair(Pair(1, 2, 1, 0.0)), 100.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandNoBudget);
			Assert::IsTrue(engine.AwaitPair(engine.RequestPair(Pair(1, 2, 1, -1.0)), 100.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandNoBudget);
			const VpMotion::VectorEngine::OnDemandCounters c = engine.OnDemandReads();
			Assert::AreEqual(4ull, c.requests);
			Assert::AreEqual(2ull, c.unavailable);
			Assert::AreEqual(2ull, c.noBudget);
			Assert::AreEqual(0ull, c.gated + c.late + c.served + c.fromSlot);
			Assert::IsTrue(VpMotion::VectorEngine::OnDemandStateOn(), L"the C1 gate is stubbed On");
		}

		TEST_METHOD(VectorEngineAwaitPairMapsEveryAnswer)
		{
			using FallbackCatalog::NoPairCause;
			VpMotion::VectorEngine engine;
			MotionAhead::FlowPtr flow = std::make_shared<MotionAhead::Flow>();
			NoPairCause cause = NoPairCause::OnDemandLate;
			Assert::IsTrue(engine.AwaitPair(PairFuture::Answered(Status::Ready, flow), 0.0, cause) == flow);
			Assert::IsTrue(cause == NoPairCause::None);
			cause = NoPairCause::OnDemandLate;
			Assert::IsTrue(engine.AwaitPair(PairFuture::Answered(Status::FromSlot, flow), 0.0, cause) == flow);
			Assert::IsTrue(cause == NoPairCause::None);
			Assert::IsTrue(engine.AwaitPair(PairFuture::Answered(Status::Ready), 0.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable, L"Ready without a flow");
			Assert::IsTrue(engine.AwaitPair(PairFuture::Answered(Status::Late), 0.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandLate);
			Assert::IsTrue(engine.AwaitPair(PairFuture::Answered(Status::NoBudget), 0.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandNoBudget);
			const Status misses[] = { Status::Failed, Status::Refused, Status::Unavailable };
			for (Status s : misses)
			{
				cause = NoPairCause::None;
				Assert::IsTrue(engine.AwaitPair(PairFuture::Answered(s), 0.0, cause) == nullptr);
				Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable);
			}
			cause = NoPairCause::None;
			Assert::IsTrue(engine.AwaitPair(PairFuture(), 0.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable, L"an invalid handle");
			const VpMotion::VectorEngine::OnDemandCounters c = engine.OnDemandReads();
			Assert::AreEqual(0ull, c.requests, L"none issued through RequestPair");
			Assert::AreEqual(1ull, c.served);
			Assert::AreEqual(1ull, c.fromSlot);
			Assert::AreEqual(1ull, c.late);
			Assert::AreEqual(1ull, c.noBudget);
			Assert::AreEqual(5ull, c.unavailable);
		}

		TEST_METHOD(VectorEngineAwaitPairAbandonsALateRequestAndServesThroughTheProducer)
		{
			using FallbackCatalog::NoPairCause;
			VpMotion::VectorEngine engine;
			auto producer = std::make_shared<MotionAhead::Producer>();
			Trace trace;
			Gate gate;
			std::atomic<bool> fail{ false };
			Assert::IsTrue(producer->StartForTest(Seam(trace, &gate, 1, &fail)));
			engine.PublishProducer(producer);
			AnalysisLumaSource luma;
			producer->Submit(1, 1, luma);
			Assert::IsTrue(gate.WaitEntered(), L"the worker is busy");
			NoPairCause cause = NoPairCause::None;
			PairFuture late = engine.RequestPair(Pair(4, 5));
			Assert::IsTrue(engine.AwaitPair(late, 20.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandLate);
			gate.Open();
			Assert::IsTrue(late.WaitFor(5000.0) && late.Get().status == Status::Late, L"the worker skipped it");
			MotionAhead::FlowPtr served = engine.AwaitPair(engine.RequestPair(Pair(5, 6)), 5000.0, cause);
			Assert::IsTrue(served != nullptr && served->halfW == 6 && cause == NoPairCause::None);
			Assert::IsTrue(engine.AwaitPair(engine.RequestPair(Pair(5, 6)), 5000.0, cause) != nullptr, L"from the slot");
			fail = true;
			Assert::IsTrue(engine.AwaitPair(engine.RequestPair(Pair(6, 7)), 5000.0, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable, L"a failed run");
			producer->Stop();
			for (long long v : trace.Order())
				Assert::IsTrue(v != -5, L"the abandoned request did no engine work");
			const VpMotion::VectorEngine::OnDemandCounters c = engine.OnDemandReads();
			Assert::AreEqual(4ull, c.requests);
			Assert::AreEqual(1ull, c.late);
			Assert::AreEqual(1ull, c.served);
			Assert::AreEqual(1ull, c.fromSlot);
			Assert::AreEqual(1ull, c.unavailable);
			Assert::AreEqual(c.requests, c.served + c.fromSlot + c.late + c.unavailable + c.noBudget, L"each counted once");
		}

		// W2-C5b commit 2 (hardware, NVIDIA): a request's Flow is byte-equal to the look-ahead job's for the same pair - Adopt, Chain
		// and OffChain alike - and all of it runs on the ONE engine session (NvOpticalFlow::Sessions: one Engine, no violation). The
		// off-chain textures are declared (VramUsers) only once an off-chain request was served.
		// Commit 4: the deadline is the source period minus the MEASURED render time (never a fixed number of ms).
		TEST_METHOD(TheBudgetIsTheSourcePeriodMinusTheMeasuredRenderTime)
		{
			RenderBudget b;
			Assert::AreEqual(0.0, b.BudgetMs(0.0), L"an unknown rate gives no budget");
			Assert::AreEqual(500.0 / 60.0, b.BudgetMs(60.0), 1e-9, L"half the period until measured");
			b.Note(-1.0);
			b.Note(std::numeric_limits<double>::quiet_NaN());
			Assert::IsFalse(b.Measured(), L"negative and NaN samples are ignored");
			b.Note(10.0);
			Assert::AreEqual(1000.0 / 60.0 - 10.0 - 100.0 / 60.0, b.BudgetMs(60.0), 1e-9);
			b.Note(20.0);
			Assert::AreEqual(11.25, b.RenderMs(), 1e-9, L"an EWMA of one eighth per sample");
			Assert::AreEqual(1000.0 / 30.0 - 11.25 - 100.0 / 30.0, b.BudgetMs(30.0), 1e-9, L"the period of the pair's rate");
			for (int i = 0; i < 100; ++i) b.Note(40.0);
			Assert::AreEqual(0.0, b.BudgetMs(60.0), L"never below zero: no budget, not issued");
			b.Reset();
			Assert::IsFalse(b.Measured());
			double r = -1.0;
			Assert::IsTrue(RenderSample(16.0, 2.0, 3.0, 60.0, r));
			Assert::AreEqual(11.0, r, 1e-9, L"the interval minus the dequeue wait and the on-demand wait");
			Assert::IsTrue(RenderSample(5.0, 4.0, 3.0, 60.0, r));
			Assert::AreEqual(0.0, r, L"clamped at zero");
			Assert::IsFalse(RenderSample(34.0, 0.0, 0.0, 60.0, r), L"more than two periods: a stall, no sample");
			Assert::IsFalse(RenderSample(16.0, 0.0, 0.0, 0.0, r), L"an unknown rate");
			Assert::IsFalse(RenderSample(0.0, 0.0, 0.0, 60.0, r));
		}

		// Commit 4: the DEFAULT keeps today's behaviour on the low-latency path (owner decision pending, answer 4).
		TEST_METHOD(TheSettingDefaultsToAutoWhichIsOffInLowLatency)
		{
			Assert::IsTrue(SettingOf("auto") == Setting::Auto);
			Assert::IsTrue(SettingOf("bogus") == Setting::Auto);
			Assert::IsTrue(SettingOf("on") == Setting::On);
			Assert::IsTrue(SettingOf("off") == Setting::Off);
			Assert::IsTrue(Wanted(Setting::Auto, false));
			Assert::IsFalse(Wanted(Setting::Auto, true), L"auto: not on the low-latency path");
			Assert::IsTrue(Wanted(Setting::On, true));
			Assert::IsFalse(Wanted(Setting::Off, false));
			Assert::AreEqual(std::string("auto"), std::string(SettingName(Setting::Auto)));
		}

		// Commit 4: the estimate's fill claims only its own pair; others are abandoned (counted) or kept (a later pair).
		TEST_METHOD(TheFillClaimsOnlyItsOwnPairAndCountsTheRest)
		{
			using FallbackCatalog::NoPairCause;
			VpMotion::VectorEngine engine;   // no producer: every request answers Unavailable at once
			NoPairCause cause = NoPairCause::None;
			engine.IssueOnDemand(Pair(5, 6));
			Assert::IsTrue(engine.OnDemandFill(nullptr, 5, 6, true, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::OnDemandUnavailable);
			Assert::IsTrue(engine.OnDemandLastCause() == NoPairCause::OnDemandUnavailable, L"reaches the no-pair cause");
			Assert::IsFalse(engine.OnDemandServed(5, 6));
			engine.IssueOnDemand(Pair(7, 8));
			Assert::IsTrue(engine.OnDemandFill(nullptr, 6, 7, true, cause) == nullptr);
			Assert::IsTrue(cause == NoPairCause::None, L"nothing asked for 6 -> 7");
			MotionAhead::FlowPtr ahead = std::make_shared<MotionAhead::Flow>();
			Assert::IsTrue(engine.OnDemandFill(ahead, 7, 8, true, cause) == ahead, L"the later request was kept; computed ahead wins");
			engine.IssueOnDemand(Pair(9, 10));
			engine.IssueOnDemand(Pair(10, 11));   // replaces 9 -> 10 unclaimed
			Assert::IsTrue(engine.OnDemandFill(nullptr, 10, 11, false, cause) == nullptr, L"refused by the vectors-valid state");
			Assert::IsTrue(cause == NoPairCause::None);
			const VpMotion::VectorEngine::OnDemandCounters c = engine.OnDemandReads();
			Assert::AreEqual(4ull, c.issued);
			Assert::AreEqual(1ull, c.aheadCovered);
			Assert::AreEqual(2ull, c.unclaimed);
			Assert::AreEqual(1ull, c.unavailable, L"only the claimed one was awaited");
		}

		// Commit 4: the dequeue hook issues the selected pair through the producer, and the fill serves it.
		TEST_METHOD(TheDequeueHookIssuesTheSelectedPairAndTheFillServesIt)
		{
			using FallbackCatalog::NoPairCause;
			FakeHost host;
			FakeHost::prepareValid = true;
			Dequeue(host, 5, 6);   // no producer: the cycle counts, nothing is issued
			Assert::AreEqual(0ull, host.vectorEngine.OnDemandReads().issued);
			Trace trace;
			auto producer = std::make_shared<MotionAhead::Producer>();
			Assert::IsTrue(producer->StartForTest(Seam(trace)));
			host.vectorEngine.PublishProducer(producer);
			host.lowLatencyQueueActive = true;
			Dequeue(host, 5, 6);
			Assert::AreEqual(0ull, host.vectorEngine.OnDemandReads().issued, L"auto: off in low latency (the default)");
			host.activeSettings.opticalFlowOnDemand = "off";
			host.lowLatencyQueueActive = false;
			Dequeue(host, 5, 6);
			Assert::AreEqual(0ull, host.vectorEngine.OnDemandReads().issued, L"off: never asked");
			host.activeSettings.opticalFlowOnDemand = "auto";
			FakeHost::prepareValid = false;
			Dequeue(host, 5, 6);
			Assert::AreEqual(0ull, host.vectorEngine.OnDemandReads().issued, L"a frame not ready yet");
			FakeHost::prepareValid = true;
			Dequeue(host, 6, 6);
			Assert::AreEqual(0ull, host.vectorEngine.OnDemandReads().issued, L"not a pair");
			Dequeue(host, 5, 7);   // a gap pair the walk never computes
			Assert::AreEqual(1ull, host.vectorEngine.OnDemandReads().issued);
			NoPairCause cause = NoPairCause::OnDemandLate;
			MotionAhead::FlowPtr flow = host.vectorEngine.OnDemandFill(nullptr, 5, 7, true, cause);
			Assert::IsTrue(flow != nullptr && flow->halfW == 7, L"served on the producer's session");
			Assert::IsTrue(cause == NoPairCause::None);
			Assert::IsTrue(host.vectorEngine.OnDemandServed(5, 7));
			for (int i = 0; i < 6; ++i) host.vectorEngine.NoteOnDemandCycle(60.0, 0.0);   // no estimate reads a pair
			Dequeue(host, 8, 10);
			const VpMotion::VectorEngine::OnDemandCounters c = host.vectorEngine.OnDemandReads();
			Assert::AreEqual(1ull, c.issued, L"no consumer: no request");
			Assert::AreEqual(1ull, c.idleSkips);
			Assert::AreEqual(1ull, c.served);
			producer->Stop();
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(RequestFlowsMatchTheLookAheadFlowsOnTheOneSession)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RequestFlowsMatchTheLookAheadFlowsOnTheOneSession)
		{
			// Real engine work only when asked for (VP_TEST_GPU=hardware: the GPU-QUIET protocol), never in a CPU run.
			char gpuMode[32] = {};
			size_t gpuModeLength = 0;
			const bool hardware = getenv_s(&gpuModeLength, gpuMode, sizeof(gpuMode), "VP_TEST_GPU") == 0 && std::string(gpuMode) == "hardware";
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "RequestFlowsMatchTheLookAheadFlowsOnTheOneSession",
					hardware, "VP_TEST_GPU=hardware is not set"))
				return;
			CComPtr<IDXGIFactory1> factory;
			CComPtr<IDXGIAdapter1> nvidia;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
				for (UINT i = 0;; ++i)
				{
					CComPtr<IDXGIAdapter1> adapter;
					if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
					DXGI_ADAPTER_DESC1 desc{};
					adapter->GetDesc1(&desc);
					if (desc.VendorId == 0x10DE) nvidia = adapter;
				}
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "RequestFlowsMatchTheLookAheadFlowsOnTheOneSession",
					nvidia != nullptr, "no NVIDIA adapter"))
				return;
			CComPtr<ID3D11Device> device;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nvidia, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));

			const int W = 1280, H = 720, dx = 6, dy = 2, BW = W + 96, BH = H + 96;
			std::vector<uint8_t> base(static_cast<size_t>(BW) * BH);
			uint32_t seed = 7u;
			for (uint8_t& v : base) { seed = seed * 1664525u + 1013904223u; v = static_cast<uint8_t>(seed >> 24); }
			auto source = [&](uint64_t index) {
				auto buffer = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(W) * H * 3 / 2, static_cast<uint16_t>(512 << 6));
				for (int y = 0; y < H; ++y)
					for (int x = 0; x < W; ++x)
					{
						int sx = x - static_cast<int>(index) * dx + 48, sy = y - static_cast<int>(index) * dy + 48;
						sx = sx < 0 ? 0 : sx >= BW ? BW - 1 : sx;
						sy = sy < 0 ? 0 : sy >= BH ? BH - 1 : sy;
						(*buffer)[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>((64 + base[static_cast<size_t>(sy) * BW + sx] * 3) << 6);
					}
				AnalysisLumaSource s;
				s.data = reinterpret_cast<const uint8_t*>(buffer->data());
				s.dataBytes = buffer->size() * 2;
				s.width = W;
				s.height = H;
				s.rowBytes = static_cast<size_t>(W) * 2;
				s.chromaRowBytes = static_cast<size_t>(W) * 2;
				s.format = AnalysisLumaFormat::P010;
				s.backingOwner = buffer;
				return s;
			};
			auto same = [](const MotionAhead::Flow& a, const MotionAhead::Flow& b) {
				return a.halfW == b.halfW && a.halfH == b.halfH && a.gridW == b.gridW && a.gridH == b.gridH && a.forward == b.forward &&
					a.backward == b.backward && a.forwardCost == b.forwardCost && a.backwardCost == b.backwardCost;
			};
			auto waitPairs = [](MotionAhead::Producer& p, unsigned long long n) {
				for (int i = 0; i < 1000 && p.GetStats().pairs < n; ++i)
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				return p.GetStats().pairs >= n;
			};
			NvOpticalFlow::Sessions::ResetForTest();
			VpSkeleton::VramUsers::Instance().ResetForTesting();
			const uint64_t generation = 3;

			// The reference: the look-ahead jobs (the Submit path) for (1,2) (2,3) (3,4).
			std::vector<MotionAhead::FlowPtr> reference(5);
			{
				MotionAhead::Producer walk;
				Assert::IsTrue(walk.Start(device, NvOpticalFlow::Preset::Medium), L"the producer starts");
				for (uint64_t s = 1; s <= 4; ++s) walk.Submit(s, generation, source(s));
				if (!waitPairs(walk, 3))
				{
					HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, "RequestFlowsMatchTheLookAheadFlowsOnTheOneSession", false,
						"the look-ahead producer made no pairs: " + walk.Error());
					return;
				}
				for (uint64_t s = 1; s <= 3; ++s)
				{
					reference[s] = walk.Take(s, s + 1);
					Assert::IsTrue(reference[s] != nullptr);
				}
				walk.Stop();
			}

			MotionAhead::Producer producer;
			Assert::IsTrue(producer.Start(device, NvOpticalFlow::Preset::Medium));
			auto ask = [&](uint64_t from, uint64_t to) {
				PairRequest rq = Pair(from, to, generation, 2000.0);
				rq.lumaFrom = source(from);
				rq.lumaTo = source(to);
				PairFuture f = producer.Request(rq);
				Assert::IsTrue(f.WaitFor(5000.0), L"answered");
				return f.Get();
			};
			// Requests only (low latency: no look-ahead jobs): (1,2) adopts the empty chain, (2,3) and (3,4) run on it.
			for (uint64_t s = 1; s <= 3; ++s)
			{
				const PairResult r = ask(s, s + 1);
				Assert::IsTrue(r.status == Status::Ready, L"the request was served");
				Assert::IsTrue(r.flow && same(*r.flow, *reference[s]), L"byte-equal to the look-ahead job's pair");
				Assert::IsTrue(producer.Take(s, s + 1) != nullptr, L"published into P2");
			}
			Assert::IsTrue(ask(2, 3).status == Status::FromSlot, L"compute once");
			// The first pair again, off the chain (the chain holds 4): the same bytes, from the request textures.
			const VpSkeleton::VramUsers::Snapshot before = VpSkeleton::VramUsers::Instance().Take();
			producer.Prune(3);   // (1,2) leaves the slot
			Assert::IsTrue(producer.Take(1, 2) == nullptr);
			const PairResult off = ask(1, 2);
			Assert::IsTrue(off.status == Status::Ready && off.flow && same(*off.flow, *reference[1]), L"off-chain: byte-equal too");
			const VpSkeleton::VramUsers::Snapshot after = VpSkeleton::VramUsers::Instance().Take();
			Assert::IsTrue(after.heldBytes > before.heldBytes, L"the request textures are declared once used");
			// And the chain was not disturbed: the next adjacent pair is still a chain run.
			const PairResult next = ask(4, 5);
			Assert::IsTrue(next.status == Status::Ready);
			const MotionAhead::Producer::RequestStats s = producer.GetRequestStats();
			Assert::AreEqual(1ull, s.adoptRuns);
			Assert::AreEqual(3ull, s.chainRuns);
			Assert::AreEqual(1ull, s.offChainRuns);
			// A cold (no temporal hints) (3,4) for the walk check below: off the chain, the chain holds 5.
			producer.Prune(5);   // pairs ending before 5 leave the slot
			Assert::IsTrue(producer.Take(3, 4) == nullptr);
			const PairResult cold = ask(3, 4);
			Assert::IsTrue(cold.status == Status::Ready && cold.flow);
			producer.Stop();

			// Audit SHOULD 10: the walk keeps submitting while a request runs off its chain; the walk's next pair is still right
			// (byte-equal to the cold pair: the off-chain run reset the hints, counted as hintsLost) and nothing else moved.
			{
				MotionAhead::Producer walk;
				Assert::IsTrue(walk.Start(device, NvOpticalFlow::Preset::Medium));
				for (uint64_t f = 1; f <= 3; ++f) walk.Submit(f, generation, source(f));
				Assert::IsTrue(waitPairs(walk, 2), L"the walk paired (1,2) (2,3)");
				PairRequest rq = Pair(1, 3, generation, 2000.0);
				rq.lumaFrom = source(1);
				rq.lumaTo = source(3);
				PairFuture gap = walk.Request(rq);
				walk.Submit(4, generation, source(4));   // queued behind the request
				Assert::IsTrue(gap.WaitFor(5000.0) && gap.Get().status == Status::Ready, L"the gap pair was served");
				Assert::IsTrue(waitPairs(walk, 3), L"the walk paired (3,4)");
				const MotionAhead::FlowPtr walked = walk.Take(3, 4);
				Assert::IsTrue(walked && same(*walked, *cold.flow), L"the walk's next pair is byte-equal to the cold pair");
				Assert::IsTrue(walk.Take(2, 3) != nullptr && walk.Take(1, 3) == nullptr, L"the walk's pairs stay; the gap pair is returned only");
				const MotionAhead::Producer::RequestStats w = walk.GetRequestStats();
				Assert::AreEqual(1ull, w.offChainRuns);
				Assert::AreEqual(0ull, w.chainRuns + w.adoptRuns + w.forcedOffChain);
				Assert::AreEqual(1ull, w.hintsLost);
				walk.Stop();
			}
			Assert::AreEqual(1, NvOpticalFlow::Sessions::Peak(NvOpticalFlow::SessionKind::Engine, 0), L"one engine session at a time (the peak: the walk stopped before the request producer started)");
			Assert::AreEqual<unsigned long long>(0, NvOpticalFlow::Sessions::Violations(), L"never a second session");
			char line[200];
			sprintf_s(line, "on-demand: %llu requests, run %.2f ms mean, queued %.2f ms last\n", s.requests,
				s.ready ? s.runMsSum / static_cast<double>(s.ready) : 0.0, s.lastQueuedMs);
			Logger::WriteMessage(line);
			VpSkeleton::VramUsers::Instance().ResetForTesting();
		}
	};
}
