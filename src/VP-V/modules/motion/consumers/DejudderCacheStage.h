#pragma once

#include <VP-V/modules/motion/consumers/RenderedWarp.h>
#include <SubtitleBoxDetector.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <functional>
#include <vector>

// WP-13 (integration batch 1): the de-judder rendered-picture cache registration (Stage 6d) that
// lived in the renderer host (LibplaceboVideoRenderer.cpp RenderLocked) as one VP-V stage object.
// The host keeps one call:
//
//   dejudderCacheStage.Register(frame, KnownNlsMapping, frameHooks)   after the extended range
//
// It disarms the cache, builds this frame's extras geometry (rounded crops, the NLS map, the
// moved subtitle's hold rectangles) into the host's geometry, and arms the cache with its
// PRE_OUTPUT hook last among the PRE_OUTPUT hooks (after the LED compensation and the VP
// calibration LUT, so the extras get the LUT-mapped picture). Statements and log text are
// today's (moved, not changed): byte-identical. The cache itself is RenderedWarp.{h,cpp}
// (motion.consumers, M2 step 17). Threading: render thread only. No locks.
namespace DejudderCacheStage
{
	struct Frame
	{
		// dejudderReason == Active && dejudderExtrasAllowed && dejudder && !cadenceRepeat &&
		// sourceSequence != 0: extras may follow this frame (not in mask-only mode; audit
		// stage10-6d item 3).
		bool extrasMayFollow = false;
		// Vector engine on demand: the sigma-delta takes the engine's vectors on a frame whose extras do not run.
		// The geometry is kept (the vectors need it); the cache is not armed and no hook is added.
		bool geometryForVectors = false;
		uint64_t sourceSequence = 0;
		pl_rect2df sourceCrop{};    // renderImage.crop
		pl_rect2df targetCrop{};    // target.crop
		float framebufferW = 0.0f;  // target.planes[0].texture size (0 without a texture)
		float framebufferH = 0.0f;
		bool nlsSubmitted = false;  // submittedNls != nullptr
		bool warpRendered = true;   // dejudder_warp_rendered (6d)
		bool interpolating = false;
		// A moved subtitle (subtitle composition succeeded): where it was, its fill, where it is
		// shown and the layout coverage, held still in the extras. nullptr = none.
		const SubtitleBoxRect* subtitleHolds = nullptr;
		int numSubtitleHolds = 0;
	};

	class Stage
	{
	public:
		Stage(DejudderRendered::Cache& cache, DejudderRendered::Geometry& geometry, bool& geometryValid)
			: cache(cache), geometry(geometry), geometryValid(geometryValid) {}

		// nlsMapping: the bundled NLS / NLS+ map of this frame (an invalid map for any other NLS
		// shader); asked only when NLS was submitted.
		void Register(const Frame& frame, const std::function<NlsMaskMapping::Mapping()>& nlsMapping,
			std::vector<const pl_hook*>& hooks);

	private:
		DejudderRendered::Cache& cache;
		DejudderRendered::Geometry& geometry;
		bool& geometryValid;
		int warpStageLogged = -1;
	};
}
