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
// analysis field exactly when it asks for an estimate, so VectorDemand's behaviour is unchanged.
// CPU only.

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
			for (int flags = 0; flags < 32; ++flags)
				for (int ahead = 0; ahead <= 2; ++ahead)
					for (int under = 0; under <= 1; ++under)
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
							for (int i = 0; i < kConsumerCount; ++i)
								in.wants[i] = (wants & (1u << i)) != 0;
							const Decision d = DecideWith(in, under != 0);
							cs.clear();
							DeclareConsumers(d, cs);
							const Graph g = BuildGraph(reg, cs, gs);
							Assert::IsTrue(g.valid);
							Assert::AreEqual(d.producer, g.Has(VpSkeleton::kP2VectorPair));
							Assert::AreEqual(d.estimate, g.Has(VpSkeleton::kP3AnalysisField));
							Assert::IsFalse(g.Has(VpSkeleton::kP4SourceStats));
							if (d.producer) {
								const Graph m = BuildGraph(reg, cs, noEngine);
								Assert::IsTrue(m.Has(VpSkeleton::kP2mMatcherPair));
								Assert::IsFalse(m.Has(VpSkeleton::kP2VectorPair));
							}
							Assert::AreEqual(uint8_t(d.mode == Mode::Backward ? 0 : 1), LaneLeadFor(d, 1));
							++checked;
						}
			// 32 flag sets x 3 queue depths x 2 under-load x 2^11 original wants x 2 camera-group states (none / all) x 2 camera-fit states.
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
			in.smoothMotion = true;   // the output consumer is unserved under smooth motion
			in.wants[int(Consumer::SigmaDelta)] = true;
			in.wants[int(Consumer::MotionIntent)] = true;
			in.wants[int(Consumer::GrainMap)] = true;
			in.wants[int(Consumer::DejudderExtras)] = true;   // synthesis: never declared
			// WAIVER-C1-09 (W2-C1 B9): input precondition only - the de-judder runs (a starter), so the reuse-only consumers are served.
			in.starterEngaged = true;
			const Decision d = Decide(in);
			std::vector<ConsumerDecl> cs;
			DeclareConsumers(d, cs);
			Assert::AreEqual(size_t(2), cs.size());
			Assert::AreEqual(int(kConsumerIdBase + int(Consumer::GrainMap)), int(cs[0].id));
			Assert::AreEqual(int(kConsumerIdBase + int(Consumer::MotionIntent)), int(cs[1].id));
		}
	};
}
