#pragma once

// ONE look-ahead walk over the already-queued frames (2026-10-24, DRAFT).
//
// UPSTREAMABLE: depends only on AlphaQueuePolicy.h. Several consumers want to look at future
// frames that are already queued (never waiting, never adding depth):
//   - the active-picture decision (upstream: SelectActivePicturePreview, its configured horizon);
//   - upstream's preparation walk (VP-0205: the preview window UNION the next-source outward
//     certificate's future frames, SelectActivePicturePreparation);
//   - statistics observers that want a deeper view (a fork's LED / intent look-ahead).
// Each declares its depth in a WalkRequest; SelectWalk visits the queue ONCE and gives each
// consumer its view. With statisticsDepth = 0 the views are exactly upstream's two selections.
//
// Cadence repeats are skipped everywhere (a repeat is never a proof vote nor a new picture).

#include <vprenderer/AlphaQueuePolicy.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace QueueLookaheadWalk
{
	struct WalkRequest
	{
		size_t activePictureConfigured = 0;   // the active-picture look-ahead setting
		size_t maxActivePicture = 0;          // ActivePictureDecisionTimeline::MAX_LOOKAHEAD_FRAMES
		size_t statisticsDepth = 0;           // the deepest statistics observer (0 = none)
		size_t outwardFutureFrames = 0;       // future frames the preparation walk must cover (0 = none)
	};

	struct Walk
	{
		// Every frame walked by depth (indices, current first): the statistics observers' view.
		AlphaQueuePolicy::ActivePicturePreviewWindow window;
		// The leading frames of window that belong to the active-picture decision.
		size_t activePictureFrames = 0;
		// Preparation view: window's active-picture frames UNION the first outwardFutureFrames
		// non-repeat future frames, sorted and unique (upstream's SelectActivePicturePreparation).
		std::vector<size_t> preparation;
	};

	template<typename Queue>
	Walk SelectWalk(const Queue& queue, const WalkRequest& request)
	{
		Walk walk;
		const size_t activePicture = (std::min)(request.activePictureConfigured, request.maxActivePicture);
		const size_t depth = (std::max)(activePicture, request.statisticsDepth);
		walk.window = AlphaQueuePolicy::SelectActivePicturePreview(queue, depth,
			(std::max)(request.maxActivePicture, request.statisticsDepth));
		walk.activePictureFrames = activePicture == 0 ? 0 :
			(std::min)(walk.window.indices.size(), activePicture + 1);
		// The active-picture preview is a prefix of the deeper walk (same inspect rule, same order).
		walk.preparation.assign(walk.window.indices.begin(),
			walk.window.indices.begin() + static_cast<std::ptrdiff_t>(walk.activePictureFrames));
		size_t outward = 0;
		for (size_t index = 1; index < queue.size() && outward < request.outwardFutureFrames; ++index)
		{
			if (queue[index].cadenceRepeat) continue;
			walk.preparation.push_back(index);
			++outward;
		}
		std::sort(walk.preparation.begin(), walk.preparation.end());
		walk.preparation.erase(std::unique(walk.preparation.begin(), walk.preparation.end()), walk.preparation.end());
		return walk;
	}

	// The active-picture view alone (upstream's SelectActivePicturePreview window).
	inline std::vector<size_t> ActivePictureView(const Walk& walk)
	{
		return std::vector<size_t>(walk.window.indices.begin(),
			walk.window.indices.begin() + static_cast<std::ptrdiff_t>(walk.activePictureFrames));
	}
}
