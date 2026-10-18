#pragma once

#include <cmath>
#include <cstddef>

#include "GeneralRate.h"

// Stage 5: a queue target expressed as time. target_latency_ms is turned into
// the existing frame count (target_frames) for the current source rate and
// handed to the unchanged frame-count pacing; nothing here is a presentation
// policy. Profiles without the key behave exactly as before.
namespace QueueLatency
{
	// Same ceiling as target_frames in the configuration grammar.
	constexpr size_t MaxTargetFrames = 16;
	constexpr int MaxTargetLatencyMs = 1000;

	// Fixed-constant batch B3 (2026-10-18, sweep #6, general_rate_fixes): the ceiling of a time
	// target. MaxTargetFrames at and below 60 Hz (unchanged); above it the same time (16 frames at
	// 60 Hz = 267 ms: 32 at 120 Hz, 64 at 240 Hz), so a 120p time target is not silently cut to a
	// quarter of the 24p one. The queue capacity still bounds it (ClampToCapacity). Off: 16.
	inline size_t MaxFramesForLatency(double rateHz, bool general = true)
	{
		return GeneralRate::FramesWithFloor(MaxTargetFrames, rateHz, general);
	}

	// ceil(ms x rate), at least one frame and at most MaxFramesForLatency. A rate
	// that lands a hair above a whole frame (23.976 x 125.1 ms) is not pushed
	// up a frame by floating-point noise.
	inline size_t FramesForLatency(int latencyMs, double rateHz, bool general = true)
	{
		if (latencyMs <= 0 || !(rateHz > 0.0))
			return 1;
		const double exact = static_cast<double>(latencyMs) * rateHz / 1000.0;
		double frames = std::ceil(exact - 1e-6);
		if (frames < 1.0) frames = 1.0;
		const double ceiling = static_cast<double>(MaxFramesForLatency(rateHz, general));
		if (frames > ceiling)
			frames = ceiling;
		return static_cast<size_t>(frames);
	}

	// The delay that frame count really gives (whole source frames).
	inline double EffectiveLatencyMs(size_t frames, double rateHz)
	{
		return rateHz > 0.0 ? static_cast<double>(frames) * 1000.0 / rateHz : 0.0;
	}

	struct TargetDecision
	{
		size_t frames = 0;
		bool fromLatency = false;   // derived from target_latency_ms
		int latencyMs = 0;          // the configured time target when derived
		double rateHz = 0.0;        // the source rate it was derived for
		double effectiveMs = 0.0;   // frames as time (0 when the rate is unknown)
		bool raisedToMinimum = false;   // a time target below the frame minimum was raised to it
	};

	// Precedence, most specific first:
	//   1. the profile's own target_latency_ms (> 0);
	//   2. the profile's own target_frames - also when the profile says
	//      target_latency_ms: 0, which switches an inherited time target off;
	//   3. the default queue's target_latency_ms (> 0), inherited;
	//   4. the default queue's target_frames.
	// So Low_Latency's own "target_frames: 1" beats a time target it would
	// otherwise inherit from the default queue. (Queue variants copy the
	// default's settings; the profile model drops an inherited
	// target_latency_ms from a variant with its own target_frames, so the
	// "own" values here are already resolved.) Until the source rate is known
	// a time target falls back to the frame count it would otherwise use.
	inline TargetDecision ResolveTargetFrames(
		bool ownHasLatency, int ownLatencyMs,
		bool ownHasFrames, size_t ownFrames,
		int baseLatencyMs, size_t baseFrames, double rateHz, bool general = true)
	{
		TargetDecision decision;
		decision.rateHz = rateHz;
		int latency = 0;
		size_t frames = baseFrames;
		if (ownHasLatency && ownLatencyMs > 0)
			latency = ownLatencyMs;
		else if (ownHasFrames)
			frames = ownFrames;
		else if (!ownHasLatency && baseLatencyMs > 0)
			latency = baseLatencyMs;
		if (latency > 0 && rateHz > 0.0)
		{
			decision.frames = FramesForLatency(latency, rateHz, general);
			decision.fromLatency = true;
			decision.latencyMs = latency;
		}
		else
		{
			// An unknown rate keeps the frame count this profile would use
			// without the time target.
			decision.frames = latency > 0 && ownHasFrames ? ownFrames : frames;
		}
		decision.effectiveMs = EffectiveLatencyMs(decision.frames, rateHz);
		return decision;
	}

	// Owner 2026-10-16 ("make sure the queue by set time aligns at a minimum"): a
	// queue set by time never holds fewer frames than `minimumFrames` - the motion
	// vectors' look-ahead (docs/VP-nvof-ahead.md: 4 ahead = 5 queued). Low latency
	// is exempt (the caller passes 0), and an explicit target_frames is the
	// operator's choice: only time-derived targets are raised.
	// Owner 2026-10-16 (later): the look-ahead is on or off, and on needs 4 frames ahead - so with
	// `explicitToo` an explicit target_frames is raised as well.
	inline TargetDecision ApplyFrameMinimum(TargetDecision decision, size_t minimumFrames, bool explicitToo = false)
	{
		if ((decision.fromLatency || explicitToo) && minimumFrames > 0 && decision.frames < minimumFrames)
		{
			decision.frames = minimumFrames;
			decision.raisedToMinimum = true;
			decision.effectiveMs = EffectiveLatencyMs(decision.frames, decision.rateHz);
		}
		return decision;
	}

	// audit 2026-10-16: the target can never exceed the queue capacity - a
	// look-ahead minimum above queue_size would otherwise ask for frames the
	// queue cannot hold. capacity 0 (unknown) leaves the target alone.
	inline TargetDecision ClampToCapacity(TargetDecision decision, size_t capacity, bool* clamped = nullptr)
	{
		const bool over = capacity > 0 && decision.frames > capacity;
		if (over)
		{
			decision.frames = capacity;
			decision.effectiveMs = EffectiveLatencyMs(decision.frames, decision.rateHz);
		}
		if (clamped)
			*clamped = over;
		return decision;
	}
}
