/*
 * Temporal sigma-delta quantizer for VideoProcessor's VP Renderer (libplacebo)
 *
 * Purpose
 *   Replace libplacebo's spatial dither with a per-pixel first-order
 *   noise-shaping quantizer that carries the sub-LSB residual forward in
 *   time. Where a pixel is unchanged between refreshes, the time-average of
 *   the 8-bit output converges to the higher-precision source value. Where a
 *   pixel changed by more than `motion_threshold` codes, the residual is
 *   discarded for that pixel (plain rounding plus a small dither).
 *
 * Motion-compensated carry (stage 4, `motion_compensation` = 1)
 *   On moving pictures the plain gate throws the residual away wherever the
 *   picture moved. With motion compensation the residual follows the motion
 *   instead: a block matcher on gamma-encoded luma at quarter resolution
 *   finds, for each 16x16 block, where its content was on the previous
 *   refresh (forward only, from a persistent previous-luma texture), and the
 *   quantizer takes the carried residual from there. A vector error only
 *   misplaces a residual of at most 0.5 + noise_amplitude code, and every pixel is still
 *   checked by the motion gate against the value it is carried from, so a
 *   wrong or low-confidence vector falls back to the plain gate.
 *   Reference: FFmpeg vf_minterpolate (block matching algorithm, not code).
 *
 * Passes (all on the OUTPUT hook, in this order)
 *   1. LUMAQ  quarter-resolution luma of this refresh (code units)
 *   2. LUMAC  coarse (1/16 resolution) luma, the top of the hierarchy
 *   3. MV     per 16x16 block (one 256-thread work group each, the
 *             candidates in parallel): vector (quarter pixels), confidence, match
 *             error. Hierarchical: a full search within +-4 coarse texels
 *             (+-64 full pixels), then a full search within +-2 quarter
 *             pixels of zero and of the coarse result, and +-1 of the
 *             block's last real motion (kept through 3:2 repeats), then a
 *             full-resolution refinement to whole pixels (+-2 px)
 *   4. state  moves last refresh's residual/value from the staging textures
 *             into the read textures, and stores this refresh's luma (both
 *             levels) for the next search (the quantizer reads one set and
 *             writes the other, so no pixel can overwrite a value another
 *             pixel still reads)
 *   5. quantizer, as before, with the motion-compensated carry
 *   With motion_compensation = 0 the search is skipped (passes 1, 2 and 4
 *   still run, cheaply) and the quantizer output is byte-for-byte the stage 2
 *   version's (same noise sequence: see `frame / 5`).
 *
 * Occlusion mask (stage 6c, `occlusion_mask` = 1, set by the renderer)
 *   On the first refresh of a new source frame the de-judder's matcher knows
 *   which blocks show content that was not visible in the previous frame
 *   (uncovered or new). The renderer's OcclusionMask hook writes that into
 *   the alpha channel of the OUTPUT texture just before this shader: 1 = seen
 *   before, 0 = new. Where it is 0 no residual is carried into the pixel (it
 *   is quantised fresh), and this shader writes alpha back to 1. The renderer
 *   sets `occlusion_mask` to 1 only on refreshes where it installed the hook;
 *   with 0 (the default) alpha is ignored and the output is unchanged.
 *   Alpha ownership: the mask hook and this shader own alpha at OUTPUT;
 *   nothing else uses it (Stage 10b keeps its remainder in its own texture).
 *
 * Grain-aware carry (S1, `grain_tolerance_scale` > 1, set by the renderer)
 *   With the Stage 8 grain map the same hook also writes the block's grain
 *   amount g (0..1) into alpha: seen 1 - 0.49 g, new 0.49 g (the `< 0.5`
 *   test above is unchanged). The block match tolerance and the per-pixel
 *   motion gate are both multiplied by mix(1, grain_tolerance_scale, g), so
 *   the residual survives film grain. The carried payload stays within
 *   +-(0.5 + noise_amplitude) code. At 1 (the default) nothing reads the grain.
 *
 * External vectors (delta 8 R1, `vector_source` = 1, set by the renderer)
 *   On a de-judder extra the renderer knows the motion since the previous
 *   present exactly: the de-judder's field times the shift step, mapped into
 *   output pixels. It writes that per 16x16 output block into VPVEC (vector
 *   in full pixels in rg, trust in b; libplacebo 377 with VP's fork patch
 *   0004, pl_mpv_user_shader_set_texture) and sets vector_source to 1 for that
 *   render only. The search pass then copies VPVEC into MV and skips its own
 *   search; the quantizer and its per-pixel gate are unchanged. VPVEC is a
 *   1x1 r8 placeholder (zero: untrusted) when nothing is bound; at 0 (the
 *   default) it is never read and the output is byte-identical.
 *
 * Per-pixel external vectors (planner D2, `vector_pixel` = 1, with
 *   vector_source = 1): the de-judder's warp writes the displacement it
 *   applied to every output pixel since the previous present into VPPIX
 *   (rgba16f, output size; rg full pixels, b = 1 moved / 0 held), so the
 *   residual is carried along exactly the per-pixel motion of the picture
 *   (with per-pixel trust the picture no longer moves in 16x16 blocks). The
 *   quantizer reads VPPIX instead of MV; the search pass still fills MV
 *   from VPVEC. A 1x1 r8 placeholder when nothing is bound (as VPVEC: an
 *   rgba16f TEXTURE's data is sized as 16 bytes on D3D11); never read at 0.
 *
 * Pipeline position
 *   Hooks OUTPUT: after colour management, the calibration LUT, and
 *   libplacebo's encode_color (so values are already in the target's code
 *   domain), and before libplacebo's own dither. The VP renderer's hook
 *   chain suppresses libplacebo's dither while an OUTPUT-stage hook is
 *   active (see the project brief, stage 2). If this shader is ever used
 *   without that change, set `dithering: off` instead.
 *
 * Value domain at the OUTPUT stage (verified against libplacebo 7.360.1
 * renderer.c / colorspace.c):
 *   limited range, 10-bit swapchain : colour = 4k / 1023 = code8 / 255.75
 *   limited range, 8-bit swapchain  : colour = code8 / 255
 *   full range                      : colour = code8 / 255
 *   After the hook the renderer multiplies by 1/scale and stores to the
 *   swapchain. For a 10-bit swapchain declared as 8-bit colour depth the
 *   exact code that lands in the top 8 bits is reproduced when this hook
 *   emits   limited: 4k/1023      full: k/255
 *   and for an 8-bit swapchain in either range  k/255.
 *   With a true 10-bit output (colour depth 10, delta 5) both ranges arrive
 *   as code10/1023 and the quantizer works in 10-bit codes, emitting k/1023.
 *   The tolerances (motion_threshold, match_tolerance) stay in 8-bit codes,
 *   as perceptual units: the luma passes work in 8-bit units, and the
 *   quantizer scales motion_threshold by 4. The OUTPUT texture is rgba16f,
 *   which near white resolves only half a 10-bit code, so at 10 bits the
 *   exact-code snap follows the rgba16f step at the value's brightness
 *   (half a code near white, finer below), and the final store writes an
 *   exactly representable half.
 *   Audit 2026-10-16: with `output_float32` = 1 (set by the renderer when
 *   libplacebo's output pass runs in rgba32f, output_fbo_float32) the
 *   texture holds values to about 1e-4 code, so the snap shrinks to 1e-3
 *   code (a few float32 ulps of colour-math error near white) and the final
 *   store skips the half-float rounding. Rgba16f's snap there reached half a
 *   code above mid grey, rounding every value to a whole code before the
 *   quantizer, so the upper half carried no residual (undithered). At 0
 *   (the default, and always on libplacebo 7.360) the output is unchanged.
 *   All three parameters (levels_full, swapchain_bits, color_bits) are set by
 *   the renderer every frame from the target; a value in the shader rule is
 *   overwritten.
 *
 * State
 *   Persistent storage textures sized to the output surface through the
 *   hook chain's {{output_width}} {{output_height}} templates (re-applied on
 *   renderer reset, so they follow resolution changes). RESID/PREV hold the
 *   carried residual (rgb, with a validity tag in a) and the last presented
 *   input value for the motion gate; RESID_NEXT/PREV_NEXT are the staging
 *   copies the quantizer writes. LQPREV and MVPREV use only their top-left
 *   quarter / sixteenth. Storage textures start uninitialised, so every
 *   read is guarded by a tag. The tag is per hook instance (`instance_tag`,
 *   set by the renderer when it creates the hook): on a hardware GPU a new
 *   hook can be given a freed hook's memory, and a constant tag made it
 *   trust that stale state on its first refresh (gate 2026-10-13).
 *
 * VP_OUTPUT_QUANTIZER (audit 2026-10-16): this marker tells the hook chain
 *   that the shader quantises the output itself, so libplacebo's dither is
 *   cleared while it runs; other OUTPUT hooks keep the dither.
 *
 * Copyright (C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

//!PARAM enabled
//!DESC 1 = sigma-delta active, 0 = plain rounding only (A/B switch)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!PARAM levels_full
//!DESC Set by the renderer from the frame's target (delta 5): 0 = limited range, 1 = full range
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM swapchain_bits
//!DESC Set by the renderer from the frame's target (delta 5): swapchain sample depth, 10 or 8
//!TYPE float
//!MINIMUM 8.0
//!MAXIMUM 10.0
10.0

//!PARAM color_bits
//!DESC Set by the renderer from the frame's target (delta 5): the colour depth it quantises to, 8 or 10
//!TYPE float
//!MINIMUM 8.0
//!MAXIMUM 10.0
8.0

//!PARAM noise_amplitude
//!DESC Decorrelation dither amplitude, in output codes (breaks sigma-delta limit cycles)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.15

//!PARAM motion_threshold
//!DESC Per-pixel change (8-bit codes) above which the carried residual is discarded
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 64.0
1.0

//!PARAM motion_compensation
//!DESC 1 = carry the residual along the picture's motion (stage 4), 0 = plain motion gate only
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM match_tolerance
//!DESC Average luma difference (8-bit codes) up to which a block's motion vector is trusted
//!TYPE float
//!MINIMUM 0.1
//!MAXIMUM 8.0
1.5

//!PARAM occlusion_mask
//!DESC Set by the renderer: 1 = read the de-judder occlusion mask from alpha (stage 6c), 0 = ignore alpha
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM grain_tolerance_scale
//!DESC Set by the renderer (S1): match_tolerance x this where the grain map in alpha sees film grain; 1 = off
//!TYPE float
//!MINIMUM 1.0
//!MAXIMUM 8.0
1.0

//!PARAM instance_tag
//!DESC Set by the renderer when it creates the hook: this instance's state tag (stale state from a freed hook never matches)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 16777216.0
0.0

//!PARAM static_refresh
//!DESC Set by the renderer: 1 = this refresh repeats the previous picture (a cadence repeat), so the motion is zero and the search is skipped
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM vector_source
//!DESC Set by the renderer (delta 8 R1): 1 = take this refresh's block vectors from VPVEC, 0 = own search
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM vector_pixel
//!DESC Set by the renderer (planner D2): 1 = the quantizer takes this refresh's per-pixel displacement from VPPIX
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM output_float32
//!DESC Set by the renderer (audit 2026-10-16): 1 = the OUTPUT texture is rgba32f (output_fbo_float32 in effect), 0 = rgba16f
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM test_ramp
//!DESC 1 = replace the picture with a 10-bit horizontal grey ramp for verification
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!TEXTURE VPVEC
//!SIZE 1 1
//!FORMAT r8
//!FILTER NEAREST
//!BORDER CLAMP
00

//!TEXTURE VPPIX
//!SIZE 1 1
//!FORMAT r8
//!FILTER NEAREST
//!BORDER CLAMP
00

//!TEXTURE RESID
//!SIZE {{output_width}} {{output_height}}
//!FORMAT rgba16f
//!FILTER NEAREST
//!BORDER CLAMP
//!STORAGE

//!TEXTURE PREV
//!SIZE {{output_width}} {{output_height}}
//!FORMAT rgba16f
//!FILTER NEAREST
//!BORDER CLAMP
//!STORAGE

//!TEXTURE RESID_NEXT
//!SIZE {{output_width}} {{output_height}}
//!FORMAT rgba16f
//!FILTER NEAREST
//!BORDER CLAMP
//!STORAGE

//!TEXTURE PREV_NEXT
//!SIZE {{output_width}} {{output_height}}
//!FORMAT rgba16f
//!FILTER NEAREST
//!BORDER CLAMP
//!STORAGE

//!TEXTURE LQPREV
//!SIZE {{output_width}} {{output_height}}
//!FORMAT rgba16f
//!FILTER NEAREST
//!BORDER CLAMP
//!STORAGE

//!TEXTURE MVPREV
//!SIZE {{output_width}} {{output_height}}
//!FORMAT rgba16f
//!FILTER NEAREST
//!BORDER CLAMP
//!STORAGE

//!HOOK OUTPUT
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

//!HOOK OUTPUT
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

//!HOOK OUTPUT
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
}

//!HOOK OUTPUT
//!BIND HOOKED
//!BIND LUMAQ
//!BIND RESID
//!BIND PREV
//!BIND RESID_NEXT
//!BIND PREV_NEXT
//!BIND LUMAC
//!BIND LQPREV
//!SAVE SIGMA_DELTA_STATE
//!COMPONENTS 1
//!WIDTH HOOKED.w 1.5 + 4 /
//!HEIGHT HOOKED.h 1.5 + 4 /
//!DESC VideoProcessor sigma-delta: carry state to this refresh
//!COMPUTE 16 16

// The validity tag of this hook's state: one of 64 values (exact in rgba16f,
// 1/128 apart against the 1e-3 test) chosen by instance_tag.
#define VALID_TAG (0.25 + 0.0078125 * mod(floor(instance_tag), 64.0))

void hook()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (pos.x >= size.x || pos.y >= size.y)
        return;
    // Last refresh's results become what this refresh reads: no copy - the
    // pair written last is read and the other written (owner 2026-10-15,
    // ping-pong; was a full-resolution copy of both). This refresh's luma for
    // the next search (after the search used the previous one). Texel (0,0)
    // carries the validity tag in g and, in a, the pair this refresh's
    // quantizer writes (0.75: RESID/PREV, 0.25: RESID_NEXT/PREV_NEXT).
    ivec2 lq_size = textureSize(LUMAQ_raw, 0);
    ivec2 q = pos;
    ivec2 lc_size = textureSize(LUMAC_raw, 0);
    // Channel b: the coarse luma, at the same index (the coarse grid is
    // smaller, so it fits in the same texels).
    float coarse = (q.x < lc_size.x && q.y < lc_size.y) ?
        (LUMAC_mul * texelFetch(LUMAC_raw, q, 0)).r : 0.0;
    float pair = 1.0;
    if (q == ivec2(0))
        pair = imageLoad(LQPREV, ivec2(0)).a > 0.5 ? 0.25 : 0.75;   // write the other pair
    if (q.x < lq_size.x && q.y < lq_size.y)
        imageStore(LQPREV, q, vec4((LUMAQ_mul * texelFetch(LUMAQ_raw, q, 0)).r,
                                   VALID_TAG, coarse, pair));
    imageStore(out_image, pos, vec4(0.0));
}

//!HOOK OUTPUT
//!BIND HOOKED
//!BIND MV
//!BIND RESID
//!BIND PREV
//!BIND RESID_NEXT
//!BIND PREV_NEXT
//!BIND VPPIX
//!BIND LQPREV
//!DESC VideoProcessor temporal sigma-delta quantizer
//!COMPUTE 16 16

// Tag stored in RESID.a to mark a texel as initialised by this shader.
// The validity tag of this hook's state: one of 64 values (exact in rgba16f,
// 1/128 apart against the 1e-3 test) chosen by instance_tag.
#define VALID_TAG (0.25 + 0.0078125 * mod(floor(instance_tag), 64.0))

// Interleaved gradient noise (Jimenez 2014) with a golden-ratio temporal
// shift: cheap, spatially well distributed, decorrelated frame to frame.
float ign(vec2 p, int f)
{
    p += 5.588238 * float(f & 63);
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// The residual carried from texel `from`, if that texel is initialised and
// its last value is within the motion gate of the value now.
// The OUTPUT texture is rgba16f. Near white it holds only values about
// half a 10-bit code apart, and a GPU may truncate rather than round when
// storing into it (WARP does): a 10-bit code k/1023 stored as is can come out
// one code low (940 -> 939). An exactly representable half is stored as is,
// whatever the rounding mode, and the one packHalf2x16 gives lies within a
// quarter code of k/1023, so the final store still lands on k (delta 5).
vec3 exact_half(vec3 v)
{
    return vec3(unpackHalf2x16(packHalf2x16(v.rg)), unpackHalf2x16(packHalf2x16(vec2(v.b, 0.0))).x);
}

// Hunt 2 L1: bound of the carried residual. k quantises x + r + d, and the
// dither d is not fed back, so |x + r - k| reaches 0.5 + noise_amplitude in
// normal operation. Clipping it at 0.5 threw away error the loop needed to
// cancel (a mean bias up to ~0.06 code at amplitude 0.5). The bound only
// stops wind-up where k is clamped at the code limits.
float resid_bound()
{
    return 0.5 + noise_amplitude;
}

// Ping-pong (see the carry pass): true = this refresh writes RESID/PREV
// and reads RESID_NEXT/PREV_NEXT; false the other way round.
bool write_a_;

bool carried(ivec2 from, vec3 x, float gate_scale, out vec3 r)
{
    r = vec3(0.0);
    vec4 rs = write_a_ ? imageLoad(RESID_NEXT, from) : imageLoad(RESID, from);
    vec4 ps = write_a_ ? imageLoad(PREV_NEXT, from) : imageLoad(PREV, from);
    if (abs(rs.a - VALID_TAG) >= 1e-3 || any(isnan(rs.rgb)) || any(isnan(ps.rgb)))
        return false;
    // motion_threshold is in 8-bit codes; x is in output codes.
    float units = color_bits > 9.0 ? 4.0 : 1.0;
    if (any(greaterThan(abs(x - ps.rgb), vec3(motion_threshold * units * gate_scale))))
        return false;
    r = clamp(rs.rgb, -resid_bound(), resid_bound());
    return true;
}

void hook()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 out_size = imageSize(out_image);
    if (pos.x >= out_size.x || pos.y >= out_size.y)
        return;
    write_a_ = imageLoad(LQPREV, ivec2(0)).a > 0.5;   // set by the carry pass

    bool limited = levels_full < 0.5;
    bool swap10 = swapchain_bits > 9.0;

    // Code-domain scale of the hooked texture and the output step that
    // reproduces an exact 8-bit code after the renderer's final store.
    // Limited range on a 10-bit swapchain declared as 8-bit colour depth
    // arrives as 4k/1023 = code8/255.75 (colorspace.c: 235/256 x 2^10/1023);
    // on an 8-bit swapchain, and in full range, as code8/255. Reading the
    // 10-bit case x256 made white 235 read 235.23 and flicker to 236 about
    // one refresh in four (measured on a real 10-bit target, stage 6a audit).
    // Delta 5: a true 10-bit output quantises to 10-bit codes.
    bool ten = color_bits > 9.0;
    float in_scale = ten ? 1023.0 : ((limited && swap10) ? 255.75 : 255.0);
    float out_step = ten ? (1.0 / 1023.0) : ((limited && swap10) ? (4.0 / 1023.0) : (1.0 / 255.0));
    // Hunt 2 L2: the codes that survive libplacebo's output clamp (beta fork
    // patch 0002, applied after every user hook). Limited range reserves only
    // the timing codes, 2^(n-8) - 1 below and above (8-bit 0 and 255, 10-bit
    // 0-3 and 1020-1023); footroom and headroom (BTB/WTW) are kept. Full range
    // uses every code. k is clamped to these before the residual is computed,
    // so the feedback sees the code really emitted and reserved codes are never
    // emitted (also on the stable DLL, which has no output clamp).
    float code_bits = ten ? 10.0 : 8.0;
    float code_unit = exp2(code_bits - 8.0);
    float code_min = limited ? code_unit : 0.0;
    float code_max = limited ? 255.0 * code_unit - 1.0 : exp2(code_bits) - 1.0;
    // 8-bit codes: 0.13 (one rgba16f step near white is 1/8 code).

    vec4 src = HOOKED_mul * texelFetch(HOOKED_raw, pos, 0);
    vec3 x = src.rgb * in_scale;
    // The OUTPUT texture is rgba16f: above mid grey its step is 1/8 of an
    // 8-bit code, and a GPU may round or truncate into it, so an exact code
    // (white 235) can arrive up to 0.125 low and the carried residual would
    // dip it to 234 every few refreshes (seen on WARP: 940 940 940 940 936).
    // A value within 0.13 code of a whole code is that code. Real 10-bit
    // detail comes in steps of 0.25 code and stays further away.
    vec3 whole = floor(x + 0.5);
    // At 10 bits one rgba16f step is half a code near white (a quarter below
    // mid grey, and so on down): within one step of a whole code there is no
    // finer information to carry, so the snap follows the step at the value's
    // own brightness (WARP truncates libplacebo's 940 to 939.55 when storing).
    vec3 snap = vec3(0.13);
    if (ten)
        snap = max(vec3(0.13), exp2(floor(log2(max(src.rgb, vec3(1e-6)))) - 10.0) * in_scale * 1.04);
    // Audit 2026-10-16: an rgba32f OUTPUT texture (output_float32) carries
    // the sub-code detail the rgba16f snap above discards; only colour-math
    // error (a few float32 ulps, ~1e-4 code near white) is snapped away.
    bool f32 = output_float32 > 0.5;
    if (f32)
        snap = vec3(1e-3);
    x = mix(x, whole, vec3(lessThan(abs(x - whole), snap)));

    if (test_ramp > 0.5) {
        // 10-bit horizontal ramp across the legal range, in 8-bit code units.
        float t = float(pos.x) / max(float(out_size.x - 1), 1.0);
        float ramp = limited ? (16.0 + 219.0 * t) : (255.0 * t);
        ramp = floor(ramp * 4.0 + 0.5) * 0.25;   // quantise to 10-bit grid
        x = vec3(ten ? ramp * 4.0 : ramp);
    }

    vec3 k;
    // libplacebo adds 1 to `frame` for every pass it runs and this is the
    // fifth of five, so frame / 5 is the refresh count: the same noise
    // sequence as the one-pass stage 2 shader (motion_compensation 0 is then
    // byte-for-byte the stage 2 output).
    vec3 d = vec3((ign(vec2(pos), frame / 5) - 0.5) * 2.0 * noise_amplitude);

    if (enabled > 0.5) {
        ivec2 state_size = imageSize(RESID);
        ivec2 spos = clamp(pos, ivec2(0), state_size - ivec2(1));
        vec3 r = vec3(0.0);
        bool have = false;
        // New or uncovered content (occlusion mask 0): nothing to carry from.
        bool occluded = occlusion_mask > 0.5 && src.a < 0.5;
        // S1: on film grain (alpha, see the block search) the per-pixel gate
        // is as much more tolerant as the block match: grain changes pixels
        // by more than motion_threshold on every frame. 1 = off.
        float gate_scale = 1.0;
        if (grain_tolerance_scale > 1.0)
            gate_scale = mix(1.0, grain_tolerance_scale,
                clamp(src.a < 0.5 ? src.a / 0.49 : (1.0 - src.a) / 0.49, 0.0, 1.0));

        // Motion-compensated carry: the residual from where this pixel's
        // content was on the previous refresh.
        if (!occluded && motion_compensation > 0.5) {
            vec4 mv = MV_mul * texelFetch(MV_raw,
                min(pos / 16, textureSize(MV_raw, 0) - ivec2(1)), 0);
            // Planner D2: this pixel's own displacement from the warp.
            if (vector_pixel > 0.5 && vector_source > 0.5) {
                vec4 e = texelFetch(VPPIX, min(pos, textureSize(VPPIX, 0) - ivec2(1)), 0);
                mv = any(isnan(e.xyz)) ? vec4(0.0) : vec4(e.xy, e.z > 0.5 ? 1.0 : 0.0, 0.0);
            }
            ivec2 v = ivec2(round(mv.xy));   // full pixels
            ivec2 from = pos - v;
            if (mv.z > 0.5 && v != ivec2(0) &&
                all(greaterThanEqual(from, ivec2(0))) && all(lessThan(from, state_size)))
                have = carried(from, x, gate_scale, r);
        }
        // Plain motion gate: the same pixel, if it did not change.
        if (!have && !occluded)
            have = carried(spos, x, gate_scale, r);

        // First-order sigma-delta: quantise the error-corrected value, carry
        // the new residual. The dither term is not fed back.
        k = clamp(floor(x + r + d + 0.5), code_min, code_max);
        vec3 r_next = clamp(x + r - k, -resid_bound(), resid_bound());

        if (write_a_) {
            imageStore(RESID, spos, vec4(r_next, VALID_TAG));
            imageStore(PREV, spos, vec4(x, 1.0));
        } else {
            imageStore(RESID_NEXT, spos, vec4(r_next, VALID_TAG));
            imageStore(PREV_NEXT, spos, vec4(x, 1.0));
        }
    } else {
        k = clamp(floor(x + d + 0.5), code_min, code_max);
        // The state this refresh does not write keeps its value, as the
        // copies did (ping-pong: carried into the pair written now).
        ivec2 spos = clamp(pos, ivec2(0), imageSize(RESID) - ivec2(1));
        if (write_a_) {
            imageStore(RESID, spos, imageLoad(RESID_NEXT, spos));
            imageStore(PREV, spos, imageLoad(PREV_NEXT, spos));
        } else {
            imageStore(RESID_NEXT, spos, imageLoad(RESID, spos));
            imageStore(PREV_NEXT, spos, imageLoad(PREV, spos));
        }
    }

    // With the occlusion mask or the grain (S1) in alpha, alpha goes back to
    // 1 here.
    vec4 outc = vec4(k * out_step, occlusion_mask > 0.5 || grain_tolerance_scale > 1.0 ? 1.0 : src.a);
    vec4 stored = outc / HOOKED_mul;
    // Audit 2026-10-16: an rgba32f texture stores k/1023 exactly enough;
    // the half-float workaround applies only to rgba16f.
    if (ten && !f32)
        stored.rgb = exact_half(stored.rgb);
    imageStore(out_image, pos, stored);
}
