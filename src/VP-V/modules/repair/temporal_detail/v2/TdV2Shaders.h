// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 + TD-S5-OPT (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5):
// Zero-fidelity-loss optimized GLSL compute shader sources for the 4-pass GPU fusion engine + chroma apply:
//   - Speedup (1): LDS-tiled single-loop separable blurs (kPass1BandH / kPass1BandV: 62 -> 3.25 fetches/px, bit-exact FP32)
//   - Speedup (2): 2-stage coalesced LDS cell-moment reduction + fused Focus Guard (kPass2CellStats -> kPass2MergeAndSolve
//                  using exact Chan-Pebay parallel second-moment merge, zero atomics, zero cancellation)
//   - Speedup (3): Compact Preset::RealTime band storage (full-res RG16F zero-mean (M,H) via fused 1-pass 2D LDS G_fine
//                  + half-res RG32F FP32 (L,D3) using U2 VpUnit::BandFromBase::HalfSigma taps; Preset::Reference stays RGBA32F)
//   - Speedup (4): Single-dispatch fused Pass 4 (kPass4FusedApplyGuard: 5 dispatches -> 1 via algebraic linearity
//                  and 22x22 LDS halo; eliminates inOwnGrainTex_, candOutScratch, guardFineH/V; Shadow skips out_y store)
//   - Speedup (5): Reused 1x warpedVal (R32F) + packed 3-ref x 3-bit flags in R16 UNORM (2 B/px read; drops unconditional
//                  cur_clean fetch on valid interior pixels)
//   - Speedup (6): 20x20 LDS-tiled bicubic warp (kPass1WarpAndGate: constant cell MV per 16x16 WG -> zero global 4x4 fetches)
//   - Speedup (7): Per-frame dynamic parameters moved from #define to VpvGpu::Var float UBOs (zero mid-playback recompiles)
//                  + empty-block DCT fetch skip in kPass3bDctPocs + zero-copy seed copy pass (kPassSeedCopy)
//
// Every shared-memory barrier is strictly workgroup-uniform and paired with memoryBarrierShared() (GLSL 450 s8.16).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>

#include <cmath>
#include <string>
#include <vector>

namespace TdV2
{
namespace Gpu
{
namespace Shaders
{
	// Shared GLSL helpers: IEEE-754 finite check, cubic Hermite ramp, Catmull-Rom weights (matching VpComposed::CubicWeights),
	// and exact 9-bit (3 refs x 3 bits) flag pack/unpack over 16-bit UNORM (65535.0 exact integer round-trip).
	constexpr const char* kCoreHelpers = R"GLSL(
bool td_is_bad(float v)
{
    // Exact IEEE-754 FP32 exponent check (exponent == 0xFF <=> NaN or +/-Inf), immune to D3D11 FXC fast-math isnan folding.
    uint bits = floatBitsToUint(v);
    return (bits & 0x7F800000u) == 0x7F800000u;
}

float td_hermite_ramp(float e0, float e1, float x)
{
    float d = e1 - e0;
    float t = (d > 1e-12) ? clamp((x - e0) / d, 0.0, 1.0) : (x >= e1 ? 1.0 : 0.0);
    return t * t * (3.0 - 2.0 * t);
}

vec4 td_cubic_weights(float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4(
        -0.5 * t3 + t2 - 0.5 * t,
         1.5 * t3 - 2.5 * t2 + 1.0,
        -1.5 * t3 + 2.0 * t2 + 0.5 * t,
         0.5 * t3 - 0.5 * t2);
}

uint td_pack_ref_flags(uint prevPacked, int refIdx, int flags3)
{
    uint sh = uint(3 * refIdx);
    uint cleared = prevPacked & (~(7u << sh));
    return cleared | ((uint(flags3) & 7u) << sh);
}

int td_unpack_ref_flags(uint packed, int refIdx)
{
    return int((packed >> uint(3 * refIdx)) & 7u);
}
)GLSL";

	// Shared half-res (L, D3) bilinear sampler clamped strictly inside halfActive (zero bar bleed, no PL_FMT_CAP_LINEAR needed).
	constexpr const char* kHalfBandSamplerHelper = R"GLSL(
vec2 td_sample_half_ld3(sampler2D halfTex, ivec2 p, ivec4 halfActive)
{
    vec2 hc = (vec2(p) - vec2(0.5)) / 2.0;
    vec2 lo = vec2(halfActive.xy);
    vec2 hi = vec2(max(halfActive.xy, halfActive.zw - ivec2(1)));
    vec2 cl = clamp(hc, lo, hi);
    ivec2 i0 = ivec2(floor(cl));
    ivec2 i1 = min(ivec2(hi), i0 + ivec2(1));
    vec2 f = cl - vec2(i0);
    vec2 v00 = texelFetch(halfTex, ivec2(i0.x, i0.y), 0).xy;
    vec2 v10 = texelFetch(halfTex, ivec2(i1.x, i0.y), 0).xy;
    vec2 v01 = texelFetch(halfTex, ivec2(i0.x, i1.y), 0).xy;
    vec2 v11 = texelFetch(halfTex, ivec2(i1.x, i1.y), 0).xy;
    vec2 top = v00 + (v10 - v00) * f.x;
    vec2 bot = v01 + (v11 - v01) * f.x;
    return top * (1.0 - f.y) + bot * f.y;
}
)GLSL";

	// Build exact CPU-supplied U2 VpUnit::BandFromBase tap arrays for full-res Reference L/M/H/D3 decomposition.
	inline std::string BuildBandTapsHeader()
	{
		const auto tFine = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
		const auto tBase = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaBase, 3.0, true, 0.0);
		const auto tLow  = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaLow,  3.0, true, 0.0);
		const int rFine = static_cast<int>(tFine.size() / 2);
		const int rBase = static_cast<int>(tBase.size() / 2);
		const int rLow  = static_cast<int>(tLow.size() / 2);

		std::string s;
		s += VpvGpu::DefineInt("TD_RAD_FINE", rFine);
		s += VpvGpu::DefineInt("TD_RAD_BASE", rBase);
		s += VpvGpu::DefineInt("TD_RAD_LOW",  rLow);
		s += VpvGpu::ConstArray("TD_TAP_FINE", static_cast<int>(tFine.size()),
			[&](int i) { return tFine[static_cast<size_t>(i)]; });
		s += VpvGpu::ConstArray("TD_TAP_BASE", static_cast<int>(tBase.size()),
			[&](int i) { return tBase[static_cast<size_t>(i)]; });
		s += VpvGpu::ConstArray("TD_TAP_LOW", static_cast<int>(tLow.size()),
			[&](int i) { return tLow[static_cast<size_t>(i)]; });
		return s;
	}

	// Build exact CPU-supplied U2 VpUnit::BandFromBase half-resolution tap arrays for compact RealTime (L, D3).
	inline std::string BuildHalfBandTapsHeader()
	{
		const double hsBase = VpUnit::BandFromBase::HalfSigma(VpUnit::BandFromBase::kSigmaBase);
		const double hsLow  = VpUnit::BandFromBase::HalfSigma(VpUnit::BandFromBase::kSigmaLow);
		const auto tHBase = VpUnit::BandFromBase::Taps<double>(hsBase, 3.0, true, 0.0);
		const auto tHLow  = VpUnit::BandFromBase::Taps<double>(hsLow,  3.0, true, 0.0);
		const int rHBase = static_cast<int>(tHBase.size() / 2);
		const int rHLow  = static_cast<int>(tHLow.size() / 2);

		std::string s;
		s += VpvGpu::DefineInt("TD_RAD_HBASE", rHBase);
		s += VpvGpu::DefineInt("TD_RAD_HLOW",  rHLow);
		s += VpvGpu::ConstArray("TD_TAP_HBASE", static_cast<int>(tHBase.size()),
			[&](int i) { return tHBase[static_cast<size_t>(i)]; });
		s += VpvGpu::ConstArray("TD_TAP_HLOW", static_cast<int>(tHLow.size()),
			[&](int i) { return tHLow[static_cast<size_t>(i)]; });
		return s;
	}

	// Build exact fine-band tap array only (for compact RealTime full-res (M, H) and Layer-4 TdGuardPort).
	inline std::string BuildFineTapHeader()
	{
		const auto tFine = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
		const int rFine = static_cast<int>(tFine.size() / 2);
		std::string s;
		s += VpvGpu::DefineInt("TD_RAD_FINE", rFine);
		s += VpvGpu::ConstArray("TD_TAP_FINE", static_cast<int>(tFine.size()),
			[&](int i) { return tFine[static_cast<size_t>(i)]; });
		return s;
	}

	// Build orthonormal 8x8 DCT-II basis matrix C[u * 8 + x] = c(u) * cos(pi * (2x + 1) * u / 16).
	inline std::string BuildDctBasisHeader()
	{
		const double invSqrt8 = 1.0 / std::sqrt(8.0);
		const double scaleAc = 0.5;
		return VpvGpu::ConstArray("TD_DCT8_BASIS", 64, [&](int idx) {
			const int u = idx / 8;
			const int x = idx % 8;
			const double cu = (u == 0) ? invSqrt8 : scaleAc;
			return cu * std::cos(kDctPi * (2.0 * static_cast<double>(x) + 1.0) * static_cast<double>(u) / 16.0);
		});
	}

	// ---- PASS 0: Zero-Copy GPU Seed Copy (for ProcessFrameGpu on cut / first frame with no refs) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: src_clean (R32F), src_rec (R32F; distinct dummy when u_same_clean_rec > 0.5).
	// Outputs: dst_carry (R32F), dst_clean (R32F), dst_out_y (R32F, written only when u_copy_out_y > 0.5).
	constexpr const char* kPassSeedCopy = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

float cVal = texelFetch(src_clean, p, 0).x;
imageStore(dst_carry, p, vec4(cVal, 0.0, 0.0, 0.0));
if (u_copy_clean > 0.5)
    imageStore(dst_clean, p, vec4(cVal, 0.0, 0.0, 0.0));
if (u_copy_out_y > 0.5)
{
    float rVal = (u_same_clean_rec > 0.5) ? cVal : texelFetch(src_rec, p, 0).x;
    imageStore(dst_out_y, p, vec4(rVal, 0.0, 0.0, 0.0));
}
)GLSL";

	// ---- PASS 1A: Camera-Relative Motion Gate + Karis 3x3 Silhouette Dilation ----
	// Dispatch: ceil((K * TD_CW) / 16) x ceil(TD_CH / 16), group 16x16.
	// Inputs: motion_vec (3*cw x ch RGBA32F: vx, vy, camVx, camVy), motion_meta (3*cw x ch RG32F: cls, valid).
	// Output: motion_gate (3*cw x ch RGBA32F: gateL, gateMH, nonFiniteCell, 0).
	constexpr const char* kPass1MotionGateHeader = R"GLSL(
bool td_cell_rel_speed(int rk, int cx, int cy, out float spd, out int clsOut, out bool wasNonFinite)
{
    wasNonFinite = false;
    vec4 mv = texelFetch(motion_vec, ivec2(rk * TD_CW + cx, cy), 0);
    vec2 mm = texelFetch(motion_meta, ivec2(rk * TD_CW + cx, cy), 0).xy;
    clsOut = int(round(mm.x));
    if (mm.y < 0.5)
    {
        spd = TD_FAST_CUTOFF * 2.0;
        return false;
    }
    if (td_is_bad(mv.x) || td_is_bad(mv.y) || td_is_bad(mv.z) || td_is_bad(mv.w))
    {
        spd = TD_FAST_CUTOFF * 2.0;
        wasNonFinite = true;
        return false;
    }
    vec2 d = mv.xy - mv.zw;
    spd = sqrt(dot(d, d));
    return true;
}
)GLSL";

	constexpr const char* kPass1MotionGate = R"GLSL(
ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
int K = int(round(u_k));
int totalW = K * TD_CW;
if (gid.x >= totalW || gid.y >= TD_CH)
    return;

int rk = gid.x / TD_CW;
int cx = gid.x % TD_CW;
int cy = gid.y;

float selfSpd = 0.0;
int selfCls = 0;
bool selfBad = false;
bool selfOk = td_cell_rel_speed(rk, cx, cy, selfSpd, selfCls, selfBad);
if (!selfOk)
{
    imageStore(motion_gate, gid, vec4(0.0, 0.0, selfBad ? 1.0 : 0.0, 0.0));
    return;
}

if (selfCls == TD_CLS_OVERLAY)
{
    imageStore(motion_gate, gid, vec4(0.0, 0.0, 0.0, 0.0));
    return;
}

float dilatedSpd = selfSpd;
for (int dy = -1; dy <= 1; ++dy)
{
    int ny = cy + dy;
    if (ny < 0 || ny >= TD_CH) continue;
    for (int dx = -1; dx <= 1; ++dx)
    {
        int nx = cx + dx;
        if (nx < 0 || nx >= TD_CW) continue;
        float nSpd = 0.0;
        int nCls = 0;
        bool nBad = false;
        if (td_cell_rel_speed(rk, nx, ny, nSpd, nCls, nBad) && nCls != TD_CLS_OVERLAY)
        {
            dilatedSpd = max(dilatedSpd, nSpd);
        }
    }
}

float speedGate = 1.0 - td_hermite_ramp(TD_STILL_SPEED, TD_FAST_CUTOFF, dilatedSpd);
float gateL = (selfCls == TD_CLS_OBJECT) ? speedGate : 1.0;
float gateMH = (selfCls == TD_CLS_DYN_TEX) ? 0.0 : speedGate;
imageStore(motion_gate, gid, vec4(gateL, gateMH, 0.0, 0.0));
)GLSL";

	// ---- PASS 1B: 20x20 LDS-Tiled Active-Rect Safe Bicubic Warp + Packed 3-Ref Flags (Speedups 5 & 6) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: ref_clean (w x h R32F), cur_clean (w x h R32F), motion_vec (3*cw x ch RGBA32F), ref_flags_in (w x h R16).
	// Outputs: warped_val (w x h R32F), ref_flags_out (w x h R16, bits [3*k .. 3*k+2]: 1=valid, 2=borderBilin, 4=nonFinite).
	// Note: cur_clean is fetched ONLY on slow paths (bar pixel, non-finite MV, or non-finite ref tap).
	constexpr const char* kPass1WarpHeader = R"GLSL(
shared float td_sh_w[20][20];
shared uint td_sh_bad[20][20];

float td_fetch_cur_fallback(ivec2 gq)
{
    return (u_same_ref_cur > 0.5) ? texelFetch(ref_clean, gq, 0).x : texelFetch(cur_clean, gq, 0).x;
}

float td_fetch_active_safe(ivec2 qLocal, ivec4 activeRect, out bool isBad)
{
    ivec2 sz = activeRect.zw - activeRect.xy;
    ivec2 gq = activeRect.xy + clamp(qLocal, ivec2(0), sz - ivec2(1));
    float v = texelFetch(ref_clean, gq, 0).x;
    if (td_is_bad(v))
    {
        isBad = true;
        float fb = td_fetch_cur_fallback(gq);
        return td_is_bad(fb) ? 0.0 : fb;
    }
    isBad = false;
    return v;
}

float td_sample_bilinear_active(vec2 sLocal, ivec4 activeRect, out bool anyBad)
{
    ivec2 sz = activeRect.zw - activeRect.xy;
    vec2 cl = clamp(sLocal, vec2(0.0), vec2(float(sz.x - 1), float(sz.y - 1)));
    ivec2 i0 = ivec2(floor(cl));
    ivec2 i1 = min(sz - ivec2(1), i0 + ivec2(1));
    vec2 f = cl - vec2(i0);
    bool b00 = false, b10 = false, b01 = false, b11 = false;
    float v00 = td_fetch_active_safe(ivec2(i0.x, i0.y), activeRect, b00);
    float v10 = td_fetch_active_safe(ivec2(i1.x, i0.y), activeRect, b10);
    float v01 = td_fetch_active_safe(ivec2(i0.x, i1.y), activeRect, b01);
    float v11 = td_fetch_active_safe(ivec2(i1.x, i1.y), activeRect, b11);
    anyBad = b00 || b10 || b01 || b11;
    float top = v00 + (v10 - v00) * f.x;
    float bot = v01 + (v11 - v01) * f.x;
    return top * (1.0 - f.y) + bot * f.y;
}

void td_sample_bicubic_active_direct(vec2 sLocal, ivec4 activeRect, ivec2 p, out float outVal, out int outFlags)
{
    ivec2 sz = activeRect.zw - activeRect.xy;
    outFlags = 0;
    if (sLocal.x >= 1.0 && sLocal.y >= 1.0 &&
        sLocal.x < float(sz.x) - 2.0 && sLocal.y < float(sz.y) - 2.0)
    {
        ivec2 i0 = ivec2(floor(sLocal));
        vec4 wx = td_cubic_weights(sLocal.x - float(i0.x));
        vec4 wy = td_cubic_weights(sLocal.y - float(i0.y));
        bool anyBad = false;
        float acc = 0.0;
        for (int j = 0; j < 4; ++j)
        {
            float row = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                bool badTap = false;
                float tap = td_fetch_active_safe(ivec2(i0.x - 1 + i, i0.y - 1 + j), activeRect, badTap);
                anyBad = anyBad || badTap;
                row += wx[i] * tap;
            }
            acc += wy[j] * row;
        }
        if (td_is_bad(acc))
        {
            outVal = td_fetch_cur_fallback(p);
            outFlags = 4;
        }
        else
        {
            outVal = acc;
            outFlags = anyBad ? 4 : 1;
        }
    }
    else if (sLocal.x >= 0.0 && sLocal.y >= 0.0 &&
             sLocal.x <= float(sz.x - 1) && sLocal.y <= float(sz.y - 1))
    {
        bool anyBad = false;
        float bVal = td_sample_bilinear_active(sLocal, activeRect, anyBad);
        if (td_is_bad(bVal))
        {
            outVal = td_fetch_cur_fallback(p);
            outFlags = 2 | 4;
        }
        else
        {
            outVal = bVal;
            outFlags = 2 | (anyBad ? 4 : 1);
        }
    }
    else
    {
        bool anyBad = false;
        float bVal = td_sample_bilinear_active(sLocal, activeRect, anyBad);
        outVal = td_is_bad(bVal) ? td_fetch_cur_fallback(p) : bVal;
        outFlags = (anyBad || td_is_bad(bVal)) ? 4 : 0;
    }
}
)GLSL";

	constexpr const char* kPass1WarpAndGate = R"GLSL(
ivec2 wg = ivec2(gl_WorkGroupID.xy);
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
int baseX = wg.x * 16;
int baseY = wg.y * 16;
ivec2 p = ivec2(baseX + loc.x, baseY + loc.y);

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
ivec4 activeRect = ivec4(ax0, ay0, ax1, ay1);
bool validRect = (ax1 > ax0) && (ay1 > ay0);
int refIdx = int(round(u_ref_idx));
bool flagsClear = (u_flags_clear > 0.5);

// Workgroup-uniform 20x20 LDS tile fill for canonical TD_CELL_PX == 16 (constant cell MV across WG).
int wgCx = min(TD_CW - 1, wg.x);
int wgCy = min(TD_CH - 1, wg.y);
vec2 wgMv = texelFetch(motion_vec, ivec2(refIdx * TD_CW + wgCx, wgCy), 0).xy;
bool useLds = (TD_CELL_PX == 16) && validRect &&
              (!td_is_bad(wgMv.x)) && (!td_is_bad(wgMv.y)) &&
              (abs(wgMv.x) <= float(TD_W)) && (abs(wgMv.y) <= float(TD_H));
int tileBaseX = 0;
int tileBaseY = 0;
if (useLds)
{
    tileBaseX = int(floor(float(baseX - ax0) + wgMv.x)) - 1;
    tileBaseY = int(floor(float(baseY - ay0) + wgMv.y)) - 1;
    int tileMaxX = int(floor(float(baseX + 15 - ax0) + wgMv.x)) + 2;
    int tileMaxY = int(floor(float(baseY + 15 - ay0) + wgMv.y)) + 2;
    useLds = (tileMaxX - tileBaseX < 20) && (tileMaxY - tileBaseY < 20);
}
if (useLds)
{
    int tid = loc.y * 16 + loc.x;
    for (int idx = tid; idx < 400; idx += 256)
    {
        int ty = idx / 20;
        int tx = idx % 20;
        bool bad = false;
        td_sh_w[ty][tx] = td_fetch_active_safe(ivec2(tileBaseX + tx, tileBaseY + ty), activeRect, bad);
        td_sh_bad[ty][tx] = bad ? 1u : 0u;
    }
}
memoryBarrierShared();
barrier();

if (p.x >= TD_W || p.y >= TD_H)
    return;

uint prevPacked = flagsClear ? 0u : uint(round(texelFetch(ref_flags_in, p, 0).x * 65535.0));

if (!validRect || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    float fb = td_fetch_cur_fallback(p);
    uint np = td_pack_ref_flags(prevPacked, refIdx, 0);
    imageStore(warped_val, p, vec4(fb, 0.0, 0.0, 0.0));
    imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
    return;
}

vec2 mv = wgMv;
if (TD_CELL_PX != 16)
{
    int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
    int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
    mv = texelFetch(motion_vec, ivec2(refIdx * TD_CW + cx, cy), 0).xy;
}

// Detect non-finite pixels at `p` in ref_clean even if motion shifts away from `p` (reuse LDS when in tile).
int extraBad = 0;
int plx = p.x - ax0;
int ply = p.y - ay0;
if (useLds && plx >= tileBaseX && plx < tileBaseX + 20 && ply >= tileBaseY && ply < tileBaseY + 20)
{
    extraBad = (td_sh_bad[ply - tileBaseY][plx - tileBaseX] != 0u) ? 4 : 0;
}
else
{
    float selfRef = texelFetch(ref_clean, p, 0).x;
    extraBad = td_is_bad(selfRef) ? 4 : 0;
}

if (td_is_bad(mv.x) || td_is_bad(mv.y))
{
    float fb = td_fetch_cur_fallback(p);
    uint np = td_pack_ref_flags(prevPacked, refIdx, 4);
    imageStore(warped_val, p, vec4(fb, 0.0, 0.0, 0.0));
    imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
    return;
}

ivec2 sz = activeRect.zw - activeRect.xy;
vec2 sLocal = vec2(float(plx), float(ply)) + mv;
float outVal = 0.0;
int outFlags = 0;

if (useLds &&
    sLocal.x >= 1.0 && sLocal.y >= 1.0 &&
    sLocal.x < float(sz.x) - 2.0 && sLocal.y < float(sz.y) - 2.0)
{
    ivec2 i0 = ivec2(floor(sLocal));
    int rx = (i0.x - 1) - tileBaseX;
    int ry = (i0.y - 1) - tileBaseY;
    vec4 wx = td_cubic_weights(sLocal.x - float(i0.x));
    vec4 wy = td_cubic_weights(sLocal.y - float(i0.y));
    uint badOr = 0u;
    float acc = 0.0;
    for (int j = 0; j < 4; ++j)
    {
        float row = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            badOr |= td_sh_bad[ry + j][rx + i];
            row += wx[i] * td_sh_w[ry + j][rx + i];
        }
        acc += wy[j] * row;
    }
    if (td_is_bad(acc))
    {
        outVal = td_fetch_cur_fallback(p);
        outFlags = 4;
    }
    else
    {
        outVal = acc;
        outFlags = (badOr != 0u) ? 4 : 1;
    }
}
else if (useLds &&
         sLocal.x >= 0.0 && sLocal.y >= 0.0 &&
         sLocal.x <= float(sz.x - 1) && sLocal.y <= float(sz.y - 1))
{
    vec2 cl = clamp(sLocal, vec2(0.0), vec2(float(sz.x - 1), float(sz.y - 1)));
    ivec2 i0 = ivec2(floor(cl));
    ivec2 i1 = min(sz - ivec2(1), i0 + ivec2(1));
    int r0x = i0.x - tileBaseX;
    int r0y = i0.y - tileBaseY;
    int r1x = i1.x - tileBaseX;
    int r1y = i1.y - tileBaseY;
    vec2 f = cl - vec2(i0);
    float v00 = td_sh_w[r0y][r0x];
    float v10 = td_sh_w[r0y][r1x];
    float v01 = td_sh_w[r1y][r0x];
    float v11 = td_sh_w[r1y][r1x];
    uint badOr = td_sh_bad[r0y][r0x] | td_sh_bad[r0y][r1x] | td_sh_bad[r1y][r0x] | td_sh_bad[r1y][r1x];
    float top = v00 + (v10 - v00) * f.x;
    float bot = v01 + (v11 - v01) * f.x;
    float bVal = top * (1.0 - f.y) + bot * f.y;
    if (td_is_bad(bVal))
    {
        outVal = td_fetch_cur_fallback(p);
        outFlags = 2 | 4;
    }
    else
    {
        outVal = bVal;
        outFlags = 2 | ((badOr != 0u) ? 4 : 1);
    }
}
else
{
    td_sample_bicubic_active_direct(sLocal, activeRect, p, outVal, outFlags);
}

outFlags = outFlags | extraBad;
uint newPacked = td_pack_ref_flags(prevPacked, refIdx, outFlags);
imageStore(warped_val, p, vec4(outVal, 0.0, 0.0, 0.0));
imageStore(ref_flags_out, p, vec4(float(newPacked) / 65535.0, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 1C (Reference): LDS-Tiled Single-Loop Separable Blurs (Speedup 1 — Bit-Exact FP32) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// kPass1BandH: src_tex (.x) -> band_tmp_h (gFine_H, gBase_H, gLow_H, srcVal), 16x52 float LDS (3,328 B).
	// kPass1BandV: band_tmp_h   -> bands_out  (L, M, H, D3),                     52x16 vec4  LDS (13,312 B).
	constexpr const char* kPass1BandHHeader = R"GLSL(
shared float td_sh_h[16][16 + 2 * TD_RAD_LOW];
)GLSL";

	constexpr const char* kPass1BandH = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
ivec2 p = ivec2(baseX + lx, baseY + ly);

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
bool validRect = (ax1 > ax0) && (ay1 > ay0);
int xMin = ax0;
int xMax = max(ax0, ax1 - 1);
int safeY = clamp(baseY + ly, 0, TD_H - 1);

int tileW = 16 + 2 * TD_RAD_LOW;
for (int col = lx; col < tileW; col += 16)
{
    int gx = baseX - TD_RAD_LOW + col;
    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
    td_sh_h[ly][col] = texelFetch(src_tex, ivec2(sx, safeY), 0).x;
}
memoryBarrierShared();
barrier();

if (p.x >= TD_W || p.y >= TD_H)
    return;

if (!validRect || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(band_tmp_h, p, vec4(0.0));
    return;
}

int center = lx + TD_RAD_LOW;
float srcVal = td_sh_h[ly][center];
float accFine = 0.0;
float accBase = 0.0;
float accLow = 0.0;
for (int k = -TD_RAD_LOW; k <= TD_RAD_LOW; ++k)
{
    float v = td_sh_h[ly][center + k];
    accLow += TD_TAP_LOW[k + TD_RAD_LOW] * v;
    if (abs(k) <= TD_RAD_BASE)
        accBase += TD_TAP_BASE[k + TD_RAD_BASE] * v;
    if (abs(k) <= TD_RAD_FINE)
        accFine += TD_TAP_FINE[k + TD_RAD_FINE] * v;
}

imageStore(band_tmp_h, p, vec4(accFine, accBase, accLow, srcVal));
)GLSL";

	constexpr const char* kPass1BandVHeader = R"GLSL(
shared vec4 td_sh_v[16 + 2 * TD_RAD_LOW][16];
)GLSL";

	constexpr const char* kPass1BandV = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
ivec2 p = ivec2(baseX + lx, baseY + ly);

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
bool validRect = (ax1 > ax0) && (ay1 > ay0);
int yMin = ay0;
int yMax = max(ay0, ay1 - 1);
int safeX = clamp(baseX + lx, 0, TD_W - 1);

int tileH = 16 + 2 * TD_RAD_LOW;
for (int row = ly; row < tileH; row += 16)
{
    int gy = baseY - TD_RAD_LOW + row;
    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
    td_sh_v[row][lx] = texelFetch(band_tmp_h, ivec2(safeX, sy), 0);
}
memoryBarrierShared();
barrier();

if (p.x >= TD_W || p.y >= TD_H)
    return;

if (!validRect || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(bands_out, p, vec4(0.0));
    return;
}

int center = ly + TD_RAD_LOW;
float srcVal = td_sh_v[center][lx].w;
float gFine = 0.0;
float gBase = 0.0;
float gLow = 0.0;
for (int k = -TD_RAD_LOW; k <= TD_RAD_LOW; ++k)
{
    vec4 v = td_sh_v[center + k][lx];
    gLow += TD_TAP_LOW[k + TD_RAD_LOW] * v.z;
    if (abs(k) <= TD_RAD_BASE)
        gBase += TD_TAP_BASE[k + TD_RAD_BASE] * v.y;
    if (abs(k) <= TD_RAD_FINE)
        gFine += TD_TAP_FINE[k + TD_RAD_FINE] * v.x;
}

float bL  = gBase;
float bM  = gFine - bL;
float bH  = srcVal - gFine;
float bD3 = bL - gLow;
imageStore(bands_out, p, vec4(bL, bM, bH, bD3));
)GLSL";

	// ---- PASS 1C (RealTime Compact — Speedup 3): Half-Res FP32 (L, D3) + 1-Pass Fused 2D LDS Full-Res RG16F (M, H) ----
	// kPass1HalfH: reduces active-clamped 2x2 area into LDS + horizontal blur at half-res (wHalf x hHalf RG32F).
	constexpr const char* kPass1HalfHHeader = R"GLSL(
shared float td_sh_hh[16][16 + 2 * TD_RAD_HLOW];
)GLSL";

	constexpr const char* kPass1HalfH = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
ivec2 hp = ivec2(baseX + lx, baseY + ly);

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
int hax0 = int(round(u_hax0));
int hay0 = int(round(u_hay0));
int hax1 = int(round(u_hax1));
int hay1 = int(round(u_hay1));
bool validRect = (ax1 > ax0) && (ay1 > ay0) && (hax1 > hax0) && (hay1 > hay0);

int xMin = ax0;
int xMax = max(ax0, ax1 - 1);
int yMin = ay0;
int yMax = max(ay0, ay1 - 1);
int hxMin = hax0;
int hxMax = max(hax0, hax1 - 1);
int hyMin = hay0;
int hyMax = max(hay0, hay1 - 1);

int chy = validRect ? clamp(baseY + ly, hyMin, hyMax) : clamp(baseY + ly, 0, TD_HALF_H - 1);
int fy0 = validRect ? clamp(2 * chy,     yMin, yMax) : clamp(2 * chy,     0, TD_H - 1);
int fy1 = validRect ? clamp(2 * chy + 1, yMin, yMax) : clamp(2 * chy + 1, 0, TD_H - 1);

int tileW = 16 + 2 * TD_RAD_HLOW;
for (int col = lx; col < tileW; col += 16)
{
    int ghx = baseX - TD_RAD_HLOW + col;
    int chx = validRect ? clamp(ghx, hxMin, hxMax) : clamp(ghx, 0, TD_HALF_W - 1);
    int fx0 = validRect ? clamp(2 * chx,     xMin, xMax) : clamp(2 * chx,     0, TD_W - 1);
    int fx1 = validRect ? clamp(2 * chx + 1, xMin, xMax) : clamp(2 * chx + 1, 0, TD_W - 1);
    float s00 = texelFetch(src_tex, ivec2(fx0, fy0), 0).x;
    float s10 = texelFetch(src_tex, ivec2(fx1, fy0), 0).x;
    float s01 = texelFetch(src_tex, ivec2(fx0, fy1), 0).x;
    float s11 = texelFetch(src_tex, ivec2(fx1, fy1), 0).x;
    td_sh_hh[ly][col] = (s00 + s10 + s01 + s11) / 4.0;
}
memoryBarrierShared();
barrier();

if (hp.x >= TD_HALF_W || hp.y >= TD_HALF_H)
    return;

if (!validRect || hp.x < hax0 || hp.x >= hax1 || hp.y < hay0 || hp.y >= hay1)
{
    imageStore(band_half_tmp, hp, vec4(0.0));
    return;
}

int center = lx + TD_RAD_HLOW;
float accBase = 0.0;
float accLow = 0.0;
for (int k = -TD_RAD_HLOW; k <= TD_RAD_HLOW; ++k)
{
    float v = td_sh_hh[ly][center + k];
    accLow += TD_TAP_HLOW[k + TD_RAD_HLOW] * v;
    if (abs(k) <= TD_RAD_HBASE)
        accBase += TD_TAP_HBASE[k + TD_RAD_HBASE] * v;
}

imageStore(band_half_tmp, hp, vec4(accBase, accLow, 0.0, 0.0));
)GLSL";

	// kPass1HalfV: vertical half-res blur -> (L, D3) strictly in FP32 RG32F (wHalf x hHalf).
	constexpr const char* kPass1HalfVHeader = R"GLSL(
shared vec2 td_sh_hv[16 + 2 * TD_RAD_HLOW][16];
)GLSL";

	constexpr const char* kPass1HalfV = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
ivec2 hp = ivec2(baseX + lx, baseY + ly);

int hax0 = int(round(u_hax0));
int hay0 = int(round(u_hay0));
int hax1 = int(round(u_hax1));
int hay1 = int(round(u_hay1));
bool validHalf = (hax1 > hax0) && (hay1 > hay0);
int hyMin = hay0;
int hyMax = max(hay0, hay1 - 1);
int safeX = clamp(baseX + lx, 0, TD_HALF_W - 1);

int tileH = 16 + 2 * TD_RAD_HLOW;
for (int row = ly; row < tileH; row += 16)
{
    int ghy = baseY - TD_RAD_HLOW + row;
    int sy = validHalf ? clamp(ghy, hyMin, hyMax) : clamp(ghy, 0, TD_HALF_H - 1);
    td_sh_hv[row][lx] = texelFetch(band_half_tmp, ivec2(safeX, sy), 0).xy;
}
memoryBarrierShared();
barrier();

if (hp.x >= TD_HALF_W || hp.y >= TD_HALF_H)
    return;

if (!validHalf || hp.x < hax0 || hp.x >= hax1 || hp.y < hay0 || hp.y >= hay1)
{
    imageStore(bands_ld3_out, hp, vec4(0.0));
    return;
}

int center = ly + TD_RAD_HLOW;
float gBase = 0.0;
float gLow = 0.0;
for (int k = -TD_RAD_HLOW; k <= TD_RAD_HLOW; ++k)
{
    vec2 v = td_sh_hv[center + k][lx];
    gLow += TD_TAP_HLOW[k + TD_RAD_HLOW] * v.y;
    if (abs(k) <= TD_RAD_HBASE)
        gBase += TD_TAP_HBASE[k + TD_RAD_HBASE] * v.x;
}

imageStore(bands_ld3_out, hp, vec4(gBase, gBase - gLow, 0.0, 0.0));
)GLSL";

	// kPass1FullMH: 1-pass fused 2D LDS G_fine (22x22 halo, zero bandTmpH!) -> full-res RG16F zero-mean (M, H).
	constexpr const char* kPass1FullMHHeader = R"GLSL(
shared float td_sh_f0[16 + 2 * TD_RAD_FINE][16 + 2 * TD_RAD_FINE];
shared float td_sh_f1[16 + 2 * TD_RAD_FINE][16];
)GLSL";

	constexpr const char* kPass1FullMH = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;
ivec2 p = ivec2(baseX + lx, baseY + ly);

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
ivec4 halfActive = ivec4(int(round(u_hax0)), int(round(u_hay0)), int(round(u_hax1)), int(round(u_hay1)));
bool validRect = (ax1 > ax0) && (ay1 > ay0);
int xMin = ax0;
int xMax = max(ax0, ax1 - 1);
int yMin = ay0;
int yMax = max(ay0, ay1 - 1);

int tileDim = 16 + 2 * TD_RAD_FINE;
int tileN = tileDim * tileDim;
for (int idx = tid; idx < tileN; idx += 256)
{
    int ty = idx / tileDim;
    int tx = idx % tileDim;
    int gx = baseX - TD_RAD_FINE + tx;
    int gy = baseY - TD_RAD_FINE + ty;
    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
    td_sh_f0[ty][tx] = texelFetch(src_tex, ivec2(sx, sy), 0).x;
}
memoryBarrierShared();
barrier();

int hN = tileDim * 16;
for (int idx = tid; idx < hN; idx += 256)
{
    int ty = idx / 16;
    int tx = idx % 16;
    int c = tx + TD_RAD_FINE;
    float acc = 0.0;
    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
        acc += TD_TAP_FINE[k + TD_RAD_FINE] * td_sh_f0[ty][c + k];
    td_sh_f1[ty][tx] = acc;
}
memoryBarrierShared();
barrier();

if (p.x >= TD_W || p.y >= TD_H)
    return;

if (!validRect || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(bands_mh_out, p, vec4(0.0));
    return;
}

int cyCenter = ly + TD_RAD_FINE;
float gFine = 0.0;
for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
    gFine += TD_TAP_FINE[k + TD_RAD_FINE] * td_sh_f1[cyCenter + k][lx];

float srcVal = td_sh_f0[cyCenter][lx + TD_RAD_FINE];
float bL = td_sample_half_ld3(bands_ld3_in, p, halfActive).x;
float bM = gFine - bL;
float bH = srcVal - gFine;
imageStore(bands_mh_out, p, vec4(bM, bH, 0.0, 0.0));
)GLSL";

	// ---- PASS 2A (Speedup 2): Coalesced 16x16 LDS Cell-Moment Reduction + Fused Focus Guard ----
	// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per cell).
	// Reads curBands, refBands[0..K-1], packed ref_flags (R16), motion_gate, cell_prior ONCE per pixel.
	// Outputs: cell_focus (cw x ch RGBA32F: focusChange, isDefocused, spatialDetailRms, 0),
	//          warp_diag  (cw x ch RG32F: borderBilinCount, warpBadRefMask),
	//          cell_stats (3*cw x 3*ch RGBA32F: per band b, row0=(n,mu0,mu1,mu2), row1=(S00,S01,S02,S11), row2=(S12,S22,0,0)).
	constexpr const char* kPass2CellStatsHeader = R"GLSL(
shared vec4 td_sh_red4[256];
shared vec4 td_sh_red4b[256];
shared uint td_sh_mask[256];
shared float td_sh_mu[3][3];
shared float td_sh_nc;

vec4 td_fetch_cur_band(ivec2 p, ivec4 halfActive)
{
#if TD_COMPACT_BANDS
    vec2 mh = texelFetch(cur_bands_mh, p, 0).xy;
    vec2 ld3 = td_sample_half_ld3(cur_bands_ld3, p, halfActive);
    return vec4(ld3.x, mh.x, mh.y, ld3.y);
#else
    return texelFetch(cur_bands, p, 0);
#endif
}

vec4 td_fetch_ref_band(int k, ivec2 p, ivec4 halfActive)
{
#if TD_COMPACT_BANDS
    vec2 mh = vec2(0.0);
    vec2 ld3 = vec2(0.0);
    if (k == 0) { mh = texelFetch(ref_bands_mh_0, p, 0).xy; ld3 = td_sample_half_ld3(ref_bands_ld3_0, p, halfActive); }
    else if (k == 1) { mh = texelFetch(ref_bands_mh_1, p, 0).xy; ld3 = td_sample_half_ld3(ref_bands_ld3_1, p, halfActive); }
    else { mh = texelFetch(ref_bands_mh_2, p, 0).xy; ld3 = td_sample_half_ld3(ref_bands_ld3_2, p, halfActive); }
    return vec4(ld3.x, mh.x, mh.y, ld3.y);
#else
    if (k == 0) return texelFetch(ref_bands_0, p, 0);
    if (k == 1) return texelFetch(ref_bands_1, p, 0);
    return texelFetch(ref_bands_2, p, 0);
#endif
}

float td_band_comp(vec4 v, int b)
{
    if (b == 0) return v.x;
    if (b == 1) return v.y;
    return v.z;
}
)GLSL";

	constexpr const char* kPass2CellStats = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;

int K = int(round(u_k));
int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
ivec4 halfActive = ivec4(int(round(u_hax0)), int(round(u_hay0)), int(round(u_hax1)), int(round(u_hay1)));

int x0 = max(ax0, cell.x * TD_CELL_PX);
int y0 = max(ay0, cell.y * TD_CELL_PX);
int x1 = min(ax1, (cell.x + 1) * TD_CELL_PX);
int y1 = min(ay1, (cell.y + 1) * TD_CELL_PX);

bool cellGateL = true;
bool cellGateMH = true;
for (int k = 0; k < K; ++k)
{
    vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).xy;
    if (mg.x <= 0.0) cellGateL = false;
    if (mg.y <= 0.0) cellGateMH = false;
}

float curM2 = 0.0;
float curD3_2 = 0.0;
float curH2 = 0.0;
float activeCnt = 0.0;
float refM2[3];
float refD3_2[3];
refM2[0] = 0.0; refM2[1] = 0.0; refM2[2] = 0.0;
refD3_2[0] = 0.0; refD3_2[1] = 0.0; refD3_2[2] = 0.0;
float borderCnt = 0.0;
uint badMask = 0u;

// Per-band local sums for Step 2.1 (and single-pixel register cache for canonical TD_CELL_PX == 16).
vec3 sumD[3];
float cntB[3];
bool singleValid[3];
vec3 singleD[3];
for (int b = 0; b < 3; ++b)
{
    sumD[b] = vec3(0.0);
    cntB[b] = 0.0;
    singleValid[b] = false;
    singleD[b] = vec3(0.0);
}

const int subN = (TD_CELL_PX + 15) / 16;
for (int sy = 0; sy < subN; ++sy)
{
    int oy = sy * 16 + ly;
    if (oy >= TD_CELL_PX) continue;
    int py = cell.y * TD_CELL_PX + oy;
    if (py < y0 || py >= y1) continue;
    for (int sx = 0; sx < subN; ++sx)
    {
        int ox = sx * 16 + lx;
        if (ox >= TD_CELL_PX) continue;
        int px = cell.x * TD_CELL_PX + ox;
        if (px < x0 || px >= x1) continue;

        ivec2 p = ivec2(px, py);
        vec4 cb = td_fetch_cur_band(p, halfActive);
        curM2 += cb.y * cb.y;
        curD3_2 += cb.w * cb.w;
        curH2 += cb.z * cb.z;
        activeCnt += 1.0;

        uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
        bool allRefValid = true;
        vec4 rb[3];
        for (int k = 0; k < K; ++k)
        {
            rb[k] = td_fetch_ref_band(k, p, halfActive);
            refM2[k] += rb[k].y * rb[k].y;
            refD3_2[k] += rb[k].w * rb[k].w;
            int fl = td_unpack_ref_flags(packedFl, k);
            if ((fl & 1) == 0) allRefValid = false;
            if ((fl & 2) != 0) borderCnt += 1.0;
            if ((fl & 4) != 0) badMask |= (1u << uint(k));
        }

        if (allRefValid)
        {
            for (int b = 0; b < 3; ++b)
            {
                bool gOk = (b == 0) ? cellGateL : cellGateMH;
                if (!gOk) continue;
                float cv = td_band_comp(cb, b);
                vec3 dv = vec3(0.0);
                bool finiteOk = true;
                for (int k = 0; k < K; ++k)
                {
                    float d = cv - td_band_comp(rb[k], b);
                    if (td_is_bad(d))
                    {
                        finiteOk = false;
                        break;
                    }
                    if (k == 0) dv.x = d;
                    else if (k == 1) dv.y = d;
                    else dv.z = d;
                }
                if (finiteOk)
                {
                    sumD[b] += dv;
                    cntB[b] += 1.0;
                    if (TD_CELL_PX <= 16)
                    {
                        singleValid[b] = true;
                        singleD[b] = dv;
                    }
                }
            }
        }
    }
}

// Reduction A: curM2, curD3_2, curH2, activeCnt + ref0/1 M2/D3_2 + warpBadMask OR.
td_sh_red4[tid]  = vec4(curM2, curD3_2, curH2, activeCnt);
td_sh_red4b[tid] = vec4(refM2[0], refD3_2[0], refM2[1], refD3_2[1]);
td_sh_mask[tid]  = badMask;
memoryBarrierShared();
barrier();
for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_sh_red4[tid]  += td_sh_red4[tid + stride];
        td_sh_red4b[tid] += td_sh_red4b[tid + stride];
        td_sh_mask[tid]  |= td_sh_mask[tid + stride];
    }
    memoryBarrierShared();
    barrier();
}

vec4 totCur = td_sh_red4[0];
vec4 totRef01 = td_sh_red4b[0];
uint totBadMask = td_sh_mask[0];
memoryBarrierShared();
barrier();

// Reduction B: ref2 M2/D3_2 + borderBilinCnt.
td_sh_red4[tid] = vec4(refM2[2], refD3_2[2], borderCnt, 0.0);
memoryBarrierShared();
barrier();
for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
        td_sh_red4[tid] += td_sh_red4[tid + stride];
    memoryBarrierShared();
    barrier();
}

if (tid == 0)
{
    float totRefM2[3];
    float totRefD3[3];
    totRefM2[0] = totRef01.x; totRefD3[0] = totRef01.y;
    totRefM2[1] = totRef01.z; totRefD3[1] = totRef01.w;
    totRefM2[2] = td_sh_red4[0].x; totRefD3[2] = td_sh_red4[0].y;
    float totBorderCnt = td_sh_red4[0].z;

    float curEnergy = 0.0;
    float cM2 = 0.0;
    float curF = 1.0;
    float spatialDetailRms = 0.0;
    if (totCur.w > 0.5)
    {
        float invC = 1.0 / totCur.w;
        cM2 = totCur.x * invC;
        float cM3 = totCur.y * invC;
        curEnergy = cM2 + cM3;
        curF = (curEnergy > TD_NUM_FLOOR) ? (cM2 / (curEnergy + TD_NUM_FLOOR)) : 1.0;
        spatialDetailRms = sqrt(max(0.0, totCur.z * invC));
    }

    float cM3Val = max(0.0, curEnergy - cM2);
    float cleanM2 = max(0.0, cM2 - u_step_noise_var * TD_GAIN_MID);
    float cleanM3 = max(0.0, cM3Val - u_step_noise_var * TD_GAIN_D3);
    float curFDebiased = cleanM2 / (cleanM2 + cleanM3 + TD_NUM_FLOOR);

    bool isDefocused = (curEnergy > u_structure_floor && curFDebiased <= TD_F_DEFOCUS);
    bool focusChange = false;

    float explicitFc = texelFetch(cell_prior, cell, 0).y;
    if (explicitFc > 0.5)
    {
        focusChange = true;
    }
    else if (totCur.w > 0.5)
    {
        float invC = 1.0 / totCur.w;
        for (int k = 0; k < K; ++k)
        {
            float rM2 = totRefM2[k] * invC;
            float rM3 = totRefD3[k] * invC;
            float rEnergy = rM2 + rM3;
            float rF = (rEnergy > TD_NUM_FLOOR) ? (rM2 / (rEnergy + TD_NUM_FLOOR)) : 1.0;
            if (rEnergy > u_structure_floor && cM2 < rM2 && curF < TD_F_SHARP && (rF - curF) > TD_F_SHIFT_DELTA)
            {
                focusChange = true;
                break;
            }
        }
    }

    imageStore(cell_focus, cell, vec4(focusChange ? 1.0 : 0.0, isDefocused ? 1.0 : 0.0, spatialDetailRms, 0.0));
    imageStore(warp_diag, cell, vec4(totBorderCnt, float(totBadMask), 0.0, 0.0));
    td_sh_mask[0] = focusChange ? 1u : 0u;
}
memoryBarrierShared();
barrier();

// Vetoed focusChange cells are excluded from all 3x3 moment pools (no early return before barriers for FXC cs_5_0 safety).
bool skipMoments = (td_sh_mask[0] != 0u);

// Per-band 2-step exact centered sufficient stats (n_c, mu_c[3], S_c[6]).
for (int b = 0; b < 3; ++b)
{
    // Step 2.1: Reduce sumD[b] and cntB[b] (zero when focusChange vetoed).
    td_sh_red4[tid] = skipMoments ? vec4(0.0) : vec4(sumD[b], cntB[b]);
    memoryBarrierShared();
    barrier();
    for (int stride = 128; stride > 0; stride >>= 1)
    {
        if (tid < stride)
            td_sh_red4[tid] += td_sh_red4[tid + stride];
        memoryBarrierShared();
        barrier();
    }

    if (tid == 0)
    {
        float nc = skipMoments ? 0.0 : td_sh_red4[0].w;
        td_sh_nc = nc;
        float invN = (nc > 0.5) ? (1.0 / nc) : 0.0;
        td_sh_mu[b][0] = td_sh_red4[0].x * invN;
        td_sh_mu[b][1] = td_sh_red4[0].y * invN;
        td_sh_mu[b][2] = td_sh_red4[0].z * invN;
    }
    memoryBarrierShared();
    barrier();

    float ncCell = td_sh_nc;
    vec3 mu = vec3(td_sh_mu[b][0], td_sh_mu[b][1], td_sh_mu[b][2]);
    vec4 scA = vec4(0.0); // (S00, S01, S02, S11)
    vec4 scB = vec4(0.0); // (S12, S22, 0, 0)

    if (!skipMoments && ncCell > 0.5)
    {
        if (TD_CELL_PX <= 16)
        {
            if (singleValid[b])
            {
                vec3 e = singleD[b] - mu;
                scA = vec4(e.x * e.x, e.x * e.y, e.x * e.z, e.y * e.y);
                scB = vec4(e.y * e.z, e.z * e.z, 0.0, 0.0);
            }
        }
        else
        {
            bool gOk = (b == 0) ? cellGateL : cellGateMH;
            int totalSub = subN * subN;
            for (int subIdx = 0; subIdx < totalSub; ++subIdx)
            {
                int sy = subIdx / subN;
                int sx = subIdx % subN;
                int oy = sy * 16 + ly;
                int ox = sx * 16 + lx;
                int py = cell.y * TD_CELL_PX + oy;
                int px = cell.x * TD_CELL_PX + ox;
                if (gOk && oy < TD_CELL_PX && ox < TD_CELL_PX && py >= y0 && py < y1 && px >= x0 && px < x1)
                {
                    ivec2 p = ivec2(px, py);
                    uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
                    bool allOk = true;
                    float cv = td_band_comp(td_fetch_cur_band(p, halfActive), b);
                    vec3 dv = vec3(0.0);
                    if (K > 0)
                    {
                        if ((td_unpack_ref_flags(packedFl, 0) & 1) == 0) allOk = false;
                        else
                        {
                            float d0 = cv - td_band_comp(td_fetch_ref_band(0, p, halfActive), b);
                            if (td_is_bad(d0)) allOk = false;
                            else dv.x = d0;
                        }
                    }
                    if (allOk && K > 1)
                    {
                        if ((td_unpack_ref_flags(packedFl, 1) & 1) == 0) allOk = false;
                        else
                        {
                            float d1 = cv - td_band_comp(td_fetch_ref_band(1, p, halfActive), b);
                            if (td_is_bad(d1)) allOk = false;
                            else dv.y = d1;
                        }
                    }
                    if (allOk && K > 2)
                    {
                        if ((td_unpack_ref_flags(packedFl, 2) & 1) == 0) allOk = false;
                        else
                        {
                            float d2 = cv - td_band_comp(td_fetch_ref_band(2, p, halfActive), b);
                            if (td_is_bad(d2)) allOk = false;
                            else dv.z = d2;
                        }
                    }
                    if (allOk)
                    {
                        vec3 e = dv - mu;
                        scA += vec4(e.x * e.x, e.x * e.y, e.x * e.z, e.y * e.y);
                        scB += vec4(e.y * e.z, e.z * e.z, 0.0, 0.0);
                    }
                }
            }
        }
    }

    // Step 2.2: Reduce centered scatter S_c[0..5] unconditionally (all threads hit every barrier).
    td_sh_red4[tid] = scA;
    td_sh_red4b[tid] = scB;
    memoryBarrierShared();
    barrier();
    for (int stride = 128; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            td_sh_red4[tid]  += td_sh_red4[tid + stride];
            td_sh_red4b[tid] += td_sh_red4b[tid + stride];
        }
        memoryBarrierShared();
        barrier();
    }

    if (tid == 0)
    {
        if (skipMoments || ncCell <= 0.5)
        {
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, cell.y), vec4(0.0));
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, TD_CH + cell.y), vec4(0.0));
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, 2 * TD_CH + cell.y), vec4(0.0));
        }
        else
        {
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, cell.y), vec4(ncCell, mu.x, mu.y, mu.z));
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, TD_CH + cell.y), td_sh_red4[0]);
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, 2 * TD_CH + cell.y), vec4(td_sh_red4b[0].xy, 0.0, 0.0));
        }
    }
    memoryBarrierShared();
    barrier();
}
)GLSL";

	// ---- PASS 2B (Speedup 2): Exact Chan-Pebay 3x3 Moment Merge + Blind A1/A2/A3 + KxK Solve + TdMaps ----
	// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8.
	// Inputs: cell_stats, cell_focus, motion_gate, cell_prior.
	// Outputs: cell_weights (3*cw x ch RGBA32F), cell_diag_c (3*cw x ch RGBA32F),
	//          td_maps (cw x 3*ch RGBA32F), cell_diag_flags (cw x ch RGBA32F).
	constexpr const char* kPass2MergeSolveHeader = R"GLSL(
float td_band_gain(int b)
{
    if (b == 0) return TD_GAIN_LOW;
    if (b == 1) return TD_GAIN_MID;
    return TD_GAIN_FINE;
}

bool td_is_exact_copy(int k)
{
    if (k == 0) return u_exact_copy_0 > 0.5;
    if (k == 1) return u_exact_copy_1 > 0.5;
    return u_exact_copy_2 > 0.5;
}

float td_est_c_a1(float crossPN, float cPP, float cNN, float retGVar, float bandShare)
{
    if (td_is_bad(crossPN) || td_is_bad(cPP) || td_is_bad(cNN) || td_is_bad(retGVar) || td_is_bad(bandShare))
        return 0.0;
    float upper = max(0.0, min(cPP, cNN));
    float gTerm = max(0.0, retGVar) * max(0.0, bandShare);
    float raw = crossPN - gTerm;
    if (raw <= 0.0) return 0.0;
    return min(raw, upper);
}

float td_select_blind_c(int estMode, float cA1, float cA2)
{
    if (estMode == 0)
        return (!td_is_bad(cA1) && cA1 > 0.0) ? cA1 : 0.0;
    if (estMode == 1)
        return (!td_is_bad(cA2) && cA2 > 0.0) ? cA2 : 0.0;
    if (td_is_bad(cA1) || td_is_bad(cA2))
        return 0.0;
    return max(0.0, min(cA1, cA2));
}

// Returns 0 = ok, 1 = singular, 2 = non-finite.
int td_solve_convex(int K, float C[3][3], float cVec[3], out float outW[3])
{
    outW[0] = 0.0; outW[1] = 0.0; outW[2] = 0.0;
    float diagSum = 0.0;
    for (int i = 0; i < K; ++i)
    {
        if (td_is_bad(cVec[i]))
            return 2;
        for (int j = 0; j < K; ++j)
        {
            if (td_is_bad(C[i][j]))
                return 2;
        }
        diagSum += max(0.0, C[i][i]);
    }
    if (diagSum <= TD_NUM_FLOOR)
        return 0;

    float meanDiag = diagSum / float(K);
    float reg = max(TD_TRACE_REG * meanDiag, TD_NUM_FLOOR);

    float a[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            a[i][j] = 0.0;

    for (int i = 0; i < K; ++i)
    {
        for (int j = 0; j < K; ++j)
            a[i][j] = C[i][j] + ((i == j) ? reg : 0.0);
        a[i][K] = cVec[i];
    }

    for (int col = 0; col < K; ++col)
    {
        int pivotRow = col;
        float pivotAbs = abs(a[col][col]);
        for (int r = col + 1; r < K; ++r)
        {
            float cand = abs(a[r][col]);
            if (cand > pivotAbs)
            {
                pivotAbs = cand;
                pivotRow = r;
            }
        }
        if (pivotAbs <= TD_NUM_FLOOR)
            return 1;
        if (pivotRow != col)
        {
            for (int j = col; j <= K; ++j)
            {
                float tmp = a[col][j];
                a[col][j] = a[pivotRow][j];
                a[pivotRow][j] = tmp;
            }
        }
        float invPivot = 1.0 / a[col][col];
        for (int r = col + 1; r < K; ++r)
        {
            float factor = a[r][col] * invPivot;
            for (int j = col; j <= K; ++j)
                a[r][j] -= factor * a[col][j];
        }
    }

    float w[3];
    w[0] = 0.0; w[1] = 0.0; w[2] = 0.0;
    for (int i = K - 1; i >= 0; --i)
    {
        float rhs = a[i][K];
        for (int j = i + 1; j < K; ++j)
            rhs -= a[i][j] * w[j];
        w[i] = rhs / a[i][i];
        if (td_is_bad(w[i]))
            return 2;
    }

    float sumW = 0.0;
    for (int i = 0; i < K; ++i)
    {
        outW[i] = max(0.0, w[i]);
        sumW += outW[i];
    }
    if (sumW > 1.0)
    {
        float invSum = 1.0 / sumW;
        for (int i = 0; i < K; ++i)
            outW[i] *= invSum;
    }
    return 0;
}
)GLSL";

	constexpr const char* kPass2MergeAndSolve = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int K = int(round(u_k));
int poolR = int(round(u_pool_r));
int estMode = int(round(u_estimator));
bool bokehLOnly = (u_bokeh_l_only > 0.5);

vec4 fInfo = texelFetch(cell_focus, cell, 0);
bool focusChange = (fInfo.x > 0.5);
bool isDefocused = (fInfo.y > 0.5);
float spatialDetailRms = fInfo.z;

float diagNonFinite = 0.0;
float diagSingular = 0.0;
float diagSingleRefA2 = 0.0;

for (int k = 0; k < K; ++k)
{
    diagNonFinite += texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).z;
}

float rawGv = texelFetch(cell_prior, cell, 0).x;
float cellRetGVar = 0.0;
if (td_is_bad(rawGv) || rawGv < 0.0)
{
    diagNonFinite += 1.0;
}
else
{
    cellRetGVar = rawGv;
}

float cellWeightSumAll = 0.0;
float midWeightSum = 0.0;
float cellResEnergy = 0.0;
int activeBandBits = 0;

for (int b = 0; b < 3; ++b)
{
    float nPool = 0.0;
    vec3 muPool = vec3(0.0);
    float s00 = 0.0, s01 = 0.0, s02 = 0.0, s11 = 0.0, s12 = 0.0, s22 = 0.0;

    for (int dy = -poolR; dy <= poolR; ++dy)
    {
        int ncy = cell.y + dy;
        if (ncy < 0 || ncy >= TD_CH) continue;
        for (int dx = -poolR; dx <= poolR; ++dx)
        {
            int ncx = cell.x + dx;
            if (ncx < 0 || ncx >= TD_CW) continue;
            vec4 t0 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, ncy), 0);
            float nc = t0.x;
            if (nc <= 0.5) continue;
            vec3 muc = t0.yzw;
            vec4 t1 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
            vec2 t2 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0).xy;

            // Exact Chan-Pebay parallel second-moment merge (zero catastrophic cancellation).
            if (nPool <= 0.5)
            {
                nPool = nc;
                muPool = muc;
                s00 = t1.x; s01 = t1.y; s02 = t1.z; s11 = t1.w;
                s12 = t2.x; s22 = t2.y;
            }
            else
            {
                float nNew = nPool + nc;
                vec3 d = muc - muPool;
                float w = (nPool * nc) / nNew;
                muPool += d * (nc / nNew);
                s00 += t1.x + d.x * d.x * w;
                s01 += t1.y + d.x * d.y * w;
                s02 += t1.z + d.x * d.z * w;
                s11 += t1.w + d.y * d.y * w;
                s12 += t2.x + d.y * d.z * w;
                s22 += t2.y + d.z * d.z * w;
                nPool = nNew;
            }
        }
    }

    vec4 wOut = vec4(0.0);
    vec4 diagOut = vec4(0.0);

    if (nPool > 0.5)
    {
        float invN = 1.0 / nPool;
        float C[3][3];
        C[0][0] = max(0.0, s00 * invN + muPool.x * muPool.x);
        C[0][1] = s01 * invN + muPool.x * muPool.y;
        C[0][2] = s02 * invN + muPool.x * muPool.z;
        C[1][0] = C[0][1];
        C[1][1] = max(0.0, s11 * invN + muPool.y * muPool.y);
        C[1][2] = s12 * invN + muPool.y * muPool.z;
        C[2][0] = C[0][2];
        C[2][1] = C[1][2];
        C[2][2] = max(0.0, s22 * invN + muPool.z * muPool.z);

        diagOut.x = (K > 0) ? C[0][0] : 0.0;
        diagOut.y = (K > 1) ? C[1][1] : 0.0;
        diagOut.z = (K > 2) ? C[2][2] : 0.0;

        float bGain = td_band_gain(b);
        float cVec[3];
        cVec[0] = 0.0; cVec[1] = 0.0; cVec[2] = 0.0;
        for (int k = 0; k < K; ++k)
        {
            bool exactCopy = td_is_exact_copy(k);
            float cA2 = (u_a2_valid > 0.5)
                ? ((u_a2_sigma_sent + (exactCopy ? 0.0 : u_a2_sigma_zero)) * bGain)
                : 0.0;
            float cA1 = cA2;
            if (K >= 2)
            {
                int partner = (k == 0 || k == 2) ? 1 : 0;
                cA1 = td_est_c_a1(C[k][partner], C[k][k], C[partner][partner], cellRetGVar, bGain);
            }
            else if (estMode != 1)
            {
                diagSingleRefA2 += 1.0;
            }
            cVec[k] = exactCopy ? 0.0 : td_select_blind_c(estMode, cA1, cA2);
        }

        float solvedW[3];
        int status = td_solve_convex(K, C, cVec, solvedW);
        if (status == 2)
        {
            diagNonFinite += 1.0;
        }
        else if (status == 1)
        {
            diagSingular += 1.0;
        }
        else
        {
            float focusGate = 1.0;
            if (focusChange)
                focusGate = 0.0;
            else if (isDefocused)
                focusGate = (b == 0 && bokehLOnly) ? 1.0 : 0.0;

            float bandWSum = 0.0;
            float wFinal[3];
            wFinal[0] = 0.0; wFinal[1] = 0.0; wFinal[2] = 0.0;
            for (int k = 0; k < K; ++k)
            {
                vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).xy;
                float mGate = (b == 0) ? mg.x : mg.y;
                wFinal[k] = solvedW[k] * mGate * focusGate;
                bandWSum += wFinal[k];
            }
            wOut = vec4(wFinal[0], wFinal[1], wFinal[2], 0.0);
            if (bandWSum > TD_NUM_FLOOR)
                activeBandBits |= (1 << b);
            cellWeightSumAll += bandWSum;
            if (b == 1)
                midWeightSum = bandWSum;
            cellResEnergy += C[0][0];
        }
    }

    imageStore(cell_weights, ivec2(b * TD_CW + cell.x, cell.y), wOut);
    imageStore(cell_diag_c, ivec2(b * TD_CW + cell.x, cell.y), diagOut);
}

// Populate U19 TdMapsView 12 channels across 3 RGBA32F slices (plane p at y = p * TD_CH + cell.y).
vec4 slice0 = vec4(
    (activeBandBits != 0) ? 1.0 : 0.0,
    clamp(cellWeightSumAll / 3.0, 0.0, 1.0),
    clamp(midWeightSum, 0.0, 1.0),
    cellResEnergy / 3.0);
vec4 slice1 = vec4(
    spatialDetailRms,
    clamp(cellWeightSumAll / 3.0, 0.0, 1.0),
    sqrt(max(0.0, cellRetGVar)),
    sqrt(max(0.0, u_a2_total_var)));
vec4 slice2 = vec4(1.0, 0.0, 0.0, float(activeBandBits));

imageStore(td_maps, ivec2(cell.x, cell.y), slice0);
imageStore(td_maps, ivec2(cell.x, TD_CH + cell.y), slice1);
imageStore(td_maps, ivec2(cell.x, 2 * TD_CH + cell.y), slice2);
imageStore(cell_diag_flags, cell, vec4(focusChange ? 1.0 : 0.0, diagNonFinite, diagSingular, diagSingleRefA2));
)GLSL";

	// ---- PASS 3A: Spatial-Only Weight Interpolation + 3-Sigma Huber Taper + Pixel-Domain Bound ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: curBands, refBands[0..2], packed ref_flags (R16), cell_weights, cell_diag_c.
	// Output: delta_out (w x h RG32F: .x = delta, .y = 1.0 when pixel-domain bound engaged).
	constexpr const char* kPass3BlendHeader = R"GLSL(
vec4 td_fetch_cur_band(ivec2 p, ivec4 halfActive)
{
#if TD_COMPACT_BANDS
    vec2 mh = texelFetch(cur_bands_mh, p, 0).xy;
    vec2 ld3 = td_sample_half_ld3(cur_bands_ld3, p, halfActive);
    return vec4(ld3.x, mh.x, mh.y, ld3.y);
#else
    return texelFetch(cur_bands, p, 0);
#endif
}

vec4 td_fetch_ref_band(int k, ivec2 p, ivec4 halfActive)
{
#if TD_COMPACT_BANDS
    vec2 mh = vec2(0.0);
    vec2 ld3 = vec2(0.0);
    if (k == 0) { mh = texelFetch(ref_bands_mh_0, p, 0).xy; ld3 = td_sample_half_ld3(ref_bands_ld3_0, p, halfActive); }
    else if (k == 1) { mh = texelFetch(ref_bands_mh_1, p, 0).xy; ld3 = td_sample_half_ld3(ref_bands_ld3_1, p, halfActive); }
    else { mh = texelFetch(ref_bands_mh_2, p, 0).xy; ld3 = td_sample_half_ld3(ref_bands_ld3_2, p, halfActive); }
    return vec4(ld3.x, mh.x, mh.y, ld3.y);
#else
    if (k == 0) return texelFetch(ref_bands_0, p, 0);
    if (k == 1) return texelFetch(ref_bands_1, p, 0);
    return texelFetch(ref_bands_2, p, 0);
#endif
}

float td_band_comp(vec4 v, int b)
{
    if (b == 0) return v.x;
    if (b == 1) return v.y;
    return v.z;
}

float td_band_gain(int b)
{
    if (b == 0) return TD_GAIN_LOW;
    if (b == 1) return TD_GAIN_MID;
    return TD_GAIN_FINE;
}

float td_vec_comp(vec4 v, int k)
{
    if (k == 0) return v.x;
    if (k == 1) return v.y;
    return v.z;
}

float td_huber_taper(float diffSq, float diagC)
{
    float denom = max(diagC, TD_NUM_FLOOR);
    float r = diffSq / denom;
    return (r <= TD_HUBER_KNEE_SQ) ? 1.0 : (TD_HUBER_KNEE_SQ / r);
}
)GLSL";

	constexpr const char* kPass3BlendAndBound = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
if (p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(delta_out, p, vec4(0.0));
    return;
}

int K = int(round(u_k));
int bx0 = int(round(u_bx0));
int by0 = int(round(u_by0));
int bx1 = int(round(u_bx1));
int by1 = int(round(u_by1));
ivec4 halfActive = ivec4(int(round(u_hax0)), int(round(u_hay0)), int(round(u_hax1)), int(round(u_hay1)));

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);

bool inFull8x8 = (u_xform_8x8 > 0.5) && (bx0 < bx1) && (by0 < by1) &&
                 (p.x >= bx0) && (p.x < bx1) && (p.y >= by0) && (p.y < by1);

float gx = (float(p.x) + 0.5) / float(TD_CELL_PX) - 0.5;
float gy = (float(p.y) + 0.5) / float(TD_CELL_PX) - 0.5;
int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
int x1 = min(TD_CW - 1, x0 + 1);
int y1 = min(TD_CH - 1, y0 + 1);
float tx = clamp(gx - float(x0), 0.0, 1.0);
float ty = clamp(gy - float(y0), 0.0, 1.0);

vec4 curB = td_fetch_cur_band(p, halfActive);
uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
vec4 refB[3];
int refFl[3];
for (int k = 0; k < K; ++k)
{
    refB[k] = td_fetch_ref_band(k, p, halfActive);
    refFl[k] = td_unpack_ref_flags(packedFl, k);
}

float totalDelta = 0.0;
float boundEngaged = 0.0;
for (int b = 0; b < 3; ++b)
{
    float curV = td_band_comp(curB, b);
    vec4 ownW = texelFetch(cell_weights, ivec2(b * TD_CW + cx, cy), 0);
    vec4 w00 = texelFetch(cell_weights, ivec2(b * TD_CW + x0, y0), 0);
    vec4 w10 = texelFetch(cell_weights, ivec2(b * TD_CW + x1, y0), 0);
    vec4 w01 = texelFetch(cell_weights, ivec2(b * TD_CW + x0, y1), 0);
    vec4 w11 = texelFetch(cell_weights, ivec2(b * TD_CW + x1, y1), 0);
    vec4 d00 = texelFetch(cell_diag_c, ivec2(b * TD_CW + x0, y0), 0);
    vec4 d10 = texelFetch(cell_diag_c, ivec2(b * TD_CW + x1, y0), 0);
    vec4 d01 = texelFetch(cell_diag_c, ivec2(b * TD_CW + x0, y1), 0);
    vec4 d11 = texelFetch(cell_diag_c, ivec2(b * TD_CW + x1, y1), 0);

    float bandDelta = 0.0;
    for (int k = 0; k < K; ++k)
    {
        if ((refFl[k] & 1) == 0)
            continue;
        // Hard veto boundary guard: zero bleed across vetoed cell boundaries.
        if (td_vec_comp(ownW, k) <= 0.0)
            continue;

        float diff = curV - td_band_comp(refB[k], b);
        if (td_is_bad(diff))
            continue;

        float c00 = td_vec_comp(w00, k);
        float c10 = td_vec_comp(w10, k);
        float c01 = td_vec_comp(w01, k);
        float c11 = td_vec_comp(w11, k);
        float q00 = td_vec_comp(d00, k);
        float q10 = td_vec_comp(d10, k);
        float q01 = td_vec_comp(d01, k);
        float q11 = td_vec_comp(d11, k);
        if (td_is_bad(c00) || td_is_bad(c10) || td_is_bad(c01) || td_is_bad(c11) ||
            td_is_bad(q00) || td_is_bad(q10) || td_is_bad(q01) || td_is_bad(q11))
            continue;

        // Per-corner 3-sigma Huber taper before bilinear combine (exact twin of CPU SpatialBilinearHuberWeight).
        float diffSq = diff * diff;
        float t00 = c00 * td_huber_taper(diffSq, q00);
        float t10 = c10 * td_huber_taper(diffSq, q10);
        float t01 = c01 * td_huber_taper(diffSq, q01);
        float t11 = c11 * td_huber_taper(diffSq, q11);
        float top = t00 + (t10 - t00) * tx;
        float bot = t01 + (t11 - t01) * tx;
        float wEff = top + (bot - top) * ty;
        if (wEff <= 0.0)
            continue;
        bandDelta -= wEff * diff;
    }

    if (!inFull8x8)
    {
        float bandVar = (u_a2_valid > 0.5) ? (u_a2_total_var * td_band_gain(b)) : 0.0;
        if (abs(bandDelta) > 0.0)
        {
            boundEngaged = 1.0;
            float bound = TD_PIXEL_BOUND_SIGMA * sqrt(max(0.0, bandVar));
            bandDelta = clamp(bandDelta, -bound, bound);
        }
    }
    totalDelta += bandDelta;
}

imageStore(delta_out, p, vec4(td_is_bad(totalDelta) ? 0.0 : totalDelta, boundEngaged, 0.0, 0.0));
)GLSL";

	// ---- PASS 3B: Optional Codec-Grid-Aligned 8x8 DCT-II STRICT/CELL POCS (Speedup 7: Empty-Block Fetch Skip) ----
	// Dispatch: ceil(TD_W / 8) x ceil(TD_H / 8), group 8x8 (1 workgroup per codec-aligned 8x8 block).
	// Inputs: delta_in (w x h RG32F), cur_clean (w x h R32F), pred_clean (w x h R32F, bit-exact refs[0].clean).
	// Output: delta_out (w x h RG32F: .x = delta, .y = 1.0 when pixel-domain bound engaged).
	constexpr const char* kPass3bDctHeader = R"GLSL(
shared float td_s_sent[64];
shared float td_s_blend[64];
shared float td_s_tmp[64];
shared float td_s_sent_dct[64];
shared uint td_s_nz[64];

float td_project_coeff(float sentCoeff, float blendCoeff, bool strictMode)
{
    if (td_is_bad(sentCoeff) || td_is_bad(blendCoeff) || td_is_bad(u_step_delta) || td_is_bad(u_deadzone_phi) ||
        u_step_delta <= 0.0 || u_deadzone_phi < 0.0 || u_deadzone_phi >= 1.0)
        return td_is_bad(sentCoeff) ? 0.0 : sentCoeff;

    float theta = (1.0 - u_deadzone_phi) * u_step_delta;
    float absSent = abs(sentCoeff);
    // Keep quantization level in float (avoids 32-bit int overflow on very small u_step_delta).
    float level = 0.0;
    if (absSent >= theta)
    {
        float mag = floor(absSent / u_step_delta + u_deadzone_phi);
        level = (sentCoeff >= 0.0) ? mag : -mag;
    }
    if (level == 0.0)
        return clamp(blendCoeff, -theta, theta);
    if (strictMode)
        return sentCoeff;

    float lo = 0.0;
    float hi = 0.0;
    if (level > 0.0)
    {
        lo = (level - u_deadzone_phi) * u_step_delta;
        hi = (level + 1.0 - u_deadzone_phi) * u_step_delta;
    }
    else
    {
        lo = (level - 1.0 + u_deadzone_phi) * u_step_delta;
        hi = (level + u_deadzone_phi) * u_step_delta;
    }
    return clamp(blendCoeff, lo, hi);
}
)GLSL";

	constexpr const char* kPass3bDctPocs = R"GLSL(
ivec2 wg = ivec2(gl_WorkGroupID.xy);
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
int bx = wg.x * 8;
int by = wg.y * 8;
ivec2 p = ivec2(bx + loc.x, by + loc.y);
int tid = loc.y * 8 + loc.x;

int bx0 = int(round(u_bx0));
int by0 = int(round(u_by0));
int bx1 = int(round(u_bx1));
int by1 = int(round(u_by1));
int pocsIters = int(round(u_pocs_iters));
bool strictMode = (u_residual_mode > 0.5);

bool blockInInterior = (bx0 < bx1) && (by0 < by1) &&
                       (bx >= bx0) && (bx + 8 <= bx1) &&
                       (by >= by0) && (by + 8 <= by1);

if (!blockInInterior)
{
    if (p.x < TD_W && p.y < TD_H)
    {
        vec2 d = texelFetch(delta_in, p, 0).xy;
        imageStore(delta_out, p, vec4(d, 0.0, 0.0));
    }
    return;
}

float dVal = texelFetch(delta_in, p, 0).x;
td_s_nz[tid] = (dVal != 0.0) ? 1u : 0u;
memoryBarrierShared();
barrier();
for (int stride = 32; stride > 0; stride >>= 1)
{
    if (tid < stride)
        td_s_nz[tid] = td_s_nz[tid] | td_s_nz[tid + stride];
    memoryBarrierShared();
    barrier();
}

// Bit-exact skip for untouched / vetoed zero-delta 8x8 blocks (defers cur_clean & pred_clean fetches!).
if (td_s_nz[0] == 0u)
{
    imageStore(delta_out, p, vec4(0.0));
    return;
}

float cVal = texelFetch(cur_clean, p, 0).x;
float pVal = texelFetch(pred_clean, p, 0).x;
td_s_sent[tid] = cVal - pVal;
td_s_blend[tid] = (cVal + dVal) - pVal;
memoryBarrierShared();
barrier();

// Forward DCT-II of sent residual once (invariant across POCS iterations).
{
    float rowSum = 0.0;
    for (int x = 0; x < 8; ++x)
        rowSum += td_s_sent[loc.y * 8 + x] * TD_DCT8_BASIS[loc.x * 8 + x];
    td_s_tmp[tid] = rowSum;
    memoryBarrierShared();
    barrier();

    float colSum = 0.0;
    for (int y = 0; y < 8; ++y)
        colSum += td_s_tmp[y * 8 + loc.x] * TD_DCT8_BASIS[loc.y * 8 + y];
    td_s_sent_dct[tid] = colSum;
    memoryBarrierShared();
    barrier();
}

for (int it = 0; it < pocsIters; ++it)
{
    float rowSum = 0.0;
    for (int x = 0; x < 8; ++x)
        rowSum += td_s_blend[loc.y * 8 + x] * TD_DCT8_BASIS[loc.x * 8 + x];
    td_s_tmp[tid] = rowSum;
    memoryBarrierShared();
    barrier();

    float blendCoeff = 0.0;
    for (int y = 0; y < 8; ++y)
        blendCoeff += td_s_tmp[y * 8 + loc.x] * TD_DCT8_BASIS[loc.y * 8 + y];

    // Set 1: Quantization-cell coefficient projection (STRICT / CELL).
    float projCoeff = td_project_coeff(td_s_sent_dct[tid], blendCoeff, strictMode);
    td_s_blend[tid] = projCoeff;
    memoryBarrierShared();
    barrier();

    // Inverse DCT-II of projected residual (transpose of orthonormal basis).
    float invRow = 0.0;
    for (int u = 0; u < 8; ++u)
        invRow += td_s_blend[loc.y * 8 + u] * TD_DCT8_BASIS[u * 8 + loc.x];
    td_s_tmp[tid] = invRow;
    memoryBarrierShared();
    barrier();

    float projRes = 0.0;
    for (int v = 0; v < 8; ++v)
        projRes += td_s_tmp[v * 8 + loc.x] * TD_DCT8_BASIS[v * 8 + loc.y];

    dVal = (pVal + projRes) - cVal;

    // Set 2: Pixel-domain valid code-range projection [0, u_peak_code].
    if (u_peak_code > 0.0 && dVal != 0.0)
    {
        float cl = clamp(cVal + dVal, 0.0, u_peak_code);
        dVal = cl - cVal;
    }

    td_s_blend[tid] = (cVal + dVal) - pVal;
    memoryBarrierShared();
    barrier();
}

imageStore(delta_out, p, vec4(td_is_bad(dVal) ? 0.0 : dVal, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 4 (Speedup 4): Fused Recompose + Layer-4 TdGuardPort + Apply in ONE Dispatch! ----
	// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per cell).
	// Direct candOut fine-band blur (zero (cv + dv + og) - rv cancellation, exact twin of CPU outFullBands.H):
	//   ownGrain = recY - curClean;  newClean = curClean + delta;  candOut = newClean + ownGrain;
	//   inFullH = recY - G_fine(recY);  outFullH = candOut - G_fine(candOut);  ownGrainH = ownGrain - G_fine(ownGrain);
	//   addBack = cellAddBack * (inFullH - outFullH).
	// Uses 22x22 LDS halo (TD_RAD_FINE = 3, 10,040 B shmem — reuses halo buffers for tree reduction!).
	// Eliminates inOwnGrainTex_, candOutScratch, guardFineH/V textures and 4 dispatches completely!
	// Mode::Shadow skips imageStore(out_y, ...) completely (caller retains bit-exact receivedY).
	constexpr const char* kPass4FusedApplyGuardHeader = R"GLSL(
shared float td_sh_r[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sh_d[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sh_g[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sh_rh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sh_dh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sh_gh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sh_gout[2];
)GLSL";

	constexpr const char* kPass4FusedApplyGuard = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
bool validRect = (ax1 > ax0) && (ay1 > ay0);
int xMin = ax0;
int xMax = max(ax0, ax1 - 1);
int yMin = ay0;
int yMax = max(ay0, ay1 - 1);
bool hasCleanSplit = (u_has_clean_split > 0.5);
bool sameCleanRec = (u_same_clean_rec > 0.5);
bool shadowMode = (u_shadow_mode > 0.5);

int tileDim = 16 + 2 * TD_RAD_FINE;
int tileN = tileDim * tileDim;
int hN = tileDim * 16;
int subTiles = (TD_CELL_PX + 15) / 16;

float sumOgE = 0.0;
float sumOgFineE = 0.0;
float sumInFineE = 0.0;
float sumOutFineE = 0.0;
float sumCnt = 0.0;

// Cached per-pixel values for canonical TD_CELL_PX <= 16 (subTiles == 1, single iteration!).
float regRecY = 0.0;
float regCandOut = 0.0;
float regFineDiff = 0.0;
bool regInCellPx = false;
bool regInActive = false;
ivec2 regP = ivec2(0);

for (int subY = 0; subY < subTiles; ++subY)
{
    for (int subX = 0; subX < subTiles; ++subX)
    {
        int baseX = cell.x * TD_CELL_PX + subX * 16;
        int baseY = cell.y * TD_CELL_PX + subY * 16;
        ivec2 p = ivec2(baseX + lx, baseY + ly);

        // Step 1: Cooperative load of 22x22 active-clamped (recY, candOut, ownGrain) into LDS.
        for (int idx = tid; idx < tileN; idx += 256)
        {
            int ty = idx / tileDim;
            int tx = idx % tileDim;
            int gx = baseX - TD_RAD_FINE + tx;
            int gy = baseY - TD_RAD_FINE + ty;
            int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
            int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
            ivec2 sp = ivec2(sx, sy);
            float rv = texelFetch(rec_y, sp, 0).x;
            float cv = sameCleanRec ? rv : texelFetch(cur_clean, sp, 0).x;
            float dv = texelFetch(delta_tex, sp, 0).x;
            float og = rv - cv;
            float candOut = (cv + dv) + og;
            td_sh_r[idx] = rv;
            td_sh_d[idx] = candOut;
            td_sh_g[idx] = og;
        }
        memoryBarrierShared();
        barrier();

        // Step 2: Separable horizontal G_fine across 22x16 strip in LDS.
        for (int idx = tid; idx < hN; idx += 256)
        {
            int ty = idx / 16;
            int tx = idx % 16;
            int rowBase = ty * tileDim + (tx + TD_RAD_FINE);
            float ar = 0.0;
            float ad = 0.0;
            float ag = 0.0;
            for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
            {
                float w = TD_TAP_FINE[k + TD_RAD_FINE];
                ar += w * td_sh_r[rowBase + k];
                ad += w * td_sh_d[rowBase + k];
                ag += w * td_sh_g[rowBase + k];
            }
            td_sh_rh[idx] = ar;
            td_sh_dh[idx] = ad;
            td_sh_gh[idx] = ag;
        }
        memoryBarrierShared();
        barrier();

        int localCellX = subX * 16 + lx;
        int localCellY = subY * 16 + ly;
        bool inCell = (localCellX < TD_CELL_PX) && (localCellY < TD_CELL_PX) && (p.x < TD_W) && (p.y < TD_H);
        bool inAct = inCell && validRect && (p.x >= ax0) && (p.x < ax1) && (p.y >= ay0) && (p.y < ay1);

        if (inCell)
        {
            float recV = texelFetch(rec_y, p, 0).x;
            float cleanV = sameCleanRec ? recV : texelFetch(cur_clean, p, 0).x;
            if (!inAct)
            {
                // Bars: bit-exact copy of cur_clean to out_clean/carry_clean and rec_y to out_y.
                imageStore(out_clean, p, vec4(cleanV, 0.0, 0.0, 0.0));
                imageStore(carry_clean, p, vec4(cleanV, 0.0, 0.0, 0.0));
                if (subTiles == 1)
                {
                    regRecY = recV;
                    regCandOut = recV;
                    regFineDiff = 0.0;
                    regInCellPx = true;
                    regInActive = false;
                    regP = p;
                }
                else if (!shadowMode)
                {
                    imageStore(out_y, p, vec4(recV, 0.0, 0.0, 0.0));
                }
            }
            else
            {
                // Step 3: Separable vertical G_fine from LDS + immediate grain-free out_clean & carry_clean store.
                int centerIdx = (ly + TD_RAD_FINE) * tileDim + (lx + TD_RAD_FINE);
                float rv = td_sh_r[centerIdx];
                float candOut = td_sh_d[centerIdx];
                float og = td_sh_g[centerIdx];
                float dRaw = texelFetch(delta_tex, p, 0).x;
                float newClean = cleanV + dRaw;

                // Write strictly grain-free out_clean & carry_clean NOW (BLOCKER-2: NEVER receives Layer-4 add-back!).
                imageStore(out_clean, p, vec4(newClean, 0.0, 0.0, 0.0));
                imageStore(carry_clean, p, vec4(newClean, 0.0, 0.0, 0.0));

                float gr = 0.0;
                float gd = 0.0;
                float gg = 0.0;
                int cyBase = ly + TD_RAD_FINE;
                for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                {
                    float w = TD_TAP_FINE[k + TD_RAD_FINE];
                    int hIdx = (cyBase + k) * 16 + lx;
                    gr += w * td_sh_rh[hIdx];
                    gd += w * td_sh_dh[hIdx];
                    gg += w * td_sh_gh[hIdx];
                }

                float inFullH = rv - gr;
                float outFullH = candOut - gd;
                float ogFineH = og - gg;

                sumOgE += og * og;
                if (hasCleanSplit)
                    sumOgFineE += ogFineH * ogFineH;
                sumInFineE += inFullH * inFullH;
                sumOutFineE += outFullH * outFullH;
                sumCnt += 1.0;

                if (subTiles == 1)
                {
                    regRecY = rv;
                    regCandOut = candOut;
                    regFineDiff = inFullH - outFullH;
                    regInCellPx = true;
                    regInActive = true;
                    regP = p;
                }
            }
        }
        memoryBarrierShared();
        barrier();
    }
}

// Step 4: LDS parallel tree reduction reusing halo arrays (zero extra shmem!).
td_sh_r[tid]  = sumOgE;
td_sh_d[tid]  = sumOgFineE;
td_sh_g[tid]  = sumInFineE;
td_sh_rh[tid] = sumOutFineE;
td_sh_dh[tid] = sumCnt;
memoryBarrierShared();
barrier();

for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_sh_r[tid]  += td_sh_r[tid + stride];
        td_sh_d[tid]  += td_sh_d[tid + stride];
        td_sh_g[tid]  += td_sh_g[tid + stride];
        td_sh_rh[tid] += td_sh_rh[tid + stride];
        td_sh_dh[tid] += td_sh_dh[tid + stride];
    }
    memoryBarrierShared();
    barrier();
}

// Step 5: Thread 0 evaluates Layer-4 TdGuardPort and writes guard_cell.
if (tid == 0)
{
    float totOgE = td_sh_r[0];
    float totOgFineE = td_sh_d[0];
    float totInFineE = td_sh_g[0];
    float totOutFineE = td_sh_rh[0];
    float totCnt = td_sh_dh[0];

    float addBack = 0.0;
    int gFlags = 0;

    if (totCnt > 0.5)
    {
        float rawGv = texelFetch(cell_prior, cell, 0).x;
        float cellRetGVar = (!td_is_bad(rawGv) && rawGv > 0.0) ? rawGv : 0.0;
        float invC = 1.0 / totCnt;

        if (totOgE * invC > TD_NUM_FLOOR || cellRetGVar > TD_NUM_FLOOR)
        {
            float leakedFineE = cellRetGVar * TD_GAIN_FINE;
            float targetFineE = hasCleanSplit
                ? (totOgFineE * invC + leakedFineE)
                : ((leakedFineE > TD_NUM_FLOOR) ? min(totInFineE * invC, leakedFineE) : (totInFineE * invC));
            float curOutFineE = totOutFineE * invC;

            gFlags = 2; // evaluated
            if (td_is_bad(targetFineE) || td_is_bad(curOutFineE))
            {
                gFlags |= 1;
                addBack = 1.0;
            }
            else if (targetFineE > TD_NUM_FLOOR)
            {
                float ratio = curOutFineE / targetFineE;
                if (ratio < TD_MIN_VAR_RETENTION)
                {
                    gFlags |= 1;
                    float targetStd = sqrt(TD_MIN_VAR_RETENTION) * sqrt(targetFineE);
                    float curStd = (curOutFineE > 0.0) ? sqrt(curOutFineE) : 0.0;
                    float denom = sqrt(targetFineE) - curStd;
                    addBack = (denom > TD_NUM_FLOOR) ? clamp((targetStd - curStd) / denom, 0.0, 1.0) : 1.0;
                }
            }
        }
    }

    imageStore(guard_cell, cell, vec4(addBack, float(gFlags), 0.0, 0.0));
    td_sh_gout[0] = addBack;
    td_sh_gout[1] = float(gFlags);
}
memoryBarrierShared();
barrier();

// Step 6: Write final out_y (Mode::Shadow skips out_y store completely — caller retains receivedY bit-exact!).
if (shadowMode)
    return;

float cellAddBack = td_sh_gout[0];
int cellGFlags = int(round(td_sh_gout[1]));
bool applyAddBack = ((cellGFlags & 1) != 0) && (cellAddBack > 0.0);

if (subTiles == 1)
{
    if (regInCellPx)
    {
        float outVal = regRecY;
        if (regInActive)
        {
            outVal = regCandOut;
            if (applyAddBack)
                outVal += cellAddBack * regFineDiff;
        }
        imageStore(out_y, regP, vec4(outVal, 0.0, 0.0, 0.0));
    }
}
else
{
    // General TD_CELL_PX > 16 path (unconditional barriers inside workgroup-uniform loop for FXC cs_5_0 safety).
    for (int subY = 0; subY < subTiles; ++subY)
    {
        for (int subX = 0; subX < subTiles; ++subX)
        {
            int baseX = cell.x * TD_CELL_PX + subX * 16;
            int baseY = cell.y * TD_CELL_PX + subY * 16;
            ivec2 p = ivec2(baseX + lx, baseY + ly);

            for (int idx = tid; idx < tileN; idx += 256)
            {
                float rVal = 0.0;
                float cOutVal = 0.0;
                if (applyAddBack)
                {
                    int ty = idx / tileDim;
                    int tx = idx % tileDim;
                    int gx = baseX - TD_RAD_FINE + tx;
                    int gy = baseY - TD_RAD_FINE + ty;
                    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
                    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
                    ivec2 sp = ivec2(sx, sy);
                    rVal = texelFetch(rec_y, sp, 0).x;
                    float cv = sameCleanRec ? rVal : texelFetch(cur_clean, sp, 0).x;
                    float dv = texelFetch(delta_tex, sp, 0).x;
                    cOutVal = (cv + dv) + (rVal - cv);
                }
                td_sh_r[idx] = rVal;
                td_sh_d[idx] = cOutVal;
            }
            memoryBarrierShared();
            barrier();

            for (int idx = tid; idx < hN; idx += 256)
            {
                float ar = 0.0;
                float ad = 0.0;
                if (applyAddBack)
                {
                    int ty = idx / 16;
                    int tx = idx % 16;
                    int rowBase = ty * tileDim + (tx + TD_RAD_FINE);
                    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                    {
                        float w = TD_TAP_FINE[k + TD_RAD_FINE];
                        ar += w * td_sh_r[rowBase + k];
                        ad += w * td_sh_d[rowBase + k];
                    }
                }
                td_sh_rh[idx] = ar;
                td_sh_dh[idx] = ad;
            }
            memoryBarrierShared();
            barrier();

            int localCellX = subX * 16 + lx;
            int localCellY = subY * 16 + ly;
            bool inCell = (localCellX < TD_CELL_PX) && (localCellY < TD_CELL_PX) && (p.x < TD_W) && (p.y < TD_H);
            bool inAct = inCell && validRect && (p.x >= ax0) && (p.x < ax1) && (p.y >= ay0) && (p.y < ay1);
            if (inAct)
            {
                float rv = texelFetch(rec_y, p, 0).x;
                float cv = sameCleanRec ? rv : texelFetch(cur_clean, p, 0).x;
                float dv = texelFetch(delta_tex, p, 0).x;
                float outVal = (cv + dv) + (rv - cv);
                if (applyAddBack)
                {
                    float gr = 0.0;
                    float gd = 0.0;
                    int cyBase = ly + TD_RAD_FINE;
                    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                    {
                        float w = TD_TAP_FINE[k + TD_RAD_FINE];
                        int hIdx = (cyBase + k) * 16 + lx;
                        gr += w * td_sh_rh[hIdx];
                        gd += w * td_sh_dh[hIdx];
                    }
                    float inFullH = rv - gr;
                    float outFullH = outVal - gd;
                    outVal += cellAddBack * (inFullH - outFullH);
                }
                imageStore(out_y, p, vec4(outVal, 0.0, 0.0, 0.0));
            }
            memoryBarrierShared();
            barrier();
        }
    }
}
)GLSL";

	// ---- CHROMA GPU APPLY PASSES (TD-S4 GPU Twins: YUV sited upsample & unclipped RGB matrix) ----
	// diagFlags bitmask: bit 0 (1) = non-finite/overflow closed, bit 1 (2) = border-bilinear fallback.
	// Active rects & identity-grid switch passed via VpvGpu::Var float UBOs (zero recompiles on aspect changes).
	constexpr const char* kPassChromaDeltaHeader = R"GLSL(
vec2 td_sample_coded_delta_sited(ivec2 dp, out int diagFlags)
{
    diagFlags = 0;
    int dax0 = int(round(u_dax0));
    int day0 = int(round(u_day0));
    int dax1 = int(round(u_dax1));
    int day1 = int(round(u_day1));
    if (dp.x < dax0 || dp.x >= dax1 || dp.y < day0 || dp.y >= day1)
        return vec2(0.0);

    if (u_all_zero_delta > 0.5)
        return vec2(0.0);

    if (u_identity_grid > 0.5)
    {
        vec2 d = texelFetch(coded_delta, dp, 0).xy;
        if (td_is_bad(d.x)) { d.x = 0.0; diagFlags |= 1; }
        if (td_is_bad(d.y)) { d.y = 0.0; diagFlags |= 1; }
        return d;
    }

    int cax0 = int(round(u_cax0));
    int cay0 = int(round(u_cay0));
    int cax1 = int(round(u_cax1));
    int cay1 = int(round(u_cay1));
    int cAw = cax1 - cax0;
    int cAh = cay1 - cay0;
    if (cAw <= 0 || cAh <= 0)
        return vec2(0.0);

    float lx = (float(dp.x) + 0.5) * float(TD_DSUB_X) - 0.5 + u_dshift_x;
    float ly = (float(dp.y) + 0.5) * float(TD_DSUB_Y) - 0.5 + u_dshift_y;
    float cxLocal = (lx + 0.5 - u_cshift_x) / float(TD_CSUB_X) - 0.5 - float(cax0);
    float cyLocal = (ly + 0.5 - u_cshift_y) / float(TD_CSUB_Y) - 0.5 - float(cay0);

    vec2 outD = vec2(0.0);
    if (cxLocal >= 1.0 && cyLocal >= 1.0 && cxLocal < float(cAw) - 2.0 && cyLocal < float(cAh) - 2.0)
    {
        ivec2 i0 = ivec2(floor(vec2(cxLocal, cyLocal)));
        vec4 wx = td_cubic_weights(cxLocal - float(i0.x));
        vec4 wy = td_cubic_weights(cyLocal - float(i0.y));
        vec2 acc = vec2(0.0);
        for (int j = 0; j < 4; ++j)
        {
            vec2 row = vec2(0.0);
            for (int i = 0; i < 4; ++i)
            {
                ivec2 gq = ivec2(cax0 + i0.x - 1 + i, cay0 + i0.y - 1 + j);
                row += wx[i] * texelFetch(coded_delta, gq, 0).xy;
            }
            acc += wy[j] * row;
        }
        outD = acc;
    }
    else
    {
        diagFlags |= 2;
        vec2 cl = clamp(vec2(cxLocal, cyLocal), vec2(0.0), vec2(float(cAw - 1), float(cAh - 1)));
        ivec2 i0 = ivec2(floor(cl));
        ivec2 i1 = min(ivec2(cAw - 1, cAh - 1), i0 + ivec2(1));
        vec2 f = cl - vec2(i0);
        vec2 v00 = texelFetch(coded_delta, ivec2(cax0 + i0.x, cay0 + i0.y), 0).xy;
        vec2 v10 = texelFetch(coded_delta, ivec2(cax0 + i1.x, cay0 + i0.y), 0).xy;
        vec2 v01 = texelFetch(coded_delta, ivec2(cax0 + i0.x, cay0 + i1.y), 0).xy;
        vec2 v11 = texelFetch(coded_delta, ivec2(cax0 + i1.x, cay0 + i1.y), 0).xy;
        vec2 top = v00 + (v10 - v00) * f.x;
        vec2 bot = v01 + (v11 - v01) * f.x;
        outD = top * (1.0 - f.y) + bot * f.y;
    }

    if (td_is_bad(outD.x)) { outD.x = 0.0; diagFlags |= 1; }
    if (td_is_bad(outD.y)) { outD.y = 0.0; diagFlags |= 1; }
    return outD;
}
)GLSL";

	// Inputs: deliv_uv (dW x dH RG32F), coded_delta (cW x cH RG32F).
	// Outputs: out_uv (dW x dH RGBA32F: .xy = outCb/outCr, .z = diagFlags, .w = 0),
	//          out_delta (dW x dH RG32F: .xy = delivDeltaCb/delivDeltaCr).
	constexpr const char* kPassChromaApplyYuv = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_DW || p.y >= TD_DH)
    return;

vec2 deliv = texelFetch(deliv_uv, p, 0).xy;
int diagFlags = 0;
vec2 delta = td_sample_coded_delta_sited(p, diagFlags);

float outCb = 0.0;
if (td_is_bad(deliv.x))
{
    outCb = 0.0;
    delta.x = 0.0;
    diagFlags |= 1;
}
else if (delta.x != 0.0)
{
    float cand = deliv.x + delta.x;
    if (td_is_bad(cand))
    {
        outCb = deliv.x;
        delta.x = 0.0;
        diagFlags |= 1;
    }
    else
    {
        outCb = cand;
    }
}
else
{
    outCb = deliv.x;
}

float outCr = 0.0;
if (td_is_bad(deliv.y))
{
    outCr = 0.0;
    delta.y = 0.0;
    diagFlags |= 1;
}
else if (delta.y != 0.0)
{
    float cand = deliv.y + delta.y;
    if (td_is_bad(cand))
    {
        outCr = deliv.y;
        delta.y = 0.0;
        diagFlags |= 1;
    }
    else
    {
        outCr = cand;
    }
}
else
{
    outCr = deliv.y;
}

imageStore(out_uv, p, vec4(outCb, outCr, float(diagFlags), 0.0));
imageStore(out_delta, p, vec4(delta, 0.0, 0.0));
)GLSL";

	// Inputs: deliv_rgb (dW x dH RGBA32F), coded_delta (cW x cH RG32F).
	// Outputs: out_rgb (dW x dH RGBA32F: .xyz = unclipped RGB preserving BTB/WTW, .w = diagFlags),
	//          out_delta (dW x dH RG32F).
	constexpr const char* kPassChromaApplyRgb = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_DW || p.y >= TD_DH)
    return;

vec3 rgb = texelFetch(deliv_rgb, p, 0).xyz;
int diagFlags = 0;
if (td_is_bad(rgb.x) || td_is_bad(rgb.y) || td_is_bad(rgb.z))
{
    vec3 safeRgb = vec3(
        td_is_bad(rgb.x) ? 0.0 : rgb.x,
        td_is_bad(rgb.y) ? 0.0 : rgb.y,
        td_is_bad(rgb.z) ? 0.0 : rgb.z);
    diagFlags |= 1;
    imageStore(out_rgb, p, vec4(safeRgb, float(diagFlags)));
    imageStore(out_delta, p, vec4(0.0));
    return;
}
vec2 delta = td_sample_coded_delta_sited(p, diagFlags);

if (delta.x == 0.0 && delta.y == 0.0)
{
    imageStore(out_rgb, p, vec4(rgb, float(diagFlags)));
    imageStore(out_delta, p, vec4(0.0));
    return;
}

if (td_is_bad(delta.x) || td_is_bad(delta.y) || td_is_bad(u_kr) || td_is_bad(u_kb) || td_is_bad(u_kg) || !(u_kg > TD_NUM_FLOOR))
{
    diagFlags |= 1;
    imageStore(out_rgb, p, vec4(rgb, float(diagFlags)));
    imageStore(out_delta, p, vec4(0.0));
    return;
}

float kR = 2.0 * (1.0 - u_kr);
float kB = 2.0 * (1.0 - u_kb);
float dR = kR * delta.y;
float dB = kB * delta.x;
float dG = -(u_kr * kR / u_kg) * delta.y - (u_kb * kB / u_kg) * delta.x;
vec3 nrgb = rgb + vec3(dR, dG, dB);

if (td_is_bad(nrgb.x) || td_is_bad(nrgb.y) || td_is_bad(nrgb.z))
{
    diagFlags |= 1;
    imageStore(out_rgb, p, vec4(rgb, float(diagFlags)));
    imageStore(out_delta, p, vec4(0.0));
    return;
}

imageStore(out_rgb, p, vec4(nrgb, float(diagFlags)));
imageStore(out_delta, p, vec4(delta, 0.0, 0.0));
)GLSL";

} // namespace Shaders
} // namespace Gpu
} // namespace TdV2
