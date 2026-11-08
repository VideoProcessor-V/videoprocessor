// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// 70b G0 (render-thread GPU-work hook): the analysis lane's GPU live deferral (LivePending, RunDeferredLive),
// ReadOf / SourceOf, the fixed-step lane jobs (StepsFor) and the P12 begin gate, on the real SlotTable, Indicator,
// LaneRunner and the real CPU producers P1 + P4, with a recording fake dispatcher. CPU only, every test bounded.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuJobs.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AnalysisLaneGpuHookTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Indicator;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	// Records every GPU call in order; fails the products named in failLive.
	class RecordingDispatcher : public IGpuDispatcher {
	public:
		struct Call {
			ProductId id;
			uint64_t seq;
			uint32_t missing;
			bool live;
			uint32_t chunk;
		};
		bool Dispatch(const LaneJobContext& job, uint32_t chunk, uint32_t) override
		{
			calls.push_back(Call{ job.node->id, job.seq, 0u, false, chunk });
			return true;
		}
		bool DispatchLive(const Node& n, uint64_t seq, int, const Header&, uint32_t missing) override
		{
			calls.push_back(Call{ n.id, seq, missing, true, 0u });
			for (ProductId f : failLive)
				if (f == n.id)
					return false;
			return true;
		}
		size_t LiveCalls() const
		{
			size_t n = 0;
			for (const Call& c : calls)
				n += c.live ? 1u : 0u;
			return n;
		}
		std::vector<Call> calls;
		std::vector<ProductId> failLive;
	};

	struct Rig {
		Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts());
		std::vector<ConsumerDecl> consumers;
		GraphSettings gs;
		CardProfile card;
		TierInput tier;
		std::atomic<uint64_t> clock{ 0 };
		Indicator ind;
		AnalysisLane lane;
		VpLaneProducts::HalfLumaProducer p1;
		VpLaneProducts::SourceStatsProducer p4;
		RecordingDispatcher disp;
		std::vector<uint16_t> store;

		Rig()
			: ind(7, 256)
			, lane(&ind, [this]() { return clock.fetch_add(3) + 3; })
			, p1([this]() { return clock.fetch_add(3) + 3; })
			, p4(Dims{ kW, kH }, [this]() { return clock.fetch_add(3) + 3; })
		{
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP10GrainSplit);
			AddConsume(c, kP11RegionBlockStats);
			AddConsume(c, kP12LightLevel);
			consumers.push_back(c);
			gs.dims.width = kW;
			gs.dims.height = kH;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			gs.queueDepth = 4;
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			card.computeScale = 1.0;
			tier.framePeriodMs = 40.0;
			store.resize(8 * kW * kH);
			uint32_t x = 777u;
			for (size_t i = 0; i < store.size(); ++i) {
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 0xFFFFu);
			}
			lane.RegisterProducer(&p1);
			lane.RegisterProducer(&p4);
			lane.RegisterCpuLiveCompute(kP1HalfLuma,
				[](const Node&, const SourceFrameView* v, const std::array<const uint8_t*, kMaxInputs>&, uint8_t* dst, size_t n) {
					return v != nullptr && VpLaneProducts::ComputeHalfLuma(*v, dst, n);
				});
			lane.RegisterCpuLiveCompute(kP4SourceStats,
				[](const Node&, const SourceFrameView*, const std::array<const uint8_t*, kMaxInputs>& in, uint8_t* dst, size_t n) {
					return in[0] != nullptr && VpLaneProducts::ComputeSourceStats(in[0], Dims{ kW, kH }, dst, n);
				});
			lane.SetGpuDispatcher(&disp);
		}

		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % 8) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			return v;
		}

		void Configure()
		{
			LaneSettings s;
			s.mode = LaneSettings::Mode::On;
			lane.Configure(s, reg, consumers, gs, card, tier, 0, false);
			Assert::IsTrue(lane.Active(), L"the lane has a graph");
		}

		void Pump()
		{
			for (int i = 0; i < 20000; ++i)
				if (!lane.StepWorker(Backend::Ofa, 1000) && !lane.StepWorker(Backend::Cpu, 1000))
					return;
			Assert::Fail(L"pump did not settle");
		}

		std::vector<ProductFrameResult> Begin(uint64_t s)
		{
			lane.OnSourceFrame(View(s));
			lane.OnSourceFrame(View(s + 1));
			Pump();
			return lane.OnRenderStart(s, 4000);
		}

		static const ProductFrameResult& Res(const std::vector<ProductFrameResult>& r, ProductId id)
		{
			for (const ProductFrameResult& x : r)
				if (x.id == id)
					return x;
			Assert::Fail(L"product not in the results");
			return r[0];
		}
	};

	TEST_CLASS(AnalysisLaneGpuHookTests)
	{
	public:
		// The tap form: P10 / P11 read the analysis luma, not the source ring (P13); everything else is kept.
		TEST_METHOD(TapFormDropsOnlyTheSourceRingInput)
		{
			const Registry full = SkeletonProducts();
			const Registry tap = VpLaneProducts::TapFormProducts(full);
			const char* why = nullptr;
			Assert::IsTrue(tap.Validate(&why), L"the tap registry is valid");
			for (ProductId id : { kP10GrainSplit, kP11RegionBlockStats }) {
				const ProductDecl& a = full.Get(id);
				const ProductDecl& b = tap.Get(id);
				Assert::IsTrue(b.inputCount + 1 == a.inputCount, L"one input fewer");
				for (uint8_t k = 0; k < b.inputCount; ++k)
					Assert::IsTrue(b.inputs[k].product != kP13SourceRing, L"no source ring input");
			}
			Assert::IsTrue(tap.Get(kP12LightLevel).inputCount == full.Get(kP12LightLevel).inputCount, L"P12 unchanged");
			Assert::IsTrue(tap.Get(kP10cGrainSplitChroma).inputCount == full.Get(kP10cGrainSplitChroma).inputCount,
				L"P10c keeps its source ring input (it waits for M2 P10)");
		}

		// Default (not deferred): the dispatcher records at the seal, as before G0.
		TEST_METHOD(NotDeferredByDefault)
		{
			Rig r;
			r.Configure();
			Assert::IsFalse(r.lane.GpuLiveDeferred(), L"default off");
			r.Begin(0);
			Assert::IsTrue(r.disp.LiveCalls() == 3, L"P10, P11, P12 recorded at the seal");
			Assert::IsTrue(r.lane.LivePendingList().empty(), L"nothing pending");
		}

		// Deferred: nothing recorded at the seal; Entrance records P10 -> P11 -> P12, and reads move Pending -> Live.
		TEST_METHOD(DeferredLiveRunsAtEntranceInTopologicalOrder)
		{
			Rig r;
			r.lane.SetGpuLiveDeferred(true);
			r.Configure();
			const std::vector<ProductFrameResult> res = r.Begin(0);
			Assert::IsTrue(r.disp.calls.empty(), L"no GPU call before the render lock");
			Assert::IsTrue(r.lane.LivePendingList().size() == 3, L"three pending live fills");
			Assert::IsTrue(Rig::Res(res, kP10GrainSplit).liveFilled, L"P10 live buffer reserved");
			Assert::IsTrue(r.lane.ReadOf(kP10GrainSplit, 0).state == AnalysisLane::ProductRead::Pending, L"P10 pending");
			Assert::IsTrue(r.lane.ReadOf(kP4SourceStats, 0).cpu != nullptr, L"P4 bytes readable (CPU product)");
			SourceFrameView v;
			Assert::IsTrue(r.lane.SourceOf(0, &v) && v.width == kW, L"the source view of the frame");

			const AnalysisLane::DeferredReport rep = r.lane.RunDeferredLive(0);
			Assert::IsTrue(rep.run == 3 && rep.failed == 0 && rep.stale == 0, L"three run");
			Assert::IsTrue(r.disp.calls.size() == 3 && r.disp.calls[0].id == kP10GrainSplit &&
				r.disp.calls[1].id == kP11RegionBlockStats && r.disp.calls[2].id == kP12LightLevel, L"topological order");
			Assert::IsTrue(r.disp.calls[1].missing == 0, L"P11 had P10");
			Assert::IsTrue(r.lane.ReadOf(kP10GrainSplit, 0).state == AnalysisLane::ProductRead::Live, L"P10 live");
			Assert::IsTrue(r.lane.LivePendingList().empty(), L"nothing left");
			Assert::IsTrue(r.lane.RunDeferredLive(0).run == 0, L"idempotent");
		}

		// A failed P10 dispatch: P11 runs in its declared missing-input form (bit of P10 set); P10's read says Failed.
		TEST_METHOD(FailedDeferredDispatchCascadesVisibly)
		{
			Rig r;
			r.lane.SetGpuLiveDeferred(true);
			r.disp.failLive.push_back(kP10GrainSplit);
			r.Configure();
			r.Begin(0);
			const AnalysisLane::DeferredReport rep = r.lane.RunDeferredLive(0);
			Assert::IsTrue(rep.failed == 1 && rep.run == 2, L"P10 failed, P11 and P12 ran");
			Assert::IsTrue(r.lane.ReadOf(kP10GrainSplit, 0).state == AnalysisLane::ProductRead::Failed, L"P10 read Failed");
			Assert::IsTrue(r.lane.ReadOf(kP10GrainSplit, 0).info.kind == ReadInfo::None, L"no content to read");
			const Node* p11 = r.lane.State()->table->NodeOf(kP11RegionBlockStats);
			uint32_t bit = 0;
			for (size_t k = 0; k < p11->inputCount; ++k)
				if (p11->inputs[k].product == kP10GrainSplit)
					bit = 1u << k;
			Assert::IsTrue(bit != 0 && (r.disp.calls[1].missing & bit) != 0, L"P11 told P10 is missing");
			Assert::IsTrue(r.lane.DeferredTotals().failed == 1, L"counted");
		}

		// Entrance never reached: the next seal marks the entries stale (counted) and their reads Failed.
		TEST_METHOD(StalePendingIsCountedAtTheNextSeal)
		{
			Rig r;
			r.lane.SetGpuLiveDeferred(true);
			r.Configure();
			r.Begin(0);
			r.lane.OnFrameRetired(0, 0);
			r.Begin(1);
			Assert::IsTrue(r.lane.DeferredTotals().stale == 3, L"three stale");
			Assert::IsTrue(r.lane.ReadOf(kP10GrainSplit, 0).state == AnalysisLane::ProductRead::Failed, L"frame 0 failed");
			Assert::IsTrue(r.lane.ReadOf(kP10GrainSplit, 1).state == AnalysisLane::ProductRead::Pending, L"frame 1 pending");
			Assert::IsTrue(r.lane.RunDeferredLive(0).stale == 3, L"a wrong frame's Entrance runs nothing");
			Assert::IsTrue(r.disp.calls.empty(), L"nothing dispatched");
		}

		// StepsFor: P10 = tap bands + extend + 5 splitter passes; P11 / P12 one pass; others not fixed-step.
		TEST_METHOD(StepsForCountsPasses)
		{
			using VpLaneProducts::StepsFor;
			using VpLaneProducts::TapBands;
			Assert::IsTrue(TapBands(Dims{ 1920, 1080 }) == 1 && StepsFor(kP10GrainSplit, Dims{ 1920, 1080 }) == 7, L"1080p");
			Assert::IsTrue(TapBands(Dims{ 3840, 2160 }) == 4 && StepsFor(kP10GrainSplit, Dims{ 3840, 2160 }) == 10, L"4K");
			Assert::IsTrue(StepsFor(kP11RegionBlockStats, Dims{ 3840, 2160 }) == 1, L"P11");
			Assert::IsTrue(StepsFor(kP12LightLevel, Dims{ 3840, 2160 }) == 1, L"P12");
			Assert::IsTrue(StepsFor(kP3AnalysisField, Dims{ 3840, 2160 }) == 0 && StepsFor(kP10GrainSplit, Dims{ 0, 0 }) == 0,
				L"not fixed-step / no size");
			uint32_t covered = 0;
			const Dims d{ 3840, 2160 };
			for (uint32_t b = 0; b < TapBands(d); ++b)
				covered += (std::min)(VpLaneProducts::TapRowsPerBand(d), d.height - b * VpLaneProducts::TapRowsPerBand(d));
			Assert::IsTrue(covered == d.height, L"bands cover every row once");
		}

		// A fixed-step job runs exactly its steps, one chunk each, whatever the cap; a closed gate refuses Begin.
		TEST_METHOD(FixedStepJobAndBeginGate)
		{
			RecordingDispatcher d;
			VpLaneProducts::GpuComputeLaneJob job(VpLaneProducts::GpuComputeLaneJob::FixedStepsTag(), kP10GrainSplit, &d,
				[]() { return 50.0; }, 7u);
			Node n;
			n.id = kP10GrainSplit;
			LaneJobContext ctx;
			ctx.node = &n;
			ctx.seq = 5;
			Assert::IsTrue(job.Begin(ctx) && job.Plan().chunkCount == 7, L"7 chunks (no budget split)");
			int chunks = 0;
			ChunkResult cr = ChunkResult::More;
			while (cr == ChunkResult::More && chunks < 100) {
				cr = job.RunChunk(ctx, 1);
				++chunks;
			}
			Assert::IsTrue(cr == ChunkResult::Done && chunks == 7 && d.calls.size() == 7 && d.calls[6].chunk == 6, L"in order");
			bool open = false;
			job.SetBeginGate([&open](const LaneJobContext&) { return open; });
			Assert::IsFalse(job.Begin(ctx), L"gate closed: Begin false");
			open = true;
			Assert::IsTrue(job.Begin(ctx) && job.NextChunk() == 0, L"gate open: job restarts");
		}

		// The P12 begin gate (70b section 3): lane P12(N) only after P12(N-1) is readable, through ReadOf on the render thread.
		TEST_METHOD(P12GateWaitsForThePreviousFrame)
		{
			Rig r;
			r.Configure();
			auto gate = [&r](uint64_t seq) {
				if (seq == 0)
					return true;   // no previous frame: the product resets (ll_reset)
				const AnalysisLane::ProductRead p = r.lane.ReadOf(kP12LightLevel, seq - 1);
				return p.state == AnalysisLane::ProductRead::Ring || p.state == AnalysisLane::ProductRead::Live;
			};
			Assert::IsFalse(gate(3), L"P12(2) never made: wait");
			r.Begin(0);
			Assert::IsTrue(gate(1), L"P12(0) live: P12(1) may run");
			r.lane.SetGpuLiveDeferred(true);
			r.lane.OnFrameRetired(0, 0);
			r.Begin(1);
			Assert::IsFalse(gate(2), L"P12(1) still pending: wait");
			r.lane.RunDeferredLive(1);
			Assert::IsTrue(gate(2), L"P12(1) recorded: go");
		}
	};
}
