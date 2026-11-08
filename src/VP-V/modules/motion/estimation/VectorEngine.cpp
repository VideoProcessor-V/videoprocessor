// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "VectorEngine.h"

#include <DebugLog.h>
#include <libplacebo/d3d11.h>
#include <vprenderer/MotionAheadProducer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace VpMotion
{
	// docs/VP-nvof-ahead.md (owner 2026-10-16): the optical-flow depth for this frame - the configured
	// depth, off in low latency or without the engine, never more than the frames queued. Logged when
	// it changes (the matcher covers the rest). The expressions are the renderer's old inline block
	// (LibplaceboVideoRenderer::RenderLocked at cda5794e), decided by ProducerPlan.
	void VectorEngine::PlanProducer(const ProducerInputs& in)
	{
		// The producer runs while the de-judder does, with optical flow on, outside low latency and with a
		// depth set; it stops otherwise (the matcher covers all).
		ProducerPlan::LifecycleInput life;
		life.lowLatency = in.lowLatency->load(std::memory_order_acquire);
		life.opticalFlow = in.opticalFlow;
		life.motionAheadFrames = in.motionAheadFrames;
		// Owner 2026-10-16: the producer stays alive once wanted (starting it again would cost a device on
		// every queue refill); with no frames queued ahead it gets no frames, so it computes nothing it
		// could not use - the startup fill and an emptied queue alike.
		// Vector engine on demand: also while a consumer of vectors is served (forward, or backward pairs).
		life.vectorProducer = vectorDemand.producer;
		life.vectorLowLatencyAllowed = vectorDemand.lowLatencyAllowed;
		life.dejudderPresent = in.dejudderPresent;
		life.producerExists = motionProducer != nullptr;
		life.producerRunning = motionProducer && motionProducer->Running();
		life.startFailedLatched = motionProducerFailedLogged;
		life.gpuAvailable = in.gpu != nullptr;
		const ProducerPlan::LifecycleOutput act = ProducerPlan::Lifecycle(life);
		if (act.action == ProducerPlan::Action::Start)
		{
			pl_d3d11 d3d = pl_d3d11_get(in.gpu);
			auto producer = std::make_unique<MotionAhead::Producer>();
			// The slow (best) preset: off the render path, and on the 7 real clips the seed with it removed
			// 41.9% of the judder vs the matcher's 41.3% (damage 0.35 vs 0.40; medium 41.2%, 0.37). The
			// render-time fallback keeps the matcher's preset (medium).
			const NvOpticalFlow::Preset preset = NvOpticalFlow::Preset::Slow;
			if (d3d && d3d->device && producer->Start(d3d->device, preset))
				motionProducer = std::move(producer);
			else
			{
				motionProducerFailedLogged = true;
				DebugLog::Log("Motion ahead: the producer did not start (%s); the de-judder estimates at render time",
					producer->Error().c_str());
			}
		}
		else if (act.action == ProducerPlan::Action::Stop)
		{
			// Owner: the producer stays alive once wanted - kept while the settings still want it (a resync
			// or format change can take longer than any timer); stopped when they change.
			motionProducer->Stop();
			motionProducer.reset();
			motionEngineReady.store(false, std::memory_order_release);
		}
		// Audit 2026-10-16: a failure is retried once the producer is wanted again (optical flow off and on,
		// low latency and back), and the look-ahead re-engages only after the refill - never on a stale
		// engagement.
		if (act.clearStartFailed)
			motionProducerFailedLogged = false;
		if (act.disengage)
			motionAheadEngaged = false;
		if (motionProducer && !motionProducer->Running() && !motionProducerFailedLogged)
		{
			motionProducerFailedLogged = true;
			DebugLog::Log("Motion ahead: the producer stopped (%s); the de-judder estimates at render time",
				motionProducer->Error().c_str());
		}
		motionEngineReady.store(motionProducer && motionProducer->Running() && motionProducer->EngineReady(), std::memory_order_release);
		// Frames are submitted only while a motion estimate runs for this source (the de-judder or the
		// interpolator); otherwise the producer stays alive, idle. Fixed-constant batch B3 2026-10-18
		// (general_rate_fixes): the depth as a time with a frame floor.
		ProducerPlan::DepthInput depth;
		depth.lowLatency = life.lowLatency;
		depth.opticalFlow = in.opticalFlow;
		depth.motionAheadFrames = in.motionAheadFrames;
		depth.generalRateFixes = in.generalRateFixes;
		depth.sourceHz = in.sourceHz;
		depth.vectorLowLatencyAllowed = vectorDemand.lowLatencyAllowed;
		depth.vectorBackward = vectorDemand.mode == VectorDemand::Mode::Backward;
		depth.vectorProducer = vectorDemand.producer;
		depth.dejudderEstimating = in.dejudderEstimating;
		depth.producerRunning = motionProducer && motionProducer->Running();
		depth.queuedFutureFrames = in.queuedFutureFrames->load(std::memory_order_acquire);
		const ProducerPlan::DepthOutput plan = ProducerPlan::Depth(depth);
		const MotionAhead::Decision& ahead = plan.ahead;
		motionAheadDepth.store(plan.storedDepth, std::memory_order_release);
		motionAheadBackward.store(plan.storedBackward, std::memory_order_release);
		motionAheadConfigured.store(plan.storedConfigured, std::memory_order_release);
		if (motionProducer)   // fixed-constant batch B3/B4 2026-10-18
		{
			motionProducer->SetDepth(ahead.configured);
			motionProducer->SetGeneralRates(in.generalRateFixes);
		}
		if (plan.disengage)
			motionAheadEngaged = false;   // no frames collected below: no stale engagement (audit 2026-10-16)
		char text[200];
		std::snprintf(text, sizeof(text), "Motion ahead: configured %d, queued ahead %d, depth %d (%s)",
			ahead.configured, (std::min)(ahead.available, 99), ahead.depth, MotionAhead::Describe(ahead.reason));
		// Log the reason and depth, not every change of the queued count.
		char key[96];
		std::snprintf(key, sizeof(key), "%d|%d|%d", ahead.configured, ahead.depth, static_cast<int>(ahead.reason));
		// The queue level swings around the depth as frames arrive and are shown: an Active / ShallowQueue
		// flip is logged at most every 5 s (audit 2026-10-16: up to once per frame).
		const bool levelOnly = ahead.reason == MotionAhead::Reason::Active || ahead.reason == MotionAhead::Reason::ShallowQueue;
		const auto now = std::chrono::steady_clock::now();
		if (motionAheadLogged != key && (!levelOnly || now - motionAheadLoggedAt >= std::chrono::seconds(5)))
		{
			motionAheadLogged = key;
			motionAheadLoggedAt = now;
			DebugLog::Log("%s", text);
		}
	}
}
