// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Vector engine on demand (VectorDemand.h): pure CPU tests, no GPU.
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/OriginalPathway.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>

#include <initializer_list>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VectorDemand;

namespace VectorDemandTest
{
	namespace
	{
		Input Make(std::initializer_list<Consumer> wants)
		{
			Input in;
			in.enabled = true;
			in.slotsRun = true;
			in.queueAheadCapacity = 4;
			for (Consumer c : wants)
				in.wants[static_cast<int>(c)] = true;
			return in;
		}
		bool Same(const Decision& a, const Decision& b)
		{
			return a.mode == b.mode && a.reason == b.reason && a.served == b.served && a.unserved == b.unserved &&
				a.estimate == b.estimate && a.producer == b.producer && a.lowLatencyAllowed == b.lowLatencyAllowed &&
				a.smoothMotion == b.smoothMotion && a.synthesisOnly == b.synthesisOnly;
		}
		const Consumer kNonSynthesis[] = { Consumer::SigmaDelta, Consumer::GrainMap, Consumer::DebandHold,
			Consumer::DetailPulse, Consumer::EntranceSegment, Consumer::SourceMonitorPan, Consumer::MotionIntent };
	}

	TEST_CLASS(VectorDemandTests)
	{
	public:
		TEST_METHOD(EachNonSynthesisConsumerAloneIsServedForward)
		{
			for (Consumer c : kNonSynthesis)
			{
				const Decision d = Decide(Make({ c }));
				Assert::IsTrue(d.mode == Mode::Forward, L"forward");
				Assert::IsTrue(d.Serves(c), L"served");
				Assert::AreEqual(0u, d.unserved);
				const bool statistics = DomainOf(c) == Domain::SourceStatistics;
				Assert::AreEqual(!statistics, d.estimate, L"estimate only for field consumers");
				Assert::IsTrue(d.producer, L"producer");
				Assert::IsTrue(d.reason == Reason::Forward, L"reason");
			}
		}

		TEST_METHOD(StatisticsOnlyForwardHasNoEstimateAndFieldConsumerAddsOne)
		{
			const Decision stats = Decide(Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent }));
			Assert::IsTrue(stats.mode == Mode::Forward, L"forward");
			Assert::IsFalse(stats.estimate, L"no render-time estimate");
			Assert::IsTrue(stats.producer, L"producer");
			const Decision both = Decide(Make({ Consumer::SourceMonitorPan, Consumer::GrainMap }));
			Assert::IsTrue(both.estimate && both.producer, L"a field consumer brings the estimate");
		}

		TEST_METHOD(StatisticsConsumersAloneAtDepthZeroRunBackward)
		{
			for (Consumer c : { Consumer::SourceMonitorPan, Consumer::MotionIntent })
			{
				Input in = Make({ c });
				in.lowLatency = true;
				in.queueAheadCapacity = 0;
				const Decision d = Decide(in);
				Assert::IsTrue(d.mode == Mode::Backward, L"backward");
				Assert::IsTrue(d.Serves(c), L"served");
				Assert::IsTrue(d.producer, L"producer");
				Assert::IsFalse(d.estimate, L"no estimate");
			}
		}

		TEST_METHOD(NoConsumerIsOff)
		{
			const Decision d = Decide(Make({}));
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::IsTrue(d.reason == Reason::NoConsumer, L"reason");
			Assert::AreEqual(0u, d.served);
			Assert::AreEqual(0u, d.unserved);
			Assert::IsFalse(d.estimate || d.producer, L"nothing on");
			Assert::AreEqual(std::string("Vector engine: idle (no consumer)"), StatusLine(d));
		}

		TEST_METHOD(SynthesisConsumersOnlyAreNeverServedHere)
		{
			const Decision d = Decide(Make({ Consumer::Interpolator, Consumer::DejudderExtras }));
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::AreEqual(0u, d.served);
			Assert::AreEqual(0u, d.unserved);
			Assert::AreEqual(std::string("Vector engine: idle (served by de-judder/interpolator policies)"), StatusLine(d));
		}

		TEST_METHOD(DisabledIsIdenticalToDefaultConstructed)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::SigmaDelta, Consumer::Interpolator, Consumer::MotionIntent });
			for (int i = 0; i < kConsumerCount; ++i)
				in.wants[i] = true;
			in.enabled = false;
			in.smoothMotion = true;
			in.interlaced = true;
			Assert::IsTrue(Same(Decide(in), Decision()), L"field for field default");
			Assert::IsTrue(Decide(in).reason == Reason::Disabled, L"disabled");
			Assert::AreEqual(std::string(), StatusLine(Decide(in)));
		}

		TEST_METHOD(SlotsOffServesNothing)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::SigmaDelta });
			in.slotsRun = false;
			const Decision d = Decide(in);
			Assert::IsTrue(d.reason == Reason::SlotsOff, L"slots off");
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::AreEqual(0u, d.served);
			Assert::AreEqual(0u, d.unserved);
			Assert::AreEqual(std::string("Vector engine: off (slot off)"), StatusLine(d));
		}

		TEST_METHOD(SmoothMotionKeepsSourceAnalysisAndDropsOutputDither)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::MotionIntent, Consumer::SigmaDelta });
			in.smoothMotion = true;
			const Decision d = Decide(in);
			Assert::IsTrue(d.mode == Mode::Forward, L"forward");
			Assert::IsTrue(d.Serves(Consumer::GrainMap) && d.Serves(Consumer::MotionIntent), L"source served");
			Assert::IsFalse(d.Serves(Consumer::SigmaDelta), L"dither not served");
			Assert::AreEqual(1u << static_cast<int>(Consumer::SigmaDelta), d.unserved);
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
			Assert::IsTrue(f.engaged, L"engaged");
			Assert::AreEqual(std::string("vector_engine"), f.id);
			Assert::IsTrue(f.reason.find("output dither") != std::string::npos, L"names output dither");
			Assert::IsTrue(f.reason.find("libplacebo smooth motion owns output") != std::string::npos, L"says why");
		}

		TEST_METHOD(ExclusiveModelLeavesEverythingUnserved)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::SigmaDelta });
			in.smoothMotion = true;
			const Decision d = DecideWith(in, false);
			Assert::IsTrue(d.reason == Reason::SmoothMotionExclusive, L"exclusive");
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::AreEqual(0u, d.served);
			Assert::AreEqual((1u << static_cast<int>(Consumer::GrainMap)) | (1u << static_cast<int>(Consumer::SigmaDelta)), d.unserved);
			Assert::IsFalse(d.estimate || d.producer, L"nothing on");
		}

		TEST_METHOD(InterlacedServesNothing)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::MotionIntent });
			in.interlaced = true;
			const Decision d = Decide(in);
			Assert::IsTrue(d.reason == Reason::Interlaced, L"interlaced");
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::AreEqual(0u, d.served);
			Assert::AreNotEqual(0u, d.unserved);
			Assert::IsFalse(d.estimate || d.producer, L"nothing on");
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
			Assert::IsTrue(f.engaged && f.reason.find("interlaced") != std::string::npos, L"visible, says interlaced");
		}

		TEST_METHOD(DepthOneLowLatencyIsShallowForward)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::DebandHold });
			in.lowLatency = true;
			in.queueAheadCapacity = 1;
			const Decision d = Decide(in);
			Assert::IsTrue(d.mode == Mode::Forward, L"forward");
			Assert::IsTrue(d.reason == Reason::ForwardShallow, L"shallow");
			Assert::IsTrue(d.lowLatencyAllowed, L"low latency allowed");
			Assert::AreEqual(0u, d.unserved);
			Assert::AreEqual(1, EngageAhead(2, 1));
			Assert::AreEqual(1, EngageAhead(4, 0));
			Assert::AreEqual(2, EngageAhead(2, 5));
			Assert::AreEqual(1, EngageAhead(0, 5));
		}

		TEST_METHOD(DepthZeroLowLatencyIsBackwardForStatisticsOnly)
		{
			Input in = Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent, Consumer::DebandHold,
				Consumer::GrainMap, Consumer::DetailPulse, Consumer::SigmaDelta });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			const Decision d = Decide(in);
			Assert::IsTrue(d.mode == Mode::Backward, L"backward");
			Assert::IsTrue(d.reason == Reason::BackwardDepth0, L"depth 0");
			Assert::IsTrue(d.lowLatencyAllowed, L"low latency allowed");
			Assert::IsTrue(d.Serves(Consumer::SourceMonitorPan) && d.Serves(Consumer::MotionIntent), L"statistics served");
			Assert::IsFalse(d.Serves(Consumer::DebandHold) || d.Serves(Consumer::GrainMap) ||
				d.Serves(Consumer::DetailPulse) || d.Serves(Consumer::SigmaDelta), L"others unserved");
			Assert::AreEqual(4u, [&] { unsigned n = 0; for (unsigned b = d.unserved; b; b &= b - 1) ++n; return n; }());
			Assert::IsTrue(d.producer, L"producer");
			Assert::IsFalse(d.estimate, L"no estimate");
			Assert::AreEqual(std::string("Vector engine: backward pairs (no frame ahead) for source monitor, intent; "
				"output dither, grain map, deband hold, detail pulse without vectors (needs a frame ahead)"), StatusLine(d));
		}

		TEST_METHOD(MotionAheadBackwardPairsIsDepthZeroAndDefaultIsUnchanged)
		{
			const MotionAhead::Decision b = MotionAhead::Decide(4, true, true, 3, true, true, true);
			Assert::IsTrue(b.reason == MotionAhead::Reason::BackwardPairs, L"backward pairs");
			Assert::AreEqual(0, b.depth);
			const MotionAhead::Decision old = MotionAhead::Decide(4, true, true, 3, true, true);
			Assert::IsTrue(old.reason == MotionAhead::Reason::LowLatency, L"old low-latency result");
			Assert::AreEqual(0, old.depth);
			const MotionAhead::Decision normal = MotionAhead::Decide(4, false, true, 3, true, true, true);
			Assert::IsTrue(normal.reason == MotionAhead::Reason::ShallowQueue, L"flag only matters in low latency");
			Assert::AreEqual(3, normal.depth);
			Assert::AreEqual(std::string("backward pairs on arrival (no frame ahead)"), std::string(MotionAhead::Describe(MotionAhead::Reason::BackwardPairs)));
		}

		TEST_METHOD(FallbackCatalogWordingAndRelease)
		{
			const Decision served = Decide(Make({ Consumer::GrainMap }));
			Assert::IsFalse(FallbackCatalog::VectorEngine(served).engaged, L"released when nothing unserved");
			Assert::AreEqual(std::string("vector_engine"), FallbackCatalog::VectorEngine(served).id);

			Input in = Make({ Consumer::DebandHold });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(Decide(in));
			Assert::IsTrue(f.engaged, L"engaged");
			Assert::AreEqual(std::string("deband hold without vectors (needs a frame ahead)"), f.reason);

			const FallbackCatalog::Decision p = FallbackCatalog::VectorEngineProducer(true);
			Assert::IsTrue(p.engaged, L"producer engaged");
			Assert::AreEqual(std::string("vector_engine_producer"), p.id);
			Assert::AreEqual(std::string("Vector engine look-ahead stores nothing for this size (render-time estimate runs)"), p.reason);
			Assert::IsFalse(FallbackCatalog::VectorEngineProducer(false).engaged, L"producer released");

			const FallbackCatalog::Decision g = FallbackCatalog::VectorEngineGlobal(true);
			Assert::IsTrue(g.engaged, L"global engaged");
			Assert::AreEqual(std::string("vector_engine_global"), g.id);
			Assert::AreEqual(std::string("source monitor fast-pan / intent without whole-frame motion (vector engine pair missed)"), g.reason);
			Assert::IsFalse(FallbackCatalog::VectorEngineGlobal(false).engaged, L"global released");
		}

		TEST_METHOD(StatusLinesAreExact)
		{
			Assert::AreEqual(std::string("Vector engine: on (forward) for grain map, deband hold"),
				StatusLine(Decide(Make({ Consumer::GrainMap, Consumer::DebandHold }))));

			Input in = Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent, Consumer::DebandHold });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			Assert::AreEqual(std::string("Vector engine: backward pairs (no frame ahead) for source monitor, intent; "
				"deband hold without vectors (needs a frame ahead)"), StatusLine(Decide(in)));

			Assert::AreEqual(std::string("Vector engine: idle (no consumer)"), StatusLine(Decide(Make({}))));

			for (Consumer c : kNonSynthesis)
			{
				Input w = Make({ c });
				w.lowLatency = true;
				w.queueAheadCapacity = 0;
				const std::string s = StatusLine(Decide(w));
				Assert::IsTrue(s.compare(0, 14, "Vector engine:") == 0, L"prefix");
			}
		}
		TEST_METHOD(OriginalPathwayTurnsVectorDemandOff)
		{
			bool found = false;
			for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
				if (std::string(o.key) == "vector_demand")
				{
					found = true;
					Assert::AreEqual(std::string("off"), std::string(o.value));
					Assert::AreEqual(std::string("motion.estimation"), std::string(o.slot));
				}
			Assert::IsTrue(found, L"vector_demand is an Original pathway override");
			// Off decides nothing, whatever wants vectors: the renderer then runs today's path.
			Input in = Make({ Consumer::GrainMap, Consumer::SigmaDelta });
			in.enabled = false;
			const Decision d = Decide(in);
			Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::Disabled && d.served == 0 && d.unserved == 0 &&
				!d.estimate && !d.producer && !d.lowLatencyAllowed);
		}
	};
}
