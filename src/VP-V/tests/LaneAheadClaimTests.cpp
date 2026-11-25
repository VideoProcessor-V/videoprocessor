// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// QKP slice C (design-queue-keyed-products s2.4 / s9.4): the lane CLAIM for a frame ahead of the render frame (AnalysisLane::RunAheadNow).
//   LANE-T1  RunAheadNowThenWorkerSkips        the render thread makes N+1 into its ring slot, the worker skips it (workerReused), the seal accepts it
//   LANE-T2  WorkerRunningGivesLanePending     a frame a worker is writing is Claimed (no wait, no second compute); the claim race has one winner
//   LANE-T3  MissingInputSealCountedReduced    the seal compute in a declared reduced form is its own ledger line
//   plus:    beyond the lane's slots / not a node / an input not made are visible Declined / NotNode, never LanePending
//   MUST-3   AheadDeclineOnRejectedStoreLeavesTheWaitCountToTheWorker  the ahead path never writes the lane thread's wait dedupe entries
// The GPU-fake form of LANE-T1 and the GPU "not at a GPU-legal phase" refusal (design s9.4 d) are NOT covered here (no GPU lane rig in
// this file): RunAheadNow declines every GPU product with that text; the Entrance-phase dispatch is a deferred row (102b).
// CPU only, single thread except the one claim race, bounded.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LaneAheadClaimTestsNs
{
	using namespace VpSkeleton;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;

	// A minimal lane rig on the real lane (copied from SkeletonAcquireTests: FirstProducts, CPU producers P1 + P4).
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
		// A CPU product with a lag-1 input (P5 id, declared like P4 plus P1 lag 1, missing-input form): frame 0 seals with input 1
		// missing. Its law marks byte 0 with whether the previous half luma was present.
		void AddLaggedCpuProduct()
		{
			std::vector<ProductDecl> d = FirstProducts().All();
			ProductDecl p = d[kP4SourceStats];
			p.id = kP5MonitorTiles;
			p.token = "p5_test_lagged";
			p.name = "test lagged stats";
			detail::AddInput(p, kP1HalfLuma, 1);
			p.missingInputForm = true;
			d[kP5MonitorTiles] = p;
			reg = Registry(d);
			consumers.clear();
			ConsumerDecl c;
			c.id = 2;
			AddConsume(c, kP5MonitorTiles);
			consumers.push_back(c);
			lane.RegisterCpuLiveCompute(kP5MonitorTiles,
				[](const Node&, const SourceFrameView*, const std::array<const uint8_t*, kMaxInputs>& in, uint8_t* dst, size_t n) {
					if (in[0] == nullptr || !VpLaneProducts::ComputeSourceStats(in[0], Dims{ kW, kH }, dst, n))
						return false;
					dst[0] = in[1] != nullptr ? uint8_t(0x55) : uint8_t(0xAA);
					return true;
				});
		}
		// P1 declared a SINGLE STORE (history 0, ungoverned, no live pool); P4 stays its lag-0 reader (a single store is read at lag 0
		// or 1). Used by the MUST-3 test: a Rejected store frame never gets content, so its reader's wait is counted (storeWaitRejected).
		void MakeHalfLumaASingleStore()
		{
			std::vector<ProductDecl> d = FirstProducts().All();
			d[kP1HalfLuma].history = 0;
			d[kP1HalfLuma].historyFloor = kHistoryFixed;
			d[kP1HalfLuma].singletonStore = true;
			reg = Registry(d);
		}
		bool Configure()
		{
			LaneSettings s;
			s.mode = LaneSettings::Mode::On;
			lane.Configure(s, reg, consumers, gs, card, tier, 0, false);
			return lane.Active();
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

	bool Has(const char* s, const char* part) { return std::string(s).find(part) != std::string::npos; }

	TEST_CLASS(LaneAheadClaimTests)
	{
	public:
		// LANE-T1: the render thread claims and makes frame 1 (P1, then P4 through the pinned P1) ahead of its seal; the worker that later
		// reaches frame 1 finds the slot taken and counts WorkerReused (nothing recomputed, nothing overwritten); the seal accepts both.
		TEST_METHOD(RunAheadNowThenWorkerSkips)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			for (uint64_t s = 0; s < 3; ++s)
				r.lane.OnSourceFrame(r.View(s));
			r.lane.OnRenderStart(0, 4000);   // the render state exists; frame 0 sealed (nothing pumped: all of it live)
			const AnalysisLane::AheadRunResult p1 = r.lane.RunAheadNow(kP1HalfLuma, 1);
			Assert::IsTrue(p1.kind == AnalysisLane::AheadRun::Ran, L"P1 of frame 1 made now");
			const AnalysisLane::AheadRunResult p4 = r.lane.RunAheadNow(kP4SourceStats, 1);
			Assert::IsTrue(p4.kind == AnalysisLane::AheadRun::Ran, L"P4 of frame 1 made now through the pinned P1");
			Assert::IsTrue(r.SlotBytes(kP1HalfLuma, 1) == r.ExpectedHalf(1), L"P1 = the one-shot law, in its ring slot");
			Assert::IsTrue(r.SlotBytes(kP4SourceStats, 1) == r.ExpectedStats(1), L"P4 = the one-shot law, in its ring slot");
			Assert::IsTrue(r.lane.AheadMadeByRender() == 2u, L"counted");
			const AnalysisLane::AheadRunResult again = r.lane.RunAheadNow(kP1HalfLuma, 1);
			Assert::IsTrue(again.kind == AnalysisLane::AheadRun::Ran && r.lane.AheadMadeByRender() == 2u && r.lane.AheadAlreadyMade() == 1u,
				L"a second call reuses the made frame, computes nothing");
			r.Pump();
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(st != nullptr, L"state");
			Assert::IsTrue(st->runner->Counters(kP1HalfLuma).workerReused.load() == 1u, L"the worker skipped P1 of frame 1 (reused)");
			Assert::IsTrue(st->runner->Counters(kP4SourceStats).workerReused.load() == 1u, L"the worker skipped P4 of frame 1 (reused)");
			Assert::IsTrue(r.SlotBytes(kP4SourceStats, 1) == r.ExpectedStats(1), L"the worker did not overwrite it");
			r.lane.OnRenderStart(1, 4000);
			Assert::IsTrue(r.lane.AheadSealReused() == 2u, L"the seal of frame 1 accepted both made frames");
			Assert::IsTrue(r.SlotBytes(kP4SourceStats, 1) == r.ExpectedStats(1), L"sealed bytes bit-identical");
		}

		// GEM-W1A S2 (skeleton MUST-1): an AHEAD run reads its lag-0 input in the Published state (made by a worker, not yet Sealed: the
		// seal of that frame has not run). P4 of frame 1 is made now through the worker's published P1, which is not computed again.
		TEST_METHOD(AnAheadRunReadsAPublishedInput)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			for (uint64_t s = 0; s < 3; ++s)
				r.lane.OnSourceFrame(r.View(s));
			r.lane.OnRenderStart(0, 4000);   // the render state exists; frame 1 is not sealed
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(st != nullptr && st->IndexOf(kP1HalfLuma) >= 0, L"state");
			const size_t p1 = size_t(st->IndexOf(kP1HalfLuma));
			SlotSet& set = st->table->At(p1);
			LaneTicket t;
			Assert::IsTrue(set.Claim(1, Provenance::AheadLane, ~uint64_t(0), &t) == ClaimResult::Claimed, L"a worker owns P1 of frame 1");
			Assert::IsTrue(set.Writable(t), L"writable");
			const std::vector<uint8_t> half = r.ExpectedHalf(1);
			Assert::IsTrue(st->bufs[p1] && st->bufs[p1]->Bytes() == half.size(), L"P1 buffer");
			set.LaneHeader(t).honoured = r.ind.StampOf(1);   // as a worker stamps it
			std::memcpy(st->bufs[p1]->Ring(size_t(t.slot)), half.data(), half.size());
			Assert::IsTrue(set.Publish(t) == PublishResult::Published, L"the worker published P1 of frame 1");
			Assert::IsTrue(set.StateOf(1) == SlotState::Published, L"Published, not Sealed");
			const AnalysisLane::AheadRunResult p4 = r.lane.RunAheadNow(kP4SourceStats, 1);
			Assert::IsTrue(p4.kind == AnalysisLane::AheadRun::Ran, L"P4 of frame 1 made now through the Published P1");
			// P4 of frame 1 is itself Published (frame 1 is not sealed): direct SlotSet::Read(1, false) answers None (negative control),
			// while SlotSet::Read(1, true) and AnalysisLane::ReadOf(kP4SourceStats, 1) (ahead CPU read: seq > renderSeq_) both read its ring slot.
			Assert::IsTrue(st->IndexOf(kP4SourceStats) >= 0, L"P4 node");
			const size_t p4i = size_t(st->IndexOf(kP4SourceStats));
			SlotSet& set4 = st->table->At(p4i);
			Assert::IsTrue(set4.StateOf(1) == SlotState::Published, L"P4 of frame 1 Published by the ahead run");
			Assert::IsTrue(set4.Read(1, false).kind == ReadInfo::None, L"negative control: non-ahead SlotSet::Read refuses unsealed Published slot");
			const ReadInfo ri4 = set4.Read(1, true);
			Assert::IsTrue(ri4.kind == ReadInfo::Ring && st->bufs[p4i], L"P4 in its ring slot");
			const uint8_t* p4bytes = st->bufs[p4i]->Ring(size_t(ri4.index));
			const std::vector<uint8_t> made(p4bytes, p4bytes + st->bufs[p4i]->Bytes());
			Assert::IsTrue(made == r.ExpectedStats(1), L"P4 = the one-shot law over the published input");
			const AnalysisLane::ProductRead pr4 = r.lane.ReadOf(kP4SourceStats, 1);
			Assert::IsTrue(pr4.state == AnalysisLane::ProductRead::Ring && pr4.cpu != nullptr && pr4.cpuBytes == st->bufs[p4i]->Bytes(),
				L"ReadOf serves Published ahead CPU frame (seq > renderSeq_) from its ring slot");
			const std::vector<uint8_t> readOfBytes(pr4.cpu, pr4.cpu + pr4.cpuBytes);
			Assert::IsTrue(readOfBytes == r.ExpectedStats(1), L"ReadOf bytes match the one-shot law over the published input");
			Assert::IsTrue(r.lane.AheadMadeByRender() == 1u, L"only P4 was made: the published P1 was read, not recomputed");
		}

		// LANE-T2: a frame a worker is writing is Claimed (the caller reports LanePending: visible, no wait, no second compute); the claim is
		// a compare-exchange on the slot word, so a racing worker and render thread never both own a frame.
		TEST_METHOD(WorkerRunningGivesLanePending)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			for (uint64_t s = 0; s < 3; ++s)
				r.lane.OnSourceFrame(r.View(s));
			r.lane.OnRenderStart(0, 4000);
			const std::shared_ptr<LaneState> st = r.lane.State();
			SlotSet& set = st->table->At(size_t(st->IndexOf(kP1HalfLuma)));
			LaneTicket t;
			Assert::IsTrue(set.Claim(1, Provenance::AheadLane, ~uint64_t(0), &t) == ClaimResult::Claimed, L"a worker owns frame 1");
			const AnalysisLane::AheadRunResult res = r.lane.RunAheadNow(kP1HalfLuma, 1);
			Assert::IsTrue(res.kind == AnalysisLane::AheadRun::Claimed, L"Claimed, not a second compute");
			Assert::IsTrue(Has(res.why, "worker") && r.lane.AheadMadeByRender() == 0u && r.lane.AheadClaimedByWorker() == 1u, L"named, counted");
			Assert::IsTrue(set.Abandon(t), L"worker lets go");

			// The race: both sides claim the same fresh frame; exactly one wins and the loser reports it (never both own it).
			int renderWon = 0, workerWon = 0;
			for (int i = 0; i < 24; ++i) {
				LaneRig q;
				Assert::IsTrue(q.Configure(), L"built");
				for (uint64_t s = 0; s < 3; ++s)
					q.lane.OnSourceFrame(q.View(s));
				q.lane.OnRenderStart(0, 4000);
				const std::shared_ptr<LaneState> qs = q.lane.State();
				SlotSet& qset = qs->table->At(size_t(qs->IndexOf(kP1HalfLuma)));
				std::atomic<int> go{ 0 };
				std::atomic<bool> workerClaimed{ false };
				LaneTicket wt;
				std::thread worker([&]() {
					while (go.load() == 0) {}
					workerClaimed = qset.Claim(1, Provenance::AheadLane, ~uint64_t(0), &wt) == ClaimResult::Claimed;
				});
				go = 1;
				const AnalysisLane::AheadRunResult rr = q.lane.RunAheadNow(kP1HalfLuma, 1);
				worker.join();
				const bool renderMade = rr.kind == AnalysisLane::AheadRun::Ran && q.lane.AheadMadeByRender() == 1u;
				Assert::IsTrue(renderMade != workerClaimed.load(), L"exactly one owner of the frame");
				if (workerClaimed.load()) {
					Assert::IsTrue(rr.kind == AnalysisLane::AheadRun::Claimed, L"the loser is told Claimed");
					qset.Abandon(wt);
					++workerWon;
				} else {
					++renderWon;
				}
			}
			Assert::IsTrue(renderWon + workerWon == 24, L"every round had one winner");
		}

		// LANE-T3: a seal compute with an input missing (the declared reduced form) is its own ledger line, apart from the full-form seal.
		TEST_METHOD(MissingInputSealCountedReduced)
		{
			LaneRig r;
			r.AddLaggedCpuProduct();
			Assert::IsTrue(r.Configure(), L"built");
			r.lane.OnSourceFrame(r.View(0));
			Assert::IsTrue(r.lane.SealComputedReduced() == 0u, L"none before the seal");
			r.lane.OnRenderStart(0, 4000);
			Assert::IsTrue(r.lane.SealComputedReduced() == 1u, L"P5 at frame 0: its lag-1 input missing, sealed in the reduced form (P1 and P4 are full)");
		}

		// Visible refusals: never LanePending, never a silent no-op.
		TEST_METHOD(DeclinedAndNotNodeAreNamed)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			r.lane.OnSourceFrame(r.View(0));
			Assert::IsTrue(r.lane.RunAheadNow(kP1HalfLuma, 1).kind == AnalysisLane::AheadRun::NotNode, L"no render state before the first seal");
			r.lane.OnRenderStart(0, 4000);
			const AnalysisLane::AheadRunResult beyond = r.lane.RunAheadNow(kP1HalfLuma, 1000);
			Assert::IsTrue(beyond.kind == AnalysisLane::AheadRun::Declined && Has(beyond.why, "beyond the lane's slots"), L"beyond the lane's slots (9.4 a)");
			Assert::IsTrue(r.lane.AheadBeyondSlots() == 1u, L"counted");
			Assert::IsTrue(r.lane.RunAheadNow(kP10GrainSplit, 1).kind == AnalysisLane::AheadRun::NotNode, L"not a node of this graph");
			const AnalysisLane::AheadRunResult noInput = r.lane.RunAheadNow(kP4SourceStats, 2);
			Assert::IsTrue(noInput.kind == AnalysisLane::AheadRun::Declined && Has(noInput.why, "input"), L"P4 with its P1 not made: input named");
		}

		// MUST-3 (opus audit qkp-bce): the ahead path (render thread) never writes the lane thread's wait dedupe entries. A reader of a
		// single-store product whose frame is Rejected: RunAheadNow declines (named) and counts nothing; the lane worker's own
		// LaneAncestors call (the default countWaits) then counts the wait exactly once, however often it polls.
		TEST_METHOD(AheadDeclineOnRejectedStoreLeavesTheWaitCountToTheWorker)
		{
			LaneRig r;
			r.MakeHalfLumaASingleStore();
			Assert::IsTrue(r.Configure(), L"built");
			for (uint64_t s = 0; s < 3; ++s)
				r.lane.OnSourceFrame(r.View(s));
			r.lane.OnRenderStart(0, 4000);   // the render state exists
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(st != nullptr, L"state");
			Assert::IsTrue(st->IndexOf(kP1HalfLuma) >= 0 && st->IndexOf(kP4SourceStats) >= 0, L"both are lane nodes");
			SlotSet& store = st->table->At(size_t(st->IndexOf(kP1HalfLuma)));
			// P1(1) is published, then its seal REFUSES it: a single store has no live fill, so the frame never gets content.
			LaneTicket t;
			Assert::IsTrue(store.Claim(1, Provenance::AheadLane, ~uint64_t(0), &t) == ClaimResult::Claimed, L"store frame 1 claimed");
			Assert::IsTrue(store.Writable(t), L"writable");
			Assert::IsTrue(store.Publish(t) == PublishResult::Published, L"store frame 1 published");
			Assert::IsTrue(store.Seal(1, [](const Header&) { return SealReason::StampMismatch; }).outcome == SealOutcome::NeedsLive, L"the seal refuses it");
			Assert::IsTrue(store.RejectedAwaitingLive(1), L"Rejected, awaiting a live fill that never comes");
			const uint64_t before = store.Counters().storeWaitRejected.load();
			for (int poll = 0; poll < 3; ++poll) {
				const AnalysisLane::AheadRunResult res = r.lane.RunAheadNow(kP4SourceStats, 1);
				Assert::IsTrue(res.kind == AnalysisLane::AheadRun::Declined && Has(res.why, "input"), L"the ahead call declines, named");
			}
			Assert::AreEqual(before, store.Counters().storeWaitRejected.load(), L"the ahead path counts nothing (it never writes the dedupe entries)");
			Assert::IsTrue(r.lane.AheadMadeByRender() == 0u, L"nothing made");
			Header scratch;
			LaneInputs pins;
			for (int poll = 0; poll < 5; ++poll)
				Assert::IsFalse(st->table->LaneAncestors(kP4SourceStats, 1, scratch, pins), L"the lane worker's call (default countWaits) waits");
			Assert::AreEqual(size_t(0), pins.count, L"nothing stays pinned");
			Assert::AreEqual(before + 1u, store.Counters().storeWaitRejected.load(), L"the worker counts the reader frame once, not per poll");
		}

		// K#3 (168): when RunAheadNow finds a claimed slot still pinned (!Writable), Unclaim restores the slot
		// without burning the target frame seq, so after Unpin the same frame can be made and published cleanly.
		TEST_METHOD(PinnedSlotDeclinesRunAheadNowWithoutBurningFrame)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			const std::shared_ptr<LaneState> st = r.lane.State();
			Assert::IsTrue(st != nullptr && st->IndexOf(kP1HalfLuma) >= 0, L"P1 node");
			SlotSet& set = st->table->At(size_t(st->IndexOf(kP1HalfLuma)));
			const uint64_t R = set.Ring();

			// Publish frame 0 into ring slot 0 via worker, then seal frame 0 so slot 0 holds Sealed frame 0.
			r.lane.OnSourceFrame(r.View(0));
			r.Pump();
			r.lane.OnRenderStart(0, 4000);
			Assert::IsTrue(set.StateOf(0) == SlotState::Sealed, L"frame 0 sealed in ring slot 0");

			// Pin frame 0 in ring slot 0, then retire frame 0 so slot 0 becomes Released(0) while pinned.
			AncestorRef ref;
			int liveIdx = -2;
			Assert::IsTrue(set.PinRef(0, &ref, &liveIdx), L"pin frame 0 in ring slot 0");
			Assert::AreEqual(-1, liveIdx, L"frame 0 resides in ring slot 0, not a live buffer");
			r.lane.OnFrameRetired(0, 0);
			Assert::IsTrue(set.StateOf(0) == SlotState::Released, L"frame 0 released while still pinned");

			// Deliver source frame R (maps to slot 0): RunAheadNow claims slot 0, sees !Writable, and Unclaims cleanly.
			r.lane.OnSourceFrame(r.View(R));
			const AnalysisLane::AheadRunResult declined = r.lane.RunAheadNow(kP1HalfLuma, R);
			Assert::IsTrue(declined.kind == AnalysisLane::AheadRun::Declined && Has(declined.why, "pinned"),
				L"RunAheadNow declines while slot 0 is pinned by frame 0 reader");
			Assert::IsTrue(set.StateOf(R) != SlotState::Abandoned,
				L"K#3 (168): declined RunAheadNow MUST NOT leave frame R Abandoned");
			Assert::IsTrue(set.StateOf(0) == SlotState::Released,
				L"K#3 (168): Unclaim restored slot 0 back to Released(0)");

			// Unpin frame 0 -> RunAheadNow(kP1HalfLuma, R) now succeeds and publishes bit-identical bytes.
			set.Unpin(0, liveIdx);
			const AnalysisLane::AheadRunResult ok = r.lane.RunAheadNow(kP1HalfLuma, R);
			Assert::IsTrue(ok.kind == AnalysisLane::AheadRun::Ran,
				L"K#3 (168): after Unpin, RunAheadNow(R) succeeds (frame R was not burned)");
			Assert::IsTrue(set.StateOf(R) == SlotState::Published, L"frame R Published in slot 0");
			Assert::IsTrue(r.SlotBytes(kP1HalfLuma, R) == r.ExpectedHalf(R),
				L"frame R bytes match one-shot law");

			// In-test negative control: pre-fix Abandon(t) on a pinned slot burns frame R (Stale after Unpin).
			SlotSet neg(kP1HalfLuma, uint16_t(R), 2);
			const auto acceptAll = [](const Header&) { return SealReason::Accepted; };
			LaneTicket t0;
			Assert::IsTrue(neg.Claim(0, Provenance::AheadLane, ~uint64_t(0), &t0) == ClaimResult::Claimed);
			Assert::IsTrue(neg.StartWriting(t0));
			Assert::IsTrue(neg.Publish(t0) == PublishResult::Published);
			Assert::IsTrue(neg.Seal(0, acceptAll).outcome == SealOutcome::Sealed);
			Assert::IsTrue(neg.PinRef(0, nullptr, nullptr));
			Assert::IsTrue(neg.Release(0, 0));
			LaneTicket tR;
			Assert::IsTrue(neg.Claim(R, Provenance::AheadLane, ~uint64_t(0), &tR) == ClaimResult::Claimed);
			Assert::IsFalse(neg.Writable(tR), L"negative control: slot 0 is pinned");
			Assert::IsTrue(neg.Abandon(tR), L"negative control: Abandon instead of Unclaim");
			neg.Unpin(0);
			Assert::IsTrue(neg.Claim(R, Provenance::AheadLane, ~uint64_t(0), nullptr) == ClaimResult::Stale,
				L"negative control: Abandon burns frame R so it returns Stale and can never be published after Unpin");
		}
	};
}
