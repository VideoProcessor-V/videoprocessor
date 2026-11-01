// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: the shader side of RegionEvidence.h. Header-only GLSL strings for a libplacebo custom
// compute shader (pl_shader_custom, compute, 8x8 groups); no libplacebo or Windows headers here.
//
// 1. StatsPassHeaderGlsl() + StatsPassGlsl(): the layer-1 producer, one thread per block, the arithmetic order of
//    StatsBlock<float>.
//    Descriptors: re_src (sampled, frame size: the RESIDENT decoded luma plane, codes in .x; nearest, clamp),
//                 re_bands (sampled rgba32f, grid size: the grain split's block texel E1, E2, E3, N),
//                 re_s0 .. re_s4 (storage rgba32f, grid size, write-only).
//    Uniforms:    ivec2 re_frame, ivec2 re_rect_lo, ivec2 re_rect_hi (clipped picture rect), int re_block,
//                 int re_transfer, int re_have_split, vec4 re_vis (sdrWhite, displayPeak, kneeShare, weber), float re_dark.
//    The aux texel (re_aux) is packed on the CPU (PackAux) and uploaded beside them: it is part of the same product.
//    This is the reference form (a thread loops over its block twice). The production form is a workgroup per block with
//    a fixed-order shared-memory tree (still deterministic); its mirror must then follow that tree.
//
// 2. RegionEvidenceGlsl(): the CONSUMER accessor, fused into each consumer's own pass (owner baseline: derived maps are
//    not stored). region_evidence(ivec2 blk) returns a RegionSample with the fields of RegionEvidence::Sample, from
//    DeriveBlock's arithmetic in the same order. region_block(vec2 px) = floor(px / re_block). region_smooth() mirrors
//    Smooth(). Self-contained (no stats-pass names).
//    Descriptors: re_s0 .. re_s4, re_aux (sampled rgba32f, grid size; nearest).
//    Uniforms:    ivec2 re_grid, int re_block, vec4 re_d0 (lapGrain, grainSpan, edgeGateLo, edgeGateHi),
//                 vec4 re_d1 (edgeGateOffLo, edgeGateOffHi, ringFloor, flatLo), vec4 re_d2 (flatHi, slopeLo, slopeHi, 0).
//    Uniforms(): fills those three vec4 from DeriveParams.
//
// StatsPassLutHeaderGlsl() replaces StatsPassHeaderGlsl() when the light-level LUT is bound (see there).
//
// TransferGlsl(): re_nits / re_pq_of / re_visibility only (no descriptors), usable by any shader.
#pragma once

#include "RegionEvidence.h"

#include <VP-V/modules/input/source_analysis/light_level/LightLevelGlsl.h>

#include <string>

namespace RegionEvidence
{
	inline const char* TransferGlsl()
	{
		return R"GLSL(
float re_clamp01(float v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
float re_nits(float code, int transfer, float sdr_white)
{
    float v = (code - 64.0) / 876.0;
    if (transfer == 1) {
        float e = re_clamp01(v);
        float p = pow(e, 0.012683313515655966);
        float num = max(p - 0.8359375, 0.0);
        return 10000.0 * pow(num / (18.8515625 - 18.6875 * p), 6.277394636015326);
    }
    if (transfer == 2) {
        float e = re_clamp01(v);
        float s = e <= 0.5 ? e * e / 3.0 : (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
        return 1000.0 * pow(s, 1.2);
    }
    float vv = max(v, 0.0);
    return sdr_white * pow(vv, 2.4);
}
float re_pq_of(float nits)
{
    float y = re_clamp01(nits / 10000.0);
    float p = pow(y, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}
float re_visibility(float code, int transfer, float sdr_white, float peak, float knee_share, float weber, float dark)
{
    float L = re_nits(code, transfer, sdr_white);
    float dL = re_nits(code + 0.5, transfer, sdr_white) - re_nits(code - 0.5, transfer, sdr_white);
    float knee = knee_share * peak;
    float Lt = L, slope_t = 1.0;
    if (!(L <= knee || peak <= knee)) {
        float span = peak - knee;
        float e = exp(-(L - knee) / span);
        Lt = knee + span * (1.0 - e);
        slope_t = e;
    }
    return re_clamp01(slope_t * dL / (weber * (Lt + dark)));
}
)GLSL";
	}

	// The stats pass header: the transfer helpers plus the clamped luma read (uses re_src and the rect uniforms).
	inline std::string StatsPassHeaderGlsl()
	{
		return std::string(TransferGlsl()) + R"GLSL(
float re_y(ivec2 p)
{
    ivec2 q = clamp(p, re_rect_lo, re_rect_hi - ivec2(1));
    return texelFetch(re_src, q, 0).x;
}
// The generic transfer law (the reduced path): per-pixel nits / PQ and the block's visibility.
void re_accumulate(float c, inout float sum_nits, inout float sum_pq)
{
    float nits = re_nits(c, re_transfer, re_vis.x);
    sum_nits += nits;
    sum_pq += re_pq_of(nits);
}
float re_visibility_mean(float code)
{
    return re_visibility(code, re_transfer, re_vis.x, re_vis.y, re_vis.z, re_vis.w, re_dark);
}
)GLSL";
	}

	// The stats pass header with the light-level LUT (WP2): same body (StatsPassGlsl), but J / L / V come from the LUT.
	// Extra descriptor: light_level_lut (LightLevelGlsl.h: sampled rgba32f 1024 x 1). re_transfer / re_vis / re_dark are not
	// read. Mirror: StatsBlock<float> with StatsConstants::lutTexels.
	inline std::string StatsPassLutHeaderGlsl()
	{
		return std::string(LightLevel::LightLevelGlsl()) + R"GLSL(
float re_clamp01(float v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
float re_y(ivec2 p)
{
    ivec2 q = clamp(p, re_rect_lo, re_rect_hi - ivec2(1));
    return texelFetch(re_src, q, 0).x;
}
void re_accumulate(float c, inout float sum_nits, inout float sum_pq)
{
    vec4 e = light_level_lut_fetch(c);
    sum_nits += e.w * 10000.0;
    sum_pq += e.x;
}
float re_visibility_mean(float code)
{
    return re_clamp01(light_level_lut_fetch(code).z);
}
)GLSL";
	}

	inline const char* StatsPassGlsl()
	{
		return R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
ivec2 grid = (re_frame + ivec2(re_block - 1)) / re_block;
if (blk.x < grid.x && blk.y < grid.y) {
    vec4 bands = re_have_split != 0 ? texelFetch(re_bands, blk, 0) : vec4(0.0);
    vec4 o0 = vec4(0.0), o1 = vec4(0.0), o2 = vec4(0.0), o3 = vec4(0.0);
    vec4 o4 = vec4(0.0, 0.0, 0.0, re_have_split != 0 ? bands.w : -1.0);
    ivec2 f0 = blk * re_block;
    ivec2 f1 = min(f0 + ivec2(re_block), re_frame);
    ivec2 a0 = max(f0, re_rect_lo);
    ivec2 a1 = min(f1, re_rect_hi);
    int frame_area = (f1.x - f0.x) * (f1.y - f0.y);
    if (a1.x > a0.x && a1.y > a0.y && frame_area > 0) {
        int nx = a1.x - a0.x, ny = a1.y - a0.y, n = nx * ny;
        float cxm = 0.5 * float(a0.x + a1.x - 1), cym = 0.5 * float(a0.y + a1.y - 1);
        float sum_y = 0.0, min_y = 1e30, max_y = -1e30, g2max = 0.0;
        float jxx = 0.0, jyy = 0.0, jxy = 0.0, lap_e = 0.0;
        float sx_y = 0.0, sy_y = 0.0, sum_nits = 0.0, sum_pq = 0.0;
        for (int y = a0.y; y < a1.y; ++y) {
            for (int x = a0.x; x < a1.x; ++x) {
                float c = re_y(ivec2(x, y)), l = re_y(ivec2(x - 1, y)), r = re_y(ivec2(x + 1, y));
                float u = re_y(ivec2(x, y - 1)), d = re_y(ivec2(x, y + 1));
                float gx = 0.5 * (r - l), gy = 0.5 * (d - u);
                float g2 = gx * gx + gy * gy;
                float lap = 4.0 * c - l - r - u - d;
                sum_y += c;
                min_y = min(min(min(min_y, c), min(l, r)), min(u, d));
                max_y = max(max(max(max_y, c), max(l, r)), max(u, d));
                g2max = max(g2max, g2);
                jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
                lap_e += lap * lap;
                sx_y += (float(x) - cxm) * c;
                sy_y += (float(y) - cym) * c;
                re_accumulate(c, sum_nits, sum_pq);
            }
        }
        float thr2 = 0.0625 * g2max;
        float off_lap = 0.0;
        int off_count = 0;
        for (int y = a0.y; y < a1.y; ++y) {
            for (int x = a0.x; x < a1.x; ++x) {
                float c = re_y(ivec2(x, y)), l = re_y(ivec2(x - 1, y)), r = re_y(ivec2(x + 1, y));
                float u = re_y(ivec2(x, y - 1)), d = re_y(ivec2(x, y + 1));
                float gx = 0.5 * (r - l), gy = 0.5 * (d - u);
                if (gx * gx + gy * gy < thr2) {
                    float lap = 4.0 * c - l - r - u - d;
                    off_lap += lap * lap;
                    off_count += 1;
                }
            }
        }
        float inv = 1.0 / float(n);
        o0 = vec4(sum_y * inv, min_y, max_y, sqrt(g2max));
        o1 = vec4(jxx * inv, jyy * inv, jxy * inv, lap_e * inv);
        o2 = vec4(bands.x, bands.y, bands.z, off_count > 0 ? off_lap / float(off_count) : 0.0);
        float sxx = float(ny) * float(nx) * (float(nx) * float(nx) - 1.0) / 12.0;
        float syy = float(nx) * float(ny) * (float(ny) * float(ny) - 1.0) / 12.0;
        o3 = vec4(nx > 1 ? sx_y / sxx : 0.0, ny > 1 ? sy_y / syy : 0.0, sum_nits * inv, sum_pq * inv);
        o4.x = float(n) / float(frame_area);
        o4.y = float(off_count) * inv;
        o4.z = re_visibility_mean(o0.x);
    }
    imageStore(re_s0, blk, o0);
    imageStore(re_s1, blk, o1);
    imageStore(re_s2, blk, o2);
    imageStore(re_s3, blk, o3);
    imageStore(re_s4, blk, o4);
}
)GLSL";
	}

	// The consumer accessor (fused into the consumer's own shader). Branch order = DeriveBlock.
	inline const char* RegionEvidenceGlsl()
	{
		return R"GLSL(
struct RegionSample {
    bool valid;
    float grain, edge, edge_contrast, ringing;
    float banding, texture_energy, brightness, visibility;
    int flags;
    float camera_confidence;
    int camera_label;
};
float re_ramp(float v, float lo, float hi)
{
    if (!(hi > lo))
        return v >= hi ? 1.0 : 0.0;
    float t = (v - lo) / (hi - lo);
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}
ivec2 region_block(vec2 px) { return ivec2(floor(px / float(re_block))); }
float region_smooth(float previous, float current, float alpha, bool discontinuity)
{
    return discontinuity ? current : previous + alpha * (current - previous);
}
RegionSample region_evidence(ivec2 blk)
{
    RegionSample r;
    r.valid = false;
    r.grain = 0.0; r.edge = 0.0; r.edge_contrast = 0.0; r.ringing = 0.0;
    r.banding = 0.0; r.texture_energy = 0.0; r.brightness = 0.0; r.visibility = 0.0;
    r.flags = 0; r.camera_confidence = 0.0; r.camera_label = 0;
    if (blk.x < 0 || blk.y < 0 || blk.x >= re_grid.x || blk.y >= re_grid.y)
        return r;
    r.valid = true;
    vec4 s0 = texelFetch(re_s0, blk, 0), s1 = texelFetch(re_s1, blk, 0), s2 = texelFetch(re_s2, blk, 0);
    vec4 s3 = texelFetch(re_s3, blk, 0), s4 = texelFetch(re_s4, blk, 0), aux = texelFetch(re_aux, blk, 0);
    bool have_split = s4.w >= 0.0;
    bool overlay = aux.x > 0.5;
    bool in_picture = s4.x >= 0.5;
    int flags = (overlay ? 2 : 0) | (have_split ? 0 : 4);
    if (!in_picture) {
        r.flags = flags;
        return r;
    }
    flags |= 1;
    float N = have_split ? s4.w : 0.0;
    float sigma = sqrt(N);
    float edge = s0.w;
    float range = s0.z - s0.y;
    r.grain = sigma;
    r.edge = edge;
    r.edge_contrast = range / (range + re_d0.y * sigma + 1.0);
    float floor_lap = re_d0.x * N;
    float gate = re_ramp(edge, re_d0.z * sigma + re_d1.x, re_d0.w * sigma + re_d1.y);
    float excess = max(s2.w - floor_lap, 0.0);
    r.ringing = have_split ? gate * excess / (s2.w + floor_lap + max(re_d1.z, 1e-6)) : 0.0;
    float texture_e = have_split ? max(s2.x + s2.y - N, 0.0) : 0.0;
    r.texture_energy = texture_e;
    float slope = sqrt(s3.x * s3.x + s3.y * s3.y);
    r.banding = (have_split && !overlay) ? (1.0 - re_ramp(texture_e, re_d1.w, re_d2.x)) * (1.0 - re_ramp(slope, re_d2.y, re_d2.z)) : 0.0;
    r.brightness = s0.x;
    r.visibility = s4.z;
    int code = int(aux.z + 0.5);
    if (((code >> 2) & 1) != 0) flags |= 8;
    r.flags = flags;
    r.camera_confidence = aux.y;
    r.camera_label = code & 3;
    return r;
}
)GLSL";
	}

	struct AccessorUniforms
	{
		float d0[4] = { 0, 0, 0, 0 }, d1[4] = { 0, 0, 0, 0 }, d2[4] = { 0, 0, 0, 0 };
	};

	inline AccessorUniforms Uniforms(const DeriveParams& p)
	{
		AccessorUniforms u;
		u.d0[0] = static_cast<float>(p.lapGrain); u.d0[1] = static_cast<float>(p.grainSpan);
		u.d0[2] = static_cast<float>(p.edgeGateLo); u.d0[3] = static_cast<float>(p.edgeGateHi);
		u.d1[0] = static_cast<float>(p.edgeGateOffLo); u.d1[1] = static_cast<float>(p.edgeGateOffHi);
		u.d1[2] = static_cast<float>(p.ringFloor); u.d1[3] = static_cast<float>(p.flatLo);
		u.d2[0] = static_cast<float>(p.flatHi); u.d2[1] = static_cast<float>(p.slopeLo); u.d2[2] = static_cast<float>(p.slopeHi);
		return u;
	}

	// Float luma as the stats pass samples it.
	inline void PackLuma(const GrainSplit::Plane& y, std::vector<float>& out)
	{
		out.resize(y.v.size());
		for (size_t i = 0; i < y.v.size(); ++i) out[i] = static_cast<float>(y.v[i]);
	}

	// The split's block texels (E1, E2, E3, N) as float, grid order (what the split pass hands the stats pass).
	inline void BandTexelGrid(const GrainSplit::Split& split, const Rect& picture, std::vector<float>& out)
	{
		const size_t n = static_cast<size_t>(split.bw) * split.bh;
		out.assign(n * 4, 0.0f);
		for (int by = 0; by < split.bh; ++by)
			for (int bx = 0; bx < split.bw; ++bx)
			{
				double t[4];
				BandTexelBlock<double>(split, picture, bx, by, t);
				const size_t i = static_cast<size_t>(by) * split.bw + bx;
				for (int c = 0; c < 4; ++c) out[i * 4 + c] = static_cast<float>(t[c]);
			}
	}

	// Float mirror of the stats pass; bands = BandTexelGrid output or null; out = Count() x kStatsChannels floats.
	inline void StatsGridMirror(const StatsConstants& k, const float* luma, const float* bands, std::vector<float>& out)
	{
		const Grid g = MakeGrid(k.w, k.h, k.B);
		out.assign(g.Count() * kStatsChannels, 0.0f);
		PackedLuma src;
		src.w = k.w;
		src.p = luma;
		for (int by = 0; by < g.gh; ++by)
			for (int bx = 0; bx < g.gw; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * g.gw + bx;
				StatsBlock<float>(k, src, bands ? bands + i * 4 : nullptr, bx, by, &out[i * kStatsChannels]);
			}
	}
}
