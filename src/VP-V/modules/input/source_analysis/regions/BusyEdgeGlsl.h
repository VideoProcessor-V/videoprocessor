// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: the shader side of BusyEdge.h (lead8/iface-L-B5.md sections B and C). Header-only GLSL strings for a
// libplacebo custom compute shader (pl_shader_custom, compute); no libplacebo or Windows headers here.
//
// 1. BusyShareHeaderGlsl() + BusyShareBodyGlsl(): the S5 variant, ONE WORKGROUP (16 x 16 threads) PER CELL, a fixed-order
//    shared-memory tree sum (deterministic; the CPU reference BusyShare<> sums in double, the share differs by < 1e-6). The
//    arithmetic of IsBusy() is VpDebander GUARD_BODY, moved.
//    Descriptors: bz_src (sampled, plane size, up to bz_ncomp meaningful components, nearest, clamp: the consumer's planes),
//                 bz_out (storage r16f, cell grid size, write-only; R = share).
//    Uniforms:    int bz_block (cell edge px), int bz_ncomp (1..3), float bz_scale (codes per texel unit: kSourceTapCodesPerUnit),
//                 float bz_lap, float bz_ext (thresholds in codes).
// 2. EdgeMapHeaderGlsl() + EdgeMapBodyGlsl(): the edge map, a 16 x 16 group reads its 18 x 18 tile once.
//    Descriptors: em_src (sampled, plane size, luma in .x), em_out (storage r32f, plane size, write-only).
//    Uniforms:    float em_scale (codes per texel unit: kSourceTapCodesPerUnit).
//    EdgeMapStageHeaderGlsl() + the same body: the stage variant (RG32F, measure luma taps; see there).
#pragma once

#include <string>

namespace RegionEvidence
{
	// THE tap form of the source planes (both products of this unit use it, so the busy share and the edge map agree in code units):
	// a normalised 16-bit container sample -> 10-bit codes, 65535 / 64 (the 10 bits at the top of the container, as the stats pass
	// reads the same planes, RegionEvidenceGlsl.h). Unrounded, so a 12-bit source keeps its two fractional bits (rule 7).
	static constexpr float kSourceTapCodesPerUnit = 65535.0f / 64.0f;

	inline std::string BusyShareHeaderGlsl()
	{
		return R"GLSL(
shared vec2 bz_sum[256];
)GLSL";
	}

	inline std::string BusyShareBodyGlsl()
	{
		return R"GLSL(
ivec2 bz_sz = textureSize(bz_src, 0);
ivec2 bz_hi = bz_sz - ivec2(1);
ivec2 bz_cell = ivec2(gl_WorkGroupID.xy);
uint bz_li = gl_LocalInvocationIndex;
ivec2 bz_org = bz_cell * bz_block;
ivec2 bz_end = min(bz_org + ivec2(bz_block), bz_sz);
float bz_busy = 0.0;
float bz_count = 0.0;
for (int y = bz_org.y + int(gl_LocalInvocationID.y); y < bz_end.y; y += 16)
{
    for (int x = bz_org.x + int(gl_LocalInvocationID.x); x < bz_end.x; x += 16)
    {
        ivec2 q = ivec2(x, y);
        vec4 c = texelFetch(bz_src, q, 0) * bz_scale;
        vec4 dw = c - texelFetch(bz_src, clamp(q + ivec2(-1, 0), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 de = c - texelFetch(bz_src, clamp(q + ivec2(1, 0), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 dn = c - texelFetch(bz_src, clamp(q + ivec2(0, -1), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 ds = c - texelFetch(bz_src, clamp(q + ivec2(0, 1), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 d1 = c - texelFetch(bz_src, clamp(q + ivec2(-1, -1), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 d2 = c - texelFetch(bz_src, clamp(q + ivec2(1, 1), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 d3 = c - texelFetch(bz_src, clamp(q + ivec2(1, -1), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 d4 = c - texelFetch(bz_src, clamp(q + ivec2(-1, 1), ivec2(0), bz_hi), 0) * bz_scale;
        vec4 l = abs(dw + de + dn + ds);
        vec4 x4 = max(max(max(min(dw, de), -max(dw, de)), max(min(dn, ds), -max(dn, ds))),
                      max(max(min(d1, d2), -max(d1, d2)), max(min(d3, d4), -max(d3, d4))));
        float m = l.x;
        float e = x4.x;
        if (bz_ncomp > 1) { m = max(m, l.y); e = max(e, x4.y); }
        if (bz_ncomp > 2) { m = max(m, l.z); e = max(e, x4.z); }
        bz_busy += (m > bz_lap || e > bz_ext) ? 1.0 : 0.0;
        bz_count += 1.0;
    }
}
bz_sum[bz_li] = vec2(bz_busy, bz_count);
barrier();
for (uint s = 128u; s > 0u; s >>= 1u)
{
    if (bz_li < s)
        bz_sum[bz_li] += bz_sum[bz_li + s];
    barrier();
}
if (bz_li == 0u)
    imageStore(bz_out, bz_cell, vec4(bz_sum[0].x / max(bz_sum[0].y, 1.0), 0.0, 0.0, 1.0));
)GLSL";
	}

	// The DEFAULT variant's tap and store (source luma, R32F): the sampled value in codes (em_scale), taps clamped to the plane.
	inline std::string EdgeMapHeaderGlsl()
	{
		return R"GLSL(
shared float em_tile[324];
ivec2 em_size() { return textureSize(em_src, 0); }
float em_tap(ivec2 p) { return texelFetch(em_src, clamp(p, ivec2(0), em_size() - ivec2(1)), 0).x * em_scale; }
void em_store(ivec2 p, float r, float e) { imageStore(em_out, p, vec4(r, 0.0, 0.0, 1.0)); }
)GLSL";
	}

	// The STAGE variant's tap and store (CB slice 18 S2b; design-EDGEMAP-DOMAIN-AXIS rev 2 s2a): the tap is the measure luma
	// ad_luma (VpMeasure::MeasureLumaGlsl, regions/MeasureDomainGlsl.h: 8-bit reference codes, clamped to the WHOLE stage texture),
	// prepended by the caller after its AD_KW / AD_HDR defines; RG32F: R = |G|, G = the tap of the pixel itself (the measure luma
	// the X5 grain sums read). Descriptors: src (sampled, the stage picture), em_out (storage rg32f, picture size); uniforms
	// linear_in (+ ad_trc, ad_a, ad_b, ad_g with AD_HDR).
	inline std::string EdgeMapStageHeaderGlsl()
	{
		return R"GLSL(
shared float em_tile[324];
ivec2 em_size() { return textureSize(src, 0); }
float em_tap(ivec2 p) { return ad_luma(p); }
void em_store(ivec2 p, float r, float e) { imageStore(em_out, p, vec4(r, e, 0.0, 1.0)); }
)GLSL";
	}

	// ONE body and ONE law for both variants (rule 20): |G| = length(vec2(gx, gy)), un-normalised, the expression order of X5's
	// former private Sobel (AdaptiveDownscale MEASURE_BODY), so X5's grad = R * 0.25 (a power of two, exact in fp32).
	inline std::string EdgeMapBodyGlsl()
	{
		return R"GLSL(
ivec2 em_p = ivec2(gl_GlobalInvocationID.xy);
uint em_li = gl_LocalInvocationIndex;
ivec2 em_sz = em_size();
ivec2 em_org = ivec2(gl_WorkGroupID.xy) * 16 - ivec2(1);
for (uint i = em_li; i < 324u; i += 256u)
    em_tile[i] = em_tap(em_org + ivec2(int(i % 18u), int(i / 18u)));
barrier();
if (em_p.x < em_sz.x && em_p.y < em_sz.y)
{
    int t = (int(gl_LocalInvocationID.y) + 1) * 18 + int(gl_LocalInvocationID.x) + 1;
    float a = em_tile[t - 19], b = em_tile[t - 18], c = em_tile[t - 17];
    float d = em_tile[t - 1], e = em_tile[t], f = em_tile[t + 1];
    float i0 = em_tile[t + 17], j = em_tile[t + 18], k = em_tile[t + 19];
    float gx = (c + 2.0 * f + k) - (a + 2.0 * d + i0);
    float gy = (i0 + 2.0 * j + k) - (a + 2.0 * b + c);
    em_store(em_p, length(vec2(gx, gy)), e);
}
)GLSL";
	}
}
