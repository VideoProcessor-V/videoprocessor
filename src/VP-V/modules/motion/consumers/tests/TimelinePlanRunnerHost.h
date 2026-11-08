// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-5: the rig that drives the MOVED timeline plan (TimelinePlanner.h: PlanVpTimeline through an R10
// OutputPlan, and VpTimelineLine for the log) over the same scenarios as the legacy copy. The scenario's
// setup / per-frame edits are applied to a legacy State used only as the input carrier (settings, low
// latency, gap repair, the interpolation and de-judder decisions, the draw-time counters); the rig copies
// those inputs into the module state before each frame. The display-repeat either/or after the call stays
// in the renderer (unchanged), so the rig runs the copy's statements for it.

#include <VP-V/modules/motion/consumers/TimelinePlanner.h>
#include <VP-V/modules/motion/consumers/tests/TimelinePlanScenarios.h>

#include <vprenderer/RendererExtension.h>

#include <string>
#include <vector>

namespace TimelinePlanRunner
{
	using TimelinePlanLegacy::Frame;
	using TimelinePlanLegacy::Scenario;

	struct Rig
	{
		TimelinePlanLegacy::State in;      // the scenario's input carrier (never run)
		VpMotion::DejudderPolicyState st;
		const Frame* frame = nullptr;
		int displayCadenceOwner = 0;
		std::vector<std::string> logs;
		std::string fallback;
		std::vector<double> motionOutputs, vpPhases;
		bool effectiveRepeatCadence = false, vpSchedulesRefreshes = false;

		// ---- the host interface PlanVpTimeline calls ----
		bool TakeReanchor() { return frame->reanchorBits; }
		bool TemporalActive() const { return frame->temporalActive; }
		bool VpTimelineWanted() const
		{
			// The renderer's VpTimelineWanted (PhaseTimeline.inl), over the module state and the inputs.
			const bool keep = st.interpolationDecision.reason == InterpolationPolicy::Reason::NoNextFrame && st.vpTimeline.Initialized();
			if (in.vpGapFillStandalone && !in.lowLatency)
				return true;
			return in.activeSettings.interpolation != InterpolationPolicy::Mode::Off && (st.interpolating || keep) && !in.lowLatency;
		}
		double QpcFrequency() const { return frame->qpcFrequency > 0 ? static_cast<double>(frame->qpcFrequency) : 0.0; }
		uint64_t ReaderGeneration(uint64_t, uint64_t) const { return frame->readerGeneration; }
		double TimelineDisplayHz() const { return frame->timelineDisplayHz; }
		void LogVpTimeline(const char* what)
		{
			VpMotion::VpTimelineCounters c;
			c.phasePictures = in.vpPhasePictures;
			c.heldPictures = in.vpHeldPictures;
			c.skippedFrames = in.vpSkippedFrames;
			c.filmUpdates = in.vpFilmUpdates;
			c.gapPairs = in.vpGapPairs;
			logs.push_back(VpMotion::VpTimelineLine(what, st.vpOutputPhase, c, st.vpTimeline));
		}

		void Run(const Frame& f)
		{
			frame = &f;
			st.interpolating = in.interpolating;
			st.interpolationDecision = in.interpolationDecision;
			st.dejudderReason = in.dejudderReason;
			st.dejudderExtrasAllowed = in.dejudderExtrasAllowed;
			if (f.queueFlushBefore)   // the BacklogFlush event (DejudderExtension -> the binding)
				st.vpQueueErrorReset.store(true, std::memory_order_release);
			logs.clear();
			fallback.clear();
			motionOutputs = f.temporalOutputs;   // Bill: auto motionOutputs=temporalRenderer.outputs;
			vpPhases.clear();
			RendererExtension::OutputPlan plan;
			plan.seq = static_cast<int64_t>(f.sourceSequence);
			plan.nextSeq = static_cast<int64_t>(f.nextSourceSequence);
			plan.generation = static_cast<int64_t>(f.frameGeneration);
			plan.ptsA = f.sourcePts;
			plan.ptsB = f.nextPts;
			plan.filmHz = f.frameFilmHz;
			plan.captureHz = f.captureRateHz;
			plan.arrivalQpc = f.arrivalQpc;
			plan.discontinuity = f.discontinuity;
			plan.fileFrame = f.currentFileFrame;
			plan.queueDepthAfterDequeue = f.queueDepthAfterDequeue;
			plan.desiredDepth = f.desiredQueueDepth;
			plan.holdFraction = f.holdFraction;
			plan.queueTimingFixes = f.queueTimingFixes;
			plan.outputs = &motionOutputs;
			plan.phases = &vpPhases;
			VpMotion::PlanVpTimeline(in.activeSettings, st, plan, *this);
			// The renderer's lines after the call (unchanged; the copy's statements).
			vpSchedulesRefreshes = st.vpTimelineActive ||
				(st.dejudderReason == DejudderPolicy::Reason::Active && st.dejudderExtrasAllowed);
			const bool requestedRepeatCadence = !f.recoveryFallback && in.activeSettings.presentEveryVsync && !f.temporalMixing;
			effectiveRepeatCadence = requestedRepeatCadence && !vpSchedulesRefreshes;
			if (requestedRepeatCadence && vpSchedulesRefreshes)
				fallback = "engaged: present_every_vsync paused: VP's per-refresh timeline / de-judder extras schedule the refreshes";
			else
				fallback = "released";
			const int cadenceOwner = f.temporalMixing ? 2 : effectiveRepeatCadence ? 1 : 0;
			if (cadenceOwner != displayCadenceOwner)
			{
				displayCadenceOwner = cadenceOwner;
				char line[256];
				std::snprintf(line, sizeof(line), "VP display cadence owner: requested_every_vsync=%d effective_every_vsync=%d smooth_motion_mixing=%d owner=%s",
					in.activeSettings.presentEveryVsync ? 1 : 0, effectiveRepeatCadence ? 1 : 0, f.temporalMixing ? 1 : 0,
					cadenceOwner == 2 ? "smooth-motion" : cadenceOwner == 1 ? "display-repeat" : "source");
				logs.push_back(line);
			}
		}

		// The copy's trace line, from the module state.
		std::string TraceLine(const Frame& f) const
		{
			TimelinePlanLegacy::State v;
			v.vpTimelineActive = st.vpTimelineActive;
			v.vpSkipOutput = st.vpSkipOutput;
			v.vpSkipIsUpdate = st.vpSkipIsUpdate;
			v.vpCaptureRateHz = st.vpCaptureRateHz;
			v.vpTimeline = st.vpTimeline;
			v.vpSchedulesRefreshes = vpSchedulesRefreshes;
			v.effectiveRepeatCadence = effectiveRepeatCadence;
			v.fallback = fallback;
			v.motionOutputs = motionOutputs;
			v.vpPhases = vpPhases;
			v.logs = logs;
			return v.TraceLine(f);
		}
	};

	inline std::vector<std::string> Trace(const Scenario& sc)
	{
		Rig r;
		if (sc.setup)
			sc.setup(r.in);
		std::vector<std::string> lines;
		for (size_t i = 0; i < sc.frames.size(); ++i)
		{
			if (sc.beforeFrame)
				sc.beforeFrame(r.in, i);
			r.Run(sc.frames[i]);
			lines.push_back(r.TraceLine(sc.frames[i]));
		}
		return lines;
	}
}
