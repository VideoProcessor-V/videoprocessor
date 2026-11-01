// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// output.dither / dither need map, shader side. Header-only GLSL strings; no libplacebo or Windows headers here.
//
// 1. BlockPassHeaderGlsl() + BlockPassBodyGlsl(): the BLOCK PASS (a libplacebo custom compute shader, 8x8 groups, one
//    thread per region block), the arithmetic order of DitherNeed::TargetBlock<float> and MapT<float>::Update.
//    Descriptors: dn_gs            sampled rgba32f, grid size: the grain split's block texture (GrainSplit::Gpu::Frame::blocks:
//                                  the block grain level N in .x, e3 in .y); nearest, clamp
//                 re_s0 .. re_s4   sampled rgba32f, grid size: the region precursors (RegionEvidence stats pass products)
//                 re_aux           sampled rgba32f, grid size: the region aux texel
//                 light_level_lut  sampled rgba32f, 1024 x 1: LightLevel::PackTexels   (V in .z)
//                 light_level_out  sampled rgba32f, 1024 x 1: LightLevel::PackOutputTexels (Sout in .y)
//                 dn_prev          sampled rgba32f, grid size: the previous state (e in .x)
//                 dn_state         storage rgba32f, grid size, write-only: the new state (e in .x, other channels 0)
//    Uniforms:    ivec2 re_grid, int re_block, vec4 re_d0, re_d1, re_d2   (RegionEvidence::Uniforms(DeriveParams), as the
//                                  region accessor needs them)
//                 vec4 dn_p0 = (aBase, aNeed, aMax, deadband), vec2 dn_p1 = (visLo, visHi)
//                 float dn_qout (DitherNeed::QOut(colorBits)), float dn_alpha (DitherNeed::AlphaFor(dt, tau))
//                 int dn_reset (1 = no valid state, discontinuity, grid / LUT / colour depth / aBase change: e = target;
//                 decided by the CPU class), int dn_off (1 = write zero: missing input or not needed).
//    The block texel layout is the product itself: e in .x. The previous state is a second texture of the same size
//    (ping-pong); the host swaps them after each pass.
//
// 2. ExtraAccessorGlsl(): `float dn_extra(vec2 posOut)`, the per-pixel bilinear sample of the state over the block centres,
//    the arithmetic order of DitherNeed::SampleExtra. THE SAME TEXT is in `shaders/Temporal sigma-delta.glsl` (the
//    quantiser pass); a CPU test asserts that file contains this text, and the WARP test runs this text. It reads the
//    sampler VPDN (state, e in .x) and the floats dn_scale_x, dn_scale_y, dn_off_x, dn_off_y, dn_block (the shader's PARAMs;
//    a test supplies float uniforms of the same names).
#pragma once

#include <VP-V/modules/input/source_analysis/light_level/LightLevelGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>

#include <string>

namespace DitherNeed
{
	// Header of the block pass: the region accessor, the light-level fetches, the clamped smoothstep of the law.
	inline std::string BlockPassHeaderGlsl()
	{
		return std::string(RegionEvidence::RegionEvidenceGlsl()) + LightLevel::LightLevelGlsl() + LightLevel::LightLevelOutGlsl() + R"GLSL(
float dn_smoothstep(float e0, float e1, float x)
{
    if (!(e1 > e0))
        return x >= e0 ? 1.0 : 0.0;
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}
)GLSL";
	}

	inline const char* BlockPassBodyGlsl()
	{
		return R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < re_grid.x && blk.y < re_grid.y) {
    float e = 0.0;
    if (dn_off == 0) {
        float N = texelFetch(dn_gs, blk, 0).x;
        RegionSample rs = region_evidence(blk);
        float mean_y = rs.brightness;
        float V = light_level_lut_fetch(mean_y).z;
        float sout = light_level_out_fetch(mean_y).y;
        bool in_picture = (rs.flags & 1) != 0;
        bool overlay = (rs.flags & 2) != 0;
        float a_s = sout < 0.0 ? -sout : sout;
        float sigma_out = sqrt(N > 0.0 ? N : 0.0) * a_s / dn_qout;
        float a_base = dn_p0.x;
        float hi = max(dn_p0.z, a_base);
        float a_target = min(max(dn_p0.y - sigma_out, a_base), hi);
        float g_vis = 0.0;
        if (a_s > 1e-6)
            g_vis = dn_smoothstep(dn_p1.x, dn_p1.y, V * dn_qout / a_s);
        float g = (in_picture && !overlay) ? rs.banding * g_vis : 0.0;
        float tgt = g * (a_target - a_base);
        float prev = texelFetch(dn_prev, blk, 0).x;
        if (dn_reset != 0) {
            e = tgt;
        } else {
            float diff = tgt - prev;
            e = prev;
            if (abs(diff) > dn_p0.w)
                e = prev + dn_alpha * diff;
        }
    }
    imageStore(dn_state, blk, vec4(e, 0.0, 0.0, 0.0));
}
)GLSL";
	}

	// The text of dn_extra (no leading newline; identical to the function in Temporal sigma-delta.glsl, 4-space indents).
	inline const char* ExtraAccessorGlsl()
	{
		return R"GLSL(float dn_extra(vec2 posOut)
{
    float sx = (posOut.x + 0.5) * dn_scale_x + dn_off_x;
    float sy = (posOut.y + 0.5) * dn_scale_y + dn_off_y;
    float u = sx / dn_block - 0.5;
    float v = sy / dn_block - 0.5;
    float fx0 = floor(u), fy0 = floor(v);
    float fx = u - fx0, fy = v - fy0;
    ivec2 sz = textureSize(VPDN, 0);
    int x0 = int(fx0), y0 = int(fy0);
    int i0 = min(max(x0, 0), sz.x - 1), i1 = min(max(x0 + 1, 0), sz.x - 1);
    int j0 = min(max(y0, 0), sz.y - 1), j1 = min(max(y0 + 1, 0), sz.y - 1);
    float a = texelFetch(VPDN, ivec2(i0, j0), 0).x, b = texelFetch(VPDN, ivec2(i1, j0), 0).x;
    float c = texelFetch(VPDN, ivec2(i0, j1), 0).x, d = texelFetch(VPDN, ivec2(i1, j1), 0).x;
    float top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}
)GLSL";
	}
}
