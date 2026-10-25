#include <pch.h>
#include "DetailPulse.h"

#include <vprenderer/GrainPreserve.h>

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// detail_pulse_stabilise (DetailPulse.h has the maths). Three compute passes per new source
// frame, two of them at the plane's resolution:
//  1. measure (one 16 x 16 workgroup per 32 x 32 tile): the plane's codes around the tile and
//     the 2x2 boxes around its half-resolution block go into shared memory; the fine Gaussian
//     and the wide half-resolution low band are done there (separable); the tile's band
//     energies, low-band mean and spread, untrusted share and veto weight are written.
//  2. reduce (one workgroup): the frame's energies from the tiles, the cut and fade checks,
//     the reference from the ring, the gains; the frame's entry into the ring.
//  3. apply (one workgroup per 32 x 32 block): the bands of the picture being drawn (both,
//     in shared memory, as the measure pass has them), the gains times the tile weight
//     (bilinear between tile centres), added to the source sample.
// A repeat runs the apply pass alone, with the stored decision and the bands of the picture
// it is given (hunt 3 F6: a Stage B phase picture, a gap fill or a de-judder extra is another
// picture than A; A's stored low band on it was a ghost of A). A picture of another geometry
// (the crossfade's path being left) reads the tile weights at the same picture position.
// One-plane RGB (hunt 3 F8 / F9): the luma of R, G, B in the source's codes, the same delta on
// all three. The CPU keeps only which ring slot is whose and how many consecutive frames stand
// behind it; every value lives on the GPU (no readback).
// detail_pulse_fast (default on; Params::fast, off = the passes above): a low band pass at half
// resolution first (once per picture, into an fp32 texture), then the measure, reduce and
// apply passes reading it back; a repeat or another geometry runs the low band pass and the
// apply pass. See "detail_pulse_fast" below and docs/agent-reports/detail-pulse-perf.md.
namespace DetailPulse
{
	namespace
	{
		// Shared by the measure and apply passes: SW x SW codes around the 32 x 32 block
		// (fine band), RB x RB half-resolution boxes around its 16 x 16 half block and the
		// separable low band on them, HL x HL low-band texels (the half block and a one-texel
		// border: the bilinear read-back of every pixel of the block).
		const char* const BAND_HEADER = R"GLSL(
#define BLK 32
#define HB 16
#define SW (BLK + 2 * R1)
#define HL (HB + 2)
#define RB (HL + 2 * GP_R)
shared float sy[SW * SW];
shared float sh[SW * BLK];
shared float slow[HL * HL];
shared float rd[1024];

// The plane's code at p: the luma plane's sample, or (one-plane RGB) the luma of R, G, B in
// the source's codes.
float code_at(ivec2 p)
{
#ifdef DP_RGB
    vec4 t = texelFetch(src, p, 0);
    return luma_mul * dot(t.rgb, luma_w) + luma_add;
#else
    return code_scale * texelFetch(src, p, 0).r;
#endif
}

// The fine band's vertical half on this block's pixel o (sh holds the horizontal half).
float fine_at(ivec2 o)
{
    float a = 0.0;
    for (int k = -R1; k <= R1; k++)
        a += fw(k) * sh[(o.y + R1 + k) * BLK + o.x];
    return a;
}

// The low band at the block's pixel o: bilinear in slow at (o + 0.5) / 2 (GrainModel::LowAt).
float low_at(ivec2 o)
{
    vec2 g = (vec2(o) + 0.5) * 0.5 - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    ivec2 j = i0 + ivec2(1);
    float top = slow[j.y * HL + j.x] * (1.0 - f.x) + slow[j.y * HL + j.x + 1] * f.x;
    float bottom = slow[(j.y + 1) * HL + j.x] * (1.0 - f.x) + slow[(j.y + 1) * HL + j.x + 1] * f.x;
    return top * (1.0 - f.y) + bottom * f.y;
}

// The sum over the workgroup (every invocation gets it).
vec4 reduce4(vec4 v)
{
    uint li = gl_LocalInvocationIndex;
    barrier();
    rd[li * 4u] = v.x;
    rd[li * 4u + 1u] = v.y;
    rd[li * 4u + 2u] = v.z;
    rd[li * 4u + 3u] = v.w;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) {
            rd[li * 4u] += rd[(li + s) * 4u];
            rd[li * 4u + 1u] += rd[(li + s) * 4u + 1u];
            rd[li * 4u + 2u] += rd[(li + s) * 4u + 2u];
            rd[li * 4u + 3u] += rd[(li + s) * 4u + 3u];
        }
        barrier();
    }
    return vec4(rd[0], rd[1], rd[2], rd[3]);
}

// The block's codes (and their horizontal fine half) into shared memory.
void load_block(ivec2 origin, ivec2 hi)
{
    uint li = gl_LocalInvocationIndex;
    for (int i = int(li); i < SW * SW; i += 256)
        sy[i] = code_at(clamp(origin - ivec2(R1) + ivec2(i % SW, i / SW), ivec2(0), hi));
    barrier();
    for (int i = int(li); i < SW * BLK; i += 256) {
        int r = i / BLK, c = i % BLK;
        float a = 0.0;
        for (int k = -R1; k <= R1; k++)
            a += fw(k) * sy[r * SW + c + R1 + k];
        sh[i] = a;
    }
}
)GLSL";

		// The half-resolution low band around a block's half block, in shared memory (the
		// measure and the apply pass: every pass reads the picture it is given).
		const char* const LOW_HEADER = R"GLSL(
shared float sbox[RB * RB];
shared float shor[RB * HL];

// slow = the low band over the half block qb and a one-texel border (GrainModel::Low: the
// 2x2 boxes at the clamped half-resolution positions, then the separable Gaussian, edges
// clamped at each step). Ends without a barrier: the caller's next one covers slow.
void load_low(ivec2 qb, ivec2 hi, ivec2 hhi)
{
    uint li = gl_LocalInvocationIndex;
    ivec2 ob = qb - ivec2(1 + GP_R);
    for (int i = int(li); i < RB * RB; i += 256) {
        ivec2 b = clamp(ob + ivec2(i % RB, i / RB), ivec2(0), hhi) * 2;
        sbox[i] = 0.25 * (code_at(clamp(b, ivec2(0), hi)) + code_at(clamp(b + ivec2(1, 0), ivec2(0), hi)) +
                          code_at(clamp(b + ivec2(0, 1), ivec2(0), hi)) + code_at(clamp(b + ivec2(1, 1), ivec2(0), hi)));
    }
    barrier();
    for (int i = int(li); i < RB * HL; i += 256) {
        int r = i / HL, c = i % HL;
        int x = clamp(qb.x - 1 + c, 0, hhi.x) - ob.x;
        float a = 0.0;
        for (int k = -GP_R; k <= GP_R; k++)
            a += gp_w(k) * sbox[r * RB + x + k];
        shor[i] = a;
    }
    barrier();
    for (int i = int(li); i < HL * HL; i += 256) {
        int r = i / HL, c = i % HL;
        int y = clamp(qb.y - 1 + r, 0, hhi.y) - ob.y;
        float a = 0.0;
        for (int k = -GP_R; k <= GP_R; k++)
            a += gp_w(k) * shor[(y + k) * HL + c];
        slow[i] = a;
    }
}
)GLSL";

		const char* const MEASURE_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 hsz = (sz + ivec2(1)) / 2;
ivec2 hhi = hsz - ivec2(1);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 origin = blk * BLK;
ivec2 qb = blk * HB;
load_low(qb, hi, hhi);
load_block(origin, hi);
barrier();
// The bands' energies over this invocation's 2 x 2 pixels.
vec4 acc = vec4(0.0);   // D1^2, D2^2, pixels, untrusted pixels
for (int q = 0; q < 4; q++) {
    ivec2 o = lid * 2 + ivec2(q & 1, q >> 1);
    ivec2 p = origin + o;
    if (p.x < sz.x && p.y < sz.y) {
        float g1 = fine_at(o);
        float d1 = sy[(o.y + R1) * SW + o.x + R1] - g1;
        float d2 = g1 - low_at(o);
        acc += vec4(d1 * d1, d2 * d2, 1.0, 0.0);
    }
}
#ifdef FIELD
// The de-judder's block under the centre of this invocation's 2 x 2 cell (by its pixels).
if (acc.z > 0.0) {
    vec2 lp = vec2(origin + lid * 2 + ivec2(1)) * to_field;
    ivec2 fs = textureSize(field, 0);
    vec4 f = texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fs - ivec2(1)), 0);
    if (f.z <= trust_min)
        acc.w = acc.z;
}
#endif
// The low band over the half block: mean and spread (about the block's centre texel).
ivec2 hl = ivec2(int(li % 16u), int(li / 16u));
ivec2 hq = qb + hl;
float shift = slow[9 * HL + 9];
vec4 hacc = vec4(0.0);
if (hq.x < hsz.x && hq.y < hsz.y) {
    float l = slow[(hl.y + 1) * HL + hl.x + 1];
    hacc = vec4(l - shift, (l - shift) * (l - shift), 1.0, 0.0);
}
vec4 tot = reduce4(acc);
vec4 htot = reduce4(hacc);
if (li == 0u) {
    float n = max(tot.z, 1.0);
    float hn = max(htot.z, 1.0);
    float hm = htot.x / hn;
    float mean_l = shift + hm;
    float spread = sqrt(max(htot.y / hn - hm * hm, 0.0));
    imageStore(stats_out, blk, vec4(tot.x / n, tot.y / n, mean_l, spread));
    float u = tot.w / n;
    float change = 0.0;
    float w = 1.0;
    float measurable = 1.0;
#ifdef PREV
    vec4 pv = texelFetch(prev_stats, blk, 0);
    change = max(abs(mean_l - pv.z), abs(spread - pv.w));
    // A tile whose band energy jumped (either way) since the previous frame shows new content
    // (an edge moving in, a flash): left out of the frame's energy.
    float en = (tot.x + tot.y) / n, ep = pv.x + pv.y;
    if (en > energy_jump * ep || ep > energy_jump * en)
        measurable = 0.0;
#endif
#ifdef FIELD
    w = 1.0 - smoothstep(0.0, untrusted_share, u);
#elif defined(PREV)
    w = 1.0 - smoothstep(veto_codes, 2.0 * veto_codes, change);
#endif
    imageStore(weight_out, blk, vec4(w, u, change, measurable));
}
)GLSL";

		// One workgroup: the frame from the tiles, then (thread 0) the decision.
		const char* const REDUCE_HEADER = R"GLSL(
shared float rd[1024];
vec4 reduce4(vec4 v)
{
    uint li = gl_LocalInvocationIndex;
    barrier();
    rd[li * 4u] = v.x;
    rd[li * 4u + 1u] = v.y;
    rd[li * 4u + 2u] = v.z;
    rd[li * 4u + 3u] = v.w;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) {
            rd[li * 4u] += rd[(li + s) * 4u];
            rd[li * 4u + 1u] += rd[(li + s) * 4u + 1u];
            rd[li * 4u + 2u] += rd[(li + s) * 4u + 2u];
            rd[li * 4u + 3u] += rd[(li + s) * 4u + 3u];
        }
        barrier();
    }
    return vec4(rd[0], rd[1], rd[2], rd[3]);
}
float gain_law(float r, float e)
{
    if (!(e > 0.0))
        return 1.0;
    float q = r / e;
    if (q <= 1.0 + dead_zone)
        return 1.0;
    return clamp(sqrt(q / (1.0 + dead_zone)), 1.0, cap);
}
)GLSL";

		const char* const REDUCE_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 nt = textureSize(stats_in, 0);
int n = nt.x * nt.y;
vec4 a1 = vec4(0.0), a2 = vec4(0.0);
for (int i = int(li); i < n; i += 256) {
    ivec2 t = ivec2(i % nt.x, i / nt.x);
    vec4 s = texelFetch(stats_in, t, 0);
    vec4 b = texelFetch(weight_in, t, 0);
    if (s.x + s.y >= floor_energy && b.x >= 0.5 && b.w > 0.5)
        a1 += vec4(s.x, s.y, 1.0, 0.0);
    a2 += vec4(s.z, b.z > cut_codes ? 1.0 : 0.0, b.y > 0.5 ? 1.0 : 0.0, 1.0);
}
vec4 t1 = reduce4(a1);
vec4 t2 = reduce4(a2);
float m1 = t1.x / max(t1.z, 1.0), m2 = t1.y / max(t1.z, 1.0);
vec4 a3 = vec4(0.0);
for (int i = int(li); i < n; i += 256) {
    ivec2 t = ivec2(i % nt.x, i / nt.x);
    vec4 s = texelFetch(stats_in, t, 0);
    vec4 b = texelFetch(weight_in, t, 0);
    if (s.x + s.y >= floor_energy && b.x >= 0.5 && b.w > 0.5)
        a3 += vec4(min(s.x, winsorise * m1), min(s.y, winsorise * m2), 0.0, 0.0);
}
vec4 t3 = reduce4(a3);
if (li == 0u) {
    bool measured = t1.z > 0.0;
    float e1 = measured ? t3.x / t1.z : 0.0;
    float e2 = measured ? t3.y / t1.z : 0.0;
    float my = t2.x / max(t2.w, 1.0);
    float cut_share = t2.y / max(t2.w, 1.0);
    float u_share = t2.z / max(t2.w, 1.0);
#ifdef AHEAD
    imageStore(ring, ivec2(RING + 1, 0), vec4(e1, e2, my, 1.0));
#else
    bool cut = n_valid > 0 && cut_share > cut_share_min;
#ifdef FIELD
    cut = cut && u_share > 0.5;
#endif
    bool fade = false;
    if (n_valid >= 2) {
        float p1 = imageLoad(ring, ivec2((cur + RING - 1) % RING, 0)).z;
        float p2 = imageLoad(ring, ivec2((cur + RING - 2) % RING, 0)).z;
        float d0 = my - p1, d1 = p1 - p2;
        fade = abs(d0) >= fade_codes && abs(d1) >= fade_codes && d0 * d1 > 0.0;
    }
    bool restart = n_valid == 0 || cut || fade;
    vec2 e = vec2(e1, e2);
    vec2 r = e;
    if (!restart) {
        float dec = 1.0;
        for (int j = 1; j <= n_use; j++) {
            vec4 s = imageLoad(ring, ivec2((cur + RING - j) % RING, 0));
            dec *= decay;
            if (s.w < 0.5)
                break;
            r = max(r, s.xy * dec);
            if (s.w > 1.5)
                break;
        }
        if (ahead_valid > 0) {
            vec4 a = imageLoad(ring, ivec2(RING + 1, 0));
            float ra = (a.x + a.y) / max(e1 + e2, 1e-20);
            if (abs(a.z - my) <= cut_codes && ra <= ahead_ratio && ra * ahead_ratio >= 1.0)
                r = max(r, a.xy * decay);
        }
    }
    vec2 g = vec2(1.0);
    if (!restart && measured)
        g = vec2(gain_law(r.x, e.x), gain_law(r.y, e.y));
    imageStore(ring, ivec2(cur, 0), vec4(e1, e2, my, restart ? 2.0 : 1.0));
    imageStore(ring, ivec2(RING, 0), vec4(g, restart ? 1.0 : 0.0, cut_share));
#endif
}
)GLSL";

		const char* const APPLY_HEADER = R"GLSL(
shared float stile[9];
)GLSL";

		const char* const APPLY_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 hhi = (sz + ivec2(1)) / 2 - ivec2(1);
ivec2 nt = textureSize(weight_in, 0);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 origin = blk * BLK;
ivec2 qb = blk * HB;
#ifndef XGEO
if (li < 9u)
    stile[li] = texelFetch(weight_in, clamp(blk - ivec2(1) + ivec2(int(li % 3u), int(li / 3u)), ivec2(0), nt - ivec2(1)), 0).x;
#endif
// This picture's own bands (hunt 3 F6): its low band, then its codes and the fine band.
load_low(qb, hi, hhi);
load_block(origin, hi);
barrier();
vec2 gm = texelFetch(ring, ivec2(RING, 0), 0).xy - vec2(1.0);
for (int q = 0; q < 4; q++) {
    ivec2 o = lid * 2 + ivec2(q & 1, q >> 1);
    ivec2 p = origin + o;
    if (p.x < sz.x && p.y < sz.y) {
        float g1 = fine_at(o);
        float d1 = sy[(o.y + R1) * SW + o.x + R1] - g1;
        float d2 = g1 - low_at(o);
#ifdef XGEO
        // Another geometry: the decided tile grid at the same picture position (decided plane
        // px = this plane px x tile_ratio), bilinear between tile centres (edges clamped).
        vec2 tp = (vec2(p) + 0.5) * tile_ratio / float(BLK) - 0.5;
        ivec2 t0 = ivec2(floor(tp));
        vec2 tf = tp - vec2(t0);
        ivec2 a = clamp(t0, ivec2(0), nt - ivec2(1));
        ivec2 b = clamp(t0 + ivec2(1), ivec2(0), nt - ivec2(1));
        float top = texelFetch(weight_in, a, 0).x * (1.0 - tf.x) + texelFetch(weight_in, ivec2(b.x, a.y), 0).x * tf.x;
        float bottom = texelFetch(weight_in, ivec2(a.x, b.y), 0).x * (1.0 - tf.x) + texelFetch(weight_in, b, 0).x * tf.x;
#else
        // The tile weight, bilinear between tile centres (edges clamped).
        vec2 tp = (vec2(p) + 0.5) / float(BLK) - 0.5;
        ivec2 t0 = ivec2(floor(tp));
        vec2 tf = tp - vec2(t0);
        ivec2 a = clamp(t0, ivec2(0), nt - ivec2(1)) - blk + ivec2(1);
        ivec2 b = clamp(t0 + ivec2(1), ivec2(0), nt - ivec2(1)) - blk + ivec2(1);
        float top = stile[a.y * 3 + a.x] * (1.0 - tf.x) + stile[a.y * 3 + b.x] * tf.x;
        float bottom = stile[b.y * 3 + a.x] * (1.0 - tf.x) + stile[b.y * 3 + b.x] * tf.x;
#endif
        float w = top * (1.0 - tf.y) + bottom * tf.y;
        float delta = w * (gm.x * d1 + gm.y * d2);
        // Added to the source sample itself: a gain of 1 gives it back exactly.
#ifdef DP_RGB
        // The same delta on R, G and B: the luma moves by it, Cb and Cr stay.
        vec4 t = texelFetch(src, p, 0);
        imageStore(dst, p, vec4(t.rgb + vec3(delta / luma_mul), t.a));
#else
        imageStore(dst, p, vec4(texelFetch(src, p, 0).r + delta / code_scale, 0.0, 0.0, 1.0));
#endif
    }
}
)GLSL";

		// ---- detail_pulse_fast (2026-10-18): the same maths, fewer reads and less repeated work ----
		//  - The low band has its own pass at half resolution (32 x 32 texels per workgroup, so
		//    its Gaussian's border is read 1.7 times per texel instead of 3), once per picture into
		//    an fp32 texture; the measure and apply passes read their 18 x 18 texels back. The
		//    value at a position is computed exactly as before (the 2x2 box at the clamped half
		//    position, the separable Gaussian at clamped positions, the same order of sums).
		//  - The plane's codes come in 2 x 2 quads by textureGather (one read for four codes;
		//    clamp-to-edge addressing = code_at's clamped positions).
		//  - The filters run register-blocked with literal weights (a thread computes 8 adjacent
		//    horizontal or 4 adjacent vertical outputs from shared memory with odd row pitches),
		//    each output's sum in the original order.
		//  - The measure pass's sums: three strided steps instead of two 9-step trees (another
		//    fp32 summation order: the tile energies move by ~1e-7 relative).
		//  - An apply block whose gains are exactly 1 or whose tile weights are all exactly 0 is
		//    a copy of the source (the delta is exactly 0 there): dead zone, restart, untrusted.
		// Shared memory: one array carved by offsets; the reduction scratch reuses the codes'.
		constexpr int kLowTile = 32;   // LT in the low pass
		const char* const QUAD_FUNCS = R"GLSL(
float code_at(ivec2 p)
{
#ifdef DP_RGB
    vec4 t = texelFetch(src, p, 0);
    return luma_mul * dot(t.rgb, luma_w) + luma_add;
#else
    return code_scale * texelFetch(src, p, 0).r;
#endif
}

// The codes of the 2 x 2 texels from p: (0,0), (1,0), (0,1), (1,1).
vec4 quad_codes(ivec2 p, vec2 isz)
{
#ifdef GATHER
    vec2 uv = vec2(p + ivec2(1)) * isz;
#ifdef DP_RGB
    vec4 r = textureGather(src, uv, 0);
    vec4 g = textureGather(src, uv, 1);
    vec4 b = textureGather(src, uv, 2);
    // textureGather: x (0,1), y (1,1), z (1,0), w (0,0)
    return vec4(luma_mul * dot(vec3(r.w, g.w, b.w), luma_w) + luma_add,
                luma_mul * dot(vec3(r.z, g.z, b.z), luma_w) + luma_add,
                luma_mul * dot(vec3(r.x, g.x, b.x), luma_w) + luma_add,
                luma_mul * dot(vec3(r.y, g.y, b.y), luma_w) + luma_add);
#else
    vec4 t = textureGather(src, uv, 0);
    return vec4(code_scale * t.w, code_scale * t.z, code_scale * t.x, code_scale * t.y);
#endif
#else
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    return vec4(code_at(clamp(p, ivec2(0), hi)), code_at(clamp(p + ivec2(1, 0), ivec2(0), hi)),
                code_at(clamp(p + ivec2(0, 1), ivec2(0), hi)), code_at(clamp(p + ivec2(1, 1), ivec2(0), hi)));
#endif
}
)GLSL";

		// The low band pass: LT x LT half-resolution texels per workgroup.
		const char* const LOW_HEADER_FAST = R"GLSL(
#define LT 32
#define LR (LT + 2 * GP_R)
// Odd row pitches (a warp reading down a column hits 32 banks).
#define LRP (LR + 1)
#define LTP (LT + 1)
#define O_HOR (LR * LRP)
shared float sm[LR * LRP + LR * LTP];
)GLSL";

		const char* const LOW_BODY_FAST = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 sz = textureSize(src, 0);
vec2 isz = vec2(1.0) / vec2(sz);
ivec2 hsz = (sz + ivec2(1)) / 2;
ivec2 hhi = hsz - ivec2(1);
ivec2 q0 = ivec2(gl_WorkGroupID.xy) * LT;
ivec2 ob = q0 - ivec2(GP_R);
// The 2x2 boxes at the clamped half-resolution positions (GrainModel::Low).
for (int i = int(li); i < LR * LR; i += 256) {
    vec4 q = quad_codes(clamp(ob + ivec2(i % LR, i / LR), ivec2(0), hhi) * 2, isz);
    sm[(i / LR) * LRP + i % LR] = 0.25 * (q.x + q.y + q.z + q.w);
}
barrier();
// The horizontal Gaussian on every box row, at this block's columns: a thread per row and 8
// columns (LR * LT / 8 <= 256 items).
if (int(li) < LR * (LT / 8)) {
    int r = int(li) % LR, g = int(li) / LR;
    gp_across8(r * LRP + 8 * g, O_HOR + r * LTP + 8 * g);
}
barrier();
// The vertical Gaussian (the low band): a thread per column and 4 rows.
{
    int c = int(li % 32u), r0 = 4 * int(li / 32u);
    vec4 l = gp_down4(O_HOR + r0 * LTP + c);
    for (int k = 0; k < 4; k++) {
        ivec2 p = q0 + ivec2(c, r0 + k);
        if (p.x < hsz.x && p.y < hsz.y)
            imageStore(low_out, p, vec4(l[k], 0.0, 0.0, 1.0));
    }
}
)GLSL";

		const char* const BAND_DECL_FAST = R"GLSL(
#define BLK 32
#define HB 16
#define SW (BLK + 2 * R1)
#define HL (HB + 2)
// Odd row pitches: a warp reading down a column (one row per lane) hits 32 banks.
#define SWP (SW + 1)
#define BLKP (BLK + 1)
#define O_SY 0
#define O_SH (O_SY + SW * SWP)
#define O_SLOW (O_SH + SW * BLKP)
shared float sm[O_SLOW + HL * HL];
)GLSL";

		const char* const BAND_FUNCS_FAST = R"GLSL(
// The low band around the half block qb (and a one-texel border), from the low pass.
void low_fetch(ivec2 qb, ivec2 hhi, int n)
{
    for (int i = int(gl_LocalInvocationIndex); i < n; i += 256)
        sm[O_SLOW + i] = texelFetch(low_in, clamp(qb - ivec2(1) + ivec2(i % HL, i / HL), ivec2(0), hhi), 0).r;
}
// The block's codes by quads.
void block_codes(ivec2 origin, vec2 isz, int n)
{
    for (int i = int(gl_LocalInvocationIndex); i < n; i += 256) {
        int gx = i % (SW / 2), gy = i / (SW / 2);
        vec4 q = quad_codes(origin - ivec2(R1) + 2 * ivec2(gx, gy), isz);
        int b = O_SY + 2 * gy * SWP + 2 * gx;
        sm[b] = q.x;
        sm[b + 1] = q.y;
        sm[b + SWP] = q.z;
        sm[b + SWP + 1] = q.w;
    }
}
// Their horizontal fine half: a thread per row and 8 columns (n = SW * BLK / 8 <= 256, or 0).
void block_across(int n)
{
    int i = int(gl_LocalInvocationIndex);
    if (i < n) {
        int r = i % SW, g = i / SW;
        across8(O_SY + r * SWP + 8 * g, O_SH + r * BLKP + 8 * g);
    }
}
// The bands of this thread's 4 pixels: column x, rows y0 .. y0 + 3 (y0 a multiple of 4). The
// same arithmetic per pixel as before (fine: the vertical sum in the same order; low: the
// bilinear read-back with its weights 0.75 / 0.25), the shared loads held in registers.
void bands4(int x, int y0, out vec4 d1, out vec4 d2)
{
    vec4 g1 = fine4(O_SH + y0 * BLKP + x);
    float gx = (float(x) + 0.5) * 0.5 - 0.5;
    int ix = int(floor(gx));
    float fx = gx - float(ix);
    int b = O_SLOW + (y0 / 2) * HL + ix + 1;
    float h0 = sm[b] * (1.0 - fx) + sm[b + 1] * fx;
    float h1 = sm[b + HL] * (1.0 - fx) + sm[b + HL + 1] * fx;
    float h2 = sm[b + 2 * HL] * (1.0 - fx) + sm[b + 2 * HL + 1] * fx;
    float h3 = sm[b + 3 * HL] * (1.0 - fx) + sm[b + 3 * HL + 1] * fx;
    vec4 low = vec4(h0 * (1.0 - 0.75) + h1 * 0.75, h1 * (1.0 - 0.25) + h2 * 0.25,
                    h1 * (1.0 - 0.75) + h2 * 0.75, h2 * (1.0 - 0.25) + h3 * 0.25);
    int c = O_SY + (y0 + R1) * SWP + x + R1;
    vec4 y = vec4(sm[c], sm[c + SWP], sm[c + 2 * SWP], sm[c + 3 * SWP]);
    d1 = y - g1;
    d2 = g1 - low;
}
)GLSL";

		const char* const FAST_MEASURE_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 sz = textureSize(src, 0);
vec2 isz = vec2(1.0) / vec2(sz);
ivec2 hsz = (sz + ivec2(1)) / 2;
ivec2 hhi = hsz - ivec2(1);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 origin = blk * BLK;
ivec2 qb = blk * HB;
low_fetch(qb, hhi, HL * HL);
block_codes(origin, isz, (SW / 2) * (SW / 2));
barrier();
block_across(SW * (BLK / 8));
barrier();
// A warp reads 32 px rows of the block (no shared-memory bank conflicts), each thread a column
// of 4 rows.
vec4 acc = vec4(0.0);   // D1^2, D2^2, pixels, untrusted pixels
#ifdef FIELD
ivec2 fs = textureSize(field, 0);
#endif
int px = int(li % 32u), py0 = 4 * int(li / 32u);
vec4 d1s, d2s;
bands4(px, py0, d1s, d2s);
for (int q = 0; q < 4; q++) {
    ivec2 o = ivec2(px, py0 + q);
    ivec2 p = origin + o;
    if (p.x < sz.x && p.y < sz.y) {
        float d1 = d1s[q];
        float d2 = d2s[q];
        float untrusted = 0.0;
#ifdef FIELD
        // The de-judder's block under the centre of the pixel's 2 x 2 cell (as before).
        vec2 lp = vec2(origin + (o / 2) * 2 + ivec2(1)) * to_field;
        if (texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fs - ivec2(1)), 0).z <= trust_min)
            untrusted = 1.0;
#endif
        acc += vec4(d1 * d1, d2 * d2, 1.0, untrusted);
    }
}
ivec2 hl = ivec2(int(li % 16u), int(li / 16u));
ivec2 hq = qb + hl;
float shift = sm[O_SLOW + 9 * HL + 9];
vec4 hacc = vec4(0.0);
if (hq.x < hsz.x && hq.y < hsz.y) {
    float l = sm[O_SLOW + (hl.y + 1) * HL + hl.x + 1];
    hacc = vec4(l - shift, (l - shift) * (l - shift), 1.0, 0.0);
}
// The sums over the workgroup: 5 values (d1^2, d2^2, untrusted, low - shift, its square) in
// three strided steps (3 barriers instead of a 9-step tree); the pixel counts are the tile's
// own sizes. fp32 sums in another order than the original tree's (well inside fp32 for 1024
// terms; the gains differ by ~1e-7 at most).
barrier();
sm[li] = acc.x;
sm[256u + li] = acc.y;
sm[512u + li] = acc.w;
sm[768u + li] = hacc.x;
sm[1024u + li] = hacc.y;
barrier();
if (li < 160u) {
    uint b = (li / 32u) * 256u + li % 32u;
    float a = 0.0;
    for (uint k = 0u; k < 256u; k += 32u)
        a += sm[b + k];
    sm[1280u + li] = a;
}
barrier();
if (li < 20u) {
    uint b = 1280u + (li / 4u) * 32u + li % 4u;
    float a = 0.0;
    for (uint k = 0u; k < 32u; k += 4u)
        a += sm[b + k];
    sm[1440u + li] = a;
}
barrier();
if (li == 0u) {
    vec4 tot = vec4(0.0);
    vec3 htot = vec3(0.0);
    tot.x = sm[1440] + sm[1441] + sm[1442] + sm[1443];
    tot.y = sm[1444] + sm[1445] + sm[1446] + sm[1447];
    tot.w = sm[1448] + sm[1449] + sm[1450] + sm[1451];
    htot.x = sm[1452] + sm[1453] + sm[1454] + sm[1455];
    htot.y = sm[1456] + sm[1457] + sm[1458] + sm[1459];
    ivec2 tn = min(ivec2(BLK), sz - origin);
    ivec2 hn2 = min(ivec2(HB), hsz - qb);
    float n = float(tn.x * tn.y);
    float hn = float(hn2.x * hn2.y);
    float hm = htot.x / hn;
    float mean_l = shift + hm;
    float spread = sqrt(max(htot.y / hn - hm * hm, 0.0));
    imageStore(stats_out, blk, vec4(tot.x / n, tot.y / n, mean_l, spread));
    float u = tot.w / n;
    float change = 0.0;
    float w = 1.0;
    float measurable = 1.0;
#ifdef PREV
    vec4 pv = texelFetch(prev_stats, blk, 0);
    change = max(abs(mean_l - pv.z), abs(spread - pv.w));
    float en = (tot.x + tot.y) / n, ep = pv.x + pv.y;
    if (en > energy_jump * ep || ep > energy_jump * en)
        measurable = 0.0;
#endif
#ifdef FIELD
    w = 1.0 - smoothstep(0.0, untrusted_share, u);
#elif defined(PREV)
    w = 1.0 - smoothstep(veto_codes, 2.0 * veto_codes, change);
#endif
    imageStore(weight_out, blk, vec4(w, u, change, measurable));
}
)GLSL";

		const char* const FAST_APPLY_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 sz = textureSize(src, 0);
vec2 isz = vec2(1.0) / vec2(sz);
ivec2 hhi = (sz + ivec2(1)) / 2 - ivec2(1);
ivec2 nt = textureSize(weight_in, 0);
ivec2 blk = ivec2(gl_WorkGroupID.xy);
ivec2 origin = blk * BLK;
ivec2 qb = blk * HB;
#ifndef XGEO
if (li < 9u)
    stile[li] = texelFetch(weight_in, clamp(blk - ivec2(1) + ivec2(int(li % 3u), int(li / 3u)), ivec2(0), nt - ivec2(1)), 0).x;
#endif
vec2 gm = texelFetch(ring, ivec2(RING, 0), 0).xy - vec2(1.0);
barrier();
// Gains of exactly 1, or a weight of exactly 0 on every tile this block reads: the delta is
// exactly 0 on every pixel, the block is the source (bit for bit).
bool work = gm.x != 0.0 || gm.y != 0.0;
#ifndef XGEO
float wmax = 0.0;
for (int i = 0; i < 9; i++)
    wmax = max(wmax, stile[i]);
work = work && wmax > 0.0;
#endif
low_fetch(qb, hhi, work ? HL * HL : 0);
block_codes(origin, isz, work ? (SW / 2) * (SW / 2) : 0);
barrier();
block_across(work ? SW * (BLK / 8) : 0);
barrier();
int px = int(li % 32u), py0 = 4 * int(li / 32u);
vec4 d1s = vec4(0.0), d2s = vec4(0.0);
if (work)
    bands4(px, py0, d1s, d2s);
for (int q = 0; q < 4; q++) {
    ivec2 o = ivec2(px, py0 + q);
    ivec2 p = origin + o;
    if (p.x < sz.x && p.y < sz.y) {
        if (work) {
            float d1 = d1s[q];
            float d2 = d2s[q];
#ifdef XGEO
            vec2 tp = (vec2(p) + 0.5) * tile_ratio / float(BLK) - 0.5;
            ivec2 t0 = ivec2(floor(tp));
            vec2 tf = tp - vec2(t0);
            ivec2 a = clamp(t0, ivec2(0), nt - ivec2(1));
            ivec2 b = clamp(t0 + ivec2(1), ivec2(0), nt - ivec2(1));
            float top = texelFetch(weight_in, a, 0).x * (1.0 - tf.x) + texelFetch(weight_in, ivec2(b.x, a.y), 0).x * tf.x;
            float bottom = texelFetch(weight_in, ivec2(a.x, b.y), 0).x * (1.0 - tf.x) + texelFetch(weight_in, b, 0).x * tf.x;
#else
            vec2 tp = (vec2(p) + 0.5) / float(BLK) - 0.5;
            ivec2 t0 = ivec2(floor(tp));
            vec2 tf = tp - vec2(t0);
            ivec2 a = clamp(t0, ivec2(0), nt - ivec2(1)) - blk + ivec2(1);
            ivec2 b = clamp(t0 + ivec2(1), ivec2(0), nt - ivec2(1)) - blk + ivec2(1);
            float top = stile[a.y * 3 + a.x] * (1.0 - tf.x) + stile[a.y * 3 + b.x] * tf.x;
            float bottom = stile[b.y * 3 + a.x] * (1.0 - tf.x) + stile[b.y * 3 + b.x] * tf.x;
#endif
            float w = top * (1.0 - tf.y) + bottom * tf.y;
            float delta = w * (gm.x * d1 + gm.y * d2);
#ifdef DP_RGB
            vec4 t = texelFetch(src, p, 0);
            imageStore(dst, p, vec4(t.rgb + vec3(delta / luma_mul), t.a));
#else
            imageStore(dst, p, vec4(texelFetch(src, p, 0).r + delta / code_scale, 0.0, 0.0, 1.0));
#endif
        } else {
#ifdef DP_RGB
            imageStore(dst, p, texelFetch(src, p, 0));
#else
            imageStore(dst, p, vec4(texelFetch(src, p, 0).r, 0.0, 0.0, 1.0));
#endif
        }
    }
}
)GLSL";

		double LowWeightAt(int i, double sigma) { return GrainPreserve::LowWeight(i, sigma); }

		// A blocked convolution: `outputs` sums, consecutive along `stride`, each in the
		// sum-of-w(k)-x(k) order of fw_conv / gp_conv, from outputs + 2 radius loads. Stored to
		// sm[o + j] (store) or returned as a vec4 (4 outputs).
		std::string BlockedConvGlsl(const char* name, int radius, double (*weight)(int, double), double param,
			int outputs, const char* stride, bool store)
		{
			std::string s = store ? std::string("void ") + name + "(int b, int o)\n{\n" : std::string("vec4 ") + name + "(int b)\n{\n";
			for (int i = 0; i < outputs + 2 * radius; ++i)
				s += "    float v" + std::to_string(i) + " = sm[b + " + std::to_string(i) + " * " + stride + "];\n";
			s += store ? "    float a;\n" : "    vec4 g;\n    float a;\n";
			const char* comp = "xyzw";
			for (int j = 0; j < outputs; ++j)
			{
				s += "    a = 0.0;\n";
				for (int k = -radius; k <= radius; ++k)
				{
					char line[96];
					snprintf(line, sizeof(line), "    a += %.9g * v%d;\n", weight(k, param), j + k + radius);
					s += line;
				}
				s += store ? "    sm[o + " + std::to_string(j) + "] = a;\n" : std::string("    g.") + comp[j] + " = a;\n";
			}
			return s + (store ? "}\n" : "    return g;\n}\n");
		}

		std::string WeightsGlsl(const char* name, const char* radiusName, int radius, double (*weight)(int, double), double scale)
		{
			const std::string taps = std::to_string(2 * radius + 1);
			std::string s = std::string("#define ") + radiusName + " " + std::to_string(radius) + "\nfloat " + name + "(int i)\n{\n    float w[" + taps + "] = float[" + taps + "](";
			for (int i = -radius; i <= radius; ++i)
			{
				char number[32];
				snprintf(number, sizeof(number), "%.9g", weight(i, scale));
				s += number;
				if (i < radius)
					s += ", ";
			}
			return s + ");\n    return w[i + " + radiusName + "];\n}\n";
		}

		pl_shader_desc Sampled(const char* name, pl_tex tex)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = tex;
			d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			return d;
		}

		pl_shader_desc Storage(const char* name, pl_tex tex, enum pl_desc_access access = PL_DESC_ACCESS_WRITEONLY)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = access;
			d.binding.object = tex;
			return d;
		}

		pl_fmt StorableLike(pl_gpu gpu, pl_fmt like)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
			// At least 16 bits (as VpDebander's): the stabilised detail is finer than an 8-bit code.
			if (like && (like->caps & caps) == caps && like->component_depth[0] >= 16) return like;
			const int n = like ? like->num_components : 1;
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_UNORM, n, 16, 16, caps);
			return f ? f : pl_find_fmt(gpu, PL_FMT_FLOAT, n, 16, 16, caps);
		}

		// 32-bit float, `components` or the next wider: the low band in codes needs fp32 (fp16
		// would round it by a quarter of a 10-bit code), and the ring is read-write.
		pl_fmt Float32(pl_gpu gpu, int components)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			for (int n = components; n <= 4; ++n)
				if (pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, n, 32, 32, caps))
					return f;
			return nullptr;
		}

		bool Ensure(pl_gpu gpu, pl_tex& tex, int w, int h, pl_fmt fmt, bool blitSrc = false)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = fmt;
			tp.sampleable = true;
			tp.storable = true;
			tp.blit_src = blitSrc;
			return fmt && pl_tex_recreate(gpu, &tex, &tp);
		}

		// Diagnostics (SetPassTimers): GPU time per pass kind (measure, reduce, apply).
		struct PassTimers
		{
			pl_gpu gpu = nullptr;
			pl_timer timer[4] = {};
			double ns[4] = {};
			int count[4] = {};
			void Drain()
			{
				for (int i = 0; i < 4; ++i)
					if (timer[i])
						for (uint64_t t; (t = pl_timer_query(gpu, timer[i])) != 0;) { ns[i] += static_cast<double>(t); ++count[i]; }
			}
		};
		PassTimers g_timers;

		bool Compute(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
			const pl_shader_desc* desc, int numDesc, const pl_shader_var* vars, int numVars, size_t shmem, int groupsX, int groupsY,
			int pass = -1)
		{
			pl_custom_shader custom{};
			custom.description = description;
			custom.header = header.c_str();
			custom.body = body;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc;
			custom.num_descriptors = numDesc;
			custom.variables = vars;
			custom.num_variables = numVars;
			custom.compute = true;
			custom.compute_shmem = shmem;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = groupsX;
			compute.dispatch_size[1] = groupsY;
			compute.dispatch_size[2] = 1;
			if (pass >= 0 && g_timers.gpu)
			{
				g_timers.Drain();
				compute.timer = g_timers.timer[pass];
			}
			return pl_dispatch_compute(dispatch, &compute);
		}

		// The plane's geometry and band layout (the history belongs to one).
		struct Geometry
		{
			int w = 0, h = 0;
			double scale = 0.0;
			bool operator==(const Geometry& o) const { return w == o.w && h == o.h && scale == o.scale; }
		};


		struct Constants
		{
			Geometry geometry;
			int r1 = 0, gpR = 0;
			int tilesX = 0, tilesY = 0;
			std::string bandHeader;
			size_t bandShmem = 0;
			// detail_pulse_fast: its header (textureGather when the plane's format allows it) and
			// shared memory with the low band computed / read back.
			std::string fastHeader, lowHeader;
			size_t fastShmem = 0, lowShmem = 0;
			int halfW = 0, halfH = 0, lowTilesX = 0, lowTilesY = 0;
			float codeScale = 1.0f;
			// Hunt 3 F8 / F9: a one-plane RGB picture (code = lumaMul x dot(rgb, lumaW) + lumaAdd).
			bool rgb = false;
			float lumaMul = 1.0f, lumaAdd = 0.0f;
			float lumaW[3] = { 0.0f, 0.0f, 0.0f };
		};
	}

	struct Module::Impl
	{
		pl_tex out = nullptr;            // the stabilised plane (a new frame or a repeat)
		pl_tex outForeign = nullptr;     // ApplyForeign's (the crossfade's path being left)
		pl_tex stats[2] = {};            // per tile: E1, E2, low mean, low spread (this frame's and the previous one's)
		pl_tex weight = nullptr;         // per tile: veto weight, untrusted share, change
		pl_tex aheadStats = nullptr, aheadWeight = nullptr;
		pl_tex ring = nullptr;           // kRingSlots entries, then the decision, then the look-ahead
		pl_tex low = nullptr;            // detail_pulse_fast: the last picture's half-resolution low band (fp32 scratch)
		pl_tex lowForeign = nullptr;     // ApplyForeign's
		int statsIndex = 0;              // stats[statsIndex] = lastSequence's
		int cur = 0;                     // ring slot of lastSequence
		int behind = 0;                  // consecutive frames in the ring behind lastSequence
		bool decided = false;            // the ring's decision is lastSequence's
		uint64_t lastSequence = 0;
		Geometry geometry;
		uint64_t aheadSequence = 0;
		Geometry aheadGeometry;
		float codeScale = 0.0f;          // the last luma-plane Apply's (MeasureAhead on a bare texture reads the same signal)
	};

	namespace
	{
		// What plane 0 of a frame is to the pulse: 0 a luma plane, 1 a one-plane RGB(A) picture,
		// -1 neither.
		int PlaneKind(const pl_frame& source)
		{
			const pl_plane& p = source.planes[0];
			if (p.components == 1 && p.component_mapping[0] == PL_CHANNEL_Y)
				return 0;
			if (source.num_planes == 1 && p.components >= 3 && p.component_mapping[0] == PL_CHANNEL_R &&
				p.component_mapping[1] == PL_CHANNEL_G && p.component_mapping[2] == PL_CHANNEL_B &&
				!pl_color_system_is_ycbcr_like(source.repr.sys))
				return 1;
			return -1;
		}

		void Prepare(pl_tex luma, const Params& params, Constants& c, const pl_frame* frame)
		{
			c.geometry.w = luma->params.w;
			c.geometry.h = luma->params.h;
			c.geometry.scale = Scale(params.planeLines > 0.0 ? params.planeLines : luma->params.h);
			c.r1 = FineRadius(c.geometry.scale);
			c.gpR = GrainPreserve::LowRadius(WideSigma(c.geometry.scale));
			c.tilesX = (c.geometry.w + kTilePx - 1) / kTilePx;
			c.tilesY = (c.geometry.h + kTilePx - 1) / kTilePx;
			c.bandHeader = "#define R1 " + std::to_string(c.r1) + "\n" + "#define RING " + std::to_string(kRingSlots) + "\n" +
				WeightsGlsl("fw", "FW_R", c.r1, FineWeight, c.geometry.scale) +
				GrainPreserve::LowWeightsGlsl(WideSigma(c.geometry.scale)) + BAND_HEADER + LOW_HEADER;
			const int sw = kTilePx + 2 * c.r1, hl = kTilePx / 2 + 2, rb = hl + 2 * c.gpR;
			c.bandShmem = sizeof(float) * (sw * sw + sw * kTilePx + hl * hl + 1024 + rb * rb + rb * hl);
			c.halfW = (c.geometry.w + 1) / 2;
			c.halfH = (c.geometry.h + 1) / 2;
			// detail_pulse_fast: the band passes' and the low pass's headers (textureGather when
			// the plane's format allows it, else four clamped fetches).
			const std::string gather = luma->params.format->gatherable ? "#define GATHER 1\n" : "";
			c.fastHeader = "#define R1 " + std::to_string(c.r1) + "\n#define RING " + std::to_string(kRingSlots) + "\n" + gather +
				BAND_DECL_FAST + BlockedConvGlsl("across8", c.r1, FineWeight, c.geometry.scale, 8, "1", true) +
				BlockedConvGlsl("fine4", c.r1, FineWeight, c.geometry.scale, 4, "BLKP", false) +
				QUAD_FUNCS + BAND_FUNCS_FAST;
			c.lowHeader = "#define GP_R " + std::to_string(c.gpR) + "\n" + gather + LOW_HEADER_FAST +
				BlockedConvGlsl("gp_across8", c.gpR, LowWeightAt, WideSigma(c.geometry.scale), 8, "1", true) +
				BlockedConvGlsl("gp_down4", c.gpR, LowWeightAt, WideSigma(c.geometry.scale), 4, "LTP", false) + QUAD_FUNCS;
			c.fastShmem = sizeof(float) * (sw * (sw + 1) + sw * (kTilePx + 1) + hl * hl + 9);
			const int lr = kLowTile + 2 * c.gpR;
			c.lowShmem = sizeof(float) * (lr * (lr + 1) + lr * (kLowTile + 1));
			c.lowTilesX = (c.halfW + kLowTile - 1) / kLowTile;
			c.lowTilesY = (c.halfH + kLowTile - 1) / kLowTile;
			// One code of the signal in the plane texture's normalised units (VpDebander's).
			const int sampleDepth = frame && frame->repr.bits.sample_depth > 0 ? frame->repr.bits.sample_depth : luma->params.format->component_depth[0];
			const int shift = frame ? frame->repr.bits.bit_shift : 0;
			const bool isFloat = luma->params.format->type == PL_FMT_FLOAT;
			const double codeUnit = isFloat ?
				1.0 / (std::pow(2.0, params.colorDepth) - 1.0) :
				std::pow(2.0, shift) / (std::pow(2.0, sampleDepth) - 1.0);
			c.codeScale = static_cast<float>(1.0 / codeUnit);
			c.rgb = frame && PlaneKind(*frame) == 1;
			if (c.rgb)
			{
				double k[3];
				LumaWeights(params.lumaSystem, frame->color.primaries, k);
				for (int i = 0; i < 3; ++i) c.lumaW[i] = static_cast<float>(k[i]);
				if (isFloat)
				{
					// A float picture holds normalised decoded values: back to the source's codes
					// (a composed picture reads the codes of the planes it was made from).
					enum pl_color_levels levels = params.codeLevels;
					if (levels == PL_COLOR_LEVELS_UNKNOWN)
						levels = pl_color_levels_guess(&frame->repr);
					const double step = std::pow(2.0, params.colorDepth - 8);
					if (levels == PL_COLOR_LEVELS_LIMITED)
					{
						c.lumaMul = static_cast<float>(219.0 * step);
						c.lumaAdd = static_cast<float>(16.0 * step);
					}
					else
					{
						c.lumaMul = static_cast<float>(std::pow(2.0, params.colorDepth) - 1.0);
						c.lumaAdd = 0.0f;
					}
				}
				else
				{
					c.lumaMul = c.codeScale;   // an integer RGB plane: its own codes
					c.lumaAdd = 0.0f;
				}
			}
		}

		std::string Defines(const Constants& c)
		{
			return c.rgb ? "#define DP_RGB 1\n" : "";
		}

		// The plane-reading variables (code_scale, or the RGB luma's).
		int CodeVars(const Constants& c, pl_shader_var* v)
		{
			if (!c.rgb)
			{
				v[0] = { pl_var_float("code_scale"), &c.codeScale, true };
				return 1;
			}
			v[0] = { pl_var_float("luma_mul"), &c.lumaMul, true };
			v[1] = { pl_var_float("luma_add"), &c.lumaAdd, true };
			v[2] = { pl_var_vec3("luma_w"), c.lumaW, true };
			return 3;
		}

		size_t MeasureShmem(const Constants& c)
		{
			return c.bandShmem;
		}

		bool EnsureRing(pl_gpu gpu, pl_tex& ring)
		{
			if (ring) return true;
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
			if (!f) return false;
			std::string zero(sizeof(float) * 4 * (kRingSlots + 2), '\0');
			pl_tex_params tp{};
			tp.w = kRingSlots + 2;
			tp.h = 1;
			tp.format = f;
			tp.sampleable = true;
			tp.storable = true;
			tp.initial_data = zero.data();
			ring = pl_tex_create(gpu, &tp);
			return ring != nullptr;
		}

		// detail_pulse_fast: the picture's half-resolution low band into lowOut (fp32).
		bool LowPass(pl_dispatch dispatch, pl_tex luma, const Constants& c, pl_tex lowOut, bool timed, std::string& error)
		{
			const pl_shader_desc d[2] = { Sampled("src", luma), Storage("low_out", lowOut) };
			pl_shader_var v[3];
			const int nv = CodeVars(c, v);
			if (!Compute(dispatch, "VideoProcessor detail pulse: low band", Defines(c) + c.lowHeader, LOW_BODY_FAST, d, 2, v, nv,
				c.lowShmem, c.lowTilesX, c.lowTilesY, timed ? 3 : -1))
			{
				error = "the low band pass failed";
				return false;
			}
			return true;
		}

		// Measure (pass 1) and reduce (pass 2). ahead: the look-ahead (no previous frame, no
		// field; the ring's look-ahead entry). detail_pulse_fast: the low band pass first, into
		// lowTex (read back by the measure pass and by the apply pass after it).
		bool MeasureAndReduce(pl_dispatch dispatch, pl_tex luma, const Params& params, const Constants& c,
			pl_tex statsOut, pl_tex weightOut, pl_tex prevStats, pl_tex ring,
			bool ahead, int cur, int nValid, int nUse, bool aheadValid, std::string& error, pl_tex lowTex = nullptr)
		{
			const bool fast = params.fast && lowTex;
			if (fast && !LowPass(dispatch, luma, c, lowTex, !ahead, error))
				return false;
			const bool useField = !ahead && params.field != nullptr;
			const bool usePrev = !ahead && prevStats != nullptr;
			const float toField[2] = {
				static_cast<float>(params.fieldScaleX > 0.0 ? params.fieldScaleX : params.fieldScale),
				static_cast<float>(params.fieldScale) };
			const float trustMin = static_cast<float>(kTrustMin);
			const float untrustedShare = static_cast<float>(kVetoUntrustedShare);
			const float vetoCodes = static_cast<float>(VetoCodes(params.bitrateQuality, params.colorDepth));
			const float energyJump = static_cast<float>(kTileEnergyJump);
			{
				pl_shader_desc d[6];
				int nd = 0;
				d[nd++] = Sampled("src", luma);
				d[nd++] = Storage("stats_out", statsOut);
				d[nd++] = Storage("weight_out", weightOut);
				if (fast) d[nd++] = Sampled("low_in", lowTex);
				if (usePrev) d[nd++] = Sampled("prev_stats", prevStats);
				if (useField) d[nd++] = Sampled("field", params.field);
				pl_shader_var v[10];
				int nv = CodeVars(c, v);
				if (usePrev && !useField) v[nv++] = { pl_var_float("veto_codes"), &vetoCodes, true };
				if (usePrev) v[nv++] = { pl_var_float("energy_jump"), &energyJump, true };
				if (useField)
				{
					v[nv++] = { pl_var_vec2("to_field"), toField, true };
					v[nv++] = { pl_var_float("trust_min"), &trustMin, true };
					v[nv++] = { pl_var_float("untrusted_share"), &untrustedShare, true };
				}
				std::string header = Defines(c) + (fast ? c.fastHeader : c.bandHeader);
				if (ahead) header = "#define AHEAD 1\n" + header;
				if (usePrev) header = "#define PREV 1\n" + header;
				if (useField) header = "#define FIELD 1\n" + header;
				if (!Compute(dispatch, ahead ? "VideoProcessor detail pulse: measure (look-ahead)" : "VideoProcessor detail pulse: measure",
					header, fast ? FAST_MEASURE_BODY : MEASURE_BODY, d, nd, v, nv, fast ? c.fastShmem : MeasureShmem(c), c.tilesX, c.tilesY, ahead ? -1 : 0))
				{
					error = "the measure pass failed";
					return false;
				}
			}
			{
				const float floorEnergy = static_cast<float>(FloorEnergy(params.colorDepth));
				const float winsorise = static_cast<float>(kWinsorise);
				const float cutCodes = static_cast<float>(CutCodes(params.bitrateQuality, params.colorDepth));
				const float cutShare = static_cast<float>(kCutShare);
				const float fadeCodes = static_cast<float>(FadeCodes(params.colorDepth));
				const float decay = static_cast<float>(DecayPerFrame(params.sourceHz));
				const float deadZone = static_cast<float>(kDeadZone);
				const float cap = static_cast<float>(ClampCap(params.cap));
				const float aheadRatio = static_cast<float>(kAheadMaxRatio);
				const int aheadOn = aheadValid ? 1 : 0;
				const pl_shader_desc d[3] = { Sampled("stats_in", statsOut), Sampled("weight_in", weightOut),
					Storage("ring", ring, PL_DESC_ACCESS_READWRITE) };
				const pl_shader_var v[14] = {
					{ pl_var_float("floor_energy"), &floorEnergy, true },
					{ pl_var_float("winsorise"), &winsorise, true },
					{ pl_var_float("cut_codes"), &cutCodes, true },
					{ pl_var_float("cut_share_min"), &cutShare, true },
					{ pl_var_float("fade_codes"), &fadeCodes, true },
					{ pl_var_float("decay"), &decay, true },
					{ pl_var_float("dead_zone"), &deadZone, true },
					{ pl_var_float("cap"), &cap, true },
					{ pl_var_float("ahead_ratio"), &aheadRatio, true },
					{ pl_var_int("cur"), &cur, true },
					{ pl_var_int("n_valid"), &nValid, true },
					{ pl_var_int("n_use"), &nUse, true },
					{ pl_var_int("ahead_valid"), &aheadOn, true } };
				std::string header = "#define RING " + std::to_string(kRingSlots) + "\n" + REDUCE_HEADER;
				if (ahead) header = "#define AHEAD 1\n" + header;
				if (useField) header = "#define FIELD 1\n" + header;
				if (!Compute(dispatch, ahead ? "VideoProcessor detail pulse: reduce (look-ahead)" : "VideoProcessor detail pulse: reduce",
					header, REDUCE_BODY, d, 3, v, 13, sizeof(float) * 1024, 1, 1, ahead ? -1 : 1))
				{
					error = "the reduce pass failed";
					return false;
				}
			}
			return true;
		}

		// The apply pass: the held decision on `luma`'s own bands into dst. tileRatio: the decided
		// geometry per this picture's px (null: this picture is the decided geometry).
		// detail_pulse_fast when lowTex is given: the picture's low band read from it, computed
		// into it first when computeLow (a repeat or another geometry; a new frame's measure pass
		// computed it already).
		bool ApplyPass(pl_dispatch dispatch, pl_tex luma, pl_tex dst, pl_tex weight, pl_tex ring, const Constants& c,
			const float* tileRatio, std::string& error, pl_tex lowTex = nullptr, bool computeLow = false)
		{
			const bool fast = lowTex != nullptr;
			if (fast && computeLow && !LowPass(dispatch, luma, c, lowTex, tileRatio == nullptr, error))
				return false;
			pl_shader_desc d[5] = { Sampled("src", luma), Storage("dst", dst), Sampled("weight_in", weight), Sampled("ring", ring) };
			int nd = 4;
			if (fast) d[nd++] = Sampled("low_in", lowTex);
			pl_shader_var v[4];
			int nv = CodeVars(c, v);
			if (tileRatio) v[nv++] = { pl_var_vec2("tile_ratio"), tileRatio, true };
			std::string header = fast ? Defines(c) + APPLY_HEADER + c.fastHeader : Defines(c) + c.bandHeader + APPLY_HEADER;
			if (tileRatio) header = "#define XGEO 1\n" + header;
			const size_t shmem = fast ? c.fastShmem : c.bandShmem + sizeof(float) * 9;
			if (!Compute(dispatch, tileRatio ? "VideoProcessor detail pulse: apply (other geometry)" : "VideoProcessor detail pulse: apply",
				header, fast ? FAST_APPLY_BODY : APPLY_BODY, d, nd, v, nv, shmem, c.tilesX, c.tilesY, tileRatio ? -1 : 2))
			{
				error = "the apply pass failed";
				return false;
			}
			return true;
		}
	}

	void SetPassTimers(pl_gpu gpu, bool on)
	{
		if (g_timers.gpu)
			for (pl_timer& t : g_timers.timer) pl_timer_destroy(g_timers.gpu, &t);
		g_timers = PassTimers();
		if (on && gpu)
		{
			g_timers.gpu = gpu;
			for (pl_timer& t : g_timers.timer) t = pl_timer_create(gpu);
		}
	}

	void PassTimes(double ms[4], bool reset)
	{
		g_timers.Drain();
		for (int i = 0; i < 4; ++i)
			ms[i] = g_timers.count[i] > 0 ? g_timers.ns[i] / g_timers.count[i] / 1e6 : 0.0;
		if (reset)
			for (int i = 0; i < 4; ++i) { g_timers.ns[i] = 0.0; g_timers.count[i] = 0; }
	}

	void LumaWeights(enum pl_color_system system, enum pl_color_primaries primaries, double out[3])
	{
		double kr = 0.2126, kb = 0.0722;
		switch (system)
		{
		case PL_COLOR_SYSTEM_BT_601: kr = 0.299; kb = 0.114; break;
		case PL_COLOR_SYSTEM_BT_2020_NC:
		case PL_COLOR_SYSTEM_BT_2020_C: kr = 0.2627; kb = 0.0593; break;
		case PL_COLOR_SYSTEM_SMPTE_240M: kr = 0.212; kb = 0.087; break;
		case PL_COLOR_SYSTEM_BT_709: break;
		default:
			if (primaries == PL_COLOR_PRIM_BT_2020) { kr = 0.2627; kb = 0.0593; }
			break;
		}
		out[0] = kr;
		out[1] = 1.0 - kr - kb;
		out[2] = kb;
	}

	Module::~Module()
	{
		delete impl;
	}

	uint64_t Module::HistorySequence() const { return impl && impl->decided ? impl->lastSequence : 0; }
	int Module::HistoryFrames() const { return impl && impl->decided ? impl->behind : 0; }

	void Module::Forget()
	{
		if (!impl) return;
		impl->decided = false;
		impl->behind = 0;
		impl->aheadSequence = 0;
	}

	void Module::Release(pl_gpu gpu)
	{
		if (impl)
		{
			for (pl_tex* tex : { &impl->out, &impl->outForeign, &impl->stats[0], &impl->stats[1], &impl->weight,
				&impl->aheadStats, &impl->aheadWeight, &impl->ring, &impl->low, &impl->lowForeign })
			{
				if (*tex && gpu) pl_tex_destroy(gpu, tex);
				*tex = nullptr;
			}
		}
		delete impl;
		impl = nullptr;
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
	{
		error.clear();
		if (!gpu || !dispatch || source.num_planes <= 0)
		{
			error = "nothing to stabilise";
			return false;
		}
		const pl_tex luma = source.planes[0].texture;
		if (!luma || !luma->params.sampleable)
		{
			error = "no sampleable luma plane";
			++failures;
			return false;
		}
		// Hunt 3 F8: counted (the renderer reports the stage inactive), never a silent no-op.
		if (PlaneKind(source) < 0)
		{
			error = "the first plane is neither luma nor one-plane RGB";
			++failures;
			return false;
		}
		if (!impl) impl = new Impl();
		Impl& m = *impl;
		Constants c;
		Prepare(luma, params, c, &source);
		if (!c.rgb)
			m.codeScale = c.codeScale;
		const bool sameGeometry = m.decided && c.geometry == m.geometry;
		const bool repeat = sameGeometry && params.sequence != 0 && params.sequence == m.lastSequence;
		const bool consecutive = sameGeometry && m.lastSequence != 0 && params.sequence == m.lastSequence + 1;
		pl_fmt f4 = Float32(gpu, 4);
		if (!Ensure(gpu, m.out, c.geometry.w, c.geometry.h, StorableLike(gpu, luma->params.format), true) ||
			!f4 || !EnsureRing(gpu, m.ring) || (params.fast && !Ensure(gpu, m.low, c.halfW, c.halfH, Float32(gpu, 1))))
		{
			error = "no storable textures";
			++failures;
			return false;
		}
		if (!repeat)
		{
			const int next = 1 - m.statsIndex;
			if (!Ensure(gpu, m.stats[next], c.tilesX, c.tilesY, f4) || !Ensure(gpu, m.weight, c.tilesX, c.tilesY, f4))
			{
				error = "no storable textures";
				++failures;
				m.decided = false;
				return false;
			}
			const int slot = m.decided ? (m.cur + 1) % kRingSlots : 0;
			const int nValid = consecutive ? (std::min)(m.behind + 1, kRingSlots - 2) : 0;
			const int nUse = (std::min)(nValid, WindowFrames(params.sourceHz));
			const bool aheadValid = m.aheadSequence != 0 && params.sequence != 0 && m.aheadSequence == params.sequence + 1 &&
				m.aheadGeometry == c.geometry;
			if (!MeasureAndReduce(dispatch, luma, params, c, m.stats[next], m.weight,
				consecutive && m.stats[m.statsIndex] ? m.stats[m.statsIndex] : nullptr, m.ring,
				false, slot, nValid, nUse, aheadValid, error, params.fast ? m.low : nullptr))
			{
				++failures;
				m.decided = false;
				return false;
			}
			m.statsIndex = next;
			m.cur = slot;
			m.behind = nValid;
			m.lastSequence = params.sequence;
			m.geometry = c.geometry;
			m.decided = true;
			if (aheadValid) ++aheadUsed;
		}
		// detail_pulse_fast: a new frame reads back the low band its measure pass wrote; a repeat
		// (another picture: hunt 3 F6) computes its own.
		if (!ApplyPass(dispatch, luma, m.out, m.weight, m.ring, c, nullptr, error, params.fast ? m.low : nullptr, repeat))
		{
			++failures;
			return false;
		}
		pl_frame frame = source;
		frame.planes[0].texture = m.out;
		result = frame;
		++frames;
		return true;
	}

	bool Module::ApplyForeign(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
	{
		error.clear();
		if (!gpu || !dispatch || source.num_planes <= 0 || !impl || !impl->decided || !impl->ring || !impl->weight)
		{
			error = "no decision held";
			return false;
		}
		const pl_tex luma = source.planes[0].texture;
		if (!luma || !luma->params.sampleable || PlaneKind(source) < 0)
		{
			error = "the first plane is neither luma nor one-plane RGB";
			++failures;
			return false;
		}
		Impl& m = *impl;
		Constants c;
		Prepare(luma, params, c, &source);
		if (!Ensure(gpu, m.outForeign, c.geometry.w, c.geometry.h, StorableLike(gpu, luma->params.format), true) ||
			(params.fast && !Ensure(gpu, m.lowForeign, c.halfW, c.halfH, Float32(gpu, 1))))
		{
			error = "no storable textures";
			++failures;
			return false;
		}
		const float ratio[2] = { static_cast<float>(m.geometry.w) / c.geometry.w, static_cast<float>(m.geometry.h) / c.geometry.h };
		if (!ApplyPass(dispatch, luma, m.outForeign, m.weight, m.ring, c, ratio, error, params.fast ? m.lowForeign : nullptr, true))
		{
			++failures;
			return false;
		}
		pl_frame frame = source;
		frame.planes[0].texture = m.outForeign;
		result = frame;
		return true;
	}

	bool Module::MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& next, uint64_t sequence, const Params& params,
		VpDebander::Module* debander, const VpDebander::Params* deband)
	{
		error.clear();
		if (!gpu || !dispatch || next.num_planes <= 0 || !next.planes[0].texture || sequence == 0 || PlaneKind(next) != 0)
		{
			error = "nothing to measure ahead";
			return false;
		}
		pl_frame measured = next;
		if (debander && deband && deband->thresholdCodes > 0.0)
		{
			// Hunt 3 F4: as Apply's frame - the debander first, per picture (no hold along a
			// field that belongs to the current frame), in its look-ahead slot.
			VpDebander::Params p = *deband;
			p.field = nullptr;
			p.grainError = nullptr;
			p.sequence = sequence;
			pl_frame debanded{};
			if (!debander->Apply(gpu, dispatch, next, p, debanded, VpDebander::Module::kSlotLookAhead))
			{
				error = "the look-ahead deband failed (" + debander->LastError() + ")";
				return false;
			}
			measured = debanded;
		}
		if (!impl) impl = new Impl();
		Impl& m = *impl;
		const pl_tex luma = measured.planes[0].texture;
		Constants c;
		Prepare(luma, params, c, &measured);
		pl_fmt f4 = Float32(gpu, 4);
		if (!f4 || !EnsureRing(gpu, m.ring) || !Ensure(gpu, m.aheadStats, c.tilesX, c.tilesY, f4) ||
			!Ensure(gpu, m.aheadWeight, c.tilesX, c.tilesY, f4) || (params.fast && !Ensure(gpu, m.low, c.halfW, c.halfH, Float32(gpu, 1))))
		{
			error = "no storable textures";
			++failures;
			return false;
		}
		m.aheadSequence = 0;
		if (!MeasureAndReduce(dispatch, luma, params, c, m.aheadStats, m.aheadWeight, nullptr, m.ring,
			true, 0, 0, 0, false, error, params.fast ? m.low : nullptr))
		{
			++failures;
			return false;
		}
		m.aheadSequence = sequence;
		m.aheadGeometry = c.geometry;
		return true;
	}

	bool Module::MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, pl_tex luma, uint64_t sequence, const Params& params)
	{
		error.clear();
		if (!gpu || !dispatch || !luma || !luma->params.sampleable || sequence == 0)
		{
			error = "nothing to measure ahead";
			return false;
		}
		if (!impl) impl = new Impl();
		Impl& m = *impl;
		// The next frame shares the current one's representation: the code scale is the last
		// luma-plane Apply's (a bare texture does not say where its signal's bits sit).
		if (!(m.codeScale > 0.0f))
		{
			error = "no frame applied yet";
			return false;
		}
		Constants c;
		Prepare(luma, params, c, nullptr);
		c.codeScale = m.codeScale;
		pl_fmt f4 = Float32(gpu, 4);
		if (!f4 || !EnsureRing(gpu, m.ring) || !Ensure(gpu, m.aheadStats, c.tilesX, c.tilesY, f4) ||
			!Ensure(gpu, m.aheadWeight, c.tilesX, c.tilesY, f4) || (params.fast && !Ensure(gpu, m.low, c.halfW, c.halfH, Float32(gpu, 1))))
		{
			error = "no storable textures";
			++failures;
			return false;
		}
		m.aheadSequence = 0;
		if (!MeasureAndReduce(dispatch, luma, params, c, m.aheadStats, m.aheadWeight, nullptr, m.ring,
			true, 0, 0, 0, false, error, params.fast ? m.low : nullptr))
		{
			++failures;
			return false;
		}
		m.aheadSequence = sequence;
		m.aheadGeometry = c.geometry;
		return true;
	}
}
