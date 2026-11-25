#include <pch.h>
#include <VP-V/modules/motion/estimation/matcher/DejudderWarp.h>
#include <vprenderer/GrainPreserve.h>
#include <VP-V/modules/input/source_analysis/picture/shared/PictureRect.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/GapSeed.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>
#include <VP-V/modules/motion/estimation/global_model/CameraGuidanceGpu.h>
#include <VP-V/modules/motion/estimation/global_model/MatcherGlobalD1.h>
#include <VP-V/modules/motion/estimation/global_model/PairGlobalFill.h>
#include <VP-V/modules/control/skeleton/SceneRecordReader.h>
#include <VP-V/core/RobustStatsShaders.h>
#include <VP-V/core/GpuAttributionPl.h>

#include <chrono>

#include <libplacebo/shaders/custom.h>
#include <libplacebo/d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Dejudder
{
	namespace
	{
		// Half-resolution luma in 8-bit code units (2x2 average of Y): `codes`
		// per unit, 255 for limited-range sources (the stored codes) and 219
		// for full range (limited-equivalent codes; delta 5, D5-3).
		// BBP-B B1: the non-fade text (head, core, tail) is the P1 owner's 2x2 level body
		// (input/source_analysis/lane/P1GpuFill.h VpFrameAnalysis::P1Gpu::MatcherHalfHead / Core / Tail, moved byte-identical);
		// only the fade pieces stay here: the fade-corrected B is the matcher's named exception (i), an edit-for-matching
		// derivative (plan lead18/coreplan-B-prereq-P2global-pyramid.md section (b)). The spliced body below is byte-identical to
		// the former single raw string.
		const char* const FADE_PLAIN_PIECE = R"GLSL(#ifdef FADE_PLAIN
    // No fade active: B's uncorrected half-size value, stored by the plain
    // pass (the same value this pass would store).
    if (texelFetch(fade_state, ivec2(0), 0).z <= 0.5)
        v = texelFetch(plain, p, 0).r;
    else
#endif
)GLSL";
		const char* const FADE_APPLY_PIECE = R"GLSL(#ifdef FADE_APPLY
        // M4: B after the fade's global gain and offset (when active).
        vec4 fs = texelFetch(fade_state, ivec2(0), 0);
        if (fs.z > 0.5)
            v = v * fs.x + fs.y;
#endif
)GLSL";
		const std::string& MatcherHalfPass()
		{
			static const std::string body = std::string(VpFrameAnalysis::P1Gpu::MatcherHalfHead()) + FADE_PLAIN_PIECE
				+ VpFrameAnalysis::P1Gpu::MatcherHalfCore() + FADE_APPLY_PIECE + VpFrameAnalysis::P1Gpu::MatcherHalfTail();
			return body;
		}

		// M4: the fade test on the 1/32 pyramids (one work group): A's and B's mean and spread; active when the mean moves smoothly
		// (the same direction as the last pair's, in the per-frame step window) with both spreads above a code. The kernel and its law
		// are the shared unit U7 (VpUnit::CutFadeJudge::FadeKernelHeader / FadeKernelBody, events/shared/CutFadeJudge.h; moved
		// unchanged; CPU twin FadeFit). state_io: gain, offset, active, this pair's change.

		// Black bars (owner 2026-10-14): the picture rectangle in input pixels. The kernel and its law are the shared unit U8
		// (VpUnit::PictureRect, input/source_analysis/picture/shared/PictureRect.h; moved unchanged).

		// (SK-MV: the engine's 8-bit input pass is gone with the render-time engine; the producer makes its own input.)

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
    // Hunt 2 04 M4: the SAD is in 8-bit codes (1.0 passed match_tolerance 3); 1e4 fails
    // every tolerance, taper and split test.
    if (length(m) > reach) {
        imageStore(dst, b, vec4(0.0, 0.0, 1e4, 1.0));
        return;
    }
    float s = 0.0;
    for (int y = -2; y < 10; y++)
        for (int x = -2; x < 10; x++) {
            ivec2 p = b * 8 + ivec2(x, y);
            s += abs(from_at(p) - to_lin(vec2(p) + m));
        }
#ifdef OF_WEIGHT
    // The engine's trust weight for this block (cost / its own consistency), carried in w.
    float w_ = clamp(texelFetch(of_weight, min(b, textureSize(of_weight, 0) - ivec2(1)), 0).r, 0.0, 1.0);
#else
    float w_ = 1.0;
#endif
    imageStore(dst, b, vec4(m, s / 144.0, w_));
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
#ifdef CAMERA_STATIC
    // Plan 19 phase 2: the camera model says nothing moves (zero motion, high confidence).
    if (prev_mean < motion_lo)
        return true;
#endif
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
#ifdef CAMERA_KEEP
    // Plan 19 phase 2: the guided pass verified this block (z >= 0); every other block searches (escalation).
    vec4 cg_k = texelFetch(camera_keep, b, 0);
    if (cg_k.z >= 0.0) {
        imageStore(dst, b, cg_k);
        return;
    }
#endif
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
// The 1/8 level's bilinear samples of `to` around each start (2026-10-15):
// every candidate's sample positions lie on the half-texel lattice
// o1 + start + 0.5 m, m in [-8, 10]^2, so each block samples them once
// (19 x 19 per start) instead of 16 per candidate. The positions are
// multiples of 0.5 (exact in float) and the same texture() calls, so every
// SAD is the same sum of the same values in the same order.
// 4 * 361 = 1444 floats; also reused after L1 for parallel half-pel |diff|
// (8 candidates * 144 pixels = 1152 <= 1444 floats).
shared float l1_tab[4 * 361];
// Cached source patches: 3x3 at 1/32 (L2), 4x4 at 1/8 (L1), 12x12 at 1/2.
shared float l2_from[9];
shared float l1_from[16];
shared float from_win[144];
// Cached target window: 18x18 = 324 floats, covering:
// - L2 target window (2*coarse_range + 3)^2 when <= 18 (coarse_range <= 7, up to 4K+);
// - Half-res 7x7 start window (12 + 7 - 1 = 18 -> 18x18) and 5x5 zero window (16x16 <= 18x18).
shared float to_win[324];

float sad_l1_tab(int k, int dx, int dy)
{
    float s = 0.0;
    for (int y = -1; y < 3; y++)
        for (int x = -1; x < 3; x++)
            s += abs(l1_from[(y + 1) * 4 + x + 1] - l1_tab[k * 361 + (2 * y + dy + 8) * 19 + 2 * x + dx + 8]);
    return s;
}

// The start each candidate belongs to (1..5, 0 = the zero vector, ZGROUP =
// the +-2 around zero) and the starts' centres: a candidate inside an earlier
// start's window repeats an earlier one (owner 2026-10-13, shaky camera).
#ifdef GLOBAL_TWO
// M2: a sixth start, D1's second candidate.
#define L1_NSTARTS 4
#define ZGROUP 7
#else
#define L1_NSTARTS 3
#define ZGROUP 6
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
shared ivec2 s_best;

void sync_group()
{
    memoryBarrierShared();
    barrier();
}

// Picks without one thread scanning every SAD: each thread keeps the best of
// its own candidates, 16 threads reduce SEARCH_NT/16 of those each in place
// into their disjoint slice starts, and thread 0 reduces the last 16. The
// serial pick (strictly smaller, in order, from a given first candidate) is
// the minimum SAD with ties to the first candidate, then to the lowest index;
// rank -1 marks the first candidate. That order is total, so the result is
// exactly the serial pick in any reduction order.
#define PK_EMPTY 0x7fffffff
shared float pk_s[L1_NSTARTS * SEARCH_NT];
shared int pk_r[L1_NSTARTS * SEARCH_NT];

bool pk_better(float s, int r, float bs, int br)
{
    return r != PK_EMPTY && (br == PK_EMPTY || s < bs || (s == bs && r < br));
}

// After pk_s/pk_r [0..SEARCH_NT-1] are written and the group synced: 16 partial picks in place.
void pk_reduce16(int li)
{
    if (li < 16) {
        int base = li * (SEARCH_NT / 16);
        float bs = 0.0;
        int br = PK_EMPTY;
        for (int k = 0; k < SEARCH_NT / 16; k++) {
            int j = base + k;
            if (pk_better(pk_s[j], pk_r[j], bs, br)) { bs = pk_s[j]; br = pk_r[j]; }
        }
        pk_s[base] = bs;
        pk_r[base] = br;
    }
}

// After pk_reduce16 and a sync (thread 0): the winner's rank and SAD.
int pk_final(out float best)
{
    float bs = 0.0;
    int br = PK_EMPTY;
    for (int k = 0; k < 16; k++) {
        int j = k * (SEARCH_NT / 16);
        if (pk_better(pk_s[j], pk_r[j], bs, br)) { bs = pk_s[j]; br = pk_r[j]; }
    }
    best = bs;
    return br;
}

// 12x12 SAD from shared from_win[144] and shared to_win[324] (18x18 covering
// o + centre + [-5..12]^2), for integer offset (dx, dy) in [-3, 3]^2.
// Loop order (y = 0..11 outer, x = 0..11 inner) matches sad() bit-for-bit.
float sad_win_cached(int dx, int dy)
{
    float s = 0.0;
    int ox = dx + 3;
    int oy = dy + 3;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 12; x++)
            s += abs(from_win[y * 12 + x] - to_win[(y + oy) * 18 + x + ox]);
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
if (lead && inside && keep)
    imageStore(dst, b, texelFetch(previous, b, 0));
#ifdef CAMERA_KEEP
// Plan 19 phase 2: the guided pass verified this block (z >= 0); every other block searches (escalation).
if (inside && !keep) {
    vec4 cg_k = texelFetch(camera_keep, b, 0);
    if (cg_k.z >= 0.0) {
        keep = true;
        if (lead)
            imageStore(dst, b, cg_k);
    }
}
#endif
bool run = inside && !keep;
half_size_ = textureSize(from_half, 0);
l1_size_ = textureSize(from_l1, 0);
l2_size_ = textureSize(from_l2, 0);
ivec2 o = b * 8;

// Level 2 (1/32 source): every vector within +-range texels.
// Cache the 3x3 from_l2 patch and (2*range+3)^2 to_l2 window in shared memory
// whenever the window fits inside to_win[324] (l2w <= 18, i.e. coarse_range <= 7).
int side = 2 * coarse_range + 1;
int n2 = side * side;
int l2r = coarse_range + 1;
int l2w = side + 2;
bool l2_cached = coarse_range >= 0 && l2w <= 18;
if (run && l2_cached) {
    ivec2 c2 = b / 2;
    if (li < 9)
        l2_from[li] = l2from(c2 + ivec2(li % 3 - 1, li / 3 - 1));
    int l2n = l2w * l2w;
    for (int t = li; t < l2n; t += NT)
        to_win[t] = l2to(c2 + ivec2(t % l2w - l2r, t / l2w - l2r));
}
sync_group();
{
    // The serial pick starts from the zero vector (rank -1).
    int centre = coarse_range * side + coarse_range;
    float bs = 0.0;
    int br = PK_EMPTY;
    if (run)
        for (int i = li; i < n2; i += NT) {
            int dx = i % side - coarse_range;
            int dy = i / side - coarse_range;
            float s;
            if (l2_cached) {
                s = 0.0;
                for (int y = -1; y <= 1; y++)
                    for (int x = -1; x <= 1; x++)
                        s += abs(l2_from[(y + 1) * 3 + x + 1] - to_win[(y + dy + l2r) * l2w + x + dx + l2r]);
            } else {
                s = sad_l2(b / 2, ivec2(dx, dy));
            }
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
// result and (D1) around a confident global vector; thread-local reduction
// across each start's 169 candidates (zero per-candidate SAD arrays).
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
// The 4x4 from_l1 patch and each start's 19x19 to_l1 half-texel table
// (0 zero, 1 the 1/32 result, 2 the global vector, 3 M2's second).
if (run) {
    if (li < 16)
        l1_from[li] = l1from(b * 2 + ivec2(li % 4 - 1, li / 4 - 1));
    for (int e = li; e < L1_NSTARTS * 361; e += NT) {
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
}
sync_group();
{
    float bz = 0.0, bc = 0.0, bg = 0.0;
    int rz = PK_EMPTY, rc = PK_EMPTY, rg = PK_EMPTY;
#ifdef GLOBAL_TWO
    float bg2 = 0.0;
    int rg2 = PK_EMPTY;
#endif
    if (run)
        for (int i = li; i < 169; i += NT) {
            int dx = i % 13 - 6, dy = i / 13 - 6;
            int r = i == 84 ? -1 : i;
            float sz = sad_l1_tab(0, dx, dy);
            if (pk_better(sz, r, bz, rz)) { bz = sz; rz = r; }
            float sc = sad_l1_tab(1, dx, dy);
            if (pk_better(sc, r, bc, rc)) { bc = sc; rc = r; }
            if (have_g) {
                float sg = sad_l1_tab(2, dx, dy);
                if (pk_better(sg, r, bg, rg)) { bg = sg; rg = r; }
            }
#ifdef GLOBAL_TWO
            if (have_g2) {
                float sg2 = sad_l1_tab(3, dx, dy);
                if (pk_better(sg2, r, bg2, rg2)) { bg2 = sg2; rg2 = r; }
            }
#endif
        }
    pk_s[li] = bz; pk_r[li] = rz;
    pk_s[NT + li] = bc; pk_r[NT + li] = rc;
    pk_s[2 * NT + li] = bg; pk_r[2 * NT + li] = rg;
#ifdef GLOBAL_TWO
    pk_s[3 * NT + li] = bg2; pk_r[3 * NT + li] = rg2;
#endif
}
sync_group();
if (li < L1_NSTARTS * 16) {
    int j = li / 16;
    int p = li % 16;
    int base = j * NT + p * (NT / 16);
    float bs = 0.0;
    int br = PK_EMPTY;
    for (int k = 0; k < NT / 16; k++) {
        int idx = base + k;
        if (pk_better(pk_s[idx], pk_r[idx], bs, br)) { bs = pk_s[idx]; br = pk_r[idx]; }
    }
    pk_s[base] = bs;
    pk_r[base] = br;
}
sync_group();
if (run && lead) {
    int rr[L1_NSTARTS];
    for (int j = 0; j < L1_NSTARTS; j++) {
        float bs = 0.0;
        int br = PK_EMPTY;
        for (int k = 0; k < 16; k++) {
            int idx = j * NT + k * (NT / 16);
            if (pk_better(pk_s[idx], pk_r[idx], bs, br)) { bs = pk_s[idx]; br = pk_r[idx]; }
        }
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
}
if (run) {
    for (int i = li; i < 144; i += NT)
        from_win[i] = from_at(o + ivec2(i % 12 - 2, i / 12 - 2));
    // Preload zero-centred 18x18 target window alongside from_win (zero extra barrier).
    for (int t = li; t < 324; t += NT)
        to_win[t] = to_at(o + ivec2(t % 18 - 5, t / 18 - 5));
}
sync_group();
int ns = run ? s_centres[0].x : 0;
{
    float bs = 0.0;
    int br = PK_EMPTY;
    // Candidate 0 (zero vector, rank 0) and the 5x5 around zero (ranks 1 + 49*ns + li, li != 12),
    // scored from the zero-centred to_win[324] already loaded above.
    if (run && li < 25) {
        int dx = li % 5 - 2;
        int dy = li / 5 - 2;
        ivec2 v = ivec2(dx, dy);
        int rank = li == 12 ? 0 : 1 + 49 * ns + li;
        bool repeat = false;
        if (li != 12) {
            for (int k = 1; k <= ns; k++)
                repeat = repeat || (abs(v.x - s_centres[k].x) <= 3 && abs(v.y - s_centres[k].y) <= 3);
        }
        if (!repeat) {
            float s = sad_win_cached(dx, dy);
            if (pk_better(s, rank, bs, br)) { bs = s; br = rank; }
        }
    }
    // Each start k = 1..ns: load its 18x18 to_win[324] cooperatively across all NT threads,
    // then score its 7x7 = 49 candidates from shared memory. Constant loop bound ZGROUP keeps
    // both sync_group() calls in statically uniform control flow (D3D11 X3663 / SPIR-V).
    for (int k = 1; k < ZGROUP; k++) {
        sync_group();
        if (run && k <= ns) {
            ivec2 c = s_centres[k];
            for (int t = li; t < 324; t += NT)
                to_win[t] = to_at(o + c + ivec2(t % 18 - 5, t / 18 - 5));
        }
        sync_group();
        if (run && k <= ns && li < 49) {
            int dx = li % 7 - 3;
            int dy = li / 7 - 3;
            ivec2 v = s_centres[k] + ivec2(dx, dy);
            bool repeat = v == ivec2(0);
            for (int j = 1; j < k; j++)
                repeat = repeat || (abs(v.x - s_centres[j].x) <= 3 && abs(v.y - s_centres[j].y) <= 3);
            if (!repeat) {
                int rank = 1 + (k - 1) * 49 + li;
                float s = sad_win_cached(dx, dy);
                if (pk_better(s, rank, bs, br)) { bs = s; br = rank; }
            }
        }
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
// Half-pixel refinement (bilinear samples of `to`): all NT threads compute the
// 8 * 144 = 1152 pixel |diff| values in parallel into l1_tab (dead after L1,
// 1444 >= 1152 floats), then 8 threads sum each candidate serially in the exact
// (y, x) order of SEARCH_BODY_SERIAL (100% bit-identical float summation order).
if (run) {
    vec2 fbase = vec2(s_best);
    for (int e = li; e < 8 * 144; e += NT) {
        int c = e / 144;
        int p = e % 144;
        int idx = c < 4 ? c : c + 1;
        vec2 v = fbase + 0.5 * vec2(idx % 3 - 1, idx / 3 - 1);
        ivec2 xy = o + ivec2(p % 12 - 2, p / 12 - 2);
        l1_tab[e] = abs(from_win[p] - to_lin(vec2(xy) + v));
    }
}
sync_group();
if (run && li < 8) {
    float s = 0.0;
    int base = li * 144;
    for (int p = 0; p < 144; p++)
        s += l1_tab[base + p];
    sub_sads[li < 4 ? li : li + 1] = s;
}
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
#ifdef CAMERA_STATIC
    // Plan 19 phase 2: the camera model says nothing moves (zero motion, high confidence).
    if (prev_mean < motion_lo)
        return true;
#endif
    return gv.w > 0.5 && gv.z < 0.9 && length(gv.xy) * 8.0 < motion_lo && prev_mean < motion_lo;
}
)GLSL";

		// The Upper median of up to nine values: the GPU twin owned by core/RobustStatsShaders.h (C6-2 U24),
		// `float vpu_median_upper(float a[9], int n)`; the same text as the former private median_ copy.
		const char* const MEDIAN_GLSL = RobustStats::kMedianUpper9Glsl;

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
#ifdef CAMERA_KEEP
    // Plan 19 phase 2 (E1 skip): the first sweep agreed with the model and passed verification.
    vec4 cg_k = texelFetch(camera_keep, b, 0);
    if (cg_k.z >= 0.0) {
        imageStore(dst, b, cg_k);
        return;
    }
#endif
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
    vec2 med = nn > 0 ? vec2(vpu_median_upper(xs, nn), vpu_median_upper(ys, nn)) : cand[0];
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
#ifdef COST_W
    // The engine's trust weight stays with its own vector (within a half-size pixel).
    float w_ = length(fbest - cand[0]) <= 1.0 ? texelFetch(sweep, b, 0).w : 1.0;
#else
    float w_ = 1.0;
#endif
    imageStore(dst, b, vec4(fbest, best_sad / 144.0, w_));
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
shared vec2 rf_cand[2][12];
shared vec2 rf_med[2];
shared int rf_nc[2];
shared int rf_dup[2][16];

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
if (lead && inside && keep)
    imageStore(dst, b, texelFetch(previous, b, 0));
#ifdef CAMERA_KEEP
// Plan 19 phase 2 (E1 skip): the first sweep agreed with the model and passed verification.
if (inside && !keep) {
    vec4 cg_k = texelFetch(camera_keep, b, 0);
    if (cg_k.z >= 0.0) {
        keep = true;
        if (lead)
            imageStore(dst, b, cg_k);
    }
}
#endif
bool run = inside && !keep;
half_size_ = textureSize(from_half, 0);
ivec2 o = b * 8;
// Only lead (li == 0) per sub-block builds the candidate list, computes the
// neighbours' median, and marks exact duplicates (first-occurrence index j < k,
// else -1) so the other 15 threads skip duplicate fetches and duplicate 144-tap SADs.
if (run && lead) {
    float xs[9];
    float ys[9];
    int nc = 0, nn = 0;
    rf_cand[sub][nc++] = texelFetch(sweep, b, 0).xy;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            ivec2 c = b + ivec2(dx, dy);
            if (c.x < 0 || c.y < 0 || c.x >= blocks.x || c.y >= blocks.y) continue;
            vec2 v = texelFetch(sweep, c, 0).xy;
            rf_cand[sub][nc++] = v;
            xs[nn] = v.x;
            ys[nn] = v.y;
            nn++;
        }
    if (use_temporal != 0)
        rf_cand[sub][nc++] = texelFetch(previous, b, 0).xy;
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
    if (use_global != 0 && gv.w > 0.5 && gv.z < 0.9)
        rf_cand[sub][nc++] = gv.xy * 4.0;
    if (use_of != 0)
        rf_cand[sub][nc++] = texelFetch(of_vec, b, 0).xy;
    rf_med[sub] = nn > 0 ? vec2(vpu_median_upper(xs, nn), vpu_median_upper(ys, nn)) : rf_cand[sub][0];
    rf_nc[sub] = nc;
    for (int k = 0; k < nc; k++) {
        int dup = -1;
        for (int j = 0; j < k; j++) {
            if (all(equal(rf_cand[sub][k], rf_cand[sub][j]))) {
                dup = j;
                break;
            }
        }
        rf_dup[sub][k] = dup;
    }
}
if (run)
    for (int i = li; i < 144; i += 16)
        rf_from[sub * 144 + i] = from_at(o + ivec2(i % 12 - 2, i / 12 - 2));
pb_sync();
if (run && li < rf_nc[sub] && rf_dup[sub][li] < 0)
    rf_sads[sub * 16 + li] = rf_sad_sub(sub, o, rf_cand[sub][li]);
pb_sync();
if (run && lead) {
    int nc = rf_nc[sub];
    vec2 med = rf_med[sub];
    vec2 best = rf_cand[sub][0];
    float best_sad = rf_sads[sub * 16];
    float best_score = best_sad / 144.0 + lambda * 2.0 * length(best - med);
    for (int k = 1; k < 12; k++) {
        if (k >= nc) break;
        int dup = rf_dup[sub][k];
        float s = dup >= 0 ? rf_sads[sub * 16 + dup] : rf_sads[sub * 16 + k];
        float score = s / 144.0 + lambda * 2.0 * length(rf_cand[sub][k] - med);
        if (score < best_score) { best_score = score; best_sad = s; best = rf_cand[sub][k]; }
    }
    rf_best[sub] = best;
    rf_best_sad[sub] = best_sad;
}
pb_sync();
vec2 best = run ? rf_best[sub] : vec2(0.0);
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
#ifdef COST_W
    // The engine's trust weight stays with its own vector (within a half-size pixel).
    float w_ = length(fbest - rf_cand[sub][0]) <= 1.0 ? texelFetch(sweep, b, 0).w : 1.0;
#else
    float w_ = 1.0;
#endif
    imageStore(dst, b, vec4(fbest, best_sad / 144.0, w_));
}
)GLSL";

		// GEM-144 (MC-SEARCH-2): 64-thread work group per block for quarter-pel refinement.
		// All 64 threads compute the 9 * 144 = 1296 candidate pixel diffs in parallel (coalesced
		// 2D texture reads of to_half), then 9 threads sum each candidate in the exact serial
		// (y, x) order of QPEL_BODY_SERIAL (100% bit-identical, 5,796 B shared memory < 16 KB).
		const char* const QPEL_HEADER = R"GLSL(
shared float qp_from[144];
shared float qp_diff[1296];
shared float qp_sads[9];
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
if (run) {
    for (int i = li; i < 144; i += 64)
        qp_from[i] = from_at(o + ivec2(i % 12 - 2, i / 12 - 2));
}
memoryBarrierShared();
barrier();
if (run) {
    for (int e = li; e < 1296; e += 64) {
        int c = e / 144;
        int p = e % 144;
        vec2 v = c == 4 ? v0 : v0 + 0.25 * vec2(c % 3 - 1, c / 3 - 1);
        ivec2 xy = o + ivec2(p % 12 - 2, p / 12 - 2);
        qp_diff[e] = abs(qp_from[p] - to_lin(vec2(xy) + v));
    }
}
memoryBarrierShared();
barrier();
if (run && li < 9) {
    float s = 0.0;
    int base = li * 144;
    for (int p = 0; p < 144; p++)
        s += qp_diff[base + p];
    qp_sads[li] = s;
}
memoryBarrierShared();
barrier();
if (inside && lead) {
    if (run) {
        vec2 best = v0;
        float best_sad = qp_sads[4];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                float s = qp_sads[(dy + 1) * 3 + dx + 1];
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

		// Identical blocks are still (bug hunt 2026-10-25): a block whose pixels A and B hold
		// bit-identically gets exactly the zero vector, trusted, error 0. The searches (and the
		// optical-flow engine) left sub-pixel noise vectors on a repeated frame (mean 0.03 px, up
		// to 1.26 px), which the interpolator resampled. block_px: the block's size in these planes'
		// pixels (the searches' 8 half-size texels: 16; 8 on the sub-field). On the sub-field w = 1
		// (the sub-block's own vector; w = 0 would mean "the parent block's vector"); on the block
		// field w is the SAD (0). dejudder_est_still_zero switches the pass (Original: off).
		// S3-H5: blocks whose centre lies outside the active picture rect (black bars) keep their
		// incoming vector/trust unchanged so StillZero never restores trust=1.0 on masked bars.
		const char* const STILL_ZERO_HEADER = R"GLSL(
#ifdef PICTURE
bool in_picture_(vec2 c) { vec4 r = texelFetch(picture, ivec2(0), 0); return c.x >= r.x && c.y >= r.y && c.x < r.z && c.y < r.w; }
#else
bool in_picture_(vec2 c) { return true; }
#endif
)GLSL";

		const char* const STILL_ZERO_BODY = R"GLSL(
#ifdef IN_PLACE
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(field_io);
if (b.x < size.x && b.y < size.y) {
    ivec2 bs = ivec2(block_px);
    ivec2 o = b * bs;
    if (in_picture_(vec2(o) + vec2(bs) * 0.5)) {
        ivec2 psz = textureSize(luma_a, 0);
        bool same = true;
        bool any_px = false;
        for (int y = 0; y < bs.y && same; y++)
            for (int x = 0; x < bs.x; x++) {
                ivec2 p = o + ivec2(x, y);
                if (p.x >= psz.x || p.y >= psz.y)
                    continue;
                any_px = true;
                if (texelFetch(luma_a, p, 0).r != texelFetch(luma_b, p, 0).r) { same = false; break; }
            }
        if (same && any_px) {
#ifdef SUB_FIELD
            imageStore(field_io, b, vec4(0.0, 0.0, 1.0, 1.0));
#else
            imageStore(field_io, b, vec4(0.0, 0.0, 1.0, 0.0));
#endif
        }
    }
}
#else
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (b.x < size.x && b.y < size.y) {
    vec4 f = texelFetch(src_field, b, 0);
    ivec2 bs = ivec2(block_px);
    ivec2 o = b * bs;
    if (!in_picture_(vec2(o) + vec2(bs) * 0.5)) {
        imageStore(dst, b, f);
    } else {
        ivec2 psz = textureSize(luma_a, 0);
        bool same = true;
        bool any_px = false;
        for (int y = 0; y < bs.y && same; y++)
            for (int x = 0; x < bs.x; x++) {
                ivec2 p = o + ivec2(x, y);
                if (p.x >= psz.x || p.y >= psz.y)
                    continue;
                any_px = true;
                if (texelFetch(luma_a, p, 0).r != texelFetch(luma_b, p, 0).r) { same = false; break; }
            }
#ifdef SUB_FIELD
        imageStore(dst, b, (same && any_px) ? vec4(0.0, 0.0, 1.0, 1.0) : f);
#else
        imageStore(dst, b, (same && any_px) ? vec4(0.0, 0.0, 1.0, 0.0) : f);
#endif
    }
}
#endif
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

		// D1 whole-frame translation shaders (U10 C2): moved byte-identical to global_model/MatcherGlobalD1.h (VpMotion::MatcherGlobalD1).

		// D3: median smoothing of the trusted field (see Settings).
		const std::string SMOOTH_HEADER = std::string(R"GLSL(
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
})GLSL") + RobustStats::kMedianUpper9Glsl;   // the median of the D3 smoothing: core/RobustStatsShaders.h (C6-2 U24)

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
        vec2 med = vec2(vpu_median_upper(xs, n), vpu_median_upper(ys, n));
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
    // Rolling 2-row cache fetches each from_half texel once (81 vs 192 fetches)
    // with exact edge clamping and exact (y, x) float summation order.
    ivec2 hs_ = textureSize(from_half, 0);
    ivec2 lim_ = hs_ - ivec2(2);
    ivec2 o_ = clamp(b * 8, ivec2(0), lim_);
    int py_cur_ = o_.y;
    float cur_[9];
    float nxt_[9];
    for (int k_ = 0; k_ < 9; k_++) {
        int px_ = min(o_.x + k_, hs_.x - 1);
        cur_[k_] = texelFetch(from_half, ivec2(px_, py_cur_), 0).r;
        nxt_[k_] = texelFetch(from_half, ivec2(px_, min(py_cur_ + 1, hs_.y - 1)), 0).r;
    }
    float tex_ = 0.0;
    for (int y = 0; y < 8; y++) {
        int py_ = clamp(b.y * 8 + y, 0, lim_.y);
        if (py_ != py_cur_) {
            for (int k_ = 0; k_ < 9; k_++) {
                cur_[k_] = nxt_[k_];
                nxt_[k_] = texelFetch(from_half, ivec2(min(o_.x + k_, hs_.x - 1), min(py_ + 1, hs_.y - 1)), 0).r;
            }
            py_cur_ = py_;
        }
        for (int x = 0; x < 8; x++) {
            int ix_ = clamp(b.x * 8 + x, 0, lim_.x) - o_.x;
            float c0 = cur_[ix_];
            tex_ += abs(cur_[ix_ + 1] - c0) + abs(nxt_[ix_] - c0);
        }
    }
    float extra_ = texture_tol * tex_ / 128.0;
#else
    float extra_ = 0.0;
#endif
    // decision_: the error the trust decision sees. The field's w keeps the raw SAD
    // (integration 2026-10-17: the engine's penalty used to inflate the stored SAD, which
    // the grain map, the taper and the occlusion fill read as a worse match).
    float decision_ = f.z;
#ifdef OF_TRUST
    // The engine's trust weight (w: its cost, its own forward/backward consistency; 1 =
    // none): a less sure engine vector counts as a worse match - tapered, or untrusted.
    decision_ += (1.0 - clamp(f.w, 0.0, 1.0)) * of_penalty;
#endif
#ifdef TAPER
    bool good = decision_ <= taper_sad + extra_;   // delta 9 H3: marginal blocks warp, tapered
#else
    bool good = decision_ <= tolerance + extra_;
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
		// GEM-144 (MC-SEARCH-2): 64-thread work group (8x8) per block replaces the 64-float
		// per-thread register array d[64] and parallelizes all 128 texture fetches across the
		// block (544 B shared memory < 16 KB), then thread 0 sums in exact i = 0..63 serial order.
		const char* const GRAIN_ERROR_HEADER = R"GLSL(
shared float ge_d[64];
shared float ge_a[64];
shared vec2 ge_v[4];
ivec2 half_size_;
float from_at(ivec2 p) { return texelFetch(from_half, clamp(p, ivec2(0), half_size_ - ivec2(1)), 0).r; }
float to_lin(vec2 p) { return texture(to_half, (p + vec2(0.5)) / vec2(half_size_)).r; }
)GLSL";

		const char* const GRAIN_ERROR_BODY = R"GLSL(
ivec2 b = ivec2(gl_WorkGroupID.xy);
int li = int(gl_LocalInvocationIndex);
half_size_ = textureSize(from_half, 0);
if (li == 0) {
    vec2 v0 = texelFetch(field, b, 0).xy * 0.5;
#ifdef SPLIT
    // 11a E2: a split block's quadrants at their own sub-block vectors.
    for (int k = 0; k < 4; k++) {
        vec4 sf = texelFetch(sub_field, b * 2 + ivec2(k % 2, k / 2), 0);
        ge_v[k] = sf.w > 0.5 ? sf.xy * 0.5 : v0;
    }
#else
    ge_v[0] = v0;
#endif
}
memoryBarrierShared();
barrier();
{
    ivec2 p = b * 8 + ivec2(li % 8, li / 8);
    float a = from_at(p);
#ifdef SPLIT
    vec2 v = ge_v[(li % 8) / 4 + 2 * ((li / 8) / 4)];
#else
    vec2 v = ge_v[0];
#endif
    ge_a[li] = a;
    ge_d[li] = a - to_lin(vec2(p) + v);
}
memoryBarrierShared();
barrier();
if (li == 0) {
    float mean_d = 0.0, mean_y = 0.0;
    for (int i = 0; i < 64; i++) {
        mean_d += ge_d[i];
        mean_y += ge_a[i];
    }
    mean_d /= 64.0;
    float e = 0.0;
    for (int i = 0; i < 64; i++)
        e += abs(ge_d[i] - mean_d);
    imageStore(dst, b, vec4(e / 64.0, mean_y / 64.0, 0.0, 1.0));
}
)GLSL";

		// Frame trust (6b-2): the share of trusted blocks, by one 16x16 group,
		// into a 1x1 texture. It stays on the GPU: the warp reads it, so a cut
		// between A and B holds the whole frame without a read-back.
		// D2/D4: the same reduction also sums the trusted |v| and writes the
		// strength scale (g), rate-limited against the last one (state_io).
		const char* const FRAME_TRUST_HEADER_A = R"GLSL(
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
)GLSL";

		const char* const FRAME_TRUST_HEADER_B = R"GLSL(#endif
#if defined(CAMERA_OUT) || defined(ROLLOFF_GLOBAL)
shared vec3 partial_c_[256];
#endif
)GLSL";

		// The header with the M3 histogram declaration of the shared unit U6 (SourceStats::CoarseHistHeader) spliced in; the
		// text is the one the matcher always carried.
		const std::string& FrameTrustHeader()
		{
			static const std::string header = std::string(FRAME_TRUST_HEADER_A) + VpUnit::SourceStats::CoarseHistHeader() +
				FRAME_TRUST_HEADER_B;
			return header;
		}

		const char* const FRAME_TRUST_BODY_A = R"GLSL(
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
#if defined(CAMERA_OUT) || defined(ROLLOFF_GLOBAL)
// Delta 14 R-B / R-A (fixed 2026-10-16): the camera, refined from the global vector to the
// mean of the trusted blocks within camera_window of it. The global moves in half-texel steps
// (4 px at full size, 8 px with the low-resolution matcher), coarser than the roll-off's 16-px
// ramp; the refined camera is the field's own speed (the D1 search and the engine agree on it).
{
    vec4 gv = texelFetch(global_vec, ivec2(0), 0);
    bool reliable = gv.w > 0.5 && gv.z < 0.9;
    vec2 gpx = gv.xy * 8.0;
    vec3 c = vec3(0.0);
    for (int k = int(i); k < count; k += 256) {
        ivec2 bk = ivec2(k % blocks.x, k / blocks.x);
        if (!in_picture_(vec2(bk * 16) + vec2(8.0)))
            continue;
        vec4 f = texelFetch(field, bk, 0);
        if (reliable && f.z > 0.5 && length(f.xy - gpx) < camera_window) c += vec3(f.xy, 1.0);
    }
    partial_c_[i] = c;
}
barrier();
for (uint stride = 128u; stride > 0u; stride >>= 1) {
    if (i < stride)
        partial_c_[i] += partial_c_[i + stride];
    barrier();
}
#endif
#ifdef REGIONAL
)GLSL";

		const char* const FRAME_TRUST_BODY_B = R"GLSL(#endif
if (i == 0u) {
    float share = partial_[0] / max(partial_n_[0], 1.0);
#ifdef U7_SCENE
    bool cut = (u7_cut > 0) || (share < 0.05);
#ifdef REGIONAL
    float ramp_lo = 0.0;
#else
    float ramp_lo = min_share;
#endif
#else
#ifdef REGIONAL
)GLSL";

		const char* const FRAME_TRUST_BODY_C = R"GLSL(    float ramp_lo = 0.0;
#else
)GLSL";

		const char* const FRAME_TRUST_BODY_D = R"GLSL(    float ramp_lo = min_share;
#endif
#endif
    float mean_v = partial_[0] > 0.0 ? partial_v_[0] / partial_[0] : 0.0;
    float scale = 1.0;
#ifdef ROLLOFF
    const bool rolloff = true;
#else
    const bool rolloff = false;
#endif
#ifdef U7_SCENE
    bool fading = (u7_fade > 0);
#elif defined(FADE)
    bool fading = texelFetch(fade_state, ivec2(0), 0).z > 0.5;
#else
    const bool fading = false;
#endif
    if (motion_hi > 0.0 || trust_full > 0.0 || rolloff || fading) {
        float m = 1.0;
#if defined(FADE) || defined(U7_SCENE)
        if (fading)
            m *= fade_floor;   // M4: a fade warps at most at the floor
#endif
        if (motion_hi > 0.0)
            m *= clamp((mean_v - motion_lo) / max(motion_hi - motion_lo, 1e-3), 0.0, 1.0);
#ifdef ROLLOFF
        // Delta 14 R-A: the fall above a fast pan, on the frame's motion.
        float frame_v = mean_v;
#ifdef ROLLOFF_GLOBAL
        // The refined camera speed (above); the global itself when no trusted block is near it.
        vec4 gv = texelFetch(global_vec, ivec2(0), 0);
        if (gv.w > 0.5 && gv.z < 0.9)
            frame_v = partial_c_[0].z > 0.0 ? length(partial_c_[0].xy / partial_c_[0].z) : length(gv.xy) * 8.0;
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
#ifdef U7_SCENE
    else if (u7_cut > 0) {
        scale = 0.0;
        imageStore(state_io, ivec2(0), vec4(0.0, mean_v, 0.0, 1.0));
    }
    float trust_out = cut ? ((u7_cut > 0) ? 0.0 : share) : max(share, min_share);
#else
    float trust_out = cut ? share : max(share, min_share);
#endif
    // M3: a hard-motion frame that is not a cut reports at least min_share,
    // so the holds (warps, hysteresis, adaptive downscale) stay per block.
    imageStore(dst, ivec2(0), vec4(trust_out, scale, mean_v, 1.0));
}
#ifdef CAMERA_OUT
// Delta 14 R-B: the refined camera (computed above).
if (i == 0u) {
    vec3 c = partial_c_[0];
    imageStore(camera_out, ivec2(0), c.z > 0.0 ? vec4(c.xy / c.z, 1.0, 1.0) : vec4(0.0, 0.0, 0.0, 1.0));
}
#endif
)GLSL";

		// The trust pass: the M3 histogram sweep (U6, SourceStats::CoarseHistBody) and the cut lines (U7, CutFadeJudge::
		// TrustCutRegionalLine / TrustCutPlainLine) are the shared units' text, spliced between the matcher's own pieces; the
		// assembled kernel is the text the matcher always carried.
		const std::string& FrameTrustBody()
		{
			static const std::string body = std::string(FRAME_TRUST_BODY_A) + VpUnit::SourceStats::CoarseHistBody() +
				FRAME_TRUST_BODY_B + VpUnit::CutFadeJudge::TrustCutRegionalLine() + FRAME_TRUST_BODY_C +
				VpUnit::CutFadeJudge::TrustCutPlainLine() + FRAME_TRUST_BODY_D;
			return body;
		}

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
    float f = max(feather, 1e-3);
    // GEM-145: hold_weight only inspects the 8 neighbours of `own` (dx, dy != 0, 0).
    // Any pixel at distance >= f from all 4 edges of `own`'s 16x16 block has
    // distance >= f to every 3x3 neighbour block (and every 8x8 sub-block inside
    // one), so smoothstep(0.0, f, d) == 1.0 identically without fetching neighbours.
    vec2 local = lp - vec2(own) * 16.0;
    if (local.x >= f && 16.0 - local.x >= f && local.y >= f && 16.0 - local.y >= f)
        return 1.0;
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
    return smoothstep(0.0, f, d);
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
		// GEM-151 (MC-SEARCH-2): cooperatively cache the 10x10 search neighborhood in shared
		// memory (1,600 B < 16 KB, single uniform barrier), deduplicate parent+neighbor vectors
		// once per split block, and cache each quadrant's 4x4 from_half patch once in registers
		// (16 vs up to 416 fetches/quadrant). Exact tie-breaking and float summation order.
		const char* const SPLIT_SEARCH_BODY = R"GLSL(
shared vec4 sp_search[10][10];
float sp_fq_[16];

float sad_q(ivec2 o, vec2 v)
{
    float s = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            ivec2 p = o + ivec2(x, y);
            s += abs(sp_fq_[y * 4 + x] - to_lin(vec2(p) + v));
        }
    return s;
}

void main_split()
{
    ivec2 wg0 = ivec2(gl_WorkGroupID.xy) * 8 - ivec2(1);
    ivec2 loc = ivec2(gl_LocalInvocationID.xy);
    ivec2 b = ivec2(gl_GlobalInvocationID.xy);
    ivec2 blocks = textureSize(search, 0);
    int li = int(gl_LocalInvocationIndex);
    for (int i = li; i < 100; i += 64) {
        int tx = i % 10;
        int ty = i / 10;
        sp_search[ty][tx] = texelFetch(search, clamp(wg0 + ivec2(tx, ty), ivec2(0), blocks - ivec2(1)), 0);
    }
    memoryBarrierShared();
    barrier();
    if (b.x >= blocks.x || b.y >= blocks.y)
        return;
    vec4 fs = sp_search[loc.y + 1][loc.x + 1];
    if (fs.z <= split_sad) {
        for (int q = 0; q < 4; q++)
            imageStore(dst, b * 2 + ivec2(q % 2, q / 2), vec4(fs.xyz, 0.0));
        return;
    }
    half_size_ = textureSize(from_half, 0);
    vec2 cand[9];
    int nc = 0;
    cand[nc++] = fs.xy;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            vec2 v = sp_search[loc.y + 1 + dy][loc.x + 1 + dx].xy;
            bool dup = false;
            for (int j = 0; j < nc; j++) {
                if (all(equal(v, cand[j]))) { dup = true; break; }
            }
            if (!dup)
                cand[nc++] = v;
        }
    for (int q = 0; q < 4; q++) {
        ivec2 sb = b * 2 + ivec2(q % 2, q / 2);
        ivec2 o = b * 8 + ivec2(q % 2, q / 2) * 4;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                sp_fq_[y * 4 + x] = from_at(o + ivec2(x, y));
        vec2 best = cand[0];
        float best_sad = sad_q(o, best);
        for (int k = 1; k < 9; k++) {
            if (k >= nc) break;
            float s = sad_q(o, cand[k]);
            if (s < best_sad) { best_sad = s; best = cand[k]; }
        }
        vec2 centre = round(best);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                vec2 v = centre + vec2(dx, dy);
                bool seen = false;
                for (int j = 0; j < nc; j++) {
                    if (all(equal(v, cand[j]))) { seen = true; break; }
                }
                if (seen) continue;
                float s = sad_q(o, v);
                if (s < best_sad) { best_sad = s; best = v; }
            }
        vec2 fbest = best;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                vec2 v = best + 0.5 * vec2(dx, dy);
                vec2 d = clamp(round(v - centre), vec2(-1.0), vec2(1.0));
                if (all(equal(v, centre + d))) continue;
                bool seen = false;
                for (int j = 0; j < nc; j++) {
                    if (all(equal(v, cand[j]))) { seen = true; break; }
                }
                if (seen) continue;
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

		// dejudder_source_warp_filter (owner 2026-10-17, grain audit): the warp's source
		// samples. bilinear = the hardware's linear filter (today's: 0.5x of white grain at a
		// half-pixel shift); catmull = Catmull-Rom from texelFetch (GrainPreserve; 0.8x or
		// more at any shift, exact at whole pixels). Positions normalised as for texture().
		const char* const WARP_SAMPLE_BILINEAR = "#define WARP_SRC(uv) texture(src, uv)\n#define WARP_SRC_B(uv) texture(src_b, uv)\n";
		const char* const WARP_SAMPLE_CATMULL = R"GLSL(
DEFINE_CATMULL(warp_cat_src, src)
#define WARP_SRC(uv) warp_cat_src((uv) * vec2(textureSize(src, 0)))
#ifdef FILL
DEFINE_CATMULL(warp_cat_src_b, src_b)
#define WARP_SRC_B(uv) warp_cat_src_b((uv) * vec2(textureSize(src_b, 0)))
#endif
)GLSL";

		// B3 (2026-10-24): the luma may be at another size than the estimate (the processed
		// frame: re-based, repaired, 4K-routed). luma_scale = estimate px per target px; the field,
		// masks and thresholds stay in estimate (source) px (lc = c * luma_scale), displacements
		// come back as v / luma_scale. At luma_scale (1, 1) every expression is exactly the old one.
		const char* const WARP_LUMA_BODY = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec2 c = vec2(q) + vec2(0.5);
    vec2 lc = c * luma_scale;   // the field's (estimate) pixels
    vec2 v;
    vec4 outv;
#ifdef PIXEL_TRUST
    // M6: every block's vector, blended with the hold by A's pixel weight.
    if (texelFetch(frame_trust, ivec2(0), 0).r >= min_trusted_share) {
        // The vector where the pixel comes from (two fixed-point steps: at a
        // moving edge the output position's own vector is the wrong one).
        float sc = shift * strength_scale();
        vec2 va = field_vector_all(lc);
        va = field_vector_all(lc - sc * va);
        va = field_vector_all(lc - sc * va);
#ifdef OBJECT
        va = object_scale(va);
#endif
        vec2 from = c - sc * va / luma_scale;
        float m = texture(pix_mask, from / vec2(size)).r;
#ifdef LAND_A
        {
            // The landing check (owner 2026-10-14): the source pixel counts
            // only if its own motion carries it back here (else it is a valid
            // pixel that is not the one visible here - an occlusion).
            vec2 vl = field_vector_all(from * luma_scale);
#ifdef OBJECT
            vl = object_scale(vl);
#endif
            if (length(from * luma_scale + sc * vl - lc) > land_a) m = 0.0;
        }
#endif
#ifdef PIXEL_MOTION
        outv = WARP_SRC((c - m * sc * va / luma_scale) / vec2(size));   // the weight's share of the motion
#else
        outv = mix(texelFetch(src, q, 0), WARP_SRC(from / vec2(size)), m);
#endif
    } else
        outv = texelFetch(src, q, 0);
    imageStore(dst, q, outv);
    return;
#endif
    if (field_vector(lc, v)) {
        // Edges clamp (the sampler's address mode): never black.
        outv = WARP_SRC((c - shift * strength_scale() * block_strength(lc) * v / luma_scale) / vec2(size));
#ifdef FEATHER
        outv = mix(texelFetch(src, q, 0), outv, hold_weight(lc, feather_px));
#endif
    } else {
#ifdef FILL
        vec2 pb;
        if (fill_position(lc, shift * strength_scale(), pb))
            outv = WARP_SRC_B(pb / luma_scale / vec2(size));
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
            if (length(from * chroma_scale + sc * vl - c * chroma_scale) > land_a) m = 0.0;
        }
#endif
#ifdef PIXEL_MOTION
        outv = WARP_SRC((c - m * sc * va / chroma_scale) / vec2(size));
#else
        outv = mix(texelFetch(src, q, 0), WARP_SRC(from / vec2(size)), m);
#endif
    } else
        outv = texelFetch(src, q, 0);
    imageStore(dst, q, outv);
    return;
#endif
    if (field_vector(c * chroma_scale, v)) {
        outv = WARP_SRC((c - shift * strength_scale() * block_strength(c * chroma_scale) * v / chroma_scale) / vec2(size));
#ifdef FEATHER
        outv = mix(texelFetch(src, q, 0), outv, hold_weight(c * chroma_scale, feather_px));
#endif
    } else {
#ifdef FILL
        vec2 pb;
        if (fill_position(c * chroma_scale, shift * strength_scale(), pb))
            outv = WARP_SRC_B(pb / chroma_scale / vec2(size));
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

		// GEM-147: thread-safe bitmask-keyed shader header cache (std::map node addresses stay stable across inserts).
		struct ShaderHeaderCache
		{
			std::mutex mutex;
			std::map<uint32_t, std::string> entries;
			template <typename Builder>
			const char* Get(uint32_t key, Builder&& build)
			{
				std::lock_guard<std::mutex> lock(mutex);
				auto it = entries.find(key);
				if (it == entries.end())
					it = entries.emplace(key, build()).first;
				return it->second.c_str();
			}
		};
	}

	MotionWarp::MotionWarp(pl_log logIn, pl_gpu gpuIn, const Settings& settingsIn)
		: log(logIn), gpu(gpuIn), settings(settingsIn), base(settingsIn),
		lastPairGlobalFallback(FallbackCatalog::PairGlobalUnavailable(false))
	{
		dispatch = pl_dispatch_create(log, gpu);
		GpuAttribution::Watch(dispatch, "dejudder");
	}

	const FallbackCatalog::Decision& MotionWarp::LastPairGlobalFallback() const
	{
		return HalvesForSearch() && inner ? inner->LastPairGlobalFallback() : lastPairGlobalFallback;
	}

	void MotionWarp::SetSkeletonMemo(VpSkeleton::SkeletonMemo* memo, uint64_t seq)
	{
		skeletonMemo = memo;
		skeletonSeq = seq;
		if (inner)
			inner->SetSkeletonMemo(memo, seq);
	}

	void MotionWarp::SetSceneVerdictReader(SceneVerdictFn reader)
	{
		sceneVerdictReader = reader;
		if (inner)
			inner->SetSceneVerdictReader(reader);
	}

	uint64_t MotionWarp::U7SceneHits() const
	{
		return HalvesForSearch() && settings.lowResAll && inner ? inner->U7SceneHits() : u7SceneHits;
	}

	uint64_t MotionWarp::U7SceneFallbacks() const
	{
		return HalvesForSearch() && settings.lowResAll && inner ? inner->U7SceneFallbacks() : u7SceneFallbacks;
	}

	const std::string& MotionWarp::LastU7SceneFallback() const
	{
		return HalvesForSearch() && settings.lowResAll && inner ? inner->LastU7SceneFallback() : lastU7SceneFallback;
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
		ReleaseBorrowed();   // L-B4: the product's textures are borrowed, never destroyed here; the scratch ones go back to their slots
		for (pl_tex* tex : { &halfA, &halfB, &l1A, &l1B, &l2A, &l2B, &halfK, &l1K, &l2K, &forward, &backward,
			&field, &frameTrust, &occlusion[0], &occlusion[1], &grainError, &warpedLuma, &warpedChroma, &processedLuma, &processedChroma,
			&rawField, &globalVector[0], &globalVector[1], &strengthState, &fillOcclusion,
			&sweep[0], &sweep[1], &ofSearch[0], &ofSearch[1], &ofNone, &previousSearch[0], &previousSearch[1], &qpelField, &subField,
			&subSearch[0], &subSearch[1], &holdState[0], &holdState[1], &subHoldState[0], &subHoldState[1],
			&hystField, &hystSub, &globalSad2, &globalSad1, &globalPick, &globalBothSad2, &globalBothSad1, &globalBothPick, &snapField, &snapTrust, &snapSub, &snapCamera, &camera, &globalSecond[0], &globalSecond[1], &fadeState, &aheadTrust, &verifyField, &stillField, &stillSub, &pixelMask, &snapMask, &vectorOk, &snapVectorOk, &picture,
			&ofFlowTex[0], &ofFlowTex[1], &ofWeight, &cgGuide, &cgCode, &cgKeep[0], &cgKeep[1], &cgCounts, &seedCheck })
			pl_tex_destroy(gpu, tex);
		for (int i = 0; i < 2; ++i) { ofFlow[i].Release(); }
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
		// GEM-141: record Estimate's plane layout and destroy stale warped output
		// planes on dimension/format change or luma-only input; allocate
		// warpedLuma / warpedChroma lazily in WarpImpl so estimation-only,
		// rendered-warp, camera-only, and WarpProcessed paths never allocate them.
		const bool hasWarpChroma = !innerOfLowRes && a.chroma != nullptr;
		lumaFormat = hasWarpChroma ? a.luma->params.format : nullptr;
		chromaW = hasWarpChroma ? a.chroma->params.w : 0;
		chromaH = hasWarpChroma ? a.chroma->params.h : 0;
		chromaFormat = hasWarpChroma ? a.chroma->params.format : nullptr;
		const bool warpedLumaMismatch = warpedLuma && (!hasWarpChroma || warpedLuma->params.w != width ||
			warpedLuma->params.h != height || warpedLuma->params.format != lumaFormat);
		const bool warpedChromaMismatch = warpedChroma && (!hasWarpChroma || warpedChroma->params.w != chromaW ||
			warpedChroma->params.h != chromaH || warpedChroma->params.format != chromaFormat);
		if (warpedLumaMismatch || warpedChromaMismatch)
		{
			pl_tex_destroy(gpu, &warpedLuma);
			pl_tex_destroy(gpu, &warpedChroma);
		}
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
		auto make = [this](pl_tex* tex, int w, int h, pl_fmt fmt, bool readable, bool blitDst = false) -> bool
		{
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = fmt;
			params.sampleable = true;
			params.storable = true;
			params.blit_dst = blitDst && (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
			params.host_readable = readable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
			return pl_tex_recreate(gpu, tex, &params);
		};
		const bool readable = settings.hostReadable;
		pyramidFmt = scalar;
		// L-B4 LOW-3: while the shared product is bound, A's own pyramid and the K set are not kept (the product replaces them;
		// B's own set stays for the fade exception): a slot freed then stays empty here and is made again by EnsureOwnPyramidScratch
		// only when the matcher builds its own pyramid.
		const auto ownLevel = [&](pl_tex* tex, int w, int h) { return (ownPyramidLazy && *tex == nullptr) || make(tex, w, h, scalar, readable); };
		if (!ownLevel(&halfA, halfW, halfH) || !make(&halfB, halfW, halfH, scalar, readable) ||
			!ownLevel(&l1A, l1W, l1H) || !make(&l1B, l1W, l1H, scalar, readable) ||
			!ownLevel(&l2A, l2W, l2H) || !make(&l2B, l2W, l2H, scalar, readable) ||
			!ownLevel(&halfK, halfW, halfH) || !ownLevel(&l1K, l1W, l1H) ||
			!ownLevel(&l2K, l2W, l2H) ||
			!make(&forward, blocksX, blocksY, vec4f, readable, true) || !make(&backward, blocksX, blocksY, vec4f, readable, true) ||
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
			// GEM-143 (MC-READBACK / P1-1): reuse holdState / subHoldState on holdResetPending via
			// pl_tex_clear_ex instead of destroying/recreating textures + allocating a multi-MB zero vector.
			const bool resized = !holdState[0] || holdState[0]->params.w != blocksX || holdState[0]->params.h != blocksY;
			const bool reset = holdResetPending;
			holdResetPending = false;
			const bool canClear = (vec4f->caps & PL_FMT_CAP_BLITTABLE) != 0;
			const bool recreate = resized || (reset && !canClear);
			for (int i = 0; i < 2; ++i)
				if (recreate)
				{
					pl_tex_destroy(gpu, &holdState[i]);
					pl_tex_destroy(gpu, &subHoldState[i]);
				}
			const union pl_clear_color zeroColor = { { 0.0f, 0.0f, 0.0f, 0.0f } };
			auto state = [&](pl_tex* tex, int w, int h) -> bool
			{
				if (*tex)
				{
					if (reset)
						pl_tex_clear_ex(gpu, *tex, zeroColor);
					return true;
				}
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = vec4f;
				tp.sampleable = true;
				tp.storable = true;
				tp.blit_dst = canClear;
				tp.host_readable = readable && (vec4f->caps & PL_FMT_CAP_HOST_READABLE);
				if (canClear)
				{
					*tex = pl_tex_create(gpu, &tp);
					if (*tex)
						pl_tex_clear_ex(gpu, *tex, zeroColor);
					return *tex != nullptr;
				}
				std::vector<uint16_t> zero(static_cast<size_t>(w) * h * 4, 0);
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
		return true;
	}

	// Identical blocks get the exact zero vector: in-place via read-write storage image
	// when supported (preserving fieldTex object identity without scratch/copy), else via scratch.
	bool MotionWarp::StillZero(pl_tex& fieldTex, pl_tex& scratch, const Planes& a, const Planes& b, int w, int h, int blockPixels,
		bool subField)
	{
		if (!fieldTex || !a.luma || !b.luma)
			return true;
		const bool same = a.luma == b.luma;   // one texture bound once (libplacebo rejects it twice)
		const bool usePicture = settings.pictureArea && picture;
		const bool inPlace = fieldTex->params.format && (fieldTex->params.format->caps & PL_FMT_CAP_READWRITE) != 0;
		const int block = blockPixels;
		const pl_shader_var vars[1] = { { pl_var_int("block_px"), &block, true } };
		static ShaderHeaderCache headerCache;
		const uint32_t headerKey = (inPlace ? 1u : 0u) | (subField ? 2u : 0u) | (same ? 4u : 0u) | (usePicture ? 8u : 0u);
		const char* header = headerCache.Get(headerKey, [&]() {
			return std::string(inPlace ? "#define IN_PLACE 1\n" : "") +
				(subField ? "#define SUB_FIELD 1\n" : "") +
				(same ? "#define luma_b luma_a\n" : "") + (usePicture ? "#define PICTURE 1\n" : "") + STILL_ZERO_HEADER;
		});
		if (inPlace)
		{
			pl_shader_desc fieldDesc{};
			fieldDesc.desc.name = "field_io";
			fieldDesc.desc.type = PL_DESC_STORAGE_IMG;
			fieldDesc.desc.access = PL_DESC_ACCESS_READWRITE;
			fieldDesc.binding.object = fieldTex;
			pl_shader_desc desc[4];
			int numDesc = 0;
			desc[numDesc++] = fieldDesc;
			desc[numDesc++] = Sampled("luma_a", a.luma, false);
			if (!same)
				desc[numDesc++] = Sampled("luma_b", b.luma, false);
			if (usePicture)
				desc[numDesc++] = Sampled("picture", picture, false);
			return Dispatch("VideoProcessor de-judder: identical blocks still", header, STILL_ZERO_BODY,
				desc, numDesc, vars, 1, w, h, 8);
		}
		pl_tex_params params = fieldTex->params;
		if (!pl_tex_recreate(gpu, &scratch, &params))
		{
			error = "cannot create the still-block field";
			return false;
		}
		pl_shader_desc desc[5];
		int numDesc = 0;
		desc[numDesc++] = Sampled("src_field", fieldTex, false);
		desc[numDesc++] = Sampled("luma_a", a.luma, false);
		if (!same)
			desc[numDesc++] = Sampled("luma_b", b.luma, false);
		desc[numDesc++] = Storage("dst", scratch);
		if (usePicture)
			desc[numDesc++] = Sampled("picture", picture, false);
		if (!Dispatch("VideoProcessor de-judder: identical blocks still", header, STILL_ZERO_BODY,
			desc, numDesc, vars, 1, w, h, 8))
			return false;
		// Copied back (not swapped): the published field / sub-field textures stay the same objects.
		const pl_shader_desc copyDesc[2] = { Sampled("src", scratch, false), Storage("dst", fieldTex) };
		return Dispatch("VideoProcessor de-judder: identical blocks still (copy back)", nullptr, COPY_BODY, copyDesc, 2,
			nullptr, 0, w, h, 8);
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
		pl_gpu_finish(gpu);   // once: the inner shares the GPU (it used to finish a second time)
		PollPassTimers();
	}

	// Efficiency (motion-timing extraction, 2026-10-24): collect whatever timer results are ready,
	// never waiting on the GPU. With SetPassTiming(true, false) this is the production-safe path
	// (WP-5's SetPassSampling / TakePassGpuMs arm it 1 frame in N; group D removed the duplicate
	// SampledPassTimers); FlushPassTimers stays the tests' drain.
	void MotionWarp::PollPassTimers()
	{
		for (const auto& entry : passTimers)
			CollectTimer(entry.first, entry.second);
		if (inner)
			inner->PollPassTimers();
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
		// L-B4 (BBP-B B3): without the fade this is the owner's law (P1Gpu::BuildPyramidInto, the one code copy; also what the
		// product's fallback runs). The fade exception (M4) keeps the matcher's own half pass below, then the same pyramid levels.
		if (!fade)
			return BuildOwnPyramid(luma, half, l1, l2, fullRange, -1, levels);
		const float codes = VpFrameAnalysis::P1Gpu::MatcherCodes(lumaInCodes, fullRange);
		const pl_shader_var variables[1] = { { pl_var_float("codes"), &codes, true } };
		const bool withPlain = fade && plain;
		const pl_shader_desc first[4] = { Sampled("src", luma, false), Storage("dst", half), Sampled("fade_state", fade, false),
			Sampled("plain", plain, false) };
		// The header carries the fade defines and the shared half-luma accessor (vpu_half_luma, unit U3).
		static ShaderHeaderCache fadeApplyHeaders;
		const uint32_t headerKey = (withPlain ? 1u : 0u) | (fade ? 2u : 0u);
		const char* header = fadeApplyHeaders.Get(headerKey, [&]() {
			return std::string(withPlain ? "#define FADE_APPLY 1\n#define FADE_PLAIN 1\n" : fade ? "#define FADE_APPLY 1\n" : "") +
				VpUnit::HalfLuma::Glsl();
		});
		return Dispatch(fade ? "VideoProcessor de-judder: half-resolution luma after the fade (M4)" : "VideoProcessor de-judder: half-resolution luma",
				header, MatcherHalfPass().c_str(),
				first, withPlain ? 4 : fade ? 3 : 2, variables, 1, halfW, halfH, 16) &&
			Pyramid(half, l1, l2, 0, levels);
	}

	bool MotionWarp::Pyramid(pl_tex half, pl_tex l1, pl_tex l2, int from, int levels)
	{
		return BuildOwnPyramid(nullptr, half, l1, l2, false, from, levels);
	}

	// The owner's pyramid law (P1Gpu::BuildPyramidInto) run through this matcher's own Dispatch into its own scratch textures:
	// the non-fade Downsample, Pyramid, and the visible fallback row product.motion_warp (B3).
	bool MotionWarp::BuildOwnPyramid(pl_tex luma, pl_tex half, pl_tex l1, pl_tex l2, bool fullRange, int from, int levels)
	{
		const float codes = VpFrameAnalysis::P1Gpu::MatcherCodes(lumaInCodes, fullRange);
		const VpFrameAnalysis::P1Gpu::PyramidPassRunner run = [this](const VpFrameAnalysis::P1Gpu::PyramidPass& pass) -> bool
		{
			const pl_shader_desc d[2] = { Sampled("src", pass.src, false), Storage("dst", pass.dst) };
			float passCodes = pass.codes;
			const pl_shader_var v[1] = { { pl_var_float("codes"), &passCodes, true } };
			return Dispatch(pass.description, pass.header.empty() ? nullptr : pass.header.c_str(), pass.body->c_str(), d, 2,
				pass.hasCodes ? v : nullptr, pass.hasCodes ? 1 : 0, pass.width, pass.height, pass.group);
		};
		return VpFrameAnalysis::P1Gpu::BuildPyramidInto(run, luma, codes, half, l1, l2, from, levels);
	}

	void MotionWarp::ReleaseBorrowed()
	{
		pl_tex* slot[6] = { &halfA, &l1A, &l2A, &halfB, &l1B, &l2B };
		for (int i = 0; i < 6; ++i)
		{
			// A borrowed slot gets its own texture back, or nothing when that one was freed (LOW-3): never the product's texture.
			if (borrowed[i])
			{
				*slot[i] = parkedOwn[i];
				parkedOwn[i] = nullptr;
				borrowed[i] = false;
			}
			pins[i].reset();
		}
		pyramidBound = false;
	}

	void MotionWarp::ReleaseBorrowedB()
	{
		pl_tex* slot[3] = { &halfB, &l1B, &l2B };
		for (int i = 0; i < 3; ++i)
			if (borrowed[3 + i])
			{
				*slot[i] = parkedOwn[3 + i];
				parkedOwn[3 + i] = nullptr;
				borrowed[3 + i] = false;
			}
	}

	// B3: the corrected B of the fade exception (M4) is rebuilt in the matcher's own scratch from the fade state; the plain B stays
	// what it was (in the K set without the product, the product's own level with it: the next A acquires it by sequence).
	bool MotionWarp::RebuildBWithFade(const Planes& b, pl_tex fadeTex, int levels)
	{
		if (pyramidBound)
		{
			const pl_tex plain = halfB;   // the product's plain B half
			ReleaseBorrowedB();           // B's slots are the matcher's own scratch again; the pins keep the product alive
			return Downsample(b.luma, halfB, l1B, l2B, b.fullRange, fadeTex, levels, plain);
		}
		KeepPlainBInK();
		return Downsample(b.luma, halfB, l1B, l2B, b.fullRange, fadeTex, levels, halfK);
	}

	// B3: A's and B's levels (levelsA / levelsB) from the shared product, bound where the matcher reads them. True = all bound.
	// False = nothing bound: the matcher builds its own (the product's source is absent: silent, as before; not served: the visible
	// row product.motion_warp via PyramidFallbackWhy()).
	bool MotionWarp::BindProductPyramid(const Planes& a, const Planes& b, int levelsA, int levelsB)
	{
		pyramidBound = false;
		if (inner)
			inner->pyramidFallbackWhy.clear();   // MED-1: the inner's decline counts only for the estimate it ran in
		if (!pyramidSource || a.sequence == 0 || b.sequence == 0 || a.sequence == b.sequence || !a.luma || !b.luma)
		{
			pyramidFallbackWhy.clear();
			return false;
		}
		pl_tex* slot[6] = { &halfA, &l1A, &l2A, &halfB, &l1B, &l2B };
		const int sizes[3][2] = { { halfW, halfH }, { l1W, l1H }, { l2W, l2H } };
		const int base = innerOfLowRes ? 2 : 1;   // the levels this instance's half / 1/8 / 1/32 are in the product (1,3,5 / 2,4,6)
		pl_tex got[6] = {};
		std::shared_ptr<const void> held[6];
		std::string why;
		bool configured = true;
		// B first: B is offered in this render frame, A only kept from when it was B. Asking B's levels first makes them even when
		// A's are not served (the first pair after a start, a gap or a fallback), so the NEXT pair's A is served; A first would
		// leave every later A without a kept product (a permanent fallback).
		if (!innerOfLowRes && HalvesForSearch())
		{
			// The lowRes outer: its inner reads levels 2 / 4 / 6 only when this instance is bound; B's are made here in any case,
			// for the same reason (the next pair's inner A).
			for (int lv = 2; lv <= 6; lv += 2)
			{
				VpMotion::MatcherPyramid::Request q;
				q.seq = b.sequence;
				q.level = lv;
				q.lumaTex = b.luma;
				q.fullRange = b.fullRange;
				if (pyramidSource(q).state != VpMotion::MatcherPyramid::Level::State::Product)
					break;   // not served: the inner's own bind shows why when it reads them
			}
		}
		for (int s = 0; s < 2 && configured && why.empty(); ++s)
		{
			const int side = 1 - s;
			const Planes& p = side ? b : a;
			// B is asked for every level the NEXT pair's A will read (levelsA may exceed levelsB: the lowRes outer's picture area).
			const int levels = side ? (levelsA > levelsB ? levelsA : levelsB) : levelsA;
			const void* src = innerOfLowRes ? productLuma[side] : static_cast<const void*>(p.luma);
			for (int lv = 0; lv <= levels && configured && why.empty(); ++lv)
			{
				const int i = side * 3 + lv;
				VpMotion::MatcherPyramid::Request q;
				q.seq = p.sequence;
				q.level = base + 2 * lv;
				q.lumaTex = src;
				q.fullRange = p.fullRange;   // MED-3: the range the matcher's own level 1 would be coded with
				const VpMotion::MatcherPyramid::Level r = pyramidSource(q);
				if (r.state == VpMotion::MatcherPyramid::Level::State::NotConfigured)
				{
					configured = false;
					break;
				}
				if (r.state != VpMotion::MatcherPyramid::Level::State::Product || !r.tex)
				{
					why = r.cause.empty() ? std::string("not served") : r.cause;
					break;
				}
				const pl_tex t = static_cast<pl_tex>(r.tex);
				// The matcher's own scratch format (EnsureTextures; its A slots may be freed while the product is bound, LOW-3).
				const pl_fmt f = t->params.format, of = pyramidFmt;
				if (t->params.w != sizes[lv][0] || t->params.h != sizes[lv][1])
					why = "the shared level has another size than the matcher's";
				else if (!t->params.sampleable || !f || !of || settings.hostReadable ||
					(f != of && (f->type != of->type || f->num_components != of->num_components ||
						f->component_depth[0] != of->component_depth[0])) || !(f->caps & PL_FMT_CAP_LINEAR))
					why = "the shared level has another format than the matcher's";
				else
				{
					got[i] = t;
					held[i] = r.pin;
				}
			}
		}
		if (!configured)
		{
			pyramidFallbackWhy.clear();
			return false;
		}
		if (!why.empty())
		{
			pyramidFallbackWhy = why;
			return false;
		}
		pyramidFallbackWhy.clear();
		for (int i = 0; i < 6; ++i)
			if (got[i])
			{
				parkedOwn[i] = *slot[i];
				*slot[i] = got[i];
				pins[i] = held[i];
				borrowed[i] = true;
			}
		// LOW-3: the product replaces A's own levels and the K set (the last pair's plain B: A is now Acquire(a.seq)); they are freed
		// while it is bound and made again only when the matcher builds its own (EnsureOwnPyramidScratch). B's own set is kept: the
		// fade exception rebuilds the corrected B in it.
		for (int i = 0; i < 3; ++i)
			if (parkedOwn[i])
				pl_tex_destroy(gpu, &parkedOwn[i]);
		for (pl_tex* tex : { &halfK, &l1K, &l2K })
			pl_tex_destroy(gpu, tex);
		ownPyramidLazy = true;
		pyramidBound = true;
		return true;
	}

	bool MotionWarp::EnsureOwnPyramidScratch()
	{
		if (!ownPyramidLazy)
			return true;
		if (!pyramidFmt)
		{
			error = "no storable float format";
			return false;
		}
		const auto make = [this](pl_tex* tex, int w, int h) -> bool
		{
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = pyramidFmt;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = settings.hostReadable && (pyramidFmt->caps & PL_FMT_CAP_HOST_READABLE);
			return pl_tex_recreate(gpu, tex, &params);
		};
		if (!make(&halfA, halfW, halfH) || !make(&l1A, l1W, l1H) || !make(&l2A, l2W, l2H) ||
			!make(&halfK, halfW, halfH) || !make(&l1K, l1W, l1H) || !make(&l2K, l2W, l2H))
		{
			error = "cannot create the motion textures";
			return false;
		}
		ownPyramidLazy = false;
		return true;
	}

	void MotionWarp::KeepPlainBInK()
	{
		std::swap(halfB, halfK);
		std::swap(l1B, l1K);
		std::swap(l2B, l2K);
		keptInK = true;
	}

	bool MotionWarp::Search(pl_tex fromHalf, pl_tex toHalf, pl_tex fromL1, pl_tex toL1,
		pl_tex fromL2, pl_tex toL2, pl_tex global, pl_tex previous, pl_tex out, pl_tex globalTwo,
		pl_tex cameraKeep, bool cameraStatic)
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
		const int range = DejudderPolicy::ScaledSearchRange(settings.coarseRange, settings.referenceHeight, height);
		const int useGlobal = settings.globalPrior ? 1 : 0;
		const int useTemporal = settings.estCandidates && previousValid ? 1 : 0;
		const int useGate = settings.estGate && settings.globalPrior && previousValid ? 1 : 0;
		const float motionLo = settings.motionScaleLo;
		const pl_shader_var variables[5] = { { pl_var_int("coarse_range"), &range, false },
			{ pl_var_int("use_global"), &useGlobal, false },
			{ pl_var_int("use_temporal"), &useTemporal, false },
			{ pl_var_int("use_gate"), &useGate, false },
			{ pl_var_float("motion_lo"), &motionLo, true } };
		// Plan 19 phase 2 (camera guidance): only with a guidance frame; otherwise the dispatch below as before.
		const bool two = globalTwo && settings.globalPrior;
		if (cameraKeep || cameraStatic)
		{
			pl_shader_desc d[12];
			int numD = two && !serialSearchForTest ? 11 : 10;
			for (int i = 0; i < numD; ++i) d[i] = descriptors[i];
			if (cameraKeep)
				d[numD++] = Sampled("camera_keep", cameraKeep, false);
			static ShaderHeaderCache guidedSearchHeaders;
			const uint32_t key = (serialSearchForTest ? 1u : 0u) | (two ? 2u : 0u) |
				(cameraKeep ? 4u : 0u) | (cameraStatic ? 8u : 0u);
			const char* h = guidedSearchHeaders.Get(key, [&]() {
				const std::string defines = std::string(cameraKeep ? "#define CAMERA_KEEP 1\n" : "") +
					(cameraStatic ? "#define CAMERA_STATIC 1\n" : "");
				if (serialSearchForTest)
					return defines + SEARCH_HEADER;
				return std::string("#define SEARCH_NT 128\n") + (two ? "#define GLOBAL_TWO 1\n" : "") + defines +
					SEARCH_HEADER + SEARCH_PARALLEL_HEADER;
			});
			if (serialSearchForTest)
				return Dispatch("VideoProcessor de-judder: block search (serial reference)", h,
					SEARCH_BODY_SERIAL, d, numD, variables, 5, blocksX, blocksY, 8);
			return DispatchPerBlock("VideoProcessor de-judder: block search", h, SEARCH_BODY,
				d, numD, variables, 5, blocksX, blocksY, 16, 128 / 16);
		}
		if (serialSearchForTest)
			return Dispatch("VideoProcessor de-judder: block search (serial reference)", SEARCH_HEADER,
				SEARCH_BODY_SERIAL, descriptors, 10, variables, 5, blocksX, blocksY, 8);
		// 128 threads per block (2026-10-15, laptop 3050 Ti 4K, alternating runs: block search 5.36/6.31/5.43 ms
		// against 5.86/5.84/5.96 ms with 256 and 6.02 ms with 64; matcher 8.3-8.8 against 9.1-9.2 ms).
		const int nt = 128;
		static const std::string header = std::string("#define SEARCH_NT 128\n") + SEARCH_HEADER + SEARCH_PARALLEL_HEADER;
		static const std::string twoHeader = std::string("#define SEARCH_NT 128\n#define GLOBAL_TWO 1\n") + SEARCH_HEADER + SEARCH_PARALLEL_HEADER;
		return DispatchPerBlock("VideoProcessor de-judder: block search", two ? twoHeader.c_str() : header.c_str(), SEARCH_BODY,
			descriptors, two ? 11 : 10, variables, 5, blocksX, blocksY, 16, nt / 16);
	}

	bool MotionWarp::Refine(pl_tex fromHalf, pl_tex toHalf, pl_tex sweepIn, pl_tex previous, pl_tex global, pl_tex out,
		pl_tex opticalFlowVectors, pl_tex cameraKeep, bool cameraStatic)
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
		// The engine's trust weights ride in the seed's w (OF_WEIGHT): kept with the winner
		// when it is the block's own engine vector.
		const char* weightDefine = refineCarriesWeight ? "#define COST_W 1\n" : "";
		// Plan 19 phase 2 (camera guidance): only with a guidance frame; otherwise the dispatch below as before.
		if (cameraKeep || cameraStatic)
		{
			pl_shader_desc d[9];
			int numD = 8;
			for (int i = 0; i < 8; ++i) d[i] = descriptors[i];
			if (cameraKeep)
				d[numD++] = Sampled("camera_keep", cameraKeep, false);
			static ShaderHeaderCache guidedRefineHeaders;
			const uint32_t key = (serialSearchForTest ? 1u : 0u) | (refineCarriesWeight ? 2u : 0u) |
				(cameraKeep ? 4u : 0u) | (cameraStatic ? 8u : 0u);
			const char* h = guidedRefineHeaders.Get(key, [&]() {
				const std::string defines = std::string(weightDefine) + (cameraKeep ? "#define CAMERA_KEEP 1\n" : "") +
					(cameraStatic ? "#define CAMERA_STATIC 1\n" : "");
				if (serialSearchForTest)
					return defines + SUBPEL_HEADER + GATE_GLSL + MEDIAN_GLSL;
				return defines + SUBPEL_HEADER + GATE_GLSL + MEDIAN_GLSL + PERBLOCK_HEADER + REFINE_PAIR_HEADER;
			});
			if (serialSearchForTest)
				return Dispatch("VideoProcessor de-judder: second sweep (11a E1, serial reference)",
					h, REFINE_BODY_SERIAL, d, numD, variables, 6, blocksX, blocksY, 8);
			return DispatchPerBlock("VideoProcessor de-judder: second sweep (11a E1)", h, REFINE_BODY,
				d, numD, variables, 6, (blocksX + 1) / 2, blocksY, 16, 2);
		}
		if (serialSearchForTest)
		{
			static const std::string serialPlain = std::string(SUBPEL_HEADER) + GATE_GLSL + MEDIAN_GLSL;
			static const std::string serialWeighted = std::string("#define COST_W 1\n") + serialPlain;
			return Dispatch("VideoProcessor de-judder: second sweep (11a E1, serial reference)",
				refineCarriesWeight ? serialWeighted.c_str() : serialPlain.c_str(),
				REFINE_BODY_SERIAL, descriptors, 8, variables, 6, blocksX, blocksY, 8);
		}
		static const std::string parallel = std::string(SUBPEL_HEADER) + GATE_GLSL + MEDIAN_GLSL + PERBLOCK_HEADER + REFINE_PAIR_HEADER;
		static const std::string parallelWeighted = std::string("#define COST_W 1\n") + parallel;
		return DispatchPerBlock("VideoProcessor de-judder: second sweep (11a E1)",
			refineCarriesWeight ? parallelWeighted.c_str() : parallel.c_str(), REFINE_BODY,
			descriptors, 8, variables, 6, (blocksX + 1) / 2, blocksY, 16, 2);
	}

	// Plan 19 phase 2 (global_model/CameraGuidanceGpu.h): the guidance textures, uploaded once per pair.
	bool MotionWarp::CameraGuidanceUpload(const CameraModel::GuidanceFrame& g)
	{
		pl_fmt rgba = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
		pl_fmt r = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
		pl_fmt keepFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
		if (!rgba || !r || !keepFmt)
		{
			error = "camera guidance: no 32-bit float texture format";
			return false;
		}
		pl_tex_params gp{};
		gp.w = g.gridW;
		gp.h = g.gridH;
		gp.format = rgba;
		gp.sampleable = true;
		gp.host_writable = true;
		pl_tex_params cp = gp;
		cp.format = r;
		pl_tex_params kp{};
		kp.w = blocksX;
		kp.h = blocksY;
		kp.format = keepFmt;
		kp.sampleable = true;
		kp.storable = true;
		kp.host_readable = settings.hostReadable && (keepFmt->caps & PL_FMT_CAP_HOST_READABLE);
		pl_tex_params np = kp;
		np.w = np.h = 1;
		np.host_readable = keepFmt->caps & PL_FMT_CAP_HOST_READABLE;
		if (!pl_tex_recreate(gpu, &cgGuide, &gp) || !pl_tex_recreate(gpu, &cgCode, &cp) ||
			!pl_tex_recreate(gpu, &cgKeep[0], &kp) || !pl_tex_recreate(gpu, &cgKeep[1], &kp) ||
			!pl_tex_recreate(gpu, &cgCounts, &np))
		{
			error = "camera guidance: cannot create the textures";
			return false;
		}
		pl_tex_transfer_params up{};
		up.tex = cgGuide;
		up.ptr = const_cast<float*>(g.guide.data());
		pl_tex_transfer_params uc{};
		uc.tex = cgCode;
		uc.ptr = const_cast<float*>(g.code.data());
		if (!pl_tex_upload(gpu, &up) || !pl_tex_upload(gpu, &uc))
		{
			error = "camera guidance: upload failed";
			return false;
		}
		return true;
	}

	// One direction of the guided pass into cgKeep[dir]: narrow (before the search) or the E1 skip check
	// (before the second sweep, sweepIn = its first sweep). Backward: the forward seeds negated (the
	// verification decides; a wrong seed escalates).
	bool MotionWarp::CameraGuidedPass(const CameraModel::GuidanceFrame& g, int dir, bool e1, pl_tex fromHalf, pl_tex toHalf, pl_tex sweepIn)
	{
		const float scale = 0.5f * cgInputScale;   // matcher (half size) px per original source px
		const float ratio = static_cast<float>(8.0 / (static_cast<double>(scale) * g.blockSize));
		const float sign = dir == 0 ? 1.0f : -1.0f;
		const int radius = std::max(1, static_cast<int>(std::ceil(g.narrowRadiusPx * scale)));
		const float tol = settings.matchTolerance;
		const float agree = static_cast<float>(g.e1AgreePx * scale);
		const pl_shader_var vars[6] = {
			{ pl_var_float("cg_scale"), &scale, true }, { pl_var_float("cg_ratio"), &ratio, true },
			{ pl_var_float("cg_sign"), &sign, true }, { pl_var_int("cg_radius"), &radius, false },
			{ pl_var_float("cg_tol"), &tol, true }, { pl_var_float("cg_agree"), &agree, true } };
		pl_shader_desc d[6];
		int numD = 0;
		d[numD++] = Sampled("from_half", fromHalf, false);
		d[numD++] = Sampled("to_half", toHalf, true);
		d[numD++] = Sampled("camera_guide", cgGuide, false);
		d[numD++] = Sampled("camera_code", cgCode, false);
		d[numD++] = Storage("dst", cgKeep[dir]);
		if (e1)
			d[numD++] = Sampled("sweep", sweepIn, false);
		static ShaderHeaderCache guidedPassHeaders;
		const uint32_t key = (e1 ? 1u : 0u) | (g.fadeNormalised ? 2u : 0u) | (e1 && refineCarriesWeight ? 4u : 0u);
		const char* header = guidedPassHeaders.Get(key, [&]() {
			return std::string(e1 ? "#define CG_E1 1\n" : "") + (g.fadeNormalised ? "#define CG_FADE 1\n" : "") +
				(e1 && refineCarriesWeight ? "#define COST_W 1\n" : "") + CameraModel::GuidedPassHeaderGlsl();
		});
		return Dispatch(e1 ? "VideoProcessor de-judder: camera guided E1 check (plan 19)" : "VideoProcessor de-judder: camera guided narrow search (plan 19)",
			header, CameraModel::GuidedPassBodyGlsl(), d, numD, vars, 6, blocksX, blocksY, 8);
	}

	// The guided passes' accepted blocks, counted on the GPU into cgCounts (1x1): r/g = narrow accepted
	// forward/backward, b/a = E1 skipped forward/backward (-1 = that pass did not run). One work group of
	// 256 threads, fixed strided partial sums then a fixed-order reduction (deterministic). Since the
	// narrow results are consumed by the search before the E1 pass overwrites cgKeep, this runs after
	// each kind: narrow counts go to r/g, E1 counts to b/a.
	bool MotionWarp::CameraGuidanceCount(bool narrow, bool e1)
	{
		static const char* const body = R"GLSL(
int li = int(gl_LocalInvocationIndex);
ivec2 n = textureSize(keep_f, 0);
int total = n.x * n.y;
float cf = 0.0, cb = 0.0;
for (int i = li; i < total; i += 256) {
    ivec2 p = ivec2(i % n.x, i / n.x);
    cf += texelFetch(keep_f, p, 0).z >= 0.0 ? 1.0 : 0.0;
    cb += texelFetch(keep_b, p, 0).z >= 0.0 ? 1.0 : 0.0;
}
cg_sf[li] = cf;
cg_sb[li] = cb;
memoryBarrierShared();
barrier();
if (li == 0) {
    float tf = 0.0, tb = 0.0;
    for (int k = 0; k < 256; k++) { tf += cg_sf[k]; tb += cg_sb[k]; }
    vec4 prev = imageLoad(dst, ivec2(0));
    imageStore(dst, ivec2(0), cg_mode == 0 ? vec4(tf, tb, -1.0, -1.0) : cg_mode == 1 ? vec4(prev.xy, tf, tb) : vec4(-1.0, -1.0, tf, tb));
}
)GLSL";
		// mode 0: the narrow pass; 1: the E1 pass after a narrow pass this pair; 2: the E1 pass alone.
		const int mode = !e1 ? 0 : narrow ? 1 : 2;
		const pl_shader_var var[1] = { { pl_var_int("cg_mode"), &mode, false } };
		pl_shader_desc out{};
		out.desc.name = "dst";
		out.desc.type = PL_DESC_STORAGE_IMG;
		out.desc.access = PL_DESC_ACCESS_READWRITE;
		out.binding.object = cgCounts;
		const pl_shader_desc d[3] = { Sampled("keep_f", cgKeep[0], false), Sampled("keep_b", cgKeep[1], false), out };
		return DispatchPerBlock("VideoProcessor de-judder: camera guidance counts (plan 19)",
			"shared float cg_sf[256];\nshared float cg_sb[256];\n", body, d, 3, var, 1, 1, 1, 16, 16);
	}

	// SK-MV gap seed validation (SAD vs the zero vector): over a sampled grid of interior 8x8 blocks of the half-size pair, the summed absolute
	// difference at the zero vector, at the forward seed (A to B) and at the backward seed (B to A), read back as (zero, forward, backward).
	// The seed holds only when GapSeed::Beats says both directions explain the pair far better than zero does. A wrong seed (the motion
	// changed during the hiccup) otherwise replaces the measured global and leaves blocks without the true start: dropped, the unseeded
	// global search runs instead. One work group, fixed-order reduction (deterministic).
	bool MotionWarp::GapSeedHolds()
	{
		static const char* const header = R"GLSL(
shared float gs_sz[256];
shared float gs_sf[256];
shared float gs_sb[256];
float gs_a(ivec2 p) { return texelFetch(gs_ta, clamp(p, ivec2(0), textureSize(gs_ta, 0) - ivec2(1)), 0).r; }
float gs_t(ivec2 p) { return texelFetch(gs_tb, clamp(p, ivec2(0), textureSize(gs_tb, 0) - ivec2(1)), 0).r; }
)GLSL";
		static const char* const body = R"GLSL(
int li = int(gl_LocalInvocationIndex);
ivec2 nb = textureSize(gs_ta, 0) / 8;
int gx = min(32, nb.x - 4);
int gy = min(18, nb.y - 4);
ivec2 vf = ivec2(round(gs_seed.xy * 4.0));
ivec2 vb = ivec2(round(gs_seed.zw * 4.0));
float acc_z = 0.0, acc_f = 0.0, acc_b = 0.0;
if (gx > 0 && gy > 0) {
    int sx = max(1, (nb.x - 4) / gx);
    int sy = max(1, (nb.y - 4) / gy);
    for (int i = li; i < gx * gy; i += 256) {
        ivec2 o = (ivec2(2) + ivec2(i % gx, i / gx) * ivec2(sx, sy)) * 8;
        // Only blocks whose seeded windows lie inside the picture (a clamped read is a false mismatch for the seed).
        ivec2 hs = textureSize(gs_ta, 0);
        ivec2 lo = min(o + vf, o + vb);
        ivec2 hi = max(o + vf, o + vb) + ivec2(7);
        if (lo.x < 0 || lo.y < 0 || hi.x >= hs.x || hi.y >= hs.y)
            continue;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                ivec2 p = o + ivec2(x, y);
                float va = gs_a(p);
                float vt = gs_t(p);
                acc_z += abs(va - vt);
                acc_f += abs(va - gs_t(p + vf));
                acc_b += abs(vt - gs_a(p + vb));
            }
    }
}
gs_sz[li] = acc_z;
gs_sf[li] = acc_f;
gs_sb[li] = acc_b;
memoryBarrierShared();
barrier();
if (li == 0) {
    float tz = 0.0, tf = 0.0, tb_ = 0.0;
    for (int k = 0; k < 256; k++) { tz += gs_sz[k]; tf += gs_sf[k]; tb_ += gs_sb[k]; }
    imageStore(dst, ivec2(0), vec4(tz, tf, tb_, 1.0));
}
)GLSL";
		if (!halfA || !halfB)
			return false;
		if (!seedCheck)
		{
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			if (!fmt)
				return false;
			pl_tex_params params{};
			params.w = params.h = 1;
			params.format = fmt;
			params.sampleable = true;
			params.storable = true;
			params.host_readable = true;
			if (!pl_tex_recreate(gpu, &seedCheck, &params))
				return false;
		}
		const float seedVar[4] = { searchSeed[0][0], searchSeed[0][1], searchSeed[1][0], searchSeed[1][1] };
		const pl_shader_var vars[1] = { { pl_var_vec4("gs_seed"), seedVar, true } };
		const pl_shader_desc d[3] = { Sampled("gs_ta", halfA, false), Sampled("gs_tb", halfB, false), Storage("dst", seedCheck) };
		if (!DispatchPerBlock("VideoProcessor de-judder: gap seed check (SAD vs zero)", header, body, d, 3, vars, 1, 1, 1, 16, 16))
		{
			error.clear();
			return false;
		}
		float sums[4] = {};
		pl_tex_transfer_params download{};
		download.tex = seedCheck;
		download.ptr = sums;
		if (!pl_tex_download(gpu, &download))
			return false;
		return VpMotion::GapSeed::Beats(sums[0], sums[1], sums[2]);
	}

	// A cut / flash pair (camera guidance skipMatcher): both searches untrusted (a match error far above any
	// tolerance), D1 and M2's candidate not written (w = 0).
	bool MotionWarp::CameraSkipSearches()
	{
		static const char* const fill = "ivec2 b = ivec2(gl_GlobalInvocationID.xy);\n"
			"if (b.x < imageSize(dst).x && b.y < imageSize(dst).y) imageStore(dst, b, fillv);\n";
		const float untrusted[4] = { 0.0f, 0.0f, 1.0e4f, 1.0f };
		const union pl_clear_color clearColor = { { 0.0f, 0.0f, 1.0e4f, 1.0f } };
		for (pl_tex t : { forward, backward })
		{
			if (t->params.blit_dst)
			{
				pl_tex_clear_ex(gpu, t, clearColor);
				continue;
			}
			const pl_shader_desc out[1] = { Storage("dst", t) };
			const pl_shader_var var[1] = { { pl_var_vec4("fillv"), untrusted, true } };
			if (!Dispatch("VideoProcessor de-judder: searches skipped (camera model: cut / flash)", nullptr, fill, out, 1, var, 1,
					blocksX, blocksY, 8))
				return false;
		}
		const auto dispatchFn = [this](const char* description, const char* header, const char* body,
			const pl_shader_desc* descriptors, int numDescriptors,
			const pl_shader_var* variables, int numVariables, int w, int h, int group) -> bool
		{
			return Dispatch(description, header, body, descriptors, numDescriptors, variables, numVariables, w, h, group);
		};
		VpMotion::PairGlobal clearedRecord;
		if (!VpMotion::ClearPairGlobalGpu(globalVector, globalSecond, dispatchFn, clearedRecord))
		{
			lastPairGlobalFallback = FallbackCatalog::PairGlobalUnavailable(true, error);
			return false;
		}
		lastPairGlobalFallback = FallbackCatalog::PairGlobalUnavailable(false);
		return true;
	}

	namespace
	{
		// SK-MV pass-through: snapshot lease generations are unique across matcher instances (a recreated matcher can never match a
		// generation a source captured from its predecessor). Render thread; atomic for safety only.
		uint64_t NextSnapshotGeneration()
		{
			static std::atomic<uint64_t> counter{ 0 };
			return ++counter;
		}
	}

	bool MotionWarp::SnapshotForWarp()
	{
		snapGeneration = 0;   // the copies are being replaced: any lease on the previous snapshot is lost from here
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
		// SK-MV pass-through: the copies are complete. A new lease generation; a bound source captures the shared view of the snapshot.
		snapGeneration = NextSnapshotGeneration();
		if (fieldSource)
		{
			IFieldSource::Set copies;
			copies.field = snapField;
			copies.frameTrust = snapTrust;
			copies.subField = snapSubValid ? snapSub : nullptr;
			fieldSource->CaptureSnapshot(copies, snapGeneration);
		}
		return true;
	}

	namespace
	{
		// M5: the pixel check of each trusted block (see Settings).
		// Cooperative 10x10 shared tile of src_field (1,600 B < 16 KB, single uniform barrier)
		// replaces 257 src_field fetches per trusted block with 100 fetches per 8x8 work group.
		const char* const VERIFY_HEADER = "shared vec4 vf_tile[10][10];\n";

		const char* const VERIFY_BODY = R"GLSL(
ivec2 wg0 = ivec2(gl_WorkGroupID.xy) * 8 - ivec2(1);
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
int li = int(gl_LocalInvocationIndex);
for (int i = li; i < 100; i += 64) {
    int tx = i % 10;
    int ty = i / 10;
    vf_tile[ty][tx] = texelFetch(src_field, clamp(wg0 + ivec2(tx, ty), ivec2(0), blocks - ivec2(1)), 0);
}
memoryBarrierShared();
barrier();
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = vf_tile[loc.y + 1][loc.x + 1];
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
                        ivec2 ti = g0 + ivec2(xx, yy) - wg0;
                        vec4 n = vf_tile[ti.y][ti.x];
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
		// fair match. Rolling 2-row cache fetches each from_half texel once (81 vs 192
		// fetches) with exact edge clamping and exact (y, x) float summation order.
		const char* const VECTOR_OK_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
ivec2 blocks = imageSize(dst);
if (b.x < blocks.x && b.y < blocks.y) {
    vec4 f = texelFetch(field, b, 0);
    float ok = f.z > 0.5 ? 1.0 : 0.0;
    if (ok < 0.5 && f.w <= own_error) {
        ivec2 hs = textureSize(from_half, 0);
        ivec2 lim = hs - ivec2(2);
        ivec2 o = clamp(b * 8, ivec2(0), lim);
        int py_cur = o.y;
        float cur[9];
        float nxt[9];
        for (int k = 0; k < 9; k++) {
            int px = min(o.x + k, hs.x - 1);
            cur[k] = texelFetch(from_half, ivec2(px, py_cur), 0).r;
            nxt[k] = texelFetch(from_half, ivec2(px, min(py_cur + 1, hs.y - 1)), 0).r;
        }
        float t = 0.0;
        for (int y = 0; y < 8; y++) {
            int py = clamp(b.y * 8 + y, 0, lim.y);
            if (py != py_cur) {
                for (int k = 0; k < 9; k++) {
                    cur[k] = nxt[k];
                    nxt[k] = texelFetch(from_half, ivec2(min(o.x + k, hs.x - 1), min(py + 1, hs.y - 1)), 0).r;
                }
                py_cur = py;
            }
            for (int x = 0; x < 8; x++) {
                int ix = clamp(b.x * 8 + x, 0, lim.x) - o.x;
                float c0 = cur[ix];
                t += abs(cur[ix + 1] - c0) + abs(nxt[ix] - c0);
            }
        }
        ok = t / 128.0 >= own_texture ? 1.0 : 0.0;
    }
    imageStore(dst, b, vec4(ok, 0.0, 0.0, 1.0));
}
)GLSL";

		// M6: the weight of each half-resolution pixel of A.
		// Cooperative 18x18 from_half tile (1,296 B) + 6x6 field tile (576 B) [+ 6x6 vector_ok
		// tile (144 B)] = 2,016 B max (< 16 KB), single uniform barrier before bounds check.
		const char* const PIXEL_MASK_HEADER = R"GLSL(
shared float pm_from[18][18];
shared vec4 pm_field[6][6];
#ifdef PIXEL_OWN_VECTORS
shared float pm_vok[6][6];
#endif
vec2 pm_field_vector_all(vec2 lp, ivec2 f0)
{
    vec2 g = lp / 16.0 - vec2(0.5);
    ivec2 g0 = ivec2(floor(g));
    vec2 t = g - vec2(g0);
    vec2 v = vec2(0.0);
    float ws = 0.0;
    for (int y = 0; y <= 1; y++)
        for (int x = 0; x <= 1; x++) {
            ivec2 ti = g0 + ivec2(x, y) - f0;
            vec4 f = pm_field[ti.y][ti.x];
            float w = (x == 1 ? t.x : 1.0 - t.x) * (y == 1 ? t.y : 1.0 - t.y);
#ifdef PIXEL_TRUSTED_VECTORS
#ifdef PIXEL_OWN_VECTORS
            if (pm_vok[ti.y][ti.x] < 0.5) w = 0.0;
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
                ivec2 ti = g0 + ivec2(x, y) - f0;
                vec4 f = pm_field[ti.y][ti.x];
#ifdef PIXEL_OWN_VECTORS
                f.z = pm_vok[ti.y][ti.x];
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

		const char* const PIXEL_MASK_BODY = R"GLSL(
ivec2 wg0 = ivec2(gl_WorkGroupID.xy) * 16;
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 hs = imageSize(dst);
ivec2 fs = textureSize(field, 0);
ivec2 p0 = wg0 - ivec2(1);
ivec2 f0 = ivec2(gl_WorkGroupID.xy) * 2 - ivec2(2);
int li = int(gl_LocalInvocationIndex);
for (int i = li; i < 324; i += 256) {
    int tx = i % 18;
    int ty = i / 18;
    pm_from[ty][tx] = texelFetch(from_half, clamp(p0 + ivec2(tx, ty), ivec2(0), hs - ivec2(1)), 0).r;
}
if (li < 36) {
    int fx = li % 6;
    int fy = li / 6;
    ivec2 fc = clamp(f0 + ivec2(fx, fy), ivec2(0), fs - ivec2(1));
    pm_field[fy][fx] = texelFetch(field, fc, 0);
#ifdef PIXEL_OWN_VECTORS
    pm_vok[fy][fx] = texelFetch(vector_ok, fc, 0).r;
#endif
}
memoryBarrierShared();
barrier();
if (p.x < hs.x && p.y < hs.y) {
    vec2 v = pm_field_vector_all((vec2(p) + vec2(0.5)) * 2.0, f0) * 0.5;
    float e = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), hs - ivec2(1));
            e += abs(pm_from[loc.y + y + 1][loc.x + x + 1] - texture(to_half, (vec2(q) + v + vec2(0.5)) / vec2(hs)).r);
        }
    e /= 9.0;
    // Two tiers: the pixel's own block passed the block test or not.
    ivec2 bi = p / 8 - f0;
    bool trusted = pm_field[bi.y][bi.x].z > 0.5;
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
		const pl_tex snap = SnapFrameTrust();   // SK-MV pass-through: the snapshot trust through the same lease as the renderer's own read
		if (!settings.motionRolloff || !snap || !frameTrust || !estimated)
			return snap;
		pl_tex_params params = snap->params;
		params.sampleable = true;
		params.storable = true;
		params.host_readable = settings.hostReadable && (params.format->caps & PL_FMT_CAP_HOST_READABLE);
		if (!pl_tex_recreate(gpu, &aheadTrust, &params))
			return snap;
		const float minShare = settings.minTrustedShare;
		const pl_shader_var v[1] = { { pl_var_float("min_share"), &minShare, true } };
		const pl_shader_desc d[3] = { Sampled("snap", snap, false), Sampled("next_trust", frameTrust, false), Storage("dst", aheadTrust) };
		return Dispatch("VideoProcessor de-judder: the roll-off ahead (Q7)", nullptr, LOOKAHEAD_TRUST_BODY, d, 3, v, 1, 1, 1, 1) ?
			aheadTrust : snap;
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
		int innerPicture[4];
		VpUnit::PictureRect::Halved(pictureGiven, innerPicture);
		inner->SetPictureArea(innerPicture[0], innerPicture[1], innerPicture[2], innerPicture[3]);
		inner->innerOfLowRes = true;
		inner->lumaInCodes = true;
		// B3: the inner reads levels 2 / 4 / 6 of the same frames, only when this instance read the product (one level of nesting:
		// an inner of the inner builds its own).
		inner->SetPyramidSource(pyramidBound && !innerOfLowRes ? pyramidSource : VpMotion::MatcherPyramid::Source());
		inner->productLuma[0] = a.luma;
		inner->productLuma[1] = b.luma;
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
		// The pair computed ahead belongs to the instance that searches.
		inner->SetExternalFlow(std::move(externalFlow));
		externalFlow.reset();
		// Plan 19 phase 2: the camera guidance belongs to the instance that searches (its pixels are half ours).
		inner->SetCameraGuidance(cameraGuidance);
		inner->cgInputScale = cgInputScale * 0.5f;
		// SK-MV hiccup gap seed: like the pair computed ahead, it belongs to the instance that searches. The shared pair's whole-frame motion
		// was handed to that instance in its own input pixels (the same space as an engine global seed), so it is forwarded as given. The
		// outer's "used" answer is the inner's: the host counts "seeded" from SearchSeedUsed().
		if (searchSeedValid)
		{
			inner->SetSearchSeed(searchSeed);
			searchSeedValid = false;
		}
		inner->SetWarmUp(warmUpEstimate);
		inner->SetSkeletonMemo(skeletonMemo, skeletonSeq);
		inner->SetSceneVerdictReader(sceneVerdictReader);
		if (!inner->Estimate(la, lb, -1, false))
		{
			lastPairGlobalFallback = inner->LastPairGlobalFallback();
			error = "lowRes: " + inner->LastError();
			return false;
		}
		lastPairGlobalFallback = inner->LastPairGlobalFallback();
		searchSeedUsed = inner->SearchSeedUsed();
		searchSeedRejected = inner->SearchSeedRejected();
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

	bool MotionWarp::UploadEngineWeights(const MotionAhead::Flow& flow)
	{
		ofWeighted = false;
		if (!settings.opticalFlowCostTrust && !settings.opticalFlowFbCheck)
			return true;
		// The producer's blocks are this instance's (the same half size, 8-px blocks).
		if (flow.blocksX != blocksX || flow.blocksY != blocksY)
			return true;
		const size_t blocks = static_cast<size_t>(blocksX) * blocksY;
		const bool fb = settings.opticalFlowFbCheck &&
			OpticalFlowTrust::FbWeights(flow.fbError, flow.gridW, flow.gridH, NvOpticalFlow::Engine::Grid,
				MotionAhead::Flow::kBlockPx, blocksX, blocksY, ConsistencyPixels(), ofFbWeights);
		if (!OpticalFlowTrust::Combine(settings.opticalFlowCostTrust ? &flow.costWeight : nullptr, fb ? &ofFbWeights : nullptr,
				blocks, ofWeights))
			return true;   // nothing to apply: exactly as without the extras
		if (!ofWeight || ofWeight->params.w != blocksX || ofWeight->params.h != blocksY)
		{
			pl_fmt r32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_tex_params params{};
			params.w = blocksX;
			params.h = blocksY;
			params.format = r32;
			params.sampleable = true;
			params.host_writable = true;
			if (!r32 || !pl_tex_recreate(gpu, &ofWeight, &params))
				return true;   // no float texture: the weights are skipped (as without them)
		}
		pl_tex_transfer_params upload{};
		upload.tex = ofWeight;
		upload.ptr = ofWeights.data();
		upload.row_pitch = static_cast<size_t>(blocksX) * sizeof(float);
		if (!pl_tex_upload(gpu, &upload))
			return true;
		ofWeighted = true;
		return true;
	}

	bool MotionWarp::OpticalFlowSearch(pl_tex outForward, pl_tex outBackward, VpMotion::P2PairRecord* outPairRecord)
	{
		// SK-MV (doc 83, MV2; owner decision 2026-11-08: ONE motion source). This matcher opens no NVOF engine: the look-ahead
		// producer (MotionAhead::Producer) is the one NvOpticalFlow session per device, and the pair it published (P2, read through the
		// engine's P2View and handed here by SetExternalFlow) is the only optical-flow input. With no pair that fits this estimate
		// (no producer running: motion_ahead_frames off, low latency, a short queue, a miss) NOTHING runs here and the block searches
		// take over (the P2m path); noPairAhead says so and the host shows it (FallbackCatalog::Motion), never silently.
		if (outPairRecord != nullptr)
			*outPairRecord = VpMotion::P2PairRecord();
		const std::shared_ptr<const MotionAhead::Flow> ahead = std::move(externalFlow);
		externalFlow.reset();
		externalFlowUsed = false;
		ofWeighted = false;
		noPairAhead = false;
		pairSizeMismatch = false;
		const bool dimsOk = ahead && ahead->halfW == halfW && ahead->halfH == halfH &&
			ahead->gridW > 0 && ahead->gridH > 0 &&
			ahead->gridW == (halfW + 3) / 4 && ahead->gridH == (halfH + 3) / 4;
		const size_t expectedInts = dimsOk ?
			static_cast<size_t>(ahead->gridW) * static_cast<size_t>(ahead->gridH) * 2u : 0u;
		const bool dataOk = dimsOk && expectedInts > 0 &&
			ahead->forward.size() >= expectedInts && ahead->backward.size() >= expectedInts;
		if (!dataOk)
		{
			noPairAhead = true;
			pairSizeMismatch = static_cast<bool>(ahead);   // a pair was handed over and did not fit: told apart from a missing pair
			return false;
		}
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
		// The flow textures of the half size's grid (the engine's grid: 4 x 4), written from the pair's CPU fields.
		if (w != ofW || h != ofH || !ofFlow[0] || !ofFlow[1] || !ofFlowTex[0] || !ofFlowTex[1])
		{
			auto releaseFlow = [&]() {
				for (int i = 0; i < 2; ++i)
				{
					pl_tex_destroy(gpu, &ofFlowTex[i]);
					ofFlow[i].Release();
				}
				ofW = 0;
				ofH = 0;
			};
			releaseFlow();
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
				if (!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), ofFlow[i]))
				{
					releaseFlow();
					return fail("cannot create its textures");
				}
				pl_d3d11_wrap_params wrapFlow{}; wrapFlow.tex = ofFlow[i];
				ofFlowTex[i] = pl_d3d11_wrap(gpu, &wrapFlow);
				if (!ofFlowTex[i])
				{
					releaseFlow();
					return fail("cannot share its textures with libplacebo");
				}
			}
			ofW = w; ofH = h;
		}
		// The flow fields straight into the flow textures: no input passes, no engine run here.
		context->UpdateSubresource(ofFlow[0], 0, nullptr, ahead->forward.data(), static_cast<UINT>(ahead->gridW * 4), 0);
		context->UpdateSubresource(ofFlow[1], 0, nullptr, ahead->backward.data(), static_cast<UINT>(ahead->gridW * 4), 0);
		externalFlowUsed = true;
		// The engine's extras as trust weights per block (cost; its own consistency).
		UploadEngineWeights(*ahead);   // none applied when it cannot (as without them)
		const std::string header = std::string(RESCORE_HEADER);
		const std::string weightedHeader = std::string("#define OF_WEIGHT 1\n") + RESCORE_HEADER;
		const pl_shader_desc fw[5] = { Sampled("flow", ofFlowTex[0], false), Sampled("from_half", halfA, false),
			Sampled("to_half", halfB, true), Storage("dst", outForward), Sampled("of_weight", ofWeight, false) };
		const pl_shader_desc bw[4] = { Sampled("flow", ofFlowTex[1], false), Sampled("from_half", halfB, false),
			Sampled("to_half", halfA, true), Storage("dst", outBackward) };
		// The searches' reach in half-size pixels: the coarse search at 1/32 of the
		// source (1/16 of the half size), +-coarseRange texels, plus the finer steps.
		// Hunt 2 04 M3: the range the search really uses (scaled to this height, clamped).
		const float reach = DejudderPolicy::OpticalFlowReach(
			DejudderPolicy::ScaledSearchRange(settings.coarseRange, settings.referenceHeight, height));
		const pl_shader_var reachVar[1] = { { pl_var_float("reach"), &reach, true } };
		if (!Dispatch("VideoProcessor de-judder: optical flow to blocks (forward)", ofWeighted ? weightedHeader.c_str() : header.c_str(),
				OF_TO_SEARCH_BODY, fw, ofWeighted ? 5 : 4, reachVar, 1, blocksX, blocksY, 8) ||
			!Dispatch("VideoProcessor de-judder: optical flow to blocks (backward)", header.c_str(), OF_TO_SEARCH_BODY, bw, 4,
				reachVar, 1, blocksX, blocksY, 8))
			return fail("its block pass failed");
		if (outPairRecord != nullptr)
			*outPairRecord = VpMotion::MakeP2PairRecord(0, 0, 0, ahead->global, ahead->second, true, ahead->globalGeneral);
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
		const VpUnit::PictureRect::Params rectParams = VpUnit::PictureRect::ForRange(pictureFullRange);
		const float black = rectParams.blackCode;
		const float barCodes = rectParams.barCodes;
		const float given[4] = { static_cast<float>(pictureGiven[0]), static_cast<float>(pictureGiven[1]),
			static_cast<float>(pictureGiven[2]), static_cast<float>(pictureGiven[3]) };
		const float frameSize[2] = { static_cast<float>(width), static_cast<float>(height) };
		const pl_shader_var v[4] = { { pl_var_vec4("given"), given, true }, { pl_var_vec2("frame_size"), frameSize, true },
			{ pl_var_float("black"), &black, true }, { pl_var_float("bar_codes"), &barCodes, true } };
		const pl_shader_desc d[2] = { Sampled("l1", l1A, false), Storage("dst", picture) };
		VPV_COMPUTE_ONCE("U8", ::ComputeOnce::Variant(width, height, pictureFullRange ? 1 : 0));   // compute-once detector 2: the one PICTURE pass
		return Dispatch("VideoProcessor de-judder: picture area (black bars)", VpUnit::PictureRect::KernelHeader(), VpUnit::PictureRect::KernelBody(), d, 2, v, 4, 16, 16, 16);
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
		static ShaderHeaderCache fadeMatchHeaders;
		const char* header = fadeMatchHeaders.Get(withPicture ? 1u : 0u, [&]() {
			return std::string(withPicture ? "#define PICTURE 1\n" : "") + VpUnit::CutFadeJudge::FadeKernelHeader();
		});
		// The test, then B's pyramid again with the gain applied (unchanged
		// when the test fails).
		// The window per frame: 0.75 to 24 codes per frame at the reference
		// rate, scaled to this source rate (general, owner 2026-10-14).
		const float perFrame = settings.sourceHz > 0.0 && settings.referenceRateHz > 0.0 ?
			static_cast<float>(settings.referenceRateHz / settings.sourceHz) : 1.0f;
		const VpUnit::CutFadeJudge::FadeParams fadeLaw;   // the unit's window (0.75 to 24 codes per reference frame)
		const float fadeLo = fadeLaw.minStepCodes * perFrame, fadeHi = fadeLaw.maxStepCodes * perFrame;
		const pl_shader_var fadeVars[2] = { { pl_var_float("fade_lo"), &fadeLo, true }, { pl_var_float("fade_hi"), &fadeHi, true } };
		if (!Dispatch("VideoProcessor de-judder: fade test (M4)", header, VpUnit::CutFadeJudge::FadeKernelBody(), d, withPicture ? 4 : 3, fadeVars, 2, 16, 16, 16))
			return false;
		// The uncorrected B stays in the K set for the next pair's A.
		const int levels = keptLevels;
		return RebuildBWithFade(b, fadeState, levels);   // KeepPlainBInK() without the product (B3)
	}

	bool MotionWarp::Estimate(const Planes& a, const Planes& b, int occlusionSlot, bool wantGrainError)
	{
		// Plan 19 phase 2: the camera guidance is for this Estimate only (cleared on every return).
		struct GuidanceClear
		{
			std::shared_ptr<const CameraModel::GuidanceFrame>& g;
			~GuidanceClear() { g.reset(); }
		} guidanceClear{ cameraGuidance };
		VpSkeleton::AcquireResult pairGlobalPin;   // U10: keeps any acquired kP7CameraFit memo GPU pin alive across Search/Refine/FrameTrust
		ReleaseBorrowed();   // B3: last pair's product levels go back; this pair's are bound after EnsureTextures
		estimated = false;
		// F2 + U10: cleared on EVERY Estimate, for this instance and all lowRes inners (4K & 8K) so an Estimate that fails
		// before an inner runs must not leave stale seeded / guidance / pair-global fallback state standing.
		for (MotionWarp* w = this; w; w = w->inner.get())
		{
			w->cameraGuidanceUse = CameraGuidanceUse();
			w->lastPairGlobalFallback = FallbackCatalog::PairGlobalUnavailable(false);
			w->seedFed = false;
			w->seedSkipped = false;
			w->fadeMatchApplied = false;
		}
		grainErrorValid = false;
		fillReady = false;
		activeFillOcclusion = nullptr;
		searchSeedUsed = false;   // SK-MV gap seed: set again below only by the instance that searched with it (an early return reads false)
		searchSeedRejected = false;
		// Hunt 2 04 M1 (motion_history_fixes): the temporal state is the last estimated
		// pair's; a pair whose A is not that pair's B (a pair in between was never
		// estimated) starts it over - as ForgetHistory, the fade state kept. Before
		// EnsureTextures, which consumes holdResetPending. The lowRes inner gets the same
		// sequences and decides for itself.
		if (base.historyFixes && !DejudderPolicy::MatcherHistoryChained(a.sequence, keptSequence))
		{
			if (previousValid || keptSequence != 0)
				++historyRestarts;
			previousValid = false;
			holdResetPending = true;
		}
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
		// L-B4 (BBP-B B3): the levels come from the shared product when it is served (A = the last pair's B is then Acquire(a.seq),
		// the same bytes); otherwise the matcher builds its own through the owner's law, with today's reuse of the last pair's B.
		const bool productBound = BindProductPyramid(a, b, levelsA, levelsB);
		const bool reuseA = !productBound && keptOwned && a.sequence != 0 && a.sequence == keptSequence && a.fullRange == keptFullRange;
		keptSequence = 0;
		if (!productBound)
		{
			if (!EnsureOwnPyramidScratch())   // LOW-3: A's own levels and the K set, freed while the product was bound
				return false;
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
		}
		reusedPlanesForTest += reuseA ? 1 : 0;
		keptSequence = b.sequence;
		keptOwned = !productBound;
		keptInK = false;
		keptLevels = levelsB;
		keptFullRange = b.fullRange;
		pictureFullRange = a.fullRange;
		if (settings.pictureArea && !PictureArea())
			return false;
		bool u7Known = false;
		bool u7Cut = false;
		bool u7Fade = false;
		if (!(outerOfLowRes && settings.lowResAll))
		{
			const char* why = nullptr;
			if (sceneVerdictReader != nullptr && skeletonSeq != 0)
			{
				u7Known = sceneVerdictReader(skeletonSeq, u7Cut, u7Fade, &why);
			}
			else if (skeletonMemo != nullptr && skeletonSeq != 0)
			{
				VpSkeleton::SceneRecordReader::SceneVerdict sv;
				u7Known = VpSkeleton::SceneRecordReader::AcquireScene(skeletonSeq, sv, &why);
				if (u7Known)
				{
					u7Cut = sv.cut;
					u7Fade = sv.fade;
				}
			}
			if (u7Known)
			{
				++u7SceneHits;
				lastU7SceneFallback.clear();
			}
			else
			{
				++u7SceneFallbacks;
				lastU7SceneFallback = (why && why[0]) ? why
					: (skeletonSeq == 0 ? "no skeleton sequence bound"
						: (sceneVerdictReader != nullptr ? "u7 reader unavailable" : "no skeleton memo bound"));
			}
		}
		if (settings.fadeMatch && !outerOfLowRes && (!u7Known || u7Fade))
		{
			if (!FadeMatch(b))
				return false;
			fadeMatchApplied = true;
		}
		if (HalvesForSearch())
		{
			if (!LowResSearch(a, b))
				return false;
			previousValid = false;   // the full-resolution searches' history is stale
			// The inner tested the fade on these planes: B here gets the same
			// gain and offset, so M6's mask and the grain map see it (audit
			// 2026-10-14: at 4K the weights fell towards the hold in a fade).
			bool innerFadeApplied = false;
			for (const MotionWarp* w = inner.get(); w; w = w->HalvesForSearch() ? w->inner.get() : nullptr)
				innerFadeApplied = w->fadeMatchApplied;
			if (settings.fadeMatch && innerFadeApplied && inner && inner->FadeStateTexture())
			{
				if (!RebuildBWithFade(b, inner->FadeStateTexture(), levelsB))   // KeepPlainBInK() without the product (B3)
					return false;
			}
		}
		else
		{
			// M2 (parallel search only; the serial reference has one candidate).
			const bool two = settings.globalTwo && settings.globalPrior && !serialSearchForTest;
			// The optical flow engine (docs/VP-nvof-ahead.md): by default its vectors join
			// the refinement's candidates (needs the two sweeps); otherwise they replace
			// the searches. Without the engine the searches run as before. (Before D1:
			// independent of it, and D1 may come from the engine - planner Q1.)
			const bool candidateMode = settings.opticalFlowCandidate && settings.estCandidates;
			// Plan 19 phase 2: the camera guidance (null = today's matcher exactly).
			const CameraModel::GuidanceFrame* cg = cameraGuidance && cameraGuidance->active ? cameraGuidance.get() : nullptr;
			if (cg)
				cameraGuidanceUse.gpuNotApplied = cg->gpuNotApplied;
			if (cg && cg->skipMatcher)
			{
				// A cut or a flash: no search across it. The searches are written untrusted, so the
				// consistency pass and the frame trust hold the pair as a failed match does; the next
				// pair starts without temporal history.
				if (!CameraSkipSearches())
					return false;
				previousValid = false;
				opticalFlowUsed = false;
				noPairAhead = false;
				pairSizeMismatch = false;
				globalFromEngine = false;
				refineCarriesWeight = false;
				externalFlow.reset();   // a pair computed ahead is for this Estimate only
				externalFlowUsed = false;
				cameraGuidanceUse.matcherSkipped = true;
				seedSkipped = true;   // F2: no seed for this pair (LastSeed says why)
			}
			else
			{
			ofWeighted = false;
			noPairAhead = false;   // set by OpticalFlowSearch when optical flow is wanted and no pair computed ahead fits
			pairSizeMismatch = false;
			VpMotion::P2PairRecord directPairRecord;
			opticalFlowUsed = settings.opticalFlow && !serialSearchForTest &&
				(candidateMode ? OpticalFlowSearch(ofSearch[0], ofSearch[1], &directPairRecord)
				               : OpticalFlowSearch(forward, backward, &directPairRecord));
			const bool replaced = opticalFlowUsed && !candidateMode;
			// Seed: the engine's vectors stand in for the first sweep (no searches).
			const bool seeded = opticalFlowUsed && candidateMode && settings.opticalFlowSeed;
			seedFed = seeded;   // F2: LastSeed (CPU record only)
			globalFromEngine = false;

			// U10 C3: pair-global selection law + GPU fill live in ONE owner (global_model/PairGlobalFill.h).
			VpMotion::PairGlobalSelectionInput pgIn;
			pgIn.cutOrFlash = false;
			pgIn.globalPrior = settings.globalPrior;
			pgIn.useModelGlobal = cg != nullptr && cg->useModelGlobal;
			if (cg != nullptr)
			{
				pgIn.modelFwd[0] = cg->globalFwd[0];
				pgIn.modelFwd[1] = cg->globalFwd[1];
				pgIn.modelBwd[0] = cg->globalBwd[0];
				pgIn.modelBwd[1] = cg->globalBwd[1];
			}
			pgIn.modelInputScale = cgInputScale;
			pgIn.opticalFlowGlobal = settings.opticalFlowGlobal;
			pgIn.opticalFlowGlobalFallback = settings.opticalFlowGlobalFallback;
			pgIn.seeded = seeded;
			const uint64_t pairSeq = skeletonSeq != 0 ? skeletonSeq : b.sequence;
			pgIn.gapSeedCandidate = searchSeedValid;
			searchSeedValid = false;   // SK-MV hiccup gap seed: used (or dropped) by THIS estimate only
			searchSeedUsed = false;
			searchSeedRejected = false;
			if (pgIn.gapSeedCandidate)
			{
				for (int d = 0; d < 2; ++d)
					for (int k = 0; k < 4; ++k)
						pgIn.gapSeedVec[d][k] = searchSeed[d][k];
				pgIn.gapSeedHolds = [this]() { return GapSeedHolds(); };
			}

			VpMotion::PairGlobalGpuContext pgCtx;
			pgCtx.gpu = gpu;
			pgCtx.serialReference = serialSearchForTest;
			pgCtx.twoCandidates = two;
			pgCtx.l1A = l1A;
			pgCtx.l1B = l1B;
			pgCtx.l2A = l2A;
			pgCtx.l2B = l2B;
			pgCtx.globalVector = globalVector;
			pgCtx.globalSecond = globalSecond;
			pgCtx.singleSad2 = &globalSad2;
			pgCtx.singleSad1 = &globalSad1;
			pgCtx.singlePick = &globalPick;
			pgCtx.bothSad2 = &globalBothSad2;
			pgCtx.bothSad1 = &globalBothSad1;
			pgCtx.bothPick = &globalBothPick;
			pgCtx.dispatch = [this](const char* description, const char* header, const char* body,
				const pl_shader_desc* descriptors, int numDescriptors,
				const pl_shader_var* variables, int numVariables, int w, int h, int group) -> bool
			{
				return Dispatch(description, header, body, descriptors, numDescriptors, variables, numVariables, w, h, group);
			};
			pgCtx.dispatchPerBlock = [this](const char* description, const char* header, const char* body,
				const pl_shader_desc* descriptors, int numDescriptors,
				const pl_shader_var* variables, int numVariables, int w, int h, int threadsX, int threadsY) -> bool
			{
				return DispatchPerBlock(description, header, body, descriptors, numDescriptors, variables, numVariables, w, h, threadsX, threadsY);
			};

			VpMotion::PairGlobal pgRecord;
			VpMotion::PairGlobalSelectionOutcome pgOutcome;
			bool pgOk = false;
			{
				const GpuAttribution::SuppressPrivateCopyScope warmUpD1(warmUpEstimate ? "U10" : nullptr);
				pgOk = VpMotion::AcquireOrFillPairGlobalGpu(skeletonMemo, pairSeq,
					opticalFlowUsed ? &directPairRecord : nullptr, pgIn, pgCtx,
					pgRecord, pgOutcome, lastPairGlobalFallback, error, nullptr, &pairGlobalPin);
			}
			if (pgOutcome.engineGlobalFallback)
				++globalFallbacks;
			searchSeedRejected = pgOutcome.searchSeedRejected;
			if (!pgOk)
				return false;   // Ruling 2: abort Estimate on pair-global failure (lastPairGlobalFallback is engaged)
			cameraGuidanceUse.modelGlobal = pgOutcome.modelGlobalUsed;
			globalFromEngine = pgOutcome.globalFromEngine;
			searchSeedUsed = pgOutcome.searchSeedUsed;
			// 11a E1: the first sweep into `sweep`, the second into the searches.
			const bool twoSweeps = settings.estCandidates && !replaced;
			// Plan 19 phase 2: the guided passes (narrow before the search, E1 skip before the second sweep)
			// and the model's static verdict for E4 (only where E4's gate runs).
			const bool cgStatic = cg && cg->staticScene && settings.estGate && settings.globalPrior && previousValid;
			bool cgBlocks = cg && cg->HasBlocks() && (cg->narrow || cg->e1Skip);
			if (cgBlocks)
			{
				// The record's grid must cover this instance's picture (in original source px); otherwise the
				// blocks would map wrongly: the per-block guidance is rejected (visible), today's search runs.
				const double srcW = width / static_cast<double>(cgInputScale), srcH = height / static_cast<double>(cgInputScale);
				if (std::fabs(cg->gridW * cg->blockSize - srcW) >= cg->blockSize || std::fabs(cg->gridH * cg->blockSize - srcH) >= cg->blockSize)
				{
					cgBlocks = false;
					cameraGuidanceUse.rejected = "camera guidance: record grid does not match the picture";
				}
				else if (!CameraGuidanceUpload(*cg))
				{
					cgBlocks = false;
					cameraGuidanceUse.rejected = "camera guidance: texture upload failed";
					error.clear();
				}
			}
			const bool cgNarrow = cgBlocks && cg->narrow && !replaced && !seeded;
			cameraGuidanceUse.narrowDroppedEngine = cgBlocks && cg->narrow && (replaced || seeded);
			if (cgNarrow &&
				(!CameraGuidedPass(*cg, 0, false, halfA, halfB, nullptr) || !CameraGuidedPass(*cg, 1, false, halfB, halfA, nullptr)))
				return false;
			if (cgNarrow && !CameraGuidanceCount(true, false))
				return false;
			cameraGuidanceUse.staticFromModel = cgStatic;
			cameraGuidanceUse.narrowPass = cgNarrow;
			cameraGuidanceUse.fadeNormalised = cgBlocks && cg->fadeNormalised;
			if (!replaced && !seeded &&
				(!Search(halfA, halfB, l1A, l1B, l2A, l2B, globalVector[0], previousSearch[0], twoSweeps ? sweep[0] : forward,
					two ? globalSecond[0] : nullptr, cgNarrow ? cgKeep[0] : nullptr, cgStatic) ||
				 !Search(halfB, halfA, l1B, l1A, l2B, l2A, globalVector[1], previousSearch[1], twoSweeps ? sweep[1] : backward,
					two ? globalSecond[1] : nullptr, cgNarrow ? cgKeep[1] : nullptr, cgStatic)))
				return false;
			const bool ofCandidates = opticalFlowUsed && candidateMode && !seeded;
			// The engine's trust weights stay with the seed's vectors through the refinement.
			refineCarriesWeight = seeded && ofWeighted;
			// Camfix R4 (doc 103 s4): like the narrow pass, E1 skip only on the matcher's own sweep, never on an engine-seeded field.
			const bool cgE1 = cgBlocks && cg->e1Skip && twoSweeps && !seeded;
			cameraGuidanceUse.e1DroppedEngine = cgBlocks && cg->e1Skip && twoSweeps && seeded;   // visible, as narrowDroppedEngine
			if (cgE1 &&
				(!CameraGuidedPass(*cg, 0, true, halfA, halfB, sweep[0]) ||
				 !CameraGuidedPass(*cg, 1, true, halfB, halfA, sweep[1])))
				return false;
			cameraGuidanceUse.e1Pass = cgE1;
			// Visible accept counts (read late by the host; never waited for here).
			if (cgE1 && !CameraGuidanceCount(cgNarrow, true))
				return false;
			if (twoSweeps &&
				(!Refine(halfA, halfB, seeded ? ofSearch[0] : sweep[0], previousSearch[0], globalVector[0], forward, ofCandidates ? ofSearch[0] : nullptr,
					cgE1 ? cgKeep[0] : nullptr, cgStatic) ||
				 !Refine(halfB, halfA, seeded ? ofSearch[1] : sweep[1], previousSearch[1], globalVector[1], backward, ofCandidates ? ofSearch[1] : nullptr,
					cgE1 ? cgKeep[1] : nullptr, cgStatic)))
				return false;
			if (settings.estCandidates || settings.estGate)
			{
				if (!CopyPair(forward, previousSearch[0], backward, previousSearch[1]))
					return false;
				previousValid = true;
			}
			else
				previousValid = false;
			}   // camera guidance: not skipped
		}
		if (searchOnly)
			return true;   // lowRes inner: the outer refines (estimated stays false)
		const float tolerance = settings.matchTolerance;
		const float consistency = ConsistencyPixels();   // fixed-constant batch B4 2026-10-18
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
			pl_shader_desc descriptors[6];
			int numDescriptors = 0;
			descriptors[numDescriptors++] = Sampled("fwd", forward, false);
			descriptors[numDescriptors++] = Sampled("bwd", backward, false);
			descriptors[numDescriptors++] = Storage("dst", refined ? rawField : field);
			// Delta 9 H3: marginal blocks (up to taper_sad) pass the consistency
			// check as trusted; the warp tapers them.
			const float taperSad = settings.taperSad > 0.0f ? settings.taperSad : 2.0f * settings.matchTolerance;
			const bool textureTol = settings.textureTolerance > 0.0f;
			const float textureTolValue = settings.textureTolerance;
			pl_shader_var consistencyVars[5];
			int numConsistencyVars = 0;
			consistencyVars[numConsistencyVars++] = variables[0];
			consistencyVars[numConsistencyVars++] = variables[1];
			if (settings.blockTaper)
				consistencyVars[numConsistencyVars++] = { pl_var_float("taper_sad"), &taperSad, true };
			if (textureTol)
				consistencyVars[numConsistencyVars++] = { pl_var_float("texture_tol"), &textureTolValue, true };
			// The engine's trust weights (in the searches' w; 1 everywhere else): a weight of 0
			// adds twice the pass limit to the block's error.
			const bool ofTrust = settings.opticalFlow && (settings.opticalFlowCostTrust || settings.opticalFlowFbCheck);
			const float ofPenalty = 2.0f * (settings.blockTaper ? taperSad : tolerance);
			if (ofTrust)
				consistencyVars[numConsistencyVars++] = { pl_var_float("of_penalty"), &ofPenalty, true };
			if (textureTol)
				descriptors[numDescriptors++] = Sampled("from_half", halfA, false);
			const bool withPicture = settings.pictureArea && picture;
			if (withPicture)
				descriptors[numDescriptors++] = Sampled("picture", picture, false);
			static ShaderHeaderCache consistencyHeaders;
			const uint32_t cKey = (settings.blockTaper ? 1u : 0u) | (textureTol ? 2u : 0u) |
				(withPicture ? 4u : 0u) | (ofTrust ? 8u : 0u);
			const char* consistencyHeader = consistencyHeaders.Get(cKey, [&]() {
				return std::string(settings.blockTaper ? "#define TAPER 1\n" : "") +
					(textureTol ? "#define TEXTURE_TOL 1\n" : "") + (withPicture ? "#define PICTURE 1\n" : "") +
					(ofTrust ? "#define OF_TRUST 1\n" : "");
			});
			if (!Dispatch("VideoProcessor de-judder: consistency", consistencyHeader[0] ? consistencyHeader : nullptr,
				CONSISTENCY_BODY, descriptors, numDescriptors, consistencyVars,
				numConsistencyVars, blocksX, blocksY, 8))
				return false;
			if (settings.estQpel)
			{
				const pl_shader_desc qpelDesc[4] = {
					Sampled("raw", rawField, false), Sampled("from_half", halfA, false),
					Sampled("to_half", halfB, true), Storage("dst", settings.vectorSmooth ? qpelField : field) };
				static const std::string qpelHeader = std::string(SUBPEL_HEADER) + QPEL_HEADER;
				if (serialSearchForTest ?
					!Dispatch("VideoProcessor de-judder: quarter-pel (11a E3, serial reference)", SUBPEL_HEADER,
						QPEL_BODY_SERIAL, qpelDesc, 4, nullptr, 0, blocksX, blocksY, 8) :
					!DispatchPerBlock("VideoProcessor de-judder: quarter-pel (11a E3)", qpelHeader.c_str(), QPEL_BODY,
						qpelDesc, 4, nullptr, 0, blocksX, blocksY, 8, 8))
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
				if (!Dispatch("VideoProcessor de-judder: pixel verification (M5)", VERIFY_HEADER, VERIFY_BODY, verifyDesc, 4, verifyVars, 2,
						blocksX, blocksY, 8))
					return false;
				std::swap(field, verifyField);
			}
			// Identical blocks: exactly the zero vector (before the split and the frame trust).
			// S3-H5: skip StillZero when camera guidance skipped the matcher (cut / flash), including when a
			// lowRes inner performed the skip, so bit-identical regions are not stamped with trust = 1.0.
			bool matcherSkipped = cameraGuidanceUse.matcherSkipped;
			for (const MotionWarp* w = this; !matcherSkipped && w->HalvesForSearch() && w->inner; w = w->inner.get())
				matcherSkipped = w->inner->cameraGuidanceUse.matcherSkipped;
			if (settings.estStillZero && !matcherSkipped && !StillZero(field, stillField, a, b, blocksX, blocksY, 16, false))
				return false;
			if (settings.estSplit)
			{
				// Both directions split (the sub-search, as a function: four
				// texels per block), then A's sub-blocks are checked against B's.
				const float splitSad = settings.estSplitSad > 0.0f ? settings.estSplitSad : settings.matchTolerance;
				const pl_shader_var splitVars[1] = { { pl_var_float("split_sad"), &splitSad, true } };
				static const std::string header = std::string(SUBPEL_HEADER) + SPLIT_SEARCH_BODY;
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
				if (settings.estStillZero && !matcherSkipped && !StillZero(subField, stillSub, a, b, blocksX * 2, blocksY * 2, 8, true))
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
			pl_shader_desc trustDescs[10];
			int numTrustDescs = 0;
			trustDescs[numTrustDescs++] = Sampled("field", field, false);
			trustDescs[numTrustDescs++] = Storage("dst", frameTrust);
			trustDescs[numTrustDescs++] = stateDesc;
			if (settings.regionalHold)
			{
				trustDescs[numTrustDescs++] = Sampled("hist_a", l2A, false);
				trustDescs[numTrustDescs++] = Sampled("hist_b", l2B, false);
			}
			const pl_tex activeFade = FadeStateTexture();
			const bool fadeOn = !u7Known && settings.fadeMatch && activeFade != nullptr;
			if (fadeOn)
				trustDescs[numTrustDescs++] = Sampled("fade_state", activeFade, false);
			if (rolloffGlobal || cameraOut)
				trustDescs[numTrustDescs++] = Sampled("global_vec", globalVector[0], false);
			if (cameraOut)
				trustDescs[numTrustDescs++] = Storage("camera_out", camera);
			const bool trustPicture = settings.pictureArea && picture;
			if (trustPicture)
				trustDescs[numTrustDescs++] = Sampled("picture", picture, false);
			const float rollLo = settings.rollLo, rollHi = settings.rollHi, rollFloor = std::min(1.0f, std::max(0.0f, settings.rollFloor));
			pl_shader_var trustVarList[15];
			int numTrustVars = 5;
			for (int vi = 0; vi < 5; ++vi) trustVarList[vi] = trustVars[vi];
			const float cutHist = settings.cutHistogram;
			if (settings.regionalHold)
				trustVarList[numTrustVars++] = { pl_var_float("cut_hist"), &cutHist, true };
			const float fadeFloor = std::min(1.0f, std::max(0.0f, settings.fadeFloor));
			if (fadeOn || u7Known)
				trustVarList[numTrustVars++] = { pl_var_float("fade_floor"), &fadeFloor, true };
			const int u7CutVar = u7Cut ? 1 : 0;
			const int u7FadeVar = (settings.fadeMatch && u7Fade) ? 1 : 0;
			if (u7Known)
			{
				trustVarList[numTrustVars++] = { pl_var_int("u7_cut"), &u7CutVar, true };
				trustVarList[numTrustVars++] = { pl_var_int("u7_fade"), &u7FadeVar, true };
			}
			// The refinement's window: the global moves in half-texel steps (4 px at full size,
			// 8 px when the low-resolution matcher searched); wider than half a step, 6 px minimum.
			// Hunt 2 04 L5: one step for every halving (an 8K input's 16 px step got 8 px).
			// Fixed-constant batch B4 (2026-10-18, general_rate_fixes): continuous in the height.
			const float cameraWindow = DejudderPolicy::CameraWindowPixels(SearchHalvings(), height, settings.generalRates);
			if (rolloffGlobal || cameraOut)
				trustVarList[numTrustVars++] = { pl_var_float("camera_window"), &cameraWindow, true };
			if (settings.motionRolloff)
			{
				trustVarList[numTrustVars++] = { pl_var_float("roll_lo"), &rollLo, true };
				trustVarList[numTrustVars++] = { pl_var_float("roll_hi"), &rollHi, true };
				trustVarList[numTrustVars++] = { pl_var_float("roll_floor"), &rollFloor, true };
			}
			static ShaderHeaderCache trustHeaders;
			const uint32_t tKey = (settings.motionRolloff ? 1u : 0u) | (rolloffGlobal ? 2u : 0u) |
				(cameraOut ? 4u : 0u) | (settings.regionalHold ? 8u : 0u) | (fadeOn ? 16u : 0u) |
				(u7Known ? 32u : 0u) | (trustPicture ? 64u : 0u);
			const char* trustHeader = trustHeaders.Get(tKey, [&]() {
				return std::string(settings.motionRolloff ? "#define ROLLOFF 1\n" : "") +
					(rolloffGlobal ? "#define ROLLOFF_GLOBAL 1\n" : "") + (cameraOut ? "#define CAMERA_OUT 1\n" : "") +
					(settings.regionalHold ? "#define REGIONAL 1\n" : "") + (fadeOn ? "#define FADE 1\n" : "") +
					(u7Known ? "#define U7_SCENE 1\n" : "") +
					(trustPicture ? "#define PICTURE 1\n" : "") + FrameTrustHeader();
			});
			if (!Dispatch("VideoProcessor de-judder: frame trust", trustHeader, FrameTrustBody().c_str(),
				trustDescs, numTrustDescs, trustVarList, numTrustVars, 16, 16, 16))
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
		const bool wantDitherOcclusion = (occlusionSlot == 0 || occlusionSlot == 1);
		bool ditherOcclusionDone = false;
		// D5's occlusion (also with the half-size matcher: its searches are at
		// this grid by now).
		if (settings.occlusionFill)
		{
			const bool subOk = settings.estSplit && subField;
			// GEM-121: reuse a single OCCLUSION_BODY dispatch for both D5 fill and
			// dither occlusion when both are requested and use identical shader
			// defines (subOk == settings.estSplit); write directly into
			// occlusion[occlusionSlot] and alias activeFillOcclusion to it (leaving
			// the alternating dither slot and non-dither estimates untouched).
			const bool shareWithDither = wantDitherOcclusion && (subOk == settings.estSplit);
			pl_tex fillTarget = shareWithDither ? occlusion[occlusionSlot] : fillOcclusion;
			const pl_shader_desc fillDesc[4] = {
				Sampled("fwd", forward, false), Sampled("bwd", backward, false), Storage("dst", fillTarget),
				Sampled("sub_field", subField, false) };
			if (!Dispatch(shareWithDither ? "VideoProcessor de-judder: occlusion for the dither"
					: "VideoProcessor de-judder: occlusion for the fill (D5)",
				subOk ? splitDefine : nullptr, OCCLUSION_BODY,
				fillDesc, subOk ? 4 : 3, variables, 2, blocksX, blocksY, 8))
				return false;
			activeFillOcclusion = fillTarget;
			ditherOcclusionDone = shareWithDither;
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
			pl_shader_desc maskDescs[5];
			int numMaskDescs = 4;
			for (int mi = 0; mi < 4; ++mi) maskDescs[mi] = maskDesc[mi];
			if (own)
				maskDescs[numMaskDescs++] = Sampled("vector_ok", vectorOk, false);
			static ShaderHeaderCache pixelMaskHeaders;
			const uint32_t mKey = (settings.pixelTrustedVectors ? 1u : 0u) | (own ? 2u : 0u) | (fast ? 4u : 0u);
			const char* maskHeader = pixelMaskHeaders.Get(mKey, [&]() {
				return std::string(settings.pixelTrustedVectors ? "#define PIXEL_TRUSTED_VECTORS 1\n" : "") +
					(own ? "#define PIXEL_OWN_VECTORS 1\n" : "") + (fast ? "#define PIXEL_FAST 1\n" : "") + PIXEL_MASK_HEADER;
			});
			if (!Dispatch("VideoProcessor de-judder: pixel trust (M6)", maskHeader, PIXEL_MASK_BODY, maskDescs,
					numMaskDescs, maskVars, fast ? 6 : 4, halfW, halfH, 16))
				return false;
			pixelMaskReady = true;
		}
		if (wantDitherOcclusion && !ditherOcclusionDone)
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
			if (!DispatchPerBlock("VideoProcessor de-judder: grain error (adaptive downscale)",
				settings.estSplit ? grainSplitHeader.c_str() : GRAIN_ERROR_HEADER,
				GRAIN_ERROR_BODY, grainDesc, settings.estSplit ? 5 : 4, nullptr, 0, blocksX, blocksY, 8, 8))
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
		return Dispatch("VideoProcessor de-judder: vector smoothing (D3)", SMOOTH_HEADER.c_str(), SMOOTH_BODY,
			smoothDesc, 4, variables, 1, blocksX, blocksY, 8);
	}

	const char* FieldVectorGlsl()
	{
		return WARP_HEADER;
	}

	const char* SweepMedianGlslForTest()
	{
		return MEDIAN_GLSL;
	}

	const char* SmoothHeaderGlslForTest()
	{
		return SMOOTH_HEADER.c_str();
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
		return WarpImpl(a, shift, out, trusted, false);
	}

	bool MotionWarp::WarpProcessed(const Planes& a, float shift, Planes& out, bool trusted)
	{
		return WarpImpl(a, shift, out, trusted, true);
	}

	bool MotionWarp::WarpImpl(const Planes& a, float shift, Planes& out, bool trusted, bool processedTarget)
	{
		out = a;
		if (!trusted || !estimated || shift == 0.0f)
			return true;   // hold: A itself
		pl_tex dstLuma = nullptr, dstChroma = nullptr;
		float lumaScale[2] = { 1.0f, 1.0f };
		if (!processedTarget)
		{
			// The warped planes match the frame Estimate saw: refuse a different
			// size or chroma layout (e.g. 4:2:2 -> 4:2:0) or a luma-only estimate
			// before lazily allocating warpedLuma / warpedChroma.
			if (!a.luma || !a.chroma || innerOfLowRes || !lumaFormat || !chromaFormat ||
				a.luma->params.w != width || a.luma->params.h != height ||
				a.luma->params.format != lumaFormat ||
				a.chroma->params.w != chromaW || a.chroma->params.h != chromaH ||
				a.chroma->params.format != chromaFormat)
			{
				error = "Warp without a matching Estimate";
				return false;
			}
			// GEM-141: lazily allocate warpedLuma / warpedChroma on first actual
			// source-plane Warp so non-source-warp paths never allocate them.
			for (const auto& plane : { std::make_pair(a.luma, &warpedLuma), std::make_pair(a.chroma, &warpedChroma) })
			{
				pl_fmt fmt = plane.first->params.format;
				if (!fmt || !(fmt->caps & PL_FMT_CAP_STORABLE))
				{
					error = std::string("plane format not storable: ") + (fmt && fmt->name ? fmt->name : "unknown");
					return false;
				}
				pl_tex_params params{};
				params.w = plane.first->params.w;
				params.h = plane.first->params.h;
				params.format = fmt;
				params.sampleable = true;
				params.storable = true;
				params.host_readable = settings.hostReadable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
				if (!pl_tex_recreate(gpu, plane.second, &params))
				{
					error = "cannot create the warped planes";
					return false;
				}
			}
			dstLuma = warpedLuma;
			dstChroma = warpedChroma;
		}
		else
		{
			// B3: the processed frame of the source frame Estimate saw, at its own size. The field
			// stays in estimate px; the shaders map positions (luma_scale, chroma_scale).
			if (!a.luma || !a.chroma || width <= 0 || height <= 0)
			{
				error = "WarpProcessed without planes or an estimate";
				return false;
			}
			for (const auto& plane : { std::make_pair(a.luma, &processedLuma), std::make_pair(a.chroma, &processedChroma) })
			{
				const pl_tex in = plane.first;
				if (!(in->params.format->caps & PL_FMT_CAP_STORABLE))
				{
					error = std::string("processed plane format not storable: ") + in->params.format->name;
					return false;
				}
				pl_tex_params params{};
				params.w = in->params.w;
				params.h = in->params.h;
				params.format = in->params.format;
				params.sampleable = true;
				params.storable = true;
				params.renderable = (in->params.format->caps & PL_FMT_CAP_RENDERABLE) != 0;
				if (!pl_tex_recreate(gpu, plane.second, &params))
				{
					error = "cannot create the processed warp planes";
					return false;
				}
			}
			dstLuma = processedLuma;
			dstChroma = processedChroma;
			lumaScale[0] = static_cast<float>(width) / static_cast<float>(a.luma->params.w);
			lumaScale[1] = static_cast<float>(height) / static_cast<float>(a.luma->params.h);
		}
		// `src` is linear for the warp; the hold reads it with texelFetch.
		const float minShare = settings.minTrustedShare;
		const float tolerance = settings.matchTolerance;
		// D5: B, its backward vectors and its occlusion bound only when on.
		const bool fill = settings.occlusionFill && fillReady && activeFillOcclusion && lastB.luma && lastB.chroma && !processedTarget;
		// 11a E2: the sub-field bound (and read) only while the split is on.
		const bool split = settings.estSplit && subField;
		// Delta 14 R-B: the camera from D1 (when it runs).
		const bool withCamera = Camera() != nullptr;
		// M6: the pixel weights (source warp).
		const bool pixel = settings.pixelTrust && pixelMaskReady && pixelMask;
		const bool ownVectors = pixel && PixelOwnVectors();
		const bool landCheck = pixel && settings.landingCheck > 0.0f;
		const float landA = settings.landingCheck;
		static ShaderHeaderCache warpHeaders;
		const uint32_t wKey =
			(fill ? 1u : 0u) |
			(pixel ? 2u : 0u) |
			(pixel && settings.pixelTrustedVectors ? 4u : 0u) |
			(ownVectors ? 8u : 0u) |
			(pixel && settings.pixelTrustMotion ? 16u : 0u) |
			(landCheck ? 32u : 0u) |
			(settings.objectStrength < 1.0f ? 64u : 0u) |
			(withCamera ? 128u : 0u) |
			(split ? 256u : 0u) |
			(settings.holdFeatherPx > 0.0f ? 512u : 0u) |
			(settings.blockTaper ? 1024u : 0u) |
			(settings.sourceWarpCatmull ? 2048u : 0u);
		const char* header = warpHeaders.Get(wKey, [&]() {
			return std::string(fill ? "#define FILL\n" : "") + (pixel ? "#define PIXEL_TRUST\n" : "") +
				(pixel && settings.pixelTrustedVectors ? "#define PIXEL_TRUSTED_VECTORS 1\n" : "") +
				(ownVectors ? "#define PIXEL_OWN_VECTORS 1\n" : "") +
				(pixel && settings.pixelTrustMotion ? "#define PIXEL_MOTION 1\n" : "") +
				(landCheck ? "#define LAND_A 1\n" : "") +
				WarpHeader(settings, split, withCamera) + (fill ? FILL_HEADER : "") + (pixel ? FIELD_ALL_GLSL : "") +
				(settings.sourceWarpCatmull ? std::string(GrainPreserve::CatmullGlsl()) + WARP_SAMPLE_CATMULL : std::string(WARP_SAMPLE_BILINEAR));
		});
		const float objectStrength = settings.objectStrength;
		// Delta 9: H1 in luma (source) pixels; H3 thresholds.
		const float featherPx = settings.holdFeatherPx;
		const float taperTol = settings.matchTolerance;
		const float taperSadW = settings.taperSad > 0.0f ? settings.taperSad : 2.0f * settings.matchTolerance;
		pl_shader_desc lumaDesc[11];
		int numLumaDesc = 0;
		lumaDesc[numLumaDesc++] = Sampled("src", a.luma, true);
		lumaDesc[numLumaDesc++] = Sampled("field", field, false);
		lumaDesc[numLumaDesc++] = Sampled("frame_trust", frameTrust, false);
		lumaDesc[numLumaDesc++] = Storage("dst", dstLuma);
		if (fill)
		{
			lumaDesc[numLumaDesc++] = Sampled("src_b", lastB.luma, true);
			lumaDesc[numLumaDesc++] = Sampled("fill_bwd", backward, false);
			lumaDesc[numLumaDesc++] = Sampled("fill_occ", activeFillOcclusion, false);
		}
		if (split)
			lumaDesc[numLumaDesc++] = Sampled("sub_field", subField, false);
		if (pixel)
			lumaDesc[numLumaDesc++] = Sampled("pix_mask", pixelMask, true);
		if (ownVectors)
			lumaDesc[numLumaDesc++] = Sampled("vector_ok", vectorOk, false);
		if (withCamera)
			lumaDesc[numLumaDesc++] = Sampled("camera", camera, false);
		pl_shader_var lumaVars[10];
		int numLumaVars = 0;
		lumaVars[numLumaVars++] = { pl_var_float("shift"), &shift, true };
		lumaVars[numLumaVars++] = { pl_var_vec2("luma_scale"), lumaScale, true };
		lumaVars[numLumaVars++] = { pl_var_float("min_trusted_share"), &minShare, true };
		if (fill) lumaVars[numLumaVars++] = { pl_var_float("fill_tolerance"), &tolerance, true };
		if (settings.holdFeatherPx > 0.0f) lumaVars[numLumaVars++] = { pl_var_float("feather_px"), &featherPx, true };
		if (settings.blockTaper)
		{
			lumaVars[numLumaVars++] = { pl_var_float("taper_tol"), &taperTol, true };
			lumaVars[numLumaVars++] = { pl_var_float("taper_sad"), &taperSadW, true };
		}
		if (settings.objectStrength < 1.0f) lumaVars[numLumaVars++] = { pl_var_float("object_strength"), &objectStrength, true };
		if (landCheck) lumaVars[numLumaVars++] = { pl_var_float("land_a"), &landA, true };
		if (!Dispatch("VideoProcessor de-judder: warp luma", header, WARP_LUMA_BODY,
			lumaDesc, numLumaDesc, lumaVars, numLumaVars,
			a.luma->params.w, a.luma->params.h, 16))
			return false;
		// Estimate (luma) px per chroma px: the subsampling, or on a processed target the real
		// plane sizes (research/chroma: never assume the (w+1)/2 rounding cancels).
		const float chromaScale[2] = {
			processedTarget ? static_cast<float>(width) / static_cast<float>(a.chroma->params.w) : static_cast<float>(1 << a.chromaShiftX),
			processedTarget ? static_cast<float>(height) / static_cast<float>(a.chroma->params.h) : static_cast<float>(1 << a.chromaShiftY) };
		pl_shader_desc chromaDesc[11];
		int numChromaDesc = 0;
		chromaDesc[numChromaDesc++] = Sampled("src", a.chroma, true);
		chromaDesc[numChromaDesc++] = Sampled("field", field, false);
		chromaDesc[numChromaDesc++] = Sampled("frame_trust", frameTrust, false);
		chromaDesc[numChromaDesc++] = Storage("dst", dstChroma);
		if (fill)
		{
			chromaDesc[numChromaDesc++] = Sampled("src_b", lastB.chroma, true);
			chromaDesc[numChromaDesc++] = Sampled("fill_bwd", backward, false);
			chromaDesc[numChromaDesc++] = Sampled("fill_occ", activeFillOcclusion, false);
		}
		if (split)
			chromaDesc[numChromaDesc++] = Sampled("sub_field", subField, false);
		if (pixel)
			chromaDesc[numChromaDesc++] = Sampled("pix_mask", pixelMask, true);
		if (ownVectors)
			chromaDesc[numChromaDesc++] = Sampled("vector_ok", vectorOk, false);
		if (withCamera)
			chromaDesc[numChromaDesc++] = Sampled("camera", camera, false);
		pl_shader_var chromaVars[10];
		int numChromaVars = 0;
		chromaVars[numChromaVars++] = { pl_var_float("shift"), &shift, true };
		chromaVars[numChromaVars++] = { pl_var_vec2("chroma_scale"), chromaScale, true };
		chromaVars[numChromaVars++] = { pl_var_float("min_trusted_share"), &minShare, true };
		if (fill) chromaVars[numChromaVars++] = { pl_var_float("fill_tolerance"), &tolerance, true };
		if (settings.holdFeatherPx > 0.0f) chromaVars[numChromaVars++] = { pl_var_float("feather_px"), &featherPx, true };
		if (settings.blockTaper)
		{
			chromaVars[numChromaVars++] = { pl_var_float("taper_tol"), &taperTol, true };
			chromaVars[numChromaVars++] = { pl_var_float("taper_sad"), &taperSadW, true };
		}
		if (settings.objectStrength < 1.0f) chromaVars[numChromaVars++] = { pl_var_float("object_strength"), &objectStrength, true };
		if (landCheck) chromaVars[numChromaVars++] = { pl_var_float("land_a"), &landA, true };
		if (!Dispatch("VideoProcessor de-judder: warp chroma", header, WARP_CHROMA_BODY,
			chromaDesc, numChromaDesc, chromaVars, numChromaVars,
			a.chroma->params.w, a.chroma->params.h, 16))
			return false;
		out.luma = dstLuma;
		out.chroma = dstChroma;
		return true;
	}
}
