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

#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/ProducerPlan.h>

#include <algorithm>
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
