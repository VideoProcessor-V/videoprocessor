// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S3-S7 (src/VP-V/core/AnalysisLane.h): the analysis lane facade on the real SlotTable,
// Indicator, LaneRunner, Regulator, GapScheduler and tier plan, with the real CPU producers P1 + P4 and
// a fake optical-flow P2. Off is inert; shadow flags; 300 frames single-threaded (accepted and
// bit-identical to the live form); regulator drop; FULL floor and recovery; the N1 bounded wait; a
// graph rebuild under running worker threads; worker start/stop; the gap work adapter. CPU only, every
// test bounded.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/FrameSeal.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuJobs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AnalysisLaneTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Indicator;
	using VpResync::Cause;

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

	// Fake optical-flow producer: two chunks, then its (immediately passed) fence. No bytes.
	class FakeOfa : public ILaneProducer {
	public:
		ProductId Product() const override { return kP2VectorPair; }
		bool Begin(const LaneJobContext&) override
		{
			chunks_ = 0;
			signal_ = ++counter_;
			completed_ = 0;
			return true;
		}
		ChunkResult RunChunk(const LaneJobContext&, uint32_t) override
		{
			if (++chunks_ < 2)
				return ChunkResult::More;
			completed_ = signal_;
			return ChunkResult::Done;
		}
		uint64_t ProducerSignal() const override { return signal_; }
		uint64_t ProducerCompleted() const override { return completed_; }
		void Cancel(const LaneJobContext&) override { completed_ = signal_; }

	private:
		uint32_t chunks_ = 0;
		uint64_t signal_ = 0;
		uint64_t completed_ = 0;
		uint64_t counter_ = 0;
	};

	// Counts the GPU-compute work the lane records on the render thread.
	class FakeDispatcher : public IGpuDispatcher {
	public:
		bool Dispatch(const LaneJobContext&, uint32_t, uint32_t) override { ++dispatched; return true; }
		bool DispatchLive(const Node&, uint64_t, int, const Header&, uint32_t) override { ++live; return true; }
		int dispatched = 0;
		int live = 0;
	};

	ConsumerDecl Consumer(uint16_t id, std::initializer_list<ProductId> products, bool reduced = false)
	{
		ConsumerDecl c;
		c.id = id;
		c.hasReducedMode = reduced;
		for (ProductId p : products)
			AddConsume(c, p);
		return c;
	}

	struct Rig {
		Registry reg = FirstProducts();
		std::vector<ConsumerDecl> consumers;
		GraphSettings gs;
		CardProfile card;
		TierInput tier;
		std::atomic<uint64_t> clock{ 0 };
		Indicator ind;
		AnalysisLane lane;
		VpLaneProducts::HalfLumaProducer p1;
		VpLaneProducts::SourceStatsProducer p4;
		FakeOfa p2;
		std::vector<uint16_t> store;
		std::atomic<int> p2Live{ 0 };
		size_t frames;
		bool registered = false;

		explicit Rig(size_t frameCount = 320)
			: ind(7, 256)
			, lane(&ind, [this]() { return clock.fetch_add(3) + 3; })
			, p1([this]() { return clock.fetch_add(3) + 3; })
			, p4(Dims{ kW, kH }, [this]() { return clock.fetch_add(3) + 3; })
			, frames(frameCount)
		{
			consumers.push_back(Consumer(1, { kP4SourceStats, kP2VectorPair }));
			gs.dims.width = kW;
			gs.dims.height = kH;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			gs.queueDepth = 4;
			gs.lookAheadWalk = 0;
			gs.ofaPresent = true;
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			card.computeScale = 1.0;
			card.ofaPresent = true;
			card.ofaMsPerPairRef = 2.0;
			tier.framePeriodMs = 40.0;
			store.resize(frames * kW * kH);
			uint32_t x = 12345u;
			for (size_t i = 0; i < store.size(); ++i) {
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 0xFFFFu);
			}
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

		void Register()
		{
			if (registered)
				return;
			registered = true;
			lane.RegisterProducer(&p1);
			lane.RegisterProducer(&p2);
			lane.RegisterProducer(&p4);
			lane.RegisterCpuLiveCompute(kP1HalfLuma,
				[](const Node&, const SourceFrameView* v, const std::array<const uint8_t*, kMaxInputs>&, uint8_t* dst, size_t n) {
					return v != nullptr && VpLaneProducts::ComputeHalfLuma(*v, dst, n);
				});
			lane.RegisterCpuLiveCompute(kP4SourceStats,
				[](const Node&, const SourceFrameView*, const std::array<const uint8_t*, kMaxInputs>& in, uint8_t* dst, size_t n) {
					return VpLaneProducts::ComputeSourceStats(in[0], Dims{ kW, kH }, dst, n);
				});
			lane.SetLiveFill([this](const Node&, uint64_t, int, Header&, uint32_t) {
				p2Live.fetch_add(1);
				return true;
			});
		}

		LaneSettings Settings(LaneSettings::Mode mode) const
		{
			LaneSettings s;
			s.mode = mode;
			return s;
		}

		uint32_t Configure(LaneSettings::Mode mode, bool hostDet = false, uint64_t fromSeq = 0)
		{
			Register();
			return lane.Configure(Settings(mode), reg, consumers, gs, card, tier, fromSeq, hostDet);
		}
		uint32_t Configure(const LaneSettings& s, bool hostDet = false, uint64_t fromSeq = 0)
		{
			Register();
			return lane.Configure(s, reg, consumers, gs, card, tier, fromSeq, hostDet);
		}

		// Single-threaded: both worker backends until neither makes progress (bounded).
		void Pump()
		{
			for (int i = 0; i < 20000; ++i) {
				const bool a = lane.StepWorker(Backend::Ofa, 1000);
				const bool b = lane.StepWorker(Backend::Cpu, 1000);
				if (!a && !b)
					return;
			}
			Assert::Fail(L"pump did not settle");
		}

		// Frame s: arrive s + 1 (lead 1), run the lane, seal at the render start. A copy of the results.
		std::vector<ProductFrameResult> Begin(uint64_t s)
		{
			lane.OnSourceFrame(View(s + 1));
			Pump();
			return lane.OnRenderStart(s, 4000);
		}
		void End(uint64_t s)
		{
			lane.OnFrameRetired(s, 0);
			lane.OnFrameTiming(s, 10.0, 1.0, false);
		}

		static const ProductFrameResult& Res(const std::vector<ProductFrameResult>& r, ProductId id)
		{
			for (const ProductFrameResult& x : r)
				if (x.id == id)
					return x;
			Assert::Fail(L"product not in the results");
			return r[0];
		}

		std::vector<uint8_t> Bytes(ProductId id, uint64_t s)
		{
			const std::shared_ptr<LaneState> st = lane.State();
			std::vector<uint8_t> v;
			const int idx = st->IndexOf(id);
			if (idx < 0 || !st->bufs[size_t(idx)])
				return v;
			const ReadInfo ri = st->table->At(size_t(idx)).Read(s);
			CpuBuffers& b = *st->bufs[size_t(idx)];
			if (ri.kind == ReadInfo::Ring)
				v.assign(b.Ring(size_t(ri.index)), b.Ring(size_t(ri.index)) + b.Bytes());
			else if (ri.kind == ReadInfo::Live)
				v.assign(b.Live(size_t(ri.index)), b.Live(size_t(ri.index)) + b.Bytes());
			return v;
		}

		std::vector<uint8_t> ExpectedHalf(uint64_t s) const
		{
			const SourceFrameView v = View(s);
			std::vector<uint8_t> out(VpLaneProducts::HalfLumaBytes(Dims{ kW, kH }));
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(v, out.data(), out.size()), L"reference half luma");
			return out;
		}
		std::vector<uint8_t> ExpectedStats(const std::vector<uint8_t>& half) const
		{
			std::vector<uint8_t> out(kStatsBytesLocal(), 0);
			Assert::IsTrue(VpLaneProducts::ComputeSourceStats(half.data(), Dims{ kW, kH }, out.data(), out.size()),
				L"reference stats");
			return out;
		}
		static size_t kStatsBytesLocal() { return size_t(VpLaneProducts::kStatsBytes); }
	};

	uint64_t Digest(LaneState& st)
	{
		uint64_t d = 0;
		for (size_t i = 0; i < st.table->Count(); ++i) {
			const SlotCounters& c = st.table->At(i).Counters();
			d += c.claims.load() + c.published.load() + c.lateDiscards.load() + c.abandoned.load() + c.sealed.load()
			     + c.live.load() + c.late.load() + c.missing.load() + c.orderErrors.load();
			const LaneProductCounters& r = st.runner->Counters(st.table->NodeAt(i).id);
			d += r.published.load() + r.abandoned.load() + r.cancelled.load() + r.laneUs.load() + r.staleSkips.load();
		}
		return d;
	}

	TEST_CLASS(AnalysisLaneTests)
	{
	public:
		// 1. Off is inert: nothing built, no hook acts, no thread.
		TEST_METHOD(OffIsInert)
		{
			Rig r;
			Assert::IsTrue(r.Configure(LaneSettings::Mode::Off) == 0, L"Configure returns 0");
			Assert::IsTrue(!r.lane.Active() && r.lane.State() == nullptr, L"no state allocated");
			Assert::IsTrue(r.lane.Rebuilds() == 0, L"no graph built");
			const uint64_t wake = r.lane.WakeCount();
			r.lane.OnSourceFrame(r.View(0));
			Assert::IsTrue(r.lane.WakeCount() == wake, L"no wake");
			Assert::IsTrue(!r.lane.StepWorker(Backend::Cpu, 100) && !r.lane.StepWorker(Backend::Ofa, 100), L"no step");
			Assert::IsTrue(r.lane.OnRenderStart(0, 4000).empty(), L"no results");
			Assert::IsTrue(!r.ind.HasSealed(), L"the indicator is untouched (nothing sealed)");
			Assert::IsTrue(r.ind.Total(VpIndicator::kGraph) == 0, L"no graph event");
			Assert::IsTrue(r.lane.DeterministicWaitUs(0, 4000) == 0, L"no wait");
			Assert::IsTrue(r.lane.OnPostSubmitGap(100000).chunks == 0, L"no gap work");
			r.lane.OnFrameRetired(0, 0);
			r.lane.OnFrameDropped(1);
			r.lane.OnFrameTiming(0, 10.0, 1.0, false);
			r.lane.OnCut();
			Assert::IsTrue(r.lane.GetRegulator().MaxLevel() == 0, L"regulator empty");
			const std::vector<std::string> lines = r.lane.StatusLines();
			Assert::IsTrue(lines.size() == 1 && Has(lines[0], "off"), L"one status line: off");
			const std::vector<FallbackCatalog::Decision> fb = r.lane.Fallbacks();
			Assert::IsTrue(fb.size() == 3, L"three decisions");
			for (const FallbackCatalog::Decision& d : fb)
				Assert::IsTrue(!d.engaged, L"nothing engaged");
			LaneWorkerThreads t(r.lane);
			Assert::IsTrue(!t.Start() && !t.Running(), L"no thread in Off mode");
			t.Stop();
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Original, L"Bill's walk");
		}

		// 2. The five config keys.
		TEST_METHOD(ParseKeys)
		{
			LaneSettings s;
			Assert::IsTrue(s.mode == LaneSettings::Mode::Off && s.chunkUs == 0 && s.testWaitUs == 0, L"defaults");
			Assert::IsTrue(s.deterministic == LaneSettings::Det::Auto && s.walk == VpLaneProducts::WalkSource::Original, L"defaults 2");
			Assert::IsTrue(s.Parse("analysis_lane", "shadow") && s.mode == LaneSettings::Mode::Shadow, L"shadow");
			Assert::IsTrue(s.Parse("analysis_lane", "ON") && s.mode == LaneSettings::Mode::On, L"on, case-insensitive");
			Assert::IsTrue(s.Parse("analysis_lane", "off") && s.mode == LaneSettings::Mode::Off, L"off");
			Assert::IsTrue(!s.Parse("analysis_lane", "maybe") && s.mode == LaneSettings::Mode::Off, L"bad value leaves it");
			Assert::IsTrue(s.Parse("analysis_lane_chunk_us", "250") && s.chunkUs == 250u, L"chunk");
			Assert::IsTrue(!s.Parse("analysis_lane_chunk_us", "-1") && !s.Parse("analysis_lane_chunk_us", "") && s.chunkUs == 250u, L"bad chunk");
			Assert::IsTrue(s.Parse("analysis_lane_deterministic", "on") && s.deterministic == LaneSettings::Det::On, L"det on");
			Assert::IsTrue(s.Parse("analysis_lane_deterministic", "Off") && s.deterministic == LaneSettings::Det::Off, L"det off");
			Assert::IsTrue(s.Parse("analysis_lane_deterministic", "auto") && s.deterministic == LaneSettings::Det::Auto, L"det auto");
			Assert::IsTrue(s.Parse("analysis_lane_test_wait_ms", "3") && s.testWaitUs == 3000u, L"test wait ms -> us");
			Assert::IsTrue(s.Parse("source_stats_walk", "lane") && s.walk == VpLaneProducts::WalkSource::Lane, L"walk lane");
			Assert::IsTrue(s.Parse("source_stats_walk", "original") && s.walk == VpLaneProducts::WalkSource::Original, L"walk original");
			Assert::IsTrue(!s.Parse("source_stats_walk", "both"), L"bad walk");
			Assert::IsTrue(!s.Parse("analysis_lane_unknown", "1") && !s.Parse(nullptr, "1"), L"unknown key");
		}

		// 3. Shadow computes the same results and flags them; On does not.
		TEST_METHOD(ShadowFlagsResults)
		{
			Rig sh;
			Assert::IsTrue(sh.Configure(LaneSettings::Mode::Shadow) != 0, L"first build reports reasons");
			Assert::IsTrue(sh.lane.Active() && sh.lane.ShadowOnly(), L"shadow flagged");
			sh.lane.OnSourceFrame(sh.View(0));
			for (uint64_t s = 0; s < 6; ++s) {
				const std::vector<ProductFrameResult> res = sh.Begin(s);
				Assert::IsTrue(Rig::Res(res, kP1HalfLuma).seal.reason == SealReason::Accepted, L"results computed in shadow");
				sh.End(s);
			}
			Assert::IsTrue(AnyLine(sh.lane.StatusLines(), "shadow"), L"status says shadow");

			Rig on;
			on.Configure(LaneSettings::Mode::On);
			Assert::IsTrue(on.lane.Active() && !on.lane.ShadowOnly(), L"on is not shadow");
			Assert::IsTrue(AnyLine(on.lane.StatusLines(), "Analysis lane: on"), L"status says on");
		}

		// 4. 300 frames, single threaded: everything accepted on time, bit-identical to the live form.
		TEST_METHOD(EndToEnd300FramesAcceptedAndBitIdentical)
		{
			Rig r;
			Assert::IsTrue(r.Configure(LaneSettings::Mode::On) != 0, L"built");
			r.lane.OnSourceFrame(r.View(0));
			uint64_t p2Accepted = 0;
			for (uint64_t s = 0; s < 300; ++s) {
				const std::vector<ProductFrameResult> res = r.Begin(s);
				const ProductFrameResult& a = Rig::Res(res, kP1HalfLuma);
				const ProductFrameResult& b = Rig::Res(res, kP4SourceStats);
				const ProductFrameResult& o = Rig::Res(res, kP2VectorPair);
				Assert::IsTrue(a.seal.reason == SealReason::Accepted && !a.liveFilled, L"P1 accepted");
				Assert::IsTrue(b.seal.reason == SealReason::Accepted && !b.liveFilled, L"P4 accepted");
				if (s >= 1) {
					Assert::IsTrue(o.seal.reason == SealReason::Accepted && !o.liveFilled, L"P2 accepted");
					++p2Accepted;
				} else {
					Assert::IsTrue(o.final == SealOutcome::Unavailable, L"P2(0) has no previous frame");
				}
				const std::vector<uint8_t> half = r.ExpectedHalf(s);
				Assert::IsTrue(r.Bytes(kP1HalfLuma, s) == half, L"P1 bytes = the one-shot form");
				Assert::IsTrue(r.Bytes(kP4SourceStats, s) == r.ExpectedStats(half), L"P4 bytes = the one-shot form");
				r.End(s);
			}
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(p2Accepted == 299, L"299 P2 results from the lane");
			Assert::IsTrue(st->table->LiveFills() == 0, L"no live fill was needed");
			Assert::IsTrue(r.p2Live.load() == 0, L"the fake P2 live fill never ran");
			Assert::IsTrue(st->runner->Counters(kP1HalfLuma).published.load() >= 300, L"P1 published for every frame");
			Assert::IsTrue(st->runner->Counters(kP4SourceStats).latePublishes.load() == 0, L"nothing late");
			Assert::IsTrue(r.lane.TotalAhead() >= 300u + 300u + 299u, L"all counted ahead");
			Assert::IsTrue(r.lane.GetRegulator().Level() == r.lane.GetRegulator().MaxLevel(), L"regulator untouched");
			Assert::IsTrue(!r.lane.Fallbacks()[0].engaged, L"no analysis_lane fallback");
			Assert::IsTrue(AnyLine(r.lane.StatusLines(), "lane on time"), L"lead state on the status");
			Assert::IsTrue(r.lane.Lead() == LeadState::Ok, L"lead ok");
		}

		// 5. A regulator drop: the product declines (FillDeclined) and analysis_lane is engaged.
		TEST_METHOD(RegulatorDropDeclinesAndEngagesFallback)
		{
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP2VectorPair }, true));    // may run reduced
			r.consumers.push_back(Consumer(2, { kP4SourceStats }, false));  // may not
			r.Configure(LaneSettings::Mode::On, false);
			r.lane.OnSourceFrame(r.View(0));
			for (uint64_t s = 0; s < 3; ++s) {
				const std::vector<ProductFrameResult> res = r.Begin(s);
				Assert::IsTrue(Rig::Res(res, kP2VectorPair).seal.reason == SealReason::Accepted || s == 0, L"P2 runs at first");
				r.End(s);
			}
			// Two windows of missed deadlines: the level halves twice.
			for (uint64_t i = 0; i < 100; ++i)
				r.lane.OnFrameTiming(1000 + i, -5.0, 1.0, true);
			const Regulator& reg = r.lane.GetRegulator();
			Assert::IsTrue(reg.StateOf(kP2VectorPair) == ProductState::Dropped, L"P2 dropped (reduced mode, not deterministic)");
			Assert::IsTrue(reg.StateOf(kP4SourceStats) == ProductState::ForcedLive, L"P4 forced live (no reduced mode)");
			for (uint64_t s = 3; s < 9; ++s) {
				const std::vector<ProductFrameResult> res = r.Begin(s);
				const ProductFrameResult& o = Rig::Res(res, kP2VectorPair);
				// Frame 3 was published ahead before the drop and may still be accepted; later ones decline.
				Assert::IsTrue(s == 3 || (o.final == SealOutcome::Unavailable && o.finalReason == SealReason::FillDeclined),
					L"dropped product declines, visibly");
				Assert::IsTrue(Rig::Res(res, kP4SourceStats).seal.reason == SealReason::Accepted, L"a forced-live product stays laned");
				r.End(s);
			}
			Assert::IsTrue(r.lane.State()->table->FillDeclines() >= 5, L"declines counted");
			Assert::IsTrue(r.p2Live.load() == 0, L"the dropped product never reached the live fill");
			const std::vector<FallbackCatalog::Decision> fb = r.lane.Fallbacks();
			Assert::IsTrue(fb[0].id == "analysis_lane" && fb[0].engaged, L"analysis_lane engaged");
			Assert::IsTrue(Has(fb[0].reason, "dropped") && Has(fb[0].reason, "forced live"), L"reason names both");
			Assert::IsTrue(AnyLine(r.lane.StatusLines(), "dropped"), L"status shows it");
			RegTrace tr[32];
			Assert::IsTrue(r.lane.RegulatorTraces(tr, 32) > 0, L"regulator traces for the diagnostics");
		}

		// 6. A FULL Advance: the regulator floors, in-flight lane work is rejected at the seal, then it recovers.
		TEST_METHOD(FullAdvanceFloorsRegulatorAndRecovers)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On, false);
			r.lane.OnSourceFrame(r.View(0));
			for (uint64_t s = 0; s < 10; ++s) {
				r.Begin(s);
				r.End(s);
			}
			const Regulator& reg = r.lane.GetRegulator();
			Assert::IsTrue(reg.Level() == reg.MaxLevel() && reg.MaxLevel() > reg.T0StepCount(), L"full level before");
			r.ind.Advance(VpIndicator::kFull, Cause::LiveQueueReset, 10);
			const std::vector<ProductFrameResult> f10 = r.Begin(10);
			Assert::IsTrue(Rig::Res(f10, kP1HalfLuma).seal.reason == SealReason::StampMismatch, L"lane work from before the event is rejected");
			Assert::IsTrue(Rig::Res(f10, kP1HalfLuma).liveFilled && Rig::Res(f10, kP1HalfLuma).final == SealOutcome::Sealed, L"filled live");
			Assert::IsTrue(Rig::Res(f10, kP4SourceStats).liveFilled, L"P4 live too");
			Assert::IsTrue(reg.LaneLevel() == reg.T0StepCount(), L"the regulator floored");
			Assert::IsTrue(reg.OfaStep() == 0, L"the floor left the OFA variant alone");
			r.End(10);
			const std::vector<ProductFrameResult> f11 = r.Begin(11);
			Assert::IsTrue(Rig::Res(f11, kP1HalfLuma).seal.reason == SealReason::Accepted, L"P1 started after the event is accepted");
			r.End(11);
			for (uint64_t s = 12; s < 20; ++s) {
				const std::vector<ProductFrameResult> res = r.Begin(s);
				Assert::IsTrue(Rig::Res(res, kP1HalfLuma).seal.reason == SealReason::Accepted, L"P1 recovered");
				Assert::IsTrue(Rig::Res(res, kP4SourceStats).seal.reason == SealReason::Accepted, L"P4 recovered");
				Assert::IsTrue(Rig::Res(res, kP2VectorPair).seal.reason == SealReason::Accepted, L"P2 recovered");
				r.End(s);
			}
		}

		// 7. N1: a deterministic run waits (bounded) for the OFA product, counts a missed bound once.
		TEST_METHOD(DeterministicWaitBoundedAndCounted)
		{
			// The OFA worker runs while the host waits: the wait ends early, nothing missed.
			{
				Rig r;
				r.Configure(LaneSettings::Mode::On, true);
				Assert::IsTrue(r.lane.Deterministic(), L"auto follows the host");
				r.lane.OnSourceFrame(r.View(0));
				r.lane.OnSourceFrame(r.View(1));
				for (int i = 0; i < 100 && r.lane.StepWorker(Backend::Cpu, 1000); ++i) {}
				r.lane.OnRenderStart(0, 4000);   // frame 0 sealed without a wait
				uint32_t w = 0;
				int calls = 0;
				while ((w = r.lane.DeterministicWaitUs(1, 4000)) > 0) {
					Assert::IsTrue(w <= 500u, L"each wait is short");
					Assert::IsTrue(++calls < 20, L"bounded");
					if (calls > 1)
						r.lane.StepWorker(Backend::Ofa, 1000);
				}
				Assert::IsTrue(calls >= 2 && calls <= 6, L"waited for the OFA job");
				Assert::IsTrue(r.lane.State()->table->At(size_t(r.lane.State()->IndexOf(kP2VectorPair))).StateOf(1) == SlotState::Published, L"P2(1) published");
				Assert::IsTrue(r.lane.MissedBounds() == 0, L"no bound missed");
				const std::vector<ProductFrameResult> res = r.lane.OnRenderStart(1, 4000);
				Assert::IsTrue(Rig::Res(res, kP2VectorPair).seal.reason == SealReason::Accepted, L"P2(1) from the lane");
				Assert::IsTrue(!r.lane.Fallbacks()[1].engaged, L"no determinism fallback");
			}
			// The OFA worker never runs: the bound runs out once, counted once.
			{
				Rig r;
				r.Configure(LaneSettings::Mode::On, true);
				r.lane.OnSourceFrame(r.View(0));
				r.lane.OnSourceFrame(r.View(1));
				for (int i = 0; i < 100 && r.lane.StepWorker(Backend::Cpu, 1000); ++i) {}
				r.lane.OnRenderStart(0, 4000);
				uint32_t total = 0;
				int calls = 0;
				uint32_t w = 0;
				while ((w = r.lane.DeterministicWaitUs(1, 4000)) > 0) {
					total += w;
					Assert::IsTrue(++calls < 50, L"bounded");
				}
				Assert::IsTrue(calls == 7 && total == 3200u, L"slack 4000 us minus the guard (the regulator margin, 800 us)");
				Assert::IsTrue(r.lane.MissedBounds() == 1, L"one missed bound");
				Assert::IsTrue(r.lane.DeterministicWaitUs(1, 4000) == 0 && r.lane.MissedBounds() == 1, L"counted once per frame");
				const std::vector<FallbackCatalog::Decision> fb = r.lane.Fallbacks();
				Assert::IsTrue(fb[1].id == "analysis_lane_determinism" && fb[1].engaged, L"determinism decision engaged");
				Assert::IsTrue(AnyLine(r.lane.StatusLines(), "missed the wait bound"), L"status shows it");
				// Not a deterministic run: no wait at all.
				Rig n;
				n.Configure(LaneSettings::Mode::On, false);
				n.lane.OnSourceFrame(n.View(0));
				Assert::IsTrue(n.lane.DeterministicWaitUs(0, 4000) == 0, L"non-deterministic: seal now");
			}
		}

		// 8. The explicit test wait works without a deterministic run and replaces the slack bound.
		TEST_METHOD(ExplicitTestWait)
		{
			Rig r;
			LaneSettings s = r.Settings(LaneSettings::Mode::On);
			Assert::IsTrue(s.Parse("analysis_lane_deterministic", "off") && s.Parse("analysis_lane_test_wait_ms", "1"), L"keys");
			r.Configure(s, true);
			Assert::IsTrue(!r.lane.Deterministic(), L"explicitly not deterministic");
			r.lane.OnSourceFrame(r.View(0));
			r.lane.OnSourceFrame(r.View(1));
			for (int i = 0; i < 100 && r.lane.StepWorker(Backend::Cpu, 1000); ++i) {}
			r.lane.OnRenderStart(0, 4000);
			uint32_t total = 0;
			int calls = 0;
			uint32_t w = 0;
			while ((w = r.lane.DeterministicWaitUs(1, 4000)) > 0) {
				total += w;
				Assert::IsTrue(++calls < 20, L"bounded");
			}
			Assert::IsTrue(total == 1000u && calls == 2, L"the bound is the explicit 1 ms");
			Assert::IsTrue(r.lane.MissedBounds() == 1, L"counted");
			Assert::IsTrue(!r.lane.Fallbacks()[1].engaged, L"the determinism decision is for deterministic runs only");
		}

		// 9. A graph rebuild under running worker threads: the shared_ptr swap, no crash, the old table untouched.
		TEST_METHOD(RebuildUnderRunningWorkers)
		{
			Rig r(420);
			Assert::IsTrue(r.Configure(LaneSettings::Mode::On) != 0, L"built");
			LaneWorkerThreads workers(r.lane);
			Assert::IsTrue(workers.Start() && workers.Running(), L"workers running");
			std::atomic<uint64_t> arrived{ 0 };
			std::atomic<uint64_t> rendered{ 0 };
			std::atomic<bool> stopCapture{ false };
			std::thread capture([&]() {
				for (uint64_t s = 0; s < 400 && !stopCapture.load(); ++s) {
					// The source never runs further ahead than the queue holds.
					while (s > rendered.load() + 4 && !stopCapture.load())
						std::this_thread::yield();
					r.lane.OnSourceFrame(r.View(s));
					arrived.store(s + 1);
				}
			});
			// Progress, not wall time, decides when the run ends; the generous bound below only turns a hang
			// into a failure and never makes the test pass.
			const auto failAt = std::chrono::steady_clock::now() + std::chrono::seconds(120);
			bool timedOut = false;
			auto expired = [&]() {
				if (std::chrono::steady_clock::now() > failAt)
					timedOut = true;
				return timedOut;
			};
			uint64_t s = 0;
			int rebuilds = 0;
			std::shared_ptr<LaneState> old;
			uint64_t oldDigest = 0;
			bool oldQuiet = false;   // every worker let go of `old` (its use count fell to ours): the digest is then fixed
			bool oldReleased = true;
			// Quiescence of the old state: a worker lets go of it at the end of its current step.
			auto settleOld = [&](bool wait) {
				if (!old || oldQuiet)
					return;
				while (wait && old.use_count() > 1 && !expired())
					std::this_thread::yield();
				if (old.use_count() == 1) {
					oldQuiet = true;
					oldDigest = Digest(*old);
				} else if (wait) {
					oldReleased = false;
				}
			};
			while (s < 380 && !timedOut) {
				if (arrived.load() < s + 2) {
					if (expired())
						break;
					std::this_thread::yield();
					continue;
				}
				r.lane.OnRenderStart(s, 4000);
				r.lane.OnFrameRetired(s, 0);
				r.lane.OnFrameTiming(s, 10.0, 1.0, false);
				settleOld(false);
				if (s % 40 == 39 && rebuilds < 4) {
					settleOld(true);
					if (old && oldQuiet)
						Assert::IsTrue(Digest(*old) == oldDigest, L"the previous table was not touched after its swap");
					old = r.lane.State();
					oldQuiet = false;
					++r.gs.queueDepth;
					Assert::IsTrue(r.Configure(LaneSettings::Mode::On, false, s + 1) != 0, L"rebuild");
					++rebuilds;
					Assert::IsTrue(old != r.lane.State(), L"the state was swapped");
				}
				++s;
				rendered.store(s);
			}
			settleOld(true);
			stopCapture.store(true);
			capture.join();
			workers.Stop();
			Assert::IsTrue(!workers.Running(), L"workers joined");
				Assert::IsTrue(!timedOut, L"the run finished (the bound only reports a hang)");
			if (old && oldQuiet)
				Assert::IsTrue(Digest(*old) == oldDigest, L"the last old table was not touched");
			Assert::IsTrue(oldReleased, L"no holder of an old state remained");
			Assert::IsTrue(rebuilds == 4 && s == 380, L"the run went through every planned rebuild");
			Assert::IsTrue(r.lane.Rebuilds() == uint32_t(1 + rebuilds), L"one first build plus the rebuilds");
			Assert::IsTrue(r.ind.Total(VpIndicator::kGraph) == uint32_t(1 + rebuilds), L"each rebuild advanced graph");
		}

		// 10. Real worker threads start and stop, and publish on arrival.
		TEST_METHOD(WorkerThreadsStartStop)
		{
			Rig r;
			LaneWorkerThreads workers(r.lane);
			Assert::IsTrue(!workers.Start(), L"not before the lane is configured on");
			r.Configure(LaneSettings::Mode::On);
			Assert::IsTrue(workers.Start() && workers.Running(), L"started");
			Assert::IsTrue(!workers.Start(), L"not twice");
			for (uint64_t s = 0; s < 4; ++s)   // the ring holds 4 frames until the render releases some
				r.lane.OnSourceFrame(r.View(s));
			const std::shared_ptr<LaneState> st = r.lane.State();
				// Waits for the event (4 publications); the bound only turns a hang into a failure.
			const auto failAt = std::chrono::steady_clock::now() + std::chrono::seconds(120);
			while (st->runner->Counters(kP4SourceStats).published.load() < 4
			       && std::chrono::steady_clock::now() < failAt)
				std::this_thread::yield();
			Assert::IsTrue(st->runner->Counters(kP1HalfLuma).published.load() >= 4, L"P1 published by the CPU worker");
			Assert::IsTrue(st->runner->Counters(kP4SourceStats).published.load() >= 4, L"P4 published by the CPU worker");
			workers.Stop();
			Assert::IsTrue(!workers.Running(), L"stopped");
			workers.Stop();
			Assert::IsTrue(workers.Start(), L"restartable");
			workers.Stop();
			// Switching the lane off: no thread can start any more.
			r.Configure(LaneSettings::Mode::Off);
			Assert::IsTrue(!workers.Start() && !r.lane.Active(), L"off: nothing starts");
		}

		// 11. Lane GPU-compute products run in the render thread's idle gap through the adapter.
		TEST_METHOD(GapRunsLaneComputeProducts)
		{
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP3AnalysisField }));
			FakeDispatcher disp;
			r.lane.SetGpuDispatcher(&disp);
			VpLaneProducts::GpuComputeLaneJob p3(
				kP3AnalysisField, &disp, []() { return 0.1; }, [&r]() { return r.lane.ChunkCapUs(); }, 6u);
			r.lane.RegisterProducer(&p3);
			r.Configure(LaneSettings::Mode::On);
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(st->graph.Has(kP3AnalysisField) && st->gapWorks.size() == 1, L"one gap work for the compute product");
			r.lane.OnSourceFrame(r.View(0));
			r.lane.OnSourceFrame(r.View(1));
			r.lane.OnSourceFrame(r.View(2));
			r.Pump();
			r.lane.OnRenderStart(0, 4000);   // frame 0 has no previous frame: sealed, then the lane moves on
			r.Pump();
			const GapReport rep = r.lane.OnPostSubmitGap(r.clock.load() + 100000);
			Assert::IsTrue(rep.chunks > 0 && disp.dispatched > 0, L"the lane compute ran in the gap");
			Assert::IsTrue(rep.usedUs[size_t(GapClass::LaneCompute)] > 0, L"time charged to lane compute");
			Assert::IsTrue(st->runner->Counters(kP3AnalysisField).published.load() >= 1, L"P3 published on the render thread");
			r.Pump();
			const std::vector<ProductFrameResult> res = r.lane.OnRenderStart(1, 4000);
			Assert::IsTrue(Rig::Res(res, kP3AnalysisField).seal.reason == SealReason::Accepted, L"P3(1) accepted");
			Assert::IsTrue(disp.live == 0, L"no live dispatch needed");
		}

		// 12. Reconfigure keeps or rebuilds the graph; an invalid graph is visible, never silent.
		TEST_METHOD(ConfigureKeepsGraphAndInvalidGraphIsVisible)
		{
			Rig r;
			Assert::IsTrue(r.Configure(LaneSettings::Mode::On) == kRebuildFirst, L"first build");
			const std::shared_ptr<LaneState> a = r.lane.State();
			Assert::IsTrue(r.Configure(LaneSettings::Mode::Shadow) == 0 && r.lane.State() == a, L"same graph kept (mode only)");
			Assert::IsTrue(r.lane.ShadowOnly(), L"mode applied");
			LaneSettings s = r.Settings(LaneSettings::Mode::On);
			s.chunkUs = 321;
			Assert::IsTrue(r.Configure(s) == 0 && r.lane.ChunkCapUs() == 321u, L"chunk cap applied without a rebuild");
			Assert::IsTrue(r.lane.State()->gap.ChunkCapUs() == 321u, L"gap scheduler cap");
			r.gs.renderAhead = 2;
			Assert::IsTrue((r.Configure(LaneSettings::Mode::On) & kRebuildQueue) != 0 && r.lane.State() != a, L"queue change rebuilds");
			Assert::IsTrue(r.lane.ChunkCapUs() == 1000u, L"derived cap: period 40 ms / 8 = 5000 us, clamped to 1000 us");
		}

		// 13. An unknown product: lane unavailable, shown and in the fallbacks.
		TEST_METHOD(InvalidGraphIsVisible)
		{
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { ProductId(99) }));
			r.Configure(LaneSettings::Mode::On);
			Assert::IsTrue(!r.lane.Active(), L"no state");
			Assert::IsTrue(AnyLine(r.lane.StatusLines(), "unavailable"), L"status says so");
			const std::vector<FallbackCatalog::Decision> fb = r.lane.Fallbacks();
			Assert::IsTrue(fb[0].id == "analysis_lane" && fb[0].engaged && Has(fb[0].reason, "unavailable"), L"engaged");
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP4SourceStats }));
			Assert::IsTrue(r.Configure(LaneSettings::Mode::On) != 0 && r.lane.Active(), L"recovers with a valid consumer set");
			Assert::IsTrue(!r.lane.Fallbacks()[0].engaged, L"released");
		}

		// 14. The active-picture walk either/or and its decision.
		TEST_METHOD(WalkEitherOr)
		{
			Rig r;
			LaneSettings s = r.Settings(LaneSettings::Mode::On);
			r.Configure(s);
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Original, L"default: Bill's walk");
			Assert::IsTrue(!r.lane.Fallbacks()[2].engaged, L"nothing to report");
			s.walk = VpLaneProducts::WalkSource::Lane;
			r.Configure(s);
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Lane, L"lane walk with P4 present");
			Assert::IsTrue(!r.lane.Fallbacks()[2].engaged, L"honoured");
			// Lane requested but the lane is off: Bill's walk, visibly.
			s.mode = LaneSettings::Mode::Off;
			r.Configure(s);
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Original, L"off: Bill's walk");
			const std::vector<FallbackCatalog::Decision> fb = r.lane.Fallbacks();
			Assert::IsTrue(fb[2].id == "source_stats_walk" && fb[2].engaged, L"decision engaged");
			// Without P4 in the graph.
			s.mode = LaneSettings::Mode::On;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP1HalfLuma }));
			r.Configure(s);
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Original && r.lane.Fallbacks()[2].engaged, L"no P4: original, decision");
		}

		// 15. The source ring: held frames, lock-free copy, carry-over.
		TEST_METHOD(SourceRingBasics)
		{
			Rig r(16);
			LaneSourceRing ring(4);
			SourceFrameView f;
			Assert::IsTrue(!ring.Find(0, &f) && !ring.Newest(nullptr), L"empty");
			for (uint64_t s = 0; s < 6; ++s)
				ring.Store(r.View(s));
			uint64_t n = 0;
			Assert::IsTrue(ring.Newest(&n) && n == 5, L"newest");
			Assert::IsTrue(!ring.Find(1, &f) && ring.Find(2, &f) && ring.Find(5, &f) && f.seq == 5, L"oldest frames are overwritten");
			SourceFrameView v;
			Assert::IsTrue(ring.TryGet(4, &v) && v.seq == 4 && v.width == kW && v.luma == r.View(4).luma, L"lock-free copy");
			Assert::IsTrue(!ring.TryGet(1, &v) && !ring.TryGet(9, &v), L"not held");
			LaneSourceRing next(8);
			next.CopyFrom(ring);
			Assert::IsTrue(next.Find(2, &f) && next.Find(5, &f) && !next.Find(1, &f), L"carried over");
			Assert::IsTrue(next.Newest(&n) && n == 5, L"newest carried");
		}

		// 16. M1: the lane job owns its view of the source frame: a ring overwrite or a drop never frees
		// the planes under a running job; they are released when the job ends.
		TEST_METHOD(SourceViewKeepsPlanesAliveForTheJob)
		{
			// A: the ring overwrites the frame while the P1 job is mid-chunk.
			{
				Rig r;
				r.Configure(LaneSettings::Mode::On);
				const std::shared_ptr<LaneState> st = r.lane.State();
				const std::shared_ptr<int> token = std::make_shared<int>(7);
				SourceFrameView v0 = r.View(0);
				v0.keepAlive = token;
				r.lane.OnSourceFrame(v0);
				v0.keepAlive.reset();   // the test keeps only `token`
				Assert::IsTrue(token.use_count() == 2, L"the ring holds a copy");
				StepOutcome o = st->runner->Step(kP1HalfLuma, 1);
				Assert::IsTrue(o == StepOutcome::Ran, L"the job is mid-chunk");
				Assert::IsTrue(st->runner->Counters(kP1HalfLuma).published.load() == 0, L"not finished");
				Assert::IsTrue(token.use_count() == 3, L"ring + the job's own copy");
				const uint64_t overwrite = st->ring->Capacity();
				r.lane.OnSourceFrame(r.View(overwrite));   // takes frame 0's ring slot
				SourceFrameView probe;
				Assert::IsTrue(!st->ring->Find(0, &probe), L"the ring let go of frame 0");
				Assert::IsTrue(token.use_count() == 2, L"the job's copy keeps the planes alive");
				int guard = 0;
				while (o != StepOutcome::Published && ++guard < 100000) {
					o = st->runner->Step(kP1HalfLuma, 1);
					if (o != StepOutcome::Published)
						Assert::IsTrue(token.use_count() == 2, L"alive for the whole job");
				}
				Assert::IsTrue(o == StepOutcome::Published, L"the job finished");
				Assert::IsTrue(token.use_count() == 1, L"released when the job ended");
				const std::vector<ProductFrameResult> res = r.lane.OnRenderStart(0, 4000);
				Assert::IsTrue(Rig::Res(res, kP1HalfLuma).seal.reason == SealReason::Accepted, L"accepted from the lane");
				Assert::IsTrue(r.Bytes(kP1HalfLuma, 0) == r.ExpectedHalf(0), L"the job's view was unchanged");
			}
			// B: the host drops the frame mid-job: the job is cancelled and the planes are released.
			{
				Rig r;
				r.Configure(LaneSettings::Mode::On);
				const std::shared_ptr<LaneState> st = r.lane.State();
				const std::shared_ptr<int> token = std::make_shared<int>(8);
				SourceFrameView v0 = r.View(0);
				v0.keepAlive = token;
				r.lane.OnSourceFrame(v0);
				v0.keepAlive.reset();   // the test keeps only `token`
				StepOutcome o = st->runner->Step(kP1HalfLuma, 1);
				Assert::IsTrue(o == StepOutcome::Ran && token.use_count() == 3, L"mid-job: ring + job");
				r.lane.OnFrameDropped(0);
				SourceFrameView probe;
				Assert::IsTrue(!st->ring->Find(0, &probe) && token.use_count() == 2, L"the ring let go on the drop");
				int guard = 0;
				while (o != StepOutcome::Abandoned && o != StepOutcome::Late && ++guard < 1000)
					o = st->runner->Step(kP1HalfLuma, 1);
				Assert::IsTrue(guard < 1000, L"the job ended");
				Assert::IsTrue(token.use_count() == 1, L"released when the job ended");
			}
		}

		// 17. M2: a lane too far behind to claim (missing, not late) still reaches the regulator.
		TEST_METHOD(MissingFramesReachTheRegulator)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On);
			const std::shared_ptr<LaneState> st = r.lane.State();
			const size_t window = r.lane.GetRegulator().WindowFrames();
			for (uint64_t s = 0; s < window + 2; ++s) {   // the lane never runs
				r.lane.OnRenderStart(s, 4000);
				r.lane.OnFrameRetired(s, 0);
				r.lane.OnFrameTiming(s, 10.0, 1.0, false);
			}
			bool missingOnly = false;
			for (size_t i = 0; i < st->table->Count(); ++i) {
				const SlotCounters& c = st->table->At(i).Counters();
				missingOnly = missingOnly || (c.missing.load() > 0 && c.late.load() == 0);
			}
			Assert::IsTrue(missingOnly, L"the plant: missing without late");
			std::vector<RegTrace> tr(Regulator::kTraceCapacity);
			tr.resize(r.lane.RegulatorTraces(tr.data(), tr.size()));
			bool skip = false;
			for (const RegTrace& t : tr)
				skip = skip || t.action == RegAction::SkipToNext;
			Assert::IsTrue(skip, L"the regulator saw the lane behind (skip to next)");
		}

		// 18. M3: a GPU-compute job's chunk is predicted by its device cost: a 3 ms job at a 1 ms cap is
		// split, each chunk predicted <= the cap, and a 0.5 ms gap runs none of it.
		TEST_METHOD(GpuChunkPredictionKeepsSmallGapsEmpty)
		{
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP3AnalysisField }));
			FakeDispatcher disp;
			r.lane.SetGpuDispatcher(&disp);
			VpLaneProducts::GpuComputeLaneJob p3(
				kP3AnalysisField, &disp, []() { return 3.0; }, [&r]() { return r.lane.ChunkCapUs(); }, 6u);
			r.lane.RegisterProducer(&p3);
			r.Configure(LaneSettings::Mode::On);
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(r.lane.ChunkCapUs() == 1000u, L"1 ms cap");
			Assert::IsTrue(st->gapWorks.size() == 1, L"one gap work");
			r.lane.OnSourceFrame(r.View(0));
			r.lane.OnSourceFrame(r.View(1));
			r.lane.OnSourceFrame(r.View(2));
			r.Pump();
			r.lane.OnRenderStart(0, 4000);
			r.Pump();
			const uint32_t predicted = st->gapWorks[0]->PredictUs();
			Assert::IsTrue(predicted >= 1000u - 1u && predicted <= r.lane.ChunkCapUs(), L"predicted by the device cost, within the cap");
			const GapReport small = r.lane.OnPostSubmitGap(r.clock.load() + 500);
			Assert::IsTrue(small.chunks == 0 && disp.dispatched == 0, L"a 0.5 ms gap runs none of it");
			const GapReport big = r.lane.OnPostSubmitGap(r.clock.load() + 100000);
			Assert::IsTrue(big.chunks > 0 && disp.dispatched > 0, L"a big gap runs it");
			Assert::IsTrue(p3.Plan().chunkCount == 3u, L"split into 3 chunks of 1 ms");
		}

		// 19. S3: a product without a producer is not waited for.
		TEST_METHOD(NoProducerNoDeterministicWait)
		{
			Rig r;
			r.registered = true;   // register P1 + P4 only: P2 has no producer
			r.lane.RegisterProducer(&r.p1);
			r.lane.RegisterProducer(&r.p4);
			r.Configure(LaneSettings::Mode::On, true);
			r.lane.OnSourceFrame(r.View(0));
			r.lane.OnSourceFrame(r.View(1));
			for (int i = 0; i < 100 && r.lane.StepWorker(Backend::Cpu, 1000); ++i) {}
			r.lane.OnRenderStart(0, 4000);
			Assert::IsTrue(!r.lane.State()->runner->HasProducer(kP2VectorPair), L"no producer");
			Assert::IsTrue(r.lane.DeterministicWaitUs(1, 4000) == 0, L"nothing to wait for");
			Assert::IsTrue(r.lane.MissedBounds() == 0, L"no bound burnt");
		}

		// 20. S4: a regulator-dropped P4 makes the lane walk unavailable (Bill's walk + the decision).
		TEST_METHOD(DroppedP4FallsBackToOriginalWalk)
		{
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP4SourceStats, kP2VectorPair }, true));
			LaneSettings s = r.Settings(LaneSettings::Mode::On);
			s.walk = VpLaneProducts::WalkSource::Lane;
			r.Configure(s);
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Lane, L"lane walk while P4 runs");
			for (uint64_t f = 0; f < 1000 && r.lane.GetRegulator().StateOf(kP4SourceStats) != ProductState::Dropped; ++f)
				r.lane.OnFrameTiming(f, 0.0, 1.0, false);
			Assert::IsTrue(r.lane.GetRegulator().StateOf(kP4SourceStats) == ProductState::Dropped, L"P4 dropped by overload");
			Assert::IsTrue(r.lane.EffectiveWalk() == VpLaneProducts::WalkSource::Original, L"Bill's walk");
			Assert::IsTrue(r.lane.Fallbacks()[2].engaged, L"the decision is shown");
		}

		// 21. S6: frames skipped because their source is gone are shown.
		TEST_METHOD(SkippedFramesAreShown)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On);
			Assert::IsTrue(!AnyLine(r.lane.StatusLines(), "source gone"), L"nothing skipped yet");
			r.lane.OnSourceFrame(r.View(0));
			r.lane.OnSourceFrame(r.View(1));
			r.lane.OnFrameDropped(0);
			for (int i = 0; i < 100 && r.lane.StepWorker(Backend::Cpu, 1000); ++i) {}
			Assert::IsTrue(AnyLine(r.lane.StatusLines(), "frames skipped (source gone)"), L"shown in plain language");
		}

		// 22. Audit-2 MUST 1: a keepAlive deleter (host pool release) never runs under the ring's mutex:
		// it may re-enter the ring from any thread without deadlock.
		TEST_METHOD(RingKeepAliveDeleterMayReenterTheRing)
		{
			struct Shared {
				LaneSourceRing ring{ 2 };
				std::atomic<int> deleted{ 0 };
				std::atomic<bool> done{ false };
			};
			const std::shared_ptr<Shared> sh = std::make_shared<Shared>();
			std::thread t([sh]() {
				auto make = [&sh](uint64_t seq) {
					SourceFrameView v;
					v.seq = seq;
					v.keepAlive = std::shared_ptr<const void>(new int(1), [sh](int* p) {
						SourceFrameView probe;
						sh->ring.Find(900, &probe);   // takes the ring mutex: a deadlock if it is held
						sh->ring.Drop(900);
						uint64_t n = 0;
						sh->ring.Newest(&n);
						++sh->deleted;
						delete p;
					});
					return v;
				};
				sh->ring.Store(make(0));
				sh->ring.Store(make(1));
				sh->ring.Store(make(2));   // overwrites frame 0's slot
				SourceFrameView out = make(7);
				sh->ring.Find(2, &out);    // replaces out: frame 7's last copy dies in the caller, outside the lock
				sh->ring.Drop(1);
				sh->ring.Clear();          // the ring's copy of frame 2; `out` still holds one
				out = SourceFrameView();
				sh->done.store(true);
			});
			const auto failAt = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (!sh->done.load() && std::chrono::steady_clock::now() < failAt)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (!sh->done.load()) {
				t.detach();   // deadlocked: the shared state outlives the thread
				Assert::Fail(L"a keepAlive deleter that re-enters the ring deadlocked");
			}
			t.join();
			Assert::IsTrue(sh->deleted.load() == 4, L"every deleter ran once (frames 0, 7, 1, 2)");
		}

		// 23. The ring's copy ends with the frame (retire) and with a swapped-out graph: the host pool is
		// not held longer or twice.
		TEST_METHOD(RingCopiesAreReleasedOnRetireAndOnSwap)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On);
			const std::shared_ptr<int> token = std::make_shared<int>(3);
			SourceFrameView v0 = r.View(0);
			v0.keepAlive = token;
			r.lane.OnSourceFrame(v0);
			v0.keepAlive.reset();
			Assert::IsTrue(token.use_count() == 2, L"the ring holds a copy");
			r.lane.OnFrameRetired(0, 0);
			Assert::IsTrue(token.use_count() == 1, L"retired: the ring let go");
			SourceFrameView v1 = r.View(1);
			v1.keepAlive = token;
			r.lane.OnSourceFrame(v1);
			v1.keepAlive.reset();
			Assert::IsTrue(token.use_count() == 2, L"held again");
			const std::shared_ptr<LaneState> old = r.lane.State();
			++r.gs.queueDepth;
			Assert::IsTrue(r.Configure(LaneSettings::Mode::On, false, 1) != 0 && r.lane.State() != old, L"rebuilt");
			SourceFrameView probe;
			Assert::IsTrue(r.lane.State()->ring->Find(1, &probe), L"the new ring carries the frame");
			probe = SourceFrameView();
			Assert::IsTrue(!old->ring->Find(1, &probe), L"the swapped-out ring let go");
			Assert::IsTrue(token.use_count() == 2, L"one copy only (new ring), not doubled");
		}

		// 24. Audit-2 MUST 2: the REAL gap adapter recovers from one jittery over-cap Step within a few gaps.
		TEST_METHOD(GapWorkRecoversAfterOneOverCapStep)
		{
			struct SlowDispatcher : IGpuDispatcher {
				std::atomic<uint64_t>* clock = nullptr;
				uint64_t slowUs = 0;
				int dispatched = 0;
				bool first = true;
				bool Dispatch(const LaneJobContext&, uint32_t, uint32_t) override
				{
					++dispatched;
					if (first) {
						first = false;
						clock->fetch_add(slowUs);   // one outlier: this Step took far longer than the cap
					}
					return true;
				}
				bool DispatchLive(const Node&, uint64_t, int, const Header&, uint32_t) override { return true; }
			};
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP3AnalysisField }));
			SlowDispatcher disp;
			disp.clock = &r.clock;
			disp.slowUs = 50000;
			r.lane.SetGpuDispatcher(&disp);
			// No device-cost prediction (0): the adapter only has its measured Step time to go on.
			VpLaneProducts::GpuComputeLaneJob p3(
				kP3AnalysisField, &disp, []() { return 0.0; }, [&r]() { return r.lane.ChunkCapUs(); }, 6u);
			r.lane.RegisterProducer(&p3);
			r.Configure(LaneSettings::Mode::On);
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(st->gapWorks.size() == 1, L"one gap work");
			r.lane.OnSourceFrame(r.View(0));
			r.lane.OnSourceFrame(r.View(1));
			r.lane.OnSourceFrame(r.View(2));
			r.Pump();
			r.lane.OnRenderStart(0, 4000);
			r.Pump();
			const GapReport first = r.lane.OnPostSubmitGap(r.clock.load() + 1000000);
			Assert::IsTrue(first.chunks >= 1 && disp.dispatched == 1, L"the outlier Step ran");
			Assert::IsTrue(st->gapWorks[0]->PredictUs() > r.lane.ChunkCapUs() || first.capNoFit > 0, L"the outlier is remembered");
			uint32_t gaps = 0;
			bool ran = false;
			while (!ran && gaps < 16) {
				const GapReport g = r.lane.OnPostSubmitGap(r.clock.load() + 1000000);
				ran = g.chunks > 0;
				++gaps;
			}
			Assert::IsTrue(ran, L"the product is scheduled again within a few gaps (it starved before)");
			Assert::IsTrue(r.lane.State()->gap.Totals().capNoFit >= 1, L"the cap misses were counted");
			Assert::IsTrue(AnyLine(r.lane.StatusLines(), "too big for the chunk cap"), L"and shown in plain language");
		}

		// 25. Audit-2 MUST 3: a product without a producer has no lane: in-render-only, never counted
		// missing, never skipping the healthy products or shedding them.
		TEST_METHOD(ProducerlessProductGetsNoLaneAndNeverCountsMissing)
		{
			Rig r;
			r.registered = true;   // P1 + P4 only: P2 has no producer
			r.lane.RegisterProducer(&r.p1);
			r.lane.RegisterProducer(&r.p4);
			r.lane.RegisterCpuLiveCompute(kP1HalfLuma,
				[](const Node&, const SourceFrameView* v, const std::array<const uint8_t*, kMaxInputs>&, uint8_t* dst, size_t n) {
					return v != nullptr && VpLaneProducts::ComputeHalfLuma(*v, dst, n);
				});
			r.lane.RegisterCpuLiveCompute(kP4SourceStats,
				[](const Node&, const SourceFrameView*, const std::array<const uint8_t*, kMaxInputs>& in, uint8_t* dst, size_t n) {
					return VpLaneProducts::ComputeSourceStats(in[0], Dims{ kW, kH }, dst, n);
				});
			r.lane.SetLiveFill([&r](const Node&, uint64_t, int, Header&, uint32_t) {
				r.p2Live.fetch_add(1);
				return true;
			});
			r.Configure(LaneSettings::Mode::On, false);
			const std::shared_ptr<LaneState> st = r.lane.State();
			const int i2 = st->IndexOf(kP2VectorPair);
			Assert::IsTrue(i2 >= 0 && !st->runner->HasProducer(kP2VectorPair), L"P2 has no producer");
			Assert::IsTrue(!st->table->At(size_t(i2)).LaneGranted() && !st->runner->Laned(kP2VectorPair), L"no lane granted to it");
			Assert::IsTrue(st->table->At(size_t(st->IndexOf(kP1HalfLuma))).LaneGranted()
			               && st->table->At(size_t(st->IndexOf(kP4SourceStats))).LaneGranted(), L"the others keep theirs");
			const Regulator& reg = r.lane.GetRegulator();
			const size_t frames = reg.WindowFrames() * 3u;
			r.lane.OnSourceFrame(r.View(0));
			for (uint64_t s = 0; s < frames; ++s) {
				const std::vector<ProductFrameResult> res = r.Begin(s);
				Assert::IsTrue(Rig::Res(res, kP1HalfLuma).seal.reason == SealReason::Accepted, L"P1 stays accepted");
				Assert::IsTrue(Rig::Res(res, kP4SourceStats).seal.reason == SealReason::Accepted, L"P4 stays accepted");
				if (s >= 2)
					Assert::IsTrue(Rig::Res(res, kP2VectorPair).seal.reason == SealReason::InRenderOnly, L"P2 is in-render-only");
				r.End(s);
			}
			Assert::IsTrue(st->table->At(size_t(i2)).Counters().missing.load() == 0, L"never counted missing");
			std::vector<RegTrace> tr(Regulator::kTraceCapacity);
			tr.resize(r.lane.RegulatorTraces(tr.data(), tr.size()));
			for (const RegTrace& t : tr)
				Assert::IsTrue(t.action != RegAction::SkipToNext, L"no healthy product was told to skip");
			Assert::IsTrue(reg.Dropped().empty() && reg.Level() == reg.MaxLevel(), L"nothing shed");
			Assert::IsTrue(!r.lane.Fallbacks()[0].engaged, L"no fallback engaged");
			Assert::IsTrue(r.p2Live.load() > 0, L"P2 was filled live");
		}

		// 26. Audit-2 SHOULD: an optical-flow step-down is a visible fallback, not only a status note.
		TEST_METHOD(OfaStepDownEngagesTheFallback)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On, false);
			const Regulator& reg = r.lane.GetRegulator();
			Assert::IsTrue(reg.MaxLevel() > 0 && reg.OfaStep() == 0, L"full at first");
			Assert::IsTrue(!r.lane.Fallbacks()[0].engaged, L"nothing engaged");
			for (uint64_t f = 0; f < 20000 && reg.OfaStep() == 0; ++f)
				r.lane.OnFrameTiming(f, -5.0, 1.0, true);
			Assert::IsTrue(reg.OfaStep() > 0, L"overload stepped the vector engine input down");
			const FallbackCatalog::Decision d = r.lane.Fallbacks()[0];
			Assert::IsTrue(d.id == "analysis_lane" && d.engaged && Has(d.reason, "vector engine input reduced"), L"engaged with the reason");
		}

		// 27. Audit-2 SHOULD: capNoFit / starved gaps / suppressed step-downs are shown only when nonzero.
		TEST_METHOD(StatusShowsSuppressedStepDowns)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On);
			Assert::IsTrue(!AnyLine(r.lane.StatusLines(), "ignored") && !AnyLine(r.lane.StatusLines(), "chunk cap")
			               && !AnyLine(r.lane.StatusLines(), "no time"), L"nothing to show yet");
			r.lane.OnCut();
			r.lane.OnCut();   // the second one in the same window is ignored
			Assert::IsTrue(r.lane.GetRegulator().SuppressedStepDowns() == 1, L"one suppressed");
			Assert::IsTrue(AnyLine(r.lane.StatusLines(), "1 source changes ignored"), L"shown in plain language");
		}

		// 28. Audit-2 SHOULD: a determinism change on a kept graph re-applies the regulator to the runner.
		TEST_METHOD(DeterminismChangeReappliesRegulator)
		{
			Rig r;
			r.consumers.clear();
			r.consumers.push_back(Consumer(1, { kP2VectorPair }, true));
			r.consumers.push_back(Consumer(2, { kP4SourceStats }, false));
			r.Configure(LaneSettings::Mode::On, false);
			const std::shared_ptr<LaneState> st = r.lane.State();
			for (uint64_t i = 0; i < 100; ++i)
				r.lane.OnFrameTiming(1000 + i, -5.0, 1.0, true);
			Assert::IsTrue(r.lane.GetRegulator().StateOf(kP2VectorPair) == ProductState::Dropped, L"P2 dropped");
			Assert::IsTrue(!st->runner->Laned(kP2VectorPair), L"and not laned");
			Assert::IsTrue(r.Configure(LaneSettings::Mode::On, true) == 0 && r.lane.State() == st, L"the graph is kept");
			Assert::IsTrue(r.lane.Deterministic(), L"now deterministic");
			Assert::IsTrue(r.lane.GetRegulator().StateOf(kP2VectorPair) == ProductState::Laned, L"the regulator restarted");
			Assert::IsTrue(st->runner->Laned(kP2VectorPair)
			               && st->table->At(size_t(st->IndexOf(kP2VectorPair))).LaneGranted(), L"the runner and the slots follow at once");
		}
	};

	// Resync Q0-B: FrameSeal owns the one seal; the lane holds the Indicator by shared handle.
	TEST_CLASS(AnalysisLaneFrameSealTests)
	{
	public:
		static std::shared_ptr<Indicator> Alias(Indicator& i) { return std::shared_ptr<Indicator>(&i, [](Indicator*) {}); }

		// A frame in external-seal mode: arrive, pump, FrameSeal seals + fans out, lane render start.
		static void Frame(Rig& r, VpSeal::FrameSeal& fs, const std::shared_ptr<Indicator>& h, uint64_t s)
		{
			r.lane.OnSourceFrame(r.View(s + 1));
			r.Pump();
			fs.Seal(h, s);
			r.lane.OnRenderStart(s, 4000);
			r.End(s);
		}

		// One seal per frame with the lane and the Drive both subscribed; the lane never seals itself.
		TEST_METHOD(OneSealPerFrameWithLaneAndDrive)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On, false, 1);
			r.lane.SetExternalSeal(true);
			const std::shared_ptr<Indicator> h = Alias(r.ind);
			VpResync::Drive drive;
			VpSeal::FrameSeal fs;
			fs.AddProvider("resync-drive", [&drive](const std::shared_ptr<Indicator>& i) { drive.RegisterOn(i); });
			fs.AddProvider("lane", [&r](const std::shared_ptr<Indicator>& i) { r.lane.BindIndicator(i); });
			fs.Subscribe("lane", [&r](const std::shared_ptr<Indicator>&, uint64_t seq, VpIndicator::CounterMask c) { r.lane.OnSealed(seq, c); });
			fs.Subscribe("resync-drive", [&drive](const std::shared_ptr<Indicator>& i, uint64_t seq, VpIndicator::CounterMask c) { drive.OnObserved(i, seq, c); });
			r.lane.OnSourceFrame(r.View(1));
			for (uint64_t s = 1; s <= 20; ++s)
				Frame(r, fs, h, s);
			Assert::IsTrue(fs.Seals() == 20 && fs.Eras() == 1, L"one seal per frame, one era");
			Assert::IsTrue(r.ind.LastSealed() == 20 && r.ind.UnsealedObserves() == 0 && r.ind.OutOfOrderObserves() == 0, L"in order");
			Assert::IsTrue(r.lane.ExternalSealMisses() == 0, L"the lane never needed to seal");
			Assert::IsTrue(r.ind.RejectedReaders() == 0 && drive.Registered() == h, L"every reader registered before the first observe");
			Assert::IsTrue(drive.LastSealed() == 20, L"the drive saw each frame");
		}

		// The standalone drive and the FrameSeal path see identical pending masks.
		TEST_METHOD(DriveProviderPathMatchesStandalone)
		{
			std::vector<VpIndicator::CounterMask> a, b;
			for (int mode = 0; mode < 2; ++mode) {
				Indicator ind(5, 256);
				const std::shared_ptr<Indicator> h = Alias(ind);
				VpResync::Drive drive;
				VpSeal::FrameSeal fs;
				fs.AddProvider("drive", [&drive](const std::shared_ptr<Indicator>& i) { drive.RegisterOn(i); });
				fs.Subscribe("drive", [&drive](const std::shared_ptr<Indicator>& i, uint64_t seq, VpIndicator::CounterMask c) { drive.OnObserved(i, seq, c); });
				std::vector<VpIndicator::CounterMask>& out = mode == 0 ? a : b;
				for (uint64_t s = 1; s <= 12; ++s) {
					if (s == 5)
						ind.Advance(VpIndicator::kFull, Cause::LiveQueueReset, 5);
					if (s == 8)
						ind.Advance(VpIndicator::kContinuity, Cause::DriftDrop, 8, 1);
					if (mode == 0)
						drive.SealAndObserve(h, s);
					else
						fs.Seal(h, s);
					out.push_back(drive.Take(VpResync::Reader::RenderAhead) | (drive.Take(VpResync::Reader::SigmaDelta) << 16));
				}
			}
			Assert::IsTrue(a == b, L"identical masks per frame");
			Assert::IsTrue(a[4] != 0 && a[7] != 0, L"the FULL and the MINOR were seen");
		}

		// A new era re-registers every provider, and the lane holds no stale Indicator after the swap.
		TEST_METHOD(NewEraReRegistersProvidersAndLaneHoldsNoStaleIndicator)
		{
			VpResync::Shadow shadow;
			shadow.Configure(VpResync::Shadow::Mode::On, 8);
			std::shared_ptr<Indicator> first = shadow.GetIndicator();
			AnalysisLane lane(first, []() { return uint64_t(0); });
			VpResync::Drive drive;
			VpSeal::FrameSeal fs;
			int calls = 0;
			fs.AddProvider("count", [&calls](const std::shared_ptr<Indicator>&) { ++calls; });
			fs.AddProvider("drive", [&drive](const std::shared_ptr<Indicator>& i) { drive.RegisterOn(i); });
			fs.AddProvider("lane", [&lane](const std::shared_ptr<Indicator>& i) { lane.BindIndicator(i); });
			fs.Seal(shadow.GetIndicator(), 1);
			fs.Seal(shadow.GetIndicator(), 2);
			Assert::IsTrue(calls == 1 && fs.Eras() == 1, L"registered once on the first era");
			const uint32_t era1 = first->Era();
			shadow.Configure(VpResync::Shadow::Mode::Off, 8);
			shadow.Configure(VpResync::Shadow::Mode::Shadow, 8);   // a fresh Indicator
			const std::shared_ptr<Indicator> second = shadow.GetIndicator();
			Assert::IsTrue(second && second->Era() != era1, L"a new era");
			fs.Seal(second, 3);
			Assert::IsTrue(calls == 2 && fs.Eras() == 2, L"every provider registered again");
			Assert::IsTrue(drive.Registered() == second && drive.ReaderIndex(VpResync::Reader::RenderAhead) >= 0, L"drive readers on the new era");
			Assert::IsTrue(second->RejectedReaders() == 0, L"before the first observe");
			Assert::IsTrue(lane.IndicatorRebinds() == 1, L"lane rebound once");
			std::weak_ptr<Indicator> old = first;
			first.reset();
			Assert::IsTrue(old.expired(), L"nothing (lane, seal, shadow) keeps the old Indicator alive");
		}

		// The storm hold reaches the lane's regulator as T0 and is counted.
		TEST_METHOD(StormHoldFloorsTheLaneRegulatorToT0)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On, false, 1);
			r.lane.SetExternalSeal(true);
			const std::shared_ptr<Indicator> h = Alias(r.ind);
			VpSeal::FrameSeal fs;
			fs.AddProvider("lane", [&r](const std::shared_ptr<Indicator>& i) { r.lane.BindIndicator(i); });
			fs.Subscribe("lane", [&r](const std::shared_ptr<Indicator>&, uint64_t seq, VpIndicator::CounterMask c) { r.lane.OnSealed(seq, c); });
			r.lane.OnSourceFrame(r.View(1));
			for (uint64_t s = 1; s <= 10; ++s)
				Frame(r, fs, h, s);
			const Regulator& reg = r.lane.GetRegulator();
			Assert::IsTrue(reg.LaneLevel() >= reg.T0StepCount(), L"at or above T0 before the storm");
			RegTrace before[64];
			const size_t nBefore = r.lane.RegulatorTraces(before, 64);
			r.lane.SetStormHold(true);
			Frame(r, fs, h, 11);
			Assert::IsTrue(reg.LaneLevel() == reg.T0StepCount(), L"held at T0");
			RegTrace after[64];
			const size_t nAfter = r.lane.RegulatorTraces(after, 64);
			Assert::IsTrue(nAfter == nBefore + 1 && after[nAfter - 1].action == RegAction::ConservativeFloor, L"the engagement is traced once as a conservative floor");
			Assert::IsTrue(r.lane.StormHoldEngagements() == 1 && r.lane.StormHold(), L"counted");
			r.lane.SetStormHold(false);
			Assert::IsTrue(!r.lane.StormHold(), L"released");
		}

		// In external-seal mode a frame nobody sealed is counted, never sealed twice or silently.
		TEST_METHOD(UnsealedFrameInExternalModeIsCounted)
		{
			Rig r;
			r.Configure(LaneSettings::Mode::On, false, 1);
			r.lane.SetExternalSeal(true);
			r.lane.OnSourceFrame(r.View(1));
			r.Pump();
			r.lane.OnRenderStart(1, 4000);
			Assert::IsTrue(r.lane.ExternalSealMisses() == 1, L"counted");
			Assert::IsTrue(!r.ind.HasSealed(), L"the lane did not seal");
		}
	};
}
