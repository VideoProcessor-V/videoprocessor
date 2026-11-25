// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// motion/estimation/global_model: D1 whole-frame translation GPU shaders and dispatch helpers
// (U10 C2 pure move from matcher/DejudderWarp.cpp).
//
// All 14 GLSL strings below are byte-for-byte identical to the matcher's D1 kernels (including J#2's
// textureLod in kGlobalBothL1Body). Scratch textures (sad2, sad1, pick / bothSad2, bothSad1, bothPick)
// remain caller-owned and caller-destroyed so MotionWarp's GPU resource lifecycle is unchanged.
// Header-only, standalone C++14.
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>

#include <functional>
#include <string>

namespace VpMotion
{
namespace MatcherGlobalD1
{
	// D1 search grid dimensions derived from the shader search radii:
	//   L2 (1/32 resolution): +-8 texels -> 2 * 8 + 1 = 17 candidates per axis (289 total).
	//   L1 (1/8 resolution):  +-2 texels in 0.5-texel steps -> 2 * 4 + 1 = 9 candidates per axis (81 total).
	//   Workgroup size: 16 x 16 = 256 threads (matching shared g_sad_[256] / gs_s_[256] / gs_n_[256]).
	static constexpr int kL2RadiusTexels = 8;
	static constexpr int kL2Candidates = 2 * kL2RadiusTexels + 1;
	static constexpr int kL1HalfStepRadius = 4;
	static constexpr int kL1Candidates = 2 * kL1HalfStepRadius + 1;
	static constexpr int kWorkgroupSide = 16;

	// D1: the whole frame's translation, by one 16x16 group. Every vector
	// within +-8 texels at 1/32 resolution (the mean absolute difference
	// over the overlap, strided to about 2048 samples), then +-2 texels in
	// half steps at 1/8 around the winner. z = best / second best (outside
	// +-1 of the best): near 1 on flat or ambiguous pictures.
	static constexpr const char kGlobalHeader[] = R"GLSL(
shared float g_sad_[256];
shared int g_idx_[256];
ivec2 l1s_;
ivec2 l2s_;
float g2from(ivec2 p) { return texelFetch(from_l2, p, 0).r; }
float g2to(ivec2 p) { return texelFetch(to_l2, p, 0).r; }
float g1from(ivec2 p) { return texelFetch(from_l1, p, 0).r; }
float g1to(vec2 p) { return texture(to_l1, (p + vec2(0.5)) / vec2(l1s_)).r; }

float frame_sad_l2(ivec2 v, int stride)
{
    float s = 0.0, n = 0.0, all_ = 0.0;
    for (int y = 0; y < l2s_.y; y += stride)
        for (int x = 0; x < l2s_.x; x += stride) {
            all_ += 1.0;
            ivec2 p = ivec2(x, y);
            ivec2 q = p + v;
            if (q.x < 0 || q.y < 0 || q.x >= l2s_.x || q.y >= l2s_.y)
                continue;
            s += abs(g2from(p) - g2to(q));
            n += 1.0;
        }
    return n * 4.0 >= all_ && n > 0.0 ? s / n : 1e30;
}

float frame_sad_l1(vec2 v, int stride)
{
    float s = 0.0, n = 0.0, all_ = 0.0;
    for (int y = 0; y < l1s_.y; y += stride)
        for (int x = 0; x < l1s_.x; x += stride) {
            all_ += 1.0;
            vec2 q = vec2(x, y) + v;
            if (q.x < 0.0 || q.y < 0.0 || q.x > float(l1s_.x - 1) || q.y > float(l1s_.y - 1))
                continue;
            s += abs(g1from(ivec2(x, y)) - g1to(q));
            n += 1.0;
        }
    return n * 4.0 >= all_ && n > 0.0 ? s / n : 1e30;
}

void argmin_(uint i)
{
    for (uint stride = 128u; stride > 0u; stride >>= 1) {
        if (i < stride && g_sad_[i + stride] < g_sad_[i]) {
            g_sad_[i] = g_sad_[i + stride];
            g_idx_[i] = g_idx_[i + stride];
        }
        barrier();
    }
}
)GLSL";

	static constexpr const char kGlobalBody[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
l1s_ = textureSize(from_l1, 0);
l2s_ = textureSize(from_l2, 0);
int stride2 = max(1, int(ceil(sqrt(float(l2s_.x * l2s_.y) / 2048.0))));
int stride1 = max(1, int(ceil(sqrt(float(l1s_.x * l1s_.y) / 4096.0))));
// 17 x 17 candidates at 1/32: at most two per thread, kept for the second best.
float mine[2] = float[2](1e30, 1e30);
int mk[2] = int[2](-1, -1);
for (int j = 0; j < 2; j++) {
    int k = int(i) + j * 256;
    if (k < 289) {
        mk[j] = k;
        mine[j] = frame_sad_l2(ivec2(k % 17 - 8, k / 17 - 8), stride2);
    }
}
g_sad_[i] = mine[1] < mine[0] ? mine[1] : mine[0];
g_idx_[i] = mine[1] < mine[0] ? mk[1] : mk[0];
barrier();
argmin_(i);
int kb = g_idx_[0];
float best = g_sad_[0];
barrier();
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
float second = 1e30;
for (int j = 0; j < 2; j++) {
    if (mk[j] < 0) continue;
    ivec2 v = ivec2(mk[j] % 17 - 8, mk[j] / 17 - 8);
    if (max(abs(v.x - vb.x), abs(v.y - vb.y)) <= 1) continue;
    second = min(second, mine[j]);
}
g_sad_[i] = second;
g_idx_[i] = 0;
barrier();
argmin_(i);
second = g_sad_[0];
barrier();
// 1/8 resolution: 9 x 9 half-texel steps around the winner.
float s1 = 1e30;
if (i < 81u && kb >= 0)
    s1 = frame_sad_l1(vec2(vb * 4) + 0.5 * vec2(int(i) % 9 - 4, int(i) / 9 - 4), stride1);
g_sad_[i] = s1;
g_idx_[i] = int(i);
barrier();
argmin_(i);
if (i == 0u) {
    int k1 = g_idx_[0];
    vec2 v = vec2(vb * 4) + 0.5 * vec2(k1 % 9 - 4, k1 / 9 - 4);
    bool ok = kb >= 0 && best < 1e29;
    float ratio = ok ? (best + 0.05) / (min(second, 1e6) + 0.05) : 1.0;
    imageStore(dst, ivec2(0), vec4(ok ? v : vec2(0.0), ratio, 1.0));
}
)GLSL";

	// D1 in parallel (owner 2026-10-13 efficiency): the serial body above
	// ran on one work group. Here one work group per candidate sums its
	// frame SAD (256 threads, a shared reduction), and single-group passes
	// pick exactly as the serial body does (lowest SAD, lowest index on a
	// tie). The sums' order differs from the serial loop (float rounding
	// only).
	static constexpr const char kGlobalSumHeader[] = R"GLSL(
shared float gs_s_[256];
shared float gs_n_[256];
)GLSL";

	static constexpr const char kGlobalL2Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
ivec2 l2s = textureSize(from_l2, 0);
int stride2 = max(1, int(ceil(sqrt(float(l2s.x * l2s.y) / 2048.0))));
ivec2 v = ivec2(gl_WorkGroupID.xy) - ivec2(8);
int cols = (l2s.x + stride2 - 1) / stride2;
int total = cols * ((l2s.y + stride2 - 1) / stride2);
float s = 0.0, n = 0.0;
for (int k = int(i); k < total; k += 256) {
    ivec2 p = ivec2(k % cols, k / cols) * stride2;
    ivec2 q = p + v;
    if (q.x < 0 || q.y < 0 || q.x >= l2s.x || q.y >= l2s.y)
        continue;
    s += abs(texelFetch(from_l2, p, 0).r - texelFetch(to_l2, q, 0).r);
    n += 1.0;
}
gs_s_[i] = s;
gs_n_[i] = n;
barrier();
for (uint st = 128u; st > 0u; st >>= 1) {
    if (i < st) {
        gs_s_[i] += gs_s_[i + st];
        gs_n_[i] += gs_n_[i + st];
    }
    barrier();
}
if (i == 0u) {
    float r = gs_n_[0] * 4.0 >= float(total) && gs_n_[0] > 0.0 ? gs_s_[0] / gs_n_[0] : 1e30;
    imageStore(dst, ivec2(gl_WorkGroupID.xy), vec4(r, 0.0, 0.0, 1.0));
}
)GLSL";

	static constexpr const char kGlobalPickHeader[] = R"GLSL(
shared float g_sad_[256];
shared int g_idx_[256];
void argmin_(uint i)
{
    for (uint stride = 128u; stride > 0u; stride >>= 1) {
        if (i < stride && g_sad_[i + stride] < g_sad_[i]) {
            g_sad_[i] = g_sad_[i + stride];
            g_idx_[i] = g_idx_[i + stride];
        }
        barrier();
    }
}
)GLSL";

	// The 1/32 winner and the second best away from it (as the serial body).
	static constexpr const char kGlobalPick2Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
float mine[2] = float[2](1e30, 1e30);
int mk[2] = int[2](-1, -1);
for (int j = 0; j < 2; j++) {
    int k = int(i) + j * 256;
    if (k < 289) {
        mk[j] = k;
        mine[j] = texelFetch(sad2, ivec2(k % 17, k / 17), 0).r;
    }
}
g_sad_[i] = mine[1] < mine[0] ? mine[1] : mine[0];
g_idx_[i] = mine[1] < mine[0] ? mk[1] : mk[0];
barrier();
argmin_(i);
int kb = g_idx_[0];
float best = g_sad_[0];
barrier();
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
float second = 1e30;
int ks = -1;
for (int j = 0; j < 2; j++) {
    if (mk[j] < 0) continue;
    ivec2 v = ivec2(mk[j] % 17 - 8, mk[j] / 17 - 8);
    if (max(abs(v.x - vb.x), abs(v.y - vb.y)) <= 1) continue;
    if (mine[j] < second) { second = mine[j]; ks = mk[j]; }
}
g_sad_[i] = second;
g_idx_[i] = ks;
barrier();
argmin_(i);
// w: 1 + the second's index (M2; 0 = none), >= 1 once written.
if (i == 0u)
    imageStore(dst, ivec2(0), vec4(float(kb), best, g_sad_[0], 1.0 + float(max(g_idx_[0], -1) + 1)));
)GLSL";

	// The 1/8 candidates around the winner, one work group each.
	static constexpr const char kGlobalL1Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
ivec2 l1s = textureSize(from_l1, 0);
int stride1 = max(1, int(ceil(sqrt(float(l1s.x * l1s.y) / 4096.0))));
vec4 pick = texelFetch(global_pick, ivec2(0), 0);
int kb = int(pick.x);
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
vec2 v = vec2(vb * 4) + 0.5 * vec2(ivec2(gl_WorkGroupID.xy) - ivec2(4));
int cols = (l1s.x + stride1 - 1) / stride1;
int total = cols * ((l1s.y + stride1 - 1) / stride1);
float s = 0.0, n = 0.0;
for (int k = int(i); k < total; k += 256) {
    ivec2 p = ivec2(k % cols, k / cols) * stride1;
    vec2 q = vec2(p) + v;
    if (q.x < 0.0 || q.y < 0.0 || q.x > float(l1s.x - 1) || q.y > float(l1s.y - 1))
        continue;
    s += abs(texelFetch(from_l1, p, 0).r - texture(to_l1, (q + vec2(0.5)) / vec2(l1s)).r);
    n += 1.0;
}
gs_s_[i] = s;
gs_n_[i] = n;
barrier();
for (uint st = 128u; st > 0u; st >>= 1) {
    if (i < st) {
        gs_s_[i] += gs_s_[i + st];
        gs_n_[i] += gs_n_[i + st];
    }
    barrier();
}
if (i == 0u) {
    float r = kb >= 0 && gs_n_[0] * 4.0 >= float(total) && gs_n_[0] > 0.0 ? gs_s_[0] / gs_n_[0] : 1e30;
    imageStore(dst, ivec2(gl_WorkGroupID.xy), vec4(r, 0.0, 0.0, 1.0));
}
)GLSL";

	// The 1/8 winner and the global vector (as the serial body).
	static constexpr const char kGlobalPick1Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
vec4 pick = texelFetch(global_pick, ivec2(0), 0);
int kb = int(pick.x);
float best = pick.y, second = pick.z;
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
g_sad_[i] = i < 81u && kb >= 0 ? texelFetch(sad1, ivec2(int(i) % 9, int(i) / 9), 0).r : 1e30;
g_idx_[i] = int(i);
barrier();
argmin_(i);
if (i == 0u) {
    int k1 = g_idx_[0];
    vec2 v = vec2(vb * 4) + 0.5 * vec2(k1 % 9 - 4, k1 / 9 - 4);
    bool ok = kb >= 0 && best < 1e29;
    float ratio = ok ? (best + 0.05) / (min(second, 1e6) + 0.05) : 1.0;
    imageStore(dst, ivec2(0), vec4(ok ? v : vec2(0.0), ratio, 1.0));
#ifdef GLOBAL_TWO
    // M2: the second candidate at 1/32 precision (in 1/8 texels); the
    // block search refines it as it refines the first.
    int k2 = int(pick.w + 0.5) - 2;
    bool ok2 = ok && k2 >= 0 && second < 1e29;
    vec2 v2nd = ok2 ? vec2(ivec2(k2 % 17 - 8, k2 / 17 - 8) * 4) : vec2(0.0);
    imageStore(second_out, ivec2(0), vec4(v2nd, 0.0, ok2 ? 1.0 : 0.0));
#endif
}
)GLSL";

	// Both directions in one dispatch per stage (2026-10-15): work group x
	// below the stage's width is A to B, from there B to A; each group's
	// work is Global's for its direction (the same sums in the same order).
	static constexpr const char kGlobalBothHeader[] = R"GLSL(
shared float gs_s_[256];
shared float gs_n_[256];
)GLSL";

	// The 1/32 stage's reads (its own header: the 1/8 stage binds other textures).
	static constexpr const char kGlobalBothL2Header[] = R"GLSL(
// Direction d: 0 = A to B (from A), 1 = B to A (from B).
float from_l2_at(int d, ivec2 p) { return d == 0 ? texelFetch(l2_a, p, 0).r : texelFetch(l2_b, p, 0).r; }
float to_l2_at(int d, ivec2 p) { return d == 0 ? texelFetch(l2_b, p, 0).r : texelFetch(l2_a, p, 0).r; }
)GLSL";

	static constexpr const char kGlobalBothL2Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
int d = int(gl_WorkGroupID.x) / 17;
ivec2 g = ivec2(int(gl_WorkGroupID.x) % 17, int(gl_WorkGroupID.y));
ivec2 l2s = textureSize(l2_a, 0);
int stride2 = max(1, int(ceil(sqrt(float(l2s.x * l2s.y) / 2048.0))));
ivec2 v = g - ivec2(8);
int cols = (l2s.x + stride2 - 1) / stride2;
int total = cols * ((l2s.y + stride2 - 1) / stride2);
float s = 0.0, n = 0.0;
for (int k = int(i); k < total; k += 256) {
    ivec2 p = ivec2(k % cols, k / cols) * stride2;
    ivec2 q = p + v;
    if (q.x < 0 || q.y < 0 || q.x >= l2s.x || q.y >= l2s.y)
        continue;
    s += abs(from_l2_at(d, p) - to_l2_at(d, q));
    n += 1.0;
}
gs_s_[i] = s;
gs_n_[i] = n;
barrier();
for (uint st = 128u; st > 0u; st >>= 1) {
    if (i < st) {
        gs_s_[i] += gs_s_[i + st];
        gs_n_[i] += gs_n_[i + st];
    }
    barrier();
}
if (i == 0u) {
    float r = gs_n_[0] * 4.0 >= float(total) && gs_n_[0] > 0.0 ? gs_s_[0] / gs_n_[0] : 1e30;
    imageStore(dst, ivec2(gl_WorkGroupID.xy), vec4(r, 0.0, 0.0, 1.0));
}
)GLSL";

	static constexpr const char kGlobalBothPick2Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
int d = int(gl_WorkGroupID.x);
float mine[2] = float[2](1e30, 1e30);
int mk[2] = int[2](-1, -1);
for (int j = 0; j < 2; j++) {
    int k = int(i) + j * 256;
    if (k < 289) {
        mk[j] = k;
        mine[j] = texelFetch(sad2, ivec2(d * 17 + k % 17, k / 17), 0).r;
    }
}
g_sad_[i] = mine[1] < mine[0] ? mine[1] : mine[0];
g_idx_[i] = mine[1] < mine[0] ? mk[1] : mk[0];
barrier();
argmin_(i);
int kb = g_idx_[0];
float best = g_sad_[0];
barrier();
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
float second = 1e30;
int ks = -1;
for (int j = 0; j < 2; j++) {
    if (mk[j] < 0) continue;
    ivec2 v = ivec2(mk[j] % 17 - 8, mk[j] / 17 - 8);
    if (max(abs(v.x - vb.x), abs(v.y - vb.y)) <= 1) continue;
    if (mine[j] < second) { second = mine[j]; ks = mk[j]; }
}
g_sad_[i] = second;
g_idx_[i] = ks;
barrier();
argmin_(i);
if (i == 0u)
    imageStore(dst, ivec2(d, 0), vec4(float(kb), best, g_sad_[0], 1.0 + float(max(g_idx_[0], -1) + 1)));
)GLSL";

	static constexpr const char kGlobalBothL1Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
int d = int(gl_WorkGroupID.x) / 9;
ivec2 g = ivec2(int(gl_WorkGroupID.x) % 9, int(gl_WorkGroupID.y));
ivec2 l1s = textureSize(l1_a, 0);
int stride1 = max(1, int(ceil(sqrt(float(l1s.x * l1s.y) / 4096.0))));
vec4 pick = texelFetch(global_pick, ivec2(d, 0), 0);
int kb = int(pick.x);
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
vec2 v = vec2(vb * 4) + 0.5 * vec2(g - ivec2(4));
int cols = (l1s.x + stride1 - 1) / stride1;
int total = cols * ((l1s.y + stride1 - 1) / stride1);
float s = 0.0, n = 0.0;
for (int k = int(i); k < total; k += 256) {
    ivec2 p = ivec2(k % cols, k / cols) * stride1;
    vec2 q = vec2(p) + v;
    if (q.x < 0.0 || q.y < 0.0 || q.x > float(l1s.x - 1) || q.y > float(l1s.y - 1))
        continue;
    vec2 t = (q + vec2(0.5)) / vec2(l1s);
    // Explicit LOD 0.0 is legal inside branches in D3D11 HLSL and Vulkan/SPIR-V,
    // halving L1 texture bandwidth per direction (bit-identical on single-mip l1_a/l1_b).
    float a, b;
    if (d == 0) {
        a = texelFetch(l1_a, p, 0).r;
        b = textureLod(l1_b, t, 0.0).r;
    } else {
        a = texelFetch(l1_b, p, 0).r;
        b = textureLod(l1_a, t, 0.0).r;
    }
    s += abs(a - b);
    n += 1.0;
}
gs_s_[i] = s;
gs_n_[i] = n;
barrier();
for (uint st = 128u; st > 0u; st >>= 1) {
    if (i < st) {
        gs_s_[i] += gs_s_[i + st];
        gs_n_[i] += gs_n_[i + st];
    }
    barrier();
}
if (i == 0u) {
    float r = kb >= 0 && gs_n_[0] * 4.0 >= float(total) && gs_n_[0] > 0.0 ? gs_s_[0] / gs_n_[0] : 1e30;
    imageStore(dst, ivec2(gl_WorkGroupID.xy), vec4(r, 0.0, 0.0, 1.0));
}
)GLSL";

	static constexpr const char kGlobalBothPick1Body[] = R"GLSL(
uint i = gl_LocalInvocationIndex;
int d = int(gl_WorkGroupID.x);
vec4 pick = texelFetch(global_pick, ivec2(d, 0), 0);
int kb = int(pick.x);
float best = pick.y, second = pick.z;
ivec2 vb = ivec2(kb % 17 - 8, kb / 17 - 8);
g_sad_[i] = i < 81u && kb >= 0 ? texelFetch(sad1, ivec2(d * 9 + int(i) % 9, int(i) / 9), 0).r : 1e30;
g_idx_[i] = int(i);
barrier();
argmin_(i);
if (i == 0u) {
    int k1 = g_idx_[0];
    vec2 v = vec2(vb * 4) + 0.5 * vec2(k1 % 9 - 4, k1 / 9 - 4);
    bool ok = kb >= 0 && best < 1e29;
    float ratio = ok ? (best + 0.05) / (min(second, 1e6) + 0.05) : 1.0;
    vec4 r = vec4(ok ? v : vec2(0.0), ratio, 1.0);
    if (d == 0)
        imageStore(dst0, ivec2(0), r);
    else
        imageStore(dst1, ivec2(0), r);
#ifdef GLOBAL_TWO
    int k2 = int(pick.w + 0.5) - 2;
    bool ok2 = ok && k2 >= 0 && second < 1e29;
    vec2 v2nd = ok2 ? vec2(ivec2(k2 % 17 - 8, k2 / 17 - 8) * 4) : vec2(0.0);
    vec4 r2 = vec4(v2nd, 0.0, ok2 ? 1.0 : 0.0);
    if (d == 0)
        imageStore(second0, ivec2(0), r2);
    else
        imageStore(second1, ivec2(0), r2);
#endif
}
)GLSL";

	inline pl_shader_desc Sampled(const char* name, pl_tex tex, bool linear)
	{
		pl_shader_desc desc{};
		desc.desc.name = name;
		desc.desc.type = PL_DESC_SAMPLED_TEX;
		desc.binding.object = tex;
		desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		desc.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
		return desc;
	}

	inline pl_shader_desc Storage(const char* name, pl_tex tex)
	{
		pl_shader_desc desc{};
		desc.desc.name = name;
		desc.desc.type = PL_DESC_STORAGE_IMG;
		desc.desc.access = PL_DESC_ACCESS_WRITEONLY;
		desc.binding.object = tex;
		return desc;
	}

	using DispatchFn = std::function<bool(
		const char* description, const char* header, const char* body,
		const pl_shader_desc* descriptors, int numDescriptors,
		const pl_shader_var* variables, int numVariables,
		int w, int h, int group)>;

	using DispatchPerBlockFn = std::function<bool(
		const char* description, const char* header, const char* body,
		const pl_shader_desc* descriptors, int numDescriptors,
		const pl_shader_var* variables, int numVariables,
		int w, int h, int threadsX, int threadsY)>;

	// Non-owning handles to caller-owned D1 scratch textures (for single-direction or bidirectional passes).
	struct ScratchTextures
	{
		pl_tex* sad2 = nullptr;
		pl_tex* sad1 = nullptr;
		pl_tex* pick = nullptr;
	};

	inline bool RunSingle(pl_gpu gpu, bool serialReference,
		pl_tex fromL1, pl_tex toL1, pl_tex fromL2, pl_tex toL2,
		pl_tex out, pl_tex second,
		pl_tex& sad2, pl_tex& sad1, pl_tex& pick,
		const DispatchFn& dispatch, const DispatchPerBlockFn& dispatchPerBlock,
		std::string& error)
	{
		if (serialReference ? !dispatch : !dispatchPerBlock)
		{
			error = "global motion: missing dispatch callback";
			return false;
		}
		if (!fromL1 || !toL1 || !fromL2 || !toL2 || !out)
		{
			error = "global motion: missing input or output texture";
			return false;
		}
		if (serialReference)
		{
			const pl_shader_desc descriptors[5] = {
				Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
				Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false), Storage("dst", out) };
			return dispatch("VideoProcessor de-judder: global motion (D1, serial reference)", kGlobalHeader, kGlobalBody,
				descriptors, 5, nullptr, 0, kWorkgroupSide, kWorkgroupSide, kWorkgroupSide);
		}
		// Parallel: candidate SADs, the pick, the 1/8 candidates, the vector.
		if (!sad2 || !sad1 || !pick)
		{
			if (!gpu)
			{
				error = "global motion: missing GPU";
				return false;
			}
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			pl_fmt r32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, caps);
			pl_fmt rgba32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
			if (!r32 || !rgba32)
			{
				error = "global motion: no 32-bit float storage format";
				return false;
			}
			auto make = [gpu](pl_tex* tex, int w, int h, pl_fmt fmt) -> bool
			{
				pl_tex_params params{};
				params.w = w;
				params.h = h;
				params.format = fmt;
				params.sampleable = true;
				params.storable = true;
				return pl_tex_recreate(gpu, tex, &params);
			};
			if (!make(&sad2, kL2Candidates, kL2Candidates, r32) ||
				!make(&sad1, kL1Candidates, kL1Candidates, r32) ||
				!make(&pick, 1, 1, rgba32))
			{
				error = "global motion: cannot create the candidate textures";
				return false;
			}
		}
		static const std::string pickHeader = std::string(kGlobalPickHeader);
		static const std::string pickTwoHeader = std::string("#define GLOBAL_TWO 1\n") + kGlobalPickHeader;
		const pl_shader_desc l2Desc[3] = {
			Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false), Storage("dst", sad2) };
		const pl_shader_desc pick2Desc[2] = { Sampled("sad2", sad2, false), Storage("dst", pick) };
		const pl_shader_desc l1Desc[4] = {
			Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
			Sampled("global_pick", pick, false), Storage("dst", sad1) };
		const pl_shader_desc pick1Desc[4] = {
			Sampled("sad1", sad1, false), Sampled("global_pick", pick, false), Storage("dst", out),
			Storage("second_out", second) };
		return dispatchPerBlock("VideoProcessor de-judder: global motion (D1)", kGlobalSumHeader, kGlobalL2Body,
				l2Desc, 3, nullptr, 0, kL2Candidates, kL2Candidates, kWorkgroupSide, kWorkgroupSide) &&
			dispatchPerBlock("VideoProcessor de-judder: global motion pick (D1)", pickHeader.c_str(), kGlobalPick2Body,
				pick2Desc, 2, nullptr, 0, 1, 1, kWorkgroupSide, kWorkgroupSide) &&
			dispatchPerBlock("VideoProcessor de-judder: global motion 1/8 (D1)", kGlobalSumHeader, kGlobalL1Body,
				l1Desc, 4, nullptr, 0, kL1Candidates, kL1Candidates, kWorkgroupSide, kWorkgroupSide) &&
			dispatchPerBlock("VideoProcessor de-judder: global motion vector (D1)", second ? pickTwoHeader.c_str() : pickHeader.c_str(),
				kGlobalPick1Body, pick1Desc, second ? 4 : 3, nullptr, 0, 1, 1, kWorkgroupSide, kWorkgroupSide);
	}

	inline bool RunBoth(pl_gpu gpu,
		pl_tex l1A, pl_tex l1B, pl_tex l2A, pl_tex l2B,
		pl_tex out[2], pl_tex second[2],
		pl_tex& bothSad2, pl_tex& bothSad1, pl_tex& bothPick,
		const DispatchPerBlockFn& dispatchPerBlock,
		std::string& error)
	{
		if (!dispatchPerBlock)
		{
			error = "global motion: missing dispatch callback";
			return false;
		}
		if (!l1A || !l1B || !l2A || !l2B || !out || !out[0] || !out[1])
		{
			error = "global motion: missing input or output texture";
			return false;
		}
		if (!bothSad2 || !bothSad1 || !bothPick)
		{
			if (!gpu)
			{
				error = "global motion: missing GPU";
				return false;
			}
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			pl_fmt r32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, caps);
			pl_fmt rgba32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
			if (!r32 || !rgba32)
			{
				error = "global motion: no 32-bit float storage format";
				return false;
			}
			auto make = [gpu](pl_tex* tex, int w, int h, pl_fmt fmt) -> bool
			{
				pl_tex_params params{};
				params.w = w;
				params.h = h;
				params.format = fmt;
				params.sampleable = true;
				params.storable = true;
				return pl_tex_recreate(gpu, tex, &params);
			};
			if (!make(&bothSad2, 2 * kL2Candidates, kL2Candidates, r32) ||
				!make(&bothSad1, 2 * kL1Candidates, kL1Candidates, r32) ||
				!make(&bothPick, 2, 1, rgba32))
			{
				error = "global motion: cannot create the candidate textures";
				return false;
			}
		}
		static const std::string pickHeader = std::string(kGlobalPickHeader);
		static const std::string pickTwoHeader = std::string("#define GLOBAL_TWO 1\n") + kGlobalPickHeader;
		const bool two = second && second[0] && second[1];
		const pl_shader_desc l2Desc[3] = { Sampled("l2_a", l2A, false), Sampled("l2_b", l2B, false), Storage("dst", bothSad2) };
		const pl_shader_desc pick2Desc[2] = { Sampled("sad2", bothSad2, false), Storage("dst", bothPick) };
		const pl_shader_desc l1Desc[4] = { Sampled("l1_a", l1A, true), Sampled("l1_b", l1B, true),
			Sampled("global_pick", bothPick, false), Storage("dst", bothSad1) };
		const pl_shader_desc pick1Desc[6] = { Sampled("sad1", bothSad1, false), Sampled("global_pick", bothPick, false),
			Storage("dst0", out[0]), Storage("dst1", out[1]), Storage("second0", two ? second[0] : nullptr),
			Storage("second1", two ? second[1] : nullptr) };
		static const std::string l2Header = std::string(kGlobalBothHeader) + kGlobalBothL2Header;
		return dispatchPerBlock("VideoProcessor de-judder: global motion (D1)", l2Header.c_str(), kGlobalBothL2Body,
				l2Desc, 3, nullptr, 0, 2 * kL2Candidates, kL2Candidates, kWorkgroupSide, kWorkgroupSide) &&
			dispatchPerBlock("VideoProcessor de-judder: global motion pick (D1)", pickHeader.c_str(), kGlobalBothPick2Body,
				pick2Desc, 2, nullptr, 0, 2, 1, kWorkgroupSide, kWorkgroupSide) &&
			dispatchPerBlock("VideoProcessor de-judder: global motion 1/8 (D1)", kGlobalBothHeader, kGlobalBothL1Body,
				l1Desc, 4, nullptr, 0, 2 * kL1Candidates, kL1Candidates, kWorkgroupSide, kWorkgroupSide) &&
			dispatchPerBlock("VideoProcessor de-judder: global motion vector (D1)", two ? pickTwoHeader.c_str() : pickHeader.c_str(),
				kGlobalBothPick1Body, pick1Desc, two ? 6 : 4, nullptr, 0, 2, 1, kWorkgroupSide, kWorkgroupSide);
	}
}
}
