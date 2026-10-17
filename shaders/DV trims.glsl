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

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor Dolby Vision L2 trims (non-certified)

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

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
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
