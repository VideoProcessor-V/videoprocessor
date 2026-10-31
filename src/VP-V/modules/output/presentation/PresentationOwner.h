// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Presentation owner (Bill v1.4.02 merge, owner decision 2, 2026-10-31): Bill's presentation queue
// (presentation_queue_depth, present_every_vsync, the device max_frame_latency he verifies) and
// VP-V's render-ahead (its chain depth and latency, the delay budget's swapchain share) are an
// EITHER/OR keyed on render_ahead:
//   render_ahead off (always on the Original pathway) -> Bill's presentation exactly;
//   render_ahead on                                   -> VP-V's render-ahead owns presentation:
//                                                        Bill's every-vsync repeat cadence is not
//                                                        used (shown under Fallbacks engaged, never
//                                                        silent), the chain depth/latency are VP's.
// The delay budget counts the swapchain frames of whichever owner presents (the configured depth
// when Bill's presentation runs), never a fixed number.
//
// Header-only, C++14, no Windows types (CPU-testable).
namespace PresentationOwner
{
	struct Decision
	{
		bool vpOwns = false;                 // render-ahead owns presentation
		bool presentEveryVsync = false;      // Bill's display repeat cadence, as it may run
		bool presentEveryVsyncWithheld = false;   // requested, but render-ahead owns presentation
	};

	inline Decision Decide(bool renderAhead, bool presentEveryVsyncRequested)
	{
		Decision d;
		d.vpOwns = renderAhead;
		d.presentEveryVsync = presentEveryVsyncRequested && !renderAhead;
		d.presentEveryVsyncWithheld = presentEveryVsyncRequested && renderAhead;
		return d;
	}

	// The device-wide maximum frame latency for the VP-owned chain: VP's chain latency while
	// render-ahead is active, otherwise the presentation depth Bill's code requested and verified.
	inline unsigned MaximumFrameLatency(bool renderAheadActive, unsigned vpChainLatency,
		unsigned appliedPresentationDepth)
	{
		if (renderAheadActive) return vpChainLatency;
		return appliedPresentationDepth > 0 ? appliedPresentationDepth : vpChainLatency;
	}

	// The swapchain frames inside the total delay (DelayBudget::Model::swapchainFrames).
	inline int DelaySwapchainFrames(bool renderAheadActive, unsigned vpChainLatency,
		unsigned appliedPresentationDepth)
	{
		return static_cast<int>(MaximumFrameLatency(renderAheadActive, vpChainLatency, appliedPresentationDepth));
	}

	inline const char* FallbackId() { return "presentation owner"; }
	inline const char* WithheldText()
	{
		return "present_every_vsync not used: render-ahead owns presentation (either/or; render_ahead off = Bill's presentation)";
	}
}
