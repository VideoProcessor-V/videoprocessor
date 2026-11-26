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
		// Uses 24x24 vec2 LDS tile + 4x4 luma cell MV LDS cache so enableSmoothWarpVectors also hits LDS!
		constexpr const char* kPassChromaWarpHeader = R"GLSL(
shared vec2 td_ch_sh_uv[24][24];
shared uint td_ch_sh_bad[24][24];
shared float td_ch_mvx[16];
shared float td_ch_mvy[16];
shared uint td_ch_mvok[16];
shared int td_ch_mvcls[16];
shared int td_ch_tbase[2];
shared uint td_ch_lds_flags; // bit 0 = useLds, bit 1 = allSameMv

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
int tid = loc.y * 16 + loc.x;
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
bool useH = (u_use_homography > 0.5);
bool enableSmoothWarp = (u_enable_smooth_warp > 0.5);

// Workgroup-uniform 4x4 luma cell MV cache + 24x24 LDS tile eligibility (supports enableSmoothWarp == true!).
ivec2 c00 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX), min(TD_CH_H - 1, baseY)));
ivec2 c11 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX + 15), min(TD_CH_H - 1, baseY + 15)));
int minCx = max(0, c00.x - 1);
int maxCx = min(TD_CW - 1, c11.x + 1);
int minCy = max(0, c00.y - 1);
int maxCy = min(TD_CH - 1, c11.y + 1);
int spanX = maxCx - minCx + 1;
int spanY = maxCy - minCy + 1;
bool mvCacheOk = (spanX <= 4) && (spanY <= 4);
int mvCount = spanX * spanY;
if (mvCacheOk && tid < mvCount)
{
    int dy = tid / spanX;
    int dx = tid - dy * spanX;
    int ncx = minCx + dx;
    int ncy = minCy + dy;
    vec2 mv = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + ncx, ncy), 0).xy;
    vec2 mm = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + ncx, ncy), 0).xy;
    bool ok = (mm.y >= 0.5) && !td_is_bad(mv.x) && !td_is_bad(mv.y);
    td_ch_mvx[tid] = ok ? mv.x : 0.0;
    td_ch_mvy[tid] = ok ? mv.y : 0.0;
    td_ch_mvok[tid] = ok ? 1u : 0u;
    td_ch_mvcls[tid] = int(round(mm.x));
}
memoryBarrierShared();
barrier();

if (tid == 0)
{
    uint ldsFl = 0u;
    int tbX = 0, tbY = 0;
    if (mvCacheOk && validRect && !useH && mvCount > 0)
    {
        bool allOk = true;
        bool allSame = true;
        float refX = td_ch_mvx[0];
        float refY = td_ch_mvy[0];
        float minX = refX, maxX = refX, minY = refY, maxY = refY;
        for (int i = 0; i < mvCount; ++i)
        {
            if (td_ch_mvok[i] == 0u) { allOk = false; break; }
            float vx = td_ch_mvx[i];
            float vy = td_ch_mvy[i];
            if (vx != refX || vy != refY) allSame = false;
            minX = min(minX, vx); maxX = max(maxX, vx);
            minY = min(minY, vy); maxY = max(maxY, vy);
        }
        if (allOk)
        {
            float invSubX = 1.0 / float(TD_CSUB_X);
            float invSubY = 1.0 / float(TD_CSUB_Y);
            float cMinX = minX * invSubX; float cMaxX = maxX * invSubX;
            float cMinY = minY * invSubY; float cMaxY = maxY * invSubY;
            if (abs(cMinX) <= float(TD_CH_W) && abs(cMaxX) <= float(TD_CH_W) &&
                abs(cMinY) <= float(TD_CH_H) && abs(cMaxY) <= float(TD_CH_H))
            {
                tbX = int(floor(float(baseX - cax0) + cMinX)) - 1;
                tbY = int(floor(float(baseY - cay0) + cMinY)) - 1;
                int tMaxX = int(floor(float(baseX + 15 - cax0) + cMaxX)) + 2;
                int tMaxY = int(floor(float(baseY + 15 - cay0) + cMaxY)) + 2;
                if ((tMaxX - tbX < 24) && (tMaxY - tbY < 24))
                    ldsFl |= 1u;
            }
            if (allSame)
                ldsFl |= 2u;
        }
    }
    td_ch_tbase[0] = tbX;
    td_ch_tbase[1] = tbY;
    td_ch_lds_flags = ldsFl;
}
memoryBarrierShared();
barrier();

uint ldsFlags = td_ch_lds_flags;
bool useLds = (ldsFlags & 1u) != 0u;
bool allSameMv = (ldsFlags & 2u) != 0u;
int tileBaseX = td_ch_tbase[0];
int tileBaseY = td_ch_tbase[1];
if (useLds)
{
    for (int idx = tid; idx < 576; idx += 256)
    {
        int ty = idx / 24;
        int tx = idx - ty * 24;
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
vec2 mvLuma = vec2(0.0);
int mvCls = 0;
bool mvValid = false;
bool mvBad = false;
if (mvCacheOk && lc.x >= minCx && lc.x <= maxCx && lc.y >= minCy && lc.y <= maxCy)
{
    int ci = (lc.y - minCy) * spanX + (lc.x - minCx);
    mvCls = td_ch_mvcls[ci];
    if (td_ch_mvok[ci] != 0u)
    {
        mvValid = true;
        mvLuma = vec2(td_ch_mvx[ci], td_ch_mvy[ci]);
    }
    else
    {
        vec2 mvRaw = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + lc.x, lc.y), 0).xy;
        vec2 mmRaw = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + lc.x, lc.y), 0).xy;
        mvValid = (mmRaw.y >= 0.5);
        mvLuma = mvRaw;
        mvBad = td_is_bad(mvRaw.x) || td_is_bad(mvRaw.y);
    }
}
else
{
    mvLuma = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + lc.x, lc.y), 0).xy;
    vec2 mvMeta = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + lc.x, lc.y), 0).xy;
    mvCls = int(round(mvMeta.x));
    mvValid = (mvMeta.y >= 0.5);
    mvBad = td_is_bad(mvLuma.x) || td_is_bad(mvLuma.y);
}

int extraBad = 0;
int selfRx = (p.x - cax0) - tileBaseX;
int selfRy = (p.y - cay0) - tileBaseY;
if (useLds && selfRx >= 0 && selfRx < 24 && selfRy >= 0 && selfRy < 24)
{
    extraBad = (td_ch_sh_bad[selfRy][selfRx] != 0u) ? 4 : 0;
}
else
{
    vec2 selfRef = texelFetch(ref_uv, p, 0).xy;
    extraBad = (td_is_bad(selfRef.x) || td_is_bad(selfRef.y)) ? 4 : 0;
}

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
    if (mvBad)
    {
        vec2 fb = td_ch_cur_fallback(p);
        uint np = td_pack_ref_flags(prevPacked, refIdx, 4);
        if (extraBad != 0) np |= 512u;
        imageStore(warped_uv, p, vec4(fb, 0.0, 0.0));
        imageStore(ref_flags_out, p, vec4(float(np) / 65535.0, 0.0, 0.0, 0.0));
        return;
    }
    if (enableSmoothWarp && !allSameMv)
    {
        float lx = td_coded_to_luma_x(float(p.x));
        float ly = td_coded_to_luma_y(float(p.y));
        float gx = (lx + 0.5) / float(TD_CELL_PX) - 0.5;
        float gy = (ly + 0.5) / float(TD_CELL_PX) - 0.5;
        int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
        int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
        int x1 = min(TD_CW - 1, x0 + 1);
        int y1 = min(TD_CH - 1, y0 + 1);
        float tx = clamp(gx - float(x0), 0.0, 1.0);
        float ty = clamp(gy - float(y0), 0.0, 1.0);
        bool cornersOk = false;
        vec2 c00m = vec2(0.0), c10m = vec2(0.0), c01m = vec2(0.0), c11m = vec2(0.0);
        if (mvCacheOk && x0 >= minCx && x1 <= maxCx && y0 >= minCy && y1 <= maxCy)
        {
            int i00 = (y0 - minCy) * spanX + (x0 - minCx);
            int i10 = (y0 - minCy) * spanX + (x1 - minCx);
            int i01 = (y1 - minCy) * spanX + (x0 - minCx);
            int i11 = (y1 - minCy) * spanX + (x1 - minCx);
            cornersOk = (td_ch_mvok[i00] != 0u) && (td_ch_mvok[i10] != 0u) &&
                        (td_ch_mvok[i01] != 0u) && (td_ch_mvok[i11] != 0u);
            if (cornersOk)
            {
                c00m = vec2(td_ch_mvx[i00], td_ch_mvy[i00]);
                c10m = vec2(td_ch_mvx[i10], td_ch_mvy[i10]);
                c01m = vec2(td_ch_mvx[i01], td_ch_mvy[i01]);
                c11m = vec2(td_ch_mvx[i11], td_ch_mvy[i11]);
            }
        }
        else
        {
            c00m = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + x0, y0), 0).xy;
            c10m = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + x1, y0), 0).xy;
            c01m = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + x0, y1), 0).xy;
            c11m = texelFetch(motion_vec, ivec2(motionSlot * TD_CW + x1, y1), 0).xy;
            vec2 m00m = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + x0, y0), 0).xy;
            vec2 m10m = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + x1, y0), 0).xy;
            vec2 m01m = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + x0, y1), 0).xy;
            vec2 m11m = texelFetch(motion_meta, ivec2(motionSlot * TD_CW + x1, y1), 0).xy;
            cornersOk = (m00m.y >= 0.5) && (m10m.y >= 0.5) && (m01m.y >= 0.5) && (m11m.y >= 0.5) &&
                        (!td_is_bad(c00m.x)) && (!td_is_bad(c00m.y)) &&
                        (!td_is_bad(c10m.x)) && (!td_is_bad(c10m.y)) &&
                        (!td_is_bad(c01m.x)) && (!td_is_bad(c01m.y)) &&
                        (!td_is_bad(c11m.x)) && (!td_is_bad(c11m.y));
        }
        if (cornersOk)
        {
            vec2 topM = c00m + (c10m - c00m) * tx;
            vec2 botM = c01m + (c11m - c01m) * tx;
            mvLuma = topM + (botM - topM) * ty;
        }
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
    ldsInBounds = (ldsRx >= 0 && ldsRx + 3 < 24 && ldsRy >= 0 && ldsRy + 3 < 24);
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
    return;
}

int center = lx + TD_RAD_BASE;
vec2 accFine = vec2(0.0);
vec2 accBase = vec2(0.0);
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    vec2 v = td_ch_h[ly][center + k];
    accBase += TD_TAP_BASE[k + TD_RAD_BASE] * v;
    if (abs(k) <= TD_RAD_FINE)
        accFine += TD_TAP_FINE[k + TD_RAD_FINE] * v;
}

imageStore(tmp_cb, p, vec4(accFine.x, accFine.y, accBase.x, accBase.y));
)GLSL";

		constexpr const char* kPassChromaBandVHeader = R"GLSL(
shared vec4 td_ch_v[16 + 2 * TD_RAD_BASE][16];
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
    td_ch_v[row][lx] = texelFetch(tmp_cb, ivec2(safeX, sy), 0);
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
vec2 sv = texelFetch(src_uv, p, 0).xy;
vec2 srcVal = vec2(td_is_bad(sv.x) ? 0.0 : sv.x, td_is_bad(sv.y) ? 0.0 : sv.y);
vec2 gFine = vec2(0.0);
vec2 gBase = vec2(0.0);
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    vec4 tv = td_ch_v[center + k][lx];
    gBase += TD_TAP_BASE[k + TD_RAD_BASE] * tv.zw;
    if (abs(k) <= TD_RAD_FINE)
        gFine += TD_TAP_FINE[k + TD_RAD_FINE] * tv.xy;
}

vec2 bL = gBase;
vec2 bM = gFine - bL;
vec2 bH = srcVal - gFine;
imageStore(bands_l, p, vec4(bL, 0.0, 0.0));
imageStore(bands_mh, p, vec4(bM.x, bM.y, bH.x, bH.y));
)GLSL";

		// ---- PASS C1B+C1C FUSED (Chroma Fusion 1 — TD-GPU-OPT-2): 1-Pass 2D LDS Separable U2 BandFromBase ----
		// Fuses c1_band_h + c1_band_v into ONE dispatch (16.75 KB LDS, padded 32x33 & 32x17), eliminating
		// sc.bandTmpCb RGBA32F write+read AND 2nd src_uv read across cur + all K refs (bit-exact!).
		constexpr const char* kPassChromaBand2DHeader = R"GLSL(
shared vec2 td_ch_s0[16 + 2 * TD_RAD_BASE][16 + 2 * TD_RAD_BASE + 1];
shared vec4 td_ch_s1[16 + 2 * TD_RAD_BASE][17];
)GLSL";

		constexpr const char* kPassChromaBand2D = R"GLSL(
int baseX = int(gl_WorkGroupID.x) * 16;
int baseY = int(gl_WorkGroupID.y) * 16;
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;
ivec2 p = ivec2(baseX + lx, baseY + ly);

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool validRect = (cax1 > cax0) && (cay1 > cay0);
int xMin = cax0;
int xMax = max(cax0, cax1 - 1);
int yMin = cay0;
int yMax = max(cay0, cay1 - 1);

int tileDim = 16 + 2 * TD_RAD_BASE;
int tileN = tileDim * tileDim;
for (int idx = tid; idx < tileN; idx += 256)
{
    int ty = idx / tileDim;
    int tx = idx - ty * tileDim;
    int gx = baseX - TD_RAD_BASE + tx;
    int gy = baseY - TD_RAD_BASE + ty;
    int sx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_CH_W - 1);
    int sy = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_CH_H - 1);
    vec2 sv = texelFetch(src_uv, ivec2(sx, sy), 0).xy;
    td_ch_s0[ty][tx] = vec2(td_is_bad(sv.x) ? 0.0 : sv.x, td_is_bad(sv.y) ? 0.0 : sv.y);
}
memoryBarrierShared();
barrier();

int hN = tileDim * 16;
for (int idx = tid; idx < hN; idx += 256)
{
    int ty = idx / 16;
    int tx = idx - ty * 16;
    int c = tx + TD_RAD_BASE;
    vec2 accFine = vec2(0.0);
    vec2 accBase = vec2(0.0);
    for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
    {
        vec2 v = td_ch_s0[ty][c + k];
        accBase += TD_TAP_BASE[k + TD_RAD_BASE] * v;
        if (abs(k) <= TD_RAD_FINE)
            accFine += TD_TAP_FINE[k + TD_RAD_FINE] * v;
    }
    td_ch_s1[ty][tx] = vec4(accFine.x, accFine.y, accBase.x, accBase.y);
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
vec2 srcVal = td_ch_s0[center][lx + TD_RAD_BASE];
vec2 gFine = vec2(0.0);
vec2 gBase = vec2(0.0);
for (int k = -TD_RAD_BASE; k <= TD_RAD_BASE; ++k)
{
    vec4 tv = td_ch_s1[center + k][lx];
    gBase += TD_TAP_BASE[k + TD_RAD_BASE] * tv.zw;
    if (abs(k) <= TD_RAD_FINE)
        gFine += TD_TAP_FINE[k + TD_RAD_FINE] * tv.xy;
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

		// ---- PASS C2A: Coalesced 8x8 (64-thread) Parallel 6-Slice Chan-Pebay Reduction for Cb & Cr Simultaneously ----
		// Dispatch: TD_CW x TD_CH workgroups of 8x8 (1 WG per luma cell; 100% active on 4:2:0, 4-8 barriers vs ~180!).
		// Carbon LOW-2: Stride-65 padding (30 * 65 = 1950 floats, +120 B LDS) & slice stride 260 eliminates bank conflicts!
		constexpr const char* kPassChromaCellStatsHeader = R"GLSL(
shared float td_c2a_sh[1950];
shared float td_c2a_row[480];
shared uint td_c2a_m[64];
shared float td_c2a_acc[60];
shared float td_c2a_meta[8];

void td_ch_cp_merge(inout float nA, inout vec3 muA, inout vec4 sAA, inout vec2 sBA,
                    float nB, vec3 muB, vec4 sAB, vec2 sBB)
{
    if (nB <= 0.5) return;
    if (nA <= 0.5) { nA = nB; muA = muB; sAA = sAB; sBA = sBB; return; }
    float nNew = nA + nB;
    vec3 d = muB - muA;
    float w = (nA * nB) / nNew;
    muA += d * (nB / nNew);
    sAA += sAB + vec4(d.x * d.x, d.x * d.y, d.x * d.z, d.y * d.y) * w;
    sBA += sBB + vec2(d.y * d.z, d.z * d.z) * w;
    nA = nNew;
}
)GLSL";

		constexpr const char* kPassChromaCellStatsPart1 = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 8 + lx;

int K = int(round(u_k));
int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
bool hasCleanSplit = (u_has_clean_split > 0.5);
bool sameCleanRec = (u_same_clean_rec > 0.5);
bool bokehAllowL = (u_bokeh_mode > 0.5);
bool needAutoFocus = (u_has_explicit_focus < 0.5) && (u_coarse_only < 0.5) && (K > 0);
bool needProjIr = (u_has_clean_split < 0.5) && (u_coarse_only < 0.5);

int expFocusBits = int(round(texelFetch(cell_prior, cell, 0).y));
bool cellFocusChange = ((expFocusBits & 1) != 0);
bool cellIsDefocused = ((expFocusBits & 2) != 0);
bool cellIsFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);

int cellGateMaskL = 0;
int cellGateMaskMH = 0;
if (K > 0) { vec2 mg = texelFetch(motion_gate, ivec2(cell.x, cell.y), 0).xy; if (mg.x > 0.0) cellGateMaskL |= 1; if (mg.y > 0.0) cellGateMaskMH |= 1; }
if (K > 1) { vec2 mg = texelFetch(motion_gate, ivec2(TD_CW + cell.x, cell.y), 0).xy; if (mg.x > 0.0) cellGateMaskL |= 2; if (mg.y > 0.0) cellGateMaskMH |= 2; }
if (K > 2) { vec2 mg = texelFetch(motion_gate, ivec2(2 * TD_CW + cell.x, cell.y), 0).xy; if (mg.x > 0.0) cellGateMaskL |= 4; if (mg.y > 0.0) cellGateMaskMH |= 4; }

// Exact siting cell bounding box (8x8 on 4:2:0, 8x16 on 4:2:2, 16x16 on 4:4:4; exact for arbitrary siting & borders).
int cellStartX = (cell.x == 0) ? 0 : max(0, int(floor(td_luma_to_coded_x(float(cell.x * TD_CELL_PX)))));
while (cellStartX < TD_CH_W && td_coded_to_luma_cell(ivec2(cellStartX, 0)).x < cell.x) ++cellStartX;
int cellEndX = (cell.x == TD_CW - 1) ? TD_CH_W : min(TD_CH_W, int(ceil(td_luma_to_coded_x(float((cell.x + 1) * TD_CELL_PX)))) + 1);
while (cellEndX > cellStartX && td_coded_to_luma_cell(ivec2(cellEndX - 1, 0)).x > cell.x) --cellEndX;
int cellStartY = (cell.y == 0) ? 0 : max(0, int(floor(td_luma_to_coded_y(float(cell.y * TD_CELL_PX)))));
while (cellStartY < TD_CH_H && td_coded_to_luma_cell(ivec2(0, cellStartY)).y < cell.y) ++cellStartY;
int cellEndY = (cell.y == TD_CH - 1) ? TD_CH_H : min(TD_CH_H, int(ceil(td_luma_to_coded_y(float((cell.y + 1) * TD_CELL_PX)))) + 1);
while (cellEndY > cellStartY && td_coded_to_luma_cell(ivec2(0, cellEndY - 1)).y > cell.y) --cellEndY;

int cx0 = max(cax0, cellStartX);
int cy0 = max(cay0, cellStartY);
int cx1 = min(cax1, cellEndX);
int cy1 = min(cay1, cellEndY);
int cellW = max(0, cellEndX - cellStartX);
int cellH = max(0, cellEndY - cellStartY);
int subX = max(1, (cellW + 7) / 8);
int subY = max(1, (cellH + 7) / 8);
bool canCacheSingle = (subX == 1 && subY == 1);

float ogCb2 = 0.0, ogCr2 = 0.0, activeCnt = 0.0, borderCnt = 0.0;
uint badMask = 0u;
vec2 curMidE = vec2(0.0), curFineE = vec2(0.0), curLSum = vec2(0.0), curL2Sum = vec2(0.0);
vec2 refMidE0 = vec2(0.0), refMidE1 = vec2(0.0), refMidE2 = vec2(0.0);
vec2 refLSum0 = vec2(0.0), refLSum1 = vec2(0.0), refLSum2 = vec2(0.0);
vec2 refL2Sum0 = vec2(0.0), refL2Sum1 = vec2(0.0), refL2Sum2 = vec2(0.0);
float pHCnt = 0.0;
vec2 pCurH2 = vec2(0.0);
vec2 pRefH2_0 = vec2(0.0), pRefH2_1 = vec2(0.0), pRefH2_2 = vec2(0.0);
vec2 pCrossH_0 = vec2(0.0), pCrossH_1 = vec2(0.0), pCrossH_2 = vec2(0.0);

vec4 cb0 = vec4(0.0), cb1 = vec4(0.0), cb2 = vec4(0.0);
vec4 cr0 = vec4(0.0), cr1 = vec4(0.0), cr2 = vec4(0.0);

if (!canCacheSingle)
{
    if (tid < 60) td_c2a_acc[tid] = 0.0;
    memoryBarrierShared();
    barrier();
}

int fullMaskH = (K > 0) ? ((1 << K) - 1) : 0;
int anyGate = cellGateMaskL | cellGateMaskMH;

for (int sy = 0; sy < subY; ++sy)
{
    for (int sx = 0; sx < subX; ++sx)
    {
        if (!canCacheSingle)
        {
            cb0 = vec4(0.0); cb1 = vec4(0.0); cb2 = vec4(0.0);
            cr0 = vec4(0.0); cr1 = vec4(0.0); cr2 = vec4(0.0);
        }
        int oy = sy * 8 + ly;
        int ox = sx * 8 + lx;
        int py = cellStartY + oy;
        int px = cellStartX + ox;
        if (oy < cellH && ox < cellW && py >= cy0 && py < cy1 && px >= cx0 && px < cx1)
        {
            ivec2 p = ivec2(px, py);
            if (td_coded_to_luma_cell(p) == cell)
            {
                if (hasCleanSplit && !sameCleanRec)
                {
                    vec2 rv = texelFetch(rec_uv, p, 0).xy;
                    vec2 cv = texelFetch(cur_uv, p, 0).xy;
                    if (!td_is_bad(rv.x) && !td_is_bad(cv.x)) { float d = rv.x - cv.x; ogCb2 += d * d; }
                    if (!td_is_bad(rv.y) && !td_is_bad(cv.y)) { float d = rv.y - cv.y; ogCr2 += d * d; }
                }
                vec2 cLp = texelFetch(cur_l, p, 0).xy;
                vec4 cMHp = texelFetch(cur_mh, p, 0);
                if (!td_is_bad(cLp.x) && !td_is_bad(cLp.y)) { curLSum += cLp; curL2Sum += cLp * cLp; }
                if (!td_is_bad(cMHp.x) && !td_is_bad(cMHp.y)) curMidE += cMHp.xy * cMHp.xy;
                if (!td_is_bad(cMHp.z) && !td_is_bad(cMHp.w)) curFineE += cMHp.zw * cMHp.zw;
                activeCnt += 1.0;

                uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
                if ((packedFl & 512u) != 0u) badMask |= 16u;
                int refValidMask = 0;
                vec2 rL0 = vec2(0.0), rL1 = vec2(0.0), rL2 = vec2(0.0);
                vec4 rMH0 = vec4(0.0), rMH1 = vec4(0.0), rMH2 = vec4(0.0);
                if (K > 0)
                {
                    int fl = td_unpack_ref_flags(packedFl, 0);
                    if ((fl & 1) != 0)
                    {
                        refValidMask |= 1;
                        if (needAutoFocus || (cellGateMaskL & 1) != 0) rL0 = texelFetch(ref_l_0, p, 0).xy;
                        if (needAutoFocus || (cellGateMaskMH & 1) != 0) rMH0 = texelFetch(ref_mh_0, p, 0);
                        if (needAutoFocus)
                        {
                            if (!td_is_bad(rL0.x) && !td_is_bad(rL0.y)) { refLSum0 += rL0; refL2Sum0 += rL0 * rL0; }
                            if (!td_is_bad(rMH0.x) && !td_is_bad(rMH0.y)) refMidE0 += rMH0.xy * rMH0.xy;
                        }
                    }
                    if ((fl & 2) != 0) borderCnt += 1.0;
                    if ((fl & 4) != 0) badMask |= 1u;
                }
                if (K > 1)
                {
                    int fl = td_unpack_ref_flags(packedFl, 1);
                    if ((fl & 1) != 0)
                    {
                        refValidMask |= 2;
                        if (needAutoFocus || (cellGateMaskL & 2) != 0) rL1 = texelFetch(ref_l_1, p, 0).xy;
                        if (needAutoFocus || (cellGateMaskMH & 2) != 0) rMH1 = texelFetch(ref_mh_1, p, 0);
                        if (needAutoFocus)
                        {
                            if (!td_is_bad(rL1.x) && !td_is_bad(rL1.y)) { refLSum1 += rL1; refL2Sum1 += rL1 * rL1; }
                            if (!td_is_bad(rMH1.x) && !td_is_bad(rMH1.y)) refMidE1 += rMH1.xy * rMH1.xy;
                        }
                    }
                    if ((fl & 2) != 0) borderCnt += 1.0;
                    if ((fl & 4) != 0) badMask |= 2u;
                }
                if (K > 2)
                {
                    int fl = td_unpack_ref_flags(packedFl, 2);
                    if ((fl & 1) != 0)
                    {
                        refValidMask |= 4;
                        if (needAutoFocus || (cellGateMaskL & 4) != 0) rL2 = texelFetch(ref_l_2, p, 0).xy;
                        if (needAutoFocus || (cellGateMaskMH & 4) != 0) rMH2 = texelFetch(ref_mh_2, p, 0);
                        if (needAutoFocus)
                        {
                            if (!td_is_bad(rL2.x) && !td_is_bad(rL2.y)) { refLSum2 += rL2; refL2Sum2 += rL2 * rL2; }
                            if (!td_is_bad(rMH2.x) && !td_is_bad(rMH2.y)) refMidE2 += rMH2.xy * rMH2.xy;
                        }
                    }
                    if ((fl & 2) != 0) borderCnt += 1.0;
                    if ((fl & 4) != 0) badMask |= 4u;
                }

                if (cellGateMaskL != 0 && (refValidMask & cellGateMaskL) == cellGateMaskL)
                {
                    vec3 db = vec3(0.0), dr = vec3(0.0); bool ok = true;
                    if ((cellGateMaskL & 1) != 0) { vec2 d = cLp - rL0; if (td_is_bad(d.x) || td_is_bad(d.y)) ok = false; else { db.x = d.x; dr.x = d.y; } }
                    if (ok && (cellGateMaskL & 2) != 0) { vec2 d = cLp - rL1; if (td_is_bad(d.x) || td_is_bad(d.y)) ok = false; else { db.y = d.x; dr.y = d.y; } }
                    if (ok && (cellGateMaskL & 4) != 0) { vec2 d = cLp - rL2; if (td_is_bad(d.x) || td_is_bad(d.y)) ok = false; else { db.z = d.x; dr.z = d.y; } }
                    if (ok) { cb0 = vec4(db, 1.0); cr0 = vec4(dr, 1.0); }
                }
                if (cellGateMaskMH != 0 && (refValidMask & cellGateMaskMH) == cellGateMaskMH)
                {
                    vec3 dbM = vec3(0.0), drM = vec3(0.0); bool okM = true;
                    if ((cellGateMaskMH & 1) != 0) { vec2 d = cMHp.xy - rMH0.xy; if (td_is_bad(d.x) || td_is_bad(d.y)) okM = false; else { dbM.x = d.x; drM.x = d.y; } }
                    if (okM && (cellGateMaskMH & 2) != 0) { vec2 d = cMHp.xy - rMH1.xy; if (td_is_bad(d.x) || td_is_bad(d.y)) okM = false; else { dbM.y = d.x; drM.y = d.y; } }
                    if (okM && (cellGateMaskMH & 4) != 0) { vec2 d = cMHp.xy - rMH2.xy; if (td_is_bad(d.x) || td_is_bad(d.y)) okM = false; else { dbM.z = d.x; drM.z = d.y; } }
                    if (okM) { cb1 = vec4(dbM, 1.0); cr1 = vec4(drM, 1.0); }

                    vec3 dbH = vec3(0.0), drH = vec3(0.0); bool okH = true;
                    if ((cellGateMaskMH & 1) != 0) { vec2 d = cMHp.zw - rMH0.zw; if (td_is_bad(d.x) || td_is_bad(d.y)) okH = false; else { dbH.x = d.x; drH.x = d.y; } }
                    if (okH && (cellGateMaskMH & 2) != 0) { vec2 d = cMHp.zw - rMH1.zw; if (td_is_bad(d.x) || td_is_bad(d.y)) okH = false; else { dbH.y = d.x; drH.y = d.y; } }
                    if (okH && (cellGateMaskMH & 4) != 0) { vec2 d = cMHp.zw - rMH2.zw; if (td_is_bad(d.x) || td_is_bad(d.y)) okH = false; else { dbH.z = d.x; drH.z = d.y; } }
                    if (okH) { cb2 = vec4(dbH, 1.0); cr2 = vec4(drH, 1.0); }
                }

                if (needProjIr && fullMaskH != 0 && cellGateMaskMH == fullMaskH && (refValidMask & fullMaskH) == fullMaskH &&
                    !td_is_bad(cMHp.z) && !td_is_bad(cMHp.w))
                {
                    vec2 rh0 = rMH0.zw;
                    vec2 rh1 = rMH1.zw;
                    vec2 rh2 = rMH2.zw;
                    bool hFin = !td_is_bad(rh0.x) && !td_is_bad(rh0.y) &&
                                (K <= 1 || (!td_is_bad(rh1.x) && !td_is_bad(rh1.y))) &&
                                (K <= 2 || (!td_is_bad(rh2.x) && !td_is_bad(rh2.y)));
                    if (hFin)
                    {
                        pHCnt += 1.0;
                        pCurH2 += cMHp.zw * cMHp.zw;
                        pRefH2_0 += rh0 * rh0; pCrossH_0 += cMHp.zw * rh0;
                        if (K > 1) { pRefH2_1 += rh1 * rh1; pCrossH_1 += cMHp.zw * rh1; }
                        if (K > 2) { pRefH2_2 += rh2 * rh2; pCrossH_2 += cMHp.zw * rh2; }
                    }
                }
            }
        }
        if (!canCacheSingle)
        {
            td_c2a_sh[0 * 260 + 0 * 65 + tid] = cb0.x; td_c2a_sh[0 * 260 + 1 * 65 + tid] = cb0.y; td_c2a_sh[0 * 260 + 2 * 65 + tid] = cb0.z; td_c2a_sh[0 * 260 + 3 * 65 + tid] = cb0.w;
            td_c2a_sh[1 * 260 + 0 * 65 + tid] = cb1.x; td_c2a_sh[1 * 260 + 1 * 65 + tid] = cb1.y; td_c2a_sh[1 * 260 + 2 * 65 + tid] = cb1.z; td_c2a_sh[1 * 260 + 3 * 65 + tid] = cb1.w;
            td_c2a_sh[2 * 260 + 0 * 65 + tid] = cb2.x; td_c2a_sh[2 * 260 + 1 * 65 + tid] = cb2.y; td_c2a_sh[2 * 260 + 2 * 65 + tid] = cb2.z; td_c2a_sh[2 * 260 + 3 * 65 + tid] = cb2.w;
            td_c2a_sh[3 * 260 + 0 * 65 + tid] = cr0.x; td_c2a_sh[3 * 260 + 1 * 65 + tid] = cr0.y; td_c2a_sh[3 * 260 + 2 * 65 + tid] = cr0.z; td_c2a_sh[3 * 260 + 3 * 65 + tid] = cr0.w;
            td_c2a_sh[4 * 260 + 0 * 65 + tid] = cr1.x; td_c2a_sh[4 * 260 + 1 * 65 + tid] = cr1.y; td_c2a_sh[4 * 260 + 2 * 65 + tid] = cr1.z; td_c2a_sh[4 * 260 + 3 * 65 + tid] = cr1.w;
            td_c2a_sh[5 * 260 + 0 * 65 + tid] = cr2.x; td_c2a_sh[5 * 260 + 1 * 65 + tid] = cr2.y; td_c2a_sh[5 * 260 + 2 * 65 + tid] = cr2.z; td_c2a_sh[5 * 260 + 3 * 65 + tid] = cr2.w;
            memoryBarrierShared(); barrier();
            if (lx < 6)
            {
                int s = lx; int bo = s * 260 + ly * 8;
                float rN = 0.0; vec3 rMu = vec3(0.0); vec4 rSA = vec4(0.0); vec2 rSB = vec2(0.0);
                for (int i = 0; i < 8; ++i)
                {
                    if (td_c2a_sh[bo + 195 + i] > 0.5)
                    {
                        vec3 x = vec3(td_c2a_sh[bo + i], td_c2a_sh[bo + 65 + i], td_c2a_sh[bo + 130 + i]);
                        float n1 = rN + 1.0; vec3 d = x - rMu; float w = rN / n1; rMu += d / n1; vec3 dw = d * w;
                        rSA += vec4(d.x * dw.x, d.x * dw.y, d.x * dw.z, d.y * dw.y);
                        rSB += vec2(d.y * dw.z, d.z * dw.z);
                        rN = n1;
                    }
                }
                int ro = s * 80 + ly * 10;
                td_c2a_row[ro + 0] = rN; td_c2a_row[ro + 1] = rMu.x; td_c2a_row[ro + 2] = rMu.y; td_c2a_row[ro + 3] = rMu.z;
                td_c2a_row[ro + 4] = rSA.x; td_c2a_row[ro + 5] = rSA.y; td_c2a_row[ro + 6] = rSA.z; td_c2a_row[ro + 7] = rSA.w;
                td_c2a_row[ro + 8] = rSB.x; td_c2a_row[ro + 9] = rSB.y;
            }
            memoryBarrierShared(); barrier();
            if (ly == 0 && lx < 6)
            {
                int s = lx; int ao = s * 10;
                float tN = td_c2a_acc[ao + 0];
                vec3 tMu = vec3(td_c2a_acc[ao + 1], td_c2a_acc[ao + 2], td_c2a_acc[ao + 3]);
                vec4 tSA = vec4(td_c2a_acc[ao + 4], td_c2a_acc[ao + 5], td_c2a_acc[ao + 6], td_c2a_acc[ao + 7]);
                vec2 tSB = vec2(td_c2a_acc[ao + 8], td_c2a_acc[ao + 9]);
                for (int r = 0; r < 8; ++r)
                {
                    int ro = s * 80 + r * 10;
                    td_ch_cp_merge(tN, tMu, tSA, tSB,
                        td_c2a_row[ro + 0], vec3(td_c2a_row[ro + 1], td_c2a_row[ro + 2], td_c2a_row[ro + 3]),
                        vec4(td_c2a_row[ro + 4], td_c2a_row[ro + 5], td_c2a_row[ro + 6], td_c2a_row[ro + 7]),
                        vec2(td_c2a_row[ro + 8], td_c2a_row[ro + 9]));
                }
                td_c2a_acc[ao + 0] = tN; td_c2a_acc[ao + 1] = tMu.x; td_c2a_acc[ao + 2] = tMu.y; td_c2a_acc[ao + 3] = tMu.z;
                td_c2a_acc[ao + 4] = tSA.x; td_c2a_acc[ao + 5] = tSA.y; td_c2a_acc[ao + 6] = tSA.z; td_c2a_acc[ao + 7] = tSA.w;
                td_c2a_acc[ao + 8] = tSB.x; td_c2a_acc[ao + 9] = tSB.y;
            }
            memoryBarrierShared(); barrier();
        }
    }
}
)GLSL";

		constexpr const char* kPassChromaCellStatsPart2 = R"GLSL(
// Phase 1: Single 2-stage 8-way reduction for ALL Diag + CurL/MH + optional RefFocus (2 barriers; stride-65 bank-conflict-free!).
td_c2a_sh[0 * 65 + tid] = ogCb2;
td_c2a_sh[1 * 65 + tid] = ogCr2;
td_c2a_sh[2 * 65 + tid] = activeCnt;
td_c2a_sh[3 * 65 + tid] = borderCnt;
td_c2a_sh[4 * 65 + tid] = curMidE.x;
td_c2a_sh[5 * 65 + tid] = curMidE.y;
td_c2a_sh[6 * 65 + tid] = curFineE.x;
td_c2a_sh[7 * 65 + tid] = curFineE.y;
td_c2a_sh[8 * 65 + tid] = curLSum.x;
td_c2a_sh[9 * 65 + tid] = curLSum.y;
td_c2a_sh[10 * 65 + tid] = curL2Sum.x;
td_c2a_sh[11 * 65 + tid] = curL2Sum.y;
if (needAutoFocus)
{
    td_c2a_sh[12 * 65 + tid] = refLSum0.x;  td_c2a_sh[13 * 65 + tid] = refLSum0.y;
    td_c2a_sh[14 * 65 + tid] = refL2Sum0.x; td_c2a_sh[15 * 65 + tid] = refL2Sum0.y;
    td_c2a_sh[16 * 65 + tid] = refMidE0.x;  td_c2a_sh[17 * 65 + tid] = refMidE0.y;
    td_c2a_sh[18 * 65 + tid] = refLSum1.x;  td_c2a_sh[19 * 65 + tid] = refLSum1.y;
    td_c2a_sh[20 * 65 + tid] = refL2Sum1.x; td_c2a_sh[21 * 65 + tid] = refL2Sum1.y;
    td_c2a_sh[22 * 65 + tid] = refMidE1.x;  td_c2a_sh[23 * 65 + tid] = refMidE1.y;
    td_c2a_sh[24 * 65 + tid] = refLSum2.x;  td_c2a_sh[25 * 65 + tid] = refLSum2.y;
    td_c2a_sh[26 * 65 + tid] = refL2Sum2.x; td_c2a_sh[27 * 65 + tid] = refL2Sum2.y;
    td_c2a_sh[28 * 65 + tid] = refMidE2.x;  td_c2a_sh[29 * 65 + tid] = refMidE2.y;
}
td_c2a_m[tid] = badMask;
memoryBarrierShared();
barrier();

int nCols = needAutoFocus ? 30 : 12;
if (tid < nCols * 2)
{
    int s = tid % nCols;
    int halfIdx = tid / nCols;
    int bo = s * 65 + halfIdx * 32;
    float acc = 0.0;
    for (int i = 0; i < 32; ++i)
        acc += td_c2a_sh[bo + i];
    td_c2a_row[s * 2 + halfIdx] = acc;
}
if (tid >= 60 && tid < 62)
{
    int bo = (tid - 60) * 32;
    uint ma = 0u;
    for (int i = 0; i < 32; ++i) ma |= td_c2a_m[bo + i];
    td_c2a_m[bo] = ma;
}
memoryBarrierShared();
barrier();

// Overlap Phase 1 finalization (tid == 0 reading td_c2a_row[0..59]) with Phase 2 6-slice residual store into td_c2a_sh[0..1559]!
if (canCacheSingle)
{
    td_c2a_sh[0 * 260 + 0 * 65 + tid] = cb0.x; td_c2a_sh[0 * 260 + 1 * 65 + tid] = cb0.y; td_c2a_sh[0 * 260 + 2 * 65 + tid] = cb0.z; td_c2a_sh[0 * 260 + 3 * 65 + tid] = cb0.w;
    td_c2a_sh[1 * 260 + 0 * 65 + tid] = cb1.x; td_c2a_sh[1 * 260 + 1 * 65 + tid] = cb1.y; td_c2a_sh[1 * 260 + 2 * 65 + tid] = cb1.z; td_c2a_sh[1 * 260 + 3 * 65 + tid] = cb1.w;
    td_c2a_sh[2 * 260 + 0 * 65 + tid] = cb2.x; td_c2a_sh[2 * 260 + 1 * 65 + tid] = cb2.y; td_c2a_sh[2 * 260 + 2 * 65 + tid] = cb2.z; td_c2a_sh[2 * 260 + 3 * 65 + tid] = cb2.w;
    td_c2a_sh[3 * 260 + 0 * 65 + tid] = cr0.x; td_c2a_sh[3 * 260 + 1 * 65 + tid] = cr0.y; td_c2a_sh[3 * 260 + 2 * 65 + tid] = cr0.z; td_c2a_sh[3 * 260 + 3 * 65 + tid] = cr0.w;
    td_c2a_sh[4 * 260 + 0 * 65 + tid] = cr1.x; td_c2a_sh[4 * 260 + 1 * 65 + tid] = cr1.y; td_c2a_sh[4 * 260 + 2 * 65 + tid] = cr1.z; td_c2a_sh[4 * 260 + 3 * 65 + tid] = cr1.w;
    td_c2a_sh[5 * 260 + 0 * 65 + tid] = cr2.x; td_c2a_sh[5 * 260 + 1 * 65 + tid] = cr2.y; td_c2a_sh[5 * 260 + 2 * 65 + tid] = cr2.z; td_c2a_sh[5 * 260 + 3 * 65 + tid] = cr2.w;
}

if (tid == 0)
{
    float s0 = td_c2a_row[0] + td_c2a_row[1];
    float s1 = td_c2a_row[2] + td_c2a_row[3];
    float nAct = td_c2a_row[4] + td_c2a_row[5];
    float totBdr = td_c2a_row[6] + td_c2a_row[7];
    vec2 totCurM = vec2(td_c2a_row[8] + td_c2a_row[9], td_c2a_row[10] + td_c2a_row[11]);
    vec2 totCurH = vec2(td_c2a_row[12] + td_c2a_row[13], td_c2a_row[14] + td_c2a_row[15]);
    vec2 totCurL = vec2(td_c2a_row[16] + td_c2a_row[17], td_c2a_row[18] + td_c2a_row[19]);
    vec2 totCurL2 = vec2(td_c2a_row[20] + td_c2a_row[21], td_c2a_row[22] + td_c2a_row[23]);
    uint totBadMask = td_c2a_m[0] | td_c2a_m[32];

    bool autoFocusDet = false;
    if (needAutoFocus && !cellFocusChange && nAct > 0.5)
    {
        float invN = 1.0 / nAct;
        vec4 priorT = texelFetch(cell_prior, cell, 0);
        float rawSc = (u_cell_prior_has_step_scale > 0.5) ? priorT.z : 1.0;
        float cellScale = (!td_is_bad(rawSc) && rawSc > 0.0) ? clamp(rawSc, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX) : 1.0;
        float scSq = cellScale * cellScale;
        float stepNoiseVar = max(0.0, u_step_noise_var) * scSq;
        float structFloor = u_structure_floor * scSq;
        float quantMidBound = TD_PIXEL_BOUND_SIGMA * sqrt(stepNoiseVar) * TD_HM_PEAK;
        vec2 cM2v = totCurM * invN;
        vec2 cLmu = totCurL * invN;
        vec2 cE3v = max(vec2(0.0), totCurL2 * invN - cLmu * cLmu);

        for (int k = 0; k < K && !autoFocusDet; ++k)
        {
            int kb = (12 + k * 6) * 2;
            vec2 rLs = vec2(td_c2a_row[kb + 0] + td_c2a_row[kb + 1], td_c2a_row[kb + 2] + td_c2a_row[kb + 3]);
            vec2 rL2 = vec2(td_c2a_row[kb + 4] + td_c2a_row[kb + 5], td_c2a_row[kb + 6] + td_c2a_row[kb + 7]);
            vec2 rMs = vec2(td_c2a_row[kb + 8] + td_c2a_row[kb + 9], td_c2a_row[kb + 10] + td_c2a_row[kb + 11]);
            vec2 rM2v = rMs * invN;
            vec2 rLmu = rLs * invN;
            vec2 rE3v = max(vec2(0.0), rL2 * invN - rLmu * rLmu);
            for (int c = 0; c < 2; ++c)
            {
                float cM2 = (c == 0) ? cM2v.x : cM2v.y;
                float cE3 = (c == 0) ? cE3v.x : cE3v.y;
                float rM2 = (c == 0) ? rM2v.x : rM2v.y;
                float rE3 = (c == 0) ? rE3v.x : rE3v.y;
                float cEn = cM2 + cE3; float rEn = rM2 + rE3;
                float cF = (cEn > TD_NUM_FLOOR) ? (cM2 / (cEn + TD_NUM_FLOOR)) : 1.0;
                float rF = (rEn > TD_NUM_FLOOR) ? (rM2 / (rEn + TD_NUM_FLOOR)) : 1.0;
                float clCM2 = max(0.0, cM2 - stepNoiseVar * TD_GAIN_MID);
                float clCE3 = max(0.0, cE3 - stepNoiseVar * TD_GAIN_D3);
                float cFDeb = (clCM2 + clCE3 > TD_NUM_FLOOR) ? (clCM2 / (clCM2 + clCE3 + TD_NUM_FLOOR)) : cF;
                float clRM2 = max(0.0, rM2 - TD_BASE_QUANT_VAR * TD_GAIN_MID);
                float clRE3 = max(0.0, rE3 - TD_BASE_QUANT_VAR * TD_GAIN_D3);
                float rFDeb = (clRM2 + clRE3 > TD_NUM_FLOOR) ? (clRM2 / (clRM2 + clRE3 + TD_NUM_FLOOR)) : rF;
                if (rEn > structFloor && cM2 < rM2 && cFDeb < TD_F_SHARP &&
                    (rFDeb - cFDeb) > TD_F_SHIFT_DELTA && (sqrt(rM2) - sqrt(cM2)) > quantMidBound)
                {
                    autoFocusDet = true;
                    break;
                }
            }
        }
    }

    float invN = (nAct > 0.5) ? (1.0 / nAct) : 0.0;
    td_c2a_meta[0] = totCurM.x * invN;
    td_c2a_meta[1] = totCurM.y * invN;
    td_c2a_meta[2] = totCurH.x * invN;
    td_c2a_meta[3] = totCurH.y * invN;
    bool effFocus = cellFocusChange || autoFocusDet;
    td_c2a_meta[4] = effFocus ? 1.0 : 0.0;
    imageStore(cell_own_grain, cell, vec4(hasCleanSplit ? s0 : 0.0, hasCleanSplit ? s1 : 0.0, nAct, effFocus ? 1.0 : 0.0));
    uint dMask = (totBadMask & 7u) | (cellIsFace ? 8u : 0u) | (totBadMask & 16u);
    imageStore(warp_diag, cell, vec4(totBdr, float(dMask), 0.0, 0.0));
}
memoryBarrierShared();
barrier();
cellFocusChange = (td_c2a_meta[4] > 0.5);

// Phase 2: Parallel 6-slice (Cb & Cr x L/M/H) Chan-Pebay reduction (ONLY 2 barriers on 4:2:0!).
if (canCacheSingle)
{
    if (lx < 6)
    {
        int s = lx;
        int b = s % 3;
        bool skipMoments = cellIsFace || cellFocusChange || (cellIsDefocused && (b >= 1 || !bokehAllowL));
        float rN = 0.0; vec3 rMu = vec3(0.0); vec4 rSA = vec4(0.0); vec2 rSB = vec2(0.0);
        if (!skipMoments)
        {
            int bo = s * 260 + ly * 8;
            for (int i = 0; i < 8; ++i)
            {
                if (td_c2a_sh[bo + 195 + i] > 0.5)
                {
                    vec3 x = vec3(td_c2a_sh[bo + i], td_c2a_sh[bo + 65 + i], td_c2a_sh[bo + 130 + i]);
                    float n1 = rN + 1.0; vec3 d = x - rMu; float w = rN / n1; rMu += d / n1; vec3 dw = d * w;
                    rSA += vec4(d.x * dw.x, d.x * dw.y, d.x * dw.z, d.y * dw.y);
                    rSB += vec2(d.y * dw.z, d.z * dw.z);
                    rN = n1;
                }
            }
        }
        int ro = s * 80 + ly * 10;
        td_c2a_row[ro + 0] = rN; td_c2a_row[ro + 1] = rMu.x; td_c2a_row[ro + 2] = rMu.y; td_c2a_row[ro + 3] = rMu.z;
        td_c2a_row[ro + 4] = rSA.x; td_c2a_row[ro + 5] = rSA.y; td_c2a_row[ro + 6] = rSA.z; td_c2a_row[ro + 7] = rSA.w;
        td_c2a_row[ro + 8] = rSB.x; td_c2a_row[ro + 9] = rSB.y;
    }
    memoryBarrierShared();
    barrier();
}

if (ly == 0 && lx < 6)
{
    int s = lx;
    int chIdx = s / 3;
    int b = s - chIdx * 3;
    bool skipMoments = cellIsFace || cellFocusChange || (cellIsDefocused && (b >= 1 || !bokehAllowL));
    float tN = 0.0; vec3 tMu = vec3(0.0); vec4 tSA = vec4(0.0); vec2 tSB = vec2(0.0);
    if (!skipMoments)
    {
        if (canCacheSingle)
        {
            for (int r = 0; r < 8; ++r)
            {
                int ro = s * 80 + r * 10;
                td_ch_cp_merge(tN, tMu, tSA, tSB,
                    td_c2a_row[ro + 0], vec3(td_c2a_row[ro + 1], td_c2a_row[ro + 2], td_c2a_row[ro + 3]),
                    vec4(td_c2a_row[ro + 4], td_c2a_row[ro + 5], td_c2a_row[ro + 6], td_c2a_row[ro + 7]),
                    vec2(td_c2a_row[ro + 8], td_c2a_row[ro + 9]));
            }
        }
        else
        {
            int ao = s * 10;
            tN = td_c2a_acc[ao + 0];
            tMu = vec3(td_c2a_acc[ao + 1], td_c2a_acc[ao + 2], td_c2a_acc[ao + 3]);
            tSA = vec4(td_c2a_acc[ao + 4], td_c2a_acc[ao + 5], td_c2a_acc[ao + 6], td_c2a_acc[ao + 7]);
            tSB = vec2(td_c2a_acc[ao + 8], td_c2a_acc[ao + 9]);
        }
    }
    float bandVarZ = (b == 1) ? td_c2a_meta[chIdx] : ((b == 2) ? td_c2a_meta[2 + chIdx] : 0.0);
    float fMaskW = float((b == 0) ? cellGateMaskL : cellGateMaskMH);
    vec4 r0 = (skipMoments || tN <= 0.5) ? vec4(0.0) : vec4(tN, tMu.x, tMu.y, tMu.z);
    vec4 r1 = (skipMoments || tN <= 0.5) ? vec4(0.0) : tSA;
    vec4 r2 = (skipMoments || tN <= 0.5) ? vec4(0.0, 0.0, bandVarZ, fMaskW) : vec4(tSB.xy, bandVarZ, fMaskW);
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

// Phase 3: Unsplit H-band Projection-IR stats (0 barriers when skipped!).
bool skipProjIr = cellIsFace || cellFocusChange || cellIsDefocused || !needProjIr;
if (skipProjIr)
{
    if (tid == 0)
    {
        imageStore(proj_ir, ivec2(cell.x, cell.y), vec4(0.0));
        imageStore(proj_ir, ivec2(TD_CW + cell.x, cell.y), vec4(0.0));
        imageStore(proj_ir, ivec2(cell.x, TD_CH + cell.y), vec4(0.0));
        imageStore(proj_ir, ivec2(TD_CW + cell.x, TD_CH + cell.y), vec4(0.0));
    }
}
else
{
    memoryBarrierShared();
    barrier();
    td_c2a_sh[0 * 65 + tid] = pHCnt;
    td_c2a_sh[1 * 65 + tid] = pCurH2.x;   td_c2a_sh[2 * 65 + tid] = pCurH2.y;
    td_c2a_sh[3 * 65 + tid] = pRefH2_0.x; td_c2a_sh[4 * 65 + tid] = pRefH2_0.y;
    td_c2a_sh[5 * 65 + tid] = pCrossH_0.x; td_c2a_sh[6 * 65 + tid] = pCrossH_0.y;
    td_c2a_sh[7 * 65 + tid] = pRefH2_1.x; td_c2a_sh[8 * 65 + tid] = pRefH2_1.y;
    td_c2a_sh[9 * 65 + tid] = pCrossH_1.x; td_c2a_sh[10 * 65 + tid] = pCrossH_1.y;
    td_c2a_sh[11 * 65 + tid] = pRefH2_2.x; td_c2a_sh[12 * 65 + tid] = pRefH2_2.y;
    td_c2a_sh[13 * 65 + tid] = pCrossH_2.x; td_c2a_sh[14 * 65 + tid] = pCrossH_2.y;
    memoryBarrierShared();
    barrier();
    if (tid < 30)
    {
        int s = tid % 15;
        int halfIdx = tid / 15;
        int bo = s * 65 + halfIdx * 32;
        float acc = 0.0;
        for (int i = 0; i < 32; ++i) acc += td_c2a_sh[bo + i];
        td_c2a_row[s * 2 + halfIdx] = acc;
    }
    memoryBarrierShared();
    barrier();
    if (tid == 0)
    {
        float p0  = td_c2a_row[0]  + td_c2a_row[1];  float p1  = td_c2a_row[2]  + td_c2a_row[3];
        float p2  = td_c2a_row[4]  + td_c2a_row[5];  float p3  = td_c2a_row[6]  + td_c2a_row[7];
        float p4  = td_c2a_row[8]  + td_c2a_row[9];  float p5  = td_c2a_row[10] + td_c2a_row[11];
        float p6  = td_c2a_row[12] + td_c2a_row[13]; float p7  = td_c2a_row[14] + td_c2a_row[15];
        float p8  = td_c2a_row[16] + td_c2a_row[17]; float p9  = td_c2a_row[18] + td_c2a_row[19];
        float p10 = td_c2a_row[20] + td_c2a_row[21]; float p11 = td_c2a_row[22] + td_c2a_row[23];
        float p12 = td_c2a_row[24] + td_c2a_row[25]; float p13 = td_c2a_row[26] + td_c2a_row[27];
        float p14 = td_c2a_row[28] + td_c2a_row[29];
        imageStore(proj_ir, ivec2(cell.x, cell.y), vec4(p0, p1, p3, p5));
        imageStore(proj_ir, ivec2(TD_CW + cell.x, cell.y), vec4(p7, p9, p11, p13));
        imageStore(proj_ir, ivec2(cell.x, TD_CH + cell.y), vec4(p0, p2, p4, p6));
        imageStore(proj_ir, ivec2(TD_CW + cell.x, TD_CH + cell.y), vec4(p8, p10, p12, p14));
    }
}
)GLSL";

		inline std::string BuildPassChromaCellStatsBody()
		{
			return std::string(kPassChromaCellStatsPart1) + kPassChromaCellStatsPart2;
		}

		// ---- PROD-GPU-BUG-18: Exact Unsplit Chroma Projection-IR Helper (1:1 with CPU TdV2Chroma.h:3697-3722) ----
		constexpr const char* kChromaProjIrHelpers = R"GLSL(
float td_compute_projection_ir(float E, float C, float Eprev, float warpBeta)
{
    if (td_is_bad(E) || td_is_bad(C) || td_is_bad(Eprev) || td_is_bad(warpBeta) ||
        E < 0.0 || Eprev < 0.0 || warpBeta < 0.0)
        return 0.0;
    if (Eprev <= TD_NUM_FLOOR || C <= 0.0)
        return E;
    float det = E * Eprev - C * C;
    if (det <= 0.0)
        return 0.0;
    float bE = warpBeta * E;
    float diff = Eprev - bE;
    float disc = diff * diff + 4.0 * warpBeta * C * C;
    float denom = (Eprev + bE) + sqrt(disc);
    if (!(denom > TD_NUM_FLOOR * TD_NUM_FLOOR))
        return 0.0;
    float ir = (2.0 * det) / denom;
    return (ir > 0.0) ? ir : 0.0;
}

float td_chroma_unsplit_proj_ir(ivec2 cell, int chIdx, int K, int hPoolR)
{
    if (K <= 0 || TD_GAIN_FINE <= TD_NUM_FLOOR)
        return 0.0;
    float hCnt = 0.0;
    float sumCurH2 = 0.0;
    float sumRefH2[3]; sumRefH2[0] = 0.0; sumRefH2[1] = 0.0; sumRefH2[2] = 0.0;
    float sumCrossH[3]; sumCrossH[0] = 0.0; sumCrossH[1] = 0.0; sumCrossH[2] = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
    {
        if (abs(dy) > hPoolR) continue;
        int ncy = cell.y + dy;
        if (ncy < 0 || ncy >= TD_CH) continue;
        for (int dx = -2; dx <= 2; ++dx)
        {
            if (abs(dx) > hPoolR) continue;
            int ncx = cell.x + dx;
            if (ncx < 0 || ncx >= TD_CW) continue;
            vec4 a0 = texelFetch(proj_ir, ivec2(ncx, chIdx * TD_CH + ncy), 0);
            if (a0.x <= 0.5) continue;
            vec4 a1 = texelFetch(proj_ir, ivec2(TD_CW + ncx, chIdx * TD_CH + ncy), 0);
            hCnt += a0.x;
            sumCurH2 += a0.y;
            sumRefH2[0] += a0.z;
            sumCrossH[0] += a0.w;
            sumRefH2[1] += a1.x;
            sumCrossH[1] += a1.y;
            sumRefH2[2] += a1.z;
            sumCrossH[2] += a1.w;
        }
    }
    if (hCnt <= 0.5)
        return 0.0;
    float invH = 1.0 / hCnt;
    float meanCur = sumCurH2 * invH;
    float refQuantFloor = (1.0 / 12.0) * TD_GAIN_FINE;
    float sumProj = 0.0;
    float validRefCnt = 0.0;
    for (int k = 0; k < K; ++k)
    {
        float meanRef = sumRefH2[k] * invH;
        float cross = sumCrossH[k] * invH;
        int mSlot = clamp(td_p2b_motion_slot(k), 0, 2);
        vec2 mv = texelFetch(motion_vec, ivec2(mSlot * TD_CW + cell.x, cell.y), 0).xy;
        vec2 mvC = (td_is_bad(mv.x) || td_is_bad(mv.y))
            ? vec2(0.0)
            : vec2(mv.x / float(TD_CSUB_X), mv.y / float(TD_CSUB_Y));
        float warpBeta = td_fine_hp_warp_gain(mvC);
        if (meanRef > refQuantFloor)
        {
            sumProj += td_compute_projection_ir(meanCur, cross, meanRef, warpBeta);
            validRefCnt += 1.0;
        }
    }
    return (validRefCnt > 0.5) ? ((sumProj / validRefCnt) / TD_GAIN_FINE) : 0.0;
}
)GLSL";

		// ---- PASS C2EB-PREP (PROD-GPU-BUG-1 / PROD-GPU-BUG-4 / PROD-GPU-BUG-5 / PROD-GPU-BUG-18): Per-Cell Band-Pooled Chroma c_IV Prep ----
		// Dispatch: ceil(TD_CW / 8) x ceil(TD_CH / 8), group 8x8 (100% parallel across SMs, zero barriers, constant -2..2 loops).
		// Output: eb_scratch (3*cw x 2*ch RGBA32F): (b*TD_CW+cx, chIdx*TD_CH+cy) -> (clipC, rawC, vSample, valid ? (ac+1) : 0).
		constexpr const char* kPassChromaEbPriorPrep = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int K = int(round(u_k));
int hPoolR = int(round(u_pool_r_h));
bool bokehAllowL = (u_bokeh_mode > 0.5);
bool coarseOnly = (u_coarse_only > 0.5);
bool isFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);
vec4 pT = texelFetch(cell_prior, cell, 0);
int fBits = int(round(pT.y));
bool focusChange = ((fBits & 1) != 0) || (texelFetch(cell_own_grain, cell, 0).w > 0.5);
bool isDefoc = ((fBits & 2) != 0);

// PROD-GPU-BUG-19: Exclude invalid-step-scale / bad-motion-gate cells from EB/A2e prior prep;
// sanitize NaN/+Inf/negative retained grain to 0.0 without skipping the cell (1:1 with CPU TdV2ReferenceEngine.h:1866-1882).
float rawGvCb = pT.x;
float rawGvCr = (u_cell_prior_has_cr_grain > 0.5) ? pT.w : pT.x;
bool cellNonFin = false;
if (u_cell_prior_has_step_scale > 0.5)
{
    if (td_is_bad(pT.z) || !(pT.z > 0.0))
        cellNonFin = true;
}
for (int kk = 0; kk < K; ++kk)
{
    if (texelFetch(motion_gate, ivec2(kk * TD_CW + cell.x, cell.y), 0).z > 0.5)
        cellNonFin = true;
}

for (int chIdx = 0; chIdx < 2; ++chIdx)
{
    // PROD-GPU-BUG-18: Classify activity (flat vs textured) BEFORE unsplit Projection-IR grain estimation (TdV2Chroma.h:3637).
    float rawG = (chIdx == 0) ? rawGvCb : rawGvCr;
    float retG = (td_is_bad(rawG) || rawG < 0.0) ? 0.0 : rawG;
    float mVar = (chIdx == 0) ? max(0.0, texelFetch(cell_stats_cb, ivec2(TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z)
                              : max(0.0, texelFetch(cell_stats_cr, ivec2(TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z);
    float hVar = (chIdx == 0) ? max(0.0, texelFetch(cell_stats_cb, ivec2(2 * TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z)
                              : max(0.0, texelFetch(cell_stats_cr, ivec2(2 * TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z);
    float cleanTexMH = max(0.0, mVar - retG * TD_GAIN_MID * u_psd_mid) +
                       max(0.0, hVar - retG * TD_GAIN_FINE * u_psd_fine);
    int cellAc = (cleanTexMH > u_activity_tex_floor) ? 1 : 0;
    if (!cellNonFin && u_has_clean_split < 0.5 && retG <= TD_NUM_FLOOR && !coarseOnly && !isFace && !focusChange && !isDefoc)
        retG = td_chroma_unsplit_proj_ir(cell, chIdx, K, hPoolR);

    for (int b = 0; b < 3; ++b)
    {
        vec4 outRec = vec4(0.0);
        if (!cellNonFin && !isFace && !focusChange && !(isDefoc && (b >= 1 || !bokehAllowL)) && !(coarseOnly && b >= 1))
        {
            int centerMask = 0;
            for (int kk = 0; kk < K; ++kk)
            {
                vec2 mgK = texelFetch(motion_gate, ivec2(kk * TD_CW + cell.x, cell.y), 0).xy;
                float mGK = (b == 0) ? mgK.x : mgK.y;
                if (mGK > 0.0) centerMask |= (1 << kk);
            }
            if (centerMask != 0)
            {
                int bandPoolR = (b == 0) ? int(round(u_pool_r_l)) : ((b == 1) ? int(round(u_pool_r_m)) : int(round(u_pool_r_h)));
                float nPool = 0.0;
                vec3 muPool = vec3(0.0);
                float s00 = 0.0, s01 = 0.0, s02 = 0.0, s11 = 0.0, s12 = 0.0, s22 = 0.0;
                for (int dy = -2; dy <= 2; ++dy)
                {
                    if (abs(dy) > bandPoolR) continue;
                    int ncy = cell.y + dy;
                    if (ncy < 0 || ncy >= TD_CH) continue;
                    for (int dx = -2; dx <= 2; ++dx)
                    {
                        if (abs(dx) > bandPoolR) continue;
                        int ncx = cell.x + dx;
                        if (ncx < 0 || ncx >= TD_CW) continue;
                        vec4 t0 = (chIdx == 0) ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, ncy), 0)
                                               : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, ncy), 0);
                        float nc = t0.x;
                        if (nc <= 0.5) continue;
                        vec4 t2full = (chIdx == 0) ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0)
                                                   : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0);
                        int nbrMask = int(round(t2full.w));
                        if ((nbrMask & centerMask) != centerMask) continue;
                        vec3 muc = t0.yzw;
                        vec4 t1 = (chIdx == 0) ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0)
                                               : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
                        vec2 t2 = t2full.xy;
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
                    float bGain = td_band_gain(b);
                    float psdScale = td_psd_scale(b);
                    for (int k = 0; k < K; ++k)
                    {
                        if (td_is_exact_copy(k) || (centerMask & (1 << k)) == 0) continue;
                        int partner = td_find_independent_a1_partner_masked(K, k, centerMask);
                        if (partner < 0) continue;
                        if (td_refs_collinear(C[k][k], C[partner][partner], C[k][partner], bGain)) continue;
                        float rawC = C[k][partner] - retG * bGain * psdScale;
                        float clipC = clamp(rawC, 0.0, max(0.0, min(C[k][k], C[partner][partner])));
                        float nEff = td_effective_samples_for_band(b, nPool);
                        float vSample = td_sampling_var_cross_moment(C[k][k], C[partner][partner], C[k][partner], nEff);
                        if (!td_is_bad(rawC) && !td_is_bad(vSample))
                            outRec = vec4(clipC, rawC, vSample, float(cellAc + 1));
                        break;
                    }
                }
            }
        }
        imageStore(eb_scratch, ivec2(b * TD_CW + cell.x, chIdx * TD_CH + cell.y), outRec);
    }
}
)GLSL";

		// ---- PASS C2EB-REDUCE (OPT-4): Parallel (6 x 2) 1-Slice-Per-WG 4-Pass 8-Bit IEEE-754 Radix Exact Median + tau^2 ----
		// Dispatch: 6 x 2 x 1 workgroups of 16x16 threads (slot = gl_WorkGroupID.x = b*2+ac, chIdx = gl_WorkGroupID.y).
		// Output: eb_prior (6 x 2 RGBA32F): x = b*2 + ac, y = chIdx (0=Cb, 1=Cr); .x = U_j, .y = tau^2, .z = validCnt, .w = 0.0.
		constexpr const char* kPassChromaEbPriorHeader = R"GLSL(
#define TD_EB_TILES ((TD_CW * TD_CH + 255) / 256)
shared uint td_ceb_hist[256];
shared float td_ceb_r0[256];
shared float td_ceb_r1[256];
shared float td_ceb_r2[256];
shared uint td_ceb_u0[256];
shared uint td_ceb_u1[256];
shared uint td_ceb_cfg[8];
shared float td_ceb_mean_raw;
shared float td_ceb_tau_out;
shared float td_ceb_p1cnt;
shared float td_ceb_sum_se;
)GLSL";

		constexpr const char* kPassChromaEbPriorReduce = R"GLSL(
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 16 + lx;
int slot = int(gl_WorkGroupID.x);
int chIdx = int(gl_WorkGroupID.y);
int b = slot / 2;
int ac = slot & 1;
int targetTag = ac + 1;
int totalCells = TD_CW * TD_CH;

td_ceb_hist[tid] = 0u;
memoryBarrierShared();
barrier();

float locSumRaw = 0.0;
float locSumSe = 0.0;
float locCnt = 0.0;
for (int tile = 0; tile < TD_EB_TILES; ++tile)
{
    int idx = tile * 256 + tid;
    if (idx < totalCells)
    {
        int cx = idx % TD_CW;
        int cy = idx / TD_CW;
        vec4 ev = texelFetch(eb_scratch, ivec2(b * TD_CW + cx, chIdx * TD_CH + cy), 0);
        if (int(round(ev.w)) == targetTag)
        {
            locSumRaw += ev.y;
            locSumSe += ev.z;
            locCnt += 1.0;
            uint bits = floatBitsToUint(max(0.0, ev.x)) & 0x7FFFFFFFu;
            atomicAdd(td_ceb_hist[bits >> 24u], 1u);
        }
    }
}
td_ceb_r0[tid] = locSumRaw;
td_ceb_r1[tid] = locSumSe;
td_ceb_r2[tid] = locCnt;
memoryBarrierShared();
barrier();

if (lx == 0)
{
    int ro = ly * 16;
    float s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < 16; ++i) { s0 += td_ceb_r0[ro + i]; s1 += td_ceb_r1[ro + i]; s2 += td_ceb_r2[ro + i]; }
    td_ceb_r0[ro] = s0; td_ceb_r1[ro] = s1; td_ceb_r2[ro] = s2;
}
memoryBarrierShared();
barrier();

if (tid == 0)
{
    float totRaw = 0.0, totSe = 0.0, p1Cnt = 0.0;
    for (int r = 0; r < 16; ++r) { totRaw += td_ceb_r0[r * 16]; totSe += td_ceb_r1[r * 16]; p1Cnt += td_ceb_r2[r * 16]; }
    uint nU = uint(max(0, int(round(p1Cnt))));
    td_ceb_p1cnt = p1Cnt;
    td_ceb_sum_se = totSe;
    td_ceb_mean_raw = (p1Cnt > 0.5) ? (totRaw / p1Cnt) : 0.0;
    td_ceb_tau_out = 0.0;
    td_ceb_cfg[5] = nU;
    td_ceb_cfg[6] = 0u;
    td_ceb_cfg[7] = 0u;
    if (nU == 0u)
    {
        td_ceb_cfg[0] = 0u; td_ceb_cfg[1] = 0u; td_ceb_cfg[2] = 0u; td_ceb_cfg[3] = 0u; td_ceb_cfg[4] = 2u;
    }
    else
    {
        uint kLo = (nU - 1u) / 2u;
        uint kHi = nU / 2u;
        uint acc = 0u;
        uint bLo = 0u, bHi = 0u, rLo = 0u, rHi = 0u;
        bool fLo = false, fHi = false;
        for (uint i = 0u; i < 256u; ++i)
        {
            uint h = td_ceb_hist[i];
            if (!fLo && acc + h > kLo) { bLo = i; rLo = kLo - acc; fLo = true; }
            if (!fHi && acc + h > kHi) { bHi = i; rHi = kHi - acc; fHi = true; break; }
            acc += h;
        }
        td_ceb_cfg[0] = bLo << 24u;
        td_ceb_cfg[1] = bHi << 24u;
        td_ceb_cfg[2] = rLo;
        td_ceb_cfg[3] = rHi;
        td_ceb_cfg[4] = (bLo == bHi) ? 0u : 1u;
    }
}
memoryBarrierShared();
barrier();

float meanRaw = td_ceb_mean_raw;
for (int pass = 1; pass <= 3; ++pass)
{
    uint state = td_ceb_cfg[4];
    bool needTau = (pass == 1) && (td_ceb_cfg[5] >= 2u);
    // Carbon LOW-3: Keep all barriers unconditionally top-level (matching Luma kPass2EbPriorReduce, zero break inside barrier loop!).
    bool needScan = needTau || (state != 2u);
    uint shift = uint((3 - pass) * 8);
    uint mask = 0xFFFFFFFFu << (shift + 8u);
    uint prefLo = td_ceb_cfg[0];
    uint prefHi = td_ceb_cfg[1];

    if (state == 0u)
        td_ceb_hist[tid] = 0u;
    memoryBarrierShared();
    barrier();

    float locM2 = 0.0;
    uint locMaxLo = 0u;
    uint locMinHi = 0xFFFFFFFFu;
    if (needScan)
    {
        for (int tile = 0; tile < TD_EB_TILES; ++tile)
        {
            int idx = tile * 256 + tid;
            if (idx < totalCells)
            {
                int cx = idx % TD_CW;
                int cy = idx / TD_CW;
                vec4 ev = texelFetch(eb_scratch, ivec2(b * TD_CW + cx, chIdx * TD_CH + cy), 0);
                if (int(round(ev.w)) == targetTag)
                {
                    if (needTau) { float dr = ev.y - meanRaw; locM2 += dr * dr; }
                    if (state == 0u)
                    {
                        uint bits = floatBitsToUint(max(0.0, ev.x)) & 0x7FFFFFFFu;
                        if ((bits & mask) == prefLo)
                            atomicAdd(td_ceb_hist[(bits >> shift) & 255u], 1u);
                    }
                    else if (state == 1u)
                    {
                        uint bits = floatBitsToUint(max(0.0, ev.x)) & 0x7FFFFFFFu;
                        if ((bits & mask) == prefLo) locMaxLo = max(locMaxLo, bits);
                        if ((bits & mask) == prefHi) locMinHi = min(locMinHi, bits);
                    }
                }
            }
        }
    }
    if (needTau) td_ceb_r0[tid] = locM2;
    if (state == 1u) { td_ceb_u0[tid] = locMaxLo; td_ceb_u1[tid] = locMinHi; }
    memoryBarrierShared();
    barrier();

    if ((needTau || state == 1u) && lx == 0)
    {
        int ro = ly * 16;
        if (needTau)
        {
            float sm = 0.0;
            for (int i = 0; i < 16; ++i) sm += td_ceb_r0[ro + i];
            td_ceb_r0[ro] = sm;
        }
        if (state == 1u)
        {
            uint mx = 0u, mn = 0xFFFFFFFFu;
            for (int i = 0; i < 16; ++i) { mx = max(mx, td_ceb_u0[ro + i]); mn = min(mn, td_ceb_u1[ro + i]); }
            td_ceb_u0[ro] = mx; td_ceb_u1[ro] = mn;
        }
    }
    memoryBarrierShared();
    barrier();

    if (tid == 0)
    {
        if (needTau)
        {
            float totM2 = 0.0;
            for (int r = 0; r < 16; ++r) totM2 += td_ceb_r0[r * 16];
            float p1Cnt = td_ceb_p1cnt;
            float varRaw = max(0.0, totM2 / (p1Cnt - 1.0));
            float meanSe = td_ceb_sum_se / p1Cnt;
            td_ceb_tau_out = max(0.0, varRaw - meanSe);
        }
        if (state == 1u)
        {
            uint gMax = 0u, gMin = 0xFFFFFFFFu;
            for (int r = 0; r < 16; ++r) { gMax = max(gMax, td_ceb_u0[r * 16]); gMin = min(gMin, td_ceb_u1[r * 16]); }
            td_ceb_cfg[6] = gMax; td_ceb_cfg[7] = gMin; td_ceb_cfg[4] = 2u;
        }
        else if (state == 0u)
        {
            uint kLo = td_ceb_cfg[2]; uint kHi = td_ceb_cfg[3];
            uint acc = 0u, bLo = 0u, bHi = 0u, rLo = 0u, rHi = 0u;
            bool fLo = false, fHi = false;
            for (uint i = 0u; i < 256u; ++i)
            {
                uint h = td_ceb_hist[i];
                if (!fLo && acc + h > kLo) { bLo = i; rLo = kLo - acc; fLo = true; }
                if (!fHi && acc + h > kHi) { bHi = i; rHi = kHi - acc; fHi = true; break; }
                acc += h;
            }
            uint newLo = prefLo | (bLo << shift);
            uint newHi = prefLo | (bHi << shift);
            td_ceb_cfg[0] = newLo; td_ceb_cfg[1] = newHi;
            td_ceb_cfg[2] = rLo; td_ceb_cfg[3] = rHi;
            if (pass == 3) { td_ceb_cfg[6] = newLo; td_ceb_cfg[7] = newHi; td_ceb_cfg[4] = 2u; }
            else if (bLo != bHi) { td_ceb_cfg[4] = 1u; }
        }
    }
    memoryBarrierShared();
    barrier();
}

if (tid == 0)
{
    float outU = (td_ceb_cfg[5] >= 1u)
        ? (0.5 * (uintBitsToFloat(td_ceb_cfg[6]) + uintBitsToFloat(td_ceb_cfg[7])))
        : 0.0;
    imageStore(eb_prior, ivec2(slot, chIdx), vec4(max(0.0, outU), td_ceb_tau_out, td_ceb_p1cnt, 0.0));
}
)GLSL";

		// ---- PASS C2B: Chan-Pebay Merge + Blind A1/A2/A2e/A3 + EB Shrinkage + Active-Set Solve (Cb & Cr) ----
		// PROD-GPU-PERF-16: Parallelize chIdx across gl_GlobalInvocationID.y (2*TD_CH rows) so FXC cs_5_0
		// compiles 1 channel loop (3 bands) instead of 2x3 = 6 unrolled Cholesky solvers (~15x faster FXC compile!).
		constexpr const char* kPassChromaMergeSolvePart1 = R"GLSL(
int chIdx = int(gl_GlobalInvocationID.y) / TD_CH;
ivec2 cell = ivec2(int(gl_GlobalInvocationID.x), int(gl_GlobalInvocationID.y) % TD_CH);
if (chIdx >= 2 || cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int K = int(round(u_k));
int hPoolR = int(round(u_pool_r_h));
int estMode = int(round(u_estimator));
bool bokehAllowL = (u_bokeh_mode > 0.5);
bool enableEb = (u_enable_eb > 0.5);
bool useA2ePrior = (u_use_a2e_prior > 0.5);
bool coarseOnly = (u_coarse_only > 0.5);

vec4 priorTexel = texelFetch(cell_prior, cell, 0);
int expFocusBits = int(round(priorTexel.y));
bool focusChange = ((expFocusBits & 1) != 0) || (texelFetch(cell_own_grain, cell, 0).w > 0.5);
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

// CHR-LOW-3: Pool own-grain variance over hPoolR (matching CPU TdV2Chroma.h hPoolR!).
vec2 ogSumPool = vec2(0.0);
float ogCntPool = 0.0;
for (int dy = -hPoolR; dy <= hPoolR; ++dy)
{
    int ncy = cell.y + dy;
    if (ncy < 0 || ncy >= TD_CH) continue;
    for (int dx = -hPoolR; dx <= hPoolR; ++dx)
    {
        int ncx = cell.x + dx;
        if (ncx < 0 || ncx >= TD_CW) continue;
        vec3 ogT = texelFetch(cell_own_grain, ivec2(ncx, ncy), 0).xyz;
        ogSumPool += ogT.xy;
        ogCntPool += ogT.z;
    }
}
vec2 ownGrainVar = (ogCntPool > 0.5) ? max(vec2(0.0), ogSumPool / ogCntPool) : vec2(0.0);
)GLSL";

		constexpr const char* kPassChromaMergeSolvePart2 = R"GLSL(
{
    vec4 bandW[3];
    bandW[0] = vec4(0.0); bandW[1] = vec4(0.0); bandW[2] = vec4(0.0);
    // PROD-GPU-BUG-18: Classify activity (flat vs textured) BEFORE unsplit Projection-IR grain estimation (TdV2Chroma.h:3637).
    float chRetGVar = retGVar[chIdx];
    float chOwnGVar = (chIdx == 0) ? ownGrainVar.x : ownGrainVar.y;
    float mVarCell = (chIdx == 0) ? max(0.0, texelFetch(cell_stats_cb, ivec2(TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z)
                                  : max(0.0, texelFetch(cell_stats_cr, ivec2(TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z);
    float hVarCell = (chIdx == 0) ? max(0.0, texelFetch(cell_stats_cb, ivec2(2 * TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z)
                                  : max(0.0, texelFetch(cell_stats_cr, ivec2(2 * TD_CW + cell.x, 2 * TD_CH + cell.y), 0).z);
    float cleanTexMH = max(0.0, mVarCell - chRetGVar * TD_GAIN_MID * u_psd_mid) +
                       max(0.0, hVarCell - chRetGVar * TD_GAIN_FINE * u_psd_fine);
    int cellAc = (cleanTexMH > u_activity_tex_floor) ? 1 : 0;
    if (u_has_clean_split < 0.5 && chRetGVar <= TD_NUM_FLOOR && !coarseOnly && !isFace && !focusChange && !isDefocused)
        chRetGVar = td_chroma_unsplit_proj_ir(cell, chIdx, K, hPoolR);

    for (int b = 0; b < 3; ++b)
    {
        int centerMask = 0;
        for (int kk = 0; kk < K; ++kk)
        {
            vec2 mgK = texelFetch(motion_gate, ivec2(kk * TD_CW + cell.x, cell.y), 0).xy;
            float mGK = (b == 0) ? mgK.x : mgK.y;
            if (mGK > 0.0) centerMask |= (1 << kk);
        }
        int bandPoolR = (b == 0) ? int(round(u_pool_r_l)) : ((b == 1) ? int(round(u_pool_r_m)) : int(round(u_pool_r_h)));
        float nPool = 0.0;
        vec3 muPool = vec3(0.0);
        float s00 = 0.0, s01 = 0.0, s02 = 0.0, s11 = 0.0, s12 = 0.0, s22 = 0.0;
        bool hasOwnStats = false;
        vec3 ownDiag = vec3(0.0);

        if (centerMask != 0)
        {
            for (int dy = -2; dy <= 2; ++dy)
            {
                if (abs(dy) > bandPoolR) continue;
                int ncy = cell.y + dy;
                if (ncy < 0 || ncy >= TD_CH) continue;
                for (int dx = -2; dx <= 2; ++dx)
                {
                    if (abs(dx) > bandPoolR) continue;
                    int ncx = cell.x + dx;
                    if (ncx < 0 || ncx >= TD_CW) continue;
                    vec4 t0 = (chIdx == 0)
                        ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, ncy), 0)
                        : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, ncy), 0);
                    float nc = t0.x;
                    if (nc <= 0.5) continue;
                    vec4 t2full = (chIdx == 0)
                        ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0)
                        : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, 2 * TD_CH + ncy), 0);
                    int nbrMask = int(round(t2full.w));
                    if ((nbrMask & centerMask) != centerMask) continue;
                    vec3 muc = t0.yzw;
                    vec4 t1 = (chIdx == 0)
                        ? texelFetch(cell_stats_cb, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0)
                        : texelFetch(cell_stats_cr, ivec2(b * TD_CW + ncx, TD_CH + ncy), 0);
                    vec2 t2 = t2full.xy;
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
        }

        vec4 ebT = texelFetch(eb_prior, ivec2(b * 2 + cellAc, chIdx), 0);
        float ebPriorU = max(0.0, ebT.x);
        float ebTauSq = max(0.0, ebT.y);
        bool ebHasA2e = (ebT.z > 0.5) && (ebPriorU >= 0.0);
        bool ebValid = enableEb && !coarseOnly && (ebT.z > 0.5) && (ebPriorU > 0.0);
        float bGain = td_band_gain(b);
        float a2BandTransfer = (coarseOnly && b == 0) ? 1.0 : bGain;
        float psdScale = td_psd_scale(b);
        float blockOwnU = (useA2ePrior && !coarseOnly && ebHasA2e)
            ? (ebPriorU * cellStepScaleSq)
            : ((u_a2_valid > 0.5) ? (u_a2_total_var * cellStepScaleSq * a2BandTransfer) : 0.0);

        vec4 wOut = vec4(0.0);
        vec4 diagOut = vec4(0.0, 0.0, 0.0, blockOwnU);
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

            // PROD-GPU-BUG-6A & 6B: Always compute cVec for per-cell blockOwnU = sumC / nC even when
            // u_a2_valid > 0.5 or u_reuse_luma_weights > 0.5, matching CPU TdV2Chroma.h:2652 & 3990 1:1!
            float nEff = td_effective_samples_for_band(b, nPool);
            float cVec[3];
            cVec[0] = 0.0; cVec[1] = 0.0; cVec[2] = 0.0;
            for (int k = 0; k < K; ++k)
            {
                bool exactCopy = td_is_exact_copy(k);
                float cA2Step = (u_a2_valid > 0.5)
                    ? ((u_a2_sigma_sent + (exactCopy ? 0.0 : u_a2_sigma_zero)) * cellStepScaleSq * a2BandTransfer)
                    : 0.0;
                int partner = td_find_independent_a1_partner_masked(K, k, centerMask);
                if (partner >= 0 && td_refs_collinear(C[k][k], C[partner][partner], C[k][partner], bGain))
                    partner = -1;
                float cA2 = (useA2ePrior && !coarseOnly && ebHasA2e)
                    ? (exactCopy ? 0.0 : (((partner >= 0) ? (ebPriorU + 3.0 * sqrt(ebTauSq)) : ebPriorU) * cellStepScaleSq))
                    : cA2Step;
                cA2 = min(cA2, C[k][k]);
                float cA1 = cA2;
                if (partner >= 0)
                {
                    float rawA1 = td_est_c_a1(C[k][partner], C[k][k], C[partner][partner], chRetGVar, bGain, psdScale, 0.0);
                    if (ebValid && !exactCopy)
                    {
                        float gTerm = max(0.0, chRetGVar) * max(0.0, bGain) * max(0.0, psdScale);
                        float cRaw = C[k][partner] - gTerm;
                        float seSq = td_sampling_var_cross_moment(C[k][k], C[partner][partner], C[k][partner], nEff);
                        cA1 = td_eb_shrink_c(cRaw, seSq, ebPriorU, ebTauSq, min(C[k][k], C[partner][partner]));
                    }
                    else
                    {
                        cA1 = rawA1;
                    }
                }
                else
                {
                    if (u_reuse_luma_weights < 0.5 && estMode != 1)
                        cellSingleRefA2 = true;
                    for (int j = 0; j < K; ++j)
                    {
                        if (j == k || td_is_exact_copy(j)) continue;
                        if (td_refs_collinear(C[k][k], C[j][j], C[k][j], bGain)) continue;
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

            if (u_use_empirical_u > 0.5 && (coarseOnly || ebHasA2e))
            {
                float sumC = 0.0;
                int nC = 0;
                for (int k = 0; k < K; ++k)
                {
                    if (!td_is_exact_copy(k))
                    {
                        sumC += max(0.0, cVec[k]);
                        ++nC;
                    }
                }
                diagOut.w = (nC > 0) ? (sumC / float(nC)) : 0.0;
            }

            if (u_reuse_luma_weights < 0.5)
            {
                float solvedW[3];
                int status = td_solve_convex(K, C, cVec, int(round(u_solver_mode)), solvedW);
                if (status == 2) cellNonFinite = true;
                else if (status == 1) cellSingular = true;
                else
                {
                    bool bandVeto = focusChange || (isDefocused && (b >= 1 || !bokehAllowL)) ||
                                    (coarseOnly && b >= 1);
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
        }
        bandW[b] = wOut;
        // Carry auto rack-focus veto sentinel (-1.0) in cell_diag_cb.w so Pass C3 stays within D3D11 CS 5.0 16-sampler limit!
        if (focusChange)
            diagOut.w = -1.0;
        if (chIdx == 0) imageStore(cell_diag_cb, ivec2(b * TD_CW + cell.x, cell.y), diagOut);
        else            imageStore(cell_diag_cr, ivec2(b * TD_CW + cell.x, cell.y), diagOut);
    }
)GLSL";

		constexpr const char* kPassChromaMergeSolvePart3 = R"GLSL(
    if (u_reuse_luma_weights < 0.5 && !isFace && estMode != 1 && chRetGVar > TD_NUM_FLOOR)
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

    if (u_reuse_luma_weights < 0.5)
    {
        for (int b = 0; b < 3; ++b)
        {
            bool bandVeto = isFace || focusChange || (isDefocused && (b >= 1 || !bokehAllowL)) ||
                            (coarseOnly && b >= 1);
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
}

if (chIdx == 0)
{
    imageStore(cell_diag_flags, cell, vec4(
        (focusChange || isDefocused) ? 1.0 : 0.0,
        cellNonFinite ? 1.0 : 0.0,
        cellSingular ? 1.0 : 0.0,
        cellSingleRefA2 ? 1.0 : 0.0));
}
)GLSL";

		inline std::string BuildPassChromaMergeSolveBody()
		{
			return std::string(kPassChromaMergeSolvePart1) + kPassChromaMergeSolvePart2 + kPassChromaMergeSolvePart3;
		}

		// ---- PASS C3: Coded-Grid Chroma Blend + 3-Sigma Per-Corner Huber + Parseval Pixel Bound ----
		constexpr const char* kPassChromaBlendHeader = R"GLSL(
shared vec4 td_c3_cw_cb[48];
shared vec4 td_c3_cw_cr[48];
shared vec4 td_c3_cd_cb[48];
shared vec4 td_c3_cd_cr[48];
shared vec2 td_c3_mg[48];
shared vec4 td_c3_prior[16];
shared float td_c3_face[16];

float td_c3_accum_ref(bool refOk, int k, int wSlot, bool reuseLumaW, int ownActiveMask, float mGateOwn,
                      float curV, float refV, vec4 w00, vec4 w10, vec4 w01, vec4 w11,
                      vec4 d00, vec4 d10, vec4 d01, vec4 d11, float tx, float ty, bool enableHuber)
{
    if (!refOk || wSlot < 0)
        return 0.0;
    if (reuseLumaW)
    {
        if (mGateOwn <= 0.0) return 0.0;
    }
    else if ((ownActiveMask & (1 << k)) == 0)
    {
        return 0.0;
    }
    float diff = curV - refV;
    if (td_is_bad(diff))
        return 0.0;
    float t00 = td_vec_comp(w00, wSlot);
    float t10 = td_vec_comp(w10, wSlot);
    float t01 = td_vec_comp(w01, wSlot);
    float t11 = td_vec_comp(w11, wSlot);
    if (td_is_bad(t00) || td_is_bad(t10) || td_is_bad(t01) || td_is_bad(t11))
        return 0.0;
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
    return (wEff > 0.0) ? (-wEff * diff) : 0.0;
}
)GLSL";

		constexpr const char* kPassChromaBlendBound = R"GLSL(
ivec2 wg = ivec2(gl_WorkGroupID.xy);
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
int tid = loc.y * 16 + loc.x;
int baseX = wg.x * 16;
int baseY = wg.y * 16;
ivec2 p = ivec2(baseX + loc.x, baseY + loc.y);

int K = int(round(u_k));
bool reuseLumaW = (u_reuse_luma_weights > 0.5);

// Workgroup-uniform 4x4 luma cell grid LDS cache (replaces ~64 global cell fetches/px with 1 fetch/px!).
ivec2 c00 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX), min(TD_CH_H - 1, baseY)));
ivec2 c11 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX + 15), min(TD_CH_H - 1, baseY + 15)));
int minCx = max(0, c00.x - 1);
int maxCx = min(TD_CW - 1, c11.x + 1);
int minCy = max(0, c00.y - 1);
int maxCy = min(TD_CH - 1, c11.y + 1);
int spanX = maxCx - minCx + 1;
int spanY = maxCy - minCy + 1;
bool cellCacheOk = (spanX <= 4) && (spanY <= 4);
int cellCount = spanX * spanY;
if (cellCacheOk)
{
    if (tid < cellCount * 3)
    {
        int bIdx = tid / cellCount;
        int ci = tid - bIdx * cellCount;
        int dy = ci / spanX;
        int dx = ci - dy * spanX;
        int ncx = minCx + dx;
        int ncy = minCy + dy;
        int slot = bIdx * 16 + ci;
        td_c3_cw_cb[slot] = texelFetch(cell_weights_cb, ivec2(bIdx * TD_CW + ncx, ncy), 0);
        if (!reuseLumaW)
            td_c3_cw_cr[slot] = texelFetch(cell_weights_cr, ivec2(bIdx * TD_CW + ncx, ncy), 0);
        td_c3_cd_cb[slot] = texelFetch(cell_diag_cb, ivec2(bIdx * TD_CW + ncx, ncy), 0);
        td_c3_cd_cr[slot] = texelFetch(cell_diag_cr, ivec2(bIdx * TD_CW + ncx, ncy), 0);
        if (reuseLumaW && bIdx < K)
            td_c3_mg[slot] = texelFetch(motion_gate, ivec2(bIdx * TD_CW + ncx, ncy), 0).xy;
    }
    if (tid < cellCount)
    {
        int dy = tid / spanX;
        int dx = tid - dy * spanX;
        int ncx = minCx + dx;
        int ncy = minCy + dy;
        td_c3_prior[tid] = texelFetch(cell_prior, ivec2(ncx, ncy), 0);
        td_c3_face[tid] = (u_has_face_mask > 0.5) ? texelFetch(face_mask, ivec2(ncx, ncy), 0).x : 0.0;
    }
}
memoryBarrierShared();
barrier();

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

bool enableHuber = (u_enable_per_corner_huber > 0.5);
float lx = td_coded_to_luma_x(float(p.x));
float ly = td_coded_to_luma_y(float(p.y));
int cx = clamp(int(floor(lx)) / TD_CELL_PX, 0, TD_CW - 1);
int cy = clamp(int(floor(ly)) / TD_CELL_PX, 0, TD_CH - 1);

float gx = (lx + 0.5) / float(TD_CELL_PX) - 0.5;
float gy = (ly + 0.5) / float(TD_CELL_PX) - 0.5;
int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
int x1 = min(TD_CW - 1, x0 + 1);
int y1 = min(TD_CH - 1, y0 + 1);
float tx = clamp(gx - float(x0), 0.0, 1.0);
float ty = clamp(gy - float(y0), 0.0, 1.0);

bool useCache = cellCacheOk &&
                cx >= minCx && cx <= maxCx && cy >= minCy && cy <= maxCy &&
                x0 >= minCx && x1 <= maxCx && y0 >= minCy && y1 <= maxCy;
int ciOwn = useCache ? ((cy - minCy) * spanX + (cx - minCx)) : 0;
int ci00  = useCache ? ((y0 - minCy) * spanX + (x0 - minCx)) : 0;
int ci10  = useCache ? ((y0 - minCy) * spanX + (x1 - minCx)) : 0;
int ci01  = useCache ? ((y1 - minCy) * spanX + (x0 - minCx)) : 0;
int ci11  = useCache ? ((y1 - minCy) * spanX + (x1 - minCx)) : 0;

float faceVal = useCache ? td_c3_face[ciOwn] : ((u_has_face_mask > 0.5) ? texelFetch(face_mask, ivec2(cx, cy), 0).x : 0.0);
if (u_has_face_mask > 0.5 && faceVal > 0.5)
{
    imageStore(coded_delta_raw, p, vec4(0.0));
    return;
}

vec4 priorT = useCache ? td_c3_prior[ciOwn] : texelFetch(cell_prior, ivec2(cx, cy), 0);
float rawScale = (u_cell_prior_has_step_scale > 0.5) ? priorT.z : 1.0;
float cellStepScale = (td_is_bad(rawScale) || !(rawScale > 0.0)) ? 0.0 : clamp(rawScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX);
float cellStepScaleSq = cellStepScale * cellStepScale;
int expFocusBits = int(round(priorT.y));
float diagCb0W = useCache ? td_c3_cd_cb[ciOwn].w : texelFetch(cell_diag_cb, ivec2(cx, cy), 0).w;
bool cellFocusVeto = ((expFocusBits & 1) != 0) || (diagCb0W < -0.5);
bool cellDefoc = ((expFocusBits & 2) != 0);
bool bokehAllowL = (u_bokeh_mode > 0.5);

// Fetch cur and valid ref bands ONCE per pixel into pure VGPRs (eliminates 2x cur_mh/ref_mh fetches & local arrays!).
uint packedFl = uint(round(texelFetch(ref_flags, p, 0).x * 65535.0));
bool refOk0 = (K > 0) && ((td_unpack_ref_flags(packedFl, 0) & 1) != 0);
bool refOk1 = (K > 1) && ((td_unpack_ref_flags(packedFl, 1) & 1) != 0);
bool refOk2 = (K > 2) && ((td_unpack_ref_flags(packedFl, 2) & 1) != 0);

vec2 curL  = texelFetch(cur_l, p, 0).xy;
vec4 curMH = texelFetch(cur_mh, p, 0);
vec2 refL0 = refOk0 ? texelFetch(ref_l_0, p, 0).xy : vec2(0.0);
vec2 refL1 = refOk1 ? texelFetch(ref_l_1, p, 0).xy : vec2(0.0);
vec2 refL2 = refOk2 ? texelFetch(ref_l_2, p, 0).xy : vec2(0.0);
vec4 refMH0 = refOk0 ? texelFetch(ref_mh_0, p, 0) : vec4(0.0);
vec4 refMH1 = refOk1 ? texelFetch(ref_mh_1, p, 0) : vec4(0.0);
vec4 refMH2 = refOk2 ? texelFetch(ref_mh_2, p, 0) : vec4(0.0);

vec2 mgOwn0 = reuseLumaW ? (useCache ? td_c3_mg[0 * 16 + ciOwn] : texelFetch(motion_gate, ivec2(0 * TD_CW + cx, cy), 0).xy) : vec2(1.0);
vec2 mgOwn1 = (reuseLumaW && K > 1) ? (useCache ? td_c3_mg[1 * 16 + ciOwn] : texelFetch(motion_gate, ivec2(1 * TD_CW + cx, cy), 0).xy) : vec2(1.0);
vec2 mgOwn2 = (reuseLumaW && K > 2) ? (useCache ? td_c3_mg[2 * 16 + ciOwn] : texelFetch(motion_gate, ivec2(2 * TD_CW + cx, cy), 0).xy) : vec2(1.0);
int wSlot0 = reuseLumaW ? td_luma_slot(0) : 0;
int wSlot1 = reuseLumaW ? td_luma_slot(1) : 1;
int wSlot2 = reuseLumaW ? td_luma_slot(2) : 2;

vec2 totalDelta = vec2(0.0);
vec2 sumBandVar = vec2(0.0);
for (int b = 0; b < 3; ++b)
{
    if (reuseLumaW && (cellFocusVeto || (cellDefoc && (b >= 1 || !bokehAllowL))))
        continue;
    vec2 curB = (b == 0) ? curL : ((b == 1) ? curMH.xy : curMH.zw);
    vec2 r0B  = (b == 0) ? refL0 : ((b == 1) ? refMH0.xy : refMH0.zw);
    vec2 r1B  = (b == 0) ? refL1 : ((b == 1) ? refMH1.xy : refMH1.zw);
    vec2 r2B  = (b == 0) ? refL2 : ((b == 1) ? refMH2.xy : refMH2.zw);
    float mg0 = (b == 0) ? mgOwn0.x : mgOwn0.y;
    float mg1 = (b == 0) ? mgOwn1.x : mgOwn1.y;
    float mg2 = (b == 0) ? mgOwn2.x : mgOwn2.y;
    float fallbackBandVar = (u_a2_valid > 0.5) ? (u_a2_total_var * cellStepScaleSq * td_band_gain(b)) : 0.0;
    int bOff = b * 16;

    for (int chIdx = 0; chIdx < 2; ++chIdx)
    {
        bool useCbWeights = (chIdx == 0) || reuseLumaW;
        vec4 ownW, ownD, w00, w10, w01, w11, d00, d10, d01, d11;
        if (useCache)
        {
            ownW = useCbWeights ? td_c3_cw_cb[bOff + ciOwn] : td_c3_cw_cr[bOff + ciOwn];
            ownD = (chIdx == 0) ? td_c3_cd_cb[bOff + ciOwn] : td_c3_cd_cr[bOff + ciOwn];
            w00  = useCbWeights ? td_c3_cw_cb[bOff + ci00]  : td_c3_cw_cr[bOff + ci00];
            w10  = useCbWeights ? td_c3_cw_cb[bOff + ci10]  : td_c3_cw_cr[bOff + ci10];
            w01  = useCbWeights ? td_c3_cw_cb[bOff + ci01]  : td_c3_cw_cr[bOff + ci01];
            w11  = useCbWeights ? td_c3_cw_cb[bOff + ci11]  : td_c3_cw_cr[bOff + ci11];
            d00  = (chIdx == 0) ? td_c3_cd_cb[bOff + ci00]  : td_c3_cd_cr[bOff + ci00];
            d10  = (chIdx == 0) ? td_c3_cd_cb[bOff + ci10]  : td_c3_cd_cr[bOff + ci10];
            d01  = (chIdx == 0) ? td_c3_cd_cb[bOff + ci01]  : td_c3_cd_cr[bOff + ci01];
            d11  = (chIdx == 0) ? td_c3_cd_cb[bOff + ci11]  : td_c3_cd_cr[bOff + ci11];
        }
        else
        {
            ownW = useCbWeights ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + cx, cy), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + cx, cy), 0);
            ownD = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + cx, cy), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + cx, cy), 0);
            w00 = useCbWeights ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x0, y0), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x0, y0), 0);
            w10 = useCbWeights ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x1, y0), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x1, y0), 0);
            w01 = useCbWeights ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x0, y1), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x0, y1), 0);
            w11 = useCbWeights ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x1, y1), 0) : texelFetch(cell_weights_cr, ivec2(b * TD_CW + x1, y1), 0);
            d00 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x0, y0), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x0, y0), 0);
            d10 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x1, y0), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x1, y0), 0);
            d01 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x0, y1), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x0, y1), 0);
            d11 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x1, y1), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x1, y1), 0);
        }

        float ownU = max(0.0, ownD.w);
        float bandVar = (ownU > 0.0) ? ownU : fallbackBandVar;
        if (chIdx == 0) sumBandVar.x += bandVar;
        else            sumBandVar.y += bandVar;
        float bound = u_pixel_bound_kappa * sqrt(max(0.0, bandVar));

        float curV = (chIdx == 0) ? curB.x : curB.y;
        int ownActiveMask = int(round(ownW.w));
        bool allowBlend = true;
        if (reuseLumaW)
        {
            float ownBandSum = 0.0;
            if (K > 0 && wSlot0 >= 0) { float w = td_vec_comp(ownW, wSlot0); if (!td_is_bad(w) && w > 0.0) ownBandSum += w; }
            if (K > 1 && wSlot1 >= 0) { float w = td_vec_comp(ownW, wSlot1); if (!td_is_bad(w) && w > 0.0) ownBandSum += w; }
            if (K > 2 && wSlot2 >= 0) { float w = td_vec_comp(ownW, wSlot2); if (!td_is_bad(w) && w > 0.0) ownBandSum += w; }
            allowBlend = (ownBandSum > 0.0);
        }
        float bandDelta = 0.0;
        if (allowBlend)
        {
            bandDelta += td_c3_accum_ref(refOk0, 0, wSlot0, reuseLumaW, ownActiveMask, mg0, curV, (chIdx == 0) ? r0B.x : r0B.y,
                                         w00, w10, w01, w11, d00, d10, d01, d11, tx, ty, enableHuber);
            bandDelta += td_c3_accum_ref(refOk1, 1, wSlot1, reuseLumaW, ownActiveMask, mg1, curV, (chIdx == 0) ? r1B.x : r1B.y,
                                         w00, w10, w01, w11, d00, d10, d01, d11, tx, ty, enableHuber);
            bandDelta += td_c3_accum_ref(refOk2, 2, wSlot2, reuseLumaW, ownActiveMask, mg2, curV, (chIdx == 0) ? r2B.x : r2B.y,
                                         w00, w10, w01, w11, d00, d10, d01, d11, tx, ty, enableHuber);
        }
        if (abs(bandDelta) > 0.0)
            bandDelta = clamp(bandDelta, -bound, bound);
        if (chIdx == 0) totalDelta.x += bandDelta;
        else            totalDelta.y += bandDelta;
    }
}

if (u_enable_joint_bound > 0.5)
{
    float jointBoundCb = u_pixel_bound_kappa * sqrt(max(0.0, sumBandVar.x));
    float jointBoundCr = u_pixel_bound_kappa * sqrt(max(0.0, sumBandVar.y));
    if (abs(totalDelta.x) > 0.0) totalDelta.x = clamp(totalDelta.x, -jointBoundCb, jointBoundCb);
    if (abs(totalDelta.y) > 0.0) totalDelta.y = clamp(totalDelta.y, -jointBoundCr, jointBoundCr);
}

vec2 safeD = vec2(td_is_bad(totalDelta.x) ? 0.0 : totalDelta.x,
                  td_is_bad(totalDelta.y) ? 0.0 : totalDelta.y);
imageStore(coded_delta_raw, p, vec4(safeD, 0.0, 0.0));
)GLSL";

		// ---- PASS C4: Fused Layer-4 TdGuardPort Grain-Preserving Signed-Delta Output (Cb & Cr in ONE pass!) ----
		// Dispatch: TD_CW x TD_CH workgroups of 8x8 (64 threads/cell, 14x14 halo, 8.2 KB LDS -> 3x higher occupancy!).
		constexpr const char* kPassChromaGuardHeader = R"GLSL(
shared vec2 td_cg_r[(8 + 2 * TD_RAD_FINE) * (8 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_d[(8 + 2 * TD_RAD_FINE) * (8 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_g[(8 + 2 * TD_RAD_FINE) * (8 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_rh[(8 + 2 * TD_RAD_FINE) * 8];
shared vec2 td_cg_dh[(8 + 2 * TD_RAD_FINE) * 8];
shared vec2 td_cg_gh[(8 + 2 * TD_RAD_FINE) * 8];
shared vec2 td_cg_raw_d[64];
shared vec2 td_cg_cov2[64];
shared vec4 td_cg_out;
)GLSL";

		constexpr const char* kPassChromaGuardPart1 = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 8 + lx;

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
bool storeFineDiff = (u_store_fine_diff > 0.5);

vec4 priorTexel = texelFetch(cell_prior, cell, 0);
int expFocusBits = int(round(priorTexel.y));
bool allBandVeto = ((expFocusBits & 1) != 0) || (texelFetch(cell_own_grain, cell, 0).w > 0.5) || (((expFocusBits & 2) != 0) && !bokehAllowL);
bool cellIsFace = (u_has_face_mask > 0.5) && (texelFetch(face_mask, cell, 0).x > 0.5);
bool cellVetoed = cellIsFace || allBandVeto;

// Exact siting cell bounding box (8x8 on 4:2:0 -> subX=1, subY=1; 8x16 on 4:2:2; 16x16 on 4:4:4).
int cellStartX = (cell.x == 0) ? 0 : max(0, int(floor(td_luma_to_coded_x(float(cell.x * TD_CELL_PX)))));
while (cellStartX < TD_CH_W && td_coded_to_luma_cell(ivec2(cellStartX, 0)).x < cell.x) ++cellStartX;
int cellEndX = (cell.x == TD_CW - 1) ? TD_CH_W : min(TD_CH_W, int(ceil(td_luma_to_coded_x(float((cell.x + 1) * TD_CELL_PX)))) + 1);
while (cellEndX > cellStartX && td_coded_to_luma_cell(ivec2(cellEndX - 1, 0)).x > cell.x) --cellEndX;
int cellStartY = (cell.y == 0) ? 0 : max(0, int(floor(td_luma_to_coded_y(float(cell.y * TD_CELL_PX)))));
while (cellStartY < TD_CH_H && td_coded_to_luma_cell(ivec2(0, cellStartY)).y < cell.y) ++cellStartY;
int cellEndY = (cell.y == TD_CH - 1) ? TD_CH_H : min(TD_CH_H, int(ceil(td_luma_to_coded_y(float((cell.y + 1) * TD_CELL_PX)))) + 1);
while (cellEndY > cellStartY && td_coded_to_luma_cell(ivec2(0, cellEndY - 1)).y > cell.y) --cellEndY;

int cellW = max(0, cellEndX - cellStartX);
int cellH = max(0, cellEndY - cellStartY);
int subX = max(1, (cellW + 7) / 8);
int subY = max(1, (cellH + 7) / 8);
bool canCacheSingle = (subX == 1 && subY == 1);

int tileDim = 8 + 2 * TD_RAD_FINE;
int tileN = tileDim * tileDim;
int hN = tileDim * 8;

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
        int baseX = cellStartX + sx * 8;
        int baseY = cellStartY + sy * 8;
        ivec2 p = ivec2(baseX + lx, baseY + ly);

        for (int idx = tid; idx < tileN; idx += 64)
        {
            int ty = idx / tileDim;
            int tx = idx - ty * tileDim;
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
            if (!sameCleanRec)
                td_cg_g[idx] = og;
            if (tx >= TD_RAD_FINE && tx < TD_RAD_FINE + 8 && ty >= TD_RAD_FINE && ty < TD_RAD_FINE + 8)
                td_cg_raw_d[(ty - TD_RAD_FINE) * 8 + (tx - TD_RAD_FINE)] = dv;
        }
        memoryBarrierShared();
        barrier();

        for (int idx = tid; idx < hN; idx += 64)
        {
            int ty = idx / 8;
            int tx = idx - ty * 8;
            int rowBase = ty * tileDim + (tx + TD_RAD_FINE);
            vec2 ar = vec2(0.0);
            vec2 ad = vec2(0.0);
            vec2 ag = vec2(0.0);
            for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
            {
                float w = TD_TAP_FINE[k + TD_RAD_FINE];
                ar += w * td_cg_r[rowBase + k];
                ad += w * td_cg_d[rowBase + k];
                if (!sameCleanRec)
                    ag += w * td_cg_g[rowBase + k];
            }
            td_cg_rh[idx] = ar;
            td_cg_dh[idx] = ad;
            if (!sameCleanRec)
                td_cg_gh[idx] = ag;
        }
        memoryBarrierShared();
        barrier();

        int ox = sx * 8 + lx;
        int oy = sy * 8 + ly;
        bool inCell = (ox < cellW) && (oy < cellH) && (p.x < TD_CH_W) && (p.y < TD_CH_H) &&
                      (td_coded_to_luma_cell(p) == cell);
        bool inAct = inCell && validRect && (p.x >= cax0) && (p.x < cax1) && (p.y >= cay0) && (p.y < cay1);

        if (inCell)
        {
            if (!inAct || cellVetoed)
            {
                if (storeFineDiff)
                    imageStore(out_coded_delta, p, vec4(0.0));
                else if (canCacheSingle) { regRawDelta = vec2(0.0); regFineDiff = vec2(0.0); regWrite = true; regP = p; }
                else imageStore(out_coded_delta, p, vec4(0.0));
            }
            else
            {
                int centerIdx = (ly + TD_RAD_FINE) * tileDim + (lx + TD_RAD_FINE);
                vec2 rv = td_cg_r[centerIdx];
                vec2 candOut = td_cg_d[centerIdx];
                vec2 og = sameCleanRec ? vec2(0.0) : td_cg_g[centerIdx];
                vec2 dRaw = td_cg_raw_d[tid];

                vec2 gr = vec2(0.0);
                vec2 gd = vec2(0.0);
                vec2 gg = vec2(0.0);
                int cyBase = ly + TD_RAD_FINE;
                for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                {
                    float w = TD_TAP_FINE[k + TD_RAD_FINE];
                    int hIdx = (cyBase + k) * 8 + lx;
                    gr += w * td_cg_rh[hIdx];
                    gd += w * td_cg_dh[hIdx];
                    if (!sameCleanRec)
                        gg += w * td_cg_gh[hIdx];
                }
                vec2 inFullH = rv - gr;
                vec2 outFullH = candOut - gd;
                vec2 ogFineH = sameCleanRec ? vec2(0.0) : (og - gg);

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

                vec2 fineDiffVal = inFullH - outFullH;
                if (storeFineDiff)
                {
                    imageStore(out_coded_delta, p, vec4(fineDiffVal, 0.0, 0.0));
                }
                else if (canCacheSingle)
                {
                    regRawDelta = dRaw;
                    regFineDiff = fineDiffVal;
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
// 2-stage 8-way LDS reduction (2 barriers instead of 8!).
td_cg_r[tid]    = sumOgE;
td_cg_d[tid]    = sumOgFineE;
td_cg_g[tid]    = sumInFineE;
td_cg_rh[tid]   = sumOutFineE;
td_cg_dh[tid]   = vec2(sumCnt, 0.0);
td_cg_gh[tid]   = sumInCovOwn;
td_cg_cov2[tid] = sumOutCovOwn;
memoryBarrierShared();
barrier();

if (lx == 0)
{
    int ro = ly * 8;
    vec2 s0 = vec2(0.0), s1 = vec2(0.0), s2 = vec2(0.0), s3 = vec2(0.0), s4 = vec2(0.0), s5 = vec2(0.0), s6 = vec2(0.0);
    for (int i = 0; i < 8; ++i)
    {
        s0 += td_cg_r[ro + i]; s1 += td_cg_d[ro + i]; s2 += td_cg_g[ro + i];
        s3 += td_cg_rh[ro + i]; s4 += td_cg_dh[ro + i]; s5 += td_cg_gh[ro + i]; s6 += td_cg_cov2[ro + i];
    }
    td_cg_r[ro] = s0; td_cg_d[ro] = s1; td_cg_g[ro] = s2;
    td_cg_rh[ro] = s3; td_cg_dh[ro] = s4; td_cg_gh[ro] = s5; td_cg_cov2[ro] = s6;
}
memoryBarrierShared();
barrier();

if (tid == 0)
{
    vec2 totOgE = vec2(0.0), totOgF = vec2(0.0), totInF = vec2(0.0), totOutF = vec2(0.0);
    vec2 totInC = vec2(0.0), totOutC = vec2(0.0);
    float totCnt = 0.0;
    for (int r = 0; r < 8; ++r)
    {
        int ro = r * 8;
        totOgE += td_cg_r[ro]; totOgF += td_cg_d[ro]; totInF += td_cg_g[ro];
        totOutF += td_cg_rh[ro]; totCnt += td_cg_dh[ro].x; totInC += td_cg_gh[ro]; totOutC += td_cg_cov2[ro];
    }
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
            float ogMean = (c == 0 ? totOgE.x : totOgE.y) * invC;
            float ogFineMean = (c == 0 ? totOgF.x : totOgF.y) * invC;
            float inFineMean = (c == 0 ? totInF.x : totInF.y) * invC;
            float outFineMean = (c == 0 ? totOutF.x : totOutF.y) * invC;
            float inCov = (c == 0 ? totInC.x : totInC.y);
            float outCov = (c == 0 ? totOutC.x : totOutC.y);
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

// When storeFineDiff is enabled (default!), fineDiff was already written directly in Part 1!
if (storeFineDiff)
    return;

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
            int baseX = cellStartX + sx * 8;
            int baseY = cellStartY + sy * 8;
            ivec2 p = ivec2(baseX + lx, baseY + ly);
            bool needAddBack = !cellVetoed &&
                               (((cellGFlags.x & 1) != 0 && cellAddBack.x > 0.0) ||
                                ((cellGFlags.y & 1) != 0 && cellAddBack.y > 0.0));
            for (int idx = tid; idx < tileN; idx += 64)
            {
                vec2 rVal = vec2(0.0);
                vec2 cOutVal = vec2(0.0);
                if (needAddBack)
                {
                    int ty = idx / tileDim;
                    int tx = idx - ty * tileDim;
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

            for (int idx = tid; idx < hN; idx += 64)
            {
                vec2 ar = vec2(0.0);
                vec2 ad = vec2(0.0);
                if (needAddBack)
                {
                    int ty = idx / 8;
                    int tx = idx - ty * 8;
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

            int ox = sx * 8 + lx;
            int oy = sy * 8 + ly;
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
                        int hIdx = (cyBase + k) * 8 + lx;
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

		// ---- PASS C3+C4 FUSED (Chroma Fusion 2 — TD-GPU-OPT-2): Fused Coded-Grid Blend + Bound + Layer-4 TdGuardPort ----
		// Eliminates full coded-grid c3_blend_bound dispatch + 14x14 halo reads of coded_delta_raw & cur_uv (cur_uv == curL+curM+curH!).
		// Strictly <= 16 SRVs (15 when sameCleanRec, 16 when !sameCleanRec via cell_weights_cr/motion_gate multiplexing),
		// 3 UAVs, 10.4 KB LDS (<< 32 KB).
		constexpr const char* kPassChromaBlendGuardHeader = R"GLSL(
shared vec4 td_c34_cw_cb[27];
shared vec4 td_c34_cw_cr[27];
shared vec4 td_c34_cd_cb[27];
shared vec4 td_c34_cd_cr[27];
shared vec4 td_c34_prior[9];
shared float td_c34_face[9];
shared vec2 td_cg_r[(8 + 2 * TD_RAD_FINE) * (8 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_d[(8 + 2 * TD_RAD_FINE) * (8 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_g[(8 + 2 * TD_RAD_FINE) * (8 + 2 * TD_RAD_FINE)];
shared vec2 td_cg_rh[(8 + 2 * TD_RAD_FINE) * 8];
shared vec2 td_cg_dh[(8 + 2 * TD_RAD_FINE) * 8];
shared vec2 td_cg_gh[(8 + 2 * TD_RAD_FINE) * 8];
shared vec2 td_cg_raw_d[64];
shared vec2 td_cg_cov2[64];
shared vec4 td_cg_out;

float td_c34_band_gain(int b) { if (b == 0) return TD_GAIN_LOW; if (b == 1) return TD_GAIN_MID; return TD_GAIN_FINE; }
float td_c34_vec_comp(vec4 v, int k) { if (k == 0) return v.x; if (k == 1) return v.y; return v.z; }
int td_c34_luma_slot(int k) { if (k == 0) return int(round(u_luma_slot_0)); if (k == 1) return int(round(u_luma_slot_1)); return int(round(u_luma_slot_2)); }
float td_c34_huber_taper(float diffSq, float diagC)
{
    float denom = max(diagC, TD_NUM_FLOOR);
    float r = diffSq / denom;
    return (r <= TD_HUBER_KNEE_SQ) ? 1.0 : (TD_HUBER_KNEE_SQ / r);
}

float td_c34_accum_ref(bool refOk, int k, int wSlot, bool reuseLumaW, int ownActiveMask, float mGateOwn,
                       float curV, float refV, vec4 w00, vec4 w10, vec4 w01, vec4 w11,
                       vec4 d00, vec4 d10, vec4 d01, vec4 d11, float tx, float ty, bool enableHuber)
{
    if (!refOk || wSlot < 0) return 0.0;
    if (reuseLumaW) { if (mGateOwn <= 0.0) return 0.0; }
    else if ((ownActiveMask & (1 << k)) == 0) return 0.0;
    float diff = curV - refV;
    if (td_is_bad(diff)) return 0.0;
    float t00 = td_c34_vec_comp(w00, wSlot);
    float t10 = td_c34_vec_comp(w10, wSlot);
    float t01 = td_c34_vec_comp(w01, wSlot);
    float t11 = td_c34_vec_comp(w11, wSlot);
    if (td_is_bad(t00) || td_is_bad(t10) || td_is_bad(t01) || td_is_bad(t11)) return 0.0;
    if (enableHuber)
    {
        float diffSq = diff * diff;
        t00 *= td_c34_huber_taper(diffSq, td_c34_vec_comp(d00, k));
        t10 *= td_c34_huber_taper(diffSq, td_c34_vec_comp(d10, k));
        t01 *= td_c34_huber_taper(diffSq, td_c34_vec_comp(d01, k));
        t11 *= td_c34_huber_taper(diffSq, td_c34_vec_comp(d11, k));
    }
    float top = t00 + (t10 - t00) * tx;
    float bot = t01 + (t11 - t01) * tx;
    float wEff = top + (bot - top) * ty;
    return (wEff > 0.0) ? (-wEff * diff) : 0.0;
}

vec2 td_c34_eval_delta_and_cur(ivec2 sp, int wgCx, int wgCy, int K, bool reuseLumaW, bool enableHuber,
                               bool bokehAllowL, bool enableJointBound, out vec2 outCurUv)
{
    vec2 curL  = texelFetch(cur_l, sp, 0).xy;
    vec4 curMH = texelFetch(cur_mh, sp, 0);
    outCurUv = curL + curMH.xy + curMH.zw;
    if (td_is_bad(outCurUv.x)) outCurUv.x = 0.0;
    if (td_is_bad(outCurUv.y)) outCurUv.y = 0.0;

    float lx = td_coded_to_luma_x(float(sp.x));
    float ly = td_coded_to_luma_y(float(sp.y));
    int cx = clamp(int(floor(lx)) / TD_CELL_PX, 0, TD_CW - 1);
    int cy = clamp(int(floor(ly)) / TD_CELL_PX, 0, TD_CH - 1);
    float gx = (lx + 0.5) / float(TD_CELL_PX) - 0.5;
    float gy = (ly + 0.5) / float(TD_CELL_PX) - 0.5;
    int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
    int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
    int x1 = min(TD_CW - 1, x0 + 1);
    int y1 = min(TD_CH - 1, y0 + 1);
    float tx = clamp(gx - float(x0), 0.0, 1.0);
    float ty = clamp(gy - float(y0), 0.0, 1.0);

    bool in3x3 = (cx >= wgCx - 1 && cx <= wgCx + 1 && cy >= wgCy - 1 && cy <= wgCy + 1 &&
                  x0 >= wgCx - 1 && x1 <= wgCx + 1 && y0 >= wgCy - 1 && y1 <= wgCy + 1);
    int qOwn = in3x3 ? ((cy - wgCy + 1) * 3 + (cx - wgCx + 1)) : 4;
    int q00  = in3x3 ? ((y0 - wgCy + 1) * 3 + (x0 - wgCx + 1)) : 0;
    int q10  = in3x3 ? ((y0 - wgCy + 1) * 3 + (x1 - wgCx + 1)) : 0;
    int q01  = in3x3 ? ((y1 - wgCy + 1) * 3 + (x0 - wgCx + 1)) : 0;
    int q11  = in3x3 ? ((y1 - wgCy + 1) * 3 + (x1 - wgCx + 1)) : 0;

    float faceVal = in3x3 ? td_c34_face[qOwn] : ((u_has_face_mask > 0.5) ? texelFetch(face_mask, ivec2(cx, cy), 0).x : 0.0);
    if (u_has_face_mask > 0.5 && faceVal > 0.5)
        return vec2(0.0);

    vec4 priorT = in3x3 ? td_c34_prior[qOwn] : texelFetch(cell_prior, ivec2(cx, cy), 0);
    float rawScale = (u_cell_prior_has_step_scale > 0.5) ? priorT.z : 1.0;
    float cellStepScale = (td_is_bad(rawScale) || !(rawScale > 0.0)) ? 0.0 : clamp(rawScale, TD_STEP_SCALE_MIN, TD_STEP_SCALE_MAX);
    float cellStepScaleSq = cellStepScale * cellStepScale;
    int expFocusBits = int(round(priorT.y));
    float diagCb0W = in3x3 ? td_c34_cd_cb[qOwn].w : texelFetch(cell_diag_cb, ivec2(cx, cy), 0).w;
    bool cellFocusVeto = ((expFocusBits & 1) != 0) || (diagCb0W < -0.5);
    bool cellDefoc = ((expFocusBits & 2) != 0);
    if (reuseLumaW && (cellFocusVeto || (cellDefoc && !bokehAllowL)))
        return vec2(0.0);

    uint packedFl = uint(round(texelFetch(ref_flags, sp, 0).x * 65535.0));
    bool refOk0 = (K > 0) && ((td_unpack_ref_flags(packedFl, 0) & 1) != 0);
    bool refOk1 = (K > 1) && ((td_unpack_ref_flags(packedFl, 1) & 1) != 0);
    bool refOk2 = (K > 2) && ((td_unpack_ref_flags(packedFl, 2) & 1) != 0);
    if (!refOk0 && !refOk1 && !refOk2)
        return vec2(0.0);

    vec2 refL0 = refOk0 ? texelFetch(ref_l_0, sp, 0).xy : vec2(0.0);
    vec2 refL1 = refOk1 ? texelFetch(ref_l_1, sp, 0).xy : vec2(0.0);
    vec2 refL2 = refOk2 ? texelFetch(ref_l_2, sp, 0).xy : vec2(0.0);
    vec4 refMH0 = refOk0 ? texelFetch(ref_mh_0, sp, 0) : vec4(0.0);
    vec4 refMH1 = refOk1 ? texelFetch(ref_mh_1, sp, 0) : vec4(0.0);
    vec4 refMH2 = refOk2 ? texelFetch(ref_mh_2, sp, 0) : vec4(0.0);

    vec2 mgOwn0 = reuseLumaW ? (in3x3 ? td_c34_cw_cr[0 * 9 + qOwn].xy : texelFetch(cell_weights_cr_or_mg, ivec2(0 * TD_CW + cx, cy), 0).xy) : vec2(1.0);
    vec2 mgOwn1 = (reuseLumaW && K > 1) ? (in3x3 ? td_c34_cw_cr[1 * 9 + qOwn].xy : texelFetch(cell_weights_cr_or_mg, ivec2(1 * TD_CW + cx, cy), 0).xy) : vec2(1.0);
    vec2 mgOwn2 = (reuseLumaW && K > 2) ? (in3x3 ? td_c34_cw_cr[2 * 9 + qOwn].xy : texelFetch(cell_weights_cr_or_mg, ivec2(2 * TD_CW + cx, cy), 0).xy) : vec2(1.0);
    int wSlot0 = reuseLumaW ? td_c34_luma_slot(0) : 0;
    int wSlot1 = reuseLumaW ? td_c34_luma_slot(1) : 1;
    int wSlot2 = reuseLumaW ? td_c34_luma_slot(2) : 2;

    vec2 totalDelta = vec2(0.0);
    vec2 sumBandVar = vec2(0.0);
    for (int b = 0; b < 3; ++b)
    {
        if (reuseLumaW && (cellFocusVeto || (cellDefoc && (b >= 1 || !bokehAllowL))))
            continue;
        vec2 curB = (b == 0) ? curL : ((b == 1) ? curMH.xy : curMH.zw);
        vec2 r0B  = (b == 0) ? refL0 : ((b == 1) ? refMH0.xy : refMH0.zw);
        vec2 r1B  = (b == 0) ? refL1 : ((b == 1) ? refMH1.xy : refMH1.zw);
        vec2 r2B  = (b == 0) ? refL2 : ((b == 1) ? refMH2.xy : refMH2.zw);
        float mg0 = (b == 0) ? mgOwn0.x : mgOwn0.y;
        float mg1 = (b == 0) ? mgOwn1.x : mgOwn1.y;
        float mg2 = (b == 0) ? mgOwn2.x : mgOwn2.y;
        float fallbackBandVar = (u_a2_valid > 0.5) ? (u_a2_total_var * cellStepScaleSq * td_c34_band_gain(b)) : 0.0;
        int bOff = b * 9;

        for (int chIdx = 0; chIdx < 2; ++chIdx)
        {
            bool useCbW = (chIdx == 0) || reuseLumaW;
            vec4 ownW, ownD, w00, w10, w01, w11, d00, d10, d01, d11;
            if (in3x3)
            {
                ownW = useCbW ? td_c34_cw_cb[bOff + qOwn] : td_c34_cw_cr[bOff + qOwn];
                ownD = (chIdx == 0) ? td_c34_cd_cb[bOff + qOwn] : td_c34_cd_cr[bOff + qOwn];
                w00  = useCbW ? td_c34_cw_cb[bOff + q00]  : td_c34_cw_cr[bOff + q00];
                w10  = useCbW ? td_c34_cw_cb[bOff + q10]  : td_c34_cw_cr[bOff + q10];
                w01  = useCbW ? td_c34_cw_cb[bOff + q01]  : td_c34_cw_cr[bOff + q01];
                w11  = useCbW ? td_c34_cw_cb[bOff + q11]  : td_c34_cw_cr[bOff + q11];
                d00  = (chIdx == 0) ? td_c34_cd_cb[bOff + q00]  : td_c34_cd_cr[bOff + q00];
                d10  = (chIdx == 0) ? td_c34_cd_cb[bOff + q10]  : td_c34_cd_cr[bOff + q10];
                d01  = (chIdx == 0) ? td_c34_cd_cb[bOff + q01]  : td_c34_cd_cr[bOff + q01];
                d11  = (chIdx == 0) ? td_c34_cd_cb[bOff + q11]  : td_c34_cd_cr[bOff + q11];
            }
            else
            {
                ownW = useCbW ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + cx, cy), 0) : texelFetch(cell_weights_cr_or_mg, ivec2(b * TD_CW + cx, cy), 0);
                ownD = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + cx, cy), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + cx, cy), 0);
                w00 = useCbW ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x0, y0), 0) : texelFetch(cell_weights_cr_or_mg, ivec2(b * TD_CW + x0, y0), 0);
                w10 = useCbW ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x1, y0), 0) : texelFetch(cell_weights_cr_or_mg, ivec2(b * TD_CW + x1, y0), 0);
                w01 = useCbW ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x0, y1), 0) : texelFetch(cell_weights_cr_or_mg, ivec2(b * TD_CW + x0, y1), 0);
                w11 = useCbW ? texelFetch(cell_weights_cb, ivec2(b * TD_CW + x1, y1), 0) : texelFetch(cell_weights_cr_or_mg, ivec2(b * TD_CW + x1, y1), 0);
                d00 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x0, y0), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x0, y0), 0);
                d10 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x1, y0), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x1, y0), 0);
                d01 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x0, y1), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x0, y1), 0);
                d11 = (chIdx == 0) ? texelFetch(cell_diag_cb, ivec2(b * TD_CW + x1, y1), 0) : texelFetch(cell_diag_cr, ivec2(b * TD_CW + x1, y1), 0);
            }

            float ownU = max(0.0, ownD.w);
            float bandVar = (ownU > 0.0) ? ownU : fallbackBandVar;
            if (chIdx == 0) sumBandVar.x += bandVar;
            else            sumBandVar.y += bandVar;
            float bound = u_pixel_bound_kappa * sqrt(max(0.0, bandVar));

            float curV = (chIdx == 0) ? curB.x : curB.y;
            int ownActiveMask = int(round(ownW.w));
            bool allowBlend = true;
            if (reuseLumaW)
            {
                float ownBandSum = 0.0;
                if (K > 0 && wSlot0 >= 0) { float w = td_c34_vec_comp(ownW, wSlot0); if (!td_is_bad(w) && w > 0.0) ownBandSum += w; }
                if (K > 1 && wSlot1 >= 0) { float w = td_c34_vec_comp(ownW, wSlot1); if (!td_is_bad(w) && w > 0.0) ownBandSum += w; }
                if (K > 2 && wSlot2 >= 0) { float w = td_c34_vec_comp(ownW, wSlot2); if (!td_is_bad(w) && w > 0.0) ownBandSum += w; }
                allowBlend = (ownBandSum > 0.0);
            }
            float bandDelta = 0.0;
            if (allowBlend)
            {
                bandDelta += td_c34_accum_ref(refOk0, 0, wSlot0, reuseLumaW, ownActiveMask, mg0, curV, (chIdx == 0) ? r0B.x : r0B.y,
                                              w00, w10, w01, w11, d00, d10, d01, d11, tx, ty, enableHuber);
                bandDelta += td_c34_accum_ref(refOk1, 1, wSlot1, reuseLumaW, ownActiveMask, mg1, curV, (chIdx == 0) ? r1B.x : r1B.y,
                                              w00, w10, w01, w11, d00, d10, d01, d11, tx, ty, enableHuber);
                bandDelta += td_c34_accum_ref(refOk2, 2, wSlot2, reuseLumaW, ownActiveMask, mg2, curV, (chIdx == 0) ? r2B.x : r2B.y,
                                              w00, w10, w01, w11, d00, d10, d01, d11, tx, ty, enableHuber);
            }
            if (abs(bandDelta) > 0.0)
                bandDelta = clamp(bandDelta, -bound, bound);
            if (chIdx == 0) totalDelta.x += bandDelta;
            else            totalDelta.y += bandDelta;
        }
    }

    if (enableJointBound)
    {
        float jointBoundCb = u_pixel_bound_kappa * sqrt(max(0.0, sumBandVar.x));
        float jointBoundCr = u_pixel_bound_kappa * sqrt(max(0.0, sumBandVar.y));
        if (abs(totalDelta.x) > 0.0) totalDelta.x = clamp(totalDelta.x, -jointBoundCb, jointBoundCb);
        if (abs(totalDelta.y) > 0.0) totalDelta.y = clamp(totalDelta.y, -jointBoundCr, jointBoundCr);
    }
    return vec2(td_is_bad(totalDelta.x) ? 0.0 : totalDelta.x,
                td_is_bad(totalDelta.y) ? 0.0 : totalDelta.y);
}
)GLSL";

		constexpr const char* kPassChromaBlendGuardPart1 = R"GLSL(
ivec2 cell = ivec2(gl_WorkGroupID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;

int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int tid = ly * 8 + lx;
int wgCx = cell.x;
int wgCy = cell.y;

int K = int(round(u_k));
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
bool storeFineDiff = (u_store_fine_diff > 0.5);
bool reuseLumaW = (u_reuse_luma_weights > 0.5);
bool enableHuber = (u_enable_per_corner_huber > 0.5);
bool enableJointBound = (u_enable_joint_bound > 0.5);

// Cooperative 3x3 luma-cell LDS cache (27 threads load 3 bands x 9 cells).
if (tid < 27)
{
    int bIdx = tid / 9;
    int q = tid - bIdx * 9;
    int dx = (q % 3) - 1;
    int dy = (q / 3) - 1;
    int ncx = clamp(wgCx + dx, 0, TD_CW - 1);
    int ncy = clamp(wgCy + dy, 0, TD_CH - 1);
    td_c34_cw_cb[tid] = texelFetch(cell_weights_cb, ivec2(bIdx * TD_CW + ncx, ncy), 0);
    if (!reuseLumaW || bIdx < K)
        td_c34_cw_cr[tid] = texelFetch(cell_weights_cr_or_mg, ivec2(bIdx * TD_CW + ncx, ncy), 0);
    else
        td_c34_cw_cr[tid] = vec4(0.0);
    td_c34_cd_cb[tid] = texelFetch(cell_diag_cb, ivec2(bIdx * TD_CW + ncx, ncy), 0);
    td_c34_cd_cr[tid] = texelFetch(cell_diag_cr, ivec2(bIdx * TD_CW + ncx, ncy), 0);
    if (bIdx == 0)
    {
        td_c34_prior[q] = texelFetch(cell_prior, ivec2(ncx, ncy), 0);
        td_c34_face[q] = (u_has_face_mask > 0.5) ? texelFetch(face_mask, ivec2(ncx, ncy), 0).x : 0.0;
    }
}
memoryBarrierShared();
barrier();

vec4 priorTexel = td_c34_prior[4];
int expFocusBits = int(round(priorTexel.y));
bool allBandVeto = ((expFocusBits & 1) != 0) || (td_c34_cd_cb[4].w < -0.5) || (((expFocusBits & 2) != 0) && !bokehAllowL);
bool cellIsFace = (u_has_face_mask > 0.5) && (td_c34_face[4] > 0.5);
bool cellVetoed = cellIsFace || allBandVeto;

int cellStartX = (cell.x == 0) ? 0 : max(0, int(floor(td_luma_to_coded_x(float(cell.x * TD_CELL_PX)))));
while (cellStartX < TD_CH_W && td_coded_to_luma_cell(ivec2(cellStartX, 0)).x < cell.x) ++cellStartX;
int cellEndX = (cell.x == TD_CW - 1) ? TD_CH_W : min(TD_CH_W, int(ceil(td_luma_to_coded_x(float((cell.x + 1) * TD_CELL_PX)))) + 1);
while (cellEndX > cellStartX && td_coded_to_luma_cell(ivec2(cellEndX - 1, 0)).x > cell.x) --cellEndX;
int cellStartY = (cell.y == 0) ? 0 : max(0, int(floor(td_luma_to_coded_y(float(cell.y * TD_CELL_PX)))));
while (cellStartY < TD_CH_H && td_coded_to_luma_cell(ivec2(0, cellStartY)).y < cell.y) ++cellStartY;
int cellEndY = (cell.y == TD_CH - 1) ? TD_CH_H : min(TD_CH_H, int(ceil(td_luma_to_coded_y(float((cell.y + 1) * TD_CELL_PX)))) + 1);
while (cellEndY > cellStartY && td_coded_to_luma_cell(ivec2(0, cellEndY - 1)).y > cell.y) --cellEndY;

int cellW = max(0, cellEndX - cellStartX);
int cellH = max(0, cellEndY - cellStartY);
int subX = max(1, (cellW + 7) / 8);
int subY = max(1, (cellH + 7) / 8);

int tileDim = 8 + 2 * TD_RAD_FINE;
int tileN = tileDim * tileDim;
int hN = tileDim * 8;

vec2 sumOgE = vec2(0.0), sumOgFineE = vec2(0.0), sumInFineE = vec2(0.0), sumOutFineE = vec2(0.0);
vec2 sumInCovOwn = vec2(0.0), sumOutCovOwn = vec2(0.0);
float sumCnt = 0.0;

vec2 regRawDelta = vec2(0.0);
vec2 regFineDiff = vec2(0.0);
bool regWrite = false;
ivec2 regP = ivec2(0);

for (int sy = 0; sy < subY; ++sy)
{
    for (int sx = 0; sx < subX; ++sx)
    {
        int baseX = cellStartX + sx * 8;
        int baseY = cellStartY + sy * 8;
        ivec2 p = ivec2(baseX + lx, baseY + ly);

        for (int idx = tid; idx < tileN; idx += 64)
        {
            int ty = idx / tileDim;
            int tx = idx - ty * tileDim;
            int gx = baseX - TD_RAD_FINE + tx;
            int gy = baseY - TD_RAD_FINE + ty;
            int clx = validRect ? clamp(gx, xMin, xMax) : clamp(gx, 0, TD_CH_W - 1);
            int cly = validRect ? clamp(gy, yMin, yMax) : clamp(gy, 0, TD_CH_H - 1);
            ivec2 sp = ivec2(clx, cly);
            vec2 cv = vec2(0.0);
            vec2 dv = validRect
                ? td_c34_eval_delta_and_cur(sp, wgCx, wgCy, K, reuseLumaW, enableHuber, bokehAllowL, enableJointBound, cv)
                : vec2(0.0);
#if TD_C34_HAS_REC_UV
            vec2 rv = texelFetch(rec_uv, sp, 0).xy;
            if (td_is_bad(rv.x)) rv.x = 0.0;
            if (td_is_bad(rv.y)) rv.y = 0.0;
#else
            vec2 rv = cv;
#endif
            vec2 og = rv - cv;
            td_cg_r[idx] = rv;
            td_cg_d[idx] = (cv + dv) + og;
            if (!sameCleanRec)
                td_cg_g[idx] = og;
            if (tx >= TD_RAD_FINE && tx < TD_RAD_FINE + 8 && ty >= TD_RAD_FINE && ty < TD_RAD_FINE + 8)
                td_cg_raw_d[(ty - TD_RAD_FINE) * 8 + (tx - TD_RAD_FINE)] = dv;
        }
        memoryBarrierShared();
        barrier();

        for (int idx = tid; idx < hN; idx += 64)
        {
            int ty = idx / 8;
            int tx = idx - ty * 8;
            int rowBase = ty * tileDim + (tx + TD_RAD_FINE);
            vec2 ar = vec2(0.0), ad = vec2(0.0), ag = vec2(0.0);
            for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
            {
                float w = TD_TAP_FINE[k + TD_RAD_FINE];
                ar += w * td_cg_r[rowBase + k];
                ad += w * td_cg_d[rowBase + k];
                if (!sameCleanRec)
                    ag += w * td_cg_g[rowBase + k];
            }
            td_cg_rh[idx] = ar;
            td_cg_dh[idx] = ad;
            if (!sameCleanRec)
                td_cg_gh[idx] = ag;
        }
        memoryBarrierShared();
        barrier();

        int ox = sx * 8 + lx;
        int oy = sy * 8 + ly;
        bool inCell = (ox < cellW) && (oy < cellH) && (p.x < TD_CH_W) && (p.y < TD_CH_H) &&
                      (td_coded_to_luma_cell(p) == cell);
        bool inAct = inCell && validRect && (p.x >= cax0) && (p.x < cax1) && (p.y >= cay0) && (p.y < cay1);

        if (inCell)
        {
            if (!inAct || cellVetoed)
            {
                if (storeFineDiff)
                {
                    imageStore(coded_delta_raw, p, vec4(0.0));
                    imageStore(out_coded_delta, p, vec4(0.0));
                }
                else
                {
                    regRawDelta = vec2(0.0); regFineDiff = vec2(0.0); regWrite = true; regP = p;
                }
            }
            else
            {
                int centerIdx = (ly + TD_RAD_FINE) * tileDim + (lx + TD_RAD_FINE);
                vec2 rv = td_cg_r[centerIdx];
                vec2 candOut = td_cg_d[centerIdx];
                vec2 og = sameCleanRec ? vec2(0.0) : td_cg_g[centerIdx];
                vec2 dRaw = td_cg_raw_d[tid];

                vec2 gr = vec2(0.0), gd = vec2(0.0), gg = vec2(0.0);
                int cyBase = ly + TD_RAD_FINE;
                for (int k = -TD_RAD_FINE; k <= TD_RAD_FINE; ++k)
                {
                    float w = TD_TAP_FINE[k + TD_RAD_FINE];
                    int hIdx = (cyBase + k) * 8 + lx;
                    gr += w * td_cg_rh[hIdx];
                    gd += w * td_cg_dh[hIdx];
                    if (!sameCleanRec)
                        gg += w * td_cg_gh[hIdx];
                }
                vec2 inFullH = rv - gr;
                vec2 outFullH = candOut - gd;
                vec2 ogFineH = sameCleanRec ? vec2(0.0) : (og - gg);

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

                vec2 fineDiffVal = inFullH - outFullH;
                if (storeFineDiff)
                {
                    imageStore(coded_delta_raw, p, vec4(dRaw, 0.0, 0.0));
                    imageStore(out_coded_delta, p, vec4(fineDiffVal, 0.0, 0.0));
                }
                else
                {
                    regRawDelta = dRaw;
                    regFineDiff = fineDiffVal;
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

		constexpr const char* kPassChromaBlendGuardPart2 = R"GLSL(
td_cg_r[tid]    = sumOgE;
td_cg_d[tid]    = sumOgFineE;
td_cg_g[tid]    = sumInFineE;
td_cg_rh[tid]   = sumOutFineE;
td_cg_dh[tid]   = vec2(sumCnt, 0.0);
td_cg_gh[tid]   = sumInCovOwn;
td_cg_cov2[tid] = sumOutCovOwn;
memoryBarrierShared();
barrier();

if (lx == 0)
{
    int ro = ly * 8;
    vec2 s0 = vec2(0.0), s1 = vec2(0.0), s2 = vec2(0.0), s3 = vec2(0.0), s4 = vec2(0.0), s5 = vec2(0.0), s6 = vec2(0.0);
    for (int i = 0; i < 8; ++i)
    {
        s0 += td_cg_r[ro + i]; s1 += td_cg_d[ro + i]; s2 += td_cg_g[ro + i];
        s3 += td_cg_rh[ro + i]; s4 += td_cg_dh[ro + i]; s5 += td_cg_gh[ro + i]; s6 += td_cg_cov2[ro + i];
    }
    td_cg_r[ro] = s0; td_cg_d[ro] = s1; td_cg_g[ro] = s2;
    td_cg_rh[ro] = s3; td_cg_dh[ro] = s4; td_cg_gh[ro] = s5; td_cg_cov2[ro] = s6;
}
memoryBarrierShared();
barrier();

if (tid == 0)
{
    vec2 totOgE = vec2(0.0), totOgF = vec2(0.0), totInF = vec2(0.0), totOutF = vec2(0.0);
    vec2 totInC = vec2(0.0), totOutC = vec2(0.0);
    float totCnt = 0.0;
    for (int r = 0; r < 8; ++r)
    {
        int ro = r * 8;
        totOgE += td_cg_r[ro]; totOgF += td_cg_d[ro]; totInF += td_cg_g[ro];
        totOutF += td_cg_rh[ro]; totCnt += td_cg_dh[ro].x; totInC += td_cg_gh[ro]; totOutC += td_cg_cov2[ro];
    }
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
            float ogMean = (c == 0 ? totOgE.x : totOgE.y) * invC;
            float ogFineMean = (c == 0 ? totOgF.x : totOgF.y) * invC;
            float inFineMean = (c == 0 ? totInF.x : totInF.y) * invC;
            float outFineMean = (c == 0 ? totOutF.x : totOutF.y) * invC;
            float inCov = (c == 0 ? totInC.x : totInC.y);
            float outCov = (c == 0 ? totOutC.x : totOutC.y);
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

if (storeFineDiff)
    return;

vec2 cellAddBack = vec2(td_cg_out.x, td_cg_out.z);
ivec2 cellGFlags = ivec2(int(round(td_cg_out.y)), int(round(td_cg_out.w)));

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
)GLSL";

		inline std::string BuildPassChromaBlendGuardBody()
		{
			return std::string(kPassChromaBlendGuardPart1) + kPassChromaBlendGuardPart2;
		}

		// ---- PASS C4B (L-3 / CHR-MEDIUM-1 / OPT-2): LDS-Cached Bilinear Smooth Layer-4 Add-Back Apply ----
		constexpr const char* kPassChromaSmoothGuardHeader = R"GLSL(
shared vec2 td_c4b_ab[16];
shared uint td_c4b_veto[16];
)GLSL";

		constexpr const char* kPassChromaSmoothGuard = R"GLSL(
ivec2 wg = ivec2(gl_WorkGroupID.xy);
ivec2 loc = ivec2(gl_LocalInvocationID.xy);
int tid = loc.y * 16 + loc.x;
int baseX = wg.x * 16;
int baseY = wg.y * 16;
ivec2 p = ivec2(baseX + loc.x, baseY + loc.y);
bool bokehAllowL = (u_bokeh_mode > 0.5);

ivec2 c00 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX), min(TD_CH_H - 1, baseY)));
ivec2 c11 = td_coded_to_luma_cell(ivec2(min(TD_CH_W - 1, baseX + 15), min(TD_CH_H - 1, baseY + 15)));
int minCx = max(0, c00.x - 1);
int maxCx = min(TD_CW - 1, c11.x + 1);
int minCy = max(0, c00.y - 1);
int maxCy = min(TD_CH - 1, c11.y + 1);
int spanX = maxCx - minCx + 1;
int spanY = maxCy - minCy + 1;
bool cellCacheOk = (spanX <= 4) && (spanY <= 4);
int cellCount = spanX * spanY;
if (cellCacheOk && tid < cellCount)
{
    int dy = tid / spanX;
    int dx = tid - dy * spanX;
    ivec2 nc = ivec2(minCx + dx, minCy + dy);
    vec4 g = texelFetch(guard_cell, nc, 0);
    td_c4b_ab[tid] = vec2(((int(round(g.y)) & 1) != 0) ? max(0.0, g.x) : 0.0,
                          ((int(round(g.w)) & 1) != 0) ? max(0.0, g.z) : 0.0);
    int fb = int(round(texelFetch(cell_prior, nc, 0).y));
    bool vt = ((u_has_face_mask > 0.5) && (texelFetch(face_mask, nc, 0).x > 0.5)) ||
              ((fb & 1) != 0) || (texelFetch(cell_own_grain, nc, 0).w > 0.5) ||
              (((fb & 2) != 0) && !bokehAllowL);
    td_c4b_veto[tid] = vt ? 1u : 0u;
}
memoryBarrierShared();
barrier();

if (p.x >= TD_CH_W || p.y >= TD_CH_H)
    return;

int cax0 = int(round(u_cax0));
int cay0 = int(round(u_cay0));
int cax1 = int(round(u_cax1));
int cay1 = int(round(u_cay1));
if (p.x < cax0 || p.x >= cax1 || p.y < cay0 || p.y >= cay1)
{
    imageStore(out_coded_delta, p, vec4(0.0));
    return;
}

float lx = td_coded_to_luma_x(float(p.x));
float ly = td_coded_to_luma_y(float(p.y));
int cx = clamp(int(floor(lx)) / TD_CELL_PX, 0, TD_CW - 1);
int cy = clamp(int(floor(ly)) / TD_CELL_PX, 0, TD_CH - 1);
float gx = (lx + 0.5) / float(TD_CELL_PX) - 0.5;
float gy = (ly + 0.5) / float(TD_CELL_PX) - 0.5;
int x0 = clamp(int(floor(gx)), 0, TD_CW - 1);
int y0 = clamp(int(floor(gy)), 0, TD_CH - 1);
int x1 = min(TD_CW - 1, x0 + 1);
int y1 = min(TD_CH - 1, y0 + 1);
float tx = clamp(gx - float(x0), 0.0, 1.0);
float ty = clamp(gy - float(y0), 0.0, 1.0);

bool useCache = cellCacheOk &&
                cx >= minCx && cx <= maxCx && cy >= minCy && cy <= maxCy &&
                x0 >= minCx && x1 <= maxCx && y0 >= minCy && y1 <= maxCy;
bool cellVetoed = false;
if (useCache)
{
    cellVetoed = (td_c4b_veto[(cy - minCy) * spanX + (cx - minCx)] != 0u);
}
else
{
    int expFocusBits = int(round(texelFetch(cell_prior, ivec2(cx, cy), 0).y));
    cellVetoed = ((u_has_face_mask > 0.5) && (texelFetch(face_mask, ivec2(cx, cy), 0).x > 0.5)) ||
                 ((expFocusBits & 1) != 0) || (texelFetch(cell_own_grain, ivec2(cx, cy), 0).w > 0.5) ||
                 (((expFocusBits & 2) != 0) && !bokehAllowL);
}
if (cellVetoed)
{
    imageStore(out_coded_delta, p, vec4(0.0));
    return;
}

vec2 ab00, ab10, ab01, ab11;
if (useCache)
{
    ab00 = td_c4b_ab[(y0 - minCy) * spanX + (x0 - minCx)];
    ab10 = td_c4b_ab[(y0 - minCy) * spanX + (x1 - minCx)];
    ab01 = td_c4b_ab[(y1 - minCy) * spanX + (x0 - minCx)];
    ab11 = td_c4b_ab[(y1 - minCy) * spanX + (x1 - minCx)];
}
else
{
    vec4 g00 = texelFetch(guard_cell, ivec2(x0, y0), 0);
    vec4 g10 = texelFetch(guard_cell, ivec2(x1, y0), 0);
    vec4 g01 = texelFetch(guard_cell, ivec2(x0, y1), 0);
    vec4 g11 = texelFetch(guard_cell, ivec2(x1, y1), 0);
    ab00 = vec2(((int(round(g00.y)) & 1) != 0) ? max(0.0, g00.x) : 0.0, ((int(round(g00.w)) & 1) != 0) ? max(0.0, g00.z) : 0.0);
    ab10 = vec2(((int(round(g10.y)) & 1) != 0) ? max(0.0, g10.x) : 0.0, ((int(round(g10.w)) & 1) != 0) ? max(0.0, g10.z) : 0.0);
    ab01 = vec2(((int(round(g01.y)) & 1) != 0) ? max(0.0, g01.x) : 0.0, ((int(round(g01.w)) & 1) != 0) ? max(0.0, g01.z) : 0.0);
    ab11 = vec2(((int(round(g11.y)) & 1) != 0) ? max(0.0, g11.x) : 0.0, ((int(round(g11.w)) & 1) != 0) ? max(0.0, g11.z) : 0.0);
}
vec2 topAb = ab00 + (ab10 - ab00) * tx;
vec2 botAb = ab01 + (ab11 - ab01) * tx;
vec2 addBack = topAb + (botAb - topAb) * ty;

vec2 outD = texelFetch(coded_delta_raw, p, 0).xy;
vec2 fineDiff = texelFetch(fine_diff, p, 0).xy;
if (td_is_bad(outD.x)) outD.x = 0.0;
if (td_is_bad(outD.y)) outD.y = 0.0;
if (td_is_bad(fineDiff.x)) fineDiff.x = 0.0;
if (td_is_bad(fineDiff.y)) fineDiff.y = 0.0;
outD += addBack * fineDiff;
if (u_fade_keep < 1.0) outD *= u_fade_keep;
imageStore(out_coded_delta, p, vec4(outD, 0.0, 0.0));
)GLSL";

		// ---- CHR-HIGH-3: Zero cell diagnostics cleanly when reuseLumaWeightsGpu skips Pass C2A/C2B ----
		constexpr const char* kPassChromaZeroDiag = R"GLSL(
ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
if (cell.x >= TD_CW || cell.y >= TD_CH)
    return;
imageStore(cell_diag_flags, cell, vec4(0.0));
imageStore(warp_diag, cell, vec4(0.0));
)GLSL";
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
		bool transformIdentified8x8 = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool isCutOrSpanInvalid = false;
		double fadeKeep = 1.0;                   // Upstream U7 gradual retention keep in [0, 1] (default 1.0 = bit-identical; <=0 or NaN closes delta to 0)
		PsdShape psdShape;                       // Optional P10g grain PSD shape multipliers (default 1.0 white-noise identity)

		// Optional precomputed luma cell weights & diagonal variance textures (3*cw x ch RGBA32F) for Plan 6.6
		// (`cfg_.chromaReuseLumaWeights`): when enabled and valid, skips Pass C2A & C2B completely!
		pl_tex lumaCellWeightsTex = nullptr;
		pl_tex lumaCellDiagCTex = nullptr;
		const RefRole* lumaRefRoles = nullptr;
		int lumaActiveRefCount = 0;
		QualityPattern::FrameRole frameRole = QualityPattern::FrameRole::Unknown;
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

		// OPUS-M1 (format-agnostic Unidentified): ScratchArena::Acquire matches TexShape by strict (w, h, fmt) equality,
		// so a format-agnostic Unidentified arena must include the deduplicated union of coded-grid shapes across all
		// possible delivered formats {Yuv420 (6 RG32F), Yuv422 (6 RG32F), Yuv444/Rgb444 (9 RG32F)} plus one shared cell-grid set.
		inline std::vector<VpSkeleton::ArenaRequest> BuildUnidentifiedAgnosticArenaRequests(
			int lumaW, int lumaH, int cellPx = kCanonicalBlockPx)
		{
			std::vector<VpSkeleton::ArenaRequest> reqs;
			if (lumaW <= 0 || lumaH <= 0)
				return reqs;
			const int effCellPx = cellPx > 0 ? cellPx : kCanonicalBlockPx;
			const uint32_t ucw = static_cast<uint32_t>((lumaW + effCellPx - 1) / effCellPx);
			const uint32_t uch = static_cast<uint32_t>((lumaH + effCellPx - 1) / effCellPx);

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

			// Deduplicate identical (cW, cH) dims (e.g. lumaW == 1 or lumaH == 1) using max(rgCount) so PlanArena
			// never double-counts mutually exclusive coded-grid pools.
			struct CodedGridSpec { uint32_t w; uint32_t h; uint8_t rgCount; };
			CodedGridSpec specs[3] = {};
			int specCount = 0;
			const ChromaSubsampling kSubs[3] = {
				ChromaSubsampling::Yuv420,
				ChromaSubsampling::Yuv422,
				ChromaSubsampling::Yuv444
			};
			const uint8_t kRgCounts[3] = { 6u, 6u, 9u };
			for (int i = 0; i < 3; ++i)
			{
				const uint32_t rw = static_cast<uint32_t>(ChromaDetail::SubsampledDim(lumaW, SubsampleX(kSubs[i])));
				const uint32_t rh = static_cast<uint32_t>(ChromaDetail::SubsampledDim(lumaH, SubsampleY(kSubs[i])));
				bool merged = false;
				for (int j = 0; j < specCount; ++j)
				{
					if (specs[j].w == rw && specs[j].h == rh)
					{
						if (kRgCounts[i] > specs[j].rgCount)
							specs[j].rgCount = kRgCounts[i];
						merged = true;
						break;
					}
				}
				if (!merged)
					specs[specCount++] = CodedGridSpec{ rw, rh, kRgCounts[i] };
			}

			for (int i = 0; i < specCount; ++i)
			{
				pushReq(specs[i].w, specs[i].h, VpSkeleton::TexFormat::RG32F, specs[i].rgCount);
				pushReq(specs[i].w, specs[i].h, VpSkeleton::TexFormat::RGBA32F, 6u);
				pushReq(specs[i].w, specs[i].h, VpSkeleton::TexFormat::R16, 2u);
			}

			// Single shared cell-grid set (identical across all delivered formats).
			pushReq(3u * ucw, uch, VpSkeleton::TexFormat::RGBA32F, 5u);
			pushReq(3u * ucw, 3u * uch, VpSkeleton::TexFormat::RGBA32F, 2u);
			pushReq(ucw, uch, VpSkeleton::TexFormat::RG32F, 1u);
			pushReq(ucw, uch, VpSkeleton::TexFormat::RGBA32F, 3u);
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
		void Reset()
		{
			a2eHistoryCb_.Reset();
			a2eHistoryCr_.Reset();
		}

		void Release(pl_gpu gpu)
		{
			a2eHistoryCb_.Reset();
			a2eHistoryCr_.Reset();
			if (gpu != nullptr)
			{
				DestroyPrivateScratch(gpu);
				for (pl_tex* t : {
					&inRecUvTex_, &inRecRgbTex_, &inCleanUvTex_, &inRefUvTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_, &inFaceMaskTex_,
					&outCodedDeltaTex_, &outDelivUvTex_, &outDelivRgbTex_, &outDelivDeltaTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_, &ebPriorTex_, &ebScratchTex_, &chProjIrTex_, &inLumaCellWeightsTex_ })
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
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_, &ebPriorTex_, &ebScratchTex_, &chProjIrTex_, &inLumaCellWeightsTex_ })
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
				outDiagFlagsTex_, outGuardCellTex_, outWarpDiagTex_, ebPriorTex_, ebScratchTex_, chProjIrTex_, inLumaCellWeightsTex_ })
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
			pl_gpu gpu, int lumaW, int lumaH, ChromaSubsampling codedSubsampling, int cellPx = kCanonicalBlockPx,
			ChromaGridStatus gridStatus = ChromaGridStatus::Identified)
		{
			if (!gpu || lumaW <= 0 || lumaH <= 0)
				return {};
			if (!VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F))
			{
				return {};
			}
			// OPUS-M1: When gridStatus == Unidentified and deliveredFormat is unspecified, emit the deduplicated union of
			// {Yuv420, Yuv422, Yuv444/Rgb444} coded-grid shapes (ScratchArena::Acquire matches TexShape by exact equality)
			// so arena-backed Unidentified frames of ANY delivered format never hit GpuScratchShortage.
			if (gridStatus == ChromaGridStatus::Unidentified)
				return ChromaGpuDetail::BuildUnidentifiedAgnosticArenaRequests(lumaW, lumaH, cellPx);
			return ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, codedSubsampling, cellPx, true);
		}

		// TD-AUDIT-L2 + OPUS-M1: Format-aware ArenaRequests omits the 3 coded-grid reduction scratch textures (-47..190 MiB at 4K!)
		// whenever deliveredFormat is native YUV matching the effective coded grid, and sizes Unidentified grids for full delivered resolution.
		static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(
			pl_gpu gpu, int lumaW, int lumaH, ChromaSubsampling codedSubsampling,
			DeliveredChromaFormat deliveredFormat, int cellPx = kCanonicalBlockPx,
			ChromaGridStatus gridStatus = ChromaGridStatus::Identified)
		{
			if (!gpu || lumaW <= 0 || lumaH <= 0)
				return {};
			if (!VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F))
			{
				return {};
			}
			const ChromaSubsampling delivSub = DeliveredSubsampling(deliveredFormat);
			const ChromaSubsampling effCodedSub =
				(gridStatus == ChromaGridStatus::Unidentified) ? delivSub : codedSubsampling;
			const bool needReduce = (deliveredFormat == DeliveredChromaFormat::Rgb444) || (effCodedSub != delivSub);
			return ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, effCodedSub, cellPx, needReduce);
		}

		static uint64_t EstimatedScratchVramBytes(
			int lumaW, int lumaH, ChromaSubsampling codedSubsampling, int cellPx = kCanonicalBlockPx,
			ChromaGridStatus gridStatus = ChromaGridStatus::Identified)
		{
			const std::vector<VpSkeleton::ArenaRequest> reqs =
				(gridStatus == ChromaGridStatus::Unidentified)
					? ChromaGpuDetail::BuildUnidentifiedAgnosticArenaRequests(lumaW, lumaH, cellPx)
					: ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, codedSubsampling, cellPx, true);
			uint64_t total = 0;
			for (const VpSkeleton::ArenaRequest& r : reqs)
				total += VpSkeleton::ShapeBytes(r.shape) * static_cast<uint64_t>(r.count);
			return total;
		}

		static uint64_t EstimatedScratchVramBytesFor(
			int lumaW, int lumaH, ChromaSubsampling codedSubsampling,
			DeliveredChromaFormat deliveredFormat, int cellPx = kCanonicalBlockPx,
			ChromaGridStatus gridStatus = ChromaGridStatus::Identified)
		{
			const ChromaSubsampling delivSub = DeliveredSubsampling(deliveredFormat);
			const ChromaSubsampling effCodedSub =
				(gridStatus == ChromaGridStatus::Unidentified) ? delivSub : codedSubsampling;
			const bool needReduce = (deliveredFormat == DeliveredChromaFormat::Rgb444) || (effCodedSub != delivSub);
			uint64_t total = 0;
			for (const VpSkeleton::ArenaRequest& r : ChromaGpuDetail::BuildChromaArenaRequests(lumaW, lumaH, effCodedSub, cellPx, needReduce))
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
			if (effectiveCut)
			{
				a2eHistoryCb_.Reset();
				a2eHistoryCr_.Reset();
			}

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
					const bool hasFb = mf.fbErrorPx.size() == numCells;
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
							mmBuf[dst * 4u + 3u] = hasFb ? static_cast<float>(mf.fbErrorPx[srcIdx]) : 0.0f;
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
			const float uMotionMetaHasFb = (motionMetaComps >= 4) ? 1.0f : 0.0f;
			const float uEnableHardFbGate = cfg_.enableHardFbOcclusionGate ? 1.0f : 0.0f;

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
				mgHdr += VpvGpu::Define("TD_HARD_FB_OCCLUSION_PX", kHardFbOcclusionPx);
				mgHdr += Shaders::kCoreHelpers;
				mgHdr += Shaders::kPass1MotionGateHeader;
				pl_shader_desc dmg[3] = {
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("motion_meta", motionMetaTex),
					VpvGpu::Storage("motion_gate", sc.motionGate)
				};
				pl_shader_var vmg[7] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefSlot0),
					VpvGpu::Var("u_ref_motion_slot_1", &uRefSlot1),
					VpvGpu::Var("u_ref_motion_slot_2", &uRefSlot2),
					VpvGpu::Var("u_motion_meta_has_conf", &uMotionMetaHasConf),
					VpvGpu::Var("u_motion_meta_has_fb", &uMotionMetaHasFb),
					VpvGpu::Var("u_enable_hard_fb_gate", &uEnableHardFbGate)
				};
				if (!runPass("td_v2_c1_motion_gate", mgHdr, Shaders::kPass1MotionGate, dmg, 3, vmg, 7,
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
			const std::string cBand2DHdr = coarseOnly ? std::string() : (chromaGeom + cBandTaps + Shaders::kCoreHelpers + ChromaShaders::kPassChromaBand2DHeader);
			const size_t cBandTile = static_cast<size_t>(16 + 2 * rBase);
			const size_t shBand2D = (cBandTile * (cBandTile + 1u) * 2u + cBandTile * 17u * 4u) * sizeof(float);

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
				// Chroma Fusion 1 (TD-GPU-OPT-2): 1-pass 2D LDS separable band split (eliminates tmp_cb RGBA32F round-trip!).
				pl_shader_var rv[4] = {
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1)
				};
				pl_shader_desc d2d[3] = {
					VpvGpu::Sampled("src_uv", srcUv),
					VpvGpu::Storage("bands_l", dstL),
					VpvGpu::Storage("bands_mh", dstMH)
				};
				return runPass("td_v2_c1_band_2d", cBand2DHdr, ChromaShaders::kPassChromaBand2D, d2d, 3, rv, 4, cGx, cGy, 16, 16, shBand2D);
			};

			if (!runChromaBands(codedCleanTex, sc.curL, sc.curMH))
				return failVisible("td_v2_c1_band current chroma failed", StandInKind::GpuDispatchFailed);

			// ---- PASS C1A + C1B/C: Warp each reference + Band Split ----
			const std::string cWarpHdr = chromaGeom +
				VpvGpu::DefineInt("TD_CLS_CAMERA", static_cast<int>(RegionMotionClass::Camera)) +
				Shaders::kCoreHelpers + ChromaShaders::kSitedCoordHelpers + ChromaShaders::kPassChromaWarpHeader;
			const size_t shChWarp = (24u * 24u * 3u + 64u + 4u) * sizeof(float);
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
				const float uEnableSmoothWarp = cfg_.enableSmoothWarpVectors ? 1.0f : 0.0f;
				pl_shader_var wv[21] = {
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_ref_idx", &uRefIdx), VpvGpu::Var("u_motion_slot", &uMotSlot),
					VpvGpu::Var("u_flags_clear", &uFlagsClear), VpvGpu::Var("u_same_ref_cur", &uSameRefCur),
					VpvGpu::Var("u_use_homography", &uUseH),
					VpvGpu::Var("u_enable_smooth_warp", &uEnableSmoothWarp),
					VpvGpu::Var("u_h0", &uH[0]), VpvGpu::Var("u_h1", &uH[1]), VpvGpu::Var("u_h2", &uH[2]),
					VpvGpu::Var("u_h3", &uH[3]), VpvGpu::Var("u_h4", &uH[4]), VpvGpu::Var("u_h5", &uH[5]),
					VpvGpu::Var("u_h6", &uH[6]), VpvGpu::Var("u_h7", &uH[7]), VpvGpu::Var("u_h8", &uH[8])
				};
				if (!runPass("td_v2_c1_warp", cWarpHdr, ChromaShaders::kPassChromaWarp, dw, 7, wv, 21, cGx, cGy, 16, 16, shChWarp))
					return failVisible("td_v2_c1_warp dispatch failed", StandInKind::GpuDispatchFailed);
				curFlagsSlot = dstSlot;

				if (!runChromaBands(sc.warpedUv, sc.refL[k], sc.refMH[k]))
					return failVisible("td_v2_c1_band ref chroma failed", StandInKind::GpuDispatchFailed);
			}
			pl_tex finalFlagsTex = sc.refFlags[curFlagsSlot];

			// Plan 6.6 / CHR-HIGH-3 / RED-BUG-6 (`cfg_.chromaReuseLumaWeights`):
			// Never apply on Branch A (`coarseOnly == true`), strictly map chroma refs by RefRole,
			// and ALWAYS compute chroma's own residual variance (sc.diagCb/Cr) & blockOwnU without requiring lumaCellDiagCTex!
			const bool reuseLumaWeightsGpu =
				!coarseOnly &&
				cfg_.chromaReuseLumaWeights &&
				in.lumaCellWeightsTex != nullptr &&
				in.lumaCellWeightsTex->params.w == 3 * cw && in.lumaCellWeightsTex->params.h == ch;
			int lumaSlotForChroma[3] = { -1, -1, -1 };
			if (reuseLumaWeightsGpu)
			{
				const int lumaK = (in.lumaRefRoles != nullptr && in.lumaActiveRefCount > 0)
					? (std::min)(in.lumaActiveRefCount, 3)
					: 3;
				for (int k = 0; k < K; ++k)
				{
					// PROD-GPU-BUG-3: Canonical RefRole (0=PrevPeak, 1=NextPeak, 2=PrevPeak2, 3=Carry).
					const int rInt = refSpecs[static_cast<size_t>(k)].refRole;
					const RefRole cRole = (rInt == 0) ? RefRole::PrevPeak
						: ((rInt == 1) ? RefRole::NextPeak : ((rInt == 2) ? RefRole::PrevPeak2 : RefRole::Carry));
					if (in.lumaRefRoles != nullptr && in.lumaActiveRefCount > 0)
					{
						for (int lk = 0; lk < lumaK; ++lk)
						{
							if (in.lumaRefRoles[lk] == cRole)
							{
								lumaSlotForChroma[k] = lk;
								break;
							}
						}
					}
					else if (k < 3)
					{
						lumaSlotForChroma[k] = k;
					}
				}
			}
			const float uReuseLumaWeights = reuseLumaWeightsGpu ? 1.0f : 0.0f;
			const double maxAllowedStepScale = cfg_.enableAqStepScaling ? kCellStepScaleMax : 1.0;

			// Derived physics for standalone chroma focus guard (CHR-MEDIUM-3) & M+H activity classification (CHR-HIGH-1).
			const double dScaleCh = DepthScale(cfg_.sourceBits);
			const double baseQuantVarCh = (dScaleCh * dScaleCh) / 12.0;
			const double stepNoiseVarCh = a2.stats.valid ? (std::max)(0.0, a2.stats.totalVar) : 0.0;
			const double effNoiseVarCh = (std::max)(baseQuantVarCh, stepNoiseVarCh);
			const double gainMD3Ch = gains.mid + gains.d3;
			const double structureFloorCh = 2.0 * gainMD3Ch * effNoiseVarCh;
			const double fSharpCh = AnalyticalEdgeFocusRatio(0.0);
			const double fDefocusCh = AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase);
			const double fShiftDeltaCh = fSharpCh - fDefocusCh;
			const float uStepNoiseVarCh = static_cast<float>(stepNoiseVarCh);
			const float uStructureFloorCh = static_cast<float>(structureFloorCh);
			const float uHasExplicitFocus = (in.explicitFocusChangeCells != nullptr) ? 1.0f : 0.0f;
			const float uActivityTexFloor = static_cast<float>(2.0 * (gains.mid + gains.fine) * baseQuantVarCh);
			const float uPoolRL = static_cast<float>(cfg_.PoolRadiusForBand(0));
			const float uPoolRM = static_cast<float>(cfg_.PoolRadiusForBand(1));
			const float uPoolRH = static_cast<float>(cfg_.PoolRadiusForBand(2));
			const float uEnableEb = cfg_.enableEmpiricalBayesShrinkage ? 1.0f : 0.0f;
			const float uUseA2ePrior = (!in.hasStepPrior && cfg_.enableA2eEmpiricalPrior && !in.transformIdentified8x8) ? 1.0f : 0.0f;
			const float uUseEmpiricalU = (cfg_.enableA2eEmpiricalPrior && (coarseOnly || !in.transformIdentified8x8)) ? 1.0f : 0.0f;

			// Always ensure 6x2 RGBA32F ebPriorTex_, 3*cw x 2*ch RGBA32F ebScratchTex_, & 2*cw x 2*ch RGBA32F chProjIrTex_ are allocated!
			if (!Detail::EnsureTex(gpu, ebPriorTex_, 6, 2, fmtRGBA32, true, true, true, true) ||
			    !Detail::EnsureTex(gpu, ebScratchTex_, 3 * cw, 2 * ch, fmtRGBA32, true, true, false, false) ||
			    !Detail::EnsureTex(gpu, chProjIrTex_, 2 * cw, 2 * ch, fmtRGBA32, true, true, false, false))
				return failVisible("TdV2ChromaGpuEngine ebPrior/ebScratch/chProjIr alloc failed", StandInKind::GpuDispatchFailed);

			// ---- PASS C2A: LDS Cell-Moment Reduction + Standalone Focus Fallback for Cb & Cr ----
			const bool sameCleanRec = (codedCleanTex == codedRecTex);
			const float uSameCleanRec = sameCleanRec ? 1.0f : 0.0f;
			{
				std::string c2aHdr = chromaGeom;
				c2aHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				c2aHdr += VpvGpu::Define("TD_GAIN_D3", gains.d3);
				c2aHdr += VpvGpu::Define("TD_F_SHARP", fSharpCh);
				c2aHdr += VpvGpu::Define("TD_F_SHIFT_DELTA", fShiftDeltaCh);
				c2aHdr += VpvGpu::Define("TD_HM_PEAK", midPeak);
				c2aHdr += VpvGpu::Define("TD_PIXEL_BOUND_SIGMA", std::sqrt(kHuberKneeSq));
				c2aHdr += VpvGpu::Define("TD_BASE_QUANT_VAR", baseQuantVarCh);
				c2aHdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				c2aHdr += VpvGpu::Define("TD_STEP_SCALE_MAX", maxAllowedStepScale);
				c2aHdr += Shaders::kCoreHelpers;
				c2aHdr += ChromaShaders::kSitedCoordHelpers;
				c2aHdr += ChromaShaders::kPassChromaCellStatsHeader;
				pl_shader_desc d2a[19] = {
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
					VpvGpu::Storage("warp_diag", dstWarpDiag),
					VpvGpu::Storage("proj_ir", chProjIrTex_)
				};
				pl_shader_var v2a[16] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_has_clean_split", &uHasSplit),
					VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode),
					VpvGpu::Var("u_has_explicit_focus", &uHasExplicitFocus),
					VpvGpu::Var("u_coarse_only", &uCoarseOnly),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep),
					VpvGpu::Var("u_step_noise_var", &uStepNoiseVarCh),
					VpvGpu::Var("u_structure_floor", &uStructureFloorCh)
				};
				const size_t sh2a = (1950u + 480u + 64u + 60u + 8u) * sizeof(float);
				const std::string c2aBody = ChromaShaders::BuildPassChromaCellStatsBody();
				if (!runPass("td_v2_c2a_cell_stats", c2aHdr, c2aBody.c_str(), d2a, 19, v2a, 16, cw, ch, 8, 8, sh2a))
					return failVisible("td_v2_c2a_cell_stats dispatch failed", StandInKind::GpuDispatchFailed);

				// ---- PASS C2EB (PROD-GPU-BUG-1 / PROD-GPU-BUG-4 / PROD-GPU-BUG-18): Parallel Prep + Torben 24-Step Median ----
				std::string c2ebPrepHdr = chromaGeom;
				c2ebPrepHdr += VpvGpu::Define("TD_GAIN_LOW", effLowGain);
				c2ebPrepHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				c2ebPrepHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				c2ebPrepHdr += VpvGpu::Define("TD_ORIGIN_LOW", effLowOrigin);
				c2ebPrepHdr += VpvGpu::Define("TD_ORIGIN_MID", gains.originMid);
				c2ebPrepHdr += VpvGpu::Define("TD_ORIGIN_FINE", gains.originFine);
				c2ebPrepHdr += VpvGpu::Define("TD_A2_PEAK_MID_SQ", a2PeakMidSq);
				c2ebPrepHdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				c2ebPrepHdr += VpvGpu::Define("TD_TRACE_REG", kDefaultTraceRegScale);
				c2ebPrepHdr += VpvGpu::Define("TD_SOLVE_FLOOR", kSolveNumericalFloor);
				c2ebPrepHdr += VpvGpu::Define("TD_PIVOT_FLOOR", kSolvePivotFloor);
				c2ebPrepHdr += VpvGpu::Define("TD_BASE_QUANT_VAR", baseQuantVarCh);
				c2ebPrepHdr += Shaders::BuildFineTapHeader();
				c2ebPrepHdr += Shaders::kCoreHelpers;
				c2ebPrepHdr += Shaders::kPass2MergeSolveHeader;
				c2ebPrepHdr += ChromaShaders::kChromaProjIrHelpers;

				pl_shader_desc d2ebPrep[9] = {
					VpvGpu::Sampled("cell_stats_cb", sc.statsCb),
					VpvGpu::Sampled("cell_stats_cr", sc.statsCr),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Sampled("cell_own_grain", sc.ownGrain),
					VpvGpu::Sampled("proj_ir", chProjIrTex_),
					VpvGpu::Storage("eb_scratch", ebScratchTex_)
				};
				pl_shader_var v2ebPrep[23] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_pool_r_l", &uPoolRL), VpvGpu::Var("u_pool_r_m", &uPoolRM), VpvGpu::Var("u_pool_r_h", &uPoolRH),
					VpvGpu::Var("u_ref_role_0", &uRefRole0), VpvGpu::Var("u_ref_role_1", &uRefRole1), VpvGpu::Var("u_ref_role_2", &uRefRole2),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefSlot0), VpvGpu::Var("u_ref_motion_slot_1", &uRefSlot1), VpvGpu::Var("u_ref_motion_slot_2", &uRefSlot2),
					VpvGpu::Var("u_exact_copy_0", &uExact0), VpvGpu::Var("u_exact_copy_1", &uExact1), VpvGpu::Var("u_exact_copy_2", &uExact2),
					VpvGpu::Var("u_has_clean_split", &uHasSplit), VpvGpu::Var("u_bokeh_mode", &uBokehMode),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_cr_grain", &uPriorHasCrGrain),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep),
					VpvGpu::Var("u_psd_low", &uPsdLow), VpvGpu::Var("u_psd_mid", &uPsdMid), VpvGpu::Var("u_psd_fine", &uPsdFine),
					VpvGpu::Var("u_coarse_only", &uCoarseOnly),
					VpvGpu::Var("u_activity_tex_floor", &uActivityTexFloor)
				};
				if (!runPass("td_v2_c2eb_prep", c2ebPrepHdr, ChromaShaders::kPassChromaEbPriorPrep, d2ebPrep, 9, v2ebPrep, 23,
				             Detail::GroupsFor(cw, 8), Detail::GroupsFor(ch, 8), 8, 8))
					return failVisible("td_v2_c2eb_prep dispatch failed", StandInKind::GpuDispatchFailed);

				std::string c2ebRedHdr = chromaGeom + ChromaShaders::kPassChromaEbPriorHeader;
				pl_shader_desc d2eb[2] = {
					VpvGpu::Sampled("eb_scratch", ebScratchTex_),
					VpvGpu::Storage("eb_prior", ebPriorTex_)
				};
				const size_t sh2eb = (256u * 6u + 12u) * sizeof(float);
				if (!runPass("td_v2_c2eb_prior_reduce", c2ebRedHdr, ChromaShaders::kPassChromaEbPriorReduce, d2eb, 2, nullptr, 0, 6, 2, 16, 16, sh2eb))
					return failVisible("td_v2_c2eb_prior_reduce dispatch failed", StandInKind::GpuDispatchFailed);

				// Apply CPU-identical empty-class fallback + temporal rolling-median A2e cap & one-sided K=1 hold on host (192 B!).
				std::vector<float> ebBuf(6u * 2u * 4u, 0.0f);
				if (!Detail::DownloadFloats(gpu, ebPriorTex_, ebBuf.data()))
					return failVisible("TdV2ChromaGpuEngine ebPriorTex download failed", StandInKind::GpuDispatchFailed);
				for (int chIdx = 0; chIdx < 2; ++chIdx)
				{
					A2ePriorHistory& hist = (chIdx == 0) ? a2eHistoryCb_ : a2eHistoryCr_;
					const float savedUnsplitG = ebBuf[static_cast<size_t>(chIdx * 6) * 4u + 3u];
					EmpiricalOwnErrorPrior rawA2e;
					float tauGrid[3][2] = {};
					bool anyV = false;
					for (int b = 0; b < 3; ++b)
					{
						for (int ac = 0; ac < 2; ++ac)
						{
							const size_t idx = static_cast<size_t>(chIdx * 6 + b * 2 + ac) * 4u;
							const float uVal = ebBuf[idx + 0u];
							const float tVal = ebBuf[idx + 1u];
							const float cVal = ebBuf[idx + 2u];
							if (cVal > 0.5f && std::isfinite(uVal) && uVal >= 0.0f)
							{
								rawA2e.U[b][ac] = static_cast<double>(uVal);
								rawA2e.cellCount[b][ac] = static_cast<uint32_t>(Detail::NonNegRoundToSize(cVal));
								tauGrid[b][ac] = (std::isfinite(tVal) && tVal > 0.0f) ? tVal : 0.0f;
								anyV = true;
							}
						}
						if (rawA2e.cellCount[b][0] == 0 && rawA2e.cellCount[b][1] > 0)
							rawA2e.U[b][0] = rawA2e.U[b][1];
						else if (rawA2e.cellCount[b][1] == 0 && rawA2e.cellCount[b][0] > 0)
							rawA2e.U[b][1] = rawA2e.U[b][0];
						if (rawA2e.cellCount[b][0] < 2 && rawA2e.cellCount[b][1] >= 2)
							tauGrid[b][0] = tauGrid[b][1];
						else if (rawA2e.cellCount[b][1] < 2 && rawA2e.cellCount[b][0] >= 2)
							tauGrid[b][1] = tauGrid[b][0];
					}
					rawA2e.valid = anyV;
					const EmpiricalOwnErrorPrior effA2e = hist.ApplyCapAndRecord(
						rawA2e, in.frameRole, cfg_.sourceHz, (effFadeKeep < 1.0), false);
					for (int b = 0; b < 3; ++b)
					{
						for (int ac = 0; ac < 2; ++ac)
						{
							const size_t idx = static_cast<size_t>(chIdx * 6 + b * 2 + ac) * 4u;
							const double effU = effA2e.valid ? effA2e.ForBandClass(b, ac) : 0.0;
							const bool cellOk = effA2e.valid && std::isfinite(effU) && effU >= 0.0;
							ebBuf[idx + 0u] = cellOk ? static_cast<float>(effU) : 0.0f;
							ebBuf[idx + 1u] = (cellOk && !effA2e.heldFromHistory && std::isfinite(tauGrid[b][ac]) && tauGrid[b][ac] > 0.0f)
								? tauGrid[b][ac] : 0.0f;
							ebBuf[idx + 2u] = cellOk ? static_cast<float>((std::max<uint32_t>)(1u, effA2e.cellCount[b][ac])) : 0.0f;
							ebBuf[idx + 3u] = (b == 0 && ac == 0) ? savedUnsplitG : 0.0f;
						}
					}
				}
				if (!Detail::UploadFloats(gpu, ebPriorTex_, ebBuf.data()))
					return failVisible("TdV2ChromaGpuEngine ebPriorTex capped upload failed", StandInKind::GpuDispatchFailed);
			}

			// ---- PASS C2B: Chan-Pebay Merge + Chroma cellDiagC/blockOwnU + Active-Set Solve (skipped when reuseLumaWeightsGpu) ----
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
				c2bHdr += VpvGpu::Define("TD_STEP_SCALE_MAX", maxAllowedStepScale);
				c2bHdr += VpvGpu::Define("TD_BASE_QUANT_VAR", baseQuantVarCh);
				c2bHdr += Shaders::BuildFineTapHeader();
				c2bHdr += Shaders::kCoreHelpers;
				c2bHdr += Shaders::kPass2MergeSolveHeader;
				c2bHdr += Shaders::BuildPass2SolverHeader();
				c2bHdr += ChromaShaders::kChromaProjIrHelpers;

				float uSolverMode = (cfg_.solverMode == SolverMode::LegacyClipRescale) ? 1.0f : 0.0f;

				pl_shader_desc d2b[14] = {
					VpvGpu::Sampled("cell_stats_cb", sc.statsCb),
					VpvGpu::Sampled("cell_stats_cr", sc.statsCr),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Sampled("cell_own_grain", sc.ownGrain),
					VpvGpu::Sampled("eb_prior", ebPriorTex_),
					VpvGpu::Sampled("proj_ir", chProjIrTex_),
					VpvGpu::Storage("cell_weights_cb", sc.weightsCb),
					VpvGpu::Storage("cell_weights_cr", sc.weightsCr),
					VpvGpu::Storage("cell_diag_cb", sc.diagCb),
					VpvGpu::Storage("cell_diag_cr", sc.diagCr),
					VpvGpu::Storage("cell_diag_flags", dstDiagFlags)
				};
				pl_shader_var v2b[33] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_pool_r_l", &uPoolRL), VpvGpu::Var("u_pool_r_m", &uPoolRM), VpvGpu::Var("u_pool_r_h", &uPoolRH),
					VpvGpu::Var("u_estimator", &uEstimator),
					VpvGpu::Var("u_solver_mode", &uSolverMode),
					VpvGpu::Var("u_ref_role_0", &uRefRole0), VpvGpu::Var("u_ref_role_1", &uRefRole1), VpvGpu::Var("u_ref_role_2", &uRefRole2),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefSlot0), VpvGpu::Var("u_ref_motion_slot_1", &uRefSlot1), VpvGpu::Var("u_ref_motion_slot_2", &uRefSlot2),
					VpvGpu::Var("u_exact_copy_0", &uExact0), VpvGpu::Var("u_exact_copy_1", &uExact1), VpvGpu::Var("u_exact_copy_2", &uExact2),
					VpvGpu::Var("u_a2_valid", &uA2Valid), VpvGpu::Var("u_a2_sigma_sent", &uA2Sent), VpvGpu::Var("u_a2_sigma_zero", &uA2Zero),
					VpvGpu::Var("u_a2_total_var", &uA2Total),
					VpvGpu::Var("u_has_clean_split", &uHasSplit), VpvGpu::Var("u_bokeh_mode", &uBokehMode),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep),
					VpvGpu::Var("u_cell_prior_has_cr_grain", &uPriorHasCrGrain),
					VpvGpu::Var("u_psd_low", &uPsdLow),
					VpvGpu::Var("u_psd_mid", &uPsdMid),
					VpvGpu::Var("u_psd_fine", &uPsdFine),
					VpvGpu::Var("u_coarse_only", &uCoarseOnly),
					VpvGpu::Var("u_enable_eb", &uEnableEb),
					VpvGpu::Var("u_use_a2e_prior", &uUseA2ePrior),
					VpvGpu::Var("u_use_empirical_u", &uUseEmpiricalU),
					VpvGpu::Var("u_activity_tex_floor", &uActivityTexFloor),
					VpvGpu::Var("u_reuse_luma_weights", &uReuseLumaWeights)
				};
				const std::string c2bBody = ChromaShaders::BuildPassChromaMergeSolveBody();
				if (!runPass("td_v2_c2b_merge_solve", c2bHdr, c2bBody.c_str(), d2b, 14, v2b, 33,
				             Detail::GroupsFor(cw, 8), Detail::GroupsFor(2 * ch, 8), 8, 8))
				{
					return failVisible("td_v2_c2b_merge_solve dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS C3 + C4: Fused Coded-Grid Blend + Bound + Layer-4 TdGuardPort (Chroma Fusion 2, TD-GPU-OPT-2) ----
			// Used for all !coarseOnly frames when smoothGuard is enabled OR canonical 4:2:0 (8x8 coded cells);
			// eliminates c3_blend_bound dispatch + 14x14 halo reads of coded_delta_raw & cur_uv!
			{
				const bool smoothGuard = cfg_.enableSmoothLayer4Guard;
				const float uStoreFineDiff = smoothGuard ? 1.0f : 0.0f;
				const float uPixelBoundKappa = static_cast<float>(ClampPixelBoundKappa(cfg_.pixelBoundKappa));
				const float uEnableJointBound = cfg_.enableJointCrossBandBound ? 1.0f : 0.0f;
				const float uLumaSlot0 = static_cast<float>(lumaSlotForChroma[0]);
				const float uLumaSlot1 = static_cast<float>(lumaSlotForChroma[1]);
				const float uLumaSlot2 = static_cast<float>(lumaSlotForChroma[2]);
				const size_t fineTile = static_cast<size_t>(8 + 2 * rFine);
				pl_tex c4FaceBind = hasFaceMask ? faceMaskTex : sc.bandTmpCb;
				const bool fuseC34 = !coarseOnly && (smoothGuard || (cSubX == 2 && cSubY == 2 && cellPx == kCanonicalBlockPx));

				if (fuseC34)
				{
					std::string c34Hdr = chromaGeom;
					c34Hdr += VpvGpu::DefineInt("TD_C34_HAS_REC_UV", sameCleanRec ? 0 : 1);
					c34Hdr += Shaders::BuildFineTapHeader();
					c34Hdr += VpvGpu::Define("TD_GAIN_LOW", effLowGain);
					c34Hdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
					c34Hdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
					c34Hdr += VpvGpu::Define("TD_HUBER_KNEE_SQ", kHuberKneeSq);
					c34Hdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
					c34Hdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
					c34Hdr += VpvGpu::Define("TD_STEP_SCALE_MAX", maxAllowedStepScale);
					c34Hdr += Shaders::kCoreHelpers;
					c34Hdr += ChromaShaders::kSitedCoordHelpers;
					c34Hdr += ChromaShaders::kPassChromaBlendGuardHeader;

					pl_tex bindWeightsCb = reuseLumaWeightsGpu ? in.lumaCellWeightsTex : sc.weightsCb;
					pl_tex bindWeightsCrOrMg = reuseLumaWeightsGpu ? sc.motionGate : sc.weightsCr;
					pl_shader_var v34[24] = {
						VpvGpu::Var("u_k", &uK),
						VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
						VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
						VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
						VpvGpu::Var("u_a2_valid", &uA2Valid), VpvGpu::Var("u_a2_total_var", &uA2Total),
						VpvGpu::Var("u_enable_per_corner_huber", &uEnableHuber),
						VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
						VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep),
						VpvGpu::Var("u_cell_prior_has_cr_grain", &uPriorHasCrGrain),
						VpvGpu::Var("u_pixel_bound_kappa", &uPixelBoundKappa),
						VpvGpu::Var("u_enable_joint_bound", &uEnableJointBound),
						VpvGpu::Var("u_reuse_luma_weights", &uReuseLumaWeights),
						VpvGpu::Var("u_bokeh_mode", &uBokehMode),
						VpvGpu::Var("u_luma_slot_0", &uLumaSlot0),
						VpvGpu::Var("u_luma_slot_1", &uLumaSlot1),
						VpvGpu::Var("u_luma_slot_2", &uLumaSlot2),
						VpvGpu::Var("u_has_clean_split", &uHasSplit),
						VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
						VpvGpu::Var("u_fade_keep", &uFadeKeep),
						VpvGpu::Var("u_store_fine_diff", &uStoreFineDiff)
					};
					const size_t shC34 = (4u * 27u * 4u + 9u * 5u + (3u * fineTile * fineTile + 3u * fineTile * 8u + 128u + 2u) * 2u) * sizeof(float);
					const std::string c34Body = ChromaShaders::BuildPassChromaBlendGuardBody();
					bool okC34 = false;
					if (sameCleanRec)
					{
						pl_shader_desc d34[18] = {
							VpvGpu::Sampled("cur_l", sc.curL), VpvGpu::Sampled("cur_mh", sc.curMH),
							VpvGpu::Sampled("ref_l_0", sc.refL[0]), VpvGpu::Sampled("ref_l_1", sc.refL[1]), VpvGpu::Sampled("ref_l_2", sc.refL[2]),
							VpvGpu::Sampled("ref_mh_0", sc.refMH[0]), VpvGpu::Sampled("ref_mh_1", sc.refMH[1]), VpvGpu::Sampled("ref_mh_2", sc.refMH[2]),
							VpvGpu::Sampled("ref_flags", finalFlagsTex),
							VpvGpu::Sampled("cell_weights_cb", bindWeightsCb),
							VpvGpu::Sampled("cell_weights_cr_or_mg", bindWeightsCrOrMg),
							VpvGpu::Sampled("cell_diag_cb", sc.diagCb), VpvGpu::Sampled("cell_diag_cr", sc.diagCr),
							VpvGpu::Sampled("cell_prior", cellPriorTex), VpvGpu::Sampled("face_mask", c4FaceBind),
							VpvGpu::Storage("coded_delta_raw", sc.codedDeltaRaw),
							VpvGpu::Storage("out_coded_delta", smoothGuard ? sc.warpedUv : out.outCodedDeltaTex),
							VpvGpu::Storage("guard_cell", dstGuardCell)
						};
						okC34 = runPass("td_v2_c34_fused_blend_guard", c34Hdr, c34Body.c_str(), d34, 18, v34, 24, cw, ch, 8, 8, shC34);
					}
					else
					{
						pl_shader_desc d34[19] = {
							VpvGpu::Sampled("cur_l", sc.curL), VpvGpu::Sampled("cur_mh", sc.curMH),
							VpvGpu::Sampled("ref_l_0", sc.refL[0]), VpvGpu::Sampled("ref_l_1", sc.refL[1]), VpvGpu::Sampled("ref_l_2", sc.refL[2]),
							VpvGpu::Sampled("ref_mh_0", sc.refMH[0]), VpvGpu::Sampled("ref_mh_1", sc.refMH[1]), VpvGpu::Sampled("ref_mh_2", sc.refMH[2]),
							VpvGpu::Sampled("ref_flags", finalFlagsTex),
							VpvGpu::Sampled("cell_weights_cb", bindWeightsCb),
							VpvGpu::Sampled("cell_weights_cr_or_mg", bindWeightsCrOrMg),
							VpvGpu::Sampled("cell_diag_cb", sc.diagCb), VpvGpu::Sampled("cell_diag_cr", sc.diagCr),
							VpvGpu::Sampled("cell_prior", cellPriorTex), VpvGpu::Sampled("face_mask", c4FaceBind),
							VpvGpu::Sampled("rec_uv", codedRecTex),
							VpvGpu::Storage("coded_delta_raw", sc.codedDeltaRaw),
							VpvGpu::Storage("out_coded_delta", smoothGuard ? sc.warpedUv : out.outCodedDeltaTex),
							VpvGpu::Storage("guard_cell", dstGuardCell)
						};
						okC34 = runPass("td_v2_c34_fused_blend_guard", c34Hdr, c34Body.c_str(), d34, 19, v34, 24, cw, ch, 8, 8, shC34);
					}
					if (!okC34)
						return failVisible("td_v2_c34_fused_blend_guard dispatch failed", StandInKind::GpuDispatchFailed);
					if (standIns != nullptr)
						standIns->Engage(StandInKind::TransformUnknownBound);
				}
				else
				{
				// ---- PASS C3: Coded-Grid Chroma Blend + 3-Sigma Huber + Honest Output Limit (TD-U3) ----
				std::string c3Hdr = chromaGeom;
				c3Hdr += VpvGpu::Define("TD_GAIN_LOW", effLowGain);
				c3Hdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				c3Hdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				c3Hdr += VpvGpu::Define("TD_HUBER_KNEE_SQ", kHuberKneeSq);
				c3Hdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				c3Hdr += VpvGpu::Define("TD_STEP_SCALE_MAX", maxAllowedStepScale);
				c3Hdr += Shaders::kCoreHelpers;
				c3Hdr += ChromaShaders::kSitedCoordHelpers;
				c3Hdr += R"GLSL(
float td_band_gain(int b) { if (b == 0) return TD_GAIN_LOW; if (b == 1) return TD_GAIN_MID; return TD_GAIN_FINE; }
float td_vec_comp(vec4 v, int k) { if (k == 0) return v.x; if (k == 1) return v.y; return v.z; }
int td_luma_slot(int k) { if (k == 0) return int(round(u_luma_slot_0)); if (k == 1) return int(round(u_luma_slot_1)); return int(round(u_luma_slot_2)); }
float td_huber_taper(float diffSq, float diagC)
{
    float denom = max(diagC, TD_NUM_FLOOR);
    float r = diffSq / denom;
    return (r <= TD_HUBER_KNEE_SQ) ? 1.0 : (TD_HUBER_KNEE_SQ / r);
}
)GLSL";
				c3Hdr += ChromaShaders::kPassChromaBlendHeader;

				// RED-BUG-6 + libplacebo non-aliasing: bind in.lumaCellWeightsTex ONLY on cell_weights_cb (never duplicate
				// on cell_weights_cr!) and ALWAYS bind chroma's own sc.diagCb / sc.diagCr for Huber & U3 bounds!
				pl_tex bindWeightsCb = reuseLumaWeightsGpu ? in.lumaCellWeightsTex : sc.weightsCb;
				pl_tex bindWeightsCr = sc.weightsCr;
				pl_tex bindDiagCb = sc.diagCb;
				pl_tex bindDiagCr = sc.diagCr;

				pl_shader_desc d3[17] = {
					VpvGpu::Sampled("cur_l", sc.curL), VpvGpu::Sampled("cur_mh", sc.curMH),
					VpvGpu::Sampled("ref_l_0", sc.refL[0]), VpvGpu::Sampled("ref_l_1", sc.refL[1]), VpvGpu::Sampled("ref_l_2", sc.refL[2]),
					VpvGpu::Sampled("ref_mh_0", sc.refMH[0]), VpvGpu::Sampled("ref_mh_1", sc.refMH[1]), VpvGpu::Sampled("ref_mh_2", sc.refMH[2]),
					VpvGpu::Sampled("ref_flags", finalFlagsTex),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("cell_weights_cb", bindWeightsCb), VpvGpu::Sampled("cell_weights_cr", bindWeightsCr),
					VpvGpu::Sampled("cell_diag_cb", bindDiagCb), VpvGpu::Sampled("cell_diag_cr", bindDiagCr),
					VpvGpu::Sampled("cell_prior", cellPriorTex), VpvGpu::Sampled("face_mask", faceMaskBind),
					VpvGpu::Storage("coded_delta_raw", sc.codedDeltaRaw)
				};
				pl_shader_var v3[19] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_a2_valid", &uA2Valid), VpvGpu::Var("u_a2_total_var", &uA2Total),
					VpvGpu::Var("u_enable_per_corner_huber", &uEnableHuber),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uPriorHasStep),
					VpvGpu::Var("u_pixel_bound_kappa", &uPixelBoundKappa),
					VpvGpu::Var("u_enable_joint_bound", &uEnableJointBound),
					VpvGpu::Var("u_reuse_luma_weights", &uReuseLumaWeights),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode),
					VpvGpu::Var("u_luma_slot_0", &uLumaSlot0),
					VpvGpu::Var("u_luma_slot_1", &uLumaSlot1),
					VpvGpu::Var("u_luma_slot_2", &uLumaSlot2)
				};
				const size_t shC3 = (48u * 16u + 48u * 2u + 16u * 5u) * sizeof(float);
				if (!runPass("td_v2_c3_blend_bound", c3Hdr, ChromaShaders::kPassChromaBlendBound, d3, 17, v3, 19, cGx, cGy, 16, 16, shC3))
					return failVisible("td_v2_c3_blend_bound dispatch failed", StandInKind::GpuDispatchFailed);
				if (standIns != nullptr)
					standIns->Engage(StandInKind::TransformUnknownBound);

				// ---- PASS C4: Fused LDS Layer-4 TdGuardPort ----
				const size_t shC4 = (3u * fineTile * fineTile + 3u * fineTile * 8u + 128u + 2u) * 2u * sizeof(float);
				std::string c4Hdr = chromaGeom;
				c4Hdr += Shaders::BuildFineTapHeader();
				c4Hdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				c4Hdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				c4Hdr += Shaders::kCoreHelpers;
				c4Hdr += ChromaShaders::kSitedCoordHelpers;
				c4Hdr += ChromaShaders::kPassChromaGuardHeader;

				pl_shader_desc d4[8] = {
					VpvGpu::Sampled("rec_uv", codedRecTex),
					VpvGpu::Sampled("cur_uv", sameCleanRec ? sc.weightsCr : codedCleanTex),
					VpvGpu::Sampled("coded_delta_raw", sc.codedDeltaRaw),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("face_mask", c4FaceBind),
					VpvGpu::Sampled("cell_own_grain", sc.ownGrain),
					VpvGpu::Storage("out_coded_delta", smoothGuard ? sc.warpedUv : out.outCodedDeltaTex),
					VpvGpu::Storage("guard_cell", dstGuardCell)
				};
				pl_shader_var v4[13] = {
					VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_has_clean_split", &uHasSplit), VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode), VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_cell_prior_has_cr_grain", &uPriorHasCrGrain),
					VpvGpu::Var("u_fade_keep", &uFadeKeep),
					VpvGpu::Var("u_store_fine_diff", &uStoreFineDiff)
				};
				const std::string c4Body = ChromaShaders::BuildPassChromaGuardBody();
				if (!runPass("td_v2_c4_guard_delta", c4Hdr, c4Body.c_str(), d4, 8, v4, 13, cw, ch, 8, 8, shC4))
					return failVisible("td_v2_c4_guard_delta dispatch failed", StandInKind::GpuDispatchFailed);
				} // end !fuseC34

				if (smoothGuard)
				{
					const std::string c4bHdr = chromaGeom + Shaders::kCoreHelpers + ChromaShaders::kSitedCoordHelpers + ChromaShaders::kPassChromaSmoothGuardHeader;
					pl_shader_desc d4b[7] = {
						VpvGpu::Sampled("coded_delta_raw", sc.codedDeltaRaw),
						VpvGpu::Sampled("fine_diff", sc.warpedUv),
						VpvGpu::Sampled("guard_cell", dstGuardCell),
						VpvGpu::Sampled("cell_prior", cellPriorTex),
						VpvGpu::Sampled("face_mask", c4FaceBind),
						VpvGpu::Sampled("cell_own_grain", sc.ownGrain),
						VpvGpu::Storage("out_coded_delta", out.outCodedDeltaTex)
					};
					pl_shader_var v4b[9] = {
						VpvGpu::Var("u_cax0", &uCAx0), VpvGpu::Var("u_cay0", &uCAy0),
						VpvGpu::Var("u_cax1", &uCAx1), VpvGpu::Var("u_cay1", &uCAy1),
						VpvGpu::Var("u_cshift_x", &uCShiftX), VpvGpu::Var("u_cshift_y", &uCShiftY),
						VpvGpu::Var("u_bokeh_mode", &uBokehMode), VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
						VpvGpu::Var("u_fade_keep", &uFadeKeep)
					};
					const size_t shC4b = (16u * 3u) * sizeof(float);
					if (!runPass("td_v2_c4b_smooth_guard", c4bHdr, ChromaShaders::kPassChromaSmoothGuard, d4b, 7, v4b, 9, cGx, cGy, 16, 16, shC4b))
						return failVisible("td_v2_c4b_smooth_guard dispatch failed", StandInKind::GpuDispatchFailed);
				}
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
			gIn.cellStepScale = in.cellStepScale;
			gIn.explicitFocusChangeCells = in.explicitFocusChangeCells;
			gIn.explicitDefocusedCells = in.explicitDefocusedCells;
			gIn.faceMask = in.faceMask;
			gIn.hasStepPrior = in.hasStepPrior;
			gIn.transformIdentified8x8 = in.transformIdentified8x8;
			gIn.estimatedStepDelta = in.estimatedStepDelta;
			gIn.deadZonePhi = in.deadZonePhi;
			gIn.zeroCoeffFraction = in.zeroCoeffFraction;
			gIn.isCutOrSpanInvalid = in.isCutOrSpanInvalid;
			gIn.fadeKeep = in.fadeKeep;
			gIn.psdShape = in.psdShape;
			gIn.lumaRefRoles = in.lumaRefRoles;
			gIn.lumaActiveRefCount = in.lumaActiveRefCount;
			gIn.frameRole = in.frameRole;

			// RED-BUG-6: Upload CPU lumaCellWeights into persistent inLumaCellWeightsTex_ (3*cw x ch RGBA32F) when supplied!
			if (cfg_.chromaReuseLumaWeights && in.lumaCellWeights != nullptr)
			{
				const int cellPxUpload = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
				const int uCw = (in.lumaW + cellPxUpload - 1) / cellPxUpload;
				const int uCh = (in.lumaH + cellPxUpload - 1) / cellPxUpload;
				const size_t nCells = static_cast<size_t>(uCw) * static_cast<size_t>(uCh);
				if (uCw > 0 && uCh > 0)
				{
					if (!Detail::EnsureTex(gpu, inLumaCellWeightsTex_, 3 * uCw, uCh, fmtRGBA32, true, false, true, false))
						return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes lumaCellWeights alloc failed", StandInKind::GpuDispatchFailed);
					std::vector<float> lwBuf(static_cast<size_t>(3 * uCw) * static_cast<size_t>(uCh) * 4u, 0.0f);
					for (int b = 0; b < 3; ++b)
					{
						for (int k = 0; k < 3 && k < kMaxBlendRefs; ++k)
						{
							const auto& srcW = (*in.lumaCellWeights)[b][k];
							if (srcW.size() != nCells)
								continue;
							for (int cy = 0; cy < uCh; ++cy)
							{
								for (int cx = 0; cx < uCw; ++cx)
								{
									const size_t srcIdx = static_cast<size_t>(cy) * uCw + cx;
									const size_t dst = static_cast<size_t>(cy) * (3 * uCw) + static_cast<size_t>(b * uCw + cx);
									lwBuf[dst * 4u + static_cast<size_t>(k)] = static_cast<float>(srcW[srcIdx]);
								}
							}
						}
					}
					if (!Detail::UploadFloats(gpu, inLumaCellWeightsTex_, lwBuf.data()))
						return failRes("TdV2ChromaGpuEngine::ProcessChromaPlanes lumaCellWeights upload failed", StandInKind::GpuDispatchFailed);
					gIn.lumaCellWeightsTex = inLumaCellWeightsTex_;
				}
			}

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
		pl_tex ebPriorTex_ = nullptr;
		pl_tex ebScratchTex_ = nullptr;
		pl_tex chProjIrTex_ = nullptr;
		pl_tex inLumaCellWeightsTex_ = nullptr;

		A2ePriorHistory a2eHistoryCb_;
		A2ePriorHistory a2eHistoryCr_;

		std::string lastError_;
		uint64_t failures_ = 0;
		int lastDispatchCount_ = 0;
		bool scratchShort_ = false;
	};
} // namespace Gpu
} // namespace TdV2
