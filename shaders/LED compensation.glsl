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
 *   compensation_eotf  the projector's measured light exponent on the final
 *                code (FL35 light-meter fit: 2.41, delta 4); used once, at
 *                the end, for the gain in linear light
 *   boost        gamma boost exponent, output gamma / gamma_boost (2.2 / 2.4
 *                for the old output_gamma 2.4 lift on VP's 2.2 output);
 *                applied first, always, even when the LED is not being
 *                driven. 1 = off
 *   boost_knee, boost_knee_width  delta 11: below boost_knee the boost
 *                blends (smoothstep over the width) into the identity, so the
 *                darkest tones keep their native contrast; 0 = the global
 *                boost as before. Below black is then untouched.
 *   shadow_lift, veil  delta 11's ANSI branch: a lift of the darks below the
 *                knee (0.10 when boost_knee is 0), shadow_lift x veil, where
 *                veil (0-1) is the LED module's per-frame estimate of the
 *                scatter veil from the frame's highlights. Zero at black and
 *                at the knee, monotonic, positive values only.
 *
 * Sign-preserving (BTB/WTW answers item 5): every power acts on |v| with v's
 * sign, so below-black detail (negative values here, before the limited-
 * range encode) survives the boost and the gain instead of being cut to 0.
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
//!DESC Projector light exponent on the final code (delta 4: 2.41)
//!TYPE float
//!MINIMUM 1.0
//!MAXIMUM 3.0
2.41

//!PARAM boost
//!DESC Gamma boost exponent (1 = off)
//!TYPE float
//!MINIMUM 0.2
//!MAXIMUM 3.0
1.0

//!PARAM boost_knee
//!DESC Delta 11: the boost blends into the identity below this (0 = global boost)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.0

//!PARAM boost_knee_width
//!DESC Delta 11: width of the boost toe's smoothstep
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.05

//!PARAM shadow_lift
//!DESC Delta 11: scene-adaptive shadow lift strength (0 = off)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM veil
//!DESC Delta 11: the LED module's veil estimate for this frame (0-1)
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor LED drive compensation

// Sign-preserving power: below-black values keep their sign and magnitude.
vec3 spow(vec3 v, float e)
{
    return sign(v) * pow(abs(v), vec3(e));
}

vec3 soft_knee(vec3 x, float k)
{
    float t = 1.0 - k;
    // Audit 2026-10-16: clamp the exponent at the knee. For x far below it
    // with a small k, exp(-(x - t) / k) overflowed to +Inf and the mix below
    // gave Inf * 0 = NaN; below the knee `rolled` is discarded anyway, so the
    // output is unchanged wherever it was finite (as SDR tone shape.glsl).
    vec3 rolled = vec3(t) + vec3(k) * (vec3(1.0) - exp(-max(x - vec3(t), vec3(0.0)) / k));
    return mix(x, rolled, step(vec3(t), x));
}

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
    float lift = shadow_lift * veil;
    if (gain == 1.0 && boost == 1.0 && !(lift > 0.0))
        return color;
    vec3 v = color.rgb;
    // The gamma boost comes first: the old pipeline boosted VP's output, and
    // the projector's contrast or LED acted on that boosted signal.
    if (boost != 1.0) {
        vec3 boosted = spow(v, boost);
        // Delta 11: the toe. Below the knee the identity, above knee + width
        // the boost; the blend is monotonic (the boost only lifts).
        if (boost_knee > 0.0)
            boosted = mix(v, boosted, smoothstep(vec3(boost_knee), vec3(boost_knee + max(boost_knee_width, 1e-4)), v));
        v = boosted;
    }
    // Delta 11, ANSI: lift the darks in proportion to the predicted veil.
    // u(1-u)^2 peaks at u = 1/3; scaled to at most a quarter of the knee so
    // the curve stays monotonic (slope >= 1 - 0.5625 x lift).
    if (lift > 0.0) {
        float k = boost_knee > 0.0 ? boost_knee : 0.10;
        vec3 u = clamp(v / k, 0.0, 1.0);
        v += vec3(lift * k * 1.6875) * u * (vec3(1.0) - u) * (vec3(1.0) - u);
    }
    if (gain == 1.0)
        return vec4(v, color.a);
    bool linear = gain_domain > 0.5;
    vec3 x = linear ? spow(v, compensation_eotf) : v;
    x *= gain;
    // Only a boost can push values past white; a reduction never needs the
    // knee. The knee acts on the bright end only; below black is untouched.
    if (gain > 1.0)
        x = knee > 0.0001 ? soft_knee(x, knee) : min(x, vec3(1.0));
    color.rgb = linear ? spow(x, 1.0 / compensation_eotf) : x;
    return color;
}
