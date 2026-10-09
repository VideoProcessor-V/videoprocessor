/*
 * LED drive compensation for VideoProcessor's VP Renderer (libplacebo)
 *
 * Installed by the LED drive module (docs/VP-led-drive-module.md section 6),
 * hooked at PREOUTPUT (libplacebo's PL_HOOK_PRE_OUTPUT; mpv-style shader files
 * spell it without the underscore - an unknown name is silently never run),
 * not as a standard shader. Runs at PRE_OUTPUT: after tone mapping, gamut
 * mapping and the target LUT, before the limited-range encode and before the
 * OUTPUT-stage sigma-delta. Values here are the target's normalised 0-1
 * signal in its transfer (VP's output_gamma).
 *
 * The module sets the parameters every frame through the hook's parameter
 * data (no reparse):
 *   gain         picture gain; 1 = untouched
 *   knee         width of the soft roll-off below white (0-0.5)
 *   gain_domain  0 = multiply the encoded signal (mode: script, emulating the
 *                projector's CNTR control); 1 = multiply linear light
 *                (mode: compensate), linearised with compensation_eotf
 *   compensation_eotf  the projector's real display gamma (FL35: film 2.2)
 *   boost        gamma boost exponent, compensation_eotf / gamma_boost
 *                (2.2 / 2.4 for the old output_gamma 2.4 lift); applied first,
 *                always, even when the LED is not being driven. 1 = off
 *
 * Above 1 - knee the result rolls off with slope 1 at the knee point and
 * approaches 1.0, so a boosted highlight keeps its gradation instead of
 * clipping.
 *
 * Copyright (C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

//!PARAM gain
//!DESC Picture gain (1 = untouched)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 16.0
1.0

//!PARAM knee
//!DESC Soft roll-off width below white
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.08

//!PARAM gain_domain
//!DESC 0 = encoded signal (script), 1 = linear light (compensate)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM compensation_eotf
//!DESC Projector display gamma used to linearise
//!TYPE float
//!MINIMUM 1.0
//!MAXIMUM 3.0
2.2

//!PARAM boost
//!DESC Gamma boost exponent (1 = off)
//!TYPE float
//!MINIMUM 0.2
//!MAXIMUM 3.0
1.0

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor LED drive compensation

vec3 soft_knee(vec3 x, float k)
{
    float t = 1.0 - k;
    vec3 rolled = vec3(t) + vec3(k) * (vec3(1.0) - exp(-(x - vec3(t)) / k));
    return mix(x, rolled, step(vec3(t), x));
}

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
    if (gain == 1.0 && boost == 1.0)
        return color;
    vec3 v = max(color.rgb, vec3(0.0));
    // The gamma boost comes first: the old pipeline boosted VP's output, and
    // the projector's contrast or LED acted on that boosted signal.
    if (boost != 1.0)
        v = pow(v, vec3(boost));
    if (gain == 1.0)
        return vec4(v, color.a);
    bool linear = gain_domain > 0.5;
    vec3 x = linear ? pow(v, vec3(compensation_eotf)) : v;
    x *= gain;
    // Only a boost can push values past white; a reduction never needs the knee.
    if (gain > 1.0)
        x = knee > 0.0001 ? soft_knee(x, knee) : min(x, vec3(1.0));
    color.rgb = linear ? pow(x, vec3(1.0 / compensation_eotf)) : x;
    return color;
}
