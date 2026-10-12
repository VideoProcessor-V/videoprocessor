#pragma once

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/dispatch.h>

#include <string>

// Stage 6 (6a): the de-judder's motion matcher and warp, as a standalone GPU
// component. The renderer runs it from Stage 6b (estimate in 6b-1, extra
// presents in 6b-2).
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
//   3. counts the trusted blocks on the GPU (frame trust): below
//      `minTrustedShare` the whole frame is held (a cut between A and B),
//   4. warps A's planes by `shift` times the A->B motion (shift = s * delta:
//      strength times the refresh's position after A, in source frames).
//      Untrusted blocks are a hard hold of A's own pixels - never a blend -
//      and samples that fall outside the picture clamp to its edge.
// Everything downstream (scaling, colour, NLS, LED, shaders, dither) then
// renders the warped planes exactly as it would render A.
namespace Dejudder
{
	// GLSL: `bool field_vector(vec2 source_pos, out vec2 v)`, the warp's
	// vector rule (bilinear between trusted block centres; the position's own
	// block decides hold; the whole frame holds below min_trusted_share).
	// Needs the samplers `field` and `frame_trust` and the float
	// `min_trusted_share`. Shared with Stage 6d's rendered warp.
	const char* FieldVectorGlsl();

	struct Planes
	{
		pl_tex luma = nullptr;     // Y, one component
		pl_tex chroma = nullptr;   // CbCr interleaved, two components
		int chromaShiftX = 1;      // log2 horizontal subsampling (1 for 4:2:x)
		int chromaShiftY = 0;      // log2 vertical subsampling (0 for 4:2:2, 1 for 4:2:0)
		// The source's luma range (delta 5, D5-3): full-range luma is
		// measured in limited-range-equivalent codes (219 per unit, not
		// 255), so matchTolerance means the same for both.
		bool fullRange = false;
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
		// Frame trust (6b-2): below this share of trusted blocks the whole
		// frame is held (a cut between A and B). Measured and applied on the
		// GPU; no read-back.
		float minTrustedShare = 0.25f;
		// Refinements (docs/VP-build-plan-refinements.md). The defaults here
		// are off (the matcher as before); the renderer's keys default on.
		// D1: a whole-frame translation (1/32 then 1/8 resolution) becomes an
		// extra search start for every block when it is confident.
		bool globalPrior = false;
		// D3: a trusted block more than 1 source pixel from the median of the
		// trusted vectors around it takes the median; an untrusted block whose
		// 7+ trusted neighbours agree (within 0.5 px) takes their median when
		// its own match there is within matchTolerance.
		bool vectorSmooth = false;
		// D2: strength x ramp(mean trusted |v|, lo, hi) source px per source
		// frame (hi 0 = off). D4: x ramp(trusted share, minTrustedShare,
		// trustScaleFull) (0 = off). The product changes by at most
		// strengthRate per source frame; a cut (share below minTrustedShare)
		// still holds at once and restarts the ramp from 0. Applied on the GPU
		// (FrameTrust().g), by both warps.
		float motionScaleLo = 0.25f;
		float motionScaleHi = 0.0f;
		float trustScaleFull = 0.0f;
		float strengthRate = 0.25f;
		// D5 (experiment, source warp only): a held position whose block of B
		// is new content (uncovered) is taken from B along B's own motion
		// instead of held.
		bool occlusionFill = false;
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

		// Motion between A and B. Call once per source-frame pair. With
		// occlusionSlot 0 or 1 it also writes B's occlusion mask (6c) into
		// that slot; the caller alternates slots so the mask for the frame
		// now showing survives the next estimate. With grainError it also
		// writes the high-passed temporal error per block (Stage 8-3).
		bool Estimate(const Planes& a, const Planes& b, int occlusionSlot = -1, bool grainError = false);

		// A warped by shift x (A->B motion). Call once per extra present.
		// The returned planes belong to this object and stay valid until the
		// next Warp call. With trusted = false nothing is warped (a cut, or
		// no estimate): `out` is A's planes.
		bool Warp(const Planes& a, float shift, Planes& out, bool trusted = true);

		// D3 on a raw field (one texel per block, as Field()) into Field(),
		// with the last Estimate's half-resolution planes for the promotion
		// check. Estimate runs it itself when vectorSmooth is on; public so the
		// rule can be tested on a constructed field.
		bool Smooth(pl_tex raw);

		// The block field from the last Estimate: rgba = (vx, vy) in source
		// pixels, trust (1/0), match error. One texel per 16x16 source block.
		pl_tex Field() const { return field; }
		// From the last Estimate (1x1): r = the share of trusted blocks,
		// g = the strength scale (D2/D4; 1 when both are off), b = the mean
		// trusted |v| in source pixels.
		pl_tex FrameTrust() const { return frameTrust; }
		// D1: the global vector of the last forward search (1x1): xy in 1/8
		// resolution texels (source pixels / 8), z = best / second-best match
		// (the prior is used below 0.9), w = 1 once written.
		pl_tex GlobalVector() const { return globalVector[0]; }
		// B's occlusion mask (6c), one texel per 16x16 source block of B:
		// r = 1 where B's content was visible in A (consistent both ways and
		// a good match), 0 where it is new or uncovered.
		pl_tex Occlusion(int slot) const { return slot == 0 || slot == 1 ? occlusion[slot] : nullptr; }
		// Stage 8-3, the grain map's input (from an Estimate with grainError):
		// one texel per 16x16 source block of A; r = the mean absolute
		// motion-compensated difference A - B after removing the block's mean
		// difference (a fade or lighting change is not grain), g = the block's
		// mean luma, both in 8-bit codes of half-resolution luma. Only
		// meaningful where Field() trusts the block; null before.
		pl_tex GrainError() const { return grainErrorValid ? grainError : nullptr; }
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
		bool Downsample(pl_tex luma, pl_tex half, pl_tex l1, pl_tex l2, bool fullRange);
		bool Search(pl_tex fromHalf, pl_tex toHalf, pl_tex fromL1, pl_tex toL1,
			pl_tex fromL2, pl_tex toL2, pl_tex global, pl_tex out);
		bool Global(pl_tex fromL1, pl_tex toL1, pl_tex fromL2, pl_tex toL2, pl_tex out);

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
		pl_tex forward = nullptr, backward = nullptr, field = nullptr, frameTrust = nullptr;
		pl_tex rawField = nullptr;                          // D3: before smoothing
		pl_tex globalVector[2] = { nullptr, nullptr };      // D1: forward, backward
		pl_tex strengthState = nullptr;                     // D2/D4: last scale (rgba32f)
		pl_tex fillOcclusion = nullptr;                     // D5: B's occlusion
		Planes lastB;                                       // D5: B of the last Estimate
		bool fillReady = false;
		pl_tex occlusion[2] = { nullptr, nullptr };
		pl_tex grainError = nullptr;
		bool grainErrorValid = false;
		pl_tex warpedLuma = nullptr, warpedChroma = nullptr;
		bool estimated = false;
	};
}
