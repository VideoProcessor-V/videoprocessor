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
#include <VP-V/modules/motion/estimation/VectorDemandProducts.h>

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
			in.starterEngaged = true;   // W2-C1: a starter runs the engine (the reuse-only rows below need one)
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
			Consumer::DetailPulse, Consumer::EntranceSegment, Consumer::SourceMonitorPan, Consumer::MotionIntent,
			Consumer::TextureRestore, Consumer::ResidualStabilise, Consumer::TemporalDetail };
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
				// W2-C1: reuse-only: served from the starter's vectors, never an estimate or a producer of its own.
				Assert::IsFalse(d.estimate, L"no estimate of its own");
				Assert::IsFalse(d.producer, L"no producer of its own");
				Assert::IsTrue(d.reason == Reason::Forward, L"reason");
			}
		}

		TEST_METHOD(StatisticsOnlyForwardHasNoEstimateAndFieldConsumerAddsOne)
		{
			const Decision stats = Decide(Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent }));
			Assert::IsTrue(stats.mode == Mode::Forward, L"forward");
			Assert::IsFalse(stats.estimate, L"no render-time estimate");
			Assert::IsFalse(stats.producer, L"W2-C1: statistics are reuse-only");
			const Decision both = Decide(Make({ Consumer::SourceMonitorPan, Consumer::GrainMap }));
			Assert::IsFalse(both.estimate || both.producer, L"W2-C1: a reuse-only field consumer brings no estimate");
			const Decision camera = Decide(Make({ Consumer::SourceMonitorPan, Consumer::CameraFit }));
			Assert::IsTrue(camera.estimate && camera.producer, L"the camera fit (a starter) brings the estimate and the producer");
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
				Assert::IsFalse(d.producer, L"W2-C1: reuse-only, the starter's producer serves it");
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

		TEST_METHOD(TheCameraFitIsASourceFieldConsumerThatReadsThePairAndTheField)
		{
			// SK-W stage D: appended at index 14, so no earlier index moved; it reads P2 and P3 like any source-field consumer.
			Assert::AreEqual(14, static_cast<int>(Consumer::CameraFit));
			Assert::AreEqual(15, kConsumerCount);
			Assert::IsTrue(DomainOf(Consumer::CameraFit) == Domain::SourceField);
			Assert::AreEqual(std::string("camera fit"), std::string(Name(Consumer::CameraFit)));
			const Decision d = Decide(Make({ Consumer::CameraFit }));
			Assert::IsTrue(d.mode == Mode::Forward);
			Assert::IsTrue(d.Serves(Consumer::CameraFit));
			Assert::IsTrue(d.estimate && d.producer);
			const VpSkeleton::ConsumerDecl decl = VectorDemand::DeclarationOf(Consumer::CameraFit);
			Assert::AreEqual(2, static_cast<int>(decl.count));
			Assert::AreEqual(static_cast<int>(VpSkeleton::kP2VectorPair), static_cast<int>(decl.consumes[0].product));
			Assert::AreEqual(static_cast<int>(VpSkeleton::kP3AnalysisField), static_cast<int>(decl.consumes[1].product));
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

		TEST_METHOD(LowLatencyUsesOnlyTheModesOwnQueuedFrames)
		{
			// Owner 2026-11-01: in low latency the vectors never ADD queue depth. Competitive (no frame
			// queued ahead, capacity 0) runs backward pairs; games (its own queue holds frames ahead,
			// capacity >= 1) uses those already-queued frames forward (ForwardShallow), never deeper than
			// the queue holds (EngageAhead bounded by the capacity).
			for (int capacity = 0; capacity <= 8; ++capacity)
			{
				Input in = Make({ Consumer::GrainMap, Consumer::DebandHold, Consumer::SourceMonitorPan });
				in.lowLatency = true;
				in.queueAheadCapacity = capacity;
				const Decision d = Decide(in);
				Assert::IsTrue(d.lowLatencyAllowed, L"low latency allowed");
				Assert::IsTrue(d.Serves(Consumer::SourceMonitorPan), L"statistics served");
				if (capacity == 0)
				{
					Assert::IsTrue(d.mode == Mode::Backward && d.reason == Reason::BackwardDepth0, L"competitive: backward");
					Assert::IsFalse(d.estimate, L"competitive: no forward estimate");
					Assert::AreEqual((1u << static_cast<int>(Consumer::GrainMap)) | (1u << static_cast<int>(Consumer::DebandHold)), d.unserved,
						L"competitive: the field consumers fall back visibly");
				}
				else
				{
					Assert::IsTrue(d.mode == Mode::Forward && d.reason == Reason::ForwardShallow, L"games: forward from the queued frames");
					Assert::AreEqual(0u, d.unserved, L"games: every consumer served");
				}
				for (int configured = 0; configured <= 8; ++configured)
				{
					const int ahead = EngageAhead(configured, capacity);
					Assert::IsTrue(ahead >= 1, L"at least one frame");
					if (capacity >= 1)
						Assert::IsTrue(ahead <= capacity, L"never deeper than the mode's own queue");
				}
			}
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
			Assert::IsFalse(d.producer, L"W2-C1: reuse-only");
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
		// ---- W2-C1 (B9): starters and reuse-only consumers -----------------------------------------------------------
		TEST_METHOD(StarterMaskIsDejudderInterpolatorHiccupAndCameraFit)
		{
			for (int i = 0; i < kConsumerCount; ++i)
			{
				const Consumer c = ConsumerAt(i);
				const bool expected = c == Consumer::DejudderExtras || c == Consumer::Interpolator || c == Consumer::HiccupRepair ||
					c == Consumer::CameraFit;
				Assert::AreEqual(expected, IsStarter(c), L"starter set (owner 2026-11-10)");
			}
			Assert::AreEqual(8, static_cast<int>(Reason::NoStarter), L"NoStarter appended, no index moved");
		}

		// W2-C1 (coordinator 2026-11-11): the status never says "idle (no starter)" while a motion engine exists; it names why the
		// engine is loaded (kept idle for hiccup repair, or loaded but not running). Serving is unchanged: still refused, nothing starts.
		TEST_METHOD(LoadedEngineWithoutAStarterIsNamedNotIdle)
		{
			const Consumer cases[] = { Consumer::SourceMonitorPan, Consumer::DetailPulse, Consumer::OcclusionMask };
			for (Consumer c : cases)
			{
				Input in = Make({ c });
				in.starterEngaged = false;
				in.engineLoaded = true;
				in.engineLoadedForHiccupRepair = true;
				const Decision d = Decide(in);
				Assert::IsTrue(d.reason == Reason::NoStarter, L"still a refused start");
				Assert::AreEqual(0u, d.served, L"an idle engine serves nothing");
				Assert::AreEqual(1u << static_cast<int>(c), d.refused, L"refused");
				Assert::IsFalse(d.estimate || d.producer, L"nothing starts");
				const std::string hiccup = StatusLine(d);
				Assert::IsTrue(hiccup.find("idle (no starter)") == std::string::npos, L"not called idle while loaded");
				Assert::IsTrue(hiccup.find("loaded idle for hiccup repair (no starter runs it)") != std::string::npos, L"names the hiccup keep");
				Assert::IsTrue(hiccup.find("no starter runs the motion engine") != std::string::npos, L"still says why it is unserved");

				in.engineLoadedForHiccupRepair = false;
				const std::string held = StatusLine(Decide(in));
				Assert::IsTrue(held.find("idle (no starter)") == std::string::npos, L"not called idle while held");
				Assert::IsTrue(held.find("loaded, not running (no starter runs it)") != std::string::npos, L"names a held engine");

				in.engineLoaded = false;
				in.engineLoadedForHiccupRepair = true;   // without an engine the hiccup flag says nothing
				const std::string none = StatusLine(Decide(in));
				Assert::IsTrue(none.find("idle (no starter)") != std::string::npos, L"no engine: idle (no starter)");
				Assert::IsTrue(none.find("loaded") == std::string::npos, L"no engine: never called loaded");
			}
		}

		TEST_METHOD(ReuseOnlyConsumersWithoutAStarterAreRefusedVisibly)
		{
			for (Consumer c : kNonSynthesis)
			{
				Input in = Make({ c });
				in.starterEngaged = false;
				const Decision d = Decide(in);
				Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::NoStarter, L"no starter: off");
				Assert::AreEqual(0u, d.served);
				Assert::AreEqual(1u << static_cast<int>(c), d.unserved, L"unserved");
				Assert::AreEqual(1u << static_cast<int>(c), d.refused, L"a refused start");
				Assert::IsFalse(d.estimate || d.producer, L"nothing starts");
				const std::string s = StatusLine(d);
				Assert::IsTrue(s.find("idle (no starter)") != std::string::npos, L"status says no starter");
				Assert::IsTrue(s.find("no starter runs the motion engine") != std::string::npos, L"says why");
			}
			Input low = Make({ Consumer::SourceMonitorPan, Consumer::GrainMap });
			low.starterEngaged = false;
			low.lowLatency = true;
			low.queueAheadCapacity = 0;
			const Decision d = Decide(low);
			Assert::IsTrue(d.reason == Reason::NoStarter && !d.producer, L"competitive low latency: no backward producer for statistics alone");
		}

		// W2-C1 (C1-4 defect fix): the occlusion mask is reuse-only but synthesis-domain (never in served). Without a starter it is a
		// refused start too - alone (the synthesis-only return) and beside other reuse-only wants - so DejudderPolicy's
		// occlusionMaskServed reads false and its own matcher never runs. With a starter it is not refused.
		TEST_METHOD(TheOcclusionMaskWithoutAStarterIsARefusedStart)
		{
			const unsigned mask = 1u << static_cast<int>(Consumer::OcclusionMask);
			Input alone = Make({ Consumer::OcclusionMask });
			alone.starterEngaged = false;
			const Decision a = Decide(alone);
			Assert::IsTrue(a.mode == Mode::Off && a.reason == Reason::NoStarter, L"mask alone, no starter: off, NoStarter");
			Assert::AreEqual(mask, a.refused, L"mask alone: a refused start");
			Assert::AreEqual(mask, a.unserved, L"mask alone: unserved (named)");
			Assert::AreEqual(0u, a.served);
			Assert::IsFalse(a.estimate || a.producer, L"nothing starts");
			Input withReuse = Make({ Consumer::OcclusionMask, Consumer::GrainMap, Consumer::DetailPulse });
			withReuse.starterEngaged = false;
			const Decision b = Decide(withReuse);
			Assert::IsTrue(b.reason == Reason::NoStarter, L"mask + reuse, no starter: NoStarter");
			Assert::AreEqual(mask, b.refused & mask, L"mask + reuse: the mask is refused too");
			Assert::AreEqual(mask, b.unserved & mask, L"mask + reuse: the mask is unserved");
			Assert::IsTrue(UnservedText(b).find("occlusion mask") != std::string::npos, L"the mask is named");
			Input engaged = Make({ Consumer::OcclusionMask });
			Assert::AreEqual(0u, Decide(engaged).refused, L"mask alone with a starter: not refused");
			Input engagedReuse = Make({ Consumer::OcclusionMask, Consumer::GrainMap });
			Assert::AreEqual(0u, Decide(engagedReuse).refused, L"mask + reuse with a starter: not refused");
		}

		TEST_METHOD(TheCameraFitStartsAloneAndEngagesTheReuseOnlyConsumers)
		{
			Input in = Make({ Consumer::CameraFit, Consumer::DetailPulse, Consumer::SourceMonitorPan });
			in.starterEngaged = false;
			const Decision d = Decide(in);
			Assert::IsTrue(d.mode == Mode::Forward && d.reason == Reason::Forward);
			Assert::IsTrue(d.Serves(Consumer::CameraFit) && d.Serves(Consumer::DetailPulse) && d.Serves(Consumer::SourceMonitorPan));
			Assert::AreEqual(0u, d.refused);
			Assert::IsTrue(d.estimate && d.producer);
			Assert::AreEqual(1u << static_cast<int>(Consumer::CameraFit), d.starters);
		}

		// W2-C1 MUST 3: with the de-judder running (the starter engaged), the reuse-only consumers are SERVED, not refused.
		TEST_METHOD(WithTheDejudderRunningTheReuseOnlyConsumersAreServed)
		{
			Input in = Make({ Consumer::DejudderExtras, Consumer::SigmaDelta, Consumer::DetailPulse, Consumer::DebandHold, Consumer::SourceMonitorPan });
			in.starterEngaged = true;
			const Decision d = Decide(in);
			Assert::AreEqual(0u, d.refused, L"nothing refused while a starter runs");
			Assert::IsTrue(d.reason != Reason::NoStarter);
			for (Consumer c : { Consumer::SigmaDelta, Consumer::DetailPulse, Consumer::DebandHold, Consumer::SourceMonitorPan })
				Assert::IsTrue(d.Serves(c), L"served");
			in.starterEngaged = false;
			const Decision off = Decide(in);
			const unsigned reuse = (1u << static_cast<int>(Consumer::SigmaDelta)) | (1u << static_cast<int>(Consumer::DetailPulse)) |
				(1u << static_cast<int>(Consumer::DebandHold)) | (1u << static_cast<int>(Consumer::SourceMonitorPan));
			Assert::AreEqual(reuse, off.refused & reuse, L"the de-judder not running: the same consumers are refused");
		}

		TEST_METHOD(SynthesisStartersAreRecordedButNeverServedHere)
		{
			Input in = Make({ Consumer::DejudderExtras, Consumer::HiccupRepair, Consumer::GrainMap });
			const Decision d = Decide(in);
			Assert::AreEqual((1u << static_cast<int>(Consumer::DejudderExtras)) | (1u << static_cast<int>(Consumer::HiccupRepair)), d.starters);
			Assert::IsTrue(d.Serves(Consumer::GrainMap) && !d.Serves(Consumer::DejudderExtras), L"synthesis never in served");
			Assert::IsFalse(d.estimate || d.producer, L"synthesis starters run through their own policies");
		}

		TEST_METHOD(EngineStatusMapsEachReason)
		{
			EngineFacts f;
			f.decision = Decide(Make({ Consumer::DejudderExtras, Consumer::GrainMap }));
			f.starterEngaged = true;
			f.producerRunning = true;
			f.frame = 7;
			MotionEngineStatus st = DeriveEngineStatus(f);
			Assert::IsTrue(st.state == MotionEngineState::On && st.reason == MotionEngineReason::None && st.reasons == 0u, L"on");
			Assert::AreEqual(7ull, static_cast<unsigned long long>(st.frame));
			Assert::AreEqual(1u << static_cast<int>(Consumer::DejudderExtras), st.starters);

			struct Row { void (*set)(EngineFacts&); MotionEngineState state; MotionEngineReason reason; };
			const Row rows[] = {
				{ [](EngineFacts& x) { x.opticalFlow = false; }, MotionEngineState::Degraded, MotionEngineReason::OpticalFlowOff },
				{ [](EngineFacts& x) { x.nvofStartError = true; x.producerRunning = false; }, MotionEngineState::Degraded, MotionEngineReason::NvofUnavailable },
				{ [](EngineFacts& x) { x.producerRecovering = true; }, MotionEngineState::Degraded, MotionEngineReason::ProducerRecovering },
				{ [](EngineFacts& x) { x.resyncWindow = true; }, MotionEngineState::Degraded, MotionEngineReason::Resync },
				{ [](EngineFacts& x) { x.starterEngaged = false; }, MotionEngineState::Off, MotionEngineReason::NoStarter },
				{ [](EngineFacts& x) { x.vramRelease = true; }, MotionEngineState::Off, MotionEngineReason::VramRelease },
				{ [](EngineFacts& x) { x.vectorDemand = false; }, MotionEngineState::Off, MotionEngineReason::VectorDemandOff },
				{ [](EngineFacts& x) { x.lowLatency = true; x.lowLatencyNoVectors = true; }, MotionEngineState::Off, MotionEngineReason::LowLatency },
			};
			for (const Row& r : rows)
			{
				EngineFacts g = f;
				r.set(g);
				st = DeriveEngineStatus(g);
				Assert::IsTrue(st.state == r.state, L"state");
				Assert::IsTrue(st.reason == r.reason, L"leading reason");
				Assert::IsTrue(st.Has(r.reason), L"reason bit");
				Assert::IsTrue(std::string(ReasonWords(r.reason)).size() > 0, L"words");
			}
			EngineFacts shallow = f;
			Input ll = Make({ Consumer::DejudderExtras, Consumer::GrainMap });
			ll.lowLatency = true;
			ll.queueAheadCapacity = 1;
			shallow.decision = Decide(ll);
			st = DeriveEngineStatus(shallow);
			Assert::IsTrue(st.state == MotionEngineState::Degraded && st.reason == MotionEngineReason::LowLatency, L"ForwardShallow = degraded");
			EngineFacts inter = f;
			Input il = Make({ Consumer::GrainMap });
			il.interlaced = true;
			inter.decision = Decide(il);
			st = DeriveEngineStatus(inter);
			Assert::IsTrue(st.state == MotionEngineState::Off && st.reason == MotionEngineReason::Interlaced, L"interlaced = off");
			Assert::IsTrue(MotionEngineStatus().state == MotionEngineState::Off && MotionEngineStatus().reason == MotionEngineReason::None,
				L"default (fakes): off / none");
			Assert::AreEqual(std::string("degraded"), std::string(StateWord(MotionEngineState::Degraded)));
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
