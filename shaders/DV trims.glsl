/*
 * Dolby Vision Level 2 trims for VideoProcessor's VP Renderer (libplacebo)
 *
 * DV extension item 3 (2026-10-17, renderer key dv_trims: off by default).
 * NOT Dolby display management and NOT certified: an independent
 * reimplementation of the L2 trim (slope/offset/power, chroma weight,
 * saturation gain) applied to libplacebo's tone-mapped output. Installed by
 * the renderer for the Dolby Vision tunnel only, not as a standard shader.
 * Hooked at PREOUTPUT (after tone mapping and the encode to the output's
 * signal). The reference is DolbyVisionTrims::ApplyPixel
 * (src/VideoProcessor-Lib/vprenderer/DolbyVisionTrims.h); the two must stay
 * identical.
 *
 *   dv_slope, dv_offset, dv_power  the CDL on PQ(Y) / PQ(peak)
 *   dv_chroma_weight          saturation follows (Y'/Y)^chroma_weight
 *   dv_sat_gain               saturation gain
 *   dv_signal_pq              1: PQ output; 0: gamma output (gamma, peak_nits)
 *   dv_peak_nits              the display peak the trims refer to
 *   dv_kr, dv_kb              luminance coefficients (kg = 1 - kr - kb)
 *   dv_ext                    1: the CM v4.0 L8 extras below are active (feat/dv-l8);
 *                             0: the L2 arithmetic exactly (DvTrimSelection::ApplyPixel)
 *   dv_rotate                 1: the hue vector is not neutral
 *   dv_mid_contrast           L8 target_mid_contrast bend (0 = neutral)
 *   dv_clip_trim              L8 clip_trim highlight bend (0 = neutral)
 *   dv_sat_r .. dv_sat_m      L8 saturation vector: chroma gain at R/Y/G/C/B/M (1 = neutral)
 *   dv_hue_r .. dv_hue_m      L8 hue vector: rotation in degrees at R/Y/G/C/B/M (0 = neutral)
 *   dv_space                  1: the trims in ICtCp (BT.2100) for the Dolby-style DV tone
 *                             mapping (DvDolbyStyle::ApplyPixelIctcp, kept identical)
 *   dv_lms00 .. dv_lms22      output linear RGB -> LMS (the output primaries to BT.2100 LMS)
 *   dv_ilms00 .. dv_ilms22    its inverse (computed once on the CPU)
 * With L8 the reference is DvTrimSelection::ApplyPixel
 * (src/VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h).
 *
 * Copyright (C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

//!PARAM dv_slope
//!DESC Trim slope (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_offset
//!DESC Trim offset (0 = neutral)
//!TYPE float
//!MINIMUM -1.0
//!MAXIMUM 1.0
0.0

//!PARAM dv_power
//!DESC Trim power (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_chroma_weight
//!DESC Trim chroma weight (0 = neutral)
//!TYPE float
//!MINIMUM -1.0
//!MAXIMUM 1.0
0.0

//!PARAM dv_sat_gain
//!DESC Trim saturation gain (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_signal_pq
//!DESC 1 = PQ output signal, 0 = gamma
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!PARAM dv_gamma
//!DESC Output gamma (gamma signal)
//!TYPE float
//!MINIMUM 1.0
//!MAXIMUM 3.0
2.2

//!PARAM dv_peak_nits
//!DESC Display peak (nits)
//!TYPE float
//!MINIMUM 1.0
//!MAXIMUM 10000.0
1000.0

//!PARAM dv_kr
//!DESC Luminance coefficient R
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.2627

//!PARAM dv_kb
//!DESC Luminance coefficient B
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0593

//!PARAM dv_ext
//!DESC L8 extras active (feat/dv-l8)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dv_rotate
//!DESC L8 hue vector active
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dv_mid_contrast
//!DESC L8 mid contrast (0 = neutral)
//!TYPE float
//!MINIMUM -0.5
//!MAXIMUM 0.5
0.0

//!PARAM dv_clip_trim
//!DESC L8 clip trim (0 = neutral)
//!TYPE float
//!MINIMUM -0.5
//!MAXIMUM 0.5
0.0

//!PARAM dv_sat_r
//!DESC L8 saturation at red (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_sat_y
//!DESC L8 saturation at yellow (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_sat_g
//!DESC L8 saturation at green (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_sat_c
//!DESC L8 saturation at cyan (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_sat_b
//!DESC L8 saturation at blue (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_sat_m
//!DESC L8 saturation at magenta (1 = neutral)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM dv_hue_r
//!DESC L8 hue rotation at red in degrees (0 = neutral)
//!TYPE float
//!MINIMUM -45.0
//!MAXIMUM 45.0
0.0

//!PARAM dv_hue_y
//!DESC L8 hue rotation at yellow in degrees (0 = neutral)
//!TYPE float
//!MINIMUM -45.0
//!MAXIMUM 45.0
0.0

//!PARAM dv_hue_g
//!DESC L8 hue rotation at green in degrees (0 = neutral)
//!TYPE float
//!MINIMUM -45.0
//!MAXIMUM 45.0
0.0

//!PARAM dv_hue_c
//!DESC L8 hue rotation at cyan in degrees (0 = neutral)
//!TYPE float
//!MINIMUM -45.0
//!MAXIMUM 45.0
0.0

//!PARAM dv_hue_b
//!DESC L8 hue rotation at blue in degrees (0 = neutral)
//!TYPE float
//!MINIMUM -45.0
//!MAXIMUM 45.0
0.0

//!PARAM dv_hue_m
//!DESC L8 hue rotation at magenta in degrees (0 = neutral)
//!TYPE float
//!MINIMUM -45.0
//!MAXIMUM 45.0
0.0

//!PARAM dv_space
//!DESC 0: luminance trims; 1: ICtCp trims (dv_tone_mapping dolby_style)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM dv_lms00
//!DESC Output RGB -> LMS (BT.2100) row 0 col 0
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.4120

//!PARAM dv_lms01
//!DESC Output RGB -> LMS (BT.2100) row 0 col 1
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.5239

//!PARAM dv_lms02
//!DESC Output RGB -> LMS (BT.2100) row 0 col 2
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.0640

//!PARAM dv_lms10
//!DESC Output RGB -> LMS (BT.2100) row 1 col 0
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.1667

//!PARAM dv_lms11
//!DESC Output RGB -> LMS (BT.2100) row 1 col 1
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.7204

//!PARAM dv_lms12
//!DESC Output RGB -> LMS (BT.2100) row 1 col 2
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.1128

//!PARAM dv_lms20
//!DESC Output RGB -> LMS (BT.2100) row 2 col 0
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.0241

//!PARAM dv_lms21
//!DESC Output RGB -> LMS (BT.2100) row 2 col 1
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.0755

//!PARAM dv_lms22
//!DESC Output RGB -> LMS (BT.2100) row 2 col 2
//!TYPE float
//!MINIMUM -4.0
//!MAXIMUM 4.0
0.9004

//!PARAM dv_ilms00
//!DESC LMS -> output RGB row 0 col 0 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
3.0

//!PARAM dv_ilms01
//!DESC LMS -> output RGB row 0 col 1 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
-2.2

//!PARAM dv_ilms02
//!DESC LMS -> output RGB row 0 col 2 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
0.07

//!PARAM dv_ilms10
//!DESC LMS -> output RGB row 1 col 0 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
-0.69

//!PARAM dv_ilms11
//!DESC LMS -> output RGB row 1 col 1 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
1.72

//!PARAM dv_ilms12
//!DESC LMS -> output RGB row 1 col 2 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
-0.03

//!PARAM dv_ilms20
//!DESC LMS -> output RGB row 2 col 0 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
-0.02

//!PARAM dv_ilms21
//!DESC LMS -> output RGB row 2 col 1 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
-0.08

//!PARAM dv_ilms22
//!DESC LMS -> output RGB row 2 col 2 (the inverse of dv_lms)
//!TYPE float
//!MINIMUM -8.0
//!MAXIMUM 8.0
1.12

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor Dolby Vision L2/L8 trims (non-certified)

#define PQ_M1 (2610.0 / 16384.0)
#define PQ_M2 (2523.0 / 4096.0 * 128.0)
#define PQ_C1 (3424.0 / 4096.0)
#define PQ_C2 (2413.0 / 4096.0 * 32.0)
#define PQ_C3 (2392.0 / 4096.0 * 32.0)

float pq_from_nits(float nits)
{
    float p = pow(max(nits, 0.0) / 10000.0, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

float nits_from_pq(float pq)
{
    float e = pow(max(pq, 0.0), 1.0 / PQ_M2);
    return 10000.0 * pow(max(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), 1.0 / PQ_M1);
}

float to_nits(float v)
{
    if (dv_signal_pq > 0.5)
        return sign(v) * nits_from_pq(abs(v));   // below-black kept
    return sign(v) * pow(abs(v), dv_gamma) * dv_peak_nits;
}

float from_nits(float n)
{
    if (dv_signal_pq > 0.5)
        return sign(n) * pq_from_nits(abs(n));
    return sign(n) * pow(abs(n) / dv_peak_nits, 1.0 / dv_gamma);
}

// DolbyVisionTrims constants: the offset fade-in (16 PQ codes), the near-black
// luminance (64 PQ codes) below which the change is additive, and the cap on
// the luminance ratio chroma follows (review C1/C2).
#define DV_OFFSET_FADE_PQ (16.0 / 4095.0)
#define DV_NEAR_BLACK_PQ (64.0 / 4095.0)
#define DV_MAX_CHROMA_RATIO 4.0

// The shoulder (review S3): identity up to k = 2 - x_max, then a C1 roll-off
// reaching exactly 1 at x_max with slope 0; x_max <= 1: identity.
float dv_shoulder(float x, float x_max)
{
    if (x_max <= 1.0)
        return x;
    float k = 2.0 - x_max;
    if (x <= k)
        return x;
    float u = clamp((x - k) / (2.0 * (1.0 - k)), 0.0, 1.0);
    return k + (1.0 - k) * (2.0 * u - u * u);
}

// feat/dv-l8: the L8 bends on the trimmed value (DvTrimSelection::MidContrast / ClipTrim).
float dv_mid_bend(float v)
{
    if (dv_mid_contrast == 0.0 || !(v > 0.0) || !(v < 1.0))
        return v;
    return v + dv_mid_contrast * (v - 0.5) * 4.0 * v * (1.0 - v);
}

float dv_clip_bend(float v)
{
    if (dv_clip_trim == 0.0)
        return v;
    float u = clamp((v - 0.5) / 0.5, 0.0, 1.0);
    return v + dv_clip_trim * u * u * (1.0 - u);
}

float dv_hue_interp(float a0, float a1, float a2, float a3, float a4, float a5, float hdeg)
{
    float pos = hdeg / 60.0;
    float fl = floor(pos);
    int i0 = int(fl) % 6;
    float f = pos - fl;
    float v[7] = float[7](a0, a1, a2, a3, a4, a5, a0);
    return v[i0] * (1.0 - f) + v[i0 + 1] * f;
}

bool dv_sat_neutral()
{
    return dv_sat_r == 1.0 && dv_sat_y == 1.0 && dv_sat_g == 1.0 && dv_sat_c == 1.0 && dv_sat_b == 1.0 && dv_sat_m == 1.0;
}

// The L8 path (dv_ext = 1): DvTrimSelection::ApplyPixel, kept identical.
vec4 dv_hook_ext(vec4 color)
{
    vec3 L = vec3(to_nits(color.r), to_nits(color.g), to_nits(color.b));
    float kg = 1.0 - dv_kr - dv_kb;
    float Y = dv_kr * L.r + kg * L.g + dv_kb * L.b;
    if (!(Y > 0.0))
        return color;
    float peak_pq = pq_from_nits(dv_peak_nits);
    float t = pq_from_nits(Y) / peak_pq;
    float inside = min(t, 1.0);
    float fade = clamp(inside / (DV_OFFSET_FADE_PQ / peak_pq), 0.0, 1.0);
    float v = inside * dv_slope + dv_offset * fade;
    v = dv_shoulder(max(v, 0.0), dv_slope + dv_offset);
    v = pow(clamp(v, 0.0, 1.0), dv_power);
    v = dv_clip_bend(dv_mid_bend(v));
    if (t > 1.0)
        v += t - 1.0;
    float Y2 = nits_from_pq(v * peak_pq);
    float r = Y2 / Y;
    vec3 C = L - vec3(Y);
    float hue_sat = 1.0;
    bool rotated = false;
    bool sat_neutral = dv_sat_neutral();
    float x = C.r - 0.5 * (C.g + C.b);
    float y = 0.86602540378443864676 * (C.g - C.b);
    if ((dv_rotate > 0.5 || !sat_neutral) && !(x == 0.0 && y == 0.0)) {
        float hdeg = degrees(atan(y, x));
        if (hdeg < 0.0)
            hdeg += 360.0;
        if (!sat_neutral)
            hue_sat = dv_hue_interp(dv_sat_r, dv_sat_y, dv_sat_g, dv_sat_c, dv_sat_b, dv_sat_m, hdeg);
        float turn = dv_rotate > 0.5 ? dv_hue_interp(dv_hue_r, dv_hue_y, dv_hue_g, dv_hue_c, dv_hue_b, dv_hue_m, hdeg) : 0.0;
        if (turn != 0.0) {
            float a = radians(turn);
            float x2 = x * cos(a) - y * sin(a);
            float y2 = x * sin(a) + y * cos(a);
            float m = (C.r + C.g + C.b) / 3.0;
            C = vec3(m + 2.0 / 3.0 * x2, m - x2 / 3.0 + y2 / 1.73205080756887729353, m - x2 / 3.0 - y2 / 1.73205080756887729353);
            C -= vec3(dv_kr * C.r + kg * C.g + dv_kb * C.b);
            rotated = true;
        }
    }
    float sat = clamp(dv_sat_gain * hue_sat * (dv_chroma_weight == 0.0 ? 1.0 : pow(max(r, 1e-6), dv_chroma_weight)), 0.0, 2.0);
    float y_near = nits_from_pq(DV_NEAR_BLACK_PQ);
    float w = clamp((Y - y_near) / y_near, 0.0, 1.0);
    float additive = 1.0 + (sat - 1.0) * clamp(Y / y_near, 0.0, 1.0);
    float multiplicative = sat * min(r, DV_MAX_CHROMA_RATIO);
    float gain = mix(additive, multiplicative, w);
    for (int c = 0; c < 3; c++) {
        if (L[c] >= 0.0 && C[c] < 0.0) {
            float limit = Y2 / -C[c];
            if (rotated)
                gain = min(gain, limit);
            else if (gain > 1.0)
                gain = min(gain, max(limit, 1.0));
        }
    }
    vec3 outn = vec3(Y2) + gain * C;
    color.rgb = vec3(from_nits(outn.r), from_nits(outn.g), from_nits(outn.b));
    return color;
}

// feat/dv-l8 task 5: the trims in ICtCp (DvDolbyStyle::ApplyPixelIctcp).
const mat3 DV_LMS_TO_ICTCP = transpose(mat3(0.5, 0.5, 0.0,
                                            6610.0 / 4096.0, -13613.0 / 4096.0, 7003.0 / 4096.0,
                                            17933.0 / 4096.0, -17390.0 / 4096.0, -543.0 / 4096.0));

vec3 dv_from_ictcp(vec3 ictcp, mat3 from_lms)
{
    vec3 pq = inverse(DV_LMS_TO_ICTCP) * ictcp;
    vec3 lms = vec3(nits_from_pq(pq.x), nits_from_pq(pq.y), nits_from_pq(pq.z)) / 10000.0;
    return (from_lms * lms) * 10000.0;
}

vec4 dv_hook_ictcp(vec4 color)
{
    mat3 to_lms = transpose(mat3(dv_lms00, dv_lms01, dv_lms02, dv_lms10, dv_lms11, dv_lms12, dv_lms20, dv_lms21, dv_lms22));
    mat3 from_lms = transpose(mat3(dv_ilms00, dv_ilms01, dv_ilms02, dv_ilms10, dv_ilms11, dv_ilms12, dv_ilms20, dv_ilms21, dv_ilms22));
    vec3 L = vec3(to_nits(color.r), to_nits(color.g), to_nits(color.b));
    vec3 pos = max(L, vec3(0.0));
    vec3 carry = L - pos;
    if (!(pos.r + pos.g + pos.b > 0.0))
        return color;
    vec3 lms = to_lms * (pos / 10000.0);
    vec3 lmspq = vec3(pq_from_nits(lms.x * 10000.0), pq_from_nits(lms.y * 10000.0), pq_from_nits(lms.z * 10000.0));
    vec3 ictcp = DV_LMS_TO_ICTCP * lmspq;
    if (!(ictcp.x > 0.0))
        return color;
    float peak_pq = pq_from_nits(dv_peak_nits);
    float t = ictcp.x / peak_pq;
    float inside = min(t, 1.0);
    float fade = clamp(inside / (DV_OFFSET_FADE_PQ / peak_pq), 0.0, 1.0);
    float v = inside * dv_slope + dv_offset * fade;
    v = dv_shoulder(max(v, 0.0), dv_slope + dv_offset);
    v = pow(clamp(v, 0.0, 1.0), dv_power);
    if (dv_ext > 0.5)
        v = dv_clip_bend(dv_mid_bend(v));
    if (t > 1.0)
        v += t - 1.0;
    float I2 = v * peak_pq;
    float kg = 1.0 - dv_kr - dv_kb;
    float Y = dv_kr * pos.r + kg * pos.g + dv_kb * pos.b;
    vec3 C = pos - vec3(Y);
    float hue_sat = 1.0;
    float turn = 0.0;
    bool sat_neutral = dv_sat_neutral();
    float x = C.r - 0.5 * (C.g + C.b);
    float y = 0.86602540378443864676 * (C.g - C.b);
    if (dv_ext > 0.5 && (dv_rotate > 0.5 || !sat_neutral) && !(x == 0.0 && y == 0.0)) {
        float hdeg = degrees(atan(y, x));
        if (hdeg < 0.0)
            hdeg += 360.0;
        if (!sat_neutral)
            hue_sat = dv_hue_interp(dv_sat_r, dv_sat_y, dv_sat_g, dv_sat_c, dv_sat_b, dv_sat_m, hdeg);
        if (dv_rotate > 0.5)
            turn = dv_hue_interp(dv_hue_r, dv_hue_y, dv_hue_g, dv_hue_c, dv_hue_b, dv_hue_m, hdeg);
    }
    float ratio = I2 / ictcp.x;
    float gain = clamp(dv_sat_gain * hue_sat * (dv_chroma_weight == 0.0 ? 1.0 : pow(max(ratio, 1e-6), dv_chroma_weight)), 0.0, 2.0);
    float a = radians(turn);
    float ct = ictcp.y * cos(a) - ictcp.z * sin(a);
    float cp = ictcp.y * sin(a) + ictcp.z * cos(a);
    float k = gain;
    vec3 outn = dv_from_ictcp(vec3(I2, ct * k, cp * k), from_lms);
    float tolerance = -1e-4 * (pos.r + pos.g + pos.b);   // DvDolbyStyle::kBelowTolerance
    for (int iter = 0; iter < 12; iter++) {
        bool below = false;
        for (int c = 0; c < 3; c++)
            below = below || outn[c] < tolerance;
        if (!below)
            break;
        k *= 0.5;
        outn = iter == 11 ? dv_from_ictcp(vec3(I2, 0.0, 0.0), from_lms) : dv_from_ictcp(vec3(I2, ct * k, cp * k), from_lms);
    }
    outn = max(outn, vec3(0.0)) + carry;
    color.rgb = vec3(from_nits(outn.r), from_nits(outn.g), from_nits(outn.b));
    return color;
}

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
    if (dv_space > 0.5)
        return dv_hook_ictcp(color);
    if (dv_ext > 0.5)
        return dv_hook_ext(color);
    vec3 L = vec3(to_nits(color.r), to_nits(color.g), to_nits(color.b));
    float kg = 1.0 - dv_kr - dv_kb;
    float Y = dv_kr * L.r + kg * L.g + dv_kb * L.b;
    if (!(Y > 0.0))
        return color;
    float peak_pq = pq_from_nits(dv_peak_nits);
    float t = pq_from_nits(Y) / peak_pq;
    float inside = min(t, 1.0);
    float fade = clamp(inside / (DV_OFFSET_FADE_PQ / peak_pq), 0.0, 1.0);
    float v = inside * dv_slope + dv_offset * fade;
    v = dv_shoulder(max(v, 0.0), dv_slope + dv_offset);
    v = pow(clamp(v, 0.0, 1.0), dv_power);
    if (t > 1.0)
        v += t - 1.0;
    float Y2 = nits_from_pq(v * peak_pq);
    float r = Y2 / Y;
    float sat = clamp(dv_sat_gain * (dv_chroma_weight == 0.0 ? 1.0 : pow(max(r, 1e-6), dv_chroma_weight)), 0.0, 2.0);
    float y_near = nits_from_pq(DV_NEAR_BLACK_PQ);
    float w = clamp((Y - y_near) / y_near, 0.0, 1.0);
    float additive = 1.0 + (sat - 1.0) * clamp(Y / y_near, 0.0, 1.0);
    float multiplicative = sat * min(r, DV_MAX_CHROMA_RATIO);
    float gain = mix(additive, multiplicative, w);
    // Gamut: a gain above 1 never pushes a channel at or above black below it.
    for (int c = 0; c < 3; c++) {
        if (gain > 1.0 && L[c] >= 0.0 && L[c] < Y)
            gain = min(gain, max(Y2 / (Y - L[c]), 1.0));
    }
    vec3 outn = vec3(Y2) + gain * (L - vec3(Y));
    color.rgb = vec3(from_nits(outn.r), from_nits(outn.g), from_nits(outn.b));
    return color;
}
