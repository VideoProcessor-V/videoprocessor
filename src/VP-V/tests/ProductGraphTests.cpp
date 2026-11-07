// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S0 (src/VP-V/core/Products.h, ProductGraph.h): registry validation, closures over
// inputs (each product once), substitution without an optical-flow engine, slot counts, rebuild
// triggers, the graph counter, Indicator ring sizing (N6) and determinism. CPU only.

#include <VP-V/core/ProductGraph.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ProductGraphTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Bit;
	using VpIndicator::Indicator;
	using VpIndicator::Stamp;
	using VpResync::Cause;

	ConsumerDecl Consumer(uint16_t id, ProductId p, uint8_t lag = 0, bool span = false)
	{
		ConsumerDecl c;
		c.id = id;
		c.name = "test consumer";
		AddConsume(c, p, lag);
		c.holdsSpan = span;
		return c;
	}

	GraphSettings Settings(uint8_t r = 0, uint8_t l = 1)
	{
		GraphSettings gs;
		gs.dims.width = 1920;
		gs.dims.height = 1080;
		gs.renderAhead = r;
		gs.laneLead = l;
		gs.queueDepth = 6;
		gs.lookAheadWalk = 2;
		return gs;
	}

	std::vector<ProductId> Order(const Graph& g)
	{
		std::vector<ProductId> o;
		for (const Node& n : g.nodes)
			o.push_back(n.id);
		return o;
	}

	TEST_CLASS(ProductGraphTests)
	{
	public:
		TEST_METHOD(FirstProductsRegistryIsValid)
		{
			const Registry reg = FirstProducts();
			const char* why = nullptr;
			Assert::IsTrue(reg.Validate(&why), L"first products validate");
			Assert::AreEqual(size_t(kFirstProductCount), reg.Size());
			for (ProductId p = 0; p < kFirstProductCount; ++p)
				Assert::IsTrue(reg.Has(p));
		}

		TEST_METHOD(RegistryRejectsUnknownInputsCyclesAndBadSubstitutes)
		{
			std::vector<ProductDecl> d(2);
			d[0].id = 0;
			d[1].id = 1;
			detail::AddInput(d[0], 5, 0);
			const char* why = nullptr;
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::AreEqual(std::string("unknown input product"), std::string(why));

			d[0].inputCount = 0;
			detail::AddInput(d[0], 1, 0);
			detail::AddInput(d[1], 0, 0);
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::AreEqual(std::string("lag-0 cycle between products"), std::string(why));

			// A lag-1 back edge (a temporal recursion) is fine.
			d[1].inputs[0].lag = 1;
			Assert::IsTrue(Registry(d).Validate(&why));

			d[0].noOfaSubstitute = 1;   // product 0 is not an optical-flow product
			Assert::IsFalse(Registry(d).Validate(&why));
			Assert::AreEqual(std::string("bad optical-flow substitute"), std::string(why));
		}

		TEST_METHOD(ClosurePullsInputsOnceInTopologicalOrder)
		{
			const Registry reg = FirstProducts();
			std::vector<ConsumerDecl> cs{ Consumer(1, kP3AnalysisField), Consumer(2, kP2VectorPair),
				Consumer(3, kP3AnalysisField) };
			const Graph g = BuildGraph(reg, cs, Settings());
			Assert::IsTrue(g.valid);
			const std::vector<ProductId> expect{ kP1HalfLuma, kP2VectorPair, kP3AnalysisField };
			Assert::IsTrue(expect == Order(g), L"P1, P2, P3 once each, inputs first");
			Assert::IsFalse(g.Has(kP4SourceStats));
			Assert::IsFalse(g.Has(kP2mMatcherPair));
			// Every node honours the graph counter.
			for (const Node& n : g.nodes)
				Assert::IsTrue((n.reads & Bit(VpIndicator::kGraph)) != 0);
		}

		TEST_METHOD(NoConsumerMeansAnEmptyGraph)
		{
			const Graph g = BuildGraph(FirstProducts(), {}, Settings());
			Assert::IsTrue(g.valid);
			Assert::AreEqual(size_t(0), g.nodes.size());
			Assert::AreEqual(uint64_t(0), g.totalRingBytes);
		}

		TEST_METHOD(EntranceAheadPullsTheRebaseDecisionAndTheFieldChain)
		{
			const Graph g = BuildGraph(FirstProducts(), { Consumer(1, kP6EntranceAhead) }, Settings());
			Assert::IsTrue(g.valid);
			const std::vector<ProductId> expect{ kP0RebaseDecision, kP1HalfLuma, kP2VectorPair, kP3AnalysisField,
				kP6EntranceAhead };
			Assert::IsTrue(expect == Order(g));
			Assert::IsTrue(g.hasRebasedPlane);
		}

		TEST_METHOD(WithoutAnEngineTheMatcherPairReplacesTheVectorPair)
		{
			GraphSettings gs = Settings();
			gs.ofaPresent = false;
			const Graph g = BuildGraph(FirstProducts(), { Consumer(1, kP3AnalysisField) }, gs);
			Assert::IsTrue(g.valid);
			Assert::IsFalse(g.Has(kP2VectorPair));
			Assert::IsTrue(g.Has(kP2mMatcherPair));
			const Node* p3 = g.Find(kP3AnalysisField);
			Assert::IsNotNull(p3);
			bool readsMatcher = false;
			for (size_t k = 0; k < p3->inputCount; ++k)
				readsMatcher = readsMatcher || p3->inputs[k].product == kP2mMatcherPair;
			Assert::IsTrue(readsMatcher, L"P3's input redirects to the matcher pair");
			Assert::AreEqual(ProductId(kP2VectorPair), g.Find(kP2mMatcherPair)->declared);
		}

		TEST_METHOD(RingSizesFollowTheDesignFormula)
		{
			const Registry reg = FirstProducts();
			// Design example: P2 (history 1, R 0-1, L 1) -> 4-5.
			for (uint8_t r = 0; r <= 1; ++r) {
				const Graph g = BuildGraph(reg, { Consumer(1, kP2VectorPair) }, Settings(r, 1));
				const Node* p2 = g.Find(kP2VectorPair);
				Assert::AreEqual(int(4 + r), int(p2->ring));
				Assert::AreEqual(int(1 + 1 + r), int(p2->livePool), L"history + 1 + R");
				Assert::AreEqual(p2->bufferBytes * p2->ring, p2->ringBytes);
			}
			// A pair consumer holding the next frame's product through the span adds one.
			const Graph span = BuildGraph(reg, { Consumer(1, kP2VectorPair, 0, true) }, Settings(0, 1));
			Assert::AreEqual(5, int(span.Find(kP2VectorPair)->ring));
			Assert::AreEqual(4, int(span.Find(kP1HalfLuma)->ring), L"the span slot is only for the held product");
			// One spare live buffer per input reference lane jobs hold; a render-only product has no ring memory.
			const Graph field = BuildGraph(reg, { Consumer(1, kP3AnalysisField) }, Settings(0, 1));
			Assert::AreEqual(1 + 1 + 0 + 4, int(field.Find(kP1HalfLuma)->livePool), L"P2 and P3 each read P1 twice");
			Assert::AreEqual(1 + 1 + 0 + 1, int(field.Find(kP2VectorPair)->livePool));
			Assert::AreEqual(1 + 1 + 0, int(field.Find(kP3AnalysisField)->livePool));
			// Render-only: no lane lead, no rejected spare. P0 history 1 + 1 + R.
			const Graph p0 = BuildGraph(reg, { Consumer(1, kP0RebaseDecision) }, Settings(2, 1));
			Assert::AreEqual(int(1 + 1 + 2), int(p0.Find(kP0RebaseDecision)->ring));
			Assert::AreEqual(uint64_t(0), p0.Find(kP0RebaseDecision)->ringBytes);
			// A consumer reading at lag 3 raises the history.
			const Graph lag = BuildGraph(reg, { Consumer(1, kP4SourceStats, 3) }, Settings(0, 1));
			Assert::AreEqual(3, int(lag.Find(kP4SourceStats)->history));
			Assert::AreEqual(3 + 1 + 0 + 1 + 1, int(lag.Find(kP4SourceStats)->ring));
		}

		TEST_METHOD(BufferBytesScaleWithDims)
		{
			GraphSettings gs = Settings();
			gs.dims.width = 3840;
			gs.dims.height = 2160;
			const Graph g = BuildGraph(FirstProducts(), { Consumer(1, kP6EntranceAhead) }, gs);
			Assert::AreEqual(uint64_t(3840) * 2160 * 8, g.Find(kP6EntranceAhead)->bufferBytes);
			// Half + quarter luma, 16-bit.
			Assert::AreEqual(uint64_t(1920) * 1080 * 2 + uint64_t(960) * 540 * 2, g.Find(kP1HalfLuma)->bufferBytes);
			// Odd sizes round up per cell.
			Dims odd{ 1921, 1081 };
			Assert::AreEqual(uint64_t(961) * 541, CellsAt(odd, 1));
		}

		TEST_METHOD(FramesInFlightSizeTheIndicatorRing)
		{
			GraphSettings gs = Settings(2, 1);
			gs.queueDepth = 40;
			gs.lookAheadWalk = 8;
			const Graph g = BuildGraph(FirstProducts(), { Consumer(1, kP3AnalysisField) }, gs);
			// queue 40 + walk 8 + max history 1 + R 2 + L 1 + 1.
			Assert::AreEqual(size_t(53), g.framesInFlight);
			Assert::AreEqual(Indicator::RequiredCapacity(53), g.IndicatorCapacity());
			Assert::IsTrue(g.IndicatorCapacity() >= 53 * Indicator::kMaxEventsPerFrame);
		}

		TEST_METHOD(UnknownConsumedProductMakesAnInvalidGraph)
		{
			const Graph g = BuildGraph(FirstProducts(), { Consumer(1, 99) }, Settings());
			Assert::IsFalse(g.valid);
			Assert::AreEqual(std::string("a consumer asks for an unknown product"), std::string(g.error));
		}

		// A CPU store of history `h` read at lag 0 by a CPU reader: the smallest shape that can overflow the slot counts.
		static Registry DeepHistoryRegistry(uint8_t h)
		{
			std::vector<ProductDecl> d;
			ProductDecl deep = detail::Make(0, "deep", "deep history", Backend::Cpu, h, Tier::T0, Bit(VpIndicator::kFull), 0);
			deep.size.fixedBytes = 64;
			d.push_back(deep);
			ProductDecl reader = detail::Make(1, "reader", "reader", Backend::Cpu, 0, Tier::T0, Bit(VpIndicator::kFull), 0);
			detail::AddInput(reader, 0, 0);
			reader.size.fixedBytes = 64;
			d.push_back(reader);
			return Registry(d);
		}

		TEST_METHOD(RingSlotOverflowIsRejectedVisiblyNeverClamped)
		{
			// ring = 250 + 1 + R 5 + L 1 + q 1 = 258 > 255: an invalid graph with a plain reason, not a ring smaller than history + 1.
			const Graph g = BuildGraph(DeepHistoryRegistry(250), { Consumer(1, 1) }, Settings(5, 1));
			Assert::IsFalse(g.valid);
			Assert::IsTrue(std::string(g.error).find("ring slots") != std::string::npos);
			Assert::IsTrue(g.nodes.empty() && g.errorProduct == 0);
			// The same product with room is built, and its ring is exactly the formula (no clamp involved).
			const Graph ok = BuildGraph(DeepHistoryRegistry(250), { Consumer(1, 1) }, Settings(2, 1));
			Assert::IsTrue(ok.valid);
			Assert::AreEqual(250 + 1 + 2 + 1 + 1, int(ok.Find(0)->ring));
			Assert::IsTrue(ok.errorProduct == kNoProduct);
		}

		TEST_METHOD(LivePoolOverflowIsRejectedVisiblyNeverClamped)
		{
			// laneLead 0: ring = 250 + 1 + 0 + 0 + q 1 = 252 fits; live = 250 + 1 + R 0 + one reader x 8 jobs = 259 does not.
			GraphSettings gs = Settings(0, 0);
			gs.laneJobsInFlight = 8;
			const Graph g = BuildGraph(DeepHistoryRegistry(250), { Consumer(1, 1) }, gs);
			Assert::IsFalse(g.valid);
			Assert::IsTrue(std::string(g.error).find("live-pool buffers") != std::string::npos);
			Assert::IsTrue(g.nodes.empty() && g.errorProduct == 0);
			gs.laneJobsInFlight = 1;
			const Graph ok = BuildGraph(DeepHistoryRegistry(250), { Consumer(1, 1) }, gs);
			Assert::IsTrue(ok.valid);
			Assert::AreEqual(250 + 1 + 1, int(ok.Find(0)->livePool));
		}

		TEST_METHOD(RebuildTriggersEachReportTheirReason)
		{
			Indicator ind(7, 64);
			const std::vector<ConsumerDecl> cs{ Consumer(1, kP3AnalysisField) };
			const GraphSettings gs = Settings();
			const GraphKey k0 = KeyOf(ind.StampOf(0), gs, cs);
			Assert::AreEqual(uint32_t(kRebuildFirst), RebuildReasons(GraphKey(), k0, false));
			Assert::AreEqual(uint32_t(0), RebuildReasons(k0, k0, true));

			auto after = [&](VpIndicator::Counter c) {
				Indicator i2(7, 64);
				i2.Advance(c, Cause::SettingsApply, 0);
				return KeyOf(i2.StampOf(0), gs, cs);
			};
			Assert::AreEqual(uint32_t(kRebuildSettings), RebuildReasons(k0, after(VpIndicator::kSettingsMotion), false));
			Assert::AreEqual(uint32_t(kRebuildSettings), RebuildReasons(k0, after(VpIndicator::kSettingsOutput), false));
			// FULL moves both rebase counters too, but reports FULL only.
			Assert::AreEqual(uint32_t(kRebuildFull), RebuildReasons(k0, after(VpIndicator::kFull), true));
			// An axis change rebuilds only a graph with re-based-plane products.
			Assert::AreEqual(uint32_t(kRebuildAxis), RebuildReasons(k0, after(VpIndicator::kRebaseH), true));
			Assert::AreEqual(uint32_t(0), RebuildReasons(k0, after(VpIndicator::kRebaseV), false));
			// Continuity, timing and the graph counter itself never rebuild.
			Assert::AreEqual(uint32_t(0), RebuildReasons(k0, after(VpIndicator::kContinuity), true));
			Assert::AreEqual(uint32_t(0), RebuildReasons(k0, after(VpIndicator::kTiming), true));
			Assert::AreEqual(uint32_t(0), RebuildReasons(k0, after(VpIndicator::kGraph), true));

			GraphSettings q = gs;
			q.renderAhead = 2;
			Assert::AreEqual(uint32_t(kRebuildQueue), RebuildReasons(k0, KeyOf(ind.StampOf(0), q, cs), false));
			q = gs;
			q.queueMode = QueueMode::Competitive;
			Assert::AreEqual(uint32_t(kRebuildQueue), RebuildReasons(k0, KeyOf(ind.StampOf(0), q, cs), false));
			q = gs;
			q.cardProfileKey = 42;
			Assert::AreEqual(uint32_t(kRebuildCard), RebuildReasons(k0, KeyOf(ind.StampOf(0), q, cs), false));
			q = gs;
			q.dims.width = 3840;
			Assert::AreEqual(uint32_t(kRebuildDims), RebuildReasons(k0, KeyOf(ind.StampOf(0), q, cs), false));
			const std::vector<ConsumerDecl> more{ Consumer(1, kP3AnalysisField), Consumer(2, kP4SourceStats) };
			Assert::AreEqual(uint32_t(kRebuildConsumers), RebuildReasons(k0, KeyOf(ind.StampOf(0), gs, more), false));
			Indicator other(8, 64);
			Assert::AreEqual(uint32_t(kRebuildEra), RebuildReasons(k0, KeyOf(other.StampOf(0), gs, cs), false));
		}

		TEST_METHOD(GraphOwnerAdvancesTheGraphCounterOncePerRebuild)
		{
			const Registry reg = FirstProducts();
			Indicator ind(3, 64);
			GraphOwner owner;
			const std::vector<ConsumerDecl> cs{ Consumer(1, kP6EntranceAhead) };
			const GraphSettings gs = Settings();
			Assert::AreEqual(uint32_t(kRebuildFirst), owner.Update(reg, cs, gs, ind.StampOf(0), &ind, 0));
			Assert::AreEqual(1u, ind.Total(VpIndicator::kGraph));
			// The graph counter moved; that alone is not a trigger.
			Assert::AreEqual(uint32_t(0), owner.Update(reg, cs, gs, ind.StampOf(1), &ind, 1));
			Assert::AreEqual(1u, ind.Total(VpIndicator::kGraph));
			// A re-base axis change from frame 5 (P6 is on the re-based plane).
			ind.Advance(VpIndicator::kRebaseV, Cause::RebaseDecision, 5);
			Assert::AreEqual(uint32_t(0), owner.Update(reg, cs, gs, ind.StampOf(4), &ind, 4), L"not yet at frame 4");
			Assert::AreEqual(uint32_t(kRebuildAxis), owner.Update(reg, cs, gs, ind.StampOf(5), &ind, 5));
			Assert::AreEqual(2u, ind.Total(VpIndicator::kGraph));
			Assert::AreEqual(2u, owner.Rebuilds());
			// The graph event applies from frame 5 on.
			Assert::AreEqual(ind.StampOf(4).v[VpIndicator::kGraph] + 1u, ind.StampOf(5).v[VpIndicator::kGraph]);
		}

		TEST_METHOD(SameInputsGiveTheSameGraph)
		{
			const Registry reg = FirstProducts();
			const std::vector<ConsumerDecl> cs{ Consumer(4, kP6EntranceAhead), Consumer(1, kP4SourceStats),
				Consumer(2, kP5MonitorTiles), Consumer(3, kP2VectorPair, 0, true) };
			const Graph a = BuildGraph(reg, cs, Settings(1, 1));
			const Graph b = BuildGraph(reg, cs, Settings(1, 1));
			Assert::IsTrue(Order(a) == Order(b));
			Assert::AreEqual(a.nodes.size(), b.nodes.size());
			for (size_t i = 0; i < a.nodes.size(); ++i) {
				Assert::AreEqual(int(a.nodes[i].ring), int(b.nodes[i].ring));
				Assert::AreEqual(a.nodes[i].bufferBytes, b.nodes[i].bufferBytes);
			}
			// Ancestors always before readers.
			for (size_t i = 0; i < a.nodes.size(); ++i)
				for (size_t k = 0; k < a.nodes[i].inputCount; ++k)
					if (a.nodes[i].inputs[k].lag == 0)
						Assert::IsTrue(a.IndexOf(a.nodes[i].inputs[k].product) < int(i));
			// Smallest ready id first: P0 and P1 lead.
			Assert::AreEqual(ProductId(kP0RebaseDecision), a.nodes[0].id);
			Assert::AreEqual(ProductId(kP1HalfLuma), a.nodes[1].id);
			Assert::AreEqual(ConsumerFingerprint(cs), ConsumerFingerprint(cs));
		}
	};
}
