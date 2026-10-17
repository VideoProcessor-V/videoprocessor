/*
 * VideoProcessor balanced nonlinear stretch for VP Renderer/libplacebo.
 *
 * This is an original two-axis extension of VideoProcessor's monotonic NLS
 * map. VideoProcessor owns target selection, source geometry, optional
 * presentation crop, safe-fit policy, and transition timing. The hook receives
 * only the bounded runtime ratio/direction and performs the selected mapping.
 *
 * Copyright (C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

//!PARAM stretch_ratio
//!DESC Absolute active-picture correction ratio supplied by VideoProcessor
//!TYPE DYNAMIC float
//!MINIMUM 1.0
//!MAXIMUM 1.5
1.0

//!PARAM warp_axis
//!DESC 0 when source is narrower than target, 1 when source is wider
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
//!DESC VideoProcessor NLS+ Balanced Stretch

float nls_plus_map_radius(float radius, float center_scale,
                          float curve, int geometry,
                          float center_protection)
{
    if (geometry == 0 || center_protection <= 0.0) {
        // This normalized power curve fixes both edges at one. It is monotonic
        // for the complete supported centre-slope range, including the
        // below-one slope used by the complementary axis.
        return center_scale * radius -
            (center_scale - 1.0) * pow(radius, curve);
    }

    if (radius <= center_protection)
        return radius * center_scale;

    float span = 1.0 - center_protection;
    float t = clamp((radius - center_protection) / span, 0.0, 1.0);
    float t2 = t * t;
    float t3 = t2 * t;
    float t4 = t3 * t;
    float t5 = t4 * t;
    float h00 = 1.0 - 10.0 * t3 + 15.0 * t4 - 6.0 * t5;
    float h10 = t - 6.0 * t3 + 8.0 * t4 - 3.0 * t5;
    float h01 = 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
    float h11 = -4.0 * t3 + 7.0 * t4 - 3.0 * t5;
    float start_position = center_protection * center_scale;
    float start_slope = center_scale;
    float edge_slope = max(
        0.20, 1.0 - 0.5 * (center_scale - 1.0) * curve);
    return h00 * start_position + h10 * span * start_slope +
        h01 + h11 * span * edge_slope;
}

float nls_plus_map_coordinate(float coordinate, float center_scale,
                              float curve, int geometry,
                              float center_protection)
{
    float centered = coordinate * 2.0 - 1.0;
    float mapped_radius = nls_plus_map_radius(abs(centered), center_scale,
        curve, geometry, center_protection);
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
    // NLS+ strength controls how much correction moves to the complementary
    // axis. It never weakens the total aspect correction: the two centre
    // slopes always retain the quotient required to preserve centre geometry.
    float strength = clamp(float({{strength}}), 0.0, 1.0);
    float curve = clamp(float({{curve}}), 1.0, 2.9);
    float ratio = clamp(stretch_ratio, 1.0, 1.5);
    float requested_balance =
        clamp(float({{axis_balance}}), 0.0, 1.0) * strength;
    float max_center_zoom =
        clamp(float({{max_center_zoom}}), 1.0, 1.25);
    bool source_wider = warp_axis >= 0.5;
    int geometry = int(clamp(float({{geometry}}), 0.0, 1.0));
    float horizontal_center_protection =
        clamp(float({{horizontal_center_protection}}), 0.0, 0.45);
    float vertical_center_protection =
        clamp(float({{vertical_center_protection}}), 0.0, 0.45);

    // Sharing correction between axes creates uniform centre zoom. Bound that
    // zoom independently of the input ratio so aggressive aspect changes
    // automatically fall back toward the established one-axis map.
    float ratio_log = log(max(ratio, 1.000001));
    float balance_limit = ratio <= 1.000001 ? 1.0 :
        log(max_center_zoom) / ratio_log;
    float balance = min(requested_balance,
        clamp(balance_limit, 0.0, 1.0));

    // q is target/source aspect. The inverse-map centre slopes must satisfy
    // x_slope / y_slope == q to cancel the presentation layer's linear aspect
    // change at every strength. Balance zero reproduces the established
    // selected axis; 0.5 splits the correction equally in logarithmic aspect
    // space when the centre-zoom limit permits it.
    float q = source_wider ? 1.0 / ratio : ratio;
    float x_exponent = source_wider ? balance : 1.0 - balance;
    float y_exponent = source_wider ? balance - 1.0 : -balance;
    float x_scale = pow(q, x_exponent);
    float y_scale = pow(q, y_exponent);

    vec2 sample_pos = HOOKED_pos;
    sample_pos.x = nls_plus_map_coordinate(HOOKED_pos.x, x_scale,
        curve, geometry, horizontal_center_protection);
    sample_pos.y = nls_plus_map_coordinate(HOOKED_pos.y, y_scale,
        curve, geometry, vertical_center_protection);
    sample_pos = clamp(sample_pos, vec2(0.0), vec2(1.0));
    if (sampling < 0.5)
        return HOOKED_tex(sample_pos);

    // Each axis's local slope over one output texel: above 1 it minifies.
    // The map is separable, so the two slopes are the whole local scale.
    vec2 h = 0.5 * HOOKED_pt;
    vec2 slope = vec2(
        abs(nls_plus_map_coordinate(HOOKED_pos.x + h.x, x_scale, curve, geometry,
                horizontal_center_protection) -
            nls_plus_map_coordinate(HOOKED_pos.x - h.x, x_scale, curve, geometry,
                horizontal_center_protection)) / HOOKED_pt.x,
        abs(nls_plus_map_coordinate(HOOKED_pos.y + h.y, y_scale, curve, geometry,
                vertical_center_protection) -
            nls_plus_map_coordinate(HOOKED_pos.y - h.y, y_scale, curve, geometry,
                vertical_center_protection)) / HOOKED_pt.y);
    return nls_sample(sample_pos, slope, bvec2(true, true));
}
