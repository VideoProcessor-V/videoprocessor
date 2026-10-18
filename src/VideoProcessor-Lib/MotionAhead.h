#pragma once

#include <algorithm>
#include <cstddef>

#include "GeneralRate.h"

// How far ahead the optical-flow engine works (docs/VP-nvof-ahead.md; owner
// 2026-10-16: "by default accumulate the optical flow vectors to 4 frame max
// depth. Low latency disables. Would ensure baseline queue defaults allow for
// at least a 4 frame lookahead and adjusts when set lower").
//
// The depth used for each frame is the configured depth (motion_ahead_frames,
// 0..4, default 4), off in low latency or without the engine, and never more
// than the source frames actually queued after the current one - a shallower
// queue means fewer pairs ahead, never waiting or added latency. Whatever is
// not computed ahead is computed by the matcher as today.
namespace MotionAhead
{
	constexpr int kMaxDepth = 4;
	constexpr int kDefaultDepth = 4;
	// Fixed-constant batch B3 (2026-10-18, sweep #7, general_rate_fixes): the depth is a time with
	// a frame floor. motion_ahead_frames (0..4) is the depth at and below 60 Hz, unchanged (4 frames:
	// 167 ms at 24p, 67 ms at 60p); above 60 Hz the depth keeps the 60 Hz time (4 frames at 60 Hz =
	// 66.7 ms: 8 at 120 Hz, 16 at 240 Hz) instead of shrinking to 33 / 17 ms. kMaxGeneralDepth bounds
	// it (the 60 Hz time at 240 Hz); the queue still bounds what is really used (ShallowQueue).
	constexpr int kMaxGeneralDepth = 16;
	inline int DepthAt(int configured, double sourceHz, bool general = true)
	{
		const int base = (std::max)(0, (std::min)(kMaxDepth, configured));
		if (base == 0)
			return 0;
		return (std::min)(kMaxGeneralDepth, GeneralRate::FramesWithFloor(base, sourceHz, general));
	}

	enum class Reason
	{
		Active,         // the configured depth, fully queued
		ShallowQueue,   // fewer frames queued than configured: what is there
		NoFramesAhead,  // nothing queued after the current frame
		Off,            // motion_ahead_frames: 0
		Disabled,       // optical_flow: off (the matcher only)
		LowLatency,     // a low-latency profile
		NoEngine,       // no optical-flow engine (not NVIDIA, failed self-check, not built yet)
		NoConsumer,     // neither the de-judder nor the interpolator estimates for this source (idle)
	};

	struct Decision
	{
		int configured = kDefaultDepth;
		int available = 0;
		int depth = 0;
		Reason reason = Reason::NoEngine;
	};

	// `opticalFlow`: the optical_flow setting (owner 2026-10-16: the engine is the
	// established default; optical_flow: off disables it).
	// consumer: a motion estimate would run for this source (the de-judder or the interpolator:
	// DejudderPolicy Active / NoNextFrame / NextFrameDiffers). Without one no frames are submitted
	// (intent audit 2026-10-16: pairs were computed for 60p after film); the producer stays alive, idle.
	inline Decision Decide(int configured, bool lowLatency, bool engineAvailable, size_t availableFutureFrames, bool opticalFlow = true,
		bool consumer = true)
	{
		Decision d;
		// `configured`: DepthAt(motion_ahead_frames, rate) - motion_ahead_frames itself at 60 Hz and below.
		d.configured = (std::max)(0, (std::min)(kMaxGeneralDepth, configured));
		d.available = static_cast<int>((std::min)(availableFutureFrames, static_cast<size_t>(1000)));
		if (!opticalFlow) d.reason = Reason::Disabled;
		else if (d.configured == 0) d.reason = Reason::Off;
		else if (lowLatency) d.reason = Reason::LowLatency;
		else if (!consumer) d.reason = Reason::NoConsumer;
		else if (!engineAvailable) d.reason = Reason::NoEngine;
		else if (d.available == 0) d.reason = Reason::NoFramesAhead;
		else
		{
			d.depth = (std::min)(d.configured, d.available);
			d.reason = d.depth < d.configured ? Reason::ShallowQueue : Reason::Active;
		}
		return d;
	}

	inline const char* Describe(Reason reason)
	{
		switch (reason)
		{
		case Reason::Active: return "active";
		case Reason::ShallowQueue: return "fewer frames queued than configured";
		case Reason::NoFramesAhead: return "no frames queued ahead";
		case Reason::Off: return "off (motion_ahead_frames 0)";
		case Reason::Disabled: return "off (optical_flow off: the matcher runs)";
		case Reason::LowLatency: return "off in low latency";
		case Reason::NoEngine: return "no optical-flow engine (the matcher runs)";
		case Reason::NoConsumer: return "idle (no motion estimate runs for this source)";
		}
		return "?";
	}

	// The queue (current frame plus the frames after it) that holds `depth` ahead.
	inline size_t QueueFramesFor(int depth) { return static_cast<size_t>((std::max)(0, depth)) + 1; }
	// The producer's pending submissions before the oldest is dropped: 6 at the default depth
	// (unchanged), the depth plus the current frame plus one above it.
	inline size_t PendingJobsFor(int depth) { return (std::max)(size_t{ 6 }, QueueFramesFor(depth) + 1); }
}
