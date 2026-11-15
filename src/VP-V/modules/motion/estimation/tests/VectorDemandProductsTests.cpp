// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S0: vector demand as registry consumer declarations
// (src/VP-V/modules/motion/estimation/VectorDemandProducts.h). Exhaustive over every VectorDemand
// input: the graph holds the vector pair exactly when the decision runs the producer and the
// analysis field exactly when a served consumer reads it (batch B, WAIVER-B1-ENGINE-07: the
// render-time estimate never runs without it, and is that field without optical-flow pairs), so
// VectorDemand's behaviour is unchanged. CPU only.

#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/motion/estimation/VectorDemandProducts.h>

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VectorDemandProductsTestsNs
{
	using namespace VectorDemand;
	constexpr int kOriginalConsumerCount = 11;   // the first consumers; the camera three (bits 11..13) are swept as a group, CameraFit (bit 14) on its own
	using VpSkeleton::BuildGraph;
	using VpSkeleton::ConsumerDecl;
	using VpSkeleton::Graph;
	using VpSkeleton::GraphSettings;
	using VpSkeleton::Registry;

	TEST_CLASS(VectorDemandProductsTests)
	{
	public:
		TEST_METHOD(GraphMatchesTheDecisionForEveryInput)
		{
			const Registry reg = VpSkeleton::FirstProducts();
			GraphSettings gs;
			gs.dims.width = 1920;
			gs.dims.height = 1080;
			GraphSettings noEngine = gs;
			noEngine.ofaPresent = false;
			std::vector<ConsumerDecl> cs;
			size_t checked = 0;
			unsigned readsField = 0;   // the consumers that read the analysis field (P3): source field and output (DeclarationOf)
			for (int i = 0; i < kConsumerCount; ++i)
				if (DomainOf(ConsumerAt(i)) == Domain::SourceField || DomainOf(ConsumerAt(i)) == Domain::Output)
					readsField |= 1u << i;
			// Batch B fold (WAIVER-B1-ENGINE-06): the third dimension was the retired analysis-under-smooth-motion flag
			// (DecideWith(in, under)); it now sweeps Input::pairsExpected, so every input of the one decision is covered and the
			// count is unchanged.
			for (int flags = 0; flags < 32; ++flags)
				for (int ahead = 0; ahead <= 2; ++ahead)
					for (int pairs = 0; pairs <= 1; ++pairs)
						for (unsigned wants = 0; wants < (1u << kConsumerCount); ++wants) {
							// The three camera consumers (bits 11..13) are covered as a group (none / all) to keep the sweep size; the camera fit (bit 14,
							// Consumer::CameraFit: a source-field consumer) is swept on its own, so all four combinations of {group, fit} are checked.
							const unsigned extra = wants >> kOriginalConsumerCount;
							const unsigned group = extra & 7u;
							if (group != 0 && group != 7u)
								continue;
							Input in;
							in.enabled = (flags & 1) != 0;
							in.slotsRun = (flags & 2) != 0;
							in.lowLatency = (flags & 4) != 0;
							in.smoothMotion = (flags & 8) != 0;
							in.interlaced = (flags & 16) != 0;
							in.queueAheadCapacity = ahead;
							in.pairsExpected = pairs != 0;
							for (int i = 0; i < kConsumerCount; ++i)
								in.wants[i] = (wants & (1u << i)) != 0;
							const Decision d = DecideForHost(in);
							cs.clear();
							DeclareConsumers(d, cs);
							const Graph g = BuildGraph(reg, cs, gs);
							Assert::IsTrue(g.valid);
							Assert::AreEqual(d.producer, g.Has(VpSkeleton::kP2VectorPair));
							// WAIVER-B1-ENGINE-07 (pre-existing since batch B, red at f45f0f509): was AreEqual(d.estimate, Has(P3)). Batch B
							// serves the source-field / output consumers from the producer's pairs while pairs are expected (no render-time
							// matcher estimate: VectorDemandTests EachNonSynthesisConsumerAloneIsServedForward), yet each of them reads the
							// analysis field (DeclarationOf), so that equivalence held only without pairs. The graph holds P3 exactly when a
							// served consumer reads it; the estimate never runs without it; without pairs the two are the same, as before.
							const bool fieldRead = (d.served & readsField) != 0;
							Assert::AreEqual(fieldRead, g.Has(VpSkeleton::kP3AnalysisField));
							Assert::IsTrue(!d.estimate || fieldRead, L"the render-time estimate makes a field a served consumer reads");
							if (!in.pairsExpected)
								Assert::AreEqual(d.estimate, g.Has(VpSkeleton::kP3AnalysisField), L"no pairs: the matcher fallback makes every field");
							Assert::IsFalse(g.Has(VpSkeleton::kP4SourceStats));
							if (d.producer) {
								const Graph m = BuildGraph(reg, cs, noEngine);
								Assert::IsTrue(m.Has(VpSkeleton::kP2mMatcherPair));
								Assert::IsFalse(m.Has(VpSkeleton::kP2VectorPair));
							}
							// Batch B fold (WAIVER-B1-ENGINE-06): was LaneLeadFor(d, 1) == (Backward ? 0 : 1); LaneLeadFor is retired with
							// the backward pairs. Depth 0 in low latency (competitive) is motion-free now: nothing declared, no pair.
							if (in.lowLatency && ahead == 0)
								Assert::IsTrue(cs.empty() && !g.Has(VpSkeleton::kP2VectorPair), L"competitive: motion-free, nothing declared");
							++checked;
						}
			// 32 flag sets x 3 queue depths x 2 pairs-expected states x 2^11 original wants x 2 camera-group states (none / all) x 2 camera-fit states.
			// Count change coordinator-approved 2026-11-08 (an extension of the sweep, no assertion weakened).
			Assert::AreEqual((size_t(32 * 3 * 2) << kOriginalConsumerCount) * 2 * 2, checked);
		}

		TEST_METHOD(DeclarationsFollowTheDomains)
		{
			const ConsumerDecl stats = DeclarationOf(Consumer::SourceMonitorPan);
			Assert::AreEqual(1, int(stats.count));
			Assert::AreEqual(VpSkeleton::ProductId(VpSkeleton::kP2VectorPair), stats.consumes[0].product);
			const ConsumerDecl field = DeclarationOf(Consumer::GrainMap);
			Assert::AreEqual(2, int(field.count));
			Assert::AreEqual(VpSkeleton::ProductId(VpSkeleton::kP3AnalysisField), field.consumes[1].product);
			const ConsumerDecl dither = DeclarationOf(Consumer::SigmaDelta);
			Assert::IsTrue(dither.holdsSpan);
			Assert::AreEqual(int(kConsumerIdBase + int(Consumer::SigmaDelta)), int(dither.id));
		}

		TEST_METHOD(OnlyServedConsumersAreDeclaredInConsumerOrder)
		{
			Input in;
			in.enabled = true;
			in.slotsRun = true;
			// Batch B fold (WAIVER-B1-ENGINE-06): was smoothMotion = true ("the output consumer is unserved under smooth motion", the
			// retired analysis-under-smooth-motion semantics; smooth motion now leaves every consumer unserved). The one unserved
			// consumer is now the statistics consumer without optical-flow pairs; one unserved, one synthesis, two served, as before.
			in.pairsExpected = false;   // the statistics consumer is unserved without optical-flow pairs
			in.wants[int(Consumer::SigmaDelta)] = true;
			in.wants[int(Consumer::MotionIntent)] = true;
			in.wants[int(Consumer::GrainMap)] = true;
			in.wants[int(Consumer::DejudderExtras)] = true;   // synthesis: never declared
			// WAIVER-C1-09, amended by WAIVER-B1-ENGINE-01 (the starter precondition is gone: the ON engine serves every reuse-only
			// want) and WAIVER-B1-ENGINE-06 (the unserved consumer): the count (2) and the consumer order are checked as before.
			const Decision d = DecideForHost(in);
			Assert::AreEqual(1u << int(Consumer::MotionIntent), d.unserved, L"the statistics consumer is unserved (no pairs)");
			std::vector<ConsumerDecl> cs;
			DeclareConsumers(d, cs);
			Assert::AreEqual(size_t(2), cs.size());
			Assert::AreEqual(int(kConsumerIdBase + int(Consumer::SigmaDelta)), int(cs[0].id));
			Assert::AreEqual(int(kConsumerIdBase + int(Consumer::GrainMap)), int(cs[1].id));
		}
	};
}
