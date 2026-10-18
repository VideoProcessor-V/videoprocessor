#include <pch.h>
#include "TextureRestore.h"

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// texture_restore (docs/VP-detail-restore.md; the algorithm and the shared constants:
// TextureRestore.h; the CPU model: src/VideoProcessor-Test/TextureRestoreModel.h).
//
// Passes per source frame (luma plane only; every pass a compute shader, nothing read back):
//  1. HALF   full -> half resolution 2x2 box                          (reads the plane once)
//  2. LOWH   half: the s2 and s3 Gaussians across                     (quarter-size)
//  3. LOWV   half: down -> low (rg = L2, L3; two sets: this frame's and the previous one's)
//  4. STATS  one workgroup per tile: G1 in shared memory, the bands, per tile (E1, E2, E3, mu),
//            the quiet sub-block energies and the low band's change energy against the
//            previous frame's L2 (along the vector where trusted)     (reads the plane once)
//  5. GRAIN  per tile: the neighbourhood minimum, calibrated          (tile grid)
//  6. FRAME  one workgroup: cut (share of changed tiles) / fade (frame mean ramp)
//  7. REF    per tile: smoothed statistics, carried references, gains (tile grid)
//  8. APPLY  per pixel: G1 again in shared memory, the pixel split, the gains -> 16-bit output
//                                                                     (reads once, writes once)
// A repeated sequence runs 1-3 and 8 with the frame's own gains.
namespace TextureRestore
{
	namespace
	{
		const char* const HALF_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    ivec2 p = q * 2;
    float acc = texelFetch(src, clamp(p, ivec2(0), hi), 0).r + texelFetch(src, clamp(p + ivec2(1, 0), ivec2(0), hi), 0).r +
                texelFetch(src, clamp(p + ivec2(0, 1), ivec2(0), hi), 0).r + texelFetch(src, clamp(p + ivec2(1, 1), ivec2(0), hi), 0).r;
    imageStore(dst, q, vec4(0.25 * acc, 0.0, 0.0, 1.0));
}
)GLSL";

		const char* const LOWH_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    vec2 acc = vec2(0.0);
    for (int i = -R2; i <= R2; i++)
        acc.x += w2(i) * texelFetch(src, clamp(q + ivec2(i, 0), ivec2(0), hi), 0).r;
    for (int i = -R3; i <= R3; i++)
        acc.y += w3(i) * texelFetch(src, clamp(q + ivec2(i, 0), ivec2(0), hi), 0).r;
    imageStore(dst, q, vec4(acc, 0.0, 1.0));
}
)GLSL";

		const char* const LOWV_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    ivec2 hi = textureSize(src, 0) - ivec2(1);
    vec2 acc = vec2(0.0);
    for (int i = -R2; i <= R2; i++)
        acc.x += w2(i) * texelFetch(src, clamp(q + ivec2(0, i), ivec2(0), hi), 0).x;
    for (int i = -R3; i <= R3; i++)
        acc.y += w3(i) * texelFetch(src, clamp(q + ivec2(0, i), ivec2(0), hi), 0).y;
    imageStore(dst, q, vec4(acc, 0.0, 1.0));
}
)GLSL";

		// The half-resolution bands (L2, L3) at a full-resolution pixel: bilinear at p / 2.
		const char* const LOW_AT_GLSL = R"GLSL(
vec2 low_at(ivec2 p)
{
    ivec2 ls = textureSize(low, 0) - ivec2(1);
    vec2 h = (vec2(p) + vec2(0.5)) * 0.5 - vec2(0.5);
    ivec2 i0 = ivec2(floor(h));
    vec2 f = h - vec2(i0);
    vec2 a = texelFetch(low, clamp(i0, ivec2(0), ls), 0).xy;
    vec2 b = texelFetch(low, clamp(i0 + ivec2(1, 0), ivec2(0), ls), 0).xy;
    vec2 c = texelFetch(low, clamp(i0 + ivec2(0, 1), ivec2(0), ls), 0).xy;
    vec2 d = texelFetch(low, clamp(i0 + ivec2(1, 1), ivec2(0), ls), 0).xy;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
)GLSL";

		// One workgroup per tile (TPX px), in chunks of 16 x 16 px: the chunk and its R1 apron
		// in shared memory, G1 separably, the bands per pixel, sums per sub-block (one thread per
		// sub-block, fixed order: deterministic).
		const char* const STATS_HEADER = R"GLSL(
#define CH 16
#define AP (CH + 2 * R1)
#define SB (TPX / 4)
shared float s_y[AP * AP];
shared float s_h[AP * CH];
shared vec4 s_px[CH * CH];
shared float s_pc[CH * CH];
shared vec4 s_sub[16];
shared float s_subc[16];
shared float s_cnt[16];
#ifdef PREV
// The previous frame's L2 at a continuous plane position (bilinear at p / 2, clamped).
float low_prev_at(vec2 pp)
{
    ivec2 ls = textureSize(low_prev, 0) - ivec2(1);
    vec2 h = pp * 0.5 - vec2(0.5);
    ivec2 i0 = ivec2(floor(h));
    vec2 f = h - vec2(i0);
    float a = texelFetch(low_prev, clamp(i0, ivec2(0), ls), 0).x;
    float b = texelFetch(low_prev, clamp(i0 + ivec2(1, 0), ivec2(0), ls), 0).x;
    float c = texelFetch(low_prev, clamp(i0 + ivec2(0, 1), ivec2(0), ls), 0).x;
    float d = texelFetch(low_prev, clamp(i0 + ivec2(1, 1), ivec2(0), ls), 0).x;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
#endif
#ifdef FIELD
// The trusted vector under a plane position, in plane px (0 on an untrusted block).
vec2 vec_at(vec2 pp)
{
    vec2 lp = pp * to_field;
    ivec2 fs = textureSize(field, 0);
    vec4 f = texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fs - ivec2(1)), 0);
    return f.z > 0.5 ? f.xy / to_field : vec2(0.0);
}
#endif
float quiet_mean(float e[16])
{
    bool taken[16];
    for (int i = 0; i < 16; i++) taken[i] = false;
    float sum = 0.0;
    float k = 0.0;
    for (int pick = 0; pick < QUIET; pick++) {
        int best = -1;
        for (int i = 0; i < 16; i++)
            if (!taken[i] && e[i] >= 0.0 && (best < 0 || e[i] < e[best])) best = i;
        if (best < 0) break;
        taken[best] = true;
        sum += e[best];
        k += 1.0;
    }
    return k > 0.0 ? sum / k : 0.0;
}
)GLSL";
		const char* const STATS_BODY = R"GLSL(
ivec2 tile = ivec2(gl_WorkGroupID.xy);
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 torg = tile * TPX;
if (li < 16u) { s_sub[li] = vec4(0.0); s_subc[li] = 0.0; s_cnt[li] = 0.0; }
for (int cy = 0; cy < TPX; cy += CH) {
    for (int cx = 0; cx < TPX; cx += CH) {
        ivec2 org = torg + ivec2(cx, cy);
        barrier();
        for (uint i = li; i < uint(AP * AP); i += 256u)
            s_y[i] = texelFetch(src, clamp(org - ivec2(R1) + ivec2(int(i % uint(AP)), int(i / uint(AP))), ivec2(0), hi), 0).r;
        barrier();
        for (uint i = li; i < uint(AP * CH); i += 256u) {
            int r = int(i / uint(CH)), c = int(i % uint(CH));
            float acc = 0.0;
            for (int k = -R1; k <= R1; k++) acc += w1(k) * s_y[r * AP + c + R1 + k];
            s_h[i] = acc;
        }
        barrier();
        {
            ivec2 p = org + lid;
            float g1 = 0.0;
            for (int k = -R1; k <= R1; k++) g1 += w1(k) * s_h[(lid.y + R1 + k) * CH + lid.x];
            float y = s_y[(lid.y + R1) * AP + lid.x + R1];
            vec2 l = low_at(p);
            float d1 = y - g1, d2 = g1 - l.x, d3 = l.x - l.y;
            bool inside = p.x < sz.x && p.y < sz.y && cx + lid.x < TPX && cy + lid.y < TPX;
            s_px[li] = inside ? vec4(d1 * d1, d2 * d2, d3 * d3, l.x) : vec4(-1.0);
            float chg = 0.0;
#ifdef PREV
            vec2 pp = vec2(p) + vec2(0.5);
#ifdef FIELD
            pp -= vec_at(pp);
#endif
            float dc = l.x - low_prev_at(pp);
            chg = dc * dc;
#endif
            s_pc[li] = chg;
        }
        barrier();
        if (li < 16u) {
            int sx = int(li % 4u), sy = int(li / 4u);
            int x0 = max(sx * SB, cx), x1 = min(sx * SB + SB, cx + CH);
            int y0 = max(sy * SB, cy), y1 = min(sy * SB + SB, cy + CH);
            vec4 acc = s_sub[li];
            float accc = s_subc[li];
            float n = s_cnt[li];
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    int k = (yy - cy) * CH + (xx - cx);
                    vec4 v = s_px[k];
                    if (v.x >= 0.0) { acc += v; accc += s_pc[k]; n += 1.0; }
                }
            s_sub[li] = acc;
            s_subc[li] = accc;
            s_cnt[li] = n;
        }
    }
}
barrier();
if (li == 0u) {
    vec4 tot = vec4(0.0);
    float totc = 0.0;
    float n = 0.0;
    float e1[16];
    float e2[16];
    for (int i = 0; i < 16; i++) {
        tot += s_sub[i];
        totc += s_subc[i];
        n += s_cnt[i];
        e1[i] = s_cnt[i] > 0.0 ? s_sub[i].x / s_cnt[i] : -1.0;
        e2[i] = s_cnt[i] > 0.0 ? s_sub[i].y / s_cnt[i] : -1.0;
    }
    imageStore(stats_a, tile, tot / max(n, 1.0));
    imageStore(stats_b, tile, vec4(quiet_mean(e1), quiet_mean(e2), totc / max(n, 1.0), 1.0));
}
)GLSL";

		const char* const GRAIN_BODY = R"GLSL(
ivec2 t = ivec2(gl_GlobalInvocationID.xy);
ivec2 ts = textureSize(stats_b, 0);
if (t.x < ts.x && t.y < ts.y) {
    float m1 = 3.0e38, m2 = 3.0e38;
    for (int dy = -NG; dy <= NG; dy++)
        for (int dx = -NG; dx <= NG; dx++) {
            vec2 r = texelFetch(stats_b, clamp(t + ivec2(dx, dy), ivec2(0), ts - ivec2(1)), 0).xy;
            m1 = min(m1, r.x);
            m2 = min(m2, r.y);
        }
    // z: the tile's own low-band change energy, passed on.
    imageStore(grain_out, t, vec4(CAL1 * m1, CAL2 * m2, texelFetch(stats_b, t, 0).z, 1.0));
}
)GLSL";

		// The previous frame's position of a tile's content (FIELD: the de-judder's vector at the
		// tile centre, native px; false = an untrusted block) and bilinear reads of tile grids.
		const char* const CARRY_GLSL = R"GLSL(
#define DEFINE_TILEBI(NAME, TEX) \
vec4 NAME(vec2 uv) \
{ \
    ivec2 tm = textureSize(TEX, 0) - ivec2(1); \
    ivec2 i0 = ivec2(floor(uv)); \
    vec2 f = uv - vec2(i0); \
    vec4 a = texelFetch(TEX, clamp(i0, ivec2(0), tm), 0); \
    vec4 b = texelFetch(TEX, clamp(i0 + ivec2(1, 0), ivec2(0), tm), 0); \
    vec4 c = texelFetch(TEX, clamp(i0 + ivec2(0, 1), ivec2(0), tm), 0); \
    vec4 d = texelFetch(TEX, clamp(i0 + ivec2(1, 1), ivec2(0), tm), 0); \
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y); \
}
#ifdef FIELD
bool carry(ivec2 t, out vec2 uv)
{
    vec2 c = min((vec2(t) + vec2(0.5)) * float(TPX), plane_size - vec2(0.5));
    vec2 lp = c * to_field;
    ivec2 fs = textureSize(field, 0);
    vec4 f = texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fs - ivec2(1)), 0);
    uv = vec2(t);
    if (!(f.z > 0.5))
        return false;
    uv = (lp - f.xy) / to_field / float(TPX) - vec2(0.5);
    return true;
}
#endif
)GLSL";

		const char* const FRAME_HEADER = R"GLSL(
shared vec2 s_acc[256];
)GLSL";
		const char* const FRAME_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 ts = textureSize(stats_cur, 0);
int n = ts.x * ts.y;
vec2 acc = vec2(0.0);
for (int i = int(li); i < n; i += 256) {
    ivec2 t = ivec2(i % ts.x, i / ts.x);
    acc.y += texelFetch(stats_cur, t, 0).w;
    vec4 g = texelFetch(grain, t, 0);
    // The tile's low band changed (along the vector where trusted; STATS).
    if (restart_all < 0.5 && g.z > allow2 + LOWN * g.y)
        acc.x += 1.0;
}
s_acc[li] = acc;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        s_acc[li] += s_acc[li + s];
    barrier();
}
if (li == 0u) {
    float count = float(n);
    float mean = s_acc[0].y / max(count, 1.0);
    vec4 old = texelFetch(state_old, ivec2(0), 0);
    vec4 st;
    if (restart_all > 0.5) {
        st = vec4(1.0, mean, mean, 1.0);
    } else {
        float d0 = mean - old.y, d1 = old.y - old.z;
        bool fade = old.w >= 2.0 && abs(d0) > fade_step && abs(d1) > fade_step && d0 * d1 > 0.0;
        bool cut = s_acc[0].x > cut_share * count;
        st = vec4((cut || fade) ? 1.0 : 0.0, mean, old.y, min(old.w + 1.0, 2.0));
    }
    imageStore(state_out, ivec2(0), st);
}
)GLSL";

		const char* const REF_HEADER = R"GLSL(
DEFINE_TILEBI(ref_bi, ref_old)
)GLSL";
		const char* const REF_BODY = R"GLSL(
ivec2 t = ivec2(gl_GlobalInvocationID.xy);
ivec2 ts = textureSize(stats_cur, 0);
if (t.x < ts.x && t.y < ts.y) {
    ivec2 tm = ts - ivec2(1);
    vec3 sE = vec3(0.0);
    vec2 sN = vec2(0.0);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            float wgt = (dx == 0 ? 0.5 : 0.25) * (dy == 0 ? 0.5 : 0.25);
            ivec2 q = clamp(t + ivec2(dx, dy), ivec2(0), tm);
            sE += wgt * texelFetch(stats_cur, q, 0).xyz;
            sN += wgt * texelFetch(grain, q, 0).xy;
        }
    float T1 = max(0.0, sE.x - EXCESS * sN.x);
    float T2 = max(0.0, sE.y - EXCESS * sN.y);
    float F = T2 / (T2 + sE.z + ft);
    vec4 gt = texelFetch(grain, t, 0);
    bool restart = texelFetch(state_new, ivec2(0), 0).x > 0.5;
    vec4 old = vec4(0.0);
    if (!restart) {
        old = texelFetch(ref_old, clamp(t, ivec2(0), textureSize(ref_old, 0) - ivec2(1)), 0);
#ifdef FIELD
        vec2 uv;
        if (!carry(t, uv))
            restart = true;
        else
            old = ref_bi(uv);
#endif
        old *= dec;
        if (gt.z > allow2 + LOWN * gt.y) restart = true;
        if (F < PULL_LO * old.w) restart = true;
        if (T2 + ft < MID_LO * (old.z + ft)) restart = true;
    }
    vec4 cur = vec4(sN.x, T1, T2, F);
    vec4 r = restart ? cur : max(cur, old);
    imageStore(ref_out, t, r);
    float gG = clamp(sqrt((r.x + fg) / ((sN.x + fg) * (1.0 + GRAIN_MARGIN))), 1.0, cap);
    float gT1 = clamp(sqrt((r.y + ft) / (T1 + TEX_MARGIN * sN.x + ft)), 1.0, cap);
    float gT2 = clamp(sqrt((r.z + ft) / (T2 + TEX_MARGIN * sN.y + ft)), 1.0, cap);
    float w = smoothstep(FOCUS_LO, FOCUS_HI, F) * smoothstep(FOCUS_LO, FOCUS_HI, r.w) *
              smoothstep(PULL_LO, PULL_HI, F / max(r.w, 1e-30)) *
              smoothstep(MID_LO, MID_HI, (T2 + ft) / (r.z + ft)) *
              smoothstep(SIG_LO, SIG_HI, r.z / (sN.y + sig_floor)) *
              smoothstep(SIG_LO, SIG_HI, T2 / (sN.y + sig_floor));
    imageStore(gains_out, t, vec4(gG, 1.0 + (gT1 - 1.0) * w, 1.0 + (gT2 - 1.0) * w, sN.x));
}
)GLSL";

		// 16 x 16 output px per workgroup: G1 over the block and a 1 px border (for the 3 x 3
		// local energy) from the R1 apron in shared memory.
		const char* const APPLY_HEADER = R"GLSL(
#define CH 16
#define AW (CH + 2)
#define AA (AW + 2 * R1)
shared float s_y[AA * AA];
shared float s_h[AA * AW];
shared float s_d[AW * AW];
vec4 gain_at(ivec2 p)
{
    ivec2 tm = textureSize(gains, 0) - ivec2(1);
    vec2 uv = (vec2(p) + vec2(0.5)) / float(TPX) - vec2(0.5);
    ivec2 i0 = ivec2(floor(uv));
    vec2 f = uv - vec2(i0);
    vec4 a = texelFetch(gains, clamp(i0, ivec2(0), tm), 0);
    vec4 b = texelFetch(gains, clamp(i0 + ivec2(1, 0), ivec2(0), tm), 0);
    vec4 c = texelFetch(gains, clamp(i0 + ivec2(0, 1), ivec2(0), tm), 0);
    vec4 d = texelFetch(gains, clamp(i0 + ivec2(1, 1), ivec2(0), tm), 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
)GLSL";
		const char* const APPLY_BODY = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
ivec2 sz = textureSize(src, 0);
ivec2 hi = sz - ivec2(1);
ivec2 org = ivec2(gl_WorkGroupID.xy) * CH - ivec2(1 + R1);
for (uint i = li; i < uint(AA * AA); i += 256u)
    s_y[i] = texelFetch(src, clamp(org + ivec2(int(i % uint(AA)), int(i / uint(AA))), ivec2(0), hi), 0).r;
barrier();
for (uint i = li; i < uint(AA * AW); i += 256u) {
    int r = int(i / uint(AW)), c = int(i % uint(AW));
    float acc = 0.0;
    for (int k = -R1; k <= R1; k++) acc += w1(k) * s_y[r * AA + c + R1 + k];
    s_h[i] = acc;
}
barrier();
for (uint i = li; i < uint(AW * AW); i += 256u) {
    int r = int(i / uint(AW)), c = int(i % uint(AW));
    float g = 0.0;
    for (int k = -R1; k <= R1; k++) g += w1(k) * s_h[(r + R1 + k) * AW + c];
    s_d[i] = s_y[(r + R1) * AA + c + R1] - g;
}
barrier();
ivec2 p = ivec2(gl_WorkGroupID.xy) * CH + lid;
if (p.x < sz.x && p.y < sz.y) {
    int ci = (lid.y + 1) * AW + lid.x + 1;
    float e1 = 0.0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            float v = s_d[ci + dy * AW + dx];
            e1 += v * v;
        }
    e1 /= 9.0;
    float d1 = s_d[ci];
    float y = s_y[(lid.y + 1 + R1) * AA + lid.x + 1 + R1];
    float d2 = (y - d1) - low_at(p).x;
    vec4 g = gain_at(p);
    float f = clamp(1.0 - KAPPA * g.w / (e1 + eps), 0.0, 1.0);
    float o = y + (g.x - 1.0) * (1.0 - f) * d1 + (g.y - 1.0) * f * d1 + (g.z - 1.0) * f * d2;
    vec4 c = texelFetch(src, p, 0);
    imageStore(dst, p, vec4(o, c.yzw));
}
)GLSL";

		std::string Num(double v)
		{
			char s[48];
			snprintf(s, sizeof(s), "%.9g", v);
			std::string r = s;
			if (r.find_first_of(".eE") == std::string::npos) r += ".0";
			return r;
		}

		// float NAME(int k), k in -R..R.
		template <typename F>
		std::string WeightsGlsl(const char* name, const char* radiusName, int radius, F weight)
		{
			const std::string taps = std::to_string(2 * radius + 1);
			std::string s = "#define " + std::string(radiusName) + " " + std::to_string(radius) + "\nfloat " + name +
				"(int k)\n{\n    float w[" + taps + "] = float[" + taps + "](";
			for (int k = -radius; k <= radius; ++k)
			{
				s += Num(weight(k));
				if (k < radius) s += ", ";
			}
			return s + ");\n    return w[k + " + radiusName + "];\n}\n";
		}

		pl_fmt FloatFormat(pl_gpu gpu, int components)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			for (int n = (std::max)(1, components); n <= 4; ++n)
				if (pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, n, 32, 32, caps))
					return f;
			return nullptr;
		}

		pl_fmt StorableLike(pl_gpu gpu, pl_fmt like)
		{
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
			// At least 16 bits: the restored detail is finer than an 8-bit code.
			if (like && (like->caps & caps) == caps && like->component_depth[0] >= 16) return like;
			const int n = like ? like->num_components : 1;
			pl_fmt f = pl_find_fmt(gpu, PL_FMT_UNORM, n, 16, 16, caps);
			return f ? f : pl_find_fmt(gpu, PL_FMT_FLOAT, n, 16, 16, caps);
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

		pl_shader_desc Storage(const char* name, pl_tex tex)
		{
			pl_shader_desc d{};
			d.desc.name = name;
			d.desc.type = PL_DESC_STORAGE_IMG;
			d.desc.access = PL_DESC_ACCESS_WRITEONLY;
			d.binding.object = tex;
			return d;
		}

		bool Pass(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
			const pl_shader_desc* desc, int numDesc, const pl_shader_var* vars, int numVars, int groupsX, int groupsY, size_t shmem)
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
			compute.dispatch_size[0] = (std::max)(1, groupsX);
			compute.dispatch_size[1] = (std::max)(1, groupsY);
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
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
	}

	struct Module::Impl
	{
		pl_tex out = nullptr, half = nullptr, tmp = nullptr, low[2] = {};
		pl_tex statsA = nullptr, statsB = nullptr, grain = nullptr, ref[2] = {}, gains = nullptr, state[2] = {};
		int cur = 0;                // low / ref / state of the last frame that advanced
		bool have = false, forget = false;
		uint64_t sequence = 0;
		int w = 0, h = 0, tile = 0, tw = 0, th = 0;
	};

	Module::~Module()
	{
		// Textures belong to the gpu (Release); only the bookkeeping is freed here.
		delete impl;
	}

	void Module::Forget()
	{
		if (impl) impl->forget = true;
	}

	uint64_t Module::HistorySequence() const { return impl && impl->have && !impl->forget ? impl->sequence : 0; }

	void Module::Release(pl_gpu gpu)
	{
		if (impl && gpu)
		{
			for (pl_tex* t : { &impl->out, &impl->half, &impl->tmp, &impl->low[0], &impl->low[1], &impl->statsA, &impl->statsB,
				&impl->grain, &impl->ref[0], &impl->ref[1], &impl->gains, &impl->state[0], &impl->state[1] })
				if (*t) pl_tex_destroy(gpu, t);
		}
		delete impl;
		impl = nullptr;
	}

	bool Module::ApplyForeign(pl_gpu, pl_dispatch, const pl_frame&, const Params&, pl_frame&)
	{
		error = "not implemented";
		return false;
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
	{
		error.clear();
		if (!gpu || !dispatch || source.num_planes <= 0)
		{
			error = "nothing to restore";
			return false;
		}
		const double cap = EffectiveCap(params.cap);
		if (!(cap > 1.0))
		{
			// cap 1.0: the input itself, bit for bit; the references restart when it rises.
			if (impl) impl->forget = true;
			result = source;
			return true;
		}
		const pl_tex luma = source.planes[0].texture;
		if (!luma || !luma->params.sampleable)
		{
			error = "the luma plane is not sampleable";
			++failures;
			return false;
		}
		if (!pl_color_system_is_ycbcr_like(source.repr.sys) || source.planes[0].component_mapping[0] != PL_CHANNEL_Y)
		{
			error = "plane 0 is not luma";
			++failures;
			return false;
		}
		if (!impl) impl = new Impl();
		Impl& m = *impl;
		const int w = luma->params.w, h = luma->params.h;
		const double scale = Scale(params.planeLines > 0.0 ? params.planeLines : h);
		const int T = TilePx(scale);
		const int tw = (w + T - 1) / T, th = (h + T - 1) / T;
		const bool sameSize = m.have && w == m.w && h == m.h && T == m.tile;
		const bool repeat = sameSize && !m.forget && params.sequence != 0 && params.sequence == m.sequence;
		const bool restartAll = !repeat && (!m.have || !sameSize || m.forget || params.sequence == 0 || params.sequence != m.sequence + 1);
		const int hw = (w + 1) / 2, hh = (h + 1) / 2;
		const pl_fmt f4 = FloatFormat(gpu, 4), f2 = FloatFormat(gpu, 2), f1 = FloatFormat(gpu, 1);
		auto fail = [&](const char* what) {
			error = what;
			++failures;
			m.have = false;   // the next frame restarts
			return false;
		};
		if (!Ensure(gpu, m.out, w, h, StorableLike(gpu, luma->params.format), true))
			return fail("no storable texture for the luma plane");
		if (!Ensure(gpu, m.half, hw, hh, f1) || !Ensure(gpu, m.tmp, hw, hh, f2) || !Ensure(gpu, m.low[0], hw, hh, f2) || !Ensure(gpu, m.low[1], hw, hh, f2))
			return fail("no float texture for the low bands");
		if (!Ensure(gpu, m.statsA, tw, th, f4) || !Ensure(gpu, m.statsB, tw, th, f4) ||
			!Ensure(gpu, m.grain, tw, th, f4) || !Ensure(gpu, m.ref[0], tw, th, f4) || !Ensure(gpu, m.ref[1], tw, th, f4) ||
			!Ensure(gpu, m.gains, tw, th, f4) || !Ensure(gpu, m.state[0], 1, 1, f4) || !Ensure(gpu, m.state[1], 1, 1, f4))
			return fail("no float texture for the tile statistics");
		if (repeat && !m.have)
			return fail("no gains for the repeated frame");

		// One code of the signal in the plane texture's normalised units (as VpDebander).
		const int sampleDepth = source.repr.bits.sample_depth > 0 ? source.repr.bits.sample_depth : luma->params.format->component_depth[0];
		const double cu = luma->params.format->type == PL_FMT_FLOAT ?
			1.0 / (std::pow(2.0, params.colorDepth) - 1.0) :
			std::pow(2.0, source.repr.bits.bit_shift) / (std::pow(2.0, sampleDepth) - 1.0);

		const int r1 = Radius1(scale);
		const std::string w1 = WeightsGlsl("w1", "R1", r1, [&](int k) { return Weight1(k, scale); });
		const double s2 = kSigma2 * scale, s3 = kSigma3 * scale;
		const std::string w23 = WeightsGlsl("w2", "R2", HalfRadius(s2), [&](int k) { return HalfWeight(k, s2); }) +
			WeightsGlsl("w3", "R3", HalfRadius(s3), [&](int k) { return HalfWeight(k, s3); });
		const std::string tileDef = "#define TPX " + std::to_string(T) + "\n";

		// The frame's own textures: a repeat rewrites its frame's low bands (the same picture) and
		// reuses its gains; a new frame writes the other set and reads the previous one.
		const int next = repeat ? m.cur : 1 - m.cur;
		const pl_tex low = m.low[next];

		// 1-3: the half-resolution low bands.
		{
			const int gx = (hw + 15) / 16, gy = (hh + 15) / 16;
			const pl_shader_desc dh[2] = { Sampled("src", luma), Storage("dst", m.half) };
			const pl_shader_desc dx[2] = { Sampled("src", m.half), Storage("dst", m.tmp) };
			const pl_shader_desc dy[2] = { Sampled("src", m.tmp), Storage("dst", low) };
			if (!Pass(dispatch, "VideoProcessor texture restore: half", std::string(), HALF_BODY, dh, 2, nullptr, 0, gx, gy, 0) ||
				!Pass(dispatch, "VideoProcessor texture restore: low bands (across)", w23, LOWH_BODY, dx, 2, nullptr, 0, gx, gy, 0) ||
				!Pass(dispatch, "VideoProcessor texture restore: low bands (down)", w23, LOWV_BODY, dy, 2, nullptr, 0, gx, gy, 0))
				return fail("the low band passes failed");
		}

		if (!repeat)
		{
			const double eight = EightBitStep(params.colorDepth);
			const double allowCodes = MeanAllowanceCodes(params.bitrateQuality, params.colorDepth);
			const float allow2 = static_cast<float>(allowCodes * allowCodes * cu * cu);
			const float fadeStep = static_cast<float>(kFadeStepShare * allowCodes * cu);
			const float cutShare = static_cast<float>(kCutShare);
			const float restartF = restartAll ? 1.0f : 0.0f;
			const float fg = static_cast<float>(std::pow(kGrainFloorCodes8 * eight * cu, 2.0));
			const float ft = static_cast<float>(std::pow(kTextureFloorCodes8 * eight * cu, 2.0));
			const float dec = static_cast<float>(Decay(params.sourceHz));
			const float sigFloor = static_cast<float>(std::pow(CodingNoiseCodes8(params.bitrateQuality) * eight * cu, 2.0));
			const float capF = static_cast<float>(cap);
			const float planeSize[2] = { static_cast<float>(w), static_cast<float>(h) };
			const float toField[2] = { static_cast<float>(params.fieldScaleX > 0.0 ? params.fieldScaleX : params.fieldScale),
				static_cast<float>(params.fieldScale) };
			const bool useField = params.field != nullptr && !restartAll;
			const std::string fieldDef = useField ? "#define FIELD 1\n" : "";
			const std::string lowDef = "#define LOWN " + Num(kLowChangeNoise) + "\n";

			// 4: STATS.
			{
				const std::string header = tileDef + fieldDef + (restartAll ? "" : "#define PREV 1\n") + "#define QUIET " +
					std::to_string(kQuietSubBlocks) + "\n" + w1 + LOW_AT_GLSL + STATS_HEADER;
				const int ap = 16 + 2 * r1;
				const size_t shmem = sizeof(float) * (ap * ap + ap * 16 + 256 * 4 + 256 + 16 * 4 + 16 + 16);
				pl_shader_desc d[6];
				int nd = 0;
				d[nd++] = Sampled("src", luma);
				d[nd++] = Sampled("low", low);
				d[nd++] = Storage("stats_a", m.statsA);
				d[nd++] = Storage("stats_b", m.statsB);
				if (!restartAll) d[nd++] = Sampled("low_prev", m.low[m.cur]);
				if (useField) d[nd++] = Sampled("field", params.field);
				pl_shader_var v[1];
				int nv = 0;
				if (useField) v[nv++] = { pl_var_vec2("to_field"), toField, true };
				if (!Pass(dispatch, "VideoProcessor texture restore: tile statistics", header, STATS_BODY, d, nd, v, nv, tw, th, shmem))
					return fail("the statistics pass failed");
			}
			// 5: GRAIN.
			{
				const std::string header = "#define NG " + std::to_string(kGrainNeighbourhood) + "\n#define CAL1 " + Num(kGrainCal1) +
					"\n#define CAL2 " + Num(kGrainCal2) + "\n";
				const pl_shader_desc d[2] = { Sampled("stats_b", m.statsB), Storage("grain_out", m.grain) };
				if (!Pass(dispatch, "VideoProcessor texture restore: grain level", header, GRAIN_BODY, d, 2, nullptr, 0, (tw + 15) / 16, (th + 15) / 16, 0))
					return fail("the grain level pass failed");
			}
			// 6: FRAME.
			{
				const std::string header = lowDef + FRAME_HEADER;
				const pl_shader_desc d[4] = { Sampled("stats_cur", m.statsA), Sampled("grain", m.grain), Sampled("state_old", m.state[m.cur]),
					Storage("state_out", m.state[next]) };
				const pl_shader_var v[4] = { { pl_var_float("allow2"), &allow2, true }, { pl_var_float("fade_step"), &fadeStep, true },
					{ pl_var_float("cut_share"), &cutShare, true }, { pl_var_float("restart_all"), &restartF, true } };
				if (!Pass(dispatch, "VideoProcessor texture restore: cuts and fades", header, FRAME_BODY, d, 4, v, 4, 1, 1, sizeof(float) * 2 * 256))
					return fail("the cut pass failed");
			}
			// 7: REF.
			{
				const std::string header = tileDef + fieldDef + lowDef +
					"#define EXCESS " + Num(kGrainExcess) + "\n#define GRAIN_MARGIN " + Num(kGrainMargin) + "\n#define TEX_MARGIN " + Num(kTextureMargin) +
					"\n#define FOCUS_LO " + Num(kFocusLo) + "\n#define FOCUS_HI " + Num(kFocusHi) + "\n#define PULL_LO " + Num(kPullLo) +
					"\n#define PULL_HI " + Num(kPullHi) + "\n#define MID_LO " + Num(kMidLossLo) + "\n#define MID_HI " + Num(kMidLossHi) +
					"\n#define SIG_LO " + Num(kTextureSigLo) + "\n#define SIG_HI " + Num(kTextureSigHi) + "\n" + CARRY_GLSL + REF_HEADER;
				pl_shader_desc d[7];
				int nd = 0;
				d[nd++] = Sampled("stats_cur", m.statsA);
				d[nd++] = Sampled("grain", m.grain);
				d[nd++] = Sampled("ref_old", m.ref[m.cur]);
				d[nd++] = Sampled("state_new", m.state[next]);
				d[nd++] = Storage("ref_out", m.ref[next]);
				d[nd++] = Storage("gains_out", m.gains);
				if (useField) d[nd++] = Sampled("field", params.field);
				pl_shader_var v[9];
				int nv = 0;
				v[nv++] = { pl_var_float("allow2"), &allow2, true };
				v[nv++] = { pl_var_float("fg"), &fg, true };
				v[nv++] = { pl_var_float("ft"), &ft, true };
				v[nv++] = { pl_var_float("dec"), &dec, true };
				v[nv++] = { pl_var_float("cap"), &capF, true };
				v[nv++] = { pl_var_float("sig_floor"), &sigFloor, true };
				if (useField)
				{
					v[nv++] = { pl_var_vec2("to_field"), toField, true };
					v[nv++] = { pl_var_vec2("plane_size"), planeSize, true };
				}
				if (!Pass(dispatch, "VideoProcessor texture restore: references and gains", header, REF_BODY, d, nd, v, nv, (tw + 15) / 16, (th + 15) / 16, 0))
					return fail("the reference pass failed");
			}
		}

		// 8: APPLY.
		{
			const float eps = static_cast<float>(kEnergyEps * cu * cu);
			const std::string header = tileDef + "#define KAPPA " + Num(kWienerKappa) + "\n" + w1 + LOW_AT_GLSL + APPLY_HEADER;
			const int aw = 18, aa = aw + 2 * r1;
			const size_t shmem = sizeof(float) * (aa * aa + aa * aw + aw * aw);
			const pl_shader_desc d[4] = { Sampled("src", luma), Sampled("low", low), Sampled("gains", m.gains), Storage("dst", m.out) };
			const pl_shader_var v[1] = { { pl_var_float("eps"), &eps, true } };
			if (!Pass(dispatch, "VideoProcessor texture restore", header, APPLY_BODY, d, 4, v, 1, (w + 15) / 16, (h + 15) / 16, shmem))
				return fail("the apply pass failed");
		}

		if (!repeat)
		{
			m.cur = next;
			m.sequence = params.sequence;
			m.have = true;
			m.forget = false;
			m.w = w;
			m.h = h;
			m.tile = T;
			m.tw = tw;
			m.th = th;
		}
		++frames;
		pl_frame frame = source;
		frame.planes[0].texture = m.out;
		result = frame;
		return true;
	}
}
