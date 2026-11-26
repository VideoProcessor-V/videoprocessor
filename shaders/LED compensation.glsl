/*
 * Dynamic Drive (LED drive) compensation for VideoProcessor's VP Renderer
 * (libplacebo)
 *
 * Installed by the Dynamic Drive module (docs/VP-led-drive-module.md section 6),
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
 *                boost as before.
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
 * Below black passes through (btb_passthrough, led_btb_passthrough, default
 * on; bug hunt 2 2026-10-17): a negative input leaves this hook with exactly
 * the value it came in with - no boost, no lift, no gain. Why not the odd-
 * symmetric gain (x *= gain on negatives too, the behaviour before)? The gain
 * restores light the dimmed LED took away; a below-black code has no light to
 * restore (the projector shows it as black), so scaling it changes nothing on
 * screen except pushing it towards, and past, the limited-range encode floor:
 * with gain 2.5 in linear light a BTB value grows ~1.46x and codes ~20 and
 * below were clipped to the floor on every dimmed scene (the gamma boost's
 * |v|^(2.2/2.4) expanded them slightly as well). Passing them through keeps
 * every BTB code exactly as captured and distinct (PLUGE-style black-level
 * setup still works, whatever the LED does), and the curve stays continuous
 * (every branch maps 0 to 0) and monotonic: negatives map to themselves,
 * non-negatives to non-negatives. Off = the sign-preserving boost and gain
 * on negatives, as before.
 *
 * A colour only partly out of range (GEM-Q15, owner rulings 2026-11-21: BTB
 * and WTW kept, hue kept, compensated like its neighbours, no pulsing) is
 * split along its own chroma: the in-range part is the same colour
 * desaturated towards its luma just far enough to fit in 0-1 (same hue). That
 * part takes the unchanged per-channel curve, exactly as an in-range
 * neighbour of that colour would. The excursion beyond it (pure chroma, in
 * the colour's own direction) is added back. When no channel is above white,
 * a below-black channel therefore keeps its exact value (with a channel above
 * white as well, it follows its in-range neighbour, continuously). Under a
 * gain below 1, an above-white excursion
 * follows the compensated white (scaled by the curve at 1), so headroom stays
 * smooth across the white edge. The excursion is shortened, all channels by
 * one factor, only as far as needed to stay inside the encodable extended
 * range (extended_floor .. extended_ceiling), so the encode clip can never
 * tilt the hue. A pixel whose luma is out of range (true BTB/WTW, every
 * grey) leaves as it came in (preserve_extended).
 *
 * Above 1 - knee the result rolls off with slope 1 at the knee point and
 * approaches 1.0, so a boosted highlight keeps its gradation instead of
 * clipping.
 *
 * Copyright (C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-only
 */

//!PARAM gain
//!DESC Picture gain (1 = untouched)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 16.0
1.0

//!PARAM knee
//!DESC Soft roll-off width below white
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.08

//!PARAM gain_domain
//!DESC 0 = encoded signal (script), 1 = linear light (compensate)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM compensation_eotf
//!DESC Projector light exponent on the final code (delta 4: 2.41)
//!TYPE DYNAMIC float
//!MINIMUM 1.0
//!MAXIMUM 3.0
2.41

//!PARAM boost
//!DESC Gamma boost exponent (1 = off)
//!TYPE DYNAMIC float
//!MINIMUM 0.2
//!MAXIMUM 3.0
1.0

//!PARAM boost_knee
//!DESC Delta 11: the boost blends into the identity below this (0 = global boost)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.0

//!PARAM boost_knee_width
//!DESC Delta 11: width of the boost toe's smoothstep
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.05

//!PARAM shadow_lift
//!DESC Delta 11: scene-adaptive shadow lift strength (0 = off)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM veil
//!DESC Delta 11: the LED module's veil estimate for this frame (0-1)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM btb_passthrough
//!DESC led_btb_passthrough: 1 = below black leaves the hook as it came in, 0 = boosted and gained with the picture (before 2026-10-17)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!PARAM wtw_passthrough
//!DESC led_wtw_passthrough: 1 = whiter-than-white keeps its value under the boost and any gain (a colour partly above white: split restore, hue kept); 0 = rolled under white (before 2026-10-17)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
1.0

//!PARAM extended_floor
//!DESC Lowest encodable signal of the extended range (default: 10-bit limited code 4, 8-bit code 1)
//!TYPE DYNAMIC float
//!MINIMUM -0.5
//!MAXIMUM 0.0
-0.0684932

//!PARAM extended_ceiling
//!DESC Highest encodable signal of the extended range (default: 10-bit limited code 1019)
//!TYPE DYNAMIC float
//!MINIMUM 1.0
//!MAXIMUM 2.0
1.0901826

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor Dynamic Drive compensation

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

// The picture curve, channel by channel: the boost (with its toe), the lift,
// then the gain `g` with its knee. Used for the pixel (with uniform `gain`) and,
// for a pixel partly out of range, inside preserve_extended.
vec3 compensate_gain(vec3 v, float lift, float g)
{
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
    if (g == 1.0)
        return v;
    bool linear = gain_domain > 0.5;
    vec3 x = linear ? spow(v, compensation_eotf) : v;
    x *= g;
    // Only a boost can push values past white; a reduction never needs the
    // knee. The knee acts on the bright end only (it never touches negatives);
    // below black is restored to its input value at the end when
    // btb_passthrough is on (see the header and preserve_extended), in both
    // gain domains.
    // Bug hunt 2026-10-17: whiter-than-white (above 1, carried by the
    // extended-range restore) keeps its own value instead of being rolled
    // under white with the boosted picture. The knee's output stays below 1,
    // so the result is still monotonic and WTW detail survives (owner rule:
    // BTB and WTW kept in all cases).
    if (g > 1.0) {
        x = knee > 0.0001 ? soft_knee(x, knee) : min(x, vec3(1.0));
    }
    return linear ? spow(x, 1.0 / compensation_eotf) : x;
}

vec3 compensate(vec3 v, float lift)
{
    return compensate_gain(v, lift, gain);
}

// GEM-W1B V6 MUST-2 (register 115 #17), GEM-Q15 (owner rulings 2026-11-21, GEM-W1-LED split restore),
// and GEM-LED-UP-SMOOTH (continuous C1 Hermite transition into true WTW under gain < 1).
// True below black (btb_passthrough) and true whiter-than-white (wtw_passthrough) leave the hook as they
// came in, on every exit path, whatever the boost, the lift or the gain did to the picture. A pixel with no
// channel out of range keeps the per-channel result, byte for byte. Any other pixel is split (header):
//   y = luma (BT.709 weights on the encoded RGB); luma out of range -> the input exactly;
//   p = y + t (o - y), the largest t in [0, 1] that puts the out-of-range channels on 0 / 1 (same hue);
//   e = o - p, the excursion, pure chroma in o's own direction;
//   Under gain < 1 with any channel above white (LED-up), the projective above-white split tAbove
//   runs from 1 at the in-range white edge (max(o) -> 1+) to 0 at the true-WTW luma-1 boundary (y -> 1-).
//   Blending eff_gain = mix(gain, 1.0, s) with the unique minimal C1 cubic Hermite spline s = u^2*(3 - 2*u)
//   (u = 1 - tAbove; h(0)=0, h(1)=1, h'(0)=h'(1)=0, zero free parameters) joins the in-range white edge
//   (eff_gain = gain) smoothly to true WTW (eff_gain = 1, out -> o) with zero hue error and zero jump
//   through gain == 1.
//   out = compensate_gain(p, lift, eff_gain) + k f e, where
//   f = compensate_gain(1, lift, eff_gain).x when anyAbove && eff_gain < 1, else 1;
//   k = 1, lowered (one factor for all channels) only as far as keeps out inside extended_floor ..
//       extended_ceiling, so the encode clip cannot tilt the hue.
vec3 preserve_extended(vec3 result, vec3 original, float lift)
{
    bool wtw = wtw_passthrough > 0.5;
    bool btb = btb_passthrough > 0.5;
    bvec3 above = greaterThan(original, vec3(1.0));
    bvec3 below = lessThan(original, vec3(0.0));
    bool anyAbove = wtw && any(above);
    bool anyBelow = btb && any(below);
    if (!anyAbove && !anyBelow)
        return result;
    float y = dot(original, vec3(0.2126, 0.7152, 0.0722));
    // True BTB passes bit-identically; true WTW also passes bit-identically when gain == 1.0.
    if ((btb && (!(y > 0.0) || all(below))) || (wtw && gain == 1.0 && (y > 1.0 || all(above))))
        return original;
    // W23-C2-S3-H1: keep exact C0 continuity across white (1,1,1) and across y = 1.0 on both neutral and
    // chromatic rays for any gain != 1.0. For y > 1.0, anchor p = vec3(1.0) (matching lim_{y->1-} y + t*d)
    // and blend eff_gain from gain -> 1.0 via the minimal C1 cubic Hermite spline h(u) = u^2*(3 - 2u) over
    // u = clamp((max(original) - 1.0) / max(extended_ceiling - 1.0, 1e-4), 0, 1) so eff_gain = gain at the
    // white edge (zero cliff) and eff_gain = 1.0 at extended_ceiling (true WTW preserved monotonically).
    vec3 p;
    if (y > 1.0) {
        p = vec3(1.0);
    } else {
        vec3 d = original - vec3(y);
        // Above white: d > 0 and 1 - y >= 0; below black: d < 0 and y > 0. Every quotient is finite.
        vec3 tv = vec3(1.0);
        if (anyAbove)
            tv = mix(tv, vec3(1.0 - y) / max(d, vec3(1e-6)), vec3(above));
        if (anyBelow)
            tv = mix(tv, vec3(y) / max(-d, vec3(1e-6)), vec3(below));
        float t = clamp(min(tv.r, min(tv.g, tv.b)), 0.0, 1.0);
        p = vec3(y) + t * d;
    }
    float over = max(max(original.r, max(original.g, original.b)) - 1.0, 0.0);
    float u = clamp(over / max(extended_ceiling - 1.0, 1e-4), 0.0, 1.0);
    float s = u * u * (3.0 - 2.0 * u);
    float eff_gain = (anyAbove && gain != 1.0) ? mix(gain, 1.0, s) : gain;
    vec3 c = compensate_gain(p, lift, eff_gain);
    float f = (anyAbove && eff_gain < 1.0) ? compensate_gain(vec3(1.0), lift, eff_gain).x : 1.0;
    vec3 e = (original - p) * f;
    vec3 up = (vec3(extended_ceiling) - c) / max(e, vec3(1e-6));
    vec3 dn = (c - vec3(extended_floor)) / max(-e, vec3(1e-6));
    vec3 kv = mix(vec3(1.0), up, vec3(greaterThan(e, vec3(1e-6))));
    kv = mix(kv, dn, vec3(lessThan(e, vec3(-1e-6))));
    float k = clamp(min(kv.r, min(kv.g, kv.b)), 0.0, 1.0);
    return c + k * e;
}

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
    float lift = shadow_lift * veil;
    if (gain == 1.0 && boost == 1.0 && !(lift > 0.0))
        return color;
    color.rgb = preserve_extended(compensate(color.rgb, lift), color.rgb, lift);
    return color;
}
