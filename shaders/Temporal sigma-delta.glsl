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
 *   misplaces a residual of at most half a code, and every pixel is still
 *   checked by the motion gate against the value it is carried from, so a
 *   wrong or low-confidence vector falls back to the plain gate.
 *   Reference: FFmpeg vf_minterpolate (block matching algorithm, not code).
 *
 * Passes (all on the OUTPUT hook, in this order)
 *   1. LUMAQ  quarter-resolution luma of this refresh (code units)
 *   2. LUMAC  coarse (1/16 resolution) luma, the top of the hierarchy
 *   3. MV     per 16x16 block: vector (quarter pixels), confidence, match
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
 *   Alpha ownership: the mask hook and this shader own alpha at OUTPUT; the
 *   extended-range carry (Stage 10) owns it only from the RGB stage to the
 *   SCALED stage, so they never overlap.
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
 *
 * State
 *   Persistent storage textures sized to the output surface through the
 *   hook chain's {{output_width}} {{output_height}} templates (re-applied on
 *   renderer reset, so they follow resolution changes). RESID/PREV hold the
 *   carried residual (rgb, with a validity tag in a) and the last presented
 *   input value for the motion gate; RESID_NEXT/PREV_NEXT are the staging
 *   copies the quantizer writes. LQPREV and MVPREV use only their top-left
 *   quarter / sixteenth. Storage textures start uninitialised, so every
 *   read is guarded by a tag.
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
//!DESC 0 = limited-range output (VP output_range: limited), 1 = full-range
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM swapchain_bits
//!DESC Swapchain surface depth: 10 (default R10G10B10A2) or 8 (diagnostic_force_8bit_sdr_swapchain)
//!TYPE float
//!MINIMUM 8.0
//!MAXIMUM 10.0
10.0

//!PARAM noise_amplitude
//!DESC Decorrelation dither amplitude in 8-bit codes (breaks sigma-delta limit cycles)
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

//!PARAM test_ramp
//!DESC 1 = replace the picture with a 10-bit horizontal grey ramp for verification
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

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
void hook()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (pos.x >= size.x || pos.y >= size.y)
        return;
    ivec2 full = textureSize(HOOKED_raw, 0);
    // The code-domain scale, as in the quantizer below.
    float scale = (levels_full < 0.5 && swapchain_bits > 9.0) ? 255.75 : 255.0;
    float sum = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            ivec2 p = min(pos * 4 + ivec2(x, y), full - ivec2(1));
            vec3 c = (HOOKED_mul * texelFetch(HOOKED_raw, p, 0)).rgb * scale;
            sum += dot(c, vec3(0.2126, 0.7152, 0.0722));
        }
    imageStore(out_image, pos, vec4(sum / 16.0, 0.0, 0.0, 1.0));
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
//!BIND RESID_NEXT
//!BIND PREV_NEXT
//!SAVE MV
//!WIDTH HOOKED.w 7.5 + 16 /
//!HEIGHT HOOKED.h 7.5 + 16 /
//!DESC VideoProcessor sigma-delta: block motion search
//!COMPUTE 8 8

const float VALID_TAG = 0.618;
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

float luma_prev(ivec2 p)
{
    p = clamp(p, ivec2(0), min(full_size, imageSize(PREV_NEXT)) - ivec2(1));
    return dot(imageLoad(PREV_NEXT, p).rgb, vec3(0.2126, 0.7152, 0.0722));
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

void hook()
{
    ivec2 block = ivec2(gl_GlobalInvocationID.xy);
    ivec2 blocks = imageSize(out_image);
    if (block.x >= blocks.x || block.y >= blocks.y)
        return;
    lq_size = textureSize(LUMAQ_raw, 0);
    ivec2 origin = block * 4;

    // Motion compensation off: no search (saves its GPU time); the block's
    // last real vector is kept for when it is switched on again.
    if (motion_compensation < 0.5) {
        imageStore(out_image, block, vec4(0.0));
        return;
    }

    // The previous refresh's luma is valid once this shader has stored it.
    vec4 tag = imageLoad(LQPREV, ivec2(0));
    prev_valid = abs(tag.g - VALID_TAG) < 1e-3 && !isnan(tag.r);
    if (!prev_valid) {
        imageStore(out_image, block, vec4(0.0));
        imageStore(MVPREV, block, vec4(0.0, 0.0, 0.0, VALID_TAG));
        return;
    }

    lc_size = textureSize(LUMAC_raw, 0);

    // Hierarchical block matching. Coarse level (1/16 resolution, one texel
    // per block): every vector within +-4 coarse texels (+-64 full pixels).
    ivec2 coarse = ivec2(0);
    float coarse_sad = sad_coarse(block, coarse);
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            float s = sad_coarse(block, ivec2(dx, dy));
            if (s < coarse_sad) { coarse_sad = s; coarse = ivec2(dx, dy); }
        }

    // Fine level (quarter resolution): every vector within +-2 quarter
    // pixels of zero and of the coarse result, and +-1 of this block's last
    // real motion (kept through repeated film frames).
    vec4 mp = imageLoad(MVPREV, block);
    bool have_prev = abs(mp.a - VALID_TAG) < 1e-3 && !any(isnan(mp.xy));
    ivec2 pv = have_prev ? ivec2(round(mp.xy)) : ivec2(0);
    ivec2 best = ivec2(0);
    float best_sad = sad(origin, best);
    ivec2 centres[3] = ivec2[3](ivec2(0), coarse * 4, pv);
    int radii[3] = int[3](2, 2, 1);
    for (int c = 0; c < 3; c++) {
        if (c > 0 && centres[c] == ivec2(0))
            continue;
        if (c == 2 && pv == coarse * 4)
            continue;
        for (int dy = -radii[c]; dy <= radii[c]; dy++)
            for (int dx = -radii[c]; dx <= radii[c]; dx++) {
                ivec2 v = centres[c] + ivec2(dx, dy);
                float s = sad(origin, v);
                if (s < best_sad) { best_sad = s; best = v; }
            }
    }

    float samples = float((WIN1 - WIN0) * (WIN1 - WIN0));
    float error = best_sad / samples;
    float confidence = error <= match_tolerance ? 1.0 : 0.0;

    // Final refinement to whole pixels (audit, stage 4): the quarter level
    // moves in 4-pixel steps, so every full-pixel vector within +-2 of it is
    // compared on the full-resolution block. Without it a pan that is not a
    // multiple of 4 px per refresh carries from beside the true source and
    // mostly fails the per-pixel gate. Skipped until the previous refresh's
    // values exist (RESID_NEXT tag).
    ivec2 full = best * 4;
    full_size = textureSize(HOOKED_raw, 0);
    full_scale = (levels_full < 0.5 && swapchain_bits > 9.0) ? 255.75 : 255.0;
    ivec2 corner = block * 16;
    ivec2 tagpos = clamp(corner, ivec2(0), imageSize(RESID_NEXT) - ivec2(1));
    if (abs(imageLoad(RESID_NEXT, tagpos).a - VALID_TAG) < 1e-3) {
        ivec2 centre = full;
        float full_sad = sad_full(corner, centre);
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                ivec2 v = centre + ivec2(dx, dy);
                float s = sad_full(corner, v);
                if (s < full_sad) { full_sad = s; full = v; }
            }
        // Trust comes from the full-resolution match: at 4-pixel steps a
        // motion between them matches only roughly at quarter resolution.
        error = full_sad / 256.0;
        confidence = error <= match_tolerance ? 1.0 : 0.0;
    }
    // MV holds the vector in full pixels (MVPREV keeps quarter pixels).
    imageStore(out_image, block, vec4(vec2(full), confidence, error));
    // A repeated film frame (no motion this refresh) keeps the last real
    // vector as the next starting point.
    if (best != ivec2(0) || !have_prev)
        imageStore(MVPREV, block, vec4(vec2(best), confidence, VALID_TAG));
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
//!DESC VideoProcessor sigma-delta: carry state to this refresh
//!COMPUTE 16 16

const float VALID_TAG = 0.618;

void hook()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (pos.x >= size.x || pos.y >= size.y)
        return;
    // Last refresh's results become what this refresh reads.
    ivec2 spos = clamp(pos, ivec2(0), imageSize(RESID) - ivec2(1));
    imageStore(RESID, spos, imageLoad(RESID_NEXT, spos));
    imageStore(PREV, spos, imageLoad(PREV_NEXT, spos));
    // This refresh's luma for the next search (after the search used the
    // previous one). Texel (0,0) carries the validity tag in g.
    ivec2 lq_size = textureSize(LUMAQ_raw, 0);
    if (pos.x % 4 == 0 && pos.y % 4 == 0) {
        ivec2 q = pos / 4;
        ivec2 lc_size = textureSize(LUMAC_raw, 0);
        // Channel b: the coarse luma, at the same index (the coarse grid is
        // smaller, so it fits in the same texels).
        float coarse = (q.x < lc_size.x && q.y < lc_size.y) ?
            (LUMAC_mul * texelFetch(LUMAC_raw, q, 0)).r : 0.0;
        if (q.x < lq_size.x && q.y < lq_size.y)
            imageStore(LQPREV, q, vec4((LUMAQ_mul * texelFetch(LUMAQ_raw, q, 0)).r,
                                       VALID_TAG, coarse, 1.0));
    }
    imageStore(out_image, pos, vec4(0.0));
}

//!HOOK OUTPUT
//!BIND HOOKED
//!BIND MV
//!BIND RESID
//!BIND PREV
//!BIND RESID_NEXT
//!BIND PREV_NEXT
//!DESC VideoProcessor temporal sigma-delta quantizer
//!COMPUTE 16 16

// Tag stored in RESID.a to mark a texel as initialised by this shader.
const float VALID_TAG = 0.618;

// Interleaved gradient noise (Jimenez 2014) with a golden-ratio temporal
// shift: cheap, spatially well distributed, decorrelated frame to frame.
float ign(vec2 p, int f)
{
    p += 5.588238 * float(f & 63);
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// The residual carried from texel `from`, if that texel is initialised and
// its last value is within the motion gate of the value now.
bool carried(ivec2 from, vec3 x, out vec3 r)
{
    r = vec3(0.0);
    vec4 rs = imageLoad(RESID, from);
    vec4 ps = imageLoad(PREV, from);
    if (abs(rs.a - VALID_TAG) >= 1e-3 || any(isnan(rs.rgb)) || any(isnan(ps.rgb)))
        return false;
    if (any(greaterThan(abs(x - ps.rgb), vec3(motion_threshold))))
        return false;
    r = clamp(rs.rgb, -0.5, 0.5);
    return true;
}

void hook()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 out_size = imageSize(out_image);
    if (pos.x >= out_size.x || pos.y >= out_size.y)
        return;

    bool limited = levels_full < 0.5;
    bool swap10 = swapchain_bits > 9.0;

    // Code-domain scale of the hooked texture and the output step that
    // reproduces an exact 8-bit code after the renderer's final store.
    // Limited range on a 10-bit swapchain declared as 8-bit colour depth
    // arrives as 4k/1023 = code8/255.75 (colorspace.c: 235/256 x 2^10/1023);
    // on an 8-bit swapchain, and in full range, as code8/255. Reading the
    // 10-bit case x256 made white 235 read 235.23 and flicker to 236 about
    // one refresh in four (measured on a real 10-bit target, stage 6a audit).
    float in_scale = (limited && swap10) ? 255.75 : 255.0;
    float out_step = (limited && swap10) ? (4.0 / 1023.0) : (1.0 / 255.0);

    vec4 src = HOOKED_mul * texelFetch(HOOKED_raw, pos, 0);
    vec3 x = src.rgb * in_scale;
    // The OUTPUT texture is rgba16f: above mid grey its step is 1/8 of an
    // 8-bit code, and a GPU may round or truncate into it, so an exact code
    // (white 235) can arrive up to 0.125 low and the carried residual would
    // dip it to 234 every few refreshes (seen on WARP: 940 940 940 940 936).
    // A value within 0.13 code of a whole code is that code. Real 10-bit
    // detail comes in steps of 0.25 code and stays further away.
    vec3 whole = floor(x + 0.5);
    x = mix(x, whole, vec3(lessThan(abs(x - whole), vec3(0.13))));

    if (test_ramp > 0.5) {
        // 10-bit horizontal ramp across the legal range, in 8-bit code units.
        float t = float(pos.x) / max(float(out_size.x - 1), 1.0);
        float ramp = limited ? (16.0 + 219.0 * t) : (255.0 * t);
        ramp = floor(ramp * 4.0 + 0.5) * 0.25;   // quantise to 10-bit grid
        x = vec3(ramp);
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

        // Motion-compensated carry: the residual from where this pixel's
        // content was on the previous refresh.
        if (!occluded && motion_compensation > 0.5) {
            vec4 mv = MV_mul * texelFetch(MV_raw,
                min(pos / 16, textureSize(MV_raw, 0) - ivec2(1)), 0);
            ivec2 v = ivec2(round(mv.xy));   // full pixels
            ivec2 from = pos - v;
            if (mv.z > 0.5 && v != ivec2(0) &&
                all(greaterThanEqual(from, ivec2(0))) && all(lessThan(from, state_size)))
                have = carried(from, x, r);
        }
        // Plain motion gate: the same pixel, if it did not change.
        if (!have && !occluded)
            have = carried(spos, x, r);

        // First-order sigma-delta: quantise the error-corrected value, carry
        // the new residual. The dither term is not fed back.
        k = clamp(floor(x + r + d + 0.5), 0.0, 255.0);
        vec3 r_next = clamp(x + r - k, -0.5, 0.5);

        imageStore(RESID_NEXT, spos, vec4(r_next, VALID_TAG));
        imageStore(PREV_NEXT, spos, vec4(x, 1.0));
    } else {
        k = clamp(floor(x + d + 0.5), 0.0, 255.0);
    }

    // With the occlusion mask in alpha, alpha goes back to 1 here.
    vec4 outc = vec4(k * out_step, occlusion_mask > 0.5 ? 1.0 : src.a);
    imageStore(out_image, pos, outc / HOOKED_mul);
}
