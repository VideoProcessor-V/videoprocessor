/*
 * VP-V post-decode I-only Dolby Vision debander (colour.dm_pre, CF-DV P5)
 *
 * Runs at MAIN (after libplacebo's DV decode, before colour mapping) on the
 * decoded PQ picture. Debands strictly the BT.2100 ICtCp intensity channel I
 * using VP-V's deterministic 4-sample ring + spatial Laplacian/extremum grain
 * guard, leaving Ct and Cp untouched.
 *
 * BTB/WTW safe: never clamps input RGB or I to [0, 1]. Uses odd-symmetric PQ
 * below 0 and exact C1 linear extension above 1 so sub-black (< 0) and
 * super-white (> 1) round-trip without clipping or poles.
 *
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

//!PARAM dm_deband_active
//!DESC 1 = active, 0 = byte-identical bypass
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dm_deband_thr
//!DESC Deband threshold in normalised 12-bit I units (codes / 4095)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dm_deband_radius
//!DESC Sample ring radius in pixels (16 px at 1080 lines, scaled by height)
//!TYPE DYNAMIC float
//!MINIMUM 4.0
//!MAXIMUM 64.0
16.0

//!PARAM dm_deband_grain_guard
//!DESC 1 = spatial Laplacian/extremum grain guard on, 0 = off
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!PARAM dm_deband_ictcp_input
//!DESC 1 = HOOKED is already (I, Ct, Cp, A), 0 = BT.2020 PQ (R, G, B, A)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dm_active_x0
//!DESC Normalized active-picture left edge [0..1] (letterbox/pillarbox/blanking guard)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dm_active_y0
//!DESC Normalized active-picture top edge [0..1] (letterbox/pillarbox/blanking guard)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dm_active_x1
//!DESC Normalized active-picture right edge [0..1] (letterbox/pillarbox/blanking guard)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!PARAM dm_active_y1
//!DESC Normalized active-picture bottom edge [0..1] (letterbox/pillarbox/blanking guard)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!HOOK MAIN
//!BIND HOOKED
//!DESC VP-V DV post-decode I-only debander (colour.dm_pre)

// SMPTE ST 2084 PQ constants (exact match to DvMath.h)
#define DM_PQ_M1 (2610.0 / 16384.0)
#define DM_PQ_M2 (2523.0 / 4096.0 * 128.0)
#define DM_PQ_C1 (3424.0 / 4096.0)
#define DM_PQ_C2 (2413.0 / 4096.0 * 32.0)
#define DM_PQ_C3 (2392.0 / 4096.0 * 32.0)
// Exact C1 derivative of ST 2084 EOTF at PQ = 1.0: ((1 + c3) / (c2 - c3)) / (m1 * m2).
// Linear extension above 1.0 keeps WTW strictly invertible with zero clipping and zero poles.
#define DM_PQ_SLOPE_1 (((1.0 + DM_PQ_C3) / (DM_PQ_C2 - DM_PQ_C3)) / (DM_PQ_M1 * DM_PQ_M2))

float dm_pq_to_lin(float v)
{
    float a = abs(v);
    float r;
    if (a > 1.0) {
        r = 1.0 + DM_PQ_SLOPE_1 * (a - 1.0);
    } else {
        float e = pow(a, 1.0 / DM_PQ_M2);
        float num = max(e - DM_PQ_C1, 0.0);
        float den = max(DM_PQ_C2 - DM_PQ_C3 * e, 1e-6);
        r = pow(num / den, 1.0 / DM_PQ_M1);
    }
    return v < 0.0 ? -r : r;
}

float dm_lin_to_pq(float l)
{
    float a = abs(l);
    float r;
    if (a > 1.0) {
        r = 1.0 + (a - 1.0) / DM_PQ_SLOPE_1;
    } else {
        float p = pow(a, DM_PQ_M1);
        r = pow((DM_PQ_C1 + DM_PQ_C2 * p) / (1.0 + DM_PQ_C3 * p), DM_PQ_M2);
    }
    return l < 0.0 ? -r : r;
}

// ITU-R BT.2100 Table 7: linear BT.2020 RGB <-> LMS (DvMath::kRgb2020ToLms and exact inverse).
vec3 dm_rgb2020_to_lms(vec3 rgb)
{
    return vec3(
        (1688.0 * rgb.r + 2146.0 * rgb.g + 262.0 * rgb.b) / 4096.0,
        (683.0 * rgb.r + 2951.0 * rgb.g + 462.0 * rgb.b) / 4096.0,
        (99.0 * rgb.r + 309.0 * rgb.g + 3688.0 * rgb.b) / 4096.0);
}

vec3 dm_lms_to_rgb2020(vec3 lms)
{
    return vec3(
        dot(vec3(3.4366066943, -2.5064521187, 0.0698454243), lms),
        dot(vec3(-0.7913295556, 1.9836004518, -0.1922708962), lms),
        dot(vec3(-0.0259498997, -0.0989137147, 1.1248636144), lms));
}

// ITU-R BT.2100 Table 7: PQ LMS <-> ICtCp (DvMath::kLmsToIctcp and exact inverse).
vec3 dm_lms_to_ictcp(vec3 p)
{
    return vec3(
        0.5 * p.x + 0.5 * p.y,
        (6610.0 * p.x - 13613.0 * p.y + 7003.0 * p.z) / 4096.0,
        (17933.0 * p.x - 17390.0 * p.y - 543.0 * p.z) / 4096.0);
}

vec3 dm_ictcp_to_lms(vec3 c)
{
    return vec3(
        dot(vec3(1.0, 0.0086090370, 0.1110296250), c),
        dot(vec3(1.0, -0.0086090370, -0.1110296250), c),
        dot(vec3(1.0, 0.5600313357, -0.3206271750), c));
}

vec3 dm_rgb_pq_to_ictcp(vec3 rgb_pq)
{
    vec3 lin = vec3(dm_pq_to_lin(rgb_pq.r), dm_pq_to_lin(rgb_pq.g), dm_pq_to_lin(rgb_pq.b));
    vec3 lms = dm_rgb2020_to_lms(lin);
    vec3 lms_pq = vec3(dm_lin_to_pq(lms.x), dm_lin_to_pq(lms.y), dm_lin_to_pq(lms.z));
    return dm_lms_to_ictcp(lms_pq);
}

vec3 dm_ictcp_to_rgb_pq(vec3 ictcp)
{
    vec3 lms_pq = dm_ictcp_to_lms(ictcp);
    vec3 lms = vec3(dm_pq_to_lin(lms_pq.x), dm_pq_to_lin(lms_pq.y), dm_pq_to_lin(lms_pq.z));
    vec3 lin = dm_lms_to_rgb2020(lms);
    return vec3(dm_lin_to_pq(lin.r), dm_lin_to_pq(lin.g), dm_lin_to_pq(lin.b));
}

float dm_rgb_pq_to_i(vec3 rgb_pq)
{
    vec3 lin = vec3(dm_pq_to_lin(rgb_pq.r), dm_pq_to_lin(rgb_pq.g), dm_pq_to_lin(rgb_pq.b));
    float l = (1688.0 * lin.r + 2146.0 * lin.g + 262.0 * lin.b) / 4096.0;
    float m = (683.0 * lin.r + 2951.0 * lin.g + 462.0 * lin.b) / 4096.0;
    return 0.5 * dm_lin_to_pq(l) + 0.5 * dm_lin_to_pq(m);
}

float dm_sample_i(vec2 pos, bool ictcp_in)
{
    vec4 s = HOOKED_tex(pos);
    return ictcp_in ? s.r : dm_rgb_pq_to_i(s.rgb);
}

vec4 hook()
{
    vec4 c = HOOKED_tex(HOOKED_pos);
    if (dm_deband_active < 0.5 || dm_deband_thr <= 0.0)
        return c;

    // POD2-H3: Active-picture / letterbox / edge-blanking guard.
    // Leave pixels outside the active rect untouched and veto/clamp any tap that crosses outside.
    vec2 act_min = vec2(min(dm_active_x0, dm_active_x1), min(dm_active_y0, dm_active_y1));
    vec2 act_max = vec2(max(dm_active_x0, dm_active_x1), max(dm_active_y0, dm_active_y1));
    if (HOOKED_pos.x < act_min.x || HOOKED_pos.x > act_max.x ||
        HOOKED_pos.y < act_min.y || HOOKED_pos.y > act_max.y)
        return c;
    vec2 tap_min = min(act_min + 0.5 * HOOKED_pt, act_max);
    vec2 tap_max = max(act_max - 0.5 * HOOKED_pt, act_min);

    bool ictcp_in = dm_deband_ictcp_input > 0.5;
    vec3 ictcp = ictcp_in ? c.rgb : dm_rgb_pq_to_ictcp(c.rgb);
    float I_c = ictcp.x;

    // VP-V deterministic per-pixel 4-sample ring hash (exact match to VpDebander.cpp).
    ivec2 p = ivec2(floor(HOOKED_pos * HOOKED_size));
    uint h = uint(p.x) * 1973u + uint(p.y) * 9277u + 26699u;
    h = (h ^ (h >> 13u)) * 1540483477u;
    h ^= h >> 15u;
    float r1 = float(h & 65535u) / 65535.0;
    float r2 = float(h >> 16u) / 65535.0;
    float d = mix(1.0, dm_deband_radius, r1);
    vec2 o = vec2(cos(r2 * 6.28318530718), sin(r2 * 6.28318530718)) * d;
    vec2 q = vec2(-o.y, o.x);

    // Symmetric pair veto: only include an opposing ring pair (+o/-o or +q/-q) when both taps lie strictly
    // inside [tap_min, tap_max], preventing any linear-sampler bleed across letterbox/blanked borders.
    vec2 p0 = HOOKED_pos + o * HOOKED_pt;
    vec2 p1 = HOOKED_pos - o * HOOKED_pt;
    vec2 p2 = HOOKED_pos + q * HOOKED_pt;
    vec2 p3 = HOOKED_pos - q * HOOKED_pt;
    bool ok_o = p0.x >= tap_min.x && p0.x <= tap_max.x && p0.y >= tap_min.y && p0.y <= tap_max.y &&
                p1.x >= tap_min.x && p1.x <= tap_max.x && p1.y >= tap_min.y && p1.y <= tap_max.y;
    bool ok_q = p2.x >= tap_min.x && p2.x <= tap_max.x && p2.y >= tap_min.y && p2.y <= tap_max.y &&
                p3.x >= tap_min.x && p3.x <= tap_max.x && p3.y >= tap_min.y && p3.y <= tap_max.y;
    float sum_I = 0.0;
    float n_taps = 0.0;
    if (ok_o) {
        sum_I += dm_sample_i(p0, ictcp_in) + dm_sample_i(p1, ictcp_in);
        n_taps += 2.0;
    }
    if (ok_q) {
        sum_I += dm_sample_i(p2, ictcp_in) + dm_sample_i(p3, ictcp_in);
        n_taps += 2.0;
    }
    if (n_taps <= 0.0)
        return c;
    float I_avg = sum_I / n_taps;

    // Soft blur weight w (VpDebander.cpp: 1 at <= 3/5 * thr, 0 at >= thr).
    float diff = abs(I_c - I_avg);
    float w = 1.0 - smoothstep(0.6 * dm_deband_thr, dm_deband_thr, diff);

    // P10 / Spatial Grain Guard on I: 4-neighbor Laplacian + local extremum veto (clamped to active rect).
    // Preserves film grain and fine oscillations while leaving monotone banded staircases unattenuated.
    if (dm_deband_grain_guard > 0.5 && w > 0.0) {
        float I_n = dm_sample_i(clamp(HOOKED_pos + vec2(0.0, -1.0) * HOOKED_pt, tap_min, tap_max), ictcp_in);
        float I_s = dm_sample_i(clamp(HOOKED_pos + vec2(0.0, 1.0) * HOOKED_pt, tap_min, tap_max), ictcp_in);
        float I_e = dm_sample_i(clamp(HOOKED_pos + vec2(1.0, 0.0) * HOOKED_pt, tap_min, tap_max), ictcp_in);
        float I_w = dm_sample_i(clamp(HOOKED_pos + vec2(-1.0, 0.0) * HOOKED_pt, tap_min, tap_max), ictcp_in);
        float lap = abs(4.0 * I_c - I_n - I_s - I_e - I_w);
        float lap_thr = dm_deband_thr / 3.0;    // GrainPreserve::kLapShareOfThreshold (1/3)
        float ext_thr = 0.25 * dm_deband_thr;   // strict local extremum threshold (1/4)
        bool ext_x = ((I_c - I_w) > ext_thr && (I_c - I_e) > ext_thr) ||
                     ((I_w - I_c) > ext_thr && (I_e - I_c) > ext_thr);
        bool ext_y = ((I_c - I_n) > ext_thr && (I_c - I_s) > ext_thr) ||
                     ((I_n - I_c) > ext_thr && (I_s - I_c) > ext_thr);
        bool non_mono = ((I_c - I_w) * (I_e - I_c) < 0.0) || ((I_c - I_n) * (I_s - I_c) < 0.0);
        if (ext_x || ext_y) {
            w = 0.0;
        } else if (non_mono) {
            w *= 1.0 - smoothstep(0.5 * lap_thr, lap_thr, lap);
        }
    }

    if (w <= 0.0)
        return c;

    float I_out = mix(I_c, I_avg, w);
    if (ictcp_in)
        return vec4(I_out, c.g, c.b, c.a);
    return vec4(dm_ictcp_to_rgb_pq(vec3(I_out, ictcp.y, ictcp.z)), c.a);
}
