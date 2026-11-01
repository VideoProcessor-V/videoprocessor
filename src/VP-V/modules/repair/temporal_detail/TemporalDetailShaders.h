// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail GLSL compute bodies (libplacebo pl_custom_shader, the renderer convention of texture_restore). They
// mirror TemporalDetailModel.h step by step; TemporalDetailGpu.cpp composes the headers (geometry and law as
// #defines from TemporalDetail.h by name, per-frame values as dynamic variables) and dispatches them. Nothing is
// read back during a frame: the per-block controller (trackers, refresh, admission/eviction, limiter, guard) runs
// on the GPU too. Reductions use a fixed tree or fixed raster loops, no atomics: deterministic. No barrier sits in
// data-dependent control flow.
//
// Data layout (codes of the signal; B = block edge, bw x bh blocks, K = kSlots past slots):
//   split (grain_split)  w x h  rgba32f  s1, s2, f, base          gblocks bw x bh rgba32f N, e3
//   meas                 bw x bh rgba32f  e1, e2, F, q
//   slots (atlas)        slot k at ((k % cols) w, (k / cols) h): rgba16f s1, s2, c1, c2 (raw and cleaned grain-free bands)
//   sbases (atlas)       same layout: r16 unorm (base x TD_SB_STORE) or r32f
//   (all w x h planes are the active picture: its border is the plane edge; the bars are copied by APPLY)
//   slot_blk             (K bw) x bh rgba32f F, q, N, rho2 of the stored frame (slot-major: x = k bw + bx)
//   mot                  (K bw) x bh rgba32f cx, cy, u (composed displacement current -> slot, uncertainty)
//   tab                  (K + 2) x 1 rgba32ui (exact integers, no 2^24 float limit) per slot (live, id, age frames, cleaned);
//                        [K] = (admitted, cur + 1, next id, refreshes); [K + 1] = (victim + 1, live count, 0, 0).
//                        cur / victim are stored +1 so that 0 means none. Slot ids are uint32 and compared by the signed
//                        difference int(a - b) < 0 (correct across a uint32 wrap while live ids span < 2^31).
//   counters             2 x 1 rgba32ui: (guard active blocks, add-back blocks, guard trips (cumulative, exact), refreshes);
//                        (admitted, live slots, 0, 0)
//   ev_a / ev_b          (K bw) x bh rgba32f (hard, ageGate, strongerGate, e1w) / (hbGate, focusGate, unc base corr, age s)
//   state                (8 bw) x bh rgba32f the model's BlockState (8 texels per block, see kState)
//   work                 bw x bh rgba32f (past slot, refresh, evidence, referee)
//   coef                 (K bw) x bh r32f template coefficients; cw / cf (w, boost, nT, members); ct (tau1, tau2, lam1, lam2)
//   delta                w x h  rg32f    delta, gdel (what the grain guard reads)
//   gsum                 (2 bw) x bh rgba32f the guard's 7 tile sums; kback bw x bh; gflag (guardOn, tripped, addBack, v)
//   prevg                w x h  rg32f    the previous frame's G_in / G_out on the shared map (guard lag-1)
#pragma once

#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>

namespace TemporalDetail
{
	namespace Shaders
	{
		// Smoothstep, Catmull-Rom, slot-major block index; needs only the #defines.
		constexpr const char* kCore = R"GLSL(
// The (uint) slot table as floats, for its small fields (flags, counts, slot indices + 1, ages); ids use the raw uint.
#define td_tabf(t, p) vec4(texelFetch(t, p, 0))
// TemporalDetail::SanitizeBlockGate on the GPU: non-finite -> 1, thresholds to [TD_BGATE_MIN, TD_BGATE_MAX], strength to [0, 1].
vec4 td_bgate(vec4 v)
{
    bvec4 bad = bvec4(isnan(v.x) || isinf(v.x), isnan(v.y) || isinf(v.y), isnan(v.z) || isinf(v.z), isnan(v.w) || isinf(v.w));
    v = mix(v, vec4(1.0), bvec4(bad));
    return vec4(clamp(v.xyz, vec3(TD_BGATE_MIN), vec3(TD_BGATE_MAX)), clamp(v.w, 0.0, 1.0));
}
float td_ss(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
vec4 td_cr(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
ivec2 td_blk(int k, ivec2 b) { return ivec2(k * TD_BW + b.x, b.y); }
)GLSL";

		// GridAt / GridAt2 of the model on the slot motion (needs `mot`).
		constexpr const char* kMot = R"GLSL(
vec3 td_mot_at(int k, int i, int j) { return texelFetch(mot, ivec2(k * TD_BW + clamp(i, 0, TD_BW - 1), clamp(j, 0, TD_BH - 1)), 0).xyz; }
// GridAt: (a (1 - tx) + b tx) (1 - ty) + (c (1 - tx) + d tx) ty, block i's centre at (i + 0.5) B - 0.5.
vec3 td_mot_grid(int k, vec2 pos)
{
    vec2 g = (pos + vec2(0.5)) / float(TD_B) - vec2(0.5);
    vec2 f = floor(g);
    vec2 t = g - f;
    int i0 = int(f.x), j0 = int(f.y);
    return (td_mot_at(k, i0, j0) * (1.0 - t.x) + td_mot_at(k, i0 + 1, j0) * t.x) * (1.0 - t.y) +
           (td_mot_at(k, i0, j0 + 1) * (1.0 - t.x) + td_mot_at(k, i0 + 1, j0 + 1) * t.x) * t.y;
}
// Position: p + GridAt2(cx, cy) at the integer pixel p (the model's four-weight form).
vec2 td_position(int k, int px, int py)
{
    vec2 g = (vec2(float(px), float(py)) + vec2(0.5)) / float(TD_B) - vec2(0.5);
    vec2 f = floor(g);
    vec2 t = g - f;
    int i0 = int(f.x), j0 = int(f.y);
    vec2 a = td_mot_at(k, i0, j0).xy, b = td_mot_at(k, i0 + 1, j0).xy, c = td_mot_at(k, i0, j0 + 1).xy, d = td_mot_at(k, i0 + 1, j0 + 1).xy;
    vec2 v = a * ((1.0 - t.x) * (1.0 - t.y)) + b * (t.x * (1.0 - t.y)) + c * ((1.0 - t.x) * t.y) + d * (t.x * t.y);
    return vec2(float(px), float(py)) + v;
}
)GLSL";

		// Catmull-Rom 4 x 4 of a stored slot at a continuous position (needs td_slot_pix / td_slot_base, generated per K):
		// v = (s1, s2, c1, c2), b = base. Edges clamped, the model's row-then-column order.
		constexpr const char* kSlotSample = R"GLSL(
void td_slot_sample(int k, vec2 pos, out vec4 v, out float b)
{
    vec2 f = floor(pos);
    vec4 wx = td_cr(pos.x - f.x), wy = td_cr(pos.y - f.y);
    ivec2 o = ivec2(f) - ivec2(1);
    v = vec4(0.0);
    b = 0.0;
    for (int j = 0; j < 4; j++) {
        vec4 rv = vec4(0.0);
        float rb = 0.0;
        for (int i = 0; i < 4; i++) {
            ivec2 q = clamp(o + ivec2(i, j), ivec2(0), ivec2(TD_W - 1, TD_H - 1));
            rv += wx[i] * td_slot_pix(k, q);
            rb += wx[i] * td_slot_base(k, q);
        }
        v += wy[j] * rv;
        b += wy[j] * rb;
    }
}
// The template's lighting gain: clamp(base / base_w, lo, hi) where both are positive, else 1.
float td_gain(float bw, float bc) { return (bw > 1e-3 && bc > 1e-3) ? clamp(bc / bw, TD_GAIN_LO, TD_GAIN_HI) : 1.0; }
)GLSL";

		// PairAt of a motion pair texture (gridW x gridH rgba32f: vx, vy, trust; plane px) with its block size.
		constexpr const char* kPairBwd = R"GLSL(
vec3 td_pair_bwd(vec2 pos)
{
    ivec2 gs = textureSize(bwd, 0) - ivec2(1);
    vec2 g = (pos + vec2(float(TD_OX), float(TD_OY)) + vec2(0.5)) / bwd_bpx - vec2(0.5);
    vec2 f = floor(g);
    vec2 t = g - f;
    int i0 = int(f.x), j0 = int(f.y);
    vec3 a = texelFetch(bwd, clamp(ivec2(i0, j0), ivec2(0), gs), 0).xyz;
    vec3 b = texelFetch(bwd, clamp(ivec2(i0 + 1, j0), ivec2(0), gs), 0).xyz;
    vec3 c = texelFetch(bwd, clamp(ivec2(i0, j0 + 1), ivec2(0), gs), 0).xyz;
    vec3 d = texelFetch(bwd, clamp(ivec2(i0 + 1, j0 + 1), ivec2(0), gs), 0).xyz;
    return (a * (1.0 - t.x) + b * t.x) * (1.0 - t.y) + (c * (1.0 - t.x) + d * t.x) * t.y;
}
)GLSL";
		constexpr const char* kPairFwd = R"GLSL(
vec3 td_pair_fwd(vec2 pos)
{
    ivec2 gs = textureSize(fwd, 0) - ivec2(1);
    vec2 g = (pos + vec2(float(TD_OX), float(TD_OY)) + vec2(0.5)) / fwd_bpx - vec2(0.5);
    vec2 f = floor(g);
    vec2 t = g - f;
    int i0 = int(f.x), j0 = int(f.y);
    vec3 a = texelFetch(fwd, clamp(ivec2(i0, j0), ivec2(0), gs), 0).xyz;
    vec3 b = texelFetch(fwd, clamp(ivec2(i0 + 1, j0), ivec2(0), gs), 0).xyz;
    vec3 c = texelFetch(fwd, clamp(ivec2(i0, j0 + 1), ivec2(0), gs), 0).xyz;
    vec3 d = texelFetch(fwd, clamp(ivec2(i0 + 1, j0 + 1), ivec2(0), gs), 0).xyz;
    return (a * (1.0 - t.x) + b * t.x) * (1.0 - t.y) + (c * (1.0 - t.x) + d * t.x) * t.y;
}
)GLSL";

		// The model's BlockState, 8 rgba32f texels per block (needs st_in; td_store needs st_out).
		constexpr const char* kStateLoad = R"GLSL(
struct TdState {
    float slot, prevSlot, xf, qref;
    float wLim, hSm, tHold, bLim;
    float on, bOn, have, retired;
    float qPrev, e2Prev, rstate, rebounds;
    float qHigh, qLow, softRun, vetoRun;
    float qPeak, e2Peak, fPeak, guard;
    float guardOn, ema0, ema1, ema2;
    float ema3, ema4, ema5, ema6;
};
TdState td_default()
{
    TdState s;
    s.slot = -1.0; s.prevSlot = -1.0; s.xf = 1.0; s.qref = 0.0;
    s.wLim = 0.0; s.hSm = 0.0; s.tHold = 0.0; s.bLim = 0.0;
    s.on = 0.0; s.bOn = 0.0; s.have = 0.0; s.retired = 0.0;
    s.qPrev = 0.0; s.e2Prev = 0.0; s.rstate = 0.0; s.rebounds = 0.0;
    s.qHigh = 0.0; s.qLow = 0.0; s.softRun = 0.0; s.vetoRun = 0.0;
    s.qPeak = 0.0; s.e2Peak = 0.0; s.fPeak = 0.0; s.guard = 1.0;
    s.guardOn = 0.0; s.ema0 = 0.0; s.ema1 = 0.0; s.ema2 = 0.0;
    s.ema3 = 0.0; s.ema4 = 0.0; s.ema5 = 0.0; s.ema6 = 0.0;
    return s;
}
vec4 td_st_texel(ivec2 b, int g) { return texelFetch(st_in, ivec2(b.x * 8 + g, b.y), 0); }
TdState td_load(ivec2 b)
{
    TdState s;
    vec4 t = td_st_texel(b, 0); s.slot = t.x; s.prevSlot = t.y; s.xf = t.z; s.qref = t.w;
    t = td_st_texel(b, 1); s.wLim = t.x; s.hSm = t.y; s.tHold = t.z; s.bLim = t.w;
    t = td_st_texel(b, 2); s.on = t.x; s.bOn = t.y; s.have = t.z; s.retired = t.w;
    t = td_st_texel(b, 3); s.qPrev = t.x; s.e2Prev = t.y; s.rstate = t.z; s.rebounds = t.w;
    t = td_st_texel(b, 4); s.qHigh = t.x; s.qLow = t.y; s.softRun = t.z; s.vetoRun = t.w;
    t = td_st_texel(b, 5); s.qPeak = t.x; s.e2Peak = t.y; s.fPeak = t.z; s.guard = t.w;
    t = td_st_texel(b, 6); s.guardOn = t.x; s.ema0 = t.y; s.ema1 = t.z; s.ema2 = t.w;
    t = td_st_texel(b, 7); s.ema3 = t.x; s.ema4 = t.y; s.ema5 = t.z; s.ema6 = t.w;
    return s;
}
)GLSL";
		constexpr const char* kStateStore = R"GLSL(
void td_store(ivec2 b, TdState s)
{
    int x = b.x * 8;
    imageStore(st_out, ivec2(x + 0, b.y), vec4(s.slot, s.prevSlot, s.xf, s.qref));
    imageStore(st_out, ivec2(x + 1, b.y), vec4(s.wLim, s.hSm, s.tHold, s.bLim));
    imageStore(st_out, ivec2(x + 2, b.y), vec4(s.on, s.bOn, s.have, s.retired));
    imageStore(st_out, ivec2(x + 3, b.y), vec4(s.qPrev, s.e2Prev, s.rstate, s.rebounds));
    imageStore(st_out, ivec2(x + 4, b.y), vec4(s.qHigh, s.qLow, s.softRun, s.vetoRun));
    imageStore(st_out, ivec2(x + 5, b.y), vec4(s.qPeak, s.e2Peak, s.fPeak, s.guard));
    imageStore(st_out, ivec2(x + 6, b.y), vec4(s.guardOn, s.ema0, s.ema1, s.ema2));
    imageStore(st_out, ivec2(x + 7, b.y), vec4(s.ema3, s.ema4, s.ema5, s.ema6));
}
)GLSL";

		// The live slots of the post-admission table in id order (the model's m_slots order), -1 padded (needs tab_new).
		constexpr const char* kOrder = R"GLSL(
void td_order(out int ord[TD_K])
{
    uint ids[TD_K];
    for (int k = 0; k < TD_K; k++) {
        uvec4 t = texelFetch(tab_new, ivec2(k, 0), 0);
        ord[k] = t.x > 0u ? k : -1;
        ids[k] = t.y;
    }
    // Live slots first, by id; ids compare by the signed difference (wrap-safe), -1 slots sort last.
    for (int i = 1; i < TD_K; i++) {
        int o = ord[i];
        uint v = ids[i];
        int j = i - 1;
        while (j >= 0 && o >= 0 && (ord[j] < 0 || int(ids[j] - v) > 0)) { ids[j + 1] = ids[j]; ord[j + 1] = ord[j]; j--; }
        ids[j + 1] = v;
        ord[j + 1] = o;
    }
}
bool td_live_new(int k) { return k >= 0 && k < TD_K && td_tabf(tab_new, ivec2(k, 0)).x > 0.5; }
)GLSL";

		// Two-vec4 fixed-tree reduction over a 16 x 16 workgroup (every invocation calls it).
		constexpr const char* kReduce = R"GLSL(
shared vec4 td_r0[256];
shared vec4 td_r1[256];
void td_reduce2(inout vec4 a, inout vec4 b)
{
    uint li = gl_LocalInvocationIndex;
    barrier();
    td_r0[li] = a;
    td_r1[li] = b;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (li < s) { td_r0[li] += td_r0[li + s]; td_r1[li] += td_r1[li + s]; }
        barrier();
    }
    a = td_r0[0];
    b = td_r1[0];
}
)GLSL";

		// --- Step 2: block measures E1, E2, F, q (thread per block, raster sums) ---
		constexpr const char* kMeasBody = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < TD_BW && b.y < TD_BH) {
    float s1 = 0.0, s2 = 0.0;
    int y1 = min(TD_H, (b.y + 1) * TD_B), x1 = min(TD_W, (b.x + 1) * TD_B);
    for (int yy = b.y * TD_B; yy < y1; yy++)
        for (int xx = b.x * TD_B; xx < x1; xx++) {
            vec4 c = texelFetch(split, ivec2(xx, yy), 0);
            s1 += c.x * c.x;
            s2 += c.y * c.y;
        }
    float n = float((x1 - b.x * TD_B) * (y1 - b.y * TD_B));
    float e1 = s1 / n, e2 = s2 / n, e3 = texelFetch(gblocks, b, 0).y;
    imageStore(meas_out, b, vec4(e1, e2, e2 / (e2 + e3 + TD_FLOOR), e1 + TD_FLOOR));
}
)GLSL";

		// --- Step 5: compose every live slot's motion with this pair (thread per slot x block) ---
		constexpr const char* kComposeBody = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < TD_BW * TD_K && g.y < TD_BH) {
    int k = g.x / TD_BW;
    ivec2 b = ivec2(g.x - k * TD_BW, g.y);
    vec4 r = vec4(0.0);
    if (compose_on > 0.5 && td_tabf(tab_old, ivec2(k, 0)).x > 0.5) {
        float px = (float(b.x) + 0.5) * float(TD_B) - 0.5, py = (float(b.y) + 0.5) * float(TD_B) - 0.5;
        vec3 v = td_pair_bwd(vec2(px, py));
        float hop = (1.0 - v.z) * bwd_bpx;
#ifdef TD_FWD
        vec3 f = td_pair_fwd(vec2(px + v.x, py + v.y));
        hop += sqrt((f.x + v.x) * (f.x + v.x) + (f.y + v.y) * (f.y + v.y));
#endif
        vec3 o = td_mot_grid(k, vec2(px + v.x, py + v.y));
        r = vec4(v.x + o.x, v.y + o.y, o.z + hop, 0.0);
    }
    imageStore(mot_out, g, r);
}
)GLSL";

		// --- Step 6: per (slot, block) validation (the model's EvaluateBlock); one workgroup each ---
		constexpr const char* kEvalBody = R"GLSL(
ivec2 wg = ivec2(gl_WorkGroupID.xy);
int k = wg.x / TD_BW;
ivec2 b = ivec2(wg.x - k * TD_BW, wg.y);
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
vec4 tab = td_tabf(tab_old, ivec2(k, 0));
bool live = tab.x > 0.5 && slots_valid > 0.5;
vec4 a0 = vec4(0.0), a1 = vec4(0.0);   // (c1, c2, w1, w2), (cs, ws, dot, bd)
if (live) {
    for (int j = 0; j < TD_B; j += 16)
        for (int i = 0; i < TD_B; i += 16) {
            ivec2 l = lid + ivec2(i, j);
            ivec2 p = b * TD_B + l;
            if (l.x < TD_B && l.y < TD_B && p.x < TD_W && p.y < TD_H) {
                vec4 sv;
                float bwv;
                td_slot_sample(k, td_position(k, p.x, p.y), sv, bwv);
                vec4 c = texelFetch(split, p, 0);
                float sc = c.x + c.y, sw = sv.x + sv.y;
                a0 += vec4(c.x * c.x, c.y * c.y, sv.x * sv.x, sv.y * sv.y);
                a1 += vec4(sc * sc, sw * sw, sc * sw, abs(c.w - bwv));
            }
        }
}
td_reduce2(a0, a1);
if (gl_LocalInvocationIndex == 0u) {
    vec4 ea = vec4(0.0), eb = vec4(0.0);
    if (live) {
        float n = float((min(TD_W, (b.x + 1) * TD_B) - b.x * TD_B) * (min(TD_H, (b.y + 1) * TD_B) - b.y * TD_B));
        float fl = TD_FLOOR;
        float N = texelFetch(gblocks, b, 0).x;
        float e1c = a0.x / n, e2c = a0.y / n, e1w = a0.z / n, e2w = a0.w / n;
#ifdef TD_BGATES
        vec4 bgv = td_bgate(texelFetch(bgates, b, 0));   // per-block gate factors: margin, base allowance, uncertainty limit, strength
#else
        vec4 bgv = vec4(1.0);
#endif
        float margin = TD_STRONGER_MARGIN * bgv.x;
        float corr = a1.z / sqrt(a1.x * a1.y + 1e-12);
        float corrGate = td_ss(0.5 * TD_CORR_MIN, TD_CORR_MIN, corr);
        float ratio = (e1w + fl) / (e1c + fl);
        float stronger = td_ss(1.0 + margin, 1.0 + 3.0 * margin, ratio) * td_ss(TD_SIG_LO, TD_SIG_HI, e1w / (N + fl));
        float harm = td_ss(1.0 - 3.0 * margin, 1.0 - margin, ratio);
        float hb = td_ss(TD_RHO2_LO, TD_RHO2_HI, (e2c + fl) / (e2w + fl));
        float cx = (float(b.x) + 0.5) * float(TD_B) - 0.5, cy = (float(b.y) + 0.5) * float(TD_B) - 0.5;
        vec2 sp = td_position(k, int(cx), int(cy));
        int sbx = clamp(int(floor((sp.x + 0.5) / float(TD_B))), 0, TD_BW - 1), sby = clamp(int(floor((sp.y + 0.5) / float(TD_B))), 0, TD_BH - 1);
        float Fw = texelFetch(slot_blk, ivec2(k * TD_BW + sbx, sby), 0).x;
        float focus = td_ss(TD_PULL_LO, TD_PULL_HI, texelFetch(meas, b, 0).z / (Fw + 1e-6));
        float u = td_mot_grid(k, vec2(cx, cy)).z;
        float umax = TD_UMAX * bgv.z;
        float unc = 1.0 - td_ss(0.3 * umax, umax, u);
        float allow = (TD_BASE_NOISE + TD_BASE_GRAIN_SIGMAS * sqrt(max(0.0, N)) / 4.0) * bgv.y;
        float bgate = 1.0 - td_ss(allow, 2.0 * allow, a1.w / n);
        float ageSec = (tab.z + 1.0) / hz;
        float ageGate = clamp((max_age - ageSec) / (max_age * TD_AGE_FADE), 0.0, 1.0);
        float hard = unc * bgate * corrGate * harm * hb * focus;
        ea = vec4(hard, ageGate, stronger, e1w);
        eb = vec4(hb, focus, unc * bgate * corrGate, ageSec);
    }
    imageStore(ev_a_out, wg, ea);
    imageStore(ev_b_out, wg, eb);
}
)GLSL";

		// --- Steps 3-4, the model's per-block pass 1: trackers, crossfade, evidence, refresh request ---
		constexpr const char* kTrackBody = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < TD_BW && b.y < TD_BH) {
    TdState st = reset > 0.5 ? td_default() : td_load(b);
    if (motion_lost > 0.5) { st.slot = -1.0; st.prevSlot = -1.0; st.xf = 1.0; st.qref = 0.0; }
    vec4 m = texelFetch(meas, b, 0);
    float q = m.w, e2 = m.y, F = m.z;
    if (st.have < 0.5) {
        st.have = 1.0; st.qPrev = q; st.e2Prev = e2; st.qHigh = q; st.qLow = q; st.rstate = 0.0;
        st.qPeak = q; st.e2Peak = e2; st.fPeak = F;
    } else {
        st.rebounds *= rebound_decay;
        if (st.rstate < 0.5) {
            if (q > st.qHigh) st.qHigh = q;
            else if (q < st.qHigh * (1.0 - TD_REBOUND_TAU) && st.qHigh - q >= TD_REBOUND_MIN) { st.rstate = 1.0; st.qLow = q; }
        } else {
            if (q < st.qLow) st.qLow = q;
            else if (q > st.qLow * (1.0 + TD_REBOUND_TAU) && q - st.qLow >= TD_REBOUND_MIN) { st.rebounds += 1.0; st.rstate = 0.0; st.qHigh = q; }
        }
        bool falling = q < st.qPrev * (1.0 - TD_SOFT_STEP) && e2 < st.e2Prev * (1.0 - TD_SOFT_STEP) && st.qPrev >= TD_RETIRE_MIN;
        st.softRun = falling ? min(st.softRun + 1.0, 1048576.0) : 0.0;   // saturates at TemporalDetail::kRunSaturate = 1 << 20 (as the model)
        st.qPeak *= peak_decay;
        if (q >= st.qPeak) { st.qPeak = q; st.e2Peak = e2; st.fPeak = F; }
        st.qPrev = q; st.e2Prev = e2;
    }
    if (st.prevSlot >= 0.0) {
        st.xf = min(1.0, st.xf + inv_fxf);
        if (st.xf >= 1.0) st.prevSlot = -1.0;
    }
#ifdef TD_QUALITY
    float hint = clamp(texelFetch(quality, b, 0).x, 0.0, 1.0);
#else
    float hint = 1.0;
#endif
    float evidence = max(td_ss(TD_EV_LO, TD_EV_HI, st.rebounds), 1.0 - hint);
    int slot = int(st.slot);
    bool live = slot >= 0 && slot < TD_K && slots_valid > 0.5 && td_tabf(tab_old, ivec2(slot, 0)).x > 0.5;
    if (slot >= 0 && !live) { st.slot = -1.0; st.prevSlot = -1.0; st.xf = 1.0; st.qref = 0.0; }
    int past = (live && have_frame > 0.5) ? slot : -1;
    bool refresh = false;
    if (st.retired < 0.5) {
        if (past < 0) refresh = true;
        else {
            vec4 eb = texelFetch(ev_b, td_blk(past, b), 0);
            float decay = pow(0.5, eb.w / max_age);
            if (q >= st.qref * decay && eb.x * eb.y >= 0.5) refresh = true;
        }
    }
    td_store(b, st);
    imageStore(work_out, b, vec4(float(past), refresh ? 1.0 : 0.0, evidence, 1.0));
}
)GLSL";

		// --- Step 4: admission / eviction (one workgroup: integer reductions, then thread 0 decides) ---
		constexpr const char* kAdmitHeader = R"GLSL(
shared int td_ci[256 * (2 * TD_K + 1)];
)GLSL";
		constexpr const char* kAdmitBody = R"GLSL(
uint li = gl_LocalInvocationIndex;
int cnt[2 * TD_K + 1];
for (int c = 0; c < 2 * TD_K + 1; c++) cnt[c] = 0;
for (int i = int(li); i < TD_BW * TD_BH; i += 256) {
    ivec2 b = ivec2(i % TD_BW, i / TD_BW);
    if (texelFetch(work, b, 0).y > 0.5) cnt[0] += 1;
    vec4 s0 = texelFetch(st_in, ivec2(b.x * 8, b.y), 0), s1 = texelFetch(st_in, ivec2(b.x * 8 + 1, b.y), 0);
    int slot = int(s0.x), prev = int(s0.y);
    bool faded = s1.x * s1.y >= TD_EVICT_WEIGHT;
    for (int k = 0; k < TD_K; k++)
        if (slot == k || prev == k) { cnt[1 + k] += 1; if (faded) cnt[1 + TD_K + k] += 1; }
}
for (int c = 0; c < 2 * TD_K + 1; c++) td_ci[c * 256 + int(li)] = cnt[c];
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        for (int c = 0; c < 2 * TD_K + 1; c++) td_ci[c * 256 + int(li)] += td_ci[c * 256 + int(li + s)];
    barrier();
}
if (li == 0u) {
    int refreshes = td_ci[0];
    float live[TD_K];
    uint ids[TD_K], age[TD_K], cleaned[TD_K];
    int nlive = 0;
    for (int k = 0; k < TD_K; k++) {
        uvec4 t = texelFetch(tab_old, ivec2(k, 0), 0);
        bool l = t.x > 0u && slots_valid > 0.5;
        live[k] = l ? 1.0 : 0.0; ids[k] = t.y; age[k] = t.z + 1u; cleaned[k] = t.w;
        if (l) nlive++;
    }
    uint nextId = reset > 0.5 ? 0u : texelFetch(tab_old, ivec2(TD_K, 0), 0).z;
    int victim = -1, cur = -1;
    bool admitted = false;
    if (refreshes > 0) {
        if (nlive >= TD_K) {
            // the first slot (in admission order) no block references, else the oldest whose blocks have faded
            uint best = 0u;   // ids compare by int(a - b) < 0 (wrap-safe); victim < 0 means none chosen yet
            for (int k = 0; k < TD_K; k++)
                if (live[k] > 0.5 && td_ci[(1 + k) * 256] == 0 && (victim < 0 || int(ids[k] - best) < 0)) { best = ids[k]; victim = k; }
            if (victim < 0) {
                uint oldest = 0u;
                for (int k = 0; k < TD_K; k++)
                    if (live[k] > 0.5 && td_ci[(1 + TD_K + k) * 256] == 0 && (victim < 0 || age[k] > oldest)) { oldest = age[k]; victim = k; }
            }
            if (victim >= 0) { live[victim] = 0.0; nlive--; }
        }
        if (nlive < TD_K) {
            cur = victim;
            if (cur < 0)
                for (int k = TD_K - 1; k >= 0; k--)
                    if (live[k] < 0.5) cur = k;
            live[cur] = 1.0; ids[cur] = nextId; nextId += 1u; age[cur] = 0u; cleaned[cur] = TD_TEMPLATE_CLEAN > 0.5 ? 1u : 0u;
            nlive++;
            admitted = true;
        }
    }
    for (int k = 0; k < TD_K; k++) imageStore(tab_out, ivec2(k, 0), uvec4(live[k] > 0.5 ? 1u : 0u, ids[k], age[k], cleaned[k]));
    imageStore(tab_out, ivec2(TD_K, 0), uvec4(admitted ? 1u : 0u, uint(cur + 1), nextId, uint(refreshes)));
    imageStore(tab_out, ivec2(TD_K + 1, 0), uvec4(uint(victim + 1), uint(nlive), 0u, 0u));
}
)GLSL";

		// --- Step 4a: the multi-frame DCT-patch cleaner, one P x P workgroup per patch of one parity class (patches of
		// one class tile the plane exactly, so four classes accumulate into ping-pong planes without atomics). The
		// members' DCTs are summed through linearity: sum_m X0 Xm = X0 DCT(sum_m xm). Off admission frames every texture
		// read is skipped and nothing is written (the transforms of zeros still run: no barrier in varying flow).
		constexpr const char* kCleanHeader = R"GLSL(
shared float td_ca[TD_P * TD_P];
shared float td_cb[TD_P * TD_P];
shared float td_cs[2];
// Separable orthonormal DCT-II of the P x P block held one value per thread (thread (i, j) = element j P + i),
// the model's Dct2 summation order. Every thread must call it (barriers).
float td_dct(float v, bool inv)
{
    int i = int(gl_LocalInvocationID.x), j = int(gl_LocalInvocationID.y);
    barrier();
    td_ca[j * TD_P + i] = v;
    barrier();
    float a = 0.0;
    if (!inv) { for (int x = 0; x < TD_P; x++) a += TD_C[i * TD_P + x] * td_ca[j * TD_P + x]; }
    else      { for (int x = 0; x < TD_P; x++) a += TD_C[x * TD_P + i] * td_ca[j * TD_P + x]; }
    td_cb[j * TD_P + i] = a;
    barrier();
    float o = 0.0;
    if (!inv) { for (int y = 0; y < TD_P; y++) o += TD_C[j * TD_P + y] * td_cb[y * TD_P + i]; }
    else      { for (int y = 0; y < TD_P; y++) o += TD_C[y * TD_P + j] * td_cb[y * TD_P + i]; }
    return o;
}
// Two raster-order sums over the patch (thread 0, the model's order), returned to every thread.
vec2 td_patch_sum2(float a, float b)
{
    int li = int(gl_LocalInvocationIndex);
    barrier();
    td_ca[li] = a;
    td_cb[li] = b;
    barrier();
    if (li == 0) {
        float sa = 0.0, sb = 0.0;
        for (int q = 0; q < TD_P * TD_P; q++) { sa += td_ca[q]; sb += td_cb[q]; }
        td_cs[0] = sa;
        td_cs[1] = sb;
    }
    barrier();
    return vec2(td_cs[0], td_cs[1]);
}
)GLSL";
		constexpr const char* kCleanBody = R"GLSL(
vec4 fs = td_tabf(tab_new, ivec2(TD_K, 0));
bool admitted = fs.x > 0.5;
int cur = (int(fs.y) - 1);
ivec2 wgid = ivec2(gl_WorkGroupID.xy);
int ix = 2 * wgid.x + TD_EX, iy = 2 * wgid.y + TD_EY;
int ox = -TD_HALF + ix * TD_HALF, oy = -TD_HALF + iy * TD_HALF;
int i = int(gl_LocalInvocationID.x), j = int(gl_LocalInvocationID.y);
int xx = ox + i, yy = oy + j;
bool inside = admitted && xx >= 0 && yy >= 0 && xx < TD_W && yy < TD_H;
ivec2 cb = ivec2(clamp((ox + TD_HALF) / TD_B, 0, TD_BW - 1), clamp((oy + TD_HALF) / TD_B, 0, TD_BH - 1));
float wv = TD_WIN[i] * TD_WIN[j];
vec4 sp = inside ? texelFetch(split, ivec2(xx, yy), 0) : vec4(0.0);
vec2 sums = td_patch_sum2(inside ? wv * wv * sp.z * sp.z : 0.0, inside ? wv * wv : 0.0);
float wf2 = sums.x / float(TD_P * TD_P), w2 = sums.y / float(TD_P * TD_P);
float X1 = td_dct(inside ? wv * sp.x : 0.0, false);
float X2 = td_dct(inside ? wv * sp.y : 0.0, false);
int ord[TD_K];
td_order(ord);
float xsum = 0.0;
int M = 0;
for (int r = 0; r < TD_K; r++) {
    int m = ord[r];
    if (m < 0 || m == cur) continue;
    vec4 ea = texelFetch(ev_a, td_blk(m, cb), 0), eb = texelFetch(ev_b, td_blk(m, cb), 0);
    if (!(eb.z >= TD_CLEAN_MEMBER_MIN && ea.y > 0.0)) continue;
    M++;
    if (inside) {
        vec4 sv;
        float bwv;
        td_slot_sample(m, td_position(m, xx, yy), sv, bwv);
        xsum += wv * td_gain(bwv, sp.w) * (sv.x + sv.y);
    }
}
float crossv = (X1 + X2) * td_dct(xsum, false);
float N = admitted ? max(0.0, texelFetch(gblocks, cb, 0).x) : 0.0;
float hs = TD_HS[j * TD_P + i];
float X0 = X1 + X2, V0 = N * hs * wf2;
float W = 1.0;
if (V0 > 0.0) {
    float Pw = max(0.0, (X0 * X0 - V0 + crossv) / (1.0 + float(M)));
    W = Pw / (Pw + clean_kappa * V0);
}
X1 *= W;
X2 *= W;
vec2 rr = td_patch_sum2(W * W * V0, N * hs * w2);
float y1 = td_dct(X1, true), y2 = td_dct(X2, true);
if (admitted && gl_LocalInvocationIndex == 0u) imageStore(patch_out, ivec2(ix, iy), vec4(rr, 0.0, 0.0));
if (inside) {
#ifdef TD_ACC_IN
    vec2 prior = texelFetch(acc_in, ivec2(xx, yy), 0).xy;
#else
    vec2 prior = vec2(0.0);
#endif
    imageStore(acc_out, ivec2(xx, yy), vec4(prior + wv * vec2(y1, y2), 0.0, 0.0));
}
)GLSL";

		// The new slot's planes (admitted frames only): slot<cur> = (s1, s2, c1, c2), c = cleaned (or s without the cleaner).
		constexpr const char* kFinishBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
vec4 fs = td_tabf(tab_new, ivec2(TD_K, 0));
if (fs.x > 0.5 && p.x < TD_W && p.y < TD_H) {
    vec4 sp = texelFetch(split, p, 0);
#ifdef TD_CLEANED
    vec2 c = texelFetch(acc_in, p, 0).xy;
#else
    vec2 c = sp.xy;
#endif
    td_slot_store((int(fs.y) - 1), p, vec4(sp.x, sp.y, c.x, c.y));
}
)GLSL";
		constexpr const char* kBaseCopyBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
vec4 fs = td_tabf(tab_new, ivec2(TD_K, 0));
if (fs.x > 0.5 && p.x < TD_W && p.y < TD_H)
    td_base_store((int(fs.y) - 1), p, vec4(texelFetch(split, p, 0).w * TD_SB_STORE, 0.0, 0.0, 1.0));
)GLSL";
		// The new slot's block maps and zero motion; rho2 = sum W^2 V0 / sum N hs w2 over the patches centred in the block.
		constexpr const char* kCopyBlkBody = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
vec4 fs = td_tabf(tab_new, ivec2(TD_K, 0));
if (fs.x > 0.5 && b.x < TD_BW && b.y < TD_BH) {
    int cur = (int(fs.y) - 1);
    float rho2 = 1.0;
#ifdef TD_CLEANED
    int ix0 = (b.x * TD_B + TD_HALF - 1) / TD_HALF, ix1 = b.x == TD_BW - 1 ? TD_NPX : ((b.x + 1) * TD_B + TD_HALF - 1) / TD_HALF;
    int iy0 = (b.y * TD_B + TD_HALF - 1) / TD_HALF, iy1 = b.y == TD_BH - 1 ? TD_NPY : ((b.y + 1) * TD_B + TD_HALF - 1) / TD_HALF;
    float rg = 0.0, rv = 0.0;
    for (int iy = iy0; iy < min(iy1, TD_NPY); iy++)
        for (int ix = ix0; ix < min(ix1, TD_NPX); ix++) {
            vec2 r = texelFetch(patches, ivec2(ix, iy), 0).xy;
            rg += r.x;
            rv += r.y;
        }
    if (rv > 0.0) rho2 = min(1.0, rg / rv);
#endif
    vec4 m = texelFetch(meas, b, 0);
    imageStore(slot_blk_out, td_blk(cur, b), vec4(m.z, m.w, texelFetch(gblocks, b, 0).x, rho2));
    imageStore(mot_out, td_blk(cur, b), vec4(0.0));
}
)GLSL";

		// --- Steps 7-8, the model's per-block pass 2: weights (limiter), retire, boost, template members ---
		constexpr const char* kWeightBody = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < TD_BW && b.y < TD_BH) {
    TdState st = td_load(b);
    vec4 wk = texelFetch(work, b, 0);
    vec4 fs = td_tabf(tab_new, ivec2(TD_K, 0));
    bool admitted = fs.x > 0.5;
    int cur = admitted ? (int(fs.y) - 1) : -1;
    int victim = int(td_tabf(tab_new, ivec2(TD_K + 1, 0)).x) - 1;
    int past = int(wk.x);
    bool refresh = wk.y > 0.5;
    float evidence = wk.z, referee = wk.w;
    if (victim >= 0) {
        if (int(st.slot) == victim) { st.slot = -1.0; st.qref = 0.0; }
        if (int(st.prevSlot) == victim) st.prevSlot = -1.0;
        if (past == victim) past = -1;
    }
    bool refreshNow = refresh && admitted;
    vec4 m = texelFetch(meas, b, 0);
#ifdef TD_BGATES
    float bstrength = strength * td_bgate(texelFetch(bgates, b, 0)).w;
#else
    float bstrength = strength;
#endif
    vec4 pa = vec4(0.0), pb = vec4(0.0);
    if (past >= 0) { pa = texelFetch(ev_a, td_blk(past, b), 0); pb = texelFetch(ev_b, td_blk(past, b), 0); }
    float soft = 0.0, hardT = 0.0;
    bool chosen = false;
    if (refreshNow) {
        if (past >= 0) { st.prevSlot = st.slot; st.xf = 0.0; } else { st.prevSlot = -1.0; st.xf = 1.0; }
        st.slot = float(cur);
        st.qref = m.w;
        soft = st.tHold;
        hardT = st.hSm;
    } else if (past >= 0) {
        chosen = true;
        hardT = pa.x * referee;
        soft = bstrength * evidence * pa.y * pa.z * (st.retired > 0.5 ? 0.0 : 1.0);
        st.tHold = soft;
        if (pb.x * pb.y < 0.1) st.vetoRun = min(st.vetoRun + 1.0, 1048576.0); else st.vetoRun = 0.0;
    } else {
        st.vetoRun = 0.0;
    }
    if (st.retired < 0.5 && past >= 0 && (st.softRun >= f_run || st.vetoRun >= f_run)) st.retired = 1.0;
    if (st.retired > 0.5) soft = 0.0;
    if (st.on < 0.5 && soft >= TD_ENTER) st.on = 1.0;
    else if (st.on > 0.5 && soft < TD_LEAVE) st.on = 0.0;
    float tEff = st.on > 0.5 ? soft : 0.0;
    st.wLim += clamp(tEff - st.wLim, -fall, rise);
    if (hardT < st.hSm) st.hSm = hardT; else st.hSm = min(hardT, st.hSm + rise);
    float weight = st.wLim * st.hSm;
    if (st.retired > 0.5 && weight < TD_LEAVE * 0.5 && st.wLim <= 0.0) { st.slot = -1.0; st.prevSlot = -1.0; st.xf = 1.0; st.qref = 0.0; st.hSm = 0.0; }
    if (st.retired > 0.5 && st.softRun < f_run && st.vetoRun < f_run && st.slot < 0.0) st.retired = 0.0;
    // fallback boost (structure only)
    float q = m.w;
    float raw = sqrt(max(st.qPeak, q) / q) - 1.0;
    float bn = boost_max > 0.0 ? clamp(raw / boost_max, 0.0, 1.0) : 0.0;
    float hbP = td_ss(TD_RHO2_LO, TD_RHO2_HI, (m.y + TD_FLOOR) / (st.e2Peak + TD_FLOOR));
    float fP = td_ss(TD_PULL_LO, TD_PULL_HI, m.z / (st.fPeak + 1e-6));
    float noRef = 1.0 - td_ss(0.0, 0.5, weight);
    bn *= hbP * fP * evidence * noRef * bstrength;
    bn = st.retired > 0.5 ? 0.0 : bn;
    if (st.bOn < 0.5 && bn >= TD_ENTER) st.bOn = 1.0;
    else if (st.bOn > 0.5 && bn < TD_LEAVE) st.bOn = 0.0;
    st.bLim += clamp((st.bOn > 0.5 ? bn : 0.0) - st.bLim, -fall, rise);
    float boost = boost_max * st.bLim;
#ifdef TD_GUARD
    float guard = st.guard;
#else
    float guard = 1.0;
#endif
    float cwv = refreshNow ? 0.0 : weight * guard;
    float cboost = boost * guard;
    int a = -1, prev = -1;
    float xf = 1.0;
    if (!refreshNow && chosen) {
        a = int(st.slot);
        if (!td_live_new(a)) a = -1;
        prev = int(st.prevSlot);
        if (!td_live_new(prev)) prev = -1;
        xf = prev >= 0 ? st.xf : 1.0;
        if (a < 0) cwv = 0.0;
    }
    float coefs[TD_K];
    for (int k = 0; k < TD_K; k++) coefs[k] = 0.0;
    float nT = 0.0;
    int nm = 0;
    if (cwv > 0.0 && a >= 0) {
        int ord[TD_K];
        td_order(ord);
        float aSel = sqrt(pa.w + TD_FLOOR);
        float aw[TD_K], om[TD_K];
        int mem[TD_K];
        for (int r = 0; r < TD_K; r++) {
            int k = ord[r];
            if (k < 0) continue;
            float awk = 0.0, omega = 0.0;
            if (k == a) { awk = aSel; omega = prev >= 0 ? xf : 1.0; }
            else if (k == cur) continue;
            else {
                vec4 ea = texelFetch(ev_a, td_blk(k, b), 0);
                bool valid = ea.x >= TD_TEMPLATE_MIN_HARD && ea.y > 0.0;
                omega = (prev >= 0 && k == prev) ? max(1.0 - xf, valid ? 1.0 : 0.0) : (valid ? 1.0 : 0.0);
                awk = sqrt(ea.w + TD_FLOOR);
            }
            if (!(omega > 0.0)) continue;
            mem[nm] = k; aw[nm] = awk; om[nm] = omega; nm++;
        }
        float den = 0.0;
        for (int r = 0; r < nm; r++) den += om[r] * aw[r] * aw[r];
        for (int r = 0; r < nm; r++) {
            int k = mem[r];
            float cm = den > 0.0 ? aSel * om[r] * aw[r] / den : 0.0;
            float rho2 = 1.0;
            if (TD_TEMPLATE_CLEAN > 0.5 && td_tabf(tab_new, ivec2(k, 0)).w > 0.5) {
                vec2 sp = td_position(k, int((float(b.x) + 0.5) * float(TD_B) - 0.5), int((float(b.y) + 0.5) * float(TD_B) - 0.5));
                int sbx = clamp(int(floor((sp.x + 0.5) / float(TD_B))), 0, TD_BW - 1), sby = clamp(int(floor((sp.y + 0.5) / float(TD_B))), 0, TD_BH - 1);
                rho2 = texelFetch(slot_blk, ivec2(k * TD_BW + sbx, sby), 0).w;
            }
            coefs[k] = cm;
            nT += cm * cm * rho2;
        }
    }
    for (int k = 0; k < TD_K; k++) imageStore(coef_out, td_blk(k, b), vec4(coefs[k], 0.0, 0.0, 0.0));
    imageStore(cw_out, b, vec4(cwv, cboost, nT, float(nm)));
    td_store(b, st);
}
)GLSL";

		// One slot's lighting-gained bands at a pixel (needs tab_new and the slot snippets): g_k (band 1, band 2) of the
		// stored frame warped once (cleaned bands where the slot was cleaned).
		constexpr const char* kTemplate = R"GLSL(
vec2 td_slot_bands(int k, int x, int y, float bc)
{
    vec4 sv;
    float bwv;
    td_slot_sample(k, td_position(k, x, y), sv, bwv);
    bool cl = TD_TEMPLATE_CLEAN > 0.5 && td_tabf(tab_new, ivec2(k, 0)).w > 0.5;
    return td_gain(bwv, bc) * (cl ? sv.zw : sv.xy);
}
)GLSL";

		// --- Step 8: per-block projection of the current structure onto its template (one workgroup per block) ---
		constexpr const char* kProjBody = R"GLSL(
ivec2 b = ivec2(gl_WorkGroupID.xy);
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
vec4 c = texelFetch(cw, b, 0);
bool act = c.x > 0.0;
int ord[TD_K];
td_order(ord);
float coefs[TD_K];
for (int k = 0; k < TD_K; k++) coefs[k] = texelFetch(coef, td_blk(k, b), 0).x;
vec4 a0 = vec4(0.0), a1 = vec4(0.0);   // (st1, tt1, ss1, st2), (tt2, ss2, 0, 0)
if (act) {
    for (int j = 0; j < TD_B; j += 16)
        for (int i = 0; i < TD_B; i += 16) {
            ivec2 l = lid + ivec2(i, j);
            ivec2 p = b * TD_B + l;
            if (l.x < TD_B && l.y < TD_B && p.x < TD_W && p.y < TD_H) {
                vec4 sp = texelFetch(split, p, 0);
                vec2 t = vec2(0.0);
                for (int r = 0; r < TD_K; r++) {
                    int k = ord[r];
                    if (k >= 0 && coefs[k] > 0.0) t += coefs[k] * td_slot_bands(k, p.x, p.y, sp.w);
                }
                a0 += vec4(sp.x * t.x, t.x * t.x, sp.x * sp.x, sp.y * t.y);
                a1 += vec4(t.y * t.y, sp.y * sp.y, 0.0, 0.0);
            }
        }
}
td_reduce2(a0, a1);
if (gl_LocalInvocationIndex == 0u) {
    vec4 tl = vec4(0.0);
    float w = 0.0;
    if (act) {
        float st1 = a0.x, tt1 = a0.y, ss1 = a0.z, st2 = a0.w, tt2 = a1.x, ss2 = a1.y;
        float lam1 = tt1 > 1e-9 ? clamp(st1 / tt1, 0.0, 1.0) : 1.0;
        float lam2 = tt2 > 1e-9 ? clamp(st2 / tt2, 0.0, 1.0) : 1.0;
        float tau1 = tt1 > 1e-9 ? (1.0 - lam1) * td_ss(TD_COHERENCE_LO, TD_COHERENCE_HI, st1 / sqrt(tt1 * ss1 + 1e-12)) : 0.0;
        float tau2 = tt2 > 1e-9 ? (1.0 - lam2) * td_ss(TD_COHERENCE_LO, TD_COHERENCE_HI, st2 / sqrt(tt2 * ss2 + 1e-12)) : 0.0;
        tl = vec4(tau1, tau2, lam1, lam2);
        w = (tau1 <= 0.0 && tau2 <= 0.0) ? 0.0 : c.x;
    }
    imageStore(ct_out, b, tl);
    imageStore(cf_out, b, vec4(w, c.y, c.z, c.w));
}
)GLSL";

		// --- Steps 8-9: the transfer on the grain-free picture (per pixel): delta and what the guard reads. Each needed
		// slot is sampled once per pixel and shared by the four neighbouring blocks' templates. ---
		constexpr const char* kTransferBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < TD_W && p.y < TD_H) {
    vec2 g = (vec2(p) + vec2(0.5)) / float(TD_B) - vec2(0.5);
    vec2 fg = floor(g);
    vec2 t = g - fg;
    int i0 = int(fg.x), j0 = int(fg.y);
    ivec2 nb[4];
    float omv[4];
    bool need[TD_K];
    for (int k = 0; k < TD_K; k++) need[k] = false;
    bool act = false;
    for (int q = 0; q < 4; q++) {
        nb[q] = ivec2(clamp(i0 + (q & 1), 0, TD_BW - 1), clamp(j0 + (q >> 1), 0, TD_BH - 1));
        omv[q] = ((q & 1) != 0 ? t.x : 1.0 - t.x) * ((q >> 1) != 0 ? t.y : 1.0 - t.y);
        vec4 c = texelFetch(cf, nb[q], 0);
        if (omv[q] > 0.0 && (c.x > 0.0 || c.y > 0.0)) act = true;
        if (omv[q] > 0.0 && c.x > 0.0)
            for (int k = 0; k < TD_K; k++)
                if (texelFetch(coef, td_blk(k, nb[q]), 0).x > 0.0) need[k] = true;
    }
    vec2 res = vec2(0.0);
    if (act) {
        vec4 sp = texelFetch(split, p, 0);
        float S = sp.x + sp.y, bs = sp.w, f = sp.z;
        vec2 sk[TD_K];
        for (int k = 0; k < TD_K; k++) sk[k] = need[k] ? td_slot_bands(k, p.x, p.y, bs) : vec2(0.0);
        int ord[TD_K];
        td_order(ord);
        float raw = 0.0, pred = 0.0, amp = 0.0, refMax = 0.0, bsum = 0.0, wpix = 0.0, tv = 0.0;
        for (int q = 0; q < 4; q++) {
            vec4 c = texelFetch(cf, nb[q], 0);
            float om = omv[q];
            if (om <= 0.0 || (c.x <= 0.0 && c.y <= 0.0)) continue;
            if (c.x > 0.0) {
                vec4 tl = texelFetch(ct, nb[q], 0);
                vec2 tt = vec2(0.0);
                for (int r = 0; r < TD_K; r++) {
                    int k = ord[r];
                    if (k < 0) continue;
                    float ck = texelFetch(coef, td_blk(k, nb[q]), 0).x;
                    if (ck > 0.0) tt += ck * sk[k];
                }
                raw += om * c.x * (tl.x * tt.x + tl.y * tt.y);
                pred += om * c.x * (tl.z * tt.x + tl.w * tt.y);
                amp += om * c.x * max(tl.x, tl.y) * sqrt(max(0.0, c.z));
                refMax = max(refMax, abs(tt.x + tt.y));
                tv += om * c.x * (tt.x + tt.y);
                wpix += om * c.x;
            }
            bsum += om * c.y;
        }
        if (wpix > 0.0 || bsum > 0.0) {
            float beta = 1.0;
            if (grain_budget > 0.0 && amp > 0.0) beta = min(1.0, grain_budget / amp);
            float dv = beta * raw;
            if (wpix > 0.0 && dv != 0.0) {
                float P = pred / wpix, tr = dv / wpix;
                float cap = TD_PIXEL_CAP * max(abs(P), refMax);
                dv = (clamp(P + tr, -cap, cap) - P) * wpix;
            }
            float dT = dv;
            float bEff = min(bsum, TD_BOOST_GRAIN_SHARE / max(f, 1e-6));
            if (bEff > 0.0) {
                float cap = (1.0 + bsum) * abs(S);
                dv += clamp(S * (1.0 + bEff), -cap, cap) - S;
            }
            float tval = wpix > 0.0 ? tv / wpix : 0.0;
            bool structural = guard_share > 0.0 && tval * tval > guard_share * texelFetch(gblocks, ivec2(p.x / TD_B, p.y / TD_B), 0).x;
            res = vec2(dv, structural ? dv - dT : dv);
        }
    }
    imageStore(delta_out, p, vec4(res, 0.0, 0.0));
}
)GLSL";

		// Catmull-Rom of the previous frame's guard grain planes (needs prevg).
		constexpr const char* kPrevG = R"GLSL(
vec2 td_prevg(vec2 pos)
{
    vec2 f = floor(pos);
    vec4 wx = td_cr(pos.x - f.x), wy = td_cr(pos.y - f.y);
    ivec2 o = ivec2(f) - ivec2(1);
    vec2 a = vec2(0.0);
    for (int j = 0; j < 4; j++) {
        vec2 r = vec2(0.0);
        for (int i = 0; i < 4; i++) r += wx[i] * texelFetch(prevg, clamp(o + ivec2(i, j), ivec2(0), ivec2(TD_W - 1, TD_H - 1)), 0).xy;
        a += wy[j] * r;
    }
    return a;
}
)GLSL";

		// --- Step 10: the guard's tile sums on the shared grain map (one workgroup per block) ---
		constexpr const char* kGuardSumsBody = R"GLSL(
ivec2 b = ivec2(gl_WorkGroupID.xy);
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
vec4 a0 = vec4(0.0), a1 = vec4(0.0);   // (Ein, Eout, cross, Lin), (Lout, Pin, Pout, 0)
for (int j = 0; j < TD_B; j += 16)
    for (int i = 0; i < TD_B; i += 16) {
        ivec2 l = lid + ivec2(i, j);
        ivec2 p = b * TD_B + l;
        if (l.x < TD_B && l.y < TD_B && p.x < TD_W && p.y < TD_H) {
            vec4 sp = texelFetch(split, p, 0);
            if (sp.z < TD_GUARD_MAX_SHARE) {
                float y = texelFetch(src, p, 0).x * TD_INV_CU;
                float gin = y - (sp.w + sp.x + sp.y);
                float dd = (1.0 - sp.z) * texelFetch(delta, p, 0).y;
                float gout = gin + dd;
                a0.xyz += vec3(gin * gin, gout * gout, dd * gin);
#ifdef TD_LAG
                vec3 v = td_pair_bwd(vec2(p));
                vec2 pg = td_prevg(vec2(p) + v.xy);
                a0.w += gin * pg.x;
                a1.xyz += vec3(gout * pg.y, pg.x * pg.x, pg.y * pg.y);
#endif
            }
        }
    }
td_reduce2(a0, a1);
if (gl_LocalInvocationIndex == 0u) {
    imageStore(gsum_out, ivec2(2 * b.x, b.y), a0);
    imageStore(gsum_out, ivec2(2 * b.x + 1, b.y), a1);
}
)GLSL";

		// (a) this frame's pooled add-back factor; the per-block EMA of the tile sums (thread per block).
		constexpr const char* kGuardEmaBody = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < TD_BW && b.y < TD_BH) {
    TdState st = td_load(b);
    float o0 = 0.0, o2 = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec4 s = texelFetch(gsum, ivec2(2 * clamp(b.x + i, 0, TD_BW - 1), clamp(b.y + j, 0, TD_BH - 1)), 0);
            o0 += s.x;
            o2 += s.z;
        }
    float kk = o0 > 1e-9 ? -o2 / o0 : 0.0;
    float kb = 0.0;
#ifdef TD_GUARD
    if (kk > TD_ADDBACK_MIN) kb = min(1.0, kk);
#endif
    vec4 s0 = texelFetch(gsum, ivec2(2 * b.x, b.y), 0), s1 = texelFetch(gsum, ivec2(2 * b.x + 1, b.y), 0);
    st.ema0 = alpha * st.ema0 + s0.x; st.ema1 = alpha * st.ema1 + s0.y; st.ema2 = alpha * st.ema2 + s0.z;
    st.ema3 = alpha * st.ema3 + s0.w; st.ema4 = alpha * st.ema4 + s1.x; st.ema5 = alpha * st.ema5 + s1.y;
    st.ema6 = alpha * st.ema6 + s1.z;
    td_store(b, st);
    imageStore(kback_out, b, vec4(kb, 0.0, 0.0, 0.0));
}
)GLSL";

		// (b) the pooled EMA: trip / release with hysteresis, the guard factor (fast fall, slow rise).
		constexpr const char* kGuardPoolBody = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < TD_BW && b.y < TD_BH) {
    TdState st = td_load(b);
    float o[7];
    for (int q = 0; q < 7; q++) o[q] = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            ivec2 nq = ivec2(clamp(b.x + i, 0, TD_BW - 1), clamp(b.y + j, 0, TD_BH - 1));
            vec4 e6 = texelFetch(st_in, ivec2(nq.x * 8 + 6, nq.y), 0), e7 = texelFetch(st_in, ivec2(nq.x * 8 + 7, nq.y), 0);
            o[0] += e6.y; o[1] += e6.z; o[2] += e6.w; o[3] += e7.x; o[4] += e7.y; o[5] += e7.z; o[6] += e7.w;
        }
    float v = 0.0;
    if (o[0] > 1e-9) {
        float L = sqrt(o[1] / o[0]);
        float drop = max(1.0 - L, -o[2] / o[0]);
        v = max(drop / TD_GUARD_DROP_TRIP, (L - 1.0) / TD_GUARD_RISE_TRIP);
        if (o[5] > 1e-9 && o[6] > 1e-9 && o[1] > 1e-9) {
            float freeze = o[4] / sqrt(o[1] * o[6]) - o[3] / sqrt(o[0] * o[5]);
            v = max(v, freeze / TD_GUARD_FREEZE_TRIP);
        }
    }
    float tripped = 0.0;
#ifdef TD_GUARD
    if (st.guardOn < 0.5 && v > 1.0) { st.guardOn = 1.0; tripped = 1.0; }
    else if (st.guardOn > 0.5 && v < TD_GUARD_RELEASE) st.guardOn = 0.0;
    float target = st.guardOn > 0.5 ? (v > 1.0 ? st.guard / v : st.guard) : 1.0;
    st.guard += clamp(target - st.guard, -g_fall, g_rise);
#endif
    td_store(b, st);
    imageStore(gflag_out, b, vec4(st.guardOn, tripped, texelFetch(kback, b, 0).x > 0.0 ? 1.0 : 0.0, v));
}
)GLSL";

		// Status counters (one workgroup): [0] (guard active blocks, add-back blocks, guard trips so far, refreshes),
		// [1] (admitted, live slots, 0, 0).
		constexpr const char* kStatsHeader = R"GLSL(
shared vec4 td_sc[256];
)GLSL";
		constexpr const char* kStatsBody = R"GLSL(
uint li = gl_LocalInvocationIndex;
vec4 a = vec4(0.0);
for (int i = int(li); i < TD_BW * TD_BH; i += 256) {
    vec4 gfl = texelFetch(gflag, ivec2(i % TD_BW, i / TD_BW), 0);
    a += vec4(gfl.x > 0.5 ? 1.0 : 0.0, gfl.z > 0.5 ? 1.0 : 0.0, gfl.y > 0.5 ? 1.0 : 0.0, 0.0);
}
td_sc[li] = a;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s) td_sc[li] += td_sc[li + s];
    barrier();
}
if (li == 0u) {
    uvec4 old = texelFetch(counters_old, ivec2(0, 0), 0);
    uvec4 fs = texelFetch(tab_new, ivec2(TD_K, 0), 0), f2 = texelFetch(tab_new, ivec2(TD_K + 1, 0), 0);
    // the per-frame tile sums are small exact counts; the guard-trip total is cumulative, kept as a uint32 (wraps at 2^32)
    imageStore(counters_out, ivec2(0, 0), uvec4(uint(td_sc[0].x), uint(td_sc[0].y), old.z + uint(td_sc[0].z), fs.w));
    imageStore(counters_out, ivec2(1, 0), uvec4(fs.x, f2.y, 0u, 0u));
}
)GLSL";

		// --- Step 9 / 10a: recompose. Y_out = Y + delta (+ the guard's add-back of measured missing grain); a pixel with
		// nothing to add is copied unchanged (bit-exact). Also the guard's previous-frame planes.
		constexpr const char* kApplyBody = R"GLSL(
ivec2 pf = ivec2(gl_GlobalInvocationID.xy);
if (pf.x < TD_FW && pf.y < TD_FH) {
    vec4 c = texelFetch(src, pf, 0);
    vec4 o = c;   // outside the active picture: the input, bit-exact
    ivec2 p = pf - ivec2(TD_OX, TD_OY);
    if (p.x >= 0 && p.y >= 0 && p.x < TD_W && p.y < TD_H) {
        vec4 sp = texelFetch(split, p, 0);
        vec2 d = texelFetch(delta, p, 0).xy;
        float y = c.x * TD_INV_CU;
        float gin = y - (sp.w + sp.x + sp.y);
        vec2 g = (vec2(p) + vec2(0.5)) / float(TD_B) - vec2(0.5);
        vec2 fg = floor(g);
        vec2 t = g - fg;
        int i0 = int(fg.x), j0 = int(fg.y);
        float k00 = texelFetch(kback, ivec2(clamp(i0, 0, TD_BW - 1), clamp(j0, 0, TD_BH - 1)), 0).x;
        float k10 = texelFetch(kback, ivec2(clamp(i0 + 1, 0, TD_BW - 1), clamp(j0, 0, TD_BH - 1)), 0).x;
        float k01 = texelFetch(kback, ivec2(clamp(i0, 0, TD_BW - 1), clamp(j0 + 1, 0, TD_BH - 1)), 0).x;
        float k11 = texelFetch(kback, ivec2(clamp(i0 + 1, 0, TD_BW - 1), clamp(j0 + 1, 0, TD_BH - 1)), 0).x;
        float kk = (k00 * (1.0 - t.x) + k10 * t.x) * (1.0 - t.y) + (k01 * (1.0 - t.x) + k11 * t.x) * t.y;
        if (kk > 0.0) { d.x += kk * gin; d.y += kk * gin; }
        if (d.x != 0.0) o.x = (y + d.x) * TD_CU;
        imageStore(prevg_out, p, vec4(gin, d.y != 0.0 ? gin + (1.0 - sp.z) * d.y : gin, 0.0, 0.0));
    }
    imageStore(dst, pf, o);
}
)GLSL";

		// The active picture copied out of the plane (bit-exact), so every pass treats its border as the plane edge.
		constexpr const char* kCropBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < TD_W && p.y < TD_H) imageStore(crop_out, p, texelFetch(src, p + ivec2(TD_OX, TD_OY), 0));
)GLSL";
	}
}
