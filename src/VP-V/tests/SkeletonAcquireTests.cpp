// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// B1 (refs/126 s1.2): GET-OR-COMPUTE-AND-PUBLISH (src/VP-V/core/SkeletonAcquire.h, SkeletonAcquireLane.h).
//   1. computed once with two readers (sequential and concurrent): the owner fill runs once, the second reader reuses it;
//   2. a variant published by the owner and reused; an undeclared variant axis is refused (no private copy);
//   3. lifetime: memo spots released at the next seal, a released frame is refused (never recomputed), budget, new epoch;
//   4. no GPU dispatcher: the GPU form is Unavailable(no GPU dispatcher), counted, on the status and the Fallbacks heading;
//      no owner fill / owner conflict are visible too;
//   5. identity: lane products read through Acquire are the lane's own bytes (bit-identical, same buffer), and a frame an
//      Acquire reader published before the seal is reused by the seal (the same law, never computed twice).
// CPU only, single process, every test bounded.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/SkeletonAcquireLane.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonAcquireTestsNs
{
	using namespace VpSkeleton;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
	bool AnyLine(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (Has(l, part))
				return true;
		return false;
	}

	// An owner CPU fill that counts its calls and writes a pattern of (seq, variant scale).
	OwnerFill CountingFill(const char* owner, std::atomic<int>* calls, uint32_t axes = 0, int sleepMs = 0, bool decline = false)
	{
		OwnerFill f;
		f.owner = owner;
		f.variantAxes = axes;
		f.cpu = [calls, sleepMs, decline](const OwnerFillRequest& r, SkeletonMemo&, std::vector<uint8_t>& out) {
			calls->fetch_add(1);
			if (sleepMs > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
			if (decline)
				return false;
			out.assign(256, 0);
			for (size_t i = 0; i < out.size(); ++i)
				out[i] = uint8_t(i + r.seq * 7u + r.variant.scale * 31u);
			return true;
		};
		return f;
	}

	// A minimal lane rig on the real lane (FirstProducts, CPU producers P1 + P4, one consumer of P4).
	struct LaneRig {
		Registry reg = FirstProducts();
		std::vector<ConsumerDecl> consumers;
		GraphSettings gs;
		CardProfile card;
		TierInput tier;
		std::atomic<uint64_t> clock{ 0 };
		VpIndicator::Indicator ind;
		AnalysisLane lane;
		VpLaneProducts::HalfLumaProducer p1;
		VpLaneProducts::SourceStatsProducer p4;
		std::vector<uint16_t> store;
		size_t frames = 64;

		LaneRig()
			: ind(7, 256)
			, lane(&ind, [this]() { return clock.fetch_add(3) + 3; })
			, p1([this]() { return clock.fetch_add(3) + 3; })
			, p4(Dims{ kW, kH }, [this]() { return clock.fetch_add(3) + 3; })
		{
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP4SourceStats);
			consumers.push_back(c);
			gs.dims.width = kW;
			gs.dims.height = kH;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			gs.queueDepth = 4;
			gs.lookAheadWalk = 0;
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			card.computeScale = 1.0;
			tier.framePeriodMs = 40.0;
			store.resize(frames * kW * kH);
			uint32_t x = 4242u;
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
		}
		bool Configure()
		{
			LaneSettings s;
			s.mode = LaneSettings::Mode::On;
			return lane.Configure(s, reg, consumers, gs, card, tier, 0, false) != 0;
		}
		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % frames) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			return v;
		}
		void Pump()
		{
			for (int i = 0; i < 20000; ++i)
				if (!lane.StepWorker(Backend::Cpu, 1000))
					return;
			Assert::Fail(L"pump did not settle");
		}
		std::vector<uint8_t> SlotBytes(ProductId id, uint64_t s) const
		{
			const AnalysisLane::ProductRead r = lane.ReadOf(id, s);
			return r.cpu ? std::vector<uint8_t>(r.cpu, r.cpu + r.cpuBytes) : std::vector<uint8_t>();
		}
		std::vector<uint8_t> ExpectedHalf(uint64_t s) const
		{
			std::vector<uint8_t> out(VpLaneProducts::HalfLumaBytes(Dims{ kW, kH }));
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(View(s), out.data(), out.size()), L"reference half luma");
			return out;
		}
		std::vector<uint8_t> ExpectedStats(uint64_t s) const
		{
			const std::vector<uint8_t> half = ExpectedHalf(s);
			std::vector<uint8_t> out(size_t(VpLaneProducts::kStatsBytes), 0);
			Assert::IsTrue(VpLaneProducts::ComputeSourceStats(half.data(), Dims{ kW, kH }, out.data(), out.size()), L"reference stats");
			return out;
		}
	};

	TEST_CLASS(SkeletonAcquireTests)
	{
	public:
		// 1a. Two readers of one frame: the owner computes once and publishes; the second reader reuses the same spot.
		TEST_METHOD(ComputedOnceWithTwoReaders)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			Assert::IsTrue(m.RegisterOwnerFill(kP10GrainSplit, CountingFill("input/source_analysis", &calls)), L"owner registered");
			const AcquireResult a = m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);
			const AcquireResult b = m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.source == AcquireSource::Memo, L"first reader: filled at read");
			Assert::IsTrue(b.status == AcquireStatus::Reused && b.source == AcquireSource::Memo, L"second reader: reused");
			Assert::IsTrue(a.cpu == b.cpu && a.cpuBytes == 256u && b.cpuBytes == 256u, L"the same published spot");
			Assert::AreEqual(1, calls.load(), L"owner fill ran once");
			const AcquireCounters c = m.CountersOf(kP10GrainSplit);
			Assert::IsTrue(c.ownerFillCalls == 1 && c.filledAtRead == 1 && c.reusedMemo == 1 && c.Unavailable() == 0, L"counted");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"another frame fills");
			Assert::AreEqual(2, calls.load(), L"once per frame");
			Assert::IsTrue(AnyLine(m.StatusLines(), "1 filled at read") || AnyLine(m.StatusLines(), "2 filled at read"), L"on the status");
		}

		// 1b. Concurrent readers: the second waits for the fill in flight instead of computing a copy.
		TEST_METHOD(ConcurrentReadersWaitForTheOneFill)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, CountingFill("input/source_analysis", &calls, 0, 40));
			AcquireResult ra, rb;
			std::thread ta([&]() { ra = m.Acquire(kP10GrainSplit, 9, AcquireForm::Cpu); });
			std::thread tb([&]() { rb = m.Acquire(kP10GrainSplit, 9, AcquireForm::Cpu); });
			ta.join();
			tb.join();
			Assert::AreEqual(1, calls.load(), L"one fill for two concurrent readers");
			Assert::IsTrue(ra.Ok() && rb.Ok() && ra.cpu == rb.cpu, L"both read the one spot");
			Assert::IsTrue((ra.status == AcquireStatus::FilledAtRead) != (rb.status == AcquireStatus::FilledAtRead), L"exactly one filled");
		}

		// 2. G4: the owner publishes a variant under its key; readers of that variant reuse it; an undeclared axis is refused.
		TEST_METHOD(VariantPublishedByTheOwnerAndReused)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, CountingFill("input/source_analysis", &calls, VariantKey::kAxisScale | VariantKey::kAxisSize));
			VariantKey half;
			half.scale = 2;
			half.width = kW / 2;
			half.height = kH / 2;
			const AcquireResult d = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu);
			const AcquireResult v1 = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, half);
			const AcquireResult v2 = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, half);
			Assert::IsTrue(d.status == AcquireStatus::FilledAtRead && v1.status == AcquireStatus::FilledAtRead, L"default and variant are two spots");
			Assert::IsTrue(v2.status == AcquireStatus::Reused && v2.cpu == v1.cpu, L"the variant is reused");
			Assert::IsTrue(d.cpu != v1.cpu && d.cpu[1] != v1.cpu[1], L"the variant is the owner's variant, not the default");
			Assert::AreEqual(2, calls.load(), L"one fill per (frame, variant)");
			VariantKey sigma;
			sigma.sigmaMilli = 1500;
			const AcquireResult u = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, sigma);
			Assert::IsTrue(u.status == AcquireStatus::Unavailable && u.cause == AcquireCause::VariantUndeclared, L"undeclared axis refused");
			Assert::AreEqual(2, calls.load(), L"the owner is not asked for an axis it does not serve");
			Assert::IsTrue(Has(m.LastCause(kP10GrainSplit), "variant not served"), L"visible cause");
			Assert::IsTrue(Has(half.Text(), "scale=2") && VariantKey().Text() == "default", L"variant text");
		}

		// 3. Lifetime: per-frame memo, released at the next seal; a released frame is refused, never recomputed; budget; epoch.
		TEST_METHOD(LifetimeEvictionAndBudget)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, CountingFill("input/source_analysis", &calls));
			m.OnSeal(5);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"frame 5 filled");
			Assert::IsTrue(m.MemoCount() == 1 && m.MemoBytes() == 256u, L"one spot, its bytes counted");
			m.OnSeal(6);
			Assert::IsTrue(m.MemoCount() == 0 && m.MemoBytes() == 0u, L"released at the next seal");
			Assert::IsTrue(m.CountersOf(kP10GrainSplit).released == 1, L"release counted");
			const AcquireResult late = m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu);
			Assert::IsTrue(late.status == AcquireStatus::Unavailable && late.cause == AcquireCause::FrameReleased, L"released frame refused");
			Assert::AreEqual(1, calls.load(), L"never recomputed");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 6, AcquireForm::Cpu).Ok(), L"the current frame fills");

			// keepFrames 1: the previous frame stays readable for one more seal.
			m.SetKeepFrames(1);
			m.OnSeal(7);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 6, AcquireForm::Cpu).status == AcquireStatus::Reused, L"kept one frame");
			m.OnSeal(8);
			Assert::IsTrue(m.MemoCount() == 0, L"then released");

			// Budget: a result that does not fit the grant declines visibly (OverBudget), nothing is kept.
			m.SetKeepFrames(0);
			m.SetBudgetBytes(100);
			const AcquireResult big = m.Acquire(kP10GrainSplit, 8, AcquireForm::Cpu);
			Assert::IsTrue(big.status == AcquireStatus::Unavailable && big.cause == AcquireCause::OverBudget, L"over budget");
			Assert::IsTrue(m.MemoBytes() == 0u, L"nothing budgeted");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 8, AcquireForm::Cpu).cause == AcquireCause::OverBudget, L"the frame's decline is kept");
			const int callsAfterBudget = calls.load();

			// A new epoch (frame numbers go back): the old numbering is forgotten, frames fill again.
			m.SetBudgetBytes(0);
			m.OnSeal(2);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 2, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"new epoch fills");
			Assert::AreEqual(callsAfterBudget + 1, calls.load());
		}

		// 4. No GPU dispatcher (the SkeletonModule wiring that declines every GPU live fill): visible, counted, on the Fallbacks
		//    heading; recovers when the dispatcher exists. No owner / a second owner / an owner decline are visible too.
		TEST_METHOD(NoDispatcherAndOtherCausesAreVisible)
		{
			SkeletonMemo m;
			m.SetNames(nullptr);
			FallbackStatus::Registry fb;
			m.SetFallbackSink(&fb);
			int gpuCalls = 0;
			OwnerFill g;
			g.owner = "input/source_analysis";
			g.gpu = [&gpuCalls](const OwnerFillRequest&, SkeletonMemo&, GpuMemoHandle& out) {
				++gpuCalls;
				static int texture = 0;
				out.resource = &texture;
				out.bytes = 1024;
				return true;
			};
			m.RegisterOwnerFill(kP1HalfLuma, g);
			const AcquireResult a = m.Acquire(kP1HalfLuma, 1, AcquireForm::Gpu);
			Assert::IsTrue(a.status == AcquireStatus::Unavailable && a.cause == AcquireCause::NoGpuDispatcher, L"no dispatcher");
			Assert::AreEqual(0, gpuCalls, L"no GPU work without a dispatcher");
			Assert::IsTrue(m.CountersOf(kP1HalfLuma).unavailable[size_t(AcquireCause::NoGpuDispatcher)] == 1, L"counted");
			Assert::IsTrue(AnyLine(m.StatusLines(), "no GPU dispatcher 1"), L"on the status lines");
			Assert::IsTrue(Has(fb.Heading(), "skeleton acquire product 1") && Has(fb.Heading(), "no GPU dispatcher"), L"Fallbacks heading");
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 1, AcquireForm::Cpu).cause == AcquireCause::NoOwnerFill, L"no CPU owner fill: visible");

			m.SetGpuDispatcherPresent(true);
			const AcquireResult b = m.Acquire(kP1HalfLuma, 2, AcquireForm::Gpu);
			Assert::IsTrue(b.status == AcquireStatus::FilledAtRead && b.gpu != nullptr && b.gpu->bytes == 1024u, L"GPU form published");
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 2, AcquireForm::Gpu).gpu == b.gpu, L"GPU form reused");
			Assert::AreEqual(1, gpuCalls, L"one GPU fill");
			Assert::IsTrue(!Has(fb.Heading(), "skeleton acquire product 1"), L"released after the recovery");

			// One owner per product: a second unit is refused and shown.
			std::atomic<int> calls{ 0 };
			Assert::IsFalse(m.RegisterOwnerFill(kP1HalfLuma, CountingFill("motion/estimation", &calls)), L"second owner refused");
			Assert::IsTrue(m.OwnerConflicts() == 1 && m.OwnerOf(kP1HalfLuma) == "input/source_analysis", L"owner kept");
			Assert::IsTrue(AnyLine(m.StatusLines(), "owner conflicts 1"), L"conflict on the status");

			// An owner decline is a visible cause, asked once per frame.
			std::atomic<int> declines{ 0 };
			m.RegisterOwnerFill(kP11RegionBlockStats, CountingFill("input/source_analysis", &declines, 0, 0, true));
			Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 3, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined, L"declined");
			Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 3, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined, L"again");
			Assert::AreEqual(1, declines.load(), L"the owner is asked once per frame");
		}

		// 5a. Identity: lane products read through Acquire are the lane's own buffers (bit-identical by construction).
		TEST_METHOD(LaneProductsViaAcquireAreBitIdentical)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			SkeletonMemo m;
			m.SetNames(&r.reg);
			BindLaneToMemo(r.lane, m);
			Assert::IsTrue(m.OwnerOf(kP1HalfLuma) == kLaneOwnerUnit && m.OwnerOf(kP4SourceStats) == kLaneOwnerUnit, L"lane computes are the owner fills");
			r.lane.OnSourceFrame(r.View(0));
			for (uint64_t s = 0; s < 40; ++s) {
				r.lane.OnSourceFrame(r.View(s + 1));
				r.Pump();
				r.lane.OnRenderStart(s, 4000);
				m.OnSeal(s);
				for (ProductId id : { kP1HalfLuma, kP4SourceStats }) {
					const AcquireResult a = m.Acquire(id, s, AcquireForm::Cpu);
					const AnalysisLane::ProductRead lr = r.lane.ReadOf(id, s);
					Assert::IsTrue(a.status == AcquireStatus::Reused && a.source == AcquireSource::LaneSlot, L"reused from the lane slot");
					Assert::IsTrue(a.cpu == lr.cpu && a.cpuBytes == lr.cpuBytes, L"the lane's own buffer");
				}
				const AcquireResult h = m.Acquire(kP1HalfLuma, s, AcquireForm::Cpu);
				const AcquireResult st = m.Acquire(kP4SourceStats, s, AcquireForm::Cpu);
				Assert::IsTrue(std::vector<uint8_t>(h.cpu, h.cpu + h.cpuBytes) == r.ExpectedHalf(s), L"P1 = the one-shot law");
				Assert::IsTrue(std::vector<uint8_t>(st.cpu, st.cpu + st.cpuBytes) == r.ExpectedStats(s), L"P4 = the one-shot law");
				r.lane.OnFrameRetired(s, 0);
				r.lane.OnFrameTiming(s, 10.0, 1.0, false);
			}
			const AcquireCounters c = m.CountersOf(kP4SourceStats);
			Assert::IsTrue(c.ownerFillCalls == 0 && c.reusedLane == 80u, L"nothing recomputed: 2 reads x 40 frames from the lane");
			Assert::IsTrue(m.MemoCount() == 0, L"no memo spot for products the lane made");
			// A GPU form of a CPU lane product is not the lane's form: no owner publishes it (visible, never a private copy).
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 39, AcquireForm::Gpu).cause == AcquireCause::NoOwnerFill, L"GPU form of P1 not served yet");
		}

		// 5b. Get-or-compute before the seal: an Acquire of a frame the lane has not made runs the lane's own law (P4 through P1 via the
		//     same chain), publishes it, and the seal then REUSES the publication: bit-identical and never computed twice.
		TEST_METHOD(PublishedBeforeTheSealIsReusedByTheSeal)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			SkeletonMemo m;
			BindLaneToMemo(r.lane, m);
			r.lane.OnSourceFrame(r.View(0));   // the lane has not run (no pump)
			const AcquireResult a = m.Acquire(kP4SourceStats, 0, AcquireForm::Cpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.source == AcquireSource::Memo, L"filled at read by the owner");
			Assert::IsTrue(std::vector<uint8_t>(a.cpu, a.cpu + a.cpuBytes) == r.ExpectedStats(0), L"the lane's law");
			Assert::IsTrue(m.CountersOf(kP1HalfLuma).filledAtRead == 1, L"its input P1 got-or-computed through the same path");
			const std::vector<ProductFrameResult>& res = r.lane.OnRenderStart(0, 4000);
			bool p1Live = false, p4Live = false;
			for (const ProductFrameResult& x : res) {
				p1Live = p1Live || (x.id == kP1HalfLuma && x.liveFilled);
				p4Live = p4Live || (x.id == kP4SourceStats && x.liveFilled);
			}
			Assert::IsTrue(p1Live && p4Live, L"the seal live-filled both");
			Assert::IsTrue(r.lane.SealReusedPublished() == 2u, L"from the publication, not recomputed");
			Assert::IsTrue(r.SlotBytes(kP4SourceStats, 0) == r.ExpectedStats(0) && r.SlotBytes(kP1HalfLuma, 0) == r.ExpectedHalf(0),
				L"slot bytes bit-identical to the law");
			const AcquireResult after = m.Acquire(kP4SourceStats, 0, AcquireForm::Cpu);
			Assert::IsTrue(after.status == AcquireStatus::Reused && after.source == AcquireSource::LaneSlot, L"now read from the slot");
			Assert::IsTrue(m.CountersOf(kP4SourceStats).ownerFillCalls == 1, L"computed once in all");
		}
	};
}
