// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-2 (doc 59 section 2.1 items a, b, d, e): the de-judder policy's and the per-vsync timeline's
// state, moved verbatim (types, initialisers, comments) out of the renderer's Impl struct. The renderer
// keeps one reference alias per member (one VPV-HOOK block, LibplaceboVideoRenderer.cpp), so every
// existing reader and writer compiles unchanged and the values are the old members'; each alias goes
// when its last reader outside this module moves (P5c extras, P6 render-ahead: README alias table).
//
// Threading (P5b-1 inventory): the render thread only. The policy (UpdateDejudderEstimate) runs in
// RenderLocked; the timeline block runs in RenderLoop BEFORE RenderLocked, on the same thread and with
// renderMutex HELD (the render loop's unique_lock; WaitForDisplaySlot releases it only between draws).
// vpQueueErrorReset stays atomic: set at the render loop's backlog flush (the QueueFlush event), taken
// (exchange) by the next frame's timeline plan.
//
// Plain data, C++14, no GPU types. Left in the renderer (not plain data or not this package's): the
// vsync output identity (CComPtr / swap-chain pointers, P5c), dejudderHoldLog (the diagnostics adapter),
// the phase picture planes (Dejudder::Planes, GPU textures; drawn in place), the extras' stats (P5c).

#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>
#include <VP-V/modules/motion/estimation/MotionResyncReaders.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <vprenderer/DisplayRateHold.h>

#include <atomic>
#include <cstdint>

namespace VpMotion
{
	struct DejudderPolicyState
	{
		// ---- d / e: the decision and its logs ----
		unsigned long long dejudderAheadLogTick = 0;
		DejudderPolicy::Reason dejudderReason = DejudderPolicy::Reason::Off;
		unsigned long long dejudderReasonLogTick = 0;
		unsigned long long dejudderReasonChangesNotLogged = 0;
		unsigned long long diagDejudderModeTick = 0;          // report-a-problem: <= 4 mode rows per second
		unsigned long long diagDejudderModeSkipped = 0;
		uint32_t diagLastHoldCode = 0;           // last de-judder hold code recorded as a decision
		bool dejudderExtrasAllowed = false;
		InterpolationPolicy::Decision interpolationDecision;
		bool interpolating = false;              // this frame's extras interpolate
		int interpolationReasonLogged = -1;
		unsigned long long interpolationLogTick = 0;
		unsigned interpolationLogCount = 0;
		float dejudderEffectiveStrength = 0.0f;   // delta 10: uneven or even strength for this source
		int dejudderEvenCycle = 0;   // refreshes per film frame on an even cadence, 0 = uneven
		bool smoothMotionEngine = false;   // motion_engine resolved to libplacebo's smooth motion
		bool motionEngineLogged = false;
		bool smoothMotionMixingLogged = false;
		bool dejudderMaskRateLogged = false;
		// The content rate the matcher's per-frame settings are converted to.
		double dejudderSourceHz = 0.0;

		// ---- a: phase state (the extras' latency floor and prediction; their readers are P5c's) ----
		DejudderPolicy::LatencyFloor dejudderLatencyFloor;
		double dejudderCallLatency = 0.0;
		double dejudderLoggedFloor = 0.0;   // the floor last reported (s)
		uint64_t dejudderPhaseGeneration = 0;
		bool dejudderPhaseLogPending = true;
		double dejudderPredictedVsync = 0.0;   // where the frame after extras should show (s)
		uint64_t dejudderPredictedGeneration = 0;
		uint64_t dejudderPredictedSequence = 0;   // the source frame the prediction is for

		// ---- b: Stage B (plan step 4): the per-vsync timeline. The render loop plans each source
		// frame's refreshes (InterpolationTimeline: none, one or several, each with its true
		// phase between A and B, any ratio) and draws each one; RenderLocked renders the
		// picture at that phase. Render-loop thread (RenderLocked runs on it). ----
		InterpolationTimeline::Timeline vpTimeline;
		bool vpTimelineActive = false;   // this source frame is presented by the timeline
		bool vpSkipOutput = false;       // no refresh falls in this frame's interval (source faster)
		bool vpSkipIsUpdate = false;     // Hunt 2 (06-interp L4): ... because it is a pulldown film update
		unsigned long long vpFilmUpdates = 0;
		double vpOutputPhase = 0.0;      // the phase of the refresh being drawn
		bool vpTimelineLogged = false;   // engaged / released, logged on change
		double vpCaptureRateHz = 0.0;     // the content rate the loop schedules with (film or measured capture)
		float vpGapPhaseScale = 1.0f;     // Bug hunt 3 F5: a standalone gap pair's phases x the de-judder strength
		AlphaQueuePolicy::TimelineQueueSteering vpQueueSteering;   // smoothed (queued - target) frames: the timeline's steering
		uint64_t vpQueueGeneration = 0;   // the queue generation vpQueueSteering belongs to
		std::atomic<bool> vpQueueErrorReset{ false };   // a backlog flush: the steering starts over
		// The display rate the timeline schedules at: measured when known, else the monitor's.
		// Bug hunt 2026-10-17 (timing_continuity_fixes): the last good measurement is held
		// while a cadence reset zeroes it (DisplayRateHold); the monitor's rate only when
		// nothing was measured since start or a mode change. Render thread.
		DisplayRateHold timelineDisplayRateHold;

		// ---- P5b-7 (doc 50 Q4): the `motion-history` reader the de-judder's apply point takes from (R2 on and
		// registered on the seal only; DejudderHistory.h). Not copyable: the seal's callbacks hold its address. ----
		VpMotionResync::Readers motionHistory;
	};
}
