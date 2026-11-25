// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// N#3 (Y1b-3 + X1 SHOULD):
//   1. P2LaneFeed claim/publish protocol (AheadOfa provenance, K1 stamp read after pins, missing-input
//      parking + FIFO Drain, bounded kPendingCap=16 ring dropping oldest, generation guard + matching-gen
//      negative control, NoLease counting, zero-render-wait TryLockClaim/Drain busy counting).
//   2. Concurrent BACK-thread Offer + render-thread Drain/TryLockClaim + SetP2PublishSink({}) teardown
//      stress test with exact conservation accounting.
//   3. X1 SHOULD deterministic final-seal skip: skips waiting at s = 2 for no-job P2(0) while still
//      waiting at s = 3 on the exact same rig when P2(1) has an active in-flight OFA job.
// Pure CPU; built without PCH.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/core/LaneRunner.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/modules/control/skeleton/P2LaneFeed.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace P2LaneFeedTestsNs
{
	using namespace VpSkeleton;
	using Vpv::Skeleton::P2LaneFeed;
	using Outcome = P2LaneFeed::Outcome;

	VpMotion::P2Published Pair(uint64_t to, uint64_t generation = 0, bool globalUsable = true)
	{
		VpMotion::P2Published p;
		p.from = to > 0 ? to - 1 : 0;
		p.to = to;
		p.generation = generation;
		p.globalUsable = globalUsable;
		return p;
	}

	MotionAhead::FlowPtr MakeUsableFlow()
	{
		std::shared_ptr<MotionAhead::Flow> f = std::make_shared<MotionAhead::Flow>();
		f->globalSet = true;
		f->global[0][2] = 0.2f;
		f->global[0][3] = 1.0f;
		return f;
	}

	ConsumerDecl MakeConsumer(uint16_t id, std::initializer_list<ProductId> products)
	{
		ConsumerDecl c;
		c.id = id;
		for (ProductId p : products)
			AddConsume(c, p);
		return c;
	}

	// Minimal self-contained SlotTable + Indicator rig holding P1, P2, P4 from FirstProducts().
	struct FeedRig
	{
		Graph graph;
		std::unique_ptr<SlotTable> table;
		std::unique_ptr<VpIndicator::Indicator> indicator;

		explicit FeedRig(uint8_t laneLead = 4)
		{
			GraphSettings gs;
			gs.dims.width = 64;
			gs.dims.height = 48;
			gs.laneLead = laneLead;
			gs.queueDepth = 8;
			gs.lookAheadWalk = 2;
			gs.ofaPresent = true;
			std::vector<ConsumerDecl> consumers;
			consumers.push_back(MakeConsumer(1, { kP4SourceStats, kP2VectorPair }));
			graph = BuildGraph(FirstProducts(), consumers, gs);
			table.reset(new SlotTable(graph));
			indicator.reset(new VpIndicator::Indicator(1, 256));
			// Seal frame 0 with P1(0) published so P2(1) has its lag-1 P1(0) ancestor.
			PublishP1(0);
			table->Resolve(0, indicator->Seal(0), [](const Node& n, uint64_t, int, VpAncestry::Header&, uint32_t) {
				return n.id != kP2VectorPair;
			});
		}

		bool PublishP1(uint64_t seq)
		{
			SlotSet* s = table->Find(kP1HalfLuma);
			if (!s)
				return false;
			LaneTicket t;
			if (s->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) != ClaimResult::Claimed)
				return false;
			if (!s->StartWriting(t))
			{
				s->Abandon(t);
				return false;
			}
			VpAncestry::Header& hdr = s->LaneHeader(t);
			LaneInputs pins;
			table->LaneAncestors(kP1HalfLuma, seq, hdr, pins);
			hdr.honoured = indicator->StampOf(seq);
			const bool ok = s->Publish(t) == PublishResult::Published;
			pins.Unpin();
			return ok;
		}

		std::shared_ptr<P2LaneFeed> MakeFeed(P2LaneFeed::GenerationFn generation = P2LaneFeed::GenerationFn())
		{
			SlotTable* t = table.get();
			VpIndicator::Indicator* ind = indicator.get();
			return std::make_shared<P2LaneFeed>(
				[t, ind]() { return LaneLease::Borrow(t, nullptr, ind); },
				std::move(generation));
		}
	};

	// Fake OFA producer matching AnalysisLaneTests (hold=true keeps P2 in Writing until released).
	class FakeOfaProducer : public ILaneProducer
	{
	public:
		explicit FakeOfaProducer(ProductId id = kP2VectorPair) : id_(id) {}
		ProductId Product() const override { return id_; }
		bool Begin(const LaneJobContext&) override
		{
			chunks_ = 0;
			signal_ = ++counter_;
			completed_ = 0;
			return true;
		}
		ChunkResult RunChunk(const LaneJobContext&, uint32_t) override
		{
			if (hold)
				return ChunkResult::More;
			if (++chunks_ < 2)
				return ChunkResult::More;
			completed_ = signal_;
			return ChunkResult::Done;
		}
		uint64_t ProducerSignal() const override { return signal_; }
		uint64_t ProducerCompleted() const override { return completed_; }
		void Cancel(const LaneJobContext&) override { completed_ = signal_; }
		bool hold = false;

	private:
		ProductId id_;
		uint32_t chunks_ = 0;
		uint64_t signal_ = 0;
		uint64_t completed_ = 0;
		uint64_t counter_ = 0;
	};

	// Deterministic AnalysisLane rig with P2 at sealLag = 2 for the X1 SHOULD final-seal skip test.
	struct LaggedLaneRig
	{
		static constexpr uint32_t kW = 64;
		static constexpr uint32_t kH = 48;

		Registry reg;
		std::vector<ConsumerDecl> consumers;
		GraphSettings gs;
		CardProfile card;
		TierInput tier;
		std::atomic<uint64_t> clock{ 0 };
		VpIndicator::Indicator ind;
		AnalysisLane lane;
		VpLaneProducts::HalfLumaProducer p1;
		VpLaneProducts::SourceStatsProducer p4;
		FakeOfaProducer p2;
		std::vector<uint16_t> store;
		std::atomic<int> p2Live{ 0 };

		LaggedLaneRig()
			: reg(BuildLaggedRegistry(2))
			, ind(7, 256)
			, lane(&ind, [this]() { return clock.fetch_add(3) + 3; })
			, p1([this]() { return clock.fetch_add(3) + 3; })
			, p4(Dims{ kW, kH }, [this]() { return clock.fetch_add(3) + 3; })
			, store(16 * kW * kH)
		{
			consumers.push_back(MakeConsumer(1, { kP4SourceStats, kP2VectorPair }));
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

			uint32_t x = 12345u;
			for (size_t i = 0; i < store.size(); ++i)
			{
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 0xFFFFu);
			}

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
			lane.SetLiveFill([this](const Node&, uint64_t, int, VpAncestry::Header&, uint32_t) {
				p2Live.fetch_add(1, std::memory_order_relaxed);
				return true;
			});
		}

		static Registry BuildLaggedRegistry(uint8_t lag)
		{
			std::vector<ProductDecl> d = FirstProducts().All();
			d[kP2VectorPair].sealLag = lag;
			return Registry(std::move(d));
		}

		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % 16) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			return v;
		}

		void ConfigureDeterministic()
		{
			LaneSettings s;
			s.mode = LaneSettings::Mode::On;
			lane.Configure(s, reg, consumers, gs, card, tier, 0, true);
		}

		void PumpCpu()
		{
			for (int i = 0; i < 100 && lane.StepWorker(Backend::Cpu, 1000); ++i) {}
		}

		void End(uint64_t s)
		{
			lane.OnFrameRetired(s, 0);
			lane.OnFrameTiming(s, 10.0, 1.0, false);
		}

		static const ProductFrameResult& ResultFor(const std::vector<ProductFrameResult>& res, ProductId id)
		{
			for (const ProductFrameResult& r : res)
				if (r.id == id)
					return r;
			Assert::Fail(L"product missing from OnRenderStart results");
			return res[0];
		}
	};

	TEST_CLASS(P2LaneFeedTests)
	{
	public:
		TEST_METHOD(OfferPublishesAheadOfaWhenInputsExistAndHonoursStampWithLateInvalidationControl)
		{
			FeedRig rig;
			Assert::IsTrue(rig.PublishP1(1));
			std::shared_ptr<P2LaneFeed> feed = rig.MakeFeed();

			Assert::IsTrue(feed->Offer(Pair(1)) == Outcome::Published);
			const SlotSet* p2Set = rig.table->Find(kP2VectorPair);
			Assert::IsNotNull(p2Set);
			const ReadInfo ri = p2Set->Read(1, true);
			Assert::IsTrue(ri.kind == ReadInfo::Ring && ri.header != nullptr, L"P2(1) published in ring slot");
			Assert::IsTrue(ri.header->prov == VpAncestry::Provenance::AheadOfa, L"provenance must be AheadOfa");

			VpIndicator::Stamp honoured;
			Assert::IsTrue(p2Set->HonouredOf(1, &honoured));
			const VpIndicator::Stamp atPublish = rig.indicator->StampOf(1);
			Assert::IsTrue(honoured == atPublish, L"hdr.honoured equals StampOf(to) at publish");

			// Negative control 1: duplicate offer of the already-published frame is Stale (never reclaims).
			Assert::IsTrue(feed->Offer(Pair(1)) == Outcome::Stale);
			const P2LaneFeed::Counters c = feed->Read();
			Assert::AreEqual(uint64_t(2), c.offered);
			Assert::AreEqual(uint64_t(1), c.published);
			Assert::AreEqual(uint64_t(1), c.stale);
			Assert::AreEqual(uint64_t(0), c.pending);

			// Negative control 2 (K1): a continuity event after publish makes the seal reject the slot.
			rig.indicator->Advance(VpIndicator::kContinuity, VpResync::Cause::DriftDrop, 1, 1);
			Assert::IsFalse(honoured == rig.indicator->StampOf(1));
			const std::vector<ProductFrameResult> res = rig.table->Resolve(1, rig.indicator->Seal(1),
				[](const Node&, uint64_t, int, VpAncestry::Header&, uint32_t) { return true; });
			bool sawP2 = false;
			for (const ProductFrameResult& r : res)
			{
				if (r.id == kP2VectorPair)
				{
					sawP2 = true;
					Assert::IsTrue(r.seal.reason == SealReason::StampMismatch,
						L"negative control: post-publish continuity advance must trigger StampMismatch at seal");
				}
			}
			Assert::IsTrue(sawP2);
		}

		TEST_METHOD(MissingInputsParkAndDrainPublishesInOrderAndBoundedRingDropsOldest)
		{
			// Part 1: Parking and FIFO Drain ordering as inputs arrive.
			{
				FeedRig rig;
				std::shared_ptr<P2LaneFeed> feed = rig.MakeFeed();

				Assert::IsTrue(feed->Offer(Pair(1)) == Outcome::Parked);
				Assert::IsTrue(feed->Offer(Pair(2)) == Outcome::Parked);
				Assert::IsTrue(feed->Offer(Pair(3)) == Outcome::Parked);
				Assert::AreEqual(size_t(3), feed->Pending());
				const SlotSet* p2Set = rig.table->Find(kP2VectorPair);
				Assert::IsFalse(p2Set->HonouredOf(1, nullptr), L"nothing claimed while P1 missing");
				Assert::IsFalse(p2Set->HonouredOf(2, nullptr));
				Assert::IsFalse(p2Set->HonouredOf(3, nullptr));

				Assert::AreEqual(size_t(3), feed->Drain(), L"inputs still missing: all 3 stay parked");
				Assert::AreEqual(uint64_t(0), feed->Read().published);

				// Publish P1(1) and P1(2), keeping P1(3) missing.
				Assert::IsTrue(rig.PublishP1(1));
				Assert::IsTrue(rig.PublishP1(2));
				Assert::AreEqual(size_t(1), feed->Drain(), L"pairs 1 and 2 publish; pair 3 stays parked");
				Assert::IsTrue(p2Set->HonouredOf(1, nullptr));
				Assert::IsTrue(p2Set->HonouredOf(2, nullptr));
				Assert::IsFalse(p2Set->HonouredOf(3, nullptr));

				Assert::IsTrue(rig.PublishP1(3));
				Assert::AreEqual(size_t(0), feed->Drain(), L"pair 3 publishes once P1(3) arrives");
				Assert::IsTrue(p2Set->HonouredOf(3, nullptr));

				const P2LaneFeed::Counters c = feed->Read();
				Assert::AreEqual(uint64_t(3), c.offered);
				Assert::AreEqual(uint64_t(3), c.published);
				Assert::AreEqual(uint64_t(0), c.pending);
				Assert::IsTrue(c.retried >= 4);
			}

			// Part 2: Bounded ring (kPendingCap == 16) drops oldest entries and counts overwritten.
			{
				FeedRig rig(6);
				std::shared_ptr<P2LaneFeed> feed = rig.MakeFeed();
				const uint64_t kOverflow = 4;
				for (uint64_t to = 1; to <= P2LaneFeed::kPendingCap + kOverflow; ++to)
					Assert::IsTrue(feed->Offer(Pair(to)) == Outcome::Parked);

				Assert::AreEqual(P2LaneFeed::kPendingCap, feed->Pending());
				Assert::AreEqual(kOverflow, feed->Read().overwritten,
					L"oldest 4 pairs must be counted as overwritten");

				// Publish P1(1..5): pairs 1..4 were overwritten (evicted), pair 5 survived as the new oldest!
				for (uint64_t seq = 1; seq <= 5; ++seq)
					Assert::IsTrue(rig.PublishP1(seq));
				Assert::AreEqual(P2LaneFeed::kPendingCap - 1u, feed->Drain());

				const SlotSet* p2Set = rig.table->Find(kP2VectorPair);
				for (uint64_t evicted = 1; evicted <= kOverflow; ++evicted)
					Assert::IsFalse(p2Set->HonouredOf(evicted, nullptr),
						L"overwritten oldest pairs 1..4 must not be published");
				Assert::IsTrue(p2Set->HonouredOf(5, nullptr),
					L"pair 5 (oldest survivor) must be published on Drain");
			}
		}

		TEST_METHOD(GenerationGuardNoLeaseAndZeroRenderWaitBusyCounters)
		{
			// 1. Generation mismatch drops visibly + matching generation negative control.
			{
				FeedRig rig;
				Assert::IsTrue(rig.PublishP1(1));
				uint64_t currentGen = 5;
				std::shared_ptr<P2LaneFeed> feed = rig.MakeFeed([&currentGen]() { return currentGen; });

				Assert::IsTrue(feed->Offer(Pair(1, 4)) == Outcome::GenerationMoved);
				Assert::AreEqual(uint64_t(1), feed->Read().generationDropped);
				Assert::IsFalse(rig.table->Find(kP2VectorPair)->HonouredOf(1, nullptr),
					L"stale generation must not publish into slot");

				// Negative control on exact same frame/inputs: matching generation succeeds.
				Assert::IsTrue(feed->Offer(Pair(1, 5)) == Outcome::Published);
				Assert::IsTrue(rig.table->Find(kP2VectorPair)->HonouredOf(1, nullptr));
				Assert::AreEqual(uint64_t(1), feed->Read().published);
			}

			// 2. NoLease counted visibly and never parked.
			{
				P2LaneFeed noLane([]() { return LaneLease(); }, P2LaneFeed::GenerationFn());
				Assert::IsTrue(noLane.Offer(Pair(1)) == Outcome::NoLease);
				const P2LaneFeed::Counters c = noLane.Read();
				Assert::AreEqual(uint64_t(1), c.noLease);
				Assert::AreEqual(uint64_t(0), c.pending);
			}

			// 3. Zero-render-wait try-lock protocol: held claim lock causes Offer to park and Drain/TryLockClaim to count renderTryBusy.
			{
				FeedRig rig;
				Assert::IsTrue(rig.PublishP1(1));
				std::shared_ptr<P2LaneFeed> feed = rig.MakeFeed();

				std::unique_lock<std::mutex> held = feed->TryLockClaim();
				Assert::IsTrue(held.owns_lock());

				// Nested TryLockClaim while held must not block and must increment renderTryBusy.
				std::unique_lock<std::mutex> second = feed->TryLockClaim();
				Assert::IsFalse(second.owns_lock());
				Assert::AreEqual(uint64_t(1), feed->Read().renderTryBusy);

				// Offer while claim lock is held must park immediately without blocking.
				Assert::IsTrue(feed->Offer(Pair(1)) == Outcome::Parked);
				Assert::AreEqual(size_t(1), feed->Pending());

				// Drain while claim lock is held must return immediately without blocking and increment renderTryBusy.
				Assert::AreEqual(size_t(1), feed->Drain());
				Assert::AreEqual(uint64_t(2), feed->Read().renderTryBusy);

				// Releasing the claim lock lets Drain publish the parked pair.
				held.unlock();
				Assert::AreEqual(size_t(0), feed->Drain());
				Assert::AreEqual(uint64_t(1), feed->Read().published);
				Assert::IsTrue(rig.table->Find(kP2VectorPair)->HonouredOf(1, nullptr));
			}
		}

		TEST_METHOD(ConcurrentBackThreadOfferAndRenderThreadDrainWithSinkTeardown)
		{
			// Repeated concurrent BACK-thread Offer (via VectorEngine::SetP2PublishSink) + render-thread
			// Drain / TryLockClaim + mid-stream SetP2PublishSink({}) teardown.
			constexpr int kCycles = 4;
			constexpr uint64_t kPairsPerCycle = 24;
			uint64_t totalOffered = 0;
			uint64_t totalPublished = 0;

			for (int cycle = 0; cycle < kCycles; ++cycle)
			{
				FeedRig rig(6);
				VpMotion::VectorEngine engine;
				std::shared_ptr<P2LaneFeed> feed = rig.MakeFeed([&engine]() { return engine.P2SlotGeneration(); });
				std::weak_ptr<P2LaneFeed> weakFeed = feed;
				engine.SetP2PublishSink([weakFeed](const VpMotion::P2Published& p) {
					if (const std::shared_ptr<P2LaneFeed> f = weakFeed.lock())
						(void)f->Offer(p);
				});

				std::atomic<bool> start{ false };
				std::atomic<uint64_t> backPublished{ 0 };
				const uint64_t gen = engine.P2SlotGeneration();
				const MotionAhead::FlowPtr flow = MakeUsableFlow();

				std::thread backThread([&]() {
					while (!start.load(std::memory_order_acquire))
						std::this_thread::yield();
					for (uint64_t to = 1; to <= kPairsPerCycle; ++to)
					{
						if (engine.p2Slot->Publish(to - 1, to, gen, flow))
							backPublished.fetch_add(1, std::memory_order_release);
						if ((to & 3u) == 0u)
							std::this_thread::yield();
					}
				});

				start.store(true, std::memory_order_release);
				// Render thread concurrently publishes P1, drains parked pairs, exercises TryLockClaim, and retires old frames.
				for (uint64_t s = 1; s <= kPairsPerCycle; ++s)
				{
					(void)rig.PublishP1(s);
					if ((s & 1u) == 0u)
					{
						std::unique_lock<std::mutex> lock = feed->TryLockClaim();
						if (lock.owns_lock())
							std::this_thread::yield();
					}
					(void)feed->Drain();
					if (s >= 4)
					{
						const uint64_t sealSeq = s - 3;
						rig.table->Resolve(sealSeq, rig.indicator->Seal(sealSeq),
							[](const Node&, uint64_t, int, VpAncestry::Header&, uint32_t) { return true; });
						for (size_t i = 0; i < rig.table->Count(); ++i)
							rig.table->At(i).Release(sealSeq);
					}
				}

				// Teardown sink while worker may still be finishing (must drain in-flight observer without deadlock/UAF).
				if ((cycle & 1) == 0)
				{
					engine.SetP2PublishSink({});
					backThread.join();
				}
				else
				{
					backThread.join();
					engine.SetP2PublishSink({});
				}

				// Final drain on render thread after worker joined.
				(void)feed->Drain();
				const P2LaneFeed::Counters c = feed->Read();
				// Exact conservation accounting invariant once quiet:
				// every offered pair either resolved (published/stale/noLease/generationDropped), was overwritten, or remains pending.
				const uint64_t accounted = c.published + c.stale + c.noLease + c.generationDropped + c.overwritten + c.pending;
				Assert::AreEqual(c.offered, accounted,
					L"P2LaneFeed exact conservation invariant must hold after concurrent stress + teardown");
				Assert::IsTrue(c.offered <= backPublished.load(std::memory_order_acquire));
				totalOffered += c.offered;
				totalPublished += c.published;
			}

			Assert::IsTrue(totalOffered > 0, L"concurrent stress must exercise real BACK-thread Offer calls");
			Assert::IsTrue(totalPublished > 0, L"concurrent stress must publish pairs through Offer/Drain");
		}

		TEST_METHOD(DeterministicFinalSealSkipsNoJobFrameAndWaitsForInFlightJob)
		{
			// X1 SHOULD: P2 at sealLag = 2 in a deterministic run with a held OFA job.
			LaggedLaneRig r;
			r.p2.hold = true;
			r.ConfigureDeterministic();
			Assert::IsTrue(r.lane.Active() && r.lane.Deterministic());
			const Node* p2Node = r.lane.State()->table->NodeOf(kP2VectorPair);
			Assert::IsTrue(p2Node != nullptr && p2Node->sealLag == 2u, L"P2 configured with sealLag == 2");

			const SlotSet& p2Set = r.lane.State()->table->At(size_t(r.lane.State()->IndexOf(kP2VectorPair)));

			r.lane.OnSourceFrame(r.View(0));
			// Frames s = 0, 1: lagged product does not wait at N; P2(0) cannot start (no P1(-1)), so OFA claims P2(1) and holds it.
			for (uint64_t s = 0; s < 2; ++s)
			{
				r.lane.OnSourceFrame(r.View(s + 1));
				r.PumpCpu();
				Assert::AreEqual(uint32_t(0), r.lane.DeterministicWaitUs(s, 4000),
					L"no deterministic wait at N for lagged product");
				Assert::IsTrue(LaggedLaneRig::ResultFor(r.lane.OnRenderStart(s, 4000), kP2VectorPair).pending);
				r.End(s);
				for (int i = 0; i < 4; ++i)
					r.lane.StepWorker(Backend::Ofa, 1000);
			}

			// Arrive frame 3 and prepare for render start s = 2 (which finalises P2(0), whose inputs can never arrive).
			r.lane.OnSourceFrame(r.View(3));
			r.PumpCpu();
			Assert::IsFalse(r.lane.State()->runner->HasActiveJobFor(kP2VectorPair, 0),
				L"precondition: LaneRunner has no active job for P2(0)");
			Assert::IsTrue(p2Set.StateOf(0) != SlotState::Writing,
				L"precondition: P2(0) slot is not Writing");

			// X1 SHOULD assertion: finalising P2(0) with no active job skips waiting completely with 0 missed bounds.
			Assert::AreEqual(uint32_t(0), r.lane.DeterministicWaitUs(2, 4000),
				L"X1 SHOULD: final-seal wait must skip no-job frame P2(0)");
			Assert::AreEqual(uint64_t(0), r.lane.MissedBounds(),
				L"X1 SHOULD: skipping no-job frame P2(0) must record 0 missed bounds");

			Assert::IsTrue(LaggedLaneRig::ResultFor(r.lane.OnRenderStart(2, 4000), kP2VectorPair).pending);
			r.End(2);
			for (int i = 0; i < 4; ++i)
				r.lane.StepWorker(Backend::Ofa, 1000);

			// Arrive frame 4 and prepare for render start s = 3 (finalising P2(1), whose OFA job IS held in Writing).
			r.lane.OnSourceFrame(r.View(4));
			r.PumpCpu();
			Assert::IsTrue(r.lane.State()->runner->HasActiveJobFor(kP2VectorPair, 1),
				L"negative-control precondition: LaneRunner holds active job for P2(1)");
			Assert::IsTrue(p2Set.StateOf(1) == SlotState::Writing,
				L"negative-control precondition: P2(1) slot is Writing");

			// In-test negative control on the EXACT SAME rig: deterministic final-seal wait MUST fire when a real job IS active!
			const uint32_t firstWait = r.lane.DeterministicWaitUs(3, 4000);
			Assert::IsTrue(firstWait > 0u,
				L"negative control: final-seal wait MUST fire for in-flight P2(1) job");

			// Release the held OFA job and verify the wait completes cleanly with 0 missed bounds and P2(1) accepted late.
			r.p2.hold = false;
			int steps = 0;
			while (r.lane.DeterministicWaitUs(3, 4000) > 0u)
			{
				Assert::IsTrue(++steps < 20, L"wait must settle once OFA finishes");
				r.lane.StepWorker(Backend::Ofa, 1000);
			}
			Assert::AreEqual(uint64_t(0), r.lane.MissedBounds(), L"completed in-flight job must not miss bound");
			Assert::IsTrue(p2Set.StateOf(1) == SlotState::Published);

			const int liveBefore = r.p2Live.load(std::memory_order_relaxed);
			r.lane.OnRenderStart(3, 4000);
			Assert::AreEqual(uint64_t(1), p2Set.Counters().sealLate.load(), L"P2(1) accepted late from lane");
			Assert::AreEqual(uint64_t(0), p2Set.Counters().sealLiveFilled.load());
			Assert::AreEqual(liveBefore, r.p2Live.load(std::memory_order_relaxed), L"P2(1) was not live-filled");
		}
	};
}
