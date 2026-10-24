#pragma once

// TRANSITION SHIM (VP-only, 2026-10-24 draft): VP's queue rules moved to VpQueueRules.h; the
// renderer host and ~10 test files still name them AlphaQueuePolicy::X. Including this header
// (instead of, or after, vprenderer/AlphaQueuePolicy.h) keeps those names compiling with the SAME
// functions, so the move is byte-identical without touching every caller. Follow-up: rename the
// callers to VpQueueRules:: (or to the policy object) and delete this shim.
//
// Never included by upstream's AlphaQueuePolicy.h: that file stays exactly upstream's.

#include "VpQueueRules.h"

namespace AlphaQueuePolicy
{
	using VpQueueRules::LookaheadWalk;
	using VpQueueRules::SelectLookaheadWalk;
	using VpQueueRules::CombineQueueTarget;
	using VpQueueRules::StaleBacklogExcessAgeMs;
	using VpQueueRules::QueueFrameRateHz;
	using VpQueueRules::ShouldShedHeldBacklog;
	using VpQueueRules::ShouldShedHeldExcess;
	using VpQueueRules::HeldShedFrames;
	using VpQueueRules::TimelineQueueSteering;
	using VpQueueRules::GapState;
	using VpQueueRules::MergeDroppedGap;
	using VpQueueRules::EarlyUploadSlots;
	using VpQueueRules::RENDER_FAILURE_SECONDS;
	using VpQueueRules::ConsecutiveFailureLimit;
	using VpQueueRules::FramesForSeconds;

	// VP's six-argument form (queue_timing_fixes' staleBeyondTarget). The five-argument calls keep
	// resolving to upstream's own ShouldRecoverBacklog, which at 83b7ffea is the same rule as
	// staleBeyondTarget = false.
	inline bool ShouldRecoverBacklog(size_t queueDepth, size_t desiredDepth,
		double oldestQueuedAgeMs, double renderCycleMs, double frameRateHz, bool staleBeyondTarget)
	{
		return VpQueueRules::ShouldRecoverBacklog(queueDepth, desiredDepth, oldestQueuedAgeMs, renderCycleMs,
			frameRateHz, staleBeyondTarget);
	}
}
