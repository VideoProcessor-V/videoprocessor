/*
 * VideoProcessor nonlinear stretch for Alpha/libplacebo
 *
 * mpv user-shader format, parsed by libplacebo. VideoProcessor owns the
 * active-picture policy, trusted crop, target aspect, safe-fit decision, and
 * transition timing. This hook only performs the selected nonlinear mapping.
 *
 * The mpv shader format and the availability of horizontal/vertical mapping
 * were evaluated against NLS-Next. This implementation uses VideoProcessor's
 * established monotonic mapping and does not require NLS-Next's Lua helper.
 *
 * Copyright (C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

//!PARAM stretch_ratio
//!DESC Active-picture stretch ratio supplied by VideoProcessor
//!TYPE DYNAMIC float
//!MINIMUM 1.0
//!MAXIMUM 1.5
1.0

//!PARAM warp_axis
//!DESC 0 for horizontal mapping, 1 for vertical mapping
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM sampling
//!DESC Set by VideoProcessor (nls_sampling): 1 = separable Catmull-Rom with a minification low-pass (faithful), 0 = bilinear
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!HOOK MAIN
//!BIND HOOKED
//!DESC VideoProcessor Nonlinear Stretch

// The NLS map of one normalised coordinate along the warped axis.
float nls_map(float coordinate, float effective_ratio, float curve,
              int geometry, float center_protection)
{
    float centered = coordinate * 2.0 - 1.0;
    float radius = abs(centered);
    float mapped_radius;

    if (geometry == 0 || center_protection <= 0.0) {
        // Preserve the centre after the presentation layer's required linear
        // expansion, then return smoothly to the untouched source edge. The
        // previous r * localScale formulation had a 0.356 edge slope for the
        // default 16:9-to-scope ratio and curve=2, making the sides appear
        // crushed. This normalized power curve gives 0.678 instead: plainly
        // nonlinear while retaining believable edge geometry.
        mapped_radius = effective_ratio * radius -
            (effective_ratio - 1.0) * pow(radius, curve);
    } else if (radius <= center_protection) {
        mapped_radius = radius * effective_ratio;
    } else {
        float span = 1.0 - center_protection;
        float t = clamp(
            (radius - center_protection) / span, 0.0, 1.0);
        float t2 = t * t;
        float t3 = t2 * t;
        float t4 = t3 * t;
        float t5 = t4 * t;
        float h00 = 1.0 - 10.0 * t3 + 15.0 * t4 - 6.0 * t5;
        float h10 = t - 6.0 * t3 + 8.0 * t4 - 3.0 * t5;
        float h01 = 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
        float h11 = -4.0 * t3 + 7.0 * t4 - 3.0 * t5;
        float start_position = center_protection * effective_ratio;
        float start_slope = effective_ratio;
        float edge_slope = max(
            0.20, 1.0 - 0.5 * (effective_ratio - 1.0) * curve);
        mapped_radius =
            h00 * start_position + h10 * span * start_slope +
            h01 + h11 * span * edge_slope;
    }

    return (centered < 0.0 ? -mapped_radius : mapped_radius) * 0.5 + 0.5;
}

// Hunt 2 M1 (VideoProcessor, `nls_sampling`): the warp reads the source
// with a separable Catmull-Rom (B = 0, C = 0.5) from texelFetch, fp32
// weights, and on an axis the map minifies (slope above 1, the centre at up
// to the stretch ratio) the kernel is widened by the slope, a low-pass that
// stops the decimation aliasing. On an axis the map leaves alone the source
// texel is read as it is. Catmull-Rom is exact at a texel centre and keeps
// 0.8x or more of white grain at every fractional position (bilinear keeps
// 0.5x at a half-pixel offset; see GrainPreserve.h). sampling = 0 restores
// the hooked texture's bilinear sampler.
float nls_catmull(float x)
{
    x = abs(x);
    if (x < 1.0)
        return 1.0 + x * x * (-2.5 + 1.5 * x);
    if (x < 2.0)
        return 2.0 + x * (-4.0 + x * (2.5 - 0.5 * x));
    return 0.0;
}

// At most 9 taps per warped axis (the widening is bounded to 2).
#define NLS_MAX_TAPS 9

// pos: normalised sample position; slope: the map's local slope per axis
// (output texel to source texels); warped: the axes the map moves.
vec4 nls_sample(vec2 pos, vec2 slope, bvec2 warped)
{
    ivec2 hi = ivec2(HOOKED_size) - ivec2(1);
    vec2 c = pos * HOOKED_size - vec2(0.5);   // texel centres at whole numbers
    vec2 sc = clamp(slope, vec2(1.0), vec2(2.0));
    // An axis the map does not move: its own texel, no filtering.
    ivec2 lo = ivec2(floor(c + vec2(0.5)));
    int nx = 1;
    int ny = 1;
    if (warped.x) {
        lo.x = int(floor(c.x - 2.0 * sc.x)) + 1;
        nx = min(int(ceil(c.x + 2.0 * sc.x)) - lo.x, NLS_MAX_TAPS);
    }
    if (warped.y) {
        lo.y = int(floor(c.y - 2.0 * sc.y)) + 1;
        ny = min(int(ceil(c.y + 2.0 * sc.y)) - lo.y, NLS_MAX_TAPS);
    }
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < NLS_MAX_TAPS; j++) {
        if (j >= ny)
            break;
        int y = lo.y + j;
        float wy = warped.y ? nls_catmull((float(y) - c.y) / sc.y) : 1.0;
        vec4 row = vec4(0.0);
        float rsum = 0.0;
        for (int i = 0; i < NLS_MAX_TAPS; i++) {
            if (i >= nx)
                break;
            int x = lo.x + i;
            float wx = warped.x ? nls_catmull((float(x) - c.x) / sc.x) : 1.0;
            row += wx * texelFetch(HOOKED_raw, clamp(ivec2(x, y), ivec2(0), hi), 0);
            rsum += wx;
        }
        acc += wy * row;
        wsum += wy * rsum;
    }
    // Normalised: a widened kernel's taps do not sum to exactly one.
    return HOOKED_mul * (acc / wsum);
}

vec4 hook()
{
    float strength = clamp(float({{strength}}), 0.0, 1.0);
    // A classic power curve needs an exponent of at least one to preserve a
    // monotonic source-coordinate map. The upper bound also keeps its edge
    // slope positive at the largest supported stretch ratio.
    float curve = clamp(float({{curve}}), 1.0, 2.9);
    float active_stretch_ratio = clamp(stretch_ratio, 1.0, 1.5);
    bool vertical_warp = warp_axis >= 0.5;
    int geometry = int(clamp(float({{geometry}}), 0.0, 1.0));
    float center_protection =
        clamp(float({{center_protection}}), 0.0, 0.45);
    float effective_ratio = mix(1.0, active_stretch_ratio, strength);

    vec2 sample_pos = HOOKED_pos;
    float coordinate = vertical_warp ? sample_pos.y : sample_pos.x;
    float mapped = nls_map(coordinate, effective_ratio, curve, geometry,
        center_protection);
    if (vertical_warp)
        sample_pos.y = mapped;
    else
        sample_pos.x = mapped;
    sample_pos = clamp(sample_pos, vec2(0.0), vec2(1.0));
    if (sampling < 0.5)
        return HOOKED_tex(sample_pos);

    // The map's local slope over one output texel: above 1 it minifies.
    float pt = vertical_warp ? HOOKED_pt.y : HOOKED_pt.x;
    float slope = abs(
        nls_map(coordinate + 0.5 * pt, effective_ratio, curve, geometry, center_protection) -
        nls_map(coordinate - 0.5 * pt, effective_ratio, curve, geometry, center_protection)) / pt;
    return nls_sample(sample_pos, vec2(slope),
        bvec2(!vertical_warp, vertical_warp));
}
