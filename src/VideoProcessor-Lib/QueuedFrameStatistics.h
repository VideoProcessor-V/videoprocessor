#pragma once

#include "AnalysisLumaSource.h"

#include <cstddef>
#include <cstdint>

// Read-only access to the renderer's queued-frame (look-ahead) walk for
// statistics consumers. The walk calls the observer once per walk and once per
// frame it visits, oldest first: the frame about to be rendered (framesAhead
// 0) and the queued frames after it. Observers must not keep the luma source
// past the call and must stay cheap; they run on the render loop thread.
namespace QueuedFrameStatistics
{
	struct PictureRect
	{
		// Source raster pixels, right/bottom exclusive.
		int left = 0;
		int top = 0;
		int right = 0;
		int bottom = 0;
	};

	class IObserver
	{
	public:
		virtual ~IObserver() = default;
		// futureFrames: how many queued frames after the current one this walk
		// visits (bounded by the frames actually queued as well as by the
		// look-ahead setting).
		virtual void BeginWalk(size_t futureFrames) = 0;
		virtual void ObserveFrame(uint64_t sourceSequence, size_t framesAhead,
			const AnalysisLumaSource& luma, const PictureRect& activePicture) = 0;
		// Efficiency round 2026-10-18 (efficiency_fixes): false when ObserveFrame would ignore
		// this frame now (already observed, or off), so the walk skips measuring its active
		// picture for it. Must not change state; the default observes every frame.
		virtual bool WantsFrame(uint64_t sourceSequence) const { (void)sourceSequence; return true; }
	};
}
