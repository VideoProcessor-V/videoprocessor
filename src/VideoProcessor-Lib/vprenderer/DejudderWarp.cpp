#include <pch.h>
#include "DejudderWarp.h"

#include <libplacebo/shaders/custom.h>

#include <algorithm>

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
    ivec2 full = textureSize(src, 0);
    float sum = 0.0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            sum += texelFetch(src, min(p * 2 + ivec2(x, y), full - ivec2(1)), 0).r;
    imageStore(dst, p, vec4(sum * 0.25 * codes, 0.0, 0.0, 1.0));
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

		const char* const SEARCH_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
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
    for (int c = 0; c < 4; c++) {
        if (c == 1 && v1c == v1z)
            continue;
        if (c == 2 && (!have_g || v1g == v1z || v1g == v1c))
            continue;
        if (c == 3 && (!have_g || vgf == v1g || vgf == v1z || vgf == v1c))
            continue;
        ivec2 centre = ivec2(round((c == 0 ? v1z : (c == 1 ? v1c : (c == 2 ? v1g : vgf))) * 4.0));
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
    bool good = f.z <= tolerance;
    float trust = (consistent && good) ? 1.0 : 0.0;
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
    float d[64];
    float mean_d = 0.0, mean_y = 0.0;
    for (int i = 0; i < 64; i++) {
        ivec2 p = o + ivec2(i % 8, i / 8);
        float a = from_at(p);
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
)GLSL";

		const char* const FRAME_TRUST_BODY = R"GLSL(
uint i = gl_LocalInvocationIndex;
ivec2 blocks = textureSize(field, 0);
int count = blocks.x * blocks.y;
float sum = 0.0, sum_v = 0.0;
for (int k = int(i); k < count; k += 256) {
    vec4 f = texelFetch(field, ivec2(k % blocks.x, k / blocks.x), 0);
    if (f.z > 0.5) { sum += 1.0; sum_v += length(f.xy); }
}
partial_[i] = sum;
partial_v_[i] = sum_v;
barrier();
for (uint stride = 128u; stride > 0u; stride >>= 1) {
    if (i < stride) {
        partial_[i] += partial_[i + stride];
        partial_v_[i] += partial_v_[i + stride];
    }
    barrier();
}
if (i == 0u) {
    float share = partial_[0] / float(max(count, 1));
    float mean_v = partial_[0] > 0.0 ? partial_v_[0] / partial_[0] : 0.0;
    float scale = 1.0;
    if (motion_hi > 0.0 || trust_full > 0.0) {
        float m = 1.0;
        if (motion_hi > 0.0)
            m *= clamp((mean_v - motion_lo) / max(motion_hi - motion_lo, 1e-3), 0.0, 1.0);
        if (trust_full > 0.0)
            m *= clamp((share - min_share) / max(trust_full - min_share, 1e-3), 0.0, 1.0);
        vec4 prev = imageLoad(state_io, ivec2(0));
        float p = prev.w > 0.5 ? prev.x : m;   // no history yet: start where the frame is
        scale = share < min_share ? 0.0 : clamp(m, p - rate, p + rate);
        imageStore(state_io, ivec2(0), vec4(scale, mean_v, share, 1.0));
    }
    imageStore(dst, ivec2(0), vec4(share, scale, mean_v, 1.0));
}
)GLSL";

		// The vector at a source-luma position: bilinear between the four
		// nearest block centres, trusted blocks only; the position's own
		// block decides whether it is warped at all (untrusted: hold).
		const char* const WARP_HEADER = R"GLSL(
bool field_vector(vec2 lp, out vec2 v)
{
    ivec2 fsize = textureSize(field, 0);
    ivec2 own = clamp(ivec2(floor(lp / 16.0)), ivec2(0), fsize - ivec2(1));
    v = vec2(0.0);
    // Too few trusted blocks (a cut between A and B): hold the whole frame.
    if (texelFetch(frame_trust, ivec2(0), 0).r < min_trusted_share)
        return false;
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

// D2/D4: the strength scale of this source frame (1 when both are off).
float strength_scale() { return texelFetch(frame_trust, ivec2(0), 0).g; }
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
    if (field_vector(c, v)) {
        // Edges clamp (the sampler's address mode): never black.
        outv = texture(src, (c - shift * strength_scale() * v) / vec2(size));
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
    if (field_vector(c * chroma_scale, v)) {
        outv = texture(src, (c - shift * strength_scale() * v / chroma_scale) / vec2(size));
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
		: log(logIn), gpu(gpuIn), settings(settingsIn)
	{
		dispatch = pl_dispatch_create(log, gpu);
	}

	MotionWarp::~MotionWarp()
	{
		for (pl_tex* tex : { &halfA, &halfB, &l1A, &l1B, &l2A, &l2B, &forward, &backward,
			&field, &frameTrust, &occlusion[0], &occlusion[1], &grainError, &warpedLuma, &warpedChroma,
			&rawField, &globalVector[0], &globalVector[1], &strengthState, &fillOcclusion })
			pl_tex_destroy(gpu, tex);
		pl_dispatch_destroy(&dispatch);
	}

	bool MotionWarp::EnsureTextures(const Planes& a)
	{
		if (!dispatch) { error = "no dispatch"; return false; }
		if (!a.luma || !a.chroma) { error = "missing planes"; return false; }
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
			!make(&forward, blocksX, blocksY, vec4f, readable) || !make(&backward, blocksX, blocksY, vec4f, readable) ||
			!make(&field, blocksX, blocksY, vec4f, readable) || !make(&frameTrust, 1, 1, vec4f, readable) ||
			!make(&occlusion[0], blocksX, blocksY, vec4f, readable) || !make(&occlusion[1], blocksX, blocksY, vec4f, readable) ||
			!make(&rawField, blocksX, blocksY, vec4f, readable) || !make(&fillOcclusion, blocksX, blocksY, vec4f, readable) ||
			!make(&globalVector[0], 1, 1, vec4f, readable) || !make(&globalVector[1], 1, 1, vec4f, readable))
		{
			error = "cannot create the motion textures";
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
		if (!pl_dispatch_compute(dispatch, &compute))
		{
			error = std::string("dispatch failed: ") + description;
			return false;
		}
		return true;
	}

	bool MotionWarp::Downsample(pl_tex luma, pl_tex half, pl_tex l1, pl_tex l2, bool fullRange)
	{
		const float codes = fullRange ? 219.0f : 255.0f;
		const pl_shader_var variables[1] = { { pl_var_float("codes"), &codes, true } };
		const pl_shader_desc first[2] = { Sampled("src", luma, false), Storage("dst", half) };
		const pl_shader_desc second[2] = { Sampled("src", half, false), Storage("dst", l1) };
		const pl_shader_desc third[2] = { Sampled("src", l1, false), Storage("dst", l2) };
		return Dispatch("VideoProcessor de-judder: half-resolution luma", nullptr, DOWNSAMPLE_BODY,
				first, 2, variables, 1, halfW, halfH, 16) &&
			Dispatch("VideoProcessor de-judder: luma pyramid 1/8", nullptr, PYRAMID_BODY,
				second, 2, nullptr, 0, l1W, l1H, 8) &&
			Dispatch("VideoProcessor de-judder: luma pyramid 1/32", nullptr, PYRAMID_BODY,
				third, 2, nullptr, 0, l2W, l2H, 8);
	}

	bool MotionWarp::Global(pl_tex fromL1, pl_tex toL1, pl_tex fromL2, pl_tex toL2, pl_tex out)
	{
		const pl_shader_desc descriptors[5] = {
			Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
			Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false), Storage("dst", out) };
		return Dispatch("VideoProcessor de-judder: global motion (D1)", GLOBAL_HEADER, GLOBAL_BODY,
			descriptors, 5, nullptr, 0, 16, 16, 16);
	}

	bool MotionWarp::Search(pl_tex fromHalf, pl_tex toHalf, pl_tex fromL1, pl_tex toL1,
		pl_tex fromL2, pl_tex toL2, pl_tex global, pl_tex out)
	{
		const pl_shader_desc descriptors[8] = {
			// One binding per texture (libplacebo rejects the same texture bound
			// twice in a pass): `to_half` is linear for the half-pixel samples;
			// texelFetch ignores the filter.
			Sampled("from_half", fromHalf, false), Sampled("to_half", toHalf, true),
			Sampled("from_l1", fromL1, false), Sampled("to_l1", toL1, true),
			Sampled("from_l2", fromL2, false), Sampled("to_l2", toL2, false),
			Sampled("global_vec", global, false), Storage("dst", out) };
		const int range = std::max(0, std::min(settings.coarseRange, 16));
		const int useGlobal = settings.globalPrior ? 1 : 0;
		const pl_shader_var variables[2] = { { pl_var_int("coarse_range"), &range, false },
			{ pl_var_int("use_global"), &useGlobal, false } };
		return Dispatch("VideoProcessor de-judder: block search", SEARCH_HEADER, SEARCH_BODY,
			descriptors, 8, variables, 2, blocksX, blocksY, 8);
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
		if (!Downsample(a.luma, halfA, l1A, l2A, a.fullRange) || !Downsample(b.luma, halfB, l1B, l2B, b.fullRange))
			return false;
		if (settings.globalPrior &&
			(!Global(l1A, l1B, l2A, l2B, globalVector[0]) || !Global(l1B, l1A, l2B, l2A, globalVector[1])))
			return false;
		if (!Search(halfA, halfB, l1A, l1B, l2A, l2B, globalVector[0], forward) ||
			!Search(halfB, halfA, l1B, l1A, l2B, l2A, globalVector[1], backward))
			return false;
		// D3 smooths a raw field into `field`; without it the consistency pass
		// writes `field` directly.
		const pl_shader_desc descriptors[3] = {
			Sampled("fwd", forward, false), Sampled("bwd", backward, false),
			Storage("dst", settings.vectorSmooth ? rawField : field) };
		const float tolerance = settings.matchTolerance;
		const float consistency = settings.consistencyPixels;
		const pl_shader_var variables[2] = {
			{ pl_var_float("tolerance"), &tolerance, true },
			{ pl_var_float("consistency"), &consistency, true } };
		if (!Dispatch("VideoProcessor de-judder: consistency", nullptr, CONSISTENCY_BODY,
			descriptors, 3, variables, 2, blocksX, blocksY, 8))
			return false;
		if (settings.vectorSmooth && !Smooth(rawField))
			return false;
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
		const pl_shader_desc trustDesc[3] = { Sampled("field", field, false), Storage("dst", frameTrust), stateDesc };
		if (!Dispatch("VideoProcessor de-judder: frame trust", FRAME_TRUST_HEADER, FRAME_TRUST_BODY,
			trustDesc, 3, trustVars, 5, 16, 16, 16))
			return false;
		if (settings.occlusionFill)
		{
			const pl_shader_desc fillDesc[3] = {
				Sampled("fwd", forward, false), Sampled("bwd", backward, false), Storage("dst", fillOcclusion) };
			if (!Dispatch("VideoProcessor de-judder: occlusion for the fill (D5)", nullptr, OCCLUSION_BODY,
				fillDesc, 3, variables, 2, blocksX, blocksY, 8))
				return false;
			lastB = b;
			fillReady = b.chroma && b.chroma->params.w == a.chroma->params.w &&
				b.chroma->params.h == a.chroma->params.h;
		}
		if (occlusionSlot == 0 || occlusionSlot == 1)
		{
			const pl_shader_desc occlusionDesc[3] = {
				Sampled("fwd", forward, false), Sampled("bwd", backward, false),
				Storage("dst", occlusion[occlusionSlot]) };
			if (!Dispatch("VideoProcessor de-judder: occlusion for the dither", nullptr, OCCLUSION_BODY,
				occlusionDesc, 3, variables, 2, blocksX, blocksY, 8))
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
			const pl_shader_desc grainDesc[4] = {
				Sampled("from_half", halfA, false), Sampled("to_half", halfB, true),
				Sampled("field", field, false), Storage("dst", grainError) };
			if (!Dispatch("VideoProcessor de-judder: grain error (adaptive downscale)", GRAIN_ERROR_HEADER,
				GRAIN_ERROR_BODY, grainDesc, 4, nullptr, 0, blocksX, blocksY, 8))
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

	bool MotionWarp::Warp(const Planes& a, float shift, Planes& out, bool trusted)
	{
		out = a;
		if (!trusted || !estimated || shift == 0.0f)
			return true;   // hold: A itself
		// The warped planes were made for the frame Estimate saw: refuse a
		// different size or chroma layout (e.g. 4:2:2 -> 4:2:0) rather than
		// write into the old one.
		if (!warpedLuma || !warpedChroma || a.luma->params.w != width || a.luma->params.h != height ||
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
		const std::string fillHeader = std::string("#define FILL\n") + WARP_HEADER + FILL_HEADER;
		const char* header = fill ? fillHeader.c_str() : WARP_HEADER;
		const pl_shader_desc lumaDesc[7] = {
			Sampled("src", a.luma, true), Sampled("field", field, false),
			Sampled("frame_trust", frameTrust, false), Storage("dst", warpedLuma),
			Sampled("src_b", lastB.luma, true), Sampled("fill_bwd", backward, false),
			Sampled("fill_occ", fillOcclusion, false) };
		const pl_shader_var lumaVars[3] = { { pl_var_float("shift"), &shift, true },
			{ pl_var_float("min_trusted_share"), &minShare, true },
			{ pl_var_float("fill_tolerance"), &tolerance, true } };
		if (!Dispatch("VideoProcessor de-judder: warp luma", header, WARP_LUMA_BODY,
			lumaDesc, fill ? 7 : 4, lumaVars, fill ? 3 : 2, width, height, 16))
			return false;
		const float chromaScale[2] = {
			static_cast<float>(1 << a.chromaShiftX), static_cast<float>(1 << a.chromaShiftY) };
		const pl_shader_desc chromaDesc[7] = {
			Sampled("src", a.chroma, true), Sampled("field", field, false),
			Sampled("frame_trust", frameTrust, false), Storage("dst", warpedChroma),
			Sampled("src_b", lastB.chroma, true), Sampled("fill_bwd", backward, false),
			Sampled("fill_occ", fillOcclusion, false) };
		const pl_shader_var chromaVars[4] = {
			{ pl_var_float("shift"), &shift, true },
			{ pl_var_vec2("chroma_scale"), chromaScale, true },
			{ pl_var_float("min_trusted_share"), &minShare, true },
			{ pl_var_float("fill_tolerance"), &tolerance, true } };
		if (!Dispatch("VideoProcessor de-judder: warp chroma", header, WARP_CHROMA_BODY,
			chromaDesc, fill ? 7 : 4, chromaVars, fill ? 4 : 3, a.chroma->params.w, a.chroma->params.h, 16))
			return false;
		out.luma = warpedLuma;
		out.chroma = warpedChroma;
		return true;
	}
}
