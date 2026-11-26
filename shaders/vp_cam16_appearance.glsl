/*
 * CAM16 appearance pass for VideoProcessor's VP Renderer (libplacebo), colour.appearance (CF-1 CAM-3).
 *
 * Maps the picture from the reference viewing conditions (large Lw_ref, dim surround) to the
 * screen's (own Lw_screen, the room's surround and veil) with the CIE 2016 colour appearance model:
 * forward with the reference side, inverse with the screen side (M compressed hue-preservingly
 * when a channel would leave the range: a hard 6-step bisection to the gamut boundary, not a soft roll-off).
 * Installed by the renderer only when appearance is on. Hooked at PREOUTPUT after the DV trims (the host's
 * slot order). Codes below black and above white (BTB / WTW) are kept: a pixel wholly outside passes unchanged,
 * a pixel with some channels outside is adapted on its in-range colour and each channel's excess is restored
 * (no gamut-edge discontinuity). The model blends out below apx_knee (gamma_boost_knee, the delta-11 toe).
 * The CPU reference is src/VP-V/modules/colour/appearance/Cam16.h and Cam16Uniforms.h packs these
 * params; the two must stay identical.
 *
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

//!PARAM apr_dc_r
//!DESC reference adaptation gain (CAT16 channel 0)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 20.0
1.0

//!PARAM apr_dc_g
//!DESC reference adaptation gain (CAT16 channel 1)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 20.0
1.0

//!PARAM apr_dc_b
//!DESC reference adaptation gain (CAT16 channel 2)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 20.0
1.0

//!PARAM apr_fl
//!DESC reference luminance-level adaptation FL
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 10.0
1.0

//!PARAM apr_nbb
//!DESC reference background induction Nbb
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 10.0
1.0

//!PARAM apr_aw
//!DESC reference achromatic response of the white
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1000.0
100.0

//!PARAM apr_cz
//!DESC reference c times z
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 5.0
1.0

//!PARAM apr_nc
//!DESC reference chromatic induction Nc
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM apr_t
//!DESC reference (1.64 - 0.29^n)^0.73
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 5.0
1.0

//!PARAM apr_flq
//!DESC reference FL^0.25
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 5.0
1.0

//!PARAM aps_dc_r
//!DESC screen adaptation gain (CAT16 channel 0)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 20.0
1.0

//!PARAM aps_dc_g
//!DESC screen adaptation gain (CAT16 channel 1)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 20.0
1.0

//!PARAM aps_dc_b
//!DESC screen adaptation gain (CAT16 channel 2)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 20.0
1.0

//!PARAM aps_fl
//!DESC screen luminance-level adaptation FL
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 10.0
1.0

//!PARAM aps_nbb
//!DESC screen background induction Nbb
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 10.0
1.0

//!PARAM aps_aw
//!DESC screen achromatic response of the white
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1000.0
100.0

//!PARAM aps_cz
//!DESC screen c times z
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 5.0
1.0

//!PARAM aps_nc
//!DESC screen chromatic induction Nc
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 2.0
1.0

//!PARAM aps_t
//!DESC screen (1.64 - 0.29^n)^0.73
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 5.0
1.0

//!PARAM aps_flq
//!DESC screen FL^0.25
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 5.0
1.0

//!PARAM apx_strength
//!DESC blend of the appearance model (0 = identity)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM apx_veil
//!DESC screen veil as a fraction of the screen white
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM apx_screen_nits
//!DESC screen white in nits (PQ output scale)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 10000.0
100.0

//!PARAM apx_ref_nits
//!DESC reference white in nits (PQ output scale)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 10000.0
100.0

//!PARAM apx_pq
//!DESC 1: PQ output signal; 0: gamma
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.0

//!PARAM apx_gamma
//!DESC output gamma for a gamma signal
//!TYPE DYNAMIC float
//!MINIMUM 1.0
//!MAXIMUM 4.0
2.4

//!PARAM apx_trc_kind
//!DESC output transfer of a gamma signal: 0 power (apx_gamma), 1 BT.1886, 2 sRGB
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 2.0
0.0

//!PARAM apx_trc_param
//!DESC BT.1886 black offset b (0 = pure 2.4)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 100.0
0.0

//!PARAM apx_knee
//!DESC encoded level below which the model blends out (gamma_boost_knee, the delta-11 toe; 0 = none)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 0.5
0.0

//!PARAM apm_00
//!DESC output RGB to XYZ (white Y = 1), row 0 col 0
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.4124

//!PARAM apm_01
//!DESC output RGB to XYZ, row 0 col 1
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.3576

//!PARAM apm_02
//!DESC output RGB to XYZ, row 0 col 2
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.1805

//!PARAM apm_10
//!DESC output RGB to XYZ, row 1 col 0
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.2126

//!PARAM apm_11
//!DESC output RGB to XYZ, row 1 col 1
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.7152

//!PARAM apm_12
//!DESC output RGB to XYZ, row 1 col 2
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.0722

//!PARAM apm_20
//!DESC output RGB to XYZ, row 2 col 0
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.0193

//!PARAM apm_21
//!DESC output RGB to XYZ, row 2 col 1
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.1192

//!PARAM apm_22
//!DESC output RGB to XYZ, row 2 col 2
//!TYPE DYNAMIC float
//!MINIMUM -5.0
//!MAXIMUM 5.0
0.9505

//!PARAM apn_00
//!DESC XYZ to output RGB (inverse of apm), row 0 col 0
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
3.2406

//!PARAM apn_01
//!DESC XYZ to output RGB (inverse of apm), row 0 col 1
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
-1.5372

//!PARAM apn_02
//!DESC XYZ to output RGB (inverse of apm), row 0 col 2
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
-0.4986

//!PARAM apn_10
//!DESC XYZ to output RGB (inverse of apm), row 1 col 0
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
-0.9689

//!PARAM apn_11
//!DESC XYZ to output RGB (inverse of apm), row 1 col 1
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
1.8758

//!PARAM apn_12
//!DESC XYZ to output RGB (inverse of apm), row 1 col 2
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
0.0415

//!PARAM apn_20
//!DESC XYZ to output RGB (inverse of apm), row 2 col 0
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
0.0557

//!PARAM apn_21
//!DESC XYZ to output RGB (inverse of apm), row 2 col 1
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
-0.2040

//!PARAM apn_22
//!DESC XYZ to output RGB (inverse of apm), row 2 col 2
//!TYPE DYNAMIC float
//!MINIMUM -20.0
//!MAXIMUM 20.0
1.0570

//!PARAM apx_hunt_scale
//!DESC factor on M keeping trim x Hunt saturation within the larger alone (doc 115 B13)
//!TYPE DYNAMIC float
//!MINIMUM 0.0
//!MAXIMUM 4.0
1.0

//!HOOK PREOUTPUT
//!BIND HOOKED
//!DESC VideoProcessor CAM16 appearance (colour.appearance)

#define AP_PQ_M1 (2610.0 / 16384.0)
#define AP_PQ_M2 (2523.0 / 4096.0 * 128.0)
#define AP_PQ_C1 (3424.0 / 4096.0)
#define AP_PQ_C2 (2413.0 / 4096.0 * 32.0)
#define AP_PQ_C3 (2392.0 / 4096.0 * 32.0)
#define AP_PI 3.14159265358979323846

float ap_pq_from_nits(float nits)
{
    float p = pow(max(nits, 0.0) / 10000.0, AP_PQ_M1);
    return pow((AP_PQ_C1 + AP_PQ_C2 * p) / (1.0 + AP_PQ_C3 * p), AP_PQ_M2);
}

float ap_nits_from_pq(float pq)
{
    float e = pow(max(pq, 0.0), 1.0 / AP_PQ_M2);
    float den = AP_PQ_C2 - AP_PQ_C3 * e;
    return 10000.0 * pow(max(e - AP_PQ_C1, 0.0) / max(den, 1e-4), 1.0 / AP_PQ_M1);
}

// S2-H2: BT.1886 and sRGB output transfers in relative linear light (odd below 0),
// matching shaders/DV trims.glsl (dv_trc_lin / dv_trc_enc) and ExtendedRange::ToLinear / FromLinear.
// Standards: ITU-R BT.1886 (gamma 2.4, black offset b = apx_trc_param) and IEC 61966-2-1 (sRGB).
float ap_trc_lin(float v)
{
    float a = abs(v);
    float r = a;
    if (apx_trc_kind < 1.5) {
        float b = max(apx_trc_param, 0.0);
        float k = pow(b, 2.4);
        r = (pow(a + b, 2.4) - k) / (pow(1.0 + b, 2.4) - k);
    } else {
        r = a <= 0.04045 ? a * (1.0 / 12.92) : pow((a + 0.055) * (1.0 / 1.055), 2.4);
    }
    return v < 0.0 ? -r : r;
}

float ap_trc_enc(float l)
{
    float a = abs(l);
    float r = a;
    if (apx_trc_kind < 1.5) {
        float b = max(apx_trc_param, 0.0);
        float k = pow(b, 2.4);
        r = pow(a * (pow(1.0 + b, 2.4) - k) + k, 1.0 / 2.4) - b;
    } else {
        r = a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055;
    }
    return l < 0.0 ? -r : r;
}

// Encoded code -> linear light relative to the reference white (1 = white), codes inside 0..1 only.
float ap_to_rel(float v)
{
    if (apx_pq > 0.5)
        return ap_nits_from_pq(v) / max(apx_ref_nits, 1e-6);
    if (apx_trc_kind > 0.5)
        return ap_trc_lin(v);
    return pow(max(v, 0.0), apx_gamma);
}

// Light relative to the nominal white -> encoded code (for ST.2084 PQ, preserve absolute nominal white scale).
float ap_from_rel(float r)
{
    r = max(r, 0.0);
    if (apx_pq > 0.5)
        return ap_pq_from_nits(r * max(apx_ref_nits, 1e-6));
    if (apx_trc_kind > 0.5)
        return ap_trc_enc(r);
    return pow(r, 1.0 / apx_gamma);
}

// GLSL mat3 constructors are column-major: these are the published CAT16 matrices, written by column.
const mat3 AP_M16 = mat3(0.401288, -0.250268, -0.002079,
                         0.650173, 1.204414, 0.048952,
                         -0.051461, 0.045854, 0.953127);
const mat3 AP_M16_INV = mat3(1.862067855087233, 0.387526542878237, -0.015841498849333,
                             -1.011254630531685, 0.621447441346826, -0.034122938028909,
                             0.149186775444452, -0.008973984225064, 1.049964436878243);

float ap_compress(float fl, float x)
{
    float p = pow(fl * abs(x) / 100.0, 0.42);
    float v = 400.0 * p / (27.13 + p);
    return (x < 0.0 ? -v : v) + 0.1;
}

float ap_uncompress(float fl, float ra)
{
    float d = ra - 0.1;
    float ad = abs(d);
    float p = pow(27.13 * ad / max(400.0 - ad, 1e-12), 1.0 / 0.42);
    float v = 100.0 / max(fl, 1e-12) * p;
    return d < 0.0 ? -v : v;
}

float ap_ecc(float h) { return 0.25 * (cos(h + 2.0) + 3.8); }

// Forward (reference side): XYZ with the white at Y = 100 -> (J, M, h radians).
vec3 ap_forward(vec3 xyz)
{
    vec3 r = AP_M16 * xyz;
    float ra = ap_compress(apr_fl, apr_dc_r * r.x);
    float ga = ap_compress(apr_fl, apr_dc_g * r.y);
    float ba = ap_compress(apr_fl, apr_dc_b * r.z);
    float a = ra - 12.0 * ga / 11.0 + ba / 11.0;
    float b = (ra + ga - 2.0 * ba) / 9.0;
    float aa = (2.0 * ra + ga + 0.05 * ba - 0.305) * apr_nbb;
    float h = (a == 0.0 && b == 0.0) ? 0.0 : atan(b, a);
    if (h < 0.0)
        h += 2.0 * AP_PI;
    float J = 100.0 * pow(max(aa / apr_aw, 0.0), apr_cz);
    float denom = ra + ga + 1.05 * ba;
    float t = abs(denom) > 1e-12 ? (50000.0 / 13.0 * apr_nc * apr_nbb * ap_ecc(h) * sqrt(a * a + b * b)) / denom : 0.0;
    float C = pow(max(t, 0.0), 0.9) * sqrt(J / 100.0) * apr_t;
    return vec3(J, C * apr_flq, h);
}

// Inverse (screen side) from (J, M, h radians) -> XYZ with the screen white at Y = 100.
vec3 ap_inverse(float J, float M, float h)
{
    float C = M / max(aps_flq, 1e-12);
    float jr = max(J, 0.0) / 100.0;
    float k = sqrt(jr) * aps_t;
    float t = (k > 1e-12 && C > 0.0) ? pow(C / k, 1.0 / 0.9) : 0.0;
    float et = ap_ecc(h);
    float aa = aps_aw * pow(jr, 1.0 / max(aps_cz, 1e-12));
    float p1 = t > 1e-12 ? (50000.0 / 13.0 * aps_nc * aps_nbb * et) / t : 0.0;
    float p2 = aa / aps_nbb + 0.305;
    float p3 = 21.0 / 20.0;
    float sn = sin(h);
    float cs = cos(h);
    float a = 0.0;
    float b = 0.0;
    if (t > 1e-12) {
        if (abs(sn) >= abs(cs)) {
            float p4 = p1 / sn;
            b = p2 * (2.0 + p3) * (460.0 / 1403.0) /
                (p4 + (2.0 + p3) * (220.0 / 1403.0) * (cs / sn) - 27.0 / 1403.0 + p3 * (6300.0 / 1403.0));
            a = b * (cs / sn);
        } else {
            float p5 = p1 / cs;
            a = p2 * (2.0 + p3) * (460.0 / 1403.0) /
                (p5 + (2.0 + p3) * (220.0 / 1403.0) - (27.0 / 1403.0 - p3 * (6300.0 / 1403.0)) * (sn / cs));
            b = a * (sn / cs);
        }
    }
    float ra = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0;
    float ga = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0;
    float ba = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0;
    vec3 rgb = vec3(ap_uncompress(aps_fl, ra) / max(aps_dc_r, 1e-12),
                    ap_uncompress(aps_fl, ga) / max(aps_dc_g, 1e-12),
                    ap_uncompress(aps_fl, ba) / max(aps_dc_b, 1e-12));
    return AP_M16_INV * rgb;
}

// Signal ceiling in nominal-white-relative linear light: ST.2084 peaks at 10000 nits for PQ, 1.0 for SDR.
float ap_upper()
{
    return apx_pq > 0.5 ? 10000.0 / max(apx_ref_nits, 1e-6) : 1.0;
}

// Flare toe: g(o) = o (o + f) / (o + 2 f), normalised to g(u) = u (u = ap_upper()). Monotonic, no level crushed to 0, f = 0 identity.
float ap_flare(float o, float f)
{
    if (f <= 0.0)
        return o;
    o = max(o, 0.0);
    float u = ap_upper();
    return (o * (o + f) / (o + 2.0 * f)) * ((u + 2.0 * f) / (u + f));
}

// Negated in-range test so any NaN channel evaluates as out-of-range (true).
bool ap_out_of_range(vec3 c)
{
    float upper = ap_upper();
    return !(c.r >= -1e-4 && c.r <= upper + 1e-4 &&
             c.g >= -1e-4 && c.g <= upper + 1e-4 &&
             c.b >= -1e-4 && c.b <= upper + 1e-4);
}

vec4 hook()
{
    vec4 color = HOOKED_texOff(0);
    if (apx_strength <= 0.0)
        return color;
    // BTB / WTW (below black, above white), FIX1 SHOULD 4 / POD2-H2: a pixel wholly below black passes
    // exactly (allBelow -> inRange = 0, outLin = 0, excess < 0); WTW pixels (excess > 0) run the model on
    // the clamped boundary and restore excess afterwards for exact C^0 continuity across all faces/edges/corners of [0, 1]^3.
    vec3 inRange = clamp(color.rgb, 0.0, 1.0);
    vec3 excess = color.rgb - inRange;
    bool outside = excess.r != 0.0 || excess.g != 0.0 || excess.b != 0.0;
    bool allBelow = color.r <= 0.0 && color.g <= 0.0 && color.b <= 0.0;
    if (outside && allBelow)
        return color;

    // Row-major published values, column-major constructor: transpose by listing columns. The inverse is published
    // (apn_*, NIT 1), not inverted per pixel.
    mat3 toXyz = mat3(apm_00, apm_10, apm_20, apm_01, apm_11, apm_21, apm_02, apm_12, apm_22);
    mat3 fromXyz = mat3(apn_00, apn_10, apn_20, apn_01, apn_11, apn_21, apn_02, apn_12, apn_22);

    vec3 lin = vec3(ap_to_rel(inRange.r), ap_to_rel(inRange.g), ap_to_rel(inRange.b));
    vec3 jmh = ap_forward(toXyz * (lin * 100.0));
    jmh.y *= apx_hunt_scale;   // FIX1 SHOULD 2: trim x Hunt within the larger of the two alone (1 without a trim)
    vec3 outLin = fromXyz * ap_inverse(jmh.x, jmh.y, jmh.z) * 0.01;

    // M compression at the same J and h when a channel would leave the range: a hard 6-step bisection of M to the
    // gamut boundary (about 1.6 % of M left as residual), not a soft roll-off (NIT 2).
    if (ap_out_of_range(outLin)) {
        float lo = 0.0;
        float hi = jmh.y;
        bool foundInRange = !ap_out_of_range(fromXyz * ap_inverse(jmh.x, 0.0, jmh.z) * 0.01);
        for (int i = 0; i < 6; i++) {
            float mid = 0.5 * (lo + hi);
            if (ap_out_of_range(fromXyz * ap_inverse(jmh.x, mid, jmh.z) * 0.01)) {
                hi = mid;
            } else {
                lo = mid;
                foundInRange = true;
            }
        }
        vec3 bisected = fromXyz * ap_inverse(jmh.x, lo, jmh.z) * 0.01;
        if (foundInRange) {
            outLin = bisected;
        } else {
            // When D < 1 at J ~ 100 (e.g. D65 white under dim screen with veil), M = 0 is equi-energy E
            // (outside [0, upper] with a red cast) rather than D65 white. If no tested M is in-range,
            // clamp the finite pre-bisection colour to [0, upper] instead of collapsing onto equi-energy E.
            // POD2-L11: FXC-safe bounded finite check (false for NaN/+Inf/-Inf without relying on FXC isnan/isinf).
            bool origFinite = all(greaterThanEqual(outLin, vec3(-1e30))) && all(lessThanEqual(outLin, vec3(1e30)));
            outLin = clamp(origFinite ? outLin : bisected, vec3(0.0), vec3(ap_upper()));
        }
    }

    // Room light reflected by the screen (flare): a toe, never a subtract-and-clip (FIX1 MUST 5; Cam16::FlareToDisplayed).
    outLin = vec3(ap_flare(outLin.r, apx_veil), ap_flare(outLin.g, apx_veil), ap_flare(outLin.b, apx_veil));

    // Blend out below the toe knee, and by strength.
    // FIX1 SHOULD 3: the luma weights are the output primaries' own (the Y row of apm), the knee the configured
    // gamma_boost_knee (an encoded level, taken to relative light).
    float luma = dot(lin, vec3(apm_10, apm_11, apm_12));
    float toe = apx_knee > 0.0 ? smoothstep(0.0, ap_to_rel(apx_knee), luma) : 1.0;
    float w = clamp(apx_strength, 0.0, 1.0) * toe;

    // The identity side on the nominal scale: both PQ and gamma preserve their nominal white at w = 0.
    vec3 inScreen = lin;
    vec3 res = mix(inScreen, outLin, w);
    color.rgb = vec3(ap_from_rel(res.r), ap_from_rel(res.g), ap_from_rel(res.b)) + excess;   // the BTB / WTW excess kept
    return color;
}
