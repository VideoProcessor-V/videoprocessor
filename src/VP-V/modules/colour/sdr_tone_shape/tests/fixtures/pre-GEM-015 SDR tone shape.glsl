/*
 * SDR tone shape and headroom for VideoProcessor's VP Renderer (libplacebo)
 *
 * Delta 13 (docs/VP-delta-11-shadow-boost.md). Installed by the renderer for
 * SDR sources when an input profile sets sdr_tone_shape: knee or
 * sdr_white_headroom above 0 - not as a standard shader. Hooked at PREOUTPUT
 * (libplacebo's PL_HOOK_PRE_OUTPUT), before the LED compensation: after
 * scaling, the Stage 10 restore and the target LUT, before the encode.
 * Values are the target's signal (0-1, above-white above 1) in the output
 * gamma. The reference is SdrToneShape::Map
 * (src/VP-V/modules/colour/sdr_tone_shape/SdrToneShape.h); the two must stay identical.
 *
 *   headroom   reference white at (1 - headroom) of peak (linear scale)
 *   knee_start where the roll-off starts (fraction of peak, linear)
 *   strength   0 = straight line (headroom only), 1 = the full knee
 *   eotf       the output gamma (linearises the signal)
 *   recovery   detail the knee compressed, added back (0 = off)
 *
 * Copyright (C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-only
 */

//!PARAM headroom
//!DESC Reference white below peak (fraction)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.0

//!PARAM knee_start
//!DESC Roll-off start (fraction of peak, linear)
//!TYPE float
//!MINIMUM 0.5
//!MAXIMUM 0.99
0.8

//!PARAM strength
//!DESC Knee strength (0 = straight)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM eotf
//!DESC Output gamma used to linearise
//!TYPE float
//!MINIMUM 1.0
//!MAXIMUM 3.0
2.2

//!PARAM recovery
//!DESC Contrast recovery under the knee (0 = off)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
0.0

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor SDR tone shape

vec3 to_linear(vec3 v)
{
    return sign(v) * pow(abs(v), vec3(eotf));
}

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
    if (headroom == 0.0 && strength == 0.0)
        return color;
    vec3 L = to_linear(color.rgb);
    vec3 y = L * (1.0 - headroom);
    vec3 shaped = y;
    vec3 slope = vec3(1.0);
    if (strength > 0.0)
    {
        // Audit 2026-10-16 (above-white preserved): the knee as before up to
        // the placed white (1 - headroom), so 0..1 is unchanged; above it
        // the curve continues as its tangent at the white (C1, strictly
        // increasing, slope >= exp(-1)) instead of squeezing the source's
        // above-white toward peak. Mirrors SdrToneShape::Knee.
        float white = 1.0 - headroom;
        float k = 1.0 - knee_start;
        vec3 yc = min(y, vec3(white));
        vec3 e = exp(-(max(yc, vec3(knee_start)) - vec3(knee_start)) / k);
        vec3 rolled = vec3(knee_start) + vec3(k) * (vec3(1.0) - e);
        vec3 above = step(vec3(knee_start), yc) * step(vec3(0.0), color.rgb);
        vec3 m = vec3(1.0 - strength) + strength * e;
        vec3 kneed = mix(yc, rolled, strength) + m * max(y - vec3(white), vec3(0.0));
        shaped = mix(y, kneed, above);
        slope = mix(vec3(1.0), m, above);
        if (recovery > 0.0 && any(greaterThan(above, vec3(0.0))))
        {
            // The local detail (the picture minus a 5x5-footprint blur, in
            // linear light), returned where the knee flattened it.
            vec3 blur = vec3(0.0);
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                    blur += to_linear(HOOKED_texOff(vec2(2 * i, 2 * j)).rgb);
            vec3 detail = L - blur / 9.0;
            shaped += recovery * (vec3(1.0) - slope) * (1.0 - headroom) * detail;
        }
    }
    color.rgb = sign(shaped) * pow(abs(shaped), vec3(1.0 / eotf));
    return color;
}
