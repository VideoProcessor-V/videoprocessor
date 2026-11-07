// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// W1-SKG: the skeleton seal/graph capabilities of doc 72 section 5 items 1-4.
//   1. lead edges: the SlotTable seals a look-ahead ancestor (Published, stamp == StampOf(N+k)) and refuses the rest
//   2. limits: a node with 16 inputs, a consumer with 10 consumes
//   3. ordered-chain lane jobs: frame order, visible carry-forward, out-of-order refused
//   4. budget-governed history: the VRAM governor sheds history in steps, visibly; the graph is built with the grant
// CPU only.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/LaneRunner.h>
#include <VP-V/core/OrderedChain.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/core/VramGovernor.h>

#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonGraphTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Bit;
	using VpIndicator::Indicator;
	using VpIndicator::Stamp;
	using VpResync::Cause;

	constexpr uint64_t kNoFence = ~uint64_t(0);

	bool Has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

	// ---- the lead registry: 0 base, 1 reader (base at lead 2), 2 causal reader (the short-queue form) ----
	constexpr ProductId kBase = 0, kLeadReader = 1, kCausal = 2;

	Registry LeadRegistry()
	{
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		std::vector<ProductDecl> d;
		d.push_back(detail::Make(kBase, "base", "base", Backend::Cpu, 0, Tier::T0, content, 0));
		d.push_back(detail::Make(kLeadReader, "lead_reader", "lead reader", Backend::Cpu, 0, Tier::T0, content, 0));
		d.push_back(detail::Make(kCausal, "causal", "causal reader", Backend::Cpu, 0, Tier::T0, content, 0));
		detail::AddLead(d[kLeadReader], kBase, 2);
		d[kLeadReader].shortQueueSubstitute = kCausal;
		d[kLeadReader].missingInputForm = true;
		detail::AddInput(d[kCausal], kBase, 0);
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	// Generic CPU harness over any registry and one consumer list: lane jobs by hand (claim, pins, stamp, write, publish).
	struct Rig {
		Graph graph;
		SlotTable table;
		Indicator ind;
		std::vector<CpuBuffers> bufs;

		Rig(const Registry& reg, const std::vector<ConsumerDecl>& consumers, uint8_t laneLead, bool stampSource = true)
			: graph(BuildGraph(reg, consumers, MakeSettings(laneLead)))
			, table(graph)
			, ind(21, 64)
		{
			if (stampSource)
				table.SetStampSource([this](uint64_t s) { return ind.StampOf(s); });
			for (const Node& n : graph.nodes)
				bufs.emplace_back(n);
		}

		static GraphSettings MakeSettings(uint8_t laneLead)
		{
			GraphSettings gs;
			gs.dims.width = 64;
			gs.dims.height = 64;
			gs.renderAhead = 0;
			gs.laneLead = laneLead;
			gs.queueDepth = 4;
			return gs;
		}

		SlotSet& Set(ProductId p) { return *table.Find(p); }
		size_t IndexOf(ProductId p) const { return size_t(graph.IndexOf(p)); }

		// False when an input has no content (the job would wait).
		bool Lane(ProductId p, uint64_t seq, bool breakChainFlag = false, bool forceCarry = false)
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
			if (forceCarry) {   // a reduced job although its predecessor exists (what a transient pin failure would produce)
				AncestorRef empty;
				empty.productId = p;
				empty.seq = seq - 1u;
				empty.slotIndex = s.SlotOf(seq - 1u);
				h.ancestors[1] = empty;
				h.chainReduced = true;
			}
			if (breakChainFlag)
				h.chainReduced = false;   // tamper: the seal must not accept an undeclared hole
			h.honoured = ind.StampOf(seq);
			std::memset(bufs[IndexOf(p)].Ring(t.slot), int(seq + p), bufs[IndexOf(p)].Bytes());
			const PublishResult r = s.Publish(t);
			pins.Unpin();
			return r == PublishResult::Published;
		}

		const std::vector<ProductFrameResult>& Resolve(uint64_t seq)
		{
			const Stamp st = ind.Seal(seq);
			return table.Resolve(seq, st, [this](const Node& n, uint64_t s, int live, Header&, uint32_t) {
				std::memset(bufs[IndexOf(n.id)].Live(size_t(live)), int(s + n.id + 100u), bufs[IndexOf(n.id)].Bytes());
				return true;
			});
		}
		const ProductFrameResult& Result(const std::vector<ProductFrameResult>& r, ProductId p) const { return r[IndexOf(p)]; }
	};

	std::vector<ConsumerDecl> OneConsumer(ProductId p)
	{
		ConsumerDecl c;
		c.id = 1;
		c.name = "reader";
		AddConsume(c, p);
		return std::vector<ConsumerDecl>(1, c);
	}

	// A scripted lead scenario; its digest is the determinism witness.
	uint64_t LeadScenarioDigest()
	{
		Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
		uint64_t d = 1469598103934665603ull;
		auto mix = [&d](uint64_t v) { d = (d ^ v) * 1099511628211ull; };
		for (uint64_t s = 0; s < 6; ++s)
			r.Lane(kBase, s);
		for (uint64_t s = 0; s < 4; ++s) {
			r.Lane(kLeadReader, s);
			const auto& res = r.Resolve(s);
			for (const ProductFrameResult& f : res) {
				mix(uint64_t(f.final));
				mix(uint64_t(f.finalReason));
				mix(f.liveFilled ? 1u : 0u);
			}
			r.table.ReleaseAfter(s);
		}
		mix(r.Set(kLeadReader).Counters().leadAccepted.load() + r.Set(kBase).Counters().leadAccepted.load());
		return d;
	}

	// ---- many inputs: 16 sources and a sink that reads all of them ----
	constexpr size_t kSources = 16;

	Registry ManyInputsRegistry()
	{
		using namespace VpIndicator;
		std::vector<ProductDecl> d;
		for (size_t i = 0; i < kSources; ++i)
			d.push_back(detail::Make(ProductId(i), "src", "source", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0));
		ProductDecl sink = detail::Make(ProductId(kSources), "sink", "sink", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0);
		for (size_t i = 0; i < kSources; ++i)
			detail::AddInput(sink, ProductId(i), 0);   // exactly the limit (one more asserts)
		d.push_back(sink);
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	// ---- ordered chain: 0 base, 1 chain (base at lag 0, itself at lag 1) ----
	constexpr ProductId kChainBase = 0, kChain = 1;

	Registry ChainRegistry(bool declareChain = true)
	{
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		std::vector<ProductDecl> d;
		d.push_back(detail::Make(kChainBase, "cbase", "chain base", Backend::Cpu, 0, Tier::T0, content, 0));
		d.push_back(detail::Make(kChain, "chain", "chain", Backend::Cpu, 1, Tier::T0, content, 0));
		detail::AddInput(d[kChain], kChainBase, 0);
		detail::AddInput(d[kChain], kChain, 1);
		d[kChain].orderedChain = declareChain;
		d[kChain].missingInputForm = true;
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	// ---- single store (W1-RES): 0 store (singletonStore), 1 reader (the store at lag 1) ----
	constexpr ProductId kStore = 0, kStoreReader = 1;

	Registry SingleStoreRegistry()
	{
		using namespace VpIndicator;
		std::vector<ProductDecl> d;
		ProductDecl store = detail::Make(kStore, "store", "single store", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0);
		store.singletonStore = true;
		d.push_back(store);
		ProductDecl reader = detail::Make(kStoreReader, "store_reader", "store reader", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0);
		detail::AddInput(reader, kStore, 1);
		reader.missingInputForm = true;
		d.push_back(reader);
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	// ---- the real catalog chain products: td.blocks, coding_evidence, quality_pattern (+ causal) ----
	// The declarations are the catalog's own; only the reads that need OTHER lane products (grain split, region stats, source ring) are
	// stripped from td.blocks and coding_evidence so the closure is the chain product itself (those products are exercised elsewhere).
	Registry CatalogChainRegistry()
	{
		std::vector<ProductDecl> d = SkeletonProducts().All();
		const ProductId stripped[] = { kTdBlocks, kCodingEvidence };
		for (ProductId id : stripped) {
			ProductDecl& p = d[id];
			p.inputs.fill(InputRef());
			p.inputCount = 0;
			detail::AddInput(p, id, 1);
		}
		return Registry(std::move(d));
	}

	// Frames 0..3 through the lane protocol by hand. leads: the product reads its input at lead 1 and 2 (quality_pattern): the render
	// is simulated sealing the lead target ahead (a real render seals in frame order, see the README note on lead readers of chains).
	void CatalogChainRuns(const Registry& reg, ProductId product, bool leads)
	{
		Assert::IsTrue(reg.Validate());
		Assert::IsTrue(reg.Get(product).orderedChain && reg.Get(product).missingInputForm, L"declared as an ordered chain with its reduced form");
		ConsumerDecl c;
		c.id = 1;
		c.name = "chain reader";
		AddConsume(c, product);
		Rig r(reg, std::vector<ConsumerDecl>(1, c), 4);
		Assert::IsTrue(r.graph.valid && r.graph.Has(product));
		Assert::IsTrue(r.graph.Find(product)->orderedChain);
		uint64_t made = 0;
		for (uint64_t s = 0; s < 4; ++s) {
			for (; made < s + (leads ? 3u : 1u); ++made)
				for (const Node& n : r.graph.nodes) {
					if (n.id == product)
						continue;
					Assert::IsTrue(r.Lane(n.id, made), L"an input of the chain job is made");
					if (leads && n.id == kCodingEvidence)
						r.Set(n.id).Seal(made, [](const Header&) { return SealReason::Accepted; });
				}
			Assert::IsTrue(r.Lane(product, s), s == 0 ? L"frame 0: the chain start runs reduced" : L"frame N: the sealed predecessor is pinned");
			const auto& res = r.Resolve(s);
			const ProductFrameResult& f = r.Result(res, product);
			Assert::IsTrue(f.seal.reason == SealReason::Accepted && f.final == SealOutcome::Sealed && !f.liveFilled, L"the lane result is accepted");
			Assert::IsTrue(r.Set(product).Read(s).header->chainReduced == (s == 0), L"only frame 0 is reduced");
			r.table.ReleaseAfter(s);
		}
		Assert::AreEqual(uint64_t(1), r.Set(product).Counters().chainCarryForward.load(), L"one declared carry-forward: the start");
		Assert::AreEqual(uint64_t(0), r.Set(product).Counters().live.load(), L"never live-filled");
	}

	class FillProducer : public ILaneProducer {
	public:
		explicit FillProducer(ProductId id) : id_(id) {}
		ProductId Product() const override { return id_; }
		bool Begin(const LaneJobContext& job) override
		{
			chainReduced = job.chainReduced;
			if (job.chainReduced)
				++reducedJobs;
			return true;
		}
		ChunkResult RunChunk(const LaneJobContext& job, uint32_t) override
		{
			if (job.out)
				std::memset(job.out, int(job.seq + id_), job.outBytes);
			return ChunkResult::Done;
		}
		bool chainReduced = false;
		int reducedJobs = 0;

	private:
		ProductId id_;
	};

	// ---- governed history: one T1 GPU product with a full history of 4 down to a floor of 1 ----
	constexpr ProductId kHist = 0;
	constexpr uint64_t kHistBuf = 8ull << 20;

	Registry HistoryRegistry(bool governed = true)
	{
		using namespace VpIndicator;
		std::vector<ProductDecl> d;
		ProductDecl p = detail::Make(kHist, "td_history", "temporal history", Backend::GpuCompute, 4, Tier::T1,
			Bit(kFull) | Bit(kContinuity), 0);
		p.size.fixedBytes = kHistBuf;
		p.refRenderMs = 2.0f;
		p.historyFloor = governed ? 1 : kHistoryFixed;
		d.push_back(p);
		return Registry(std::move(d));
	}

	CardProfile BigCard(uint64_t budgetBytes)
	{
		CardProfile c;
		c.vramBudgetBytes = budgetBytes;
		c.systemRamFreeBytes = 1ull << 30;
		c.uploadGBps = 10.0;
		c.computeScale = 1.0;
		return c;
	}

	struct HistRig {
		Registry reg;
		std::vector<ConsumerDecl> consumers;
		GraphSettings gs;
		HistRig(bool governed = true) : reg(HistoryRegistry(governed)), consumers(OneConsumer(kHist))
		{
			gs.dims.width = 64;
			gs.dims.height = 64;
			gs.laneLead = 1;
			gs.queueDepth = 4;
		}
		TierPlan Plan(uint64_t budgetBytes, const std::vector<HistoryCap>& caps = std::vector<HistoryCap>())
		{
			GraphSettings g = gs;
			g.historyCaps = caps;
			const Graph graph = BuildGraph(reg, consumers, g);
			TierInput in;
			in.headroom = 0.9;
			in.framePeriodMs = 40.0;
			return PlanTiers(reg, graph, BigCard(budgetBytes), in);
		}
	};

	TEST_CLASS(SkeletonGraphTests)
	{
	public:
		// ---- limits ----

		TEST_METHOD(LimitsAreRaisedAndEveryFixedArrayFollows)
		{
			static_assert(kMaxInputs >= 16, "doc 72 5.2");
			static_assert(kMaxConsumes >= 10, "doc 72 5.2");
			static_assert(VpAncestry::kMaxAncestors >= kMaxInputs, "a header records every input");
			Node n;
			Assert::IsTrue(n.inputs.size() == kMaxInputs);
			LaneInputs li;
			Assert::IsTrue(li.sets.size() == kMaxInputs && li.seqs.size() == kMaxInputs && li.liveIndex.size() == kMaxInputs);
			LaneJobContext job;
			Assert::IsTrue(job.inputData.size() == kMaxInputs);
			ConsumerDecl c;
			Assert::IsTrue(c.consumes.size() == kMaxConsumes);
		}

		TEST_METHOD(ANodeWithSixteenInputsSealsAndRecordsEveryAncestor)
		{
			const Registry reg = ManyInputsRegistry();
			Assert::IsTrue(reg.Validate());
			Assert::AreEqual(int(kMaxInputs), int(reg.Get(ProductId(kSources)).inputCount), L"all 16 inputs declared");
			Rig r(reg, OneConsumer(ProductId(kSources)), 1);
			Assert::AreEqual(kSources + 1u, r.graph.nodes.size());
			for (size_t i = 0; i < kSources; ++i)
				Assert::IsTrue(r.Lane(ProductId(i), 0));
			Assert::IsTrue(r.Lane(ProductId(kSources), 0));
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, ProductId(kSources));
			Assert::IsTrue(f.seal.reason == SealReason::Accepted && f.final == SealOutcome::Sealed);
			Assert::AreEqual(int(kSources), int(r.Set(ProductId(kSources)).Read(0).header->ancestorCount));
			// Input 15 is checked as strictly as input 0: replace source 15 after the sink published -> cascade.
			Rig r2(reg, OneConsumer(ProductId(kSources)), 1);
			for (size_t i = 0; i < kSources; ++i)
				r2.Lane(ProductId(i), 0);
			r2.Lane(ProductId(kSources), 0);
			// Source 15 is rejected at its seal (no lane result for frame 0 of it would leave a hole): drop its content.
			r2.Set(ProductId(kSources - 1u)).Discard(0);
			const auto& res2 = r2.Resolve(0);
			Assert::IsTrue(r2.Result(res2, ProductId(kSources)).seal.outcome == SealOutcome::NeedsLive,
				L"the 16th ancestor is validated");
		}

		TEST_METHOD(AConsumerWithTenConsumesBuildsTheWholeClosure)
		{
			const Registry reg = ManyInputsRegistry();
			ConsumerDecl c;
			c.id = 3;
			for (size_t i = 0; i < kMaxConsumes; ++i)
				AddConsume(c, ProductId(i));   // exactly the limit (one more is an overflow, rejected: see below)
			Assert::AreEqual(int(kMaxConsumes), int(c.count));
			Assert::IsFalse(c.overflow);
			GraphSettings gs = Rig::MakeSettings(1);
			const Graph g = BuildGraph(reg, std::vector<ConsumerDecl>(1, c), gs);
			Assert::IsTrue(g.valid);
			Assert::AreEqual(size_t(kMaxConsumes), g.nodes.size());
			for (size_t i = 0; i < kMaxConsumes; ++i)
				Assert::IsTrue(g.Has(ProductId(i)));
			Assert::IsFalse(g.Has(ProductId(kMaxConsumes)));
			// The fingerprint covers the tenth consume: changing it changes the consumer key.
			ConsumerDecl c2 = c;
			c2.consumes[kMaxConsumes - 1u].lag = 1;
			Assert::IsTrue(ConsumerFingerprint(std::vector<ConsumerDecl>(1, c)) != ConsumerFingerprint(std::vector<ConsumerDecl>(1, c2)));
		}

		// ---- lead edges ----

		TEST_METHOD(ATableWithLeadEdgesIsValidAndTheLaneReadsThePublishedAncestor)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			Assert::IsTrue(r.table.Valid());
			const Node* n = r.graph.Find(kLeadReader);
			Assert::IsNotNull(n);
			Assert::AreEqual(2, int(n->inputs[0].lead));
			// The reader cannot run before the ancestor N+2 exists.
			Assert::IsTrue(r.Lane(kBase, 0));
			Assert::IsTrue(r.Lane(kBase, 1));
			{
				Header scratch;
				LaneInputs pins;
				Assert::IsFalse(r.table.LaneAncestors(kLeadReader, 0, scratch, pins), L"base(2) not published yet: the job waits");
				Assert::AreEqual(size_t(0), pins.count, L"nothing stays pinned");
			}
			Assert::IsTrue(r.Lane(kBase, 2));
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			const ReadInfo ri = r.Set(kLeadReader).Read(0, true);
			Assert::IsTrue(ri.kind == ReadInfo::Ring && ri.header->ancestors[0].seq == 2u, L"the ancestor recorded is frame N+2");
			Assert::IsTrue(r.Set(kLeadReader).Read(0).kind == ReadInfo::None, L"Published is not readable without the lead flag");
		}

		TEST_METHOD(TheSealAcceptsAPublishedLeadAncestorWithTheRightStamp)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			Assert::IsTrue(r.Set(kBase).StateOf(2) == SlotState::Published, L"N+2 is not sealed yet");
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, kLeadReader);
			Assert::IsTrue(f.seal.outcome == SealOutcome::Sealed && f.seal.reason == SealReason::Accepted);
			Assert::IsFalse(f.liveFilled);
			Assert::AreEqual(uint64_t(1), r.Set(kBase).Counters().leadAccepted.load());
			Assert::IsTrue(r.Set(kBase).StateOf(2) == SlotState::Published, L"the seal of N does not seal N+2");
		}

		TEST_METHOD(TheSealRejectsALeadAncestorWhoseStampMovedAfterItPublished)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			// A late event re-stamps frame 2 and later: base(2) was honoured under the old stamp.
			r.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 2);
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, kLeadReader);
			Assert::IsTrue(f.seal.outcome == SealOutcome::NeedsLive && f.seal.reason == SealReason::StampMismatch);
			Assert::IsTrue(f.liveFilled && f.final == SealOutcome::Sealed, L"visible live fill, not a silent accept");
			// K1 binds the live fill too: the stale N+2 is a MISSING input, so the declared form runs and is marked.
			Assert::IsTrue((f.missingInputs & 1u) != 0u, L"stale lead ancestor = missing input");
			Assert::IsTrue(r.Set(kLeadReader).Read(0).header->prov == Provenance::LiveFallback, L"never a full LiveRender result");
		}

		TEST_METHOD(ALiveFillRefusesAStaleLeadAncestorEvenWithoutALaneResult)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);   // base(2) Published; no reader lane result
			r.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 2);   // base(2) is now stale
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, kLeadReader);
			Assert::IsTrue(f.liveFilled && (f.missingInputs & 1u) != 0u);
			Assert::IsTrue(r.Set(kLeadReader).Read(0).header->prov == Provenance::LiveFallback);
			// Without a stamp source the live fill cannot vouch for the lead ancestor either.
			Rig n(LeadRegistry(), OneConsumer(kLeadReader), 4, false);
			for (uint64_t s = 0; s < 4; ++s)
				n.Lane(kBase, s);
			const auto& nres = n.Resolve(0);
			Assert::IsTrue((n.Result(nres, kLeadReader).missingInputs & 1u) != 0u);
		}

		TEST_METHOD(AnAcceptedLeadAncestorRejectedAtItsOwnSealIsCounted)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			r.Resolve(0);
			Assert::AreEqual(uint64_t(1), r.Set(kBase).Counters().leadAccepted.load());
			r.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 2);   // base(2) turns stale after the reader used it
			r.Resolve(1);
			r.Resolve(2);
			Assert::AreEqual(uint64_t(1), r.Set(kBase).Counters().leadLaterRejected.load(), L"the documented residual risk is visible");
		}

		TEST_METHOD(ARejectedLeadSealCountsNothing)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			r.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 2);
			r.Resolve(0);
			Assert::AreEqual(uint64_t(0), r.Set(kBase).Counters().leadAccepted.load());
		}

		TEST_METHOD(TheSealRefusesALeadAncestorWithoutAStampSourceVisibly)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4, false);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			const auto& res = r.Resolve(0);
			Assert::IsTrue(r.Result(res, kLeadReader).seal.reason == SealReason::AncestryIncomplete);
			Assert::AreEqual(uint64_t(0), r.Set(kBase).Counters().leadAccepted.load());
		}

		TEST_METHOD(TheSealRejectsAReplacedAndAMissingLeadAncestor)
		{
			// Replaced: the reader recorded base(2); the render re-fills base(2) live first (cgen moves).
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);
			Assert::IsTrue(r.Lane(kLeadReader, 0));
			SlotSet& base = r.Set(kBase);
			Assert::IsTrue(base.Seal(2, [](const Header&) { return SealReason::StampMismatch; }).outcome == SealOutcome::NeedsLive);
			const int live = base.BeginLive(2);
			Assert::IsTrue(live >= 0);
			Header lh;
			lh.prov = Provenance::LiveRender;
			Assert::IsTrue(base.CommitLive(2, lh));
			const auto& res = r.Resolve(0);
			Assert::IsTrue(r.Result(res, kLeadReader).seal.reason == SealReason::AncestorReplaced);

			// Missing: nothing of base(2) exists at the live fill of the reader: the declared missing-input form runs.
			Rig m(LeadRegistry(), OneConsumer(kLeadReader), 4);
			m.Lane(kBase, 0);
			m.Lane(kBase, 1);
			const auto& mres = m.Resolve(0);
			const ProductFrameResult& f = m.Result(mres, kLeadReader);
			Assert::IsTrue(f.missingInputs == 1u && f.liveFilled, L"input 0 (the lead) was missing; the declared form ran");
			Assert::IsTrue(m.Set(kLeadReader).Read(0).header->prov == Provenance::LiveFallback);
		}

		TEST_METHOD(ALiveFillOfTheReaderUsesThePublishedLeadAncestor)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);   // base(2) Published, no reader lane result for frame 0
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, kLeadReader);
			Assert::IsTrue(f.liveFilled && f.missingInputs == 0u, L"a Published lead ancestor is a valid input of a live fill");
			Assert::IsTrue(r.Set(kLeadReader).Read(0).header->ancestors[0].seq == 2u);
		}

		TEST_METHOD(LeadSealingIsDeterministic)
		{
			const uint64_t a = LeadScenarioDigest();
			const uint64_t b = LeadScenarioDigest();
			Assert::IsTrue(a == b);
		}

		TEST_METHOD(AShortQueueStillResolvesToTheCausalSubstituteNotALead)
		{
			GraphSettings gs = Rig::MakeSettings(1);   // lane lead 1: no lead available
			const Graph g = BuildGraph(LeadRegistry(), OneConsumer(kLeadReader), gs);
			Assert::IsTrue(g.valid && g.Has(kCausal) && !g.Has(kLeadReader));
			SlotTable t(g);
			Assert::IsTrue(t.Valid());
			Assert::AreEqual(size_t(1), g.ReducedNodes().size());
		}

		// ---- ordered chain ----

		TEST_METHOD(RegistryRejectsAnOrderedChainWithoutASelfLagInputOrALane)
		{
			Registry ok = ChainRegistry();
			Assert::IsTrue(ok.Validate());
			std::vector<ProductDecl> d = ok.All();
			d[kChain].inputCount = 1;   // drop the self input
			const char* why = "";
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "ordered chain"));
			d = ok.All();
			d[kChain].missingInputForm = false;   // the chain start / a gap live-fills in the reduced form: it must be declared
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "missing-input form"));
			d = ok.All();
			d[kChain].backend = Backend::RenderOnly;
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "ordered chain"));
		}

		TEST_METHOD(ChainOrderAdmitsInOrderCarriesGapsAndRefusesOutOfOrder)
		{
			ChainOrder c;
			Assert::IsTrue(c.Admit(0) == ChainOrder::Verdict::CarryForward, L"chain start has no predecessor");
			c.Published(5);
			Assert::IsTrue(c.Admit(6) == ChainOrder::Verdict::InOrder);
			Assert::IsTrue(c.Admit(9) == ChainOrder::Verdict::CarryForward, L"a gap is a declared carry-forward");
			Assert::IsTrue(c.Admit(5) == ChainOrder::Verdict::Refused, L"a repeat");
			Assert::IsTrue(c.Admit(4) == ChainOrder::Verdict::Refused, L"a late N after N+1: refused, never published");
			c.Published(4);   // an out-of-order publish never moves the tail backwards
			Assert::AreEqual(uint64_t(5), c.Last());
		}

		TEST_METHOD(ChainJobsRunInOrderWithOneCountedStartAndOneCountedGap)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			LaneRunner run(r.table, &r.ind, []() { return uint64_t(0); });
			FillProducer pb(kChainBase), pc(kChain);
			run.Register(&pb);
			run.Register(&pc);
			run.SetBuffers([&r](ProductId p, int slot) { return r.bufs[r.IndexOf(p)].Ring(size_t(slot)); },
				[&r](ProductId p, uint64_t seq, int live) -> const uint8_t* {
					const size_t i = r.IndexOf(p);
					return live >= 0 ? r.bufs[i].Live(size_t(live)) : r.bufs[i].Ring(r.Set(p).SlotOf(seq));
				},
				[&r](ProductId p) { return r.bufs[r.IndexOf(p)].Bytes(); });
			for (uint64_t s = 0; s < 8; ++s)
				run.OnSourceFrame(s);
			auto drive = [&](ProductId p) {
				for (int i = 0; i < 20; ++i) {
					const StepOutcome o = run.Step(p, 1000);
					if (o == StepOutcome::Published || o == StepOutcome::WaitingInputs || o == StepOutcome::WaitingSlot
						|| o == StepOutcome::WaitingSource || o == StepOutcome::Late)
						return o;
				}
				return StepOutcome::Failed;
			};
			for (uint64_t s = 0; s < 3; ++s) {
				Assert::IsTrue(drive(kChainBase) == StepOutcome::Published);
				Assert::IsTrue(drive(kChain) == StepOutcome::Published);
				r.Resolve(s);   // the render seals and releases (frees the slots for the later claims)
				r.table.ReleaseAfter(s);
			}
			Assert::AreEqual(uint64_t(1), run.Counters(kChain).chainCarryForward.load(), L"frame 0: the chain start");
			Assert::AreEqual(1, pc.reducedJobs);
			// A skipped range: the lane resumes at frame 6 (a drop, a seek): the gap is declared, not silent.
			run.SetSkipBefore(6);
			Assert::IsTrue(drive(kChainBase) == StepOutcome::Published);
			Assert::IsTrue(drive(kChain) == StepOutcome::Published);
			Assert::AreEqual(uint64_t(2), run.Counters(kChain).chainCarryForward.load());
			Assert::AreEqual(2, pc.reducedJobs);
			// And the seal accepts the carried-forward frame (the empty self ancestor is declared).
			const auto& res = r.Resolve(6);
			const ProductFrameResult& f = r.Result(res, kChain);
			Assert::IsTrue(f.seal.reason == SealReason::Accepted && f.final == SealOutcome::Sealed);
			Assert::AreEqual(uint64_t(2), r.Set(kChain).Counters().chainCarryForward.load(), L"sealed carry-forwards: start + gap");
			Assert::IsTrue(r.Set(kChain).Read(6).header->chainReduced);
		}

		TEST_METHOD(AnUndeclaredHoleInTheChainIsRejectedAtTheSeal)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 2);
			Assert::IsTrue(r.Lane(kChain, 2, true), L"frame 1 never published; the flag is tampered off");
			const auto& res = r.Resolve(2);
			const ProductFrameResult& f = r.Result(res, kChain);
			Assert::IsTrue(f.seal.outcome == SealOutcome::NeedsLive, L"not accepted without the declaration");
			Assert::IsTrue(f.seal.reason == SealReason::AncestorGone || f.seal.reason == SealReason::AncestryIncomplete
				|| f.seal.reason == SealReason::AncestorInvalid);
		}

		TEST_METHOD(WithoutTheChainDeclarationAMissingSelfInputStillWaitsAsToday)
		{
			Rig r(ChainRegistry(false), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 2);
			Assert::IsFalse(r.Lane(kChain, 2), L"no chain: the lag-1 self input is required (today's behaviour)");
		}

		TEST_METHOD(ALateChainStartLiveFillsInTheReducedFormNotUnavailable)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);   // no lane result for the chain at frame 0
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, kChain);
			Assert::IsTrue(f.liveFilled && f.final == SealOutcome::Sealed);
			Assert::IsTrue((f.missingInputs & 2u) != 0u, L"the self lag input is the missing one");
			Assert::IsTrue(r.Set(kChain).Read(0).header->prov == Provenance::LiveFallback);
		}

		TEST_METHOD(TheSealRejectsAChainCarryForwardWhenThePredecessorIsValid)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);
			r.Lane(kChainBase, 1);
			Assert::IsTrue(r.Lane(kChain, 0));
			r.Resolve(0);   // chain(0) is Sealed: a valid predecessor
			Assert::IsTrue(r.Lane(kChain, 1, false, true));
			const auto& res = r.Resolve(1);
			const ProductFrameResult& f = r.Result(res, kChain);
			Assert::IsTrue(f.seal.outcome == SealOutcome::NeedsLive && f.seal.reason == SealReason::AncestryIncomplete);
			Assert::IsTrue(f.liveFilled && f.missingInputs == 0u, L"made in full by the live fill, not reduced");
			Assert::IsTrue(r.Set(kChain).Read(1).header->prov == Provenance::LiveRender);
		}

		TEST_METHOD(ATransientPredecessorIsWaitedForNotCarriedForward)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);
			r.Lane(kChainBase, 1);
			// chain(0): rejected by the seal and not live-filled yet (the render will fill it): content may still come.
			Assert::IsTrue(r.Set(kChain).Seal(0, [](const Header&) { return SealReason::StampMismatch; }).outcome == SealOutcome::NeedsLive);
			Assert::IsTrue(r.Set(kChain).AbsenceOf(0) == SlotSet::Absence::Transient);
			Header scratch;
			LaneInputs pins;
			Assert::IsFalse(r.table.LaneAncestors(kChain, 1, scratch, pins), L"the job waits");
			Assert::IsFalse(scratch.chainReduced);
			// A frame that was never claimed is absent for good: carried forward.
			Assert::IsTrue(r.Set(kChain).AbsenceOf(5) == SlotSet::Absence::Absent);
		}

		TEST_METHOD(AChainWaitingOnARejectedPredecessorIsCountedOncePerFrame)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);
			r.Lane(kChainBase, 1);
			// The live fill of chain(0) is DECLINED: the slot stays Rejected with no content.
			const Stamp st = r.ind.Seal(0);
			const auto& res = r.table.Resolve(0, st, [](const Node& n, uint64_t, int, Header&, uint32_t) { return n.id != kChain; });
			Assert::IsTrue(r.Result(res, kChain).final == SealOutcome::Unavailable
				&& r.Result(res, kChain).finalReason == SealReason::FillDeclined);
			Assert::IsTrue(r.Set(kChain).RejectedAwaitingLive(0));
			Assert::AreEqual(uint64_t(0), r.Set(kChain).Counters().chainWaitRejected.load());
			Header scratch;
			LaneInputs pins;
			for (int poll = 0; poll < 5; ++poll)
				Assert::IsFalse(r.table.LaneAncestors(kChain, 1, scratch, pins), L"the successor job waits");
			Assert::AreEqual(uint64_t(1), r.Set(kChain).Counters().chainWaitRejected.load(), L"one frame waited, not five polls");
			// A frame that was never claimed is Absent, not Rejected: carried forward, nothing counted.
			Assert::IsFalse(r.Set(kChain).RejectedAwaitingLive(5));
			// The same through the seal-only path (no live buffer was ever begun: the NoLiveBuffer shape).
			Rig q(ChainRegistry(), OneConsumer(kChain), 2);
			q.Lane(kChainBase, 0);
			q.Lane(kChainBase, 1);
			q.Set(kChain).Seal(0, [](const Header&) { return SealReason::StampMismatch; });
			Assert::IsFalse(q.table.LaneAncestors(kChain, 1, scratch, pins));
			Assert::AreEqual(uint64_t(1), q.Set(kChain).Counters().chainWaitRejected.load());
		}

		TEST_METHOD(ARealPredecessorIsPinnedNotCarriedForward)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);
			r.Lane(kChainBase, 1);
			Assert::IsTrue(r.Lane(kChain, 0));   // chain start: carried
			r.Resolve(0);                        // the predecessor is sealed: render-valid
			Assert::IsTrue(r.Lane(kChain, 1));   // predecessor present
			Assert::IsTrue(r.Set(kChain).Read(0, true).header->chainReduced);
			Assert::IsFalse(r.Set(kChain).Read(1, true).header->chainReduced);
			Assert::IsTrue(r.Set(kChain).Read(1, true).header->ancestors[1].cgen != 0u);
		}

		TEST_METHOD(AChainJobPinsOnlyARenderValidPredecessorAndTheWaitIsCounted)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);
			r.Lane(kChainBase, 1);
			Assert::IsTrue(r.Lane(kChain, 0));
			Assert::IsTrue(r.Set(kChain).StateOf(0) == SlotState::Published, L"published, not sealed");
			Header scratch;
			LaneInputs pins;
			for (int poll = 0; poll < 5; ++poll)
				Assert::IsFalse(r.table.LaneAncestors(kChain, 1, scratch, pins), L"a Published predecessor is Transient: the job waits");
			Assert::AreEqual(size_t(0), pins.count, L"nothing stays pinned");
			Assert::IsFalse(scratch.chainReduced, L"waited, not carried forward");
			Assert::AreEqual(uint64_t(1), r.Set(kChain).Counters().chainWaitUnsealed.load(), L"one frame waited, not five polls");
			Assert::AreEqual(uint64_t(0), r.Set(kChain).Counters().chainWaitRejected.load());
			// The pin rule is the chain's: an ordinary reader (and a lead reader) may still pin the Published content.
			AncestorRef ref;
			int live = -1;
			Assert::IsFalse(r.Set(kChain).PinRef(0, &ref, &live, true), L"render-valid only: refused");
			Assert::IsTrue(r.Set(kChain).PinRef(0, &ref, &live), L"the default pin still takes Published content");
			r.Set(kChain).Unpin(0, live);
			r.Resolve(0);
			Assert::IsTrue(r.Set(kChain).PinRef(0, &ref, &live, true), L"sealed: pinnable");
			r.Set(kChain).Unpin(0, live);
			Assert::IsTrue(r.Lane(kChain, 1), L"the wait ends at the seal");
			Assert::IsFalse(r.Set(kChain).Read(1, true).header->chainReduced);
		}

		TEST_METHOD(AChainRecoversFromOneRejectionWithoutACascade)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			std::vector<bool> accepted;
			std::vector<bool> liveFilled;
			r.Lane(kChainBase, 0);
			for (uint64_t s = 0; s < 8; ++s) {
				Assert::IsTrue(r.Lane(kChain, s), L"the chain job runs once its predecessor is sealed or live");
				if (s == 3)
					r.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 3);   // frame 3 was made under the old stamp: rejected at its seal
				r.Lane(kChainBase, s + 1);
				// The lane tries to run one frame ahead of the seal: it must wait (the predecessor is only Published).
				Header scratch;
				LaneInputs pins;
				Assert::IsFalse(r.table.LaneAncestors(kChain, s + 1, scratch, pins), L"no chain job ahead of the predecessor's seal");
				Assert::AreEqual(size_t(0), pins.count);
				const auto& res = r.Resolve(s);
				const ProductFrameResult& f = r.Result(res, kChain);
				accepted.push_back(f.seal.reason == SealReason::Accepted);
				liveFilled.push_back(f.liveFilled);
				Assert::IsTrue(f.final == SealOutcome::Sealed, L"every frame has valid content");
				r.table.ReleaseAfter(s);
			}
			for (uint64_t s = 0; s < 8; ++s) {
				if (s == 3) {
					Assert::IsFalse(accepted[3]);
					Assert::IsTrue(liveFilled[3], L"the rejected frame is live-filled once");
				} else {
					Assert::IsTrue(accepted[s] && !liveFilled[s], L"no cascade: the next frame (and every later one) is accepted from the lane");
				}
			}
			Assert::AreEqual(uint64_t(1), r.Set(kChain).Counters().live.load(), L"exactly one live fill of the chain product");
			Assert::AreEqual(uint64_t(8), r.Set(kChain).Counters().chainWaitUnsealed.load(), L"each waiting frame is visible, once");
		}

		TEST_METHOD(TheCatalogChainProductsAreMadeByTheLaneFromFrameOne)
		{
			const Registry reg = CatalogChainRegistry();
			CatalogChainRuns(reg, kTdBlocks, false);
			CatalogChainRuns(reg, kCodingEvidence, false);
			CatalogChainRuns(reg, kQualityPatternCausal, false);
			CatalogChainRuns(reg, kQualityPattern, true);
			// The full catalog (nothing stripped) validates with every chain declared.
			Assert::IsTrue(SkeletonProducts().Validate());
		}

		TEST_METHOD(ALiveFillThatReadAPublishedLeadAncestorIsCountedWhenThatAncestorIsRejectedLater)
		{
			Rig r(LeadRegistry(), OneConsumer(kLeadReader), 4);
			for (uint64_t s = 0; s < 4; ++s)
				r.Lane(kBase, s);   // base(2) Published; no reader lane result for frame 0: the reader is live-filled from it
			const auto& res = r.Resolve(0);
			const ProductFrameResult& f = r.Result(res, kLeadReader);
			Assert::IsTrue(f.liveFilled && f.missingInputs == 0u, L"the live fill used the Published lead ancestor");
			Assert::AreEqual(uint64_t(0), r.Set(kBase).Counters().leadLaterRejected.load());
			r.ind.Advance(VpIndicator::kFull, Cause::RendererReset, 2);   // base(2) turns stale after the live fill used it
			r.Resolve(1);
			r.Resolve(2);
			Assert::AreEqual(uint64_t(1), r.Set(kBase).Counters().leadLaterRejected.load(), L"the live fill marked the lead ancestor: the residual risk is visible");
		}

		// ---- declaration overflow fails closed (also in Release, where the assert is compiled out) ----

		TEST_METHOD(AnInputOverflowIsRejectedByTheRegistryWithAPlainReason)
		{
			std::vector<ProductDecl> d = ManyInputsRegistry().All();
			ProductDecl& sink = d[kSources];
			Assert::IsFalse(sink.overflow, L"exactly the limit is not an overflow");
			Assert::IsTrue(Registry(d).Validate());
#ifdef NDEBUG   // the helper asserts in a Debug build; in Release it must set the flag, not drop the read silently
			detail::AddInput(sink, ProductId(0), 0);
			Assert::IsTrue(sink.overflow);
			Assert::AreEqual(size_t(kMaxInputs), size_t(sink.inputCount), L"the extra read was not recorded");
#else
			sink.overflow = true;
#endif
			const char* why = "";
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "more inputs"));
			const Graph g = BuildGraph(Registry(d), OneConsumer(ProductId(kSources)), Rig::MakeSettings(1));
			Assert::IsFalse(g.valid);
			Assert::IsTrue(Has(g.error, "more inputs"));
			// A look-ahead overflow too.
			d = ManyInputsRegistry().All();
#ifdef NDEBUG
			detail::AddLead(d[kSources], ProductId(0), 1);
			Assert::IsTrue(d[kSources].overflow);
#else
			d[kSources].overflow = true;
#endif
			Assert::IsFalse(Registry(d).Validate());
		}

		TEST_METHOD(AConsumerOverflowFailsTheGraphWithAPlainReason)
		{
			ConsumerDecl c;
			c.id = 1;
			c.name = "greedy";
			for (size_t i = 0; i < kMaxConsumes; ++i)
				AddConsume(c, kBase);
			Assert::IsFalse(c.overflow, L"exactly the limit is not an overflow");
			Assert::IsTrue(BuildGraph(LeadRegistry(), std::vector<ConsumerDecl>(1, c), Rig::MakeSettings(2)).valid);
#ifdef NDEBUG
			AddConsume(c, kBase);
			Assert::IsTrue(c.overflow);
			Assert::AreEqual(size_t(kMaxConsumes), size_t(c.count), L"the extra read was not recorded");
#else
			c.overflow = true;
#endif
			const Graph g = BuildGraph(LeadRegistry(), std::vector<ConsumerDecl>(1, c), Rig::MakeSettings(2));
			Assert::IsFalse(g.valid);
			Assert::IsTrue(Has(g.error, "more reads"));
		}

		TEST_METHOD(AnOverflowedDeclarationMakesTheLaneUnavailableAndSaysSo)
		{
			std::vector<ProductDecl> d = ManyInputsRegistry().All();
#ifdef NDEBUG
			detail::AddInput(d[kSources], ProductId(0), 0);
#else
			d[kSources].overflow = true;
#endif
			Indicator ind(9, 256);
			AnalysisLane lane(&ind, []() { return uint64_t(0); });
			LaneSettings ls;
			ls.mode = LaneSettings::Mode::On;
			TierInput tier;
			tier.framePeriodMs = 40.0;
			lane.Configure(ls, Registry(d), OneConsumer(ProductId(kSources)), Rig::MakeSettings(1), BigCard(1ull << 30), tier, 0, false);
			Assert::IsFalse(lane.Active());
			bool shown = false;
			for (const FallbackCatalog::Decision& f : lane.Fallbacks())
				shown = shown || (f.engaged && Has(f.reason, "more inputs"));
			Assert::IsTrue(shown, L"the reason reaches the Fallbacks list (Config status)");
			Assert::IsTrue(Has(lane.StatusLines().front(), "more inputs"));
		}

		// ---- single store (W1-RES) ----

		TEST_METHOD(ASingleStoreIsValidatedAndCopiedToTheNode)
		{
			using namespace VpIndicator;
			std::vector<ProductDecl> d;
			ProductDecl store = detail::Make(0, "store", "single store", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0);
			store.singletonStore = true;
			store.size.fixedBytes = 64;
			d.push_back(store);
			ProductDecl reader = detail::Make(1, "reader", "store reader", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0);
			detail::AddInput(reader, 0, 1);
			reader.size.fixedBytes = 64;
			d.push_back(reader);
			Assert::IsTrue(Registry(d).Validate());
			const Graph g = BuildGraph(Registry(d), OneConsumer(1), Rig::MakeSettings(1));
			Assert::IsTrue(g.valid);
			Assert::IsTrue(g.Find(0)->singletonStore);
			Assert::IsFalse(g.Find(1)->singletonStore);
			Assert::AreEqual(1, int(g.Find(0)->history), L"depth stays max(0, largest lag read)");
			Assert::AreEqual(1, int(g.Find(0)->historyMin), L"never governed: no shedding below the lag");

			const char* why = "";
			std::vector<ProductDecl> bad = d;
			bad[0].history = 2;
			Assert::IsFalse(Registry(bad).Validate(&why));
			Assert::IsTrue(Has(why, "single-store"));
			bad = d;
			bad[0].historyFloor = 0;
			Assert::IsFalse(Registry(bad).Validate(&why));
			Assert::IsTrue(Has(why, "single-store"));
			bad = d;
			detail::AddInput(bad[0], 0, 1);
			Assert::IsFalse(Registry(bad).Validate(&why));
			Assert::IsTrue(Has(why, "single-store"));
		}

		TEST_METHOD(ASingleStoreIsATwoBufferPingPongWithoutALivePool)
		{
			GraphSettings gs = Rig::MakeSettings(4);
			gs.renderAhead = 3;
			gs.laneJobsInFlight = 2;
			const Graph g = BuildGraph(SingleStoreRegistry(), OneConsumer(kStoreReader), gs);
			Assert::IsTrue(g.valid);
			const Node* store = g.Find(kStore);
			Assert::IsNotNull(store);
			Assert::AreEqual(int(kSingletonStoreBuffers), int(store->ring), L"ring = 2 whatever R and L are");
			Assert::AreEqual(0, int(store->livePool), L"lane-only: no live pool");
			Assert::AreEqual(1, int(store->history), L"depth stays the largest lag read");
			Assert::AreEqual(store->bufferBytes * 2u, store->ringBytes, L"2x the store");
			Assert::AreEqual(uint64_t(0), store->liveBytes);
			Assert::IsTrue(g.Find(kStoreReader)->ring > 2u, L"an ordinary product keeps its R / L ring");
			// The slot table follows the node: no live buffer exists.
			SlotTable t(g);
			Assert::AreEqual(2, int(t.Find(kStore)->Ring()));
			Assert::AreEqual(0, int(t.Find(kStore)->LivePool()));
			Assert::AreEqual(-1, t.Find(kStore)->BeginLive(0), L"nothing to begin");
			// Different R and L: the same two buffers.
			gs.renderAhead = 0;
			gs.laneLead = 1;
			Assert::AreEqual(2, int(BuildGraph(SingleStoreRegistry(), OneConsumer(kStoreReader), gs).Find(kStore)->ring));
		}

		TEST_METHOD(AnAbsentSingleStoreFrameIsUnavailableAndTheReaderRunsItsOwnFallback)
		{
			Rig r(SingleStoreRegistry(), OneConsumer(kStoreReader), 2);
			const auto& res0 = r.Resolve(0);   // the lane made nothing: there is no live fill for a store
			const ProductFrameResult& s0 = r.Result(res0, kStore);
			Assert::IsTrue(s0.final == SealOutcome::Unavailable && s0.finalReason == SealReason::NoLiveBuffer && !s0.liveFilled);
			Assert::AreEqual(uint64_t(0), r.Set(kStore).Counters().noLiveBuffer.load(), L"not a pool shortage: there is no pool");
			const ProductFrameResult& rd0 = r.Result(res0, kStoreReader);
			Assert::IsTrue(rd0.liveFilled && (rd0.missingInputs & 1u) != 0u, L"frame 0: no lag-1 store at all");
			const auto& res1 = r.Resolve(1);
			const ProductFrameResult& rd1 = r.Result(res1, kStoreReader);
			Assert::IsTrue(rd1.liveFilled && (rd1.missingInputs & 1u) != 0u, L"frame 1: the store of frame 0 was never made: the declared reduced form");
			Assert::IsTrue(r.Set(kStoreReader).Read(1).header->prov == Provenance::LiveFallback, L"visible, never a full result");
		}

		TEST_METHOD(ASingleStoreProducerWaitsWhileALagOneReaderPinsItsSlotAndTheWaitIsCounted)
		{
			Rig r(SingleStoreRegistry(), OneConsumer(kStoreReader), 2);
			Assert::IsTrue(r.Lane(kStore, 0));
			r.Resolve(0);
			Assert::IsTrue(r.Lane(kStore, 1));
			// A lag-1 reader job at frame 1 holds store(0) (slot 0) through its whole job.
			Header readerHdr;
			LaneInputs held;
			Assert::IsTrue(r.table.LaneAncestors(kStoreReader, 1, readerHdr, held));
			Assert::AreEqual(size_t(1), held.count);
			r.Resolve(1);
			r.table.ReleaseAfter(1);   // the render is done with frame 0 (history 1): the slot is released, the lane reader is not
			// The producer's job for frame 2 uses the slot of frame 0.
			LaneRunner run(r.table, &r.ind, []() { return uint64_t(0); });
			FillProducer ps(kStore);
			run.Register(&ps);
			for (uint64_t s = 0; s < 3; ++s)
				run.OnSourceFrame(s);
			for (int poll = 0; poll < 4; ++poll)
				Assert::IsTrue(run.Step(kStore, 1000) == StepOutcome::WaitingSlot, L"waits (Transient) while the reader pins the slot");
			Assert::AreEqual(uint64_t(1), r.Set(kStore).Counters().writeWaitPinned.load(), L"counted once per job, not per poll");
			held.Unpin();
			StepOutcome o = StepOutcome::Failed;
			for (int i = 0; i < 10 && o != StepOutcome::Published; ++i)
				o = run.Step(kStore, 1000);
			Assert::IsTrue(o == StepOutcome::Published, L"the wait ends when the reader lets go");
			Assert::AreEqual(uint64_t(1), r.Set(kStore).Counters().writeWaitPinned.load());
		}

		TEST_METHOD(ASingleStoreAcceptsOnlyLagZeroOrOneReaders)
		{
			std::vector<ProductDecl> d = SingleStoreRegistry().All();
			Assert::IsTrue(Registry(d).Validate());
			detail::AddInput(d[kStoreReader], kStore, 0);   // lag 0 is fine too
			Assert::IsTrue(Registry(d).Validate());
			d = SingleStoreRegistry().All();
			d[kStoreReader].inputs[0].lag = 2;
			const char* why = "";
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "single-store") && Has(why, "lag"));
			// A consumer reading it deeper than lag 1 fails the graph with the same plain reason.
			ConsumerDecl c;
			c.id = 1;
			c.name = "deep reader";
			AddConsume(c, kStore, 2);
			const Graph g = BuildGraph(SingleStoreRegistry(), std::vector<ConsumerDecl>(1, c), Rig::MakeSettings(2));
			Assert::IsFalse(g.valid);
			Assert::IsTrue(Has(g.error, "single-store"));
			Assert::IsTrue(g.errorProduct == kStore);
			ConsumerDecl ok;
			ok.id = 1;
			ok.name = "lag 1 reader";
			AddConsume(ok, kStore, 1);
			Assert::IsTrue(BuildGraph(SingleStoreRegistry(), std::vector<ConsumerDecl>(1, ok), Rig::MakeSettings(2)).valid);
			// No lane, no live fill: a render-only store would have no storage at all.
			d = SingleStoreRegistry().All();
			d[kStore].backend = Backend::RenderOnly;
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "single-store") && Has(why, "lane"));
		}

		// ---- budget-governed history ----

		TEST_METHOD(RegistryRejectsAHistoryFloorAboveTheFullHistory)
		{
			std::vector<ProductDecl> d = HistoryRegistry().All();
			d[0].historyFloor = 5;
			const char* why = "";
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::IsTrue(Has(why, "history floor"));
		}

		TEST_METHOD(AFixedHistoryProductIsNeverShed)
		{
			HistRig h(false);
			const TierPlan big = h.Plan(1ull << 30);
			const TierPlan tiny = h.Plan(64ull << 20);
			Assert::IsTrue(tiny.historyCaps.empty());
			Assert::AreEqual(4, int(tiny.Find(kHist)->historyGranted));
			Assert::IsTrue(big.historyCaps.empty());
		}

		TEST_METHOD(TheGovernorGrantsFullHistoryWhenItFits)
		{
			HistRig h;
			const TierPlan p = h.Plan(1ull << 30);
			Assert::IsTrue(p.historyCaps.empty());
			Assert::AreEqual(4, int(p.Find(kHist)->historyGranted));
			Assert::IsFalse(p.Find(kHist)->historyReduced);
			Assert::IsTrue(p.Laned(kHist));
		}

		TEST_METHOD(TheGovernorShedsHistoryInStepsAndSaysSo)
		{
			HistRig h;
			// Sweep the budget down: the lane drops first (lossless), then history steps 4 -> 1, monotonically.
			int last = 4;
			bool sawMiddle = false, sawFloor = false, sawLaneDrop = false;
			for (uint64_t mb = 400; mb >= 8; mb -= 4) {
				const TierPlan p = h.Plan(mb << 20);
				const TierEntry* e = p.Find(kHist);
				Assert::IsNotNull(e);
				Assert::IsTrue(int(e->historyGranted) <= last, L"history never grows as the budget shrinks");
				last = int(e->historyGranted);
				Assert::IsTrue(e->historyGranted >= 1);
				sawLaneDrop = sawLaneDrop || (!p.Laned(kHist) && e->historyGranted == 4);
				if (e->historyGranted > 1 && e->historyGranted < 4)
					sawMiddle = true;
				if (e->historyGranted == 1)
					sawFloor = true;
				if (e->historyReduced) {
					Assert::IsTrue(p.historyCaps.size() == 1 && p.historyCaps[0].id == kHist
						&& p.historyCaps[0].history == e->historyGranted);
					Assert::IsFalse(p.Laned(kHist), L"history is shed after the lossless lane drop");
					Assert::IsTrue(Has(p.notice, "past frames of history"), L"visible, never silent");
				} else {
					Assert::IsTrue(p.historyCaps.empty());
				}
			}
			Assert::IsTrue(sawLaneDrop && sawMiddle && sawFloor);
		}

		TEST_METHOD(TheGraphIsBuiltWithTheGrantAndListsTheReducedNode)
		{
			HistRig h;
			GraphSettings gs = h.gs;
			const Graph full = BuildGraph(h.reg, h.consumers, gs);
			Assert::AreEqual(4, int(full.Find(kHist)->history));
			Assert::IsTrue(full.HistoryReducedNodes().empty());
			HistoryCap cap;
			cap.id = kHist;
			cap.history = 2;
			gs.historyCaps.push_back(cap);
			const Graph cut = BuildGraph(h.reg, h.consumers, gs);
			const Node* n = cut.Find(kHist);
			Assert::AreEqual(2, int(n->history));
			Assert::AreEqual(4, int(n->historyFull));
			Assert::AreEqual(1, int(n->historyMin));
			Assert::AreEqual(int(full.Find(kHist)->ring) - 2, int(n->ring), L"ring and live pool shrink by the shed frames");
			Assert::AreEqual(int(full.Find(kHist)->livePool) - 2, int(n->livePool));
			Assert::AreEqual(size_t(1), cut.HistoryReducedNodes().size());
			// A cap below the floor is clamped; a cap above the full depth is clamped.
			gs.historyCaps[0].history = 0;
			Assert::AreEqual(1, int(BuildGraph(h.reg, h.consumers, gs).Find(kHist)->history));
			gs.historyCaps[0].history = 9;
			Assert::AreEqual(4, int(BuildGraph(h.reg, h.consumers, gs).Find(kHist)->history));
			// A rebuild is triggered by a changed grant, and only by that.
			GraphSettings a = h.gs, b = h.gs;
			b.historyCaps.push_back(cap);
			const VpIndicator::Stamp st;
			Assert::AreEqual(uint32_t(kRebuildHistory),
				RebuildReasons(KeyOf(st, a, h.consumers), KeyOf(st, b, h.consumers), false));
		}

		TEST_METHOD(AReaderLagKeepsTheHistoryItReadsEvenAtTheFloor)
		{
			// A consumer reading the product at lag 3 pins the depth at 3 whatever the governor grants.
			HistRig h;
			h.consumers[0].count = 0;
			AddConsume(h.consumers[0], kHist, 3);
			GraphSettings gs = h.gs;
			HistoryCap cap;
			cap.id = kHist;
			cap.history = 1;
			gs.historyCaps.push_back(cap);
			const Graph g = BuildGraph(h.reg, h.consumers, gs);
			Assert::AreEqual(3, int(g.Find(kHist)->history));
			Assert::AreEqual(3, int(g.Find(kHist)->historyMin));
		}

		TEST_METHOD(TheAnalysisLaneAppliesTheGrantAndShowsIt)
		{
			Indicator ind(9, 256);
			AnalysisLane lane(&ind, []() { return uint64_t(0); });
			HistRig h;
			LaneSettings ls;
			ls.mode = LaneSettings::Mode::On;
			TierInput tier;
			tier.framePeriodMs = 40.0;
			// Full history: no grant.
			lane.Configure(ls, h.reg, h.consumers, h.gs, BigCard(1ull << 30), tier, 0, false);
			Assert::IsTrue(lane.Active());
			Assert::AreEqual(4, int(lane.State()->graph.Find(kHist)->history));
			// A card with room for little: the grant caps the history and the graph is rebuilt with it.
			AnalysisLane lane2(&ind, []() { return uint64_t(0); });
			lane2.Configure(ls, h.reg, h.consumers, h.gs, BigCard(30ull << 20), tier, 0, false);
			Assert::IsTrue(lane2.Active());
			const Node* n = lane2.State()->graph.Find(kHist);
			Assert::IsTrue(n->history < n->historyFull, L"the lane built the graph with the granted depth");
			bool shown = false;
			for (const std::string& l : lane2.StatusLines())
				shown = shown || Has(l, "past frames of history");
			Assert::IsTrue(shown, L"the reduced history is on the status lines");
			// The same Configure again keeps the graph (no flapping on the grant).
			const uint32_t rebuilds = lane2.Rebuilds();
			Assert::AreEqual(uint32_t(0), lane2.Configure(ls, h.reg, h.consumers, h.gs, BigCard(30ull << 20), tier, 0, false));
			Assert::AreEqual(rebuilds, lane2.Rebuilds());
		}
	};
}
