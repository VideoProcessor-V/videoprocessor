#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Fixed-constant batch B3 (2026-10-18, sweep #14 / #15, general_rate_fixes; owner rule
// [[vp-general-not-fixed]]): the DirectShow pin's startup buffering target and its delivery
// hold, without the fixed 30 fps step and the 23.976-sized hold. Pure, so the identical defaults
// at 24 and 60 Hz are proven by tests (GeneralRateMatrixTests).
namespace DirectShowRatePolicy
{
	// The startup lead: about 200 ms buffered (5 frames at 23.976 = 208.5 ms), never more than the
	// 5 frames that cover it at film rates.
	constexpr size_t kStartupLeadFrames = 5;
	constexpr double kStartupLeadSeconds = 5.0 * 1001.0 / 24000.0;
	// madVR's buffering floor (both rules).
	constexpr size_t kMinimumStartupFrames = 3;

	// nominalTarget: queue_size / 8. general (with a measured rate): max(nominalTarget + 1, the
	// lead) - with the default queue (32: nominalTarget 4) 5 at every rate, as the legacy rule
	// gave; a low-latency queue is bounded by its capacity downstream. Legacy: below 30 fps
	// max(5, nominalTarget), else nominalTarget + 1.
	inline size_t StartupBufferingFrames(size_t nominalTarget, double fps, bool general)
	{
		size_t frames;
		if (general && std::isfinite(fps) && fps > 0.0)
		{
			const double lead = (std::max)(1.0, std::ceil(kStartupLeadSeconds * fps - 1e-6));
			frames = (std::max)(nominalTarget + 1, (std::min)(kStartupLeadFrames, static_cast<size_t>(lead)));
		}
		else if (fps < 30.0)
		{
			// LOW FPS: At 23.976fps, each frame is ~42ms. With 200ms lead time, we need
			// at least 5 frames buffered (5 * 42ms = 210ms > 200ms lead time)
			// Use nominalTarget directly (no halving) for low fps
			frames = (std::max)(kStartupLeadFrames, nominalTarget);
		}
		else
		{
			frames = nominalTarget + 1;
		}
		// CRITICAL: Ensure minimum of 3 frames for MadVR buffering stability
		return (std::max)(kMinimumStartupFrames, frames);
	}

	// The coherent delivery hold: 50 ms (legacy, sized for one 23.976 interval of 41.7 ms);
	// general: one measured interval plus 15 %, never under 50 ms (so unchanged from 23 fps up)
	// and never over 1 s (a stuck renderer stays bounded).
	constexpr int64_t kDeliveryHoldMinimumUs = 50000;
	inline int64_t DeliveryHoldTimeoutUs(bool general, uint64_t timeScale, uint64_t frameDurationTicks)
	{
		if (!general || timeScale == 0 || frameDurationTicks == 0)
			return kDeliveryHoldMinimumUs;
		const double periodUs = 1.0e6 * static_cast<double>(frameDurationTicks) / static_cast<double>(timeScale);
		const double holdUs = (std::min)(1.0e6, 1.15 * periodUs);
		return (std::max)(kDeliveryHoldMinimumUs, static_cast<int64_t>(std::ceil(holdUs)));
	}
}
