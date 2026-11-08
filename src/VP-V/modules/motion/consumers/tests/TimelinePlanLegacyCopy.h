// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-0 (doc 59 sections 1.5 and 2.3): a TEST-ONLY verbatim copy of the per-vsync timeline block of
// the renderer's RenderLoop (vprenderer/LibplaceboVideoRenderer.cpp, fork/integ/batch1 @ 4bd69399e: from
// the R3 re-anchor to the display-repeat either/or), and of VpTimelineWanted / LogVpTimeline. It is the
// oracle the recorded traces in TimelinePlanTraceRows.inl were taken from; P5b-5's PlanOutputs must give
// the same traces. Never compiled into the product. Impl members are plain members of the same names;
// "COPY:" marks the substitutions (Bill's temporalRenderer / QPC / resync state as per-frame facts, the
// fallback and log calls recorded). Bill's lines in the block (temporalRenderer.outputs, the
// UseRepeatCadence request, the cadence owner) are facts or copied unchanged, as marked.

#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace TimelinePlanLegacy
{
	// One dequeued frame's facts (the render loop's locals and Bill's state at the block).
	struct Frame
	{
		bool temporalActive = false;       // temporalRenderer.Active()
		bool temporalMixing = false;       // temporalRenderer.Mixing()
		std::vector<double> temporalOutputs;   // temporalRenderer.outputs (Bill's list)
		bool recoveryFallback = false;     // displayCadenceRecovery.Fallback()
		bool reanchorBits = false;         // TakeIfDriving(Timeline) & (continuity | timing)
		uint64_t readerGeneration = 1;     // VpResync::ReaderGeneration(resync, frameGeneration, sourceSequence)
		uint64_t frameGeneration = 1;
		uint64_t sourceSequence = 0;
		uint64_t nextSourceSequence = 0;
		double nextPts = 0.0;
		double sourcePts = 0.0;            // displaySourceClock.Select(...)
		double filmArrivalSeconds = 0.0;   // arrivalQpc / QPF (0 = arrivalQpc 0)
		double frameFilmHz = 0.0;
		double captureRateHz = 0.0;
		bool discontinuity = false;        // frame.IsSourceDiscontinuity()
		bool currentFileFrame = false;
		size_t queueDepthAfterDequeue = 0;
		size_t desiredQueueDepth = 0;
		bool queueTimingFixes = true;      // m_queueTimingFixes
		double holdFraction = 0.0;         // CurrentQueueHold().fraction
		double timelineDisplayHz = 0.0;    // TimelineDisplayHz()
		bool queueFlushBefore = false;     // the flush path stored vpQueueErrorReset before this frame
	};

	struct Settings
	{
		InterpolationPolicy::Mode interpolation = InterpolationPolicy::Mode::Off;
		bool generalRateFixes = true;
		bool presentEveryVsync = false;
	};

	struct State
	{
		Settings activeSettings;
		bool lowLatency = false;           // COPY: lowLatencyQueueActive.load(acquire)
		InterpolationPolicy::Decision interpolationDecision;
		bool interpolating = false;
		bool vpGapFillStandalone = false;
		DejudderPolicy::Reason dejudderReason = DejudderPolicy::Reason::Off;
		bool dejudderExtrasAllowed = false;

		InterpolationTimeline::Timeline vpTimeline;
		AlphaQueuePolicy::TimelineQueueSteering vpQueueSteering;
		uint64_t vpQueueGeneration = 0;
		std::atomic<bool> vpQueueErrorReset{ false };
		bool vpTimelineActive = false;
		bool vpSkipOutput = false;
		bool vpSkipIsUpdate = false;
		double vpOutputPhase = 0.0;
		double vpCaptureRateHz = 0.0;
		bool vpTimelineLogged = false;
		int displayCadenceOwner = 0;

		// recorded
		std::vector<std::string> logs;
		std::string fallback;              // "engaged: <text>" / "released"
		std::vector<double> motionOutputs;
		std::vector<double> vpPhases;
		bool effectiveRepeatCadence = false;
		bool vpSchedulesRefreshes = false;

		void Log(const char* format, ...)
		{
			char line[512];
			va_list args;
			va_start(args, format);
			std::vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			logs.push_back(line);
		}
		void EngageFallback(const char*, const char* text) { fallback = std::string("engaged: ") + text; }
		void ReleaseFallback(const char*) { fallback = "released"; }

		// Verbatim (the low-latency load is COPY).
		bool VpTimelineWanted() const
		{
			// A queue momentarily without B keeps the schedule (A is held, never waited for), so a
			// shallow queue does not restart the phases.
			const bool keep = interpolationDecision.reason == InterpolationPolicy::Reason::NoNextFrame && vpTimeline.Initialized();
			if (vpGapFillStandalone && !lowLatency)
				return true;   // hiccup repair: this gap pair only
			return activeSettings.interpolation != InterpolationPolicy::Mode::Off && (interpolating || keep) &&
				!lowLatency;
		}
		// Verbatim apart from the draw-time counters (0 here: they count pictures drawn later).
		void LogVpTimeline(const char* what)
		{
			const unsigned long long zero = 0;
			Log("Interpolation timeline: %s (phase %.3f; %llu pictures between frames, %llu without vectors or B, "
				"%llu source frames skipped, %llu film updates, %llu gap pairs, %llu restarts, %llu re-anchors, span %d)", what,
				vpOutputPhase, zero, zero, zero, zero, zero, vpTimeline.Restarts(),
				vpTimeline.Reanchors(), vpTimeline.LastSpanFrames());
		}

		// The RenderLoop block for one dequeued frame.
		void Run(const Frame& f)
		{
			if (f.queueFlushBefore)
				vpQueueErrorReset.store(true, std::memory_order_release);   // the flush path (another thread)
			const uint64_t frameGeneration = f.frameGeneration, sourceSequence = f.sourceSequence;
			const uint64_t nextSourceSequence = f.nextSourceSequence;
			const double nextPts = f.nextPts, sourcePts = f.sourcePts, frameFilmHz = f.frameFilmHz;
			const double captureRateHz = f.captureRateHz;
			const bool currentFileFrame = f.currentFileFrame;
			const size_t queueDepthAfterDequeue = f.queueDepthAfterDequeue, desiredQueueDepth = f.desiredQueueDepth;
			logs.clear();
			fallback.clear();
			motionOutputs = f.temporalOutputs;   // COPY (Bill): auto motionOutputs=m_impl->temporalRenderer.outputs;
			// Stage B: VP's per-vsync timeline (exclusive with smooth motion, never in low
			// latency): this frame's refreshes, each at its true phase between A and B.
			vpPhases.clear();
			// R3 (resync_event on): a continuity slip or a timing event re-anchors the timeline's
			// source clock; a FULL restarts it through the generation (VpResync::ReaderGeneration).
			if (f.reanchorBits)   // COPY: TakeIfDriving(...) & (continuity | timing)
				vpTimeline.Reanchor();
			vpTimelineActive=false;
			vpSkipOutput=false;
			vpOutputPhase=0.0;
			if(!f.temporalActive&&VpTimelineWanted()) {
				const bool film=frameFilmHz>0.0&&f.filmArrivalSeconds>0.0;   // COPY: arrivalQpc>0 && QPF
				const double ptsA=film?f.filmArrivalSeconds:sourcePts;
				const double rateHz=frameFilmHz>0.0?frameFilmHz:captureRateHz;
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
						f.queueTimingFixes,   // COPY: m_queueTimingFixes.load(relaxed)
						f.holdFraction);      // COPY: CurrentQueueHold().fraction
				vpTimeline.SetRateTrim(currentFileFrame?1.0:vpQueueSteering.RateTrim());
				vpTimeline.SetSpanInTime(activeSettings.generalRateFixes);   // fixed-constant batch 2026-10-18
				const unsigned long long updatesBefore=vpTimeline.Updates();
				const auto& planned=vpTimeline.Plan(f.readerGeneration,   // COPY: VpResync::ReaderGeneration(...)
					sourceSequence,ptsA,nextSourceSequence?nextPts:0.0,
					rateHz,f.timelineDisplayHz,f.discontinuity);   // COPY: TimelineDisplayHz(), IsSourceDiscontinuity()
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
				LogVpTimeline(vpTimelineActive?"engaged (one picture per display refresh)":"released");
			}
			// Bill merge (c-item 11): Bill's display repeat cadence (present_every_vsync) and VP's
			// per-vsync timeline or de-judder extras would both schedule refreshes: VP's own owns
			// them while active (VP-V only: the Original pathway has neither), visibly.
			vpSchedulesRefreshes=vpTimelineActive ||
				(dejudderReason==DejudderPolicy::Reason::Active && dejudderExtrasAllowed);
			const bool requestedRepeatCadence=!f.recoveryFallback &&
				activeSettings.presentEveryVsync && !f.temporalMixing;   // COPY: PresentationConfig::UseRepeatCadence (requested && !mixing)
			effectiveRepeatCadence=requestedRepeatCadence && !vpSchedulesRefreshes;
			if (requestedRepeatCadence && vpSchedulesRefreshes)
				EngageFallback("display repeat", "present_every_vsync paused: VP's per-refresh timeline / de-judder extras schedule the refreshes");
			else
				ReleaseFallback("display repeat");
			const int cadenceOwner=f.temporalMixing?2:effectiveRepeatCadence?1:0;   // Bill
			if (cadenceOwner!=displayCadenceOwner) {
				displayCadenceOwner=cadenceOwner;
				Log("VP display cadence owner: requested_every_vsync=%d effective_every_vsync=%d smooth_motion_mixing=%d owner=%s",
					activeSettings.presentEveryVsync?1:0,effectiveRepeatCadence?1:0,
					f.temporalMixing?1:0,cadenceOwner==2?"smooth-motion":cadenceOwner==1?"display-repeat":"source");
			}
		}

		// One trace line per frame (exact: %.17g).
		std::string TraceLine(const Frame& f) const
		{
			char head[256];
			std::snprintf(head, sizeof(head), "seq=%llu act=%d skip=%d upd=%d cap=%.17g trim=%.17g sched=%d rep=%d fb=%s out=",
				static_cast<unsigned long long>(f.sourceSequence), vpTimelineActive ? 1 : 0, vpSkipOutput ? 1 : 0,
				vpSkipIsUpdate ? 1 : 0, vpCaptureRateHz, vpTimeline.RateTrim(), vpSchedulesRefreshes ? 1 : 0,
				effectiveRepeatCadence ? 1 : 0, fallback.c_str());
			std::string line = head;
			for (size_t i = 0; i < motionOutputs.size(); ++i)
			{
				char item[96];
				if (i < vpPhases.size())
					std::snprintf(item, sizeof(item), "%s%.17g@%.17g", i ? "," : "", motionOutputs[i], vpPhases[i]);
				else
					std::snprintf(item, sizeof(item), "%s%.17g", i ? "," : "", motionOutputs[i]);
				line += item;
			}
			for (const std::string& l : logs)
				line += " | " + l;
			return line;
		}
	};
}
