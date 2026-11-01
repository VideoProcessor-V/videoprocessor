// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link layer K0: the camera products P7/P8/P9 in the registry (look-ahead inputs, the
// short-queue substitute), the camera consumers, the Indicator reader policies, the seal-to-store
// adapter and the "PublishCamera has one caller" source scan. CPU only.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/modules/motion/estimation/VectorDemandProducts.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>
#include <VP-V/modules/motion/estimation/global_model/CameraReaders.h>
#include <VP-V/modules/motion/estimation/global_model/CameraStoreAdapter.h>

#include "../../../../tests/SettingsTestSupport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraProductsTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Bit;
	using VpIndicator::Indicator;
	using VpResync::Cause;

	GraphSettings Settings(uint8_t laneLead, bool ofa = true, uint8_t r = 0)
	{
		GraphSettings gs;
		gs.dims.width = 1920;
		gs.dims.height = 1080;
		gs.renderAhead = r;
		gs.laneLead = laneLead;
		gs.queueDepth = 6;
		gs.lookAheadWalk = 2;
		gs.ofaPresent = ofa;
		return gs;
	}

	std::vector<ConsumerDecl> Camera(std::initializer_list<CameraLink::CameraConsumer> active)
	{
		CameraLink::CameraDemandInput in;
		for (CameraLink::CameraConsumer c : active)
			in.active[static_cast<int>(c)] = true;
		std::vector<ConsumerDecl> out;
		CameraLink::DeclareCameraConsumers(in, out);
		return out;
	}

	int CountOf(const Graph& g, ProductId id)
	{
		int n = 0;
		for (const Node& node : g.nodes)
			n += node.id == id ? 1 : 0;
		return n;
	}

	bool ValidateFails(std::vector<ProductDecl> decls)
	{
		const char* why = "";
		const bool ok = Registry(std::move(decls)).Validate(&why);
		return !ok && why != nullptr && why[0] != '\0';
	}

	FrameAnalysis::Identity Id(uint64_t seq)
	{
		FrameAnalysis::Identity id;
		id.seq = seq;
		id.width = 64;
		id.height = 36;
		return id;
	}

	std::shared_ptr<const CameraModel::CameraMotion> Cam(uint64_t seq)
	{
		auto c = std::make_shared<CameraModel::CameraMotion>();
		c->seq = seq;
		c->valid = true;
		c->confidence = 0.9;
		return c;
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		std::replace(s.begin(), s.end(), '/', '\\');
		return s;
	}

	// Reader counts for the policy tests.
	struct Counts
	{
		int resets = 0;
		int relatches = 0;
	};

	TEST_CLASS(CameraProductsTests)
	{
	public:
		TEST_METHOD(SkeletonProductsValidateAndKeepTheFirstIds)
		{
			const Registry reg = SkeletonProducts();
			const char* why = "";
			Assert::IsTrue(reg.Validate(&why), L"skeleton validates");
			Assert::AreEqual(size_t(kSkeletonProductEnd), reg.Size());
			Assert::AreEqual(int(kFirstProductCount), 8);
			Assert::AreEqual(int(kP7CameraFit), int(kFirstProductCount));
			Assert::AreEqual(int(kP8CameraLabels), int(kFirstProductCount) + 1);
			Assert::AreEqual(int(kP9CameraSmoothed), int(kFirstProductCount) + 2);
			const Registry first = FirstProducts();
			for (size_t i = 0; i < first.Size(); ++i)
				Assert::AreEqual(std::string(first.Get(ProductId(i)).token), std::string(reg.Get(ProductId(i)).token));
		}

		TEST_METHOD(CameraProductFields)
		{
			using namespace VpIndicator;
			const Registry reg = SkeletonProducts();
			const CounterMask content = Bit(kFull) | Bit(kContinuity);

			const ProductDecl& p7 = reg.Get(kP7CameraFit);
			Assert::AreEqual(std::string("p7_camera_fit"), std::string(p7.token));
			Assert::IsTrue(p7.backend == Backend::Cpu);
			Assert::IsTrue(p7.residency == Residency::SystemRam);
			Assert::AreEqual(1, int(p7.history));
			Assert::IsTrue(p7.tier == Tier::T0);
			Assert::IsTrue(p7.variants == Variants::Identical);
			Assert::AreEqual(3, int(p7.inputCount));
			Assert::AreEqual(int(kP3AnalysisField), int(p7.inputs[0].product));
			Assert::AreEqual(int(kP4SourceStats), int(p7.inputs[1].product));
			Assert::AreEqual(int(kP2VectorPair), int(p7.inputs[2].product));
			for (int k = 0; k < 3; ++k)
				Assert::IsTrue(p7.inputs[k].lag == 0 && p7.inputs[k].lead == 0);
			Assert::AreEqual(content | Bit(kSettingsMotion), p7.reads);
			Assert::AreEqual(content, p7.spanMask);
			Assert::AreEqual(1024u, p7.size.fixedBytes);
			Assert::AreEqual(0, int(p7.size.termCount));

			const ProductDecl& p8 = reg.Get(kP8CameraLabels);
			Assert::IsTrue(p8.backend == Backend::GpuCompute);
			Assert::IsTrue(p8.residency == Residency::Vram);
			Assert::AreEqual(0, int(p8.history));
			Assert::IsTrue(p8.tier == Tier::T0 && p8.variants == Variants::Identical);
			Assert::AreEqual(int(kP7CameraFit), int(p8.inputs[0].product));
			Assert::AreEqual(int(kP3AnalysisField), int(p8.inputs[1].product));
			Assert::AreEqual(content, p8.reads);
			Assert::AreEqual(1, int(p8.size.termCount));
			Assert::AreEqual(4, int(p8.size.terms[0].shift));
			Assert::AreEqual(16, int(p8.size.terms[0].bytesPerCell));
			Assert::AreEqual(256u, p8.size.fixedBytes);

			const ProductDecl& p9 = reg.Get(kP9CameraSmoothed);
			Assert::IsTrue(p9.backend == Backend::Cpu && p9.residency == Residency::SystemRam);
			Assert::IsTrue(p9.tier == Tier::T1 && p9.variants == Variants::NonIdentical);
			Assert::AreEqual(2, int(p9.inputCount));
			Assert::AreEqual(int(kP7CameraFit), int(p9.inputs[0].product));
			Assert::AreEqual(0, int(p9.inputs[0].lead));
			Assert::AreEqual(int(kP7CameraFit), int(p9.inputs[1].product));
			Assert::AreEqual(int(kMaxCameraLead), int(p9.inputs[1].lead));
			Assert::AreEqual(0, int(p9.inputs[1].lag));
			Assert::AreEqual(int(kP7CameraFit), int(p9.shortQueueSubstitute));
			Assert::AreEqual(content | Bit(kSettingsMotion), p9.reads);
			Assert::AreEqual(content, p9.spanMask);
			Assert::AreEqual(256u, p9.size.fixedBytes);
		}

		TEST_METHOD(CameraProductSizesAtThreeResolutions)
		{
			const Registry reg = SkeletonProducts();
			struct Case { uint32_t w, h; uint64_t labels; };
			// ceil(w/16) * ceil(h/16) * 8 + 256
			const Case cases[] = { { 1920, 1080, 120u * 68u * 16u + 256u },
				{ 3840, 2160, 240u * 135u * 16u + 256u },
				{ 7680, 4320, 480u * 270u * 16u + 256u } };
			for (const Case& c : cases) {
				Dims d;
				d.width = c.w;
				d.height = c.h;
				Assert::AreEqual(uint64_t(1024), BytesOf(reg.Get(kP7CameraFit).size, d));
				Assert::AreEqual(c.labels, BytesOf(reg.Get(kP8CameraLabels).size, d));
				Assert::AreEqual(uint64_t(256), BytesOf(reg.Get(kP9CameraSmoothed).size, d));
			}
			Assert::AreEqual(uint64_t(130816), uint64_t(120u * 68u * 16u + 256u));
			Assert::AreEqual(uint64_t(518656), uint64_t(240u * 135u * 16u + 256u));
			Assert::AreEqual(uint64_t(2073856), uint64_t(480u * 270u * 16u + 256u));
		}

		TEST_METHOD(ValidateRejectsBadLookAheadDeclarations)
		{
			const std::vector<ProductDecl> good = SkeletonProducts().All();
			{   // a lead input naming the product itself
				std::vector<ProductDecl> d = good;
				d[kP9CameraSmoothed].inputs[1].product = kP9CameraSmoothed;
				Assert::IsTrue(ValidateFails(d), L"self lead");
			}
			{   // an unknown lead product
				std::vector<ProductDecl> d = good;
				d[kP9CameraSmoothed].inputs[1].product = ProductId(0x7777);
				Assert::IsTrue(ValidateFails(d), L"unknown lead product");
			}
			{   // a lead without a short-queue substitute
				std::vector<ProductDecl> d = good;
				d[kP9CameraSmoothed].shortQueueSubstitute = kNoProduct;
				Assert::IsTrue(ValidateFails(d), L"no substitute");
			}
			{   // a substitute that itself has a lead input
				std::vector<ProductDecl> d = good;
				d[kP8CameraLabels].shortQueueSubstitute = kP9CameraSmoothed;
				Assert::IsTrue(ValidateFails(d), L"substitute with a lead");
			}
			{   // a lead input that also has a lag
				std::vector<ProductDecl> d = good;
				d[kP9CameraSmoothed].inputs[1].lag = 1;
				Assert::IsTrue(ValidateFails(d), L"lead with a lag");
			}
			{   // a substitute naming the product itself
				std::vector<ProductDecl> d = good;
				d[kP9CameraSmoothed].shortQueueSubstitute = kP9CameraSmoothed;
				Assert::IsTrue(ValidateFails(d), L"self substitute");
			}
			Assert::IsTrue(Registry(good).Validate());
		}

		TEST_METHOD(LeadEdgesAreNotLagZeroEdgesAndNeverChainOrLoop)
		{
			// P5 (monitor tiles) reads P7 at a LEAD: acyclic (P7 never reaches P5), so valid; the same edge at
			// lag 0 is also acyclic but is not a lead.
			std::vector<ProductDecl> d = SkeletonProducts().All();
			VpSkeleton::detail::AddLead(d[kP5MonitorTiles], kP7CameraFit, 1);
			d[kP5MonitorTiles].shortQueueSubstitute = kP4SourceStats;   // a substitute without leads
			const char* why = "";
			Assert::IsTrue(Registry(d).Validate(&why), L"an acyclic lead is valid");

			// A lead loop: P3 reads P7 ahead while P7 reads P3.
			std::vector<ProductDecl> loop = SkeletonProducts().All();
			VpSkeleton::detail::AddLead(loop[kP3AnalysisField], kP7CameraFit, 1);
			loop[kP3AnalysisField].shortQueueSubstitute = kP4SourceStats;
			Assert::IsTrue(ValidateFails(loop), L"a lead that closes a loop");

			// A lead chain: P5 reads P9, which itself looks ahead.
			std::vector<ProductDecl> chain = SkeletonProducts().All();
			VpSkeleton::detail::AddLead(chain[kP5MonitorTiles], kP9CameraSmoothed, 1);
			chain[kP5MonitorTiles].shortQueueSubstitute = kP4SourceStats;
			Assert::IsTrue(ValidateFails(chain), L"a lead chain");

			// The same edge at lag 0 closing a loop is a lag-0 cycle.
			std::vector<ProductDecl> cyc = SkeletonProducts().All();
			VpSkeleton::detail::AddInput(cyc[kP3AnalysisField], kP7CameraFit, 0);
			Assert::IsTrue(ValidateFails(cyc), L"the same edge at lag 0 is a cycle");

			// A substitute that reads the product it replaces.
			std::vector<ProductDecl> sub = SkeletonProducts().All();
			sub[kP9CameraSmoothed].shortQueueSubstitute = kP8CameraLabels;   // P8 reads P7 not P9: fine
			Assert::IsTrue(Registry(sub).Validate());
			VpSkeleton::detail::AddInput(sub[kP8CameraLabels], kP9CameraSmoothed, 0);
			Assert::IsTrue(ValidateFails(sub), L"substitute reads the original");
		}

		TEST_METHOD(SlotTableFailsClosedOnLookAheadInputs)
		{
			const Registry reg = SkeletonProducts();
			const Graph lead = BuildGraph(reg, Camera({ CameraLink::CameraConsumer::TemporalDetail }), Settings(3));
			Assert::IsTrue(lead.valid);
			VpSkeleton::SlotTable bad(lead);
			Assert::IsFalse(bad.Valid());
			Assert::IsTrue(bad.Error()[0] != '\0', L"a plain visible reason");
			Assert::AreEqual(size_t(0), bad.Count());
			// The causal form (no lead input) is fine.
			const Graph causal = BuildGraph(reg, Camera({ CameraLink::CameraConsumer::TemporalDetail }), Settings(1));
			VpSkeleton::SlotTable ok(causal);
			Assert::IsTrue(ok.Valid());
			Assert::AreEqual(causal.nodes.size(), ok.Count());
		}

		TEST_METHOD(GraphOffHasNoCameraNodesAndMatchesTheFirstProducts)
		{
			const Registry first = FirstProducts();
			const Registry skel = SkeletonProducts();
			for (int ofa = 0; ofa <= 1; ++ofa)
				for (uint8_t lead = 0; lead <= 2; ++lead)
					for (uint8_t r = 0; r <= 2; ++r)
						for (int c = 0; c < VectorDemand::kConsumerCount; ++c) {
							std::vector<ConsumerDecl> cs;
							cs.push_back(VectorDemand::DeclarationOf(VectorDemand::ConsumerAt(c)));
							const GraphSettings gs = Settings(lead, ofa != 0, r);
							const Graph a = BuildGraph(first, cs, gs);
							const Graph b = BuildGraph(skel, cs, gs);
							Assert::IsTrue(a.valid && b.valid);
							Assert::IsFalse(b.Has(kP7CameraFit) || b.Has(kP8CameraLabels) || b.Has(kP9CameraSmoothed));
							Assert::AreEqual(a.nodes.size(), b.nodes.size());
							for (size_t i = 0; i < a.nodes.size(); ++i) {
								Assert::AreEqual(int(a.nodes[i].id), int(b.nodes[i].id));
								Assert::AreEqual(int(a.nodes[i].ring), int(b.nodes[i].ring));
								Assert::AreEqual(int(a.nodes[i].livePool), int(b.nodes[i].livePool));
								Assert::AreEqual(a.nodes[i].bufferBytes, b.nodes[i].bufferBytes);
								Assert::AreEqual(a.nodes[i].ringBytes, b.nodes[i].ringBytes);
								Assert::AreEqual(a.nodes[i].liveBytes, b.nodes[i].liveBytes);
								Assert::AreEqual(int(a.nodes[i].history), int(b.nodes[i].history));
								Assert::AreEqual(0, int(b.nodes[i].lead));
								Assert::IsFalse(b.nodes[i].shortQueue);
							}
							Assert::AreEqual(a.framesInFlight, b.framesInFlight);
							Assert::AreEqual(a.totalRingBytes, b.totalRingBytes);
							Assert::AreEqual(a.totalLiveBytes, b.totalLiveBytes);
							Assert::AreEqual(size_t(0), b.ReducedNodes().size());
						}
		}

		TEST_METHOD(DiagnosticsClosureHasEachProductOnce)
		{
			const Registry reg = SkeletonProducts();
			const std::vector<ConsumerDecl> cs = Camera({ CameraLink::CameraConsumer::Diagnostics });
			const Graph g = BuildGraph(reg, cs, Settings(1, true));
			Assert::IsTrue(g.valid);
			const ProductId expected[] = { ProductId(kP1HalfLuma), ProductId(kP2VectorPair), ProductId(kP3AnalysisField),
				ProductId(kP4SourceStats), ProductId(kP7CameraFit), ProductId(kP8CameraLabels) };
			for (ProductId p : expected)
				Assert::AreEqual(1, CountOf(g, p));
			Assert::AreEqual(size_t(6), g.nodes.size());
			Assert::IsFalse(g.Has(kP9CameraSmoothed));
			// P7 before P8 (lag-0 edge), and both after their inputs.
			Assert::IsTrue(g.IndexOf(kP7CameraFit) < g.IndexOf(kP8CameraLabels));
			Assert::IsTrue(g.IndexOf(kP3AnalysisField) < g.IndexOf(kP7CameraFit));

			const Graph noEngine = BuildGraph(reg, cs, Settings(1, false));
			Assert::IsTrue(noEngine.valid);
			Assert::IsTrue(noEngine.Has(kP2mMatcherPair));
			Assert::IsFalse(noEngine.Has(kP2VectorPair));
			Assert::AreEqual(1, CountOf(noEngine, kP2mMatcherPair));
		}

		TEST_METHOD(TemporalDetailLeadFollowsTheLaneLeadMinusOne)
		{
			const Registry reg = SkeletonProducts();
			const std::vector<ConsumerDecl> td = Camera({ CameraLink::CameraConsumer::TemporalDetail });
			const std::vector<ConsumerDecl> diag = Camera({ CameraLink::CameraConsumer::Diagnostics });

			// Effective lead = min(declared 2, laneLead - 1): lane 2 -> 1 (partial), lane 3 -> 2 (full).
			for (uint8_t lane = 2; lane <= 3; ++lane) {
				const int expected = lane - 1;
				const Graph g = BuildGraph(reg, td, Settings(lane));
				const Graph base = BuildGraph(reg, diag, Settings(lane));
				Assert::IsTrue(g.valid && base.valid);
				const Node* p9 = g.Find(kP9CameraSmoothed);
				Assert::IsNotNull(p9);
				Assert::AreEqual(expected, int(p9->lead));
				Assert::AreEqual(int(kMaxCameraLead), int(p9->declaredLead));
				Assert::IsFalse(p9->shortQueue);
				Assert::AreEqual(expected, int(p9->inputs[1].lead));
				Assert::AreEqual(0, int(p9->inputs[0].lead));
				const Node* p7 = g.Find(kP7CameraFit);
				const Node* p7Base = base.Find(kP7CameraFit);
				Assert::IsNotNull(p7);
				Assert::IsNotNull(p7Base);
				Assert::AreEqual(expected, int(p7->leadRead));
				Assert::AreEqual(int(p7Base->ring) + expected, int(p7->ring));
				Assert::AreEqual(base.framesInFlight + size_t(expected), g.framesInFlight);
				Assert::AreEqual(size_t(0), g.ReducedNodes().size());
				// A partial lead is listed with both numbers.
				Assert::AreEqual(size_t(lane == 2 ? 1 : 0), g.PartialLeadNodes().size());
			}

			// A queue that cannot supply any lead (lane 0 or 1): P9 is replaced by the causal P7.
			for (uint8_t lane = 0; lane <= 1; ++lane) {
				const Graph causal = BuildGraph(reg, td, Settings(lane));
				const Graph causalBase = BuildGraph(reg, diag, Settings(lane));
				Assert::IsTrue(causal.valid);
				Assert::IsFalse(causal.Has(kP9CameraSmoothed));
				const Node* p7 = causal.Find(kP7CameraFit);
				Assert::IsNotNull(p7);
				Assert::IsTrue(p7->shortQueue);
				Assert::AreEqual(int(kP9CameraSmoothed), int(p7->reducedFrom));
				Assert::AreEqual(0, int(p7->leadRead));
				Assert::AreEqual(int(causalBase.Find(kP7CameraFit)->ring), int(p7->ring));
				Assert::AreEqual(causalBase.framesInFlight, causal.framesInFlight);
				const std::vector<const Node*> reduced = causal.ReducedNodes();
				Assert::AreEqual(size_t(1), reduced.size());
				Assert::AreEqual(int(kP7CameraFit), int(reduced[0]->id));
				// The record names what was reduced even though P7 was also asked for directly.
				Assert::AreEqual(size_t(1), causal.reductions.size());
				Assert::AreEqual(int(kP9CameraSmoothed), int(causal.reductions[0].asked));
				Assert::AreEqual(int(kP7CameraFit), int(causal.reductions[0].made));
				Assert::AreEqual(size_t(0), causal.PartialLeadNodes().size());
			}
		}

		TEST_METHOD(SmoothedOnlyConsumerKeepsTheDeclaredIdWhenSubstituted)
		{
			const Registry reg = SkeletonProducts();
			const std::vector<ConsumerDecl> roll = Camera({ CameraLink::CameraConsumer::RollOff });
			const Graph on = BuildGraph(reg, roll, Settings(3));
			Assert::IsTrue(on.valid && on.Has(kP9CameraSmoothed) && on.Has(kP7CameraFit));
			Assert::IsFalse(on.Has(kP8CameraLabels));
			const Graph off = BuildGraph(reg, roll, Settings(0));
			Assert::IsTrue(off.valid);
			Assert::IsFalse(off.Has(kP9CameraSmoothed));
			const Node* p7 = off.Find(kP7CameraFit);
			Assert::IsNotNull(p7);
			Assert::AreEqual(int(kP9CameraSmoothed), int(p7->declared));   // the asked id is kept
			Assert::IsTrue(p7->shortQueue);
			Assert::AreEqual(size_t(1), off.ReducedNodes().size());
		}

		TEST_METHOD(MeasurementOnlyKeepsP7WithHistoryOne)
		{
			const Registry reg = SkeletonProducts();
			const Graph g = BuildGraph(reg, Camera({ CameraLink::CameraConsumer::Measurement }), Settings(1));
			Assert::IsTrue(g.valid);
			const Node* p7 = g.Find(kP7CameraFit);
			Assert::IsNotNull(p7);
			Assert::AreEqual(1, int(p7->history));
			Assert::IsFalse(g.Has(kP8CameraLabels));
			Assert::IsFalse(g.Has(kP9CameraSmoothed));
			Assert::AreEqual(size_t(0), g.ReducedNodes().size());
		}

		TEST_METHOD(CameraConsumerDeclarationsAreDeterministicAndComplete)
		{
			using CameraLink::CameraConsumer;
			CameraLink::CameraDemandInput in;
			for (int i = 0; i < CameraLink::kCameraConsumerCount; ++i)
				in.active[i] = true;
			std::vector<ConsumerDecl> cs;
			CameraLink::DeclareCameraConsumers(in, cs);
			Assert::AreEqual(size_t(CameraLink::kCameraConsumerCount), cs.size());
			for (int i = 0; i < CameraLink::kCameraConsumerCount; ++i) {
				Assert::AreEqual(int(CameraLink::kCameraConsumerIdBase) + i, int(cs[size_t(i)].id));
				Assert::IsTrue(cs[size_t(i)].hasReducedMode);
			}
			const ConsumerDecl meas = CameraLink::DeclarationOf(CameraConsumer::Measurement);
			Assert::AreEqual(1, int(meas.count));
			Assert::AreEqual(int(kP7CameraFit), int(meas.consumes[0].product));
			Assert::AreEqual(1, int(meas.consumes[0].lag));
			Assert::IsTrue(CameraLink::DeclarationOf(CameraConsumer::DitherVectors).holdsSpan);
			Assert::IsTrue(CameraLink::DeclarationOf(CameraConsumer::DejudderWarp).holdsSpan);
			Assert::IsFalse(CameraLink::DeclarationOf(CameraConsumer::Debander).holdsSpan);
			CameraModel::Consumer m = CameraModel::Consumer::Debander;
			Assert::IsTrue(CameraLink::ToModelConsumer(CameraConsumer::OcclusionFiller, &m));
			Assert::IsTrue(m == CameraModel::Consumer::OcclusionMask);
			Assert::IsFalse(CameraLink::ToModelConsumer(CameraConsumer::Diagnostics, &m));
			// The whole set builds a valid graph on every lane lead.
			for (uint8_t lane = 0; lane <= 3; ++lane) {
				const Graph g = BuildGraph(SkeletonProducts(), cs, Settings(lane));
				Assert::IsTrue(g.valid);
				Assert::AreEqual(1, CountOf(g, kP7CameraFit));
				Assert::AreEqual(lane <= 1 ? 0 : 1, CountOf(g, kP9CameraSmoothed));
			}
		}

		TEST_METHOD(ReaderPoliciesOnARealIndicator)
		{
			using namespace VpIndicator;
			Indicator ind(NextEra(), 64);
			Counts counts;
			CameraLink::CameraReaderHooks hooks;
			hooks.reset = [&counts]() { ++counts.resets; };
			hooks.relatchTiming = [&counts]() { ++counts.relatches; };
			Assert::AreEqual(0, CameraLink::RegisterCameraReader(ind, "camera", hooks));

			ind.SealAndObserve(10);   // the first observe reports everything
			counts = Counts();

			struct Step { Counter counter; int resets; int relatches; };
			const Step steps[] = {
				{ kFull, 1, 0 },             // full: reset ONCE although it also moves continuity/rebase
				{ kContinuity, 1, 0 },
				{ kTiming, 0, 1 },
				{ kRebaseH, 0, 0 },
				{ kRebaseV, 0, 0 },
				{ kSettingsMotion, 1, 0 },
				{ kGraph, 1, 0 },
				{ kSettingsAnalysis, 0, 0 },   // not read at all
				{ kSettingsOutput, 0, 0 },
			};
			uint64_t seq = 11;
			for (const Step& s : steps) {
				counts = Counts();
				ind.Advance(s.counter, Cause::GapOther, seq);
				ind.SealAndObserve(seq);
				Assert::AreEqual(s.resets, counts.resets);
				Assert::AreEqual(s.relatches, counts.relatches);
				++seq;
			}
		}

		TEST_METHOD(PairSpansSlipFollowsContinuityAndFull)
		{
			using namespace VpIndicator;
			Indicator ind(NextEra(), 64);
			ind.SealAndObserve(10);
			ind.Advance(kContinuity, Cause::MissedFrames, 21);
			ind.Seal(30);
			Assert::IsTrue(CameraLink::PairSpansSlip(ind, 20, 22));
			Assert::IsTrue(CameraLink::PairSpansSlip(ind, 10, 29));
			Assert::IsFalse(CameraLink::PairSpansSlip(ind, 21, 29));
			Assert::IsFalse(CameraLink::PairSpansSlip(ind, 12, 20));
			ind.Advance(kTiming, Cause::RateStep, 35);
			ind.Seal(40);
			Assert::IsFalse(CameraLink::PairSpansSlip(ind, 31, 38), L"timing is not a slip");
			ind.Advance(kFull, Cause::LiveQueueReset, 45);
			ind.Seal(50);
			Assert::IsTrue(CameraLink::PairSpansSlip(ind, 44, 46));
		}

		TEST_METHOD(BindersResetTheirComponents)
		{
			using namespace VpIndicator;
			Indicator ind(NextEra(), 64);
			CameraModel::TemporalFilter filter;
			CameraModel::SimilarityRates rates;
			rates.tx = 3.0;
			filter.Update(1.0 / 24.0, rates);
			Assert::IsTrue(filter.Valid());
			int calls = 0;
			CameraLink::CameraReaderHooks counter;
			counter.reset = [&calls]() { ++calls; };
			Assert::IsTrue(CameraLink::RegisterCameraReader(
				ind, "camera", CameraLink::Merge(CameraLink::BindTemporalFilter(filter), counter)) >= 0);
			ind.SealAndObserve(5);   // first observe: reset
			Assert::IsFalse(filter.Valid());
			Assert::AreEqual(1, calls);
			filter.Update(1.0 / 24.0, rates);
			ind.Advance(kTiming, Cause::RateStep, 6);
			ind.SealAndObserve(6);
			Assert::IsTrue(filter.Valid(), L"timing relatches, it does not reset");
			ind.Advance(kContinuity, Cause::MissedFrames, 7);
			ind.SealAndObserve(7);
			Assert::IsFalse(filter.Valid());
			Assert::AreEqual(2, calls);
		}

		TEST_METHOD(SealPublishesToTheStoreAndNullIsSkipped)
		{
			using namespace FrameAnalysis;
			Store store;
			store.Begin(Id(1), kCamera);
			store.Begin(Id(2), kCamera);
			CameraLink::CameraSealToStore adapter(store);
			auto cam = Cam(1);
			Assert::IsTrue(adapter.OnSealed(1, cam));
			Assert::AreEqual(uint64_t(1), adapter.Published());
			Record out;
			Assert::IsTrue(store.Read(1, Use::SingleFrame, kCamera, out) == ReadStatus::Ok);
			Assert::IsTrue(out.camera.get() == cam.get(), L"the sealed record itself is served");

			Assert::IsFalse(adapter.OnSealed(2, nullptr));
			Assert::AreEqual(uint64_t(1), adapter.Skipped());
			Assert::AreEqual(uint64_t(1), adapter.Published());
			Assert::IsFalse(store.Read(2, Use::SingleFrame, kCamera, out) == ReadStatus::Ok, L"a rejected P7 is never served");

			Assert::IsFalse(adapter.OnSealed(99, Cam(99)), L"no record for that frame");
			Assert::AreEqual(uint64_t(1), adapter.Refused());
			Assert::AreEqual(uint64_t(1), adapter.Published());
		}

		TEST_METHOD(PublishCameraHasOneProductionCaller)
		{
			namespace fs = std::filesystem;
			const std::string root = SettingsTestSupport::RepoRoot() + "src";
			Assert::IsTrue(fs::exists(root), L"src folder found from __FILE__");
			size_t scanned = 0;
			std::string offenders;
			const std::regex call("PublishCamera\\s*\\(|::PublishCamera\\b");
			const std::string adapter = Lower("src/VP-V/modules/motion/estimation/global_model/CameraStoreAdapter.h");
			for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
				if (!e.is_regular_file())
					continue;
				const std::string path = e.path().string();
				const std::string ext = Lower(e.path().extension().string());
				if (ext != ".h" && ext != ".cpp")
					continue;
				const std::string low = Lower(path);
				const std::string name = Lower(e.path().filename().string());
				if (name == "frameanalysisstore.h")
					continue;
				if (low.size() >= adapter.size() && low.compare(low.size() - adapter.size(), adapter.size(), adapter) == 0)
					continue;
				if (low.find("\\tests\\") != std::string::npos || low.find("\\videoprocessor-test\\") != std::string::npos)
					continue;
				++scanned;
				std::istringstream lines(SettingsTestSupport::ReadFile(path));
				std::string line;
				int lineNo = 0;
				while (std::getline(lines, line)) {
					++lineNo;
					const size_t first = line.find_first_not_of(" \t");
					if (first == std::string::npos)
						continue;
					if (line.compare(first, 2, "//") == 0 || line[first] == '*' || line.compare(first, 2, "/*") == 0)
						continue;
					if (std::regex_search(line, call))
						offenders += path + ":" + std::to_string(lineNo) + "\n";
				}
			}
			Assert::IsTrue(scanned > 100, L"the scan walked the source tree");
			Assert::AreEqual(std::string(), offenders);
		}
	};
}
