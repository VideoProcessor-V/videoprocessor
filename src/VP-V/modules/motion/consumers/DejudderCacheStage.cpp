#include <pch.h>
#include "DejudderCacheStage.h"

#include <DebugLog.h>

// WP-13: moved from LibplaceboVideoRenderer.cpp (RenderLocked, Stage 6d). Statement order and log
// text unchanged.
namespace DejudderCacheStage
{
	void Stage::Register(const Frame& frame, const std::function<NlsMaskMapping::Mapping()>& nlsMapping,
		std::vector<const pl_hook*>& hooks)
	{
		// Stage 6d: when this frame may get de-judder extras, keep A's PRE_OUTPUT picture for them
		// (the hook goes last among the PRE_OUTPUT hooks, after the LED compensation). With NLS the
		// warp needs the NLS map; an unknown NLS shader keeps the source path.
		cache.Disarm();
		geometryValid = false;
		// Only when extras may follow. The geometry is kept on the source path too: the
		// sigma-delta's vectors on the extras need it (delta 8 R1).
		if (!frame.extrasMayFollow && !frame.geometryForVectors)
			return;
		DejudderRendered::Geometry next;
		const DejudderRendered::Rects rounded = DejudderRendered::Rounded(frame.sourceCrop, frame.targetCrop,
			frame.framebufferW, frame.framebufferH);
		next.sourceCrop = rounded.source;
		next.targetCrop = rounded.target;
		bool mapped = true;
		if (frame.nlsSubmitted)
		{
			next.nls = nlsMapping();
			next.nlsDomain = frame.sourceCrop;
			mapped = next.nls.valid;
		}
		// A moved subtitle: where it was, its fill and where it is shown stay still in the extras
		// (2 source pixels of margin).
		for (int i = 0; frame.subtitleHolds && i < frame.numSubtitleHolds; ++i)
		{
			const SubtitleBoxRect& r = frame.subtitleHolds[i];
			if (r.Valid() && next.numHolds < static_cast<int>(next.holds.size()))
				next.holds[next.numHolds++] = { static_cast<float>(r.left - 2), static_cast<float>(r.top - 2),
					static_cast<float>(r.right + 2), static_cast<float>(r.bottom + 2) };
		}
		if (mapped)
		{
			geometry = next;
			geometryValid = true;
		}
		if (mapped && frame.extrasMayFollow && frame.warpRendered && !frame.interpolating)
		{
			cache.Arm(frame.sourceSequence);
			hooks.push_back(cache.Hook());
		}
		const int stage = mapped ? 1 : 0;
		if (frame.extrasMayFollow && frame.warpRendered && stage != warpStageLogged)
		{
			warpStageLogged = stage;
			DebugLog::Log(mapped ? "De-judder extras: warping the rendered picture (6d)" :
				"De-judder extras: NLS shader map not known; warping the source instead");
		}
	}
}
