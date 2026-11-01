// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Light-level block product P12, shader side (WP2). Header-only GLSL for libplacebo custom shaders (pl_shader_custom, compute
// 8x8 groups); no libplacebo or Windows headers. The mirror of LightLevelBlocks.h (BlockOut<float>, EffectiveAt<float>).
//
// 1. LightLevelBlocksPassGlsl(): the producer, one thread per block. Header = LightLevelGlsl() (the LUT fetch).
//    Descriptors: re_s0, re_s3, re_s4 (sampled rgba32f, grid size, nearest: P11's stats textures S0, S3, S4),
//                 ll_prev (sampled, grid size: the previous frame's P12 slot, or any texture of that size when ll_reset),
//                 light_level_lut (sampled rgba32f 1024 x 1), ll_out (storage, write-only, grid size; rgba16f or rgba32f).
//    Uniforms:    ivec2 ll_grid, float ll_sigma (blocks), int ll_radius, float ll_frame_j, float ll_gate, int ll_reset,
//                 float ll_alpha, int ll_mode (0 Full, 1 FrameLevel: every block gets ll_frame_j / ll_frame_v / ll_frame_conf),
//                 float ll_frame_v, float ll_frame_conf.
//    Output texel: (J_surround, J_local, V, conf).
//
// 2. LightLevelBlocksAccessorGlsl(): the CONSUMER accessor, fused into each consumer's own pass.
//    vpu_light_block(ivec2 blk)            the raw texel (conf 0 outside the grid)
//    vpu_light_jeff(vec2 px, float code10) vec4(J_eff, V at the pixel's own code, conf, 1): J_eff = J_p + (J_s - J_p) *
//                                          (ll_mix * conf), J_s bilinear over the block centres. conf 0: J_eff = J_p.
//    Descriptors: ll_blocks (sampled, grid size, nearest), light_level_lut. Uniforms: ivec2 ll_grid, int ll_block, float ll_mix.
#pragma once

#include "LightLevelGlsl.h"

#include <string>

namespace LightLevel
{
	namespace Blocks
	{
		inline std::string LightLevelBlocksHeaderGlsl()
		{
			return LightLevelGlsl();
		}

		inline const char* LightLevelBlocksPassGlsl()
		{
			return R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < ll_grid.x && blk.y < ll_grid.y) {
    vec4 own0 = texelFetch(re_s0, blk, 0);
    vec4 own3 = texelFetch(re_s3, blk, 0);
    vec4 own4 = texelFetch(re_s4, blk, 0);
    bool in_picture = own4.x >= 0.5;
    bool have_grain = own4.w >= 0.0;
    vec4 o;
    if (ll_mode == 1) {
        o = vec4(ll_frame_j, ll_frame_j, ll_frame_v, ll_frame_conf);
    } else {
        float sw = 0.0, sj = 0.0;
        int y0 = max(0, blk.y - ll_radius), y1 = min(ll_grid.y - 1, blk.y + ll_radius);
        int x0 = max(0, blk.x - ll_radius), x1 = min(ll_grid.x - 1, blk.x + ll_radius);
        for (int y = y0; y <= y1; ++y) {
            float dy = float(y - blk.y) / ll_sigma;
            float wy = exp(-0.5 * dy * dy);
            for (int x = x0; x <= x1; ++x) {
                float dx = float(x - blk.x) / ll_sigma;
                float wc = exp(-0.5 * dx * dx) * wy * texelFetch(re_s4, ivec2(x, y), 0).x;
                sw += wc;
                sj += wc * texelFetch(re_s3, ivec2(x, y), 0).w;
            }
        }
        float js = sw > 1e-6 ? sj / sw : ll_frame_j;
        if (ll_reset == 0) {
            float pj = texelFetch(ll_prev, blk, 0).x;
            js = pj + ll_alpha * (js - pj);
        }
        float jl = in_picture ? own3.w : js;
        float v = in_picture ? min(light_level_lut_fetch(own0.x).z, 64.0) : 0.0;
        float conf = (in_picture && have_grain) ? ll_gate : 0.0;
        o = vec4(js, jl, v, conf);
    }
    imageStore(ll_out, blk, o);
}
)GLSL";
		}

		inline const char* LightLevelBlocksAccessorGlsl()
		{
			return R"GLSL(
vec4 vpu_light_block(ivec2 blk)
{
    if (blk.x < 0 || blk.y < 0 || blk.x >= ll_grid.x || blk.y >= ll_grid.y)
        return vec4(0.0);
    return texelFetch(ll_blocks, blk, 0);
}
vec4 vpu_light_jeff(vec2 px, float code10)
{
    float fx = px.x / float(ll_block) - 0.5, fy = px.y / float(ll_block) - 0.5;
    float flx = floor(fx), fly = floor(fy);
    int xa = int(flx), ya = int(fly);
    float tx = fx - flx, ty = fy - fly;
    int x0 = min(max(xa, 0), ll_grid.x - 1), x1 = min(max(xa + 1, 0), ll_grid.x - 1);
    int y0 = min(max(ya, 0), ll_grid.y - 1), y1 = min(max(ya + 1, 0), ll_grid.y - 1);
    vec4 a = texelFetch(ll_blocks, ivec2(x0, y0), 0);
    vec4 b = texelFetch(ll_blocks, ivec2(x1, y0), 0);
    vec4 c = texelFetch(ll_blocks, ivec2(x0, y1), 0);
    vec4 d = texelFetch(ll_blocks, ivec2(x1, y1), 0);
    vec4 top = a + (b - a) * tx;
    vec4 bot = c + (d - c) * tx;
    vec4 m = top + (bot - top) * ty;
    vec4 e = light_level_lut_fetch(code10);
    float conf = m.w;
    float j_eff = e.x + (m.x - e.x) * (ll_mix * conf);
    return vec4(j_eff, e.z, conf, 1.0);
}
)GLSL";
		}
	}
}
