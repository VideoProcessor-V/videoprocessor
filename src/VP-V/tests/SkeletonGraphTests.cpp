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
		bool Lane(ProductId p, uint64_t seq, bool breakChainFlag = false)
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
		for (size_t i = 0; i < kSources + 1u; ++i)   // one more than fits: the extra is dropped, never written past the array
			detail::AddInput(sink, ProductId(i < kSources ? i : 0), 0);
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
			Assert::AreEqual(int(kMaxInputs), int(reg.Get(ProductId(kSources)).inputCount), L"the 17th input is dropped, not written past the array");
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
			for (size_t i = 0; i < 12; ++i)
				AddConsume(c, ProductId(i));   // 12 asked, 10 fit
			Assert::AreEqual(int(kMaxConsumes), int(c.count));
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

		TEST_METHOD(ARealPredecessorIsPinnedNotCarriedForward)
		{
			Rig r(ChainRegistry(), OneConsumer(kChain), 2);
			r.Lane(kChainBase, 0);
			r.Lane(kChainBase, 1);
			Assert::IsTrue(r.Lane(kChain, 0));   // chain start: carried
			Assert::IsTrue(r.Lane(kChain, 1));   // predecessor present
			Assert::IsTrue(r.Set(kChain).Read(0, true).header->chainReduced);
			Assert::IsFalse(r.Set(kChain).Read(1, true).header->chainReduced);
			Assert::IsTrue(r.Set(kChain).Read(1, true).header->ancestors[1].cgen != 0u);
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
