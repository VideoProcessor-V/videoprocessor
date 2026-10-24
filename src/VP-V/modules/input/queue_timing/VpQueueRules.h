#pragma once

// VP's queue rules, MOVED VERBATIM (2026-10-24 draft, commit a) out of upstream's
// vprenderer/AlphaQueuePolicy.h, where VP had added them since 83b7ffea. VP-ONLY.
// With this move AlphaQueuePolicy.h returns byte-for-byte to upstream's file, so an upstream merge
// takes his version without conflict (the forecast's 2 hunks / 212 lines there go away).
//
// Function bodies are unchanged text; the only additions are this header, the namespace, the two
// using-declarations for upstream's depth/rate helpers and VP's OWN copy of the two thresholds
// the VP backlog rule uses (so upstream's later threshold changes - e.g. v1.4.02's depth-scaled
// stale age - change only the upstream policy, never VP's).
// Callers that still say AlphaQueuePolicy::X get these through AlphaQueuePolicyVpCompat.h.

#include <vprenderer/AlphaQueuePolicy.h>
#include <DisplayRefreshRatePolicy.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace VpQueueRules
{
	using AlphaQueuePolicy::NormalizeDesiredDepth;
	using AlphaQueuePolicy::ValidFrameRateOrDefault;
	using AlphaQueuePolicy::ActivePicturePreviewWindow;
	using AlphaQueuePolicy::SelectActivePicturePreview;
	constexpr double MINIMUM_RENDER_STALL_MS = 50.0;          // = upstream's at 83b7ffea
	constexpr double MINIMUM_STALE_BACKLOG_AGE_MS = 100.0;    // = upstream's at 83b7ffea

	// VP's copies of the 83b7ffea thresholds (identical text).
	inline double RenderStallThresholdMs(double frameRateHz)
	{
		const double framePeriodMs =
			1000.0 / ValidFrameRateOrDefault(frameRateHz);
		return std::max(MINIMUM_RENDER_STALL_MS, framePeriodMs * 2.0);
	}

	inline double StaleBacklogAgeThresholdMs(double frameRateHz)
	{
		const double framePeriodMs =
			1000.0 / ValidFrameRateOrDefault(frameRateHz);
		return std::max(MINIMUM_STALE_BACKLOG_AGE_MS, framePeriodMs * 3.0);
	}

	// led_lookahead_ms (2026-10-17): the queued-frame walk visits
	// max(active_picture_lookahead_frames, the LED's wanted depth) future frames,
	// bounded by what is queued. Only the first active-picture count (the current
	// frame and its configured look-ahead) goes to the active-picture decision; the
	// statistics observer (the LED) sees every frame walked. The walk runs for the
	// LED alone when the active-picture look-ahead is 0. With the LED wanting no
	// more than the active picture, this is exactly SelectActivePicturePreview.
	struct LookaheadWalk
	{
		ActivePicturePreviewWindow window;   // every frame walked (indices), current first
		size_t activePictureFrames = 0;      // the leading frames of window for the active picture
	};

	template<typename Queue>
	LookaheadWalk SelectLookaheadWalk(const Queue& queue, size_t activePictureConfigured,
		size_t ledWanted, size_t maxActivePicture)
	{
		LookaheadWalk walk;
		const size_t activePicture = (std::min)(activePictureConfigured, maxActivePicture);
		const size_t depth = (std::max)(activePicture, ledWanted);
		walk.window = SelectActivePicturePreview(queue, depth, (std::max)(maxActivePicture, ledWanted));
		walk.activePictureFrames = activePicture == 0 ? 0 :
			(std::min)(walk.window.indices.size(), activePicture + 1);
		return walk;
	}
	// The queue target the renderer steers to (PrefillTargetLocked; the stress harness uses the
	// same function, so the delay gate tests the real rule). desired = the delay budget's whole
	// hold frames when a budget is active (time-based already), else the (film-scaled) frame-count
	// target. render_ahead_queue_credit gives back frames, never below 2; with queue_timing_fixes the
	// credit never takes the target below the look-ahead floor - but never above `desired`, so a
	// short delay budget is never exceeded (the look-ahead uses what is queued).
	inline size_t CombineQueueTarget(size_t desired, size_t credit, size_t lookaheadFloor, bool queueTimingFixes)
	{
		const size_t target = credit > 0 && desired > 2 ? desired - (std::min)(credit, desired - 2) : desired;
		const size_t floor = queueTimingFixes ? (std::min)(desired, lookaheadFloor) : 0;
		return (std::max)(target, floor);
	}

	// Hunt 2 13-timing H1 (queue_timing_fixes): the front frame of a queue that
	// holds its target has already waited about target periods; that wait is the
	// queue doing its job, not staleness. With staleBeyondTarget the age is measured
	// beyond the target's own wait, so target+1 and target+2 (arrival jitter, the
	// timeline steering's margin, a ppm step) are left to the steering and the
	// cadence correction, and only a real backlog (about 3 periods or 100 ms more
	// than the target holds) is flushed. Off: the age from zero (the old rule, which
	// at 24-50 Hz flushed every depth above the target).
	inline double StaleBacklogExcessAgeMs(double oldestQueuedAgeMs,
		size_t desiredDepth, double frameRateHz)
	{
		const double framePeriodMs =
			1000.0 / ValidFrameRateOrDefault(frameRateHz);
		return oldestQueuedAgeMs -
			static_cast<double>(NormalizeDesiredDepth(desiredDepth)) * framePeriodMs;
	}

	// The rate of the frames the queue holds (hunt 2 13-timing H1): while a pulldown
	// lock holds they are film frames (frontFilmHz, ~41.7 ms apart at 3:2 on a 60 Hz
	// signal); otherwise the signal's frames. Judging a film queue with the signal's
	// period would call target+1 stale again.
	inline double QueueFrameRateHz(double frontFilmHz, double signalRateHz)
	{
		return frontFilmHz > 0.0 && std::isfinite(frontFilmHz) ? frontFilmHz : signalRateHz;
	}

	inline bool ShouldRecoverBacklog(size_t queueDepth, size_t desiredDepth,
		double oldestQueuedAgeMs, double renderCycleMs, double frameRateHz,
		bool staleBeyondTarget = false)
	{
		if (queueDepth <= NormalizeDesiredDepth(desiredDepth))
			return false;
		const double staleAgeMs = staleBeyondTarget ?
			StaleBacklogExcessAgeMs(oldestQueuedAgeMs, desiredDepth, frameRateHz) :
			oldestQueuedAgeMs;
		return renderCycleMs >= RenderStallThresholdMs(frameRateHz) ||
			staleAgeMs >= StaleBacklogAgeThresholdMs(frameRateHz);
	}

	// VP-V delay budget (gate 5, 2026-10-24): with a time hold the head waits holdMs by design, so
	// the stale-age rule above (100 ms beyond the frame-count target) left up to ~100 ms of backlog
	// in place after a start-up or render stall - the total delay stayed above the budget. With a
	// hold, a backlog is a head older than the hold by more than two frames; it is shed to target.
	inline bool ShouldShedHeldBacklog(size_t queueDepth, size_t desiredDepth, double headAgeMs, double holdMs,
		double frameRateHz)
	{
		if (!(holdMs > 0.0) || queueDepth <= NormalizeDesiredDepth(desiredDepth))
			return false;
		const double periodMs = 1000.0 / ValidFrameRateOrDefault(frameRateHz);
		return headAgeMs > holdMs + 2.0 * periodMs;
	}

	// One frame behind for good (gate 7): when the source rate equals the display's (60p on 60 Hz)
	// a frame the queue gained (an early burst, a stall) never drains: the total stays a frame above
	// the budget. A head older than the hold by more than 1.25 frames for half a second in a row
	// (jitter never lasts) is that frame: shed it too. `run` counts the consecutive late heads.
	// Audit MED 6: also by depth - at a locked 60p on 60 Hz with a small vblank phase the head's age
	// cannot tell an extra frame (it is due at once); the queue then holds a frame more than the
	// hold's own frames (ceil(hold / period)) plus the one arriving.
	// queueHoldMs: the part of holdMs the frames wait IN the queue (after their transfer; < 0: all).
	inline bool ShouldShedHeldExcess(size_t queueDepth, size_t desiredDepth, double headAgeMs, double holdMs,
		double frameRateHz, unsigned& run, double queueHoldMs = -1.0)
	{
		const double periodMs = 1000.0 / ValidFrameRateOrDefault(frameRateHz);
		const double inQueueMs = queueHoldMs >= 0.0 ? queueHoldMs : holdMs;
		const size_t holdFrames = inQueueMs > 0.0 ? static_cast<size_t>(std::ceil(inQueueMs / periodMs - 1e-9)) : 0;
		const bool deep = holdMs > 0.0 && queueDepth > holdFrames + 1;
		const bool late = holdMs > 0.0 && queueDepth > NormalizeDesiredDepth(desiredDepth) &&
			(headAgeMs > holdMs + 1.25 * periodMs || deep);
		run = late ? run + 1 : 0;
		const unsigned needed = (std::max)(8u, static_cast<unsigned>(std::lround(0.5 * ValidFrameRateOrDefault(frameRateHz))));
		if (run < needed)
			return false;
		run = 0;
		return true;
	}

	// How many frames a held shed drops (audit MED 6): the frames the head is late by (at least
	// one), never below the hold's whole frames - not the flush-to-target of a frame-count queue,
	// which would undershoot the budget and stutter.
	inline size_t HeldShedFrames(size_t queueDepth, size_t desiredDepth, double headAgeMs, double holdMs, double frameRateHz)
	{
		const double periodMs = 1000.0 / ValidFrameRateOrDefault(frameRateHz);
		const double lateFrames = (headAgeMs - holdMs) / periodMs;
		const size_t want = lateFrames >= 2.0 ? static_cast<size_t>(std::floor(lateFrames)) : 1;
		const size_t target = NormalizeDesiredDepth(desiredDepth);
		const size_t room = queueDepth > target ? queueDepth - target : 0;
		return (std::min)(want, room);
	}

	// The VP timeline's queue steering (hunt 2 13-timing H2, L3). The refresh
	// spacing is trimmed by the smoothed distance of the queue (after the dequeue)
	// from its target, at most 0.3%. The target after the dequeue keeps one frame of
	// margin above the dequeue gate (HealthyLowWater), so arrival jitter never
	// reaches the gate; H1's beyond-target staleness keeps that margin from being
	// flushed. With timeConstant the smoothing is a time constant in seconds
	// converted with the source rate (the old weight, 0.05 per frame, was 0.8 s at
	// 24p and 0.17 s at 120p); off, the old per-frame weight.
	class TimelineQueueSteering
	{
	public:
		static constexpr double kSmoothingSeconds = 0.8;   // the old weight at 24p
		static constexpr double kLegacyWeight = 0.05;
		static constexpr double kGain = 0.0005;            // trim per frame of error
		static constexpr double kMaximumTrim = 0.003;

		void Reset() { m_error = 0.0; }

		static double Weight(double sourceRateHz, bool timeConstant)
		{
			if (!timeConstant || !CaptureRateBounds::Accepts(sourceRateHz))
				return kLegacyWeight;
			return 1.0 - std::exp(-1.0 / (sourceRateHz * kSmoothingSeconds));
		}

		// targetFraction (VP-V delay budget, ADR-0020): the sub-frame part of the queue's time
		// hold (DelayBudget::Plan::holdFraction), so the queue settles at desired + fraction
		// frames on average and the total delay equals the budget at every rate. 0 = as before.
		double Update(size_t queueDepthAfterDequeue, size_t desiredDepth,
			double sourceRateHz, bool timeConstant, double targetFraction = 0.0)
		{
			const double sample = static_cast<double>(queueDepthAfterDequeue) -
				(static_cast<double>(NormalizeDesiredDepth(desiredDepth)) + targetFraction);
			m_error += Weight(sourceRateHz, timeConstant) * (sample - m_error);
			return m_error;
		}

		double Error() const { return m_error; }

		// Below 1: more refreshes per source frame, so the queue fills.
		double RateTrim() const
		{
			return 1.0 + (std::max)(-kMaximumTrim, (std::min)(kMaximumTrim, kGain * m_error));
		}

	private:
		double m_error = 0.0;
	};

	// A queue-overflow drop hands its gap to the next frame (hunt 2 13-timing L1).
	// gapFrames 0 with gapBefore is a no-fill gap (a timestamp break, a pulldown
	// lock change, a backlog flush): it must survive the drop, so no gap-fill pair
	// spans it. Otherwise the missed frames add up: the dropped frame's own missed
	// frames, the dropped frame itself and the successor's missed frames. Without
	// mergeDroppedGap: the old rule (only +1, the dropped frame's own state ignored).
	struct GapState
	{
		bool gapBefore = false;
		uint32_t gapFrames = 0;
	};

	inline GapState MergeDroppedGap(const GapState& dropped, const GapState& next,
		bool mergeDroppedGap)
	{
		GapState merged = next;
		if (!mergeDroppedGap)
		{
			if (!next.gapBefore || next.gapFrames > 0)
				merged.gapFrames = next.gapFrames + 1;
			merged.gapBefore = true;
			return merged;
		}
		const bool noFill = (dropped.gapBefore && dropped.gapFrames == 0) ||
			(next.gapBefore && next.gapFrames == 0);
		merged.gapBefore = true;
		merged.gapFrames = noFill ? 0 :
			(dropped.gapBefore ? dropped.gapFrames : 0) + (next.gapBefore ? next.gapFrames : 0) + 1;
		return merged;
	}

	// The early-upload pool's slots (hunt 2 13-timing L4): the frames in flight
	// follow the queue target (target_frames up to 16), not a fixed 8. In flight: the
	// queue at its target plus the backlog it tolerates before a flush (3), and next,
	// ahead and the frame being filled (3); a margin of 3 (owner 2026-10-15). At the
	// default target (5) this is the old 8 + 3. The pool stays capped by bytes.
	inline int EarlyUploadSlots(size_t desiredDepth)
	{
		const size_t inFlight = (std::max)(size_t{ 8 }, NormalizeDesiredDepth(desiredDepth) + 3);
		return static_cast<int>((std::min)(inFlight, size_t{ 64 })) + 3;
	}

	// Consecutive failures that mark the renderer failed (hunt 2 13-timing L3): a
	// time, converted with the source rate (300 frames was 12.5 s at 24p and 2.5 s
	// at 120p); 300 without the fixes or without a valid rate.
	constexpr double RENDER_FAILURE_SECONDS = 5.0;
	inline uint32_t ConsecutiveFailureLimit(double frameRateHz, bool timeBased)
	{
		if (!timeBased || !CaptureRateBounds::Accepts(frameRateHz))
			return 300;
		return static_cast<uint32_t>((std::max)(1.0, std::ceil(RENDER_FAILURE_SECONDS * frameRateHz)));
	}

	// Frames for a time at a source rate (at least 1); legacy when not time based.
	inline size_t FramesForSeconds(double seconds, double frameRateHz, size_t legacy, bool timeBased)
	{
		if (!timeBased || !CaptureRateBounds::Accepts(frameRateHz))
			return legacy;
		return static_cast<size_t>((std::max)(1.0, std::ceil(seconds * frameRateHz)));
	}

}
