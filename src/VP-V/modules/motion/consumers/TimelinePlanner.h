// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-5 (doc 59 section 2.1 item h, R10 PlanOutputs): the per-vsync timeline plan of one dequeued frame,
// moved out of the renderer's RenderLoop. Statements are the old ones; the renderer's members are the
// policy state (`st`, local references of the old names) and the settings; the frame's facts come in the
// R10 OutputPlan (in: seq, nextSeq, generation, ptsA = the display source clock's time, ptsB = B's time,
// filmHz, captureHz, arrivalQpc, fileFrame, discontinuity, queue depth / desired depth / hold fraction,
// queueTimingFixes; out: outputs, phases, skip). Everything renderer-side (the resync reader, the
// generation, the QPC frequency, TimelineDisplayHz, VpTimelineWanted, LogVpTimeline) is a host call at the
// old point and in the old order. OutputPlan::displayHz is not read: TimelineDisplayHz() keeps its
// DisplayRateHold update only when the timeline plans (as before).
//
// Thread: the render loop, renderMutex HELD (the loop's unique_lock; released only between draws). The
// display-repeat either/or (requested / effective / the "display repeat" fallback line) stays in the
// renderer after the call, unchanged, reading vpTimelineActive. Header-only template, C++14.

#include <VP-V/modules/motion/consumers/DejudderState.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace VpMotion
{
	// LogVpTimeline's counters (the renderer's draw-time members; the timeline's own come from the Timeline).
	struct VpTimelineCounters
	{
		unsigned long long phasePictures = 0, heldPictures = 0, skippedFrames = 0, filmUpdates = 0, gapPairs = 0;
	};

	// The "Interpolation timeline:" line (LogVpTimeline's format and arguments, unchanged).
	inline std::string VpTimelineLine(const char* what, double vpOutputPhase, const VpTimelineCounters& c,
		const InterpolationTimeline::Timeline& vpTimeline)
	{
		char line[512];
		std::snprintf(line, sizeof(line), "Interpolation timeline: %s (phase %.3f; %llu pictures between frames, %llu without vectors or B, "
			"%llu source frames skipped, %llu film updates, %llu gap pairs, %llu restarts, %llu re-anchors, span %d)", what,
			vpOutputPhase, c.phasePictures, c.heldPictures, c.skippedFrames, c.filmUpdates, c.gapPairs, vpTimeline.Restarts(),
			vpTimeline.Reanchors(), vpTimeline.LastSpanFrames());
		return line;
	}

	// After the R10 call: when no handler planned this frame (no binding / hub, or the plan threw and the hub contained
	// it), the timeline flags from the last plan must not stand (a stale skip would drop frames): the frame is
	// drawn as the host built it. True = not planned (the caller engages the visible "timeline" fallback).
	inline bool TimelineNotPlanned(DejudderPolicyState& st, bool planned)
	{
		if (planned)
			return false;
		st.vpTimelineActive = false;
		st.vpSkipOutput = false;
		st.vpSkipIsUpdate = false;
		st.vpOutputPhase = 0.0;
		return true;
	}

	// The per-vsync timeline plan of one dequeued frame (statements as before; see the file comment).
	template<class Settings, class Plan, class Host>
	void PlanVpTimeline(const Settings& activeSettings, DejudderPolicyState& st, Plan& plan, Host& host)
	{
		InterpolationTimeline::Timeline& vpTimeline = st.vpTimeline;
		bool& vpTimelineActive = st.vpTimelineActive;
		bool& vpSkipOutput = st.vpSkipOutput;
		bool& vpSkipIsUpdate = st.vpSkipIsUpdate;
		double& vpOutputPhase = st.vpOutputPhase;
		double& vpCaptureRateHz = st.vpCaptureRateHz;
		bool& vpTimelineLogged = st.vpTimelineLogged;
		AlphaQueuePolicy::TimelineQueueSteering& vpQueueSteering = st.vpQueueSteering;
		uint64_t& vpQueueGeneration = st.vpQueueGeneration;
		std::atomic<bool>& vpQueueErrorReset = st.vpQueueErrorReset;
		std::vector<double>& motionOutputs = *plan.outputs;
		std::vector<double>& vpPhases = *plan.phases;
		const uint64_t frameGeneration = static_cast<uint64_t>(plan.generation);
		const uint64_t sourceSequence = static_cast<uint64_t>(plan.seq);
		const uint64_t nextSourceSequence = plan.nextSeq > 0 ? static_cast<uint64_t>(plan.nextSeq) : 0;
		const double sourcePts = plan.ptsA, nextPts = plan.ptsB;
		const double frameFilmHz = plan.filmHz, captureRateHz = plan.captureHz;
		const int64_t arrivalQpc = plan.arrivalQpc;
		const bool currentFileFrame = plan.fileFrame;
		const size_t queueDepthAfterDequeue = plan.queueDepthAfterDequeue, desiredQueueDepth = plan.desiredDepth;

		// R3 (resync_event on): a continuity slip or a timing event re-anchors the timeline's
		// source clock; a FULL restarts it through the generation (VpResync::ReaderGeneration).
		if(host.TakeReanchor())
			vpTimeline.Reanchor();
		vpTimelineActive=false;
		vpSkipOutput=false;
		vpOutputPhase=0.0;
		if(!host.TemporalActive()&&host.VpTimelineWanted()) {
			const double qpcFrequencyTimeline=host.QpcFrequency();
			const bool film=frameFilmHz>0.0&&arrivalQpc>0&&qpcFrequencyTimeline>0.0;
			const double ptsA=film?static_cast<double>(arrivalQpc)/qpcFrequencyTimeline:sourcePts;
			const double rateHz=frameFilmHz>0.0?frameFilmHz:captureRateHz;
			// (8) Closed on the queue: the refresh spacing is trimmed (at most 0.3%) by the
			// smoothed distance of the queue from its target, so a ppm difference between
			// the capture and display clocks cannot walk the queue.
			// A backlog flush or a new queue generation starts the steering over.
			// Hunt 2 13-timing H2/L3 (queue_timing_fixes): the smoothing is a time
			// constant in seconds (AlphaQueuePolicy::TimelineQueueSteering); the
			// one-frame margin it steers to is no longer flushed (H1).
			if(vpQueueErrorReset.exchange(false,std::memory_order_acq_rel)||frameGeneration!=vpQueueGeneration) {
				vpQueueSteering.Reset();
				vpQueueGeneration=frameGeneration;
			}
			// ABI 37: a file source is paced by its due times and keeps the queue
			// full by design: no steering on its depth.
			if(currentFileFrame)
				vpQueueSteering.Reset();
			else
				vpQueueSteering.Update(queueDepthAfterDequeue,desiredQueueDepth,rateHz,
					plan.queueTimingFixes,
					plan.holdFraction);
			vpTimeline.SetRateTrim(currentFileFrame?1.0:vpQueueSteering.RateTrim());
			vpTimeline.SetSpanInTime(activeSettings.generalRateFixes);   // fixed-constant batch 2026-10-18
			const unsigned long long updatesBefore=vpTimeline.Updates();
			const auto& planned=vpTimeline.Plan(host.ReaderGeneration(frameGeneration,sourceSequence),
				sourceSequence,ptsA,nextSourceSequence?nextPts:0.0,
				rateHz,host.TimelineDisplayHz(),plan.discontinuity);
			vpTimelineActive=true;
			vpSkipOutput=planned.empty();
			vpSkipIsUpdate=planned.empty()&&vpTimeline.Updates()!=updatesBefore;
			motionOutputs.clear();
			for(const auto& o:planned){motionOutputs.push_back(o.pts);vpPhases.push_back(o.phase);}
		} else if(vpTimeline.Initialized()) {
			vpTimeline.Reset();
			vpQueueSteering.Reset();
		}
		vpCaptureRateHz=frameFilmHz>0.0?frameFilmHz:captureRateHz;
		if(vpTimelineActive!=vpTimelineLogged) {
			vpTimelineLogged=vpTimelineActive;
			host.LogVpTimeline(vpTimelineActive?"engaged (one picture per display refresh)":"released");
		}
		plan.skip = vpSkipOutput;
	}
}
