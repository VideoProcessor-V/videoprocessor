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
#include <VP-V/modules/motion/estimation/VectorProducts.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <dxgi1_2.h>
#include <cstdio>
#include <string>

#include <atomic>
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

		// W2-C5b commit 2 (hardware, NVIDIA): a request's Flow is byte-equal to the look-ahead job's for the same pair - Adopt, Chain
		// and OffChain alike - and all of it runs on the ONE engine session (NvOpticalFlow::Sessions: one Engine, no violation). The
		// off-chain textures are declared (VramUsers) only once an off-chain request was served.
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
			producer.Stop();
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
