#pragma once

#include <DisplayRefreshRatePolicy.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace AlphaQueuePolicy
{
	constexpr size_t DEFAULT_DESIRED_DEPTH = 4;
	constexpr double DEFAULT_FRAME_RATE_HZ = 60.0;
	constexpr double MINIMUM_RENDER_STALL_MS = 50.0;
	constexpr double MINIMUM_STALE_BACKLOG_AGE_MS = 100.0;

	struct ActivePicturePreviewWindow
	{
		size_t availableFutureFrames = 0;
		size_t effectiveFutureFrames = 0;
		std::vector<size_t> indices;
	};

	// Snapshot selection only: never changes queue depth or waits for evidence.
	// Each queue entry exposes cadenceRepeat; repeats cannot add proof votes.
	template<typename Queue>
	ActivePicturePreviewWindow SelectActivePicturePreview(const Queue& queue,
		size_t configured, size_t maxLookahead)
	{
		ActivePicturePreviewWindow result;
		if (queue.empty()) return result;
		const size_t requested = (std::min)(configured, maxLookahead);
		const bool inspect = requested > 0 && !queue.front().cadenceRepeat;
		for (size_t index = 0; index < queue.size(); ++index)
		{
			if (queue[index].cadenceRepeat) continue;
			// Count the complete buffered source window even when work is capped
			// or disabled. The current queue entry is not a future frame.
			if (index != 0) ++result.availableFutureFrames;
			if (inspect && result.indices.size() <= requested)
				result.indices.push_back(index);
		}
		result.effectiveFutureFrames = result.indices.empty() ? 0 : result.indices.size() - 1;
		return result;
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

	inline size_t NormalizeDesiredDepth(size_t value)
	{
		return std::max<size_t>(1, value);
	}

	inline size_t ResolveDesiredDepth(size_t configuredOverride,
		size_t rememberedValue = DEFAULT_DESIRED_DEPTH)
	{
		return configuredOverride > 0 ?
			NormalizeDesiredDepth(configuredOverride) :
			NormalizeDesiredDepth(rememberedValue);
	}

	inline size_t HardCapacity(size_t configuredQueueSize)
	{
		// queue_size is user-visible and is the actual FIFO cap.  A lower
		// steady reserve controls presentation pacing; it must not shrink this.
		return NormalizeDesiredDepth(configuredQueueSize);
	}

	inline size_t ClampDesiredDepthToCapacity(size_t desiredDepth,
		size_t hardCapacity)
	{
		return std::min(NormalizeDesiredDepth(desiredDepth),
			HardCapacity(hardCapacity));
	}

	inline size_t HealthyLowWater(size_t desiredDepth)
	{
		const size_t desired = NormalizeDesiredDepth(desiredDepth);
		return desired > 1 ? desired - 1 : 0;
	}

	inline size_t HealthyHighWater(size_t desiredDepth)
	{
		const size_t desired = NormalizeDesiredDepth(desiredDepth);
		return desired;
	}

	inline bool CanDequeue(size_t queueDepth, size_t desiredDepth,
		bool startupPrefillPending)
	{
		if (queueDepth == 0)
			return false;

		const size_t desired = NormalizeDesiredDepth(desiredDepth);
		return startupPrefillPending ?
			queueDepth >= desired :
			queueDepth > HealthyLowWater(desired);
	}

	inline size_t ConversionPreparationDepth(size_t queueDepth,
		size_t desiredDepth)
	{
		return std::min(queueDepth, NormalizeDesiredDepth(desiredDepth));
	}

	inline bool CanDequeuePrepared(size_t queueDepth, size_t desiredDepth,
		bool startupPrefillPending, bool requiresConversion,
		bool converted, bool conversionFailed)
	{
		return CanDequeue(queueDepth, desiredDepth, startupPrefillPending) &&
			(!requiresConversion || converted || conversionFailed);
	}

	inline double ValidFrameRateOrDefault(double frameRateHz)
	{
		return CaptureRateBounds::Accepts(frameRateHz) ?
			frameRateHz : DEFAULT_FRAME_RATE_HZ;
	}

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

		double Update(size_t queueDepthAfterDequeue, size_t desiredDepth,
			double sourceRateHz, bool timeConstant)
		{
			const double sample = static_cast<double>(queueDepthAfterDequeue) -
				static_cast<double>(NormalizeDesiredDepth(desiredDepth));
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

	inline bool TryParsePositiveSize(const std::string& value, size_t& parsedValue)
	{
		if (value.empty())
			return false;
		for (const char character : value)
		{
			if (character < '0' || character > '9')
				return false;
		}

		errno = 0;
		char* end = nullptr;
		const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
		if (errno != 0 || end == value.c_str() || *end != '\0' || parsed == 0 ||
			parsed > static_cast<unsigned long long>(
				std::numeric_limits<size_t>::max()))
			return false;

		parsedValue = static_cast<size_t>(parsed);
		return true;
	}
}
