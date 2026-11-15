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
			// Batch B: no starter input any more (the engine is one switch, `enabled`); pairsExpected defaults to true.
			for (Consumer c : wants)
				in.wants[static_cast<int>(c)] = true;
			return in;
		}
		bool Same(const Decision& a, const Decision& b)
		{
			// Batch B: the no-pairs fields are compared too (a disabled decision carries none of them).
			return a.mode == b.mode && a.reason == b.reason && a.served == b.served && a.unserved == b.unserved &&
				a.estimate == b.estimate && a.producer == b.producer && a.lowLatencyAllowed == b.lowLatencyAllowed &&
				a.smoothMotion == b.smoothMotion && a.synthesisOnly == b.synthesisOnly && a.noPairs == b.noPairs &&
				a.matcherFallback == b.matcherFallback && a.pairsDeliberatelyOff == b.pairsDeliberatelyOff;
		}
		unsigned Bit(Consumer c) { return 1u << static_cast<int>(c); }
		const Consumer kNonSynthesis[] = { Consumer::SigmaDelta, Consumer::GrainMap, Consumer::DebandHold,
			Consumer::DetailPulse, Consumer::EntranceSegment, Consumer::SourceMonitorPan, Consumer::MotionIntent,
			Consumer::TextureRestore, Consumer::ResidualStabilise, Consumer::TemporalDetail };
	}

	// The decision the engine host acts on: VectorDemand::DecideForHost (VectorEngine::UpdateVectorEngine), the ONE decision since the
	// batch B fold (gate-union-b1 (i); WAIVER-B1-ENGINE-06). Every test here pins that function: under libplacebo smooth motion and in
	// competitive low latency it decides Off with the reason (plan (b)/(d4)); the expectations of the retired Decide() (analysis under
	// smooth motion, backward pairs at depth 0) moved to those Off decisions, each consumer still named (VectorDemandHostTests too).
	TEST_CLASS(VectorDemandTests)
	{
	public:
		TEST_METHOD(EachNonSynthesisConsumerAloneIsServedForward)
		{
			for (Consumer c : kNonSynthesis)
			{
				const Decision d = DecideForHost(Make({ c }));
				Assert::IsTrue(d.mode == Mode::Forward, L"forward");
				Assert::IsTrue(d.Serves(c), L"served");
				Assert::AreEqual(0u, d.unserved);
				// Batch B: the ON engine runs the producer for any served want (no starter); with pairs expected the render-time matcher
				// estimate does not run (it is the fallback without pairs; the camera fit is not in this list).
				Assert::IsFalse(d.estimate, L"pairs expected: no render-time estimate");
				Assert::IsTrue(d.producer, L"the ON engine runs the producer for any served want");
				Assert::IsFalse(d.matcherFallback, L"pairs expected: no matcher fallback");
				Assert::IsTrue(d.reason == Reason::Forward, L"reason");
			}
		}

		TEST_METHOD(StatisticsOnlyForwardHasNoEstimateAndFieldConsumerAddsOne)
		{
			const Decision stats = DecideForHost(Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent }));
			Assert::IsTrue(stats.mode == Mode::Forward, L"forward");
			Assert::IsFalse(stats.estimate, L"no render-time estimate");
			Assert::IsTrue(stats.producer, L"batch B: the producer runs for the served statistics consumers");
			const Decision both = DecideForHost(Make({ Consumer::SourceMonitorPan, Consumer::GrainMap }));
			Assert::IsFalse(both.estimate, L"pairs expected: a field consumer brings no render-time estimate");
			Assert::IsTrue(both.producer, L"batch B: the producer runs");
			const Decision camera = DecideForHost(Make({ Consumer::SourceMonitorPan, Consumer::CameraFit }));
			Assert::IsTrue(camera.estimate && camera.producer, L"the camera fit (its own matcher field) brings the estimate and the producer");
		}

		// Batch B fold (WAIVER-B1-ENGINE-06; was StatisticsConsumersAloneAtDepthZeroRunBackward): competitive low latency (no frame
		// ahead) is motion-free, so a statistics consumer alone is named unserved and nothing starts (the backward pairs are retired).
		TEST_METHOD(StatisticsConsumersAloneAtDepthZeroAreMotionFree)
		{
			for (Consumer c : { Consumer::SourceMonitorPan, Consumer::MotionIntent })
			{
				Input in = Make({ c });
				in.lowLatency = true;
				in.queueAheadCapacity = 0;
				const Decision d = DecideForHost(in);
				Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::BackwardDepth0, L"competitive: off, motion-free");
				Assert::IsFalse(d.Serves(c), L"not served");
				Assert::AreEqual(Bit(c), d.unserved, L"named unserved");
				Assert::IsFalse(d.producer, L"no producer (no backward pair)");
				Assert::IsFalse(d.estimate, L"no estimate");
				Assert::IsFalse(d.lowLatencyAllowed, L"no low-latency vectors path");
				Assert::AreEqual(std::string(Name(c)) + " without vectors (competitive low latency (motion-free))", UnservedText(d));
			}
		}

		TEST_METHOD(NoConsumerIsOff)
		{
			const Decision d = DecideForHost(Make({}));
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
			const Decision d = DecideForHost(Make({ Consumer::CameraFit }));
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
			const Decision d = DecideForHost(Make({ Consumer::Interpolator, Consumer::DejudderExtras }));
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
			Assert::IsTrue(Same(DecideForHost(in), Decision()), L"field for field default");
			Assert::IsTrue(DecideForHost(in).reason == Reason::Disabled, L"disabled");
			Assert::AreEqual(std::string(), StatusLine(DecideForHost(in)));
		}

		TEST_METHOD(SlotsOffServesNothing)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::SigmaDelta });
			in.slotsRun = false;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.reason == Reason::SlotsOff, L"slots off");
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::AreEqual(0u, d.served);
			Assert::AreEqual(0u, d.unserved);
			Assert::AreEqual(std::string("Vector engine: off (slot off)"), StatusLine(d));
		}

		// Batch B fold (WAIVER-B1-ENGINE-06; was SmoothMotionKeepsSourceAnalysisAndDropsOutputDither, the retired analysis-under-smooth-
		// motion semantics): libplacebo smooth motion owns output exclusively, so the source consumers go without vectors like the dither,
		// and the one vector_engine entry names every one of them with why.
		TEST_METHOD(SmoothMotionOwnsOutputAndNamesTheSourceConsumersAndTheDither)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::MotionIntent, Consumer::SigmaDelta });
			in.smoothMotion = true;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::SmoothMotionExclusive, L"off, exclusive");
			Assert::IsFalse(d.Serves(Consumer::GrainMap) || d.Serves(Consumer::MotionIntent), L"source not served");
			Assert::IsFalse(d.Serves(Consumer::SigmaDelta), L"dither not served");
			Assert::AreEqual(Bit(Consumer::GrainMap) | Bit(Consumer::MotionIntent) | Bit(Consumer::SigmaDelta), d.unserved);
			Assert::IsFalse(d.producer || d.estimate, L"nothing starts");
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
			Assert::IsTrue(f.engaged, L"engaged");
			Assert::AreEqual(std::string("vector_engine"), f.id);
			Assert::IsTrue(f.reason.find("output dither") != std::string::npos, L"names output dither");
			Assert::IsTrue(f.reason.find("grain map") != std::string::npos && f.reason.find("intent") != std::string::npos,
				L"names the source consumers");
			Assert::IsTrue(f.reason.find("libplacebo smooth motion owns output") != std::string::npos, L"says why");
			Assert::AreEqual(std::string("output dither, grain map, intent without vectors (libplacebo smooth motion owns output)"), f.reason);
		}

		TEST_METHOD(ExclusiveModelLeavesEverythingUnserved)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::SigmaDelta });
			in.smoothMotion = true;
			const Decision d = DecideForHost(in);
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
			const Decision d = DecideForHost(in);
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
			// Owner 2026-11-01: in low latency the vectors never ADD queue depth. Games (its own queue holds
			// frames ahead, capacity >= 1) uses those already-queued frames forward (ForwardShallow), never
			// deeper than the queue holds (EngageAhead bounded by the capacity). Batch B fold
			// (WAIVER-B1-ENGINE-06): competitive (no frame queued ahead, capacity 0) is motion-free (was:
			// backward pairs for the statistics consumers): every consumer falls back visibly, nothing starts.
			for (int capacity = 0; capacity <= 8; ++capacity)
			{
				Input in = Make({ Consumer::GrainMap, Consumer::DebandHold, Consumer::SourceMonitorPan });
				in.lowLatency = true;
				in.queueAheadCapacity = capacity;
				const Decision d = DecideForHost(in);
				if (capacity == 0)
				{
					Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::BackwardDepth0, L"competitive: off, motion-free");
					Assert::IsFalse(d.lowLatencyAllowed, L"competitive: no low-latency vectors path");
					Assert::IsFalse(d.Serves(Consumer::SourceMonitorPan), L"competitive: the statistics consumer is not served");
					Assert::IsFalse(d.estimate || d.producer, L"competitive: no estimate, no producer");
					Assert::AreEqual(Bit(Consumer::GrainMap) | Bit(Consumer::DebandHold) | Bit(Consumer::SourceMonitorPan), d.unserved,
						L"competitive: every consumer falls back visibly");
				}
				else
				{
					Assert::IsTrue(d.lowLatencyAllowed, L"low latency allowed");
					Assert::IsTrue(d.Serves(Consumer::SourceMonitorPan), L"statistics served");
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

		// Batch B fold (WAIVER-B1-ENGINE-06; was DepthZeroLowLatencyIsBackwardForStatisticsOnly): at depth 0 in low latency every
		// consumer, the statistics consumers too, goes visibly without vectors; nothing starts; the Ctrl+I line names each with why.
		TEST_METHOD(DepthZeroLowLatencyIsMotionFreeForEveryConsumer)
		{
			Input in = Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent, Consumer::DebandHold,
				Consumer::GrainMap, Consumer::DetailPulse, Consumer::SigmaDelta });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Off, L"off (no backward pairs)");
			Assert::IsTrue(d.reason == Reason::BackwardDepth0, L"depth 0");
			Assert::IsFalse(d.lowLatencyAllowed, L"no low-latency vectors path");
			Assert::IsFalse(d.Serves(Consumer::SourceMonitorPan) || d.Serves(Consumer::MotionIntent), L"statistics unserved");
			Assert::IsFalse(d.Serves(Consumer::DebandHold) || d.Serves(Consumer::GrainMap) ||
				d.Serves(Consumer::DetailPulse) || d.Serves(Consumer::SigmaDelta), L"others unserved");
			Assert::AreEqual(6u, [&] { unsigned n = 0; for (unsigned b = d.unserved; b; b &= b - 1) ++n; return n; }());
			Assert::IsFalse(d.producer, L"no producer");
			Assert::IsFalse(d.estimate, L"no estimate");
			Assert::AreEqual(std::string("Vector engine: off; output dither, grain map, deband hold, detail pulse, source monitor, intent "
				"without vectors (competitive low latency (motion-free))"), StatusLine(d));
		}

		// Batch B fold (WAIVER-B1-ENGINE-06; was MotionAheadBackwardPairsIsDepthZeroAndDefaultIsUnchanged): MotionAhead::Decide has no
		// backward-pair flag any more. Low latency is depth 0 with reason LowLatency, outside it the queued frames are the depth, and no
		// reason describes backward pairs.
		TEST_METHOD(MotionAheadLowLatencyIsDepthZeroWithoutBackwardPairs)
		{
			const MotionAhead::Decision low = MotionAhead::Decide(4, true, true, 3, true, true);
			Assert::IsTrue(low.reason == MotionAhead::Reason::LowLatency, L"low latency: off");
			Assert::AreEqual(0, low.depth);
			const MotionAhead::Decision normal = MotionAhead::Decide(4, false, true, 3, true, true);
			Assert::IsTrue(normal.reason == MotionAhead::Reason::ShallowQueue, L"outside low latency: the queued frames");
			Assert::AreEqual(3, normal.depth);
			for (int r = 0; r <= static_cast<int>(MotionAhead::Reason::NoConsumer); ++r)
				Assert::IsTrue(std::string(MotionAhead::Describe(static_cast<MotionAhead::Reason>(r))).find("backward") == std::string::npos,
					L"no reason describes backward pairs");
		}

		TEST_METHOD(FallbackCatalogWordingAndRelease)
		{
			const Decision served = DecideForHost(Make({ Consumer::GrainMap }));
			Assert::IsFalse(FallbackCatalog::VectorEngine(served).engaged, L"released when nothing unserved");
			Assert::AreEqual(std::string("vector_engine"), FallbackCatalog::VectorEngine(served).id);

			// Batch B fold (WAIVER-B1-ENGINE-06, review b1-fin L1): the competitive why was "needs a frame ahead" (the retired
			// backward-pair cause).
			Input in = Make({ Consumer::DebandHold });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(DecideForHost(in));
			Assert::IsTrue(f.engaged, L"engaged");
			Assert::AreEqual(std::string("deband hold without vectors (competitive low latency (motion-free))"), f.reason);

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

		// refs/109 F1 (AUDIT-R): the look-ahead lead cap binds at 60p with the standard 250 ms queue and is visible (status line +
		// "motion_lookahead_capped"), only sustained; never while the look-ahead does not run.
		TEST_METHOD(LookAheadLeadCapIsVisibleWhenItBinds)
		{
			// 60p, 250 ms queue: 15 frames, 14 queued after the current one; the depth is 4 (66.7 ms at 60 Hz).
			const int depth60 = MotionAhead::DepthAt(4, 60.0);
			Assert::AreEqual(4, depth60);
			// OD 11-14 (BEHAVIOUR-CHANGE OD-11-14-AHEAD-CAP): the cap is the ahead-compute ceiling, was the P2 ring's 13.
			Assert::AreEqual(int(VpSkeleton::kAheadCeiling), MotionAhead::kMaxLead);
			Assert::AreEqual(13, MotionAhead::kRingMaxLead, L"the ring's bound is unchanged");
			Assert::AreEqual(MotionAhead::kMaxLead, MotionAhead::LeadFor(depth60, 14), L"14 queued ahead: the lead stops at the ceiling");
			Assert::IsTrue(MotionAhead::LeadCapped(depth60, 14), L"the cap binds at 60p / 250 ms");
			Assert::IsTrue(MotionAhead::LeadCapped(depth60, 15));
			Assert::IsTrue(MotionAhead::LeadCapped(depth60, 13), L"13 ahead (the old ring cap): now capped too");
			Assert::IsFalse(MotionAhead::LeadCapped(depth60, static_cast<size_t>(MotionAhead::kMaxLead)),
				L"exactly kMaxLead ahead: every queued frame is computed");
			Assert::IsFalse(MotionAhead::LeadCapped(0, 20), L"no look-ahead (depth 0): nothing is capped");
			Assert::IsTrue(MotionAhead::LeadCapped(MotionAhead::DepthAt(4, 240.0), 0), L"240p's depth 16 is held to the ceiling");
			Assert::IsTrue(MotionAhead::LeadCapped(MotionAhead::DepthAt(4, 120.0), 0), L"120p's depth 8 is held to the ceiling");
			// R3 (lead 12): the ceiling with more queued is the DESIGNED state (info line only); the entry only when the depth asks more.
			Assert::IsFalse(MotionAhead::LeadShortOfDepth(depth60), L"60p / 250 ms: the depth (4) is served in full: no fallback entry");
			Assert::IsFalse(MotionAhead::LeadShortOfDepth(MotionAhead::kMaxLead), L"a depth of exactly the ceiling is served in full");
			Assert::IsTrue(MotionAhead::LeadShortOfDepth(MotionAhead::DepthAt(4, 120.0)), L"120p's 8 gets 6: less than asked: the entry");
			Assert::IsTrue(MotionAhead::LeadShortOfDepth(MotionAhead::DepthAt(4, 240.0)), L"240p's 16 gets 6: the entry");
			Assert::IsFalse(MotionAhead::LeadShortOfDepth(0), L"no look-ahead: nothing asked");

			MotionAhead::LeadCapTracker t;
			for (unsigned i = 0; i + 1 < MotionAhead::LeadCapTracker::kLeadCapRun; ++i)
				t.Note(true);
			Assert::IsFalse(t.sustained, L"23 frames: not yet shown");
			t.Note(false);
			t.Note(true);
			Assert::IsFalse(t.sustained, L"a free frame restarts the run");
			for (unsigned i = 0; i < MotionAhead::LeadCapTracker::kLeadCapRun; ++i)
				t.Note(true);
			Assert::IsTrue(t.sustained, L"24 in a row: shown");
			for (unsigned i = 0; i + 1 < MotionAhead::LeadCapTracker::kLeadCapRun; ++i)
				t.Note(false);
			Assert::IsTrue(t.sustained, L"held until 24 free frames in a row");
			t.Note(false);
			Assert::IsFalse(t.sustained, L"released");

			const Decision forward = DecideForHost(Make({ Consumer::GrainMap }));
			Assert::IsTrue(forward.mode == Mode::Forward);
			Assert::AreEqual(StatusLine(forward), StatusLine(forward, 0), L"not capped: the line is unchanged");
			Assert::AreEqual(StatusLine(forward) + "; look-ahead computes " + std::to_string(MotionAhead::kMaxLead) +
				" frames ahead (the ahead-compute ceiling; more are queued)", StatusLine(forward, MotionAhead::kMaxLead),
				L"R3: at the ceiling the line is information, not a downgrade");
			const Decision idle = DecideForHost(Make({}));
			Assert::AreEqual(StatusLine(idle), StatusLine(idle, MotionAhead::kMaxLead), L"no forward look-ahead: no cap text");

			const FallbackCatalog::Decision on = FallbackCatalog::MotionLookaheadCapped(true, MotionAhead::kMaxLead);
			Assert::IsTrue(on.engaged);
			Assert::AreEqual(std::string("motion_lookahead_capped"), on.id);
			Assert::IsTrue(on.reason.find(std::to_string(MotionAhead::kMaxLead) + " frames ahead") != std::string::npos, L"the reason names the cap");
			Assert::IsTrue(on.reason.find("ahead-compute ceiling") != std::string::npos, L"and says it is the ceiling (OD 11-14)");
			const int asked120 = MotionAhead::DepthAt(4, 120.0);
			const FallbackCatalog::Decision short120 = FallbackCatalog::MotionLookaheadCapped(true, MotionAhead::kMaxLead, asked120);
			Assert::IsTrue(short120.engaged);
			Assert::IsTrue(short120.reason.find("depth asks (" + std::to_string(asked120) + ")") != std::string::npos,
				L"R3: the entry names what the consumer asked for");
			const FallbackCatalog::Decision off = FallbackCatalog::MotionLookaheadCapped(false, MotionAhead::kMaxLead);
			Assert::IsFalse(off.engaged);
			Assert::AreEqual(std::string("motion_lookahead_capped"), off.id);
		}

		TEST_METHOD(StatusLinesAreExact)
		{
			Assert::AreEqual(std::string("Vector engine: on (forward) for grain map, deband hold"),
				StatusLine(DecideForHost(Make({ Consumer::GrainMap, Consumer::DebandHold }))));

			// Batch B fold (WAIVER-B1-ENGINE-06): competitive low latency is motion-free (was "backward pairs (no frame ahead) for source
			// monitor, intent; deband hold without vectors (needs a frame ahead)"): every consumer named, with review b1-fin L1's why.
			Input in = Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent, Consumer::DebandHold });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			Assert::AreEqual(std::string("Vector engine: off; deband hold, source monitor, intent without vectors (competitive low latency "
				"(motion-free))"), StatusLine(DecideForHost(in)));

			Assert::AreEqual(std::string("Vector engine: idle (no consumer)"), StatusLine(DecideForHost(Make({}))));

			for (Consumer c : kNonSynthesis)
			{
				Input w = Make({ c });
				w.lowLatency = true;
				w.queueAheadCapacity = 0;
				const std::string s = StatusLine(DecideForHost(w));
				Assert::IsTrue(s.compare(0, 14, "Vector engine:") == 0, L"prefix");
			}
		}
		// ---- Batch B (lead18 plan-motion-engine-switch.md): the engine is ONE switch; no starter gate -----------------------
		// Replaces StarterMaskIsDejudderInterpolatorHiccupAndCameraFit: the starter set is retired (IsStarter / StarterMask removed);
		// ON serves every reuse-only want alone. The enum values stay append-only.
		TEST_METHOD(OnEngineServesEveryReuseOnlyWant)
		{
			for (int i = 0; i < kConsumerCount; ++i)
			{
				const Consumer c = ConsumerAt(i);
				const Decision d = DecideForHost(Make({ c }));
				Assert::AreEqual(Bit(c), d.wants, L"the want is recorded");
				Assert::AreEqual(0u, d.unserved, L"never refused while ON");
				if (DomainOf(c) == Domain::Synthesis)
				{
					Assert::AreEqual(0u, d.served, L"synthesis: its own policy, never in served");
					Assert::IsTrue(d.synthesisOnly && d.reason == Reason::NoConsumer, L"synthesis only");
					Assert::IsFalse(d.producer || d.estimate, L"synthesis decides for itself");
					continue;
				}
				Assert::IsTrue(d.mode == Mode::Forward && d.reason == Reason::Forward, L"forward");
				Assert::IsTrue(d.Serves(c), L"served alone (no starter needed)");
				Assert::IsTrue(d.producer, L"the producer runs for it");
				const std::string s = StatusLine(d);
				Assert::IsTrue(s.find(Name(c)) != std::string::npos, L"named in the status");
				Assert::IsTrue(s.find("starter") == std::string::npos, L"the starter wording is retired");
			}
			Assert::AreEqual(8, static_cast<int>(Reason::NoStarter), L"NoStarter kept (retired), no index moved");
			Assert::AreEqual(9, static_cast<int>(Reason::NoPairs), L"NoPairs appended");
		}

		// Replaces LoadedEngineWithoutAStarterIsNamedNotIdle (the loaded / no-starter wording is retired). Engine OFF by the setting:
		// the decision is Disabled, and OffLine names every registered want (Q2: a status line, never a Fallbacks entry).
		TEST_METHOD(EngineOffByTheSettingNamesEveryWantInItsLine)
		{
			Input in = Make({ Consumer::DejudderExtras, Consumer::GrainMap, Consumer::SourceMonitorPan });
			in.enabled = false;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::Disabled, L"disabled");
			Assert::AreEqual(0u, d.served);
			Assert::AreEqual(0u, d.unserved);
			Assert::IsFalse(d.producer || d.estimate, L"no motion work with the engine off");
			Assert::AreEqual(Bit(Consumer::DejudderExtras) | Bit(Consumer::GrainMap) | Bit(Consumer::SourceMonitorPan), d.wants);
			Assert::AreEqual(std::string("motion engine off (setting): dejudder extras, grain map, source monitor use their non-motion path"),
				OffLine(d));
			Assert::AreEqual(std::string(), StatusLine(d), L"the vector engine's own line is empty; OffLine is the line");
			Assert::IsFalse(FallbackCatalog::VectorEngine(d).engaged, L"Q2: not a Fallbacks entry");
			EngineFacts f;
			f.vectorDemand = false;
			f.decision = d;
			const MotionEngineStatus st = DeriveEngineStatus(f);
			Assert::IsTrue(st.state == MotionEngineState::Off && st.reason == MotionEngineReason::VectorDemandOff);
			Assert::AreEqual(0u, st.affected, L"nothing affected");
			Assert::IsFalse(FallbackCatalog::MotionEngine(st).engaged, L"Q2: the motion engine entry is released");

			Input none = Make({});
			none.enabled = false;
			Assert::AreEqual(std::string(), OffLine(DecideForHost(none)), L"no want: no line");

			for (Consumer c : { Consumer::SourceMonitorPan, Consumer::DetailPulse, Consumer::OcclusionMask })
			{
				Input on = Make({ c });
				const std::string s = StatusLine(DecideForHost(on));
				Assert::IsTrue(s.find("starter") == std::string::npos && s.find("loaded") == std::string::npos, L"ON: no starter / loaded wording");
				on.enabled = false;
				Assert::IsTrue(OffLine(DecideForHost(on)).find(Name(c)) != std::string::npos, L"OFF: the consumer is named");
			}
		}

		// Replaces ReuseOnlyConsumersWithoutAStarterAreRefusedVisibly: ON serves them; the refusal (each consumer named, visible
		// entry) moves to the hard absences that remain: interlaced and exclusive smooth motion.
		TEST_METHOD(ReuseOnlyConsumersAreServedAndHardAbsencesNameEachOne)
		{
			for (Consumer c : kNonSynthesis)
			{
				Input in = Make({ c });
				const Decision on = DecideForHost(in);
				Assert::IsTrue(on.mode == Mode::Forward && on.Serves(c) && on.unserved == 0u && on.producer, L"ON: served");
				Assert::IsFalse(FallbackCatalog::VectorEngine(on).engaged, L"ON: nothing unserved");

				Input il = in;
				il.interlaced = true;
				const Decision i = DecideForHost(il);
				Assert::IsTrue(i.mode == Mode::Off && i.reason == Reason::Interlaced, L"interlaced: off");
				Assert::AreEqual(0u, i.served);
				Assert::AreEqual(Bit(c), i.unserved, L"interlaced: unserved");
				Assert::IsFalse(i.estimate || i.producer, L"interlaced: nothing starts");
				const FallbackCatalog::Decision fi = FallbackCatalog::VectorEngine(i);
				Assert::IsTrue(fi.engaged, L"interlaced: visible");
				Assert::AreEqual(std::string(Name(c)) + " without vectors (interlaced)", fi.reason, L"interlaced: named with why");

				Input sm = in;
				sm.smoothMotion = true;
				const Decision x = DecideForHost(sm);
				Assert::IsTrue(x.mode == Mode::Off && x.reason == Reason::SmoothMotionExclusive, L"exclusive smooth motion: off");
				Assert::AreEqual(Bit(c), x.unserved, L"exclusive smooth motion: unserved");
				Assert::IsFalse(x.estimate || x.producer, L"exclusive smooth motion: nothing starts");
				const FallbackCatalog::Decision fx = FallbackCatalog::VectorEngine(x);
				Assert::IsTrue(fx.engaged, L"exclusive smooth motion: visible");
				Assert::AreEqual(std::string(Name(c)) + " without vectors (libplacebo smooth motion owns output)", fx.reason,
					L"exclusive smooth motion: named with why");
			}
			// Batch B fold (WAIVER-B1-ENGINE-06): competitive low latency is a hard absence for every consumer (was: the statistics
			// consumer served by a backward producer): off, nothing served, each named with why.
			Input low = Make({ Consumer::SourceMonitorPan, Consumer::GrainMap });
			low.lowLatency = true;
			low.queueAheadCapacity = 0;
			const Decision d = DecideForHost(low);
			Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::BackwardDepth0, L"competitive low latency: off, motion-free");
			Assert::AreEqual(0u, d.served, L"competitive: nothing served");
			Assert::AreEqual(Bit(Consumer::SourceMonitorPan) | Bit(Consumer::GrainMap), d.unserved, L"competitive: both fall back visibly");
			Assert::IsFalse(d.producer, L"competitive: no producer");
			const FallbackCatalog::Decision fl = FallbackCatalog::VectorEngine(d);
			Assert::IsTrue(fl.engaged, L"competitive: visible");
			Assert::AreEqual(std::string("grain map, source monitor without vectors (competitive low latency (motion-free))"), fl.reason,
				L"competitive: each named with why");
		}

		// Replaces TheOcclusionMaskWithoutAStarterIsARefusedStart: the mask is synthesis-domain (never in served) and is never refused
		// while ON (DejudderPolicy's occlusionMaskServed is gone: the mask alone starts its own estimate under its own policy).
		TEST_METHOD(TheOcclusionMaskIsNeverRefusedWhileOn)
		{
			const unsigned mask = Bit(Consumer::OcclusionMask);
			const Decision a = DecideForHost(Make({ Consumer::OcclusionMask }));
			Assert::IsTrue(a.mode == Mode::Off && a.reason == Reason::NoConsumer && a.synthesisOnly, L"mask alone: its own policy decides");
			Assert::AreEqual(mask, a.wants, L"mask alone: the want is recorded");
			Assert::AreEqual(0u, a.served);
			Assert::AreEqual(0u, a.unserved, L"mask alone: not unserved");
			Assert::IsFalse(a.estimate || a.producer, L"synthesis: the engine starts nothing for it here");
			const Decision b = DecideForHost(Make({ Consumer::OcclusionMask, Consumer::GrainMap, Consumer::DetailPulse }));
			Assert::AreEqual(Bit(Consumer::GrainMap) | Bit(Consumer::DetailPulse), b.served, L"mask + reuse: the reuse consumers are served");
			Assert::AreEqual(0u, b.unserved, L"mask + reuse: nothing unserved");
			Assert::AreEqual(std::string(), UnservedText(b));
			Assert::IsTrue(b.producer && !b.synthesisOnly, L"mask + reuse: the producer runs");
		}

		TEST_METHOD(TheCameraFitIsServedWithTheReuseOnlyConsumers)
		{
			const Decision d = DecideForHost(Make({ Consumer::CameraFit, Consumer::DetailPulse, Consumer::SourceMonitorPan }));
			Assert::IsTrue(d.mode == Mode::Forward && d.reason == Reason::Forward);
			Assert::IsTrue(d.Serves(Consumer::CameraFit) && d.Serves(Consumer::DetailPulse) && d.Serves(Consumer::SourceMonitorPan));
			Assert::AreEqual(0u, d.unserved);
			Assert::IsTrue(d.estimate && d.producer, L"the camera fit's own matcher field and the producer");
			Assert::IsFalse(d.matcherFallback, L"pairs expected: the camera fit's estimate is its own field, not the fallback");
		}

		// W2-C1 MUST 3, batch B form: the reuse-only consumers are SERVED whether or not the de-judder runs (no starter).
		TEST_METHOD(WithOrWithoutTheDejudderTheReuseOnlyConsumersAreServed)
		{
			const unsigned reuse = Bit(Consumer::SigmaDelta) | Bit(Consumer::DetailPulse) | Bit(Consumer::DebandHold) |
				Bit(Consumer::SourceMonitorPan);
			const Decision with = DecideForHost(Make({ Consumer::DejudderExtras, Consumer::SigmaDelta, Consumer::DetailPulse, Consumer::DebandHold,
				Consumer::SourceMonitorPan }));
			Assert::AreEqual(reuse, with.served, L"with the de-judder: served");
			Assert::AreEqual(0u, with.unserved);
			const Decision without = DecideForHost(Make({ Consumer::SigmaDelta, Consumer::DetailPulse, Consumer::DebandHold, Consumer::SourceMonitorPan }));
			Assert::AreEqual(reuse, without.served, L"without the de-judder: the same consumers are served");
			Assert::AreEqual(0u, without.unserved);
			Assert::IsTrue(with.reason == Reason::Forward && without.reason == Reason::Forward);
			Assert::IsTrue(with.producer && without.producer);
		}

		TEST_METHOD(SynthesisWantsAreRecordedButNeverServedHere)
		{
			const Decision d = DecideForHost(Make({ Consumer::DejudderExtras, Consumer::HiccupRepair, Consumer::GrainMap }));
			Assert::AreEqual(Bit(Consumer::DejudderExtras) | Bit(Consumer::HiccupRepair) | Bit(Consumer::GrainMap), d.wants, L"every want recorded");
			Assert::AreEqual(Bit(Consumer::GrainMap), d.served, L"synthesis never in served");
			Assert::IsTrue(d.producer, L"batch B: the producer runs for the served grain map");
			Assert::IsFalse(d.estimate, L"pairs expected: no render-time estimate (synthesis runs through its own policies)");
		}

		// Batch B Q1 + review M3: the hybrid scaler's want registers as ResidualStabilise (not the grain map): served exactly like the
		// grain map (the same source-field domain), named as itself in Ctrl+I and in the OFF line.
		TEST_METHOD(TheHybridScalerWantIsResidualStabiliseNotTheGrainMap)
		{
			Assert::IsTrue(DomainOf(Consumer::ResidualStabilise) == DomainOf(Consumer::GrainMap), L"served identically");
			Assert::AreEqual(std::string("residual stabilise"), std::string(Name(Consumer::ResidualStabilise)));
			const Decision d = DecideForHost(Make({ Consumer::ResidualStabilise }));
			Assert::AreEqual(Bit(Consumer::ResidualStabilise), d.served);
			Assert::AreEqual(std::string("Vector engine: on (forward) for residual stabilise"), StatusLine(d), L"not called the grain map");
			Input off = Make({ Consumer::ResidualStabilise });
			off.enabled = false;
			Assert::AreEqual(std::string("motion engine off (setting): residual stabilise use their non-motion path"), OffLine(DecideForHost(off)));
		}

		// ---- Batch B: no optical-flow pairs expected (optical flow off, NVOF start failed, producer recovering) ---------------
		TEST_METHOD(WithoutPairsTheFieldConsumersUseTheMatcherFallback)
		{
			Input in = Make({ Consumer::GrainMap, Consumer::DebandHold });
			in.pairsExpected = false;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Forward && d.reason == Reason::Forward, L"forward");
			Assert::AreEqual(Bit(Consumer::GrainMap) | Bit(Consumer::DebandHold), d.served, L"served by the matcher");
			Assert::AreEqual(0u, d.unserved);
			Assert::AreEqual(0u, d.noPairs);
			Assert::IsTrue(d.estimate && d.matcherFallback, L"the render-time matcher estimate is the fallback");
			Assert::AreEqual(std::string("Vector engine: on (forward; matcher fallback, no optical-flow pairs) for grain map, deband hold"),
				StatusLine(d), L"the fallback is named");
			Assert::IsFalse(FallbackCatalog::VectorEngine(d).engaged, L"served (by the fallback): no vector_engine entry");

			in.pairsExpected = true;
			const Decision p = DecideForHost(in);
			Assert::IsFalse(p.estimate || p.matcherFallback, L"pairs expected: the matcher costs nothing");
			Assert::AreEqual(std::string("Vector engine: on (forward) for grain map, deband hold"), StatusLine(p));

			Input dither = Make({ Consumer::SigmaDelta });
			dither.pairsExpected = false;
			const Decision o = DecideForHost(dither);
			Assert::IsTrue(o.Serves(Consumer::SigmaDelta) && o.estimate && o.matcherFallback, L"the output consumer uses the fallback too");

			Input camera = Make({ Consumer::CameraFit });
			camera.pairsExpected = false;
			const Decision c = DecideForHost(camera);
			Assert::IsTrue(c.estimate, L"the camera fit's own field");
			Assert::IsFalse(c.matcherFallback, L"the camera fit alone is not the fallback");
		}

		TEST_METHOD(WithoutPairsTheStatisticsConsumersAreNamedUnserved)
		{
			for (Consumer c : { Consumer::SourceMonitorPan, Consumer::MotionIntent })
			{
				Input in = Make({ c });
				in.pairsExpected = false;
				const Decision d = DecideForHost(in);
				Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::NoPairs, L"statistics alone: NoPairs");
				Assert::AreEqual(0u, d.served);
				Assert::AreEqual(Bit(c), d.unserved, L"unserved");
				Assert::AreEqual(Bit(c), d.noPairs, L"for want of pairs");
				Assert::IsFalse(d.estimate || d.producer || d.matcherFallback, L"the matcher has no statistics form");
				const std::string why = std::string(Name(c)) + " without vectors (no optical-flow pairs (optical flow off or not running))";
				Assert::AreEqual(why, UnservedText(d), L"named with why");
				Assert::AreEqual("Vector engine: off; " + why, StatusLine(d));
				const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
				Assert::IsTrue(f.engaged, L"not deliberate: a visible fallback");
				Assert::AreEqual(why, f.reason);
			}
			Input mixed = Make({ Consumer::SourceMonitorPan, Consumer::GrainMap });
			mixed.pairsExpected = false;
			const Decision m = DecideForHost(mixed);
			Assert::IsTrue(m.mode == Mode::Forward && m.reason == Reason::Forward);
			Assert::AreEqual(Bit(Consumer::GrainMap), m.served);
			Assert::AreEqual(Bit(Consumer::SourceMonitorPan), m.unserved);
			Assert::AreEqual(Bit(Consumer::SourceMonitorPan), m.noPairs);
			Assert::IsTrue(m.estimate && m.matcherFallback && m.producer);
			Assert::AreEqual(std::string("Vector engine: on (forward; matcher fallback, no optical-flow pairs) for grain map; "
				"source monitor without vectors (no optical-flow pairs (optical flow off or not running))"), StatusLine(m));
		}

		// Review M4: no pairs because of a SETTING (optical flow off / motion_ahead_frames 0) is deliberate: the statistics consumers stay
		// named in Ctrl+I (StatusLine / UnservedText) but are not a vector_engine Fallbacks entry. Any other unserved cause still is one.
		TEST_METHOD(ADeliberateNoPairsIsNamedButIsNotAFallbacksEntry)
		{
			Input in = Make({ Consumer::SourceMonitorPan });
			in.pairsExpected = false;
			in.pairsDeliberatelyOff = true;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.pairsDeliberatelyOff, L"copied to the decision");
			Assert::AreEqual(Bit(Consumer::SourceMonitorPan), d.noPairs);
			Assert::IsTrue(UnservedText(d).find("source monitor without vectors (no optical-flow pairs") != std::string::npos, L"still named");
			Assert::IsTrue(StatusLine(d).find("source monitor") != std::string::npos, L"still in the Ctrl+I line");
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
			Assert::IsFalse(f.engaged, L"deliberate: not an entry");
			Assert::AreEqual(std::string("vector_engine"), f.id);

			Input fault = in;
			fault.pairsDeliberatelyOff = false;
			Assert::IsTrue(FallbackCatalog::VectorEngine(DecideForHost(fault)).engaged, L"a producer fault stays an entry");

			Input other = Make({ Consumer::SourceMonitorPan, Consumer::SigmaDelta, Consumer::GrainMap });
			other.pairsExpected = false;
			other.pairsDeliberatelyOff = true;
			other.smoothMotion = true;
			const FallbackCatalog::Decision o = FallbackCatalog::VectorEngine(DecideForHost(other));
			Assert::IsTrue(o.engaged, L"another unserved cause still engages");
			// Batch B fold (WAIVER-B1-ENGINE-06): smooth motion owns output exclusively, so it names every consumer (was: the output
			// dither alone, "output dither without vectors (...)", the retired analysis-under-smooth-motion semantics).
			Assert::AreEqual(std::string("output dither, grain map, source monitor without vectors (libplacebo smooth motion owns output)"),
				o.reason);
			// The deliberate no-pairs beside another cause in ONE decision (competitive low latency): the deliberate statistics consumer
			// is left out of the entry's trigger, the competitive ones still engage it, and the text names both causes.
			Input both = Make({ Consumer::SourceMonitorPan, Consumer::SigmaDelta, Consumer::GrainMap });
			both.pairsExpected = false;
			both.pairsDeliberatelyOff = true;
			both.lowLatency = true;
			both.queueAheadCapacity = 0;
			const Decision bd = DecideForHost(both);
			Assert::AreEqual(Bit(Consumer::SourceMonitorPan), bd.noPairs, L"the deliberate no-pairs");
			const FallbackCatalog::Decision ob = FallbackCatalog::VectorEngine(bd);
			Assert::IsTrue(ob.engaged, L"competitive beside a deliberate no-pairs: still an entry");
			Assert::AreEqual(std::string("output dither, grain map without vectors (competitive low latency (motion-free)); source monitor "
				"without vectors (no optical-flow pairs (optical flow off or not running))"), ob.reason);

			Decision handBuilt;
			handBuilt.unserved = handBuilt.noPairs = Bit(Consumer::MotionIntent);
			Assert::IsFalse(handBuilt.pairsDeliberatelyOff, L"a hand-built decision defaults to not deliberate");
			Assert::IsTrue(FallbackCatalog::VectorEngine(handBuilt).engaged, L"so it stays an entry");
		}

		TEST_METHOD(EngineStatusMapsEachReason)
		{
			EngineFacts f;
			f.decision = DecideForHost(Make({ Consumer::DejudderExtras, Consumer::GrainMap }));
			f.producerRunning = true;
			f.frame = 7;
			MotionEngineStatus st = DeriveEngineStatus(f);
			Assert::IsTrue(st.state == MotionEngineState::On && st.reason == MotionEngineReason::None && st.reasons == 0u, L"on");
			Assert::AreEqual(7ull, static_cast<unsigned long long>(st.frame));
			Assert::AreEqual(0u, st.affected, L"on: nothing affected");

			struct Row { void (*set)(EngineFacts&); MotionEngineState state; MotionEngineReason reason; };
			const Row rows[] = {
				{ [](EngineFacts& x) { x.opticalFlow = false; }, MotionEngineState::Degraded, MotionEngineReason::OpticalFlowOff },
				{ [](EngineFacts& x) { x.nvofStartError = true; x.producerRunning = false; }, MotionEngineState::Degraded, MotionEngineReason::NvofUnavailable },
				{ [](EngineFacts& x) { x.producerRecovering = true; }, MotionEngineState::Degraded, MotionEngineReason::ProducerRecovering },
				{ [](EngineFacts& x) { x.resyncWindow = true; }, MotionEngineState::Degraded, MotionEngineReason::Resync },
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
				Assert::IsFalse(st.Has(MotionEngineReason::NoStarter), L"batch B: NoStarter is never derived");
				Assert::IsTrue(std::string(ReasonWords(r.reason)).size() > 0, L"words");
			}
			// Batch B: nothing wanted (the old NoStarter row's place): Off, no reason, nothing affected (information only, Q1).
			EngineFacts idle = f;
			idle.decision = DecideForHost(Make({}));
			st = DeriveEngineStatus(idle);
			Assert::IsTrue(st.state == MotionEngineState::Off && st.reason == MotionEngineReason::None, L"nothing wanted: off, no reason");
			Assert::AreEqual(0u, st.reasons);
			Assert::AreEqual(0u, st.affected);
			Assert::IsFalse(FallbackCatalog::MotionEngine(st).engaged, L"nothing wanted: released");
			EngineFacts shallow = f;
			Input ll = Make({ Consumer::DejudderExtras, Consumer::GrainMap });
			ll.lowLatency = true;
			ll.queueAheadCapacity = 1;
			shallow.decision = DecideForHost(ll);
			st = DeriveEngineStatus(shallow);
			Assert::IsTrue(st.state == MotionEngineState::Degraded && st.reason == MotionEngineReason::LowLatency, L"ForwardShallow = degraded");
			EngineFacts inter = f;
			Input il = Make({ Consumer::GrainMap });
			il.interlaced = true;
			inter.decision = DecideForHost(il);
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
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Off && d.reason == Reason::Disabled && d.served == 0 && d.unserved == 0 &&
				!d.estimate && !d.producer && !d.lowLatencyAllowed);
		}

		// R3 (audit S5): the composed cap state as the vector engine reads it, frame by frame. 60p with the standard queue (depth 4,
		// 14 queued ahead): the designed state, the info text only, never the entry. 120p (depth 8): the entry, naming the 8 asked.
		TEST_METHOD(LeadCapStateShowsInfoAtTheCeilingAndTheEntryOnlyWhenShort)
		{
			const unsigned run = MotionAhead::LeadCapTracker::kLeadCapRun;
			const int depth60 = MotionAhead::DepthAt(4, 60.0);
			MotionAhead::LeadCapState s60;
			for (unsigned i = 0; i < run; ++i)
				s60.Note(depth60, 14);
			Assert::IsTrue(s60.InfoShown(true), L"60p / 250 ms: the ceiling binds sustained: the info text");
			Assert::IsFalse(s60.EntryShown(true), L"60p / 250 ms: the depth is served in full: no fallback entry");
			Assert::IsFalse(s60.InfoShown(false) || s60.EntryShown(false), L"not forward: neither");
			Assert::AreEqual(0, s60.asked, L"nothing asked above the ceiling");

			const int depth120 = MotionAhead::DepthAt(4, 120.0);
			MotionAhead::LeadCapState s120;
			for (unsigned i = 0; i + 1 < run; ++i)
				s120.Note(depth120, 20);
			Assert::IsFalse(s120.EntryShown(true), L"not yet sustained");
			s120.Note(depth120, 20);
			Assert::IsTrue(s120.EntryShown(true) && s120.InfoShown(true), L"120p: the entry (and the info text)");
			Assert::AreEqual(depth120, s120.asked, L"the depth asked is kept for the entry");
			const FallbackCatalog::Decision e = FallbackCatalog::MotionLookaheadCapped(s120.EntryShown(true), MotionAhead::kMaxLead, s120.asked);
			Assert::IsTrue(e.engaged && e.reason.find("(" + std::to_string(depth120) + ")") != std::string::npos, L"the entry names 8");

			// Back to 60p: the entry is released only after a sustained free run (no toggling with the queue level).
			for (unsigned i = 0; i + 1 < run; ++i)
				s120.Note(depth60, 14);
			Assert::IsTrue(s120.EntryShown(true), L"held until a full free run");
			s120.Note(depth60, 14);
			Assert::IsFalse(s120.EntryShown(true), L"released");
			Assert::IsTrue(s120.InfoShown(true), L"the info text stays while the ceiling binds");
		}
	};

	// WAIVER-B1-ENGINE-05 (review-b1-fin M1): the decision the engine HOST acts on, VectorDemand::DecideForHost (VectorEngine::
	// UpdateVectorEngine); since the batch B fold (WAIVER-B1-ENGINE-06) it is the only decision.
	// Plan (b) "Hard Off: SmoothMotion, LowLatency (competitive)" and (d4) "smooth motion / competitive = Off with the reason"; owner
	// 2026-11-12 18:47 (smooth motion on: VP's engine stays off, the dither uses its own visible fallback search); doc 91 C / register
	// 115 G6 "keep Competitive motion-free" (WAIVER-C1-08). The engine facts mirror the host: lowLatencyNoVectors = lowLatency &&
	// !decision.lowLatencyAllowed (VectorEngine.cpp:95).
	TEST_CLASS(VectorDemandHostTests)
	{
	public:
		TEST_METHOD(SmoothMotionIsOffWithTheReasonAndNamesEveryConsumer)
		{
			Input in = Make({ Consumer::DejudderExtras, Consumer::GrainMap, Consumer::MotionIntent, Consumer::SigmaDelta });
			in.smoothMotion = true;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Off, L"off");
			Assert::IsTrue(d.reason == Reason::SmoothMotionExclusive, L"smooth motion owns output exclusively");
			Assert::AreEqual(0u, d.served, L"served 0");
			Assert::AreEqual(Bit(Consumer::GrainMap) | Bit(Consumer::MotionIntent) | Bit(Consumer::SigmaDelta), d.unserved,
				L"every non-synthesis want unserved (synthesis never in served / unserved)");
			Assert::IsFalse(d.producer, L"no producer");
			Assert::IsFalse(d.estimate, L"no render-time estimate");
			Assert::IsFalse(d.lowLatencyAllowed, L"not a low-latency path");
			const std::string why = "output dither, grain map, intent without vectors (libplacebo smooth motion owns output)";
			Assert::AreEqual(why, UnservedText(d), L"Ctrl+I names each consumer with why");
			Assert::AreEqual("Vector engine: off; " + why, StatusLine(d));
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
			Assert::IsTrue(f.engaged, L"a visible Fallbacks entry");
			Assert::AreEqual(why, f.reason);

			EngineFacts facts;
			facts.decision = d;
			facts.producerRunning = false;
			const MotionEngineStatus st = DeriveEngineStatus(facts);
			Assert::IsTrue(st.state == MotionEngineState::Off, L"hard off");
			Assert::IsTrue(st.reason == MotionEngineReason::SmoothMotion, L"reason SmoothMotion");
			Assert::AreEqual(d.unserved, st.affected, L"the unserved consumers are affected");
			const FallbackCatalog::Decision m = FallbackCatalog::MotionEngine(st);
			Assert::IsTrue(m.engaged, L"the motion engine entry is engaged");
			Assert::AreEqual(std::string("motion engine off: libplacebo smooth motion owns output; output dither, grain map, intent take their "
				"non-motion path"), m.reason);

			for (Consumer c : kNonSynthesis)
			{
				Input one = Make({ c });
				one.smoothMotion = true;
				const Decision x = DecideForHost(one);
				Assert::IsTrue(x.mode == Mode::Off && x.reason == Reason::SmoothMotionExclusive, L"each alone: off, exclusive");
				Assert::AreEqual(0u, x.served);
				Assert::AreEqual(Bit(c), x.unserved);
				Assert::IsFalse(x.producer || x.estimate || x.lowLatencyAllowed, L"each alone: nothing starts");
				Assert::AreEqual(std::string(Name(c)) + " without vectors (libplacebo smooth motion owns output)", UnservedText(x));
			}
		}

		TEST_METHOD(CompetitiveLowLatencyIsMotionFreeOffWithTheReason)
		{
			Input in = Make({ Consumer::SourceMonitorPan, Consumer::MotionIntent, Consumer::DebandHold });
			in.lowLatency = true;
			in.queueAheadCapacity = 0;
			const Decision d = DecideForHost(in);
			Assert::IsTrue(d.mode == Mode::Off, L"competitive: off (no backward pair)");
			Assert::AreEqual(0u, d.served, L"served 0");
			Assert::AreEqual(Bit(Consumer::SourceMonitorPan) | Bit(Consumer::MotionIntent) | Bit(Consumer::DebandHold), d.unserved,
				L"every consumer goes without vectors, the statistics consumers too");
			Assert::IsFalse(d.producer, L"no producer");
			Assert::IsFalse(d.estimate, L"no estimate");
			Assert::IsFalse(d.lowLatencyAllowed, L"lowLatencyAllowed false: the host sets lowLatencyNoVectors");
			// The line is Off and names every consumer as without vectors. Batch B fold (WAIVER-B1-ENGINE-06, review b1-fin L1): the
			// "why" is pinned too, competitive low latency (motion-free).
			const std::string s = StatusLine(d);
			Assert::IsTrue(s.compare(0, 20, "Vector engine: off; ") == 0, (L"off line: " + std::wstring(s.begin(), s.end())).c_str());
			Assert::IsTrue(s.find("deband hold, source monitor, intent without vectors (") != std::string::npos, L"each named without vectors");
			Assert::IsTrue(s.find("backward") == std::string::npos, L"no backward-pair wording");
			Assert::AreEqual(std::string("Vector engine: off; deband hold, source monitor, intent without vectors (competitive low latency "
				"(motion-free))"), s, L"the competitive why");
			const FallbackCatalog::Decision f = FallbackCatalog::VectorEngine(d);
			Assert::IsTrue(f.engaged, L"a visible Fallbacks entry");
			Assert::AreEqual(UnservedText(d), f.reason);

			EngineFacts facts;
			facts.decision = d;
			facts.lowLatency = in.lowLatency;
			facts.lowLatencyNoVectors = in.lowLatency && !d.lowLatencyAllowed;   // VectorEngine.cpp:95
			const MotionEngineStatus st = DeriveEngineStatus(facts);
			Assert::IsTrue(facts.lowLatencyNoVectors, L"the host's low-latency-without-vectors fact holds");
			Assert::IsTrue(st.state == MotionEngineState::Off, L"hard off");
			Assert::IsTrue(st.reason == MotionEngineReason::LowLatency, L"reason LowLatency");
			Assert::AreEqual(d.unserved, st.affected, L"every consumer affected");
			const FallbackCatalog::Decision m = FallbackCatalog::MotionEngine(st);
			Assert::IsTrue(m.engaged, L"the motion engine entry is engaged");
			Assert::AreEqual(std::string("motion engine off: low latency; deband hold, source monitor, intent take their non-motion path"), m.reason);

			for (Consumer c : kNonSynthesis)
			{
				Input one = Make({ c });
				one.lowLatency = true;
				one.queueAheadCapacity = 0;
				const Decision x = DecideForHost(one);
				Assert::IsTrue(x.mode == Mode::Off, L"each alone: off");
				Assert::AreEqual(0u, x.served);
				Assert::AreEqual(Bit(c), x.unserved);
				Assert::IsFalse(x.producer || x.estimate || x.lowLatencyAllowed, L"each alone: motion-free");
				Assert::IsTrue(UnservedText(x).compare(0, std::string(Name(c)).size() + 18, std::string(Name(c)) + " without vectors (") == 0,
					L"each alone: named without vectors");
				Assert::AreEqual(std::string(Name(c)) + " without vectors (competitive low latency (motion-free))", UnservedText(x),
					L"each alone: the competitive why");
			}
		}

		// Batch B fold (WAIVER-B1-ENGINE-06; was GamesAndOrdinaryInputsDecideAsDecide, "identical to Decide()"): the retired Decide()
		// is gone, so the decisions it shared with the host are pinned field for field (Same) against the expected Decision, built
		// from the rules: games (capacity >= 1, ForwardShallow) and every input without smooth motion and outside competitive keep
		// exactly the decisions the host made before the fold; only those two cases are Off with the reason (the tests above).
		TEST_METHOD(GamesAndOrdinaryInputsKeepTheirDecisions)
		{
			for (int capacity = 1; capacity <= 8; ++capacity)
			{
				Input in = Make({ Consumer::GrainMap, Consumer::DebandHold, Consumer::SourceMonitorPan });
				in.lowLatency = true;
				in.queueAheadCapacity = capacity;
				const Decision h = DecideForHost(in);
				Decision games;   // forward from the frames the games queue already holds; pairs expected: no render-time estimate
				games.mode = Mode::Forward;
				games.reason = Reason::ForwardShallow;
				games.served = Bit(Consumer::GrainMap) | Bit(Consumer::DebandHold) | Bit(Consumer::SourceMonitorPan);
				games.producer = true;
				games.lowLatencyAllowed = true;
				Assert::IsTrue(Same(h, games), L"games: the forward-shallow decision, field for field");
				Assert::IsTrue(h.mode == Mode::Forward && h.reason == Reason::ForwardShallow && h.lowLatencyAllowed, L"games: forward shallow");
				Assert::IsTrue(h.producer, L"games: the producer runs");
			}
			for (int i = 0; i < kConsumerCount; ++i)
			{
				const Consumer c = ConsumerAt(i);
				const unsigned wanted = (DomainOf(c) == Domain::Synthesis ? 0u : Bit(c)) | Bit(Consumer::DetailPulse);
				const unsigned stats = DomainOf(c) == Domain::SourceStatistics ? Bit(c) : 0u;
				Input in = Make({ c, Consumer::DetailPulse });
				Decision ordinary;   // forward, every non-synthesis want served; the render-time matcher only for the camera fit's field
				ordinary.mode = Mode::Forward;
				ordinary.reason = Reason::Forward;
				ordinary.served = wanted;
				ordinary.producer = true;
				ordinary.estimate = c == Consumer::CameraFit;
				Assert::IsTrue(Same(DecideForHost(in), ordinary), L"ordinary: forward, every want served");
				Input noPairs = in;
				noPairs.pairsExpected = false;
				Decision fallback;   // the matcher fallback serves the field consumers; a statistics consumer is named without pairs
				fallback.mode = Mode::Forward;
				fallback.reason = Reason::Forward;
				fallback.served = wanted & ~stats;
				fallback.unserved = stats;
				fallback.noPairs = stats;
				fallback.producer = true;
				fallback.estimate = true;
				fallback.matcherFallback = true;
				Assert::IsTrue(Same(DecideForHost(noPairs), fallback), L"no pairs: the matcher fallback");
				Input il = in;
				il.interlaced = true;
				Decision interlaced;   // off, every non-synthesis want unserved
				interlaced.reason = Reason::Interlaced;
				interlaced.unserved = wanted;
				Assert::IsTrue(Same(DecideForHost(il), interlaced), L"interlaced: off, every want unserved");
				Input off = in;
				off.enabled = false;
				Assert::IsTrue(Same(DecideForHost(off), Decision()), L"disabled: the default decision");
			}
		}
	};
}
