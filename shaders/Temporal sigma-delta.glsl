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
 *   limited range : colour = code8 / 256   (black 16/256, white 235/256)
 *   full range    : colour = code8 / 255
 *   After the hook the renderer multiplies by 1/scale and stores to the
 *   swapchain. For a 10-bit swapchain declared as 8-bit colour depth the
 *   exact code that lands in the top 8 bits is reproduced when this hook
 *   emits   limited: 4k/1023      full: k/255
 *   and for an 8-bit swapchain in either range  k/255.
 *
 * State
 *   Two persistent storage textures sized to the output surface through the
 *   hook chain's {{output_width}} {{output_height}} templates (re-applied on
 *   renderer reset, so they follow resolution changes). The OUTPUT texture is
 *   never larger than the surface. RESID holds the carried residual (rgb) and a validity tag
 *   (a); PREV holds the last presented input code value for the motion gate.
 *   Storage textures start uninitialised, so the tag guards the first use.
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

//!HOOK OUTPUT
//!BIND HOOKED
//!BIND RESID
//!BIND PREV
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
    float in_scale = limited ? 256.0 : 255.0;
    float out_step = (limited && swap10) ? (4.0 / 1023.0) : (1.0 / 255.0);

    vec4 src = HOOKED_mul * texelFetch(HOOKED_raw, pos, 0);
    vec3 x = src.rgb * in_scale;

    if (test_ramp > 0.5) {
        // 10-bit horizontal ramp across the legal range, in 8-bit code units.
        float t = float(pos.x) / max(float(out_size.x - 1), 1.0);
        float ramp = limited ? (16.0 + 219.0 * t) : (255.0 * t);
        ramp = floor(ramp * 4.0 + 0.5) * 0.25;   // quantise to 10-bit grid
        x = vec3(ramp);
    }

    vec3 k;
    vec3 r_next = vec3(0.0);
    vec3 d = vec3((ign(vec2(pos), frame) - 0.5) * 2.0 * noise_amplitude);

    if (enabled > 0.5) {
        ivec2 spos = clamp(pos, ivec2(0), imageSize(RESID) - ivec2(1));
        vec4 rs = imageLoad(RESID, spos);
        vec4 ps = imageLoad(PREV, spos);

        bool valid = abs(rs.a - VALID_TAG) < 1e-3
                  && !any(isnan(rs.rgb)) && !any(isnan(ps.rgb));
        vec3 r = valid ? clamp(rs.rgb, -0.5, 0.5) : vec3(0.0);
        vec3 p = valid ? ps.rgb : x;

        // Motion gate: a pixel that changed meaningfully gets a fresh start.
        if (any(greaterThan(abs(x - p), vec3(motion_threshold))))
            r = vec3(0.0);

        // First-order sigma-delta: quantise the error-corrected value, carry
        // the new residual. The dither term is not fed back.
        k = clamp(floor(x + r + d + 0.5), 0.0, 255.0);
        r_next = clamp(x + r - k, -0.5, 0.5);

        imageStore(RESID, spos, vec4(r_next, VALID_TAG));
        imageStore(PREV, spos, vec4(x, 1.0));
    } else {
        k = clamp(floor(x + d + 0.5), 0.0, 255.0);
    }

    vec4 outc = vec4(k * out_step, src.a);
    imageStore(out_image, pos, outc / HOOKED_mul);
}
