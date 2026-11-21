// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P1 GPU form (L-B4, iface-L-CORE section 7): the kernel that fills the P1 half plane on the render device, and the exact CPU
// emulation of it that the identity test compares with the lane's law (VpLaneProducts::HalfLumaRows).
//   kPrecisionUnorm16  R16 unorm, FULL 16-bit scale, msb-aligned codes: stored k = (a + b + c + d + 2) >> 2 on the msb-aligned
//                      16-bit codes, so GPU == CPU bit-exact (the P1 default variant).
//   kPrecisionFloat32  R32F, the unrounded 2x2 mean in the plane's normalised units, (a + b + c + d) * 0.25 in the accessor's
//                      tap order (vpu_half_luma): the canonical 12-bit safe variant the bands (L-B3) read.
//   codeUnit 8, scale L  the MATCHER PYRAMID variant (BBP-B, plan lead18/coreplan-B-prereq-P2global-pyramid.md section (b), the
//                      variant contract below): the matcher's 8-bit code units, stored R16F. This is the owner-made precision
//                      variant OD:670 (d) permits ("R16F 8-bit codes only as an owner-made precision variant for the matcher if
//                      bit-identity needs it"; LEAD 8 ruling, Sat 2026-11-14 05:16 EST, refs/owner-decisions-2026-11-07.md:670):
//                      bit identity needs it, because the R16F store rounding and the matcher's bilinear `to_l1` sampling of the
//                      stored level differ from an R32F level. It is ANALYSIS data (block-match codes), never pixel values; the
//                      canonical P1 GPU forms stay R16 unorm / R32F (12-bit safe, OD:668).
// The source plane is read through texelFetch with the second tap clamped to the last texel (HalfLuma.h taps). Bits and the
// msb shift come in as uniforms: src_max = 2^bits - 1 (the normalised-to-code scale), shift_mul = 2^(16 - bits).
// Header-only, C++14, no Windows or GPU headers (the shader is text; the render device runs it; the pyramid builder below is
// declared with forward-declared libplacebo handles and defined in P1GpuFill.cpp).
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/VariantKey.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

// libplacebo's texture handle, forward-declared (pl_tex = const pl_tex_t*, libplacebo gpu.h).
struct pl_tex_t;
namespace VpSkeleton { struct OwnerFill; struct Dims; }

namespace VpFrameAnalysis
{
namespace P1Gpu
{
	// The compute body (libplacebo custom-shader style: `src` sampled, `dst` storage). Defines: P1_UNORM16 selects the exact
	// integer law, otherwise the Float32 mean. The accessor header is VpUnit::HalfLuma::Glsl().
	inline const char* FillBody()
	{
		return
			"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"ivec2 size = imageSize(dst);\n"
			"if (p.x < size.x && p.y < size.y) {\n"
			"#ifdef P1_UNORM16\n"
			"    ivec2 hi = textureSize(src, 0) - ivec2(1);\n"
			"    ivec2 p0 = min(p * 2, hi);\n"
			"    ivec2 p1 = min(p * 2 + ivec2(1), hi);\n"
			"    float a = floor(texelFetch(src, ivec2(p0.x, p0.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float b = floor(texelFetch(src, ivec2(p1.x, p0.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float c = floor(texelFetch(src, ivec2(p0.x, p1.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float d = floor(texelFetch(src, ivec2(p1.x, p1.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float k = floor((a + b + c + d + 2.0) * 0.25);\n"
			"    imageStore(dst, p, vec4(k / 65535.0, 0.0, 0.0, 1.0));\n"
			"#else\n"
			"    imageStore(dst, p, vec4(vpu_half_luma(src, p), 0.0, 0.0, 1.0));\n"
			"#endif\n"
			"}\n";
	}

	// CPU emulation of the kernel in 32-bit float arithmetic, one output sample. `s` are the four normalised source samples in
	// the tap order a, b, c, d. Returns the stored R16 unorm code (Unorm16) .
	inline uint16_t EmulateUnorm16(float s0, float s1, float s2, float s3, int bits)
	{
		const float srcMax = float((1u << bits) - 1u);
		const float shiftMul = float(1u << (16 - bits));
		const float a = std::floor(s0 * srcMax + 0.5f) * shiftMul, b = std::floor(s1 * srcMax + 0.5f) * shiftMul;
		const float c = std::floor(s2 * srcMax + 0.5f) * shiftMul, d = std::floor(s3 * srcMax + 0.5f) * shiftMul;
		const float k = std::floor((a + b + c + d + 2.0f) * 0.25f);
		// R16 unorm store: round(k / 65535 * 65535) = k exactly.
		return static_cast<uint16_t>(std::floor((k / 65535.0f) * 65535.0f + 0.5f));
	}
	// The Float32 variant: the unrounded mean, the accessor's arithmetic.
	inline float EmulateFloat32(float s0, float s1, float s2, float s3) { return VpUnit::HalfLuma::MeanF<float>(s0, s1, s2, s3); }

	// ---- The matcher pyramid variant of kP1HalfLuma (GPU form): the variant contract (BBP-B commit B1) ----
	// Acquire(kP1HalfLuma, seq, Gpu, MatcherPyramidKey(L)). One product, one owner (refs/129 L-B4); the matcher
	// (motion/estimation/matcher) is a reader. Levels are counted in halvings of the SOURCE luma, so VariantDims (core/VariantShape.h,
	// one rounded-up halving per scale step) gives exactly the matcher's own sizes (DejudderWarp.cpp EnsureTextures: half =
	// (w + 1) / 2, each 4x4 level = (n + 3) / 4):
	//   1 = half    2x2 law (MatcherHalfBody) on the P13 luma, x the range codes (MatcherCodes)
	//   3 = 1/8     4x4 law (MatcherPyramidLevelBody) on level 1
	//   5 = 1/32    4x4 law on level 3
	// the lowRes inner matcher (its luma is the outer half, already in codes):
	//   2 = 1/4     2x2 law on level 1, codes 1 (the matcher's lumaInCodes law)
	//   4 = 1/16    4x4 law on level 2
	//   6 = 1/64    4x4 law on level 4
	// Each level's fill Acquires the level it is made from (nested get-or-compute: a level is never built twice per frame).
	enum : uint8_t { kMatcherCodeUnit = 8 };
	enum : uint8_t { kMatcherLevelFirst = 1, kMatcherLevelLast = 6 };

	// The axes the P1 GPU owner serves: the precision axis (the canonical Unorm16 / Float32 forms) and the pyramid's code-unit
	// and scale axes (OwnerFill::variantAxes).
	inline uint32_t OwnerVariantAxes()
	{
		return uint32_t(VpSkeleton::VariantKey::kAxisPrecision) | uint32_t(VpSkeleton::VariantKey::kAxisCodeUnit)
		       | uint32_t(VpSkeleton::VariantKey::kAxisScale);
	}

	// The ONE spelling of a pyramid level's key (already canonical against the owner's defaults, VariantKey{}).
	inline VpSkeleton::VariantKey MatcherPyramidKey(uint8_t level)
	{
		VpSkeleton::VariantKey k;
		k.codeUnit = kMatcherCodeUnit;
		k.scale = level;
		return k;
	}

	// Canonicalisation for the code-unit / scale axes: the pyramid level a CANONICAL key asks, 0 = not a matcher pyramid key
	// (the owner then serves the canonical forms, or declines). A pyramid key is exactly { codeUnit 8, scale 1..6 }: codeUnit 8
	// without a level (scale 0), a level without codeUnit 8 (the canonical forms have no levels), any other code unit, a
	// precision request (the variant's storage is the owner's own, MatcherStorageFormat) or any other axis is refused, so two
	// spellings of one level can never make two spots and no reduced form exists beside the published one (00-UNIVERSAL 2).
	inline uint8_t MatcherPyramidLevel(const VpSkeleton::VariantKey& canonical)
	{
		const uint32_t pyramidAxes = uint32_t(VpSkeleton::VariantKey::kAxisCodeUnit) | uint32_t(VpSkeleton::VariantKey::kAxisScale);
		if (canonical.Axes() != pyramidAxes || canonical.codeUnit != kMatcherCodeUnit)
			return 0;
		return canonical.scale >= kMatcherLevelFirst && canonical.scale <= kMatcherLevelLast ? canonical.scale : uint8_t(0);
	}

	// How level `level` is made: from `input` (0 = the P13 source luma, else that pyramid level) by the 2x2 law (twoByTwo) or
	// the 4x4 law; rangeCodes = the 2x2 pass multiplies by the range codes (level 1 only), else by 1 (level 2). False = no such
	// level.
	struct MatcherLevelLaw {
		uint8_t input = 0;
		bool twoByTwo = false;
		bool rangeCodes = false;
	};
	inline bool MatcherLevelLawOf(uint8_t level, MatcherLevelLaw* out)
	{
		MatcherLevelLaw law;
		if (level == 1) {
			law.input = 0;
			law.twoByTwo = true;
			law.rangeCodes = true;
		} else if (level == 2) {
			law.input = 1;
			law.twoByTwo = true;
		} else if (level >= 3 && level <= kMatcherLevelLast) {
			law.input = uint8_t(level - 2);
		} else {
			return false;
		}
		if (out != nullptr)
			*out = law;
		return true;
	}

	// The 2x2 pass's `codes` uniform (moved from DejudderWarp.cpp Downsample, unchanged): 8-bit code units per normalised unit,
	// 255 for limited-range sources (the stored codes) and 219 for full range (limited-equivalent codes; delta 5, D5-3); 1 when
	// the input is already in codes (level 2, the lowRes inner matcher's lumaInCodes). The range comes from the P13 plane.
	inline float MatcherCodes(bool inputInCodes, bool fullRange) { return inputInCodes ? 1.0f : fullRange ? 219.0f : 255.0f; }

	// The variant's storage: the owner's own default format for every pyramid level (VariantTexShape `fallback`; the precision
	// axis stays 0, so core/VariantShape.h still refuses every Float16 PRECISION request). OD:670 (d); N2 row requested (BBP-B).
	inline VpSkeleton::TexFormat MatcherStorageFormat() { return VpSkeleton::TexFormat::R16F; }  // PRECISION-OK 119#64: matcher pyramid analysis variant (codeUnit 8, scale 1..6), never pixels; OD:670 (d)

	// The 2x2 level body, MOVED from DejudderWarp.cpp DOWNSAMPLE_BODY (its non-fade branch, byte-identical): head + core + tail.
	// The matcher splices its fade exception between the pieces (FADE_PLAIN after the head, FADE_APPLY after the core), so its
	// own body stays byte-identical to the former DOWNSAMPLE_BODY; the owner's body is the three pieces alone. Uniform `codes`;
	// header VpUnit::HalfLuma::Glsl() (vpu_half_luma); descriptors `src` (sampled) and `dst` (storage); work group 16 x 16.
	inline const char* MatcherHalfHead()
	{
		return R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    float v;
)GLSL";
	}
	inline const char* MatcherHalfCore()
	{
		return R"GLSL(    {
        // The shared U3 accessor (HalfLuma.h): the same taps, edge clamp and (a + b + c + d) * 0.25 as the
        // matcher's former private loop, then the code scale; the arithmetic order is unchanged.
        v = vpu_half_luma(src, p) * codes;
)GLSL";
	}
	inline const char* MatcherHalfTail()
	{
		return R"GLSL(    }
    imageStore(dst, p, vec4(v, 0.0, 0.0, 1.0));
}
)GLSL";
	}
	inline const std::string& MatcherHalfBody()
	{
		static const std::string body = std::string(MatcherHalfHead()) + MatcherHalfCore() + MatcherHalfTail();
		return body;
	}

	// The 4x4 level body (levels 3..6): the shared unit U3's VpUnit::HalfLuma::GlslBox4x4LevelBody (one copy, owner
	// 2026-11-15), the body the matcher's former PyramidBody() referenced (B3: the matcher now runs it through BuildPyramidInto). No uniform, no header; work group 8 x 8.
	inline const std::string& MatcherPyramidLevelBody()
	{
		static const std::string body = VpUnit::HalfLuma::GlslBox4x4LevelBody();
		return body;
	}

	// One compute pass of the pyramid law, run by the CALLER's dispatch path: the owner fill through OwnerFillRequest::gpu, the
	// matcher's visible fallback (row product.motion_warp) through its own Dispatch (its pass timing maps keep their names).
	// `description` is the matcher's former pass name; `header` is VpUnit::HalfLuma::Glsl() for a 2x2 pass and empty for a 4x4
	// pass; `codes` is set (hasCodes) only for a 2x2 pass; width / height = the dst texture's size; group = 16 (2x2) or 8 (4x4).
	struct PyramidPass {
		const char* description = nullptr;
		std::string header;
		const std::string* body = nullptr;
		const pl_tex_t* src = nullptr;
		const pl_tex_t* dst = nullptr;
		bool hasCodes = false;
		float codes = 0.0f;
		int width = 0, height = 0;
		int group = 0;
	};
	typedef std::function<bool(const PyramidPass&)> PyramidPassRunner;

	// THE ONE CODE COPY of the matcher pyramid law (defined in P1GpuFill.cpp, BBP-B commit B2), used by the owner fill and by
	// the matcher's fallback into its own scratch textures (fallback == previous behaviour byte for byte, rule 25 addendum).
	// Today's MotionWarp::Downsample (non-fade) + Pyramid in one call:
	//   from < 0   the 2x2 pass luma -> half with `codes` (MatcherCodes), then the 4x4 levels up to `levels`;
	//   from >= 0  `half` exists: only the 4x4 levels above `from` up to `levels` (Pyramid(half, l1, l2, from, levels)).
	// levels: 0 the half only, 1 also l1 (one 4x4 step), 2 also l2. For the outer matcher half / l1 / l2 are levels 1 / 3 / 5,
	// for the lowRes inner (luma = the outer half, codes 1) levels 2 / 4 / 6. False = a pass failed (the runner's error).
	bool BuildPyramidInto(const PyramidPassRunner& run, const pl_tex_t* luma, float codes, const pl_tex_t* half, const pl_tex_t* l1,
		const pl_tex_t* l2, int from, int levels);
	// THE (kP1HalfLuma, Gpu) owner fill of the matcher pyramid variant, as the P1 module registers it (defined in P1GpuFill.cpp);
	// public so a GPU test registers the production fill itself, never a copy.
	// GF-P1 MUST-2: sized from the LARGEST source luma (`maxLuma`, the module's largest observed size). reserveBytes = the
	// per-frame working set MatcherPyramidReserveBytes(maxLuma) (the ahead planner's figure, which the window multiplies by
	// 1 + back + depth); reserveFor = the exact size of the ASKED level (the reserve before each fill and its context's grant).
	// The matcher's STRUCTURAL read ahead of the render seal (GF-P1 MUST-1): the render pair B = N + 1 and the EstimateAhead pair
	// C = N + 2 (VectorEngineEstimate.inl offer; VectorEngine.cpp P13 / P1 acquires). Declared as the fill's aheadDepth only while
	// the matcher is an active consumer (P1GpuModule; owner OD 2026-11-14: ahead cap 6, derived from active consumers).
	constexpr uint16_t kMatcherStructuralAhead = 2;
	// `aheadDepth`: the declared ahead need (kMatcherStructuralAhead while the matcher is active, 0 otherwise).
	VpSkeleton::OwnerFill MatcherPyramidOwnerFill(const VpSkeleton::Dims& maxLuma, uint16_t aheadDepth = kMatcherStructuralAhead);
	// The per-frame working set of the matcher pyramid of a `luma`-sized source: the sum of levels 1..6 (one frame can hold every
	// level: 1 / 3 / 5 for the outer matcher, 2 / 4 / 6 for the lowRes inner). 0 for an empty size.
	uint64_t MatcherPyramidReserveBytes(const VpSkeleton::Dims& luma);
}
}

namespace Vpv
{
namespace FrameAnalysis
{
	// Start-up only (core/hub/HubBootstrap.cpp, after the skeleton module; idempotent): registers the (kP1HalfLuma, Gpu) owner fill
	// of the matcher pyramid variant (BBP-B commit B2; defined in P1GpuFill.cpp).
	void RegisterP1GpuModule();
	// Render thread: the source luma size of a frame whose planes a VP-V uploader offered as P13 views before the Entrance phase (the
	// matcher's A / B / ahead uploads, VectorEngine::OfferMatcherPlanes). Same effect as the module's Entrance observation: the fill
	// is registered from the largest size (re-registered only when it changes). No-op before RegisterP1GpuModule.
	void NoteP1GpuSourceLuma(const void* host, uint32_t width, uint32_t height);
}
}
