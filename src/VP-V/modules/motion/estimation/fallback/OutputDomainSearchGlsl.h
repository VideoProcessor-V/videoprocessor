// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// motion.estimation / OUTPUT-DOMAIN SEARCH (W2-C8): the hierarchical GPU block search that the output sigma-delta used to
// keep privately (doc 91 D: "No feature keeps private motion code (dither's search moves into the module)"; rule 21,
// rule 25 addendum 3). It is the motion engine's OUTPUT-DOMAIN fallback estimator: motion between two consecutive
// presented OUTPUT pictures, per 16x16 output block, measured on the presented picture itself (the step a consumer
// takes when no shared pair covers the refresh: DitherVectorSource step 4).
//
// FORM. Three libplacebo mpv-user-shader passes (LUMAQ quarter luma, LUMAC coarse luma, MV block search), the text
// cut byte-for-byte from shaders/Temporal sigma-delta.glsl at b5ae6eda5 (lines 387-789, the same //!HOOK order). They
// run INSIDE the consuming OUTPUT hook, not as a hook of their own: libplacebo keeps SAVEd pass textures and //!TEXTURE
// storage per parsed shader (custom_mpv.c struct hook_priv: descriptors + pass_textures), so a separately parsed
// search could not hand MV / LUMAQ / LUMAC to the quantizer or keep LQPREV / MVPREV across refreshes. The consumer's
// shader file names the passes with ONE plain comment line
//     // VP_INCLUDE motion.output_domain_search
// which the custom hook chain (output.dither CustomHookChain::ExpandVpIncludes) replaces by Glsl() before templates
// and parsing; the expanded sigma-delta text is byte-identical to the b5ae6eda5 file (OutputDomainSearchTests).
//
// CONTRACT with the including shader (it declares these; the passes only read / write them):
//   //!PARAM  motion_compensation, match_tolerance, grain_tolerance_scale, static_refresh, vector_source, occlusion_mask,
//             instance_tag (float; as in Temporal sigma-delta.glsl)
//   //!TEXTURE VPVEC (sampled; external block vectors, vector_source = 1), LQPREV + MVPREV (storage, the search's
//             previous-refresh luma and vectors; sizes {{output_width_q4}} / {{output_width_b16}}), RESID / PREV /
//             RESID_NEXT / PREV_NEXT (storage, bound by the MV pass as in the original shader)
//   OUT (SAVE): LUMAQ, LUMAC, MV (vector in quarter pixels, confidence, match error) for the passes that follow.
// Edits to this text change the sigma-delta output: keep OutputDomainSearchTests' golden in step with a measured,
// owner-visible change (rule 28: never edit the golden to pass).
#pragma once

#include <string>

namespace VpMotion
{
namespace OutputDomainSearch
{
	// The include id the consumer's shader names (`// VP_INCLUDE <id>`).
	constexpr const char* kIncludeId = "motion.output_domain_search";

	// Pass 1: LUMAQ, quarter-resolution luma of this refresh (code units). LF line ends, no trailing newline.
	inline const char* LumaQuarterPassGlsl()
	{
		return R"VPGLSL(//!HOOK OUTPUT
//!BIND HOOKED
//!SAVE LUMAQ
//!WIDTH HOOKED.w 1.5 + 4 /
//!HEIGHT HOOKED.h 1.5 + 4 /
//!DESC VideoProcessor sigma-delta: quarter-resolution luma
//!COMPUTE 16 16

// Quarter-resolution luma of the gamma-encoded picture, in 8-bit code units
// (the same domain as the quantizer). Each texel averages a 4x4 block.
// Efficiency (owner 2026-10-15): the work group's 64x64 source pixels are
// read once in a coalesced sweep into shared memory, then each texel sums its
// 4x4 from there - the same per-pixel values in the same order, so the
// result is bit-identical to the direct 16 reads per texel it replaces.
shared float lq_tile_[4096];

void hook()
{
    ivec2 full = textureSize(HOOKED_raw, 0);
    // The code-domain scale, as in the quantizer below.
    // 8-bit units: a true 10-bit output (color_bits 10) arrives as /1023.
    float scale = color_bits > 9.0 ? 1023.0 / 4.0 :
        ((levels_full < 0.5 && swapchain_bits > 9.0) ? 255.75 : 255.0);
    ivec2 base = ivec2(gl_WorkGroupID.xy) * 64;
    for (int k = 0; k < 16; k++) {
        int idx = int(gl_LocalInvocationIndex) + k * 256;
        ivec2 p = min(base + ivec2(idx % 64, idx / 64), full - ivec2(1));
        vec3 c = (HOOKED_mul * texelFetch(HOOKED_raw, p, 0)).rgb * scale;
        lq_tile_[idx] = dot(c, vec3(0.2126, 0.7152, 0.0722));
    }
    memoryBarrierShared();
    barrier();
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (pos.x < size.x && pos.y < size.y) {
        ivec2 local = ivec2(gl_LocalInvocationID.xy) * 4;
        float sum = 0.0;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                sum += lq_tile_[(local.y + y) * 64 + local.x + x];
        imageStore(out_image, pos, vec4(sum / 16.0, 0.0, 0.0, 1.0));
    }
}
)VPGLSL";
	}

	// Pass 2: LUMAC, coarse (1/16) luma, the top of the hierarchy.
	inline const char* LumaCoarsePassGlsl()
	{
		return R"VPGLSL(//!HOOK OUTPUT
//!BIND HOOKED
//!BIND LUMAQ
//!SAVE LUMAC
//!WIDTH HOOKED.w 7.5 + 16 /
//!HEIGHT HOOKED.h 7.5 + 16 /
//!DESC VideoProcessor sigma-delta: coarse (1/16) luma
//!COMPUTE 8 8

// The coarse level of the hierarchical search: each texel averages a 4x4
// block of the quarter-resolution luma (16x16 full pixels).
void hook()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (pos.x >= size.x || pos.y >= size.y)
        return;
    ivec2 lq = textureSize(LUMAQ_raw, 0);
    float sum = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            sum += (LUMAQ_mul * texelFetch(LUMAQ_raw, min(pos * 4 + ivec2(x, y), lq - ivec2(1)), 0)).r;
    imageStore(out_image, pos, vec4(sum / 16.0, 0.0, 0.0, 1.0));
}
)VPGLSL";
	}

	// Pass 3: MV, per 16x16 block: vector (quarter pixels), confidence, match error; VPVEC copied when vector_source = 1.
	inline const char* BlockSearchPassGlsl()
	{
		return R"VPGLSL(//!HOOK OUTPUT
//!BIND HOOKED
//!BIND LUMAQ
//!BIND LUMAC
//!BIND LQPREV
//!BIND MVPREV
//!BIND RESID
//!BIND PREV
//!BIND RESID_NEXT
//!BIND PREV_NEXT
//!BIND VPVEC
//!SAVE MV
//!WIDTH HOOKED.w 7.5 + 16 /
//!HEIGHT HOOKED.h 7.5 + 16 /
//!DESC VideoProcessor sigma-delta: block motion search
//!COMPUTE 1 1 16 16

// The validity tag of this hook's state: one of 64 values (exact in rgba16f,
// 1/128 apart against the 1e-3 test) chosen by instance_tag.
#define VALID_TAG (0.25 + 0.0078125 * mod(floor(instance_tag), 64.0))
// Window of quarter-resolution texels compared per block: the block's own
// 4x4 plus a 2-texel margin, so a block that is only partly textured still
// locks on.
const int WIN0 = -2;
const int WIN1 = 6;

ivec2 lq_size;
bool prev_valid;

float lq_cur(ivec2 p)
{
    p = clamp(p, ivec2(0), lq_size - ivec2(1));
    return (LUMAQ_mul * texelFetch(LUMAQ_raw, p, 0)).r;
}

float lq_prev(ivec2 p)
{
    p = clamp(p, ivec2(0), lq_size - ivec2(1));
    return imageLoad(LQPREV, p).r;
}

// Full-resolution luma for the final refinement: this refresh from the
// picture, the previous refresh from PREV_NEXT (the value the quantizer
// stored last refresh; the state pass has not moved it yet).
float full_scale;
ivec2 full_size;

float luma_cur(ivec2 p)
{
    p = clamp(p, ivec2(0), full_size - ivec2(1));
    vec3 c = (HOOKED_mul * texelFetch(HOOKED_raw, p, 0)).rgb * full_scale;
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Efficiency (owner 2026-10-15): the state is ping-ponged instead of copied
// each refresh. LQPREV (0,0).a says which pair the quantizer wrote last
// (> 0.5: RESID/PREV, else RESID_NEXT/PREV_NEXT, which is also the fresh
// state's); this pass reads that pair, as it read the staging pair before.
bool last_in_a_;

float luma_prev(ivec2 p)
{
    p = clamp(p, ivec2(0), min(full_size, imageSize(PREV_NEXT)) - ivec2(1));
    // Audit 2026-10-16: PREV/PREV_NEXT hold the quantizer's x = src * in_scale,
    // which in true 10-bit mode (color_bits > 9) is 10-bit codes (x1023), while
    // luma_cur and the match tolerance are in 8-bit units (x1023/4). Bring the
    // stored value to 8-bit units (exact power-of-two scale); the 8-bit and
    // limited-on-10-bit-swapchain modes already match (scale 1.0, unchanged).
    float to8 = color_bits > 9.0 ? 0.25 : 1.0;
    return dot((last_in_a_ ? imageLoad(PREV, p) : imageLoad(PREV_NEXT, p)).rgb, vec3(0.2126, 0.7152, 0.0722)) * to8;
}

// Full-resolution match of the 16x16 block displaced by v (full pixels).
float sad_full(ivec2 corner, ivec2 v)
{
    float s = 0.0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            ivec2 p = corner + ivec2(x, y);
            s += abs(luma_cur(p) - luma_prev(p - v));
        }
    return s;
}

ivec2 lc_size;

float lc_cur(ivec2 p)
{
    p = clamp(p, ivec2(0), lc_size - ivec2(1));
    return (LUMAC_mul * texelFetch(LUMAC_raw, p, 0)).r;
}

// The previous refresh's coarse luma shares LQPREV (channel b, same index).
float lc_prev(ivec2 p)
{
    p = clamp(p, ivec2(0), lc_size - ivec2(1));
    return imageLoad(LQPREV, p).b;
}

// Coarse match: a 5x5 window of coarse texels around the block.
float sad_coarse(ivec2 c, ivec2 v)
{
    float s = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            ivec2 p = c + ivec2(x, y);
            s += abs(lc_cur(p) - lc_prev(p - v));
        }
    return s;
}

// Sum of absolute differences between this block now and the previous
// refresh displaced by v (quarter pixels): where the content came from.
float sad(ivec2 origin, ivec2 v)
{
    float s = 0.0;
    for (int y = WIN0; y < WIN1; y++)
        for (int x = WIN0; x < WIN1; x++) {
            ivec2 p = origin + ivec2(x, y);
            s += abs(lq_cur(p) - lq_prev(p - v));
        }
    return s;
}

// One work group of 256 threads per 16x16 block (owner 2026-10-13, the pass
// was 36 ms per frame at 2560x1600 on an RTX 3050 Ti with one thread per
// block). The candidates of each level are evaluated in parallel; every
// candidate's sum runs in the original order and thread 0 picks the winner in
// the original scan order with the original strict comparison, so the
// vectors are exactly the serial search's (test: the serial reference in
// src/VideoProcessor-Test/fixtures).
shared float coarse_sads[81];
shared float coarse_cur[25];     // the block's 5x5 coarse window
shared float coarse_prev[169];   // the previous refresh's 13x13 around it
shared float fine_sads[64];
shared ivec2 fine_vectors[64];
shared float cur_luma[256];
shared float prev_luma[400];
shared float full_sads[25];
shared ivec2 shared_coarse;
shared int fine_count;

void sync_group()
{
    memoryBarrierShared();
    barrier();
}

// Every barrier is at the top level of hook() and reached by all threads:
// D3D11's HLSL compiler rejects a barrier after a conditional return or
// inside a branch (X3663), even a group-uniform one. The early cases are
// flags instead of returns.
void hook()
{
    ivec2 block = ivec2(gl_WorkGroupID.xy);
    ivec2 blocks = imageSize(out_image);
    int li = int(gl_LocalInvocationIndex);
    bool lead = li == 0;
    bool inside = block.x < blocks.x && block.y < blocks.y;
    lq_size = textureSize(LUMAQ_raw, 0);
    lc_size = textureSize(LUMAC_raw, 0);
    ivec2 origin = block * 4;

    // Motion compensation off: no search (saves its GPU time); the block's
    // last real vector is kept for when it is switched on again. Delta 8 R1:
    // the renderer's vectors for this refresh (exact on a de-judder extra);
    // no search, MVPREV keeps the last real search result. The previous
    // refresh's luma is valid once this shader has stored it.
    // A cadence repeat (static_refresh) shows the previous picture again:
    // the motion is zero, so the search is skipped (it would find zero, the
    // quantizer's plain path either way); MVPREV keeps the last real vector,
    // as the search does on a repeat.
    bool searching = motion_compensation >= 0.5 && vector_source <= 0.5 && static_refresh <= 0.5;
    vec4 tag = imageLoad(LQPREV, ivec2(0));
    prev_valid = abs(tag.g - VALID_TAG) < 1e-3 && !isnan(tag.r);
    last_in_a_ = tag.a > 0.5;
    bool run_search = inside && searching && prev_valid;
    if (lead && inside && !run_search) {
        if (motion_compensation < 0.5) {
            imageStore(out_image, block, vec4(0.0));
        } else if (vector_source > 0.5) {
            vec4 e = texelFetch(VPVEC, min(block, textureSize(VPVEC, 0) - ivec2(1)), 0);
            bool usable = !any(isnan(e.xyz));
            imageStore(out_image, block, usable ? vec4(round(e.xy), e.z > 0.5 ? 1.0 : 0.0, 0.0) : vec4(0.0));
        } else if (static_refresh > 0.5) {
            imageStore(out_image, block, vec4(0.0));
        } else {
            imageStore(out_image, block, vec4(0.0));
            imageStore(MVPREV, block, vec4(0.0, 0.0, 0.0, VALID_TAG));
        }
    }

    // Hierarchical block matching. Coarse level (1/16 resolution, one texel
    // per block): every vector within +-4 coarse texels (+-64 full pixels),
    // one thread per candidate, from the two windows in shared memory (the
    // same values as lc_cur / lc_prev, the sum in sad_coarse's order).
    if (run_search && li < 169) {
        if (li < 25)
            coarse_cur[li] = lc_cur(block + ivec2(li % 5 - 2, li / 5 - 2));
        coarse_prev[li] = lc_prev(block + ivec2(li % 13 - 6, li / 13 - 6));
    }
    sync_group();
    if (run_search && li < 81) {
        int dx = li % 9 - 4, dy = li / 9 - 4;
        float s = 0.0;
        for (int y = -2; y <= 2; y++)
            for (int x = -2; x <= 2; x++)
                s += abs(coarse_cur[(y + 2) * 5 + x + 2] - coarse_prev[(y - dy + 6) * 13 + (x - dx + 6)]);
        coarse_sads[li] = s;
    }
    sync_group();
    if (run_search && lead) {
        ivec2 c = ivec2(0);
        float c_sad = coarse_sads[40];
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                float s = coarse_sads[(dy + 4) * 9 + dx + 4];
                if (s < c_sad) { c_sad = s; c = ivec2(dx, dy); }
            }
        shared_coarse = c;
    }
    sync_group();
    ivec2 coarse = shared_coarse;

    // Fine level (quarter resolution): every vector within +-2 quarter
    // pixels of zero and of the coarse result, and +-1 of this block's last
    // real motion (kept through repeated film frames). The candidate list is
    // the serial search's order (zero first), one thread per candidate.
    vec4 mp = imageLoad(MVPREV, block);
    bool have_prev = abs(mp.a - VALID_TAG) < 1e-3 && !any(isnan(mp.xy));
    ivec2 pv = have_prev ? ivec2(round(mp.xy)) : ivec2(0);
    if (run_search && lead) {
        int n = 0;
        fine_vectors[n++] = ivec2(0);
        ivec2 centres[3] = ivec2[3](ivec2(0), coarse * 4, pv);
        int radii[3] = int[3](2, 2, 1);
        for (int c = 0; c < 3; c++) {
            if (c > 0 && centres[c] == ivec2(0))
                continue;
            if (c == 2 && pv == coarse * 4)
                continue;
            for (int dy = -radii[c]; dy <= radii[c]; dy++)
                for (int dx = -radii[c]; dx <= radii[c]; dx++)
                    fine_vectors[n++] = centres[c] + ivec2(dx, dy);
        }
        fine_count = n;
    }
    sync_group();
    if (run_search && li < fine_count)
        fine_sads[li] = sad(origin, fine_vectors[li]);
    sync_group();
    ivec2 best = ivec2(0);
    float best_sad = 0.0;
    if (run_search) {
        best_sad = fine_sads[0];
        for (int k = 1; k < fine_count; k++)
            if (fine_sads[k] < best_sad) { best_sad = fine_sads[k]; best = fine_vectors[k]; }
    }

    // S1: more tolerant where the grain map (alpha, from VP's mask hook:
    // seen 1 - 0.49 g, new 0.49 g) sees film grain, so the residual survives
    // the grain; exactly match_tolerance at scale 1.
    float tolerance = match_tolerance;
    if (run_search && lead && grain_tolerance_scale > 1.0) {
        ivec2 hs = textureSize(HOOKED_raw, 0);
        float gsum = 0.0;
        for (int y = 2; y < 16; y += 4)
            for (int x = 2; x < 16; x += 4) {
                float a = (HOOKED_mul * texelFetch(HOOKED_raw, clamp(block * 16 + ivec2(x, y), ivec2(0), hs - ivec2(1)), 0)).a;
                gsum += clamp(a < 0.5 ? a / 0.49 : (1.0 - a) / 0.49, 0.0, 1.0);
            }
        tolerance *= mix(1.0, grain_tolerance_scale, gsum / 16.0);
    }

    float samples = float((WIN1 - WIN0) * (WIN1 - WIN0));
    float error = best_sad / samples;
    float confidence = error <= tolerance ? 1.0 : 0.0;

    // Final refinement to whole pixels (audit, stage 4): the quarter level
    // moves in 4-pixel steps, so every full-pixel vector within +-2 of it is
    // compared on the full-resolution block. Without it a pan that is not a
    // multiple of 4 px per refresh carries from beside the true source and
    // mostly fails the per-pixel gate. Skipped until the previous refresh's
    // values exist (RESID_NEXT tag). The block's luma and the previous
    // refresh's 20x20 window around it are loaded once into shared memory.
    ivec2 full = best * 4;
    full_size = textureSize(HOOKED_raw, 0);
    full_scale = color_bits > 9.0 ? 1023.0 / 4.0 :
        ((levels_full < 0.5 && swapchain_bits > 9.0) ? 255.75 : 255.0);
    ivec2 corner = block * 16;
    ivec2 tagpos = clamp(corner, ivec2(0), imageSize(RESID_NEXT) - ivec2(1));
    bool refine = run_search && abs((last_in_a_ ? imageLoad(RESID, tagpos) : imageLoad(RESID_NEXT, tagpos)).a - VALID_TAG) < 1e-3;
    ivec2 centre = full;
    if (refine) {
        cur_luma[li] = luma_cur(corner + ivec2(li % 16, li / 16));
        ivec2 window = corner - centre - ivec2(2);
        for (int i = li; i < 400; i += 256)
            prev_luma[i] = luma_prev(window + ivec2(i % 20, i / 20));
    }
    sync_group();
    if (refine && li < 25) {
        int dx = li % 5 - 2, dy = li / 5 - 2;
        float s = 0.0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                s += abs(cur_luma[y * 16 + x] - prev_luma[(y - dy + 2) * 20 + (x - dx + 2)]);
        full_sads[li] = s;
    }
    sync_group();
    if (refine && lead) {
        float full_sad = full_sads[12];
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                float s = full_sads[(dy + 2) * 5 + dx + 2];
                if (s < full_sad) { full_sad = s; full = centre + ivec2(dx, dy); }
            }
        // Trust comes from the full-resolution match: at 4-pixel steps a
        // motion between them matches only roughly at quarter resolution.
        error = full_sad / 256.0;
        confidence = error <= tolerance ? 1.0 : 0.0;
    }
    if (run_search && lead) {
        // MV holds the vector in full pixels (MVPREV keeps quarter pixels).
        imageStore(out_image, block, vec4(vec2(full), confidence, error));
        // A repeated film frame (no motion this refresh) keeps the last real
        // vector as the next starting point.
        if (best != ivec2(0) || !have_prev)
            imageStore(MVPREV, block, vec4(vec2(best), confidence, VALID_TAG));
    }
})VPGLSL";
	}

	// The three passes in hook order, as one block of text (LF line ends, no trailing newline).
	inline std::string Glsl()
	{
		std::string text = LumaQuarterPassGlsl();
		text += '\n';
		text += LumaCoarsePassGlsl();
		text += '\n';
		text += BlockSearchPassGlsl();
		return text;
	}
}
}
