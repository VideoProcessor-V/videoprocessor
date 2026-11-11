// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-0 identity record: VpMotion::ProducerPlan against a TEST-ONLY copy of the producer lifecycle and
// depth decision as it ran inline in LibplaceboVideoRenderer::RenderLocked before P5a (cda5794e, the block
// "docs/VP-nvof-ahead.md (owner 2026-10-16): the optical-flow depth for this frame"). The copy below keeps
// the old expression order and its side-effect order. Pure CPU, no GPU. The input space is enumerated, not
// sampled: every boolean input, motion_ahead_frames 0/1/2/4, the source rate classes and the queue depths.
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/input/queue_timing/PulldownDetector.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/ProducerPlan.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion::ProducerPlan;

namespace ProducerPlanTest
{
	namespace
	{
		// The producer-side state the old block read and wrote, as plain data.
		struct OldState
		{
			bool exists = false, running = false, failedLogged = false, engaged = false, engineReady = false;
			bool startWasTried = false;
			bool stopWasCalled = false;
			bool stoppedLogged = false;   // "the producer stopped": the death note
			int depthStored = -1, configuredStored = -1;
			bool backwardStored = false;
			MotionAhead::Decision ahead;
		};

		// ---- the OLD inline logic, verbatim in expression and order -------------------------------------
		// startSucceeds: what producer->Start() returned when the old block tried it.
		OldState OldLogic(const LifecycleInput& l, const DepthInput& d, OldState s, bool startSucceeds, bool dejudderEstimating,
			bool producerEngineReady)
		{
			const bool lowLatency = l.lowLatency;
			const bool vectorProducer = l.vectorProducer && l.opticalFlow && l.motionAheadFrames > 0 &&
				(!lowLatency || l.vectorLowLatencyAllowed);
			const bool wantProducer = (l.dejudderPresent && l.opticalFlow && l.motionAheadFrames > 0 && !lowLatency) || vectorProducer;
			if (wantProducer && !s.exists && !s.failedLogged && l.gpuAvailable)
			{
				s.startWasTried = true;
				if (startSucceeds)
				{
					s.exists = true;
					s.running = true;
				}
				else
					s.failedLogged = true;
			}
			else if (!wantProducer)
			{
				const bool onlyReleased = !l.dejudderPresent && l.opticalFlow && l.motionAheadFrames > 0 && !lowLatency;
				const bool keepBriefly = s.exists && s.running && onlyReleased;
				if (s.exists && !keepBriefly)
				{
					s.stopWasCalled = true;
					s.exists = false;
					s.running = false;
					s.engineReady = false;
				}
				if (!keepBriefly)
					s.failedLogged = false;
				s.engaged = false;
			}
			if (s.exists && !s.running && !s.failedLogged)
			{
				s.failedLogged = true;
				s.stoppedLogged = true;
			}
			s.engineReady = s.exists && s.running && producerEngineReady;
			const bool motionConsumer = dejudderEstimating || d.vectorProducer;
			s.ahead = MotionAhead::Decide(
				MotionAhead::DepthAt(d.motionAheadFrames, d.sourceHz, d.generalRateFixes),
				d.lowLatency && (!d.vectorLowLatencyAllowed || d.vectorBackward),
				s.exists && s.running,
				d.queuedFutureFrames, d.opticalFlow, motionConsumer,
				d.vectorBackward);
			s.depthStored = s.exists && s.running ? s.ahead.depth : 0;
			s.backwardStored = s.exists && s.running && s.ahead.reason == MotionAhead::Reason::BackwardPairs;
			s.configuredStored = (std::max)(1, s.ahead.configured);
			if (!(s.exists && s.running) || s.ahead.depth == 0)
				s.engaged = false;
			return s;
		}

		// ---- the NEW path: plan, then the executor the extension runs ------------------------------------
		OldState NewLogic(const LifecycleInput& l, DepthInput d, OldState s, bool startSucceeds, bool dejudderEstimating,
			bool producerEngineReady)
		{
			const LifecycleOutput lo = Lifecycle(l);
			if (lo.action == Action::Start)
			{
				s.startWasTried = true;
				if (startSucceeds)
				{
					s.exists = true;
					s.running = true;
				}
				else
					s.failedLogged = true;
			}
			else if (lo.action == Action::Stop)
			{
				s.stopWasCalled = true;
				s.exists = false;
				s.running = false;
				s.engineReady = false;
			}
			if (lo.clearStartFailed)
				s.failedLogged = false;
			if (lo.disengage)
				s.engaged = false;
			if (s.exists && !s.running && !s.failedLogged)
			{
				s.failedLogged = true;
				s.stoppedLogged = true;
			}
			s.engineReady = s.exists && s.running && producerEngineReady;
			d.producerRunning = s.exists && s.running;
			d.dejudderEstimating = dejudderEstimating;
			const DepthOutput dout = Depth(d);
			s.ahead = dout.ahead;
			s.depthStored = dout.storedDepth;
			s.backwardStored = dout.storedBackward;
			s.configuredStored = dout.storedConfigured;
			if (dout.disengage)
				s.engaged = false;
			return s;
		}

		bool Same(const OldState& a, const OldState& b)
		{
			return a.exists == b.exists && a.running == b.running && a.failedLogged == b.failedLogged && a.engaged == b.engaged &&
				a.engineReady == b.engineReady && a.startWasTried == b.startWasTried && a.stopWasCalled == b.stopWasCalled &&
				a.stoppedLogged == b.stoppedLogged && a.depthStored == b.depthStored && a.configuredStored == b.configuredStored &&
				a.backwardStored == b.backwardStored && a.ahead.configured == b.ahead.configured &&
				a.ahead.available == b.ahead.available && a.ahead.depth == b.ahead.depth && a.ahead.reason == b.ahead.reason;
		}

		const int kFrames[] = { 0, 1, 2, 4 };
		const double kRates[] = { 0.0, 23.976, 24.0, 50.0, 59.94, 120.0, 240.0 };
		const size_t kQueued[] = { 0, 1, 2, 3, 4, 9, 40 };
	}

	TEST_CLASS(ProducerPlanTests)
	{
	public:
		// Every lifecycle input combination, with every producer start outcome: the plan's action, latch and
		// engagement effects equal the old block's, and the depth decision equals it from the resulting state.
		TEST_METHOD(LifecycleAndDepthEqualTheOldInlineLogicOnTheWholeInputSpace)
		{
			size_t rows = 0;
			for (unsigned bits = 0; bits < (1u << 11); ++bits)
			{
				for (int frames : kFrames)
				{
					LifecycleInput l;
					l.lowLatency = (bits & 1u) != 0;
					l.opticalFlow = (bits & 2u) != 0;
					l.vectorProducer = (bits & 4u) != 0;
					l.vectorLowLatencyAllowed = (bits & 8u) != 0;
					l.dejudderPresent = (bits & 16u) != 0;
					l.producerExists = (bits & 32u) != 0;
					l.producerRunning = l.producerExists && (bits & 64u) != 0;   // a running flag needs a producer
					l.startFailedLatched = (bits & 128u) != 0;
					l.gpuAvailable = (bits & 256u) != 0;
					const bool startSucceeds = (bits & 512u) != 0;
					const bool vectorBackward = (bits & 1024u) != 0;
					l.motionAheadFrames = frames;
					OldState s0;
					s0.exists = l.producerExists;
					s0.running = l.producerRunning;
					s0.failedLogged = l.startFailedLatched;
					s0.engaged = true;   // the engagement is only ever cleared: start from set
					s0.engineReady = true;
					for (bool estimating : { false, true })
						for (double hz : kRates)
							for (size_t queued : kQueued)
								for (bool general : { false, true })
									for (bool producerReady : { false, true })
									{
										DepthInput d;
										d.lowLatency = l.lowLatency;
										d.opticalFlow = l.opticalFlow;
										d.motionAheadFrames = frames;
										d.generalRateFixes = general;
										d.sourceHz = hz;
										d.vectorLowLatencyAllowed = l.vectorLowLatencyAllowed;
										d.vectorBackward = vectorBackward;
										d.vectorProducer = l.vectorProducer;
										d.queuedFutureFrames = queued;
										const OldState a = OldLogic(l, d, s0, startSucceeds, estimating, producerReady);
										const OldState b = NewLogic(l, d, s0, startSucceeds, estimating, producerReady);
										++rows;
										if (!Same(a, b))
										{
											const std::wstring msg = L"plan differs from the old inline logic at lifecycle bits " +
												std::to_wstring(bits) + L", frames " + std::to_wstring(frames) +
												L", estimating " + std::to_wstring(estimating) + L", hz " + std::to_wstring(hz) +
												L", queued " + std::to_wstring(queued);
											Assert::Fail(msg.c_str());
										}
									}
				}
			}
			Assert::IsTrue(rows > 100000u, L"the enumeration really ran");
		}

		// W2-C5b commit 5: optical_flow_on_demand=on keeps a producer in low latency (depth stays 0); default false = unchanged.
		TEST_METHOD(OnDemandWantedKeepsAProducerInLowLatencyOnlyWithTheDejudderAndOpticalFlow)
		{
			LifecycleInput l;
			l.lowLatency = true;
			l.dejudderPresent = true;
			l.gpuAvailable = true;
			Assert::IsFalse(Lifecycle(l).wanted, L"default: not wanted in low latency");
			l.onDemandWanted = true;
			Assert::IsTrue(Lifecycle(l).wanted);
			Assert::IsTrue(Lifecycle(l).action == Action::Start);
			l.opticalFlow = false;
			Assert::IsFalse(Lifecycle(l).wanted, L"optical flow off");
			l.opticalFlow = true;
			l.dejudderPresent = false;
			Assert::IsFalse(Lifecycle(l).wanted, L"no de-judder to serve");
			DepthInput d;
			d.lowLatency = true;
			d.motionAheadFrames = 4;
			d.sourceHz = 60.0;
			d.queuedFutureFrames = 4;
			Assert::AreEqual(0, Depth(d).ahead.depth, L"no look-ahead depth for it");
		}

		// Spot rows with their meaning, so a wrong enumeration cannot hide a wrong rule.
		TEST_METHOD(StartsWhenWantedAndAbsentAndTheDeviceExists)
		{
			LifecycleInput l;
			l.motionAheadFrames = 4;
			l.dejudderPresent = true;
			l.gpuAvailable = true;
			Assert::IsTrue(Lifecycle(l).action == Action::Start);
			l.startFailedLatched = true;
			Assert::IsTrue(Lifecycle(l).action == Action::None, L"a failed start is not retried while wanted");
			l.retryDue = true;   // W2-C1: ... until its back-off is due
			Assert::IsTrue(Lifecycle(l).action == Action::Start, L"a due retry starts it again");
			{
				// W2-C1: the latch was cleared (the want ended and came back) but the back-off still runs: no start until it is due.
				LifecycleInput b = l;
				b.startFailedLatched = false;
				b.retryDue = false;
				b.backoffActive = true;
				Assert::IsTrue(Lifecycle(b).action == Action::None, L"no start while the back-off runs, latch or not");
				Assert::IsTrue(Lifecycle(b).wanted, L"still wanted");
				b.backoffActive = false;
				Assert::IsTrue(Lifecycle(b).action == Action::Start, L"the back-off over: it starts");
			}
			l.gpuAvailable = false;
			Assert::IsTrue(Lifecycle(l).action == Action::None, L"never without the device");
			l.gpuAvailable = true;
			l.producerExists = true;
			Assert::IsTrue(Lifecycle(l).action == Action::None, L"never a second producer");
			l.producerExists = false;
			l.retryDue = false;
			l.startFailedLatched = false;
			l.lowLatency = true;
			Assert::IsFalse(Lifecycle(l).wanted, L"the matcher's de-judder does not want it in low latency");
			l.vectorProducer = true;
			l.vectorLowLatencyAllowed = true;
			Assert::IsTrue(Lifecycle(l).wanted, L"vector demand wants it in low latency when allowed");
		}

		TEST_METHOD(AReleasedDejuddersProducerIsKeptBrieflyButADeadOneIsStopped)
		{
			LifecycleInput l;
			l.motionAheadFrames = 4;
			l.producerExists = true;
			l.producerRunning = true;
			l.startFailedLatched = true;
			LifecycleOutput o = Lifecycle(l);
			Assert::IsTrue(o.keepBriefly && o.action == Action::None && !o.clearStartFailed && o.disengage);
			l.producerRunning = false;
			o = Lifecycle(l);
			Assert::IsTrue(!o.keepBriefly && o.action == Action::Stop && o.clearStartFailed);
			l.producerRunning = true;
			l.opticalFlow = false;   // the settings no longer want it: stopped, not kept
			o = Lifecycle(l);
			Assert::IsTrue(!o.keepBriefly && o.action == Action::Stop);
		}

		// SK-MV warm engine (owner MUST KEEP 1 + 2): a de-judder that is enabled but idle is a STANDING consumer, so frames keep flowing to the
		// producer; at a transient it finds the pair of its first estimate already made, with no warm-up gap. Without the standing demand
		// (the old decision) the same sequence misses, which is why the de-judder used to be always on.
		TEST_METHOD(AnEnabledIdleDejudderIsAStandingConsumerAndHasAPairOnTheFirstTransientFrame)
		{
			DepthInput d;
			d.opticalFlow = true;
			d.motionAheadFrames = 4;
			d.sourceHz = 24.0;
			d.producerRunning = true;
			d.queuedFutureFrames = 8;
			Assert::IsFalse(Depth(d).motionConsumer, L"idle and not standing: no consumer (the old decision)");
			Assert::AreEqual(0, Depth(d).ahead.depth);
			d.dejudderStanding = true;
			Assert::IsTrue(Depth(d).motionConsumer, L"enabled: a standing consumer");
			Assert::IsTrue(Depth(d).storedDepth > 0, L"frames go to the producer while the de-judder is idle");
			d.opticalFlow = false;
			Assert::IsFalse(Depth(d).ahead.reason == MotionAhead::Reason::Active, L"optical flow off: no engine to keep warm");

			auto firstTransientHasAPair = [](bool standing) {
				VpMotion::P2Slot slot(16);
				slot.SetGeneration(1);
				DepthInput in;
				in.opticalFlow = true;
				in.motionAheadFrames = 4;
				in.sourceHz = 24.0;
				in.producerRunning = true;
				in.queuedFutureFrames = 8;
				in.dejudderStanding = standing;
				const uint64_t transient = 20;
				uint64_t nextSubmit = 1;
				bool pairBeforeThePlan = false;
				for (uint64_t n = 1; n <= transient; ++n)
				{
					if (n == transient)
						pairBeforeThePlan = static_cast<bool>(slot.Find(transient, transient + 1));   // what the first transient estimate would take
					in.dejudderEstimating = n >= transient;
					const DepthOutput o = Depth(in);
					if (o.storedDepth <= 0)
						continue;
					for (; nextSubmit <= n + static_cast<uint64_t>(o.storedDepth); ++nextSubmit)
						if (nextSubmit > 1)
							slot.Publish(nextSubmit - 1, nextSubmit, 1, std::make_shared<const MotionAhead::Flow>());   // the producer pairs each submitted frame
				}
				return pairBeforeThePlan;
			};
			Assert::IsFalse(firstTransientHasAPair(false), L"without the standing demand the first transient frame has no pair (the warm-up gap)");
			Assert::IsTrue(firstTransientHasAPair(true), L"with it, the pair is already there");
		}

		// SK-MV warm engine: the standing class by reason. The coordinator's trigger is "the de-judder setting is on AND the cadence is
		// eligible": the reasons that act now or become Active at a transient keep the engine warm; the reasons where it can never act stay
		// idle (an idle matcher kept loaded for hiccup repair runs no NVOF).
		// W2-C1 MUST 2: the occlusion mask is reuse-only: alone it runs the estimate only while the vector engine serves it.
		TEST_METHOD(TheOcclusionMaskAloneRunsTheEstimateOnlyWhileServed)
		{
			DejudderPolicy::Input in;
			in.strength = 0.0;
			in.strengthEven = 0.0;
			in.occlusionMask = true;
			in.sourceHz = 23.976;
			in.displayHz = 60.0;
			in.haveNext = true;
			in.nextMatches = true;
			Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Active, L"served: the mask runs the estimate");
			in.occlusionMaskServed = false;
			Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Off, L"refused (no starter): no matcher for the mask alone");
			in.strength = 0.5;
			Assert::IsTrue(DejudderPolicy::Decide(in) == DejudderPolicy::Reason::Active, L"the de-judder itself is a starter: unaffected");
		}

		TEST_METHOD(OnlyReasonsThatCanBecomeActiveAreStanding)
		{
			using R = DejudderPolicy::Reason;
			const R standing[] = { R::Active, R::NoNextFrame, R::NextFrameDiffers, R::UnknownRates, R::PlaneFormat };
			const R idle[] = { R::Off, R::LowLatency, R::Interlaced, R::IneligibleCadence, R::VideoRateMaskOnly, R::NativeRgb, R::SmoothMotion };
			for (R r : standing) Assert::IsTrue(DejudderPolicy::CanEngage(r), L"can engage: the engine stays warm");
			for (R r : idle) Assert::IsFalse(DejudderPolicy::CanEngage(r), L"cannot act on this source: idle");
			// Each idle reason keeps the engine idle: with no other demand there is no consumer and no frame goes to the producer.
			for (R r : idle)
			{
				DepthInput d;
				d.opticalFlow = true;
				d.motionAheadFrames = 4;
				d.sourceHz = 60.0;
				d.producerRunning = true;
				d.queuedFutureFrames = 8;
				d.dejudderStanding = DejudderPolicy::CanEngage(r);
				Assert::IsFalse(Depth(d).motionConsumer, L"idle reason: no standing consumer");
				Assert::AreEqual(0, Depth(d).storedDepth);
			}
		}

		// The other demands keep their OWN trigger: hiccup repair, the camera fit and TD ask through the vector demand (vectorProducer), which
		// is unchanged by the standing flag in both directions.
		TEST_METHOD(OtherDemandsDoNotRideOnTheDejudderStandingTrigger)
		{
			DepthInput d;
			d.opticalFlow = true;
			d.motionAheadFrames = 4;
			d.sourceHz = 24.0;
			d.producerRunning = true;
			d.queuedFutureFrames = 8;
			for (bool standing : { false, true })
				for (bool other : { false, true })
				{
					d.dejudderStanding = standing;
					d.vectorProducer = other;
					Assert::AreEqual(standing || other, Depth(d).motionConsumer);
				}
			// A vector-demand consumer without the de-judder standing still gets its frames, and the standing flag alone never sets or
			// clears the vector demand's own fields.
			d.dejudderStanding = false;
			d.vectorProducer = true;
			Assert::IsTrue(Depth(d).storedDepth > 0, L"camera / TD / hiccup repair demand: served on its own");
			d.vectorProducer = false;
			Assert::AreEqual(0, Depth(d).storedDepth, L"no demand, no standing: idle");
		}

		// The transient-start failure the owner fixed once: the cadence switches from ineligible (60p video) to eligible (24p-in-60). The
		// reasons run IneligibleCadence ..., UnknownRates (the rates are measured), Active. Standing from the first eligible
		// reason means pairs exist BEFORE the de-judder first engages (Active); without the standing reasons the first Active frame is a miss.
		TEST_METHOD(ACadenceSwitchToEligibleFindsTheEngineWarmBeforeTheDejudderFirstEngages)
		{
			using R = DejudderPolicy::Reason;
			struct Frame { R reason; };
			std::vector<R> sequence(10, R::IneligibleCadence);
			sequence.push_back(R::UnknownRates);
			sequence.push_back(R::UnknownRates);
			sequence.push_back(R::Active);
			sequence.push_back(R::Active);
			auto pairAtFirstActive = [&](bool hookDeliversStanding) {
				VpMotion::P2Slot slot(16);
				slot.SetGeneration(1);
				uint64_t nextSubmit = 1;
				for (size_t i = 0; i < sequence.size(); ++i)
				{
					const uint64_t n = i + 1;
					const R r = sequence[i];
					if (r == R::Active)
						return static_cast<bool>(slot.Find(n, n + 1));   // what the first estimate takes, before this frame's plan
					DepthInput d;
					d.opticalFlow = true;
					d.motionAheadFrames = 4;
					d.sourceHz = 24.0;
					d.producerRunning = true;
					d.queuedFutureFrames = 8;
					d.dejudderEstimating = r == R::NoNextFrame || r == R::NextFrameDiffers;   // the existing hook input
					d.dejudderStanding = hookDeliversStanding && DejudderPolicy::CanEngage(r);
					const DepthOutput o = Depth(d);
					if (o.storedDepth <= 0)
						continue;
					for (; nextSubmit <= n + static_cast<uint64_t>(o.storedDepth); ++nextSubmit)
						if (nextSubmit > 1)
							slot.Publish(nextSubmit - 1, nextSubmit, 1, std::make_shared<const MotionAhead::Flow>());
				}
				return false;
			};
			Assert::IsTrue(pairAtFirstActive(true), L"with the standing reasons: warm, valid pairs before the first Active frame");
			// Without the host hook line (UnknownRates not delivered as standing) nothing runs before the first Active frame, which finds its
			// pair missing: the gap the hook line closes (the status line says "waiting for host hook").
			Assert::IsFalse(pairAtFirstActive(false), L"without the hook line the UnknownRates frames are not standing");
		}

		// A DIRECT switch IneligibleCadence -> Active (no UnknownRates in between) with NO film cadence being acquired (a settings change, a
		// display-rate switch) is cold for a few frames, and that is measured here, not assumed: an ineligible cadence is not standing, because
		// keeping NVOF running on every frame of a source the de-judder can never act on is what the MUST KEEP forbids. The first Active frame finds
		// no pair (a counted P2m miss; the producer's first pair takes about one NVOF latency, one to a few frames, always below the sustained
		// threshold, so never shown), the plan of that frame submits the queue, and the following frames are served by the producer. The pulldown
		// lock - the common source of this switch - is NOT cold: see the Acquiring tests below.
		TEST_METHOD(ADirectIneligibleCadenceToActiveSwitchWithoutAnAcquiringCadenceIsColdForAFewFramesBelowTheSustainedThreshold)
		{
			using R = DejudderPolicy::Reason;
			std::vector<R> sequence(10, R::IneligibleCadence);
			sequence.push_back(R::Active);
			sequence.push_back(R::Active);
			sequence.push_back(R::Active);
			Assert::IsFalse(DejudderPolicy::CanEngage(R::IneligibleCadence), L"the ineligible cadence is not standing");
			VpMotion::P2Slot slot(16);
			slot.SetGeneration(1);
			uint64_t nextSubmit = 1;
			std::vector<bool> activeFrameFoundPair;
			int depthSeen = 0;
			for (size_t i = 0; i < sequence.size(); ++i)
			{
				const uint64_t n = i + 1;
				const R r = sequence[i];
				if (r == R::Active)
					activeFrameFoundPair.push_back(static_cast<bool>(slot.Find(n, n + 1)));   // what this frame's estimate takes, before this frame's plan
				DepthInput d;
				d.opticalFlow = true;
				d.motionAheadFrames = 4;
				d.sourceHz = 24.0;
				d.producerRunning = true;
				d.queuedFutureFrames = 8;
				d.dejudderEstimating = r == R::Active;
				d.dejudderStanding = DejudderPolicy::CanEngage(r);
				const DepthOutput o = Depth(d);
				depthSeen = o.storedConfigured;
				if (o.storedDepth <= 0)
					continue;
				for (; nextSubmit <= n + static_cast<uint64_t>(o.storedDepth); ++nextSubmit)
					if (nextSubmit > 1)
						slot.Publish(nextSubmit - 1, nextSubmit, 1, std::make_shared<const MotionAhead::Flow>());
			}
			Assert::AreEqual<size_t>(3, activeFrameFoundPair.size());
			Assert::IsFalse(activeFrameFoundPair[0], L"the first Active frame after a direct switch with no cadence being acquired is cold (documented)");
			Assert::IsTrue(activeFrameFoundPair[1] && activeFrameFoundPair[2], L"the producer serves from the next frame on");
			VpMotion::NoPairTracker tracker;
			tracker.Note(true, depthSeen);
			Assert::IsFalse(tracker.Sustained(depthSeen), L"the few cold frames are counters only, never an engaged fallback");
		}

		// The pattern of `filmHz` carried in `signalHz` the way a source does it (each signal frame shows the latest film frame due).
		static std::string CarriedPatternFor(double filmHz, double signalHz, size_t frames)
		{
			std::string pattern;
			long long previous = -1;
			for (size_t i = 0; i < frames; ++i)
			{
				const long long film = static_cast<long long>(std::floor(static_cast<double>(i) * filmHz / signalHz + 1e-9));
				pattern += film == previous ? 'R' : 'N';
				previous = film;
			}
			return pattern;
		}

		// 60p video with sporadic repeats (hiccups, a dropped-and-repeated frame) and a frozen picture never look like a pulldown cycle: the
		// standing engine stays idle where the de-judder can never act.
		TEST_METHOD(SporadicRepeatsOnVideoAndAFrozenPictureNeverStartTheStandingEngine)
		{
			using R = DejudderPolicy::Reason;
			std::string video(600, 'N');
			for (const size_t at : { size_t(13), size_t(50), size_t(131), size_t(157), size_t(301), size_t(419), size_t(420), size_t(555) })
				video[at] = 'R';
			Pulldown::Detector detector;
			for (size_t i = 0; i < video.size(); ++i)
			{
				const Pulldown::Decision d = detector.Next(video[i] == 'R', false, 60.0);
				Assert::IsFalse(d.locked, L"no lock on sporadic repeats");
				Assert::IsFalse(detector.Acquiring(), L"sporadic repeats are not a cycle");
				Assert::IsFalse(DejudderPolicy::CanEngageOrAcquiring(R::IneligibleCadence, detector.Acquiring()), L"idle");
			}
			Pulldown::Detector frozen;
			for (int i = 0; i < 300; ++i)
			{
				frozen.Next(i != 0, false, 60.0);   // the first frame is new, then the picture freezes
				Assert::IsFalse(frozen.Acquiring(), L"a frozen picture (all repeats) is not a pulldown");
			}
			// A source the de-judder can never act on is not made standing by a cadence either.
			Assert::IsFalse(DejudderPolicy::CanEngageOrAcquiring(R::Off, true));
			Assert::IsFalse(DejudderPolicy::CanEngageOrAcquiring(R::VideoRateMaskOnly, true));
			Assert::IsFalse(DejudderPolicy::CanEngageOrAcquiring(R::LowLatency, true));
			Assert::IsTrue(DejudderPolicy::CanEngageOrAcquiring(R::IneligibleCadence, true), L"the ineligible cadence that is being acquired");
			Assert::IsTrue(DejudderPolicy::CanEngageOrAcquiring(R::UnknownRates, false), L"the existing standing reasons are unchanged");
		}

		// 24p carried in 60 Hz: the pulldown lock needs FramesToLock frames (three cycles and two seconds), and the lock frame is where the source
		// rate turns into the film's and the de-judder first engages. The cadence is ACQUIRED well before (three cycles), so with the acquiring
		// signal the engine is warm: valid pairs exist at the lock frame. Without it the same switch is cold (the control).
		TEST_METHOD(ACadenceBeingAcquiredHasValidPairsBeforeThePulldownLockFrame)
		{
			using R = DejudderPolicy::Reason;
			const double signalHz = 60.0;
			const std::string frames = CarriedPatternFor(24.0, signalHz, 200);
			auto pairAtLock = [&](bool useAcquiring, int& firstAcquiring, int& lockFrame) {
				VpMotion::P2Slot slot(16);
				slot.SetGeneration(1);
				Pulldown::Detector detector;
				uint64_t nextSubmit = 1;
				firstAcquiring = lockFrame = -1;
				for (size_t i = 0; i < frames.size(); ++i)
				{
					const uint64_t n = i + 1;
					const Pulldown::Decision d = detector.Next(frames[i] == 'R', false, signalHz);
					const bool acquiring = !d.locked && detector.Acquiring();
					if (acquiring && firstAcquiring < 0)
						firstAcquiring = static_cast<int>(i);
					if (d.locked)
					{
						lockFrame = static_cast<int>(i);
						return static_cast<bool>(slot.Find(n, n + 1));   // what the first Active frame's estimate takes, before its own plan
					}
					DepthInput in;
					in.opticalFlow = true;
					in.motionAheadFrames = 4;
					in.sourceHz = signalHz;   // before the lock the source rate is the signal's
					in.producerRunning = true;
					in.queuedFutureFrames = 8;
					in.dejudderEstimating = false;
					in.dejudderStanding = DejudderPolicy::CanEngageOrAcquiring(R::IneligibleCadence, useAcquiring && acquiring);
					const DepthOutput o = Depth(in);
					if (o.storedDepth <= 0)
						continue;
					for (; nextSubmit <= n + static_cast<uint64_t>(o.storedDepth); ++nextSubmit)
						if (nextSubmit > 1)
							slot.Publish(nextSubmit - 1, nextSubmit, 1, std::make_shared<const MotionAhead::Flow>());
				}
				return false;
			};
			int acquiredAt = -1, lockedAt = -1;
			Assert::IsTrue(pairAtLock(true, acquiredAt, lockedAt), L"acquiring: valid pairs exist at the lock frame");
			Assert::IsTrue(lockedAt >= 0 && acquiredAt >= 0, L"the pattern locked and was acquired first");
			Assert::IsTrue(lockedAt + 1 >= Pulldown::Detector::FramesToLock(5, signalHz), L"the lock waits for the cycles AND the two seconds");
			Assert::IsTrue(acquiredAt + 60 < lockedAt, L"the cadence is acquired at least a second of signal frames before the lock");
			int a2 = -1, l2 = -1;
			Assert::IsFalse(pairAtLock(false, a2, l2), L"control: without the acquiring signal the same switch finds no pair at the lock frame");
			Assert::AreEqual(lockedAt, l2);
		}

		TEST_METHOD(DepthIsZeroWhileTheProducerDoesNotRun)
		{
			DepthInput d;
			d.motionAheadFrames = 4;
			d.dejudderEstimating = true;
			d.queuedFutureFrames = 9;
			d.sourceHz = 60.0;
			DepthOutput o = Depth(d);
			Assert::AreEqual(0, o.storedDepth);
			Assert::IsTrue(o.disengage);
			d.producerRunning = true;
			o = Depth(d);
			Assert::AreEqual(4, o.storedDepth, L"60p: 4 frames, queue holds 9");
			Assert::IsFalse(o.disengage);
			d.sourceHz = 24.0;
			Assert::AreEqual(2, Depth(d).storedDepth, L"24p: the depth is a time, 2 frames");
		}
	};
}
