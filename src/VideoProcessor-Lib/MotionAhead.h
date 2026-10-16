#pragma once

#include <algorithm>
#include <cstddef>

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

	enum class Reason
	{
		Active,         // the configured depth, fully queued
		ShallowQueue,   // fewer frames queued than configured: what is there
		NoFramesAhead,  // nothing queued after the current frame
		Off,            // motion_ahead_frames: 0
		LowLatency,     // a low-latency profile
		NoEngine,       // no optical-flow engine (not NVIDIA, failed self-check, not built yet)
	};

	struct Decision
	{
		int configured = kDefaultDepth;
		int available = 0;
		int depth = 0;
		Reason reason = Reason::NoEngine;
	};

	inline Decision Decide(int configured, bool lowLatency, bool engineAvailable, size_t availableFutureFrames)
	{
		Decision d;
		d.configured = (std::max)(0, (std::min)(kMaxDepth, configured));
		d.available = static_cast<int>((std::min)(availableFutureFrames, static_cast<size_t>(1000)));
		if (d.configured == 0) d.reason = Reason::Off;
		else if (lowLatency) d.reason = Reason::LowLatency;
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
		case Reason::LowLatency: return "off in low latency";
		case Reason::NoEngine: return "no optical-flow engine (the matcher runs)";
		}
		return "?";
	}

	// The queue (current frame plus the frames after it) that holds `depth` ahead.
	inline size_t QueueFramesFor(int depth) { return static_cast<size_t>((std::max)(0, depth)) + 1; }
}
