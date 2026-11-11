#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <GeneralRate.h>

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
// B1 (2026-11-10): the frames really computed ahead are the LEAD (LeadFor below): every frame
// queued ahead, at least the depth, at most kMaxLead. motion_ahead_frames is now the minimum
// (queue floor + engage threshold; 0 = off); it no longer bounds the lead.
namespace MotionAhead
{
	constexpr int kMaxDepth = 4;
	constexpr int kDefaultDepth = 4;
	// HISTORY (superseded by r6 fix 3 below; kept for kMaxGeneralDepth):
	// Fixed-constant batch B3 (2026-10-18, sweep #7, general_rate_fixes): the depth is a time with
	// a frame floor. motion_ahead_frames (0..4) is the depth at and below 60 Hz, unchanged (4 frames:
	// 167 ms at 24p, 67 ms at 60p); above 60 Hz the depth keeps the 60 Hz time (4 frames at 60 Hz =
	// 66.7 ms: 8 at 120 Hz, 16 at 240 Hz) instead of shrinking to 33 / 17 ms. kMaxGeneralDepth bounds
	// it (the 60 Hz time at 240 Hz); the queue still bounds what is really used (ShallowQueue).
	constexpr int kMaxGeneralDepth = 16;
	// r6 fix 3 (2026-10-24, look-ahead inventory item 41): the depth is a TIME at every rate. The
	// configured depth is frames at the 60 Hz reference (4 = 66.7 ms); a source rate turns it into
	// whole frames covering at least that time, at least one: 23.976/24/25/30p 2, 50p 4, 59.94/60p 4,
	// 120p 8, 240p 16 (kMaxGeneralDepth). Before, 24p held 4 (167 ms) and pushed the queue past the
	// 200 ms budget. An unmeasured rate keeps the configured frames, and general_rate_fixes off the
	// legacy count; either way the queue bounds what is really used (Decide: ShallowQueue).
	inline double AheadMs(int configured)
	{
		return static_cast<double>((std::max)(0, (std::min)(kMaxDepth, configured))) * 1000.0 / GeneralRate::kFrameCountReferenceHz;
	}
	inline int DepthAt(int configured, double sourceHz, bool general = true)
	{
		const int base = (std::max)(0, (std::min)(kMaxDepth, configured));
		if (base == 0)
			return 0;
		if (!general || !GeneralRate::Measured(sourceHz))
			return base;
		const double exact = AheadMs(base) * sourceHz / 1000.0;
		const double timed = std::ceil(exact * (1.0 - GeneralRate::kRateTolerance));
		return static_cast<int>((std::max)(1.0, (std::min)(static_cast<double>(kMaxGeneralDepth), timed)));
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
		BackwardPairs,  // low latency, no frame ahead: backward pairs on arrival (Vector engine on demand)
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
		bool consumer = true, bool backwardPairs = false)
	{
		Decision d;
		// `configured`: DepthAt(motion_ahead_frames, rate) - motion_ahead_frames itself at 60 Hz and below.
		d.configured = (std::max)(0, (std::min)(kMaxGeneralDepth, configured));
		d.available = static_cast<int>((std::min)(availableFutureFrames, static_cast<size_t>(1000)));
		if (!opticalFlow) d.reason = Reason::Disabled;
		else if (d.configured == 0) d.reason = Reason::Off;
		else if (lowLatency && backwardPairs && consumer && engineAvailable) d.reason = Reason::BackwardPairs;
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
		case Reason::BackwardPairs: return "backward pairs on arrival (no frame ahead)";
		}
		return "?";
	}

	// B1 (owner 2026-11-10; doc 91 C "Precompute lane runs the FULL queue depth, tied dynamically"): the LEAD, the frames the walk
	// actually hands the producer ahead of the render, is every frame queued after the current one, whatever made the queue that deep
	// (queue mode By frames or By time; the queue target is not changed here). The measurement is the render loop's one count of the
	// non-repeat frames queued after the current one (window.availableFutureFrames -> queuedFutureFrames, the same count Decide's
	// `available` reads); no second measurement. `depth` (Decide, DepthAt of motion_ahead_frames) keeps its meaning and every reader
	// of it (the queue floor m_queueTargetFloor / LockedFilmQueueTarget, the engage threshold, the no-pair tracker) is unchanged: it is
	// the MINIMUM the queue is kept deep enough for, never the source of the lead. Reading the lead there instead would feed the queue
	// size back into its own floor (a ratchet: the queue could never shrink).
	// The one cap is structural, not a setting: the P2 slot ring holds kP2RingPairs pairs (VectorEngineState::p2Slot and the
	// standalone Producer::slot, both 16) and keeps kPairsKeptBehind of them for frames already at the render (the current pair (N-1, N)
	// and the two Prune keeps behind it: VectorEngine.cpp P2().Prune(sequence - 2)); a lead above the rest would evict the pairs of the
	// next frames to show before they are read (NVOF work thrown away, a matcher run instead). 13 = 217 ms at 60p, 542 ms at 24p.
	constexpr int kP2RingPairs = 16;
	constexpr int kPairsKeptBehind = 3;
	constexpr int kMaxLead = kP2RingPairs - kPairsKeptBehind;
	// The lead for a frame: 0 while the look-ahead is not running (depth 0: off, low latency, no engine, nothing queued; backward pairs
	// keep their own current-frame-only rule in the walk), else the queued frames ahead up to kMaxLead and never below the depth (a
	// rate above 60 Hz keeps its time-based depth, e.g. 16 at 240p). Where the queue held exactly the depth (60p with 4 queued ahead)
	// the lead is the depth: the old behaviour.
	inline int LeadFor(int depth, size_t queuedAhead)
	{
		if (depth <= 0)
			return 0;
		const int queued = static_cast<int>((std::min)(queuedAhead, static_cast<size_t>(kMaxLead)));
		return (std::max)(depth, queued);
	}

	// The queue (current frame plus the frames after it) that holds `depth` ahead.
	inline size_t QueueFramesFor(int depth) { return static_cast<size_t>((std::max)(0, depth)) + 1; }
	// The producer's pending submissions before the oldest is dropped: 6 at the default depth
	// (unchanged), the depth plus the current frame plus one above it. B1: the producer sizes it for
	// the larger of the depth and the lead (Producer::SetDepth / SetLead), so a walk that hands it the
	// whole queue at an engage (up to lead + 1 frames in one pass) drops nothing.
	inline size_t PendingJobsFor(int depth) { return (std::max)(size_t{ 6 }, QueueFramesFor(depth) + 1); }
}
