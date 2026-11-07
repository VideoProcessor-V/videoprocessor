// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// U15 FieldRefine v1, shader twin of FieldRefineRef.h (same law, same scan order, float arithmetic with `precise`
// accumulations). Header-only; the strings go into a libplacebo custom shader HEADER.
//
// FieldRefineSearchGlsl() declares functions only; the host declares:
//   sampler2D fr_a, fr_b   (r32f half luma 0..255, texelFetch only)
//   sampler2D fr_p3        (rgba32f per block: vx, vy in source px, valid 0/1, occluded 0/1)
//   sampler2D fr_cam       (rgba32f per block: vx, vy in source px, camera-block mask 0/1, unused)
//   uniforms: ivec2 fr_size (half luma), ivec2 fr_grid (blocks), int fr_block_half (4), int fr_margin (2),
//             float fr_boundary_thr, float fr_keep_eps, int fr_use_cam
// and calls `vec4 vpu_field_refine_search(ivec2 blk)` -> (vx, vy, confidence 0..255, flags) with the Ref flag bits
// (1 refined, 2 kept, 4 boundary, 8 occluded). The P3 "valid" channel replaces the CPU's non-finite test.
//
// FieldRefineOffsetGlsl() is the per-pixel accessor `vec2 vpu_field_refine_offset(vec2 source_pixel)`: the consumer's
// own band calls it on the fly (~20 ops/px here; the BDOF gradient term of doc 72 is not part of v1). The host declares
//   sampler2D vpu_field_refine_tex (rgba32f per block: vx, vy, confidence, flags) and
//   ivec2 vpu_field_refine_grid, int vpu_field_refine_block (source px).
#pragma once

namespace VpUnit
{
	namespace FieldRefine
	{
		inline const char* FieldRefineSearchGlsl()
		{
			return R"GLSL(
float fr_sample(sampler2D t, vec2 p)
{
    float fx = floor(p.x), fy = floor(p.y);
    float tx = p.x - fx, ty = p.y - fy;
    int ix = int(fx), iy = int(fy);
    int x0 = clamp(ix, 0, fr_size.x - 1), x1 = clamp(ix + 1, 0, fr_size.x - 1);
    int y0 = clamp(iy, 0, fr_size.y - 1), y1 = clamp(iy + 1, 0, fr_size.y - 1);
    float a = texelFetch(t, ivec2(x0, y0), 0).r;
    float b = texelFetch(t, ivec2(x1, y0), 0).r;
    float c = texelFetch(t, ivec2(x0, y1), 0).r;
    float d = texelFetch(t, ivec2(x1, y1), 0).r;
    precise float top = a * (1.0 - tx) + b * tx;
    precise float bot = c * (1.0 - tx) + d * tx;
    precise float r = top * (1.0 - ty) + bot * ty;
    return r;
}

float fr_cost(ivec2 org, int win, vec2 shift)
{
    precise float s = 0.0;
    for (int j = 0; j < win; ++j)
        for (int i = 0; i < win; ++i) {
            ivec2 ap = clamp(ivec2(org.x + i, org.y + j), ivec2(0), fr_size - ivec2(1));
            float a = texelFetch(fr_a, ap, 0).r;
            float b = fr_sample(fr_b, vec2(float(org.x + i) + shift.x, float(org.y + j) + shift.y));
            s += abs(a - b);
        }
    return s;
}

// Fills cost[25]; returns the best index; mean in `mean`.
int fr_surface(ivec2 blk, vec2 seed_half, out float cost[25], out float mean)
{
    int win = fr_block_half + 2 * fr_margin;
    ivec2 org = blk * fr_block_half - ivec2(fr_margin);
    float best = 0.0;
    float sum = 0.0;
    int best_idx = -1;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            int idx = (dy + 2) * 5 + (dx + 2);
            float c = fr_cost(org, win, vec2(seed_half.x + float(dx), seed_half.y + float(dy)));
            cost[idx] = c;
            sum += c;
            if (best_idx < 0 || c < best) { best = c; best_idx = idx; }
        }
    mean = sum / 25.0;
    return best_idx;
}

float fr_parabola(float cm, float c0, float cp)
{
    float den = cm - 2.0 * c0 + cp;
    if (!(den > 1e-6)) return 0.0;
    float off = (cm - cp) / (2.0 * den);
    return clamp(off, -0.5, 0.5);
}

vec4 vpu_field_refine_search(ivec2 blk)
{
    vec4 s = texelFetch(fr_p3, blk, 0);
    if (s.z < 0.5) return vec4(0.0, 0.0, 0.0, 2.0);
    if (s.w > 0.5) return vec4(s.xy, 0.0, 10.0);
    vec2 seed = s.xy;
    float cost3[25]; float mean3;
    int b3 = fr_surface(blk, seed * 0.5, cost3, mean3);
    float cost[25]; float mean = mean3; int bi = b3;
    for (int k = 0; k < 25; ++k) cost[k] = cost3[k];
    vec2 sh = seed * 0.5;
    vec4 cam = vec4(0.0);
    if (fr_use_cam != 0) cam = texelFetch(fr_cam, blk, 0);
    if (cam.z > 0.5) {
        float costc[25]; float meanc;
        vec2 cs = cam.xy * 0.5;
        int bc = fr_surface(blk, cs, costc, meanc);
        if (costc[bc] < cost3[b3]) {
            for (int k = 0; k < 25; ++k) cost[k] = costc[k];
            mean = meanc; bi = bc; sh = cs;
        }
    }
    int bx = bi % 5 - 2, by = bi / 5 - 2;
    int ix = bx + 2, iy = by + 2;
    bool saturated = false;
    float sub_x = 0.0, sub_y = 0.0;
    if (bx > -2 && bx < 2) sub_x = fr_parabola(cost[iy * 5 + ix - 1], cost[iy * 5 + ix], cost[iy * 5 + ix + 1]);
    else saturated = true;
    if (by > -2 && by < 2) sub_y = fr_parabola(cost[(iy - 1) * 5 + ix], cost[iy * 5 + ix], cost[(iy + 1) * 5 + ix]);
    else saturated = true;
    vec2 v = vec2((sh.x + float(bx) + sub_x) * 2.0, (sh.y + float(by) + sub_y) * 2.0);
    float conf = 0.0;
    if (mean > 1e-6) {
        float c = clamp(1.0 - cost[bi] / mean, 0.0, 1.0);
        conf = floor(255.0 * c + 0.5);
    }
    bool boundary = saturated;
    ivec2 nb[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
    for (int k = 0; k < 4; ++k) {
        ivec2 n = blk + nb[k];
        if (n.x < 0 || n.y < 0 || n.x >= fr_grid.x || n.y >= fr_grid.y) continue;
        vec4 nv = texelFetch(fr_p3, n, 0);
        if (nv.z < 0.5) continue;
        if (abs(nv.x - seed.x) > fr_boundary_thr || abs(nv.y - seed.y) > fr_boundary_thr) boundary = true;
    }
    bool moved = abs(v.x - seed.x) > fr_keep_eps || abs(v.y - seed.y) > fr_keep_eps;
    float flags = (moved ? 1.0 : 2.0) + (boundary ? 4.0 : 0.0);
    return vec4(v, conf, flags);
}
)GLSL";
		}

		inline const char* FieldRefineOffsetGlsl()
		{
			return R"GLSL(
vec2 vpu_field_refine_offset(vec2 px)
{
    float bs = float(vpu_field_refine_block);
    ivec2 g = vpu_field_refine_grid;
    ivec2 c = clamp(ivec2(floor(px / bs)), ivec2(0), g - ivec2(1));
    vec4 own = texelFetch(vpu_field_refine_tex, c, 0);
    int own_flags = int(own.w + 0.5);
    if ((own_flags & 12) != 0) return vec2(0.0);
    vec2 uv = px / bs - vec2(0.5);
    vec2 fl = floor(uv);
    float tx = uv.x - fl.x, ty = uv.y - fl.y;
    ivec2 i0 = ivec2(fl);
    vec2 v[4];
    for (int k = 0; k < 4; ++k) {
        ivec2 q = clamp(i0 + ivec2(k & 1, k >> 1), ivec2(0), g - ivec2(1));
        vec4 b = texelFetch(vpu_field_refine_tex, q, 0);
        bool use = (int(b.w + 0.5) & 12) == 0;
        v[k] = use ? b.xy : own.xy;
    }
    precise float ax = (v[0].x * (1.0 - tx) + v[1].x * tx) * (1.0 - ty) + (v[2].x * (1.0 - tx) + v[3].x * tx) * ty;
    precise float ay = (v[0].y * (1.0 - tx) + v[1].y * tx) * (1.0 - ty) + (v[2].y * (1.0 - tx) + v[3].y * tx) * ty;
    return vec2(ax - own.x, ay - own.y);
}
)GLSL";
		}
	}
}
