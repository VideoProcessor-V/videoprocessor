// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5):
// GLSL compute shader sources for the 4-pass GPU fusion engine + chroma delta apply passes.
// Exact FP32 GPU twins of TD-S1 (TdV2BlendMath.h), TD-S3 (TdV2ReferenceEngine.h), and TD-S4 (TdV2Chroma.h):
//   - Pass 1 (kPass1MotionGate, kPass1WarpAndGate, kPass1BandH, kPass1BandV):
//       * Camera-relative fast-object motion gate + Karis 3x3 max-velocity silhouette dilation + Overlay/DynamicTexture masks
//       * Catmull-Rom bicubic strictly inside ActiveRect with 1-2 px border bilinear fallback, edge-clamped OOB fill
//         (valid = 0, zero 0.0-hole ringing), and explicit IEEE-754 non-finite closure
//       * Separable H + V 4-band decomposition (L, M, H, D3) from CPU-supplied U2 VpUnit::BandFromBase::Taps
//   - Pass 2 (kPass2Focus, kPass2MomentsAndSolve):
//       * Physics-derived noise-debiased focus guard + Parseval focal-plane shift detection
//       * FP32 shifted-origin second-moment covariance pooling (3x3 or 1x1, excluding vetoed cells)
//       * Blind c estimators A1/A2/A3, K x K Gaussian elimination with partial pivoting + trace conditioning + convex clip
//       * Populates U19 TdMapsView 12 channels across 3 RGBA32F slices + per-cell diagnostic flags
//   - Pass 3 (kPass3BlendAndBound, kPass3bDctPocs):
//       * Spatial-only bilinear weight interpolation (zero bleed across hard-vetoed cell boundaries) + 3-sigma Huber taper
//       * Derived pixel-domain band bound for transform-unknown & non-8-aligned remainder strips
//       * Optional codec-grid-aligned 8x8 orthonormal DCT-II STRICT/CELL POCS (1-2 iterations, bit-exact skip on zero delta)
//   - Pass 4 (kPass4Recompose, kPass4GuardFineH, kPass4GuardFineV, kPass4GuardCell, kPass4ApplyAndGuard):
//       * Recomposition strictly inside ActiveRect (bars bit-exact receivedY; carryClean strictly grain-free)
//       * Layer-4 TdGuardPort fine-band variance retention guard (modifies outputY ONLY, never carryClean)
//       * Shadow mode preserves outputY == receivedY bit-exact while publishing outputClean, tdDelta, and tdMaps
//   - Chroma GPU Apply (kPassChromaApplyYuv, kPassChromaApplyRgb):
//       * Sited coded-to-delivered delta upsample (0.0 -> 0.0 exact) + unclipped RGB matrix via CPU-passed U26 LumaCoeff
//
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
	// Shared GLSL helpers: Catmull-Rom weights (matching VpComposed::CubicWeights), cubic Hermite ramp,
	// active-rect safe bicubic/bilinear sampler, and finite check.
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
)GLSL";

	// Build exact CPU-supplied U2 VpUnit::BandFromBase tap arrays for L/M/H/D3 decomposition.
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

	// Build exact fine-band tap array only (for Layer-4 TdGuardPort fine extraction).
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

	// ---- PASS 1A: Camera-Relative Motion Gate + Karis 3x3 Silhouette Dilation ----
	// Dispatch: ceil((TD_K * TD_CW) / 16) x ceil(TD_CH / 16), group 16x16.
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
int totalW = TD_K * TD_CW;
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

if (selfCls == 3)
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
        if (td_cell_rel_speed(rk, nx, ny, nSpd, nCls, nBad) && nCls != 3)
        {
            dilatedSpd = max(dilatedSpd, nSpd);
        }
    }
}

float speedGate = 1.0 - td_hermite_ramp(TD_STILL_SPEED, TD_FAST_CUTOFF, dilatedSpd);
float gateL = (selfCls == 2) ? speedGate : 1.0;
float gateMH = (selfCls == 4) ? 0.0 : speedGate;
imageStore(motion_gate, gid, vec4(gateL, gateMH, 0.0, 0.0));
)GLSL";

	// ---- PASS 1B: Active-Rect Safe Bicubic Warp (kPass1WarpAndGate) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: ref_clean (w x h R32F), cur_clean (w x h R32F), motion_vec (3*cw x ch RGBA32F).
	// Output: warped_out (w x h RG32F: .x = warpedClean, .y = bitmask [1=valid, 2=borderBilin, 4=nonFinite]).
	constexpr const char* kPass1WarpHeader = R"GLSL(
float td_fetch_active_safe(ivec2 qLocal, ivec4 activeRect, out bool isBad)
{
    ivec2 sz = activeRect.zw - activeRect.xy;
    ivec2 gq = activeRect.xy + clamp(qLocal, ivec2(0), sz - ivec2(1));
    float v = texelFetch(ref_clean, gq, 0).x;
    if (td_is_bad(v))
    {
        isBad = true;
        return texelFetch(cur_clean, gq, 0).x;
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

void td_sample_bicubic_active(vec2 sLocal, ivec4 activeRect, float fallbackVal, out float outVal, out int outFlags)
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
            outVal = fallbackVal;
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
            outVal = fallbackVal;
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
        outVal = td_is_bad(bVal) ? fallbackVal : bVal;
        outFlags = (anyBad || td_is_bad(bVal)) ? 4 : 0;
    }
}
)GLSL";

	constexpr const char* kPass1WarpAndGate = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

ivec4 activeRect = ivec4(TD_AX0, TD_AY0, TD_AX1, TD_AY1);
float fallbackVal = texelFetch(cur_clean, p, 0).x;

if (p.x < activeRect.x || p.x >= activeRect.z || p.y < activeRect.y || p.y >= activeRect.w)
{
    imageStore(warped_out, p, vec4(fallbackVal, 0.0, 0.0, 0.0));
    return;
}

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
vec2 mv = texelFetch(motion_vec, ivec2(TD_REF_IDX * TD_CW + cx, cy), 0).xy;

// Also detect non-finite pixels at `p` in ref_clean even if motion shifts away from `p`.
float selfRef = texelFetch(ref_clean, p, 0).x;
int extraBad = td_is_bad(selfRef) ? 4 : 0;

if (td_is_bad(mv.x) || td_is_bad(mv.y))
{
    imageStore(warped_out, p, vec4(fallbackVal, 4.0, 0.0, 0.0));
    return;
}

vec2 sLocal = vec2(float(p.x - activeRect.x), float(p.y - activeRect.y)) + mv;
float outVal = fallbackVal;
int outFlags = 0;
td_sample_bicubic_active(sLocal, activeRect, fallbackVal, outVal, outFlags);
outFlags = outFlags | extraBad;
imageStore(warped_out, p, vec4(outVal, float(outFlags), 0.0, 0.0));
)GLSL";

	// ---- PASS 1C: Separable Horizontal + Vertical 4-Band Decomposition (U2 BandFromBase taps) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// kPass1BandH: src_tex (.x) -> band_tmp_h (gFine_H, gBase_H, gLow_H, srcVal) strictly inside ActiveRect.
	// kPass1BandV: band_tmp_h -> bands_out (L, M, H, D3) strictly inside ActiveRect.
	constexpr const char* kPass1BandH = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

if (p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(band_tmp_h, p, vec4(0.0));
    return;
}

int xMin = TD_AX0;
int xMax = TD_AX1 - 1;
float srcVal = texelFetch(src_tex, p, 0).x;

float accFine = 0.0;
for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
{
    int sx = clamp(p.x + k, xMin, xMax);
    accFine += TD_TAP_FINE[k + TD_RAD_FINE] * texelFetch(src_tex, ivec2(sx, p.y), 0).x;
}

float accBase = 0.0;
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    int sx = clamp(p.x + k, xMin, xMax);
    accBase += TD_TAP_BASE[k + TD_RAD_BASE] * texelFetch(src_tex, ivec2(sx, p.y), 0).x;
}

float accLow = 0.0;
for (int k = -TD_RAD_LOW; k <= TD_RAD_LOW; ++k)
{
    int sx = clamp(p.x + k, xMin, xMax);
    accLow += TD_TAP_LOW[k + TD_RAD_LOW] * texelFetch(src_tex, ivec2(sx, p.y), 0).x;
}

imageStore(band_tmp_h, p, vec4(accFine, accBase, accLow, srcVal));
)GLSL";

	constexpr const char* kPass1BandV = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

if (p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(bands_out, p, vec4(0.0));
    return;
}

int yMin = TD_AY0;
int yMax = TD_AY1 - 1;
float srcVal = texelFetch(band_tmp_h, p, 0).w;

float gFine = 0.0;
for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
{
    int sy = clamp(p.y + k, yMin, yMax);
    gFine += TD_TAP_FINE[k + TD_RAD_FINE] * texelFetch(band_tmp_h, ivec2(p.x, sy), 0).x;
}

float gBase = 0.0;
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    int sy = clamp(p.y + k, yMin, yMax);
    gBase += TD_TAP_BASE[k + TD_RAD_BASE] * texelFetch(band_tmp_h, ivec2(p.x, sy), 0).y;
}

float gLow = 0.0;
for (int k = -TD_RAD_LOW; k <= TD_RAD_LOW; ++k)
{
    int sy = clamp(p.y + k, yMin, yMax);
    gLow += TD_TAP_LOW[k + TD_RAD_LOW] * texelFetch(band_tmp_h, ivec2(p.x, sy), 0).z;
}

float bL  = gBase;
float bM  = gFine - bL;
float bH  = srcVal - gFine;
float bD3 = bL - gLow;
imageStore(bands_out, p, vec4(bL, bM, bH, bD3));
)GLSL";

	// ---- PASS 2A: Per-Cell Noise-Debiased Focus Guard + Parseval Focal-Plane Shift ----
	// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8.
	// Inputs: cur_bands, ref_bands_0/1/2, cell_prior (.y = explicitFocusChange), warped_0/1/2.
	// Outputs: cell_focus (cw x ch RG32F: .x = focusChange, .y = isDefocused),
	//          warp_diag (cw x ch RG32F: .x = borderBilinCount, .y = warpNonFiniteCount).
	constexpr const char* kPass2FocusHeader = R"GLSL(
vec4 td_fetch_ref_band(int k, ivec2 p)
{
    if (k == 0) return texelFetch(ref_bands_0, p, 0);
    if (k == 1) return texelFetch(ref_bands_1, p, 0);
    return texelFetch(ref_bands_2, p, 0);
}

int td_fetch_ref_flags(int k, ivec2 p)
{
    float fl = 0.0;
    if (k == 0) fl = texelFetch(warped_0, p, 0).y;
    else if (k == 1) fl = texelFetch(warped_1, p, 0).y;
    else fl = texelFetch(warped_2, p, 0).y;
    return int(round(fl));
}
)GLSL";

	constexpr const char* kPass2Focus = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int x0 = max(TD_AX0, cell.x * TD_CELL_PX);
int y0 = max(TD_AY0, cell.y * TD_CELL_PX);
int x1 = min(TD_AX1, (cell.x + 1) * TD_CELL_PX);
int y1 = min(TD_AY1, (cell.y + 1) * TD_CELL_PX);

float curE2 = 0.0;
float curE3 = 0.0;
float refE2[3];
float refE3[3];
refE2[0] = 0.0; refE2[1] = 0.0; refE2[2] = 0.0;
refE3[0] = 0.0; refE3[1] = 0.0; refE3[2] = 0.0;
int cnt = 0;
float borderBilinCnt = 0.0;
float warpBadCnt = 0.0;

for (int y = y0; y < y1; ++y)
{
    for (int x = x0; x < x1; ++x)
    {
        ivec2 p = ivec2(x, y);
        vec4 cb = texelFetch(cur_bands, p, 0);
        curE2 += cb.y * cb.y;
        curE3 += cb.w * cb.w;
        for (int k = 0; k < TD_K; ++k)
        {
            vec4 rb = td_fetch_ref_band(k, p);
            refE2[k] += rb.y * rb.y;
            refE3[k] += rb.w * rb.w;
            int fl = td_fetch_ref_flags(k, p);
            if ((fl & 2) != 0) borderBilinCnt += 1.0;
            if ((fl & 4) != 0) warpBadCnt += 1.0;
        }
        ++cnt;
    }
}

float curEnergy = 0.0;
float curM2 = 0.0;
float curF = 1.0;
if (cnt > 0)
{
    float invC = 1.0 / float(cnt);
    curM2 = curE2 * invC;
    float curM3 = curE3 * invC;
    curEnergy = curM2 + curM3;
    curF = (curEnergy > TD_NUM_FLOOR) ? (curM2 / (curEnergy + TD_NUM_FLOOR)) : 1.0;
}

float curM3Val = max(0.0, curEnergy - curM2);
float cleanM2 = max(0.0, curM2 - TD_STEP_NOISE_VAR * TD_GAIN_MID);
float cleanM3 = max(0.0, curM3Val - TD_STEP_NOISE_VAR * TD_GAIN_D3);
float curFDebiased = cleanM2 / (cleanM2 + cleanM3 + TD_NUM_FLOOR);

bool isDefocused = (curEnergy > TD_STRUCTURE_FLOOR && curFDebiased <= TD_F_DEFOCUS);
bool focusChange = false;

float explicitFc = texelFetch(cell_prior, cell, 0).y;
if (explicitFc > 0.5)
{
    focusChange = true;
}
else if (cnt > 0)
{
    float invC = 1.0 / float(cnt);
    for (int k = 0; k < TD_K; ++k)
    {
        float rM2 = refE2[k] * invC;
        float rM3 = refE3[k] * invC;
        float rEnergy = rM2 + rM3;
        float rF = (rEnergy > TD_NUM_FLOOR) ? (rM2 / (rEnergy + TD_NUM_FLOOR)) : 1.0;
        if (rEnergy > TD_STRUCTURE_FLOOR && curM2 < rM2 && curF < TD_F_SHARP && (rF - curF) > TD_F_SHIFT_DELTA)
        {
            focusChange = true;
            break;
        }
    }
}

imageStore(cell_focus, cell, vec4(focusChange ? 1.0 : 0.0, isDefocused ? 1.0 : 0.0, 0.0, 0.0));
imageStore(warp_diag, cell, vec4(borderBilinCnt, warpBadCnt, 0.0, 0.0));
)GLSL";

	// ---- PASS 2B: FP32 Shifted-Origin Covariance Moments + Blind A1/A2/A3 + KxK Solve + TdMaps ----
	// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8.
	// Inputs: cur_bands, ref_bands_0/1/2, warped_0/1/2, motion_gate, cell_focus, cell_prior.
	// Outputs: cell_weights (3*cw x ch RGBA32F), cell_diag_c (3*cw x ch RGBA32F),
	//          td_maps (cw x 3*ch RGBA32F), cell_diag_flags (cw x ch RGBA32F).
	constexpr const char* kPass2SolveHeader = R"GLSL(
vec4 td_fetch_ref_band(int k, ivec2 p)
{
    if (k == 0) return texelFetch(ref_bands_0, p, 0);
    if (k == 1) return texelFetch(ref_bands_1, p, 0);
    return texelFetch(ref_bands_2, p, 0);
}

int td_fetch_ref_flags(int k, ivec2 p)
{
    float fl = 0.0;
    if (k == 0) fl = texelFetch(warped_0, p, 0).y;
    else if (k == 1) fl = texelFetch(warped_1, p, 0).y;
    else fl = texelFetch(warped_2, p, 0).y;
    return int(round(fl));
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

bool td_is_exact_copy(int k)
{
    if (k == 0) return TD_EXACT_COPY_0 != 0;
    if (k == 1) return TD_EXACT_COPY_1 != 0;
    return TD_EXACT_COPY_2 != 0;
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

float td_select_blind_c(float cA1, float cA2)
{
    if (TD_ESTIMATOR == 0)
        return (!td_is_bad(cA1) && cA1 > 0.0) ? cA1 : 0.0;
    if (TD_ESTIMATOR == 1)
        return (!td_is_bad(cA2) && cA2 > 0.0) ? cA2 : 0.0;
    if (td_is_bad(cA1) || td_is_bad(cA2))
        return 0.0;
    return max(0.0, min(cA1, cA2));
}

// Returns 0 = ok, 1 = singular, 2 = non-finite.
int td_solve_convex(float C[3][3], float cVec[3], out float outW[3])
{
    outW[0] = 0.0; outW[1] = 0.0; outW[2] = 0.0;
    float diagSum = 0.0;
    for (int i = 0; i < TD_K; ++i)
    {
        if (td_is_bad(cVec[i]))
            return 2;
        for (int j = 0; j < TD_K; ++j)
        {
            if (td_is_bad(C[i][j]))
                return 2;
        }
        diagSum += max(0.0, C[i][i]);
    }
    if (diagSum <= TD_NUM_FLOOR)
        return 0;

    float meanDiag = diagSum / float(TD_K);
    float reg = max(TD_TRACE_REG * meanDiag, TD_NUM_FLOOR);

    float a[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            a[i][j] = 0.0;

    for (int i = 0; i < TD_K; ++i)
    {
        for (int j = 0; j < TD_K; ++j)
            a[i][j] = C[i][j] + ((i == j) ? reg : 0.0);
        a[i][TD_K] = cVec[i];
    }

    for (int col = 0; col < TD_K; ++col)
    {
        int pivotRow = col;
        float pivotAbs = abs(a[col][col]);
        for (int r = col + 1; r < TD_K; ++r)
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
            for (int j = col; j <= TD_K; ++j)
            {
                float tmp = a[col][j];
                a[col][j] = a[pivotRow][j];
                a[pivotRow][j] = tmp;
            }
        }
        float invPivot = 1.0 / a[col][col];
        for (int r = col + 1; r < TD_K; ++r)
        {
            float factor = a[r][col] * invPivot;
            for (int j = col; j <= TD_K; ++j)
                a[r][j] -= factor * a[col][j];
        }
    }

    float w[3];
    w[0] = 0.0; w[1] = 0.0; w[2] = 0.0;
    for (int i = TD_K - 1; i >= 0; --i)
    {
        float rhs = a[i][TD_K];
        for (int j = i + 1; j < TD_K; ++j)
            rhs -= a[i][j] * w[j];
        w[i] = rhs / a[i][i];
        if (td_is_bad(w[i]))
            return 2;
    }

    float sumW = 0.0;
    for (int i = 0; i < TD_K; ++i)
    {
        outW[i] = max(0.0, w[i]);
        sumW += outW[i];
    }
    if (sumW > 1.0)
    {
        float invSum = 1.0 / sumW;
        for (int i = 0; i < TD_K; ++i)
            outW[i] *= invSum;
    }
    return 0;
}
)GLSL";

	constexpr const char* kPass2MomentsAndSolve = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

vec2 fInfo = texelFetch(cell_focus, cell, 0).xy;
bool focusChange = (fInfo.x > 0.5);
bool isDefocused = (fInfo.y > 0.5);

float diagNonFinite = 0.0;
float diagSingular = 0.0;
float diagSingleRefA2 = 0.0;

// Include any non-finite motion gate flags for this cell across active refs.
for (int k = 0; k < TD_K; ++k)
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

// Spatial fine-band detail RMS for TdMapsView::kRealDetail.
int sx0 = max(TD_AX0, cell.x * TD_CELL_PX);
int sy0 = max(TD_AY0, cell.y * TD_CELL_PX);
int sx1 = min(TD_AX1, (cell.x + 1) * TD_CELL_PX);
int sy1 = min(TD_AY1, (cell.y + 1) * TD_CELL_PX);
float spatialFineE = 0.0;
int spatialCnt = 0;
for (int y = sy0; y < sy1; ++y)
{
    for (int x = sx0; x < sx1; ++x)
    {
        float hv = texelFetch(cur_bands, ivec2(x, y), 0).z;
        spatialFineE += hv * hv;
        ++spatialCnt;
    }
}
float spatialDetailRms = (spatialCnt > 0) ? sqrt(spatialFineE / float(spatialCnt)) : 0.0;

int py0 = max(TD_AY0, (cell.y - TD_POOL_R) * TD_CELL_PX);
int py1 = min(TD_AY1, (cell.y + TD_POOL_R + 1) * TD_CELL_PX);
int px0 = max(TD_AX0, (cell.x - TD_POOL_R) * TD_CELL_PX);
int px1 = min(TD_AX1, (cell.x + TD_POOL_R + 1) * TD_CELL_PX);

float cellWeightSumAll = 0.0;
float midWeightSum = 0.0;
float cellResEnergy = 0.0;
int activeBandBits = 0;

for (int b = 0; b < 3; ++b)
{
    bool hasAnchor = false;
    float anchor[3];
    float sumS[3];
    float sumP[3][3];
    for (int i = 0; i < 3; ++i)
    {
        anchor[i] = 0.0;
        sumS[i] = 0.0;
        for (int j = 0; j < 3; ++j)
            sumP[i][j] = 0.0;
    }
    int sampleCount = 0;

    for (int y = py0; y < py1; ++y)
    {
        int pcy = min(TD_CH - 1, y / TD_CELL_PX);
        for (int x = px0; x < px1; ++x)
        {
            int pcx = min(TD_CW - 1, x / TD_CELL_PX);
            if (texelFetch(cell_focus, ivec2(pcx, pcy), 0).x > 0.5)
                continue;

            ivec2 p = ivec2(x, y);
            float curV = td_band_comp(texelFetch(cur_bands, p, 0), b);
            bool allValid = true;
            float diffs[3];
            diffs[0] = 0.0; diffs[1] = 0.0; diffs[2] = 0.0;

            for (int k = 0; k < TD_K; ++k)
            {
                vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + pcx, pcy), 0).xy;
                float pGate = (b == 0) ? mg.x : mg.y;
                int fl = td_fetch_ref_flags(k, p);
                if ((fl & 1) == 0 || pGate <= 0.0)
                {
                    allValid = false;
                    break;
                }
                float rv = td_band_comp(td_fetch_ref_band(k, p), b);
                float d = curV - rv;
                if (td_is_bad(d))
                {
                    allValid = false;
                    break;
                }
                diffs[k] = d;
            }
            if (allValid)
            {
                if (!hasAnchor)
                {
                    for (int i = 0; i < TD_K; ++i)
                        anchor[i] = diffs[i];
                    hasAnchor = true;
                }
                for (int i = 0; i < TD_K; ++i)
                {
                    float si = diffs[i] - anchor[i];
                    sumS[i] += si;
                    for (int j = i; j < TD_K; ++j)
                    {
                        float sj = diffs[j] - anchor[j];
                        sumP[i][j] += si * sj;
                    }
                }
                ++sampleCount;
            }
        }
    }

    vec4 wOut = vec4(0.0);
    vec4 diagOut = vec4(0.0);

    if (sampleCount > 0)
    {
        float C[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                C[i][j] = 0.0;

        float invN = 1.0 / float(sampleCount);
        for (int i = 0; i < TD_K; ++i)
        {
            float mi = sumS[i] * invN;
            float meanI = anchor[i] + mi;
            for (int j = i; j < TD_K; ++j)
            {
                float mj = sumS[j] * invN;
                float meanJ = anchor[j] + mj;
                float cov = sumP[i][j] * invN - mi * mj;
                float m2 = cov + meanI * meanJ;
                float val = (i == j && m2 < 0.0) ? 0.0 : m2;
                C[i][j] = val;
                C[j][i] = val;
            }
        }
        diagOut.x = (TD_K > 0) ? C[0][0] : 0.0;
        diagOut.y = (TD_K > 1) ? C[1][1] : 0.0;
        diagOut.z = (TD_K > 2) ? C[2][2] : 0.0;

        float bGain = td_band_gain(b);
        float cVec[3];
        cVec[0] = 0.0; cVec[1] = 0.0; cVec[2] = 0.0;
        for (int k = 0; k < TD_K; ++k)
        {
            bool exactCopy = td_is_exact_copy(k);
            float cA2 = (TD_A2_VALID != 0)
                ? ((TD_A2_SIGMA_SENT + (exactCopy ? 0.0 : TD_A2_SIGMA_ZERO)) * bGain)
                : 0.0;
            float cA1 = cA2;
            if (TD_K >= 2)
            {
                int partner = (k == 0 || k == 2) ? 1 : 0;
                cA1 = td_est_c_a1(C[k][partner], C[k][k], C[partner][partner], cellRetGVar, bGain);
            }
            else if (TD_ESTIMATOR != 1)
            {
                diagSingleRefA2 += 1.0;
            }
            cVec[k] = exactCopy ? 0.0 : td_select_blind_c(cA1, cA2);
        }

        float solvedW[3];
        int status = td_solve_convex(C, cVec, solvedW);
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
                focusGate = (b == 0 && TD_BOKEH_L_ONLY != 0) ? 1.0 : 0.0;

            float bandWSum = 0.0;
            float wFinal[3];
            wFinal[0] = 0.0; wFinal[1] = 0.0; wFinal[2] = 0.0;
            for (int k = 0; k < TD_K; ++k)
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
    sqrt(max(0.0, TD_A2_TOTAL_VAR)));
vec4 slice2 = vec4(1.0, 0.0, 0.0, float(activeBandBits));

imageStore(td_maps, ivec2(cell.x, cell.y), slice0);
imageStore(td_maps, ivec2(cell.x, TD_CH + cell.y), slice1);
imageStore(td_maps, ivec2(cell.x, 2 * TD_CH + cell.y), slice2);
imageStore(cell_diag_flags, cell, vec4(focusChange ? 1.0 : 0.0, diagNonFinite, diagSingular, diagSingleRefA2));
)GLSL";

	// ---- PASS 3A: Spatial-Only Weight Interpolation + 3-Sigma Huber Taper + Pixel-Domain Bound ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: cur_bands, ref_bands_0/1/2, warped_0/1/2, cell_weights, cell_diag_c.
	// Output: delta_out (w x h R32F).
	constexpr const char* kPass3BlendHeader = R"GLSL(
vec4 td_fetch_ref_band(int k, ivec2 p)
{
    if (k == 0) return texelFetch(ref_bands_0, p, 0);
    if (k == 1) return texelFetch(ref_bands_1, p, 0);
    return texelFetch(ref_bands_2, p, 0);
}

int td_fetch_ref_flags(int k, ivec2 p)
{
    float fl = 0.0;
    if (k == 0) fl = texelFetch(warped_0, p, 0).y;
    else if (k == 1) fl = texelFetch(warped_1, p, 0).y;
    else fl = texelFetch(warped_2, p, 0).y;
    return int(round(fl));
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
)GLSL";

	constexpr const char* kPass3BlendAndBound = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

if (p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(delta_out, p, vec4(0.0));
    return;
}

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);

bool inFull8x8 = (TD_XFORM_8X8 != 0) && (TD_BX0 < TD_BX1) && (TD_BY0 < TD_BY1) &&
                 (p.x >= TD_BX0) && (p.x < TD_BX1) && (p.y >= TD_BY0) && (p.y < TD_BY1);

float gx = (float(p.x) + 0.5) / float(TD_CELL_PX) - 0.5;
float gy = (float(p.y) + 0.5) / float(TD_CELL_PX) - 0.5;
int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
int x1 = min(TD_CW - 1, x0 + 1);
int y1 = min(TD_CH - 1, y0 + 1);
float tx = clamp(gx - float(x0), 0.0, 1.0);
float ty = clamp(gy - float(y0), 0.0, 1.0);

vec4 curB = texelFetch(cur_bands, p, 0);
vec4 refB[3];
int refFl[3];
for (int k = 0; k < TD_K; ++k)
{
    refB[k] = td_fetch_ref_band(k, p);
    refFl[k] = td_fetch_ref_flags(k, p);
}

float totalDelta = 0.0;
for (int b = 0; b < 3; ++b)
{
    float curV = td_band_comp(curB, b);
    vec4 ownW = texelFetch(cell_weights, ivec2(b * TD_CW + cx, cy), 0);
    vec4 ownDiag = texelFetch(cell_diag_c, ivec2(b * TD_CW + cx, cy), 0);
    vec4 w00 = texelFetch(cell_weights, ivec2(b * TD_CW + x0, y0), 0);
    vec4 w10 = texelFetch(cell_weights, ivec2(b * TD_CW + x1, y0), 0);
    vec4 w01 = texelFetch(cell_weights, ivec2(b * TD_CW + x0, y1), 0);
    vec4 w11 = texelFetch(cell_weights, ivec2(b * TD_CW + x1, y1), 0);

    float bandDelta = 0.0;
    for (int k = 0; k < TD_K; ++k)
    {
        if ((refFl[k] & 1) == 0)
            continue;
        // Hard veto boundary guard: zero bleed across vetoed cell boundaries.
        if (td_vec_comp(ownW, k) <= 0.0)
            continue;

        float c00 = td_vec_comp(w00, k);
        float c10 = td_vec_comp(w10, k);
        float c01 = td_vec_comp(w01, k);
        float c11 = td_vec_comp(w11, k);
        if (td_is_bad(c00) || td_is_bad(c10) || td_is_bad(c01) || td_is_bad(c11))
            continue;
        float top = c00 + (c10 - c00) * tx;
        float bot = c01 + (c11 - c01) * tx;
        float wSpat = top + (bot - top) * ty;
        if (wSpat <= 0.0)
            continue;

        float diff = curV - td_band_comp(refB[k], b);
        float diagC = td_vec_comp(ownDiag, k);
        if (td_is_bad(diff) || td_is_bad(diagC))
            continue;
        float denom = max(diagC, TD_NUM_FLOOR);
        float r = (diff * diff) / denom;
        float taper = (r <= TD_HUBER_KNEE_SQ) ? 1.0 : (TD_HUBER_KNEE_SQ / r);
        bandDelta -= (wSpat * taper) * diff;
    }

    if (!inFull8x8)
    {
        float bandVar = (TD_A2_VALID != 0) ? (TD_A2_TOTAL_VAR * td_band_gain(b)) : 0.0;
        if (abs(bandDelta) > 0.0)
        {
            float bound = TD_PIXEL_BOUND_SIGMA * sqrt(max(0.0, bandVar));
            bandDelta = clamp(bandDelta, -bound, bound);
        }
    }
    totalDelta += bandDelta;
}

imageStore(delta_out, p, vec4(td_is_bad(totalDelta) ? 0.0 : totalDelta, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 3B: Optional Codec-Grid-Aligned 8x8 DCT-II STRICT/CELL POCS ----
	// Dispatch: ceil(TD_W / 8) x ceil(TD_H / 8), group 8x8 (1 workgroup per codec-aligned 8x8 block).
	// Inputs: delta_in (w x h R32F), cur_clean (w x h R32F), pred_clean (w x h RG32F, .x = refs[0].clean).
	// Output: delta_out (w x h R32F).
	constexpr const char* kPass3bDctHeader = R"GLSL(
shared float td_s_sent[64];
shared float td_s_blend[64];
shared float td_s_tmp[64];
shared float td_s_sent_dct[64];
shared uint td_s_nz[64];

float td_project_coeff(float sentCoeff, float blendCoeff)
{
    if (td_is_bad(sentCoeff) || td_is_bad(blendCoeff) || TD_STEP_DELTA <= 0.0 || TD_DEADZONE_PHI < 0.0 || TD_DEADZONE_PHI >= 1.0)
        return td_is_bad(sentCoeff) ? 0.0 : sentCoeff;

    float theta = (1.0 - TD_DEADZONE_PHI) * TD_STEP_DELTA;
    float absSent = abs(sentCoeff);
    int level = 0;
    if (absSent >= theta)
    {
        int mag = int(floor(absSent / TD_STEP_DELTA + TD_DEADZONE_PHI));
        level = (sentCoeff >= 0.0) ? mag : -mag;
    }
    if (level == 0)
        return clamp(blendCoeff, -theta, theta);
    if (TD_RESIDUAL_MODE == 1)
        return sentCoeff;

    float kf = float(level);
    float lo = 0.0;
    float hi = 0.0;
    if (level > 0)
    {
        lo = (kf - TD_DEADZONE_PHI) * TD_STEP_DELTA;
        hi = (kf + 1.0 - TD_DEADZONE_PHI) * TD_STEP_DELTA;
    }
    else
    {
        lo = (kf - 1.0 + TD_DEADZONE_PHI) * TD_STEP_DELTA;
        hi = (kf + TD_DEADZONE_PHI) * TD_STEP_DELTA;
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

bool blockInInterior = (TD_BX0 < TD_BX1) && (TD_BY0 < TD_BY1) &&
                       (bx >= TD_BX0) && (bx + 8 <= TD_BX1) &&
                       (by >= TD_BY0) && (by + 8 <= TD_BY1);

if (!blockInInterior)
{
    if (p.x < TD_W && p.y < TD_H)
    {
        float d = texelFetch(delta_in, p, 0).x;
        imageStore(delta_out, p, vec4(d, 0.0, 0.0, 0.0));
    }
    return;
}

float dVal = texelFetch(delta_in, p, 0).x;
float cVal = texelFetch(cur_clean, p, 0).x;
float pVal = texelFetch(pred_clean, p, 0).x;
td_s_nz[tid] = (dVal != 0.0) ? 1u : 0u;
barrier();
for (int stride = 32; stride > 0; stride >>= 1)
{
    if (tid < stride)
        td_s_nz[tid] = td_s_nz[tid] | td_s_nz[tid + stride];
    barrier();
}

// Bit-exact skip for untouched / vetoed zero-delta 8x8 blocks (zero DCT round-trip drift, zero atomics).
if (td_s_nz[0] == 0u)
{
    imageStore(delta_out, p, vec4(0.0));
    return;
}

td_s_sent[tid] = cVal - pVal;
td_s_blend[tid] = (cVal + dVal) - pVal;
barrier();

// Forward DCT-II of sent residual once (invariant across POCS iterations).
{
    float rowSum = 0.0;
    for (int x = 0; x < 8; ++x)
        rowSum += td_s_sent[loc.y * 8 + x] * TD_DCT8_BASIS[loc.x * 8 + x];
    td_s_tmp[tid] = rowSum;
    barrier();

    float colSum = 0.0;
    for (int y = 0; y < 8; ++y)
        colSum += td_s_tmp[y * 8 + loc.x] * TD_DCT8_BASIS[loc.y * 8 + y];
    td_s_sent_dct[tid] = colSum;
    barrier();
}

for (int it = 0; it < TD_POCS_ITERS; ++it)
{
    // Forward DCT-II of blended residual.
    float rowSum = 0.0;
    for (int x = 0; x < 8; ++x)
        rowSum += td_s_blend[loc.y * 8 + x] * TD_DCT8_BASIS[loc.x * 8 + x];
    td_s_tmp[tid] = rowSum;
    barrier();

    float blendCoeff = 0.0;
    for (int y = 0; y < 8; ++y)
        blendCoeff += td_s_tmp[y * 8 + loc.x] * TD_DCT8_BASIS[loc.y * 8 + y];

    // Set 1: Quantization-cell coefficient projection (STRICT / CELL).
    float projCoeff = td_project_coeff(td_s_sent_dct[tid], blendCoeff);
    td_s_blend[tid] = projCoeff;
    barrier();

    // Inverse DCT-II of projected residual (transpose of orthonormal basis).
    float invRow = 0.0;
    for (int u = 0; u < 8; ++u)
        invRow += td_s_blend[loc.y * 8 + u] * TD_DCT8_BASIS[u * 8 + loc.x];
    td_s_tmp[tid] = invRow;
    barrier();

    float projRes = 0.0;
    for (int v = 0; v < 8; ++v)
        projRes += td_s_tmp[v * 8 + loc.x] * TD_DCT8_BASIS[v * 8 + loc.y];

    dVal = (pVal + projRes) - cVal;

    // Set 2: Pixel-domain valid code-range projection [0, TD_PEAK_CODE].
    if (TD_PEAK_CODE > 0.0 && dVal != 0.0)
    {
        float cl = clamp(cVal + dVal, 0.0, TD_PEAK_CODE);
        dVal = cl - cVal;
    }

    td_s_blend[tid] = (cVal + dVal) - pVal;
    barrier();
}

imageStore(delta_out, p, vec4(td_is_bad(dVal) ? 0.0 : dVal, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 4A: Recompose (candClean + candOut) + Carry Update ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: rec_y, cur_clean, own_grain, delta_tex.
	// Outputs: out_clean, carry_clean (both strictly grain-free!), cand_out.
	constexpr const char* kPass4Recompose = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

float recY = texelFetch(rec_y, p, 0).x;
float cClean = texelFetch(cur_clean, p, 0).x;

if (p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(out_clean, p, vec4(cClean, 0.0, 0.0, 0.0));
    imageStore(carry_clean, p, vec4(cClean, 0.0, 0.0, 0.0));
    imageStore(cand_out, p, vec4(recY, 0.0, 0.0, 0.0));
    return;
}

float d = texelFetch(delta_tex, p, 0).x;
float ownG = texelFetch(own_grain, p, 0).x;
float newClean = cClean + d;
float newOut = newClean + ownG;

imageStore(out_clean, p, vec4(newClean, 0.0, 0.0, 0.0));
imageStore(carry_clean, p, vec4(newClean, 0.0, 0.0, 0.0));
imageStore(cand_out, p, vec4(newOut, 0.0, 0.0, 0.0));
)GLSL";

	// ---- PASS 4B: Layer-4 TdGuardPort Fine-Band Extraction (H + V on recY, candOut, ownGrain) ----
	constexpr const char* kPass4GuardFineH = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

if (p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(band_tmp_h, p, vec4(0.0));
    return;
}

int xMin = TD_AX0;
int xMax = TD_AX1 - 1;
vec3 acc = vec3(0.0);
for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
{
    int sx = clamp(p.x + k, xMin, xMax);
    float w = TD_TAP_FINE[k + TD_RAD_FINE];
    acc.x += w * texelFetch(rec_y, ivec2(sx, p.y), 0).x;
    acc.y += w * texelFetch(cand_out, ivec2(sx, p.y), 0).x;
    acc.z += w * texelFetch(own_grain, ivec2(sx, p.y), 0).x;
}
imageStore(band_tmp_h, p, vec4(acc, 0.0));
)GLSL";

	constexpr const char* kPass4GuardFineV = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

if (p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(guard_bands, p, vec4(0.0));
    return;
}

int yMin = TD_AY0;
int yMax = TD_AY1 - 1;
vec3 gFine = vec3(0.0);
for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
{
    int sy = clamp(p.y + k, yMin, yMax);
    float w = TD_TAP_FINE[k + TD_RAD_FINE];
    gFine += w * texelFetch(band_tmp_h, ivec2(p.x, sy), 0).xyz;
}

float recVal = texelFetch(rec_y, p, 0).x;
float outVal = texelFetch(cand_out, p, 0).x;
float ogVal = texelFetch(own_grain, p, 0).x;
imageStore(guard_bands, p, vec4(recVal - gFine.x, outVal - gFine.y, ogVal - gFine.z, ogVal));
)GLSL";

	// ---- PASS 4C: Layer-4 TdGuardPort Per-Cell Evaluation ----
	// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8.
	// Output: guard_cell (cw x ch RG32F: .x = addBackFraction, .y = bitmask [1=violated, 2=evaluated]).
	constexpr const char* kPass4GuardCell = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int x0 = max(TD_AX0, cell.x * TD_CELL_PX);
int y0 = max(TD_AY0, cell.y * TD_CELL_PX);
int x1 = min(TD_AX1, (cell.x + 1) * TD_CELL_PX);
int y1 = min(TD_AY1, (cell.y + 1) * TD_CELL_PX);

float ogE = 0.0;
float ogFineE = 0.0;
float inFineE = 0.0;
float outFineE = 0.0;
int cnt = 0;

for (int y = y0; y < y1; ++y)
{
    for (int x = x0; x < x1; ++x)
    {
        vec4 gb = texelFetch(guard_bands, ivec2(x, y), 0);
        inFineE += gb.x * gb.x;
        outFineE += gb.y * gb.y;
        if (TD_HAS_CLEAN_SPLIT != 0)
            ogFineE += gb.z * gb.z;
        ogE += gb.w * gb.w;
        ++cnt;
    }
}

if (cnt == 0)
{
    imageStore(guard_cell, cell, vec4(0.0));
    return;
}

float rawGv = texelFetch(cell_prior, cell, 0).x;
float cellRetGVar = (!td_is_bad(rawGv) && rawGv > 0.0) ? rawGv : 0.0;
float invC = 1.0 / float(cnt);

if (ogE * invC <= TD_NUM_FLOOR && cellRetGVar <= TD_NUM_FLOOR)
{
    imageStore(guard_cell, cell, vec4(0.0));
    return;
}

float leakedFineE = cellRetGVar * TD_GAIN_FINE;
float targetFineE = (TD_HAS_CLEAN_SPLIT != 0)
    ? (ogFineE * invC + leakedFineE)
    : ((leakedFineE > TD_NUM_FLOOR) ? min(inFineE * invC, leakedFineE) : (inFineE * invC));
float curOutFineE = outFineE * invC;

float addBack = 0.0;
int flags = 2; // evaluated

if (td_is_bad(targetFineE) || td_is_bad(curOutFineE))
{
    flags |= 1;
    addBack = 1.0;
}
else if (targetFineE > TD_NUM_FLOOR)
{
    float ratio = curOutFineE / targetFineE;
    if (ratio < TD_MIN_VAR_RETENTION)
    {
        flags |= 1;
        float targetStd = 0.98 * sqrt(targetFineE);
        float curStd = (curOutFineE > 0.0) ? sqrt(curOutFineE) : 0.0;
        float denom = sqrt(targetFineE) - curStd;
        addBack = (denom > TD_NUM_FLOOR) ? clamp((targetStd - curStd) / denom, 0.0, 1.0) : 1.0;
    }
}

imageStore(guard_cell, cell, vec4(addBack, float(flags), 0.0, 0.0));
)GLSL";

	// ---- PASS 4D: Final Apply + Layer-4 Guard Add-Back (outputY ONLY; Shadow = bit-exact receivedY) ----
	// Dispatch: ceil(TD_W / 16) x ceil(TD_H / 16), group 16x16.
	// Inputs: rec_y, cand_out, guard_bands, guard_cell.
	// Output: out_y (w x h R32F).
	constexpr const char* kPass4ApplyAndGuard = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

float recVal = texelFetch(rec_y, p, 0).x;
if (TD_SHADOW_MODE != 0 || p.x < TD_AX0 || p.x >= TD_AX1 || p.y < TD_AY0 || p.y >= TD_AY1)
{
    imageStore(out_y, p, vec4(recVal, 0.0, 0.0, 0.0));
    return;
}

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
vec2 g = texelFetch(guard_cell, ivec2(cx, cy), 0).xy;
int gFlags = int(round(g.y));
float val = texelFetch(cand_out, p, 0).x;

if ((gFlags & 1) != 0 && g.x > 0.0)
{
    vec2 gb = texelFetch(guard_bands, p, 0).xy;
    val += g.x * (gb.x - gb.y);
}

imageStore(out_y, p, vec4(val, 0.0, 0.0, 0.0));
)GLSL";

	// ---- Utility: Copy R32F texture (used to seed carryCleanTex_ cleanly on first/cut frame) ----
	constexpr const char* kPassCopyR32F = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;
float v = texelFetch(src_tex, p, 0).x;
imageStore(dst_tex, p, vec4(v, 0.0, 0.0, 0.0));
)GLSL";

	// ---- CHROMA GPU APPLY PASSES (TD-S4 GPU Twins: YUV sited upsample & unclipped RGB matrix) ----
	constexpr const char* kPassChromaDeltaHeader = R"GLSL(
vec2 td_sample_coded_delta_sited(ivec2 dp)
{
    if (dp.x < TD_DAX0 || dp.x >= TD_DAX1 || dp.y < TD_DAY0 || dp.y >= TD_DAY1)
        return vec2(0.0);

    if (TD_IDENTITY_GRID != 0)
    {
        vec2 d = texelFetch(coded_delta, dp, 0).xy;
        if (td_is_bad(d.x)) d.x = 0.0;
        if (td_is_bad(d.y)) d.y = 0.0;
        return d;
    }

    int cAw = TD_CAX1 - TD_CAX0;
    int cAh = TD_CAY1 - TD_CAY0;
    if (cAw <= 0 || cAh <= 0)
        return vec2(0.0);

    float lx = (float(dp.x) + 0.5) * float(TD_DSUB_X) - 0.5 + TD_DSHIFT_X;
    float ly = (float(dp.y) + 0.5) * float(TD_DSUB_Y) - 0.5 + TD_DSHIFT_Y;
    float cxLocal = (lx + 0.5 - TD_CSHIFT_X) / float(TD_CSUB_X) - 0.5 - float(TD_CAX0);
    float cyLocal = (ly + 0.5 - TD_CSHIFT_Y) / float(TD_CSUB_Y) - 0.5 - float(TD_CAY0);

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
                ivec2 gq = ivec2(TD_CAX0 + i0.x - 1 + i, TD_CAY0 + i0.y - 1 + j);
                row += wx[i] * texelFetch(coded_delta, gq, 0).xy;
            }
            acc += wy[j] * row;
        }
        outD = acc;
    }
    else
    {
        vec2 cl = clamp(vec2(cxLocal, cyLocal), vec2(0.0), vec2(float(cAw - 1), float(cAh - 1)));
        ivec2 i0 = ivec2(floor(cl));
        ivec2 i1 = min(ivec2(cAw - 1, cAh - 1), i0 + ivec2(1));
        vec2 f = cl - vec2(i0);
        vec2 v00 = texelFetch(coded_delta, ivec2(TD_CAX0 + i0.x, TD_CAY0 + i0.y), 0).xy;
        vec2 v10 = texelFetch(coded_delta, ivec2(TD_CAX0 + i1.x, TD_CAY0 + i0.y), 0).xy;
        vec2 v01 = texelFetch(coded_delta, ivec2(TD_CAX0 + i0.x, TD_CAY0 + i1.y), 0).xy;
        vec2 v11 = texelFetch(coded_delta, ivec2(TD_CAX0 + i1.x, TD_CAY0 + i1.y), 0).xy;
        vec2 top = v00 + (v10 - v00) * f.x;
        vec2 bot = v01 + (v11 - v01) * f.x;
        outD = top * (1.0 - f.y) + bot * f.y;
    }

    if (td_is_bad(outD.x)) outD.x = 0.0;
    if (td_is_bad(outD.y)) outD.y = 0.0;
    return outD;
}
)GLSL";

	// Inputs: deliv_uv (dW x dH RG32F), coded_delta (cW x cH RG32F).
	// Output: out_uv_delta (dW x dH RGBA32F: .xy = outCb/outCr, .zw = delivDeltaCb/delivDeltaCr).
	constexpr const char* kPassChromaApplyYuv = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_DW || p.y >= TD_DH)
    return;

vec2 deliv = texelFetch(deliv_uv, p, 0).xy;
vec2 delta = td_sample_coded_delta_sited(p);
float outCb = (delta.x == 0.0) ? deliv.x : (deliv.x + delta.x);
float outCr = (delta.y == 0.0) ? deliv.y : (deliv.y + delta.y);
imageStore(out_uv_delta, p, vec4(outCb, outCr, delta.x, delta.y));
)GLSL";

	// Inputs: deliv_rgb (dW x dH RGBA32F), coded_delta (cW x cH RG32F).
	// Outputs: out_rgb (dW x dH RGBA32F, unclipped BTB/WTW preserved), out_delta (dW x dH RG32F).
	constexpr const char* kPassChromaApplyRgb = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_DW || p.y >= TD_DH)
    return;

vec3 rgb = texelFetch(deliv_rgb, p, 0).xyz;
vec2 delta = td_sample_coded_delta_sited(p);

if (delta.x == 0.0 && delta.y == 0.0)
{
    imageStore(out_rgb, p, vec4(rgb, 0.0));
    imageStore(out_delta, p, vec4(0.0));
    return;
}

if (td_is_bad(delta.x) || td_is_bad(delta.y) || !(TD_MAT_KG > TD_NUM_FLOOR))
{
    imageStore(out_rgb, p, vec4(rgb, 0.0));
    imageStore(out_delta, p, vec4(0.0));
    return;
}

float kR = 2.0 * (1.0 - TD_MAT_KR);
float kB = 2.0 * (1.0 - TD_MAT_KB);
float dR = kR * delta.y;
float dB = kB * delta.x;
float dG = -(TD_MAT_KR * kR / TD_MAT_KG) * delta.y - (TD_MAT_KB * kB / TD_MAT_KG) * delta.x;
vec3 nrgb = rgb + vec3(dR, dG, dB);

if (td_is_bad(nrgb.x) || td_is_bad(nrgb.y) || td_is_bad(nrgb.z))
{
    imageStore(out_rgb, p, vec4(rgb, 0.0));
    imageStore(out_delta, p, vec4(0.0));
    return;
}

imageStore(out_rgb, p, vec4(nrgb, 0.0));
imageStore(out_delta, p, vec4(delta, 0.0, 0.0));
)GLSL";

} // namespace Shaders
} // namespace Gpu
} // namespace TdV2
