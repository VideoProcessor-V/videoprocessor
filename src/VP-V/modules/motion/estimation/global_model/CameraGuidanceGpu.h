// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2): the GPU side of the measurement plan for the vector engine's
// matcher (DejudderWarp.cpp, Dejudder::MotionWarp). A GuidanceFrame is made on the CPU from one
// MeasurementPlan (GlobalModelMeasurement.h) and handed to MotionWarp::SetCameraGuidance before the
// pair's Estimate. The matcher then
//   * writes the model's centre translation into D1's global vector instead of running the D1 search
//     (useModelGlobal),
//   * lets E4's static gate also fire on the model's static verdict (staticScene; with the E4 gate on),
//   * skips every search across a cut / flash (skipMatcher): the searches are written as untrusted, so the
//     consistency pass and the frame trust hold the pair exactly as a failed match does,
//   * runs the guided pass below before the block search (narrow) and before the second sweep (E1 skip).
//     The pass writes one texel per matcher block: (vx, vy, sad, w) when the block passed verification,
//     z = -1 when it did not. The block search and the second sweep keep an accepted texel (as E4 keeps the
//     previous vectors); every other block runs today's search in the same dispatch: the escalation.
// Without a frame (or an inactive one) nothing is bound or dispatched: today's matcher exactly.
//
// This is the GPU mirror of Reference::Match (ReferenceMatcher.cpp): narrow = whole pixels c + (dx, dy)
// with |dx|, |dy| <= R in dy-outer / dx-inner order (strict <, the first wins ties), then the eight
// half-pixel neighbours; accepted iff sad <= tolerance. E1 skip: the first sweep's vector within e1Agree of
// the prediction and its sad <= tolerance. Zero-mean SAD in a fade. Taps clamp per sample (the reference
// clamps the window origin; the two differ only at the picture edge, where both escalate on failure).
#pragma once

#include <vector>

#include "GlobalModelMeasurement.h"

namespace CameraModel
{
	struct GuidanceFrame
	{
		bool active = false;
		bool skipMatcher = false;
		bool useModelGlobal = false;
		float globalFwd[2] = { 0, 0 }, globalBwd[2] = { 0, 0 };   // source px over the pair
		bool staticScene = false;
		bool narrow = false;           // the plan has narrow blocks
		bool e1Skip = false;
		bool fadeNormalised = false;
		int gridW = 0, gridH = 0;      // the record's block grid
		double blockSize = 0;          // source px per record block
		double narrowRadiusPx = 0;     // source px
		double e1AgreePx = 0;          // source px
		std::vector<float> guide;      // RGBA per record block: seed xy, prediction xy (source px)
		std::vector<float> code;       // per record block: hint + 4 * eligible
		bool HasBlocks() const
		{
			return gridW > 0 && gridH > 0 && blockSize > 0 && guide.size() == static_cast<size_t>(gridW) * gridH * 4 &&
				code.size() == static_cast<size_t>(gridW) * gridH;
		}
	};

	inline GuidanceFrame MakeGuidanceFrame(const MeasurementPlan& plan)
	{
		GuidanceFrame g;
		if (!plan.active)
			return g;
		g.active = true;
		g.skipMatcher = plan.skipMatcher;
		g.useModelGlobal = plan.useModelGlobal;
		g.globalFwd[0] = static_cast<float>(plan.globalX);
		g.globalFwd[1] = static_cast<float>(plan.globalY);
		g.globalBwd[0] = static_cast<float>(plan.globalBwdX);
		g.globalBwd[1] = static_cast<float>(plan.globalBwdY);
		g.staticScene = plan.staticScene;
		g.fadeNormalised = plan.fadeNormalised;
		g.e1Skip = plan.e1Skip;
		PackGuideTexels(plan, g.guide, g.code);
		g.gridW = plan.gridW;
		g.gridH = plan.gridH;
		g.blockSize = plan.blockSize;
		g.narrowRadiusPx = plan.narrowRadiusPx;
		g.e1AgreePx = plan.e1AgreePx;
		g.narrow = plan.counters.narrowBlocks > 0;
		if (!g.HasBlocks())
		{
			g.narrow = false;
			g.e1Skip = false;
		}
		return g;
	}

	// The guided pass (libplacebo compute body; one thread per matcher block, 8x8 groups). Bindings:
	// from_half (nearest), to_half (linear), camera_guide (RGBA32F), camera_code (R32F), dst (RGBA32F storage),
	// and with CG_E1 also `sweep` (the first sweep / the engine's seeds). Defines: CG_E1 (E1 mode, else
	// narrow), CG_FADE (zero-mean cost), COST_W (E1: keep the seed's weight, as the second sweep does).
	// Uniforms: cg_scale (matcher px per source px), cg_ratio (matcher block centre -> record block:
	// 8 / (cg_scale * record block size)), cg_sign (+1 forward, -1 backward), cg_radius (int),
	// cg_tol (codes per sample, the matcher's tolerance), cg_agree (matcher px).
	inline const char* GuidedPassHeaderGlsl()
	{
		return R"GLSL(
ivec2 cg_half_size;
float cg_from(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), cg_half_size - ivec2(1)), 0).r; }
float cg_to(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(cg_half_size)).r; }
// The block's 8x8 plus a 2-pixel margin, per sample (the search's sad() / 144).
float cg_cost(ivec2 o, vec2 v)
{
#ifdef CG_FADE
    float mf = 0.0, mt = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            mf += cg_from(p);
            mt += cg_to(vec2(p) + v);
        }
    mf /= 144.0;
    mt /= 144.0;
#endif
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
#ifdef CG_FADE
            s += abs((cg_from(p) - mf) - (cg_to(vec2(p) + v) - mt));
#else
            s += abs(cg_from(p) - cg_to(vec2(p) + v));
#endif
        }
    return s / 144.0;
}
)GLSL";
	}

	inline const char* GuidedPassBodyGlsl()
	{
		return R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    cg_half_size = textureSize(from_half, 0);
    ivec2 gsize = textureSize(camera_code, 0);
    ivec2 g = clamp(ivec2(floor((vec2(b) + vec2(0.5)) * cg_ratio)), ivec2(0), gsize - ivec2(1));
    int code = int(texelFetch(camera_code, g, 0).r + 0.5);
    int hint = code % 4;
    int eligible = code / 4;
    vec4 gd = texelFetch(camera_guide, g, 0);
    ivec2 o = b * 8;
    vec4 res = vec4(0.0, 0.0, -1.0, 0.0);
#ifdef CG_E1
    vec4 sw = texelFetch(sweep, b, 0);
    vec2 pred = cg_sign * gd.zw * cg_scale;
    if (eligible == 1 && length(sw.xy - pred) <= cg_agree) {
        float s = cg_cost(o, sw.xy);
#ifdef COST_W
        float w_ = sw.w;
#else
        float w_ = 1.0;
#endif
        if (s <= cg_tol)
            res = vec4(sw.xy, s, w_);
    }
#else
    if (hint == 1) {
        ivec2 c = ivec2(floor(cg_sign * gd.xy * cg_scale + vec2(0.5)));
        vec2 best = vec2(c - ivec2(cg_radius));
        float best_s = 3.0e38;
        for (int dy = -cg_radius; dy <= cg_radius; dy++)
            for (int dx = -cg_radius; dx <= cg_radius; dx++) {
                vec2 v = vec2(c + ivec2(dx, dy));
                float s = cg_cost(o, v);
                if (s < best_s) { best_s = s; best = v; }
            }
        vec2 base = best;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                vec2 v = base + 0.5 * vec2(dx, dy);
                float s = cg_cost(o, v);
                if (s < best_s) { best_s = s; best = v; }
            }
        if (best_s <= cg_tol)
            res = vec4(best, best_s, 1.0);
    }
#endif
    imageStore(dst, b, res);
}
)GLSL";
	}
}
