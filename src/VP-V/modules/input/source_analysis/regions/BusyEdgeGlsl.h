// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: the shader side of BusyEdge.h (lead8/iface-L-B5.md section B). Header-only GLSL strings for a
// libplacebo custom compute shader (pl_shader_custom, compute); no libplacebo or Windows headers here.
//
// 1. BusyShareHeaderGlsl() + BusyShareBodyGlsl(): the S5 variant, ONE WORKGROUP (16 x 16 threads) PER CELL, a fixed-order
//    shared-memory tree sum (deterministic; the CPU reference BusyShare<> sums in double, the share differs by < 1e-6). The
//    arithmetic of IsBusy() is VpDebander GUARD_BODY, moved.
//    Descriptors: bz_src (sampled, plane size, up to bz_ncomp meaningful components, nearest, clamp: the consumer's planes),
//                 bz_out (storage r16f, cell grid size, write-only; R = share).
//    Uniforms:    int bz_block (cell edge px), int bz_ncomp (1..3), float bz_scale (codes per texel unit: the tap form
//                 65535 / 64), float bz_lap, float bz_ext (thresholds in codes).
#pragma once

#include <string>

namespace RegionEvidence
{
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
}
