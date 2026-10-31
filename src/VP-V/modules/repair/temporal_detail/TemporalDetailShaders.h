// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// DRAFT - not dispatched, not compiled by libplacebo yet.
// TemporalDetail's GLSL bodies in the renderer's convention (libplacebo hook bodies as C++ raw strings;
// pattern: texture_restore/DetailPulse.cpp). They mirror TemporalDetailModel.h pass by pass and use the
// constants of TemporalDetail.h by name through Defines(). Nothing includes this header except the model
// test (a compile-only reference). Passes: split, block measures, validate + weights, transfer + recompose.
// Reductions run in a fixed tree (workgroup order) so the result is deterministic.
#pragma once

#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>

#include <string>

namespace TemporalDetail
{
	namespace Shaders
	{
		// The #define block the shaders are compiled with (the CPU model's constants, same names).
		inline std::string Defines(double planeLines)
		{
			const double s = Scale(planeLines);
			auto d = [](const char* name, double v) { return std::string("#define ") + name + " " + std::to_string(v) + "\n"; };
			std::string t;
			t += d("TD_SCALE", s);
			t += d("TD_BLOCK", kBlockPx * s);
			t += d("TD_SIGMA_FINE", kSigmaFine * s);
			t += d("TD_SIGMA_BASE", kSigmaBase * s);
			t += d("TD_SIGMA_LOW", kSigmaLow * s);
			t += d("TD_GRAIN_CAL", kGrainCal);
			t += d("TD_WIENER_KAPPA", kWienerKappa);
			t += d("TD_ENERGY_FLOOR", kEnergyFloor);
			t += d("TD_RHO2_LO", kRho2Lo);
			t += d("TD_RHO2_HI", kRho2Hi);
			t += d("TD_PULL_LO", kPullLo);
			t += d("TD_PULL_HI", kPullHi);
			t += d("TD_UNCERTAINTY_PX", kUncertaintyPx * s);
			t += d("TD_BASE_NOISE", kBaseNoiseCodes);
			t += d("TD_BASE_GRAIN_SIGMAS", kBaseGrainSigmas);
			t += d("TD_CORR_MIN", kCorrMin);
			t += d("TD_STRONGER_MARGIN", kStrongerMargin);
			t += d("TD_GAIN_LO", kGainLo);
			t += d("TD_GAIN_HI", kGainHi);
			t += d("TD_PIXEL_CAP", kPixelCap);
			return t;
		}

		// Pass 1: split. Half-resolution base G(sigma_base) Y, D1 / D2 / E3 bands, per-pixel Wiener share f.
		// Outputs: base (r16f, half res), s1 = f D1, s2 = f D2 (rg16f), g = (1 - f) D (r16f, kept for the audit only).
		constexpr const char* kSplit = R"GLSL(
// DRAFT - not dispatched, not compiled by libplacebo yet
// split: base / bands / Wiener share (README step 1)
float gauss_w(int k, float sigma) { return exp(-0.5 * float(k * k) / (sigma * sigma)); }

float blur_at(sampler2D src, ivec2 p, float sigma, bool horizontal)
{
    int r = max(1, int(ceil(3.0 * sigma)));
    float a = 0.0, ws = 0.0;
    for (int k = -r; k <= r; k++) {
        float w = gauss_w(k, sigma);
        ivec2 q = clamp(p + (horizontal ? ivec2(k, 0) : ivec2(0, k)), ivec2(0), textureSize(src, 0) - 1);
        a += w * texelFetch(src, q, 0).r;
        ws += w;
    }
    return a / ws;
}

// block grain level N (quiet sub-blocks, minimum over 5 x 5 blocks) comes from the block-measures pass of the
// previous dispatch of the same frame (tdN, one texel per block).
void split_pixel(ivec2 p, out float y, out float base, out float d1, out float d2, out float f)
{
    y = texelFetch(src_luma, p, 0).r;
    float g1 = blur2(src_luma, p, TD_SIGMA_FINE);      // separable pair, rows then columns
    base = blur2(src_luma, p, TD_SIGMA_BASE);
    d1 = y - g1;
    d2 = g1 - base;
    float d = d1 + d2;
    float e = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            float v = texelFetch(src_luma, clamp(p + ivec2(i, j), ivec2(0), textureSize(src_luma, 0) - 1), 0).r;
            float vb = texelFetch(tdBaseFull, clamp(p + ivec2(i, j), ivec2(0), textureSize(src_luma, 0) - 1), 0).r;
            e += (v - vb) * (v - vb);
        }
    e /= 9.0;
    float N = texelFetch(tdN, p / int(TD_BLOCK), 0).r;
    f = clamp(1.0 - TD_WIENER_KAPPA * N / max(e, TD_ENERGY_FLOOR), 0.0, 1.0);
}
)GLSL";

		// Pass 2: block measures. Fixed-tree reduction over the block: E1, E2, E3, F, q (README step 2).
		constexpr const char* kBlockMeasures = R"GLSL(
// DRAFT - not dispatched, not compiled by libplacebo yet
// block measures: one workgroup per block, fixed reduction tree
shared vec4 td_rd[256];

vec4 td_reduce(vec4 v)
{
    uint li = gl_LocalInvocationIndex;
    barrier();
    td_rd[li] = v;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) td_rd[li] += td_rd[li + s];
        barrier();
    }
    return td_rd[0];
}

void block_measures()
{
    ivec2 b = ivec2(gl_WorkGroupID.xy);
    int B = int(TD_BLOCK);
    vec4 acc = vec4(0.0);
    // 256 invocations cover the block in a strided raster (B / 16 pixels per invocation per axis)
    int step = max(1, B / 16);
    for (int j = 0; j < step; j++)
        for (int i = 0; i < step; i++) {
            ivec2 p = b * B + ivec2(gl_LocalInvocationID.xy) * step + ivec2(i, j);
            if (any(greaterThanEqual(p, textureSize(td_s1, 0)))) continue;
            float s1 = texelFetch(td_s1, p, 0).r, s2 = texelFetch(td_s2, p, 0).r, d3 = texelFetch(td_d3, p, 0).r;
            acc += vec4(s1 * s1, s2 * s2, d3 * d3, 1.0);
        }
    acc = td_reduce(acc);
    if (gl_LocalInvocationIndex == 0u) {
        float e1 = acc.x / acc.w, e2 = acc.y / acc.w, e3 = acc.z / acc.w;
        float F = e2 / (e2 + e3 + TD_ENERGY_FLOOR);
        imageStore(td_measures, b, vec4(e1, e2, F, e1 + TD_ENERGY_FLOOR));   // E1, E2, F, q
    }
}
)GLSL";

		// Pass 3: validation + weights per block (README steps 3, 6, 7): the model's Eval and the limiter.
		constexpr const char* kValidate = R"GLSL(
// DRAFT - not dispatched, not compiled by libplacebo yet
// validate + weights: per block, the warped measures and the smooth gates (they multiply)
float td_smooth(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }

struct TdGates { float unc, base, corr, stronger, hb, focus, age; };

TdGates td_gates(float e1c, float e1w, float e2c, float e2w, float corr, float baseDiff, float N,
                 float Fc, float Fw, float u, float ageSeconds, float maxAge)
{
    TdGates g;
    g.corr = td_smooth(0.5 * TD_CORR_MIN, TD_CORR_MIN, corr);
    g.stronger = td_smooth(1.0 + TD_STRONGER_MARGIN, 1.0 + 3.0 * TD_STRONGER_MARGIN, (e1w + TD_ENERGY_FLOOR) / (e1c + TD_ENERGY_FLOOR));
    g.hb = td_smooth(TD_RHO2_LO, TD_RHO2_HI, (e2c + TD_ENERGY_FLOOR) / (e2w + TD_ENERGY_FLOOR));
    g.focus = td_smooth(TD_PULL_LO, TD_PULL_HI, Fc / (Fw + 1e-6));
    g.unc = 1.0 - td_smooth(0.3 * TD_UNCERTAINTY_PX, TD_UNCERTAINTY_PX, u);
    float allow = TD_BASE_NOISE + TD_BASE_GRAIN_SIGMAS * sqrt(max(N, 0.0)) / 4.0;
    g.base = 1.0 - td_smooth(allow, 2.0 * allow, baseDiff);
    g.age = clamp((maxAge - ageSeconds) / (maxAge * 0.5), 0.0, 1.0);
    return g;
}

// hysteresis (enter 0.15, leave 0.05) and the rate limiter (rise 1 / Frames(0.15 s), fall 1 / Frames(0.3 s))
float td_limit(float target, float w, inout float on, float rise, float fall)
{
    if (on < 0.5 && target >= 0.15) on = 1.0;
    else if (on > 0.5 && target < 0.05) on = 0.0;
    float t = on > 0.5 ? target : 0.0;
    return w + clamp(t - w, -fall, rise);
}
)GLSL";

		// Pass 4: transfer + recompose (README steps 8-9). Catmull-Rom 4 x 4 taps from the unwarped slot at
		// p + C(p); bilinear weights between block centres; bit-exact copy where w = b = 0.
		constexpr const char* kTransfer = R"GLSL(
// DRAFT - not dispatched, not compiled by libplacebo yet
// transfer + recompose: Y_out = Y + (S_out - S)
void td_cr(float t, out float w[4])
{
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

float td_sample(sampler2D tex, vec2 pos)
{
    vec2 f = floor(pos);
    float wx[4], wy[4];
    td_cr(pos.x - f.x, wx);
    td_cr(pos.y - f.y, wy);
    ivec2 o = ivec2(f) - 1, hi = textureSize(tex, 0) - 1;
    float a = 0.0;
    for (int j = 0; j < 4; j++) {
        float row = 0.0;
        for (int i = 0; i < 4; i++) row += wx[i] * texelFetch(tex, clamp(o + ivec2(i, j), ivec2(0), hi), 0).r;
        a += wy[j] * row;
    }
    return a;
}

float td_transfer_pixel(ivec2 p, float S, float base, vec4 w4, vec4 b4, vec4 om)
{
    // w4 / b4: the weights and boosts of the four nearest blocks; om: their bilinear weights.
    float delta = 0.0, refMax = 0.0, bsum = 0.0;
    for (int k = 0; k < 4; k++) {
        float w = w4[k], b = b4[k], o = om[k];
        if (o <= 0.0 || (w <= 0.0 && b <= 0.0)) continue;
        if (w > 0.0) {
            vec2 pos = vec2(p) + td_composed(k, p);                    // composed map of the block's slot
            float sw = td_sample(td_slot_s, pos), bw = td_sample(td_slot_base, pos);
            float g = (bw > 1e-3 && base > 1e-3) ? clamp(base / bw, TD_GAIN_LO, TD_GAIN_HI) : 1.0;
            delta += o * w * (g * sw - S);
            refMax = max(refMax, abs(g * sw));
        }
        delta += o * b * S;
        bsum += o * b;
    }
    if (delta == 0.0) return 0.0;                                       // bit-exact copy of Y
    float sOut = S + delta;
    float cap = max(TD_PIXEL_CAP, 1.0 + bsum) * max(abs(S), refMax);
    return clamp(sOut, -cap, cap) - S;                                  // Y_out = Y + this
}
)GLSL";
	}
}
