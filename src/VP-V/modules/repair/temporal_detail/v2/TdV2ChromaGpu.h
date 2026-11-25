// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — SOTA #3: Fused RG32F GPU Coded-Grid Chroma Fusion Compute Pipeline
// Modular header (zero bloat in TdV2Gpu.h) operating on packed RG32F (Cb, Cr) at the exact coded
// chroma grid (cW x cH):
//   - 4:2:0 coded -> 1/2 x 1/2 = 1/4 luma pixels (~0.3 ms on RTX 3090, ~0.8 ms on RTX 3050 Ti)
//   - 4:2:2 coded -> 1/2 x 1   = 1/2 luma pixels (never discards vertical chroma!)
//   - 4:4:4 / RGB -> 1 x 1     = full resolution (never discards true 4:4:4 chroma!)
//   - Exact identity (0 reduction dispatches) when coded == delivered on YUV RG32F!
//
// Compute passes (strictly FP32 RG32F / RGBA32F, zero FP16):
//   - Pass C0 (optional): Sited coded-grid reduction (YUV RG32F or RGB RGBA32F via U26 LumaCoeff)
//   - Pass C1: Packed RG32F LDS bicubic warp with scaled luma motion + separable U2 BandFromBase L / M / H split
//   - Pass C2: 2-stage LDS cell stats + Chan-Pebay merge + closed-form w* = C^-1 c solve on Cb & Cr
//              with 3-sigma Huber gate, Pass-1 face mask, LOW-3 rack-focus/bokeh veto, and H9 missing-motion skip
//   - Pass C3/C4: Coded-grid blend + Parseval pixel bound + fused LDS Layer-4 TdGuardPort grain-preserving
//                 signed-delta output -> feeds directly into zero-copy GPU chroma delta apply (zero CPU roundtrip!)
//
// Shared ScratchArena: distinct scope kChromaArenaScope = 0x0403, owner "temporal_detail_v2_chroma", Tier::T1.
// Any arena shortage fails visibly (scratchShort = true, StandInKind::GpuScratchShortage, ZERO silent private fallback!).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TdV2
{
namespace Gpu
{
	// Distinct ScratchArena scope and owner name for TD v2 GPU chroma compute passes.
	constexpr uint16_t kChromaArenaScope = 0x0403;
	constexpr const char* kChromaArenaOwner = "temporal_detail_v2_chroma";

	// Maximum simultaneous chroma references fused on GPU (prevPeak, nextPeak, optional 3rd ref).
	constexpr int kMaxChromaGpuRefs = 3;

	namespace ChromaShaders
	{
		// Exact U2 BandFromBase L/M/H tap arrays for coded-grid chroma (sigmaFine = 1.0, sigmaBase = 2.5).
		inline std::string BuildChromaBandTapsHeader()
		{
			const auto tFine = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
			const auto tBase = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaBase, 3.0, true, 0.0);
			const int rFine = static_cast<int>(tFine.size() / 2);
			const int rBase = static_cast<int>(tBase.size() / 2);

			std::string s;
			s += VpvGpu::DefineInt("TD_RAD_FINE", rFine);
			s += VpvGpu::DefineInt("TD_RAD_BASE", rBase);
			s += VpvGpu::ConstArray("TD_TAP_FINE", static_cast<int>(tFine.size()),
				[&](int i) { return tFine[static_cast<size_t>(i)]; });
			s += VpvGpu::ConstArray("TD_TAP_BASE", static_cast<int>(tBase.size()),
				[&](int i) { return tBase[static_cast<size_t>(i)]; });
			return s;
		}

		// Shared sited coordinate helpers (exact algebraic match to SitingSpec::CodedToLumaX/Y & LumaToCodedX/Y).
		constexpr const char* kSitedCoordHelpers = R"GLSL(
float td_coded_to_luma_x(float cx)
{
    float s = (TD_CSUB_X > 1) ? u_cshift_x : 0.0;
    return (cx + 0.5) * float(TD_CSUB_X) - 0.5 + s;
}

float td_coded_to_luma_y(float cy)
{
    float s = (TD_CSUB_Y > 1) ? u_cshift_y : 0.0;
    return (cy + 0.5) * float(TD_CSUB_Y) - 0.5 + s;
}

float td_luma_to_coded_x(float lx)
{
    float s = (TD_CSUB_X > 1) ? u_cshift_x : 0.0;
    return (lx + 0.5 - s) / float(TD_CSUB_X) - 0.5;
}

float td_luma_to_coded_y(float ly)
{
    float s = (TD_CSUB_Y > 1) ? u_cshift_y : 0.0;
    return (ly + 0.5 - s) / float(TD_CSUB_Y) - 0.5;
}

ivec2 td_coded_to_luma_cell(ivec2 cp)
{
    float lx = td_coded_to_luma_x(float(cp.x));
    float ly = td_coded_to_luma_y(float(cp.y));
    int cx = clamp(max(0, int(floor(lx))) / TD_CELL_PX, 0, TD_CW - 1);
    int cy = clamp(max(0, int(floor(ly))) / TD_CELL_PX, 0, TD_CH - 1);
    return ivec2(cx, cy);
}
)GLSL";

		// ---- PASS C0: Sited Reduction of Delivered YUV (RG32F) or RGB (RGBA32F) -> Coded Grid (RG32F) ----
		// Skipped completely (0 dispatches) when deliveredFormat is YUV and codedSubsampling == deliveredSubsampling!
		constexpr const char* kPassChromaReduceHeader = R"GLSL(
vec2 td_fetch_deliv_uv(ivec2 q)
{
#if TD_REDUCE_FROM_RGB
    vec3 rgb = texelFetch(src_deliv, q, 0).xyz;
    if (td_is_bad(rgb.x) || td_is_bad(rgb.y) || td_is_bad(rgb.z))
        return vec2(u_mid_code, u_mid_code);
    float y = u_kr * rgb.x + u_kg * rgb.y + u_kb * rgb.z;
    float denB = 2.0 * (1.0 - u_kb);
    float denR = 2.0 * (1.0 - u_kr);
    float cb = (denB > TD_NUM_FLOOR) ? (u_mid_code + (rgb.z - y) / denB) : u_mid_code;
    float cr = (denR > TD_NUM_FLOOR) ? (u_mid_code + (rgb.x - y) / denR) : u_mid_code;
    return vec2(cb, cr);
#else
    vec2 uv = texelFetch(src_deliv, q, 0).xy;
    return vec2(td_is_bad(uv.x) ? u_mid_code : uv.x, td_is_bad(uv.y) ? u_mid_code : uv.y);
#endif
}
)GLSL";

		constexpr const char* kPassChromaReduce = R"GLSL(
ivec2 cp = ivec2(gl_GlobalInvocationID.xy);
if (cp.x >= TD_CH_W || cp.y >= TD_CH_H)
    return;

int dax0 = int(round(u_dax0));
int day0 = int(round(u_day0));
int dax1 = int(round(u_dax1));
int day1 = int(round(u_day1));
int dAw = max(1, dax1 - dax0);
int dAh = max(1, day1 - day0);

float lx = (float(cp.x) + 0.5) * float(TD_CSUB_X) - 0.5 + ((TD_CSUB_X > 1) ? u_cshift_x : 0.0);
float ly = (float(cp.y) + 0.5) * float(TD_CSUB_Y) - 0.5 + ((TD_CSUB_Y > 1) ? u_cshift_y : 0.0);
float dsx = (TD_DSUB_X > 1) ? u_dshift_x : 0.0;
float dsy = (TD_DSUB_Y > 1) ? u_dshift_y : 0.0;
float dxLocal = (lx + 0.5 - dsx) / float(TD_DSUB_X) - 0.5 - float(dax0);
float dyLocal = (ly + 0.5 - dsy) / float(TD_DSUB_Y) - 0.5 - float(day0);

vec2 outUv = vec2(u_mid_code);
if (dxLocal >= 1.0 && dyLocal >= 1.0 && dxLocal < float(dAw) - 2.0 && dyLocal < float(dAh) - 2.0)
{
    ivec2 i0 = ivec2(floor(vec2(dxLocal, dyLocal)));
    vec4 wx = td_cubic_weights(dxLocal - float(i0.x));
    vec4 wy = td_cubic_weights(dyLocal - float(i0.y));
    vec2 acc = vec2(0.0);
    for (int j = 0; j < 4; ++j)
    {
        vec2 row = vec2(0.0);
        for (int i = 0; i < 4; ++i)
        {
            ivec2 gq = ivec2(dax0 + i0.x - 1 + i, day0 + i0.y - 1 + j);
            row += wx[i] * td_fetch_deliv_uv(gq);
        }
        acc += wy[j] * row;
    }
    outUv = acc;
}
else
{
    vec2 cl = clamp(vec2(dxLocal, dyLocal), vec2(0.0), vec2(float(dAw - 1), float(dAh - 1)));
    ivec2 i0 = ivec2(floor(cl));
    ivec2 i1 = min(ivec2(dAw - 1, dAh - 1), i0 + ivec2(1));
    vec2 f = cl - vec2(i0);
    vec2 v00 = td_fetch_deliv_uv(ivec2(dax0 + i0.x, day0 + i0.y));
    vec2 v10 = td_fetch_deliv_uv(ivec2(dax0 + i1.x, day0 + i0.y));
    vec2 v01 = td_fetch_deliv_uv(ivec2(dax0 + i0.x, day0 + i1.y));
    vec2 v11 = td_fetch_deliv_uv(ivec2(dax0 + i1.x, day0 + i1.y));
    vec2 top = v00 + (v10 - v00) * f.x;
    vec2 bot = v01 + (v11 - v01) * f.x;
    outUv = top * (1.0 - f.y) + bot * f.y;
}

imageStore(dst_coded, cp, vec4(outUv, 0.0, 0.0));
)GLSL";

		// ---- PASS C1A: Packed RG32F LDS Bicubic Warp with Scaled Luma Motion Vectors ----
		// Dispatch: ceil(TD_CH_W / 16) x ceil(TD_CH_H / 16), group 16x16.
		// Uses 20x20 vec2 LDS tile when the 4 WG corner cells share an identical finite MV (workgroup-uniform).
		constexpr const char* kPassChromaWarpHeader = R"GLSL(
shared vec2 td_ch_sh_uv[20][20];
shared uint td_ch_sh_bad[20][20];

vec2 td_ch_cur_fallback(ivec2 gq)
{
    vec2 v = (u_same_ref_cur > 0.5) ? texelFetch(ref_uv, gq, 0).xy : texelFetch(cur_uv, gq, 0).xy;
    return vec2(td_is_bad(v.x) ? 0.0 : v.x, td_is_bad(v.y) ? 0.0 : v.y);
}

vec2 td_ch_fetch_active(ivec2 qLocal, ivec4 cActive, out bool isBad)
{
    ivec2 sz = cActive.zw - cActive.xy;
    ivec2 gq = cActive.xy + clamp(qLocal, ivec2(0), sz - ivec2(1));
    vec2 v = texelFetch(ref_uv, gq, 0).xy;
    if (td_is_bad(v.x) || td_is_bad(v.y))
    {
        isBad = true;
        return td_ch_cur_fallback(gq);
    }
    isBad = false;
    return v;
}

vec2 td_ch_bilinear_active(vec2 sLocal, ivec4 cActive, out bool anyBad)
{
    ivec2 sz = cActive.zw - cActive.xy;
    vec2 cl = clamp(sLocal, vec2(0.0), vec2(float(sz.x - 1), float(sz.y - 1)));
    ivec2 i0 = ivec2(floor(cl));
    ivec2 i1 = min(sz - ivec2(1), i0 + ivec2(1));
    vec2 f = cl - vec2(i0);
    bool b00 = false, b10 = false, b01 = false, b11 = false;
    vec2 v00 = td_ch_fetch_active(ivec2(i0.x, i0.y), cActive, b00);
    vec2 v10 = td_ch_fetch_active(ivec2(i1.x, i0.y), cActive, b10);
    vec2 v01 = td_ch_fetch_active(ivec2(i0.x, i1.y), cActive, b01);
    vec2 v11 = td_ch_fetch_active(ivec2(i1.x, i1.y), cActive, b11);
    anyBad = b00 || b10 || b01 || b11;
    vec2 top = v00 + (v10 - v00) * f.x;
    vec2 bot = v01 + (v11 - v01) * f.x;
    return top * (1.0 - f.y) + bot * f.y;
}

void td_ch_bicubic_direct(vec2 sLocal, ivec4 cActive, ivec2 p, out vec2 outUv, out int outFlags)
{
    ivec2 sz = cActive.zw - cActive.xy;
    outFlags = 0;
    if (sLocal.x >= 1.0 && sLocal.y >= 1.0 && sLocal.x < float(sz.x) - 2.0 && sLocal.y < float(sz.y) - 2.0)
    {
        ivec2 i0 = ivec2(floor(sLocal));
        vec4 wx = td_cubic_weights(sLocal.x - float(i0.x));
        vec4 wy = td_cubic_weights(sLocal.y - float(i0.y));
        bool anyBad = false;
        vec2 acc = vec2(0.0);
        for (int j = 0; j < 4; ++j)
        {
            vec2 row = vec2(0.0);
            for (int i = 0; i < 4; ++i)
            {
                bool bt = false;
                vec2 tap = td_ch_fetch_active(ivec2(i0.x - 1 + i, i0.y - 1 + j), cActive, bt);
                anyBad = anyBad || bt;
                row += wx[i] * tap;
            }
            acc += wy[j] * row;
        }
        if (td_is_bad(acc.x) || td_is_bad(acc.y))
        {
            outUv = td_ch_cur_fallback(p);
            outFlags = 4;
        }
        else
        {
            outUv = acc;
            outFlags = anyBad ? 4 : 1;
        }
    }
    else if (sLocal.x >= 0.0 && sLocal.y >= 0.0 && sLocal.x <= float(sz.x - 1) && sLocal.y <= float(sz.y - 1))
    {
        bool anyBad = false;
        vec2 bv = td_ch_bilinear_active(sLocal, cActive, anyBad);
        if (td_is_bad(bv.x) || td_is_bad(bv.y))
        {
            outUv = td_ch_cur_fallback(p);
            outFlags = 2 | 4;
        }
        else
        {
            outUv = bv;
            outFlags = 2 | (anyBad ? 4 : 1);
        }
    }
    else
    {
        // Out-of-bounds landing: mirror CPU TdV2Chroma.h:544 (keep cur_uv fallback when finite, else clamped ref border), valid = 0.
        vec2 fb = (u_same_ref_cur > 0.5) ? texelFetch(ref_uv, p, 0).xy : texelFetch(cur_uv, p, 0).xy;
        bool anyBad = false;
        vec2 bv = td_ch_bilinear_active(sLocal, cActive, anyBad);
        outUv = vec2(td_is_bad(fb.x) ? bv.x : fb.x, td_is_bad(fb.y) ? bv.y : fb.y);
        outFlags = 0;
    }
}
)GLSL";

		constexpr const char* kPassChromaWarp = R"GLSL(
ivec2 wg = ivec2(gl_WorkGroupID.xy);
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
int baseX = wg.x * 16;
int baseY = wg.y * 16;
ivec2 p = ivec2(baseX + loc.x, baseY + loc.y);

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
ivec4 cActive = ivec4(cax0, cay0, cax1, cay1);
bool validRect = (cax1 > cax0) && (cay1 > cay0);
int refIdx = int(round(u_ref_idx));
int motionSlot = clamp(int(round(u_motion_slot)), 0, 2);
bool flagsClear = (u_flags_clear > 0.5);

// Workgroup-uniform 20x20 LDS eligibility check: all 4 WG corners map to identical valid finite MV.
ivec2 c00 = td_coded_to_luma_cell(ivec2(baseX, baseY));
ivec2 c10 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX + 15), baseY));
ivec2 c01 = td_coded_to_luma_cell(ivec2(baseX, min(TD_CH_H - 1, baseY + 15)));
ivec2 c11 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX + 15), min(TD_CH_H - 1, baseY + 15)));
vec2 mv00 = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + c00.x, c00.y), 0).xy;
vec2 mv10 = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + c10.x, c10.y), 0).xy;
vec2 mv01 = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + c01.x, c01.y), 0).xy;
vec2 mv11 = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + c11.x, c11.y), 0).xy;
bool v00 = (texelFetch(motion_meta, ivec2(motionSlot * TD_CW + c00.x, c00.y), 0).y >= 0.5);
bool v10 = (texelFetch(motion_meta, ivec2(motionSlot * TD_CW + c10.x, c10.y), 0).y >= 0.5);
bool v01 = (texelFetch(motion_meta, ivec2(motionSlot * TD_CW + c01.x, c01.y), 0).y >= 0.5);
bool v11 = (texelFetch(motion_meta, ivec2(motionSlot * TD_CW + c11.x, c11.y), 0).y >= 0.5);
bool sameMv = v00 && v10 && v01 && v11 &&
              (!td_is_bad(mv00.x)) && (!td_is_bad(mv00.y)) &&
              (mv00 == mv10) && (mv00 == mv01) && (mv00 == mv11);
bool useH = (u_use_homography > 0.5);
vec2 wgMvCoded = vec2(mv00.x / float(TD_CSUB_X), mv00.y / float(TD_CSUB_Y));
// TD-AUDIT-H3: Only use 20x20 LDS tile when the 4 WG corners span at most 2x2 cells (guaranteed monotone coverage
// even when cellPx == 8 or |siting.shift| > 0.5 so every interior pixel shares the identical MV!).
bool useLds = (!useH) && validRect && sameMv &&
              (c11.x - c00.x <= 1) && (c11.y - c00.y <= 1) &&
              (abs(wgMvCoded.x) <= float(TD_CH_W)) && (abs(wgMvCoded.y) <= float(TD_CH_H));
int tileBaseX = 0;
int tileBaseY = 0;
if (useLds)
{
    tileBaseX = int(floor(float(baseX - cax0) + wgMvCoded.x)) - 1;
    tileBaseY = int(floor(float(baseY - cay0) + wgMvCoded.y)) - 1;
    int tileMaxX = int(floor(float(baseX + 15 - cax0) + wgMvCoded.x)) + 2;
    int tileMaxY = int(floor(float(baseY + 15 - cay0) + wgMvCoded.y)) + 2;
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
        td_ch_sh_uv[ty][tx] = td_ch_fetch_active(ivec2(tileBaseX + tx, tileBaseY + ty), cActive, bad);
        td_ch_sh_bad[ty][tx] = bad ? 1u : 0u;
    }
}
memoryBarrierShared();
barrier();

if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;

uint prevPacked = flagsClear ? 0u : uint(round(texelFetch(ref_flags_in, p, 0).x * 65535.0));

if (!validRect || p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    vec2 fb = td_ch_cur_fallback(p);
    uint np = td_pack_ref_flags(prevPacked, refIdx, 0);
    imageStore(warped_uv, p, vec4(fb, 0.0, 0.0));
    imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
    return;
}

ivec2 lc = td_coded_to_luma_cell(p);
vec2 mvLuma = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + lc.x, lc.y), 0).xy;
vec2 mvMeta = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + lc.x, lc.y), 0).xy;
int mvCls = int(round(mvMeta.x));
bool mvValid = (mvMeta.y >= 0.5);

vec2 selfRef = texelFetch(ref_uv, p, 0).xy;
int extraBad = (td_is_bad(selfRef.x) || td_is_bad(selfRef.y)) ? 4 : 0;

if (!mvValid)
{
    vec2 fb = td_ch_cur_fallback(p);
    uint np = td_pack_ref_flags(prevPacked, refIdx, 0);
    if (extraBad != 0) np |= 512u;
    imageStore(warped_uv, p, vec4(fb, 0.0, 0.0));
    imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
    return;
}

ivec2 sz = cActive.zw - cActive.xy;
vec2 sLocal = vec2(0.0);
if (useH && mvCls == TD_CLS_CAMERA)
{
    float lx = td_coded_to_luma_x(float(p.x));
    float ly = td_coded_to_luma_y(float(p.y));
    float hw = u_h6 * lx + u_h7 * ly + u_h8;
    float refLx = (abs(hw) > TD_NUM_FLOOR) ? ((u_h0 * lx + u_h1 * ly + u_h2) / hw) : 0.0;
    float refLy = (abs(hw) > TD_NUM_FLOOR) ? ((u_h3 * lx + u_h4 * ly + u_h5) / hw) : 0.0;
    bool hBad = td_is_bad(u_h0) || td_is_bad(u_h1) || td_is_bad(u_h2) ||
                td_is_bad(u_h3) || td_is_bad(u_h4) || td_is_bad(u_h5) ||
                td_is_bad(u_h6) || td_is_bad(u_h7) || td_is_bad(u_h8) ||
                td_is_bad(hw) || !(abs(hw) > TD_NUM_FLOOR) || td_is_bad(refLx) || td_is_bad(refLy);
    if (hBad)
    {
        vec2 fb = td_ch_cur_fallback(p);
        uint np = td_pack_ref_flags(prevPacked, refIdx, 4) | 512u;
        imageStore(warped_uv, p, vec4(fb, 0.0, 0.0));
        imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
        return;
    }
    sLocal = vec2(td_luma_to_coded_x(refLx) - float(cax0), td_luma_to_coded_y(refLy) - float(cay0));
}
else
{
    if (td_is_bad(mvLuma.x) || td_is_bad(mvLuma.y))
    {
        vec2 fb = td_ch_cur_fallback(p);
        uint np = td_pack_ref_flags(prevPacked, refIdx, 4);
        if (extraBad != 0) np |= 512u;
        imageStore(warped_uv, p, vec4(fb, 0.0, 0.0));
        imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
        return;
    }
    vec2 mvCoded = vec2(mvLuma.x / float(TD_CSUB_X), mvLuma.y / float(TD_CSUB_Y));
    sLocal = vec2(float(p.x - cax0), float(p.y - cay0)) + mvCoded;
}
vec2 outUv = vec2(0.0);
int outFlags = 0;

bool ldsInBounds = false;
ivec2 ldsI0 = ivec2(0);
int ldsRx = 0;
int ldsRy = 0;
if (useLds &&
    sLocal.x >= 1.0 && sLocal.y >= 1.0 &&
    sLocal.x < float(sz.x) - 2.0 && sLocal.y < float(sz.y) - 2.0)
{
    ldsI0 = ivec2(floor(sLocal));
    ldsRx = (ldsI0.x - 1) - tileBaseX;
    ldsRy = (ldsI0.y - 1) - tileBaseY;
    ldsInBounds = (ldsRx >= 0 && ldsRx + 3 < 20 && ldsRy >= 0 && ldsRy + 3 < 20);
}
if (ldsInBounds)
{
    vec4 wx = td_cubic_weights(sLocal.x - float(ldsI0.x));
    vec4 wy = td_cubic_weights(sLocal.y - float(ldsI0.y));
    uint badOr = 0u;
    vec2 acc = vec2(0.0);
    for (int j = 0; j < 4; ++j)
    {
        vec2 row = vec2(0.0);
        for (int i = 0; i < 4; ++i)
        {
            badOr |= td_ch_sh_bad[ldsRy + j][ldsRx + i];
            row += wx[i] * td_ch_sh_uv[ldsRy + j][ldsRx + i];
        }
        acc += wy[j] * row;
    }
    if (td_is_bad(acc.x) || td_is_bad(acc.y))
    {
        outUv = td_ch_cur_fallback(p);
        outFlags = 4;
    }
    else
    {
        outUv = acc;
        outFlags = (badOr != 0u) ? 4 : 1;
    }
}
else
{
    td_ch_bicubic_direct(sLocal, cActive, p, outUv, outFlags);
}

outFlags = outFlags | extraBad;
uint newPacked = td_pack_ref_flags(prevPacked, refIdx, outFlags);
if (extraBad != 0)
    newPacked |= 512u;
imageStore(warped_uv, p, vec4(outUv, 0.0, 0.0));
imageStore(ref_flags_out, p, vec4(float(newPacked) / 65535.0, 0.0, 0.0, 0.0));
)GLSL";

		// ---- PASS C1B & C1C: Packed RG32F Separable U2 BandFromBase L/M/H Split (Cb & Cr in ONE pass pair!) ----
		constexpr const char* kPassChromaBandHHeader = R"GLSL(
shared vec2 td_ch_h[16][16 + 2 * TD_RAD_BASE];
)GLSL";

		constexpr const char* kPassChromaBandH = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
ivec2 p = ivec2(baseX + lx, baseY + ly);

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool validRect = (cax1 > cax0) && (cay1 > cay0);
int xMin = cax0;
int xMax = max(cax0, cax1 - 1);
int safeY = clamp(baseY + ly, 0, TD_CH_H - 1);

int tileW = 16 + 2 * TD_RAD_BASE;
for (int col = lx; col < tileW; col += 16)
{
    int gx = baseX - TD_RAD_BASE + col;
    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_CH_W - 1);
    vec2 sv = texelFetch(src_uv, ivec2(sx, safeY), 0).xy;
    td_ch_h[ly][col] = vec2(td_is_bad(sv.x) ? 0.0 : sv.x, td_is_bad(sv.y) ? 0.0 : sv.y);
}
memoryBarrierShared();
barrier();

if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;

if (!validRect || p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    imageStore(tmp_cb, p, vec4(0.0));
    imageStore(tmp_cr, p, vec4(0.0));
    return;
}

int center = lx + TD_RAD_BASE;
vec2 srcVal = td_ch_h[ly][center];
vec2 accFine = vec2(0.0);
vec2 accBase = vec2(0.0);
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    vec2 v = td_ch_h[ly][center + k];
    accBase += TD_TAP_BASE[k + TD_RAD_BASE] * v;
    if (abs(k) <= TD_RAD_FINE)
        accFine += TD_TAP_FINE[k + TD_RAD_FINE] * v;
}

imageStore(tmp_cb, p, vec4(accFine.x, accBase.x, srcVal.x, 0.0));
imageStore(tmp_cr, p, vec4(accFine.y, accBase.y, srcVal.y, 0.0));
)GLSL";

		constexpr const char* kPassChromaBandVHeader = R"GLSL(
shared vec4 td_ch_vcb[16 + 2 * TD_RAD_BASE][16];
shared vec4 td_ch_vcr[16 + 2 * TD_RAD_BASE][16];
)GLSL";

		constexpr const char* kPassChromaBandV = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
ivec2 p = ivec2(baseX + lx, baseY + ly);

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool validRect = (cax1 > cax0) && (cay1 > cay0);
int yMin = cay0;
int yMax = max(cay0, cay1 - 1);
int safeX = clamp(baseX + lx, 0, TD_CH_W - 1);

int tileH = 16 + 2 * TD_RAD_BASE;
for (int row = ly; row < tileH; row += 16)
{
    int gy = baseY - TD_RAD_BASE + row;
    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_CH_H - 1);
    td_ch_vcb[row][lx] = texelFetch(tmp_cb, ivec2(safeX, sy), 0);
    td_ch_vcr[row][lx] = texelFetch(tmp_cr, ivec2(safeX, sy), 0);
}
memoryBarrierShared();
barrier();

if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;

if (!validRect || p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    imageStore(bands_l, p, vec4(0.0));
    imageStore(bands_mh, p, vec4(0.0));
    return;
}

int center = ly + TD_RAD_BASE;
vec2 srcVal = vec2(td_ch_vcb[center][lx].z, td_ch_vcr[center][lx].z);
vec2 gFine = vec2(0.0);
vec2 gBase = vec2(0.0);
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    vec2 vFine = vec2(td_ch_vcb[center + k][lx].x, td_ch_vcr[center + k][lx].x);
    vec2 vBase = vec2(td_ch_vcb[center + k][lx].y, td_ch_vcr[center + k][lx].y);
    gBase += TD_TAP_BASE[k + TD_RAD_BASE] * vBase;
    if (abs(k) <= TD_RAD_FINE)
        gFine += TD_TAP_FINE[k + TD_RAD_FINE] * vFine;
}

vec2 bL = gBase;
vec2 bM = gFine - bL;
vec2 bH = srcVal - gFine;
imageStore(bands_l, p, vec4(bL, 0.0, 0.0));
imageStore(bands_mh, p, vec4(bM.x, bM.y, bH.x, bH.y));
)GLSL";

		// ---- PASS C1B/C1C (Branch A Unidentified): Exact Separable Coarse-Only Blur (sigma = 32 luma px, M = H = 0) ----
		constexpr const char* kPassChromaCoarseH = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;
int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool validRect = (cax1 > cax0) && (cay1 > cay0);
if (!validRect || p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    imageStore(tmp_cb, p, vec4(0.0));
    return;
}
int xMax = max(cax0, cax1 - 1);
int rad = max(0, int(round(u_coarse_rad_x)));
float inv2SigSq = (u_coarse_sigma_x > TD_NUM_FLOOR) ? (0.5 / (u_coarse_sigma_x * u_coarse_sigma_x)) : 0.0;
vec2 acc = vec2(0.0);
float wSum = 0.0;
for (int k = -rad; k <= rad; ++k)
{
    int sx = clamp(p.x + k, cax0, xMax);
    vec2 v = texelFetch(src_uv, ivec2(sx, p.y), 0).xy;
    if (td_is_bad(v.x)) v.x = 0.0;
    if (td_is_bad(v.y)) v.y = 0.0;
    float w = (rad == 0) ? 1.0 : exp(-float(k * k) * inv2SigSq);
    acc += w * v;
    wSum += w;
}
vec2 outUv = (wSum > TD_NUM_FLOOR) ? (acc / wSum) : vec2(0.0);
imageStore(tmp_cb, p, vec4(outUv, 0.0, 0.0));
)GLSL";

		constexpr const char* kPassChromaCoarseV = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;
int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool validRect = (cax1 > cax0) && (cay1 > cay0);
if (!validRect || p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    imageStore(bands_l, p, vec4(0.0));
    imageStore(bands_mh, p, vec4(0.0));
    return;
}
int yMax = max(cay0, cay1 - 1);
int rad = max(0, int(round(u_coarse_rad_y)));
float inv2SigSq = (u_coarse_sigma_y > TD_NUM_FLOOR) ? (0.5 / (u_coarse_sigma_y * u_coarse_sigma_y)) : 0.0;
vec2 acc = vec2(0.0);
float wSum = 0.0;
for (int k = -rad; k <= rad; ++k)
{
    int sy = clamp(p.y + k, cay0, yMax);
    vec2 v = texelFetch(tmp_cb, ivec2(p.x, sy), 0).xy;
    float w = (rad == 0) ? 1.0 : exp(-float(k * k) * inv2SigSq);
    acc += w * v;
    wSum += w;
}
vec2 outUv = (wSum > TD_NUM_FLOOR) ? (acc / wSum) : vec2(0.0);
imageStore(bands_l, p, vec4(outUv, 0.0, 0.0));
imageStore(bands_mh, p, vec4(0.0));
)GLSL";

		// ---- PASS C2A: Coalesced 16x16 LDS Cell-Moment Reduction for Cb & Cr Simultaneously ----
		// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per luma cell).
		constexpr const char* kPassChromaCellStatsHeader = R"GLSL(
shared vec4 td_cs_red4[256];
shared vec4 td_cs_red4b[256];
shared uint td_cs_mask[256];
shared float td_cs_mu[3];
shared float td_cs_nc;

vec2 td_ch_fetch_cur_b(ivec2 p, int b)
{
    if (b == 0) return texelFetch(cur_l, p, 0).xy;
    vec4 mh = texelFetch(cur_mh, p, 0);
    return (b == 1) ? mh.xy : mh.zw;
}

vec2 td_ch_fetch_ref_b(int k, ivec2 p, int b)
{
    if (b == 0)
    {
        if (k == 0) return texelFetch(ref_l_0, p, 0).xy;
        if (k == 1) return texelFetch(ref_l_1, p, 0).xy;
        return texelFetch(ref_l_2, p, 0).xy;
    }
    vec4 mh = vec4(0.0);
    if (k == 0) mh = texelFetch(ref_mh_0, p, 0);
    else if (k == 1) mh = texelFetch(ref_mh_1, p, 0);
    else mh = texelFetch(ref_mh_2, p, 0);
    return (b == 1) ? mh.xy : mh.zw;
}
)GLSL";

		constexpr const char* kPassChromaCellStatsPart1 = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;

int K = int(round(u_k));
int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool hasCleanSplit = (u_has_clean_split > 0.5);
bool sameCleanRec = (u_same_clean_rec > 0.5);
bool bokehAllowL = (u_bokeh_mode > 0.5);

int expFocusBits = int(round(texelFetch(cell_prior, cell, 0).y));
bool cellFocusChange = ((expFocusBits & 1) != 0);
bool cellIsDefocused = ((expFocusBits & 2) != 0);
bool cellIsFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);

bool cellGateL = true;
bool cellGateMH = true;
for (int k = 0; k < K; ++k)
{
    vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).xy;
    if (mg.x <= 0.0) cellGateL = false;
    if (mg.y <= 0.0) cellGateMH = false;
}

// Coded pixel window spanning luma cell (cell.x, cell.y), padded for arbitrary siting shifts.
int padX = (TD_CSUB_X > 1) ? (int(ceil(abs(u_cshift_x) / float(TD_CSUB_X))) + 1) : 0;
int padY = (TD_CSUB_Y > 1) ? (int(ceil(abs(u_cshift_y) / float(TD_CSUB_Y))) + 1) : 0;
int cellStartX = max(0, (cell.x * TD_CELL_PX) / TD_CSUB_X - padX);
int cellStartY = max(0, (cell.y * TD_CELL_PX) / TD_CSUB_Y - padY);
int cellEndX = min(TD_CH_W, ((cell.x + 1) * TD_CELL_PX + TD_CSUB_X - 1) / TD_CSUB_X + padX);
int cellEndY = min(TD_CH_H, ((cell.y + 1) * TD_CELL_PX + TD_CSUB_Y - 1) / TD_CSUB_Y + padY);
int cx0 = max(cax0, cellStartX);
int cy0 = max(cay0, cellStartY);
int cx1 = min(cax1, cellEndX);
int cy1 = min(cay1, cellEndY);
int cellW = max(0, cellEndX - cellStartX);
int cellH = max(0, cellEndY - cellStartY);
int subX = (cellW + 15) / 16;
int subY = (cellH + 15) / 16;
bool canCacheSingle = (subX <= 1 && subY <= 1);

float ogCb2 = 0.0;
float ogCr2 = 0.0;
float activeCnt = 0.0;
float borderCnt = 0.0;
uint badMask = 0u;

vec3 sumCb[3];
vec3 sumCr[3];
float cntB[3];
bool singleValid[3];
vec3 singleCb[3];
vec3 singleCr[3];
for (int b = 0; b < 3; ++b)
{
    sumCb[b] = vec3(0.0);
    sumCr[b] = vec3(0.0);
    cntB[b] = 0.0;
    singleValid[b] = false;
    singleCb[b] = vec3(0.0);
    singleCr[b] = vec3(0.0);
}

for (int sy = 0; sy < subY; ++sy)
{
    int oy = sy * 16 + ly;
    int py = cellStartY + oy;
    if (oy >= cellH || py < cy0 || py >= cy1) continue;
    for (int sx = 0; sx < subX; ++sx)
    {
        int ox = sx * 16 + lx;
        int px = cellStartX + ox;
        if (ox >= cellW || px < cx0 || px >= cx1) continue;
        ivec2 p = ivec2(px, py);
        if (td_coded_to_luma_cell(p) != cell) continue;

        if (hasCleanSplit && !sameCleanRec)
        {
            vec2 rv = texelFetch(rec_uv, p, 0).xy;
            vec2 cv = texelFetch(cur_uv, p, 0).xy;
            if (!td_is_bad(rv.x) && !td_is_bad(cv.x)) { float d = rv.x - cv.x; ogCb2 += d * d; }
            if (!td_is_bad(rv.y) && !td_is_bad(cv.y)) { float d = rv.y - cv.y; ogCr2 += d * d; }
        }
        activeCnt += 1.0;

        uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
        if ((packedFl & 512u) != 0u) badMask |= 16u;
        bool allRefValid = true;
        for (int k = 0; k < K; ++k)
        {
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
                vec2 curB = td_ch_fetch_cur_b(p, b);
                vec3 dCb = vec3(0.0);
                vec3 dCr = vec3(0.0);
                bool finiteOk = true;
                for (int k = 0; k < K; ++k)
                {
                    vec2 rB = td_ch_fetch_ref_b(k, p, b);
                    vec2 d = curB - rB;
                    if (td_is_bad(d.x) || td_is_bad(d.y)) { finiteOk = false; break; }
                    if (k == 0) { dCb.x = d.x; dCr.x = d.y; }
                    else if (k == 1) { dCb.y = d.x; dCr.y = d.y; }
                    else { dCb.z = d.x; dCr.z = d.y; }
                }
                if (finiteOk)
                {
                    sumCb[b] += dCb;
                    sumCr[b] += dCr;
                    cntB[b] += 1.0;
                    if (canCacheSingle)
                    {
                        singleValid[b] = true;
                        singleCb[b] = dCb;
                        singleCr[b] = dCr;
                    }
                }
            }
        }
    }
}

td_cs_red4[tid] = vec4(ogCb2, ogCr2, activeCnt, borderCnt);
td_cs_mask[tid] = badMask;
memoryBarrierShared();
barrier();
for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_cs_red4[tid] += td_cs_red4[tid + stride];
        td_cs_mask[tid] |= td_cs_mask[tid + stride];
    }
    memoryBarrierShared();
    barrier();
}
if (tid == 0)
{
    float nAct = td_cs_red4[0].z;
    float ogSumCb2 = hasCleanSplit ? td_cs_red4[0].x : 0.0;
    float ogSumCr2 = hasCleanSplit ? td_cs_red4[0].y : 0.0;
    imageStore(cell_own_grain, cell, vec4(ogSumCb2, ogSumCr2, nAct, 0.0));
    uint dMask = (td_cs_mask[0] & 7u) | (cellIsFace ? 8u : 0u) | (td_cs_mask[0] & 16u);
    imageStore(warp_diag, cell, vec4(td_cs_red4[0].w, float(dMask), 0.0, 0.0));
}
memoryBarrierShared();
barrier();
)GLSL";

		constexpr const char* kPassChromaCellStatsPart2 = R"GLSL(
for (int chIdx = 0; chIdx < 2; ++chIdx)
{
    for (int b = 0; b < 3; ++b)
    {
        bool bandVeto = cellFocusChange || (cellIsDefocused && (b >= 1 || !bokehAllowL));
        bool skipMoments = cellIsFace || bandVeto;
        vec3 sVec = (chIdx == 0) ? sumCb[b] : sumCr[b];
        td_cs_red4[tid] = skipMoments ? vec4(0.0) : vec4(sVec, cntB[b]);
        memoryBarrierShared();
        barrier();
        for (int stride = 128; stride > 0; stride >>= 1)
        {
            if (tid < stride)
                td_cs_red4[tid] += td_cs_red4[tid + stride];
            memoryBarrierShared();
            barrier();
        }
        if (tid == 0)
        {
            float nc = skipMoments ? 0.0 : td_cs_red4[0].w;
            td_cs_nc = nc;
            float invN = (nc > 0.5) ? (1.0 / nc) : 0.0;
            td_cs_mu[0] = td_cs_red4[0].x * invN;
            td_cs_mu[1] = td_cs_red4[0].y * invN;
            td_cs_mu[2] = td_cs_red4[0].z * invN;
        }
        memoryBarrierShared();
        barrier();

        float ncCell = td_cs_nc;
        vec3 mu = vec3(td_cs_mu[0], td_cs_mu[1], td_cs_mu[2]);
        vec4 scA = vec4(0.0);
        vec4 scB = vec4(0.0);
        if (!skipMoments && ncCell > 0.5)
        {
            if (canCacheSingle)
            {
                if (singleValid[b])
                {
                    vec3 dv = (chIdx == 0) ? singleCb[b] : singleCr[b];
                    vec3 e = dv - mu;
                    scA = vec4(e.x * e.x, e.x * e.y, e.x * e.z, e.y * e.y);
                    scB = vec4(e.y * e.z, e.z * e.z, 0.0, 0.0);
                }
            }
            else
            {
                bool gOk = (b == 0) ? cellGateL : cellGateMH;
                int totalSub = subX * subY;
                for (int subIdx = 0; subIdx < totalSub; ++subIdx)
                {
                    int sy = subIdx / max(1, subX);
                    int sx = subIdx - sy * max(1, subX);
                    int oy = sy * 16 + ly;
                    int ox = sx * 16 + lx;
                    int py = cellStartY + oy;
                    int px = cellStartX + ox;
                    if (gOk && oy < cellH && ox < cellW && py >= cy0 && py < cy1 && px >= cx0 && px < cx1)
                    {
                        ivec2 p = ivec2(px, py);
                        if (td_coded_to_luma_cell(p) == cell)
                        {
                            uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
                            bool allOk = true;
                            vec2 curB = td_ch_fetch_cur_b(p, b);
                            vec3 dv = vec3(0.0);
                            if (K > 0)
                            {
                                if ((td_unpack_ref_flags(packedFl, 0) & 1) == 0) allOk = false;
                                else
                                {
                                    vec2 r0 = td_ch_fetch_ref_b(0, p, b);
                                    float d0 = (chIdx == 0) ? (curB.x - r0.x) : (curB.y - r0.y);
                                    if (td_is_bad(d0)) allOk = false;
                                    else dv.x = d0;
                                }
                            }
                            if (allOk && K > 1)
                            {
                                if ((td_unpack_ref_flags(packedFl, 1) & 1) == 0) allOk = false;
                                else
                                {
                                    vec2 r1 = td_ch_fetch_ref_b(1, p, b);
                                    float d1 = (chIdx == 0) ? (curB.x - r1.x) : (curB.y - r1.y);
                                    if (td_is_bad(d1)) allOk = false;
                                    else dv.y = d1;
                                }
                            }
                            if (allOk && K > 2)
                            {
                                if ((td_unpack_ref_flags(packedFl, 2) & 1) == 0) allOk = false;
                                else
                                {
                                    vec2 r2 = td_ch_fetch_ref_b(2, p, b);
                                    float d2 = (chIdx == 0) ? (curB.x - r2.x) : (curB.y - r2.y);
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
        }

        td_cs_red4[tid] = scA;
        td_cs_red4b[tid] = scB;
        memoryBarrierShared();
        barrier();
        for (int stride = 128; stride > 0; stride >>= 1)
        {
            if (tid < stride)
            {
                td_cs_red4[tid]  += td_cs_red4[tid + stride];
                td_cs_red4b[tid] += td_cs_red4b[tid + stride];
            }
            memoryBarrierShared();
            barrier();
        }
        if (tid == 0)
        {
            vec4 r0 = (skipMoments || ncCell <= 0.5) ? vec4(0.0) : vec4(ncCell, mu.x, mu.y, mu.z);
            vec4 r1 = (skipMoments || ncCell <= 0.5) ? vec4(0.0) : td_cs_red4[0];
            vec4 r2 = (skipMoments || ncCell <= 0.5) ? vec4(0.0) : vec4(td_cs_red4b[0].xy, 0.0, 0.0);
            if (chIdx == 0)
            {
                imageStore(cell_stats_cb, ivec2(b * TD_CW + cell.x, cell.y), r0);
                imageStore(cell_stats_cb, ivec2(b * TD_CW + cell.x, TD_CH + cell.y), r1);
                imageStore(cell_stats_cb, ivec2(b * TD_CW + cell.x, 2 * TD_CH + cell.y), r2);
            }
            else
            {
                imageStore(cell_stats_cr, ivec2(b * TD_CW + cell.x, cell.y), r0);
                imageStore(cell_stats_cr, ivec2(b * TD_CW + cell.x, TD_CH + cell.y), r1);
                imageStore(cell_stats_cr, ivec2(b * TD_CW + cell.x, 2 * TD_CH + cell.y), r2);
            }
        }
        memoryBarrierShared();
        barrier();
    }
}
)GLSL";

		inline std::string BuildPassChromaCellStatsBody()
		{
			return std::string(kPassChromaCellStatsPart1) + kPassChromaCellStatsPart2;
		}

		// ---- PASS C2B: Chan-Pebay Merge + Blind A1/A2/A3 + Closed-Form w* = C^-1 c Solve (Cb & Cr) ----
		// Reuses kPass2MergeSolveHeader helpers (td_solve_convex, td_est_c_a1, td_select_blind_c, td_find_independent_a1_partner).
		constexpr const char* kPassChromaMergeSolve = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int K = int(round(u_k));
int poolR = int(round(u_pool_r));
int estMode = int(round(u_estimator));
bool bokehAllowL = (u_bokeh_mode > 0.5);

vec4 priorTexel = texelFetch(cell_prior, cell, 0);
int expFocusBits = int(round(priorTexel.y));
bool focusChange = ((expFocusBits & 1) != 0);
bool isDefocused = ((expFocusBits & 2) != 0);
bool isFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);

bool cellNonFinite = false;
bool cellSingular = false;
bool cellSingleRefA2 = false;

for (int k = 0; k < K; ++k)
{
    if (texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).z > 0.5)
        cellNonFinite = true;
}

float rawGvCb = priorTexel.x;
float rawGvCr = (u_cell_prior_has_cr_grain > 0.5) ? priorTexel.w : priorTexel.x;
float retGVar[2];
retGVar[0] = 0.0;
retGVar[1] = 0.0;
if (td_is_bad(rawGvCb) || rawGvCb < 0.0) cellNonFinite = true;
else retGVar[0] = rawGvCb;
if (td_is_bad(rawGvCr) || rawGvCr < 0.0) cellNonFinite = true;
else retGVar[1] = rawGvCr;

float rawStepScale = (u_cell_prior_has_step_scale > 0.5) ? priorTexel.z : 1.0;
float cellStepScale = 0.0;
if (td_is_bad(rawStepScale) || !(rawStepScale > 0.0)) cellNonFinite = true;
else cellStepScale = clamp(rawStepScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX);
float cellStepScaleSq = cellStepScale * cellStepScale;

// Pool own-grain variance over the exact same [-poolR, +poolR] window as C (1:1 parity with CPU TdV2Chroma.h).
vec2 ogSumPool = vec2(0.0);
float ogCntPool = 0.0;
for (int dy = -poolR; dy <= poolR; ++dy)
{
    int ncy = cell.y + dy;
    if (ncy < 0 || ncy >= TD_CH) continue;
    for (int dx = -poolR; dx <= poolR; ++dx)
    {
        int ncx = cell.x + dx;
        if (ncx < 0 || ncx >= TD_CW) continue;
        vec3 ogT = texelFetch(cell_own_grain, ivec2(ncx, ncy), 0).xyz;
        ogSumPool += ogT.xy;
        ogCntPool += ogT.z;
    }
}
vec2 ownGrainVar = (ogCntPool > 0.5) ? max(vec2(0.0), ogSumPool / ogCntPool) : vec2(0.0);

for (int chIdx = 0; chIdx < 2; ++chIdx)
{
    vec4 bandW[3];
    bandW[0] = vec4(0.0); bandW[1] = vec4(0.0); bandW[2] = vec4(0.0);
    float chRetGVar = retGVar[chIdx];
    float chOwnGVar = (chIdx == 0) ? ownGrainVar.x : ownGrainVar.y;

    for (int b = 0; b < 3; ++b)
    {
        float nPool = 0.0;
        vec3 muPool = vec3(0.0);
        float s00 = 0.0, s01 = 0.0, s02 = 0.0, s11 = 0.0, s12 = 0.0, s22 = 0.0;
        bool hasOwnStats = false;
        vec3 ownDiag = vec3(0.0);

        for (int dy = -poolR; dy <= poolR; ++dy)
        {
            int ncy = cell.y + dy;
            if (ncy < 0 || ncy >= TD_CH) continue;
            for (int dx = -poolR; dx <= poolR; ++dx)
            {
                int ncx = cell.x + dx;
                if (ncx < 0 || ncx >= TD_CW) continue;
                vec4 t0 = (chIdx == 0)
                    ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, ncy), 0)
                    : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, ncy), 0);
                float nc = t0.x;
                if (nc <= 0.5) continue;
                vec3 muc = t0.yzw;
                vec4 t1 = (chIdx == 0)
                    ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0)
                    : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
                vec2 t2 = (chIdx == 0)
                    ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0).xy
                    : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0).xy;
                if (dx == 0 && dy == 0)
                {
                    float invOwn = 1.0 / nc;
                    ownDiag = vec3(
                        max(0.0, t1.x * invOwn + muc.x * muc.x),
                        max(0.0, t1.w * invOwn + muc.y * muc.y),
                        max(0.0, t2.y * invOwn + muc.z * muc.z));
                    hasOwnStats = true;
                }
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

        vec4 wOut = vec4(0.0);
        vec4 diagOut = vec4(0.0);
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
            float cVec[3];
            cVec[0] = 0.0; cVec[1] = 0.0; cVec[2] = 0.0;
            for (int k = 0; k < K; ++k)
            {
                bool exactCopy = td_is_exact_copy(k);
                float cA2 = (u_a2_valid > 0.5)
                    ? ((u_a2_sigma_sent + (exactCopy ? 0.0 : u_a2_sigma_zero)) * cellStepScaleSq * td_a2_band_peak_sq(b))
                    : 0.0;
                cA2 = min(cA2, C[k][k]);
                float cA1 = cA2;
                int partner = td_find_independent_a1_partner(K, k);
                if (partner >= 0)
                {
                    cA1 = td_est_c_a1(C[k][partner], C[k][k], C[partner][partner], chRetGVar, bGain, psdScale, 0.0);
                }
                else
                {
                    if (estMode != 1)
                        cellSingleRefA2 = true;
                    for (int j = 0; j < K; ++j)
                    {
                        if (j == k || td_is_exact_copy(j)) continue;
                        float cCap = td_est_c_a1(C[k][j], C[k][k], C[j][j], chRetGVar, bGain, psdScale, 0.0);
                        cA1 = min(cA1, cCap);
                    }
                    if (u_has_clean_split < 0.5 && chRetGVar > TD_NUM_FLOOR)
                    {
                        float excess = max(0.0, C[k][k] - 2.0 * chRetGVar * bGain * psdScale);
                        cA1 = min(cA1, excess);
                    }
                }
                cVec[k] = exactCopy ? 0.0 : td_select_blind_c(estMode, cA1, cA2);
            }

            float solvedW[3];
            int status = td_solve_convex(K, C, cVec, solvedW);
            if (status == 2) cellNonFinite = true;
            else if (status == 1) cellSingular = true;
            else
            {
                bool bandVeto = focusChange || (isDefocused && (b >= 1 || !bokehAllowL)) ||
                                (u_coarse_only > 0.5 && b >= 1);
                float focusGate = bandVeto ? 0.0 : 1.0;
                float wFinal[3];
                wFinal[0] = 0.0; wFinal[1] = 0.0; wFinal[2] = 0.0;
                for (int k = 0; k < K; ++k)
                {
                    vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).xy;
                    float mGate = (b == 0) ? mg.x : mg.y;
                    wFinal[k] = solvedW[k] * mGate * focusGate;
                }
                wOut = vec4(wFinal[0], wFinal[1], wFinal[2], 0.0);
            }
        }
        bandW[b] = wOut;
        if (chIdx == 0) imageStore(cell_diag_cb, ivec2(b * TD_CW + cell.x, cell.y), diagOut);
        else            imageStore(cell_diag_cr, ivec2(b * TD_CW + cell.x, cell.y), diagOut);
    }

    if (!isFace && estMode != 1 && chRetGVar > TD_NUM_FLOOR)
    {
        float alphaLeak = 0.0;
        if (u_has_clean_split < 0.5) alphaLeak = 1.0;
        else if (chOwnGVar > chRetGVar)
        {
            float sL = sqrt(chRetGVar);
            float sO = sqrt(chOwnGVar);
            float sT = sO + sL;
            alphaLeak = (sT > TD_NUM_FLOOR) ? (sL / sT) : 0.0;
        }
        if (alphaLeak > TD_NUM_FLOOR)
        {
            float remBudget = (1.0 - sqrt(TD_MIN_VAR_RETENTION)) / sqrt(2.0);
            for (int b = 0; b < 3; ++b)
            {
                float bSum = bandW[b].x + bandW[b].y + bandW[b].z;
                float lossPerW = alphaLeak * td_band_origin(b);
                if (lossPerW > TD_NUM_FLOOR && bSum > TD_NUM_FLOOR)
                {
                    float maxW = remBudget / lossPerW;
                    if (bSum >= maxW)
                    {
                        float sc = (maxW > 0.0) ? (maxW / bSum) : 0.0;
                        bandW[b].xyz *= sc;
                        bSum = (maxW > 0.0) ? maxW : 0.0;
                        remBudget = 0.0;
                    }
                    else
                    {
                        remBudget = max(0.0, remBudget - lossPerW * bSum);
                    }
                }
            }
        }
    }

    for (int b = 0; b < 3; ++b)
    {
        bool bandVeto = isFace || focusChange || (isDefocused && (b >= 1 || !bokehAllowL)) ||
                        (u_coarse_only > 0.5 && b >= 1);
        float bSum = bandW[b].x + bandW[b].y + bandW[b].z;
        int activeMask = 0;
        if (!bandVeto && bSum > 0.0)
        {
            for (int k = 0; k < K; ++k)
            {
                vec2 mg = texelFetch(motion_gate, ivec2(k * TD_CW + cell.x, cell.y), 0).xy;
                float mGate = (b == 0) ? mg.x : mg.y;
                if (!td_is_exact_copy(k) && mGate > 0.0)
                    activeMask |= (1 << k);
            }
        }
        bandW[b].w = float(activeMask);
        if (chIdx == 0) imageStore(cell_weights_cb, ivec2(b * TD_CW + cell.x, cell.y), bandW[b]);
        else            imageStore(cell_weights_cr, ivec2(b * TD_CW + cell.x, cell.y), bandW[b]);
    }
}

imageStore(cell_diag_flags, cell, vec4(
    (focusChange || isDefocused) ? 1.0 : 0.0,
    cellNonFinite ? 1.0 : 0.0,
    cellSingular ? 1.0 : 0.0,
    cellSingleRefA2 ? 1.0 : 0.0));
)GLSL";

		// ---- PASS C3: Coded-Grid Chroma Blend + 3-Sigma Per-Corner Huber + Parseval Pixel Bound ----
		constexpr const char* kPassChromaBlendBound = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
if (p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    imageStore(coded_delta_raw, p, vec4(0.0));
    return;
}

int K = int(round(u_k));
bool enableHuber = (u_enable_per_corner_huber > 0.5);

float lx = td_coded_to_luma_x(float(p.x));
float ly = td_coded_to_luma_y(float(p.y));
int cx = clamp(int(floor(lx)) / TD_CELL_PX, 0, TD_CW - 1);
int cy = clamp(int(floor(ly)) / TD_CELL_PX, 0, TD_CH - 1);

if (u_has_face_mask > 0.5 && texelFetch(face_mask, ivec2(cx, cy), 0).x > 0.5)
{
    imageStore(coded_delta_raw, p, vec4(0.0));
    return;
}

float rawScale = (u_cell_prior_has_step_scale > 0.5) ? texelFetch(cell_prior, ivec2(cx, cy), 0).z : 1.0;
float cellStepScale = (td_is_bad(rawScale) || !(rawScale > 0.0)) ? 0.0 : clamp(rawScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX);
float cellStepScaleSq = cellStepScale * cellStepScale;

float gx = (lx + 0.5) / float(TD_CELL_PX) - 0.5;
float gy = (ly + 0.5) / float(TD_CELL_PX) - 0.5;
int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
int x1 = min(TD_CW - 1, x0 + 1);
int y1 = min(TD_CH - 1, y0 + 1);
float tx = clamp(gx - float(x0), 0.0, 1.0);
float ty = clamp(gy - float(y0), 0.0, 1.0);

uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
int refFl[3];
for (int k = 0; k < K; ++k)
    refFl[k] = td_unpack_ref_flags(packedFl, k);

vec2 totalDelta = vec2(0.0);
for (int b = 0; b < 3; ++b)
{
    vec2 curB = (b == 0) ? texelFetch(cur_l, p, 0).xy
                         : ((b == 1) ? texelFetch(cur_mh, p, 0).xy : texelFetch(cur_mh, p, 0).zw);
    vec2 refB[3];
    for (int k = 0; k < K; ++k)
    {
        if (b == 0)
            refB[k] = (k == 0) ? texelFetch(ref_l_0, p, 0).xy : ((k == 1) ? texelFetch(ref_l_1, p, 0).xy : texelFetch(ref_l_2, p, 0).xy);
        else
        {
            vec4 rmh = (k == 0) ? texelFetch(ref_mh_0, p, 0) : ((k == 1) ? texelFetch(ref_mh_1, p, 0) : texelFetch(ref_mh_2, p, 0));
            refB[k] = (b == 1) ? rmh.xy : rmh.zw;
        }
    }

    float bandVar = (u_a2_valid > 0.5) ? (u_a2_total_var * cellStepScaleSq * td_a2_band_peak_sq(b)) : 0.0;
    float bound = TD_PIXEL_BOUND_SIGMA * sqrt(max(0.0, bandVar));

    for (int chIdx = 0; chIdx < 2; ++chIdx)
    {
        vec4 ownW = (chIdx == 0) ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + cx, cy), 0)
                                 : texelFetch(cell_weights_cr, ivec2(b * TD_CW + cx, cy), 0);
        vec4 w00 = (chIdx == 0) ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x0, y0), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x0, y0), 0);
        vec4 w10 = (chIdx == 0) ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x1, y0), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x1, y0), 0);
        vec4 w01 = (chIdx == 0) ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x0, y1), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x0, y1), 0);
        vec4 w11 = (chIdx == 0) ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x1, y1), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x1, y1), 0);
        vec4 d00 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x0, y0), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x0, y0), 0);
        vec4 d10 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x1, y0), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x1, y0), 0);
        vec4 d01 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x0, y1), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x0, y1), 0);
        vec4 d11 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x1, y1), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x1, y1), 0);

        float curV = (chIdx == 0) ? curB.x : curB.y;
        int ownActiveMask = int(round(ownW.w));
        float bandDelta = 0.0;
        for (int k = 0; k < K; ++k)
        {
            if ((refFl[k] & 1) == 0) continue;
            if ((ownActiveMask & (1 << k)) == 0) continue;
            float diff = curV - ((chIdx == 0) ? refB[k].x : refB[k].y);
            if (td_is_bad(diff)) continue;

            float t00 = td_vec_comp(w00, k);
            float t10 = td_vec_comp(w10, k);
            float t01 = td_vec_comp(w01, k);
            float t11 = td_vec_comp(w11, k);
            if (td_is_bad(t00) || td_is_bad(t10) || td_is_bad(t01) || td_is_bad(t11)) continue;
            if (enableHuber)
            {
                float diffSq = diff * diff;
                t00 *= td_huber_taper(diffSq, td_vec_comp(d00, k));
                t10 *= td_huber_taper(diffSq, td_vec_comp(d10, k));
                t01 *= td_huber_taper(diffSq, td_vec_comp(d01, k));
                t11 *= td_huber_taper(diffSq, td_vec_comp(d11, k));
            }
            float top = t00 + (t10 - t00) * tx;
            float bot = t01 + (t11 - t01) * tx;
            float wEff = top + (bot - top) * ty;
            if (wEff > 0.0)
                bandDelta -= wEff * diff;
        }
        if (abs(bandDelta) > 0.0)
            bandDelta = clamp(bandDelta, -bound, bound);
        if (chIdx == 0) totalDelta.x += bandDelta;
        else            totalDelta.y += bandDelta;
    }
}

vec2 safeD = vec2(td_is_bad(totalDelta.x) ? 0.0 : totalDelta.x,
                  td_is_bad(totalDelta.y) ? 0.0 : totalDelta.y);
imageStore(coded_delta_raw, p, vec4(safeD, 0.0, 0.0));
)GLSL";

		// ---- PASS C4: Fused Layer-4 TdGuardPort Grain-Preserving Signed-Delta Output (Cb & Cr in ONE pass!) ----
		// Dispatch: TD_CW x TD_CH workgroups of 16x16 (1 WG per luma cell).
		constexpr const char* kPassChromaGuardHeader = R"GLSL(
shared vec2 td_cg_r[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_d[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_g[(16 + 2 * TD_RAD_FINE) * (16 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_rh[(16 + 2 * TD_RAD_FINE) * 16];
shared vec2 td_cg_dh[(16 + 2 * TD_RAD_FINE) * 16];
shared vec2 td_cg_gh[(16 + 2 * TD_RAD_FINE) * 16];
shared vec2 td_cg_cov2[256];
shared vec4 td_cg_out;
)GLSL";

		constexpr const char* kPassChromaGuardPart1 = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool validRect = (cax1 > cax0) && (cay1 > cay0);
int xMin = cax0;
int xMax = max(cax0, cax1 - 1);
int yMin = cay0;
int yMax = max(cay0, cay1 - 1);
bool hasCleanSplit = (u_has_clean_split > 0.5);
bool sameCleanRec = (u_same_clean_rec > 0.5);
bool bokehAllowL = (u_bokeh_mode > 0.5);

vec4 priorTexel = texelFetch(cell_prior, cell, 0);
int expFocusBits = int(round(priorTexel.y));
bool allBandVeto = ((expFocusBits & 1) != 0) || (((expFocusBits & 2) != 0) && !bokehAllowL);
bool cellIsFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);
bool cellVetoed = cellIsFace || allBandVeto;

int padX = (TD_CSUB_X > 1) ? (int(ceil(abs(u_cshift_x) / float(TD_CSUB_X))) + 1) : 0;
int padY = (TD_CSUB_Y > 1) ? (int(ceil(abs(u_cshift_y) / float(TD_CSUB_Y))) + 1) : 0;
int cellStartX = max(0, (cell.x * TD_CELL_PX) / TD_CSUB_X - padX);
int cellStartY = max(0, (cell.y * TD_CELL_PX) / TD_CSUB_Y - padY);
int cellEndX = min(TD_CH_W, ((cell.x + 1) * TD_CELL_PX + TD_CSUB_X - 1) / TD_CSUB_X + padX);
int cellEndY = min(TD_CH_H, ((cell.y + 1) * TD_CELL_PX + TD_CSUB_Y - 1) / TD_CSUB_Y + padY);
int cellW = max(0, cellEndX - cellStartX);
int cellH = max(0, cellEndY - cellStartY);
int subX = (cellW + 15) / 16;
int subY = (cellH + 15) / 16;
bool canCacheSingle = (subX <= 1 && subY <= 1);

int tileDim = 16 + 2 * TD_RAD_FINE;
int tileN = tileDim * tileDim;
int hN = tileDim * 16;

vec2 sumOgE = vec2(0.0);
vec2 sumOgFineE = vec2(0.0);
vec2 sumInFineE = vec2(0.0);
vec2 sumOutFineE = vec2(0.0);
vec2 sumInCovOwn = vec2(0.0);
vec2 sumOutCovOwn = vec2(0.0);
float sumCnt = 0.0;

vec2 regRawDelta = vec2(0.0);
vec2 regFineDiff = vec2(0.0);
bool regWrite = false;
ivec2 regP = ivec2(0);

for (int sy = 0; sy < subY; ++sy)
{
    for (int sx = 0; sx < subX; ++sx)
    {
        int baseX = cellStartX + sx * 16;
        int baseY = cellStartY + sy * 16;
        ivec2 p = ivec2(baseX + lx, baseY + ly);

        for (int idx = tid; idx < tileN; idx += 256)
        {
            int ty = idx / tileDim;
            int tx = idx % tileDim;
            int gx = baseX - TD_RAD_FINE + tx;
            int gy = baseY - TD_RAD_FINE + ty;
            int clx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_CH_W - 1);
            int cly = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_CH_H - 1);
            ivec2 sp = ivec2(clx, cly);
            vec2 rv = texelFetch(rec_uv, sp, 0).xy;
            vec2 cv = sameCleanRec ? rv : texelFetch(cur_uv, sp, 0).xy;
            vec2 dv = texelFetch(coded_delta_raw, sp, 0).xy;
            if (td_is_bad(rv.x)) rv.x = 0.0;
            if (td_is_bad(rv.y)) rv.y = 0.0;
            if (td_is_bad(cv.x)) cv.x = 0.0;
            if (td_is_bad(cv.y)) cv.y = 0.0;
            if (td_is_bad(dv.x)) dv.x = 0.0;
            if (td_is_bad(dv.y)) dv.y = 0.0;
            vec2 og = rv - cv;
            td_cg_r[idx] = rv;
            td_cg_d[idx] = (cv + dv) + og;
            td_cg_g[idx] = og;
        }
        memoryBarrierShared();
        barrier();

        for (int idx = tid; idx < hN; idx += 256)
        {
            int ty = idx / 16;
            int tx = idx % 16;
            int rowBase = ty * tileDim + (tx + TD_RAD_FINE);
            vec2 ar = vec2(0.0);
            vec2 ad = vec2(0.0);
            vec2 ag = vec2(0.0);
            for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
            {
                float w = TD_TAP_FINE[k + TD_RAD_FINE];
                ar += w * td_cg_r[rowBase + k];
                ad += w * td_cg_d[rowBase + k];
                ag += w * td_cg_g[rowBase + k];
            }
            td_cg_rh[idx] = ar;
            td_cg_dh[idx] = ad;
            td_cg_gh[idx] = ag;
        }
        memoryBarrierShared();
        barrier();

        int ox = sx * 16 + lx;
        int oy = sy * 16 + ly;
        bool inCell = (ox < cellW) && (oy < cellH) && (p.x < TD_CH_W) && (p.y < TD_CH_H) &&
                      (td_coded_to_luma_cell(p) == cell);
        bool inAct = inCell && validRect && (p.x >= cax0) && (p.x < cax1) && (p.y >= cay0) && (p.y < cay1);

        if (inCell)
        {
            if (!inAct || cellVetoed)
            {
                if (canCacheSingle) { regRawDelta = vec2(0.0); regFineDiff = vec2(0.0); regWrite = true; regP = p; }
                else imageStore(out_coded_delta, p, vec4(0.0));
            }
            else
            {
                int centerIdx = (ly + TD_RAD_FINE) * tileDim + (lx + TD_RAD_FINE);
                vec2 rv = td_cg_r[centerIdx];
                vec2 candOut = td_cg_d[centerIdx];
                vec2 og = td_cg_g[centerIdx];
                vec2 dRaw = texelFetch(coded_delta_raw, p, 0).xy;
                if (td_is_bad(dRaw.x)) dRaw.x = 0.0;
                if (td_is_bad(dRaw.y)) dRaw.y = 0.0;

                vec2 gr = vec2(0.0);
                vec2 gd = vec2(0.0);
                vec2 gg = vec2(0.0);
                int cyBase = ly + TD_RAD_FINE;
                for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                {
                    float w = TD_TAP_FINE[k + TD_RAD_FINE];
                    int hIdx = (cyBase + k) * 16 + lx;
                    gr += w * td_cg_rh[hIdx];
                    gd += w * td_cg_dh[hIdx];
                    gg += w * td_cg_gh[hIdx];
                }
                vec2 inFullH = rv - gr;
                vec2 outFullH = candOut - gd;
                vec2 ogFineH = og - gg;

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

                if (canCacheSingle)
                {
                    regRawDelta = dRaw;
                    regFineDiff = inFullH - outFullH;
                    regWrite = true;
                    regP = p;
                }
            }
        }
        memoryBarrierShared();
        barrier();
    }
}
)GLSL";

		constexpr const char* kPassChromaGuardPart2 = R"GLSL(
td_cg_r[tid]    = sumOgE;
td_cg_d[tid]    = sumOgFineE;
td_cg_g[tid]    = sumInFineE;
td_cg_rh[tid]   = sumOutFineE;
td_cg_dh[tid]   = vec2(sumCnt, 0.0);
td_cg_gh[tid]   = sumInCovOwn;
td_cg_cov2[tid] = sumOutCovOwn;
memoryBarrierShared();
barrier();

for (int stride = 128; stride > 0; stride >>= 1)
{
    if (tid < stride)
    {
        td_cg_r[tid]    += td_cg_r[tid + stride];
        td_cg_d[tid]    += td_cg_d[tid + stride];
        td_cg_g[tid]    += td_cg_g[tid + stride];
        td_cg_rh[tid]   += td_cg_rh[tid + stride];
        td_cg_dh[tid]   += td_cg_dh[tid + stride];
        td_cg_gh[tid]   += td_cg_gh[tid + stride];
        td_cg_cov2[tid] += td_cg_cov2[tid + stride];
    }
    memoryBarrierShared();
    barrier();
}

if (tid == 0)
{
    float totCnt = td_cg_dh[0].x;
    vec2 addBack = vec2(0.0);
    ivec2 gFlags = ivec2(0);

    if (!cellVetoed && totCnt > 0.5)
    {
        float invC = 1.0 / totCnt;
        vec2 retG = vec2(
            (!td_is_bad(priorTexel.x) && priorTexel.x > 0.0) ? priorTexel.x : 0.0,
            (u_cell_prior_has_cr_grain > 0.5)
                ? ((!td_is_bad(priorTexel.w) && priorTexel.w > 0.0) ? priorTexel.w : 0.0)
                : ((!td_is_bad(priorTexel.x) && priorTexel.x > 0.0) ? priorTexel.x : 0.0));

        for (int c = 0; c < 2; ++c)
        {
            float ogMean = (c == 0 ? td_cg_r[0].x : td_cg_r[0].y) * invC;
            float ogFineMean = (c == 0 ? td_cg_d[0].x : td_cg_d[0].y) * invC;
            float inFineMean = (c == 0 ? td_cg_g[0].x : td_cg_g[0].y) * invC;
            float outFineMean = (c == 0 ? td_cg_rh[0].x : td_cg_rh[0].y) * invC;
            float inCov = (c == 0 ? td_cg_gh[0].x : td_cg_gh[0].y);
            float outCov = (c == 0 ? td_cg_cov2[0].x : td_cg_cov2[0].y);
            float rGv = (c == 0) ? retG.x : retG.y;

            if (!hasCleanSplit || ogMean > TD_NUM_FLOOR || rGv > TD_NUM_FLOOR)
            {
                float leakedFineE = rGv * TD_GAIN_FINE;
                float targetFineE = inFineMean;
                if (hasCleanSplit)
                {
                    float tStd = sqrt(max(0.0, ogFineMean)) + sqrt(max(0.0, leakedFineE));
                    targetFineE = min(inFineMean, tStd * tStd);
                }
                float effOut = outFineMean;
                if (hasCleanSplit && ogFineMean > TD_NUM_FLOOR && rGv > TD_NUM_FLOOR && inCov > TD_NUM_FLOOR)
                {
                    float covRatio = (outCov > 0.0) ? (outCov / inCov) : 0.0;
                    float covImplied = targetFineE * covRatio * covRatio;
                    if (covImplied < effOut) effOut = covImplied;
                }
                int fl = 2;
                float ab = 0.0;
                if (td_is_bad(targetFineE) || td_is_bad(effOut))
                {
                    fl |= 1; ab = 1.0;
                }
                else if (targetFineE > TD_NUM_FLOOR)
                {
                    float ratio = effOut / targetFineE;
                    if (ratio < TD_MIN_VAR_RETENTION)
                    {
                        fl |= 1;
                        float tStd = sqrt(TD_MIN_VAR_RETENTION) * sqrt(targetFineE);
                        float cStd = (effOut > 0.0) ? sqrt(effOut) : 0.0;
                        float den = sqrt(targetFineE) - cStd;
                        ab = (den > TD_NUM_FLOOR) ? clamp((tStd - cStd) / den, 0.0, 1.0) : 1.0;
                    }
                }
                if (c == 0) { addBack.x = ab; gFlags.x = fl; }
                else        { addBack.y = ab; gFlags.y = fl; }
            }
        }
    }

    vec4 packed = vec4(addBack.x, float(gFlags.x), addBack.y, float(gFlags.y));
    imageStore(guard_cell, cell, packed);
    td_cg_out = packed;
}
memoryBarrierShared();
barrier();

vec2 cellAddBack = vec2(td_cg_out.x, td_cg_out.z);
ivec2 cellGFlags = ivec2(int(round(td_cg_out.y)), int(round(td_cg_out.w)));

if (canCacheSingle)
{
    if (regWrite)
    {
        vec2 outD = regRawDelta;
        if (!cellVetoed)
        {
            if ((cellGFlags.x & 1) != 0 && cellAddBack.x > 0.0) outD.x += cellAddBack.x * regFineDiff.x;
            if ((cellGFlags.y & 1) != 0 && cellAddBack.y > 0.0) outD.y += cellAddBack.y * regFineDiff.y;
            if (u_fade_keep < 1.0) outD *= u_fade_keep;
        }
        else
        {
            outD = vec2(0.0);
        }
        imageStore(out_coded_delta, regP, vec4(outD, 0.0, 0.0));
    }
}
else
{
    for (int sy = 0; sy < subY; ++sy)
    {
        for (int sx = 0; sx < subX; ++sx)
        {
            int baseX = cellStartX + sx * 16;
            int baseY = cellStartY + sy * 16;
            ivec2 p = ivec2(baseX + lx, baseY + ly);
            bool needAddBack = !cellVetoed && (((cellGFlags.x & 1) != 0 && cellAddBack.x > 0.0) ||
                                               ((cellGFlags.y & 1) != 0 && cellAddBack.y > 0.0));
            for (int idx = tid; idx < tileN; idx += 256)
            {
                vec2 rVal = vec2(0.0);
                vec2 cOutVal = vec2(0.0);
                if (needAddBack)
                {
                    int ty = idx / tileDim;
                    int tx = idx % tileDim;
                    int clx = validRect ? clamp(baseX - TD_RAD_FINE + tx, xMin, xMax) : clamp(baseX - TD_RAD_FINE + tx, 0, TD_CH_W - 1);
                    int cly = validRect ? clamp(baseY - TD_RAD_FINE + ty, yMin, yMax) : clamp(baseY - TD_RAD_FINE + ty, 0, TD_CH_H - 1);
                    ivec2 sp = ivec2(clx, cly);
                    rVal = texelFetch(rec_uv, sp, 0).xy;
                    vec2 cv = sameCleanRec ? rVal : texelFetch(cur_uv, sp, 0).xy;
                    vec2 dv = texelFetch(coded_delta_raw, sp, 0).xy;
                    if (td_is_bad(rVal.x)) rVal.x = 0.0;
                    if (td_is_bad(rVal.y)) rVal.y = 0.0;
                    if (td_is_bad(cv.x)) cv.x = 0.0;
                    if (td_is_bad(cv.y)) cv.y = 0.0;
                    if (td_is_bad(dv.x)) dv.x = 0.0;
                    if (td_is_bad(dv.y)) dv.y = 0.0;
                    cOutVal = (cv + dv) + (rVal - cv);
                }
                td_cg_r[idx] = rVal;
                td_cg_d[idx] = cOutVal;
            }
            memoryBarrierShared();
            barrier();

            for (int idx = tid; idx < hN; idx += 256)
            {
                vec2 ar = vec2(0.0);
                vec2 ad = vec2(0.0);
                if (needAddBack)
                {
                    int ty = idx / 16;
                    int tx = idx % 16;
                    int rowBase = ty * tileDim + (tx + TD_RAD_FINE);
                    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                    {
                        float w = TD_TAP_FINE[k + TD_RAD_FINE];
                        ar += w * td_cg_r[rowBase + k];
                        ad += w * td_cg_d[rowBase + k];
                    }
                }
                td_cg_rh[idx] = ar;
                td_cg_dh[idx] = ad;
            }
            memoryBarrierShared();
            barrier();

            int ox = sx * 16 + lx;
            int oy = sy * 16 + ly;
            bool inCell = (ox < cellW) && (oy < cellH) && (p.x < TD_CH_W) && (p.y < TD_CH_H) &&
                          (td_coded_to_luma_cell(p) == cell);
            bool inAct = inCell && validRect && (p.x >= cax0) && (p.x < cax1) && (p.y >= cay0) && (p.y < cay1);
            if (inAct && !cellVetoed)
            {
                vec2 outD = texelFetch(coded_delta_raw, p, 0).xy;
                if (td_is_bad(outD.x)) outD.x = 0.0;
                if (td_is_bad(outD.y)) outD.y = 0.0;
                if (needAddBack)
                {
                    int centerIdx = (ly + TD_RAD_FINE) * tileDim + (lx + TD_RAD_FINE);
                    vec2 rv = td_cg_r[centerIdx];
                    vec2 cOut = td_cg_d[centerIdx];
                    vec2 gr = vec2(0.0);
                    vec2 gd = vec2(0.0);
                    int cyBase = ly + TD_RAD_FINE;
                    for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                    {
                        float w = TD_TAP_FINE[k + TD_RAD_FINE];
                        int hIdx = (cyBase + k) * 16 + lx;
                        gr += w * td_cg_rh[hIdx];
                        gd += w * td_cg_dh[hIdx];
                    }
                    vec2 fineDiff = (rv - gr) - (cOut - gd);
                    if ((cellGFlags.x & 1) != 0 && cellAddBack.x > 0.0) outD.x += cellAddBack.x * fineDiff.x;
                    if ((cellGFlags.y & 1) != 0 && cellAddBack.y > 0.0) outD.y += cellAddBack.y * fineDiff.y;
                }
                if (u_fade_keep < 1.0) outD *= u_fade_keep;
                imageStore(out_coded_delta, p, vec4(outD, 0.0, 0.0));
            }
            memoryBarrierShared();
            barrier();
        }
    }
}
)GLSL";

		inline std::string BuildPassChromaGuardBody()
		{
			return std::string(kPassChromaGuardPart1) + kPassChromaGuardPart2;
		}
	} // namespace ChromaShaders

	// ---- Zero-Copy GPU Chroma Fusion Input / Output ----
	struct ChromaGpuRefInput
	{
		pl_tex cleanUvTex = nullptr;             // Grain-free reference chroma: cW x cH RG32F (or dW x dH RG32F / RGBA32F RGB)
		const CellMotionField* lumaMotion = nullptr;
		bool isExactCopyOfCurrent = false;
		Homography3x3 cameraHomography;          // Optional P7 3x3 camera homography (sited coded->luma->H->coded on Camera cells)
	};
	using ChromaGpuReferenceInput = ChromaGpuRefInput;

	struct ChromaGpuInput
	{
		uint64_t seq = 0;
		int lumaW = 0;
		int lumaH = 0;
		DeliveredChromaFormat deliveredFormat = DeliveredChromaFormat::Yuv420;
		ChromaSubsampling codedSubsampling = ChromaSubsampling::Yuv420;
		ChromaGridStatus gridStatus = ChromaGridStatus::Identified;
		SitingSpec siting = SitingSpec::Left();
		YCbCrMatrixSpec matrix = YCbCrMatrixSpec::Bt709();
		ActiveRect activeRect;

		// Received chroma texture: dW x dH RG32F (YUV) or dW x dH RGBA32F (RGB).
		pl_tex receivedTex = nullptr;
		// Optional grain-free clean chroma texture (cW x cH or dW x dH RG32F; null => uses receivedTex).
		pl_tex cleanUvTex = nullptr;
		bool hasCleanSplit = false;

		ChromaGpuRefInput prevPeak;
		ChromaGpuRefInput nextPeak;

		// Optional pre-packed GPU motion, cell prior, and Pass-1 face mask (ZERO CPU upload when provided!).
		// cellPriorTex: cw x ch RG32F or RGBA32F (retainedGrainVarCb, focusBits[bit0=rackFocus,bit1=bokeh],
		//               optional .z = cellStepScale [default 1.0], optional .w = retainedGrainVarCr [default .x]).
		pl_tex motionVecTex = nullptr;
		pl_tex motionMetaTex = nullptr;
		pl_tex cellPriorTex = nullptr;
		pl_tex faceMaskTex = nullptr;

		// Optional CPU fallback vectors when corresponding GPU textures above are null.
		std::vector<double> retainedGrainVarCb;
		std::vector<double> retainedGrainVarCr;
		std::vector<double> cellStepScale;
		const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
		const std::vector<uint8_t>* explicitDefocusedCells = nullptr;
		const std::vector<uint8_t>* faceMask = nullptr;

		// Coding step prior (1:1 parity with TdV2ChromaEngine).
		bool hasStepPrior = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool isCutOrSpanInvalid = false;
		double fadeKeep = 1.0;                   // Upstream U7 gradual retention keep in [0, 1] (default 1.0 = bit-identical; <=0 or NaN closes delta to 0)
		PsdShape psdShape;                       // Optional P10g grain PSD shape multipliers (default 1.0 white-noise identity)
	};

	struct ChromaGpuOutput
	{
		// Signed coded-grid chroma delta output (cW x cH RG32F, storable — required when Mode != Off).
		pl_tex outCodedDeltaTex = nullptr;

		// Optional zero-copy delivered apply outputs (ZERO CPU roundtrip in live playback!):
		//   YUV: outDeliveredUvTex (dW x dH RGBA32F: .xy = outCb/Cr, .z = diagFlags)
		//   RGB: outDeliveredRgbTex (dW x dH RGBA32F: .xyz = unclipped RGB, .w = diagFlags)
		//   Optional outDeliveredDeltaTex (dW x dH RG32F)
		pl_tex outDeliveredUvTex = nullptr;
		pl_tex outDeliveredRgbTex = nullptr;
		pl_tex outDeliveredDeltaTex = nullptr;

		bool readbackCellDiagnostics = false;

		int cellsW = 0;
		int cellsH = 0;
		int codedW = 0;
		int codedH = 0;
		size_t activeRefCount = 0;
		int dispatchCount = 0;
		size_t guardViolationCells = 0;
		size_t focusVetoCells = 0;
		size_t faceMaskedCells = 0;
		size_t nonFiniteCells = 0;
		size_t a2InvalidCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
		bool coarseBandOnly = false;
		bool scratchShort = false;
		bool succeeded = false;
		std::string lastError;
	};

	struct ChromaGpuProcessResult
	{
		TdV2Plane outputCb;
		TdV2Plane outputCr;
		TdV2Plane outputR;
		TdV2Plane outputG;
		TdV2Plane outputB;
		TdV2Plane codedDeltaCb;
		TdV2Plane codedDeltaCr;
		TdV2Plane deliveredDeltaCb;
		TdV2Plane deliveredDeltaCr;

		int cellsW = 0;
		int cellsH = 0;
		int codedW = 0;
		int codedH = 0;
		size_t activeRefCount = 0;
		int dispatchCount = 0;
		size_t guardViolationCells = 0;
		size_t focusVetoCells = 0;
		size_t faceMaskedCells = 0;
		size_t nonFiniteCells = 0;
		size_t a2InvalidCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
		bool coarseBandOnly = false;
		bool scratchShort = false;
		bool succeeded = false;
		std::string lastError;
	};

	namespace ChromaGpuDetail
	{
		inline std::vector<VpSkeleton::ArenaRequest> BuildChromaArenaRequests(
			int lumaW, int lumaH, ChromaSubsampling codedSubsampling, int cellPx = kCanonicalBlockPx,
			bool includeReduceScratch = true)
		{
			std::vector<VpSkeleton::ArenaRequest> reqs;
			if (lumaW <= 0 || lumaH <= 0)
				return reqs;
			const int effCellPx = cellPx > 0 ? cellPx : kCanonicalBlockPx;
			const int cW = ChromaDetail::SubsampledDim(lumaW, SubsampleX(codedSubsampling));
			const int cH = ChromaDetail::SubsampledDim(lumaH, SubsampleY(codedSubsampling));
			const uint32_t ucw = static_cast<uint32_t>((lumaW + effCellPx - 1) / effCellPx);
			const uint32_t uch = static_cast<uint32_t>((lumaH + effCellPx - 1) / effCellPx);
			const uint32_t ucW = static_cast<uint32_t>(cW);
			const uint32_t ucH = static_cast<uint32_t>(cH);

			auto pushReq = [&](uint32_t rw, uint32_t rh, VpSkeleton::TexFormat fmt, uint8_t count)
			{
				VpSkeleton::ArenaRequest r;
				r.scope = kChromaArenaScope;
				r.owner = kChromaArenaOwner;
				r.tier = VpSkeleton::Tier::T1;
				r.shape = VpSkeleton::TexShape{ rw, rh, fmt };
				r.count = count;
				reqs.push_back(r);
			};

			// Coded-grid RG32F (6 native YUV, or 9 when reduction scratch is needed):
			//   core (6): curL, refL[0..2], warpedUv, codedDeltaRaw
			//   optional reduce (3): reducedRec, reducedClean, reducedRef (TD-AUDIT-L2: -47..190 MiB on native YUV!)
			pushReq(ucW, ucH, VpSkeleton::TexFormat::RG32F, includeReduceScratch ? 9u : 6u);
			// Coded-grid RGBA32F (6): curMH, refMH[0..2], bandTmpCb, bandTmpCr
			pushReq(ucW, ucH, VpSkeleton::TexFormat::RGBA32F, 6);
			// Coded-grid R16 (2): refFlags[0..1]
			pushReq(ucW, ucH, VpSkeleton::TexFormat::R16, 2);
			// Cell-grid 3*cw x ch RGBA32F (5): motionGate, cellWeightsCb, cellWeightsCr, cellDiagCb, cellDiagCr
			pushReq(3u * ucw, uch, VpSkeleton::TexFormat::RGBA32F, 5);
			// Cell-grid 3*cw x 3*ch RGBA32F (2): cellStatsCb, cellStatsCr
			pushReq(3u * ucw, 3u * uch, VpSkeleton::TexFormat::RGBA32F, 2);
			// Cell-grid cw x ch RG32F (1): warpDiag
			pushReq(ucw, uch, VpSkeleton::TexFormat::RG32F, 1);
			// Cell-grid cw x ch RGBA32F (3): cellOwnGrain (sumCb2, sumCr2, nAct, 0), cellDiagFlags, guardCell
			pushReq(ucw, uch, VpSkeleton::TexFormat::RGBA32F, 3);
			return reqs;
		}
	} // namespace ChromaGpuDetail

	// ---- Modular Fused RG32F GPU Coded-Grid Chroma Fusion Engine (SOTA #3) ----
	class TdV2ChromaGpuEngine
	{
	public:
		explicit TdV2ChromaGpuEngine(const Config& cfg = Config{})
			: cfg_(cfg)
		{
		}

		~TdV2ChromaGpuEngine() = default;
		TdV2ChromaGpuEngine(const TdV2ChromaGpuEngine&) = delete;
		TdV2ChromaGpuEngine& operator=(const TdV2ChromaGpuEngine&) = delete;

		void SetConfig(const Config& cfg) { cfg_ = cfg; }
		const Config& GetConfig() const { return cfg_; }

		void Release(pl_gpu gpu)
		{
			if (gpu != nullptr)
			{
				DestroyPrivateScratch(gpu);
				for (pl_tex* t : {
					&inRecUvTex_, &inRecRgbTex_, &inCleanUvTex_, &inRefUvTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_, &inFaceMaskTex_,
					&outCodedDeltaTex_, &outDelivUvTex_, &outDelivRgbTex_, &outDelivDeltaTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_ })
				{
					if (*t != nullptr)
						pl_tex_destroy(gpu, t);
				}
			}
			else
			{
				for (pl_tex* t : {
					&privCurL_, &privRefL_[0], &privRefL_[1], &privRefL_[2],
					&privWarpedUv_, &privCodedDeltaRaw_, &privReducedRec_, &privReducedClean_, &privReducedRef_,
					&privCurMH_, &privRefMH_[0], &privRefMH_[1], &privRefMH_[2], &privBandTmpCb_, &privBandTmpCr_,
					&privRefFlags_[0], &privRefFlags_[1],
					&privMotionGate_, &privWeightsCb_, &privWeightsCr_, &privDiagCb_, &privDiagCr_,
					&privStatsCb_, &privStatsCr_, &privOwnGrain_, &privWarpDiag_, &privDiagFlags_, &privGuardCell_,
					&inRecUvTex_, &inRecRgbTex_, &inCleanUvTex_, &inRefUvTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_, &inFaceMaskTex_,
					&outCodedDeltaTex_, &outDelivUvTex_, &outDelivRgbTex_, &outDelivDeltaTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_ })
				{
					*t = nullptr;
				}
			}
		}

		uint64_t AllocatedPersistentVramBytes() const
		{
			uint64_t total = 0;
			for (pl_tex t : {
				inRecUvTex_, inRecRgbTex_, inCleanUvTex_, inRefUvTex_,
				inMotionVecTex_, inMotionMetaTex_, inCellPriorTex_, inFaceMaskTex_,
				outCodedDeltaTex_, outDelivUvTex_, outDelivRgbTex_, outDelivDeltaTex_,
				outDiagFlagsTex_, outGuardCellTex_, outWarpDiagTex_ })
			{
				total += VpSkeleton::TexBytesOf(t);
			}
			return total;
		}

		const std::string& LastError() const { return lastError_; }
		uint64_t Failures() const { return failures_; }
		int LastDispatchCount() const { return lastDispatchCount_; }
		bool ScratchShort() const { return scratchShort_; }

		static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(
			pl_gpu gpu, int lumaW, int lumaH, ChromaSubsampling codedSubsampling, int cellPx = kCanonicalBlockPx)
		{
			if (!gpu || lumaW <= 0 || lumaH <= 0)
				return {};
			if (!VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F))
			{
				return {};
			}
			return ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, codedSubsampling, cellPx, true);
		}

		// TD-AUDIT-L2: Format-aware ArenaRequests omits the 3 coded-grid reduction scratch textures (-47..190 MiB at 4K!)
		// whenever deliveredFormat is native YUV matching codedSubsampling (zero Pass C0 reduction dispatches).
		static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(
			pl_gpu gpu, int lumaW, int lumaH, ChromaSubsampling codedSubsampling,
			DeliveredChromaFormat deliveredFormat, int cellPx = kCanonicalBlockPx)
		{
			if (!gpu || lumaW <= 0 || lumaH <= 0)
				return {};
			if (!VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F))
			{
				return {};
			}
			const bool needReduce = (deliveredFormat == DeliveredChromaFormat::Rgb444) ||
			                        (codedSubsampling != DeliveredSubsampling(deliveredFormat));
			return ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, codedSubsampling, cellPx, needReduce);
		}

		static uint64_t EstimatedScratchVramBytes(
			int lumaW, int lumaH, ChromaSubsampling codedSubsampling, int cellPx = kCanonicalBlockPx)
		{
			uint64_t total = 0;
			for (const VpSkeleton::ArenaRequest& r : ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, codedSubsampling, cellPx, true))
				total += VpSkeleton::ShapeBytes(r.shape) * static_cast<uint64_t>(r.count);
			return total;
		}

		static uint64_t EstimatedScratchVramBytesFor(
			int lumaW, int lumaH, ChromaSubsampling codedSubsampling,
			DeliveredChromaFormat deliveredFormat, int cellPx = kCanonicalBlockPx)
		{
			const bool needReduce = (deliveredFormat == DeliveredChromaFormat::Rgb444) ||
			                        (codedSubsampling != DeliveredSubsampling(deliveredFormat));
			uint64_t total = 0;
			for (const VpSkeleton::ArenaRequest& r : ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, codedSubsampling, cellPx, needReduce))
				total += VpSkeleton::ShapeBytes(r.shape) * static_cast<uint64_t>(r.count);
			return total;
		}

		// Zero-copy GPU-resident coded-grid chroma fusion (+ optional zero-copy delivered apply!).
		bool ProcessChromaGpu(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const ChromaGpuInput& in,
			ChromaGpuOutput& out,
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
			StandInRegistry* standIns = nullptr)
		{
			out.cellsW = 0;
			out.cellsH = 0;
			out.codedW = 0;
			out.codedH = 0;
			out.activeRefCount = 0;
			out.dispatchCount = 0;
			out.guardViolationCells = 0;
			out.focusVetoCells = 0;
			out.faceMaskedCells = 0;
			out.nonFiniteCells = 0;
			out.a2InvalidCells = 0;
			out.singularCells = 0;
			out.singleRefA2Cells = 0;
			out.motionStandInRefs = 0;
			out.borderBilinearPixels = 0;
			out.coarseBandOnly = false;
			out.scratchShort = false;
			out.succeeded = false;
			out.lastError.clear();
			lastDispatchCount_ = 0;
			scratchShort_ = false;
			lastError_.clear();

			const ChromaSubsampling delivSubInit = DeliveredSubsampling(in.deliveredFormat);
			const bool coarseOnly = (in.gridStatus == ChromaGridStatus::Unidentified);
			const ChromaSubsampling effCodedSub = coarseOnly ? delivSubInit : in.codedSubsampling;

			const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
			if (in.lumaW > 0 && in.lumaH > 0)
			{
				out.cellsW = (in.lumaW + cellPx - 1) / cellPx;
				out.cellsH = (in.lumaH + cellPx - 1) / cellPx;
				out.codedW = ChromaDetail::SubsampledDim(in.lumaW, SubsampleX(effCodedSub));
				out.codedH = ChromaDetail::SubsampledDim(in.lumaH, SubsampleY(effCodedSub));
			}

			// Mode::Off -> 0 GPU dispatches!
			if (!cfg_.IsEnabled())
			{
				out.succeeded = true;
				return true;
			}

			auto failVisible = [&](const std::string& msg, StandInKind kind) -> bool
			{
				lastError_ = msg;
				out.lastError = msg;
				out.succeeded = false;
				++failures_;
				if (standIns != nullptr)
					standIns->Engage(kind, msg.c_str());
				return false;
			};

			if (!gpu || !dispatch)
				return failVisible("TdV2ChromaGpuEngine::ProcessChromaGpu null pl_gpu or pl_dispatch", StandInKind::GpuDispatchFailed);
			if (in.lumaW <= 0 || in.lumaH <= 0 || in.receivedTex == nullptr || out.outCodedDeltaTex == nullptr)
				return failVisible("TdV2ChromaGpuEngine::ProcessChromaGpu invalid dimensions or null required texture", StandInKind::GpuDispatchFailed);
			if (!(PeakCodeForBits(cfg_.sourceBits) > 0.0) || !in.psdShape.IsValid())
				return failVisible(
					!(PeakCodeForBits(cfg_.sourceBits) > 0.0)
						? "TdV2ChromaGpuEngine::ProcessChromaGpu invalid sourceBits"
						: "TdV2ChromaGpuEngine::ProcessChromaGpu invalid/non-finite psdShape",
					StandInKind::NonFiniteClosure);
			if (!std::isfinite(in.siting.shiftX) || !std::isfinite(in.siting.shiftY))
				return failVisible("TdV2ChromaGpuEngine::ProcessChromaGpu non-finite siting", StandInKind::NonFiniteClosure);

			const bool isRgb = (in.deliveredFormat == DeliveredChromaFormat::Rgb444);
			const ChromaSubsampling delivSub = delivSubInit;
			const int dSubX = SubsampleX(delivSub);
			const int dSubY = SubsampleY(delivSub);
			const int cSubX = SubsampleX(effCodedSub);
			const int cSubY = SubsampleY(effCodedSub);
			// TD-AUDIT-H2: Never discard real chroma on Unidentified grids (coarse-only on delivered grid), and reject
			// physically impossible codedSubsampling finer than deliveredFormat (cSub < dSub).
			if (coarseOnly)
			{
				out.coarseBandOnly = true;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::ChromaGridDetector, "TdV2ChromaGpuEngine coarse band only (coded chroma grid not identified)");
			}
			else if (cSubX < dSubX || cSubY < dSubY)
			{
				return failVisible("TdV2ChromaGpuEngine codedSubsampling finer than deliveredFormat", StandInKind::GpuDispatchFailed);
			}
			const int dW = ChromaDetail::SubsampledDim(in.lumaW, dSubX);
			const int dH = ChromaDetail::SubsampledDim(in.lumaH, dSubY);
			const int cW = out.codedW;
			const int cH = out.codedH;
			const int cw = out.cellsW;
			const int ch = out.cellsH;
			const size_t numCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);

			if (isRgb && (!std::isfinite(in.matrix.kr) || !std::isfinite(in.matrix.kg) || !std::isfinite(in.matrix.kb) || !(in.matrix.kg > kEngineNumFloor)))
				return failVisible("TdV2ChromaGpuEngine::ProcessChromaGpu invalid YCbCr matrix", StandInKind::NonFiniteClosure);

			// Strict input/output aliasing check (Rule 4 / H6).
			{
				const pl_tex inputs[] = {
					in.receivedTex, in.cleanUvTex, in.prevPeak.cleanUvTex, in.nextPeak.cleanUvTex,
					in.motionVecTex, in.motionMetaTex, in.cellPriorTex, in.faceMaskTex
				};
				const pl_tex outputs[] = {
					out.outCodedDeltaTex, out.outDeliveredUvTex, out.outDeliveredRgbTex, out.outDeliveredDeltaTex
				};
				for (pl_tex inT : inputs)
				{
					if (!inT) continue;
					for (pl_tex outT : outputs)
					{
						if (outT && inT == outT)
							return failVisible("TdV2ChromaGpuEngine input/output texture aliasing forbidden", StandInKind::GpuInputOutputAlias);
					}
				}
				for (size_t i = 0; i < 4; ++i)
				{
					if (!outputs[i]) continue;
					for (size_t j = i + 1; j < 4; ++j)
					{
						if (outputs[j] && outputs[i] == outputs[j])
							return failVisible("TdV2ChromaGpuEngine output/output texture aliasing forbidden", StandInKind::GpuInputOutputAlias);
					}
				}
			}

			// Exact dimension checks on all caller textures.
			auto checkDim = [](pl_tex t, int ew, int eh) -> bool { return t == nullptr || (t->params.w == ew && t->params.h == eh); };
			if (!checkDim(in.receivedTex, dW, dH))
				return failVisible("TdV2ChromaGpuEngine receivedTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkDim(out.outCodedDeltaTex, cW, cH))
				return failVisible("TdV2ChromaGpuEngine outCodedDeltaTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (in.cleanUvTex != nullptr && !(checkDim(in.cleanUvTex, cW, cH) || checkDim(in.cleanUvTex, dW, dH)))
				return failVisible("TdV2ChromaGpuEngine cleanUvTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if ((in.motionVecTex != nullptr) != (in.motionMetaTex != nullptr))
				return failVisible("TdV2ChromaGpuEngine partial prepacked motion textures", StandInKind::GpuDispatchFailed);
			if (!checkDim(in.motionVecTex, 3 * cw, ch) || !checkDim(in.motionMetaTex, 3 * cw, ch))
				return failVisible("TdV2ChromaGpuEngine motion texture dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkDim(in.cellPriorTex, cw, ch))
				return failVisible("TdV2ChromaGpuEngine cellPriorTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkDim(in.faceMaskTex, cw, ch))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::FaceMaskSizeMismatch, "TdV2ChromaGpuEngine faceMaskTex dimension mismatch");
				return failVisible("TdV2ChromaGpuEngine faceMaskTex dimension mismatch", StandInKind::GpuDispatchFailed);
			}
			if (!checkDim(out.outDeliveredUvTex, dW, dH) || !checkDim(out.outDeliveredRgbTex, dW, dH) || !checkDim(out.outDeliveredDeltaTex, dW, dH))
				return failVisible("TdV2ChromaGpuEngine delivered output texture dimension mismatch", StandInKind::GpuDispatchFailed);

			const pl_fmt fmtR16 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16);
			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtR16 || !fmtR32 || !fmtRG32 || !fmtRGBA32)
				return failVisible("TdV2ChromaGpuEngine required FP32/R16 texture formats unsupported", StandInKind::GpuDispatchFailed);

			// Assemble references with strict H9 missing-motion skip (NEVER whole-frame Still blend!).
			struct ChromaRefSpec
			{
				pl_tex tex = nullptr;
				int motionSlot = 0;
				int refRole = 0;
				CellMotionField motion;
				bool isExactCopy = false;
				Homography3x3 cameraHomography;
			};
			const bool hasPrepackedMotion = (in.motionVecTex != nullptr && in.motionMetaTex != nullptr);
			std::vector<ChromaRefSpec> refSpecs;
			refSpecs.reserve(kMaxChromaGpuRefs);

			auto tryAddRef = [&](const ChromaGpuRefInput& rIn, int slot, int role)
			{
				if (rIn.cleanUvTex == nullptr)
					return;
				if (!(checkDim(rIn.cleanUvTex, cW, cH) || checkDim(rIn.cleanUvTex, dW, dH)))
				{
					++out.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::GpuDispatchFailed, "TdV2ChromaGpuEngine ref dimension mismatch; ref skipped");
					return;
				}
				ChromaRefSpec sp;
				sp.tex = rIn.cleanUvTex;
				sp.motionSlot = slot;
				sp.refRole = role;
				sp.isExactCopy = rIn.isExactCopyOfCurrent;
				sp.cameraHomography = rIn.cameraHomography;
				if (!hasPrepackedMotion)
				{
					if (rIn.lumaMotion == nullptr)
					{
						++out.motionStandInRefs;
						if (standIns != nullptr)
						{
							standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "TdV2ChromaGpuEngine null motion field; ref skipped");
							standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaGpuEngine null motion field; ref skipped");
						}
						return;
					}
					if (!rIn.lumaMotion->IsWellFormed(cw, ch))
					{
						++out.motionStandInRefs;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2ChromaGpuEngine malformed motion field; ref rejected");
						return;
					}
					sp.motion = *rIn.lumaMotion;
				}
				refSpecs.push_back(std::move(sp));
			};

			bool fadeNonFinite = false;
			const double effFadeKeep = ResolveFadeKeep(in.fadeKeep, fadeNonFinite);
			if (fadeNonFinite)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaGpuEngine non-finite fadeKeep; failing closed to 0.0");
				out.nonFiniteCells = numCells;
			}
			const bool effectiveCut = in.isCutOrSpanInvalid || (effFadeKeep <= 0.0);

			if (!effectiveCut)
			{
				tryAddRef(in.prevPeak, 0, 0);
				tryAddRef(in.nextPeak, 1, 1);
			}
			out.activeRefCount = refSpecs.size();

			const ActiveRect lumaActive = in.activeRect.Empty()
				? ActiveRect::Full(in.lumaW, in.lumaH)
				: in.activeRect.ClampedTo(in.lumaW, in.lumaH);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, in.lumaW, in.lumaH, cSubX, cSubY, cW, cH);
			const ActiveRect dActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, in.lumaW, in.lumaH, dSubX, dSubY, dW, dH);

			const float uBokehMode = (cfg_.bokehMode == BokehMode::LowBandCellOnly) ? 1.0f : 0.0f;
			const float uCAx0 = static_cast<float>(cActive.x0), uCAy0 = static_cast<float>(cActive.y0);
			const float uCAx1 = static_cast<float>(cActive.x1), uCAy1 = static_cast<float>(cActive.y1);
			const float uDAx0 = static_cast<float>(dActive.x0), uDAy0 = static_cast<float>(dActive.y0);
			const float uDAx1 = static_cast<float>(dActive.x1), uDAy1 = static_cast<float>(dActive.y1);
			const float uCShiftX = in.siting.shiftX, uCShiftY = in.siting.shiftY;
			const float uDShiftX = in.siting.shiftX, uDShiftY = in.siting.shiftY;
			const float uKr = static_cast<float>(in.matrix.kr);
			const float uKg = static_cast<float>(in.matrix.kg);
			const float uKb = static_cast<float>(in.matrix.kb);

			const bool wantZeroCopyApply = cfg_.ModifiesPixels() &&
				((!isRgb && out.outDeliveredUvTex != nullptr) || (isRgb && out.outDeliveredRgbTex != nullptr));

			// Shared helper for zero-copy delivered chroma apply (normal veto-clamped path + all-zero passthrough closure).
			auto runZeroCopyApply = [&](bool allZeroDelta, pl_tex vetoPriorTex, pl_tex vetoFaceTex,
			                            float vetoHasPrior, float vetoHasFace, int& dispCount) -> bool
			{
				if (wantZeroCopyApply)
				{
					pl_tex delivDeltaTarget = out.outDeliveredDeltaTex;
					if (!delivDeltaTarget)
					{
						if (!Detail::EnsureTex(gpu, outDelivDeltaTex_, dW, dH, fmtRG32, true, true, false, false))
							return failVisible("TdV2ChromaGpuEngine outDelivDeltaTex alloc failed", StandInKind::GpuDispatchFailed);
						delivDeltaTarget = outDelivDeltaTex_;
					}
					const bool identityGrid = (effCodedSub == delivSub && cW == dW && cH == dH);
					const float uAllZero = allZeroDelta ? 1.0f : 0.0f;
					const float uIdent = identityGrid ? 1.0f : 0.0f;
					const float uDShX = dSubX > 1 ? in.siting.shiftX : 0.0f;
					const float uDShY = dSubY > 1 ? in.siting.shiftY : 0.0f;
					const float uCShX = cSubX > 1 ? in.siting.shiftX : 0.0f;
					const float uCShY = cSubY > 1 ? in.siting.shiftY : 0.0f;
					const bool withVeto = (!allZeroDelta && vetoPriorTex != nullptr && vetoFaceTex != nullptr);

					std::string aHdr;
					aHdr += VpvGpu::DefineInt("TD_DW", dW);
					aHdr += VpvGpu::DefineInt("TD_DH", dH);
					aHdr += VpvGpu::DefineInt("TD_DSUB_X", dSubX);
					aHdr += VpvGpu::DefineInt("TD_DSUB_Y", dSubY);
					aHdr += VpvGpu::DefineInt("TD_CSUB_X", cSubX);
					aHdr += VpvGpu::DefineInt("TD_CSUB_Y", cSubY);
					aHdr += VpvGpu::Define("TD_NUM_FLOOR", kEngineNumFloor);
					if (withVeto)
					{
						aHdr += VpvGpu::DefineInt("TD_CHROMA_APPLY_HAS_VETO", 1);
						aHdr += VpvGpu::DefineInt("TD_CW", cw);
						aHdr += VpvGpu::DefineInt("TD_CH", ch);
						aHdr += VpvGpu::DefineInt("TD_CELL_PX", cellPx);
					}
					aHdr += Shaders::kCoreHelpers;
					aHdr += Shaders::kPassChromaDeltaHeader;

					const int dGx = Detail::GroupsFor(dW, 16);
					const int dGy = Detail::GroupsFor(dH, 16);
					if (!isRgb)
					{
						pl_shader_desc da[6] = {
							VpvGpu::Sampled("deliv_uv", in.receivedTex),
							VpvGpu::Sampled("coded_delta", out.outCodedDeltaTex),
							VpvGpu::Storage("out_uv", out.outDeliveredUvTex),
							VpvGpu::Storage("out_delta", delivDeltaTarget),
							VpvGpu::Sampled("cell_prior", withVeto ? vetoPriorTex : in.receivedTex),
							VpvGpu::Sampled("face_mask", withVeto ? vetoFaceTex : in.receivedTex)
						};
						pl_shader_var va[17] = {
							VpvGpu::Var("u_dshift_x", &uDShX), VpvGpu::Var("u_dshift_y", &uDShY),
							VpvGpu::Var("u_cshift_x", &uCShX), VpvGpu::Var("u_cshift_y", &uCShY),
							VpvGpu::Var("u_all_zero_delta", &uAllZero), VpvGpu::Var("u_identity_grid", &uIdent),
							VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
							VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
							VpvGpu::Var("u_dax0", &uDAx0), VpvGpu::Var("u_day0", &uDAy0),
							VpvGpu::Var("u_dax1", &uDAx1), VpvGpu::Var("u_day1", &uDAy1),
							VpvGpu::Var("u_has_face_mask", &vetoHasFace),
							VpvGpu::Var("u_has_cell_prior", &vetoHasPrior),
							VpvGpu::Var("u_bokeh_mode", &uBokehMode)
						};
						if (!VpvGpu::Pass(dispatch, "td_v2_chroma_apply_yuv", aHdr, Shaders::kPassChromaApplyYuv,
						                  da, withVeto ? 6 : 4, va, withVeto ? 17 : 14, dGx, dGy, 0, 16, 16))
							return failVisible("td_v2_chroma_apply_yuv dispatch failed", StandInKind::GpuDispatchFailed);
					}
					else
					{
						pl_shader_desc da[6] = {
							VpvGpu::Sampled("deliv_rgb", in.receivedTex),
							VpvGpu::Sampled("coded_delta", out.outCodedDeltaTex),
							VpvGpu::Storage("out_rgb", out.outDeliveredRgbTex),
							VpvGpu::Storage("out_delta", delivDeltaTarget),
							VpvGpu::Sampled("cell_prior", withVeto ? vetoPriorTex : in.receivedTex),
							VpvGpu::Sampled("face_mask", withVeto ? vetoFaceTex : in.receivedTex)
						};
						pl_shader_var va[20] = {
							VpvGpu::Var("u_dshift_x", &uDShX), VpvGpu::Var("u_dshift_y", &uDShY),
							VpvGpu::Var("u_cshift_x", &uCShX), VpvGpu::Var("u_cshift_y", &uCShY),
							VpvGpu::Var("u_all_zero_delta", &uAllZero), VpvGpu::Var("u_identity_grid", &uIdent),
							VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
							VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
							VpvGpu::Var("u_dax0", &uDAx0), VpvGpu::Var("u_day0", &uDAy0),
							VpvGpu::Var("u_dax1", &uDAx1), VpvGpu::Var("u_day1", &uDAy1),
							VpvGpu::Var("u_kr", &uKr), VpvGpu::Var("u_kg", &uKg), VpvGpu::Var("u_kb", &uKb),
							VpvGpu::Var("u_has_face_mask", &vetoHasFace),
							VpvGpu::Var("u_has_cell_prior", &vetoHasPrior),
							VpvGpu::Var("u_bokeh_mode", &uBokehMode)
						};
						if (!VpvGpu::Pass(dispatch, "td_v2_chroma_apply_rgb", aHdr, Shaders::kPassChromaApplyRgb,
						                  da, withVeto ? 6 : 4, va, withVeto ? 20 : 17, dGx, dGy, 0, 16, 16))
							return failVisible("td_v2_chroma_apply_rgb dispatch failed", StandInKind::GpuDispatchFailed);
					}
					++dispCount;
				}
				else if (allZeroDelta && out.outDeliveredDeltaTex != nullptr)
				{
					const std::string zdHdr = VpvGpu::DefineInt("TD_CH_W", dW) + VpvGpu::DefineInt("TD_CH_H", dH);
					constexpr const char* kZeroDelivBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_CH_W || p.y >= TD_CH_H) return;
imageStore(out_coded_delta, p, vec4(0.0));
)GLSL";
					pl_shader_desc dzd[1] = { VpvGpu::Storage("out_coded_delta", out.outDeliveredDeltaTex) };
					if (!VpvGpu::Pass(dispatch, "td_v2_chroma_zero_deliv_delta", zdHdr, kZeroDelivBody, dzd, 1, nullptr, 0,
					                  Detail::GroupsFor(dW, 16), Detail::GroupsFor(dH, 16), 0, 16, 16))
					{
						return failVisible("td_v2_chroma_zero_deliv_delta dispatch failed", StandInKind::GpuDispatchFailed);
					}
					++dispCount;
				}
				return true;
			};

			// Helper to zero outCodedDeltaTex AND refresh delivered outputs cleanly on no-ref / cut / invalid-A2 closure.
			auto zeroCodedDeltaAndApply = [&]() -> bool
			{
				const std::string zHdr = VpvGpu::DefineInt("TD_CH_W", cW) + VpvGpu::DefineInt("TD_CH_H", cH);
				constexpr const char* kZeroBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_CH_W || p.y >= TD_CH_H) return;
imageStore(out_coded_delta, p, vec4(0.0));
)GLSL";
				pl_shader_desc dz[1] = { VpvGpu::Storage("out_coded_delta", out.outCodedDeltaTex) };
				if (!VpvGpu::Pass(dispatch, "td_v2_chroma_zero_delta", zHdr, kZeroBody, dz, 1, nullptr, 0,
				                  Detail::GroupsFor(cW, 16), Detail::GroupsFor(cH, 16), 0, 16, 16))
				{
					return failVisible("td_v2_chroma_zero_delta dispatch failed", StandInKind::GpuDispatchFailed);
				}
				int zDispatches = 1;
				if (!runZeroCopyApply(true, nullptr, nullptr, 0.0f, 0.0f, zDispatches))
					return false;
				out.dispatchCount = zDispatches;
				lastDispatchCount_ = zDispatches;
				out.succeeded = true;
				return true;
			};

			if (refSpecs.empty())
				return zeroCodedDeltaAndApply();

			// Validate coding step prior upfront (1:1 parity with TdV2ChromaEngine / TdV2GpuEngine).
			const Detail::ResolvedA2StepPrior a2 = Detail::ResolveA2StepPrior(
				in.hasStepPrior, in.estimatedStepDelta, in.deadZonePhi, in.zeroCoeffFraction, standIns);
			if (!a2.stats.valid)
			{
				if (standIns != nullptr)
				{
					if (a2.inputNonFinite)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaGpuEngine non-finite A2 step/phi stats");
					standIns->Engage(StandInKind::A2StatsInvalid, "TdV2ChromaGpuEngine invalid A2 stats; delta closed to 0");
				}
				out.a2InvalidCells = numCells;
				if (a2.inputNonFinite)
					out.nonFiniteCells = numCells;
				return zeroCodedDeltaAndApply();
			}

			// TD-AUDIT-L2: Determine upfront which coded-grid reduction scratch textures are actually needed
			// so native YUV frames acquire ZERO reduction scratch (-47..190 MiB VRAM at 4K!).
			const bool needReduceRec = isRgb || (in.receivedTex->params.w != cW || in.receivedTex->params.h != cH) || (effCodedSub != delivSub);
			const bool needReduceClean = (in.hasCleanSplit && in.cleanUvTex != nullptr) &&
			                             (in.cleanUvTex->params.w != cW || in.cleanUvTex->params.h != cH);
			bool needReduceRef = false;
			for (const ChromaRefSpec& rs : refSpecs)
			{
				if (rs.tex->params.w != cW || rs.tex->params.h != cH)
				{
					needReduceRef = true;
					break;
				}
			}

			// Acquire per-frame scratch from shared ScratchArena (kChromaArenaScope = 0x0403, ZERO silent private fallback!).
			Detail::ArenaScopeGuard scopeGuard(arena, kChromaArenaScope);
			ChromaScratchSet sc{};
			if (arena != nullptr)
			{
				DestroyPrivateScratch(gpu);
				const uint32_t ucW = static_cast<uint32_t>(cW);
				const uint32_t ucH = static_cast<uint32_t>(cH);
				const uint32_t ucw = static_cast<uint32_t>(cw);
				const uint32_t uch = static_cast<uint32_t>(ch);
				const VpSkeleton::TexShape shRG{ ucW, ucH, VpSkeleton::TexFormat::RG32F };
				const VpSkeleton::TexShape shRGBA{ ucW, ucH, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shR16{ ucW, ucH, VpSkeleton::TexFormat::R16 };
				const VpSkeleton::TexShape shCell3RGBA{ 3u * ucw, uch, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shCellStats{ 3u * ucw, 3u * uch, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shCellRG{ ucw, uch, VpSkeleton::TexFormat::RG32F };
				const VpSkeleton::TexShape shCellRGBA{ ucw, uch, VpSkeleton::TexFormat::RGBA32F };

				bool ok = scopeGuard.opened;
				if (ok)
				{
					sc.curL = arena->Acquire(kChromaArenaOwner, shRG);
					for (int i = 0; i < kMaxChromaGpuRefs; ++i)
						sc.refL[i] = arena->Acquire(kChromaArenaOwner, shRG);
					sc.warpedUv = arena->Acquire(kChromaArenaOwner, shRG);
					sc.codedDeltaRaw = arena->Acquire(kChromaArenaOwner, shRG);
					if (needReduceRec)
						sc.reducedRec = arena->Acquire(kChromaArenaOwner, shRG);
					if (needReduceClean)
						sc.reducedClean = arena->Acquire(kChromaArenaOwner, shRG);
					if (needReduceRef)
						sc.reducedRef = arena->Acquire(kChromaArenaOwner, shRG);

					sc.curMH = arena->Acquire(kChromaArenaOwner, shRGBA);
					for (int i = 0; i < kMaxChromaGpuRefs; ++i)
						sc.refMH[i] = arena->Acquire(kChromaArenaOwner, shRGBA);
					sc.bandTmpCb = arena->Acquire(kChromaArenaOwner, shRGBA);
					sc.bandTmpCr = arena->Acquire(kChromaArenaOwner, shRGBA);

					sc.refFlags[0] = arena->Acquire(kChromaArenaOwner, shR16);
					sc.refFlags[1] = arena->Acquire(kChromaArenaOwner, shR16);

					sc.motionGate = arena->Acquire(kChromaArenaOwner, shCell3RGBA);
					sc.weightsCb = arena->Acquire(kChromaArenaOwner, shCell3RGBA);
					sc.weightsCr = arena->Acquire(kChromaArenaOwner, shCell3RGBA);
					sc.diagCb = arena->Acquire(kChromaArenaOwner, shCell3RGBA);
					sc.diagCr = arena->Acquire(kChromaArenaOwner, shCell3RGBA);

					sc.statsCb = arena->Acquire(kChromaArenaOwner, shCellStats);
					sc.statsCr = arena->Acquire(kChromaArenaOwner, shCellStats);

					sc.ownGrain = arena->Acquire(kChromaArenaOwner, shCellRGBA);
					sc.warpDiag = arena->Acquire(kChromaArenaOwner, shCellRG);
					sc.diagFlags = arena->Acquire(kChromaArenaOwner, shCellRGBA);
					sc.guardCell = arena->Acquire(kChromaArenaOwner, shCellRGBA);
					ok = sc.AllNonNull(needReduceRec, needReduceClean, needReduceRef);
				}
				if (!ok)
				{
					scratchShort_ = true;
					out.scratchShort = true;
					return failVisible("TdV2ChromaGpuEngine GPU scratch arena shortage (frame skipped visibly)", StandInKind::GpuScratchShortage);
				}
			}
			else
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::GpuPrivateScratchFallback, "TdV2ChromaGpuEngine arena == nullptr; using owned priv* scratch");
				if (!EnsurePrivateScratch(gpu, cW, cH, cw, ch, fmtR16, fmtRG32, fmtRGBA32,
				                          needReduceRec, needReduceClean, needReduceRef, sc))
					return failVisible("TdV2ChromaGpuEngine private scratch allocation failed", StandInKind::GpuDispatchFailed);
			}

			// Resolve host-readable tiny cell-grid diagnostic targets when readbackCellDiagnostics is requested.
			pl_tex dstWarpDiag = sc.warpDiag;
			pl_tex dstDiagFlags = sc.diagFlags;
			pl_tex dstGuardCell = sc.guardCell;
			if (out.readbackCellDiagnostics)
			{
				if (!Detail::EnsureTex(gpu, outWarpDiagTex_, cw, ch, fmtRG32, true, true, false, true) ||
				    !Detail::EnsureTex(gpu, outDiagFlagsTex_, cw, ch, fmtRGBA32, true, true, false, true) ||
				    !Detail::EnsureTex(gpu, outGuardCellTex_, cw, ch, fmtRGBA32, true, true, false, true))
				{
					return failVisible("TdV2ChromaGpuEngine diagnostic readback target alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstWarpDiag = outWarpDiagTex_;
				dstDiagFlags = outDiagFlagsTex_;
				dstGuardCell = outGuardCellTex_;
			}

			// Upload motion, cellPrior, and faceMask ONLY when prepacked GPU textures are absent.
			const int K = static_cast<int>(refSpecs.size());
			pl_tex motionVecTex = in.motionVecTex;
			pl_tex motionMetaTex = in.motionMetaTex;
			if (!motionVecTex || !motionMetaTex)
			{
				if (!Detail::EnsureTex(gpu, inMotionVecTex_, 3 * cw, ch, fmtRGBA32, true, false, true, false) ||
				    !Detail::EnsureTex(gpu, inMotionMetaTex_, 3 * cw, ch, fmtRGBA32, true, false, true, false))
					return failVisible("TdV2ChromaGpuEngine motion staging alloc failed", StandInKind::GpuDispatchFailed);
				std::vector<float> mvBuf(static_cast<size_t>(3 * cw) * static_cast<size_t>(ch) * 4u, 0.0f);
				std::vector<float> mmBuf(static_cast<size_t>(3 * cw) * static_cast<size_t>(ch) * 4u, 0.0f);
				for (int k = 0; k < K; ++k)
				{
					const CellMotionField& mf = refSpecs[static_cast<size_t>(k)].motion;
					const int slot = (std::max)(0, (std::min)(2, refSpecs[static_cast<size_t>(k)].motionSlot));
					const bool hasConf = !mf.confidence.empty();
					for (int cy = 0; cy < ch; ++cy)
					{
						for (int cx = 0; cx < cw; ++cx)
						{
							const size_t srcIdx = static_cast<size_t>(cy) * cw + cx;
							const size_t dst = static_cast<size_t>(cy) * (3 * cw) + static_cast<size_t>(slot * cw + cx);
							mvBuf[dst * 4u + 0u] = static_cast<float>(mf.vx[srcIdx]);
							mvBuf[dst * 4u + 1u] = static_cast<float>(mf.vy[srcIdx]);
							mvBuf[dst * 4u + 2u] = static_cast<float>(mf.camVx[srcIdx]);
							mvBuf[dst * 4u + 3u] = static_cast<float>(mf.camVy[srcIdx]);
							mmBuf[dst * 4u + 0u] = static_cast<float>(static_cast<int>(mf.cls[srcIdx]));
							mmBuf[dst * 4u + 1u] = mf.valid[srcIdx] ? 1.0f : 0.0f;
							mmBuf[dst * 4u + 2u] = hasConf ? static_cast<float>(mf.confidence[srcIdx]) : 1.0f;
							mmBuf[dst * 4u + 3u] = 0.0f;
						}
					}
				}
				if (!Detail::UploadFloats(gpu, inMotionVecTex_, mvBuf.data()) ||
				    !Detail::UploadFloats(gpu, inMotionMetaTex_, mmBuf.data()))
					return failVisible("TdV2ChromaGpuEngine motion upload failed", StandInKind::GpuDispatchFailed);
				motionVecTex = inMotionVecTex_;
				motionMetaTex = inMotionMetaTex_;
			}
			const int motionMetaComps = (motionMetaTex->params.format != nullptr) ? motionMetaTex->params.format->num_components : 2;
			const float uMotionMetaHasConf = (motionMetaComps >= 3) ? 1.0f : 0.0f;

			pl_tex cellPriorTex = in.cellPriorTex;
			if (!cellPriorTex)
			{
				if (!Detail::EnsureTex(gpu, inCellPriorTex_, cw, ch, fmtRGBA32, true, false, true, false))
					return failVisible("TdV2ChromaGpuEngine cellPrior staging alloc failed", StandInKind::GpuDispatchFailed);
				const int grainCellPx = cfg_.grainCellPx > 0 ? cfg_.grainCellPx : cellPx;
				const int gW = (in.lumaW + grainCellPx - 1) / grainCellPx;
				std::vector<float> cpBuf(numCells * 4u, 0.0f);
				bool oob = false;
				for (int cy = 0; cy < ch; ++cy)
				{
					for (int cx = 0; cx < cw; ++cx)
					{
						const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
						const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
						const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
						const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
						float gvCb = 0.0f;
						if (!in.retainedGrainVarCb.empty())
						{
							if (gIdx < in.retainedGrainVarCb.size()) gvCb = static_cast<float>(in.retainedGrainVarCb[gIdx]);
							else oob = true;
						}
						float gvCr = gvCb;
						if (!in.retainedGrainVarCr.empty())
						{
							if (gIdx < in.retainedGrainVarCr.size()) gvCr = static_cast<float>(in.retainedGrainVarCr[gIdx]);
							else oob = true;
						}
						float expFc = 0.0f;
						if (in.explicitFocusChangeCells != nullptr && cIdx < in.explicitFocusChangeCells->size() && (*in.explicitFocusChangeCells)[cIdx] != 0u)
							expFc += 1.0f;
						if (in.explicitDefocusedCells != nullptr && cIdx < in.explicitDefocusedCells->size() && (*in.explicitDefocusedCells)[cIdx] != 0u)
							expFc += 2.0f;
						float sScale = 1.0f;
						if (cIdx < in.cellStepScale.size())
							sScale = static_cast<float>(in.cellStepScale[cIdx]);
						cpBuf[cIdx * 4u + 0u] = gvCb;
						cpBuf[cIdx * 4u + 1u] = expFc;
						cpBuf[cIdx * 4u + 2u] = sScale;
						cpBuf[cIdx * 4u + 3u] = gvCr;
					}
				}
				if (oob && standIns != nullptr)
					standIns->Engage(StandInKind::GrainPriorOutOfRange, "TdV2ChromaGpuEngine retainedGrainVar undersized");
				if (!Detail::UploadFloats(gpu, inCellPriorTex_, cpBuf.data()))
					return failVisible("TdV2ChromaGpuEngine cellPrior upload failed", StandInKind::GpuDispatchFailed);
				cellPriorTex = inCellPriorTex_;
			}
			const int priorComps = cellPriorTex->params.format ? cellPriorTex->params.format->num_components : 2;
			const float uPriorHasStep = (priorComps >= 3) ? 1.0f : 0.0f;
			const float uPriorHasCrGrain = (priorComps >= 4) ? 1.0f : 0.0f;

			pl_tex faceMaskTex = in.faceMaskTex;
			bool hasFaceMask = (faceMaskTex != nullptr);
			size_t cpuFaceMasked = 0;
			if (!hasFaceMask && in.faceMask != nullptr && !in.faceMask->empty())
			{
				if (in.faceMask->size() != numCells && standIns != nullptr)
					standIns->Engage(StandInKind::FaceMaskSizeMismatch, "TdV2ChromaGpuEngine faceMask size mismatch");
				std::vector<float> fmBuf(numCells, 0.0f);
				for (size_t i = 0; i < numCells; ++i)
				{
					if (i >= in.faceMask->size() || (*in.faceMask)[i] != 0u) { fmBuf[i] = 1.0f; ++cpuFaceMasked; }
				}
				if (cpuFaceMasked > 0)
				{
					if (!Detail::EnsureTex(gpu, inFaceMaskTex_, cw, ch, fmtR32, true, false, true, false) ||
					    !Detail::UploadFloats(gpu, inFaceMaskTex_, fmBuf.data()))
						return failVisible("TdV2ChromaGpuEngine faceMask upload failed", StandInKind::GpuDispatchFailed);
					faceMaskTex = inFaceMaskTex_;
					hasFaceMask = true;
				}
			}
			const float uHasFaceMask = hasFaceMask ? 1.0f : 0.0f;
			pl_tex faceMaskBind = hasFaceMask ? faceMaskTex : sc.warpedUv;

			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double s1Sq = VpUnit::BandFromBase::kSigmaFine * VpUnit::BandFromBase::kSigmaFine;
			const double s2Sq = VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase;
			const double wStarSq = (2.0 * std::log(VpUnit::BandFromBase::kSigmaBase / VpUnit::BandFromBase::kSigmaFine)) / (s2Sq - s1Sq);
			const double midPeak = std::exp(-(s1Sq * wStarSq) / 2.0) - std::exp(-(s2Sq * wStarSq) / 2.0);
			const double a2PeakMidSq = midPeak * midPeak;

			const float uK = static_cast<float>(K);
			const float uPoolR = static_cast<float>(cfg_.momentPoolRadiusCells > 0 ? cfg_.momentPoolRadiusCells : 0);
			const float uEstimator = static_cast<float>(static_cast<int>(cfg_.estimator));
			const float uRefSlot0 = (K > 0) ? static_cast<float>(refSpecs[0].motionSlot) : 0.0f;
			const float uRefSlot1 = (K > 1) ? static_cast<float>(refSpecs[1].motionSlot) : 1.0f;
			const float uRefSlot2 = (K > 2) ? static_cast<float>(refSpecs[2].motionSlot) : 2.0f;
			const float uRefRole0 = (K > 0) ? static_cast<float>(refSpecs[0].refRole) : 0.0f;
			const float uRefRole1 = (K > 1) ? static_cast<float>(refSpecs[1].refRole) : 1.0f;
			const float uRefRole2 = (K > 2) ? static_cast<float>(refSpecs[2].refRole) : 2.0f;
			const float uExact0 = (K > 0 && refSpecs[0].isExactCopy) ? 1.0f : 0.0f;
			const float uExact1 = (K > 1 && refSpecs[1].isExactCopy) ? 1.0f : 0.0f;
			const float uExact2 = (K > 2 && refSpecs[2].isExactCopy) ? 1.0f : 0.0f;
			const float uHasSplit = (in.hasCleanSplit && in.cleanUvTex != nullptr) ? 1.0f : 0.0f;
			const float uEnableHuber = cfg_.enablePerCornerHuber ? 1.0f : 0.0f;
			const float uA2Valid = a2.stats.valid ? 1.0f : 0.0f;
			const float uA2Sent = a2.stats.valid ? static_cast<float>(a2.stats.sigmaSentSq) : 0.0f;
			const float uA2Zero = a2.stats.valid ? static_cast<float>(a2.stats.sigmaZeroSq) : 0.0f;
			const float uA2Total = a2.stats.valid ? static_cast<float>(a2.stats.totalVar) : 0.0f;
			const float uMidCode = static_cast<float>(std::ldexp(1.0, cfg_.sourceBits - 1));
			const float uPsdLow = static_cast<float>(in.psdShape.bandShareLow);
			const float uPsdMid = static_cast<float>(in.psdShape.bandShareMid);
			const float uPsdFine = static_cast<float>(in.psdShape.bandShareFine);
			const float uFadeKeep = static_cast<float>(effFadeKeep);

			std::string chromaGeom = Detail::StaticGeomDefines(in.lumaW, in.lumaH, cw, ch, cellPx);
			chromaGeom += VpvGpu::DefineInt("TD_CH_W", cW);
			chromaGeom += VpvGpu::DefineInt("TD_CH_H", cH);
			chromaGeom += VpvGpu::DefineInt("TD_CSUB_X", cSubX);
			chromaGeom += VpvGpu::DefineInt("TD_CSUB_Y", cSubY);
			chromaGeom += VpvGpu::DefineInt("TD_DSUB_X", dSubX);
			chromaGeom += VpvGpu::DefineInt("TD_DSUB_Y", dSubY);

			int dispatches = 0;
			auto runPass = [&](const char* desc, const std::string& hdr, const char* body,
			                   const pl_shader_desc* descs, int nDescs,
			                   const pl_shader_var* vars, int nVars,
			                   int gx, int gy, int gw = 16, int gh = 16, size_t shmem = 0) -> bool
			{
				if (!VpvGpu::Pass(dispatch, desc, hdr, body, descs, nDescs, vars, nVars, gx, gy, shmem, gw, gh))
					return false;
				++dispatches;
				return true;
			};

			const int cGx = Detail::GroupsFor(cW, 16);
			const int cGy = Detail::GroupsFor(cH, 16);

			// ---- PASS C0 (Optional): Reduce Delivered Chroma -> Coded Grid (0 dispatches when coded == delivered YUV!) ----
			auto reduceIfNeeded = [&](pl_tex srcTex, bool srcIsRgb, pl_tex dstScratch, pl_tex& outCodedTex) -> bool
			{
				const bool alreadyCodedYuv = (!srcIsRgb && srcTex->params.w == cW && srcTex->params.h == cH &&
				                              effCodedSub == delivSub);
				if (alreadyCodedYuv || (!srcIsRgb && srcTex->params.w == cW && srcTex->params.h == cH))
				{
					outCodedTex = srcTex; // Exact identity: zero reduction dispatches!
					return true;
				}
				if (dstScratch == nullptr)
					return false;
				std::string rHdr = chromaGeom;
				rHdr += VpvGpu::DefineInt("TD_REDUCE_FROM_RGB", srcIsRgb ? 1 : 0);
				rHdr += Shaders::kCoreHelpers;
				rHdr += ChromaShaders::kPassChromaReduceHeader;
				pl_shader_desc dr[2] = {
					VpvGpu::Sampled("src_deliv", srcTex),
					VpvGpu::Storage("dst_coded", dstScratch)
				};
				pl_shader_var vr[12] = {
					VpvGpu::Var("u_dax0", &uDAx0), VpvGpu::Var("u_day0", &uDAy0),
					VpvGpu::Var("u_dax1", &uDAx1), VpvGpu::Var("u_day1", &uDAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_dshift_x", &uDShiftX), VpvGpu::Var("u_dshift_y", &uDShiftY),
					VpvGpu::Var("u_kr", &uKr), VpvGpu::Var("u_kg", &uKg), VpvGpu::Var("u_kb", &uKb),
					VpvGpu::Var("u_mid_code", &uMidCode)
				};
				if (!runPass(srcIsRgb ? "td_v2_c0_reduce_rgb" : "td_v2_c0_reduce_yuv", rHdr,
				             ChromaShaders::kPassChromaReduce, dr, 2, vr, 12, cGx, cGy))
				{
					return false;
				}
				outCodedTex = dstScratch;
				return true;
			};

			pl_tex codedRecTex = nullptr;
			if (!reduceIfNeeded(in.receivedTex, isRgb, sc.reducedRec, codedRecTex))
				return failVisible("td_v2_c0_reduce current received chroma failed", StandInKind::GpuDispatchFailed);

			pl_tex codedCleanTex = codedRecTex;
			if (in.hasCleanSplit && in.cleanUvTex != nullptr)
			{
				if (!reduceIfNeeded(in.cleanUvTex, false, sc.reducedClean, codedCleanTex))
					return failVisible("td_v2_c0_reduce current clean chroma failed", StandInKind::GpuDispatchFailed);
			}
			else if (standIns != nullptr)
			{
				standIns->Engage(StandInKind::P10gGrainSeparator, "TdV2ChromaGpuEngine no upstream chroma split");
			}

			// ---- Motion Gate (reuses exact luma kPass1MotionGate shader!) ----
			{
				std::string mgHdr = Detail::StaticGeomDefines(in.lumaW, in.lumaH, cw, ch, cellPx);
				mgHdr += VpvGpu::DefineInt("TD_CLS_OBJECT", static_cast<int>(RegionMotionClass::Object));
				mgHdr += VpvGpu::DefineInt("TD_CLS_OVERLAY", static_cast<int>(RegionMotionClass::Overlay));
				mgHdr += VpvGpu::DefineInt("TD_CLS_DYN_TEX", static_cast<int>(RegionMotionClass::DynamicTexture));
				mgHdr += VpvGpu::Define("TD_STILL_SPEED", kStillObjectSpeedPx);
				mgHdr += VpvGpu::Define("TD_FAST_CUTOFF", kFastObjectCutoffPx);
				mgHdr += Shaders::kCoreHelpers;
				mgHdr += Shaders::kPass1MotionGateHeader;
				pl_shader_desc dmg[3] = {
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("motion_meta", motionMetaTex),
					VpvGpu::Storage("motion_gate", sc.motionGate)
				};
				pl_shader_var vmg[5] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefSlot0),
					VpvGpu::Var("u_ref_motion_slot_1", &uRefSlot1),
					VpvGpu::Var("u_ref_motion_slot_2", &uRefSlot2),
					VpvGpu::Var("u_motion_meta_has_conf", &uMotionMetaHasConf)
				};
				if (!runPass("td_v2_c1_motion_gate", mgHdr, Shaders::kPass1MotionGate, dmg, 3, vmg, 5,
				             Detail::GroupsFor(K * cw, 16), Detail::GroupsFor(ch, 16), 16, 16))
				{
					return failVisible("td_v2_c1_motion_gate dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS C1B/C1C Helper: Packed RG32F Separable L/M/H Band Split (or Branch-A Coarse-Only sigma = 32 luma px) ----
			if (standIns != nullptr)
				standIns->Engage(StandInKind::BandMemoW2);
			const int rBase = VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaBase);
			const int rFine = VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaFine);
			const double coarseSigmaX = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(cSubX);
			const double coarseSigmaY = kUnidentifiedChromaSigmaLumaPx / static_cast<double>(cSubY);
			const int coarseRadX = VpUnit::BandFromBase::BandRadius(coarseSigmaX);
			const int coarseRadY = VpUnit::BandFromBase::BandRadius(coarseSigmaY);
			double effLowGain = gains.low;
			double effLowOrigin = gains.originLow;
			if (coarseOnly)
			{
				const auto gxCoarse = VpUnit::BandFromBase::Taps<double>(coarseSigmaX, 3.0, true, 0.0);
				const auto gyCoarse = VpUnit::BandFromBase::Taps<double>(coarseSigmaY, 3.0, true, 0.0);
				double sumX2 = 0.0, sumY2 = 0.0;
				for (double v : gxCoarse) sumX2 += v * v;
				for (double v : gyCoarse) sumY2 += v * v;
				effLowGain = sumX2 * sumY2;
				const double g0X = !gxCoarse.empty() ? gxCoarse[gxCoarse.size() >> 1u] : 0.0;
				const double g0Y = !gyCoarse.empty() ? gyCoarse[gyCoarse.size() >> 1u] : 0.0;
				effLowOrigin = g0X * g0Y;
			}
			const float uCoarseOnly = coarseOnly ? 1.0f : 0.0f;
			const float uCoarseSigmaX = static_cast<float>(coarseSigmaX);
			const float uCoarseSigmaY = static_cast<float>(coarseSigmaY);
			const float uCoarseRadX = static_cast<float>(coarseRadX);
			const float uCoarseRadY = static_cast<float>(coarseRadY);
			const std::string cCoarseHdr = coarseOnly ? (chromaGeom + Shaders::kCoreHelpers) : std::string();
			const std::string cBandTaps = coarseOnly ? std::string() : ChromaShaders::BuildChromaBandTapsHeader();
			const std::string cBandHdrH = coarseOnly ? std::string() : (chromaGeom + cBandTaps + Shaders::kCoreHelpers + ChromaShaders::kPassChromaBandHHeader);
			const std::string cBandHdrV = coarseOnly ? std::string() : (chromaGeom + cBandTaps + ChromaShaders::kPassChromaBandVHeader);
			const size_t shBandH = static_cast<size_t>(16 * (16 + 2 * rBase)) * 2u * sizeof(float);
			const size_t shBandV = static_cast<size_t>((16 + 2 * rBase) * 16) * 8u * sizeof(float);

			auto runChromaBands = [&](pl_tex srcUv, pl_tex dstL, pl_tex dstMH) -> bool
			{
				if (coarseOnly)
				{
					pl_shader_var rch[6] = {
						VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
						VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
						VpvGpu::Var("u_coarse_sigma_x", &uCoarseSigmaX),
						VpvGpu::Var("u_coarse_rad_x", &uCoarseRadX)
					};
					pl_shader_desc dch[2] = {
						VpvGpu::Sampled("src_uv", srcUv),
						VpvGpu::Storage("tmp_cb", sc.bandTmpCb)
					};
					if (!runPass("td_v2_c1_coarse_h", cCoarseHdr, ChromaShaders::kPassChromaCoarseH, dch, 2, rch, 6, cGx, cGy))
						return false;
					pl_shader_var rcv[6] = {
						VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
						VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
						VpvGpu::Var("u_coarse_sigma_y", &uCoarseSigmaY),
						VpvGpu::Var("u_coarse_rad_y", &uCoarseRadY)
					};
					pl_shader_desc dcv[3] = {
						VpvGpu::Sampled("tmp_cb", sc.bandTmpCb),
						VpvGpu::Storage("bands_l", dstL),
						VpvGpu::Storage("bands_mh", dstMH)
					};
					return runPass("td_v2_c1_coarse_v", cCoarseHdr, ChromaShaders::kPassChromaCoarseV, dcv, 3, rcv, 6, cGx, cGy);
				}
				pl_shader_var rv[4] = {
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1)
				};
				pl_shader_desc dh[3] = {
					VpvGpu::Sampled("src_uv", srcUv),
					VpvGpu::Storage("tmp_cb", sc.bandTmpCb),
					VpvGpu::Storage("tmp_cr", sc.bandTmpCr)
				};
				if (!runPass("td_v2_c1_band_h", cBandHdrH, ChromaShaders::kPassChromaBandH, dh, 3, rv, 4, cGx, cGy, 16, 16, shBandH))
					return false;
				pl_shader_desc dv[4] = {
					VpvGpu::Sampled("tmp_cb", sc.bandTmpCb),
					VpvGpu::Sampled("tmp_cr", sc.bandTmpCr),
					VpvGpu::Storage("bands_l", dstL),
					VpvGpu::Storage("bands_mh", dstMH)
				};
				return runPass("td_v2_c1_band_v", cBandHdrV, ChromaShaders::kPassChromaBandV, dv, 4, rv, 4, cGx, cGy, 16, 16, shBandV);
			};

			if (!runChromaBands(codedCleanTex, sc.curL, sc.curMH))
				return failVisible("td_v2_c1_band current chroma failed", StandInKind::GpuDispatchFailed);

			// ---- PASS C1A + C1B/C: Warp each reference + Band Split ----
			const std::string cWarpHdr = chromaGeom +
				VpvGpu::DefineInt("TD_CLS_CAMERA", static_cast<int>(RegionMotionClass::Camera)) +
				Shaders::kCoreHelpers + ChromaShaders::kSitedCoordHelpers + ChromaShaders::kPassChromaWarpHeader;
			const size_t shChWarp = (20u * 20u * 3u) * sizeof(float);
			int curFlagsSlot = 0;
			for (int k = K - 1; k >= 0; --k)
			{
				const ChromaRefSpec& rSpec = refSpecs[static_cast<size_t>(k)];
				pl_tex refCodedTex = nullptr;
				if (!reduceIfNeeded(rSpec.tex, false, sc.reducedRef, refCodedTex))
					return failVisible("td_v2_c0_reduce ref chroma failed", StandInKind::GpuDispatchFailed);

				const bool isFirst = (k == K - 1);
				const int dstSlot = isFirst ? 0 : (1 - curFlagsSlot);
				const int srcSlot = isFirst ? 1 : curFlagsSlot;
				const bool sameRefCur = (refCodedTex == codedCleanTex);
				const float uRefIdx = static_cast<float>(k);
				const float uMotSlot = static_cast<float>(rSpec.motionSlot);
				const float uFlagsClear = isFirst ? 1.0f : 0.0f;
				const float uSameRefCur = sameRefCur ? 1.0f : 0.0f;
				const float uUseH = rSpec.cameraHomography.valid ? 1.0f : 0.0f;
				const float uH[9] = {
					static_cast<float>(rSpec.cameraHomography.h[0]),
					static_cast<float>(rSpec.cameraHomography.h[1]),
					static_cast<float>(rSpec.cameraHomography.h[2]),
					static_cast<float>(rSpec.cameraHomography.h[3]),
					static_cast<float>(rSpec.cameraHomography.h[4]),
					static_cast<float>(rSpec.cameraHomography.h[5]),
					static_cast<float>(rSpec.cameraHomography.h[6]),
					static_cast<float>(rSpec.cameraHomography.h[7]),
					static_cast<float>(rSpec.cameraHomography.h[8])
				};

				pl_shader_desc dw[7] = {
					VpvGpu::Sampled("ref_uv", refCodedTex),
					VpvGpu::Sampled("cur_uv", sameRefCur ? sc.diagFlags : codedCleanTex),
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("motion_meta", motionMetaTex),
					VpvGpu::Sampled("ref_flags_in", sc.refFlags[srcSlot]),
					VpvGpu::Storage("warped_uv", sc.warpedUv),
					VpvGpu::Storage("ref_flags_out", sc.refFlags[dstSlot])
				};
				pl_shader_var wv[20] = {
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_ref_idx", &uRefIdx), VpvGpu::Var("u_motion_slot", &uMotSlot),
					VpvGpu::Var("u_flags_clear", &uFlagsClear), VpvGpu::Var("u_same_ref_cur", &uSameRefCur),
					VpvGpu::Var("u_use_homography", &uUseH),
					VpvGpu::Var("u_h0", &uH[0]), VpvGpu::Var("u_h1", &uH[1]), VpvGpu::Var("u_h2", &uH[2]),
					VpvGpu::Var("u_h3", &uH[3]), VpvGpu::Var("u_h4", &uH[4]), VpvGpu::Var("u_h5", &uH[5]),
					VpvGpu::Var("u_h6", &uH[6]), VpvGpu::Var("u_h7", &uH[7]), VpvGpu::Var("u_h8", &uH[8])
				};
				if (!runPass("td_v2_c1_warp", cWarpHdr, ChromaShaders::kPassChromaWarp, dw, 7, wv, 20, cGx, cGy, 16, 16, shChWarp))
					return failVisible("td_v2_c1_warp dispatch failed", StandInKind::GpuDispatchFailed);
				curFlagsSlot = dstSlot;

				if (!runChromaBands(sc.warpedUv, sc.refL[k], sc.refMH[k]))
					return failVisible("td_v2_c1_band ref chroma failed", StandInKind::GpuDispatchFailed);
			}
			pl_tex finalFlagsTex = sc.refFlags[curFlagsSlot];

			// ---- PASS C2A: LDS Cell-Moment Reduction for Cb & Cr ----
			const bool sameCleanRec = (codedCleanTex == codedRecTex);
			const float uSameCleanRec = sameCleanRec ? 1.0f : 0.0f;
			{
				const std::string c2aHdr = chromaGeom + Shaders::kCoreHelpers + ChromaShaders::kSitedCoordHelpers +
				                           ChromaShaders::kPassChromaCellStatsHeader;
				pl_shader_desc d2a[18] = {
					VpvGpu::Sampled("cur_l", sc.curL), VpvGpu::Sampled("cur_mh", sc.curMH),
					VpvGpu::Sampled("ref_l_0", sc.refL[0]), VpvGpu::Sampled("ref_l_1", sc.refL[1]), VpvGpu::Sampled("ref_l_2", sc.refL[2]),
					VpvGpu::Sampled("ref_mh_0", sc.refMH[0]), VpvGpu::Sampled("ref_mh_1", sc.refMH[1]), VpvGpu::Sampled("ref_mh_2", sc.refMH[2]),
					VpvGpu::Sampled("ref_flags", finalFlagsTex),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Sampled("rec_uv", codedRecTex),
					VpvGpu::Sampled("cur_uv", sameCleanRec ? sc.weightsCb : codedCleanTex),
					VpvGpu::Storage("cell_stats_cb", sc.statsCb),
					VpvGpu::Storage("cell_stats_cr", sc.statsCr),
					VpvGpu::Storage("cell_own_grain", sc.ownGrain),
					VpvGpu::Storage("warp_diag", dstWarpDiag)
				};
				pl_shader_var v2a[11] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_has_clean_split", &uHasSplit),
					VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode)
				};
				const size_t sh2a = (256u * 9u + 4u) * sizeof(float);
				const std::string c2aBody = ChromaShaders::BuildPassChromaCellStatsBody();
				if (!runPass("td_v2_c2a_cell_stats", c2aHdr, c2aBody.c_str(), d2a, 18, v2a, 11, cw, ch, 16, 16, sh2a))
					return failVisible("td_v2_c2a_cell_stats dispatch failed", StandInKind::GpuDispatchFailed);
			}

			// ---- PASS C2B: Chan-Pebay Merge + Closed-Form w* = C^-1 c Solve (Cb & Cr) ----
			{
				std::string c2bHdr = chromaGeom;
				c2bHdr += VpvGpu::Define("TD_GAIN_LOW", effLowGain);
				c2bHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				c2bHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				c2bHdr += VpvGpu::Define("TD_ORIGIN_LOW", effLowOrigin);
				c2bHdr += VpvGpu::Define("TD_ORIGIN_MID", gains.originMid);
				c2bHdr += VpvGpu::Define("TD_ORIGIN_FINE", gains.originFine);
				c2bHdr += VpvGpu::Define("TD_A2_PEAK_MID_SQ", a2PeakMidSq);
				c2bHdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				c2bHdr += VpvGpu::Define("TD_TRACE_REG", kDefaultTraceRegScale);
				c2bHdr += VpvGpu::Define("TD_SOLVE_FLOOR", kSolveNumericalFloor);
				c2bHdr += VpvGpu::Define("TD_PIVOT_FLOOR", kSolvePivotFloor);
				c2bHdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				c2bHdr += VpvGpu::Define("TD_STEP_SCALE_MAX", kCellStepScaleMax);
				c2bHdr += Shaders::BuildFineTapHeader();
				c2bHdr += Shaders::kCoreHelpers;
				c2bHdr += Shaders::kPass2MergeSolveHeader;

				pl_shader_desc d2b[11] = {
					VpvGpu::Sampled("cell_stats_cb", sc.statsCb),
					VpvGpu::Sampled("cell_stats_cr", sc.statsCr),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Sampled("cell_own_grain", sc.ownGrain),
					VpvGpu::Storage("cell_weights_cb", sc.weightsCb),
					VpvGpu::Storage("cell_weights_cr", sc.weightsCr),
					VpvGpu::Storage("cell_diag_cb", sc.diagCb),
					VpvGpu::Storage("cell_diag_cr", sc.diagCr),
					VpvGpu::Storage("cell_diag_flags", dstDiagFlags)
				};
				pl_shader_var v2b[24] = {
					VpvGpu::Var("u_k", &uK), VpvGpu::Var("u_pool_r", &uPoolR), VpvGpu::Var("u_estimator", &uEstimator),
					VpvGpu::Var("u_ref_role_0", &uRefRole0), VpvGpu::Var("u_ref_role_1", &uRefRole1), VpvGpu::Var("u_ref_role_2", &uRefRole2),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefSlot0), VpvGpu::Var("u_ref_motion_slot_1", &uRefSlot1), VpvGpu::Var("u_ref_motion_slot_2", &uRefSlot2),
					VpvGpu::Var("u_exact_copy_0", &uExact0), VpvGpu::Var("u_exact_copy_1", &uExact1), VpvGpu::Var("u_exact_copy_2", &uExact2),
					VpvGpu::Var("u_a2_valid", &uA2Valid), VpvGpu::Var("u_a2_sigma_sent", &uA2Sent), VpvGpu::Var("u_a2_sigma_zero", &uA2Zero),
					VpvGpu::Var("u_has_clean_split", &uHasSplit), VpvGpu::Var("u_bokeh_mode", &uBokehMode),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep),
					VpvGpu::Var("u_cell_prior_has_cr_grain", &uPriorHasCrGrain),
					VpvGpu::Var("u_psd_low", &uPsdLow),
					VpvGpu::Var("u_psd_mid", &uPsdMid),
					VpvGpu::Var("u_psd_fine", &uPsdFine),
					VpvGpu::Var("u_coarse_only", &uCoarseOnly)
				};
				if (!runPass("td_v2_c2b_merge_solve", c2bHdr, ChromaShaders::kPassChromaMergeSolve, d2b, 11, v2b, 24,
				             Detail::GroupsFor(cw, 8), Detail::GroupsFor(ch, 8), 8, 8))
				{
					return failVisible("td_v2_c2b_merge_solve dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS C3: Coded-Grid Chroma Blend + 3-Sigma Huber + Parseval Bound ----
			{
				std::string c3Hdr = chromaGeom;
				c3Hdr += VpvGpu::Define("TD_A2_PEAK_MID_SQ", a2PeakMidSq);
				c3Hdr += VpvGpu::Define("TD_HUBER_KNEE_SQ", kHuberKneeSq);
				c3Hdr += VpvGpu::Define("TD_PIXEL_BOUND_SIGMA", kPixelBoundSigma);
				c3Hdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				c3Hdr += VpvGpu::Define("TD_STEP_SCALE_MAX", kCellStepScaleMax);
				c3Hdr += Shaders::kCoreHelpers;
				c3Hdr += ChromaShaders::kSitedCoordHelpers;
				c3Hdr += R"GLSL(
float td_a2_band_peak_sq(int b) { return (b == 1) ? TD_A2_PEAK_MID_SQ : 1.0; }
float td_vec_comp(vec4 v, int k) { if (k == 0) return v.x; if (k == 1) return v.y; return v.z; }
float td_huber_taper(float diffSq, float diagC)
{
    float denom = max(diagC, TD_NUM_FLOOR);
    float r = diffSq / denom;
    return (r <= TD_HUBER_KNEE_SQ) ? 1.0 : (TD_HUBER_KNEE_SQ / r);
}
)GLSL";

				pl_shader_desc d3[16] = {
					VpvGpu::Sampled("cur_l", sc.curL), VpvGpu::Sampled("cur_mh", sc.curMH),
					VpvGpu::Sampled("ref_l_0", sc.refL[0]), VpvGpu::Sampled("ref_l_1", sc.refL[1]), VpvGpu::Sampled("ref_l_2", sc.refL[2]),
					VpvGpu::Sampled("ref_mh_0", sc.refMH[0]), VpvGpu::Sampled("ref_mh_1", sc.refMH[1]), VpvGpu::Sampled("ref_mh_2", sc.refMH[2]),
					VpvGpu::Sampled("ref_flags", finalFlagsTex),
					VpvGpu::Sampled("cell_weights_cb", sc.weightsCb), VpvGpu::Sampled("cell_weights_cr", sc.weightsCr),
					VpvGpu::Sampled("cell_diag_cb", sc.diagCb), VpvGpu::Sampled("cell_diag_cr", sc.diagCr),
					VpvGpu::Sampled("cell_prior", cellPriorTex), VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Storage("coded_delta_raw", sc.codedDeltaRaw)
				};
				pl_shader_var v3[12] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_a2_valid", &uA2Valid), VpvGpu::Var("u_a2_total_var", &uA2Total),
					VpvGpu::Var("u_enable_per_corner_huber", &uEnableHuber),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep)
				};
				if (!runPass("td_v2_c3_blend_bound", c3Hdr, ChromaShaders::kPassChromaBlendBound, d3, 16, v3, 12, cGx, cGy))
					return failVisible("td_v2_c3_blend_bound dispatch failed", StandInKind::GpuDispatchFailed);
				if (standIns != nullptr)
					standIns->Engage(StandInKind::TransformUnknownBound);
			}

			// ---- PASS C4: Fused LDS Layer-4 TdGuardPort -> Grain-Preserving Signed-Delta Output ----
			{
				const size_t fineTile = static_cast<size_t>(16 + 2 * rFine);
				const size_t shC4 = (3u * fineTile * fineTile + 3u * fineTile * 16u + 256u + 2u) * 2u * sizeof(float);
				std::string c4Hdr = chromaGeom;
				c4Hdr += Shaders::BuildFineTapHeader();
				c4Hdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				c4Hdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				c4Hdr += Shaders::kCoreHelpers;
				c4Hdr += ChromaShaders::kSitedCoordHelpers;
				c4Hdr += ChromaShaders::kPassChromaGuardHeader;

				pl_shader_desc d4[7] = {
					VpvGpu::Sampled("rec_uv", codedRecTex),
					VpvGpu::Sampled("cur_uv", sameCleanRec ? sc.weightsCr : codedCleanTex),
					VpvGpu::Sampled("coded_delta_raw", sc.codedDeltaRaw),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Storage("out_coded_delta", out.outCodedDeltaTex),
					VpvGpu::Storage("guard_cell", dstGuardCell)
				};
				pl_shader_var v4[12] = {
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_has_clean_split", &uHasSplit), VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode), VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_cr_grain", &uPriorHasCrGrain),
					VpvGpu::Var("u_fade_keep", &uFadeKeep)
				};
				const std::string c4Body = ChromaShaders::BuildPassChromaGuardBody();
				if (!runPass("td_v2_c4_guard_delta", c4Hdr, c4Body.c_str(), d4, 7, v4, 12, cw, ch, 16, 16, shC4))
					return failVisible("td_v2_c4_guard_delta dispatch failed", StandInKind::GpuDispatchFailed);
				if (standIns != nullptr)
					standIns->Engage(StandInKind::GrainGuardW4c);
			}

			// ---- Optional Zero-Copy GPU Chroma Delta Apply (ZERO CPU roundtrip; post-upsample face/focus/bokeh veto parity!) ----
			if (!runZeroCopyApply(false, cellPriorTex, faceMaskBind, 1.0f, uHasFaceMask, dispatches))
				return false;

			out.dispatchCount = dispatches;
			lastDispatchCount_ = dispatches;

			if (out.readbackCellDiagnostics)
			{
				std::vector<float> diagBuf(numCells * 4u, 0.0f);
				std::vector<float> guardBuf(numCells * 4u, 0.0f);
				std::vector<float> warpBuf(numCells * 2u, 0.0f);
				if (!Detail::DownloadFloats(gpu, dstDiagFlags, diagBuf.data()) ||
				    !Detail::DownloadFloats(gpu, dstGuardCell, guardBuf.data()) ||
				    !Detail::DownloadFloats(gpu, dstWarpDiag, warpBuf.data()))
				{
					return failVisible("TdV2ChromaGpuEngine diagnostic download failed", StandInKind::GpuDispatchFailed);
				}
				for (size_t i = 0; i < numCells; ++i)
				{
					if (diagBuf[i * 4u + 0u] > 0.5f) ++out.focusVetoCells;
					const bool nfDiag = diagBuf[i * 4u + 1u] > 0.5f;
					if (diagBuf[i * 4u + 2u] > 0.5f) ++out.singularCells;
					if (diagBuf[i * 4u + 3u] > 0.5f) ++out.singleRefA2Cells;
					const unsigned gFlCb = static_cast<unsigned>(Detail::NonNegRoundToSize(guardBuf[i * 4u + 1u]));
					const unsigned gFlCr = static_cast<unsigned>(Detail::NonNegRoundToSize(guardBuf[i * 4u + 3u]));
					if (((gFlCb & 1u) != 0u && guardBuf[i * 4u + 0u] > 0.0f) ||
					    ((gFlCr & 1u) != 0u && guardBuf[i * 4u + 2u] > 0.0f))
						++out.guardViolationCells;
					out.borderBilinearPixels += Detail::NonNegRoundToSize(warpBuf[i * 2u + 0u]);
					const unsigned wMask = static_cast<unsigned>(Detail::NonNegRoundToSize(warpBuf[i * 2u + 1u]));
					if ((wMask & 8u) != 0u) ++out.faceMaskedCells;
					if (nfDiag || ((wMask & 16u) != 0u)) ++out.nonFiniteCells;
				}
				if (standIns != nullptr)
				{
					if (out.borderBilinearPixels > 0) standIns->Engage(StandInKind::BorderBilinearFallback);
					if (out.nonFiniteCells > 0) standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaGpuEngine non-finite closure");
					if (out.singularCells > 0) standIns->Engage(StandInKind::SingularSolveClosure, "TdV2ChromaGpuEngine singular solve");
					if (out.singleRefA2Cells > 0) standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
					if (out.faceMaskedCells > 0) standIns->Engage(StandInKind::FaceNonRigidMask);
				}
			}
			else
			{
				out.faceMaskedCells = cpuFaceMasked;
				if (hasFaceMask && standIns != nullptr) standIns->Engage(StandInKind::FaceNonRigidMask);
				if (K == 1 && cfg_.estimator != VarianceEstimator::A2_RoundingCells && standIns != nullptr)
					standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
				if (standIns != nullptr && !cActive.Empty())
				{
					// TD-AUDIT-L1: Evaluate degenerate-homography bbox strictly over sited coded-to-luma coordinates.
					const int minLx = static_cast<int>(std::floor(in.siting.CodedToLumaX(static_cast<double>(cActive.x0), cSubX)));
					const int maxLx = static_cast<int>(std::ceil(in.siting.CodedToLumaX(static_cast<double>(cActive.x1 - 1), cSubX))) + 1;
					const int minLy = static_cast<int>(std::floor(in.siting.CodedToLumaY(static_cast<double>(cActive.y0), cSubY)));
					const int maxLy = static_cast<int>(std::ceil(in.siting.CodedToLumaY(static_cast<double>(cActive.y1 - 1), cSubY))) + 1;
					for (const ChromaRefSpec& rs : refSpecs)
					{
						if (!rs.cameraHomography.HasDegenerateProjection(minLx, minLy, maxLx, maxLy))
							continue;
						bool mayHaveCamera = rs.motion.cls.empty();
						for (RegionMotionClass c : rs.motion.cls)
							if (c == RegionMotionClass::Camera) { mayHaveCamera = true; break; }
						if (mayHaveCamera)
						{
							standIns->Engage(StandInKind::NonFiniteClosure, "TdV2ChromaGpuEngine degenerate or non-finite cameraHomography");
							break;
						}
					}
				}
			}

			out.succeeded = true;
			return true;
		}

		// WARP-testable CPU-plane wrapper routing strictly through ProcessChromaGpu (for direct parity testing!).
		ChromaGpuProcessResult ProcessChromaPlanes(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const ChromaProcessInput& in,
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
			StandInRegistry* standIns = nullptr)
		{
			ChromaGpuProcessResult res;
			const bool isRgb = (in.deliveredFormat == DeliveredChromaFormat::Rgb444);
			if (isRgb)
			{
				res.outputR = in.receivedR;
				res.outputG = in.receivedG;
				res.outputB = in.receivedB;
			}
			else
			{
				res.outputCb = in.receivedCb;
				res.outputCr = in.receivedCr;
			}

			if (!cfg_.IsEnabled())
			{
				res.succeeded = true;
				return res;
			}

			auto failRes = [&](const std::string& msg, StandInKind kind) -> ChromaGpuProcessResult
			{
				lastError_ = msg;
				res.lastError = msg;
				res.succeeded = false;
				++failures_;
				if (standIns != nullptr) standIns->Engage(kind, msg.c_str());
				return res;
			};

			if (!gpu || !dispatch || in.lumaW <= 0 || in.lumaH <= 0)
				return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes invalid args", StandInKind::GpuDispatchFailed);

			const ChromaSubsampling delivSub = DeliveredSubsampling(in.deliveredFormat);
			const ChromaSubsampling effCodedSub = (in.gridStatus == ChromaGridStatus::Unidentified) ? delivSub : in.codedSubsampling;
			const int dW = ChromaDetail::SubsampledDim(in.lumaW, SubsampleX(delivSub));
			const int dH = ChromaDetail::SubsampledDim(in.lumaH, SubsampleY(delivSub));
			const int cW = ChromaDetail::SubsampledDim(in.lumaW, SubsampleX(effCodedSub));
			const int cH = ChromaDetail::SubsampledDim(in.lumaH, SubsampleY(effCodedSub));

			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtRG32 || !fmtRGBA32)
				return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes FP32 formats unsupported", StandInKind::GpuDispatchFailed);

			auto uploadUvPair = [&](pl_tex& tex, const TdV2Plane& cb, const TdV2Plane& cr) -> bool
			{
				if (cb.Empty() || cr.Empty() || cb.w != cr.w || cb.h != cr.h)
					return false;
				if (!Detail::EnsureTex(gpu, tex, cb.w, cb.h, fmtRG32, true, false, true, false))
					return false;
				std::vector<float> buf(cb.v.size() * 2u, 0.0f);
				for (size_t i = 0; i < cb.v.size(); ++i)
				{
					buf[i * 2u + 0u] = static_cast<float>(cb.v[i]);
					buf[i * 2u + 1u] = static_cast<float>(cr.v[i]);
				}
				return Detail::UploadFloats(gpu, tex, buf.data());
			};

			ChromaGpuInput gIn{};
			gIn.seq = in.seq;
			gIn.lumaW = in.lumaW;
			gIn.lumaH = in.lumaH;
			gIn.deliveredFormat = in.deliveredFormat;
			gIn.codedSubsampling = in.codedSubsampling;
			gIn.gridStatus = in.gridStatus;
			gIn.siting = in.siting;
			gIn.matrix = in.matrix;
			gIn.activeRect = in.activeRect;
			gIn.hasCleanSplit = in.hasCleanSplit;
			gIn.retainedGrainVarCb = in.retainedGrainVarCb;
			gIn.retainedGrainVarCr = in.retainedGrainVarCr;
			gIn.explicitFocusChangeCells = in.explicitFocusChangeCells;
			gIn.explicitDefocusedCells = in.explicitDefocusedCells;
			gIn.faceMask = in.faceMask;
			gIn.hasStepPrior = in.hasStepPrior;
			gIn.estimatedStepDelta = in.estimatedStepDelta;
			gIn.deadZonePhi = in.deadZonePhi;
			gIn.zeroCoeffFraction = in.zeroCoeffFraction;
			gIn.isCutOrSpanInvalid = in.isCutOrSpanInvalid;
			gIn.fadeKeep = in.fadeKeep;
			gIn.psdShape = in.psdShape;

			if (isRgb)
			{
				if (in.receivedR.w != dW || in.receivedR.h != dH || in.receivedG.w != dW || in.receivedG.h != dH || in.receivedB.w != dW || in.receivedB.h != dH)
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes RGB dimension mismatch", StandInKind::GpuDispatchFailed);
				if (!Detail::EnsureTex(gpu, inRecRgbTex_, dW, dH, fmtRGBA32, true, false, true, false))
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes RGB alloc failed", StandInKind::GpuDispatchFailed);
				std::vector<float> buf(in.receivedR.v.size() * 4u, 0.0f);
				for (size_t i = 0; i < in.receivedR.v.size(); ++i)
				{
					buf[i * 4u + 0u] = static_cast<float>(in.receivedR.v[i]);
					buf[i * 4u + 1u] = static_cast<float>(in.receivedG.v[i]);
					buf[i * 4u + 2u] = static_cast<float>(in.receivedB.v[i]);
				}
				if (!Detail::UploadFloats(gpu, inRecRgbTex_, buf.data()))
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes RGB upload failed", StandInKind::GpuDispatchFailed);
				gIn.receivedTex = inRecRgbTex_;
			}
			else
			{
				if (in.receivedCb.w != dW || in.receivedCb.h != dH || in.receivedCr.w != dW || in.receivedCr.h != dH)
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes YUV dimension mismatch", StandInKind::GpuDispatchFailed);
				if (!uploadUvPair(inRecUvTex_, in.receivedCb, in.receivedCr))
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes YUV upload failed", StandInKind::GpuDispatchFailed);
				gIn.receivedTex = inRecUvTex_;
			}

			if (in.hasCleanSplit && !in.cleanCb.Empty() && !in.cleanCr.Empty())
			{
				if (!uploadUvPair(inCleanUvTex_, in.cleanCb, in.cleanCr))
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes clean upload failed", StandInKind::GpuDispatchFailed);
				gIn.cleanUvTex = inCleanUvTex_;
			}

			pl_tex tmpPrevTex = nullptr;
			pl_tex tmpNextTex = nullptr;
			auto cleanupTempRefs = [&]()
			{
				if (tmpPrevTex) pl_tex_destroy(gpu, &tmpPrevTex);
				if (tmpNextTex) pl_tex_destroy(gpu, &tmpNextTex);
			};

			if (in.prevPeak.cleanCb != nullptr && in.prevPeak.cleanCr != nullptr)
			{
				if (!uploadUvPair(tmpPrevTex, *in.prevPeak.cleanCb, *in.prevPeak.cleanCr))
				{
					cleanupTempRefs();
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes prevPeak upload failed", StandInKind::GpuDispatchFailed);
				}
				gIn.prevPeak.cleanUvTex = tmpPrevTex;
				gIn.prevPeak.lumaMotion = in.prevPeak.lumaMotion;
				gIn.prevPeak.isExactCopyOfCurrent = in.prevPeak.isExactCopyOfCurrent;
				gIn.prevPeak.cameraHomography = in.prevPeak.cameraHomography;
			}
			if (in.nextPeak.cleanCb != nullptr && in.nextPeak.cleanCr != nullptr)
			{
				if (!uploadUvPair(tmpNextTex, *in.nextPeak.cleanCb, *in.nextPeak.cleanCr))
				{
					cleanupTempRefs();
					return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes nextPeak upload failed", StandInKind::GpuDispatchFailed);
				}
				gIn.nextPeak.cleanUvTex = tmpNextTex;
				gIn.nextPeak.lumaMotion = in.nextPeak.lumaMotion;
				gIn.nextPeak.isExactCopyOfCurrent = in.nextPeak.isExactCopyOfCurrent;
				gIn.nextPeak.cameraHomography = in.nextPeak.cameraHomography;
			}

			if (!Detail::EnsureTex(gpu, outCodedDeltaTex_, cW, cH, fmtRG32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outDelivDeltaTex_, dW, dH, fmtRG32, true, true, false, true))
			{
				cleanupTempRefs();
				return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes output alloc failed", StandInKind::GpuDispatchFailed);
			}
			if (!isRgb && !Detail::EnsureTex(gpu, outDelivUvTex_, dW, dH, fmtRGBA32, true, true, false, true))
			{
				cleanupTempRefs();
				return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes outUv alloc failed", StandInKind::GpuDispatchFailed);
			}
			if (isRgb && !Detail::EnsureTex(gpu, outDelivRgbTex_, dW, dH, fmtRGBA32, true, true, false, true))
			{
				cleanupTempRefs();
				return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes outRgb alloc failed", StandInKind::GpuDispatchFailed);
			}

			ChromaGpuOutput gOut{};
			gOut.outCodedDeltaTex = outCodedDeltaTex_;
			gOut.outDeliveredUvTex = (!isRgb && cfg_.ModifiesPixels()) ? outDelivUvTex_ : nullptr;
			gOut.outDeliveredRgbTex = (isRgb && cfg_.ModifiesPixels()) ? outDelivRgbTex_ : nullptr;
			gOut.outDeliveredDeltaTex = cfg_.ModifiesPixels() ? outDelivDeltaTex_ : nullptr;
			gOut.readbackCellDiagnostics = true;

			const bool ok = ProcessChromaGpu(gpu, dispatch, gIn, gOut, arena, standIns);
			cleanupTempRefs();

			res.cellsW = gOut.cellsW;
			res.cellsH = gOut.cellsH;
			res.codedW = gOut.codedW;
			res.codedH = gOut.codedH;
			res.activeRefCount = gOut.activeRefCount;
			res.dispatchCount = gOut.dispatchCount;
			res.guardViolationCells = gOut.guardViolationCells;
			res.focusVetoCells = gOut.focusVetoCells;
			res.faceMaskedCells = gOut.faceMaskedCells;
			res.nonFiniteCells = gOut.nonFiniteCells;
			res.a2InvalidCells = gOut.a2InvalidCells;
			res.singularCells = gOut.singularCells;
			res.singleRefA2Cells = gOut.singleRefA2Cells;
			res.motionStandInRefs = gOut.motionStandInRefs;
			res.borderBilinearPixels = gOut.borderBilinearPixels;
			res.coarseBandOnly = gOut.coarseBandOnly;
			res.scratchShort = gOut.scratchShort;
			res.lastError = gOut.lastError;
			if (!ok)
			{
				res.succeeded = false;
				return res;
			}

			std::vector<float> cdBuf(static_cast<size_t>(cW) * static_cast<size_t>(cH) * 2u, 0.0f);
			if (!Detail::DownloadFloats(gpu, outCodedDeltaTex_, cdBuf.data()))
				return failRes("TdV2ChromaGpuEngine codedDelta download failed", StandInKind::GpuDispatchFailed);
			res.codedDeltaCb = TdV2Plane(cW, cH, 0.0);
			res.codedDeltaCr = TdV2Plane(cW, cH, 0.0);
			for (size_t i = 0; i < res.codedDeltaCb.v.size(); ++i)
			{
				res.codedDeltaCb.v[i] = static_cast<double>(cdBuf[i * 2u + 0u]);
				res.codedDeltaCr.v[i] = static_cast<double>(cdBuf[i * 2u + 1u]);
			}

			if (cfg_.ModifiesPixels())
			{
				std::vector<float> ddBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
				if (!Detail::DownloadFloats(gpu, outDelivDeltaTex_, ddBuf.data()))
					return failRes("TdV2ChromaGpuEngine deliveredDelta download failed", StandInKind::GpuDispatchFailed);
				res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
				res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);
				for (size_t i = 0; i < res.deliveredDeltaCb.v.size(); ++i)
				{
					res.deliveredDeltaCb.v[i] = static_cast<double>(ddBuf[i * 2u + 0u]);
					res.deliveredDeltaCr.v[i] = static_cast<double>(ddBuf[i * 2u + 1u]);
				}

				std::vector<float> outBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 4u, 0.0f);
				if (!isRgb)
				{
					if (!Detail::DownloadFloats(gpu, outDelivUvTex_, outBuf.data()))
						return failRes("TdV2ChromaGpuEngine outUv download failed", StandInKind::GpuDispatchFailed);
					res.outputCb = TdV2Plane(dW, dH, 0.0);
					res.outputCr = TdV2Plane(dW, dH, 0.0);
					for (size_t i = 0; i < res.outputCb.v.size(); ++i)
					{
						// Preserve bit-exact double passthrough when GPU delivered delta is strictly 0.0f (face/focus/zero-conf parity).
						res.outputCb.v[i] = (ddBuf[i * 2u + 0u] == 0.0f && std::isfinite(in.receivedCb.v[i])) ? in.receivedCb.v[i] : static_cast<double>(outBuf[i * 4u + 0u]);
						res.outputCr.v[i] = (ddBuf[i * 2u + 1u] == 0.0f && std::isfinite(in.receivedCr.v[i])) ? in.receivedCr.v[i] : static_cast<double>(outBuf[i * 4u + 1u]);
					}
				}
				else
				{
					if (!Detail::DownloadFloats(gpu, outDelivRgbTex_, outBuf.data()))
						return failRes("TdV2ChromaGpuEngine outRgb download failed", StandInKind::GpuDispatchFailed);
					res.outputR = TdV2Plane(dW, dH, 0.0);
					res.outputG = TdV2Plane(dW, dH, 0.0);
					res.outputB = TdV2Plane(dW, dH, 0.0);
					for (size_t i = 0; i < res.outputR.v.size(); ++i)
					{
						const bool zeroD = (ddBuf[i * 2u + 0u] == 0.0f && ddBuf[i * 2u + 1u] == 0.0f) &&
						                   std::isfinite(in.receivedR.v[i]) && std::isfinite(in.receivedG.v[i]) && std::isfinite(in.receivedB.v[i]);
						res.outputR.v[i] = zeroD ? in.receivedR.v[i] : static_cast<double>(outBuf[i * 4u + 0u]);
						res.outputG.v[i] = zeroD ? in.receivedG.v[i] : static_cast<double>(outBuf[i * 4u + 1u]);
						res.outputB.v[i] = zeroD ? in.receivedB.v[i] : static_cast<double>(outBuf[i * 4u + 2u]);
					}
				}
			}
			else
			{
				res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
				res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);
			}

			res.succeeded = true;
			return res;
		}

	private:
		struct ChromaScratchSet
		{
			pl_tex curL = nullptr;
			pl_tex refL[kMaxChromaGpuRefs] = {};
			pl_tex warpedUv = nullptr;
			pl_tex codedDeltaRaw = nullptr;
			pl_tex reducedRec = nullptr;
			pl_tex reducedClean = nullptr;
			pl_tex reducedRef = nullptr;
			pl_tex curMH = nullptr;
			pl_tex refMH[kMaxChromaGpuRefs] = {};
			pl_tex bandTmpCb = nullptr;
			pl_tex bandTmpCr = nullptr;
			pl_tex refFlags[2] = {};
			pl_tex motionGate = nullptr;
			pl_tex weightsCb = nullptr;
			pl_tex weightsCr = nullptr;
			pl_tex diagCb = nullptr;
			pl_tex diagCr = nullptr;
			pl_tex statsCb = nullptr;
			pl_tex statsCr = nullptr;
			pl_tex ownGrain = nullptr;
			pl_tex warpDiag = nullptr;
			pl_tex diagFlags = nullptr;
			pl_tex guardCell = nullptr;

			bool AllNonNull(bool needReduceRec = true, bool needReduceClean = true, bool needReduceRef = true) const
			{
				if (!curL || !warpedUv || !codedDeltaRaw ||
				    (needReduceRec && !reducedRec) || (needReduceClean && !reducedClean) || (needReduceRef && !reducedRef) ||
				    !curMH || !bandTmpCb || !bandTmpCr || !refFlags[0] || !refFlags[1] ||
				    !motionGate || !weightsCb || !weightsCr || !diagCb || !diagCr ||
				    !statsCb || !statsCr || !ownGrain || !warpDiag || !diagFlags || !guardCell)
					return false;
				for (int i = 0; i < kMaxChromaGpuRefs; ++i)
					if (!refL[i] || !refMH[i]) return false;
				return true;
			}
		};

		bool EnsurePrivateScratch(pl_gpu gpu, int cW, int cH, int cw, int ch,
		                          pl_fmt fmtR16, pl_fmt fmtRG32, pl_fmt fmtRGBA32,
		                          bool needReduceRec, bool needReduceClean, bool needReduceRef,
		                          ChromaScratchSet& out)
		{
			bool ok = Detail::EnsureTex(gpu, privCurL_, cW, cH, fmtRG32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privWarpedUv_, cW, cH, fmtRG32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCodedDeltaRaw_, cW, cH, fmtRG32, true, true, false, false) &&
			          (!needReduceRec || Detail::EnsureTex(gpu, privReducedRec_, cW, cH, fmtRG32, true, true, false, false)) &&
			          (!needReduceClean || Detail::EnsureTex(gpu, privReducedClean_, cW, cH, fmtRG32, true, true, false, false)) &&
			          (!needReduceRef || Detail::EnsureTex(gpu, privReducedRef_, cW, cH, fmtRG32, true, true, false, false)) &&
			          Detail::EnsureTex(gpu, privCurMH_, cW, cH, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privBandTmpCb_, cW, cH, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privBandTmpCr_, cW, cH, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privRefFlags_[0], cW, cH, fmtR16, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privRefFlags_[1], cW, cH, fmtR16, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privMotionGate_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privWeightsCb_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privWeightsCr_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privDiagCb_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privDiagCr_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privStatsCb_, 3 * cw, 3 * ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privStatsCr_, 3 * cw, 3 * ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privOwnGrain_, cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privWarpDiag_, cw, ch, fmtRG32, true, true, false, true) &&
			          Detail::EnsureTex(gpu, privDiagFlags_, cw, ch, fmtRGBA32, true, true, false, true) &&
			          Detail::EnsureTex(gpu, privGuardCell_, cw, ch, fmtRGBA32, true, true, false, true);
			for (int i = 0; i < kMaxChromaGpuRefs && ok; ++i)
			{
				ok = Detail::EnsureTex(gpu, privRefL_[i], cW, cH, fmtRG32, true, true, false, false) &&
				     Detail::EnsureTex(gpu, privRefMH_[i], cW, cH, fmtRGBA32, true, true, false, false);
			}
			if (!ok) return false;
			out.curL = privCurL_;
			out.warpedUv = privWarpedUv_;
			out.codedDeltaRaw = privCodedDeltaRaw_;
			out.reducedRec = needReduceRec ? privReducedRec_ : nullptr;
			out.reducedClean = needReduceClean ? privReducedClean_ : nullptr;
			out.reducedRef = needReduceRef ? privReducedRef_ : nullptr;
			out.curMH = privCurMH_;
			out.bandTmpCb = privBandTmpCb_;
			out.bandTmpCr = privBandTmpCr_;
			out.refFlags[0] = privRefFlags_[0];
			out.refFlags[1] = privRefFlags_[1];
			out.motionGate = privMotionGate_;
			out.weightsCb = privWeightsCb_;
			out.weightsCr = privWeightsCr_;
			out.diagCb = privDiagCb_;
			out.diagCr = privDiagCr_;
			out.statsCb = privStatsCb_;
			out.statsCr = privStatsCr_;
			out.ownGrain = privOwnGrain_;
			out.warpDiag = privWarpDiag_;
			out.diagFlags = privDiagFlags_;
			out.guardCell = privGuardCell_;
			for (int i = 0; i < kMaxChromaGpuRefs; ++i)
			{
				out.refL[i] = privRefL_[i];
				out.refMH[i] = privRefMH_[i];
			}
			return true;
		}

		void DestroyPrivateScratch(pl_gpu gpu)
		{
			if (!gpu) return;
			for (pl_tex* t : {
				&privCurL_, &privRefL_[0], &privRefL_[1], &privRefL_[2],
				&privWarpedUv_, &privCodedDeltaRaw_, &privReducedRec_, &privReducedClean_, &privReducedRef_,
				&privCurMH_, &privRefMH_[0], &privRefMH_[1], &privRefMH_[2], &privBandTmpCb_, &privBandTmpCr_,
				&privRefFlags_[0], &privRefFlags_[1],
				&privMotionGate_, &privWeightsCb_, &privWeightsCr_, &privDiagCb_, &privDiagCr_,
				&privStatsCb_, &privStatsCr_, &privOwnGrain_, &privWarpDiag_, &privDiagFlags_, &privGuardCell_ })
			{
				if (*t != nullptr) pl_tex_destroy(gpu, t);
			}
		}

		Config cfg_;
		pl_tex privCurL_ = nullptr;
		pl_tex privRefL_[kMaxChromaGpuRefs] = {};
		pl_tex privWarpedUv_ = nullptr;
		pl_tex privCodedDeltaRaw_ = nullptr;
		pl_tex privReducedRec_ = nullptr;
		pl_tex privReducedClean_ = nullptr;
		pl_tex privReducedRef_ = nullptr;
		pl_tex privCurMH_ = nullptr;
		pl_tex privRefMH_[kMaxChromaGpuRefs] = {};
		pl_tex privBandTmpCb_ = nullptr;
		pl_tex privBandTmpCr_ = nullptr;
		pl_tex privRefFlags_[2] = {};
		pl_tex privMotionGate_ = nullptr;
		pl_tex privWeightsCb_ = nullptr;
		pl_tex privWeightsCr_ = nullptr;
		pl_tex privDiagCb_ = nullptr;
		pl_tex privDiagCr_ = nullptr;
		pl_tex privStatsCb_ = nullptr;
		pl_tex privStatsCr_ = nullptr;
		pl_tex privOwnGrain_ = nullptr;
		pl_tex privWarpDiag_ = nullptr;
		pl_tex privDiagFlags_ = nullptr;
		pl_tex privGuardCell_ = nullptr;

		pl_tex inRecUvTex_ = nullptr;
		pl_tex inRecRgbTex_ = nullptr;
		pl_tex inCleanUvTex_ = nullptr;
		pl_tex inRefUvTex_ = nullptr;
		pl_tex inMotionVecTex_ = nullptr;
		pl_tex inMotionMetaTex_ = nullptr;
		pl_tex inCellPriorTex_ = nullptr;
		pl_tex inFaceMaskTex_ = nullptr;
		pl_tex outCodedDeltaTex_ = nullptr;
		pl_tex outDelivUvTex_ = nullptr;
		pl_tex outDelivRgbTex_ = nullptr;
		pl_tex outDelivDeltaTex_ = nullptr;
		pl_tex outDiagFlagsTex_ = nullptr;
		pl_tex outGuardCellTex_ = nullptr;
		pl_tex outWarpDiagTex_ = nullptr;

		std::string lastError_;
		uint64_t failures_ = 0;
		int lastDispatchCount_ = 0;
		bool scratchShort_ = false;
	};
} // namespace Gpu
} // namespace TdV2
