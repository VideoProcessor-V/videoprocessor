#include <pch.h>
#include "DejudderWarp.h"

#include <chrono>

#include <libplacebo/shaders/custom.h>
#include <libplacebo/d3d11.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Dejudder
{
	namespace
	{
		// Half-resolution luma in 8-bit code units (2x2 average of Y): `codes`
		// per unit, 255 for limited-range sources (the stored codes) and 219
		// for full range (limited-equivalent codes; delta 5, D5-3).
		const char* const DOWNSAMPLE_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    float v;
#ifdef FADE_PLAIN
    // No fade active: B's uncorrected half-size value, stored by the plain
    // pass (the same value this pass would store).
    if (texelFetch(fade_state, ivec2(0), 0).z <= 0.5)
        v = texelFetch(plain, p, 0).r;
    else
#endif
    {
        ivec2 full = textureSize(src, 0);
        float sum = 0.0;
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++)
                sum += texelFetch(src, min(p * 2 + ivec2(x, y), full - ivec2(1)), 0).r;
        v = sum * 0.25 * codes;
#ifdef FADE_APPLY
        // M4: B after the fade's global gain and offset (when active).
        vec4 fs = texelFetch(fade_state, ivec2(0), 0);
        if (fs.z > 0.5)
            v = v * fs.x + fs.y;
#endif
    }
    imageStore(dst, p, vec4(v, 0.0, 0.0, 1.0));
}
)GLSL";

		// M4: the fade test on the 1/32 pyramids (one work group): A's and B's
		// mean and spread; active when the mean moves smoothly (the same
		// direction as the last pair's, 0.75 to 24 codes) with both spreads
		// above a code. state_io: gain, offset, active, this pair's change.
		const char* const FADE_HEADER = R"GLSL(
shared vec4 fm_[256];
shared float fn_[256];
)GLSL";

		const char* const FADE_BODY = R"GLSL(
uint i = gl_LocalInvocationIndex;
ivec2 ls = textureSize(l2_a, 0);
int lc = ls.x * ls.y;
vec4 acc = vec4(0.0);
float an = 0.0;
#ifdef PICTURE
vec4 pa_r = texelFetch(picture, ivec2(0), 0);
#endif
for (int k = int(i); k < lc; k += 256) {
    ivec2 p = ivec2(k % ls.x, k / ls.x);
#ifdef PICTURE
    // Black bars: the picture's mean and spread only.
    vec2 c_ = (vec2(p) + vec2(0.5)) * 32.0;
    if (c_.x < pa_r.x || c_.y < pa_r.y || c_.x >= pa_r.z || c_.y >= pa_r.w)
        continue;
#endif
    float a = texelFetch(l2_a, p, 0).r, b = texelFetch(l2_b, p, 0).r;
    acc += vec4(a, b, a * a, b * b);
    an += 1.0;
}
fm_[i] = acc;
fn_[i] = an;
barrier();
for (uint st = 128u; st > 0u; st >>= 1) {
    if (i < st) {
        fm_[i] += fm_[i + st];
        fn_[i] += fn_[i + st];
    }
    barrier();
}
if (i == 0u) {
    float n = max(fn_[0], 1.0);
    float ma = fm_[0].x / n, mb = fm_[0].y / n;
    float sa = sqrt(max(fm_[0].z / n - ma * ma, 0.0)), sb = sqrt(max(fm_[0].w / n - mb * mb, 0.0));
    vec4 prev = imageLoad(state_io, ivec2(0));
    float d = mb - ma;
    // A fade scales the picture about its black level: B ~ black + g x (A -
    // black), with g = B's spread over A's. A pan moves the mean with the
    // content entering and leaving while the spread stays (g ~ 1), which puts
    // the implied black level far outside the plausible range.
    float g = sb / max(sa, 1e-3);
    float black = abs(1.0 - g) > 0.01 ? (mb - g * ma) / (1.0 - g) : 1e9;
    bool scaled = abs(1.0 - g) > 0.01 && black > -12.0 && black < 48.0;
    bool fade_on = scaled && abs(d) > fade_lo && abs(d) < fade_hi && abs(prev.w) > fade_lo && sign(d) == sign(prev.w) && sa > 1.0 && sb > 1.0;
    float gain = clamp(sa / max(sb, 1e-3), 0.5, 2.0);
    imageStore(state_io, ivec2(0), vec4(gain, ma - gain * mb, fade_on ? 1.0 : 0.0, d));
}
)GLSL";

		// Black bars (owner 2026-10-14): the picture rectangle in input pixels.
		// The renderer's active picture when given; else, on the 1/8 luma, the
		// first and last rows and columns brighter than black + bar_codes (a
		// frame with none, a fade to black, is all picture). One work group.
		const char* const PICTURE_HEADER = R"GLSL(
shared int pa_[4];
)GLSL";

		const char* const PICTURE_BODY = R"GLSL(
uint i = gl_LocalInvocationIndex;
if (i == 0u) { pa_[0] = 1 << 30; pa_[1] = -1; pa_[2] = 1 << 30; pa_[3] = -1; }
barrier();
ivec2 s = textureSize(l1, 0);
if (given.z <= given.x || given.w <= given.y) {
    for (int y = int(i); y < s.y; y += 256) {
        float m = 0.0;
        for (int x = 0; x < s.x; x++)
            m = max(m, texelFetch(l1, ivec2(x, y), 0).r);
        if (m > black + bar_codes) { atomicMin(pa_[0], y); atomicMax(pa_[1], y); }
    }
    for (int x = int(i); x < s.x; x += 256) {
        float m = 0.0;
        for (int y = 0; y < s.y; y++)
            m = max(m, texelFetch(l1, ivec2(x, y), 0).r);
        if (m > black + bar_codes) { atomicMin(pa_[2], x); atomicMax(pa_[3], x); }
    }
}
barrier();
if (i == 0u) {
    vec4 r = vec4(0.0, 0.0, frame_size);
    if (given.z > given.x && given.w > given.y)
        r = clamp(given, vec4(0.0), vec4(frame_size, frame_size));
    else if (pa_[1] >= pa_[0] && pa_[3] >= pa_[2])
        r = vec4(float(pa_[2]) * 8.0, float(pa_[0]) * 8.0,
                 min(float(pa_[3] + 1) * 8.0, frame_size.x), min(float(pa_[1] + 1) * 8.0, frame_size.y));
    imageStore(dst, ivec2(0), r);
}
)GLSL";

		// Optical flow source: the half-size luma (8-bit codes) as the
		// engine's 8-bit input.
		const char* const OF_INPUT_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < imageSize(dst).x && p.y < imageSize(dst).y)
    imageStore(dst, p, vec4(clamp(texelFetch(src, p, 0).r / 255.0, 0.0, 1.0), 0.0, 0.0, 1.0));
)GLSL";

		// Optical flow source: a block's vector (8x8 half-size pixels = 2x2
		// engine vectors, S10.5) as the median of its four per component,
		// re-scored like the searches (the 12x12 window): rg vector, b SAD.
		const char* const OF_TO_SEARCH_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    half_size_ = textureSize(from_half, 0);
    ivec2 fs = textureSize(flow, 0);
    vec2 v[4];
    for (int k = 0; k < 4; k++)
        v[k] = vec2(texelFetch(flow, min(b * 2 + ivec2(k & 1, k >> 1), fs - ivec2(1)), 0).rg) / 32.0;
    // The median of four: the mean of the middle two, per component.
    vec2 lo = min(min(v[0], v[1]), min(v[2], v[3]));
    vec2 hi = max(max(v[0], v[1]), max(v[2], v[3]));
    vec2 m = (v[0] + v[1] + v[2] + v[3] - lo - hi) * 0.5;
    // Beyond the searches' own reach the engine can match a far, similar texture
    // across a cut (corpus 2026-10-16: the cut was warped): such a vector is no
    // candidate - zero motion with a failing SAD, so the block is not trusted on it.
    if (length(m) > reach) {
        imageStore(dst, b, vec4(0.0, 0.0, 1.0, 1.0));
        return;
    }
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = b * 8 + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + m));
        }
    imageStore(dst, b, vec4(m, s / 144.0, 1.0));
}
)GLSL";

		// One pyramid step: a 4x4 average (half -> 1/8, 1/8 -> 1/32 source).
		const char* const PYRAMID_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    ivec2 src_size = textureSize(src, 0);
    float sum = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            sum += texelFetch(src, min(p * 4 + ivec2(x, y), src_size - ivec2(1)), 0).r;
    imageStore(dst, p, vec4(sum / 16.0, 0.0, 0.0, 1.0));
}
)GLSL";

		// Block search from -> to on half-resolution luma. The vector v says
		// where the block's content is in `to`: from(p) ~ to(p + v).
		const char* const SEARCH_HEADER = R"GLSL(
ivec2 half_size_;
ivec2 l1_size_;
ivec2 l2_size_;

float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_at(ivec2 p) { return texelFetch(to_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
float l1from(ivec2 p) { return texelFetch(from_l1, clamp(p, ivec2(0), l1_size_ - ivec2(1)), 0).r; }
float l1to(vec2 p) { return texture(to_l1, (p + vec2(0.5)) / vec2(l1_size_)).r; }
float l2from(ivec2 p) { return texelFetch(from_l2, clamp(p, ivec2(0), l2_size_ - ivec2(1)), 0).r; }
float l2to(ivec2 p) { return texelFetch(to_l2, clamp(p, ivec2(0), l2_size_ - ivec2(1)), 0).r; }

// 11a E4: nothing moves (the global vector and the previous frame's mean
// trusted |v| below motion_lo, the global vector confident).
bool static_scene()
{
    if (use_gate == 0)
        return false;
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
    float prev_mean = texelFetch(prev_trust, ivec2(0), 0).b;
    return gv.w > 0.5 && gv.z < 0.9 && length(gv.xy) * 8.0 < motion_lo && prev_mean < motion_lo;
}

// The block's own 8x8 plus a 2-pixel margin.
float sad(ivec2 o, ivec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_at(p) - to_at(p + v));
        }
    return s;
}

float sad_sub(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + v));
        }
    return s;
}

// Level 1 (1/8 source): the block is 2x2 texels; window 4x4. `to` is
// sampled bilinearly, so v can be fractional (half-texel steps).
float sad_l1(ivec2 o1, vec2 v)
{
    float s = 0.0;
    for (int y = -1; y < 3; y++)
        for (int x = -1; x < 3; x++) {
            ivec2 p = o1 + ivec2(x, y);
            s += abs(l1from(p) - l1to(vec2(p) + v));
        }
    return s;
}

// Level 2 (1/32 source): two blocks per texel; window 3x3.
float sad_l2(ivec2 c, ivec2 v)
{
    float s = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 p = c + ivec2(x, y);
            s += abs(l2from(p) - l2to(p + v));
        }
    return s;
}
)GLSL";

		// The serial search (one thread per block): kept as the reference the
		// parallel search must reproduce exactly (SetSerialSearchForTest).
		const char* const SEARCH_BODY_SERIAL = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    // 11a E4: a static scene keeps the previous frame's vectors.
    if (static_scene()) {
        imageStore(dst, b, texelFetch(previous, b, 0));
        return;
    }
    half_size_ = textureSize(from_half, 0);
    l1_size_ = textureSize(from_l1, 0);
    l2_size_ = textureSize(from_l2, 0);
    ivec2 o = b * 8;

    // Hierarchical: each level refines the one above with a margin that
    // covers its quantisation (one texel above is 4 texels here).
    // Level 2 (1/32 source): every vector within +-range texels.
    ivec2 v2 = ivec2(0);
    float best2 = sad_l2(b / 2, v2);
    for (int dy = -coarse_range; dy <= coarse_range; dy++)
        for (int dx = -coarse_range; dx <= coarse_range; dx++) {
            float s = sad_l2(b / 2, ivec2(dx, dy));
            if (s < best2) { best2 = s; v2 = ivec2(dx, dy); }
        }
    // Level 1 (1/8 source), in half-texel steps: the best within +-3 of
    // zero, and the best within +-3 of the level-2 result. Both go down to
    // the next level, so a poor coarse guess (small pictures, flat areas)
    // cannot hide the true motion; the half steps keep the estimate within
    // the next level's +-3.
    vec2 v1z = vec2(0.0);
    float best1z = sad_l1(b * 2, v1z);
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            vec2 v = 0.5 * vec2(dx, dy);
            float s = sad_l1(b * 2, v);
            if (s < best1z) { best1z = s; v1z = v; }
        }
    vec2 v1c = vec2(v2 * 4);
    float best1c = sad_l1(b * 2, v1c);
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            vec2 v = vec2(v2 * 4) + 0.5 * vec2(dx, dy);
            float s = sad_l1(b * 2, v);
            if (s < best1c) { best1c = s; v1c = v; }
        }
    // D1: the best within +-3 of the frame's global vector, when it is
    // confident (a pan beyond the coarse range still starts near the truth).
    bool have_g = false;
    vec2 v1g = vec2(0.0), vgf = vec2(0.0);
    if (use_global != 0) {
        vec4 gv = texelFetch(global_vec, ivec2(0), 0);
        if (gv.w > 0.5 && gv.z < 0.9) {
            have_g = true;
            vgf = gv.xy;
            v1g = gv.xy;
            float best1g = sad_l1(b * 2, v1g);
            for (int dy = -6; dy <= 6; dy++)
                for (int dx = -6; dx <= 6; dx++) {
                    vec2 v = gv.xy + 0.5 * vec2(dx, dy);
                    float s = sad_l1(b * 2, v);
                    if (s < best1g) { best1g = s; v1g = v; }
                }
        }
    }

    // Half resolution, whole pixels: +-3 around both level-1 results and +-2
    // around zero; the exact match decides. D1 adds the block's refinement of
    // the global vector and the global vector itself (the 1/8 level can drift
    // most of a texel on fine texture; the global vector is exact on a pan).
    ivec2 best = ivec2(0);
    float best_sad = sad(o, best);
    // 11a E1: the previous frame's vector of this block (half-resolution
    // pixels) as one more start.
    vec2 vpt = use_temporal != 0 ? texelFetch(previous, b, 0).xy * 0.25 : vec2(0.0);
    for (int c = 0; c < 5; c++) {
        if (c == 1 && v1c == v1z)
            continue;
        if (c == 2 && (!have_g || v1g == v1z || v1g == v1c))
            continue;
        if (c == 3 && (!have_g || vgf == v1g || vgf == v1z || vgf == v1c))
            continue;
        if (c == 4 && (use_temporal == 0 || vpt == v1z || vpt == v1c))
            continue;
        ivec2 centre = ivec2(round((c == 0 ? v1z : (c == 1 ? v1c : (c == 2 ? v1g : (c == 3 ? vgf : vpt)))) * 4.0));
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                ivec2 v = centre + ivec2(dx, dy);
                float s = sad(o, v);
                if (s < best_sad) { best_sad = s; best = v; }
            }
    }
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            ivec2 v = ivec2(dx, dy);
            float s = sad(o, v);
            if (s < best_sad) { best_sad = s; best = v; }
        }

    // Half-pixel refinement (bilinear samples of `to`).
    vec2 fbest = vec2(best);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            vec2 v = vec2(best) + 0.5 * vec2(dx, dy);
            float s = sad_sub(o, v);
            if (s < best_sad) { best_sad = s; fbest = v; }
        }
    imageStore(dst, b, vec4(fbest, best_sad / 144.0, 1.0));
}
)GLSL";

		// One work group of 128 threads (SEARCH_NT) per block (owner 2026-10-13: the serial
		// search was 95 ms per 4K frame on an RTX 3050 Ti). Each level's
		// candidates are evaluated in parallel; every sum runs in the serial
		// order and thread 0 picks each winner in the serial scan order with
		// the serial strict comparison, so the field is exactly the serial
		// search's (test: SetSerialSearchForTest). Every barrier is at the top
		// level and reached by all threads (D3D11's HLSL compiler, X3663).
		const char* const SEARCH_PARALLEL_HEADER = R"GLSL(
shared float l1_sads_z[169];
shared float l1_sads_c[169];
shared float l1_sads_g[169];
// The 1/8 level's bilinear samples of `to` around each start (2026-10-15):
// every candidate's sample positions lie on the half-texel lattice
// o1 + start + 0.5 m, m in [-8, 10]^2, so each block samples them once
// (19 x 19 per start) instead of 16 per candidate. The positions are
// multiples of 0.5 (exact in float) and the same texture() calls, so every
// SAD is the same sum of the same values in the same order.
shared float l1_tab[4 * 361];

float sad_l1_tab(int k, ivec2 o1, int dx, int dy)
{
    float s = 0.0;
    for (int y = -1; y < 3; y++)
        for (int x = -1; x < 3; x++) {
            ivec2 p = o1 + ivec2(x, y);
            s += abs(l1from(p) - l1_tab[k * 361 + (2 * y + dy + 8) * 19 + 2 * x + dx + 8]);
        }
    return s;
}
shared float from_win[144];
// The start each candidate belongs to (1..5, 0 = the zero vector, ZGROUP =
// the +-2 around zero) and the starts' centres: a candidate inside an earlier
// start's window repeats an earlier one (owner 2026-10-13, shaky camera).
#ifdef GLOBAL_TWO
// M2: a sixth start, D1's second candidate.
#define ZGROUP 7
#define NHALF 321
shared float l1_sads_g2[169];
shared vec2 s_v1g2;
#else
#define ZGROUP 6
#define NHALF 272
#endif
shared ivec2 s_centres[ZGROUP];

// Candidate i of the half-size list in the serial order: zero, the 7x7
// around each start (dy outer, dx inner), then the 5x5 around zero.
void half_cand(int i, out ivec2 v, out int start)
{
    int ns = s_centres[0].x;
    int j = i - 1;
    if (i == 0) {
        v = ivec2(0);
        start = 0;
    } else if (j < 49 * ns) {
        start = j / 49 + 1;
        int d = j % 49;
        v = s_centres[start] + ivec2(d % 7 - 3, d / 7 - 3);
    } else {
        j -= 49 * ns;
        start = ZGROUP;
        v = ivec2(j % 5 - 2, j / 5 - 2);
    }
}

shared float sub_sads[9];
shared ivec2 s_v2;
shared vec2 s_v1z;
shared vec2 s_v1c;
shared vec2 s_v1g;
shared int s_half_count;
shared ivec2 s_best;

void sync_group()
{
    memoryBarrierShared();
    barrier();
}

// Picks without one thread scanning every SAD: each thread keeps the best of
// its own candidates, 16 threads reduce 16 of those each, thread 0 the last
// 16. The serial pick (strictly smaller, in order, from a given first
// candidate) is the minimum SAD with ties to the first candidate, then to the
// lowest index; rank -1 marks the first candidate. That order is total, so
// the result is exactly the serial pick in any reduction order.
#define PK_EMPTY 0x7fffffff
shared float pk_s[SEARCH_NT];
shared int pk_r[SEARCH_NT];
shared float pk2_s[16];
shared int pk2_r[16];

bool pk_better(float s, int r, float bs, int br)
{
    return r != PK_EMPTY && (br == PK_EMPTY || s < bs || (s == bs && r < br));
}

// After pk_s/pk_r are written and the group synced: 16 partial picks.
void pk_reduce16(int li)
{
    if (li < 16) {
        float bs = 0.0;
        int br = PK_EMPTY;
        for (int k = 0; k < SEARCH_NT / 16; k++) {
            int j = li * (SEARCH_NT / 16) + k;
            if (pk_better(pk_s[j], pk_r[j], bs, br)) { bs = pk_s[j]; br = pk_r[j]; }
        }
        pk2_s[li] = bs;
        pk2_r[li] = br;
    }
}

// After pk_reduce16 and a sync (thread 0): the winner's rank and SAD.
int pk_final(out float best)
{
    float bs = 0.0;
    int br = PK_EMPTY;
    for (int k = 0; k < 16; k++)
        if (pk_better(pk2_s[k], pk2_r[k], bs, br)) { bs = pk2_s[k]; br = pk2_r[k]; }
    best = bs;
    return br;
}

// The block's 12x12 window of `from`, from shared memory (the same values
// as from_at()).
float sad_win(ivec2 o, ivec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_win[(y + 2) * 12 + x + 2] - to_at(p + v));
        }
    return s;
}

float sad_sub_win(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_win[(y + 2) * 12 + x + 2] - to_lin(vec2(p) + v));
        }
    return s;
}
)GLSL";

		const char* const SEARCH_BODY = R"GLSL(
ivec2 b = ivec2(gl_WorkGroupID.xy);
ivec2 blocks = imageSize(dst);
int li = int(gl_LocalInvocationIndex);
const int NT = SEARCH_NT;
bool lead = li == 0;
bool inside = b.x < blocks.x && b.y < blocks.y;
// 11a E4: a static scene keeps the previous frame's vectors.
bool keep = static_scene();
bool run = inside && !keep;
if (lead && inside && keep)
    imageStore(dst, b, texelFetch(previous, b, 0));
half_size_ = textureSize(from_half, 0);
l1_size_ = textureSize(from_l1, 0);
l2_size_ = textureSize(from_l2, 0);
ivec2 o = b * 8;

// Level 2 (1/32 source): every vector within +-range texels.
int side = 2 * coarse_range + 1;
int n2 = side * side;
{
    // The serial pick starts from the zero vector (rank -1).
    int centre = coarse_range * side + coarse_range;
    float bs = 0.0;
    int br = PK_EMPTY;
    if (run)
        for (int i = li; i < n2; i += NT) {
            float s = sad_l2(b / 2, ivec2(i % side - coarse_range, i / side - coarse_range));
            int r = i == centre ? -1 : i;
            if (pk_better(s, r, bs, br)) { bs = s; br = r; }
        }
    pk_s[li] = bs;
    pk_r[li] = br;
}
sync_group();
pk_reduce16(li);
sync_group();
if (run && lead) {
    float best2;
    int r = pk_final(best2);
    s_v2 = r < 0 ? ivec2(0) : ivec2(r % side - coarse_range, r / side - coarse_range);
}
sync_group();
ivec2 v2 = s_v2;

// Level 1 (1/8 source), half-texel steps, +-3 around zero, around the level-2
// result and (D1) around a confident global vector; one thread per candidate
// of each search.
bool have_g = false;
vec2 vgf = vec2(0.0);
if (use_global != 0) {
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
#ifdef GLOBAL_TWO
    if (gv.w > 0.5) {   // M2: a start whether or not D1 is confident
#else
    if (gv.w > 0.5 && gv.z < 0.9) {
#endif
        have_g = true;
        vgf = gv.xy;
    }
}
#ifdef GLOBAL_TWO
bool have_g2 = false;
vec2 vg2 = vec2(0.0);
{
    vec4 g2 = texelFetch(global_two, ivec2(0), 0);
    if (g2.w > 0.5 && !(have_g && g2.xy == vgf)) {
        have_g2 = true;
        vg2 = g2.xy;
    }
}
#endif
// The starts' sample tables (0 zero, 1 the 1/32 result, 2 the global
// vector, 3 M2's second), then each candidate's SAD from them.
if (run)
    for (int e = li; e < 4 * 361; e += NT) {
        int k = e / 361;
        bool wanted = k < 2 || (k == 2 && have_g);
#ifdef GLOBAL_TWO
        wanted = wanted || (k == 3 && have_g2);
#endif
        if (!wanted)
            continue;
        int m = e % 361;
        vec2 start = k == 0 ? vec2(0.0) : k == 1 ? vec2(v2 * 4) : k == 2 ? vgf :
#ifdef GLOBAL_TWO
            vg2;
#else
            vec2(0.0);
#endif
        l1_tab[e] = l1to(vec2(b * 2) + start + 0.5 * vec2(m % 19 - 8, m / 19 - 8));
    }
sync_group();
if (run)
    for (int i = li; i < 169; i += NT) {
        int dx = i % 13 - 6, dy = i / 13 - 6;
        l1_sads_z[i] = sad_l1_tab(0, b * 2, dx, dy);
        l1_sads_c[i] = sad_l1_tab(1, b * 2, dx, dy);
        if (have_g)
            l1_sads_g[i] = sad_l1_tab(2, b * 2, dx, dy);
#ifdef GLOBAL_TWO
        if (have_g2)
            l1_sads_g2[i] = sad_l1_tab(3, b * 2, dx, dy);
#endif
    }
sync_group();
if (run && li < 13) {
    float bz = 0.0, bc = 0.0, bg = 0.0;
    int rz = PK_EMPTY, rc = PK_EMPTY, rg = PK_EMPTY;
#ifdef GLOBAL_TWO
    float bg2 = 0.0;
    int rg2 = PK_EMPTY;
#endif
    for (int k = 0; k < 13; k++) {
        int i = li * 13 + k;
        int r = i == 84 ? -1 : i;
        if (pk_better(l1_sads_z[i], r, bz, rz)) { bz = l1_sads_z[i]; rz = r; }
        if (pk_better(l1_sads_c[i], r, bc, rc)) { bc = l1_sads_c[i]; rc = r; }
        if (have_g && pk_better(l1_sads_g[i], r, bg, rg)) { bg = l1_sads_g[i]; rg = r; }
#ifdef GLOBAL_TWO
        if (have_g2 && pk_better(l1_sads_g2[i], r, bg2, rg2)) { bg2 = l1_sads_g2[i]; rg2 = r; }
#endif
    }
    pk_s[li] = bz; pk_r[li] = rz;
    pk_s[13 + li] = bc; pk_r[13 + li] = rc;
    pk_s[26 + li] = bg; pk_r[26 + li] = rg;
#ifdef GLOBAL_TWO
    pk_s[39 + li] = bg2; pk_r[39 + li] = rg2;
#endif
}
sync_group();
if (run && lead) {
    int rr[4];
    for (int j = 0; j < 4; j++) {
        float bs = 0.0;
        int br = PK_EMPTY;
        for (int k = 0; k < 13; k++)
            if (pk_better(pk_s[j * 13 + k], pk_r[j * 13 + k], bs, br)) { bs = pk_s[j * 13 + k]; br = pk_r[j * 13 + k]; }
        rr[j] = br;
    }
    // Rank -1 (the centre) and an empty list (no global start) keep the
    // serial pick's first candidate: the start itself.
#define L1_STEP(r) ((r) < 0 || (r) == PK_EMPTY ? vec2(0.0) : 0.5 * vec2((r) % 13 - 6, (r) / 13 - 6))
    vec2 v1z = L1_STEP(rr[0]);
    vec2 v1c = vec2(v2 * 4) + L1_STEP(rr[1]);
    vec2 v1g = vgf + (have_g ? L1_STEP(rr[2]) : vec2(0.0));
#ifdef GLOBAL_TWO
    vec2 v1g2 = vg2 + (have_g2 ? L1_STEP(rr[3]) : vec2(0.0));
#endif
    s_v1z = v1z;
    s_v1c = v1c;
    s_v1g = have_g ? v1g : vec2(0.0);

    // Half resolution, whole pixels: the candidate list in the serial
    // order - zero first, +-3 around each start (zero-level, level-2,
    // global refinement, global vector, 11a E1's previous vector), then
    // +-2 around zero.
    vec2 vpt = use_temporal != 0 ? texelFetch(previous, b, 0).xy * 0.25 : vec2(0.0);
    int nstarts = 0;
    for (int c = 0; c < 5; c++) {
        if (c == 1 && v1c == v1z)
            continue;
        if (c == 2 && (!have_g || v1g == v1z || v1g == v1c))
            continue;
        if (c == 3 && (!have_g || vgf == v1g || vgf == v1z || vgf == v1c))
            continue;
        if (c == 4 && (use_temporal == 0 || vpt == v1z || vpt == v1c))
            continue;
        ivec2 centre = ivec2(round((c == 0 ? v1z : (c == 1 ? v1c : (c == 2 ? v1g : (c == 3 ? vgf : vpt)))) * 4.0));
        s_centres[++nstarts] = centre;
    }
#ifdef GLOBAL_TWO
    if (have_g2 && v1g2 != v1z && v1g2 != v1c && !(have_g && (v1g2 == v1g || v1g2 == vgf))) {
        ivec2 centre = ivec2(round(v1g2 * 4.0));
        s_centres[++nstarts] = centre;
    }
#endif
    s_centres[0] = ivec2(nstarts, 0);   // the number of starts
    s_half_count = 1 + 49 * nstarts + 25;
}
if (run)
    for (int i = li; i < 144; i += NT)
        from_win[i] = from_at(o + ivec2(i % 12 - 2, i / 12 - 2));
sync_group();
int nh = s_half_count;
{
float bs = 0.0;
int br = PK_EMPTY;
if (run)
    for (int i = li; i < nh; i += NT) {
        // A repeat of an earlier candidate scores the same and comes later,
        // so with the strict comparison it never wins: not scored (each
        // thread checks its own; exactly the serial result).
        ivec2 v;
        int start;
        half_cand(i, v, start);
        bool repeat = i > 0 && v == ivec2(0);
        int earlier = start == ZGROUP ? s_centres[0].x : start - 1;
        for (int k = 1; k <= earlier; k++)
            repeat = repeat || (abs(v.x - s_centres[k].x) <= 3 && abs(v.y - s_centres[k].y) <= 3);
        float s = repeat ? 3.0e38 : sad_win(o, v);
        if (pk_better(s, i, bs, br)) { bs = s; br = i; }
    }
pk_s[li] = bs;
pk_r[li] = br;
}
sync_group();
pk_reduce16(li);
sync_group();
float best_sad = 0.0;
ivec2 best = ivec2(0);
if (run && lead) {
    int r = pk_final(best_sad);
    int start;
    half_cand(r, best, start);
    s_best = best;
}
sync_group();
// Half-pixel refinement (bilinear samples of `to`).
if (run && li < 9 && li != 4)
    sub_sads[li] = sad_sub_win(o, vec2(s_best) + 0.5 * vec2(li % 3 - 1, li / 3 - 1));
sync_group();
if (run && lead) {
    vec2 fbest = vec2(best);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            float s = sub_sads[(dy + 1) * 3 + dx + 1];
            if (s < best_sad) { best_sad = s; fbest = vec2(best) + 0.5 * vec2(dx, dy); }
        }
    imageStore(dst, b, vec4(fbest, best_sad / 144.0, 1.0));
}
)GLSL";


		// 11a: sub-pixel SAD on half-resolution luma (the search header's).
		const char* const SUBPEL_HEADER = R"GLSL(
ivec2 half_size_;
float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
float sad_sub(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + v));
        }
    return s;
}
)GLSL";

		// 11a E4, as in the search header.
		const char* const GATE_GLSL = R"GLSL(
bool static_scene()
{
    if (use_gate == 0)
        return false;
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
    float prev_mean = texelFetch(prev_trust, ivec2(0), 0).b;
    return gv.w > 0.5 && gv.z < 0.9 && length(gv.xy) * 8.0 < motion_lo && prev_mean < motion_lo;
}
)GLSL";

		const char* const MEDIAN_GLSL = R"GLSL(
float median_(float a[9], int n)
{
    for (int i = 1; i < 9; i++) {
        if (i >= n) break;
        float x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
    return a[n / 2];
}
)GLSL";

		// 11a E1: the second sweep. Candidates in half-resolution pixels: the
		// block's own first-sweep result, its eight neighbours', its previous-
		// frame vector, the global vector; score SAD per sample + lambda x the
		// distance (source pixels) from the neighbours' median. Then half-pel.
		const char* const REFINE_BODY_SERIAL = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    if (static_scene()) {
        imageStore(dst, b, texelFetch(previous, b, 0));
        return;
    }
    half_size_ = textureSize(from_half, 0);
    ivec2 o = b * 8;
    vec2 cand[12];
    float xs[9];
    float ys[9];
    int nc = 0, nn = 0;
    cand[nc++] = texelFetch(sweep, b, 0).xy;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            ivec2 c = b + ivec2(dx, dy);
            if (c.x < 0 || c.y < 0 || c.x >= blocks.x || c.y >= blocks.y) continue;
            vec2 v = texelFetch(sweep, c, 0).xy;
            cand[nc++] = v;
            xs[nn] = v.x;
            ys[nn] = v.y;
            nn++;
        }
    if (use_temporal != 0)
        cand[nc++] = texelFetch(previous, b, 0).xy;
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
    if (use_global != 0 && gv.w > 0.5 && gv.z < 0.9)
        cand[nc++] = gv.xy * 4.0;
    if (use_of != 0)
        cand[nc++] = texelFetch(of_vec, b, 0).xy;
    vec2 med = nn > 0 ? vec2(median_(xs, nn), median_(ys, nn)) : cand[0];
    vec2 best = cand[0];
    float best_sad = sad_sub(o, best);
    float best_score = best_sad / 144.0 + lambda * 2.0 * length(best - med);
    for (int k = 1; k < 12; k++) {
        if (k >= nc) break;
        float s = sad_sub(o, cand[k]);
        float score = s / 144.0 + lambda * 2.0 * length(cand[k] - med);
        if (score < best_score) { best_score = score; best_sad = s; best = cand[k]; }
    }
    vec2 fbest = best;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            vec2 v = best + 0.5 * vec2(dx, dy);
            float s = sad_sub(o, v);
            if (s < best_sad) { best_sad = s; fbest = v; }
        }
    imageStore(dst, b, vec4(fbest, best_sad / 144.0, 1.0));
}
)GLSL";

		// 11a E3: one quarter-pel step (half-resolution 0.25 = half a source
		// pixel) on trusted blocks of the field (source pixels).
		const char* const QPEL_BODY_SERIAL = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = texelFetch(raw, b, 0);
    if (f.z > 0.5) {
        half_size_ = textureSize(from_half, 0);
        ivec2 o = b * 8;
        vec2 v0 = f.xy * 0.5;
        vec2 best = v0;
        float best_sad = sad_sub(o, v0);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                vec2 v = v0 + 0.25 * vec2(dx, dy);
                float s = sad_sub(o, v);
                if (s < best_sad) { best_sad = s; best = v; }
            }
        f.xy = best * 2.0;
    }
    imageStore(dst, b, f);
}
)GLSL";

		// The second sweep and the quarter-pel step with one 32-thread work
		// group per block (the candidates in parallel, the block's window in
		// shared memory); the serial versions above are the references
		// (SetSerialSearchForTest). Winners chosen by thread 0 in the serial
		// order with the serial comparisons: exactly the serial results.
		const char* const PERBLOCK_HEADER = R"GLSL(
shared float pb_from[144];
shared float pb_sads[16];
shared vec2 pb_best;
shared float pb_best_sad;

void pb_sync()
{
    memoryBarrierShared();
    barrier();
}

float pb_sad_sub(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(pb_from[(y + 2) * 12 + x + 2] - to_lin(vec2(p) + v));
        }
    return s;
}

void pb_load_from(ivec2 o, int li, bool run)
{
    if (run)
        for (int i = li; i < 144; i += 32)
            pb_from[i] = from_at(o + ivec2(i % 12 - 2, i / 12 - 2));
}
)GLSL";

		// Two blocks per 32-thread group (2026-10-15): threads 0-15 the left
		// block, 16-31 the right; each block's candidates, sums and picks as
		// before (the serial results exactly).
		const char* const REFINE_PAIR_HEADER = R"GLSL(
shared float rf_from[288];
shared float rf_sads[32];
shared vec2 rf_best[2];
shared float rf_best_sad[2];

float rf_sad_sub(int sub, ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(rf_from[sub * 144 + (y + 2) * 12 + x + 2] - to_lin(vec2(p) + v));
        }
    return s;
}
)GLSL";

		const char* const REFINE_BODY = R"GLSL(
int sub = int(gl_LocalInvocationIndex) / 16;
int li = int(gl_LocalInvocationIndex) % 16;
ivec2 b = ivec2(int(gl_WorkGroupID.x) * 2 + sub, int(gl_WorkGroupID.y));
ivec2 blocks = imageSize(dst);
bool lead = li == 0;
bool inside = b.x < blocks.x && b.y < blocks.y;
bool keep = static_scene();
bool run = inside && !keep;
if (lead && inside && keep)
    imageStore(dst, b, texelFetch(previous, b, 0));
half_size_ = textureSize(from_half, 0);
ivec2 o = b * 8;
// The candidate list and the neighbours' median, as the serial sweep (every
// thread of the block builds the same list).
vec2 cand[12];
float xs[9];
float ys[9];
int nc = 0, nn = 0;
cand[nc++] = inside ? texelFetch(sweep, b, 0).xy : vec2(0.0);
for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
        if (dx == 0 && dy == 0) continue;
        ivec2 c = b + ivec2(dx, dy);
        if (c.x < 0 || c.y < 0 || c.x >= blocks.x || c.y >= blocks.y) continue;
        vec2 v = texelFetch(sweep, c, 0).xy;
        cand[nc++] = v;
        xs[nn] = v.x;
        ys[nn] = v.y;
        nn++;
    }
if (use_temporal != 0)
    cand[nc++] = texelFetch(previous, b, 0).xy;
vec4 gv = texelFetch(global_vec, ivec2(0), 0);
if (use_global != 0 && gv.w > 0.5 && gv.z < 0.9)
    cand[nc++] = gv.xy * 4.0;
if (use_of != 0 && inside)
    cand[nc++] = texelFetch(of_vec, b, 0).xy;
if (run)
    for (int i = li; i < 144; i += 16)
        rf_from[sub * 144 + i] = from_at(o + ivec2(i % 12 - 2, i / 12 - 2));
pb_sync();
if (run && li < nc)
    rf_sads[sub * 16 + li] = rf_sad_sub(sub, o, cand[li]);
pb_sync();
if (run && lead) {
    vec2 med = nn > 0 ? vec2(median_(xs, nn), median_(ys, nn)) : cand[0];
    vec2 best = cand[0];
    float best_sad = rf_sads[sub * 16];
    float best_score = best_sad / 144.0 + lambda * 2.0 * length(best - med);
    for (int k = 1; k < 12; k++) {
        if (k >= nc) break;
        float s = rf_sads[sub * 16 + k];
        float score = s / 144.0 + lambda * 2.0 * length(cand[k] - med);
        if (score < best_score) { best_score = score; best_sad = s; best = cand[k]; }
    }
    rf_best[sub] = best;
    rf_best_sad[sub] = best_sad;
}
pb_sync();
vec2 best = rf_best[sub];
if (run && li < 9 && li != 4)
    rf_sads[sub * 16 + li] = rf_sad_sub(sub, o, best + 0.5 * vec2(li % 3 - 1, li / 3 - 1));
pb_sync();
if (run && lead) {
    float best_sad = rf_best_sad[sub];
    vec2 fbest = best;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            float s = rf_sads[sub * 16 + (dy + 1) * 3 + dx + 1];
            if (s < best_sad) { best_sad = s; fbest = best + 0.5 * vec2(dx, dy); }
        }
    imageStore(dst, b, vec4(fbest, best_sad / 144.0, 1.0));
}
)GLSL";

		const char* const QPEL_BODY = R"GLSL(
ivec2 b = ivec2(gl_WorkGroupID.xy);
ivec2 blocks = imageSize(dst);
int li = int(gl_LocalInvocationIndex);
bool lead = li == 0;
bool inside = b.x < blocks.x && b.y < blocks.y;
vec4 f = inside ? texelFetch(raw, b, 0) : vec4(0.0);
bool run = inside && f.z > 0.5;
half_size_ = textureSize(from_half, 0);
ivec2 o = b * 8;
vec2 v0 = f.xy * 0.5;
pb_load_from(o, li, run);
pb_sync();
if (run && li < 9)
    pb_sads[li] = pb_sad_sub(o, v0 + 0.25 * vec2(li % 3 - 1, li / 3 - 1));
pb_sync();
if (inside && lead) {
    if (run) {
        vec2 best = v0;
        float best_sad = pb_sads[4];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                float s = pb_sads[(dy + 1) * 3 + dx + 1];
                if (s < best_sad) { best_sad = s; best = v0 + 0.25 * vec2(dx, dy); }
            }
        f.xy = best * 2.0;
    }
    imageStore(dst, b, f);
}
)GLSL";


		// Delta 9 H2: hold hysteresis per block (or sub-block). State: r the
		// effective trust, g frames the raw trust has disagreed, ba the last
		// trusted vector (a block still warping through its hysteresis keeps
		// it). A cut (frame trust below the minimum) holds everything at once.
		const char* const HYST_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (b.x < size.x && b.y < size.y) {
    vec4 f = texelFetch(src_field, b, 0);
    vec4 st = texelFetch(state_in, b, 0);
    bool cut = texelFetch(frame_trust, ivec2(0), 0).r < min_share;
    float raw = f.z > 0.5 ? 1.0 : 0.0;
    float eff = st.r, count = st.g;
    vec2 last = raw > 0.5 ? f.xy : st.ba;
    if (cut) { eff = 0.0; count = 0.0; }
    else if (raw == eff) count = 0.0;
    else {
        count += 1.0;
        if (count >= hysteresis) { eff = raw; count = 0.0; }
    }
    imageStore(state_out, b, vec4(eff, count, last));
    imageStore(dst, b, vec4(eff > 0.5 && raw < 0.5 ? st.ba : f.xy, eff, f.w));
}
)GLSL";

		const char* const COPY_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (b.x < size.x && b.y < size.y)
    imageStore(dst, b, texelFetch(src, b, 0));
)GLSL";

		const char* const COPY_PAIR_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst0);
if (b.x < size.x && b.y < size.y) {
    imageStore(dst0, b, texelFetch(src0, b, 0));
    imageStore(dst1, b, texelFetch(src1, b, 0));
}
)GLSL";

		// D1: the whole frame's translation, by one 16x16 group. Every vector
		// within +-8 texels at 1/32 resolution (the mean absolute difference
		// over the overlap, strided to about 2048 samples), then +-2 texels in
		// half steps at 1/8 around the winner. z = best / second best (outside
		// +-1 of the best): near 1 on flat or ambiguous pictures.
		const char* const GLOBAL_HEADER = R"GLSL(
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

		const char* const GLOBAL_BODY = R"GLSL(
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
		const char* const GLOBAL_SUM_HEADER = R"GLSL(
shared float gs_s_[256];
shared float gs_n_[256];
)GLSL";

		const char* const GLOBAL_L2_BODY = R"GLSL(
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

		const char* const GLOBAL_PICK_HEADER = R"GLSL(
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
		const char* const GLOBAL_PICK2_BODY = R"GLSL(
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
		const char* const GLOBAL_L1_BODY = R"GLSL(
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
		const char* const GLOBAL_PICK1_BODY = R"GLSL(
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
		const char* const GLOBAL_BOTH_HEADER = R"GLSL(
shared float gs_s_[256];
shared float gs_n_[256];
)GLSL";

		// The 1/32 stage's reads (its own header: the 1/8 stage binds other textures).
		const char* const GLOBAL_BOTH_L2_HEADER = R"GLSL(
// Direction d: 0 = A to B (from A), 1 = B to A (from B).
float from_l2_at(int d, ivec2 p) { return d == 0 ? texelFetch(l2_a, p, 0).r : texelFetch(l2_b, p, 0).r; }
float to_l2_at(int d, ivec2 p) { return d == 0 ? texelFetch(l2_b, p, 0).r : texelFetch(l2_a, p, 0).r; }
)GLSL";

		const char* const GLOBAL_BOTH_L2_BODY = R"GLSL(
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

		const char* const GLOBAL_BOTH_PICK2_BODY = R"GLSL(
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

		const char* const GLOBAL_BOTH_L1_BODY = R"GLSL(
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
    // Both sampled, then chosen: an implicit-LOD sample may not sit in a
    // branch (D3D11's HLSL compiler).
    float fa = texelFetch(l1_a, p, 0).r, fb = texelFetch(l1_b, p, 0).r;
    float ta = texture(l1_a, t).r, tb = texture(l1_b, t).r;
    float a = d == 0 ? fa : fb;
    float b = d == 0 ? tb : ta;
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

		const char* const GLOBAL_BOTH_PICK1_BODY = R"GLSL(
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

		// D3: median smoothing of the trusted field (see Settings).
		const char* const SMOOTH_HEADER = R"GLSL(
ivec2 half_size_;
float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
float sad_sub(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + v));
        }
    return s;
}
float median_(float a[9], int n)
{
    for (int i = 1; i < 9; i++) {
        if (i >= n) break;
        float x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
    return a[n / 2];
}
)GLSL";

		const char* const SMOOTH_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    half_size_ = textureSize(from_half, 0);
    vec4 f = texelFetch(raw, b, 0);
    float xs[9];
    float ys[9];
    int n = 0, neighbours = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 c = b + ivec2(dx, dy);
            if (c.x < 0 || c.y < 0 || c.x >= blocks.x || c.y >= blocks.y)
                continue;
            vec4 g = texelFetch(raw, c, 0);
            if (g.z > 0.5) {
                xs[n] = g.x;
                ys[n] = g.y;
                n++;
                if (dx != 0 || dy != 0) neighbours++;
            }
        }
    vec4 o = f;
    if (n >= 5) {
        vec2 med = vec2(median_(xs, n), median_(ys, n));
        if (f.z > 0.5) {
            if (length(f.xy - med) > 1.0)
                o.xy = med;   // an outlier: the neighbours' motion, still trusted
        } else if (neighbours >= 7) {
            float spread = 0.0;
            for (int k = 0; k < 9; k++)
                if (k < n) spread = max(spread, length(vec2(xs[k], ys[k]) - med));
            if (spread <= 0.5) {
                // Promote only when the block itself matches there (planner
                // answer 4: new content inside a moving region stays held).
                float e = sad_sub(b * 8, med * 0.5) / 144.0;
                if (e <= tolerance)
                    o = vec4(med, 1.0, e);
            }
        }
    }
    imageStore(dst, b, o);
}
)GLSL";

		// Trust: a good match whose backward vector (from where the block
		// lands in B) points back to it.
		const char* const CONSISTENCY_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = texelFetch(fwd, b, 0);
    vec2 landed = vec2(b * 8) + vec2(4.0) + f.xy;
    ivec2 bt = clamp(ivec2(floor(landed / 8.0)), ivec2(0), blocks - ivec2(1));
    vec4 g = texelFetch(bwd, bt, 0);
    bool consistent = length(f.xy + g.xy) <= consistency;
#ifdef TEXTURE_TOL
    // M1: the block's texture (mean absolute neighbour difference of A's
    // half-resolution luma) raises the error a correct match may have.
    ivec2 hs_ = textureSize(from_half, 0);
    float tex_ = 0.0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            ivec2 p = clamp(b * 8 + ivec2(x, y), ivec2(0), hs_ - ivec2(2));
            float c0 = texelFetch(from_half, p, 0).r;
            tex_ += abs(texelFetch(from_half, p + ivec2(1, 0), 0).r - c0) + abs(texelFetch(from_half, p + ivec2(0, 1), 0).r - c0);
        }
    float extra_ = texture_tol * tex_ / 128.0;
#else
    float extra_ = 0.0;
#endif
#ifdef TAPER
    bool good = f.z <= taper_sad + extra_;   // delta 9 H3: marginal blocks warp, tapered
#else
    bool good = f.z <= tolerance + extra_;
#endif
    float trust = (consistent && good) ? 1.0 : 0.0;
#ifdef PICTURE
    // Black bars: a block whose centre is outside the picture is untrusted.
    vec4 pa_r = texelFetch(picture, ivec2(0), 0);
    vec2 bc_ = vec2(b * 16) + vec2(8.0);
    if (bc_.x < pa_r.x || bc_.y < pa_r.y || bc_.x >= pa_r.z || bc_.y >= pa_r.w)
        trust = 0.0;
#endif
    imageStore(dst, b, vec4(f.xy * 2.0, trust, f.z));
}
)GLSL";

		// Occlusion for the dither (6c): the same consistency test from B's
		// side. A block of B whose content cannot be found in A, or whose
		// backward vector is not matched by A's forward vector, is new or
		// uncovered content: the sigma-delta must not carry a residual into
		// it on B's first refresh. 1 = seen before (carry allowed), 0 = new.
		const char* const OCCLUSION_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 g = texelFetch(bwd, b, 0);
    vec2 landed = vec2(b * 8) + vec2(4.0) + g.xy;
    ivec2 ft = clamp(ivec2(floor(landed / 8.0)), ivec2(0), blocks - ivec2(1));
    vec4 f = texelFetch(fwd, ft, 0);
#ifdef SPLIT
    // 11a E2: a split block of A answers with the sub-block it landed in.
    {
        ivec2 st = clamp(ivec2(floor(landed / 4.0)), ivec2(0), blocks * 2 - ivec2(1));
        vec4 sf = texelFetch(sub_field, st, 0);
        if (sf.w > 0.5)
            f.xy = sf.xy * 0.5;
    }
#endif
    bool consistent = length(g.xy + f.xy) <= consistency;
    bool good = g.z <= tolerance;
    imageStore(dst, b, vec4((consistent && good) ? 1.0 : 0.0, 0.0, 0.0, 1.0));
}
)GLSL";

		// Stage 8-3: the temporal grain measure per block of A, at the block's
		// own A->B vector (the field, in source pixels; half resolution here):
		// the differences over the block's 8x8, their mean removed (a fade, a
		// lighting change), then the mean absolute residual. Temporal noise
		// survives the motion compensation; static texture does not.
		const char* const GRAIN_ERROR_HEADER = R"GLSL(
ivec2 half_size_;
float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
)GLSL";

		const char* const GRAIN_ERROR_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    half_size_ = textureSize(from_half, 0);
    vec2 v = texelFetch(field, b, 0).xy * 0.5;
    ivec2 o = b * 8;
#ifdef SPLIT
    // 11a E2: a split block's quadrants at their own sub-block vectors.
    vec2 vq[4];
    for (int k = 0; k < 4; k++) {
        vec4 sf = texelFetch(sub_field, b * 2 + ivec2(k % 2, k / 2), 0);
        vq[k] = sf.w > 0.5 ? sf.xy * 0.5 : v;
    }
#endif
    float d[64];
    float mean_d = 0.0, mean_y = 0.0;
    for (int i = 0; i < 64; i++) {
        ivec2 p = o + ivec2(i % 8, i / 8);
        float a = from_at(p);
#ifdef SPLIT
        v = vq[(i % 8) / 4 + 2 * ((i / 8) / 4)];
#endif
        d[i] = a - to_lin(vec2(p) + v);
        mean_d += d[i];
        mean_y += a;
    }
    mean_d /= 64.0;
    float e = 0.0;
    for (int i = 0; i < 64; i++)
        e += abs(d[i] - mean_d);
    imageStore(dst, b, vec4(e / 64.0, mean_y / 64.0, 0.0, 1.0));
}
)GLSL";

		// Frame trust (6b-2): the share of trusted blocks, by one 16x16 group,
		// into a 1x1 texture. It stays on the GPU: the warp reads it, so a cut
		// between A and B holds the whole frame without a read-back.
		// D2/D4: the same reduction also sums the trusted |v| and writes the
		// strength scale (g), rate-limited against the last one (state_io).
		const char* const FRAME_TRUST_HEADER = R"GLSL(
shared float partial_[256];
shared float partial_v_[256];
shared float partial_n_[256];
#ifdef PICTURE
bool in_picture_(vec2 c) { vec4 r = texelFetch(picture, ivec2(0), 0); return c.x >= r.x && c.y >= r.y && c.x < r.z && c.y < r.w; }
#else
bool in_picture_(vec2 c) { return true; }
#endif
// Texels of a 1/scale level whose centres are inside the picture.
float picture_texels_(ivec2 ls, float scale)
{
#ifdef PICTURE
    vec4 r = texelFetch(picture, ivec2(0), 0);
    vec2 lo = clamp(ceil(r.xy / scale - vec2(0.5)), vec2(0.0), vec2(ls));
    vec2 hi = clamp(ceil(r.zw / scale - vec2(0.5)), vec2(0.0), vec2(ls));
    return max(hi.x - lo.x, 0.0) * max(hi.y - lo.y, 0.0);
#else
    return float(ls.x * ls.y);
#endif
}
#ifdef REGIONAL
shared uint hist_bins_[32];   // M3: A's 16 bins, then B's (integer counts)
#endif
#ifdef CAMERA_OUT
shared vec3 partial_c_[256];
#endif
)GLSL";

		const char* const FRAME_TRUST_BODY = R"GLSL(
uint i = gl_LocalInvocationIndex;
ivec2 blocks = textureSize(field, 0);
int count = blocks.x * blocks.y;
float sum = 0.0, sum_v = 0.0, sum_n = 0.0;
for (int k = int(i); k < count; k += 256) {
    ivec2 bk = ivec2(k % blocks.x, k / blocks.x);
    if (!in_picture_(vec2(bk * 16) + vec2(8.0)))
        continue;
    sum_n += 1.0;
    vec4 f = texelFetch(field, bk, 0);
    if (f.z > 0.5) { sum += 1.0; sum_v += length(f.xy); }
}
partial_[i] = sum;
partial_v_[i] = sum_v;
partial_n_[i] = sum_n;
barrier();
for (uint stride = 128u; stride > 0u; stride >>= 1) {
    if (i < stride) {
        partial_[i] += partial_[i + stride];
        partial_v_[i] += partial_v_[i + stride];
        partial_n_[i] += partial_n_[i + stride];
    }
    barrier();
}
#ifdef REGIONAL
// M3: the change of the 1/32 luma histograms (16 bins of 16 codes), L1 of
// the normalised histograms (0..2): a cut changes it, a hard pan does not.
float hist_d = 0.0;
ivec2 ls_ = textureSize(hist_a, 0);
int lc_ = ls_.x * ls_.y;
float hist_n = picture_texels_(ls_, 32.0);   // 1/32 texels inside the picture
// One sweep into shared integer bins (efficiency, owner 2026-10-14: was 16
// sweeps with a reduction each); the counts and so the result are the same.
if (i < 32u)
    hist_bins_[i] = 0u;
barrier();
for (int k = int(i); k < lc_; k += 256) {
    ivec2 p = ivec2(k % ls_.x, k / ls_.x);
    if (!in_picture_((vec2(p) + vec2(0.5)) * 32.0))
        continue;
    atomicAdd(hist_bins_[clamp(int(texelFetch(hist_a, p, 0).r / 16.0), 0, 15)], 1u);
    atomicAdd(hist_bins_[16 + clamp(int(texelFetch(hist_b, p, 0).r / 16.0), 0, 15)], 1u);
}
barrier();
if (i == 0u) {
    for (int j = 0; j < 16; j++)
        hist_d += abs(float(hist_bins_[j]) - float(hist_bins_[16 + j]));
    hist_d /= max(hist_n, 1.0);
}
#endif
if (i == 0u) {
    float share = partial_[0] / max(partial_n_[0], 1.0);
#ifdef REGIONAL
    bool cut = share < min_share && (hist_d > cut_hist || share < 0.05);
    float ramp_lo = 0.0;
#else
    bool cut = share < min_share;
    float ramp_lo = min_share;
#endif
    float mean_v = partial_[0] > 0.0 ? partial_v_[0] / partial_[0] : 0.0;
    float scale = 1.0;
#ifdef ROLLOFF
    const bool rolloff = true;
#else
    const bool rolloff = false;
#endif
#ifdef FADE
    bool fading = texelFetch(fade_state, ivec2(0), 0).z > 0.5;
#else
    const bool fading = false;
#endif
    if (motion_hi > 0.0 || trust_full > 0.0 || rolloff || fading) {
        float m = 1.0;
#ifdef FADE
        if (fading)
            m *= fade_floor;   // M4: a fade warps at most at the floor
#endif
        if (motion_hi > 0.0)
            m *= clamp((mean_v - motion_lo) / max(motion_hi - motion_lo, 1e-3), 0.0, 1.0);
#ifdef ROLLOFF
        // Delta 14 R-A: the fall above a fast pan, on the frame's motion.
        float frame_v = mean_v;
#ifdef ROLLOFF_GLOBAL
        vec4 gv = texelFetch(global_vec, ivec2(0), 0);
        if (gv.w > 0.5 && gv.z < 0.9)
            frame_v = length(gv.xy) * 8.0;
#endif
        m *= mix(1.0, roll_floor, smoothstep(roll_lo, max(roll_hi, roll_lo + 1e-3), frame_v));
#endif
        if (trust_full > 0.0)
            m *= clamp((share - ramp_lo) / max(trust_full - ramp_lo, 1e-3), 0.0, 1.0);
        vec4 prev = imageLoad(state_io, ivec2(0));
        float p = prev.w > 0.5 ? prev.x : m;   // no history yet: start where the frame is
        scale = cut ? 0.0 : clamp(m, p - rate, p + rate);
        imageStore(state_io, ivec2(0), vec4(scale, mean_v, share, 1.0));
    }
    // M3: a hard-motion frame that is not a cut reports at least min_share,
    // so the holds (warps, hysteresis, adaptive downscale) stay per block.
    imageStore(dst, ivec2(0), vec4(cut ? share : max(share, min_share), scale, mean_v, 1.0));
}
#ifdef CAMERA_OUT
// Delta 14 R-B: the camera, refined from D1's global vector (4-px steps) to
// the mean of the trusted blocks near it.
{
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
    bool reliable = gv.w > 0.5 && gv.z < 0.9;
    vec2 gpx = gv.xy * 8.0;
    vec3 c = vec3(0.0);
    for (int k = int(i); k < count; k += 256) {
        vec4 f = texelFetch(field, ivec2(k % blocks.x, k / blocks.x), 0);
        if (reliable && f.z > 0.5 && length(f.xy - gpx) < 6.0) c += vec3(f.xy, 1.0);
    }
    partial_c_[i] = c;
}
barrier();
for (uint stride = 128u; stride > 0u; stride >>= 1) {
    if (i < stride)
        partial_c_[i] += partial_c_[i + stride];
    barrier();
}
if (i == 0u) {
    vec3 c = partial_c_[0];
    imageStore(camera_out, ivec2(0), c.z > 0.0 ? vec4(c.xy / c.z, 1.0, 1.0) : vec4(0.0, 0.0, 0.0, 1.0));
}
#endif
)GLSL";

		// The vector at a source-luma position: bilinear between the four
		// nearest block centres, trusted blocks only; the position's own
		// block decides whether it is warped at all (untrusted: hold).
		const char* const WARP_HEADER = R"GLSL(
#ifdef OBJECT
#define field_vector field_vector_raw
#endif
bool field_vector(vec2 lp, out vec2 v)
{
    ivec2 fsize = textureSize(field, 0);
    ivec2 own = clamp(ivec2(floor(lp / 16.0)), ivec2(0), fsize - ivec2(1));
    v = vec2(0.0);
    // Too few trusted blocks (a cut between A and B): hold the whole frame.
    if (texelFetch(frame_trust, ivec2(0), 0).r < min_trusted_share)
        return false;
#ifdef SPLIT
    // 11a E2: inside a split block, the sub-block decides the hold and the
    // vector is bilinear between the trusted sub-block centres (8 px).
    {
        ivec2 ssize = textureSize(sub_field, 0);
        vec4 sown = texelFetch(sub_field, clamp(ivec2(floor(lp / 8.0)), ivec2(0), ssize - ivec2(1)), 0);
        if (sown.w > 0.5) {
            if (sown.z < 0.5)
                return false;
            vec2 sg = lp / 8.0 - vec2(0.5);
            ivec2 s0 = ivec2(floor(sg));
            vec2 st = sg - vec2(s0);
            float swsum = 0.0;
            for (int y = 0; y <= 1; y++)
                for (int x = 0; x <= 1; x++) {
                    vec4 sf = texelFetch(sub_field, clamp(s0 + ivec2(x, y), ivec2(0), ssize - ivec2(1)), 0);
                    float w = (x == 1 ? st.x : 1.0 - st.x) * (y == 1 ? st.y : 1.0 - st.y);
                    if (sf.z > 0.5) { v += w * sf.xy; swsum += w; }
                }
            if (swsum <= 0.0)
                return false;
            v /= swsum;
            return true;
        }
    }
#endif
    if (texelFetch(field, own, 0).z < 0.5)
        return false;
    vec2 g = lp / 16.0 - vec2(0.5);
    ivec2 g0 = ivec2(floor(g));
    vec2 t = g - vec2(g0);
    float wsum = 0.0;
    for (int y = 0; y <= 1; y++)
        for (int x = 0; x <= 1; x++) {
            ivec2 c = clamp(g0 + ivec2(x, y), ivec2(0), fsize - ivec2(1));
            vec4 f = texelFetch(field, c, 0);
            float w = (x == 1 ? t.x : 1.0 - t.x) * (y == 1 ? t.y : 1.0 - t.y);
            if (f.z > 0.5) { v += w * f.xy; wsum += w; }
        }
    if (wsum <= 0.0)
        return false;
    v /= wsum;
    return true;
}
#ifdef OBJECT
#undef field_vector
// Delta 14 R-B: the camera (MotionWarp::Camera(), source px) applied fully,
// the motion within the frame by object_strength. object_scale() is the same
// rule for M6's vectors (audit 2026-10-14: M6 ignored R-B).
vec2 object_scale(vec2 v)
{
    vec2 vg = vec2(0.0);
#ifdef CAMERA
    vec4 g = texelFetch(camera, ivec2(0), 0);
    if (g.z > 0.5)
        vg = g.xy;
#endif
    return vg + object_strength * (v - vg);
}
bool field_vector(vec2 lp, out vec2 v)
{
    bool ok = field_vector_raw(lp, v);
    if (ok)
        v = object_scale(v);
    return ok;
}
#endif

// D2/D4: the strength scale of this source frame (1 when both are off).
float strength_scale() { return texelFetch(frame_trust, ivec2(0), 0).g; }

// Delta 9 H3: a marginal block's strength, ramped down by its SAD.
#ifdef TAPER
float block_strength(vec2 lp)
{
    ivec2 fsize = textureSize(field, 0);
#ifdef SPLIT
    // A split block's sub-vectors are its own trust decision: not tapered.
    {
        ivec2 ssize = textureSize(sub_field, 0);
        if (texelFetch(sub_field, clamp(ivec2(floor(lp / 8.0)), ivec2(0), ssize - ivec2(1)), 0).w > 0.5)
            return 1.0;
    }
#endif
    float sad = texelFetch(field, clamp(ivec2(floor(lp / 16.0)), ivec2(0), fsize - ivec2(1)), 0).w;
    return sad <= taper_tol ? 1.0 : 1.0 - smoothstep(taper_tol, taper_sad, sad);
}
#else
float block_strength(vec2 lp) { return 1.0; }
#endif

// Delta 9 H1: 0 at the edge of a held neighbour block, 1 from `feather`
// source pixels away (smoothstep), on the warped side.
#ifdef FEATHER
float hold_weight(vec2 lp, float feather)
{
    ivec2 fsize = textureSize(field, 0);
    ivec2 own = clamp(ivec2(floor(lp / 16.0)), ivec2(0), fsize - ivec2(1));
    float d = 1e9;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            ivec2 c = own + ivec2(dx, dy);
            if (c.x < 0 || c.y < 0 || c.x >= fsize.x || c.y >= fsize.y) continue;
            bool held = texelFetch(field, c, 0).z < 0.5;
#ifdef SPLIT
            // A split block's parent is untrusted by construction; it is held
            // only where its own sub-blocks are (audit 2026-10-14).
            if (held && texelFetch(sub_field, c * 2, 0).w > 0.5) {
                held = false;
                for (int k = 0; k < 4; k++)
                    if (texelFetch(sub_field, c * 2 + ivec2(k % 2, k / 2), 0).z < 0.5) {
                        vec2 slo = vec2(c * 2 + ivec2(k % 2, k / 2)) * 8.0;
                        d = min(d, length(lp - clamp(lp, slo, slo + vec2(8.0))));
                    }
            }
#endif
            if (held) {
                vec2 lo = vec2(c) * 16.0;
                d = min(d, length(lp - clamp(lp, lo, lo + vec2(16.0))));
            }
        }
    return smoothstep(0.0, max(feather, 1e-3), d);
}
#endif
)GLSL";

		// 11a E2: the split, both directions. A block whose best search SAD
		// (per sample) exceeds split_sad is searched as four 8x8-source-pixel
		// sub-blocks on their own 4x4 half-resolution window (no margin: it
		// would reach into the neighbour the split exists to separate):
		// starts at the parent's search vector and its eight neighbours',
		// then whole half-resolution pixels +-1 (= +-2 source px) and half a
		// pixel. Output per sub-block: (vector, SAD per sample, 1); other
		// blocks repeat the parent search (vector, SAD, 0).
		const char* const SPLIT_SEARCH_BODY = R"GLSL(
float sad_q(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + v));
        }
    return s;
}

void main_split()
{
    ivec2 b = ivec2(gl_GlobalInvocationID.xy);
    ivec2 blocks = textureSize(search, 0);
    if (b.x >= blocks.x || b.y >= blocks.y)
        return;
    vec4 fs = texelFetch(search, b, 0);
    bool split = fs.z > split_sad;
    half_size_ = textureSize(from_half, 0);
    for (int q = 0; q < 4; q++) {
        ivec2 sb = b * 2 + ivec2(q % 2, q / 2);
        if (!split) {
            imageStore(dst, sb, vec4(fs.xyz, 0.0));
            continue;
        }
        ivec2 o = b * 8 + ivec2(q % 2, q / 2) * 4;
        vec2 best = fs.xy;
        float best_sad = sad_q(o, best);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                ivec2 c = clamp(b + ivec2(dx, dy), ivec2(0), blocks - ivec2(1));
                vec2 v = texelFetch(search, c, 0).xy;
                float s = sad_q(o, v);
                if (s < best_sad) { best_sad = s; best = v; }
            }
        vec2 centre = round(best);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                vec2 v = centre + vec2(dx, dy);
                float s = sad_q(o, v);
                if (s < best_sad) { best_sad = s; best = v; }
            }
        vec2 fbest = best;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                vec2 v = best + 0.5 * vec2(dx, dy);
                float s = sad_q(o, v);
                if (s < best_sad) { best_sad = s; fbest = v; }
            }
        imageStore(dst, sb, vec4(fbest, best_sad / 16.0, 1.0));
    }
}
)GLSL";

		// 11a E2: the sub-field. A split sub-block of A is trusted when its
		// match is good and B's sub-block where it lands points back (B's own
		// split sub-search, or its parent search repeated); unsplit blocks copy
		// the parent's final vector and trust with flag 0. Source pixels.
		const char* const SUB_TRUST_BODY = R"GLSL(
ivec2 sb = ivec2(gl_GlobalInvocationID.xy);
ivec2 subs = imageSize(dst);
if (sb.x < subs.x && sb.y < subs.y) {
    vec4 s = texelFetch(sub_fwd, sb, 0);
    if (s.w < 0.5) {
        vec4 pf = texelFetch(field, sb / 2, 0);
        imageStore(dst, sb, vec4(pf.xyz, 0.0));
    } else {
        vec2 landed = vec2(sb * 4) + vec2(2.0) + s.xy;
        ivec2 st = clamp(ivec2(floor(landed / 4.0)), ivec2(0), subs - ivec2(1));
        vec2 g = texelFetch(sub_bwd, st, 0).xy;
        bool consistent = length(s.xy + g) <= consistency;
        bool good = s.z <= tolerance;
        imageStore(dst, sb, vec4(s.xy * 2.0, (consistent && good) ? 1.0 : 0.0, 1.0));
    }
}
)GLSL";

		// D5: where a held position's block of B is new content, the position
		// in B (source luma pixels) that shows it at the warped time: B's own
		// A->B motion (minus its backward vector, when that matched) back from
		// B by 1 - shift. Needs `fill_occ`, `fill_bwd`, `fill_tolerance`.
		const char* const FILL_HEADER = R"GLSL(
bool fill_position(vec2 lp, float s, out vec2 pos)
{
    pos = lp;
    if (texelFetch(frame_trust, ivec2(0), 0).r < min_trusted_share)
        return false;   // a cut: B is another scene
    ivec2 bs = textureSize(fill_occ, 0);
    ivec2 own = clamp(ivec2(floor(lp / 16.0)), ivec2(0), bs - ivec2(1));
    if (texelFetch(fill_occ, own, 0).r > 0.5)
        return false;   // seen in A: the hold stays
    vec4 g = texelFetch(fill_bwd, own, 0);
    vec2 motion = g.z <= fill_tolerance ? -2.0 * g.xy : vec2(0.0);
    pos = lp + (1.0 - s) * motion;
    return true;
}
)GLSL";

		const char* const WARP_LUMA_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec2 c = vec2(q) + vec2(0.5);
    vec2 v;
    vec4 outv;
#ifdef PIXEL_TRUST
    // M6: every block's vector, blended with the hold by A's pixel weight.
    if (texelFetch(frame_trust, ivec2(0), 0).r >= min_trusted_share) {
        // The vector where the pixel comes from (two fixed-point steps: at a
        // moving edge the output position's own vector is the wrong one).
        float sc = shift * strength_scale();
        vec2 va = field_vector_all(c);
        va = field_vector_all(c - sc * va);
        va = field_vector_all(c - sc * va);
#ifdef OBJECT
        va = object_scale(va);
#endif
        vec2 from = c - sc * va;
        float m = texture(pix_mask, from / vec2(size)).r;
#ifdef LAND_A
        {
            // The landing check (owner 2026-10-14): the source pixel counts
            // only if its own motion carries it back here (else it is a valid
            // pixel that is not the one visible here - an occlusion).
            vec2 vl = field_vector_all(from);
#ifdef OBJECT
            vl = object_scale(vl);
#endif
            if (length(from + sc * vl - c) > LAND_A) m = 0.0;
        }
#endif
#ifdef PIXEL_MOTION
        outv = texture(src, (c - m * sc * va) / vec2(size));   // the weight's share of the motion
#else
        outv = mix(texelFetch(src, q, 0), texture(src, from / vec2(size)), m);
#endif
    } else
        outv = texelFetch(src, q, 0);
    imageStore(dst, q, outv);
    return;
#endif
    if (field_vector(c, v)) {
        // Edges clamp (the sampler's address mode): never black.
        outv = texture(src, (c - shift * strength_scale() * block_strength(c) * v) / vec2(size));
#ifdef FEATHER
        outv = mix(texelFetch(src, q, 0), outv, hold_weight(c, feather_px));
#endif
    } else {
#ifdef FILL
        vec2 pb;
        if (fill_position(c, shift * strength_scale(), pb))
            outv = texture(src_b, pb / vec2(size));
        else
#endif
        outv = texelFetch(src, q, 0);   // hard hold of A
    }
    imageStore(dst, q, outv);
}
)GLSL";

		// CbCr: the same motion in chroma pixels. 4:2:2 halves the horizontal
		// component (4:2:0 also the vertical).
		const char* const WARP_CHROMA_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec2 c = vec2(q) + vec2(0.5);
    vec2 v;
    vec4 outv;
#ifdef PIXEL_TRUST
    if (texelFetch(frame_trust, ivec2(0), 0).r >= min_trusted_share) {
        float sc = shift * strength_scale();
        vec2 va = field_vector_all(c * chroma_scale);
        va = field_vector_all(c * chroma_scale - sc * va);
        va = field_vector_all(c * chroma_scale - sc * va);
#ifdef OBJECT
        va = object_scale(va);
#endif
        vec2 from = c - sc * va / chroma_scale;
        float m = texture(pix_mask, from * chroma_scale / vec2(textureSize(pix_mask, 0) * 2)).r;
#ifdef LAND_A
        {
            vec2 vl = field_vector_all(from * chroma_scale);
#ifdef OBJECT
            vl = object_scale(vl);
#endif
            if (length(from * chroma_scale + sc * vl - c * chroma_scale) > LAND_A) m = 0.0;
        }
#endif
#ifdef PIXEL_MOTION
        outv = texture(src, (c - m * sc * va / chroma_scale) / vec2(size));
#else
        outv = mix(texelFetch(src, q, 0), texture(src, from / vec2(size)), m);
#endif
    } else
        outv = texelFetch(src, q, 0);
    imageStore(dst, q, outv);
    return;
#endif
    if (field_vector(c * chroma_scale, v)) {
        outv = texture(src, (c - shift * strength_scale() * block_strength(c * chroma_scale) * v / chroma_scale) / vec2(size));
#ifdef FEATHER
        outv = mix(texelFetch(src, q, 0), outv, hold_weight(c * chroma_scale, feather_px));
#endif
    } else {
#ifdef FILL
        vec2 pb;
        if (fill_position(c * chroma_scale, shift * strength_scale(), pb))
            outv = texture(src_b, pb / chroma_scale / vec2(size));
        else
#endif
        outv = texelFetch(src, q, 0);
    }
    imageStore(dst, q, outv);
}
)GLSL";

		pl_shader_desc Sampled(const char* name, pl_tex tex, bool linear)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_SAMPLED_TEX;
			desc.binding.object = tex;
			desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
			return desc;
		}

		pl_shader_desc Storage(const char* name, pl_tex tex)
		{
			pl_shader_desc desc{};
			desc.desc.name = name;
			desc.desc.type = PL_DESC_STORAGE_IMG;
			desc.desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc.binding.object = tex;
			return desc;
		}
	}

	MotionWarp::MotionWarp(pl_log logIn, pl_gpu gpuIn, const Settings& settingsIn)
		: log(logIn), gpu(gpuIn), settings(settingsIn), base(settingsIn)
	{
		dispatch = pl_dispatch_create(log, gpu);
	}

	Settings MotionWarp::Scaled(const Settings& given, int inputHeight)
	{
		Settings s = given;
		if (inputHeight > 0 && given.referenceHeight > 0.0f)
		{
			const float k = static_cast<float>(inputHeight) / given.referenceHeight;
			s.motionScaleLo *= k;
			s.motionScaleHi *= k;
			s.rollLo *= k;
			s.rollHi *= k;
			s.holdFeatherPx *= k;
			if (s.landingCheck > 0.0f)
				s.landingCheck = std::max(0.75f, s.landingCheck * k);
		}
		// The rates: converted once, by the outermost matcher (an inner, with
		// referenceHeight < 0, receives them converted; it keeps sourceHz for
		// its fade test).
		if (given.sourceHz > 0.0 && given.referenceRateHz > 0.0 && given.referenceHeight >= 0.0f)
		{
			// Per frame at the reference rate -> per frame at this rate.
			const double r = given.referenceRateHz / given.sourceHz;
			if (s.strengthRate > 0.0f && s.strengthRate < 1.0f)
				s.strengthRate = static_cast<float>(1.0 - std::pow(1.0 - s.strengthRate, r));
			if (s.holdHysteresis > 1)
				s.holdHysteresis = std::max(1, static_cast<int>(std::lround(s.holdHysteresis / r)));
		}
		return s;
	}

	MotionWarp::~MotionWarp()
	{
		// The optical-flow engine first: it unregisters its D3D11 textures, which must
		// still exist (as members they would be released before it - a crash at
		// teardown on NVIDIA GPUs once the engine became the default, 2026-10-16).
		opticalFlowEngine.reset();
		for (pl_tex* tex : { &halfA, &halfB, &l1A, &l1B, &l2A, &l2B, &halfK, &l1K, &l2K, &forward, &backward,
			&field, &frameTrust, &occlusion[0], &occlusion[1], &grainError, &warpedLuma, &warpedChroma,
			&rawField, &globalVector[0], &globalVector[1], &strengthState, &fillOcclusion,
			&sweep[0], &sweep[1], &ofSearch[0], &ofSearch[1], &ofNone, &previousSearch[0], &previousSearch[1], &qpelField, &subField,
			&subSearch[0], &subSearch[1], &holdState[0], &holdState[1], &subHoldState[0], &subHoldState[1],
			&hystField, &hystSub, &globalSad2, &globalSad1, &globalPick, &globalBothSad2, &globalBothSad1, &globalBothPick, &snapField, &snapTrust, &snapSub, &snapCamera, &camera, &globalSecond[0], &globalSecond[1], &fadeState, &aheadTrust, &verifyField, &pixelMask, &snapMask, &vectorOk, &snapVectorOk, &picture,
			&ofInTex[0], &ofInTex[1], &ofFlowTex[0], &ofFlowTex[1] })
			pl_tex_destroy(gpu, tex);
		for (int i = 0; i < 2; ++i) { ofIn[i].Release(); ofFlow[i].Release(); ofCost[i].Release(); }
		inner.reset();   // its textures before the dispatch and the device
		for (auto& entry : passTimers)
			pl_timer_destroy(gpu, &entry.second);
		pl_dispatch_destroy(&dispatch);
	}

	bool MotionWarp::EnsureTextures(const Planes& a)
	{
		if (!dispatch) { error = "no dispatch"; return false; }
		// a.chroma may be missing: an RGB source gives the matcher its luma
		// only (roadmap item 2); then there is no source warp (Warp refuses).
		if (!a.luma) { error = "missing planes"; return false; }
		width = a.luma->params.w;
		height = a.luma->params.h;
		halfW = (width + 1) / 2;
		halfH = (height + 1) / 2;
		blocksX = (halfW + 7) / 8;
		blocksY = (halfH + 7) / 8;
		l1W = (halfW + 3) / 4;
		l1H = (halfH + 3) / 4;
		l2W = (l1W + 3) / 4;
		l2H = (l1H + 3) / 4;
		const enum pl_fmt_caps scalarCaps = static_cast<enum pl_fmt_caps>(
			PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
		pl_fmt scalar = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 16, 16, scalarCaps);
		pl_fmt vec4f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
		if (!scalar || !vec4f) { error = "no storable float format"; return false; }
		auto make = [this](pl_tex* tex, int w, int h, pl_fmt fmt, bool readable) -> bool
		{
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = readable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
			return pl_tex_recreate(gpu, tex, &params);
		};
		const bool readable = settings.hostReadable;
		if (!make(&halfA, halfW, halfH, scalar, readable) || !make(&halfB, halfW, halfH, scalar, readable) ||
			!make(&l1A, l1W, l1H, scalar, readable) || !make(&l1B, l1W, l1H, scalar, readable) ||
			!make(&l2A, l2W, l2H, scalar, readable) || !make(&l2B, l2W, l2H, scalar, readable) ||
			!make(&halfK, halfW, halfH, scalar, readable) || !make(&l1K, l1W, l1H, scalar, readable) ||
			!make(&l2K, l2W, l2H, scalar, readable) ||
			!make(&forward, blocksX, blocksY, vec4f, readable) || !make(&backward, blocksX, blocksY, vec4f, readable) ||
			!make(&field, blocksX, blocksY, vec4f, readable) || !make(&frameTrust, 1, 1, vec4f, readable) ||
			!make(&occlusion[0], blocksX, blocksY, vec4f, readable) || !make(&occlusion[1], blocksX, blocksY, vec4f, readable) ||
			!make(&rawField, blocksX, blocksY, vec4f, readable) || !make(&fillOcclusion, blocksX, blocksY, vec4f, readable) ||
			!make(&globalVector[0], 1, 1, vec4f, readable) || !make(&globalVector[1], 1, 1, vec4f, readable) ||
			!make(&globalSecond[0], 1, 1, vec4f, readable) || !make(&globalSecond[1], 1, 1, vec4f, readable) ||
			!make(&sweep[0], blocksX, blocksY, vec4f, readable) || !make(&sweep[1], blocksX, blocksY, vec4f, readable) ||
			!make(&ofSearch[0], blocksX, blocksY, vec4f, readable) || !make(&ofSearch[1], blocksX, blocksY, vec4f, readable) ||
			!make(&ofNone, 1, 1, vec4f, readable) ||
			!make(&qpelField, blocksX, blocksY, vec4f, readable))
		{
			error = "cannot create the motion textures";
			return false;
		}
		const bool previousResized = !previousSearch[0] || previousSearch[0]->params.w != blocksX ||
			previousSearch[0]->params.h != blocksY;
		if (!make(&previousSearch[0], blocksX, blocksY, vec4f, readable) ||
			!make(&previousSearch[1], blocksX, blocksY, vec4f, readable))
		{
			error = "cannot create the motion textures";
			return false;
		}
		if (previousResized)
			previousValid = false;
		// 11a E2: allocated only while the split is on.
		// Delta 9 H2: the hysteresis state (zero = held, no history) and the
		// output it swaps with the field.
		if (settings.holdHysteresis > 1)
		{
			const bool fresh = holdResetPending || !holdState[0] || holdState[0]->params.w != blocksX || holdState[0]->params.h != blocksY;
			holdResetPending = false;
			for (int i = 0; i < 2; ++i)
				if (fresh)
				{
					pl_tex_destroy(gpu, &holdState[i]);
					pl_tex_destroy(gpu, &subHoldState[i]);
				}
			auto state = [&](pl_tex* tex, int w, int h) -> bool
			{
				if (*tex)
					return true;
				std::vector<uint16_t> zero(static_cast<size_t>(w) * h * 4, 0);
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = vec4f;
				tp.sampleable = true;
				tp.storable = true;
				tp.host_readable = readable && (vec4f->caps & PL_FMT_CAP_HOST_READABLE);
				tp.initial_data = zero.data();
				*tex = pl_tex_create(gpu, &tp);
				return *tex != nullptr;
			};
			if (!state(&holdState[0], blocksX, blocksY) || !state(&holdState[1], blocksX, blocksY) ||
				!make(&hystField, blocksX, blocksY, vec4f, readable) ||
				(settings.estSplit && (!state(&subHoldState[0], blocksX * 2, blocksY * 2) ||
					!state(&subHoldState[1], blocksX * 2, blocksY * 2) ||
					!make(&hystSub, blocksX * 2, blocksY * 2, vec4f, readable))))
			{
				error = "cannot create the hold hysteresis state";
				return false;
			}
		}
		if (settings.estSplit && (!make(&subField, blocksX * 2, blocksY * 2, vec4f, readable) ||
			!make(&subSearch[0], blocksX * 2, blocksY * 2, vec4f, readable) ||
			!make(&subSearch[1], blocksX * 2, blocksY * 2, vec4f, readable)))
		{
			error = "cannot create the sub-field";
			return false;
		}
		// D2/D4's last scale: read-write, so rgba32f (as Stage 8's state);
		// zero at creation = no history.
		if (!strengthState)
		{
			pl_fmt state = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			pl_tex_params params{};
			params.w = params.h = 1;
			params.format = state;
			params.sampleable = true;
			params.storable = true;
			params.initial_data = zero;
			strengthState = state ? pl_tex_create(gpu, &params) : nullptr;
			if (!strengthState)
			{
				error = "cannot create the strength state";
				return false;
			}
		}
		if (innerOfLowRes || !a.chroma)
			return true;   // the lowRes inner never warps; a luma-only source has no source warp
		// The warped planes keep A's formats, so the render sees the same
		// representation.
		for (const auto& plane : { std::make_pair(a.luma, &warpedLuma), std::make_pair(a.chroma, &warpedChroma) })
		{
			pl_fmt fmt = plane.first->params.format;
			if (!(fmt->caps & PL_FMT_CAP_STORABLE))
			{
				error = std::string("plane format not storable: ") + fmt->name;
				return false;
			}
			if (!make(plane.second, plane.first->params.w, plane.first->params.h, fmt, readable))
			{
				error = "cannot create the warped planes";
				return false;
			}
		}
		return true;
	}

	bool MotionWarp::Dispatch(const char* description, const char* header, const char* body,
		const pl_shader_desc* descriptors, int numDescriptors,
		const pl_shader_var* variables, int numVariables, int w, int h, int group)
	{
		pl_custom_shader custom{};
		custom.description = description;
		custom.header = header;
		custom.body = body;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = numDescriptors;
		custom.variables = variables;
		custom.num_variables = numVariables;
		custom.compute = true;
		custom.compute_group_size[0] = group;
		custom.compute_group_size[1] = group;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			error = std::string("shader rejected: ") + description;
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (w + group - 1) / group;
		compute.dispatch_size[1] = (h + group - 1) / group;
		compute.dispatch_size[2] = 1;
		// Tests only (SetPassTiming): each pass timed with the GPU drained.
		bool ok = false;
		TimedDispatch(description, compute, ok);
		return ok;
	}

	void MotionWarp::TimedDispatch(const char* description, pl_dispatch_compute_params& compute, bool& ok)
	{
		// Tests only (SetPassTiming): each pass timed with the GPU drained, and
		// by a GPU timer (the pass alone, without the drain's overhead).
		std::chrono::steady_clock::time_point start;
		if (passTiming)
		{
			pl_timer& timer = passTimers[description];
			if (!timer)
				timer = pl_timer_create(gpu);
			compute.timer = timer;
			if (passDrain)
			{
				pl_gpu_finish(gpu);
				start = std::chrono::steady_clock::now();
			}
		}
		ok = pl_dispatch_compute(dispatch, &compute);
		if (!ok)
		{
			error = std::string("dispatch failed: ") + description;
			return;
		}
		if (passTiming)
		{
			if (passDrain)
			{
				pl_gpu_finish(gpu);
				MotionWarp& sink = timingParent ? *timingParent : *this;
				sink.passMs[timingParent ? std::string(description) + " (half size)" : std::string(description)] +=
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			}
			CollectTimer(description, compute.timer);
		}
	}

	void MotionWarp::CollectTimer(const std::string& description, pl_timer timer)
	{
		MotionWarp& sink = timingParent ? *timingParent : *this;
		const std::string key = timingParent ? description + " (half size)" : description;
		for (uint64_t ns = pl_timer_query(gpu, timer); ns; ns = pl_timer_query(gpu, timer))
			sink.passGpuMs[key] += ns / 1e6;
	}

	void MotionWarp::FlushPassTimers()
	{
		pl_gpu_finish(gpu);
		for (const auto& entry : passTimers)
			CollectTimer(entry.first, entry.second);
		if (inner)
			inner->FlushPassTimers();
	}

	bool MotionWarp::DispatchPerBlock(const char* description, const char* header, const char* body,
		const pl_shader_desc* descriptors, int numDescriptors,
		const pl_shader_var* variables, int numVariables, int w, int h, int threadsX, int threadsY)
	{
		pl_custom_shader custom{};
		custom.description = description;
		custom.header = header;
		custom.body = body;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = numDescriptors;
		custom.variables = variables;
		custom.num_variables = numVariables;
		custom.compute = true;
		custom.compute_group_size[0] = threadsX;
		custom.compute_group_size[1] = threadsY;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			error = std::string("shader rejected: ") + description;
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = w;   // one work group per block
		compute.dispatch_size[1] = h;
		compute.dispatch_size[2] = 1;
		bool ok = false;
		TimedDispatch(description, compute, ok);
		return ok;
	}

	bool MotionWarp::Downsample(pl_tex luma, pl_tex half, pl_tex l1, pl_tex l2, bool fullRange, pl_tex fade, int levels,
		pl_tex plain)
	{
		const float codes = lumaInCodes ? 1.0f : fullRange ? 219.0f : 255.0f;
		const pl_shader_var variables[1] = { { pl_var_float("codes"), &codes, true } };
		const bool withPlain = fade && plain;
		const pl_shader_desc first[4] = { Sampled("src", luma, false), Storage("dst", half), Sampled("fade_state", fade, false),
			Sampled("plain", plain, false) };
		return Dispatch(fade ? "VideoProcessor de-judder: half-resolution luma after the fade (M4)" : "VideoProcessor de-judder: half-resolution luma",
				withPlain ? "#define FADE_APPLY 1\n#define FADE_PLAIN 1\n" : fade ? "#define FADE_APPLY 1\n" : nullptr, DOWNSAMPLE_BODY,
				first, withPlain ? 4 : fade ? 3 : 2, variables, 1, halfW, halfH, 16) &&
			Pyramid(half, l1, l2, 0, levels);
	}

	bool MotionWarp::Pyramid(pl_tex half, pl_tex l1, pl_tex l2, int from, int levels)
	{
		const pl_shader_desc second[2] = { Sampled("src", half, false), Storage("dst", l1) };
		const pl_shader_desc third[2] = { Sampled("src", l1, false), Storage("dst", l2) };
		return (from >= 1 || levels < 1 || Dispatch("VideoProcessor de-judder: luma pyramid 1/8", nullptr, PYRAMID_BODY,
				second, 2, nullptr, 0, l1W, l1H, 8)) &&
			(from >= 2 || levels < 2 || Dispatch("VideoProcessor de-judder: luma pyramid 1/32", nullptr, PYRAMID_BODY,
				third, 2, nullptr, 0, l2W, l2H, 8));
	}

	void MotionWarp::KeepPlainBInK()
	{
		std::swap(halfB, halfK);
		std::swap(l1B, l1K);
		std::swap(l2B, l2K);
		keptInK = true;
	}

	bool MotionWarp::Global(pl_tex fromL1, pl_tex toL1, pl_tex fromL2, pl_tex toL2, pl_tex out, pl_tex second)
	{
		if (serialSearchForTest)
		{
			const pl_shader_desc descriptors[5] = {
				Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
				Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false), Storage("dst", out) };
			return Dispatch("VideoProcessor de-judder: global motion (D1, serial reference)", GLOBAL_HEADER, GLOBAL_BODY,
				descriptors, 5, nullptr, 0, 16, 16, 16);
		}
		// Parallel: candidate SADs, the pick, the 1/8 candidates, the vector.
		if (!globalSad2)
		{
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			pl_fmt r32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, caps);
			pl_fmt rgba32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
			if (!r32 || !rgba32)
			{
				error = "global motion: no 32-bit float storage format";
				return false;
			}
			auto make = [this](pl_tex* tex, int w, int h, pl_fmt fmt) -> bool
			{
				pl_tex_params params{};
				params.w = w;
				params.h = h;
				params.format = fmt;
				params.sampleable = true;
				params.storable = true;
				return pl_tex_recreate(gpu, tex, &params);
			};
			if (!make(&globalSad2, 17, 17, r32) || !make(&globalSad1, 9, 9, r32) || !make(&globalPick, 1, 1, rgba32))
			{
				error = "global motion: cannot create the candidate textures";
				return false;
			}
		}
		static const std::string pickHeader = std::string(GLOBAL_PICK_HEADER);
		static const std::string pickTwoHeader = std::string("#define GLOBAL_TWO 1\n") + GLOBAL_PICK_HEADER;
		const pl_shader_desc l2Desc[3] = {
			Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false), Storage("dst", globalSad2) };
		const pl_shader_desc pick2Desc[2] = { Sampled("sad2", globalSad2, false), Storage("dst", globalPick) };
		const pl_shader_desc l1Desc[4] = {
			Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
			Sampled("global_pick", globalPick, false), Storage("dst", globalSad1) };
		const pl_shader_desc pick1Desc[4] = {
			Sampled("sad1", globalSad1, false), Sampled("global_pick", globalPick, false), Storage("dst", out),
			Storage("second_out", second) };
		return DispatchPerBlock("VideoProcessor de-judder: global motion (D1)", GLOBAL_SUM_HEADER, GLOBAL_L2_BODY,
				l2Desc, 3, nullptr, 0, 17, 17, 16, 16) &&
			DispatchPerBlock("VideoProcessor de-judder: global motion pick (D1)", pickHeader.c_str(), GLOBAL_PICK2_BODY,
				pick2Desc, 2, nullptr, 0, 1, 1, 16, 16) &&
			DispatchPerBlock("VideoProcessor de-judder: global motion 1/8 (D1)", GLOBAL_SUM_HEADER, GLOBAL_L1_BODY,
				l1Desc, 4, nullptr, 0, 9, 9, 16, 16) &&
			DispatchPerBlock("VideoProcessor de-judder: global motion vector (D1)", second ? pickTwoHeader.c_str() : pickHeader.c_str(),
				GLOBAL_PICK1_BODY, pick1Desc, second ? 4 : 3, nullptr, 0, 1, 1, 16, 16);
	}

	bool MotionWarp::GlobalBoth(pl_tex l1A, pl_tex l1B, pl_tex l2A, pl_tex l2B, pl_tex out[2], pl_tex second[2])
	{
		if (!globalBothSad2)
		{
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
			pl_fmt r32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, caps);
			pl_fmt rgba32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
			if (!r32 || !rgba32)
			{
				error = "global motion: no 32-bit float storage format";
				return false;
			}
			auto make = [this](pl_tex* tex, int w, int h, pl_fmt fmt) -> bool
			{
				pl_tex_params params{};
				params.w = w;
				params.h = h;
				params.format = fmt;
				params.sampleable = true;
				params.storable = true;
				return pl_tex_recreate(gpu, tex, &params);
			};
			if (!make(&globalBothSad2, 34, 17, r32) || !make(&globalBothSad1, 18, 9, r32) || !make(&globalBothPick, 2, 1, rgba32))
			{
				error = "global motion: cannot create the candidate textures";
				return false;
			}
		}
		static const std::string pickHeader = std::string(GLOBAL_PICK_HEADER);
		static const std::string pickTwoHeader = std::string("#define GLOBAL_TWO 1\n") + GLOBAL_PICK_HEADER;
		const bool two = second && second[0] && second[1];
		const pl_shader_desc l2Desc[3] = { Sampled("l2_a", l2A, false), Sampled("l2_b", l2B, false), Storage("dst", globalBothSad2) };
		const pl_shader_desc pick2Desc[2] = { Sampled("sad2", globalBothSad2, false), Storage("dst", globalBothPick) };
		const pl_shader_desc l1Desc[4] = { Sampled("l1_a", l1A, true), Sampled("l1_b", l1B, true),
			Sampled("global_pick", globalBothPick, false), Storage("dst", globalBothSad1) };
		const pl_shader_desc pick1Desc[6] = { Sampled("sad1", globalBothSad1, false), Sampled("global_pick", globalBothPick, false),
			Storage("dst0", out[0]), Storage("dst1", out[1]), Storage("second0", two ? second[0] : nullptr),
			Storage("second1", two ? second[1] : nullptr) };
		static const std::string l2Header = std::string(GLOBAL_BOTH_HEADER) + GLOBAL_BOTH_L2_HEADER;
		return DispatchPerBlock("VideoProcessor de-judder: global motion (D1)", l2Header.c_str(), GLOBAL_BOTH_L2_BODY,
				l2Desc, 3, nullptr, 0, 34, 17, 16, 16) &&
			DispatchPerBlock("VideoProcessor de-judder: global motion pick (D1)", pickHeader.c_str(), GLOBAL_BOTH_PICK2_BODY,
				pick2Desc, 2, nullptr, 0, 2, 1, 16, 16) &&
			DispatchPerBlock("VideoProcessor de-judder: global motion 1/8 (D1)", GLOBAL_BOTH_HEADER, GLOBAL_BOTH_L1_BODY,
				l1Desc, 4, nullptr, 0, 18, 9, 16, 16) &&
			DispatchPerBlock("VideoProcessor de-judder: global motion vector (D1)", two ? pickTwoHeader.c_str() : pickHeader.c_str(),
				GLOBAL_BOTH_PICK1_BODY, pick1Desc, two ? 6 : 4, nullptr, 0, 2, 1, 16, 16);
	}

	bool MotionWarp::Search(pl_tex fromHalf, pl_tex toHalf, pl_tex fromL1, pl_tex toL1,
		pl_tex fromL2, pl_tex toL2, pl_tex global, pl_tex previous, pl_tex out, pl_tex globalTwo)
	{
		const pl_shader_desc descriptors[11] = {
			// One binding per texture (libplacebo rejects the same texture bound
			// twice in a pass): `to_half` is linear for the half-pixel samples;
			// texelFetch ignores the filter.
			Sampled("from_half", fromHalf, false), Sampled("to_half", toHalf, true),
			Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
			Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false),
			Sampled("global_vec", global, false), Storage("dst", out),
			Sampled("previous", previous, false), Sampled("prev_trust", frameTrust, false),
			Sampled("global_two", globalTwo, false) };
		// General: the range is given for a 1080-line working picture.
		const int scaledRange = settings.referenceHeight != 0.0f && height > 0 ?
			static_cast<int>(std::lround(settings.coarseRange * height / 1080.0)) : settings.coarseRange;
		const int range = std::max(0, std::min(scaledRange, 16));
		const int useGlobal = settings.globalPrior ? 1 : 0;
		const int useTemporal = settings.estCandidates && previousValid ? 1 : 0;
		const int useGate = settings.estGate && settings.globalPrior && previousValid ? 1 : 0;
		const float motionLo = settings.motionScaleLo;
		const pl_shader_var variables[5] = { { pl_var_int("coarse_range"), &range, false },
			{ pl_var_int("use_global"), &useGlobal, false },
			{ pl_var_int("use_temporal"), &useTemporal, false },
			{ pl_var_int("use_gate"), &useGate, false },
			{ pl_var_float("motion_lo"), &motionLo, true } };
		if (serialSearchForTest)
			return Dispatch("VideoProcessor de-judder: block search (serial reference)", SEARCH_HEADER,
				SEARCH_BODY_SERIAL, descriptors, 10, variables, 5, blocksX, blocksY, 8);
		// 128 threads per block (2026-10-15, laptop 3050 Ti 4K, alternating runs: block search 5.36/6.31/5.43 ms
		// against 5.86/5.84/5.96 ms with 256 and 6.02 ms with 64; matcher 8.3-8.8 against 9.1-9.2 ms).
		const int nt = 128;
		static const std::string header = std::string("#define SEARCH_NT 128\n") + SEARCH_HEADER + SEARCH_PARALLEL_HEADER;
		static const std::string twoHeader = std::string("#define SEARCH_NT 128\n#define GLOBAL_TWO 1\n") + SEARCH_HEADER + SEARCH_PARALLEL_HEADER;
		const bool two = globalTwo && settings.globalPrior;
		return DispatchPerBlock("VideoProcessor de-judder: block search", two ? twoHeader.c_str() : header.c_str(), SEARCH_BODY,
			descriptors, two ? 11 : 10, variables, 5, blocksX, blocksY, 16, nt / 16);
	}

	bool MotionWarp::Refine(pl_tex fromHalf, pl_tex toHalf, pl_tex sweepIn, pl_tex previous, pl_tex global, pl_tex out,
		pl_tex opticalFlowVectors)
	{
		// The engine's vectors as one more candidate (each texture is bound once: a
		// separate 1x1 texture stands in when there are none).
		const pl_shader_desc descriptors[8] = {
			Sampled("from_half", fromHalf, false), Sampled("to_half", toHalf, true),
			Sampled("sweep", sweepIn, false), Sampled("previous", previous, false),
			Sampled("global_vec", global, false), Sampled("prev_trust", frameTrust, false),
			Sampled("of_vec", opticalFlowVectors ? opticalFlowVectors : ofNone, false),
			Storage("dst", out) };
		const int useGlobal = settings.globalPrior ? 1 : 0;
		const int useTemporal = previousValid ? 1 : 0;
		const int useGate = settings.estGate && settings.globalPrior && previousValid ? 1 : 0;
		const int useOf = opticalFlowVectors ? 1 : 0;
		const float motionLo = settings.motionScaleLo;
		const float lambda = std::max(0.0f, settings.estLambda);
		const pl_shader_var variables[6] = {
			{ pl_var_int("use_global"), &useGlobal, false },
			{ pl_var_int("use_temporal"), &useTemporal, false },
			{ pl_var_int("use_gate"), &useGate, false },
			{ pl_var_int("use_of"), &useOf, false },
			{ pl_var_float("motion_lo"), &motionLo, true },
			{ pl_var_float("lambda"), &lambda, true } };
		const std::string header = std::string(SUBPEL_HEADER) + GATE_GLSL + MEDIAN_GLSL;
		if (serialSearchForTest)
			return Dispatch("VideoProcessor de-judder: second sweep (11a E1, serial reference)", header.c_str(),
				REFINE_BODY_SERIAL, descriptors, 8, variables, 6, blocksX, blocksY, 8);
		static const std::string parallel = std::string(SUBPEL_HEADER) + GATE_GLSL + MEDIAN_GLSL + PERBLOCK_HEADER + REFINE_PAIR_HEADER;
		return DispatchPerBlock("VideoProcessor de-judder: second sweep (11a E1)", parallel.c_str(), REFINE_BODY,
			descriptors, 8, variables, 6, (blocksX + 1) / 2, blocksY, 16, 2);
	}

	bool MotionWarp::SnapshotForWarp()
	{
		snapSubValid = false;
		if (!estimated || !field || !frameTrust)
		{
			error = "snapshot without an estimate";
			return false;
		}
		auto copy = [this](pl_tex src, pl_tex* dst) -> bool
		{
			pl_tex_params params{};
			params.w = src->params.w;
			params.h = src->params.h;
			params.format = src->params.format;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = settings.hostReadable && (src->params.format->caps & PL_FMT_CAP_HOST_READABLE);
			return pl_tex_recreate(gpu, dst, &params) && Copy(src, *dst);
		};
		if (!copy(field, &snapField) || !copy(frameTrust, &snapTrust))
			return false;
		if (pl_tex sub = SubField())
		{
			if (!copy(sub, &snapSub))
				return false;
			snapSubValid = true;
		}
		// Delta 14 R-B's camera.
		if (Camera() && !copy(camera, &snapCamera))
			return false;
		// M6's pixel weights (and its vector flags).
		snapMaskValid = false;
		if (pl_tex mask = PixelMask())
		{
			if (!copy(mask, &snapMask))
				return false;
			if (PixelOwnVectors() && !copy(vectorOk, &snapVectorOk))
				return false;
			snapMaskValid = true;
		}
		return true;
	}

	namespace
	{
		// M5: the pixel check of each trusted block (see Settings).
		const char* const VERIFY_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = texelFetch(src_field, b, 0);
    if (f.z > 0.5) {
        ivec2 hs = textureSize(from_half, 0);
        float bad = 0.0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                ivec2 p = clamp(b * 8 + ivec2(x, y), ivec2(0), hs - ivec2(1));
                vec2 lp = (vec2(p) + vec2(0.5)) * 2.0;
                vec2 g = lp / 16.0 - vec2(0.5);
                ivec2 g0 = ivec2(floor(g));
                vec2 t = g - vec2(g0);
                vec2 v = vec2(0.0);
                float ws = 0.0;
                for (int yy = 0; yy <= 1; yy++)
                    for (int xx = 0; xx <= 1; xx++) {
                        vec4 n = texelFetch(src_field, clamp(g0 + ivec2(xx, yy), ivec2(0), blocks - ivec2(1)), 0);
                        float w = (xx == 1 ? t.x : 1.0 - t.x) * (yy == 1 ? t.y : 1.0 - t.y);
                        if (n.z > 0.5) { v += w * n.xy; ws += w; }
                    }
                v = ws > 0.0 ? v / ws : f.xy;
                float e = abs(texelFetch(from_half, p, 0).r - texture(to_half, (vec2(p) + v * 0.5 + vec2(0.5)) / vec2(hs)).r);
                bad += e > verify_codes ? 1.0 : 0.0;
            }
        if (bad / 64.0 > verify_share)
            f.z = 0.0;
    }
    imageStore(dst, b, f);
}
)GLSL";

		// M6: every block's vector at a source position (no trust gating).
		const char* const FIELD_ALL_GLSL = R"GLSL(
vec2 field_vector_all(vec2 lp)
{
    ivec2 fsize = textureSize(field, 0);
    vec2 g = lp / 16.0 - vec2(0.5);
    ivec2 g0 = ivec2(floor(g));
    vec2 t = g - vec2(g0);
    vec2 v = vec2(0.0);
    float ws = 0.0;
    for (int y = 0; y <= 1; y++)
        for (int x = 0; x <= 1; x++) {
            vec4 f = texelFetch(field, clamp(g0 + ivec2(x, y), ivec2(0), fsize - ivec2(1)), 0);
            float w = (x == 1 ? t.x : 1.0 - t.x) * (y == 1 ? t.y : 1.0 - t.y);
#ifdef PIXEL_TRUSTED_VECTORS
            // Only trusted blocks' vectors (a failed block follows its
            // trusted neighbours); none among the four: the 4x4 around.
#ifdef PIXEL_OWN_VECTORS
            if (texelFetch(vector_ok, clamp(g0 + ivec2(x, y), ivec2(0), fsize - ivec2(1)), 0).r < 0.5) w = 0.0;
#else
            if (f.z < 0.5) w = 0.0;
#endif
#endif
            v += w * f.xy;
            ws += w;
        }
#ifdef PIXEL_TRUSTED_VECTORS
    if (ws <= 0.0) {
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++) {
                vec4 f = texelFetch(field, clamp(g0 + ivec2(x, y), ivec2(0), fsize - ivec2(1)), 0);
#ifdef PIXEL_OWN_VECTORS
                f.z = texelFetch(vector_ok, clamp(g0 + ivec2(x, y), ivec2(0), fsize - ivec2(1)), 0).r;
#endif
                if (f.z > 0.5) { v += f.xy; ws += 1.0; }
            }
    }
    return ws > 0.0 ? v / ws : vec2(0.0);
#else
    return v;
#endif
}
)GLSL";

		// M6: which blocks' vectors may be used: trusted, or detailed with a
		// fair match.
		const char* const VECTOR_OK_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = texelFetch(field, b, 0);
    float ok = f.z > 0.5 ? 1.0 : 0.0;
    if (ok < 0.5 && f.w <= own_error) {
        ivec2 hs = textureSize(from_half, 0);
        float t = 0.0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                ivec2 p = clamp(b * 8 + ivec2(x, y), ivec2(0), hs - ivec2(2));
                float c0 = texelFetch(from_half, p, 0).r;
                t += abs(texelFetch(from_half, p + ivec2(1, 0), 0).r - c0) + abs(texelFetch(from_half, p + ivec2(0, 1), 0).r - c0);
            }
        ok = t / 128.0 >= own_texture ? 1.0 : 0.0;
    }
    imageStore(dst, b, vec4(ok, 0.0, 0.0, 1.0));
}
)GLSL";

		// M6: the weight of each half-resolution pixel of A.
		const char* const PIXEL_MASK_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 hs = imageSize(dst);
if (p.x < hs.x && p.y < hs.y) {
    vec2 v = field_vector_all((vec2(p) + vec2(0.5)) * 2.0) * 0.5;
    float e = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), hs - ivec2(1));
            e += abs(texelFetch(from_half, q, 0).r - texture(to_half, (vec2(q) + v + vec2(0.5)) / vec2(hs)).r);
        }
    e /= 9.0;
    // Two tiers: the pixel's own block passed the block test or not.
    ivec2 fs = textureSize(field, 0);
    bool trusted = texelFetch(field, clamp(p / 8, ivec2(0), fs - ivec2(1)), 0).z > 0.5;
    float lo = trusted ? mask_lo : mask_lo_u, hi = trusted ? mask_hi : mask_hi_u;
#ifdef PIXEL_FAST
    // Fast motion must predict B more exactly (v in half-resolution pixels).
    float speed = length(v) * 2.0;
    float fast = mix(1.0, fast_floor, clamp((speed - fast_from) / max(3.0 * fast_from, 1.0), 0.0, 1.0));
    lo *= fast;
    hi *= fast;
#endif
    imageStore(dst, p, vec4(1.0 - smoothstep(lo, hi, e), 0.0, 0.0, 1.0));
}
)GLSL";

		const char* const LOOKAHEAD_TRUST_BODY = R"GLSL(
if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) {
    vec4 s = texelFetch(snap, ivec2(0), 0);
    vec4 n = texelFetch(next_trust, ivec2(0), 0);
    if (n.r >= min_share)
        s.g = min(s.g, n.g);
    imageStore(dst, ivec2(0), s);
}
)GLSL";
	}

	const char* FieldAllGlsl() { return FIELD_ALL_GLSL; }

	pl_tex MotionWarp::LookAheadTrust()
	{
		if (!settings.motionRolloff || !snapTrust || !frameTrust || !estimated)
			return snapTrust;
		pl_tex_params params = snapTrust->params;
		params.sampleable = true;
		params.storable = true;
		params.host_readable = settings.hostReadable && (params.format->caps & PL_FMT_CAP_HOST_READABLE);
		if (!pl_tex_recreate(gpu, &aheadTrust, &params))
			return snapTrust;
		const float minShare = settings.minTrustedShare;
		const pl_shader_var v[1] = { { pl_var_float("min_share"), &minShare, true } };
		const pl_shader_desc d[3] = { Sampled("snap", snapTrust, false), Sampled("next_trust", frameTrust, false), Storage("dst", aheadTrust) };
		return Dispatch("VideoProcessor de-judder: the roll-off ahead (Q7)", nullptr, LOOKAHEAD_TRUST_BODY, d, 3, v, 1, 1, 1, 1) ?
			aheadTrust : snapTrust;
	}

	bool MotionWarp::CopyPair(pl_tex src0, pl_tex dst0, pl_tex src1, pl_tex dst1)
	{
		const pl_shader_desc descriptors[4] = { Sampled("src0", src0, false), Storage("dst0", dst0),
			Sampled("src1", src1, false), Storage("dst1", dst1) };
		return Dispatch("VideoProcessor de-judder: keep the searches (11a)", nullptr, COPY_PAIR_BODY,
			descriptors, 4, nullptr, 0, dst0->params.w, dst0->params.h, 8);
	}

	bool MotionWarp::Copy(pl_tex src, pl_tex dst)
	{
		const pl_shader_desc descriptors[2] = { Sampled("src", src, false), Storage("dst", dst) };
		return Dispatch("VideoProcessor de-judder: keep the searches (11a)", nullptr, COPY_BODY,
			descriptors, 2, nullptr, 0, dst->params.w, dst->params.h, 8);
	}

	namespace
	{
		// lowRes: an inner search texture (or the 1x1 global vector) at the
		// source's grid; xy times 2 (inner half-resolution pixels are two of
		// this one's), the rest as is.
		// The same for both searches (and, with GLOBALS, both 1x1 global
		// vectors) in one dispatch (2026-10-15): each texel as GRID_UP_BODY.
		const char* const GRID_UP_ALL_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 s = imageSize(dst_f);
if (q.x < s.x && q.y < s.y) {
    vec4 f = texelFetch(src_f, min(q / 2, textureSize(src_f, 0) - ivec2(1)), 0);
    f.xy *= 2.0;
    imageStore(dst_f, q, f);
    vec4 b = texelFetch(src_b, min(q / 2, textureSize(src_b, 0) - ivec2(1)), 0);
    b.xy *= 2.0;
    imageStore(dst_b, q, b);
}
#ifdef GLOBALS
if (q.x == 0 && q.y == 0) {
    vec4 g0 = texelFetch(src_g0, ivec2(0), 0);
    g0.xy *= 2.0;
    imageStore(dst_g0, ivec2(0), g0);
    vec4 g1 = texelFetch(src_g1, ivec2(0), 0);
    g1.xy *= 2.0;
    imageStore(dst_g1, ivec2(0), g1);
}
#endif
)GLSL";

		const char* const GRID_UP_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 s = imageSize(dst);
if (q.x < s.x && q.y < s.y) {
    vec4 v = texelFetch(src, min(q / 2, textureSize(src, 0) - ivec2(1)), 0);
    v.xy *= 2.0;
    imageStore(dst, q, v);
}
)GLSL";

		// lowRes: the previous frame trust for the inner's E4 gate (b = mean
		// |v| in source pixels; the inner's pixels are two of them).
		const char* const TRUST_SCALE_BODY = R"GLSL(
if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) {
    vec4 t = texelFetch(src, ivec2(0), 0);
    t.b *= trust_scale;
    imageStore(dst, ivec2(0), t);
}
)GLSL";

		// lowRes: an inner search scaled back to this grid and re-scored per
		// block (the search's 12x12 window at half resolution), so the
		// consistency test, the dither's occlusion and the split judge each
		// 16-pixel block by its own match.
		const char* const RESCORE_HEADER = R"GLSL(
ivec2 half_size_;
float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
)GLSL";

		const char* const RESCORE_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    half_size_ = textureSize(from_half, 0);
    vec4 f = texelFetch(src, min(b / 2, textureSize(src, 0) - ivec2(1)), 0);
    f.xy *= 2.0;
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = b * 8 + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + f.xy));
        }
    f.z = s / 144.0;
    imageStore(dst, b, f);
}
)GLSL";
	}

	bool MotionWarp::LowResSearch(const Planes& a, const Planes& b)
	{
		// The inner matcher: the same settings, the source-pixel ones halved;
		// it stops after the searches unless every pass is coarse (lowResAll).
		const bool all = settings.lowResAll;
		Settings innerSettings = settings;
		// It halves again when still tall (8K); its pixel values come from
		// this one's (scaled here, halved below), its search range from its
		// own working height.
		innerSettings.referenceHeight = -1.0f;
		innerSettings.motionScaleLo *= 0.5f;
		innerSettings.motionScaleHi *= 0.5f;
		innerSettings.rollLo *= 0.5f;   // delta 14 R-A, in the inner's pixels
		innerSettings.rollHi *= 0.5f;
		innerSettings.holdFeatherPx *= 0.5f;
		innerSettings.occlusionFill = false;
		innerSettings.pixelTrust = false;    // the outer makes M6's mask (audit 2026-10-14: wasted work)
		if (!all)
		{
			innerSettings.estQpel = false;
			innerSettings.vectorSmooth = false;
			innerSettings.estSplit = false;
			innerSettings.holdHysteresis = 1;
		}
		if (!inner)
			inner = std::make_unique<MotionWarp>(log, gpu, innerSettings);
		else
			inner->SetSettings(innerSettings);
		inner->rescoreForTest = rescoreForTest;
		inner->searchOnly = !all;
		inner->SetPictureArea(pictureGiven[0] / 2, pictureGiven[1] / 2, (pictureGiven[2] + 1) / 2, (pictureGiven[3] + 1) / 2);
		inner->innerOfLowRes = true;
		inner->lumaInCodes = true;
		inner->timingParent = this;
		if (inner->passTiming != passTiming || inner->passDrain != passDrain)
			inner->SetPassTiming(passTiming, passDrain);
		// The inner never runs its frame trust: its E4 gate reads this one's
		// (the previous frame's, as on the full-resolution path).
		if (!all && inner->previousValid && inner->frameTrust && frameTrust)
		{
			const float half = 0.5f;
			const pl_shader_var v[1] = { { pl_var_float("trust_scale"), &half, true } };
			const pl_shader_desc d[2] = { Sampled("src", frameTrust, false), Storage("dst", inner->frameTrust) };
			if (!Dispatch("VideoProcessor de-judder: previous trust to the half size (lowRes)", nullptr, TRUST_SCALE_BODY,
					d, 2, v, 1, 1, 1, 1))
				return false;
		}
		Planes la = a, lb = b;
		la.luma = halfA;
		lb.luma = halfB;
		if (!inner->Estimate(la, lb, -1, false))
		{
			error = "lowRes: " + inner->LastError();
			return false;
		}
		auto up = [&](pl_tex src, pl_tex dst) -> bool
		{
			const pl_shader_desc d[2] = { Sampled("src", src, false), Storage("dst", dst) };
			return Dispatch("VideoProcessor de-judder: searches to the source grid (lowRes)", nullptr, GRID_UP_BODY,
				d, 2, nullptr, 0, dst->params.w, dst->params.h, 8);
		};
		auto rescore = [&](pl_tex src, pl_tex from, pl_tex to, pl_tex dst) -> bool
		{
			const pl_shader_desc d[4] = { Sampled("src", src, false), Sampled("from_half", from, false),
				Sampled("to_half", to, true), Storage("dst", dst) };
			return Dispatch("VideoProcessor de-judder: searches re-scored per block (lowRes)", RESCORE_HEADER, RESCORE_BODY,
				d, 4, nullptr, 0, dst->params.w, dst->params.h, 8);
		};
		if (rescoreForTest)
		{
			if (!rescore(inner->forward, halfA, halfB, forward) || !rescore(inner->backward, halfB, halfA, backward) ||
				(settings.globalPrior && (!up(inner->globalVector[0], globalVector[0]) || !up(inner->globalVector[1], globalVector[1]))))
				return false;
		}
		else
		{
			// Both searches and both global vectors in one dispatch.
			const bool globals = settings.globalPrior;
			const pl_shader_desc d[8] = { Sampled("src_f", inner->forward, false), Storage("dst_f", forward),
				Sampled("src_b", inner->backward, false), Storage("dst_b", backward),
				Sampled("src_g0", inner->globalVector[0], false), Storage("dst_g0", globalVector[0]),
				Sampled("src_g1", inner->globalVector[1], false), Storage("dst_g1", globalVector[1]) };
			if (!Dispatch("VideoProcessor de-judder: searches to the source grid (lowRes)", globals ? "#define GLOBALS 1\n" : nullptr,
					GRID_UP_ALL_BODY, d, globals ? 8 : 4, nullptr, 0, forward->params.w, forward->params.h, 8))
				return false;
		}
		if (all)
		{
			// The inner's field, sub-field and trust at this grid.
			if (!up(inner->Field(), field))
				return false;
			if (settings.estSplit && inner->subField && subField && !up(inner->subField, subField))
				return false;
			const float twice = 2.0f;
			const pl_shader_var v[1] = { { pl_var_float("trust_scale"), &twice, true } };
			const pl_shader_desc d[2] = { Sampled("src", inner->frameTrust, false), Storage("dst", frameTrust) };
			if (!Dispatch("VideoProcessor de-judder: frame trust to the source (lowRes)", nullptr, TRUST_SCALE_BODY,
					d, 2, v, 1, 1, 1, 1))
				return false;
			// Delta 14 R-B: the inner's camera, in this one's pixels.
			if (inner->Camera())
			{
				pl_tex_params params = inner->Camera()->params;
				params.host_readable = settings.hostReadable && (params.format->caps & PL_FMT_CAP_HOST_READABLE);
				if (!pl_tex_recreate(gpu, &camera, &params) || !up(inner->Camera(), camera))
					return false;
			}
		}
		return true;
	}

	bool MotionWarp::OpticalFlowSearch(pl_tex outForward, pl_tex outBackward)
	{
		if (opticalFlowFailed)
			return false;
		auto fail = [this](const std::string& why) {
			opticalFlowFailed = true;
			opticalFlowError = why;   // the renderer logs it (OpticalFlowError)
			return false;
		};
		pl_d3d11 d3d = pl_d3d11_get(gpu);
		if (!d3d || !d3d->device)
			return fail("not a D3D11 GPU");
		CComPtr<ID3D11DeviceContext> context;
		d3d->device->GetImmediateContext(&context);
		const int w = halfW, h = halfH;
		// Textures of the half size (the engine's grid: 4 x 4).
		if (w != ofW || h != ofH)
		{
			opticalFlowEngine.reset();   // unregisters the textures while they still exist
			for (int i = 0; i < 2; ++i)
			{
				pl_tex_destroy(gpu, &ofInTex[i]);
				pl_tex_destroy(gpu, &ofFlowTex[i]);
				ofIn[i].Release(); ofFlow[i].Release(); ofCost[i].Release();
			}
			auto make = [&](int tw, int th, DXGI_FORMAT format, CComPtr<ID3D11Texture2D>& out) {
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = static_cast<UINT>(tw); desc.Height = static_cast<UINT>(th);
				desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				return SUCCEEDED(d3d->device->CreateTexture2D(&desc, nullptr, &out));
			};
			const int gw = (w + 3) / 4, gh = (h + 3) / 4;
			for (int i = 0; i < 2; ++i)
			{
				if (!make(w, h, NvOpticalFlow::Engine::InputFormat(), ofIn[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), ofFlow[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), ofCost[i]))
					return fail("cannot create its textures");
				pl_d3d11_wrap_params wrapIn{}; wrapIn.tex = ofIn[i];
				pl_d3d11_wrap_params wrapFlow{}; wrapFlow.tex = ofFlow[i];
				ofInTex[i] = pl_d3d11_wrap(gpu, &wrapIn);
				ofFlowTex[i] = pl_d3d11_wrap(gpu, &wrapFlow);
				if (!ofInTex[i] || !ofFlowTex[i])
					return fail("cannot share its textures with libplacebo");
			}
			ofW = w; ofH = h;
		}
		if (!opticalFlowEngine || opticalFlowPresetOpen != settings.opticalFlowPreset)
		{
			opticalFlowEngine = std::make_unique<NvOpticalFlow::Engine>();
			const NvOpticalFlow::Preset preset = settings.opticalFlowPreset <= 0 ? NvOpticalFlow::Preset::Slow :
				settings.opticalFlowPreset >= 2 ? NvOpticalFlow::Preset::Fast : NvOpticalFlow::Preset::Medium;
			if (!opticalFlowEngine->Open(d3d->device, context, w, h, preset, true))
				return fail(opticalFlowEngine->LastError());
			opticalFlowPresetOpen = settings.opticalFlowPreset;
		}
		// A's and B's half-size luma as the engine's input, the flow both
		// ways, then the blocks' vectors and SADs in the searches' format.
		const pl_shader_desc inA[2] = { Sampled("src", halfA, false), Storage("dst", ofInTex[0]) };
		const pl_shader_desc inB[2] = { Sampled("src", halfB, false), Storage("dst", ofInTex[1]) };
		if (!Dispatch("VideoProcessor de-judder: optical flow input A", nullptr, OF_INPUT_BODY, inA, 2, nullptr, 0, w, h, 16) ||
			!Dispatch("VideoProcessor de-judder: optical flow input B", nullptr, OF_INPUT_BODY, inB, 2, nullptr, 0, w, h, 16))
			return fail("its input pass failed");
		// libplacebo's queued work reaches the context before the engine runs.
		pl_gpu_flush(gpu);
		if (!opticalFlowEngine->Run(ofIn[0], ofIn[1], ofFlow[0], ofFlow[1], ofCost[0], ofCost[1], nullptr, true))
			return fail(opticalFlowEngine->LastError());
		const std::string header = std::string(RESCORE_HEADER);
		const pl_shader_desc fw[4] = { Sampled("flow", ofFlowTex[0], false), Sampled("from_half", halfA, false),
			Sampled("to_half", halfB, true), Storage("dst", outForward) };
		const pl_shader_desc bw[4] = { Sampled("flow", ofFlowTex[1], false), Sampled("from_half", halfB, false),
			Sampled("to_half", halfA, true), Storage("dst", outBackward) };
		// The searches' reach in half-size pixels: the coarse search at 1/32 of the
		// source (1/16 of the half size), +-coarseRange texels, plus the finer steps.
		const float reach = static_cast<float>(std::max(1, settings.coarseRange) * 16 + 8);
		const pl_shader_var reachVar[1] = { { pl_var_float("reach"), &reach, true } };
		if (!Dispatch("VideoProcessor de-judder: optical flow to blocks (forward)", header.c_str(), OF_TO_SEARCH_BODY, fw, 4,
				reachVar, 1, blocksX, blocksY, 8) ||
			!Dispatch("VideoProcessor de-judder: optical flow to blocks (backward)", header.c_str(), OF_TO_SEARCH_BODY, bw, 4,
				reachVar, 1, blocksX, blocksY, 8))
			return fail("its block pass failed");
		return true;
	}

	bool MotionWarp::PictureArea()
	{
		if (!picture)
		{
			pl_fmt rgba32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
			pl_tex_params params{};
			params.w = params.h = 1;
			params.format = rgba32;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = settings.hostReadable && rgba32 && (rgba32->caps & PL_FMT_CAP_HOST_READABLE);
			picture = rgba32 ? pl_tex_create(gpu, &params) : nullptr;
			if (!picture)
			{
				error = "cannot create the picture area";
				return false;
			}
		}
		// Black in this matcher's luma codes (limited 16, full range 0) and
		// the margin a bar's 1/8 average may have above it.
		const float black = pictureFullRange ? 0.0f : 16.0f;
		const float barCodes = 4.0f;
		const float given[4] = { static_cast<float>(pictureGiven[0]), static_cast<float>(pictureGiven[1]),
			static_cast<float>(pictureGiven[2]), static_cast<float>(pictureGiven[3]) };
		const float frameSize[2] = { static_cast<float>(width), static_cast<float>(height) };
		const pl_shader_var v[4] = { { pl_var_vec4("given"), given, true }, { pl_var_vec2("frame_size"), frameSize, true },
			{ pl_var_float("black"), &black, true }, { pl_var_float("bar_codes"), &barCodes, true } };
		const pl_shader_desc d[2] = { Sampled("l1", l1A, false), Storage("dst", picture) };
		return Dispatch("VideoProcessor de-judder: picture area (black bars)", PICTURE_HEADER, PICTURE_BODY, d, 2, v, 4, 16, 16, 16);
	}

	bool MotionWarp::FadeMatch(const Planes& b)
	{
		if (!fadeState)
		{
			pl_fmt state = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
			const float zero[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			pl_tex_params params{};
			params.w = params.h = 1;
			params.format = state;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = settings.hostReadable && state && (state->caps & PL_FMT_CAP_HOST_READABLE);
			params.initial_data = zero;
			fadeState = state ? pl_tex_create(gpu, &params) : nullptr;
			if (!fadeState)
			{
				error = "cannot create the fade state";
				return false;
			}
		}
		pl_shader_desc stateDesc{};
		stateDesc.desc.name = "state_io";
		stateDesc.desc.type = PL_DESC_STORAGE_IMG;
		stateDesc.desc.access = PL_DESC_ACCESS_READWRITE;
		stateDesc.binding.object = fadeState;
		const bool withPicture = settings.pictureArea && picture;
		const pl_shader_desc d[4] = { Sampled("l2_a", l2A, false), Sampled("l2_b", l2B, false), stateDesc,
			Sampled("picture", picture, false) };
		const std::string header = std::string(withPicture ? "#define PICTURE 1\n" : "") + FADE_HEADER;
		// The test, then B's pyramid again with the gain applied (unchanged
		// when the test fails).
		// The window per frame: 0.75 to 24 codes per frame at the reference
		// rate, scaled to this source rate (general, owner 2026-10-14).
		const float perFrame = settings.sourceHz > 0.0 && settings.referenceRateHz > 0.0 ?
			static_cast<float>(settings.referenceRateHz / settings.sourceHz) : 1.0f;
		const float fadeLo = 0.75f * perFrame, fadeHi = 24.0f * perFrame;
		const pl_shader_var fadeVars[2] = { { pl_var_float("fade_lo"), &fadeLo, true }, { pl_var_float("fade_hi"), &fadeHi, true } };
		if (!Dispatch("VideoProcessor de-judder: fade test (M4)", header.c_str(), FADE_BODY, d, withPicture ? 4 : 3, fadeVars, 2, 16, 16, 16))
			return false;
		// The uncorrected B stays in the K set for the next pair's A.
		const int levels = keptLevels;
		KeepPlainBInK();
		return Downsample(b.luma, halfB, l1B, l2B, b.fullRange, fadeState, levels, halfK);
	}

	bool MotionWarp::Estimate(const Planes& a, const Planes& b, int occlusionSlot, bool wantGrainError)
	{
		estimated = false;
		grainErrorValid = false;
		fillReady = false;
		if (!EnsureTextures(a)) return false;
		if (!b.luma || b.luma->params.w != width || b.luma->params.h != height)
		{
			error = "A and B differ in size";
			return false;
		}
		// M4: in the lowRes outer the inner matches (and tests the fade) itself.
		ApplyScaled(Scaled(base, height));
		const bool outerOfLowRes = HalvesForSearch();
		// The lowRes outer's pyramid levels are read only by the picture area
		// (A's 1/8) and by its own frame trust's regional histograms (both
		// 1/32); the inner builds its own pyramids (2026-10-15: not built
		// otherwise - exact, the levels are unread).
		const bool outerTrust = !settings.lowResAll;
		const bool outerHist = outerTrust && settings.regionalHold;
		const int levelsA = !outerOfLowRes || outerHist ? 2 : settings.pictureArea ? 1 : 0;
		const int levelsB = !outerOfLowRes || outerHist ? 2 : 0;
		// 2026-10-15: A is the last pair's B (the same source frame, no gap or
		// resync since - those forget it): its uncorrected planes are kept, so
		// A's are those exactly; only missing pyramid levels are built.
		const bool reuseA = a.sequence != 0 && a.sequence == keptSequence && a.fullRange == keptFullRange;
		keptSequence = 0;
		if (reuseA)
		{
			if (keptInK)
			{
				std::swap(halfA, halfK);
				std::swap(l1A, l1K);
				std::swap(l2A, l2K);
			}
			else
			{
				std::swap(halfA, halfB);
				std::swap(l1A, l1B);
				std::swap(l2A, l2B);
			}
		}
		if (!(reuseA ? Pyramid(halfA, l1A, l2A, keptLevels, levelsA) : Downsample(a.luma, halfA, l1A, l2A, a.fullRange, nullptr, levelsA)) ||
			!Downsample(b.luma, halfB, l1B, l2B, b.fullRange, nullptr, levelsB))
			return false;
		reusedPlanesForTest += reuseA ? 1 : 0;
		keptSequence = b.sequence;
		keptInK = false;
		keptLevels = levelsB;
		keptFullRange = b.fullRange;
		pictureFullRange = a.fullRange;
		if (settings.pictureArea && !PictureArea())
			return false;
		if (settings.fadeMatch && !outerOfLowRes && !FadeMatch(b))
			return false;
		if (HalvesForSearch())
		{
			if (!LowResSearch(a, b))
				return false;
			previousValid = false;   // the full-resolution searches' history is stale
			// The inner tested the fade on these planes: B here gets the same
			// gain and offset, so M6's mask and the grain map see it (audit
			// 2026-10-14: at 4K the weights fell towards the hold in a fade).
			if (settings.fadeMatch && inner && inner->FadeStateTexture())
			{
				KeepPlainBInK();
				if (!Downsample(b.luma, halfB, l1B, l2B, b.fullRange, inner->FadeStateTexture(), levelsB, halfK))
					return false;
			}
		}
		else
		{
			// M2 (parallel search only; the serial reference has one candidate).
			const bool two = settings.globalTwo && settings.globalPrior && !serialSearchForTest;
			if (settings.globalPrior && (serialSearchForTest ?
					(!Global(l1A, l1B, l2A, l2B, globalVector[0], two ? globalSecond[0] : nullptr) ||
					 !Global(l1B, l1A, l2B, l2A, globalVector[1], two ? globalSecond[1] : nullptr)) :
					!GlobalBoth(l1A, l1B, l2A, l2B, globalVector, two ? globalSecond : nullptr)))
				return false;
			// The optical flow engine (docs/VP-nvof-ahead.md): by default its vectors join
			// the refinement's candidates (needs the two sweeps); otherwise they replace
			// the searches. Without the engine the searches run as before.
			const bool candidateMode = settings.opticalFlowCandidate && settings.estCandidates;
			opticalFlowUsed = settings.opticalFlow && !serialSearchForTest &&
				(candidateMode ? OpticalFlowSearch(ofSearch[0], ofSearch[1]) : OpticalFlowSearch(forward, backward));
			const bool replaced = opticalFlowUsed && !candidateMode;
			// Seed: the engine's vectors stand in for the first sweep (no searches).
			const bool seeded = opticalFlowUsed && candidateMode && settings.opticalFlowSeed;
			// 11a E1: the first sweep into `sweep`, the second into the searches.
			const bool twoSweeps = settings.estCandidates && !replaced;
			if (!replaced && !seeded &&
				(!Search(halfA, halfB, l1A, l1B, l2A, l2B, globalVector[0], previousSearch[0], twoSweeps ? sweep[0] : forward,
					two ? globalSecond[0] : nullptr) ||
				 !Search(halfB, halfA, l1B, l1A, l2B, l2A, globalVector[1], previousSearch[1], twoSweeps ? sweep[1] : backward,
					two ? globalSecond[1] : nullptr)))
				return false;
			const bool ofCandidates = opticalFlowUsed && candidateMode && !seeded;
			if (twoSweeps &&
				(!Refine(halfA, halfB, seeded ? ofSearch[0] : sweep[0], previousSearch[0], globalVector[0], forward, ofCandidates ? ofSearch[0] : nullptr) ||
				 !Refine(halfB, halfA, seeded ? ofSearch[1] : sweep[1], previousSearch[1], globalVector[1], backward, ofCandidates ? ofSearch[1] : nullptr)))
				return false;
			if (settings.estCandidates || settings.estGate)
			{
				if (!CopyPair(forward, previousSearch[0], backward, previousSearch[1]))
					return false;
				previousValid = true;
			}
			else
				previousValid = false;
		}
		if (searchOnly)
			return true;   // lowRes inner: the outer refines (estimated stays false)
		const float tolerance = settings.matchTolerance;
		const float consistency = settings.consistencyPixels;
		const pl_shader_var variables[2] = {
			{ pl_var_float("tolerance"), &tolerance, true },
			{ pl_var_float("consistency"), &consistency, true } };
		const char* splitDefine = settings.estSplit ? "#define SPLIT 1\n" : nullptr;
		// lowResAll: the inner made the field, the sub-field and the trust.
		if (!(settings.lowResAll && HalvesForSearch()))
		{
			// The consistency pass writes `field` directly, or a raw field that
			// E3 (quarter-pel) and D3 (smoothing) refine into `field`.
			const bool refined = settings.vectorSmooth || settings.estQpel;
			std::vector<pl_shader_desc> descriptors = {
				Sampled("fwd", forward, false), Sampled("bwd", backward, false),
				Storage("dst", refined ? rawField : field) };
			// Delta 9 H3: marginal blocks (up to taper_sad) pass the consistency
			// check as trusted; the warp tapers them.
			const float taperSad = settings.taperSad > 0.0f ? settings.taperSad : 2.0f * settings.matchTolerance;
			const bool textureTol = settings.textureTolerance > 0.0f;
			const float textureTolValue = settings.textureTolerance;
			std::vector<pl_shader_var> consistencyVars = { variables[0], variables[1] };
			if (settings.blockTaper)
				consistencyVars.push_back({ pl_var_float("taper_sad"), &taperSad, true });
			if (textureTol)
				consistencyVars.push_back({ pl_var_float("texture_tol"), &textureTolValue, true });
			if (textureTol)
				descriptors.push_back(Sampled("from_half", halfA, false));
			const bool withPicture = settings.pictureArea && picture;
			if (withPicture)
				descriptors.push_back(Sampled("picture", picture, false));
			const std::string consistencyHeader = std::string(settings.blockTaper ? "#define TAPER 1\n" : "") +
				(textureTol ? "#define TEXTURE_TOL 1\n" : "") + (withPicture ? "#define PICTURE 1\n" : "");
			if (!Dispatch("VideoProcessor de-judder: consistency", consistencyHeader.empty() ? nullptr : consistencyHeader.c_str(),
				CONSISTENCY_BODY, descriptors.data(), static_cast<int>(descriptors.size()), consistencyVars.data(),
				static_cast<int>(consistencyVars.size()), blocksX, blocksY, 8))
				return false;
			if (settings.estQpel)
			{
				const pl_shader_desc qpelDesc[4] = {
					Sampled("raw", rawField, false), Sampled("from_half", halfA, false),
					Sampled("to_half", halfB, true), Storage("dst", settings.vectorSmooth ? qpelField : field) };
				static const std::string qpelHeader = std::string(SUBPEL_HEADER) + PERBLOCK_HEADER;
				if (serialSearchForTest ?
					!Dispatch("VideoProcessor de-judder: quarter-pel (11a E3, serial reference)", SUBPEL_HEADER,
						QPEL_BODY_SERIAL, qpelDesc, 4, nullptr, 0, blocksX, blocksY, 8) :
					!DispatchPerBlock("VideoProcessor de-judder: quarter-pel (11a E3)", qpelHeader.c_str(), QPEL_BODY,
						qpelDesc, 4, nullptr, 0, blocksX, blocksY, 8, 4))
					return false;
			}
			if (settings.vectorSmooth && !Smooth(settings.estQpel ? qpelField : rawField))
				return false;
			if (settings.verifyPixels)
			{
				// M5: before the split and the frame trust, so both see it.
				pl_tex_params params = field->params;
				if (!pl_tex_recreate(gpu, &verifyField, &params))
				{
					error = "cannot create the verification field";
					return false;
				}
				const float verifyCodes = settings.verifyFactor * settings.matchTolerance;
				const float verifyShare = settings.verifyShare;
				const pl_shader_var verifyVars[2] = { { pl_var_float("verify_codes"), &verifyCodes, true },
					{ pl_var_float("verify_share"), &verifyShare, true } };
				const pl_shader_desc verifyDesc[4] = { Sampled("src_field", field, false), Sampled("from_half", halfA, false),
					Sampled("to_half", halfB, true), Storage("dst", verifyField) };
				if (!Dispatch("VideoProcessor de-judder: pixel verification (M5)", nullptr, VERIFY_BODY, verifyDesc, 4, verifyVars, 2,
						blocksX, blocksY, 8))
					return false;
				std::swap(field, verifyField);
			}
			if (settings.estSplit)
			{
				// Both directions split (the sub-search, as a function: four
				// texels per block), then A's sub-blocks are checked against B's.
				const float splitSad = settings.estSplitSad > 0.0f ? settings.estSplitSad : settings.matchTolerance;
				const pl_shader_var splitVars[1] = { { pl_var_float("split_sad"), &splitSad, true } };
				const std::string header = std::string(SUBPEL_HEADER) + SPLIT_SEARCH_BODY;
				for (int dir = 0; dir < 2; ++dir)
				{
					const pl_shader_desc splitDesc[4] = {
						Sampled("search", dir == 0 ? forward : backward, false),
						Sampled("from_half", dir == 0 ? halfA : halfB, false), Sampled("to_half", dir == 0 ? halfB : halfA, true),
						Storage("dst", subSearch[dir]) };
					if (!Dispatch("VideoProcessor de-judder: block split (11a E2)", header.c_str(), "main_split();",
						splitDesc, 4, splitVars, 1, blocksX, blocksY, 8))
						return false;
				}
				const pl_shader_desc trustDesc2[4] = {
					Sampled("sub_fwd", subSearch[0], false), Sampled("sub_bwd", subSearch[1], false),
					Sampled("field", field, false), Storage("dst", subField) };
				if (!Dispatch("VideoProcessor de-judder: sub-block trust (11a E2)", nullptr, SUB_TRUST_BODY,
					trustDesc2, 4, variables, 2, blocksX * 2, blocksY * 2, 8))
					return false;
			}
			const float motionLo = settings.motionScaleLo;
			const float motionHi = std::max(0.0f, settings.motionScaleHi);
			const float trustFull = std::max(0.0f, settings.trustScaleFull);
			const float minShare = settings.minTrustedShare;
			const float rate = std::max(0.0f, settings.strengthRate);
			const pl_shader_var trustVars[5] = {
				{ pl_var_float("motion_lo"), &motionLo, true },
				{ pl_var_float("motion_hi"), &motionHi, true },
				{ pl_var_float("trust_full"), &trustFull, true },
				{ pl_var_float("min_share"), &minShare, true },
				{ pl_var_float("rate"), &rate, true } };
			pl_shader_desc stateDesc{};
			stateDesc.desc.name = "state_io";
			stateDesc.desc.type = PL_DESC_STORAGE_IMG;
			stateDesc.desc.access = PL_DESC_ACCESS_READWRITE;
			stateDesc.binding.object = strengthState;
			// Delta 14 R-A: the roll-off (on D1's global vector when it runs).
			const bool rolloffGlobal = settings.motionRolloff && settings.globalPrior && globalVector[0];
			// Delta 14 R-B: the camera, refined here.
			const bool cameraOut = settings.objectStrength < 1.0f && settings.globalPrior && globalVector[0];
			if (cameraOut && !camera)
			{
				pl_fmt vec4f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
				pl_tex_params params{};
				params.w = params.h = 1;
				params.format = vec4f;
				params.sampleable = true;
				params.storable = true;
				params.host_readable = settings.hostReadable && vec4f && (vec4f->caps & PL_FMT_CAP_HOST_READABLE);
				if (!vec4f || !pl_tex_recreate(gpu, &camera, &params))
				{
					error = "cannot create the camera texture";
					return false;
				}
			}
			std::vector<pl_shader_desc> trustDescs = { Sampled("field", field, false), Storage("dst", frameTrust), stateDesc };
			if (settings.regionalHold)
			{
				trustDescs.push_back(Sampled("hist_a", l2A, false));
				trustDescs.push_back(Sampled("hist_b", l2B, false));
			}
			const bool fadeOn = settings.fadeMatch && fadeState;
			if (fadeOn)
				trustDescs.push_back(Sampled("fade_state", fadeState, false));
			if (rolloffGlobal || cameraOut)
				trustDescs.push_back(Sampled("global_vec", globalVector[0], false));
			if (cameraOut)
				trustDescs.push_back(Storage("camera_out", camera));
			const bool trustPicture = settings.pictureArea && picture;
			if (trustPicture)
				trustDescs.push_back(Sampled("picture", picture, false));
			const float rollLo = settings.rollLo, rollHi = settings.rollHi, rollFloor = std::min(1.0f, std::max(0.0f, settings.rollFloor));
			std::vector<pl_shader_var> trustVarList(trustVars, trustVars + 5);
			const float cutHist = settings.cutHistogram;
			if (settings.regionalHold)
				trustVarList.push_back({ pl_var_float("cut_hist"), &cutHist, true });
			const float fadeFloor = std::min(1.0f, std::max(0.0f, settings.fadeFloor));
			if (fadeOn)
				trustVarList.push_back({ pl_var_float("fade_floor"), &fadeFloor, true });
			if (settings.motionRolloff)
			{
				trustVarList.push_back({ pl_var_float("roll_lo"), &rollLo, true });
				trustVarList.push_back({ pl_var_float("roll_hi"), &rollHi, true });
				trustVarList.push_back({ pl_var_float("roll_floor"), &rollFloor, true });
			}
			const std::string trustHeader = std::string(settings.motionRolloff ? "#define ROLLOFF 1\n" : "") +
				(rolloffGlobal ? "#define ROLLOFF_GLOBAL 1\n" : "") + (cameraOut ? "#define CAMERA_OUT 1\n" : "") +
				(settings.regionalHold ? "#define REGIONAL 1\n" : "") + (fadeOn ? "#define FADE 1\n" : "") +
				(trustPicture ? "#define PICTURE 1\n" : "") + FRAME_TRUST_HEADER;
			const pl_shader_desc* trustDesc = trustDescs.data();
			if (!Dispatch("VideoProcessor de-judder: frame trust", trustHeader.c_str(), FRAME_TRUST_BODY,
				trustDesc, static_cast<int>(trustDescs.size()), trustVarList.data(), static_cast<int>(trustVarList.size()), 16, 16, 16))
				return false;
			if (settings.holdHysteresis > 1)
			{
				// Delta 9 H2, after the frame trust (the cut flag); the result
				// becomes the field (and the sub-field) by swapping.
				const float hysteresis = static_cast<float>(settings.holdHysteresis);
				const pl_shader_var hystVars[2] = { { pl_var_float("hysteresis"), &hysteresis, true },
					{ pl_var_float("min_share"), &minShare, true } };
				const int next = 1 - holdIndex;
				const pl_shader_desc hystDesc[5] = { Sampled("src_field", field, false), Sampled("state_in", holdState[holdIndex], false),
					Sampled("frame_trust", frameTrust, false), Storage("state_out", holdState[next]), Storage("dst", hystField) };
				if (!Dispatch("VideoProcessor de-judder: hold hysteresis (delta 9 H2)", nullptr, HYST_BODY,
					hystDesc, 5, hystVars, 2, blocksX, blocksY, 8))
					return false;
				std::swap(field, hystField);
				if (settings.estSplit)
				{
					const pl_shader_desc subDesc[5] = { Sampled("src_field", subField, false), Sampled("state_in", subHoldState[holdIndex], false),
						Sampled("frame_trust", frameTrust, false), Storage("state_out", subHoldState[next]), Storage("dst", hystSub) };
					if (!Dispatch("VideoProcessor de-judder: sub-block hysteresis (delta 9 H2)", nullptr, HYST_BODY,
						subDesc, 5, hystVars, 2, blocksX * 2, blocksY * 2, 8))
						return false;
					std::swap(subField, hystSub);
				}
				holdIndex = next;
			}
		}
		// D5's occlusion (also with the half-size matcher: its searches are at
		// this grid by now).
		if (settings.occlusionFill)
		{
			const bool subOk = settings.estSplit && subField;
			const pl_shader_desc fillDesc[4] = {
				Sampled("fwd", forward, false), Sampled("bwd", backward, false), Storage("dst", fillOcclusion),
				Sampled("sub_field", subField, false) };
			if (!Dispatch("VideoProcessor de-judder: occlusion for the fill (D5)", subOk ? splitDefine : nullptr, OCCLUSION_BODY,
				fillDesc, subOk ? 4 : 3, variables, 2, blocksX, blocksY, 8))
				return false;
			lastB = b;
			fillReady = b.chroma && a.chroma && b.chroma->params.w == a.chroma->params.w &&
				b.chroma->params.h == a.chroma->params.h;
		}
		pixelMaskReady = false;
		if (settings.pixelTrust)
		{
			pl_fmt scalar = halfA->params.format;
			pl_tex_params params{};
			params.w = halfW;
			params.h = halfH;
			params.format = scalar;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = settings.hostReadable && (scalar->caps & PL_FMT_CAP_HOST_READABLE);
			if (!pl_tex_recreate(gpu, &pixelMask, &params))
			{
				error = "cannot create the pixel mask";
				return false;
			}
			const float lo = settings.pixelTrustLo * settings.matchTolerance, hi = std::max(lo + 0.01f, settings.pixelTrustHi * settings.matchTolerance);
			const bool tiers = settings.pixelTrustHiUntrusted > 0.0f;
			const float loU = tiers ? settings.pixelTrustLoUntrusted * settings.matchTolerance : lo;
			const float hiU = tiers ? std::max(loU + 0.01f, settings.pixelTrustHiUntrusted * settings.matchTolerance) : hi;
			const bool fast = settings.pixelFastFrom > 0.0f;
			const float fastFrom = settings.pixelFastFrom, fastFloor = std::min(1.0f, std::max(0.05f, settings.pixelFastFloor));
			const pl_shader_var maskVars[6] = { { pl_var_float("mask_lo"), &lo, true }, { pl_var_float("mask_hi"), &hi, true },
				{ pl_var_float("mask_lo_u"), &loU, true }, { pl_var_float("mask_hi_u"), &hiU, true },
				{ pl_var_float("fast_from"), &fastFrom, true }, { pl_var_float("fast_floor"), &fastFloor, true } };
			const pl_shader_desc maskDesc[4] = { Sampled("field", field, false), Sampled("from_half", halfA, false),
				Sampled("to_half", halfB, true), Storage("dst", pixelMask) };
			const bool own = settings.pixelTrustedVectors && settings.pixelOwnTexture > 0.0f;
			if (own)
			{
				pl_tex_params okParams = field->params;
				okParams.host_readable = settings.hostReadable && (okParams.format->caps & PL_FMT_CAP_HOST_READABLE);
				if (!pl_tex_recreate(gpu, &vectorOk, &okParams))
				{
					error = "cannot create the vector flags";
					return false;
				}
				const float ownError = settings.pixelOwnError * settings.matchTolerance, ownTexture = settings.pixelOwnTexture;
				const pl_shader_var okVars[2] = { { pl_var_float("own_error"), &ownError, true }, { pl_var_float("own_texture"), &ownTexture, true } };
				const pl_shader_desc okDesc[3] = { Sampled("field", field, false), Sampled("from_half", halfA, false), Storage("dst", vectorOk) };
				if (!Dispatch("VideoProcessor de-judder: pixel trust vectors (M6)", nullptr, VECTOR_OK_BODY, okDesc, 3, okVars, 2, blocksX, blocksY, 8))
					return false;
			}
			std::vector<pl_shader_desc> maskDescs(maskDesc, maskDesc + 4);
			if (own)
				maskDescs.push_back(Sampled("vector_ok", vectorOk, false));
			const std::string maskHeader = std::string(settings.pixelTrustedVectors ? "#define PIXEL_TRUSTED_VECTORS 1\n" : "") +
				(own ? "#define PIXEL_OWN_VECTORS 1\n" : "") + (fast ? "#define PIXEL_FAST 1\n" : "") + FIELD_ALL_GLSL;
			if (!Dispatch("VideoProcessor de-judder: pixel trust (M6)", maskHeader.c_str(), PIXEL_MASK_BODY, maskDescs.data(),
					static_cast<int>(maskDescs.size()), maskVars, fast ? 6 : 4, halfW, halfH, 16))
				return false;
			pixelMaskReady = true;
		}
		if (occlusionSlot == 0 || occlusionSlot == 1)
		{
			const pl_shader_desc occlusionDesc[4] = {
				Sampled("fwd", forward, false), Sampled("bwd", backward, false),
				Storage("dst", occlusion[occlusionSlot]), Sampled("sub_field", subField, false) };
			if (!Dispatch("VideoProcessor de-judder: occlusion for the dither", splitDefine, OCCLUSION_BODY,
				occlusionDesc, settings.estSplit ? 4 : 3, variables, 2, blocksX, blocksY, 8))
				return false;
		}
		if (wantGrainError)
		{
			// Created on first use: the de-judder alone never pays for it.
			if (!grainError || grainError->params.w != blocksX || grainError->params.h != blocksY)
			{
				pl_fmt vec4f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
				pl_tex_params params{};
				params.w = blocksX;
				params.h = blocksY;
				params.format = vec4f;
				params.sampleable = true;
				params.storable = true;
				params.host_readable = settings.hostReadable && vec4f && (vec4f->caps & PL_FMT_CAP_HOST_READABLE);
				if (!vec4f || !pl_tex_recreate(gpu, &grainError, &params))
				{
					error = "cannot create the grain error texture";
					return false;
				}
			}
			const pl_shader_desc grainDesc[5] = {
				Sampled("from_half", halfA, false), Sampled("to_half", halfB, true),
				Sampled("field", field, false), Storage("dst", grainError), Sampled("sub_field", subField, false) };
			static const std::string grainSplitHeader = std::string("#define SPLIT 1\n") + GRAIN_ERROR_HEADER;
			if (!Dispatch("VideoProcessor de-judder: grain error (adaptive downscale)",
				settings.estSplit ? grainSplitHeader.c_str() : GRAIN_ERROR_HEADER,
				GRAIN_ERROR_BODY, grainDesc, settings.estSplit ? 5 : 4, nullptr, 0, blocksX, blocksY, 8))
				return false;
			grainErrorValid = true;
		}
		estimated = true;
		return true;
	}

	bool MotionWarp::Smooth(pl_tex raw)
	{
		if (!raw || !field || !halfA || !halfB) { error = "Smooth without an Estimate"; return false; }
		const float tolerance = settings.matchTolerance;
		const pl_shader_var variables[1] = { { pl_var_float("tolerance"), &tolerance, true } };
		const pl_shader_desc smoothDesc[4] = {
			Sampled("raw", raw, false), Sampled("from_half", halfA, false),
			Sampled("to_half", halfB, true), Storage("dst", field) };
		return Dispatch("VideoProcessor de-judder: vector smoothing (D3)", SMOOTH_HEADER, SMOOTH_BODY,
			smoothDesc, 4, variables, 1, blocksX, blocksY, 8);
	}

	const char* FieldVectorGlsl()
	{
		return WARP_HEADER;
	}

	std::string WarpHeader(const Settings& settings, bool split, bool camera)
	{
		std::string h;
		if (settings.objectStrength < 1.0f)
		{
			h += "#define OBJECT 1\n";
			if (camera) h += "#define CAMERA 1\n";
		}
		if (split) h += "#define SPLIT 1\n";
		if (settings.holdFeatherPx > 0.0f) h += "#define FEATHER 1\n";
		if (settings.blockTaper) h += "#define TAPER 1\n";
		return h + WARP_HEADER;
	}

	const char* FieldVectorGlslSplit()
	{
		static const std::string header = std::string("#define SPLIT 1\n") + WARP_HEADER;
		return header.c_str();
	}

	bool MotionWarp::Warp(const Planes& a, float shift, Planes& out, bool trusted)
	{
		out = a;
		if (!trusted || !estimated || shift == 0.0f)
			return true;   // hold: A itself
		// The warped planes were made for the frame Estimate saw: refuse a
		// different size or chroma layout (e.g. 4:2:2 -> 4:2:0) rather than
		// write into the old one.
		if (!a.chroma || !warpedLuma || !warpedChroma || a.luma->params.w != width || a.luma->params.h != height ||
			a.chroma->params.w != warpedChroma->params.w || a.chroma->params.h != warpedChroma->params.h ||
			a.chroma->params.format != warpedChroma->params.format ||
			a.luma->params.format != warpedLuma->params.format)
		{
			error = "Warp without a matching Estimate";
			return false;
		}
		// `src` is linear for the warp; the hold reads it with texelFetch.
		const float minShare = settings.minTrustedShare;
		const float tolerance = settings.matchTolerance;
		// D5: B, its backward vectors and its occlusion bound only when on.
		const bool fill = settings.occlusionFill && fillReady && lastB.luma && lastB.chroma;
		// 11a E2: the sub-field bound (and read) only while the split is on.
		const bool split = settings.estSplit && subField;
		// Delta 14 R-B: the camera from D1 (when it runs).
		const bool withCamera = Camera() != nullptr;
		// M6: the pixel weights (source warp).
		const bool pixel = settings.pixelTrust && pixelMaskReady && pixelMask;
		const std::string header = std::string(fill ? "#define FILL\n" : "") + (pixel ? "#define PIXEL_TRUST\n" : "") +
			(pixel && settings.pixelTrustedVectors ? "#define PIXEL_TRUSTED_VECTORS 1\n" : "") +
			(pixel && PixelOwnVectors() ? "#define PIXEL_OWN_VECTORS 1\n" : "") +
			(pixel && settings.pixelTrustMotion ? "#define PIXEL_MOTION 1\n" : "") +
			(pixel && settings.landingCheck > 0.0f ? "#define LAND_A " + std::to_string(settings.landingCheck) + "\n" : std::string()) +
			WarpHeader(settings, split, withCamera) + (fill ? FILL_HEADER : "") + (pixel ? FIELD_ALL_GLSL : "");
		const float objectStrength = settings.objectStrength;
		// Delta 9: H1 in luma (source) pixels; H3 thresholds.
		const float featherPx = settings.holdFeatherPx;
		const float taperTol = settings.matchTolerance;
		const float taperSadW = settings.taperSad > 0.0f ? settings.taperSad : 2.0f * settings.matchTolerance;
		std::vector<pl_shader_desc> lumaDesc = {
			Sampled("src", a.luma, true), Sampled("field", field, false),
			Sampled("frame_trust", frameTrust, false), Storage("dst", warpedLuma) };
		if (fill)
		{
			lumaDesc.push_back(Sampled("src_b", lastB.luma, true));
			lumaDesc.push_back(Sampled("fill_bwd", backward, false));
			lumaDesc.push_back(Sampled("fill_occ", fillOcclusion, false));
		}
		if (split)
			lumaDesc.push_back(Sampled("sub_field", subField, false));
		if (pixel)
			lumaDesc.push_back(Sampled("pix_mask", pixelMask, true));
		if (pixel && PixelOwnVectors())
			lumaDesc.push_back(Sampled("vector_ok", vectorOk, false));
		if (withCamera)
			lumaDesc.push_back(Sampled("camera", camera, false));
		std::vector<pl_shader_var> lumaVars = { { pl_var_float("shift"), &shift, true },
			{ pl_var_float("min_trusted_share"), &minShare, true } };
		if (fill) lumaVars.push_back({ pl_var_float("fill_tolerance"), &tolerance, true });
		if (settings.holdFeatherPx > 0.0f) lumaVars.push_back({ pl_var_float("feather_px"), &featherPx, true });
		if (settings.blockTaper)
		{
			lumaVars.push_back({ pl_var_float("taper_tol"), &taperTol, true });
			lumaVars.push_back({ pl_var_float("taper_sad"), &taperSadW, true });
		}
		if (settings.objectStrength < 1.0f) lumaVars.push_back({ pl_var_float("object_strength"), &objectStrength, true });
		if (!Dispatch("VideoProcessor de-judder: warp luma", header.c_str(), WARP_LUMA_BODY,
			lumaDesc.data(), static_cast<int>(lumaDesc.size()), lumaVars.data(), static_cast<int>(lumaVars.size()),
			width, height, 16))
			return false;
		const float chromaScale[2] = {
			static_cast<float>(1 << a.chromaShiftX), static_cast<float>(1 << a.chromaShiftY) };
		std::vector<pl_shader_desc> chromaDesc = {
			Sampled("src", a.chroma, true), Sampled("field", field, false),
			Sampled("frame_trust", frameTrust, false), Storage("dst", warpedChroma) };
		if (fill)
		{
			chromaDesc.push_back(Sampled("src_b", lastB.chroma, true));
			chromaDesc.push_back(Sampled("fill_bwd", backward, false));
			chromaDesc.push_back(Sampled("fill_occ", fillOcclusion, false));
		}
		if (split)
			chromaDesc.push_back(Sampled("sub_field", subField, false));
		if (pixel)
			chromaDesc.push_back(Sampled("pix_mask", pixelMask, true));
		if (pixel && PixelOwnVectors())
			chromaDesc.push_back(Sampled("vector_ok", vectorOk, false));
		if (withCamera)
			chromaDesc.push_back(Sampled("camera", camera, false));
		std::vector<pl_shader_var> chromaVars = {
			{ pl_var_float("shift"), &shift, true },
			{ pl_var_vec2("chroma_scale"), chromaScale, true },
			{ pl_var_float("min_trusted_share"), &minShare, true } };
		if (fill) chromaVars.push_back({ pl_var_float("fill_tolerance"), &tolerance, true });
		if (settings.holdFeatherPx > 0.0f) chromaVars.push_back({ pl_var_float("feather_px"), &featherPx, true });
		if (settings.blockTaper)
		{
			chromaVars.push_back({ pl_var_float("taper_tol"), &taperTol, true });
			chromaVars.push_back({ pl_var_float("taper_sad"), &taperSadW, true });
		}
		if (settings.objectStrength < 1.0f) chromaVars.push_back({ pl_var_float("object_strength"), &objectStrength, true });
		if (!Dispatch("VideoProcessor de-judder: warp chroma", header.c_str(), WARP_CHROMA_BODY,
			chromaDesc.data(), static_cast<int>(chromaDesc.size()), chromaVars.data(), static_cast<int>(chromaVars.size()),
			a.chroma->params.w, a.chroma->params.h, 16))
			return false;
		out.luma = warpedLuma;
		out.chroma = warpedChroma;
		return true;
	}
}
