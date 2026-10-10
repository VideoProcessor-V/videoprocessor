#pragma once

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/dispatch.h>

#include <string>

// Stage 6 (6a): the de-judder's motion matcher and warp, as a standalone GPU
// component. It is not connected to the renderer yet (that is 6b).
//
// Given two consecutive source frames A and B as planar YCbCr textures (the
// renderer's upload planes: a Y plane and an interleaved CbCr plane, 4:2:2
// or 4:2:0), it
//   1. estimates block motion A->B and B->A on half-resolution luma
//      (8x8 blocks at half resolution = 16x16 source pixels). A three-level
//      pyramid: a full search at 1/32 resolution; at 1/8, half-texel steps
//      within +-3 of zero and of the 1/32 result; at half resolution, whole
//      pixels within +-3 of both 1/8 results and +-2 of zero, then half a
//      pixel. Carrying both candidates down means a poor coarse guess (small
//      pictures, flat areas) is outvoted by the exact match, not followed.
//      Note: libplacebo reports a rejected pass (for example the same
//      texture bound twice) only in its log, not as a failed dispatch, so
//      every pass here binds each texture once,
//   2. marks each block trusted when its match error is within
//      `matchTolerance` and the two directions agree within
//      `consistencyPixels` (half-resolution pixels) - otherwise it is an
//      occlusion or a bad match,
//   3. warps A's planes by `shift` times the A->B motion (shift = s * delta:
//      strength times the refresh's position after A, in source frames).
//      Untrusted blocks are a hard hold of A's own pixels - never a blend -
//      and samples that fall outside the picture clamp to its edge.
// Everything downstream (scaling, colour, NLS, LED, shaders, dither) then
// renders the warped planes exactly as it would render A.
namespace Dejudder
{
	struct Planes
	{
		pl_tex luma = nullptr;     // Y, one component
		pl_tex chroma = nullptr;   // CbCr interleaved, two components
		int chromaShiftX = 1;      // log2 horizontal subsampling (1 for 4:2:x)
		int chromaShiftY = 0;      // log2 vertical subsampling (0 for 4:2:2, 1 for 4:2:0)
	};

	struct Settings
	{
		// Mean absolute luma difference per sample (8-bit code units) up to
		// which a block's match is trusted.
		float matchTolerance = 3.0f;
		// Forward and backward vectors must agree within this many
		// half-resolution pixels.
		float consistencyPixels = 1.0f;
		// Search range at the top of the pyramid, in 1/32-resolution texels
		// (+-3 = +-96 source pixels per source frame, plus the refinements).
		int coarseRange = 3;
		// Allocate host-readable outputs (tests only).
		bool hostReadable = false;
	};

	class MotionWarp
	{
	public:
		MotionWarp(pl_log log, pl_gpu gpu, const Settings& settings = Settings());
		~MotionWarp();
		MotionWarp(const MotionWarp&) = delete;
		MotionWarp& operator=(const MotionWarp&) = delete;

		// Motion between A and B. Call once per source-frame pair.
		bool Estimate(const Planes& a, const Planes& b);

		// A warped by shift x (A->B motion). Call once per extra present.
		// The returned planes belong to this object and stay valid until the
		// next Warp call. With trusted = false nothing is warped (a cut, or
		// no estimate): `out` is A's planes.
		bool Warp(const Planes& a, float shift, Planes& out, bool trusted = true);

		// The block field from the last Estimate: rgba = (vx, vy) in source
		// pixels, trust (1/0), match error. One texel per 16x16 source block.
		pl_tex Field() const { return field; }
		// The raw half-resolution searches (rgba = vector in half-resolution
		// pixels, match error); for diagnostics.
		pl_tex Forward() const { return forward; }
		pl_tex Backward() const { return backward; }
		// Pyramid levels (0 half, 1 = 1/8, 2 = 1/32) of A (b = false) or B.
		pl_tex Level(int level, bool b) const
		{
			return level == 0 ? (b ? halfB : halfA) : level == 1 ? (b ? l1B : l1A) : (b ? l2B : l2A);
		}
		int BlocksX() const { return blocksX; }
		int BlocksY() const { return blocksY; }
		const std::string& LastError() const { return error; }
		const Settings& GetSettings() const { return settings; }
		void SetSettings(const Settings& value) { settings = value; }

	private:
		bool EnsureTextures(const Planes& a);
		bool Dispatch(const char* description, const char* header, const char* body,
			const struct pl_shader_desc* descriptors, int numDescriptors,
			const struct pl_shader_var* variables, int numVariables,
			int width, int height, int group);
		bool Downsample(pl_tex luma, pl_tex half, pl_tex l1, pl_tex l2);
		bool Search(pl_tex fromHalf, pl_tex toHalf, pl_tex fromL1, pl_tex toL1,
			pl_tex fromL2, pl_tex toL2, pl_tex out);

		pl_log log = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		Settings settings;
		std::string error;

		int width = 0, height = 0;          // source luma size
		int halfW = 0, halfH = 0;
		int l1W = 0, l1H = 0;               // 1/8 source (pyramid level 1)
		int l2W = 0, l2H = 0;               // 1/32 source (pyramid level 2)
		int blocksX = 0, blocksY = 0;
		pl_tex halfA = nullptr, halfB = nullptr;
		pl_tex l1A = nullptr, l1B = nullptr, l2A = nullptr, l2B = nullptr;
		pl_tex forward = nullptr, backward = nullptr, field = nullptr;
		pl_tex warpedLuma = nullptr, warpedChroma = nullptr;
		bool estimated = false;
	};
}
