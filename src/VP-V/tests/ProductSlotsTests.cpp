// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S1 (src/VP-V/core/ProductSlots.h): the slot ownership protocol. ABA-proof claims, the
// seal (lane-owned rejection N3, stamp/span/ancestry validation), live fills for CPU products while
// the lane is mid-chunk, the ancestry cascade, stamp last, late never overwrites, release fences
// (N4), lane input pins, discard, overrun, determinism, and a 10^7 seal/publish stress. CPU only.

#include <VP-V/core/ProductSlots.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ProductSlotsTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Bit;
	using VpIndicator::Indicator;
	using VpIndicator::Stamp;
	using VpResync::Cause;

	constexpr ProductId kA = 0;   // single-frame product (reads settings[analysis] too)
	constexpr ProductId kB = 1;   // pair product: A(N) and A(N-1), span rule on full + continuity
	constexpr ProductId kC = 2;   // reads B(N)
	constexpr uint64_t kNoFence = ~uint64_t(0);

	Registry Chain(bool bMissingForm = true)
	{
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		std::vector<ProductDecl> d;
		d.push_back(detail::Make(kA, "a", "a", Backend::Cpu, 0, Tier::T0, content, Bit(kSettingsAnalysis)));
		d.push_back(detail::Make(kB, "b", "b", Backend::Cpu, 0, Tier::T0, content, 0));
		d.push_back(detail::Make(kC, "c", "c", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0));
		detail::AddInput(d[kB], kA, 0);
		detail::AddInput(d[kB], kA, 1);
		d[kB].spanMask = content;
		d[kB].missingInputForm = bMissingForm;   // B(0) has no A(-1): a declared single-frame form
		detail::AddInput(d[kC], kB, 0);
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	Graph ChainGraph(ProductId top, bool bMissingForm = true)
	{
		ConsumerDecl c;
		c.id = 1;
		AddConsume(c, top);
		GraphSettings gs;
		gs.dims.width = 64;
		gs.dims.height = 64;
		gs.renderAhead = 0;
		gs.laneLead = 1;
		return BuildGraph(Chain(bMissingForm), { c }, gs);
	}

	uint8_t Pattern(uint64_t seq, ProductId p, bool live) { return uint8_t(seq * 31u + p * 7u + (live ? 128u : 0u)); }

	bool Filled(const uint8_t* b, size_t n, uint8_t v)
	{
		for (size_t i = 0; i < n; ++i)
			if (b[i] != v)
				return false;
		return true;
	}

	// The lane and render of one SlotTable, CPU buffers, and an Indicator.
	struct Harness {
		Graph graph;
		SlotTable table;
		Indicator ind;
		std::vector<CpuBuffers> bufs;
		uint32_t declineMask = 0;   // products whose live fill declines
		uint64_t fills = 0;

		explicit Harness(ProductId top, uint32_t era = 11, bool bMissingForm = true)
			: graph(ChainGraph(top, bMissingForm))
			, table(graph)
			, ind(era, 64)
		{
			for (const Node& n : graph.nodes)
				bufs.emplace_back(n);
		}

		size_t IndexOf(ProductId p) const { return size_t(graph.IndexOf(p)); }
		SlotSet& Set(ProductId p) { return *table.Find(p); }
		CpuBuffers& Buf(ProductId p) { return bufs[IndexOf(p)]; }

		// Claim, pin inputs, stamp, write, publish: the whole lane job at once.
		PublishResult Lane(ProductId p, uint64_t seq, ClaimResult* claim = nullptr)
		{
			SlotSet& s = Set(p);
			LaneTicket t;
			const ClaimResult c = s.Claim(seq, Provenance::AheadLane, kNoFence, &t);
			if (claim)
				*claim = c;
			if (c != ClaimResult::Claimed)
				return PublishResult::BadTicket;
			Header& h = s.LaneHeader(t);
			LaneInputs pins;
			if (!table.LaneAncestors(p, seq, h, pins)) {
				s.Abandon(t);
				return PublishResult::BadTicket;
			}
			h.honoured = ind.StampOf(seq);
			std::memset(Buf(p).Ring(t.slot), Pattern(seq, p, false), Buf(p).Bytes());
			const PublishResult r = s.Publish(t);
			pins.Unpin();
			return r;
		}

		const std::vector<ProductFrameResult>& Resolve(uint64_t seq)
		{
			const Stamp st = ind.Seal(seq);
			return table.Resolve(seq, st, [this](const Node& n, uint64_t s, int live, Header&, uint32_t) {
				if ((declineMask >> n.id) & 1u)
					return false;
				++fills;
				std::memset(Buf(n.id).Live(size_t(live)), Pattern(s, n.id, true), Buf(n.id).Bytes());
				return true;
			});
		}

		const ProductFrameResult& Result(const std::vector<ProductFrameResult>& r, ProductId p) const
		{
			return r[IndexOf(p)];
		}

		// The bytes the render would read for frame seq of product p.
		const uint8_t* ReadBytes(ProductId p, uint64_t seq, bool* live)
		{
			const ReadInfo ri = Set(p).Read(seq);
			*live = ri.kind == ReadInfo::Live;
			if (ri.kind == ReadInfo::Ring)
				return Buf(p).Ring(size_t(ri.index));
			if (ri.kind == ReadInfo::Live)
				return Buf(p).Live(size_t(ri.index));
			return nullptr;
		}
	};

	// ---- B3 X1 seal lag: 0 a (lag 0), 1 b (reads a(N), seal lag `lagB`), 2 c (lag 0, reads b(N); missing-input form) ----
	Registry LagRegistry(uint8_t lagB)
	{
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		std::vector<ProductDecl> d;
		d.push_back(detail::Make(kA, "a", "a", Backend::Cpu, 0, Tier::T0, content, 0));
		d.push_back(detail::Make(kB, "b", "b", Backend::Cpu, 0, Tier::T0, content, 0));
		d.push_back(detail::Make(kC, "c", "c", Backend::Cpu, 0, Tier::T0, content, 0));
		detail::AddInput(d[kB], kA, 0);
		d[kB].sealLag = lagB;
		detail::AddInput(d[kC], kB, 0);
		d[kC].missingInputForm = true;
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	GraphSettings LagSettings(QueueMode mode = QueueMode::Normal, uint8_t laneLead = 1)
	{
		GraphSettings gs;
		gs.dims.width = 64;
		gs.dims.height = 64;
		gs.renderAhead = 0;
		gs.laneLead = laneLead;
		gs.queueMode = mode;
		return gs;
	}

	Graph LagGraph(uint8_t lagB, QueueMode mode = QueueMode::Normal, uint8_t laneLead = 1)
	{
		ConsumerDecl c;
		c.id = 1;
		AddConsume(c, kC);
		return BuildGraph(LagRegistry(lagB), { c }, LagSettings(mode, laneLead));
	}

	// The Harness's lane and render over a seal-lag graph; FinalSeal = SlotTable::ResolveLagged with the render stamp (the frame's
	// render still reads it), Retire = ReleaseAfter (the frame is done); Finalize = both.
	struct LagHarness {
		Graph graph;
		SlotTable table;
		Indicator ind;
		std::vector<CpuBuffers> bufs;

		explicit LagHarness(uint8_t lagB, QueueMode mode = QueueMode::Normal, uint8_t laneLead = 1)
			: graph(LagGraph(lagB, mode, laneLead))
			, table(graph)
			, ind(13, 64)
		{
			table.SetStampSource([this](uint64_t s) { return ind.StampOf(s); });
			for (const Node& n : graph.nodes)
				bufs.emplace_back(n);
		}
		size_t IndexOf(ProductId p) const { return size_t(graph.IndexOf(p)); }
		SlotSet& Set(ProductId p) { return *table.Find(p); }
		CpuBuffers& Buf(ProductId p) { return bufs[IndexOf(p)]; }
		bool Lane(ProductId p, uint64_t seq)
		{
			SlotSet& s = Set(p);
			LaneTicket t;
			if (s.Claim(seq, Provenance::AheadLane, kNoFence, &t) != ClaimResult::Claimed)
				return false;
			Header& h = s.LaneHeader(t);
			LaneInputs pins;
			if (!table.LaneAncestors(p, seq, h, pins)) {
				s.Abandon(t);
				return false;
			}
			h.honoured = ind.StampOf(seq);
			std::memset(Buf(p).Ring(t.slot), Pattern(seq, p, false), Buf(p).Bytes());
			const PublishResult r = s.Publish(t);
			pins.Unpin();
			return r == PublishResult::Published;
		}
		bool FillOne(const Node& n, uint64_t s, int live)
		{
			std::memset(Buf(n.id).Live(size_t(live)), Pattern(s, n.id, true), Buf(n.id).Bytes());
			return true;
		}
		const std::vector<ProductFrameResult>& Resolve(uint64_t seq)
		{
			const Stamp st = ind.Seal(seq);
			return table.Resolve(seq, st, [this](const Node& n, uint64_t s, int live, Header&, uint32_t) { return FillOne(n, s, live); });
		}
		const std::vector<ProductFrameResult>& FinalSeal(uint64_t seq)
		{
			const Stamp st = ind.StampOf(seq);
			return table.ResolveLagged(seq, st, [this](const Node& n, uint64_t s, int live, Header&, uint32_t) { return FillOne(n, s, live); });
		}
		void Retire(uint64_t seq) { table.ReleaseAfter(seq); }   // the frame retires after its render (AnalysisLane::OnFrameRetired order)
		const std::vector<ProductFrameResult>& Finalize(uint64_t seq)
		{
			const std::vector<ProductFrameResult>& r = FinalSeal(seq);
			Retire(seq);
			return r;
		}
		const ProductFrameResult& Result(const std::vector<ProductFrameResult>& r, ProductId p) const { return r[IndexOf(p)]; }
	};

	TEST_CLASS(ProductSlotsTests)
	{
	public:
		TEST_METHOD(StateWordPacksAndUnpacks)
		{
			SlotWord w;
			w.seq = kSlotSeqMask - 3;
			w.gen = 0xBEEF;
			w.state = SlotState::ReleasedLaneOwned;
			w.prov = Provenance::LiveFallback;
			const SlotWord u = Unpack(Pack(w));
			Assert::AreEqual(w.seq, u.seq);
			Assert::AreEqual(int(w.gen), int(u.gen));
			Assert::IsTrue(u.state == w.state);
			Assert::IsTrue(u.prov == w.prov);
		}

		TEST_METHOD(OnTimeLaneResultIsSealedAndReadFromTheRing)
		{
			Harness h(kA);
			Assert::AreEqual(3, int(h.Set(kA).Ring()), L"history 0 + 1 + R 0 + L 1 + q 1");
			Assert::IsTrue(h.Lane(kA, 0) == PublishResult::Published);
			const auto& r = h.Resolve(0);
			Assert::IsTrue(h.Result(r, kA).final == SealOutcome::Sealed);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::Accepted);
			Assert::IsFalse(h.Result(r, kA).liveFilled);
			bool live = true;
			const uint8_t* b = h.ReadBytes(kA, 0, &live);
			Assert::IsFalse(live);
			Assert::IsTrue(Filled(b, 64, Pattern(0, kA, false)));
			Assert::IsTrue(h.Set(kA).Read(0).header->prov == Provenance::AheadLane);
			Assert::AreEqual(uint64_t(1), h.Set(kA).Counters().sealed.load());
		}

		TEST_METHOD(StaleClaimCannotMatchAFreedSlotAbaProof)
		{
			Harness h(kA);   // ring 3
			SlotSet& s = h.Set(kA);
			// Frame 2 is sealed with no lane result, live-filled and released.
			h.Resolve(0);
			s.Release(0);
			h.Resolve(1);
			s.Release(1);
			h.Resolve(2);
			s.Release(2);
			// A very late lane now claims frame 2 again: the slot's seq already says 2 -> Stale.
			LaneTicket t;
			Assert::IsTrue(s.Claim(2, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Stale);
			Assert::IsTrue(s.Claim(0, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Stale);
			// Frame 5 (same slot) is claimable; frame 8 (a ring too far ahead) is not yet.
			Assert::IsTrue(s.Claim(8, Provenance::AheadLane, kNoFence, &t) == ClaimResult::TooEarly);
			Assert::IsTrue(s.Claim(5, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Claimed);
			Assert::IsTrue(s.Claim(5, Provenance::AheadLane, kNoFence, nullptr) == ClaimResult::Stale,
				L"a second claim of the same frame cannot match");
		}

		TEST_METHOD(SealRejectsStaleLaneWorkAsLaneOwnedN3)
		{
			Harness h(kA);   // ring 3: frames 1 and 4 share a slot
			SlotSet& s = h.Set(kA);
			h.Resolve(0);
			s.Release(0);
			LaneTicket t1;
			Assert::IsTrue(s.Claim(1, Provenance::AheadLane, kNoFence, &t1) == ClaimResult::Claimed);
			// Frames 1..3 dropped without a discard; the render seals 4 while the lane still writes 1.
			const auto& r = h.Resolve(4);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::LaneLate);
			Assert::IsTrue(h.Result(r, kA).liveFilled);
			Assert::IsTrue(s.StateOf(4) == SlotState::LiveLaneOwned, L"the lane still owns its buffer");
			// The late publish of frame 1 is refused and returns the buffer.
			h.Set(kA).LaneHeader(t1).honoured = h.ind.StampOf(1);
			Assert::IsTrue(s.Publish(t1) == PublishResult::Late);
			Assert::IsTrue(s.StateOf(4) == SlotState::Live);
			bool live = false;
			Assert::IsTrue(Filled(h.ReadBytes(kA, 4, &live), 64, Pattern(4, kA, true)));
			Assert::IsTrue(live);
			Assert::AreEqual(uint64_t(1), s.Counters().lateDiscards.load());
			Assert::AreEqual(uint64_t(0), s.Counters().overruns.load());
		}

		TEST_METHOD(LaneOwnedReleaseBlocksTheNextClaimUntilTheAck)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			LaneTicket t;
			Assert::IsTrue(s.Claim(1, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Claimed);
			h.Resolve(0);
			s.Release(0);
			h.Resolve(1);   // lane late
			s.Release(1);
			Assert::IsTrue(s.StateOf(1) == SlotState::ReleasedLaneOwned);
			Assert::IsTrue(s.Claim(4, Provenance::AheadLane, kNoFence, nullptr) == ClaimResult::TooEarly);
			s.Abandon(t);
			Assert::IsTrue(s.StateOf(1) == SlotState::Empty);
			Assert::IsTrue(s.Claim(4, Provenance::AheadLane, kNoFence, nullptr) == ClaimResult::Claimed);
		}

		TEST_METHOD(CpuLiveFillWhileTheLaneIsMidChunk)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			const size_t bytes = h.Buf(kA).Bytes();
			std::atomic<int> phase(0);   // 1 lane chunk 1 done, 2 render filling, 3 lane chunk 2 done, 4 committed
			bool laneSawCancel = false;
			uint16_t laneSlot = 0;
			std::thread lane([&] {
				LaneTicket t;
				if (s.Claim(0, Provenance::AheadLane, kNoFence, &t) != ClaimResult::Claimed)
					return;
				laneSlot = t.slot;
				uint8_t* ring = h.Buf(kA).Ring(t.slot);
				std::memset(ring, 0xAA, bytes / 2);
				phase.store(1);
				while (phase.load() < 2)
					std::this_thread::yield();
				// Still mid-job: write the second chunk while the render writes its live buffer.
				std::memset(ring + bytes / 2, 0xAA, bytes - bytes / 2);
				phase.store(3);
				while (phase.load() < 4)
					std::this_thread::yield();
				laneSawCancel = !s.LaneContinue(t);
				if (laneSawCancel)
					s.Abandon(t);
				else
					s.Publish(t);
			});
			while (phase.load() < 1)
				std::this_thread::yield();
			const Stamp st = h.ind.Seal(0);
			const auto& r = h.table.Resolve(0, st, [&](const Node&, uint64_t, int live, Header&, uint32_t) {
				uint8_t* b = h.Buf(kA).Live(size_t(live));
				phase.store(2);
				std::memset(b, 0x55, bytes);
				while (phase.load() < 3)
					std::this_thread::yield();
				return Filled(b, bytes, 0x55);
			});
			phase.store(4);
			lane.join();
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::LaneLate);
			Assert::IsTrue(h.Result(r, kA).liveFilled);
			Assert::IsTrue(laneSawCancel, L"the lane sees the cancel at its next chunk");
			Assert::IsTrue(s.StateOf(0) == SlotState::Live);
			bool live = false;
			Assert::IsTrue(Filled(h.ReadBytes(kA, 0, &live), bytes, 0x55), L"the live buffer is intact");
			Assert::IsTrue(live);
			Assert::IsTrue(Filled(h.Buf(kA).Ring(laneSlot), bytes, 0xAA), L"the render never wrote the lane buffer");
			Assert::AreEqual(uint64_t(1), s.Counters().late.load());
			Assert::AreEqual(uint64_t(1), s.Counters().abandoned.load());
			Assert::AreEqual(uint64_t(0), s.Counters().published.load());
			Assert::IsTrue(s.Release(0));
			Assert::IsTrue(s.Claim(3, Provenance::AheadLane, kNoFence, nullptr) == ClaimResult::Claimed);
		}

		TEST_METHOD(AncestryCascadeRefillsEveryDescendant)
		{
			Harness h(kC);
			Assert::AreEqual(size_t(3), h.graph.nodes.size());
			h.Resolve(0);   // nothing published: all live (B has no previous frame; its fill accepts)
			h.Set(kB).Release(0);   // history 0: released before the next seal (A(0) is B's lag-1 input)
			h.Set(kC).Release(0);
			Assert::IsTrue(h.Lane(kA, 1) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kB, 1) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kC, 1) == PublishResult::Published);
			// A settings change only A honours, from frame 1, after the lane made A(1).
			h.ind.Advance(VpIndicator::kSettingsAnalysis, Cause::SettingsApply, 1);
			const auto& r = h.Resolve(1);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::StampMismatch);
			Assert::IsTrue(h.Result(r, kB).seal.reason == SealReason::AncestorReplaced, L"cascade to B");
			Assert::IsTrue(h.Result(r, kC).seal.reason == SealReason::AncestorReplaced, L"cascade to C");
			for (ProductId p : { kA, kB, kC }) {
				Assert::IsTrue(h.Result(r, p).liveFilled);
				Assert::IsTrue(h.Result(r, p).final == SealOutcome::Sealed);
			}
			// B's live header records A's live content.
			const Header* hb = h.Set(kB).Read(1).header;
			Assert::AreEqual(h.Set(kA).Read(1).header->cgen, hb->ancestors[0].cgen);
			Assert::IsTrue(hb->ancestors[0].prov == Provenance::LiveRender);

			// Frame 2: everything on time and valid.
			h.Set(kA).Release(0);
			h.Set(kB).Release(1);
			h.Set(kC).Release(1);
			Assert::IsTrue(h.Lane(kA, 2) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kB, 2) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kC, 2) == PublishResult::Published);
			const auto& r2 = h.Resolve(2);
			for (ProductId p : { kA, kB, kC })
				Assert::IsTrue(h.Result(r2, p).seal.reason == SealReason::Accepted);
			Assert::AreEqual(uint64_t(0), h.Set(kB).Counters().ancestorGone.load());
		}

		TEST_METHOD(AncestorLateMeansTheDescendantIsMissingNotStale)
		{
			Harness h(kC);
			h.Resolve(0);
			LaneTicket ta;
			Assert::IsTrue(h.Set(kA).Claim(1, Provenance::AheadLane, kNoFence, &ta) == ClaimResult::Claimed);
			// B cannot pin A(1) (no content yet) so the lane cannot start it.
			ClaimResult c;
			Assert::IsTrue(h.Lane(kB, 1, &c) == PublishResult::BadTicket);
			Assert::IsTrue(c == ClaimResult::Claimed);
			const auto& r = h.Resolve(1);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::LaneLate);
			Assert::IsTrue(h.Result(r, kB).seal.reason == SealReason::LaneMissing);
			Assert::IsTrue(h.Result(r, kC).seal.reason == SealReason::LaneMissing);
			h.Set(kA).Abandon(ta);
		}

		TEST_METHOD(PairSpanningASlipIsRejected)
		{
			Harness h(kB);
			h.Resolve(0);
			Assert::IsTrue(h.Lane(kA, 1) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kB, 1) == PublishResult::Published);
			h.Resolve(1);
			h.Set(kA).Release(0);
			h.Set(kB).Release(0);
			// A MINOR slip at frame 2, before the lane runs: stamps match, the pair (1, 2) spans it.
			h.ind.Advance(VpIndicator::kContinuity, Cause::DriftDrop, 2);
			Assert::IsTrue(h.Lane(kA, 2) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kB, 2) == PublishResult::Published);
			const auto& r = h.Resolve(2);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::Accepted, L"single-frame product keeps its result");
			Assert::IsTrue(h.Result(r, kB).seal.reason == SealReason::SpanBroken);
			Assert::IsTrue(h.Result(r, kB).liveFilled);
		}

		TEST_METHOD(StampLastAndLateNeverOverwrites)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			LaneTicket t;
			Assert::IsTrue(s.Claim(0, Provenance::AheadOfa, kNoFence, &t) == ClaimResult::Claimed);
			s.LaneHeader(t).honoured = h.ind.StampOf(0);
			s.LaneHeader(t).reads = h.graph.nodes[0].reads;
			std::memset(h.Buf(kA).Ring(t.slot), Pattern(0, kA, false), 64);
			// The producer signalled 5; only 4 has completed: nothing is published.
			Assert::IsTrue(s.Publish(t, 5, 4) == PublishResult::NotComplete);
			Assert::IsTrue(s.StateOf(0) == SlotState::Writing);
			const auto& r = h.Resolve(0);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::LaneLate);
			const uint32_t liveCgen = s.Read(0).header->cgen;
			Assert::IsTrue(s.Publish(t, 5, 5) == PublishResult::Late);
			const ReadInfo ri = s.Read(0);
			Assert::IsTrue(ri.kind == ReadInfo::Live);
			Assert::AreEqual(liveCgen, ri.header->cgen, L"the live header is untouched");
			Assert::IsTrue(Filled(h.Buf(kA).Live(size_t(ri.index)), 64, Pattern(0, kA, true)));
			// A second seal is idempotent and counted.
			const auto& again = h.Resolve(0);
			Assert::IsTrue(h.Result(again, kA).seal.reason == SealReason::AlreadySealed);
			Assert::AreEqual(uint64_t(1), s.Counters().doubleSeals.load());
			Assert::AreEqual(uint64_t(1), s.Counters().live.load(), L"no refill of a live frame");
		}

		TEST_METHOD(ProducerFenceValueIsWhatTheRenderWaitsOn)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			LaneTicket t;
			s.Claim(0, Provenance::AheadOfa, kNoFence, &t);
			s.LaneHeader(t).honoured = h.ind.StampOf(0);
			s.LaneHeader(t).reads = h.graph.nodes[0].reads;
			Assert::IsTrue(s.Publish(t, 77, 80) == PublishResult::Published);
			h.Resolve(0);
			const ReadInfo ri = s.Read(0);
			Assert::IsTrue(ri.kind == ReadInfo::Ring);
			Assert::AreEqual(uint64_t(77), ri.waitValue);
			Assert::IsTrue(s.WordFor(0).prov == Provenance::AheadOfa);
		}

		TEST_METHOD(ReleaseFenceGatesReuseAndTheCpuFlagIsTheState)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			Assert::IsTrue(h.Lane(kA, 0) == PublishResult::Published);
			h.Resolve(0);
			Assert::IsTrue(s.Release(0, 10));
			Assert::IsTrue(s.StateOf(0) == SlotState::Released);
			Assert::IsTrue(s.Claim(3, Provenance::AheadLane, 9, nullptr) == ClaimResult::ReleasePending);
			Assert::IsTrue(s.Claim(3, Provenance::AheadLane, 10, nullptr) == ClaimResult::Claimed);
			// CPU products: release value 0, no fence to wait for.
			Assert::IsTrue(h.Lane(kA, 1) == PublishResult::Published);
			h.Resolve(1);
			Assert::IsTrue(s.Release(1, 0));
			Assert::IsTrue(s.Claim(4, Provenance::AheadLane, 0, nullptr) == ClaimResult::Claimed);
		}

		TEST_METHOD(OlderPendingReleaseValueSurvivesARejectedTenant)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			Assert::IsTrue(h.Lane(kA, 0) == PublishResult::Published);
			h.Resolve(0);
			s.Release(0, 20);   // GPU reads of the ring buffer pending until 20
			h.Resolve(1);
			s.Release(1, 21);
			h.Resolve(2);
			s.Release(2, 22);
			h.Resolve(3);       // same slot as 0: no lane result, live
			s.Release(3, 0);
			Assert::IsTrue(s.Claim(6, Provenance::AheadLane, 19, nullptr) == ClaimResult::ReleasePending);
			Assert::IsTrue(s.Claim(6, Provenance::AheadLane, 20, nullptr) == ClaimResult::Claimed);
		}

		TEST_METHOD(PinnedInputBlocksTheWriteNotTheClaim)
		{
			Harness h(kB);   // A ring 4 (history 1)
			SlotSet& a = h.Set(kA);
			Assert::IsTrue(h.Lane(kA, 0) == PublishResult::Published);
			h.Resolve(0);
			AncestorRef ref;
			Assert::IsTrue(a.PinRef(0, &ref), L"a lane reader pins A(0)");
			Assert::AreEqual(uint64_t(0), ref.seq);
			Assert::IsTrue(a.Release(0));
			LaneTicket t;
			Assert::IsTrue(a.Claim(4, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Claimed);
			Assert::IsFalse(a.Writable(t), L"the reader still holds the old tenant");
			a.Unpin(0);
			Assert::IsTrue(a.Writable(t));
			Assert::IsFalse(a.PinRef(0, &ref), L"no content for frame 0 any more");
		}

		TEST_METHOD(PinnedLiveBufferIsNotReusedUnderALaneReader)
		{
			Harness h(kB);   // A: history 1, read twice by B's lane -> live pool 1 + 1 + 0 + 2 spares = 4
			SlotSet& a = h.Set(kA);
			Assert::AreEqual(4, int(a.LivePool()));
			h.Resolve(0);   // A(0) live
			h.Set(kB).Release(0);
			AncestorRef ref;
			int pinnedLive = -1;
			Assert::IsTrue(a.PinRef(0, &ref, &pinnedLive), L"a late lane reader pins A(0)");
			Assert::AreEqual(a.Read(0).index, pinnedLive, L"the reader learns the live buffer");
			Assert::IsTrue(ref.prov == Provenance::LiveRender);
			// B's lane job for frame 1 records where to read A(0).
			Header hb;
			LaneInputs in;
			Assert::IsFalse(h.table.LaneAncestors(kB, 1, hb, in), L"A(1) has no content yet");
			a.Release(0);
			h.Resolve(1);   // A(1) live: must not take the pinned buffer
			h.Set(kB).Release(1);
			Assert::AreNotEqual(pinnedLive, a.Read(1).index);
			h.Resolve(2);   // A(2) live, A(1) still held (history)
			h.Set(kB).Release(2);
			Assert::AreNotEqual(pinnedLive, a.Read(2).index);
			a.Release(1);
			Assert::IsTrue(h.Lane(kA, 3) == PublishResult::Published);
			Assert::IsTrue(h.table.LaneAncestors(kB, 3, hb, in));
			Assert::AreEqual(size_t(2), in.count);
			Assert::AreEqual(-1, in.liveIndex[0], L"A(3) is read from the ring");
			Assert::AreEqual(a.Read(2).index, in.liveIndex[1], L"A(2) is read from its live buffer");
			in.Unpin();
			// Hold A(2) and A(3); with A(1) released only the pinned buffer and A(1)'s are free.
			h.Resolve(3);
			h.Set(kB).Release(3);
			a.Release(2);
			LaneTicket t;
			Assert::IsTrue(a.Claim(4, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Claimed);
			a.Abandon(t);   // A(4) will need a live fill
			const auto& r = h.Resolve(4);
			Assert::IsTrue(h.Result(r, kA).liveFilled);
			Assert::AreNotEqual(pinnedLive, a.Read(4).index);
			a.Unpin(0, pinnedLive);
			Assert::AreEqual(uint64_t(0), a.Counters().noLiveBuffer.load());
		}

		TEST_METHOD(OnlyPinnedFreeLiveBuffersAreCountedNotReused)
		{
			Harness h(kB);   // A live pool 4
			SlotSet& a = h.Set(kA);
			h.Resolve(0);
			h.Set(kB).Release(0);
			h.Resolve(1);
			h.Set(kB).Release(1);
			int live0 = -1, live1 = -1;
			Assert::IsTrue(a.PinRef(0, nullptr, &live0));
			Assert::IsTrue(a.PinRef(1, nullptr, &live1));   // more pins than one reader job holds
			a.Release(0);
			h.Resolve(2);
			h.Set(kB).Release(2);
			a.Release(1);
			h.Resolve(3);
			h.Set(kB).Release(3);
			// A(2), A(3) held; the two other buffers are free but pinned.
			const auto& r = h.Resolve(4);
			Assert::IsTrue(h.Result(r, kA).finalReason == SealReason::NoLiveBuffer, L"slot 0 is free; the live pool is not");
			Assert::AreEqual(uint64_t(1), a.Counters().pinnedLiveSkips.load());
			a.Unpin(0, live0);
			Assert::AreEqual(live0, a.BeginLive(4), L"free again once unpinned");
			a.Unpin(1, live1);
		}

		TEST_METHOD(DiscardFreesTheSlotForTheLane)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			Assert::IsTrue(h.Lane(kA, 0) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kA, 1) == PublishResult::Published);
			h.Resolve(0);
			s.Release(0);
			Assert::IsTrue(s.Discard(1), L"frame 1 dropped before render");
			Assert::IsTrue(s.Claim(4, Provenance::AheadLane, kNoFence, nullptr) == ClaimResult::Claimed);
			// Discard while the lane writes: lane-owned until its (late) publish.
			LaneTicket t;
			Assert::IsTrue(s.Claim(2, Provenance::AheadLane, kNoFence, &t) == ClaimResult::Claimed);
			Assert::IsTrue(s.Discard(2));
			Assert::IsTrue(s.StateOf(2) == SlotState::ReleasedLaneOwned);
			Assert::IsTrue(s.Publish(t) == PublishResult::Late);
			Assert::IsTrue(s.StateOf(2) == SlotState::Empty);
			Assert::IsTrue(s.Claim(5, Provenance::AheadLane, kNoFence, nullptr) == ClaimResult::Claimed);
			Assert::IsFalse(s.Discard(2), L"discarding twice is an order error");
		}

		TEST_METHOD(OverrunIsCountedWhenFramesAreNotReleased)
		{
			Harness h(kA);   // ring 3
			h.Resolve(0);
			h.Resolve(1);
			h.Resolve(2);
			const auto& r = h.Resolve(3);
			Assert::IsTrue(h.Result(r, kA).final == SealOutcome::Unavailable);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::Overrun);
			Assert::AreEqual(uint64_t(1), h.Set(kA).Counters().overruns.load());
			bool live = false;
			Assert::IsTrue(h.ReadBytes(kA, 0, &live) != nullptr, L"frame 0 is untouched");
		}

		TEST_METHOD(LivePoolExhaustionIsCounted)
		{
			Harness h(kA);   // live pool history 0 + 1 + R 0 = 1
			SlotSet& s = h.Set(kA);
			h.Resolve(0);   // live buffer 0 held by frame 0
			const auto& r = h.Resolve(1);
			Assert::IsTrue(h.Result(r, kA).finalReason == SealReason::NoLiveBuffer);
			Assert::AreEqual(uint64_t(1), s.Counters().noLiveBuffer.load());
			s.Release(0);
			Assert::IsTrue(s.BeginLive(1) == 0, L"freed on release");
		}

		TEST_METHOD(MissingInputIsAVisibleFallbackOrUnavailable)
		{
			Harness with(kB);
			const auto& r = with.Resolve(0);
			Assert::IsTrue(with.Result(r, kB).final == SealOutcome::Sealed);
			Assert::AreEqual(2u, with.Result(r, kB).missingInputs, L"input 1 (A at lag 1) missing");
			const Header* hb = with.Set(kB).Read(0).header;
			Assert::IsTrue(hb->prov == Provenance::LiveFallback);
			Assert::AreEqual(2, int(hb->ancestorCount), L"positional ancestors");
			Assert::AreEqual(0u, hb->ancestors[1].cgen);
			Assert::IsTrue(hb->ancestors[0].cgen != 0);
			Assert::AreEqual(uint64_t(1), with.Set(kB).Counters().liveMissingInputs.load());

			Harness without(kB, 11, false);
			const auto& r2 = without.Resolve(0);
			Assert::IsTrue(without.Result(r2, kB).final == SealOutcome::Unavailable);
			Assert::IsTrue(without.Result(r2, kB).finalReason == SealReason::InputMissing);
			Assert::AreEqual(uint64_t(1), without.Set(kB).Counters().inputMissing.load());
			Assert::AreEqual(uint64_t(0), without.fills - 1, L"only A was filled");
		}

		TEST_METHOD(AbandonWaitsForTheProducerFence)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			LaneTicket t;
			Assert::IsTrue(s.Claim(0, Provenance::AheadOfa, kNoFence, &t) == ClaimResult::Claimed);
			h.Resolve(0);   // late: rejected, live
			Assert::IsFalse(s.LaneContinue(t));
			Assert::IsFalse(s.Abandon(t, 9, 8), L"submitted engine work has not landed");
			s.Release(0);
			Assert::IsTrue(s.StateOf(0) == SlotState::ReleasedLaneOwned);
			Assert::IsTrue(s.Claim(3, Provenance::AheadOfa, kNoFence, nullptr) == ClaimResult::TooEarly,
				L"no second writer while the engine may still write");
			Assert::IsTrue(s.Abandon(t, 9, 9));
			Assert::IsTrue(s.Claim(3, Provenance::AheadOfa, kNoFence, nullptr) == ClaimResult::Claimed);
		}

		TEST_METHOD(StaleTicketCopyNeverTouchesTheCurrentHeader)
		{
			Harness h(kA);
			SlotSet& s = h.Set(kA);
			LaneTicket t;
			s.Claim(0, Provenance::AheadLane, kNoFence, &t);
			s.LaneHeader(t).honoured = h.ind.StampOf(0);
			s.LaneHeader(t).reads = h.graph.nodes[0].reads;
			const LaneTicket copy = t;
			Assert::IsTrue(s.Publish(t) == PublishResult::Published);
			h.Resolve(0);
			const uint32_t cgen = s.Read(0).header->cgen;
			LaneTicket stale = copy;
			Assert::IsTrue(s.Publish(stale) == PublishResult::Late);
			Assert::AreEqual(cgen, s.Read(0).header->cgen);
			Assert::IsTrue(s.StateOf(0) == SlotState::Sealed);
		}

		TEST_METHOD(ReleaseAfterKeepsEachHistory)
		{
			Harness h(kB);   // A history 1, B history 0
			h.Resolve(0);
			h.table.ReleaseAfter(0);
			Assert::IsTrue(h.Set(kB).StateOf(0) == SlotState::Released);
			Assert::IsTrue(h.Set(kA).StateOf(0) == SlotState::Live, L"A(0) is B(1)'s lag-1 input");
			h.Resolve(1);
			h.table.ReleaseAfter(1);
			Assert::IsTrue(h.Set(kA).StateOf(0) == SlotState::Released);
			Assert::IsTrue(h.Set(kA).StateOf(1) == SlotState::Live);
		}

		TEST_METHOD(FillDeclinedLeavesTheProductUnavailable)
		{
			Harness h(kB);
			h.declineMask = 1u << kB;
			const auto& r = h.Resolve(0);
			Assert::IsTrue(h.Result(r, kA).final == SealOutcome::Sealed);
			Assert::IsTrue(h.Result(r, kB).final == SealOutcome::Unavailable);
			Assert::IsTrue(h.Result(r, kB).finalReason == SealReason::FillDeclined);
			Assert::IsTrue(h.Set(kB).Read(0).kind == ReadInfo::None);
			Assert::AreEqual(uint64_t(1), h.table.FillDeclines());
		}

		TEST_METHOD(WithoutALaneGrantAMissIsNotCounted)
		{
			Harness h(kA);
			h.Set(kA).SetLaneGranted(false);
			const auto& r = h.Resolve(0);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::InRenderOnly);
			Assert::AreEqual(uint64_t(0), h.Set(kA).Counters().missing.load());
		}

		TEST_METHOD(FullResyncRejectsLaneWorkFromBeforeIt)
		{
			Harness h(kA);
			Assert::IsTrue(h.Lane(kA, 0) == PublishResult::Published);
			Assert::IsTrue(h.Lane(kA, 1) == PublishResult::Published);
			h.Resolve(0);
			h.Set(kA).Release(0);
			// A request-time FULL clamped above the seal (frame 1 on).
			Assert::AreEqual(uint64_t(1), h.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 0));
			const auto& r = h.Resolve(1);
			Assert::IsTrue(h.Result(r, kA).seal.reason == SealReason::StampMismatch);
			Assert::IsTrue(h.Result(r, kA).liveFilled);
		}

		TEST_METHOD(SameScriptGivesTheSameTrace)
		{
			auto run = [](std::vector<uint64_t>& trace) {
				Harness h(kC, 5);
				for (uint64_t n = 0; n < 24; ++n) {
					if (n % 5 != 3)
						h.Lane(kA, n);
					if (n % 4 != 1)
						h.Lane(kB, n);
					if (n % 3 != 2)
						h.Lane(kC, n);
					if (n == 9)
						h.ind.Advance(VpIndicator::kContinuity, Cause::DriftDrop, n);
					if (n == 15)
						h.ind.Advance(VpIndicator::kSettingsAnalysis, Cause::SettingsApply, n);
					const auto& r = h.Resolve(n);
					for (const ProductFrameResult& p : r) {
						const ReadInfo ri = h.Set(p.id).Read(n);
						trace.push_back((uint64_t(p.id) << 48) | (uint64_t(p.seal.reason) << 40)
							| (uint64_t(p.finalReason) << 32) | (uint64_t(ri.kind) << 24)
							| (ri.header ? ri.header->cgen : 0u));
					}
					if (n >= 1)
						h.Set(kA).Release(n - 1);
					h.Set(kB).Release(n);
					h.Set(kC).Release(n);
				}
			};
			std::vector<uint64_t> a, b;
			run(a);
			run(b);
			Assert::AreEqual(size_t(24 * 3), a.size());
			Assert::IsTrue(a == b);
		}

		// 2.5 x 10^6 frames x 2 products = 5 x 10^6 seals, plus up to 5 x 10^6 publishes from a
		// concurrent lane, with slips. Every frame is either a complete lane result or a live fill;
		// a sealed buffer is never overwritten before its release; no ancestor is lost early.
		TEST_METHOD(SealPublishStressTenMillionOperations)
		{
			Harness h(kB);
			const uint64_t frames = 2500000;
			SlotSet& a = h.Set(kA);
			SlotSet& b = h.Set(kB);
			const size_t bytes = h.Buf(kA).Bytes();
			std::atomic<uint64_t> progress(0);
			std::atomic<bool> done(false);
			std::atomic<uint64_t> laneCorrupt(0), laneReads(0);

			auto produce = [&](SlotSet& s, ProductId p, uint64_t seq) -> bool {
				LaneTicket t;
				for (;;) {
					const ClaimResult c = s.Claim(seq, Provenance::AheadLane, kNoFence, &t);
					if (c == ClaimResult::Claimed)
						break;
					if (c == ClaimResult::Stale || done.load(std::memory_order_relaxed))
						return false;
					std::this_thread::yield();
				}
				Header& hd = s.LaneHeader(t);
				LaneInputs pins;
				if (!h.table.LaneAncestors(p, seq, hd, pins)) {
					s.Abandon(t);
					return false;
				}
				while (!s.Writable(t))
					std::this_thread::yield();
				// Read every pinned input (ring slot or live buffer): it must stay intact under the pin.
				for (size_t k = 0; k < pins.count; ++k) {
					const uint64_t is = pins.seqs[k];
					const int li = pins.liveIndex[k];
					const uint8_t* src = li >= 0 ? h.Buf(kA).Live(size_t(li)) : h.Buf(kA).Ring(a.SlotOf(is));
					if (!Filled(src, bytes, Pattern(is, kA, li >= 0)))
						laneCorrupt.fetch_add(1, std::memory_order_relaxed);
					laneReads.fetch_add(1, std::memory_order_relaxed);
				}
				hd.honoured = h.ind.StampOf(seq);
				uint8_t* ring = h.Buf(p).Ring(t.slot);
				const uint8_t v = Pattern(seq, p, false);
				std::memset(ring, v, bytes / 2);
				if (!s.LaneContinue(t)) {
					pins.Unpin();
					s.Abandon(t);
					return false;
				}
				std::memset(ring + bytes / 2, v, bytes - bytes / 2);
				const bool ok = s.Publish(t) == PublishResult::Published;
				pins.Unpin();
				return ok;
			};

			std::thread lane([&] {
				for (uint64_t s = 0; s < frames && !done.load(std::memory_order_relaxed); ++s) {
					if (produce(a, kA, s))
						produce(b, kB, s);
					progress.store(s + 1, std::memory_order_release);
				}
			});

			uint64_t corrupt = 0, unavailable = 0, slips = 0;
			for (uint64_t n = 0; n < frames; ++n) {
				for (int spin = 0; spin < 64 && progress.load(std::memory_order_acquire) <= n; ++spin) {
				}
				if (n % 100000 == 50000 && slips < 24) {
					h.ind.Advance(VpIndicator::kContinuity, Cause::DriftDrop, n);
					++slips;
				}
				const auto& r = h.Resolve(n);
				for (ProductId p : { kA, kB }) {
					if (h.Result(r, p).final != SealOutcome::Sealed) {
						++unavailable;
						continue;
					}
					bool live = false;
					const uint8_t* bytesRead = h.ReadBytes(p, n, &live);
					if (!bytesRead || !Filled(bytesRead, bytes, Pattern(n, p, live)))
						++corrupt;
				}
				// A(n-1) is B's lag-1 ancestor until now: re-check it (never overwritten), then release.
				if (n >= 1) {
					bool live = false;
					const uint8_t* prev = h.ReadBytes(kA, n - 1, &live);
					if (!prev || !Filled(prev, bytes, Pattern(n - 1, kA, live)))
						++corrupt;
				}
				bool live = false;
				const uint8_t* cur = h.ReadBytes(kB, n, &live);
				if (cur && !Filled(cur, bytes, Pattern(n, kB, live)))
					++corrupt;
				h.table.ReleaseAfter(n);   // A(n-1) and B(n): the N2 rule
			}
			done.store(true);
			lane.join();

			Assert::AreEqual(uint64_t(0), corrupt, L"no torn, partial or overwritten buffer");
			Assert::AreEqual(uint64_t(0), laneCorrupt.load(), L"pinned inputs stay intact for lane readers");
			Assert::IsTrue(laneReads.load() > 0);
			Assert::AreEqual(uint64_t(0), unavailable);
			for (SlotSet* s : { &a, &b }) {
				const SlotCounters& c = s->Counters();
				Assert::AreEqual(uint64_t(0), c.ancestorGone.load());
				Assert::AreEqual(uint64_t(0), c.overruns.load());
				Assert::AreEqual(uint64_t(0), c.orderErrors.load());
				Assert::AreEqual(uint64_t(0), c.noLiveBuffer.load());
				Assert::AreEqual(uint64_t(0), c.inputMissing.load());
				Assert::AreEqual(frames, c.sealed.load() + c.live.load());
				Assert::IsTrue(c.live.load() > 0);
			}
			Assert::IsTrue(a.Counters().sealed.load() > 0, L"the on-time path is exercised");
			Assert::IsTrue(b.Counters().sealed.load() > 0);
			Assert::AreEqual(uint64_t(1), b.Counters().liveMissingInputs.load(), L"only B(0) lacks A(-1)");
			Logger::WriteMessage((std::string("stress: A sealed ") + std::to_string(a.Counters().sealed.load())
				+ " live " + std::to_string(a.Counters().live.load()) + " late " + std::to_string(a.Counters().late.load())
				+ " lateDiscards " + std::to_string(a.Counters().lateDiscards.load()) + "; B sealed "
				+ std::to_string(b.Counters().sealed.load()) + " invalid " + std::to_string(b.Counters().invalid.load())
				+ "; lane input reads " + std::to_string(laneReads.load()) + " pinned live skips "
				+ std::to_string(a.Counters().pinnedLiveSkips.load()))
				.c_str());
		}

		// ---- B3 X1: per-product seal lag ----

		TEST_METHOD(OnTimeOrAbsentReadNeverInheritsLag)
		{
			LagHarness h(2);
			Assert::IsTrue(h.table.Valid());
			Assert::AreEqual(2, int(h.graph.Find(kB)->sealLag));
			Assert::AreEqual(0, int(h.graph.Find(kC)->sealLag), L"the reader declares no lag and inherits none");
			Assert::AreEqual(2, int(h.table.MaxSealLag()));
			// Frame 0: a and c have lane results, b does not (its lane is behind): b is PENDING at 0.
			Assert::IsTrue(h.Lane(kA, 0));
			const auto& res = h.Resolve(0);
			const ProductFrameResult& b = h.Result(res, kB);
			Assert::IsTrue(b.pending && b.final == SealOutcome::Pending && b.finalReason == SealReason::AwaitingFinalSeal);
			Assert::IsTrue(std::string(Token(SealReason::AwaitingFinalSeal)) == "awaiting_final_seal");
			// c is sealed ON TIME at frame 0 (never Pending): b is ABSENT for it, so c is its declared missing-input form.
			const ProductFrameResult& c = h.Result(res, kC);
			Assert::IsFalse(c.pending);
			Assert::AreEqual(0, int(c.sealLag));
			Assert::IsTrue(c.liveFilled && c.final == SealOutcome::Sealed && (c.missingInputs & 1u) != 0u);
			Assert::IsTrue(h.Set(kC).Read(0).header->prov == Provenance::LiveFallback, L"visible reduced form, not a full result");
			Assert::AreEqual(uint64_t(1), h.Set(kB).Counters().onTimeAbsent.load(), L"counted on the lagged product");
			Assert::IsTrue(h.Set(kB).Read(0).kind == ReadInfo::None, L"pending is not readable");
			// The final seal of b(0) at frame 2 does not change c(0).
			h.Resolve(1);
			h.Finalize(1);
			h.Resolve(2);
			const auto& fin = h.FinalSeal(2);   // read during the render of 2, before it retires
			const ProductFrameResult& bf = h.Result(fin, kB);
			Assert::AreEqual(uint64_t(0), bf.seq);
			Assert::IsTrue(bf.liveFilled && bf.final == SealOutcome::Sealed, L"no lane result: live-filled at the final seal");
			Assert::IsTrue(h.Set(kC).Read(0).header->prov == Provenance::LiveFallback, L"the reader's frame stays what it was");
		}

		TEST_METHOD(ALaggedSealCountsAheadLiveFilledLateAndAbsent)
		{
			LagHarness h(2);
			// Frame 0: b on time -> AHEAD.
			Assert::IsTrue(h.Lane(kA, 0));
			Assert::IsTrue(h.Lane(kB, 0));
			const auto& r0 = h.Resolve(0);
			Assert::IsTrue(h.Result(r0, kB).final == SealOutcome::Sealed && h.Result(r0, kB).seal.reason == SealReason::Accepted);
			Assert::IsTrue(h.Result(r0, kC).final == SealOutcome::Sealed && h.Result(r0, kC).missingInputs == 0u, L"an ahead frame is read in full");
			h.Finalize(0);
			// Frame 1: no b lane result at 1; it publishes after the render of 1 -> LATE at frame 3.
			Assert::IsTrue(h.Lane(kA, 1));
			Assert::IsTrue(h.Resolve(1)[h.IndexOf(kB)].pending);
			Assert::IsTrue(h.Lane(kB, 1), L"a pending frame keeps its slot for the late lane result");
			h.Finalize(1);
			// Frame 2: no b lane result at all -> LIVE-FILLED at frame 4.
			Assert::IsTrue(h.Lane(kA, 2));
			Assert::IsTrue(h.Resolve(2)[h.IndexOf(kB)].pending);
			h.Finalize(2);   // b(0) was ahead: nothing to finalise
			Assert::IsTrue(h.Lane(kA, 3));
			h.Resolve(3);
			const auto& f3 = h.FinalSeal(3);
			const ProductFrameResult& late = h.Result(f3, kB);
			Assert::IsTrue(late.late && late.seq == 1u && late.final == SealOutcome::Sealed && !late.liveFilled);
			Assert::IsTrue(h.Set(kB).Read(1).kind == ReadInfo::Ring, L"the late lane result is the frame");
			h.Retire(3);
			Assert::IsTrue(h.Lane(kA, 4));
			h.Resolve(4);
			const auto& f4 = h.Finalize(4);
			Assert::IsTrue(h.Result(f4, kB).liveFilled && h.Result(f4, kB).seq == 2u);
			// Frame 3 (pending, from Resolve(3)): a resync after 3 -> ABSENT, never live-filled from pre-resync state.
			h.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 5);
			Assert::IsTrue(h.Lane(kA, 5));
			h.Resolve(5);
			const auto& f5 = h.Finalize(5);
			const ProductFrameResult& rej = h.Result(f5, kB);
			Assert::IsTrue(rej.seq == 3u && rej.final == SealOutcome::Unavailable && rej.finalReason == SealReason::StampMismatch);
			Assert::IsFalse(rej.liveFilled);
			const SlotCounters& k = h.Set(kB).Counters();
			Assert::AreEqual(uint64_t(1), k.sealAhead.load());
			Assert::AreEqual(uint64_t(1), k.sealLate.load());
			Assert::AreEqual(uint64_t(1), k.sealLiveFilled.load());
			Assert::AreEqual(uint64_t(1), k.sealAbsent.load());
			Assert::AreEqual(uint64_t(1), k.lateResyncRejected.load());
			Assert::AreEqual(uint64_t(5), k.sealPending.load(), L"frames 1..5 pending (0 was ahead)");
			// a and c (sealLag 0) never count a seal class.
			Assert::AreEqual(uint64_t(0), h.Set(kA).Counters().sealAhead.load() + h.Set(kC).Counters().sealPending.load());
		}

		TEST_METHOD(ADeterministicRunSealsALaggedProductAtTheFinalSealOnly)
		{
			LagHarness h(2);
			h.table.SetDeterministicSeal(true);
			Assert::IsTrue(h.Lane(kA, 0));
			Assert::IsTrue(h.Lane(kB, 0));
			Assert::IsTrue(h.Resolve(0)[h.IndexOf(kB)].pending, L"never ahead in a deterministic run");
			h.Resolve(1);
			h.Finalize(1);
			h.Resolve(2);
			const auto& f = h.Finalize(2);
			Assert::IsTrue(h.Result(f, kB).late && h.Result(f, kB).seq == 0u);
			Assert::AreEqual(uint64_t(0), h.Set(kB).Counters().sealAhead.load());
		}

		TEST_METHOD(CompetitiveLowLatencyRunsASealLagAtZero)
		{
			for (int m = 0; m < 2; ++m) {
				const Graph g = m == 0 ? LagGraph(2, QueueMode::Competitive) : LagGraph(2, QueueMode::Normal, 0);
				Assert::IsTrue(g.valid);
				Assert::AreEqual(0, int(g.Find(kB)->sealLag));
				Assert::AreEqual(2, int(g.Find(kB)->declaredSealLag));
				Assert::AreEqual(0, int(g.maxSealLag));
				Assert::IsTrue(g.sealLagForcedZero);
				SlotTable t(g);
				Assert::AreEqual(0, int(t.MaxSealLag()));
			}
			// Lag 0 (and the forced-0 graph): the same sizes as a graph that never declared a seal lag.
			const Graph none = LagGraph(0);
			const Graph forced = LagGraph(2, QueueMode::Competitive);
			const Graph lagged = LagGraph(2);
			Assert::IsFalse(none.sealLagForcedZero);
			for (size_t i = 0; i < none.nodes.size(); ++i) {
				Assert::AreEqual(int(none.nodes[i].ring), int(forced.nodes[i].ring));
				Assert::AreEqual(int(none.nodes[i].ring) + 2, int(lagged.nodes[i].ring), L"graph K = 2: every ring + 2");
			}
			Assert::AreEqual(none.framesInFlight + 2u, lagged.framesInFlight);
		}

		TEST_METHOD(TheRegistryRejectsAnInvalidSealLag)
		{
			const char* why = "";
			Assert::IsTrue(LagRegistry(kMaxSealLag).Validate(&why));
			Assert::IsFalse(LagRegistry(uint8_t(kMaxSealLag + 1)).Validate(&why));
			Assert::IsTrue(std::string(why).find("seal lag deeper") != std::string::npos);
			std::vector<ProductDecl> d = LagRegistry(2).All();
			d[kB].backend = Backend::RenderOnly;
			Assert::IsFalse(Registry(d).Validate(&why));
			d = LagRegistry(2).All();
			d[kA].singletonStore = true;
			Assert::IsFalse(Registry(d).Validate(&why));
		}

		TEST_METHOD(ReleaseRunsTheGraphSealLagLater)
		{
			LagHarness h(2);
			const uint64_t hist = h.graph.Find(kB)->history;
			// b(0) is finalised at frame 2; with K = 2 it is released only when frame history + 2 retires (today: frame history).
			for (uint64_t s = 0; s <= hist + 2u; ++s) {
				Assert::IsTrue(h.Lane(kA, s));
				h.Resolve(s);
				h.FinalSeal(s);
				if (s >= 2u)
					Assert::IsTrue(h.Set(kB).Read(0).kind != ReadInfo::None, L"kept until its final seal and history");
				h.Retire(s);
			}
			Assert::IsTrue(h.Set(kB).Read(0).kind == ReadInfo::None, L"released K frames after today's release point");
		}
	};
}
