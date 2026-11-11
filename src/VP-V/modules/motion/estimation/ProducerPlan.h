// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a: the optical-flow producer's lifecycle and look-ahead depth decision as pure functions.
// The decision used to be inline in the renderer's render pass (docs/VP-nvof-ahead.md, owner 2026-10-16);
// the expressions below are that logic verbatim, split in the two phases the pass ran in:
//   Lifecycle  - start the producer, stop it, keep it briefly, clear its start-failure latch, drop the
//                look-ahead engagement (before the producer is acted on);
//   Depth      - the frames-ahead decision, from the producer's state AFTER the lifecycle step.
// No state, no clock, no host types, so the identity test (ProducerPlanTests) can enumerate the input
// space against a test-only copy of the old inline logic. C++14 (VideoProcessor-Lib compiles at C++14).
#pragma once

#include <VP-V/modules/motion/estimation/MotionAhead.h>

#include <cstddef>

namespace VpMotion
{
	namespace ProducerPlan
	{
		enum class Action { None, Start, Stop };

		struct LifecycleInput
		{
			bool lowLatency = false;
			bool opticalFlow = true;               // the optical_flow setting
			int motionAheadFrames = 0;             // the motion_ahead_frames setting
			bool vectorProducer = false;           // vectorDemand.producer
			bool vectorLowLatencyAllowed = false;  // vectorDemand.lowLatencyAllowed
			bool dejudderPresent = false;          // the de-judder's matcher exists
			bool producerExists = false;           // a producer object is held
			bool producerRunning = false;          // ... and its worker runs (meaningful with producerExists)
			bool startFailedLatched = false;       // an earlier start failed: not retried ...
			bool retryDue = false;                 // ... unless its back-off is due (W2-C1; false = the old decision)
			bool gpuAvailable = false;             // the device the engine needs exists
		};

		struct LifecycleOutput
		{
			bool wanted = false;           // wantProducer
			Action action = Action::None;  // Start: try to create it; Stop: stop and release it (engine no longer ready)
			bool keepBriefly = false;      // unwanted only because the de-judder was released: kept alive
			bool clearStartFailed = false; // the retry latch is cleared (it may be started again once wanted)
			bool disengage = false;        // the look-ahead engagement is dropped (unwanted: never a stale one)
		};

		inline LifecycleOutput Lifecycle(const LifecycleInput& in)
		{
			LifecycleOutput out;
			const bool vectorProducer = in.vectorProducer && in.opticalFlow && in.motionAheadFrames > 0 &&
				(!in.lowLatency || in.vectorLowLatencyAllowed);
			out.wanted = (in.dejudderPresent && in.opticalFlow && in.motionAheadFrames > 0 && !in.lowLatency) || vectorProducer;
			if (out.wanted && !in.producerExists && (!in.startFailedLatched || in.retryDue) && in.gpuAvailable)
				out.action = Action::Start;
			else if (!out.wanted)
			{
				const bool onlyReleased = !in.dejudderPresent && in.opticalFlow && in.motionAheadFrames > 0 && !in.lowLatency;
				out.keepBriefly = in.producerExists && in.producerRunning && onlyReleased;   // never a dead one
				if (in.producerExists && !out.keepBriefly)
					out.action = Action::Stop;
				if (!out.keepBriefly)
					out.clearStartFailed = true;
				out.disengage = true;
			}
			return out;
		}

		struct DepthInput
		{
			bool lowLatency = false;
			bool opticalFlow = true;
			int motionAheadFrames = 0;
			bool generalRateFixes = true;
			double sourceHz = 0.0;
			bool vectorLowLatencyAllowed = false;
			bool vectorBackward = false;           // vectorDemand.mode == Backward
			bool vectorProducer = false;           // vectorDemand.producer
			bool dejudderEstimating = false;       // the de-judder's reason is Active / NoNextFrame / NextFrameDiffers
			// SK-MV warm engine (owner MUST KEEP): a STANDING demand, true whenever the de-judder is enabled (its matcher exists), whether
			// or not it is estimating this frame. The shared engine is the de-judder's only optical-flow source now, so the producer keeps
			// making pairs continuously and a transient finds valid vectors on its FIRST frame (no warm-up gap, the reason the de-judder is
			// always on). Default false = the old decision exactly.
			bool dejudderStanding = false;
			bool producerRunning = false;          // producer exists AND runs, after the lifecycle step
			size_t queuedFutureFrames = 0;
		};

		struct DepthOutput
		{
			MotionAhead::Decision ahead;
			bool motionConsumer = false;
			int storedDepth = 0;           // motionAheadDepth: 0 unless the producer runs
			bool storedBackward = false;   // motionAheadBackward
			int storedConfigured = 1;      // motionAheadConfigured, at least 1
			bool disengage = false;        // no frames are collected: no stale engagement
		};

		inline DepthOutput Depth(const DepthInput& in)
		{
			DepthOutput out;
			out.motionConsumer = in.dejudderEstimating || in.vectorProducer || in.dejudderStanding;
			out.ahead = MotionAhead::Decide(
				MotionAhead::DepthAt(in.motionAheadFrames, in.sourceHz, in.generalRateFixes),
				in.lowLatency && (!in.vectorLowLatencyAllowed || in.vectorBackward),
				in.producerRunning, in.queuedFutureFrames, in.opticalFlow, out.motionConsumer, in.vectorBackward);
			out.storedDepth = in.producerRunning ? out.ahead.depth : 0;
			out.storedBackward = in.producerRunning && out.ahead.reason == MotionAhead::Reason::BackwardPairs;
			out.storedConfigured = (std::max)(1, out.ahead.configured);
			out.disengage = !in.producerRunning || out.ahead.depth == 0;
			return out;
		}
	}
}
