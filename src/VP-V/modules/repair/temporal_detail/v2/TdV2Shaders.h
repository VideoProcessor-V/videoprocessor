// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 + TD-S5-OPT (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5):
// Optimized GLSL compute shader sources for the 4-pass GPU fusion engine + chroma apply:
//   - Speedup (1): LDS-tiled single-loop separable blurs (kPass1BandH / kPass1BandV: 62 -> 3.25 fetches/px, FP32)
//   - Speedup (2): 2-stage coalesced LDS cell-moment reduction + fused Focus Guard (kPass2CellStats -> kPass2MergeAndSolve
//                  using exact Chan-Pebay parallel second-moment merge, zero atomics, zero cancellation, FP32)
//   - Speedup (3): Compact Preset::RealTime band storage (SC-05 trade-off: half-res 2x2 area-reduced FP32 RG32F (L,D3)
//                  using U2 VpUnit::BandFromBase::HalfSigma taps + full-res FP32 RG32F (M,H) via fused 1-pass 2D LDS
//                  G_fine; ALL band storage and moment inputs stay strictly FP32 per plan L60, never FP16!
//                  Preset::Reference retains full-res RGBA32F for <= 1e-3 10-bit CPU parity)
//   - Speedup (4): Single-dispatch fused Pass 4 (kPass4FusedApplyGuard: 5 dispatches -> 1 via algebraic linearity
//                  and 22x22 LDS halo; eliminates inOwnGrainTex_, candOutScratch, guardFineH/V; Shadow skips out_y store)
//   - Speedup (5): Reused 1x warpedVal (R32F) + packed 3-ref x 3-bit flags in R16 UNORM (2 B/px read; drops unconditional
//                  cur_clean fetch on valid interior pixels)
//   - Speedup (6): 20x20 LDS-tiled bicubic warp (kPass1WarpAndGate: constant cell MV per 16x16 WG -> zero global 4x4 fetches;
//                  explicit per-ref motion slot uniform u_motion_slot for fixed prev=0/next=1/carry=2 prepacked layout)
//   - Speedup (7): Per-frame dynamic parameters moved from #define to VpvGpu::Var float UBOs (zero mid-playback recompiles)
//                  + empty-block DCT fetch skip in kPass3bDctPocs + zero-copy seed copy/zero passes (kPassSeedCopy/ZeroCells)
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

	// Build exact half-res Gaussian semigroup complement taps for kBandMemo D3 = L - G(kSigmaLow):
	//   G(kSigmaLow) = G(kSigmaBase) * G(sigmaDiff), sigmaDiff = sqrt(kSigmaLow^2 - kSigmaBase^2).
	// Evaluated at half resolution on the already-bandlimited L = G(kSigmaBase) (aliasing < -60 dB).
	inline std::string BuildHalfDiffTapHeader()
	{
		const double sigmaDiff = std::sqrt(
			VpUnit::BandFromBase::kSigmaLow * VpUnit::BandFromBase::kSigmaLow -
			VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase);
		const double sigmaHalfDiff = 0.5 * sigmaDiff;
		const auto tHDiff = VpUnit::BandFromBase::Taps<double>(sigmaHalfDiff, 3.0, true, 0.0);
		const int rHDiff = static_cast<int>(tHDiff.size() / 2);
		std::string s;
		s += VpvGpu::DefineInt("TD_RAD_HDIFF", rHDiff);
		s += VpvGpu::ConstArray("TD_TAP_HDIFF", static_cast<int>(tHDiff.size()),
			[&](int i) { return tHDiff[static_cast<size_t>(i)]; });
		return s;
	}

	// Build exact full-res Gaussian semigroup complement taps for Reference kBandMemo D3 = L - G(kSigmaLow):
	//   G(kSigmaLow) = G(kSigmaBase) * G(sigmaDiff), sigmaDiff = sqrt(kSigmaLow^2 - kSigmaBase^2) (OPUS-L1).
	inline std::string BuildFullDiffTapHeader()
	{
		const double sigmaDiff = std::sqrt(
			VpUnit::BandFromBase::kSigmaLow * VpUnit::BandFromBase::kSigmaLow -
			VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase);
		const auto tFDiff = VpUnit::BandFromBase::Taps<double>(sigmaDiff, 3.0, true, 0.0);
		const int rFDiff = static_cast<int>(tFDiff.size() / 2);
		std::string s;
		s += VpvGpu::DefineInt("TD_RAD_FDIFF", rFDiff);
		s += VpvGpu::ConstArray("TD_TAP_FDIFF", static_cast<int>(tFDiff.size()),
			[&](int i) { return tFDiff[static_cast<size_t>(i)]; });
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

	// ---- PASS 0: Zero-Copy GPU Seed Copy & Cell-Target Zero (for ProcessFrameGpu on cut / first frame with no refs) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Compile-time target guards (TD_SEED_HAS_CLEAN / TD_SEED_HAS_OUT_Y / TD_SEED_HAS_DELTA / TD_SEED_DISTINCT_REC)
	// declare ONLY active descriptors so Mode::Shadow / partial-output RealTime callers NEVER allocate unused full-res
	// dummy storage targets (LOW-1).
	constexpr const char* kPassSeedCopy = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

float rawClean = texelFetch(src_clean, p, 0).x;
float cVal = td_is_bad(rawClean) ? 0.0 : rawClean;
imageStore(dst_carry, p, vec4(cVal, 0.0, 0.0, 0.0));
#if TD_SEED_HAS_CLEAN
imageStore(dst_clean, p, vec4(cVal, 0.0, 0.0, 0.0));
#endif
#if TD_SEED_HAS_OUT_Y
#if TD_SEED_DISTINCT_REC
float rawRec = texelFetch(src_rec, p, 0).x;
#else
float rawRec = rawClean;
#endif
float rVal = td_is_bad(rawRec) ? 0.0 : rawRec;
imageStore(dst_out_y, p, vec4(rVal, 0.0, 0.0, 0.0));
#endif
#if TD_SEED_HAS_DELTA
imageStore(dst_delta, p, vec4(0.0));
#endif
)GLSL";

	// Zero caller-supplied cell-grid output targets on seed/cut frames so stale GPU memory is never left behind.
	// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8.
	constexpr const char* kPassSeedZeroCells = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

if (u_zero_maps > 0.5)
{
    imageStore(dst_td_maps, ivec2(cell.x, cell.y), vec4(0.0));
    imageStore(dst_td_maps, ivec2(cell.x, TD_CH + cell.y), vec4(0.0));
    imageStore(dst_td_maps, ivec2(cell.x, 2 * TD_CH + cell.y), vec4(u_maps_light_gain, 0.0, 0.0, 0.0));
}
if (u_zero_diag > 0.5)
    imageStore(dst_diag, cell, vec4(0.0));
if (u_zero_guard > 0.5)
    imageStore(dst_guard, cell, vec4(0.0));
if (u_zero_warp_diag > 0.5)
    imageStore(dst_warp_diag, cell, vec4(0.0));
)GLSL";

	// ---- PASS 1A: Camera-Relative Motion Gate + Karis 3x3 Silhouette Dilation ----
	// Dispatch: ceil((K * TD_CW) / 16) x ceil(TD_CH / 16), group 16x16.
	// Inputs: motion_vec (3*cw x ch RGBA32F: fixed slots prev=0, next=1, carry=2), motion_meta (3*cw x ch RG32F: cls, valid).
	// Uniforms: u_k, u_ref_motion_slot_0/1/2 mapping compacted ref index rk -> fixed motion slot {0,1,2}.
	// Output: motion_gate (3*cw x ch RGBA32F: compacted rk -> gateL, gateMH, nonFiniteCell, 0).
	constexpr const char* kPass1MotionGateHeader = R"GLSL(
int td_motion_slot(int rk)
{
    if (rk == 0) return int(round(u_ref_motion_slot_0));
    if (rk == 1) return int(round(u_ref_motion_slot_1));
    return int(round(u_ref_motion_slot_2));
}

bool td_cell_rel_speed(int rk, int cx, int cy, out float spd, out int clsOut, out float confOut, out bool wasNonFinite, out bool fbOccluded)
{
    wasNonFinite = false;
    fbOccluded = false;
    confOut = 1.0;
    int slot = clamp(td_motion_slot(rk), 0, 2);
    vec4 mv = texelFetch(motion_vec, ivec2(slot * TD_CW + cx, cy), 0);
    vec4 mm = texelFetch(motion_meta, ivec2(slot * TD_CW + cx, cy), 0);
    clsOut = int(round(mm.x));
    if (mm.y < 0.5)
    {
        spd = TD_FAST_CUTOFF * 2.0;
        confOut = 0.0;
        return false;
    }
    if (td_is_bad(mv.x) || td_is_bad(mv.y) || td_is_bad(mv.z) || td_is_bad(mv.w))
    {
        spd = TD_FAST_CUTOFF * 2.0;
        confOut = 0.0;
        wasNonFinite = true;
        return false;
    }
    if (u_motion_meta_has_conf > 0.5)
    {
        if (td_is_bad(mm.z))
        {
            spd = TD_FAST_CUTOFF * 2.0;
            confOut = 0.0;
            wasNonFinite = true;
            return false;
        }
        confOut = clamp(mm.z, 0.0, 1.0);
    }
    if (u_motion_meta_has_fb > 0.5)
    {
        if (td_is_bad(mm.w) || mm.w < 0.0)
        {
            spd = TD_FAST_CUTOFF * 2.0;
            confOut = 0.0;
            wasNonFinite = true;
            return false;
        }
        if (u_enable_hard_fb_gate > 0.5 && mm.w > TD_HARD_FB_OCCLUSION_PX)
            fbOccluded = true;
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
float selfConf = 1.0;
bool selfBad = false;
bool selfFbOcc = false;
bool selfOk = td_cell_rel_speed(rk, cx, cy, selfSpd, selfCls, selfConf, selfBad, selfFbOcc);
if (!selfOk)
{
    imageStore(motion_gate, gid, vec4(0.0, 0.0, selfBad ? 1.0 : 0.0, 0.0));
    return;
}

if (selfCls == TD_CLS_OVERLAY || selfFbOcc)
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
        float nConf = 1.0;
        bool nBad = false;
        bool nFbOcc = false;
        if (td_cell_rel_speed(rk, nx, ny, nSpd, nCls, nConf, nBad, nFbOcc) && nCls != TD_CLS_OVERLAY)
        {
            dilatedSpd = max(dilatedSpd, nSpd);
        }
    }
}

float speedGate = 1.0 - td_hermite_ramp(TD_STILL_SPEED, TD_FAST_CUTOFF, dilatedSpd);
float gateL = ((selfCls == TD_CLS_OBJECT) ? speedGate : 1.0) * selfConf;
float gateMH = ((selfCls == TD_CLS_DYN_TEX) ? 0.0 : speedGate) * selfConf;
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
    float v = (u_same_ref_cur > 0.5) ? texelFetch(ref_clean, gq, 0).x : texelFetch(cur_clean, gq, 0).x;
    return td_is_bad(v) ? 0.0 : v;
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
int motionSlot = clamp(int(round(u_motion_slot)), 0, 2);
bool flagsClear = (u_flags_clear > 0.5);

// Workgroup-uniform 20x20 LDS tile fill for canonical TD_CELL_PX == 16 (constant cell MV across WG).
// Disabled on Camera cells when P7 per-pixel homography is active so zooms/rotations/perspective have zero 16-px tile seams.
int wgCx = min(TD_CW - 1, wg.x);
int wgCy = min(TD_CH - 1, wg.y);
vec2 wgMv = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + wgCx, wgCy), 0).xy;
vec2 wgMeta = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + wgCx, wgCy), 0).xy;
int wgCls = int(round(wgMeta.x));
bool wgValid = (wgMeta.y >= 0.5);
bool useH = (u_use_homography > 0.5);
bool smoothWarp = (u_enable_smooth_warp > 0.5);
bool useLds = (!smoothWarp) && (!useH || (TD_CELL_PX == 16 && wgCls != TD_CLS_CAMERA)) &&
              (TD_CELL_PX == 16) && validRect && wgValid &&
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
int mvCls = wgCls;
bool mvValid = wgValid;
if (TD_CELL_PX != 16)
{
    int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
    int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
    mv = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + cx, cy), 0).xy;
    vec2 pm = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + cx, cy), 0).xy;
    mvCls = int(round(pm.x));
    mvValid = (pm.y >= 0.5);
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

if (!mvValid)
{
    float fb = td_fetch_cur_fallback(p);
    uint np = td_pack_ref_flags(prevPacked, refIdx, 0);
    if (extraBad != 0)
        np |= 512u;
    imageStore(warped_val, p, vec4(fb, 0.0, 0.0, 0.0));
    imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
    return;
}

ivec2 sz = activeRect.zw - activeRect.xy;
vec2 sLocal = vec2(0.0);
if (useH && mvCls == TD_CLS_CAMERA)
{
    float px = float(p.x);
    float py = float(p.y);
    float hw = u_h6 * px + u_h7 * py + u_h8;
    float hsx = (abs(hw) > TD_NUM_FLOOR) ? ((u_h0 * px + u_h1 * py + u_h2) / hw) : 0.0;
    float hsy = (abs(hw) > TD_NUM_FLOOR) ? ((u_h3 * px + u_h4 * py + u_h5) / hw) : 0.0;
    bool hBad = td_is_bad(u_h0) || td_is_bad(u_h1) || td_is_bad(u_h2) ||
                td_is_bad(u_h3) || td_is_bad(u_h4) || td_is_bad(u_h5) ||
                td_is_bad(u_h6) || td_is_bad(u_h7) || td_is_bad(u_h8) ||
                td_is_bad(hw) || !(abs(hw) > TD_NUM_FLOOR) || td_is_bad(hsx) || td_is_bad(hsy);
    if (hBad)
    {
        float fb = td_fetch_cur_fallback(p);
        uint np = td_pack_ref_flags(prevPacked, refIdx, 4) | 512u;
        imageStore(warped_val, p, vec4(fb, 0.0, 0.0, 0.0));
        imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
        return;
    }
    sLocal = vec2(hsx - float(ax0), hsy - float(ay0));
}
else
{
    if (td_is_bad(mv.x) || td_is_bad(mv.y))
    {
        float fb = td_fetch_cur_fallback(p);
        uint np = td_pack_ref_flags(prevPacked, refIdx, 4);
        if (extraBad != 0)
            np |= 512u;
        imageStore(warped_val, p, vec4(fb, 0.0, 0.0, 0.0));
        imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
        return;
    }
    vec2 effMv = mv;
    if (smoothWarp)
    {
        float gx = (float(p.x) + 0.5) / float(TD_CELL_PX) - 0.5;
        float gy = (float(p.y) + 0.5) / float(TD_CELL_PX) - 0.5;
        int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
        int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
        int x1 = min(TD_CW - 1, x0 + 1);
        int y1 = min(TD_CH - 1, y0 + 1);
        float tx = clamp(gx - float(x0), 0.0, 1.0);
        float ty = clamp(gy - float(y0), 0.0, 1.0);
        ivec2 cs[4];
        cs[0] = ivec2(x0, y0); cs[1] = ivec2(x1, y0);
        cs[2] = ivec2(x0, y1); cs[3] = ivec2(x1, y1);
        float ws[4];
        ws[0] = (1.0 - tx) * (1.0 - ty); ws[1] = tx * (1.0 - ty);
        ws[2] = (1.0 - tx) * ty;         ws[3] = tx * ty;
        float wSum = 0.0;
        vec2 accMv = vec2(0.0);
        for (int q = 0; q < 4; ++q)
        {
            ivec2 tc = ivec2(motionSlot * TD_CW + cs[q].x, cs[q].y);
            if (texelFetch(motion_meta, tc, 0).y < 0.5)
                continue;
            vec2 qv = texelFetch(motion_vec, tc, 0).xy;
            if (td_is_bad(qv.x) || td_is_bad(qv.y))
                continue;
            wSum += ws[q];
            accMv += ws[q] * qv;
        }
        if (wSum > TD_NUM_FLOOR)
            effMv = accMv / wSum;
    }
    sLocal = vec2(float(plx), float(ply)) + effMv;
}
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
if (extraBad != 0)
    newPacked |= 512u; // Bit 9: ref_clean itself is non-finite at pixel p (for unique-cell nonFiniteCells parity)
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
    float sv = texelFetch(src_tex, ivec2(sx, safeY), 0).x;
    td_sh_h[ly][col] = td_is_bad(sv) ? 0.0 : sv;
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

	// ---- PASS 1C (RealTime Compact — SC-05): Half-Res FP32 (L, D3) + 1-Pass Fused 2D LDS Full-Res FP32 RG32F (M, H) ----
	// Honest SC-05 trade-off: half-res 2x2 area-reduced (L, D3) with U2 HalfSigma taps; all band storage & moment inputs stay FP32 (plan L60).
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
    if (td_is_bad(s00)) s00 = 0.0;
    if (td_is_bad(s10)) s10 = 0.0;
    if (td_is_bad(s01)) s01 = 0.0;
    if (td_is_bad(s11)) s11 = 0.0;
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

	// kPass1FullMH: 1-pass fused 2D LDS G_fine (22x22 halo, zero bandTmpH!) -> full-res FP32 RG32F (M, H).
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
    float sv = texelFetch(src_tex, ivec2(sx, sy), 0).x;
    td_sh_f0[ty][tx] = td_is_bad(sv) ? 0.0 : sv;
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

	// ---- PASS 1C (SOTA #10 — kBandMemo Shared Current-Frame Low-Band Zero-Duplicate Reuse) ----
	// Single fused 16x16 LDS compute pass taking curCleanY + precomputed curLowBandTex (G(kSigmaBase)):
	// - Computes exact G(kSigmaFine) via 22x22 LDS -> exact M = G_fine - L and H = src - G_fine.
	// - Computes D3 = L - G(kSigmaLow) via exact Gaussian semigroup G(sqrt(6.0^2 - 2.5^2)) at half-res (7.8 KB total LDS).
	// - Reference (TD_MEMO_COMPACT == 0): writes full-res RGBA32F bands_out in 1 dispatch (replaces 2 passes).
	// - RealTime  (TD_MEMO_COMPACT == 1): writes full-res RG32F bands_mh_out + half-res RG32F bands_ld3_out in 1 dispatch (replaces 3 passes).
	constexpr const char* kPass1BandFromMemoHeader = R"GLSL(
shared float td_sh_m0[16 + 2 * TD_RAD_FINE][16 + 2 * TD_RAD_FINE];
shared float td_sh_m1[16 + 2 * TD_RAD_FINE][16];
#if TD_MEMO_FULL_D3
shared float td_sh_fd0[16 + 2 * TD_RAD_FDIFF][16 + 2 * TD_RAD_FDIFF];
shared float td_sh_fd1[16 + 2 * TD_RAD_FDIFF][16];
#else
shared float td_sh_mh0[10 + 2 * TD_RAD_HDIFF][10 + 2 * TD_RAD_HDIFF];
shared float td_sh_mh1[10 + 2 * TD_RAD_HDIFF][10];
shared float td_sh_ml[10][10];
#endif
)GLSL";

	constexpr const char* kPass1BandFromMemo = R"GLSL(
int wgX = int(gl_WorkGroupID.x);
int wgY = int(gl_WorkGroupID.y);
int baseX = wgX * 16;
int baseY = wgY * 16;
int hBaseX = wgX * 8;
int hBaseY = wgY * 8;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;
ivec2 p = ivec2(baseX + lx, baseY + ly);

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
int hax0 = int(round(u_hax0));
int hay0 = int(round(u_hay0));
int hax1 = int(round(u_hax1));
int hay1 = int(round(u_hay1));
ivec4 halfActive = ivec4(hax0, hay0, hax1, hay1);
bool validRect = (ax1 > ax0) && (ay1 > ay0);
bool validHalf = (hax1 > hax0) && (hay1 > hay0);
int xMin = ax0;
int xMax = max(ax0, ax1 - 1);
int yMin = ay0;
int yMax = max(ay0, ay1 - 1);
int hxMin = hax0;
int hxMax = max(hax0, hax1 - 1);
int hyMin = hay0;
int hyMax = max(hay0, hay1 - 1);

// 1. Load 22x22 fine tile of curCleanY + full-res (OPUS-L1) or half-res tile of cur_low_band into LDS.
int fTileDim = 16 + 2 * TD_RAD_FINE;
int fTileN = fTileDim * fTileDim;
for (int idx = tid; idx < fTileN; idx += 256)
{
    int ty = idx / fTileDim;
    int tx = idx % fTileDim;
    int gx = baseX - TD_RAD_FINE + tx;
    int gy = baseY - TD_RAD_FINE + ty;
    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
    float sv = texelFetch(src_tex, ivec2(sx, sy), 0).x;
    td_sh_m0[ty][tx] = td_is_bad(sv) ? 0.0 : sv;
}

#if TD_MEMO_FULL_D3
int fdTileDim = 16 + 2 * TD_RAD_FDIFF;
int fdTileN = fdTileDim * fdTileDim;
for (int idx = tid; idx < fdTileN; idx += 256)
{
    int ty = idx / fdTileDim;
    int tx = idx % fdTileDim;
    int gx = baseX - TD_RAD_FDIFF + tx;
    int gy = baseY - TD_RAD_FDIFF + ty;
    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
    float lv = texelFetch(cur_low_band, ivec2(sx, sy), 0).x;
    td_sh_fd0[ty][tx] = td_is_bad(lv) ? 0.0 : lv;
}
#else
int hTileDim = 10 + 2 * TD_RAD_HDIFF;
int hTileN = hTileDim * hTileDim;
for (int idx = tid; idx < hTileN; idx += 256)
{
    int ty = idx / hTileDim;
    int tx = idx % hTileDim;
    int ghx = (hBaseX - 1) - TD_RAD_HDIFF + tx;
    int ghy = (hBaseY - 1) - TD_RAD_HDIFF + ty;
    int chx = validHalf ? clamp(ghx, hxMin, hxMax) : clamp(ghx, 0, TD_HALF_W - 1);
    int chy = validHalf ? clamp(ghy, hyMin, hyMax) : clamp(ghy, 0, TD_HALF_H - 1);
    float lv = texelFetch(cur_low_band, ivec2(chx, chy), 0).x;
    td_sh_mh0[ty][tx] = td_is_bad(lv) ? 0.0 : lv;
}
#endif
memoryBarrierShared();
barrier();

// 2. Separable horizontal convolutions in LDS (G_fine on src_tex, G_diff on cur_low_band).
int fHN = fTileDim * 16;
for (int idx = tid; idx < fHN; idx += 256)
{
    int ty = idx / 16;
    int tx = idx % 16;
    int c = tx + TD_RAD_FINE;
    float acc = 0.0;
    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
        acc += TD_TAP_FINE[k + TD_RAD_FINE] * td_sh_m0[ty][c + k];
    td_sh_m1[ty][tx] = acc;
}
#if TD_MEMO_FULL_D3
int fdHN = fdTileDim * 16;
for (int idx = tid; idx < fdHN; idx += 256)
{
    int ty = idx / 16;
    int tx = idx % 16;
    int c = tx + TD_RAD_FDIFF;
    float acc = 0.0;
    for (int k = -TD_RAD_FDIFF; k <= TD_RAD_FDIFF; ++k)
        acc += TD_TAP_FDIFF[k + TD_RAD_FDIFF] * td_sh_fd0[ty][c + k];
    td_sh_fd1[ty][tx] = acc;
}
memoryBarrierShared();
barrier();
#else
int hHN = hTileDim * 10;
for (int idx = tid; idx < hHN; idx += 256)
{
    int ty = idx / 10;
    int tx = idx % 10;
    int c = tx + TD_RAD_HDIFF;
    float acc = 0.0;
    for (int k = -TD_RAD_HDIFF; k <= TD_RAD_HDIFF; ++k)
        acc += TD_TAP_HDIFF[k + TD_RAD_HDIFF] * td_sh_mh0[ty][c + k];
    td_sh_mh1[ty][tx] = acc;
}
memoryBarrierShared();
barrier();

// 3. Vertical half-res G_diff -> 10x10 gLow tile in td_sh_ml.
if (tid < 100)
{
    int ty = tid / 10;
    int tx = tid % 10;
    int c = ty + TD_RAD_HDIFF;
    float gLow = 0.0;
    for (int k = -TD_RAD_HDIFF; k <= TD_RAD_HDIFF; ++k)
        gLow += TD_TAP_HDIFF[k + TD_RAD_HDIFF] * td_sh_mh1[c + k][tx];
    td_sh_ml[ty][tx] = gLow;
}
memoryBarrierShared();
barrier();
#endif

// 4. Write compact half-res (L, D3) when TD_MEMO_COMPACT == 1 (first 8x8 threads of each WG).
#if TD_MEMO_COMPACT
if (lx < 8 && ly < 8)
{
    ivec2 hp = ivec2(hBaseX + lx, hBaseY + ly);
    if (hp.x < TD_HALF_W && hp.y < TD_HALF_H)
    {
        if (!validHalf || hp.x < hax0 || hp.x >= hax1 || hp.y < hay0 || hp.y >= hay1)
        {
            imageStore(bands_ld3_out, hp, vec4(0.0));
        }
        else
        {
            float lH = td_sh_mh0[ly + 1 + TD_RAD_HDIFF][lx + 1 + TD_RAD_HDIFF];
            float gLowH = td_sh_ml[ly + 1][lx + 1];
            imageStore(bands_ld3_out, hp, vec4(lH, lH - gLowH, 0.0, 0.0));
        }
    }
}
#endif

if (p.x >= TD_W || p.y >= TD_H)
    return;

if (!validRect || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
#if TD_MEMO_COMPACT
    imageStore(bands_mh_out, p, vec4(0.0));
#else
    imageStore(bands_out, p, vec4(0.0));
#endif
    return;
}

int cyCenter = ly + TD_RAD_FINE;
float gFine = 0.0;
for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
    gFine += TD_TAP_FINE[k + TD_RAD_FINE] * td_sh_m1[cyCenter + k][lx];
float srcVal = td_sh_m0[cyCenter][lx + TD_RAD_FINE];

#if TD_MEMO_FULL_D3
float bL = td_sh_fd0[ly + TD_RAD_FDIFF][lx + TD_RAD_FDIFF];
#elif TD_MEMO_IS_HALF
float rawL = td_sample_half_ld3(cur_low_band, p, halfActive).x;
float bL = td_is_bad(rawL) ? 0.0 : rawL;
#else
float rawL = texelFetch(cur_low_band, p, 0).x;
float bL = td_is_bad(rawL) ? 0.0 : rawL;
#endif
float bM = gFine - bL;
float bH = srcVal - gFine;

#if TD_MEMO_COMPACT
imageStore(bands_mh_out, p, vec4(bM, bH, 0.0, 0.0));
#elif TD_MEMO_FULL_D3
int fdCenter = ly + TD_RAD_FDIFF;
float gLow = 0.0;
for (int k = -TD_RAD_FDIFF; k <= TD_RAD_FDIFF; ++k)
    gLow += TD_TAP_FDIFF[k + TD_RAD_FDIFF] * td_sh_fd1[fdCenter + k][lx];
imageStore(bands_out, p, vec4(bL, bM, bH, bL - gLow));
#else
vec2 hc = (vec2(p) - vec2(0.5)) / 2.0;
vec2 hLo = vec2(float(hax0), float(hay0));
vec2 hHi = vec2(float(max(hax0, hax1 - 1)), float(max(hay0, hay1 - 1)));
vec2 hCl = clamp(hc, hLo, hHi) - vec2(float(hBaseX - 1), float(hBaseY - 1));
vec2 locCl = clamp(hCl, vec2(0.0), vec2(9.0));
ivec2 i0 = ivec2(floor(locCl));
ivec2 i1 = min(ivec2(9), i0 + ivec2(1));
vec2 f = locCl - vec2(i0);
float g00 = td_sh_ml[i0.y][i0.x];
float g10 = td_sh_ml[i0.y][i1.x];
float g01 = td_sh_ml[i1.y][i0.x];
float g11 = td_sh_ml[i1.y][i1.x];
float gLow = (g00 + (g10 - g00) * f.x) * (1.0 - f.y) + (g01 + (g11 - g01) * f.x) * f.y;
imageStore(bands_out, p, vec4(bL, bM, bH, bL - gLow));
#endif
)GLSL";

	// ---- PASS 2A (Speedup 2): Coalesced 16x16 LDS Cell-Moment Reduction + Fused Focus Guard ----
	// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per cell).
	// Reads curBands, refBands[0..K-1], packed ref_flags (R16), motion_gate, cell_prior ONCE per pixel.
	// Outputs: cell_focus (cw x ch RGBA32F: focusFlags[bit0=focusChange, bit1=isDefocused], cellOwnGrainVar, spatialDetailRms, isFace),
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

	// Split into Part1 + Part2 to stay well below MSVC's 16,380-byte raw string literal limit (C2026).
	constexpr const char* kPass2CellStatsPart1 = R"GLSL(
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
float curOg2 = 0.0;
float sumCleanY = 0.0;
float singleCleanY = 0.0;
bool singleHasCleanY = false;
float activeCnt = 0.0;
bool hasCleanSplit = (u_has_clean_split > 0.5);
bool sameCleanRec = (u_same_clean_rec > 0.5);
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
        float cleanYVal = cb.x + cb.y + cb.z;
        sumCleanY += cleanYVal;
        if (TD_CELL_PX <= 16)
        {
            singleCleanY = cleanYVal;
            singleHasCleanY = true;
        }
        curM2 += cb.y * cb.y;
        curD3_2 += cb.w * cb.w;
        curH2 += cb.z * cb.z;
        if (hasCleanSplit && !sameCleanRec)
        {
            float rv = texelFetch(rec_y, p, 0).x;
            float cv = texelFetch(cur_clean, p, 0).x;
            if (!td_is_bad(rv) && !td_is_bad(cv))
            {
                float og = rv - cv;
                curOg2 += og * og;
            }
        }
        activeCnt += 1.0;

        uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
        if ((packedFl & 512u) != 0u)
            badMask |= 16u; // Bit 4 (16u): at least one ref_clean pixel in this cell is non-finite
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

// Reduction B: ref2 M2/D3_2 + borderBilinCnt + curOg2 (+ optional sumCleanY for SOTA #4 edge-aware guidance).
td_sh_red4[tid] = vec4(refM2[2], refD3_2[2], borderCnt, curOg2);
#if TD_EDGE_AWARE_UPSAMPLE
td_sh_red4b[tid] = vec4(sumCleanY, 0.0, 0.0, 0.0);
#endif
memoryBarrierShared();
barrier();
for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_sh_red4[tid]  += td_sh_red4[tid + stride];
#if TD_EDGE_AWARE_UPSAMPLE
        td_sh_red4b[tid] += td_sh_red4b[tid + stride];
#endif
    }
    memoryBarrierShared();
    barrier();
}

float cellMeanCleanY = 0.0;
float cellStdCleanY = 0.0;
float cellMidVar = 0.0;
#if TD_EDGE_AWARE_UPSAMPLE
if (tid == 0)
{
    cellMeanCleanY = (totCur.w > 0.5) ? (td_sh_red4b[0].x / totCur.w) : 0.0;
    td_sh_nc = cellMeanCleanY;
}
#endif
if (tid == 0)
{
    float totRefM2[3];
    float totRefD3[3];
    totRefM2[0] = totRef01.x; totRefD3[0] = totRef01.y;
    totRefM2[1] = totRef01.z; totRefD3[1] = totRef01.w;
    totRefM2[2] = td_sh_red4[0].x; totRefD3[2] = td_sh_red4[0].y;
    float totBorderCnt = td_sh_red4[0].z;
    float cellOwnGrainVar = (hasCleanSplit && totCur.w > 0.5) ? (td_sh_red4[0].w / totCur.w) : 0.0;

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
    cellMidVar = cM2;

    vec4 priorT = texelFetch(cell_prior, cell, 0);
    float rawSc = (u_cell_prior_has_step_scale > 0.5) ? priorT.z : 1.0;
    float cellScale = (!td_is_bad(rawSc) && rawSc > 0.0) ? clamp(rawSc, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX) : 1.0;
    float cellStepScaleSq = cellScale * cellScale;
    float cellStepNoiseVar = max(0.0, u_step_noise_var) * cellStepScaleSq;
    float cellStructureFloor = u_structure_floor * cellStepScaleSq;
    float quantMidRmsBound = TD_PIXEL_BOUND_SIGMA * sqrt(cellStepNoiseVar) * TD_HM_PEAK;

    float cM3Val = max(0.0, curEnergy - cM2);
    float cleanM2 = max(0.0, cM2 - cellStepNoiseVar * TD_GAIN_MID);
    float cleanM3 = max(0.0, cM3Val - cellStepNoiseVar * TD_GAIN_D3);
    float curFDebiased = (cleanM2 + cleanM3 > TD_NUM_FLOOR)
        ? (cleanM2 / (cleanM2 + cleanM3 + TD_NUM_FLOOR))
        : curF;

    bool focusChange = false;
    int expFocusBits = int(round(priorT.y));
    if ((expFocusBits & 1) != 0)
    {
        focusChange = true;
    }
    else if (totCur.w > 0.5 && K > 0)
    {
        float invC = 1.0 / totCur.w;
        for (int k = 0; k < K; ++k)
        {
            float rM2 = totRefM2[k] * invC;
            float rM3 = totRefD3[k] * invC;
            float rEnergy = rM2 + rM3;
            float rF = (rEnergy > TD_NUM_FLOOR) ? (rM2 / (rEnergy + TD_NUM_FLOOR)) : 1.0;
            float cleanRefM2 = max(0.0, rM2 - TD_BASE_QUANT_VAR * TD_GAIN_MID);
            float cleanRefM3 = max(0.0, rM3 - TD_BASE_QUANT_VAR * TD_GAIN_D3);
            float refFDebiased = (cleanRefM2 + cleanRefM3 > TD_NUM_FLOOR)
                ? (cleanRefM2 / (cleanRefM2 + cleanRefM3 + TD_NUM_FLOOR))
                : rF;
            if (rEnergy > cellStructureFloor && cM2 < rM2 && curFDebiased < TD_F_SHARP &&
                (refFDebiased - curFDebiased) > TD_F_SHIFT_DELTA &&
                (sqrt(rM2) - sqrt(cM2)) > quantMidRmsBound)
            {
                focusChange = true;
                break;
            }
        }
    }

    // Matched-focus optical bokeh from upstream kFocusMap / explicitDefocusedCells (Plan s1.4 L140):
    // bit 1 of cell_prior.y; zero false vetoes on smooth curved skies or 12-bit smooth Hermite BTB/WTW ramps.
    bool isDefocused = ((expFocusBits & 2) != 0);

    bool isFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);
    float focusFlags = (focusChange ? 1.0 : 0.0) + (isDefocused ? 2.0 : 0.0);

    imageStore(cell_focus, cell, vec4(focusFlags, cellOwnGrainVar, spatialDetailRms, isFace ? 1.0 : 0.0));
    uint diagMask = (totBadMask & 7u) | (isFace ? 8u : 0u) | (totBadMask & 16u);
    imageStore(warp_diag, cell, vec4(totBorderCnt, float(diagMask), 0.0, 0.0));
    td_sh_mask[0] = (focusChange ? 1u : 0u) | (isFace ? 2u : 0u) | (isDefocused ? 4u : 0u);
}
memoryBarrierShared();
barrier();

// Centered clean-luma std-dev per cell for SOTA #4 SC-04 edge-aware weight upsampling (OPUS-L2: zero cost when off).
#if TD_EDGE_AWARE_UPSAMPLE
{
    float meanY = td_sh_nc;
    float varAcc = 0.0;
    if (TD_CELL_PX <= 16)
    {
        if (singleHasCleanY)
        {
            float dY = singleCleanY - meanY;
            varAcc = dY * dY;
        }
    }
    else
    {
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
                vec4 cb2 = td_fetch_cur_band(ivec2(px, py), halfActive);
                float dY = (cb2.x + cb2.y + cb2.z) - meanY;
                varAcc += dY * dY;
            }
        }
    }
    td_sh_red4[tid] = vec4(varAcc, 0.0, 0.0, 0.0);
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
        cellStdCleanY = (totCur.w > 0.5) ? sqrt(max(0.0, td_sh_red4[0].x / totCur.w)) : 0.0;
    memoryBarrierShared();
    barrier();
}
#endif
)GLSL";

	constexpr const char* kPass2CellStatsPart2 = R"GLSL(
// Face cells and vetoed focus/bokeh cells are excluded from affected 3x3 band moment pools
// (focusChange: all bands; isDefocused: M/H always, L only when BokehMode::Untouched). No early return before barriers for FXC cs_5_0 safety.
uint cellMaskFlags = td_sh_mask[0];
bool cellFocusChange = (cellMaskFlags & 1u) != 0u;
bool cellIsFace = (cellMaskFlags & 2u) != 0u;
bool cellIsDefocused = (cellMaskFlags & 4u) != 0u;
bool bokehAllowL = (u_bokeh_mode > 0.5);

// Per-band 2-step exact centered sufficient stats (n_c, mu_c[3], S_c[6]).
for (int b = 0; b < 3; ++b)
{
    bool bandFocusVeto = cellFocusChange || (cellIsDefocused && (b >= 1 || !bokehAllowL));
    bool skipMoments = cellIsFace || bandFocusVeto;
    // Step 2.1: Reduce sumD[b] and cntB[b] (zero when masked/vetoed for this band).
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
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, 2 * TD_CH + cell.y),
                       (b == 0) ? vec4(0.0, 0.0, cellMeanCleanY, cellStdCleanY)
                                : ((b == 1) ? vec4(0.0, 0.0, cellMidVar, 0.0) : vec4(0.0)));
        }
        else
        {
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, cell.y), vec4(ncCell, mu.x, mu.y, mu.z));
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, TD_CH + cell.y), td_sh_red4[0]);
            imageStore(cell_stats, ivec2(b * TD_CW + cell.x, 2 * TD_CH + cell.y),
                       (b == 0) ? vec4(td_sh_red4b[0].xy, cellMeanCleanY, cellStdCleanY)
                                : ((b == 1) ? vec4(td_sh_red4b[0].xy, cellMidVar, 0.0) : vec4(td_sh_red4b[0].xy, 0.0, 0.0)));
        }
    }
    memoryBarrierShared();
    barrier();
}
)GLSL";

	inline std::string BuildPass2CellStatsBody()
	{
		return std::string(kPass2CellStatsPart1) + kPass2CellStatsPart2;
	}

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

float td_band_origin(int b)
{
    if (b == 0) return TD_ORIGIN_LOW;
    if (b == 1) return TD_ORIGIN_MID;
    return TD_ORIGIN_FINE;
}

float td_a2_band_peak_sq(int b)
{
    return (b == 1) ? TD_A2_PEAK_MID_SQ : 1.0;
}

bool td_is_exact_copy(int k)
{
    if (k == 0) return u_exact_copy_0 > 0.5;
    if (k == 1) return u_exact_copy_1 > 0.5;
    return u_exact_copy_2 > 0.5;
}

int td_ref_role(int k)
{
    if (k == 0) return int(round(u_ref_role_0));
    if (k == 1) return int(round(u_ref_role_1));
    return int(round(u_ref_role_2));
}

int td_p2b_motion_slot(int rk)
{
    if (rk == 0) return int(round(u_ref_motion_slot_0));
    if (rk == 1) return int(round(u_ref_motion_slot_1));
    return int(round(u_ref_motion_slot_2));
}

void td_hp_warp_axis(float v, out float outWW, out float outUU, out float outWU, out float outGG, out float outG0)
{
    float f = v - floor(v);
    vec4 w = td_cubic_weights(f);
    outWW = dot(w, w);
    int gLen = 2 * TD_RAD_FINE + 1;
    outGG = 0.0;
    for (int j = 0; j < gLen; ++j)
    {
        float gj = TD_TAP_FINE[j];
        outGG += gj * gj;
    }
    outG0 = TD_TAP_FINE[TD_RAD_FINE];
    outUU = 0.0;
    outWU = 0.0;
    for (int m = -TD_RAD_FINE - 1; m <= TD_RAD_FINE + 2; ++m)
    {
        float u = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            int idx = (m - (i - 1)) + TD_RAD_FINE;
            if (idx >= 0 && idx < gLen)
                u += w[i] * TD_TAP_FINE[idx];
        }
        outUU += u * u;
        if (m >= -1 && m <= 2)
            outWU += w[m + 1] * u;
    }
}

float td_fine_hp_warp_gain(vec2 mv)
{
    if (td_is_bad(mv.x) || td_is_bad(mv.y))
        return 1.0;
    float wwX = 0.0, uuX = 0.0, wuX = 0.0, ggX = 0.0, g0X = 0.0;
    float wwY = 0.0, uuY = 0.0, wuY = 0.0, ggY = 0.0, g0Y = 0.0;
    td_hp_warp_axis(mv.x, wwX, uuX, wuX, ggX, g0X);
    td_hp_warp_axis(mv.y, wwY, uuY, wuY, ggY, g0Y);
    float den = 1.0 - 2.0 * g0X * g0Y + ggX * ggY;
    if (!(den > TD_NUM_FLOOR))
        return clamp(wwX * wwY, 0.0, 1.0);
    float num = wwX * wwY - 2.0 * wuX * wuY + uuX * uuY;
    float ratio = num / den;
    return td_is_bad(ratio) ? clamp(wwX * wwY, 0.0, 1.0) : clamp(ratio, 0.0, 1.0);
}

float td_subpixel_h_atten_bias(vec2 mvP, vec2 mvQ, float curHVar, float grainHVar)
{
    if (td_is_bad(mvP.x) || td_is_bad(mvP.y) || td_is_bad(mvQ.x) || td_is_bad(mvQ.y) ||
        td_is_bad(curHVar) || td_is_bad(grainHVar))
        return 0.0;
    float alphaP = sqrt(max(0.0, td_fine_hp_warp_gain(mvP)));
    float alphaQ = sqrt(max(0.0, td_fine_hp_warp_gain(mvQ)));
    float texHVar = max(0.0, curHVar - max(0.0, grainHVar));
    float bias = (1.0 - alphaP) * (1.0 - alphaQ) * texHVar;
    return (!td_is_bad(bias) && bias > 0.0) ? bias : 0.0;
}

// Mirrors TdV2ReferenceEngine::findIndependentA1Partner (plan s1.1 L70):
// Roles: 0=PrevPeak, 1=NextPeak, 2=Carry, 3=PrevPeak2.
// Carry (role == 2) NEVER instruments another reference in A1 (shares recursive past error).
int td_find_independent_a1_partner(int K, int k)
{
    int myRole = td_ref_role(k);
    int primary = -1;
    int secondary = -1;
    for (int j = 0; j < K; ++j)
    {
        if (j == k) continue;
        int other = td_ref_role(j);
        if (other == 2) continue;
        if (myRole == 0)
        {
            if (other == 1) primary = j;
            else if (other == 3) secondary = j;
        }
        else if (myRole == 1)
        {
            if (other == 0) primary = j;
            else if (other == 3) secondary = j;
        }
        else if (myRole == 3)
        {
            if (other == 1) primary = j;
            else if (other == 0) secondary = j;
        }
        else if (myRole == 2)
        {
            if (other == 1) primary = j;
        }
    }
    return (primary >= 0) ? primary : secondary;
}

float td_psd_scale(int b)
{
    if (b == 0) return u_psd_low;
    if (b == 1) return u_psd_mid;
    return u_psd_fine;
}

float td_est_c_a1(float crossPN, float cPP, float cNN, float retGVar, float bandShare, float psdScale, float subpixelBias)
{
    if (td_is_bad(crossPN) || td_is_bad(cPP) || td_is_bad(cNN) || td_is_bad(retGVar) ||
        td_is_bad(bandShare) || td_is_bad(psdScale) || td_is_bad(subpixelBias))
        return 0.0;
    float upper = max(0.0, min(cPP, cNN));
    float gTerm = max(0.0, retGVar) * max(0.0, bandShare) * max(0.0, psdScale);
    float raw = crossPN - gTerm - max(0.0, subpixelBias);
    if (raw <= 0.0) return 0.0;
    return min(raw, upper);
}

// TD-U4 (H-3): Derived effective independent samples & Empirical-Bayes shrinkage helpers (1:1 CPU parity).
float td_effective_samples_for_band(int b, float sampleCountPx)
{
    if (td_is_bad(sampleCountPx) || sampleCountPx <= 0.0)
        return 1.0;
    float sigma = (b == 0) ? 2.5 : ((b == 1) ? 1.0 : 0.5);
    float corrArea = 6.283185307179586 * sigma * sigma;
    return max(1.0, sampleCountPx / corrArea);
}

float td_sampling_var_cross_moment(float cPP, float cNN, float cPN, float nEff)
{
    if (td_is_bad(cPP) || td_is_bad(cNN) || td_is_bad(cPN) || td_is_bad(nEff))
        return 0.0;
    float effN = max(1.0, nEff);
    float pp = max(0.0, cPP);
    float nn = max(0.0, cNN);
    float v = (pp * nn + cPN * cPN) / effN;
    return (!td_is_bad(v) && v > 0.0) ? v : 0.0;
}

float td_eb_shrink_c(float cRaw, float seSq, float priorU, float tauSq, float upperCap)
{
    if (td_is_bad(cRaw) || td_is_bad(seSq) || td_is_bad(priorU) || td_is_bad(tauSq) || td_is_bad(upperCap))
        return 0.0;
    float cap = max(0.0, upperCap);
    float u = max(0.0, priorU);
    float t2 = max(0.0, tauSq);
    float s2 = max(0.0, seSq);
    float denom = t2 + s2;
    float B = (denom > TD_NUM_FLOOR) ? clamp(t2 / denom, 0.0, 1.0) : 0.0;
    float shrunk = u + B * (cRaw - u);
    if (td_is_bad(shrunk) || shrunk <= 0.0)
        return 0.0;
    return min(shrunk, cap);
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
)GLSL";

	// Split solver header into Part1 + Part2 (CHR-LOW-2 / UPG-MED-1: stay well below MSVC C2026 16,380 B limit!).
	constexpr const char* kPass2SolverHeaderPart1 = R"GLSL(
bool td_cholesky_solve_3(float L[3][3], float rhs[3], int sz, out float outX[3])
{
    outX[0] = 0.0; outX[1] = 0.0; outX[2] = 0.0;
    float y[3];
    y[0] = 0.0; y[1] = 0.0; y[2] = 0.0;
    for (int i = 0; i < sz; ++i)
    {
        float s = rhs[i];
        for (int p = 0; p < i; ++p)
            s -= L[i][p] * y[p];
        y[i] = s / L[i][i];
        if (td_is_bad(y[i]))
            return false;
    }
    for (int i = sz - 1; i >= 0; --i)
    {
        float s = y[i];
        for (int p = i + 1; p < sz; ++p)
            s -= L[p][i] * outX[p];
        outX[i] = s / L[i][i];
        if (td_is_bad(outX[i]))
            return false;
    }
    return true;
}

// TD-U2 (H-2): Constrained convex QP solver (solverMode 0 = ActiveSetExact, 1 = LegacyClipRescale).
// Returns 0 = ok, 1 = singular, 2 = non-finite.
int td_solve_convex(int K, float C[3][3], float cVec[3], int solverMode, out float outW[3])
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
    if (diagSum <= TD_SOLVE_FLOOR)
        return 0;

    float meanDiag = diagSum / float(K);
    float reg = max(TD_TRACE_REG * meanDiag, TD_SOLVE_FLOOR);
    float pivotFloor = max(TD_PIVOT_FLOOR, TD_PIVOT_FLOOR * meanDiag);

    if (solverMode == 1)
    {
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
            if (pivotAbs <= pivotFloor)
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

    // ActiveSetExact: symmetrize regularized A = 0.5*(C + C') + reg*I and verify full SPD via Cholesky.
)GLSL";

	constexpr const char* kPass2SolverHeaderPart2 = R"GLSL(
    float A[3][3];
    float Lf[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            A[i][j] = 0.0;
            Lf[i][j] = 0.0;
        }
    for (int i = 0; i < K; ++i)
        for (int j = 0; j < K; ++j)
            A[i][j] = 0.5 * (C[i][j] + C[j][i]) + ((i == j) ? reg : 0.0);

    for (int i = 0; i < K; ++i)
    {
        for (int j = 0; j < i; ++j)
        {
            float s = A[i][j];
            for (int p = 0; p < j; ++p)
                s -= Lf[i][p] * Lf[j][p];
            Lf[i][j] = s / Lf[j][j];
        }
        float d = A[i][i];
        for (int p = 0; p < i; ++p)
            d -= Lf[i][p] * Lf[i][p];
        if (td_is_bad(d) || d <= pivotFloor)
            return 1;
        Lf[i][i] = sqrt(d);
    }

    float bestW[3];
    bestW[0] = 0.0; bestW[1] = 0.0; bestW[2] = 0.0;
    float bestObj = 0.0;

    int totalMasks = 1 << K;
    for (int mask = 1; mask < totalMasks; ++mask)
    {
        int idx[3];
        idx[0] = 0; idx[1] = 0; idx[2] = 0;
        int sz = 0;
        for (int i = 0; i < K; ++i)
        {
            if ((mask & (1 << i)) != 0)
            {
                idx[sz] = i;
                sz += 1;
            }
        }

        float Ls[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                Ls[i][j] = 0.0;
        bool subOk = true;
        for (int i = 0; i < sz && subOk; ++i)
        {
            for (int j = 0; j < i; ++j)
            {
                float s = A[idx[i]][idx[j]];
                for (int p = 0; p < j; ++p)
                    s -= Ls[i][p] * Ls[j][p];
                Ls[i][j] = s / Ls[j][j];
            }
            float d = A[idx[i]][idx[i]];
            for (int p = 0; p < i; ++p)
                d -= Ls[i][p] * Ls[i][p];
            if (td_is_bad(d) || d <= pivotFloor)
                subOk = false;
            else
                Ls[i][i] = sqrt(d);
        }
        if (!subOk)
            continue;

        float cSub[3];
        cSub[0] = 0.0; cSub[1] = 0.0; cSub[2] = 0.0;
        for (int i = 0; i < sz; ++i)
            cSub[i] = cVec[idx[i]];

        float uSub[3];
        if (!td_cholesky_solve_3(Ls, cSub, sz, uSub))
            continue;

        // Candidate A: unconstrained on subset S; Candidate B: simplex sum==1 KKT border on S.
        float sumU = 0.0;
        for (int i = 0; i < sz; ++i)
            sumU += uSub[i];

        for (int pass = 0; pass < 2; ++pass)
        {
            float cand[3];
            cand[0] = 0.0; cand[1] = 0.0; cand[2] = 0.0;
            bool hasCand = false;
            if (pass == 0)
            {
                for (int i = 0; i < sz; ++i)
                    cand[idx[i]] = uSub[i];
                hasCand = true;
            }
            else if (sz == 1)
            {
                cand[idx[0]] = 1.0;
                hasCand = true;
            }
            else if (sumU > 1.0 - 1e-5)
            {
                float onesSub[3];
                onesSub[0] = 1.0; onesSub[1] = 1.0; onesSub[2] = 1.0;
                float vSub[3];
                if (td_cholesky_solve_3(Ls, onesSub, sz, vSub))
                {
                    float sumV = 0.0;
                    for (int i = 0; i < sz; ++i)
                        sumV += vSub[i];
                    if (!td_is_bad(sumV) && sumV > pivotFloor)
                    {
                        float mu = (sumU - 1.0) / sumV;
                        for (int i = 0; i < sz; ++i)
                            cand[idx[i]] = uSub[i] - mu * vSub[i];
                        hasCand = true;
                    }
                }
            }
            if (!hasCand)
                continue;

            bool feas = true;
            float sumC = 0.0;
            for (int i = 0; i < K; ++i)
            {
                if (td_is_bad(cand[i]) || cand[i] < -1e-6)
                {
                    feas = false;
                    break;
                }
                cand[i] = max(0.0, cand[i]);
                sumC += cand[i];
            }
            if (!feas || sumC > 1.0 + 1e-5)
                continue;
            if (sumC > 1.0)
            {
                float invS = 1.0 / sumC;
                for (int i = 0; i < K; ++i)
                    cand[i] *= invS;
            }
            float quad = 0.0;
            float lin = 0.0;
            for (int i = 0; i < K; ++i)
            {
                lin += cand[i] * cVec[i];
                for (int j = 0; j < K; ++j)
                    quad += cand[i] * A[i][j] * cand[j];
            }
            float obj = quad - 2.0 * lin;
            if (!td_is_bad(obj) && obj < bestObj)
            {
                bestObj = obj;
                for (int i = 0; i < K; ++i)
                    bestW[i] = cand[i];
            }
        }
    }

    for (int i = 0; i < K; ++i)
        outW[i] = bestW[i];
    return 0;
}
)GLSL";

	inline std::string BuildPass2SolverHeader()
	{
		return std::string(kPass2SolverHeaderPart1) + kPass2SolverHeaderPart2;
	}

	// ---- PASS 2-EB (TD-U4 / H-3 / UPG-HIGH-5): 1-Workgroup Robust Cell-Grid Reduction for U_j(class) & Spread tau^2 ----
	// Dispatch: 1x1x1 workgroup of 16x16 threads (256 threads striding over cw*ch cells).
	// Activity class uses grain-corrected (M + H) matching CPU TdV2ReferenceEngine.h:1898.
	// Uses a 2-pass robust trimmed mean (rejecting outlier cells > mean + 1.5*std) so worst-5% spikes never distort U_j(class).
	// Output: eb_prior_out (3 x 2 RGBA32F: (b, actClass) -> vec4(U_prior, tauSq, validFlag, cellCount)).
	constexpr const char* kPass2EbPriorReduceHeader = R"GLSL(
shared vec4 td_eb_acc[256];
shared float td_eb_cnt[256];
shared float td_eb_trim_cap;
)GLSL";

	constexpr const char* kPass2EbPriorReduce = R"GLSL(
int tid = int(gl_LocalInvocationID.y) * 16 + int(gl_LocalInvocationID.x);
int K = int(round(u_k));
int totalCells = TD_CW * TD_CH;

float outU[6];
float outTau[6];
float outCnt[6];
for (int i = 0; i < 6; ++i)
{
    outU[i] = 0.0;
    outTau[i] = 0.0;
    outCnt[i] = 0.0;
}

for (int b = 0; b < 3; ++b)
{
    float bGain = td_band_gain(b);
    float psdScale = td_psd_scale(b);
    int bandPoolR = (b == 0) ? int(round(u_pool_r_l)) : ((b == 1) ? int(round(u_pool_r_m)) : int(round(u_pool_r_h)));
    for (int ac = 0; ac < 2; ++ac)
    {
        // Pass 1: accumulate raw & clipped moments to compute mean, variance, tau^2, and 1.5-sigma robust trim ceiling.
        vec4 locAcc = vec4(0.0);
        float locCnt = 0.0;
        float locClipSq = 0.0;
        for (int idx = tid; idx < totalCells; idx += 256)
        {
            int cx = idx % TD_CW;
            int cy = idx / TD_CW;
            ivec2 cell = ivec2(cx, cy);
            vec4 fInfo = texelFetch(cell_focus, cell, 0);
            int fFlags = int(round(fInfo.x));
            bool focusChange = ((fFlags & 1) != 0);
            bool isDefocused = ((fFlags & 2) != 0);
            bool isFace = (fInfo.w > 0.5);
            if (isFace || focusChange || (isDefocused && (b >= 1 || u_bokeh_mode < 0.5)))
                continue;

            float retG = max(0.0, texelFetch(cell_prior, cell, 0).x);
            float mVar = max(0.0, texelFetch(cell_stats, ivec2(TD_CW + cx, 2 * TD_CH + cy), 0).z);
            float hVar = fInfo.z * fInfo.z;
            float cleanTexMH = max(0.0, mVar - retG * TD_GAIN_MID * u_psd_mid) +
                               max(0.0, hVar - retG * TD_GAIN_FINE * u_psd_fine);
            int cellAc = (cleanTexMH > u_activity_tex_floor) ? 1 : 0;
            if (cellAc != ac)
                continue;

            float nPool = 0.0;
            vec3 muPool = vec3(0.0);
            float s00 = 0.0, s01 = 0.0, s02 = 0.0, s11 = 0.0, s12 = 0.0, s22 = 0.0;
            for (int dy = -bandPoolR; dy <= bandPoolR; ++dy)
            {
                int ncy = cy + dy;
                if (ncy < 0 || ncy >= TD_CH) continue;
                for (int dx = -bandPoolR; dx <= bandPoolR; ++dx)
                {
                    int ncx = cx + dx;
                    if (ncx < 0 || ncx >= TD_CW) continue;
                    vec4 t0 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, ncy), 0);
                    float nc = t0.x;
                    if (nc <= 0.5) continue;
                    vec3 muc = t0.yzw;
                    vec4 t1 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
                    vec2 t2 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0).xy;
                    if (nPool <= 0.5)
                    {
                        nPool = nc; muPool = muc;
                        s00 = t1.x; s01 = t1.y; s02 = t1.z; s11 = t1.w; s12 = t2.x; s22 = t2.y;
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
            if (nPool <= 0.5)
                continue;

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

            for (int k = 0; k < K; ++k)
            {
                if (td_is_exact_copy(k)) continue;
                int partner = td_find_independent_a1_partner(K, k);
                if (partner < 0) continue;
                vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cx, cy), 0).xy;
                float mGate = (b == 0) ? mg.x : mg.y;
                if (k == int(round(u_anchor_ref_idx)) && texelFetch(anchor_barred, cell, 0).x > 0.5)
                    mGate = 0.0;
                if (!(mGate > 0.0)) continue;
                float gTerm = retG * bGain * psdScale;
                float cRaw = C[k][partner] - gTerm;
                float cClip = clamp(cRaw, 0.0, max(0.0, min(C[k][k], C[partner][partner])));
                float nEff = td_effective_samples_for_band(b, nPool);
                float seSq = td_sampling_var_cross_moment(C[k][k], C[partner][partner], C[k][partner], nEff);
                if (!td_is_bad(cRaw) && !td_is_bad(seSq))
                {
                    locAcc += vec4(cClip, cRaw, cRaw * cRaw, seSq);
                    locClipSq += cClip * cClip;
                    locCnt += 1.0;
                }
                break;
            }
        }

        td_eb_acc[tid] = locAcc;
        td_eb_cnt[tid] = locCnt;
        memoryBarrierShared();
        barrier();
        for (int s = 128; s > 0; s >>= 1)
        {
            if (tid < s)
            {
                td_eb_acc[tid] += td_eb_acc[tid + s];
                td_eb_cnt[tid] += td_eb_cnt[tid + s];
            }
            memoryBarrierShared();
            barrier();
        }
        float p1Cnt = td_eb_cnt[0];
        vec4 p1Tot = td_eb_acc[0];

        // Reduce sum(cClip^2) for robust upper trim ceiling (meanClip + 1.5*stdClip).
        td_eb_cnt[tid] = locClipSq;
        memoryBarrierShared();
        barrier();
        for (int s = 128; s > 0; s >>= 1)
        {
            if (tid < s) td_eb_cnt[tid] += td_eb_cnt[tid + s];
            memoryBarrierShared();
            barrier();
        }
        if (tid == 0)
        {
            int slot = b * 2 + ac;
            outCnt[slot] = p1Cnt;
            td_eb_trim_cap = 1e30;
            if (p1Cnt > 0.5)
            {
                float meanClip = max(0.0, p1Tot.x / p1Cnt);
                outU[slot] = meanClip;
                float varClip = max(0.0, td_eb_cnt[0] / p1Cnt - meanClip * meanClip);
                td_eb_trim_cap = meanClip + 1.5 * sqrt(varClip);
                if (p1Cnt > 1.5)
                {
                    float meanRaw = p1Tot.y / p1Cnt;
                    float varRaw = max(0.0, (p1Tot.z - p1Cnt * meanRaw * meanRaw) / (p1Cnt - 1.0));
                    float meanSe = p1Tot.w / p1Cnt;
                    outTau[slot] = max(0.0, varRaw - meanSe);
                }
            }
        }
        memoryBarrierShared();
        barrier();

        // Pass 2: robust trimmed estimate of U_j(class) (excludes right-tail outlier cells above trim cap).
        if (p1Cnt > 3.5)
        {
            float trimCap = td_eb_trim_cap;
            float tSum = 0.0;
            float tCnt = 0.0;
            for (int idx = tid; idx < totalCells; idx += 256)
            {
                int cx = idx % TD_CW;
                int cy = idx / TD_CW;
                ivec2 cell = ivec2(cx, cy);
                vec4 fInfo = texelFetch(cell_focus, cell, 0);
                int fFlags = int(round(fInfo.x));
                if (fInfo.w > 0.5 || ((fFlags & 1) != 0) || (((fFlags & 2) != 0) && (b >= 1 || u_bokeh_mode < 0.5)))
                    continue;
                float retG = max(0.0, texelFetch(cell_prior, cell, 0).x);
                float mVar = max(0.0, texelFetch(cell_stats, ivec2(TD_CW + cx, 2 * TD_CH + cy), 0).z);
                float hVar = fInfo.z * fInfo.z;
                float cleanTexMH = max(0.0, mVar - retG * TD_GAIN_MID * u_psd_mid) +
                                   max(0.0, hVar - retG * TD_GAIN_FINE * u_psd_fine);
                if (((cleanTexMH > u_activity_tex_floor) ? 1 : 0) != ac)
                    continue;
                vec4 t0 = texelFetch(cell_stats, ivec2(b * TD_CW + cx, cy), 0);
                if (t0.x <= 0.5) continue;
                float invN = 1.0 / t0.x;
                vec3 muc = t0.yzw;
                vec4 t1 = texelFetch(cell_stats, ivec2(b * TD_CW + cx, TD_CH + cy), 0);
                vec2 t2 = texelFetch(cell_stats, ivec2(b * TD_CW + cx, 2 * TD_CH + cy), 0).xy;
                float C00 = max(0.0, t1.x * invN + muc.x * muc.x);
                float C01 = t1.y * invN + muc.x * muc.y;
                float C02 = t1.z * invN + muc.x * muc.z;
                float C11 = max(0.0, t1.w * invN + muc.y * muc.y);
                float C12 = t2.x * invN + muc.y * muc.z;
                float C22 = max(0.0, t2.y * invN + muc.z * muc.z);
                float Cmat[3][3];
                Cmat[0][0] = C00; Cmat[0][1] = C01; Cmat[0][2] = C02;
                Cmat[1][0] = C01; Cmat[1][1] = C11; Cmat[1][2] = C12;
                Cmat[2][0] = C02; Cmat[2][1] = C12; Cmat[2][2] = C22;
                for (int k = 0; k < K; ++k)
                {
                    if (td_is_exact_copy(k)) continue;
                    int partner = td_find_independent_a1_partner(K, k);
                    if (partner < 0) continue;
                    vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cx, cy), 0).xy;
                    float mGate2 = (b == 0) ? mg.x : mg.y;
                    if (k == int(round(u_anchor_ref_idx)) && texelFetch(anchor_barred, cell, 0).x > 0.5)
                        mGate2 = 0.0;
                    if (!(mGate2 > 0.0)) continue;
                    float cClip = clamp(Cmat[k][partner] - retG * bGain * psdScale, 0.0, max(0.0, min(Cmat[k][k], Cmat[partner][partner])));
                    if (!td_is_bad(cClip) && cClip <= trimCap)
                    {
                        tSum += cClip;
                        tCnt += 1.0;
                    }
                    break;
                }
            }
            td_eb_acc[tid] = vec4(tSum, tCnt, 0.0, 0.0);
            memoryBarrierShared();
            barrier();
            for (int s = 128; s > 0; s >>= 1)
            {
                if (tid < s) td_eb_acc[tid].xy += td_eb_acc[tid + s].xy;
                memoryBarrierShared();
                barrier();
            }
            if (tid == 0 && td_eb_acc[0].y > 0.5)
                outU[b * 2 + ac] = max(0.0, td_eb_acc[0].x / td_eb_acc[0].y);
            memoryBarrierShared();
            barrier();
        }
    }
}

if (tid == 0)
{
    for (int b = 0; b < 3; ++b)
    {
        int s0 = b * 2;
        int s1 = b * 2 + 1;
        float u0 = outU[s0], t0 = outTau[s0], c0 = outCnt[s0];
        float u1 = outU[s1], t1 = outTau[s1], c1 = outCnt[s1];
        if (c0 < 0.5 && c1 > 0.5) { u0 = u1; t0 = t1; }
        else if (c1 < 0.5 && c0 > 0.5) { u1 = u0; t1 = t0; }
        float anyV = (c0 > 0.5 || c1 > 0.5) ? 1.0 : 0.0;
        imageStore(eb_prior_out, ivec2(b, 0), vec4(u0, t0, anyV, c0));
        imageStore(eb_prior_out, ivec2(b, 1), vec4(u1, t1, anyV, c1));
    }
}
)GLSL";

	// Tiny cell-stats snapshot copy for optional default-OFF 2-frame same-role stats-only pooling (TD-U4 / D4(b)).
	constexpr const char* kPass2CopyCellStats = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= 3 * TD_CW || p.y >= 3 * TD_CH)
    return;
imageStore(dst_stats, p, texelFetch(src_stats, p, 0));
)GLSL";

	// Tiny 3*cw x ch cell-weights copy so Plan 6.6 chroma reuse_luma reads a lifetime-safe persistent texture (CHR-HIGH-3).
	constexpr const char* kPass2CopyCellWeights = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= 3 * TD_CW || p.y >= TD_CH)
    return;
imageStore(dst_weights, p, texelFetch(src_weights, p, 0));
)GLSL";

	// Tiny full-res R32F plane copy for single-resample anchored carry peak snapshots (UPG-BLKR-3).
	constexpr const char* kPassCopyPlaneR32 = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;
imageStore(dst_plane, p, vec4(texelFetch(src_plane, p, 0).x, 0.0, 0.0, 0.0));
)GLSL";

	// Split kPass2MergeAndSolve into Part1 + Part2 (UPG-MED-1: stay well below MSVC C2026 16,380 B limit!).
	constexpr const char* kPass2MergeAndSolvePart1 = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int K = int(round(u_k));
int estMode = int(round(u_estimator));
bool enableEbShrink = (u_enable_eb_shrink > 0.5);
bool mergePrevStats = (u_merge_prev_stats > 0.5);

vec4 fInfo = texelFetch(cell_focus, cell, 0);
int fFlags = int(round(fInfo.x));
bool focusChange = ((fFlags & 1) != 0);
bool isDefocused = ((fFlags & 2) != 0);
bool bokehAllowL = (u_bokeh_mode > 0.5);
float cellOwnGrainVar = max(0.0, fInfo.y);
float spatialDetailRms = fInfo.z;
bool isFace = (fInfo.w > 0.5);

bool cellNonFinite = false;
bool cellSingular = false;
bool cellSingleRefA2 = false;

for (int k = 0; k < K; ++k)
{
    if (texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).z > 0.5)
        cellNonFinite = true;
}

vec4 priorTexel = texelFetch(cell_prior, cell, 0);
float rawGv = priorTexel.x;
float cellRetGVar = 0.0;
if (td_is_bad(rawGv) || rawGv < 0.0)
{
    cellNonFinite = true;
}
else
{
    cellRetGVar = rawGv;
}

float rawStepScale = (u_cell_prior_has_step_scale > 0.5) ? priorTexel.z : 1.0;
float cellStepScale = 0.0;
if (td_is_bad(rawStepScale) || !(rawStepScale > 0.0))
{
    cellNonFinite = true;
}
else
{
    cellStepScale = clamp(rawStepScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX);
}
float cellStepScaleSq = cellStepScale * cellStepScale;

vec2 cellLumaStat = texelFetch(cell_stats, ivec2(cell.x, 2 * TD_CH + cell.y), 0).zw;
float cellMeanCleanY = cellLumaStat.x;
float cellStdCleanY = cellLumaStat.y;

float cellWeightSumAll = 0.0;
float midWeightSum = 0.0;
float cellResEnergy = 0.0;
int activeBandBits = 0;
vec4 bandW[3];
bandW[0] = vec4(0.0);
bandW[1] = vec4(0.0);
bandW[2] = vec4(0.0);
float bandU[3];
bandU[0] = 0.0;
bandU[1] = 0.0;
bandU[2] = 0.0;

for (int b = 0; b < 3; ++b)
{
    int bandPoolR = (b == 0) ? int(round(u_pool_r_l)) : ((b == 1) ? int(round(u_pool_r_m)) : int(round(u_pool_r_h)));
    float nPool = 0.0;
    vec3 muPool = vec3(0.0);
    float s00 = 0.0, s01 = 0.0, s02 = 0.0, s11 = 0.0, s12 = 0.0, s22 = 0.0;
    bool hasOwnStats = false;
    vec3 ownDiag = vec3(0.0);

    for (int dy = -bandPoolR; dy <= bandPoolR; ++dy)
    {
        int ncy = cell.y + dy;
        if (ncy < 0 || ncy >= TD_CH) continue;
        for (int dx = -bandPoolR; dx <= bandPoolR; ++dx)
        {
            int ncx = cell.x + dx;
            if (ncx < 0 || ncx >= TD_CW) continue;
            vec4 t0 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, ncy), 0);
            float nc = t0.x;
            if (nc <= 0.5) continue;
            vec3 muc = t0.yzw;
            vec4 t1 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
            vec2 t2 = texelFetch(cell_stats, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0).xy;
            if (dx == 0 && dy == 0)
            {
                float invOwn = 1.0 / nc;
                ownDiag = vec3(
                    max(0.0, t1.x * invOwn + muc.x * muc.x),
                    max(0.0, t1.w * invOwn + muc.y * muc.y),
                    max(0.0, t2.y * invOwn + muc.z * muc.z));
                hasOwnStats = true;
            }

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

    // Optional default-OFF 2-frame same-role statistics-only pooling (TD-U4 / Owner D4(b) / UPG-MED-2):
    // merges ONLY cell statistics from the previous same-role frame on low-motion cells (|v| < 0.5 * TD_CELL_PX).
    bool lowMotionForMerge = mergePrevStats;
    if (lowMotionForMerge)
    {
        float maxSpd = 0.5 * float(TD_CELL_PX);
        for (int kk = 0; kk < K; ++kk)
        {
            int slot = clamp(td_p2b_motion_slot(kk), 0, 2);
            vec2 mv = texelFetch(motion_vec, ivec2(slot * TD_CW + cell.x, cell.y), 0).xy;
            if (td_is_bad(mv.x) || td_is_bad(mv.y) || length(mv) >= maxSpd)
            {
                lowMotionForMerge = false;
                break;
            }
        }
    }
    if (lowMotionForMerge && nPool > 0.5)
    {
        for (int dy = -bandPoolR; dy <= bandPoolR; ++dy)
        {
            int ncy = cell.y + dy;
            if (ncy < 0 || ncy >= TD_CH) continue;
            for (int dx = -bandPoolR; dx <= bandPoolR; ++dx)
            {
                int ncx = cell.x + dx;
                if (ncx < 0 || ncx >= TD_CW) continue;
                vec4 pt0 = texelFetch(prev_cell_stats, ivec2(b * TD_CW + ncx, ncy), 0);
                float pnc = pt0.x;
                if (pnc <= 0.5) continue;
                vec3 pmuc = pt0.yzw;
                vec4 pt1 = texelFetch(prev_cell_stats, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
                vec2 pt2 = texelFetch(prev_cell_stats, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0).xy;
                float nNew = nPool + pnc;
                vec3 d = pmuc - muPool;
                float w = (nPool * pnc) / nNew;
                muPool += d * (pnc / nNew);
                s00 += pt1.x + d.x * d.x * w;
                s01 += pt1.y + d.x * d.y * w;
                s02 += pt1.z + d.x * d.z * w;
                s11 += pt1.w + d.y * d.y * w;
                s12 += pt2.x + d.y * d.z * w;
                s22 += pt2.y + d.z * d.z * w;
                nPool = nNew;
            }
        }
    }
)GLSL";

	constexpr const char* kPass2MergeAndSolvePart2 = R"GLSL(
    vec4 wOut = vec4(0.0);
    vec4 diagOut = vec4(0.0);

    // Pass-1 Face / Non-Rigid Mask (H13, plan Q2 / s1.3 L127): force w = 0 strictly on all bands (exact passthrough).
    if (!isFace && nPool > 0.5)
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

        diagOut.x = (K > 0) ? (hasOwnStats ? min(ownDiag.x, C[0][0]) : C[0][0]) : 0.0;
        diagOut.y = (K > 1) ? (hasOwnStats ? min(ownDiag.y, C[1][1]) : C[1][1]) : 0.0;
        diagOut.z = (K > 2) ? (hasOwnStats ? min(ownDiag.z, C[2][2]) : C[2][2]) : 0.0;

        float bGain = td_band_gain(b);
        float psdScale = td_psd_scale(b);
        float curHVar = spatialDetailRms * spatialDetailRms;
        bool useSubpixelH = (u_enable_subpixel_h_debias > 0.5) && (b == 2);
        vec2 refMv[3];
        refMv[0] = vec2(0.0); refMv[1] = vec2(0.0); refMv[2] = vec2(0.0);
        if (useSubpixelH)
        {
            for (int kk = 0; kk < K; ++kk)
            {
                int slot = clamp(td_p2b_motion_slot(kk), 0, 2);
                refMv[kk] = texelFetch(motion_vec, ivec2(slot * TD_CW + cell.x, cell.y), 0).xy;
            }
        }
        float curMVar = max(0.0, texelFetch(cell_stats, ivec2(TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z);
        float cleanTexMH = max(0.0, curMVar - cellRetGVar * TD_GAIN_MID * u_psd_mid) +
                           max(0.0, curHVar - cellRetGVar * TD_GAIN_FINE * u_psd_fine);
        int cellAc = (cleanTexMH > u_activity_tex_floor) ? 1 : 0;
        vec4 ebInfo = texelFetch(eb_prior, ivec2(b, cellAc), 0);
        float ebPriorU = max(0.0, ebInfo.x);
        float ebTauSq = max(0.0, ebInfo.y);
        bool ebValid = (ebInfo.z > 0.5);
        bool useA2ePrior = (u_use_a2e_prior > 0.5) && ebValid;
        float nEff = td_effective_samples_for_band(b, nPool);

        float cVec[3];
        cVec[0] = 0.0; cVec[1] = 0.0; cVec[2] = 0.0;
        for (int k = 0; k < K; ++k)
        {
            bool exactCopy = td_is_exact_copy(k);
            int partner = td_find_independent_a1_partner(K, k);
            // UPG-BLKR-4 & UPG-HIGH-5: When A2e empirical prior is active on transform-unknown streams without explicit step prior,
            // use ebPriorU (with + 3*tau upper envelope when partner >= 0 so worst-5% damaged cells get repaired, and conservative
            // class prior ebPriorU on one-sided K = 1 cells!). Never starve HEVC/AV1 GPU repair to 1-LSB!
            float rawA2 = 0.0;
            if (useA2ePrior)
            {
                if (!exactCopy)
                {
                    float upperEnv = (partner >= 0) ? (ebPriorU + 3.0 * sqrt(ebTauSq)) : ebPriorU;
                    rawA2 = upperEnv * cellStepScaleSq;
                }
            }
            else if (u_a2_valid > 0.5)
            {
                rawA2 = (u_a2_sigma_sent + (exactCopy ? 0.0 : u_a2_sigma_zero)) * cellStepScaleSq * bGain;
            }
            // Plan L81 physical upper bound: c_k in [0, C_{k,k}].
            float cA2 = min(rawA2, C[k][k]);
            float cA1 = cA2;
            if (partner >= 0)
            {
                float subBias = useSubpixelH
                    ? td_subpixel_h_atten_bias(refMv[k], refMv[partner], curHVar, cellRetGVar * bGain * psdScale)
                    : 0.0;
                if (enableEbShrink && !exactCopy && ebValid)
                {
                    float gTerm = max(0.0, cellRetGVar) * max(0.0, bGain) * max(0.0, psdScale);
                    float cRaw = C[k][partner] - gTerm - max(0.0, subBias);
                    float seSq = td_sampling_var_cross_moment(C[k][k], C[partner][partner], C[k][partner], nEff);
                    cA1 = td_eb_shrink_c(cRaw, seSq, ebPriorU, ebTauSq, min(C[k][k], C[partner][partner]));
                }
                else
                {
                    cA1 = td_est_c_a1(C[k][partner], C[k][k], C[partner][partner], cellRetGVar, bGain, psdScale, subBias);
                }
            }
            else if (estMode != 1)
            {
                cellSingleRefA2 = true;
            }
            cVec[k] = exactCopy ? 0.0 : td_select_blind_c(estMode, cA1, cA2);
            if (!exactCopy && partner < 0 && estMode != 1)
            {
                for (int j = 0; j < K; ++j)
                {
                    if (j == k || td_is_exact_copy(j)) continue;
                    float capBias = useSubpixelH
                        ? td_subpixel_h_atten_bias(refMv[k], refMv[j], curHVar, cellRetGVar * bGain * psdScale)
                        : 0.0;
                    float cCap = td_est_c_a1(C[k][j], C[k][k], C[j][j], cellRetGVar, bGain, psdScale, capBias);
                    cVec[k] = min(cVec[k], cCap);
                }
            }
        }

        // TD-U3 (H-1 / UPG-BLKR-4): Per-block own-error variance U_j(block) for the honest transform-unknown statistical limit.
        float blockOwnU = (u_a2_valid > 0.5) ? (u_a2_total_var * cellStepScaleSq * bGain) : 0.0;
        if (u_use_empirical_u > 0.5 && ebValid)
        {
            float sumC = 0.0;
            float nC = 0.0;
            for (int k = 0; k < K; ++k)
            {
                if (!td_is_exact_copy(k))
                {
                    sumC += max(0.0, cVec[k]);
                    nC += 1.0;
                }
            }
            blockOwnU = (nC > 0.5) ? (sumC / nC) : 0.0;
        }
        bandU[b] = blockOwnU;

        float solvedW[3];
        int status = td_solve_convex(K, C, cVec, int(round(u_solver_mode)), solvedW);
        if (status == 2)
        {
            cellNonFinite = true;
        }
        else if (status == 1)
        {
            cellSingular = true;
        }
        else
        {
            // Focus & Bokeh Rule (H8 & Q4):
            // - focusChange == true: hard-veto ALL 3 bands (L, M, H).
            // - !focusChange && isDefocused: hard-veto M/H always; L allowed ONLY in BokehMode::LowBandCellOnly.
            // - !focusChange && !isDefocused (sharp or curved smooth sky): focusGate = 1.0 on all bands.
            bool bandFocusVeto = focusChange || (isDefocused && (b >= 1 || !bokehAllowL));
            float focusGate = bandFocusVeto ? 0.0 : 1.0;

            float bandWSum = 0.0;
            float wFinal[3];
            wFinal[0] = 0.0; wFinal[1] = 0.0; wFinal[2] = 0.0;
            int ancRefIdx = int(round(u_anchor_ref_idx));
            bool ancBarred = (ancRefIdx >= 0) && (texelFetch(cell_drift, cell, 0).y > 0.5);
            for (int k = 0; k < K; ++k)
            {
                vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).xy;
                float mGate = (b == 0) ? mg.x : mg.y;
                if (k == ancRefIdx && ancBarred)
                    mGate = 0.0;
                wFinal[k] = solvedW[k] * mGate * focusGate;
                bandWSum += wFinal[k];
            }
            wOut = vec4(wFinal[0], wFinal[1], wFinal[2], 0.0);
            if (bandWSum > TD_NUM_FLOOR)
                activeBandBits |= (1 << b);
            cellWeightSumAll += bandWSum;
            cellResEnergy += C[0][0];
        }
    }

    bandW[b] = wOut;
    if (b == 0)
        diagOut.w = cellStdCleanY;
    else if (b == 2)
        diagOut.w = bandU[2];
    imageStore(cell_diag_c, ivec2(b * TD_CW + cell.x, cell.y), diagOut);
}

// Layer 2/3 grain-leak transfer budget (refs/111 s7.3 & TdGuardPort::kMinVarianceRetention = 0.98^2):
// Mirrors TdV2ReferenceEngine.h strictly coarse-to-fine (L -> M -> H) when estimator != A2_RoundingCells (1):
// - Raw no-split path (u_has_clean_split < 0.5): alphaLeak = 1.0.
// - Split path with minor leak (cellOwnGrainVar > cellRetGVar): alphaLeak = sigmaLeak / (sigmaOwn + sigmaLeak).
// - Split path with dominant leak (cellRetGVar >= cellOwnGrainVar): alphaLeak = 0.0 (A1 + Layer 4 guard govern).
if (!isFace && estMode != 1 && cellRetGVar > TD_NUM_FLOOR)
{
    float alphaLeak = 0.0;
    if (u_has_clean_split < 0.5)
    {
        alphaLeak = 1.0;
    }
    else if (cellOwnGrainVar > cellRetGVar)
    {
        float sigmaLeak = sqrt(cellRetGVar);
        float sigmaOwn = sqrt(cellOwnGrainVar);
        float sigmaTotal = sigmaOwn + sigmaLeak;
        alphaLeak = (sigmaTotal > TD_NUM_FLOOR) ? (sigmaLeak / sigmaTotal) : 0.0;
    }
    if (alphaLeak > TD_NUM_FLOOR)
    {
        float remGrainBudget = (1.0 - sqrt(TD_MIN_VAR_RETENTION)) / sqrt(2.0);
        activeBandBits = 0;
        cellWeightSumAll = 0.0;
        for (int b = 0; b < 3; ++b)
        {
            float bandWSum = bandW[b].x + bandW[b].y + bandW[b].z;
            float bandLossPerUnitW = alphaLeak * td_band_origin(b);
            if (bandLossPerUnitW > TD_NUM_FLOOR && bandWSum > TD_NUM_FLOOR)
            {
                float maxBandW = remGrainBudget / bandLossPerUnitW;
                if (bandWSum >= maxBandW)
                {
                    float scale = (maxBandW > 0.0) ? (maxBandW / bandWSum) : 0.0;
                    bandW[b].xyz *= scale;
                    bandWSum = (maxBandW > 0.0) ? maxBandW : 0.0;
                    remGrainBudget = 0.0;
                }
                else
                {
                    remGrainBudget = max(0.0, remGrainBudget - bandLossPerUnitW * bandWSum);
                }
            }
            if (bandWSum > TD_NUM_FLOOR)
                activeBandBits |= (1 << b);
            cellWeightSumAll += bandWSum;
        }
    }
}

// SOTA-7 (U7 gradual fadeKeep): taper recurrent Carry (role == 2) weight strictly AFTER grain-budget scaling
// so grain-budget normalization never cancels fadeKeep (u_fade_keep == 1.0 -> bit-identical).
if (u_fade_keep < 1.0)
{
    activeBandBits = 0;
    cellWeightSumAll = 0.0;
    for (int b = 0; b < 3; ++b)
    {
        if (K > 0 && td_ref_role(0) == 2) bandW[b].x *= u_fade_keep;
        if (K > 1 && td_ref_role(1) == 2) bandW[b].y *= u_fade_keep;
        if (K > 2 && td_ref_role(2) == 2) bandW[b].z *= u_fade_keep;
        float bandWSum = bandW[b].x + bandW[b].y + bandW[b].z;
        if (bandWSum > TD_NUM_FLOOR)
            activeBandBits |= (1 << b);
        cellWeightSumAll += bandWSum;
    }
}

midWeightSum = bandW[1].x + bandW[1].y + bandW[1].z;
bandW[0].w = cellMeanCleanY;
bandW[1].w = bandU[0];
bandW[2].w = bandU[1];
for (int b = 0; b < 3; ++b)
    imageStore(cell_weights, ivec2(b * TD_CW + cell.x, cell.y), bandW[b]);

// Populate U19 TdMapsView 12 channels across 3 RGBA32F slices (plane p at y = p * TD_CH + cell.y).
// UPG-BLKR-3: Write real measured kDrift from cell_drift texture into slice2.z (no longer hardcoded 0.0!).
float measuredDriftPx = (u_has_cell_drift > 0.5) ? max(0.0, texelFetch(cell_drift, cell, 0).x) : 0.0;
vec4 slice0 = vec4(
    (activeBandBits != 0) ? 1.0 : 0.0,
    clamp(cellWeightSumAll / 3.0, 0.0, 1.0),
    clamp(midWeightSum, 0.0, 1.0),
    cellResEnergy / 3.0);
vec4 slice1 = vec4(
    spatialDetailRms,
    clamp(cellWeightSumAll / 3.0, 0.0, 1.0),
    sqrt(max(0.0, cellRetGVar)),
    sqrt(max(0.0, u_a2_total_var * cellStepScaleSq)));
vec4 slice2 = vec4(1.0, 0.0, measuredDriftPx, float(activeBandBits));

imageStore(td_maps, ivec2(cell.x, cell.y), slice0);
imageStore(td_maps, ivec2(cell.x, TD_CH + cell.y), slice1);
imageStore(td_maps, ivec2(cell.x, 2 * TD_CH + cell.y), slice2);
imageStore(cell_diag_flags, cell, vec4(
    (focusChange || isDefocused) ? 1.0 : 0.0,
    cellNonFinite ? 1.0 : 0.0,
    cellSingular ? 1.0 : 0.0,
    cellSingleRefA2 ? 1.0 : 0.0));
)GLSL";

	inline std::string BuildPass2MergeAndSolveBody()
	{
		return std::string(kPass2MergeAndSolvePart1) + kPass2MergeAndSolvePart2;
	}

	// ---- PASS 1-ANC (UPG-BLKR-3 / H-4): GPU Per-Cell Held-Out Structure Check on Anchored Carry ----
	// Dispatch: TD_CW x TD_CH workgroups of 16x16 threads (1 WG per cell, strided for any TD_CELL_PX).
	// Mirrors CPU TdV2ReferenceEngine.h held-out check:
	//   - Checks persistent anchor_barred (drift > kAnchorDriftBarPx or prior held-out failure awaiting next peak).
	//   - Computes mid-band NCC (invariant to detail loss) and coarse-band mean diff vs kappa * sqrt(U_L).
	//   - Zeroes anchor ref motion_gate on failure and latches anchor_barred = 1.0 until next confirmed peak.
	constexpr const char* kPassAnchorHeldOutHeader = R"GLSL(
shared vec4 td_anc_s1[256];
shared vec2 td_anc_s2[256];
shared float td_anc_mu[4];
)GLSL";

	constexpr const char* kPassAnchorHeldOutCheck = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;
int ancIdx = int(round(u_anchor_ref_idx));
int ax0 = int(round(u_ax0)), ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1)), ay1 = int(round(u_ay1));
ivec4 halfActive = ivec4(int(round(u_hax0)), int(round(u_hay0)), int(round(u_hax1)), int(round(u_hay1)));

vec4 mgOrig = texelFetch(motion_gate_in, ivec2(ancIdx * TD_CW + cell.x, cell.y), 0);
bool alreadyBarred = (texelFetch(anchor_barred_in, cell, 0).x > 0.5);
bool activeCell = (!alreadyBarred) && (mgOrig.x > 0.0 || mgOrig.y > 0.0);

vec4 loc1 = vec4(0.0); // (sumCurM, sumAncM, sumCurL, sumAncL)
float locCnt = 0.0;
if (activeCell)
{
    for (int dy = ly; dy < TD_CELL_PX; dy += 16)
    {
        int py = cell.y * TD_CELL_PX + dy;
        if (py < ay0 || py >= ay1) continue;
        for (int dx = lx; dx < TD_CELL_PX; dx += 16)
        {
            int px = cell.x * TD_CELL_PX + dx;
            if (px < ax0 || px >= ax1) continue;
            ivec2 p = ivec2(px, py);
            uint pf = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
            if ((td_unpack_ref_flags(pf, ancIdx) & 1) == 0) continue;
            vec3 cb = td_fetch_cur_band(p, halfActive);
            vec3 ab = td_fetch_ref_band(ancIdx, p, halfActive);
            loc1 += vec4(cb.y, ab.y, cb.x, ab.x);
            locCnt += 1.0;
        }
    }
}
td_anc_s1[tid] = loc1;
td_anc_s2[tid] = vec2(locCnt, 0.0);
memoryBarrierShared();
barrier();
for (int s = 128; s > 0; s >>= 1)
{
    if (tid < s)
    {
        td_anc_s1[tid] += td_anc_s1[tid + s];
        td_anc_s2[tid].x += td_anc_s2[tid + s].x;
    }
    memoryBarrierShared();
    barrier();
}
if (tid == 0)
{
    float n = td_anc_s2[0].x;
    float invN = (n > 0.5) ? (1.0 / n) : 0.0;
    td_anc_mu[0] = td_anc_s1[0].x * invN;
    td_anc_mu[1] = td_anc_s1[0].y * invN;
    td_anc_mu[2] = td_anc_s1[0].z * invN;
    td_anc_mu[3] = td_anc_s1[0].w * invN;
}
memoryBarrierShared();
barrier();

float totN = td_anc_s2[0].x;
vec4 locCov = vec4(0.0); // (varCurM, varAncM, covM, 0)
if (activeCell && totN > 0.5)
{
    float muCurM = td_anc_mu[0];
    float muAncM = td_anc_mu[1];
    for (int dy = ly; dy < TD_CELL_PX; dy += 16)
    {
        int py = cell.y * TD_CELL_PX + dy;
        if (py < ay0 || py >= ay1) continue;
        for (int dx = lx; dx < TD_CELL_PX; dx += 16)
        {
            int px = cell.x * TD_CELL_PX + dx;
            if (px < ax0 || px >= ax1) continue;
            ivec2 p = ivec2(px, py);
            uint pf = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
            if ((td_unpack_ref_flags(pf, ancIdx) & 1) == 0) continue;
            float dc = td_fetch_cur_band(p, halfActive).y - muCurM;
            float da = td_fetch_ref_band(ancIdx, p, halfActive).y - muAncM;
            locCov += vec4(dc * dc, da * da, dc * da, 0.0);
        }
    }
}
td_anc_s1[tid] = locCov;
memoryBarrierShared();
barrier();
for (int s = 128; s > 0; s >>= 1)
{
    if (tid < s) td_anc_s1[tid] += td_anc_s1[tid + s];
    memoryBarrierShared();
    barrier();
}

if (tid == 0)
{
    if (alreadyBarred)
    {
        imageStore(anchor_barred_out, cell, vec4(1.0, 0.0, 0.0, 0.0));
        return;
    }
    if (!activeCell)
    {
        imageStore(anchor_barred_out, cell, vec4(0.0));
        return;
    }
    bool ok = (totN > 0.5);
    if (ok)
    {
        float invN = 1.0 / totN;
        float varCurM = td_anc_s1[0].x * invN;
        float varAncM = td_anc_s1[0].y * invN;
        float covM = td_anc_s1[0].z * invN;
        if (varCurM > u_mid_noise_floor && varAncM > u_mid_noise_floor)
        {
            float ncc = covM / sqrt(varCurM * varAncM);
            float nEffM = max(1.0, totN / (4.0 * 3.14159265358979323846 * 1.0 * 1.0));
            float nccMin = min(0.5, 3.0 / sqrt(nEffM));
            if (td_is_bad(ncc) || ncc < nccMin)
                ok = false;
        }
        vec4 pt = texelFetch(cell_prior, cell, 0);
        float sc = (u_cell_prior_has_step_scale > 0.5 && !td_is_bad(pt.z) && pt.z > 0.0)
            ? clamp(pt.z, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX) : 1.0;
        float coarseVar = max(u_a2_total_var * sc * sc, u_base_quant_var) * TD_GAIN_LOW;
        float coarseLimit = u_kappa * sqrt(max(0.0, coarseVar));
        if (abs(td_anc_mu[3] - td_anc_mu[2]) > coarseLimit)
            ok = false;
    }
    imageStore(anchor_barred_out, cell, vec4(ok ? 0.0 : 1.0, 0.0, 0.0, 0.0));
}
)GLSL";

	// ---- PASS 1-DRIFT (UPG-BLKR-3 / UPG-HIGH-2): Real GPU kDrift Measurement & Bar Update ----
	// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8.
	// Mode 0 (u_drift_mode == 0): clear cell_drift to 0.0 (non-peak frames).
	// Mode 1 (u_drift_mode == 1): direct peak-to-peak vs composed trail vector difference.
	// Mode 2 (u_drift_mode == 2): seeded +/- 2 px bilinear sub-pixel search around exact float composed trail ("drift: reduced").
	constexpr const char* kPassAnchorDriftAudit = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;
int mode = int(round(u_drift_mode));
float curBarred = texelFetch(anchor_barred_in, cell, 0).x;
if (mode == 0)
{
    imageStore(cell_drift_out, cell, vec4(0.0, curBarred, 0.0, 0.0));
    return;
}

vec4 compMv = texelFetch(anchor_comp_mv, cell, 0);
float drift = 0.0;
bool valid = (compMv.z > 0.5) && !td_is_bad(compMv.x) && !td_is_bad(compMv.y);

if (valid && mode == 1)
{
    vec4 dirMv = texelFetch(direct_p2p_mv, cell, 0);
    if (dirMv.z > 0.5 && !td_is_bad(dirMv.x) && !td_is_bad(dirMv.y))
        drift = length(dirMv.xy - compMv.xy);
}
else if (valid && mode == 2)
{
    int ax0 = int(round(u_ax0)), ay0 = int(round(u_ay0));
    int ax1 = int(round(u_ax1)), ay1 = int(round(u_ay1));
    int x0 = max(ax0, cell.x * TD_CELL_PX);
    int y0 = max(ay0, cell.y * TD_CELL_PX);
    int x1 = min(ax1, (cell.x + 1) * TD_CELL_PX);
    int y1 = min(ay1, (cell.y + 1) * TD_CELL_PX);
    float sadGrid[25];
    int bestDx = 0, bestDy = 0;
    float bestSad = 1e30;
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            float sad = 0.0;
            float cnt = 0.0;
            for (int y = y0; y < y1; y += 2)
            {
                float sy = float(y) + compMv.y + float(dy);
                if (sy < float(ay0) || sy > float(ay1 - 1)) continue;
                int iy0 = int(floor(sy));
                int iy1 = min(ay1 - 1, iy0 + 1);
                float fy = sy - float(iy0);
                for (int x = x0; x < x1; x += 2)
                {
                    float sx = float(x) + compMv.x + float(dx);
                    if (sx < float(ax0) || sx > float(ax1 - 1)) continue;
                    int ix0 = int(floor(sx));
                    int ix1 = min(ax1 - 1, ix0 + 1);
                    float fx = sx - float(ix0);
                    float a00 = texelFetch(anchor_clean, ivec2(ix0, iy0), 0).x;
                    float a10 = texelFetch(anchor_clean, ivec2(ix1, iy0), 0).x;
                    float a01 = texelFetch(anchor_clean, ivec2(ix0, iy1), 0).x;
                    float a11 = texelFetch(anchor_clean, ivec2(ix1, iy1), 0).x;
                    float aVal = mix(mix(a00, a10, fx), mix(a01, a11, fx), fy);
                    float cVal = texelFetch(cur_clean, ivec2(x, y), 0).x;
                    if (!td_is_bad(aVal) && !td_is_bad(cVal))
                    {
                        sad += abs(cVal - aVal);
                        cnt += 1.0;
                    }
                }
            }
            float mSad = (cnt > 0.5) ? (sad / cnt) : 1e30;
            sadGrid[(dy + 2) * 5 + (dx + 2)] = mSad;
            if (mSad < bestSad) { bestSad = mSad; bestDx = dx; bestDy = dy; }
        }
    }
    float subDx = float(bestDx), subDy = float(bestDy);
    if (bestDx > -2 && bestDx < 2)
    {
        float cL = sadGrid[(bestDy + 2) * 5 + (bestDx + 1)];
        float c0 = sadGrid[(bestDy + 2) * 5 + (bestDx + 2)];
        float cR = sadGrid[(bestDy + 2) * 5 + (bestDx + 3)];
        float den = cL - 2.0 * c0 + cR;
        if (!td_is_bad(den) && den > TD_NUM_FLOOR)
            subDx += clamp(0.5 * (cL - cR) / den, -0.5, 0.5);
    }
    if (bestDy > -2 && bestDy < 2)
    {
        float cU = sadGrid[(bestDy + 1) * 5 + (bestDx + 2)];
        float c0 = sadGrid[(bestDy + 2) * 5 + (bestDx + 2)];
        float cD = sadGrid[(bestDy + 3) * 5 + (bestDx + 2)];
        float den = cU - 2.0 * c0 + cD;
        if (!td_is_bad(den) && den > TD_NUM_FLOOR)
            subDy += clamp(0.5 * (cU - cD) / den, -0.5, 0.5);
    }
    drift = length(vec2(subDx, subDy));
}

imageStore(cell_drift_out, cell, vec4(drift, curBarred, 0.0, 0.0));
imageStore(anchor_barred_out, cell, vec4((drift > TD_ANCHOR_DRIFT_BAR_PX) ? 1.0 : 0.0, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 3A: Spatial-Only Weight Interpolation + 3-Sigma Huber Taper + Pixel-Domain Bound ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: curBands, refBands[0..2], packed ref_flags (R16), cell_weights, cell_diag_c.
	// Output: delta_out (w x h RG32F: .x = delta, .y = 1.0 when pixel-domain bound engaged).
	constexpr const char* kPass3BlendHeader = R"GLSL(
shared vec2 td_sh_mh[18][18];

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

float td_a2_band_peak_sq(int b)
{
    return (b == 1) ? TD_A2_PEAK_MID_SQ : 1.0;
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

// SOTA #5: Grain-free 3x3 neighborhood 3-sigma C^1 soft clamp (matches CPU SoftClampToNeighborhood3Sigma 1:1).
float td_soft_clamp_nbr_3sigma(float refVal, float mu, float sig)
{
    if (td_is_bad(refVal) || td_is_bad(mu) || td_is_bad(sig))
        return td_is_bad(mu) ? 0.0 : mu;
    if (!(sig > TD_NUM_FLOOR))
        return mu;
    float gamma = sqrt(TD_HUBER_KNEE_SQ);
    float r0 = sqrt(8.0) * sig;
    float d = refVal - mu;
    float absD = abs(d);
    if (absD <= r0)
        return refVal;
    float shoulder = (gamma - sqrt(8.0)) * sig;
    float excess = absD - r0;
    float clampedMag = r0 + shoulder * (excess / (excess + shoulder));
    return mu + ((d >= 0.0) ? clampedMag : -clampedMag);
}
)GLSL";

	constexpr const char* kPass3BlendAndBound = R"GLSL(
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
bool enableNbrClamp = (u_enable_nbr_soft_clamp > 0.5);

// Workgroup-uniform cooperative 18x18 LDS tile load of curBands (M, H) BEFORE any early return (FXC cs_5_0 barrier safe!).
if (enableNbrClamp)
{
    int xMin = ax0;
    int xMax = max(ax0, ax1 - 1);
    int yMin = ay0;
    int yMax = max(ay0, ay1 - 1);
    int tid = ly * 16 + lx;
    for (int idx = tid; idx < 324; idx += 256)
    {
        int ty = idx / 18;
        int tx = idx % 18;
        int gx = baseX - 1 + tx;
        int gy = baseY - 1 + ty;
        int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_W - 1);
        int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_H - 1);
#if TD_COMPACT_BANDS
        td_sh_mh[ty][tx] = texelFetch(cur_bands_mh, ivec2(sx, sy), 0).xy;
#else
        td_sh_mh[ty][tx] = texelFetch(cur_bands, ivec2(sx, sy), 0).yz;
#endif
    }
}
memoryBarrierShared();
barrier();

if (p.x >= TD_W || p.y >= TD_H)
    return;

if (p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(delta_out, p, vec4(0.0));
    return;
}

int K = int(round(u_k));
bool enableHuber = (u_enable_per_corner_huber > 0.5);
int bx0 = int(round(u_bx0));
int by0 = int(round(u_by0));
int bx1 = int(round(u_bx1));
int by1 = int(round(u_by1));
ivec4 halfActive = ivec4(int(round(u_hax0)), int(round(u_hay0)), int(round(u_hax1)), int(round(u_hay1)));

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
float rawStepScale = (u_cell_prior_has_step_scale > 0.5) ? texelFetch(cell_prior, ivec2(cx, cy), 0).z : 1.0;
float cellStepScale = (td_is_bad(rawStepScale) || !(rawStepScale > 0.0)) ? 0.0 : clamp(rawStepScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX);
float cellStepScaleSq = cellStepScale * cellStepScale;

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

// Compute spatial (or SOTA #4 clean-luma-guided edge-aware) 2x2 corner interpolation weights once per pixel.
float a00 = (1.0 - tx) * (1.0 - ty);
float a10 = tx * (1.0 - ty);
float a01 = (1.0 - tx) * ty;
float a11 = tx * ty;
if (u_enable_edge_aware_upsample > 0.5)
{
    float curCleanY = curB.x + curB.y + curB.z;
    float baseQVar = 1.0 / 12.0;
    float a2PxVar = (u_a2_valid > 0.5) ? (u_a2_total_var * cellStepScaleSq) : 0.0;
    float effNoiseFloor = sqrt(max(baseQVar, a2PxVar));
    float m00 = texelFetch(cell_weights, ivec2(x0, y0), 0).w;
    float m10 = texelFetch(cell_weights, ivec2(x1, y0), 0).w;
    float m01 = texelFetch(cell_weights, ivec2(x0, y1), 0).w;
    float m11 = texelFetch(cell_weights, ivec2(x1, y1), 0).w;
    float s00 = max(texelFetch(cell_diag_c, ivec2(x0, y0), 0).w, effNoiseFloor);
    float s10 = max(texelFetch(cell_diag_c, ivec2(x1, y0), 0).w, effNoiseFloor);
    float s01 = max(texelFetch(cell_diag_c, ivec2(x0, y1), 0).w, effNoiseFloor);
    float s11 = max(texelFetch(cell_diag_c, ivec2(x1, y1), 0).w, effNoiseFloor);
    float dL00 = curCleanY - m00;
    float dL10 = curCleanY - m10;
    float dL01 = curCleanY - m01;
    float dL11 = curCleanY - m11;
    float sw00 = a00 * exp(-(dL00 * dL00) / (2.0 * s00 * s00));
    float sw10 = a10 * exp(-(dL10 * dL10) / (2.0 * s10 * s10));
    float sw01 = a01 * exp(-(dL01 * dL01) / (2.0 * s01 * s01));
    float sw11 = a11 * exp(-(dL11 * dL11) / (2.0 * s11 * s11));
    float swSum = sw00 + sw10 + sw01 + sw11;
    if (swSum > TD_NUM_FLOOR)
    {
        float invSw = 1.0 / swSum;
        a00 = sw00 * invSw;
        a10 = sw10 * invSw;
        a01 = sw01 * invSw;
        a11 = sw11 * invSw;
    }
}

float cellU[3];
cellU[0] = max(0.0, texelFetch(cell_weights, ivec2(1 * TD_CW + cx, cy), 0).w);
cellU[1] = max(0.0, texelFetch(cell_weights, ivec2(2 * TD_CW + cx, cy), 0).w);
cellU[2] = max(0.0, texelFetch(cell_diag_c,  ivec2(2 * TD_CW + cx, cy), 0).w);

float totalDelta = 0.0;
float boundedDelta = 0.0;
float sumBandVar = 0.0;
float boundEngaged = 0.0;
for (int b = 0; b < 3; ++b)
{
    float curV = td_band_comp(curB, b);
    float nbrMean = curV;
    float nbrSigma = 0.0;
    bool applyNbrClamp = enableNbrClamp && (b >= 1);
    if (applyNbrClamp)
    {
        float sumS = 0.0;
        float sumSq = 0.0;
        for (int dy = 0; dy <= 2; ++dy)
        {
            for (int dx = 0; dx <= 2; ++dx)
            {
                vec2 mv = td_sh_mh[ly + dy][lx + dx];
                float v = (b == 1) ? mv.x : mv.y;
                float s = v - curV;
                sumS += s;
                sumSq += s * s;
            }
        }
        float meanS = sumS / 9.0;
        nbrMean = curV + meanS;
        nbrSigma = sqrt(max(0.0, (sumSq / 9.0) - meanS * meanS));
    }
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

        float refV = td_band_comp(refB[k], b);
        if (applyNbrClamp)
            refV = td_soft_clamp_nbr_3sigma(refV, nbrMean, nbrSigma);
        float diff = curV - refV;
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
        if (td_is_bad(c00) || td_is_bad(c10) || td_is_bad(c01) || td_is_bad(c11))
            continue;
        if (enableHuber && (td_is_bad(q00) || td_is_bad(q10) || td_is_bad(q01) || td_is_bad(q11)))
            continue;

        // Per-corner 3-sigma Huber taper before bilinear combine when enabled (exact twin of CPU SpatialBilinear[Huber]Weight).
        float t00 = c00;
        float t10 = c10;
        float t01 = c01;
        float t11 = c11;
        if (enableHuber)
        {
            float diffSq = diff * diff;
            t00 *= td_huber_taper(diffSq, q00);
            t10 *= td_huber_taper(diffSq, q10);
            t01 *= td_huber_taper(diffSq, q01);
            t11 *= td_huber_taper(diffSq, q11);
        }
        float wEff = a00 * t00 + a10 * t10 + a01 * t01 + a11 * t11;
        if (wEff <= 0.0)
            continue;
        bandDelta -= wEff * diff;
    }

    float bandVar = cellU[b];
    sumBandVar += bandVar;
    float clampedBand = bandDelta;
    if (abs(bandDelta) > 0.0)
    {
        float bound = u_pixel_bound_kappa * sqrt(max(0.0, bandVar));
        clampedBand = clamp(bandDelta, -bound, bound);
        if (!inFull8x8)
        {
            boundEngaged = 1.0;
            bandDelta = clampedBand;
        }
    }
    totalDelta += bandDelta;
    boundedDelta += clampedBand;
}

// TD-U3 (H-1 / Owner D3): Joint cross-band limit |sum_j Delta_j(p)| <= kappa * sqrt(sum_j U_j(block)).
if (u_enable_joint_bound > 0.5)
{
    float jointBound = u_pixel_bound_kappa * sqrt(max(0.0, sumBandVar));
    if (abs(boundedDelta) > 0.0)
        boundedDelta = clamp(boundedDelta, -jointBound, jointBound);
    if (!inFull8x8 && abs(totalDelta) > 0.0)
    {
        boundEngaged = 1.0;
        totalDelta = clamp(totalDelta, -jointBound, jointBound);
    }
}

float outX = td_is_bad(totalDelta) ? 0.0 : totalDelta;
float outY = inFull8x8 ? (td_is_bad(boundedDelta) ? 0.0 : boundedDelta) : boundEngaged;
imageStore(delta_out, p, vec4(outX, outY, 0.0, 0.0));
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

float td_project_coeff(float sentCoeff, float blendCoeff, float stepDelta, bool strictMode)
{
    if (td_is_bad(sentCoeff) || td_is_bad(blendCoeff) || td_is_bad(stepDelta) || td_is_bad(u_deadzone_phi) ||
        stepDelta <= 0.0 || u_deadzone_phi < 0.0 || u_deadzone_phi >= 1.0)
        return td_is_bad(sentCoeff) ? 0.0 : sentCoeff;

    float theta = (1.0 - u_deadzone_phi) * stepDelta;
    float absSent = abs(sentCoeff);
    // Keep quantization level in float (avoids 32-bit int overflow on very small stepDelta).
    float level = 0.0;
    if (absSent >= theta)
    {
        float mag = floor(absSent / stepDelta + u_deadzone_phi);
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
        lo = (level - u_deadzone_phi) * stepDelta;
        hi = (level + 1.0 - u_deadzone_phi) * stepDelta;
    }
    else
    {
        lo = (level - 1.0 + u_deadzone_phi) * stepDelta;
        hi = (level + u_deadzone_phi) * stepDelta;
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

int pCx = min(TD_CW - 1, max(0, p.x) / TD_CELL_PX);
int pCy = min(TD_CH - 1, max(0, p.y) / TD_CELL_PX);
bool isFacePx = (p.x < TD_W && p.y < TD_H) && (texelFetch(cell_focus, ivec2(pCx, pCy), 0).w > 0.5);

if (!blockInInterior)
{
    if (p.x < TD_W && p.y < TD_H)
    {
        vec2 d = texelFetch(delta_in, p, 0).xy;
        if (td_is_bad(d.x))
            d = vec2(0.0, 2.0);
        else if (isFacePx)
            d = vec2(0.0);
        imageStore(delta_out, p, vec4(d, 0.0, 0.0));
    }
    return;
}

vec2 dIn = texelFetch(delta_in, p, 0).xy;
float rawDIn = dIn.x;
float boundedDIn = (isFacePx || td_is_bad(dIn.y)) ? 0.0 : dIn.y;
bool badDIn = td_is_bad(rawDIn);
float dVal = (isFacePx || badDIn) ? 0.0 : rawDIn;
td_s_nz[tid] = ((dVal != 0.0) ? 1u : 0u) | (badDIn ? 2u : 0u) | ((boundedDIn != 0.0) ? 4u : 0u);
memoryBarrierShared();
barrier();
for (int stride = 32; stride > 0; stride >>= 1)
{
    if (tid < stride)
        td_s_nz[tid] = td_s_nz[tid] | td_s_nz[tid + stride];
    memoryBarrierShared();
    barrier();
}

// Keep all barriers in workgroup-uniform flow control (FXC cs_5_0 X3663 safe: zero early returns after groupshared reads!).
// Barrier after reading td_s_nz[0] prevents WAR data race before threads overwrite td_s_nz[tid] with cpBits below!
uint nzFlags = td_s_nz[0];
memoryBarrierShared();
barrier();
bool blkBadIn = ((nzFlags & 2u) != 0u);
bool blkHasDelta = ((nzFlags & 1u) != 0u);
bool blkActive = (blkHasDelta || ((nzFlags & 4u) != 0u)) && !blkBadIn;

// Speedup 7 empty-block fetch skip: defer cur_clean & pred_clean fetches unless block has active non-zero delta.
bool hasExplicitPb = (u_has_explicit_pb > 0.5);
float rawC = 0.0;
float rawP = 0.0;
uint cpBits = 0u; // bit 0 (1u) = non-finite cur/pred, bit 1 (2u) = invalid ref0 validity when using stand-in P_B
if (blkActive)
{
    rawC = texelFetch(cur_clean, p, 0).x;
    rawP = texelFetch(pred_clean, p, 0).x;
    if (td_is_bad(rawC) || td_is_bad(rawP))
        cpBits |= 1u;
    if (!hasExplicitPb)
    {
        uint fl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
        if ((fl & 1u) == 0u)
            cpBits |= 2u;
    }
}
// MEDIUM-6 + M2: NEVER substitute 0.0 for non-finite pred_clean/cur_clean; skip POCS if stand-in ref0 pixel invalid.
td_s_nz[tid] = cpBits;
memoryBarrierShared();
barrier();
for (int stride = 32; stride > 0; stride >>= 1)
{
    if (tid < stride)
        td_s_nz[tid] = td_s_nz[tid] | td_s_nz[tid + stride];
    memoryBarrierShared();
    barrier();
}

uint cpFlags = td_s_nz[0];
#if defined(TD_POCS_HAS_STEP_SCALE) && TD_POCS_HAS_STEP_SCALE
int bCx = min(TD_CW - 1, max(0, bx) / TD_CELL_PX);
int bCy = min(TD_CH - 1, max(0, by) / TD_CELL_PX);
float rawBlockScale = (u_cell_prior_has_step_scale > 0.5) ? texelFetch(cell_prior, ivec2(bCx, bCy), 0).z : 1.0;
bool blkBadScale = td_is_bad(rawBlockScale) || !(rawBlockScale > 0.0);
float blockStepDelta = blkBadScale ? 0.0 : (u_step_delta * clamp(rawBlockScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX));
bool blkBad = blkBadIn || ((cpFlags & 1u) != 0u) || blkBadScale;
#else
float blockStepDelta = u_step_delta;
bool blkBad = blkBadIn || ((cpFlags & 1u) != 0u);
#endif
bool blkInvalidRef0 = ((cpFlags & 2u) != 0u);
bool runPocs = blkHasDelta && !blkBad && !blkInvalidRef0;
float cVal = runPocs ? rawC : 0.0;
float pVal = runPocs ? rawP : 0.0;
td_s_sent[tid] = runPocs ? (cVal - pVal) : 0.0;
td_s_blend[tid] = runPocs ? ((cVal + dVal) - pVal) : 0.0;
memoryBarrierShared();
barrier();

// Forward DCT-II of sent residual once (invariant across POCS iterations; barriers unconditional for cs_5_0).
{
    float rowSum = 0.0;
    if (runPocs)
    {
        for (int x = 0; x < 8; ++x)
            rowSum += td_s_sent[loc.y * 8 + x] * TD_DCT8_BASIS[loc.x * 8 + x];
    }
    td_s_tmp[tid] = rowSum;
    memoryBarrierShared();
    barrier();

    float colSum = 0.0;
    if (runPocs)
    {
        for (int y = 0; y < 8; ++y)
            colSum += td_s_tmp[y * 8 + loc.x] * TD_DCT8_BASIS[loc.y * 8 + y];
    }
    td_s_sent_dct[tid] = colSum;
    memoryBarrierShared();
    barrier();
}

for (int it = 0; it < pocsIters; ++it)
{
    float rowSum = 0.0;
    if (runPocs)
    {
        for (int x = 0; x < 8; ++x)
            rowSum += td_s_blend[loc.y * 8 + x] * TD_DCT8_BASIS[loc.x * 8 + x];
    }
    td_s_tmp[tid] = rowSum;
    memoryBarrierShared();
    barrier();

    float projCoeff = 0.0;
    if (runPocs)
    {
        float blendCoeff = 0.0;
        for (int y = 0; y < 8; ++y)
            blendCoeff += td_s_tmp[y * 8 + loc.x] * TD_DCT8_BASIS[loc.y * 8 + y];
        // Set 1: Quantization-cell coefficient projection (STRICT / CELL) with per-cell AQ/PQ step scale.
        projCoeff = td_project_coeff(td_s_sent_dct[tid], blendCoeff, blockStepDelta, strictMode);
    }
    td_s_blend[tid] = projCoeff;
    memoryBarrierShared();
    barrier();

    // Inverse DCT-II of projected residual (transpose of orthonormal basis).
    float invRow = 0.0;
    if (runPocs)
    {
        for (int u = 0; u < 8; ++u)
            invRow += td_s_blend[loc.y * 8 + u] * TD_DCT8_BASIS[u * 8 + loc.x];
    }
    td_s_tmp[tid] = invRow;
    memoryBarrierShared();
    barrier();

    if (runPocs)
    {
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
    }

    td_s_blend[tid] = runPocs ? ((cVal + dVal) - pVal) : 0.0;
    memoryBarrierShared();
    barrier();
}

if (blkBad || td_is_bad(dVal))
    imageStore(delta_out, p, vec4(0.0, 2.0, 0.0, 0.0));
else if (blkInvalidRef0)
{
    float bOut = isFacePx ? 0.0 : boundedDIn;
    imageStore(delta_out, p, vec4(bOut, (abs(bOut) > 0.0) ? 1.0 : 0.0, 0.0, 0.0));
}
else if (!runPocs || isFacePx)
    imageStore(delta_out, p, vec4(0.0));
else
    imageStore(delta_out, p, vec4(dVal, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 4 (Speedup 4): Fused Recompose + Layer-4 TdGuardPort + Apply in ONE Dispatch! ----
	// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per cell).
	// Direct candOut fine-band blur (zero (cv + dv + og) - rv cancellation, exact twin of CPU outFullBands.H):
	//   ownGrain = recY - curClean;  newClean = curClean + delta;  candOut = newClean + ownGrain;
	//   inFullH = recY - G_fine(recY);  outFullH = candOut - G_fine(candOut);  ownGrainH = ownGrain - G_fine(ownGrain);
	//   addBack = cellAddBack * (inFullH - outFullH).
	// Uses 22x22 LDS halo (TD_RAD_FINE = 3, 12,088 B shmem — reuses halo buffers for tree reduction!).
	// Eliminates inOwnGrainTex_, candOutScratch, guardFineH/V textures and 4 dispatches completely!
	// Mode::Shadow skips imageStore(out_y, ...) completely (caller retains bit-exact receivedY).
	constexpr const char* kPass4FusedApplyGuardHeader = R"GLSL(
shared float td_sh_r[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sh_d[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sh_g[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sh_rh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sh_dh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sh_gh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sh_cov2[256];
shared uint td_sh_gbits[256];
shared float td_sh_gout[2];
)GLSL";

	// Split into Part1 + Part2 to stay well below MSVC's 16,380-byte raw string literal limit (C2026).
	constexpr const char* kPass4FusedApplyGuardPart1 = R"GLSL(
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
// Workgroup-uniform Pass-1 face/non-rigid mask flag from cell_focus.w (H13: strict bit-exact passthrough).
bool cellIsFace = (texelFetch(cell_focus, cell, 0).w > 0.5);

int tileDim = 16 + 2 * TD_RAD_FINE;
int tileN = tileDim * tileDim;
int hN = tileDim * 16;
int subTiles = (TD_CELL_PX + 15) / 16;

float sumOgE = 0.0;
float sumOgFineE = 0.0;
float sumInFineE = 0.0;
float sumOutFineE = 0.0;
float sumInCovOwn = 0.0;
float sumOutCovOwn = 0.0;
float sumCnt = 0.0;
uint pxBits = 0u; // bit 2 (4u) = non-finite input pixel closed; bit 3 (8u) = TransformUnknownBound engaged

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
        // Sanitize non-finite samples upfront so a single NaN/Inf pixel NEVER spreads across the 7x7 fine halo!
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
            if (td_is_bad(rv)) rv = 0.0;
            if (td_is_bad(cv)) cv = 0.0;
            if (td_is_bad(dv)) dv = 0.0;
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
            float rawRec = texelFetch(rec_y, p, 0).x;
            float rawClean = sameCleanRec ? rawRec : texelFetch(cur_clean, p, 0).x;
            bool badRecOrClean = td_is_bad(rawRec) || td_is_bad(rawClean);
            if (badRecOrClean)
                pxBits |= 4u;
            float recV = td_is_bad(rawRec) ? 0.0 : rawRec;
            float cleanV = td_is_bad(rawClean) ? 0.0 : rawClean;
            if (!inAct || cellIsFace)
            {
                // Bars and Pass-1 face-masked cells: bit-exact copy of cur_clean to out_clean/carry_clean and rec_y to out_y.
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
                vec2 dTex = texelFetch(delta_tex, p, 0).xy;
                int dFlags = int(round(dTex.y));
                bool badD = td_is_bad(dTex.x) || ((dFlags & 2) != 0);
                if (badD)
                    pxBits |= 4u;
                if ((dFlags & 1) != 0)
                    pxBits |= 8u;
                float dRaw = (badRecOrClean || badD) ? 0.0 : dTex.x;
                float newClean = cleanV + dRaw;
                float carryCleanVal = (u_fade_keep < 1.0)
                    ? (u_fade_keep * newClean + (1.0 - u_fade_keep) * cleanV)
                    : newClean;

                // Write strictly grain-free out_clean & carry_clean NOW (BLOCKER-2: NEVER receives Layer-4 add-back!).
                imageStore(out_clean, p, vec4(newClean, 0.0, 0.0, 0.0));
                imageStore(carry_clean, p, vec4(carryCleanVal, 0.0, 0.0, 0.0));

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
                {
                    sumOgFineE += ogFineH * ogFineH;
                    sumInCovOwn += inFullH * ogFineH;
                    sumOutCovOwn += outFullH * ogFineH;
                }
                sumInFineE += inFullH * inFullH;
                sumOutFineE += outFullH * outFullH;
                sumCnt += 1.0;

                if (subTiles == 1)
                {
                    regRecY = rv;
                    regCandOut = (badRecOrClean || badD) ? rv : candOut;
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
)GLSL";

	constexpr const char* kPass4FusedApplyGuardPart2 = R"GLSL(
// Step 4: LDS parallel tree reduction reusing halo arrays + td_sh_cov2/td_sh_gbits for covariance and flags.
td_sh_r[tid]    = sumOgE;
td_sh_d[tid]    = sumOgFineE;
td_sh_g[tid]    = sumInFineE;
td_sh_rh[tid]   = sumOutFineE;
td_sh_dh[tid]   = sumCnt;
td_sh_gh[tid]   = sumInCovOwn;
td_sh_cov2[tid] = sumOutCovOwn;
td_sh_gbits[tid] = pxBits;
memoryBarrierShared();
barrier();

for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_sh_r[tid]    += td_sh_r[tid + stride];
        td_sh_d[tid]    += td_sh_d[tid + stride];
        td_sh_g[tid]    += td_sh_g[tid + stride];
        td_sh_rh[tid]   += td_sh_rh[tid + stride];
        td_sh_dh[tid]   += td_sh_dh[tid + stride];
        td_sh_gh[tid]   += td_sh_gh[tid + stride];
        td_sh_cov2[tid] += td_sh_cov2[tid + stride];
        td_sh_gbits[tid] |= td_sh_gbits[tid + stride];
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
    float totInCovOwn = td_sh_gh[0];
    float totOutCovOwn = td_sh_cov2[0];

    float addBack = 0.0;
    int gFlags = int(td_sh_gbits[0] & 12u); // Preserve bit 2 (4 = non-finite) and bit 3 (8 = pixel bound engaged)

    if (!cellIsFace && totCnt > 0.5)
    {
        float rawGv = texelFetch(cell_prior, cell, 0).x;
        float cellRetGVar = (!td_is_bad(rawGv) && rawGv > 0.0) ? rawGv : 0.0;
        float invC = 1.0 / totCnt;

        // Mirrors TdV2ReferenceEngine.h Layer-4 guard:
        // - In H2-luma no-split mode (!hasCleanSplit), ALWAYS evaluate guard against inFullH energy.
        // - In split mode (hasCleanSplit), skip only when both ownGrain and retainedGrainVar are zero.
        if (!hasCleanSplit || totOgE * invC > TD_NUM_FLOOR || cellRetGVar > TD_NUM_FLOOR)
        {
            float leakedFineE = cellRetGVar * TD_GAIN_FINE;
            float inFineMean = totInFineE * invC;
            float ogFineMean = totOgFineE * invC;
            float targetFineE = inFineMean;
            if (hasCleanSplit)
            {
                // ownGrain and leaked own grain inside curClean are 100% positively correlated fractions
                // of the same grain field G, so their fine-band std-devs add linearly (not in quadrature).
                float targetStd = sqrt(max(0.0, ogFineMean)) + sqrt(max(0.0, leakedFineE));
                targetFineE = min(inFineMean, targetStd * targetStd);
            }

            // Texture-immune own-grain fine-band covariance check when leaked grain is reported.
            float curOutFineE = totOutFineE * invC;
            if (hasCleanSplit && ogFineMean > TD_NUM_FLOOR && cellRetGVar > TD_NUM_FLOOR &&
                totInCovOwn > TD_NUM_FLOOR)
            {
                float covRatio = (totOutCovOwn > 0.0) ? (totOutCovOwn / totInCovOwn) : 0.0;
                float covImpliedOutE = targetFineE * covRatio * covRatio;
                if (covImpliedOutE < curOutFineE)
                    curOutFineE = covImpliedOutE;
            }

            gFlags |= 2; // evaluated
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
bool deferSmoothGuard = (u_smooth_guard > 0.5);
bool applyAddBack = (!deferSmoothGuard) && (!cellIsFace) && ((cellGFlags & 1) != 0) && (cellAddBack > 0.0);

if (subTiles == 1)
{
    if (regInCellPx)
    {
        float outVal = regRecY;
        if (regInActive && !cellIsFace)
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
                    if (td_is_bad(rVal)) rVal = 0.0;
                    if (td_is_bad(cv)) cv = 0.0;
                    if (td_is_bad(dv)) dv = 0.0;
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
            if (inAct && !cellIsFace)
            {
                float rawR = texelFetch(rec_y, p, 0).x;
                float rawC = sameCleanRec ? rawR : texelFetch(cur_clean, p, 0).x;
                float rawD = texelFetch(delta_tex, p, 0).x;
                bool badPx = td_is_bad(rawR) || td_is_bad(rawC) || td_is_bad(rawD);
                float rv = td_is_bad(rawR) ? 0.0 : rawR;
                float cv = td_is_bad(rawC) ? 0.0 : rawC;
                float dv = badPx ? 0.0 : rawD;
                float outVal = badPx ? rv : ((cv + dv) + (rv - cv));
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

	inline std::string BuildPass4FusedApplyGuardBody()
	{
		return std::string(kPass4FusedApplyGuardPart1) + kPass4FusedApplyGuardPart2;
	}

	// ---- PASS 4B (Fable L-3): Spatially Bilinearly Interpolated Layer-4 Grain-Guard Add-Back ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Bilinearly interpolates per-cell addBackFraction (guard_cell.x) across cell centers and applies
	// frac * (inFullH - outFullH) cleanly with zero 16-px block seams while keeping face-masked cells untouched.
	constexpr const char* kPass4SmoothGuardHeader = R"GLSL(
shared float td_sg_r[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sg_o[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared float td_sg_rh[(16 + 2 * TD_RAD_FINE) * 16];
shared float td_sg_oh[(16 + 2 * TD_RAD_FINE) * 16];
)GLSL";

	constexpr const char* kPass4SmoothGuardApply = R"GLSL(
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
    float rv = texelFetch(rec_y, ivec2(sx, sy), 0).x;
    float ov = texelFetch(cand_out, ivec2(sx, sy), 0).x;
    td_sg_r[idx] = td_is_bad(rv) ? 0.0 : rv;
    td_sg_o[idx] = td_is_bad(ov) ? 0.0 : ov;
}
memoryBarrierShared();
barrier();

int hN = tileDim * 16;
for (int idx = tid; idx < hN; idx += 256)
{
    int ty = idx / 16;
    int tx = idx % 16;
    int rowOff = ty * tileDim;
    int c = tx + TD_RAD_FINE;
    float accR = 0.0;
    float accO = 0.0;
    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
    {
        float tap = TD_TAP_FINE[k + TD_RAD_FINE];
        accR += tap * td_sg_r[rowOff + c + k];
        accO += tap * td_sg_o[rowOff + c + k];
    }
    td_sg_rh[idx] = accR;
    td_sg_oh[idx] = accO;
}
memoryBarrierShared();
barrier();

if (p.x >= TD_W || p.y >= TD_H)
    return;

float curOut = texelFetch(cand_out, p, 0).x;
if (!validRect || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(out_y, p, vec4(curOut, 0.0, 0.0, 0.0));
    return;
}

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
if (texelFetch(cell_focus, ivec2(cx, cy), 0).w > 0.5)
{
    imageStore(out_y, p, vec4(curOut, 0.0, 0.0, 0.0));
    return;
}

float gx = (float(p.x) + 0.5) / float(TD_CELL_PX) - 0.5;
float gy = (float(p.y) + 0.5) / float(TD_CELL_PX) - 0.5;
int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
int x1 = min(TD_CW - 1, x0 + 1);
int y1 = min(TD_CH - 1, y0 + 1);
float tx = clamp(gx - float(x0), 0.0, 1.0);
float ty = clamp(gy - float(y0), 0.0, 1.0);

float f00 = max(0.0, texelFetch(guard_cell, ivec2(x0, y0), 0).x);
float f10 = max(0.0, texelFetch(guard_cell, ivec2(x1, y0), 0).x);
float f01 = max(0.0, texelFetch(guard_cell, ivec2(x0, y1), 0).x);
float f11 = max(0.0, texelFetch(guard_cell, ivec2(x1, y1), 0).x);
float top = f00 + (f10 - f00) * tx;
float bot = f01 + (f11 - f01) * tx;
float frac = top + (bot - top) * ty;

if (frac > 0.0)
{
    int cyCenter = ly + TD_RAD_FINE;
    float gFineR = 0.0;
    float gFineO = 0.0;
    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
    {
        float tap = TD_TAP_FINE[k + TD_RAD_FINE];
        int rOff = (cyCenter + k) * 16 + lx;
        gFineR += tap * td_sg_rh[rOff];
        gFineO += tap * td_sg_oh[rOff];
    }
    int cIdx = cyCenter * tileDim + (lx + TD_RAD_FINE);
    float inFullH = td_sg_r[cIdx] - gFineR;
    float outFullH = td_sg_o[cIdx] - gFineO;
    curOut += frac * (inFullH - outFullH);
}
imageStore(out_y, p, vec4(curOut, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 5 (Fable M-9): Compact 2-Texel (8-Float / 32-Byte) Live GPU Counter Summary Reduction ----
	// Dispatch: 1x1x1 workgroup of 16x16 threads (256 threads striding over cw*ch cells).
	// Output: counter_summary (2 x 1 RGBA32F):
	//   texel (0,0): [focusVetoes, nonFiniteCells, guardAddBacks, cellsLimited]
	//   texel (1,0): [singularCells, singleRefA2Cells, faceMaskedCells, borderBilinearPx]
	constexpr const char* kPassCounterSummaryReduceHeader = R"GLSL(
shared vec4 td_csum0[256];
shared vec4 td_csum1[256];
)GLSL";

	constexpr const char* kPassCounterSummaryReduce = R"GLSL(
int tid = int(gl_LocalInvocationID.y) * 16 + int(gl_LocalInvocationID.x);
int totalCells = TD_CW * TD_CH;

vec4 a0 = vec4(0.0);
vec4 a1 = vec4(0.0);
for (int idx = tid; idx < totalCells; idx += 256)
{
    int cx = idx % TD_CW;
    int cy = idx / TD_CW;
    ivec2 cell = ivec2(cx, cy);
    vec4 df = texelFetch(cell_diag_flags, cell, 0);
    vec2 gc = texelFetch(guard_cell, cell, 0).xy;
    vec2 wd = texelFetch(warp_diag, cell, 0).xy;
    float isFace = (texelFetch(cell_focus, cell, 0).w > 0.5) ? 1.0 : 0.0;

    int gFlags = int(round(gc.y));
    bool nonFin = (df.y > 0.5) || (wd.y > 0.5) || ((gFlags & 4) != 0);
    bool guardHit = ((gFlags & 1) != 0);
    bool limited = ((gFlags & 8) != 0);

    a0.x += (df.x > 0.5) ? 1.0 : 0.0;
    a0.y += nonFin ? 1.0 : 0.0;
    a0.z += guardHit ? 1.0 : 0.0;
    a0.w += limited ? 1.0 : 0.0;

    a1.x += (df.z > 0.5) ? 1.0 : 0.0;
    a1.y += (df.w > 0.5) ? 1.0 : 0.0;
    a1.z += isFace;
    a1.w += max(0.0, wd.x);
}

td_csum0[tid] = a0;
td_csum1[tid] = a1;
memoryBarrierShared();
barrier();
for (int s = 128; s > 0; s >>= 1)
{
    if (tid < s)
    {
        td_csum0[tid] += td_csum0[tid + s];
        td_csum1[tid] += td_csum1[tid + s];
    }
    memoryBarrierShared();
    barrier();
}
if (tid == 0)
{
    imageStore(counter_summary, ivec2(0, 0), td_csum0[0]);
    imageStore(counter_summary, ivec2(1, 0), td_csum1[0]);
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

#if defined(TD_CHROMA_APPLY_HAS_VETO) && TD_CHROMA_APPLY_HAS_VETO
    bool vDefocLowOnly = false;
    if (u_has_face_mask > 0.5 || u_has_cell_prior > 0.5)
    {
        float vlx = (float(dp.x) + 0.5) * float(TD_DSUB_X) - 0.5 + u_dshift_x;
        float vly = (float(dp.y) + 0.5) * float(TD_DSUB_Y) - 0.5 + u_dshift_y;
        int vcx = clamp(max(0, int(floor(vlx))) / TD_CELL_PX, 0, TD_CW - 1);
        int vcy = clamp(max(0, int(floor(vly))) / TD_CELL_PX, 0, TD_CH - 1);
        ivec2 vcell = ivec2(vcx, vcy);
        if (u_has_face_mask > 0.5 && texelFetch(face_mask, vcell, 0).x > 0.5)
            return vec2(0.0);
        if (u_has_cell_prior > 0.5)
        {
            int vBits = int(round(texelFetch(cell_prior, vcell, 0).y));
            bool vFocus = ((vBits & 1) != 0);
            bool vDefoc = ((vBits & 2) != 0);
            if (vFocus || (vDefoc && u_bokeh_mode < 0.5))
                return vec2(0.0);
            if (vDefoc && u_bokeh_mode >= 0.5)
                vDefocLowOnly = true;
        }
    }
#endif

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
    bool useCatmull = (cxLocal >= 1.0 && cyLocal >= 1.0 && cxLocal < float(cAw) - 2.0 && cyLocal < float(cAh) - 2.0);
#if defined(TD_CHROMA_APPLY_HAS_VETO) && TD_CHROMA_APPLY_HAS_VETO
    // CHR-MEDIUM-2: Under BokehMode::LowBandCellOnly, never let Catmull-Rom/bilinear upsample taps
    // pull M/H coded deltas from adjacent sharp cells 1-2 px across a bokeh boundary.
    if (vDefocLowOnly)
    {
        bool allDefoc = false;
        if (useCatmull)
        {
            allDefoc = true;
            ivec2 bi0 = ivec2(floor(vec2(cxLocal, cyLocal)));
            for (int j = 0; j < 4 && allDefoc; ++j)
            {
                for (int i = 0; i < 4; ++i)
                {
                    ivec2 gq = ivec2(cax0 + bi0.x - 1 + i, cay0 + bi0.y - 1 + j);
                    float tlx = (float(gq.x) + 0.5) * float(TD_CSUB_X) - 0.5 + u_cshift_x;
                    float tly = (float(gq.y) + 0.5) * float(TD_CSUB_Y) - 0.5 + u_cshift_y;
                    int tcx = clamp(max(0, int(floor(tlx))) / TD_CELL_PX, 0, TD_CW - 1);
                    int tcy = clamp(max(0, int(floor(tly))) / TD_CELL_PX, 0, TD_CH - 1);
                    int tBits = int(round(texelFetch(cell_prior, ivec2(tcx, tcy), 0).y));
                    if ((tBits & 2) == 0 || (tBits & 1) != 0)
                    {
                        allDefoc = false;
                        break;
                    }
                }
            }
        }
        if (!allDefoc)
        {
            vec2 cl = clamp(vec2(cxLocal, cyLocal), vec2(0.0), vec2(float(cAw - 1), float(cAh - 1)));
            ivec2 i0 = ivec2(floor(cl));
            ivec2 i1 = min(ivec2(cAw - 1, cAh - 1), i0 + ivec2(1));
            vec2 f = cl - vec2(i0);
            ivec2 pts[4];
            pts[0] = ivec2(cax0 + i0.x, cay0 + i0.y);
            pts[1] = ivec2(cax0 + i1.x, cay0 + i0.y);
            pts[2] = ivec2(cax0 + i0.x, cay0 + i1.y);
            pts[3] = ivec2(cax0 + i1.x, cay0 + i1.y);
            float bw[4];
            bw[0] = (1.0 - f.x) * (1.0 - f.y);
            bw[1] = f.x * (1.0 - f.y);
            bw[2] = (1.0 - f.x) * f.y;
            bw[3] = f.x * f.y;
            vec2 sumD = vec2(0.0);
            float sumW = 0.0;
            for (int t = 0; t < 4; ++t)
            {
                float tlx = (float(pts[t].x) + 0.5) * float(TD_CSUB_X) - 0.5 + u_cshift_x;
                float tly = (float(pts[t].y) + 0.5) * float(TD_CSUB_Y) - 0.5 + u_cshift_y;
                int tcx = clamp(max(0, int(floor(tlx))) / TD_CELL_PX, 0, TD_CW - 1);
                int tcy = clamp(max(0, int(floor(tly))) / TD_CELL_PX, 0, TD_CH - 1);
                int tBits = int(round(texelFetch(cell_prior, ivec2(tcx, tcy), 0).y));
                if ((tBits & 2) != 0 && (tBits & 1) == 0)
                {
                    sumD += bw[t] * texelFetch(coded_delta, pts[t], 0).xy;
                    sumW += bw[t];
                }
            }
            outD = (sumW > TD_NUM_FLOOR) ? (sumD / sumW) : vec2(0.0);
            if (td_is_bad(outD.x)) { outD.x = 0.0; diagFlags |= 1; }
            if (td_is_bad(outD.y)) { outD.y = 0.0; diagFlags |= 1; }
            return outD;
        }
    }
#endif
    if (useCatmull)
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

	// ---- TD-U1: GPU Cell-Grid Quality & Own-Error Evidence Reduction (AlignedSharpness + NonBlockPictureStep) ----
	// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per 16x16 cell).
	// Output quality_cells (TD_CW x TD_CH RGBA32F):
	//   .x = E_cur (grain-corrected M+H energy of current cell)
	//   .y = E_prev_debiased (grain-corrected M+H/beta_HP energy of warped previous cell)
	//   .z = grid-free NonBlockPictureStep (plateau staircase jump + scale-invariant dead-zone deficit)
	//   .w = cell flags (bit0 = trusted & textured & !focusFlagged, bit1 = textured activity class, bit2 = nonFinite)
	constexpr const char* kPassQualityEvidenceHeader = R"GLSL(
shared float td_qe_cur_m[256];
shared float td_qe_cur_h[256];
shared float td_qe_prv_m[256];
shared float td_qe_prv_h[256];
shared float td_qe_stair_sum[256];
shared float td_qe_stair_cnt[256];
shared float td_qe_px_cnt[256];
shared uint td_qe_bad[256];
)GLSL";

	constexpr const char* kPassQualityEvidenceReduce = R"GLSL(
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
bool hasPrev = (u_has_prev_bands > 0.5);
float halfLsb = u_half_lsb;
float minBandJump = u_min_band_jump;
float maxBandJump = u_max_band_jump;

bool isOverlayCell = hasPrev && (int(round(texelFetch(motion_meta, cell, 0).x)) == 3);

float sCurM = 0.0, sCurH = 0.0, sPrvM = 0.0, sPrvH = 0.0;
float sStair = 0.0, sStairCnt = 0.0, sCnt = 0.0;
uint badFlag = 0u;

// UPG-HIGH-4: Stride across full TD_CELL_PX (never hardcoded 16x16!) for energy accumulation.
int cellX0 = cell.x * TD_CELL_PX;
int cellY0 = cell.y * TD_CELL_PX;
int cellX1 = min(ax1, cellX0 + TD_CELL_PX);
int cellY1 = min(ay1, cellY0 + TD_CELL_PX);
for (int dy = ly; dy < TD_CELL_PX; dy += 16)
{
    int py = cellY0 + dy;
    if (py < ay0 || py >= cellY1) continue;
    for (int dx = lx; dx < TD_CELL_PX; dx += 16)
    {
        int px = cellX0 + dx;
        if (px < ax0 || px >= cellX1) continue;
        ivec2 p = ivec2(px, py);
        vec4 cb = texelFetch(cur_bands, p, 0);
        float cv = texelFetch(cur_clean, p, 0).x;
        if (td_is_bad(cb.y) || td_is_bad(cb.z) || td_is_bad(cv))
        {
            badFlag = 1u;
            continue;
        }
        sCurM += cb.y * cb.y;
        sCurH += cb.z * cb.z;
        sCnt += 1.0;
        if (hasPrev)
        {
            vec4 pb = texelFetch(prev_bands, p, 0);
            if (!td_is_bad(pb.y) && !td_is_bad(pb.z))
            {
                sPrvM += pb.y * pb.y;
                sPrvH += pb.z * pb.z;
            }
            else
            {
                badFlag = 1u;
            }
        }
    }
}

// UPG-HIGH-4: Exact 1:1 mirror of CPU TdV2NonBlockStepEstimator monotonic 3-px-plateau per-row & per-column
// staircase check (excludes Overlay cells, 1-LSB native ramps, smooth gradients, upscaled frames, & single crisp edges!).
if (!isOverlayCell && lx == 0)
{
    for (int dy = ly; dy < TD_CELL_PX; dy += 16)
    {
        int py = cellY0 + dy;
        if (py < ay0 || py >= cellY1) continue;
        int posCnt = 0, negCnt = 0;
        float posSum = 0.0, posMin = maxBandJump, posMax = 0.0;
        float negSum = 0.0, negMin = maxBandJump, negMax = 0.0;
        for (int px = max(ax0, cellX0); px < cellX1; ++px)
        {
            if (px < ax0 + 3 || px + 2 >= ax1) continue;
            float vm3 = texelFetch(cur_clean, ivec2(px - 3, py), 0).x;
            float vm2 = texelFetch(cur_clean, ivec2(px - 2, py), 0).x;
            float vm1 = texelFetch(cur_clean, ivec2(px - 1, py), 0).x;
            float v0  = texelFetch(cur_clean, ivec2(px, py), 0).x;
            float vp1 = texelFetch(cur_clean, ivec2(px + 1, py), 0).x;
            float vp2 = texelFetch(cur_clean, ivec2(px + 2, py), 0).x;
            if (td_is_bad(vm3) || td_is_bad(vm2) || td_is_bad(vm1) || td_is_bad(v0) || td_is_bad(vp1) || td_is_bad(vp2))
                continue;
            if (!(abs(vm1 - vm2) <= halfLsb && abs(vm2 - vm3) <= halfLsb)) continue;
            float sj = 0.0;
            bool ok = false;
            if (abs(vp1 - v0) <= halfLsb && abs(vp2 - vp1) <= halfLsb) { sj = v0 - vm1; ok = true; }
            else if (px + 3 < ax1)
            {
                float vp3 = texelFetch(cur_clean, ivec2(px + 3, py), 0).x;
                if (!td_is_bad(vp3) && ((v0 - vm1) * (vp1 - v0) >= 0.0) &&
                    abs(vp2 - vp1) <= halfLsb && abs(vp3 - vp2) <= halfLsb)
                {
                    sj = vp1 - vm1; ok = true;
                }
            }
            if (!ok) continue;
            float aj = abs(sj);
            if (aj > minBandJump && aj <= maxBandJump)
            {
                if (sj > 0.0) { posCnt += 1; posSum += aj; posMin = min(posMin, aj); posMax = max(posMax, aj); }
                else          { negCnt += 1; negSum += aj; negMin = min(negMin, aj); negMax = max(negMax, aj); }
            }
        }
        if (posCnt >= 2 && posMax <= 2.0 * posMin) { sStair += posSum; sStairCnt += float(posCnt); }
        if (negCnt >= 2 && negMax <= 2.0 * negMin) { sStair += negSum; sStairCnt += float(negCnt); }
    }
}
if (!isOverlayCell && ly == 0)
{
    for (int dx = lx; dx < TD_CELL_PX; dx += 16)
    {
        int px = cellX0 + dx;
        if (px < ax0 || px >= cellX1) continue;
        int posCnt = 0, negCnt = 0;
        float posSum = 0.0, posMin = maxBandJump, posMax = 0.0;
        float negSum = 0.0, negMin = maxBandJump, negMax = 0.0;
        for (int py = max(ay0, cellY0); py < cellY1; ++py)
        {
            if (py < ay0 + 3 || py + 2 >= ay1) continue;
            float vm3 = texelFetch(cur_clean, ivec2(px, py - 3), 0).x;
            float vm2 = texelFetch(cur_clean, ivec2(px, py - 2), 0).x;
            float vm1 = texelFetch(cur_clean, ivec2(px, py - 1), 0).x;
            float v0  = texelFetch(cur_clean, ivec2(px, py), 0).x;
            float vp1 = texelFetch(cur_clean, ivec2(px, py + 1), 0).x;
            float vp2 = texelFetch(cur_clean, ivec2(px, py + 2), 0).x;
            if (td_is_bad(vm3) || td_is_bad(vm2) || td_is_bad(vm1) || td_is_bad(v0) || td_is_bad(vp1) || td_is_bad(vp2))
                continue;
            if (!(abs(vm1 - vm2) <= halfLsb && abs(vm2 - vm3) <= halfLsb)) continue;
            float sj = 0.0;
            bool ok = false;
            if (abs(vp1 - v0) <= halfLsb && abs(vp2 - vp1) <= halfLsb) { sj = v0 - vm1; ok = true; }
            else if (py + 3 < ay1)
            {
                float vp3 = texelFetch(cur_clean, ivec2(px, py + 3), 0).x;
                if (!td_is_bad(vp3) && ((v0 - vm1) * (vp1 - v0) >= 0.0) &&
                    abs(vp2 - vp1) <= halfLsb && abs(vp3 - vp2) <= halfLsb)
                {
                    sj = vp1 - vm1; ok = true;
                }
            }
            if (!ok) continue;
            float aj = abs(sj);
            if (aj > minBandJump && aj <= maxBandJump)
            {
                if (sj > 0.0) { posCnt += 1; posSum += aj; posMin = min(posMin, aj); posMax = max(posMax, aj); }
                else          { negCnt += 1; negSum += aj; negMin = min(negMin, aj); negMax = max(negMax, aj); }
            }
        }
        if (posCnt >= 2 && posMax <= 2.0 * posMin) { sStair += posSum; sStairCnt += float(posCnt); }
        if (negCnt >= 2 && negMax <= 2.0 * negMin) { sStair += negSum; sStairCnt += float(negCnt); }
    }
}

td_qe_cur_m[tid] = sCurM;
td_qe_cur_h[tid] = sCurH;
td_qe_prv_m[tid] = sPrvM;
td_qe_prv_h[tid] = sPrvH;
td_qe_stair_sum[tid] = sStair;
td_qe_stair_cnt[tid] = sStairCnt;
td_qe_px_cnt[tid] = sCnt;
td_qe_bad[tid] = badFlag;
memoryBarrierShared();
barrier();

for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_qe_cur_m[tid] += td_qe_cur_m[tid + stride];
        td_qe_cur_h[tid] += td_qe_cur_h[tid + stride];
        td_qe_prv_m[tid] += td_qe_prv_m[tid + stride];
        td_qe_prv_h[tid] += td_qe_prv_h[tid + stride];
        td_qe_stair_sum[tid] += td_qe_stair_sum[tid + stride];
        td_qe_stair_cnt[tid] += td_qe_stair_cnt[tid + stride];
        td_qe_px_cnt[tid] += td_qe_px_cnt[tid + stride];
        td_qe_bad[tid] |= td_qe_bad[tid + stride];
    }
    memoryBarrierShared();
    barrier();
}

if (tid == 0)
{
    float cnt = td_qe_px_cnt[0];
    if (cnt <= 0.5 || td_qe_bad[0] != 0u)
    {
        imageStore(quality_cells, cell, vec4(0.0, 0.0, 0.0, (td_qe_bad[0] != 0u) ? 4.0 : 0.0));
        return;
    }
    float invC = 1.0 / cnt;
    float eCurM = td_qe_cur_m[0] * invC;
    float eCurH = td_qe_cur_h[0] * invC;
    float ePrvM = td_qe_prv_m[0] * invC;
    float ePrvH = td_qe_prv_h[0] * invC;

    vec4 priorT = texelFetch(cell_prior, cell, 0);
    float retGVar = (!td_is_bad(priorT.x) && priorT.x > 0.0) ? priorT.x : 0.0;
    int focusBits = int(round(priorT.y));
    bool focusFlagged = ((focusBits & 3) != 0);

    float gM = retGVar * TD_GAIN_MID;
    float gH = retGVar * TD_GAIN_FINE;
    float cleanCur = max(0.0, eCurM - gM) + max(0.0, eCurH - gH);

    float betaHp = 1.0;
    bool mvTrusted = false;
    if (hasPrev)
    {
        vec2 mm = texelFetch(motion_meta, cell, 0).xy;
        vec2 mv = texelFetch(motion_vec, cell, 0).xy;
        mvTrusted = (mm.y >= 0.5) && !td_is_bad(mv.x) && !td_is_bad(mv.y);
        if (mvTrusted)
            betaHp = max(td_fine_hp_warp_gain(mv), TD_NUM_FLOOR);
    }
    float cleanPrv = hasPrev ? (max(0.0, ePrvM - gM) + max(0.0, (ePrvH / betaHp) - gH)) : 0.0;

    // Strictly monotonic staircase step estimate (zero bogus E_H/E_M band-ratio dzStep).
    float stepEst = (td_qe_stair_cnt[0] > 0.5) ? (td_qe_stair_sum[0] / td_qe_stair_cnt[0]) : 0.0;

    int flags = 0;
    if (cleanCur > TD_ACT_TEX_FLOOR)
        flags |= 2;
    if (hasPrev && mvTrusted && !focusFlagged && cleanCur > TD_ACT_TEX_FLOOR && cleanPrv > TD_ACT_TEX_FLOOR)
        flags |= 1;
    imageStore(quality_cells, cell, vec4(cleanCur, cleanPrv, stepEst, float(flags)));
}
)GLSL";

} // namespace Shaders
} // namespace Gpu
} // namespace TdV2
