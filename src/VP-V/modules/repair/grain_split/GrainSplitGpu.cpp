// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split GPU passes (GrainSplitGpu.h). Mirrors GrainSplit::Compute (GrainSplit.h), the v3 law (refs/116 s6):
// the two low bands at half resolution and the full-resolution work in shared-memory tiles, the pass structure of
// texture_restore (TextureRestore.cpp: HALF / LOWH / LOWV / STATS / GRAIN / APPLY). No full-resolution intermediates.
#include <pch.h>

#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/SkeletonAcquire.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace GrainSplit
{
	namespace Gpu
	{
		namespace
		{
			// Geometry shared by the passes (defines): GS_W/GS_H plane, GS_HW/GS_HH half plane, GS_B block, GS_BW/GS_BH grid.
			// Weights: w1/R1 full-resolution fine band (sigmaFine s), wb/RB and w6/R6 the half-resolution base and low bands
			// (V3HalfWeight of sigmaBase s and sigmaLow s), RL = max(RB, R6).

			// 1. HALF + ACROSS: the half plane by the shared U3 accessor vpu_half_luma_comp (VpUnit::HalfLuma::GlslComponent,
			// channel GS_COMP) in codes (GS_INV_CU) into shared memory (GrainSplit::V3Half), then the base and low Gaussians
			// across (half-resolution edges clamped). Tile TW x TH half px per workgroup (64 x 4).
			const char* const HALF_HEADER = R"GLSL(
#define TW 64
#define TH 4
#define SW (TW + 2 * RL)
shared float s_r[TH * SW];
)GLSL";
			const char* const HALF_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * ivec2(TW, TH);
for (uint i = li; i < uint(TH * SW); i += uint(TW * TH)) {
    int r = int(i / uint(SW)), c = int(i % uint(SW));
    ivec2 q = ivec2(clamp(org.x - RL + c, 0, GS_HW - 1), clamp(org.y + r, 0, GS_HH - 1));
    s_r[i] = vpu_half_luma_comp(src, q, GS_COMP) * GS_INV_CU;
}
barrier();
ivec2 q = org + lid;
if (q.x < GS_HW && q.y < GS_HH) {
    int c0 = lid.y * SW + lid.x + RL;
    float ab = 0.0, a6 = 0.0;
    for (int k = -RB; k <= RB; k++) ab += wb(k) * s_r[c0 + k];
    for (int k = -R6; k <= R6; k++) a6 += w6(k) * s_r[c0 + k];
    imageStore(tmp_out, q, vec4(ab, a6, 0.0, 0.0));
}
)GLSL";

			// 2. DOWN: the base and low Gaussians down (half resolution) -> low = (Gb, G6). Tile TC x TR half px (16 x 64).
			const char* const DOWN_HEADER = R"GLSL(
#define TC 16
#define TR 64
#define SH (TR + 2 * RL)
shared vec2 s_c[SH * TC];
)GLSL";
			const char* const DOWN_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * ivec2(TC, TR);
for (uint i = li; i < uint(SH * TC); i += 256u) {
    int r = int(i / uint(TC)), c = int(i % uint(TC));
    ivec2 q = ivec2(clamp(org.x + c, 0, GS_HW - 1), clamp(org.y - RL + r, 0, GS_HH - 1));
    s_c[i] = texelFetch(tmp, q, 0).xy;
}
barrier();
for (int s = 0; s < TR / 16; s++) {
    int ry = lid.y + 16 * s;
    ivec2 q = org + ivec2(lid.x, ry);
    if (q.x < GS_HW && q.y < GS_HH) {
        float ab = 0.0, a6 = 0.0;
        for (int k = -RB; k <= RB; k++) ab += wb(k) * s_c[(ry + RL + k) * TC + lid.x].x;
        for (int k = -R6; k <= R6; k++) a6 += w6(k) * s_c[(ry + RL + k) * TC + lid.x].y;
        imageStore(low_out, q, vec4(ab, a6, 0.0, 0.0));
    }
}
)GLSL";

			// The half-resolution bands at a full-resolution pixel from the shared tile s_l (half indices lo .. lo + LA - 1 with
			// lo = org / 2 - 1, loaded clamped): bilinear at (p + 0.5) / 2 - 0.5, the arithmetic of GrainSplit::V3LowAt. Any
			// origin parity: for p in [org, org + n - 1] the taps span lo + 1 .. lo + (n + 1) / 2 + 1 <= lo + n / 2 + 2.
			const char* const LOW_S_GLSL = R"GLSL(
vec2 low_s(ivec2 p, ivec2 lo)
{
    vec2 h = (vec2(p) + vec2(0.5)) * 0.5 - vec2(0.5);
    ivec2 i0 = ivec2(floor(h));
    vec2 f = h - vec2(i0);
    ivec2 a = i0 - lo;
    vec2 v00 = s_l[a.y * LA + a.x], v10 = s_l[a.y * LA + a.x + 1];
    vec2 v01 = s_l[(a.y + 1) * LA + a.x], v11 = s_l[(a.y + 1) * LA + a.x + 1];
    vec2 top = v00 + (v10 - v00) * f.x, bot = v01 + (v11 - v01) * f.x;
    return top + (bot - top) * f.y;
}
)GLSL";

			// 3a. STATS (refs/127 A1, unit 5): one workgroup per CH x CH chunk (CH = min(B, 16); GS_NC = ceil(B / CH) chunks per
			// block edge, grid bw * GS_NC x bh * GS_NC, every chunk in parallel): the chunk and its R1 apron in shared memory, G1
			// separably, the bands per pixel; per sub-block that meets the chunk (one thread each, the pixel order of unit 4) the
			// sum of (D^2, d1^2, d2^2, d3^2) over its in-plane pixels -> part_out at ((bx * SUB + si) * GS_S + ox, (by * SUB + sj)
			// * GS_S + oy), (ox, oy) = the chunk's place in the sub-block's chunk span (GS_S = the longest span, 1 when CH is a
			// multiple of B / SUB). Deterministic: no atomics.
			const char* const STATS_HEADER = R"GLSL(
#define AP (CH + 2 * R1)
#define LA (CH / 2 + 3)
#define NSUB (GS_SUB * GS_SUB)
shared float s_y[AP * AP];
shared float s_hh[AP * CH];
shared vec2 s_l[LA * LA];
shared vec4 s_px[CH * CH];
)GLSL";
			const char* const STATS_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 blk = ivec2(gl_WorkGroupID.xy) / GS_NC;
ivec2 kc = ivec2(gl_WorkGroupID.xy) - blk * GS_NC;
int cx = kc.x * CH, cy = kc.y * CH;
ivec2 org = blk * GS_B + ivec2(cx, cy);
ivec2 lo = org / 2 - ivec2(1);
for (uint i = li; i < uint(AP * AP); i += 256u) {
    int r = int(i / uint(AP)), c = int(i % uint(AP));
    s_y[i] = gs_y(clamp(org.x - R1 + c, 0, GS_W - 1), clamp(org.y - R1 + r, 0, GS_H - 1));
}
for (uint i = li; i < uint(LA * LA); i += 256u) {
    int r = int(i / uint(LA)), c = int(i % uint(LA));
    s_l[i] = texelFetch(low, clamp(lo + ivec2(c, r), ivec2(0), ivec2(GS_HW - 1, GS_HH - 1)), 0).xy;
}
barrier();
for (uint i = li; i < uint(AP * CH); i += 256u) {
    int r = int(i / uint(CH)), c = int(i % uint(CH));
    float acc = 0.0;
    for (int k = -R1; k <= R1; k++) acc += w1(k) * s_y[r * AP + c + R1 + k];
    s_hh[i] = acc;
}
barrier();
if (lid.x < CH && lid.y < CH) {
    ivec2 p = org + lid;
    float g1 = 0.0;
    for (int k = -R1; k <= R1; k++) g1 += w1(k) * s_hh[(lid.y + R1 + k) * CH + lid.x];
    float y = s_y[(lid.y + R1) * AP + lid.x + R1];
    vec2 l = low_s(p, lo);
    float d1 = y - g1, d2 = g1 - l.x, d3 = l.x - l.y, dd = d1 + d2;
    bool inside = p.x < GS_W && p.y < GS_H;
    s_px[lid.y * CH + lid.x] = inside ? vec4(dd * dd, d1 * d1, d2 * d2, d3 * d3) : vec4(-1.0);
}
barrier();
if (li < uint(NSUB)) {
    int si = int(li) % GS_SUB, sj = int(li) / GS_SUB;
    int sx0 = (si * GS_B) / GS_SUB, sy0 = (sj * GS_B) / GS_SUB;
    int x0 = max(sx0, cx), x1 = min(((si + 1) * GS_B) / GS_SUB, cx + CH);
    int y0 = max(sy0, cy), y1 = min(((sj + 1) * GS_B) / GS_SUB, cy + CH);
    if (x0 < x1 && y0 < y1) {
        vec4 acc = vec4(0.0);
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
                vec4 v = s_px[(yy - cy) * CH + (xx - cx)];
                if (v.x >= 0.0) acc += v;
            }
        ivec2 o = kc - ivec2(sx0 / CH, sy0 / CH);
        imageStore(part_out, ivec2((blk.x * GS_SUB + si) * GS_S + o.x, (blk.y * GS_SUB + sj) * GS_S + o.y), acc);
    }
}
)GLSL";

			// 3b. REDUCE (unit 5): one thread per block. Per sub-block (fixed order) the chunk partials of its span (chunk order
			// of unit 4) and its in-plane pixel count (analytic: the sub-block rectangle clipped to the plane); then the
			// quietest sub-block energies (sorted) x CAL and the block means -> quiet = (quiet, e3, e1, e2).
			const char* const REDUCE_BODY = R"GLSL(
ivec2 blk = ivec2(gl_GlobalInvocationID.xy);
if (blk.x < GS_BW && blk.y < GS_BH) {
    float e[NSUB];
    int n = 0;
    vec4 tot = vec4(0.0);
    float cnt = 0.0;
    ivec2 borg = blk * GS_B;
    for (int i = 0; i < NSUB; i++) {
        int si = i % GS_SUB, sj = i / GS_SUB;
        int x0 = (si * GS_B) / GS_SUB, x1 = ((si + 1) * GS_B) / GS_SUB;
        int y0 = (sj * GS_B) / GS_SUB, y1 = ((sj + 1) * GS_B) / GS_SUB;
        vec4 s = vec4(0.0);
        float c = 0.0;
        if (x0 < x1 && y0 < y1) {
            for (int ky = y0 / CH; ky <= (y1 - 1) / CH; ky++)
                for (int kx = x0 / CH; kx <= (x1 - 1) / CH; kx++)
                    s += texelFetch(part, ivec2((blk.x * GS_SUB + si) * GS_S + kx - x0 / CH, (blk.y * GS_SUB + sj) * GS_S + ky - y0 / CH), 0);
            c = float(max(0, min(borg.x + x1, GS_W) - (borg.x + x0)) * max(0, min(borg.y + y1, GS_H) - (borg.y + y0)));
        }
        if (c > 0.0) { e[n] = s.x / c; n++; }
        tot += s;
        cnt += c;
    }
    for (int i = 1; i < n; i++) {
        float v = e[i];
        int j = i - 1;
        while (j >= 0 && e[j] > v) { e[j + 1] = e[j]; j--; }
        e[j + 1] = v;
    }
    int nq = min(n, max(1, GS_QUIET));
    float m = 0.0;
    for (int i = 0; i < nq; i++) m += e[i];
    float quiet = n > 0 ? GS_CAL * m / float(nq) : 0.0;
    float c = max(cnt, 1.0);
    imageStore(quiet_out, blk, vec4(quiet, tot.w / c, tot.y / c, tot.z / c));
}
)GLSL";

			// 4a. LEVEL (refs/116b, unit 4; GS 155 zero-energy exclusion): ONE workgroup. Per luma bin the 10th percentile of
			// the blocks' quiet values, by an integer histogram of 1/8-octave classes (key from the float bits: identical to
			// GrainSplit::LevelKey), -> level_out(bin) = (class-midpoint level in ten-bit-equivalent codes^2, valid, 0, 0).
			// Integer counts make the table order-independent.
			// GS 155: at the two quantization boundary bins (bin 0 crushed black, bin GS_LEVEL_BINS - 1 clipped white), blocks
			// with ten-bit-equivalent energy q <= 1/12 code^2 (GS_LEVEL_ZERO_FLOOR, variance of uniform [-0.5, +0.5] code
			// quantization rounding error: integral_{-0.5}^{+0.5} x^2 dx = 1/12) are excluded so flat-clipped blocks cannot
			// collapse the boundary percentile toward zero. Exclusion is strictly by energy, NEVER by pixel code value
			// (ADR-0002: grainy BTB/WTW blocks have q > 1/12 and remain included). Mirrors GrainSplit::GrainLevelTable.
			const char* const LEVEL_HEADER = R"GLSL(
#define GS_KEYS 160
shared uint s_hist[GS_LEVEL_BINS * GS_KEYS];
shared uint s_n[GS_LEVEL_BINS];
float gs_block_luma(ivec2 b)
{
    ivec2 p = ivec2(min(b.x * GS_B + GS_B / 2, GS_W - 1), min(b.y * GS_B + GS_B / 2, GS_H - 1));
    vec2 h = (vec2(p) + vec2(0.5)) * 0.5 - vec2(0.5);
    ivec2 i0 = ivec2(floor(h));
    vec2 f = h - vec2(i0);
    ivec2 hm = ivec2(GS_HW - 1, GS_HH - 1);
    float a = texelFetch(low, clamp(i0, ivec2(0), hm), 0).x, bb = texelFetch(low, clamp(i0 + ivec2(1, 0), ivec2(0), hm), 0).x;
    float c = texelFetch(low, clamp(i0 + ivec2(0, 1), ivec2(0), hm), 0).x, d = texelFetch(low, clamp(i0 + ivec2(1, 1), ivec2(0), hm), 0).x;
    float top = a + (bb - a) * f.x, bot = c + (d - c) * f.x;
    return top + (bot - top) * f.y;
}
int gs_level_bin(float lumaTen)
{
    return clamp(int(floor(lumaTen * float(GS_LEVEL_BINS) / 1024.0)), 0, GS_LEVEL_BINS - 1);
}
)GLSL";
			const char* const LEVEL_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
for (uint i = li; i < uint(GS_LEVEL_BINS * GS_KEYS); i += 256u) s_hist[i] = 0u;
if (li < uint(GS_LEVEL_BINS)) s_n[li] = 0u;
barrier();
for (uint i = li; i < uint(GS_BW * GS_BH); i += 256u) {
    ivec2 b = ivec2(int(i % uint(GS_BW)), int(i / uint(GS_BW)));
    float q = texelFetch(quiet, b, 0).x / (GS_CODE_SCALE * GS_CODE_SCALE);
    int bin = gs_level_bin(gs_block_luma(b) / GS_CODE_SCALE);
    if ((bin == 0 || bin == GS_LEVEL_BINS - 1) && !(q > GS_LEVEL_ZERO_FLOOR)) continue;
    int key = clamp(int(floatBitsToUint(max(q, 0.015625)) >> 20) - 968, 0, GS_KEYS - 1);
    atomicAdd(s_hist[bin * GS_KEYS + key], 1u);
    atomicAdd(s_n[bin], 1u);
}
barrier();
if (li < uint(GS_LEVEL_BINS)) {
    int j = int(li);
    int n = int(s_n[j]);
    vec4 o = vec4(0.0);
    if (n >= GS_LEVEL_MINB && n > 0) {
        int r = max(1, (GS_LEVEL_PCT * n + 99) / 100);
        int acc = 0, k = 0;
        for (; k < GS_KEYS; k++) {
            acc += int(s_hist[j * GS_KEYS + k]);
            if (acc >= r) break;
        }
        k = min(k, GS_KEYS - 1);
        o = vec4(uintBitsToFloat((uint(k + 968) << 20) | (1u << 19)), 1.0, 0.0, 0.0);
    }
    imageStore(level_out, ivec2(j, 0), o);
}
)GLSL";

			// 4b. NMIN: per block, the minimum of quiet over (2 n + 1)^2 blocks, then (unit 4) capped by the frame level function
			// at the block's luma (piecewise linear over the valid bin centres, constant beyond) -> blocks = (N, e3, e1, e2).
			const char* const NMIN_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < GS_BW && b.y < GS_BH) {
    float m = 1e30;
    for (int j = -GS_NG; j <= GS_NG; j++)
        for (int i = -GS_NG; i <= GS_NG; i++)
            m = min(m, texelFetch(quiet, ivec2(clamp(b.x + i, 0, GS_BW - 1), clamp(b.y + j, 0, GS_BH - 1)), 0).x);
#if GS_LEVEL_ON
    float lumaTen = gs_block_luma(b) / GS_CODE_SCALE;
    float pos = lumaTen * float(GS_LEVEL_BINS) / 1024.0 - 0.5;
    int lo = -1, hi = -1;
    for (int j = 0; j < GS_LEVEL_BINS; j++) {
        if (texelFetch(level, ivec2(j, 0), 0).y < 0.5) continue;
        if (float(j) <= pos) lo = j;
        if (float(j) >= pos && hi < 0) hi = j;
    }
    if (lo >= 0 || hi >= 0) {
        float cap;
        if (lo < 0) cap = texelFetch(level, ivec2(hi, 0), 0).x;
        else if (hi < 0 || hi == lo) cap = texelFetch(level, ivec2(lo, 0), 0).x;
        else {
            float a = texelFetch(level, ivec2(lo, 0), 0).x, c = texelFetch(level, ivec2(hi, 0), 0).x;
            cap = a + (c - a) * ((pos - float(lo)) / float(hi - lo));
        }
        m = min(m, cap * GS_CODE_SCALE * GS_CODE_SCALE);
    }
#endif
    vec4 q = texelFetch(quiet, b, 0);
    imageStore(blocks_out, b, vec4(m, q.y, q.z, q.w));
}
)GLSL";

			// 5. APPLY: 16 x 16 output px per workgroup; the plane with a 1 px border and the R1 apron in shared memory,
			// G1 for the 16 x 16 centre, D = Y - Gb over the 18 x 18 (positions clamped, as GrainSplit's d.get), the 3 x 3
			// energy, f = clamp(1 - kappa N / max(e, floor), 0, 1) -> the product (Full: split rgba32f; Compact: A, B).
			const char* const APPLY_HEADER = R"GLSL(
#define AC 16
#define AW (AC + 2)
#define AA (AW + 2 * R1)
#define LA (AC / 2 + 2)
shared float s_y[AA * AA];
shared float s_hh[AA * AC];
shared vec2 s_l[LA * LA];
shared float s_d[AW * AW];
)GLSL";
			const char* const APPLY_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 org = ivec2(gl_WorkGroupID.xy) * AC;
ivec2 so = org - ivec2(1 + R1);
ivec2 lo = org / 2 - ivec2(1);
for (uint i = li; i < uint(AA * AA); i += 256u) {
    int r = int(i / uint(AA)), c = int(i % uint(AA));
    s_y[i] = gs_y(clamp(so.x + c, 0, GS_W - 1), clamp(so.y + r, 0, GS_H - 1));
}
for (uint i = li; i < uint(LA * LA); i += 256u) {
    int r = int(i / uint(LA)), c = int(i % uint(LA));
    s_l[i] = texelFetch(low, clamp(lo + ivec2(c, r), ivec2(0), ivec2(GS_HW - 1, GS_HH - 1)), 0).xy;
}
barrier();
for (uint i = li; i < uint(AA * AC); i += 256u) {
    int r = int(i / uint(AC)), c = int(i % uint(AC));
    float acc = 0.0;
    for (int k = -R1; k <= R1; k++) acc += w1(k) * s_y[r * AA + c + 1 + R1 + k];
    s_hh[i] = acc;
}
for (uint i = li; i < uint(AW * AW); i += 256u) {
    int r = int(i / uint(AW)), c = int(i % uint(AW));
    ivec2 q = clamp(org - ivec2(1) + ivec2(c, r), ivec2(0), ivec2(GS_W - 1, GS_H - 1));
    ivec2 sq = q - so;
    s_d[i] = s_y[sq.y * AA + sq.x] - low_s(q, lo).x;
}
barrier();
ivec2 p = org + lid;
if (p.x < GS_W && p.y < GS_H) {
    float g1 = 0.0;
    for (int k = -R1; k <= R1; k++) g1 += w1(k) * s_hh[(lid.y + 1 + R1 + k) * AC + lid.x];
    int ci = (lid.y + 1) * AW + lid.x + 1;
    float e = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            float v = s_d[ci + j * AW + i];
            e += v * v;
        }
    e /= 9.0;
    float y = s_y[(lid.y + 1 + R1) * AA + lid.x + 1 + R1];
    float gb = low_s(p, lo).x;
    float d1 = y - g1, d2 = g1 - gb;
    float nn = texelFetch(blocks, p / GS_B, 0).x;
    float f = clamp(1.0 - GS_KAPPA * nn / max(e, GS_FLOOR), 0.0, 1.0);
#ifdef GS_COMPACT
    imageStore(split_a, p, vec4(f * d1, f * d2, 0.0, 0.0));
    imageStore(split_b, p, vec4(f, gb * GS_CU, 0.0, 0.0));
#else
    imageStore(split_out, p, vec4(f * d1, f * d2, f, gb));
#endif
}
)GLSL";

			constexpr const char* kArenaOwner = "grain_split";
			constexpr int kHalfTw = 64, kHalfTh = 4, kDownTc = 16, kDownTr = 64, kApplyTile = 16;

			// Opens the arena scope of one Compute and closes it on every exit.
			struct ScopeGuard
			{
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* a;
				bool opened = false;   // a refused (nested) scope is not ours to close
				explicit ScopeGuard(VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena) : a(arena) { if (a) opened = a->BeginScope(kArenaScope); }
				~ScopeGuard() { if (a && opened) a->EndScope(); }
				ScopeGuard(const ScopeGuard&) = delete;
				ScopeGuard& operator=(const ScopeGuard&) = delete;
			};

			// float NAME(int k), k in -R..R, with the weights `weight(k)` and radius r.
			template <typename F>
			std::string WeightsGlsl(const char* name, const char* radiusName, int r, F weight)
			{
				return VpvGpu::DefineInt(radiusName, r) + "float " + name + "(int k)\n{\n    " +
					VpvGpu::ConstArray("w", 2 * r + 1, [&](int i) { return weight(i - r); }) + "    return w[k + " + radiusName + "];\n}\n";
			}

			int HalfOf(int v) { return (v + 1) / 2; }
			int ChunkOf(int B) { return (std::min)(B, 16); }
			int ChunksPerBlock(int B) { const int ch = ChunkOf(B); return (B + ch - 1) / ch; }
			// The longest chunk span of a sub-block (in chunks, per axis): sub-block i covers [i B / sub, (i + 1) B / sub).
			int SpanOf(int B, int sub)
			{
				const int ch = ChunkOf(B);
				int s = 1;
				for (int i = 0; i < sub; ++i)
				{
					const int x0 = (i * B) / sub, x1 = ((i + 1) * B) / sub;
					if (x0 < x1) s = (std::max)(s, (x1 - 1) / ch - x0 / ch + 1);
				}
				return s;
			}
			// The partial-sum texture of the grain level pass: (bw * sub * span) x (bh * sub * span).
			void PartialSize(int bw, int bh, int B, int sub, int* pw, int* ph)
			{
				const int span = SpanOf(B, sub);
				*pw = bw * sub * span;
				*ph = bh * sub * span;
			}
			// The shapes of the four working intermediates (tmp + low half-size, part, quiet block grid): the one source of
			// ArenaRequests and WorkingBytes.
			struct WorkingShapes
			{
				int hw = 0, hh = 0, pw = 0, ph = 0, bw = 0, bh = 0;
			};
			WorkingShapes WorkingOf(int w, int h, int B, int sub)
			{
				WorkingShapes s;
				s.hw = HalfOf(w);
				s.hh = HalfOf(h);
				s.bw = (w + B - 1) / B;
				s.bh = (h + B - 1) / B;
				PartialSize(s.bw, s.bh, B, sub, &s.pw, &s.ph);
				return s;
			}
			// Input and law validation shared by Splitter::Begin, Splitter::StepOn, and Splitter::Compute(OwnerGpuContext&).
			// Rejects NaN, +/-Inf, non-positive values, and reciprocals/products that overflow or underflow IEEE-754 double
			// before any GPU allocation or GaussRadius/V3HalfRadius cast.
			bool ValidateInput(pl_tex plane, int component, double codeUnit, double scale, int B, std::string& error)
			{
				if (!plane || !plane->params.sampleable || plane->params.w < 1 || plane->params.h < 1 || component < 0 || component > 3 || B < 1)
				{
					error = "grain split: invalid input";
					return false;
				}
				if (!std::isfinite(codeUnit) || !std::isfinite(scale))
				{
					error = "grain split: invalid input (non-finite codeUnit or scale)";
					return false;
				}
				if (!(codeUnit > 0.0) || !(scale > 0.0))
				{
					error = "grain split: invalid input";
					return false;
				}
				if (!std::isfinite(1.0 / codeUnit))
				{
					error = "grain split: invalid input (non-finite 1 / codeUnit)";
					return false;
				}
				return true;
			}
			bool ValidateLaw(double scale, const Params& params, std::string& error)
			{
				if (params.subBlocks < 1 || params.subBlocks > 16 || params.levelBins < 1 || params.levelBins > 16 || params.levelPercentile < 1 ||
					params.levelPercentile > 99 || params.levelMinBlocks < 1 || params.sourceBits < 8 || params.sourceBits > 16)
				{
					error = "grain split: invalid law";
					return false;
				}
				const double s1 = params.sigmaFine * scale;
				const double sb = params.sigmaBase * scale;
				const double s6 = params.sigmaLow * scale;
				const double ds = GrainSplit::DepthScale(params.sourceBits);
				const double energyFloorScaled = params.energyFloor * ds * ds;
				if (!std::isfinite(params.sigmaFine) || !std::isfinite(params.sigmaBase) || !std::isfinite(params.sigmaLow) ||
					!std::isfinite(s1) || !std::isfinite(sb) || !std::isfinite(s6) ||
					!std::isfinite(s1 * s1) || !std::isfinite(sb * sb) || !std::isfinite(s6 * s6) ||
					!std::isfinite(params.grainCal) || !std::isfinite(params.kappa) ||
					!std::isfinite(params.energyFloor) || !std::isfinite(energyFloorScaled))
				{
					error = "grain split: invalid law (non-finite parameter)";
					return false;
				}
				if (!(params.sigmaFine > 0.0) || !(s1 > 0.0) || !(s1 * s1 > 0.0) ||
					!(sb >= GrainSplit::kV3MinLowSigma) || !(s6 >= GrainSplit::kV3MinLowSigma) ||
					!(params.grainCal >= 0.0) || !(params.kappa >= 0.0) || !(params.energyFloor > 0.0))
				{
					error = "grain split: invalid law";
					return false;
				}
				return true;
			}
			// Checks pass shared-memory demand against maxShmem, guarding GaussRadius / V3HalfRadius against int/size_t overflow
			// before casting to int. The 3.0 and 3.5 / 1e-9 factors mirror the exact contract of BandFromBase::BandRadius
			// (BandFromBase.h) and GrainSplit::V3HalfRadius (GrainSplit.h).
			bool CheckPassSharedMemory(uint64_t maxShmem, int B, double scale, const Params& params, std::string& error)
			{
				const double s1 = params.sigmaFine * scale;
				const double sb = params.sigmaBase * scale;
				const double s6 = params.sigmaLow * scale;
				const double hb = GrainSplit::V3HalfSigma(sb);
				const double h6 = GrainSplit::V3HalfSigma(s6);
				// Largest radius where tileSpan + 2 * r fits in int and sizeof(float) * aa * aa stays <= SIZE_MAX / 2.
				const int maxTileSpan = (std::max)(kApplyTile + 2, (std::max)(kHalfTw, kDownTr));
				const size_t maxAa = static_cast<size_t>(std::sqrt(static_cast<double>((std::numeric_limits<size_t>::max)() / (2u * sizeof(float)))));
				const size_t maxSpan = (std::min)(static_cast<size_t>((std::numeric_limits<int>::max)()), maxAa);
				const double maxSafeRadius = static_cast<double>(maxSpan - static_cast<size_t>(maxTileSpan)) / 2.0;
				const double r1Raw = std::ceil(3.0 * s1);
				const double rbRaw = std::ceil(3.5 * hb - 1e-9);
				const double r6Raw = std::ceil(3.5 * h6 - 1e-9);
				if (!std::isfinite(hb) || !std::isfinite(h6) || !std::isfinite(r1Raw) || !std::isfinite(rbRaw) || !std::isfinite(r6Raw))
				{
					error = "grain split: invalid law (non-finite filter radius)";
					return false;
				}
				if (r1Raw > maxSafeRadius || rbRaw > maxSafeRadius || r6Raw > maxSafeRadius)
				{
					error = "grain split: the passes exceed shared memory (filter radius overflow), the device offers " +
						std::to_string(maxShmem) + " B";
					return false;
				}
				const int r1 = GaussRadius(s1), rl = (std::max)(GrainSplit::V3HalfRadius(sb), GrainSplit::V3HalfRadius(s6));
				const int ch = ChunkOf(B), ap = ch + 2 * r1, la = ch / 2 + 3;
				const int aw = kApplyTile + 2, aa = aw + 2 * r1, lapp = kApplyTile / 2 + 2;
				const size_t need[5] = { sizeof(float) * size_t(kHalfTh) * size_t(kHalfTw + 2 * rl), 2 * sizeof(float) * size_t(kDownTc) * size_t(kDownTr + 2 * rl),
					sizeof(float) * (size_t(ap) * ap + size_t(ap) * ch + 2u * size_t(la) * la + 4u * size_t(ch) * ch),
					sizeof(float) * (size_t(aa) * aa + size_t(aa) * kApplyTile + 2u * size_t(lapp) * lapp + size_t(aw) * aw),
					sizeof(uint32_t) * (size_t(params.levelBins) * 160u + size_t(params.levelBins)) };
				const size_t most = (std::max)((std::max)(need[0], need[1]), (std::max)((std::max)(need[2], need[3]), need[4]));
				if (uint64_t(most) > maxShmem)
				{
					error = "grain split: the passes need " + std::to_string(most) + " B of shared memory, the device offers " +
						std::to_string(maxShmem) + " B";
					return false;
				}
				return true;
			}
		}

		uint64_t WorkingBytes(int w, int h, int B, int sub)
		{
			if (w < 1 || h < 1 || B < 1 || sub < 1)
				return 0;
			const WorkingShapes s = WorkingOf(w, h, B, sub);
			return 2u * uint64_t(s.hw) * uint64_t(s.hh) * 8u   // tmp + low, rg32f
				+ uint64_t(s.pw) * uint64_t(s.ph) * 16u        // part, rgba32f
				+ uint64_t(s.bw) * uint64_t(s.bh) * 16u;       // quiet, rgba32f
		}

		ContextShapes ContextShapesOf(int w, int h, int B, const Params& params, VpSkeleton::TexFormat t2, VpSkeleton::TexFormat t4)
		{
			ContextShapes c;
			if (w < 1 || h < 1 || B < 1 || params.subBlocks < 1 || params.levelBins < 1)
				return c;   // all empty: not Valid, 0 bytes
			const WorkingShapes s = WorkingOf(w, h, B, params.subBlocks);   // the shapes of ArenaRequests / WorkingBytes and of Begin
			const auto shape = [](int sw, int sh, VpSkeleton::TexFormat f) {
				VpSkeleton::TexShape t;
				t.w = static_cast<uint32_t>(sw);
				t.h = static_cast<uint32_t>(sh);
				t.fmt = f;
				return t;
			};
			c.tmp = shape(s.hw, s.hh, t2);
			c.low = shape(s.hw, s.hh, t2);
			c.part = shape(s.pw, s.ph, t4);
			c.quiet = shape(s.bw, s.bh, t4);
			c.level = shape(params.levelBins, 1, t4);
			c.split = shape(w, h, t4);         // Full: (s1, s2, f, base)
			c.blocks = shape(s.bw, s.bh, t4);  // (N, e3, 0, 0)
			return c;
		}

		// ND03: the law's one band Gaussian (the U2 unit's VpUnit::BandFromBase::BandRadius / BandWeight).
		int GaussRadius(double sigma) { return VpUnit::BandFromBase::BandRadius(sigma); }

		double GaussWeight(int k, double sigma) { return VpUnit::BandFromBase::BandWeight(k, sigma); }

		void Splitter::Release(pl_gpu gpu)
		{
			for (pl_tex* t : { &m_tmp, &m_low, &m_part, &m_quiet, &m_level, &m_blocks, &m_outFull, &m_outA, &m_outB })
			{
				if (gpu && *t)
					pl_tex_destroy(gpu, t);
				*t = nullptr;
			}
			m_run = Run();
			m_out = Frame();
		}

		namespace
		{
			std::string ViewGlslBody(GrainSplit::Storage storage, double codeUnit);
		}

		// The unit version is embedded in every prelude (a mismatch is detectable in the GLSL itself).
		std::string ViewGlsl(GrainSplit::Storage storage, double codeUnit)
		{
			return std::string("#define GS_UNIT_VERSION ") + std::to_string(GrainSplit::kUnitVersion) + "\n" + ViewGlslBody(storage, codeUnit);
		}

		namespace
		{
		std::string ViewGlslBody(GrainSplit::Storage storage, double codeUnit)
		{
			if (storage != GrainSplit::Storage::Compact)
				return "vec4 gs_fetch(ivec2 p) { return texelFetch(split, p, 0); }\n"
					"vec2 gs_blocks(ivec2 b) { return texelFetch(gblocks, b, 0).xy; }\n"
					"#define GS_HAS_ENERGIES 0\n"   // Full is read as before: no band energies through the view
					"vec2 gs_energies(ivec2 b) { return vec2(-1.0); }\n"
					"float gs_tilt(ivec2 b) { return 0.0; }\n";
			return "const float gs_cu = " + VpvGpu::Num(codeUnit) + ";\n"
				"vec4 gs_fetch(ivec2 p)\n{\n    vec2 a = texelFetch(gs_a, p, 0).xy;\n    vec2 b = texelFetch(gs_b, p, 0).xy;\n"
				"    return vec4(a.x, a.y, b.x, b.y / gs_cu);\n}\n"
				"vec2 gs_blocks(ivec2 b) { return texelFetch(gblocks, b, 0).xy; }\n"
				"#define GS_HAS_ENERGIES 1\n"
				"vec2 gs_energies(ivec2 b) { return texelFetch(gblocks, b, 0).zw; }\n"   // (e1, e2): block means of d1^2, d2^2 (codes^2)
				"float gs_tilt(ivec2 b)\n{\n    vec2 e = texelFetch(gblocks, b, 0).zw;\n    return log2((e.x + 1e-3) / (e.y + 1e-3));\n}\n";   // derived, not stored
		}
		}

		uint64_t FullShaderFingerprint()
		{
			uint64_t h = 14695981039346656037ull;
			for (const char* body : { HALF_HEADER, HALF_BODY, DOWN_HEADER, DOWN_BODY, LOW_S_GLSL, STATS_HEADER, STATS_BODY, REDUCE_BODY, LEVEL_HEADER, LEVEL_BODY, NMIN_BODY, APPLY_HEADER, APPLY_BODY })
				for (const char* c = body; *c; ++c)
					if (*c != '\r')
					{
						h ^= static_cast<unsigned char>(*c);
						h *= 1099511628211ull;
					}
			return h;
		}

		void BindView(const Frame& f, std::vector<pl_shader_desc>& out)
		{
			if (Compact(&f))
			{
				out.push_back(VpvGpu::Sampled("gs_a", f.split));
				out.push_back(VpvGpu::Sampled("gs_b", f.splitB));
			}
			else
				out.push_back(VpvGpu::Sampled("split", f.split));
			out.push_back(VpvGpu::Sampled("gblocks", f.blocks));
		}

		std::vector<VpSkeleton::ArenaRequest> Splitter::ArenaRequests(pl_gpu gpu, int w, int h, int B, GrainSplit::Storage storage)
		{
			(void)storage;   // v3: the same intermediates for both layouts
			std::vector<VpSkeleton::ArenaRequest> r;
			VpSkeleton::TexFormat t2 = VpSkeleton::TexFormat::RG32F, t4 = VpSkeleton::TexFormat::RGBA32F;
			if (!gpu || w < 1 || h < 1 || B < 1 || !VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 2), &t2) ||
				!VpSkeleton::PlTexBackend::FormatOf(VpvGpu::FloatFormat(gpu, 4), &t4))
				return r;
			const WorkingShapes s = WorkingOf(w, h, B, Params().subBlocks);   // the same shapes as WorkingBytes
			VpSkeleton::ArenaRequest a;
			a.scope = kArenaScope;
			a.owner = kArenaOwner;
			a.tier = VpSkeleton::Tier::T0;
			a.shape.w = static_cast<uint32_t>(s.hw); a.shape.h = static_cast<uint32_t>(s.hh); a.shape.fmt = t2;
			a.count = 2;   // tmp (across) + low (Gb, G6) at half resolution
			r.push_back(a);
			a.shape.w = static_cast<uint32_t>(s.pw); a.shape.h = static_cast<uint32_t>(s.ph); a.shape.fmt = t4;
			a.count = 1;   // part = the grain level pass's per-(sub-block, chunk) partial sums
			r.push_back(a);
			a.shape.w = static_cast<uint32_t>(s.bw); a.shape.h = static_cast<uint32_t>(s.bh); a.shape.fmt = t4;
			a.count = 1;   // quiet = (quiet, e3, e1, e2) per block
			r.push_back(a);
			return r;
		}

		bool Splitter::Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params)
		{
			return Compute(gpu, dispatch, plane, component, codeUnit, scale, B, params, Options());
		}

		bool Splitter::Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params,
			const Options& options)
		{
			// One arena scope across the seven steps (they share the arena's temporaries), closed on every exit.
			ScopeGuard scope(options.arena);
			for (uint32_t i = 0; i < StepCount(); ++i)
				if (!Step(gpu, dispatch, plane, component, codeUnit, scale, B, params, options, i))
					return false;
			return true;
		}

		bool Splitter::Compute(VpSkeleton::OwnerGpuContext& ctx, pl_tex plane, int component, double codeUnit, double scale, int B,
			const Params& params, uint64_t seq, ContextHold& hold)
		{
			hold = ContextHold();
			m_run = Run();
			m_out = Frame();
			m_error.clear();
			m_scratchShort = false;
			// R19-27: no device handle is taken here. The context's dispatch is handed to the passes in the house form (the cast
			// of ctx.Dispatch() at each call), its shared-memory limit replaces the device's, and every texture is the context's.
			if (ctx.Dispatch() == nullptr)
			{
				m_error = "grain split: no GPU context (used after its fill returned, or no skeleton GPU device)";
				return false;
			}
			if (ctx.MaxSharedMemory() == 0)
			{
				m_error = "grain split: the GPU context does not report the device's compute shared memory";
				return false;
			}
			if (!ValidateInput(plane, component, codeUnit, scale, B, m_error) || !ValidateLaw(scale, params, m_error) ||
				!CheckPassSharedMemory(ctx.MaxSharedMemory(), B, scale, params, m_error))
				return false;
			const ContextShapes shapes = ContextShapesOf(plane->params.w, plane->params.h, B, params);   // rg32f / rgba32f
			if (!shapes.Valid())
			{
				m_error = "grain split: invalid input";
				return false;
			}
			Options o;
			o.storage = GrainSplit::Storage::Full;
			o.seq = seq;
			struct Want
			{
				const VpSkeleton::TexShape* shape;
				pl_tex* tex;
				const char* name;
			};
			const Want want[7] = {
				{ &shapes.tmp, &o.scratch.tmp, "tmp" }, { &shapes.low, &o.scratch.low, "low" }, { &shapes.part, &o.scratch.part, "part" },
				{ &shapes.quiet, &o.scratch.quiet, "quiet" }, { &shapes.level, &o.scratch.level, "level table" },
				{ &shapes.split, &o.targets.split, "split" }, { &shapes.blocks, &o.targets.blocks, "blocks" },
			};
			for (const Want& item : want)
			{
				VpSkeleton::GpuMemoHandle made;
				std::string why;
				if (!ctx.Allocate(*item.shape, made, why) || made.resource == nullptr)
				{
					m_error = std::string("grain split: the ") + item.name + " texture was refused: " + why;
					hold = ContextHold();   // what was made goes back to the device
					return false;
				}
				*item.tex = static_cast<pl_tex>(made.resource);
				hold.keep.push_back(made.keepAlive);
				hold.bytes += made.bytes;
			}
			Device dev;
			dev.maxShmem = ctx.MaxSharedMemory();   // dev.gpu stays null: every texture above is supplied, no lookup is needed
			for (uint32_t i = 0; i < StepCount(); ++i)
				if (!StepOn(dev, static_cast<pl_dispatch>(ctx.Dispatch()), plane, component, codeUnit, scale, B, params, o, i))
				{
					hold = ContextHold();
					return false;
				}
			// GF-P1 S6 (GV AMEND): after the last pass only the two outputs are read (Frame: split, blocks). The five scratch
			// keep-alives (tmp, low, part, quiet, level table: want[0..4]) are let go here, so the memo spot pins only the outputs;
			// the context's device retires them and destroys them at its FlushReleases, after the passes that used them. The fill's
			// reserve (ReserveFor, ContextShapesOf(...).Bytes()) stays the seven-texture peak; `hold.bytes` is what the spot keeps.
			static_assert(sizeof(want) / sizeof(want[0]) == 7, "five scratch textures, then split and blocks");
			hold.keep.erase(hold.keep.begin(), hold.keep.begin() + 5);
			hold.bytes = VpSkeleton::ShapeBytes(shapes.split) + VpSkeleton::ShapeBytes(shapes.blocks);
			return true;
		}

		namespace
		{
			struct PassText
			{
				std::string geo, w1, wlow, yread;
				int r1 = 1, rb = 1, r6 = 1;
			};
			PassText MakePassText(int w, int h, int B, int bw, int bh, int component, double codeUnit, double scale, const Params& params)
			{
				using namespace VpvGpu;
				static const char* const kComp[4] = { "x", "y", "z", "w" };
				PassText t;
				t.geo = DefineInt("GS_W", w) + DefineInt("GS_H", h) + DefineInt("GS_HW", HalfOf(w)) + DefineInt("GS_HH", HalfOf(h)) +
					DefineInt("GS_B", B) + DefineInt("GS_BW", bw) + DefineInt("GS_BH", bh) + DefineInt("GS_UNIT_VERSION", GrainSplit::kUnitVersion) +
					DefineInt("GS_LEVEL_BINS", params.levelBins) + DefineInt("GS_LEVEL_PCT", params.levelPercentile) +
					DefineInt("GS_LEVEL_MINB", params.levelMinBlocks) + DefineInt("GS_LEVEL_ON", params.levelCap ? 1 : 0) +
					Define("GS_CODE_SCALE", GrainSplit::DepthScale(params.sourceBits)) +
					Define("GS_LEVEL_ZERO_FLOOR", GrainSplit::kLevelZeroEnergyFloor);
				const double s1 = params.sigmaFine * scale, sb = params.sigmaBase * scale, s6 = params.sigmaLow * scale;
				t.r1 = GaussRadius(s1);
				t.rb = GrainSplit::V3HalfRadius(sb);
				t.r6 = GrainSplit::V3HalfRadius(s6);
				t.w1 = WeightsGlsl("w1", "R1", t.r1, [&](int k) { return GaussWeight(k, s1); });
				t.wlow = WeightsGlsl("wb", "RB", t.rb, [&](int k) { return GrainSplit::V3HalfWeight(k, sb); }) +
					WeightsGlsl("w6", "R6", t.r6, [&](int k) { return GrainSplit::V3HalfWeight(k, s6); }) +
					DefineInt("RL", (std::max)(t.rb, t.r6));
				t.yread = "float gs_y(int x, int y) { return texelFetch(src, ivec2(x, y), 0)." + std::string(kComp[component]) + " * " +
					Num(1.0 / codeUnit) + "; }\n";
				return t;
			}
		}

		bool Splitter::Step(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params,
			const Options& options, uint32_t step)
		{
			Device dev;
			dev.gpu = gpu;
			dev.maxShmem = gpu ? uint64_t(gpu->glsl.max_shmem_size) : 0u;
			return StepOn(dev, dispatch, plane, component, codeUnit, scale, B, params, options, step);
		}

		bool Splitter::StepOn(const Device& dev, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B,
			const Params& params, const Options& options, uint32_t step)
		{
			using namespace VpvGpu;
			if (step >= StepCount())
			{
				m_error = "grain split: step out of range";
				m_run = Run();
				return false;
			}
			if (step == 0)
			{
				m_run = Run();
				if (!Begin(dev, dispatch, plane, component, codeUnit, B, params, options, scale))
					return false;
			}
			else if (!m_run.active || m_run.next != step)
			{
				m_error = "grain split: step out of order";
				m_run = Run();
				m_out = Frame();
				return false;
			}
			else if (!dispatch)
			{
				m_error = "grain split: invalid input";
				m_run = Run();
				m_out = Frame();
				return false;
			}
			else if (!ValidateInput(plane, component, codeUnit, scale, B, m_error) || !ValidateLaw(scale, params, m_error) ||
				!CheckPassSharedMemory(dev.maxShmem, B, scale, params, m_error))
			{
				m_run = Run();
				m_out = Frame();
				return false;
			}
			const Run r = m_run;
			const int w = r.w, h = r.h, bw = r.bw, bh = r.bh, hw = HalfOf(w), hh = HalfOf(h);
			const bool compact = r.compact;
			const PassText txt = MakePassText(w, h, B, bw, bh, component, codeUnit, scale, params);
			const int rl = (std::max)(txt.rb, txt.r6);
			auto fail = [&](const char* why) {
				m_error = why;
				m_run = Run();
				m_out = Frame();
				return false;
			};
			switch (step)
			{
			case 0:
			{
				const pl_shader_desc d[2] = { Sampled("src", plane), VpvGpu::Storage("tmp_out", r.tmp) };
				const size_t shmem = sizeof(float) * size_t(kHalfTh) * size_t(kHalfTw + 2 * rl);
				const std::string hdr = txt.geo + txt.wlow + Define("GS_INV_CU", 1.0 / codeUnit) + DefineInt("GS_COMP", component) +
					VpUnit::HalfLuma::GlslComponent() + HALF_HEADER;
				if (!Pass(dispatch, "VideoProcessor grain split: half + across", hdr, HALF_BODY, d, 2, nullptr, 0,
					(hw + kHalfTw - 1) / kHalfTw, (hh + kHalfTh - 1) / kHalfTh, shmem, kHalfTw, kHalfTh, options.timer))
					return fail("grain split: half pass failed");
				break;
			}
			case 1:
			{
				const pl_shader_desc d[2] = { Sampled("tmp", r.tmp), VpvGpu::Storage("low_out", r.low) };
				const size_t shmem = 2 * sizeof(float) * size_t(kDownTc) * size_t(kDownTr + 2 * rl);
				if (!Pass(dispatch, "VideoProcessor grain split: low bands down", txt.geo + txt.wlow + DOWN_HEADER, DOWN_BODY, d, 2, nullptr, 0,
					(hw + kDownTc - 1) / kDownTc, (hh + kDownTr - 1) / kDownTr, shmem, 16, 16, options.timer))
					return fail("grain split: low pass failed");
				break;
			}
			case 2:
			{
				const int ch = ChunkOf(B), nc = ChunksPerBlock(B), ap = ch + 2 * txt.r1, la = ch / 2 + 3;
				const std::string hdr = txt.geo + txt.w1 + txt.yread + DefineInt("CH", ch) + DefineInt("GS_NC", nc) + DefineInt("GS_SUB", params.subBlocks) +
					DefineInt("GS_S", SpanOf(B, params.subBlocks)) + STATS_HEADER + LOW_S_GLSL;
				const size_t shmem = sizeof(float) * (size_t(ap) * ap + size_t(ap) * ch + 2u * size_t(la) * la + 4u * size_t(ch) * ch);
				const pl_shader_desc d[3] = { Sampled("src", plane), Sampled("low", r.low), VpvGpu::Storage("part_out", r.part) };
				if (!Pass(dispatch, "VideoProcessor grain split: grain level", hdr, STATS_BODY, d, 3, nullptr, 0, bw * nc, bh * nc, shmem, 16, 16, options.timer))
					return fail("grain split: grain level pass failed");
				break;
			}
			case 3:
			{
				const std::string hdr = txt.geo + DefineInt("CH", ChunkOf(B)) + DefineInt("GS_SUB", params.subBlocks) + DefineInt("GS_S", SpanOf(B, params.subBlocks)) +
					DefineInt("NSUB", params.subBlocks * params.subBlocks) + DefineInt("GS_QUIET", params.quietSubBlocks) + Define("GS_CAL", params.grainCal);
				const pl_shader_desc d[2] = { Sampled("part", r.part), VpvGpu::Storage("quiet_out", r.quiet) };
				if (!Pass(dispatch, "VideoProcessor grain split: grain level reduce", hdr, REDUCE_BODY, d, 2, nullptr, 0, (bw + 15) / 16, (bh + 15) / 16, 0, 16, 16,
					options.timer))
					return fail("grain split: grain level reduce pass failed");
				break;
			}
			case 4:
			{
				const pl_shader_desc d[3] = { Sampled("quiet", r.quiet), Sampled("low", r.low), VpvGpu::Storage("level_out", r.level) };
				const size_t shmem = sizeof(uint32_t) * (size_t(params.levelBins) * 160u + size_t(params.levelBins));
				if (!Pass(dispatch, "VideoProcessor grain split: noise level function", txt.geo + LEVEL_HEADER, LEVEL_BODY, d, 3, nullptr, 0, 1, 1, shmem, 16, 16,
					options.timer))
					return fail("grain split: noise level pass failed");
				break;
			}
			case 5:
			{
				const pl_shader_desc d[4] = { Sampled("quiet", r.quiet), Sampled("low", r.low), Sampled("level", r.level), VpvGpu::Storage("blocks_out", r.blocks) };
				if (!Pass(dispatch, "VideoProcessor grain split: neighbourhood minimum", txt.geo + DefineInt("GS_NG", params.neighbourhood) + LEVEL_HEADER, NMIN_BODY, d, 4,
					nullptr, 0, (bw + 15) / 16, (bh + 15) / 16, sizeof(uint32_t) * (size_t(params.levelBins) * 160u + size_t(params.levelBins)), 16, 16, options.timer))
					return fail("grain split: minimum pass failed");
				break;
			}
			default:
			{
				const int aw = kApplyTile + 2, aa = aw + 2 * txt.r1, la = kApplyTile / 2 + 2;
				const size_t shmem = sizeof(float) * (size_t(aa) * aa + size_t(aa) * kApplyTile + 2u * size_t(la) * la + size_t(aw) * aw);
				std::string hdr = txt.geo + txt.w1 + txt.yread + Define("GS_KAPPA", params.kappa) + Define("GS_FLOOR", params.energyFloor * GrainSplit::DepthScale(params.sourceBits) * GrainSplit::DepthScale(params.sourceBits)) +
					APPLY_HEADER + LOW_S_GLSL;
				const int gx = (w + kApplyTile - 1) / kApplyTile, gy = (h + kApplyTile - 1) / kApplyTile;
				if (compact)
				{
					hdr = DefineInt("GS_COMPACT", 1) + Define("GS_CU", codeUnit) + hdr;
					const pl_shader_desc d[5] = { Sampled("src", plane), Sampled("low", r.low), Sampled("blocks", r.blocks),
						VpvGpu::Storage("split_a", r.outA), VpvGpu::Storage("split_b", r.outB) };
					if (!Pass(dispatch, "VideoProcessor grain split: Wiener share (compact)", hdr, APPLY_BODY, d, 5, nullptr, 0, gx, gy, shmem, 16, 16, options.timer))
						return fail("grain split: Wiener pass failed");
					m_out.split = r.outA;
					m_out.splitB = r.outB;
				}
				else
				{
					const pl_shader_desc d[4] = { Sampled("src", plane), Sampled("low", r.low), Sampled("blocks", r.blocks),
						VpvGpu::Storage("split_out", r.outFull) };
					if (!Pass(dispatch, "VideoProcessor grain split: Wiener share", hdr, APPLY_BODY, d, 4, nullptr, 0, gx, gy, shmem, 16, 16, options.timer))
						return fail("grain split: Wiener pass failed");
					m_out.split = r.outFull;
					m_out.splitB = nullptr;
				}
				m_out.blocks = r.blocks;
				m_out.w = w; m_out.h = h; m_out.bw = bw; m_out.bh = bh; m_out.B = B; m_out.scale = scale;
				m_out.codeUnit = codeUnit; m_out.component = component;
				m_out.storage = r.storage;
				m_out.seq = options.seq;
				m_out.version = GrainSplit::kUnitVersion;
				break;
			}
			}
			m_run.next = step + 1;
			if (step + 1 == StepCount())
				m_run = Run();
			return true;
		}

		bool Splitter::Begin(const Device& dev, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, int B, const Params& params,
			const Options& options, double scale)
		{
			using namespace VpvGpu;
			const pl_gpu gpu = dev.gpu;   // null on the context entry: every texture is supplied, nothing is looked up or made here
			m_error.clear();
			m_scratchShort = false;
			m_out = Frame();   // a failed Compute never leaves the previous frame's product looking valid
			if (!dispatch)
			{
				m_error = "grain split: invalid input";
				return false;
			}
			if (!ValidateInput(plane, component, codeUnit, scale, B, m_error))
				return false;
			if (!gpu && (options.storage != GrainSplit::Storage::Full || options.arena || !options.scratch.All() || !options.targets.split ||
				!options.targets.blocks || options.targets.splitB))
			{
				m_error = "grain split: invalid input (without a device every texture of the Full layout is supplied by the caller)";
				return false;
			}
			if (!ValidateLaw(scale, params, m_error))
				return false;
			VPV_COMPUTE_ONCE_AT("U1", options.seq ? static_cast<int64_t>(options.seq) : ::ComputeOnce::CurrentSeq(), ::ComputeOnce::Variant(plane->params.w, plane->params.h, component, B));
			// The passes' shared memory against the device limit (a visible refusal, never a silent fallback).
			if (!CheckPassSharedMemory(dev.maxShmem, B, scale, params, m_error))
				return false;
			const StorageChoice choice = ChooseStorage(options.storage, plane->params.format, codeUnit);
			m_storageNote = choice.why;
			const bool compact = choice.storage == GrainSplit::Storage::Compact;
			const Targets& tg = options.targets;
			if (choice.downgraded && tg.splitB)
			{
				m_error = "grain split: targets are for the compact layout but storage fell back to full (" + choice.why + ")";
				return false;
			}
			const int w = plane->params.w, h = plane->params.h;
			const int bw = (w + B - 1) / B, bh = (h + B - 1) / B, hw = HalfOf(w), hh = HalfOf(h);
			const pl_fmt f4 = gpu ? FloatFormat(gpu, 4) : nullptr, f2 = gpu ? FloatFormat(gpu, 2) : nullptr;
			pl_fmt fA = nullptr, fB = nullptr, fBlocks = f4;
			if (compact)   // a device-free run is Full (refused above otherwise)
			{
				fA = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG16F);
				fB = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG16);
				if (!fA || !fB || !f4)
				{
					m_error = "grain split: no compact storage formats (rg16f, rg16, rgba32f)";
					return false;
				}
			}
			if (gpu && (!f4 || !f2))
			{
				m_error = "grain split: no rgba32f / rg32f storage texture";
				return false;
			}
			// A supplied texture fits: its size, sampleable + storable, and its format: the device's (with a device) or, device-free,
			// the ledger format of the context's shapes (ContextShapesOf: rg32f for the half planes, rgba32f for the rest).
			const auto formatIs = [&](pl_tex t, pl_fmt fmt, VpSkeleton::TexFormat expected) {
				if (gpu)
					return t->params.format == fmt;
				// FormatOf writes `got` whenever it returns true; the initial value is never compared. R32F is never an
				// `expected` here (only rg32f / rgba32f are), so even an unwritten `got` could not match.
				VpSkeleton::TexFormat got = VpSkeleton::TexFormat::R32F;
				return VpSkeleton::PlTexBackend::FormatOf(t->params.format, &got) && got == expected;
			};
			const VpSkeleton::TexFormat t2x = VpSkeleton::TexFormat::RG32F, t4x = VpSkeleton::TexFormat::RGBA32F;
			auto usable = [&](pl_tex t, int tw, int th, pl_fmt fmt, VpSkeleton::TexFormat expected) {
				return t && t->params.w == tw && t->params.h == th && formatIs(t, fmt, expected) && t->params.sampleable && t->params.storable;
			};
			if ((tg.split && !usable(tg.split, w, h, compact ? fA : f4, t4x)) || (tg.blocks && !usable(tg.blocks, bw, bh, fBlocks, t4x)) ||
				(compact && tg.splitB && !usable(tg.splitB, w, h, fB, t4x)))
			{
				m_error = "grain split: unusable target texture";
				return false;
			}

			// Intermediates (half-resolution tmp + low, block-grid quiet): from the arena (scope closed on every exit), else private.
			pl_tex tmp = nullptr, low = nullptr, part = nullptr, quiet = nullptr, levelTex = nullptr;
			int pw = 0, ph = 0;
			PartialSize(bw, bh, B, params.subBlocks, &pw, &ph);
			const Scratch& sc = options.scratch;
			if (sc.Any() && (!sc.All() || options.arena))
			{
				m_error = "grain split: caller intermediates are all five or none, and never with an arena";
				return false;
			}
			if (sc.All())
			{
				// The caller's intermediates (the context entry's: made through a fill's GPU context). Nothing private is made.
				if (!usable(sc.tmp, hw, hh, f2, t2x) || !usable(sc.low, hw, hh, f2, t2x) || !usable(sc.part, pw, ph, f4, t4x) ||
					!usable(sc.quiet, bw, bh, f4, t4x) || !usable(sc.level, params.levelBins, 1, f4, t4x))
				{
					m_error = "grain split: unusable intermediate texture";
					return false;
				}
				tmp = sc.tmp;
				low = sc.low;
				part = sc.part;
				quiet = sc.quiet;
			}
			else if (options.arena)
			{
				// The private temporaries of the non-arena path are not used any more: free them.
				for (pl_tex* t : { &m_tmp, &m_low, &m_part, &m_quiet })
					if (*t) pl_tex_destroy(gpu, t);
				VpSkeleton::TexFormat t2 = VpSkeleton::TexFormat::RG32F, t4 = VpSkeleton::TexFormat::RGBA32F;
				if (!VpSkeleton::PlTexBackend::FormatOf(f2, &t2) || !VpSkeleton::PlTexBackend::FormatOf(f4, &t4))
				{
					m_error = "grain split: rg32f / rgba32f is not an arena format";
					return false;
				}
				VpSkeleton::TexShape half, pshape, qshape;
				pshape.w = static_cast<uint32_t>(pw); pshape.h = static_cast<uint32_t>(ph); pshape.fmt = t4;
				half.w = static_cast<uint32_t>(hw); half.h = static_cast<uint32_t>(hh); half.fmt = t2;
				qshape.w = static_cast<uint32_t>(bw); qshape.h = static_cast<uint32_t>(bh); qshape.fmt = t4;
				tmp = options.arena->Acquire(kArenaOwner, half);
				low = options.arena->Acquire(kArenaOwner, half);
				part = options.arena->Acquire(kArenaOwner, pshape);
				quiet = options.arena->Acquire(kArenaOwner, qshape);
				if (!tmp || !low || !part || !quiet)
				{
					m_scratchShort = true;
					m_error = std::string("scratch memory short (grain split): ") + (!tmp ? "tmp " : "") + (!low ? "low " : "") + (!part ? "part " : "") +
						(!quiet ? "quiet " : "") + "unavailable";
					return false;
				}
			}
			else
			{
				if (!Ensure(gpu, m_tmp, hw, hh, f2) || !Ensure(gpu, m_low, hw, hh, f2) || !Ensure(gpu, m_part, pw, ph, f4) || !Ensure(gpu, m_quiet, bw, bh, f4))
				{
					m_error = "grain split: no rg32f / rgba32f storage texture";
					return false;
				}
				tmp = m_tmp;
				low = m_low;
				part = m_part;
				quiet = m_quiet;
			}
			if (sc.All())
				levelTex = sc.level;
			else
			{
				if (!Ensure(gpu, m_level, params.levelBins, 1, f4))
				{
					m_error = "grain split: no rgba32f noise level table";
					return false;
				}
				levelTex = m_level;
			}
			// Outputs: the caller's targets, else the Splitter's own.
			pl_tex outA = nullptr, outB = nullptr, outFull = nullptr, blocksTex = tg.blocks;
			if (compact)
			{
				outA = tg.split;
				outB = tg.splitB;
				if ((!outA && !Ensure(gpu, m_outA, w, h, fA)) || (!outB && !Ensure(gpu, m_outB, w, h, fB)))
				{
					m_error = "grain split: no compact output textures";
					return false;
				}
				if (!outA) outA = m_outA;
				if (!outB) outB = m_outB;
			}
			else
			{
				outFull = tg.split;
				if (!outFull && !Ensure(gpu, m_outFull, w, h, f4))
				{
					m_error = "grain split: no rgba32f storage texture";
					return false;
				}
				if (!outFull) outFull = m_outFull;
			}
			if (!blocksTex)
			{
				if (!Ensure(gpu, m_blocks, bw, bh, fBlocks))
				{
					m_error = "grain split: no rgba32f storage texture";
					return false;
				}
				blocksTex = m_blocks;
			}
			m_run.active = true;
			m_run.next = 0;
			m_run.compact = compact;
			m_run.storage = choice.storage;
			m_run.w = w; m_run.h = h; m_run.bw = bw; m_run.bh = bh;
			m_run.tmp = tmp; m_run.low = low; m_run.part = part; m_run.quiet = quiet; m_run.level = levelTex; m_run.blocks = blocksTex;
			m_run.outFull = outFull; m_run.outA = outA; m_run.outB = outB;
			return true;
		}
	}
}
