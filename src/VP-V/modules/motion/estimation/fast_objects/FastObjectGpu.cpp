// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Milestone 2: Native Direct3D 11 CS 5.0 implementation of Stage A (3-frame same-polarity
// symmetric residual, Sobel gradient + bilinear 2nd-order Taylor curvature bound, optional
// half-res Cb/Cr 3-DOF chi^2 residual, 4x4 groupshared cell reduction, bounded InterlockedAdd
// active-cell compaction <= 4 KB, 3-slot async staging readback ring, and sparse CPU blob
// extraction over only the <= 256 active cells).
#include "FastObjectGpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace FastObjects
{
	namespace
	{
		// Exact HLSL CS 5.0 compute shaders mirroring FastObjectResidualRef.cpp.
		// Strictly SM 5.0: [numthreads(8,8,1)], groupshared, InterlockedAdd, zero Vulkan/SM5.1 extensions.
		const char* const kFastObjectGpuHlslPart1 = R"HLSL(
struct GpuActiveCell
{
    uint cellXY;
    uint flagsGrow;
    float maxAbsZ;
    float zCell;
};

cbuffer FastObjectCB : register(b0)
{
    uint4 dims;       // x: width, y: height, z: cellW, w: cellH
    uint4 flags;      // x: hasChroma, y: hasGrainMap, z: hasGuardMap, w: bit0=dormantCoarse, bit1=writeZMap, bit2=fadePrev, bit3=fadeNext
    uint4 grids;      // x: grainGridW, y: grainGridH, z: guardGridW, w: guardGridH
    uint4 limits;     // x: maxActiveCellsCap, y: stride (1 or 2), z: outCellW, w: outCellH

    float4 picRect;   // x: picLeft, y: picTop, z: picRight, w: picBottom
    float4 picScale;  // x: picCx, y: picCy, z: sourceScale, w: invSourceScale
    float4 noiseCfg;  // x: baseDiffVarLuma, y: baseDiffVarCb, z: baseDiffVarCr, w: twoSigmaQ2
    float4 threshCfg; // x: seedSigma, y: growSigma, z: seedChi2Dof3, w: growChi2Dof3
    float4 regCfg;    // x: sigmaHInSamples, y: invGrainBlock, z: invGuardBlock, w: streakSampleSigma
    float4 fadeCfg;   // x: fadeGainPrev, y: fadeOffsetPrev, z: fadeGainNext, w: fadeOffsetNext

    float4 mapPrev0;  // h[0..3]
    float4 mapPrev1;  // h[4..7]
    float4 mapPrev2;  // h[8], invW, invH, 0

    float4 mapNext0;  // h[0..3]
    float4 mapNext1;  // h[4..7]
    float4 mapNext2;  // h[8], 0, 0, 0
};

SamplerState gPointClamp : register(s0);

Texture2D<float> gLumaPrev : register(t0);
Texture2D<float> gLumaCur  : register(t1);
Texture2D<float> gLumaNext : register(t2);
Texture2D<float> gCbPrev   : register(t3);
Texture2D<float> gCbCur    : register(t4);
Texture2D<float> gCbNext   : register(t5);
Texture2D<float> gCrPrev   : register(t6);
Texture2D<float> gCrCur    : register(t7);
Texture2D<float> gCrNext   : register(t8);
Texture2D<float> gGrainVar : register(t9);
Texture2D<uint>  gGuardMap : register(t10);
Texture2D<float4> gCellGridSrv : register(t11);

RWTexture2D<float>  gZMapUav     : register(u0);
RWTexture2D<float4> gCellGridUav : register(u1);
RWStructuredBuffer<GpuActiveCell> gCompactUav : register(u2);

float2 ApplyHomog(float4 m0, float4 m1, float4 m2, float2 p)
{
    float w = m1.z * p.x + m1.w * p.y + m2.x;
    float iw = (abs(w) > 1e-12) ? (1.0 / w) : 0.0;
    float ox = (m0.x * p.x + m0.y * p.y + m0.z) * iw;
    float oy = (m0.w * p.x + m1.x * p.y + m1.y) * iw;
    return float2(ox, oy);
}

float SamePolarityResidual(float dm, float dp)
{
    if (dm * dp <= 0.0)
        return 0.0;
    float mag = min(abs(dm), abs(dp));
    return (dm > 0.0) ? mag : -mag;
}

float SobelGradDirect(Texture2D<float> tex, int w, int h, int x, int y)
{
    int xm = max(x - 1, 0);
    int xp = min(x + 1, w - 1);
    int ym = max(y - 1, 0);
    int yp = min(y + 1, h - 1);
    float a00 = tex.Load(int3(xm, ym, 0));
    float a01 = tex.Load(int3(x,  ym, 0));
    float a02 = tex.Load(int3(xp, ym, 0));
    float a10 = tex.Load(int3(xm, y,  0));
    float a12 = tex.Load(int3(xp, y,  0));
    float a20 = tex.Load(int3(xm, yp, 0));
    float a21 = tex.Load(int3(x,  yp, 0));
    float a22 = tex.Load(int3(xp, yp, 0));
    float gx = ((a02 - a00) + 2.0 * (a12 - a10) + (a22 - a20)) * 0.125;
    float gy = ((a20 - a00) + 2.0 * (a21 - a01) + (a22 - a02)) * 0.125;
    return sqrt(gx * gx + gy * gy);
}

// Fast exact float32 bilinear read via single CS 5.0 GatherRed instruction.
// Returns (a, b, c, d) in outQuad so lazy curvature evaluation reuses them with 0 extra reads.
void SampleBilinFast(
    Texture2D<float> tex, int w, int h, float2 invDims, float sx, float sy,
    bool doFade, float fadeGain, float fadeOff,
    out float outVal, out float4 outQuad, out int2 outBase, out float2 outFrac)
{
    float cx = clamp(sx, 0.0, (float)(w - 1));
    float cy = clamp(sy, 0.0, (float)(h - 1));
    int x0 = (int)floor(cx);
    int y0 = (int)floor(cy);
    float fx = cx - (float)x0;
    float fy = cy - (float)y0;

    float2 uv = (float2((float)x0, (float)y0) + 1.0) * invDims;
    float4 g = tex.GatherRed(gPointClamp, uv);
    float a = g.w;
    float b = g.z;
    float c = g.x;
    float d = g.y;
    float v = (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + d * fx) * fy;
    if (doFade && abs(fadeGain) > 1e-6)
        v = (v - fadeOff) / fadeGain;
    outVal = v;
    outQuad = float4(a, b, c, d);
    outBase = int2(x0, y0);
    outFrac = float2(fx, fy);
}

// Lazy exact 2nd-order bilinear curvature Taylor truncation bound (called ONLY when r != 0.0).
float BilinearCurvFast(
    Texture2D<float> tex, int w, int h, float2 invDims,
    float4 quad, int2 baseXY, float2 fracXY,
    bool doFade, float fadeGain)
{
    float wx = 0.5 * min(fracXY.x, 1.0 - fracXY.x);
    float wy = 0.5 * min(fracXY.y, 1.0 - fracXY.y);
    if (!(wx > 1e-9) && !(wy > 1e-9))
        return 0.0;

    float a = quad.x, b = quad.y, c = quad.z, d = quad.w;
    int x0 = baseXY.x, y0 = baseXY.y;
    int x1 = min(x0 + 1, w - 1);
    int y1 = min(y0 + 1, h - 1);

    float maxD2x = 0.0;
    float maxD2y = 0.0;
    if (wx > 1e-9)
    {
        int xm = max(x0 - 1, 0);
        float4 gL = tex.GatherRed(gPointClamp, (float2((float)xm, (float)y0) + 1.0) * invDims);
        float4 gR = tex.GatherRed(gPointClamp, (float2((float)x1, (float)y0) + 1.0) * invDims);
        float am = gL.w, cm = gL.x;
        float bp = gR.z, dp = gR.y;
        maxD2x = max(max(abs(am - 2.0 * a + b), abs(a - 2.0 * b + bp)),
                     max(abs(cm - 2.0 * c + d), abs(c - 2.0 * d + dp)));
    }
    if (wy > 1e-9)
    {
        int ym = max(y0 - 1, 0);
        float4 gT = tex.GatherRed(gPointClamp, (float2((float)x0, (float)ym) + 1.0) * invDims);
        float4 gB = tex.GatherRed(gPointClamp, (float2((float)x0, (float)y1) + 1.0) * invDims);
        float am = gT.w, bm = gT.z;
        float cp = gB.x, dp = gB.y;
        maxD2y = max(max(abs(am - 2.0 * a + c), abs(a - 2.0 * c + cp)),
                     max(abs(bm - 2.0 * b + d), abs(b - 2.0 * d + dp)));
    }

    float bnd = wx * maxD2x + wy * maxD2y;
    if (doFade && abs(fadeGain) > 1e-6)
        bnd /= abs(fadeGain);
    return bnd;
}

// Pass 0: Clear exact active-cell atomic counter header slot 0.
)HLSL";

		const char* const kFastObjectGpuHlslPart2 = R"HLSL(
[numthreads(1, 1, 1)]
void CSClearHeader(uint3 dtid : SV_DispatchThreadID)
{
    GpuActiveCell hdr;
    hdr.cellXY = 0u;
    hdr.flagsGrow = (limits.z & 0xFFFFu) | ((limits.w & 0xFFFFu) << 16);
    hdr.maxAbsZ = 0.0;
    hdr.zCell = 0.0;
    gCompactUav[0] = hdr;
}

#ifndef FAST_OBJ_HAS_CHROMA
#define FAST_OBJ_HAS_CHROMA 1
#endif
#ifndef FAST_OBJ_DORMANT
#define FAST_OBJ_DORMANT 0
#endif
#ifndef FAST_OBJ_TILE_FAST
#define FAST_OBJ_TILE_FAST 0
#endif

#if FAST_OBJ_TILE_FAST
// 100%-overlapped 3-frame shared tiles with odd bank-conflict-free strides (19 and 23):
// cur 19x10 (190) + prev 23x14 (322) + next 23x14 (322) = 3,336 B TGSM.
// Loaded concurrently before ONE single barrier -> zero global loads in math & curvature!
groupshared float sCurT[190];
groupshared float sPrevT[322];
groupshared float sNextT[322];
#elif !FAST_OBJ_DORMANT
groupshared float sCurY[190];
#endif
groupshared float sMaxZ[128];
groupshared float sSumZ[128];
#if FAST_OBJ_HAS_CHROMA
groupshared float sMaxChi2[128];
groupshared float sSumZCb[128];
groupshared float sSumZCr[128];
#endif
groupshared uint  sActGrow[128]; // Packed: (act << 16) | grow

#if FAST_OBJ_TILE_FAST
float CurvFromTilePrev(uint r0, float4 abcd, float fx, float fy, bool doFade, float fadeGain)
{
    float wx = 0.5 * min(fx, 1.0 - fx);
    float wy = 0.5 * min(fy, 1.0 - fy);
    if (!(wx > 1e-9) && !(wy > 1e-9))
        return 0.0;
    uint r1 = r0 + 23u;
    float a = abcd.x, b = abcd.y, c = abcd.z, d = abcd.w;
    float maxD2x = 0.0, maxD2y = 0.0;
    if (wx > 1e-9)
    {
        float am = sPrevT[r0 - 1u], bp = sPrevT[r0 + 2u];
        float cm = sPrevT[r1 - 1u], dp = sPrevT[r1 + 2u];
        maxD2x = max(max(abs(am - 2.0 * a + b), abs(a - 2.0 * b + bp)),
                     max(abs(cm - 2.0 * c + d), abs(c - 2.0 * d + dp)));
    }
    if (wy > 1e-9)
    {
        uint rm = r0 - 23u, rp = r1 + 23u;
        float am = sPrevT[rm], bm = sPrevT[rm + 1u];
        float cp = sPrevT[rp], dp = sPrevT[rp + 1u];
        maxD2y = max(max(abs(am - 2.0 * a + c), abs(a - 2.0 * c + cp)),
                     max(abs(bm - 2.0 * b + d), abs(b - 2.0 * d + dp)));
    }
    float bnd = wx * maxD2x + wy * maxD2y;
    if (doFade && abs(fadeGain) > 1e-6)
        bnd /= abs(fadeGain);
    return bnd;
}

float CurvFromTileNext(uint r0, float4 abcd, float fx, float fy, bool doFade, float fadeGain)
{
    float wx = 0.5 * min(fx, 1.0 - fx);
    float wy = 0.5 * min(fy, 1.0 - fy);
    if (!(wx > 1e-9) && !(wy > 1e-9))
        return 0.0;
    uint r1 = r0 + 23u;
    float a = abcd.x, b = abcd.y, c = abcd.z, d = abcd.w;
    float maxD2x = 0.0, maxD2y = 0.0;
    if (wx > 1e-9)
    {
        float am = sNextT[r0 - 1u], bp = sNextT[r0 + 2u];
        float cm = sNextT[r1 - 1u], dp = sNextT[r1 + 2u];
        maxD2x = max(max(abs(am - 2.0 * a + b), abs(a - 2.0 * b + bp)),
                     max(abs(cm - 2.0 * c + d), abs(c - 2.0 * d + dp)));
    }
    if (wy > 1e-9)
    {
        uint rm = r0 - 23u, rp = r1 + 23u;
        float am = sNextT[rm], bm = sNextT[rm + 1u];
        float cp = sNextT[rp], dp = sNextT[rp + 1u];
        maxD2y = max(max(abs(am - 2.0 * a + c), abs(a - 2.0 * c + cp)),
                     max(abs(bm - 2.0 * b + d), abs(b - 2.0 * d + dp)));
    }
    float bnd = wx * maxD2x + wy * maxD2y;
    if (doFade && abs(fadeGain) > 1e-6)
        bnd /= abs(fadeGain);
    return bnd;
}

float2 ApplyAffine(float4 m0, float4 m1, float invW, float2 p)
{
    return float2((m0.x * p.x + m0.y * p.y + m0.z) * invW,
                  (m0.w * p.x + m1.x * p.y + m1.y) * invW);
}
#endif

// Pass 1: Camera-compensated 3-frame same-polarity residual + noise/gradient/bilinear-curvature
// normalization + optional half-res Cb/Cr 3-DOF chi^2 + 4x4 cell groupshared reduction
// + fused InterlockedAdd active-cell compaction into bounded <= 4 KB buffer.
[numthreads(16, 8, 1)]
void CSResidualAndReduce(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    int w = (int)dims.x;
    int h = (int)dims.y;
    float2 invDims = mapPrev2.yz;
    uint stride = limits.y;
    bool writeZ = (flags.w & 2u) != 0u;
    bool fadePrev = (flags.w & 4u) != 0u;
    bool fadeNext = (flags.w & 8u) != 0u;
#if FAST_OBJ_HAS_CHROMA
    bool hasChroma = (flags.x != 0u);
#endif
    bool hasGrain = (flags.y != 0u);
    bool hasGuard = (flags.z != 0u);

    int hx = (int)(dtid.x * stride);
    int hy = (int)(dtid.y * stride);
    bool inBounds = (hx < w && hy < h);

    float scale = picScale.z;
    float invScale = picScale.w;
    float sx = ((float)hx + 0.5) * scale;
    float sy = ((float)hy + 0.5) * scale;
    bool ok = inBounds && (sx >= picRect.x && sx < picRect.z && sy >= picRect.y && sy < picRect.w);

    if (ok && hasGuard)
    {
        int gbx = clamp((int)floor(sx * regCfg.z), 0, (int)grids.z - 1);
        int gby = clamp((int)floor(sy * regCfg.z), 0, (int)grids.w - 1);
        uint gbits = gGuardMap.Load(int3(gbx, gby, 0));
        if ((gbits & 1u) != 0u)
            ok = false;
    }

    float2 hPrev = float2(-1.0, -1.0);
    float2 hNext = float2(-1.0, -1.0);
#if FAST_OBJ_TILE_FAST
    float invWPrev = 1.0 / mapPrev2.x;
    float invWNext = 1.0 / mapNext2.x;
#endif
    if (ok)
    {
        float2 pc = float2(sx - picScale.x, sy - picScale.y);
#if FAST_OBJ_TILE_FAST
        float2 prevC = ApplyAffine(mapPrev0, mapPrev1, invWPrev, pc);
        float2 nextC = ApplyAffine(mapNext0, mapNext1, invWNext, pc);
#else
        float2 prevC = ApplyHomog(mapPrev0, mapPrev1, mapPrev2, pc);
        float2 nextC = ApplyHomog(mapNext0, mapNext1, mapNext2, pc);
#endif
        hPrev = (prevC + picScale.xy) * invScale - 0.5;
        hNext = (nextC + picScale.xy) * invScale - 0.5;

        float maxX = (float)(w - 1);
        float maxY = (float)(h - 1);
        if (!(hPrev.x >= 0.0 && hPrev.x <= maxX && hPrev.y >= 0.0 && hPrev.y <= maxY &&
              hNext.x >= 0.0 && hNext.x <= maxX && hNext.y >= 0.0 && hNext.y <= maxY))
        {
            ok = false;
        }
    }

    float zY = 0.0;
    float absZY = 0.0;
#if FAST_OBJ_HAS_CHROMA
    float zCb = 0.0;
    float zCr = 0.0;
    float chi2Val = 0.0;
#endif
    uint isValid = 0u;
    uint isGrow = 0u;

#if FAST_OBJ_TILE_FAST
    // Concurrent 3-frame GatherRed load before ONE single barrier (overlaps all 3 DRAM fetches!).
    uint tid = gtid.y * 16u + gtid.x;
    float2 t0Src = (float2((float)(gid.x * 16u), (float)(gid.y * 8u)) + 0.5) * scale;
    float2 t0Pc = t0Src - picScale.xy;
    float2 t0PrevH = (ApplyAffine(mapPrev0, mapPrev1, invWPrev, t0Pc) + picScale.xy) * invScale - 0.5;
    float2 t0NextH = (ApplyAffine(mapNext0, mapNext1, invWNext, t0Pc) + picScale.xy) * invScale - 0.5;
    int prevOx = (int)floor(clamp(t0PrevH.x, 0.0, (float)(w - 1))) - 2;
    int prevOy = (int)floor(clamp(t0PrevH.y, 0.0, (float)(h - 1))) - 2;
    int nextOx = (int)floor(clamp(t0NextH.x, 0.0, (float)(w - 1))) - 2;
    int nextOy = (int)floor(clamp(t0NextH.y, 0.0, (float)(h - 1))) - 2;

    if (tid < 45u)
    {
        uint qy = tid / 9u;
        uint qx = tid - qy * 9u;
        float2 uv = (float2((float)((int)(gid.x * 16u) - 1 + (int)(qx * 2u)),
                            (float)((int)(gid.y * 8u)  - 1 + (int)(qy * 2u))) + 1.0) * invDims;
        float4 g = gLumaCur.GatherRed(gPointClamp, uv);
        uint r0 = (qy * 2u) * 19u + (qx * 2u);
        sCurT[r0] = g.w; sCurT[r0 + 1u] = g.z;
        sCurT[r0 + 19u] = g.x; sCurT[r0 + 20u] = g.y;
    }
    if (tid < 77u)
    {
        uint qy = tid / 11u;
        uint qx = tid - qy * 11u;
        float2 uv = (float2((float)(prevOx + (int)(qx * 2u)), (float)(prevOy + (int)(qy * 2u))) + 1.0) * invDims;
        float4 g = gLumaPrev.GatherRed(gPointClamp, uv);
        uint r0 = (qy * 2u) * 23u + (qx * 2u);
        sPrevT[r0] = g.w; sPrevT[r0 + 1u] = g.z;
        sPrevT[r0 + 23u] = g.x; sPrevT[r0 + 24u] = g.y;
    }
    if (tid >= 51u)
    {
        uint tN = tid - 51u;
        uint qy = tN / 11u;
        uint qx = tN - qy * 11u;
        float2 uv = (float2((float)(nextOx + (int)(qx * 2u)), (float)(nextOy + (int)(qy * 2u))) + 1.0) * invDims;
        float4 g = gLumaNext.GatherRed(gPointClamp, uv);
        uint r0 = (qy * 2u) * 23u + (qx * 2u);
        sNextT[r0] = g.w; sNextT[r0 + 1u] = g.z;
        sNextT[r0 + 23u] = g.x; sNextT[r0 + 24u] = g.y;
    }
    GroupMemoryBarrierWithGroupSync();
#elif !FAST_OBJ_DORMANT
    uint tid = gtid.y * 16u + gtid.x;
    if (tid < 45u)
    {
        uint qy = tid / 9u;
        uint qx = tid - qy * 9u;
        float2 uv = (float2((float)((int)(gid.x * 16u) - 1 + (int)(qx * 2u)),
                            (float)((int)(gid.y * 8u)  - 1 + (int)(qy * 2u))) + 1.0) * invDims;
        float4 g = gLumaCur.GatherRed(gPointClamp, uv);
        uint r0 = (qy * 2u) * 19u + (qx * 2u);
        sCurY[r0] = g.w; sCurY[r0 + 1u] = g.z;
        sCurY[r0 + 19u] = g.x; sCurY[r0 + 20u] = g.y;
    }
    GroupMemoryBarrierWithGroupSync();
#endif

    if (ok)
    {
        isValid = 1u;
        float diffVar = noiseCfg.x;
        bool grainLoaded = false;

#if FAST_OBJ_TILE_FAST
        uint lx = gtid.x + 1u;
        uint ly = gtid.y + 1u;
        float curY = sCurT[ly * 19u + lx];

        float cpx = clamp(hPrev.x, 0.0, (float)(w - 1));
        float cpy = clamp(hPrev.y, 0.0, (float)(h - 1));
        int px0 = (int)floor(cpx), py0 = (int)floor(cpy);
        float pfx = cpx - (float)px0, pfy = cpy - (float)py0;
        uint pr0 = (uint)(py0 - prevOy) * 23u + (uint)(px0 - prevOx);
        float4 pQuad = float4(sPrevT[pr0], sPrevT[pr0 + 1u], sPrevT[pr0 + 23u], sPrevT[pr0 + 24u]);
        float prevY = (pQuad.x * (1.0 - pfx) + pQuad.y * pfx) * (1.0 - pfy) +
                      (pQuad.z * (1.0 - pfx) + pQuad.w * pfx) * pfy;
        if (fadePrev && abs(fadeCfg.x) > 1e-6) prevY = (prevY - fadeCfg.y) / fadeCfg.x;

        float cnx = clamp(hNext.x, 0.0, (float)(w - 1));
        float cny = clamp(hNext.y, 0.0, (float)(h - 1));
        int nx0 = (int)floor(cnx), ny0 = (int)floor(cny);
        float nfx = cnx - (float)nx0, nfy = cny - (float)ny0;
        uint nr0 = (uint)(ny0 - nextOy) * 23u + (uint)(nx0 - nextOx);
        float4 nQuad = float4(sNextT[nr0], sNextT[nr0 + 1u], sNextT[nr0 + 23u], sNextT[nr0 + 24u]);
        float nextY = (nQuad.x * (1.0 - nfx) + nQuad.y * nfx) * (1.0 - nfy) +
                      (nQuad.z * (1.0 - nfx) + nQuad.w * nfx) * nfy;
        if (fadeNext && abs(fadeCfg.z) > 1e-6) nextY = (nextY - fadeCfg.w) / fadeCfg.z;

        float rY = SamePolarityResidual(curY - prevY, curY - nextY);
        if (rY != 0.0)
        {
            if (hasGrain)
            {
                int gx = clamp((int)floor(sx * regCfg.y), 0, (int)grids.x - 1);
                int gy = clamp((int)floor(sy * regCfg.y), 0, (int)grids.y - 1);
                float gv = max(0.0, gGrainVar.Load(int3(gx, gy, 0)));
                diffVar = 2.0 * gv + noiseCfg.w;
                grainLoaded = true;
            }
            float bPrevY = CurvFromTilePrev(pr0, pQuad, pfx, pfy, fadePrev, fadeCfg.x);
            float bilinY = 0.0;
            if (bPrevY > 0.0)
            {
                float bNextY = CurvFromTileNext(nr0, nQuad, nfx, nfy, fadeNext, fadeCfg.z);
                bilinY = min(bPrevY, bNextY);
            }
            float regY = 0.0;
            if (regCfg.x > 0.0)
            {
                uint r0 = (ly - 1u) * 19u;
                uint r1 = ly * 19u;
                uint r2 = (ly + 1u) * 19u;
                float gxv = ((sCurT[r0 + lx + 1u] - sCurT[r0 + lx - 1u]) +
                             2.0 * (sCurT[r1 + lx + 1u] - sCurT[r1 + lx - 1u]) +
                             (sCurT[r2 + lx + 1u] - sCurT[r2 + lx - 1u])) * 0.125;
                float gyv = ((sCurT[r2 + lx - 1u] - sCurT[r0 + lx - 1u]) +
                             2.0 * (sCurT[r2 + lx] - sCurT[r0 + lx]) +
                             (sCurT[r2 + lx + 1u] - sCurT[r0 + lx + 1u])) * 0.125;
                regY = regCfg.x * sqrt(gxv * gxv + gyv * gyv);
            }
            float sigmaY = sqrt(max(diffVar + regY * regY + bilinY * bilinY, 1e-24));
            zY = rY / sigmaY;
            absZY = abs(zY);
            isGrow = (absZY >= threshCfg.y) ? 1u : 0u;
        }
#else
#if !FAST_OBJ_DORMANT
        float curY = sCurY[(gtid.y + 1u) * 19u + (gtid.x + 1u)];
#else
        float curY = gLumaCur.Load(int3(hx, hy, 0));
#endif
        float prevY, nextY;
        float4 qPrevY, qNextY;
        int2 bPrevXY, bNextXY;
        float2 fPrevXY, fNextXY;
        SampleBilinFast(gLumaPrev, w, h, invDims, hPrev.x, hPrev.y, fadePrev, fadeCfg.x, fadeCfg.y, prevY, qPrevY, bPrevXY, fPrevXY);
        SampleBilinFast(gLumaNext, w, h, invDims, hNext.x, hNext.y, fadeNext, fadeCfg.z, fadeCfg.w, nextY, qNextY, bNextXY, fNextXY);

        float rY = SamePolarityResidual(curY - prevY, curY - nextY);
        if (rY != 0.0)
        {
            if (hasGrain)
            {
                int gx = clamp((int)floor(sx * regCfg.y), 0, (int)grids.x - 1);
                int gy = clamp((int)floor(sy * regCfg.y), 0, (int)grids.y - 1);
                float gv = max(0.0, gGrainVar.Load(int3(gx, gy, 0)));
                diffVar = 2.0 * gv + noiseCfg.w;
                grainLoaded = true;
            }
            float bPrevY = BilinearCurvFast(gLumaPrev, w, h, invDims, qPrevY, bPrevXY, fPrevXY, fadePrev, fadeCfg.x);
            float bilinY = 0.0;
            if (bPrevY > 0.0)
            {
                float bNextY = BilinearCurvFast(gLumaNext, w, h, invDims, qNextY, bNextXY, fNextXY, fadeNext, fadeCfg.z);
                bilinY = min(bPrevY, bNextY);
            }
            float regY = 0.0;
            if (regCfg.x > 0.0)
            {
#if !FAST_OBJ_DORMANT
                uint lx = gtid.x + 1u;
                uint ly = gtid.y + 1u;
                uint r0 = (ly - 1u) * 19u;
                uint r1 = ly * 19u;
                uint r2 = (ly + 1u) * 19u;
                float gxv = ((sCurY[r0 + lx + 1u] - sCurY[r0 + lx - 1u]) +
                             2.0 * (sCurY[r1 + lx + 1u] - sCurY[r1 + lx - 1u]) +
                             (sCurY[r2 + lx + 1u] - sCurY[r2 + lx - 1u])) * 0.125;
                float gyv = ((sCurY[r2 + lx - 1u] - sCurY[r0 + lx - 1u]) +
                             2.0 * (sCurY[r2 + lx] - sCurY[r0 + lx]) +
                             (sCurY[r2 + lx + 1u] - sCurY[r0 + lx + 1u])) * 0.125;
                regY = regCfg.x * sqrt(gxv * gxv + gyv * gyv);
#else
                regY = regCfg.x * SobelGradDirect(gLumaCur, w, h, hx, hy);
#endif
            }
            float sigmaY = sqrt(max(diffVar + regY * regY + bilinY * bilinY, 1e-24));
            zY = rY / sigmaY;
            absZY = abs(zY);
            isGrow = (absZY >= threshCfg.y) ? 1u : 0u;
        }
#endif

#if FAST_OBJ_HAS_CHROMA
)HLSL";

		const char* const kFastObjectGpuHlslPart3 = R"HLSL(
        if (hasChroma)
        {
            float curCb = gCbCur.Load(int3(hx, hy, 0));
            float prevCb, nextCb;
            float4 qPrevCb, qNextCb;
            int2 bPrevCb, bNextCb;
            float2 fPrevCb, fNextCb;
            SampleBilinFast(gCbPrev, w, h, invDims, hPrev.x, hPrev.y, fadePrev, fadeCfg.x, fadeCfg.y, prevCb, qPrevCb, bPrevCb, fPrevCb);
            SampleBilinFast(gCbNext, w, h, invDims, hNext.x, hNext.y, fadeNext, fadeCfg.z, fadeCfg.w, nextCb, qNextCb, bNextCb, fNextCb);
            float rCb = SamePolarityResidual(curCb - prevCb, curCb - nextCb);
            if (rCb != 0.0)
            {
                if (hasGrain && !grainLoaded)
                {
                    int gx = clamp((int)floor(sx * regCfg.y), 0, (int)grids.x - 1);
                    int gy = clamp((int)floor(sy * regCfg.y), 0, (int)grids.y - 1);
                    float gv = max(0.0, gGrainVar.Load(int3(gx, gy, 0)));
                    diffVar = 2.0 * gv + noiseCfg.w;
                    grainLoaded = true;
                }
                float bPrevC = BilinearCurvFast(gCbPrev, w, h, invDims, qPrevCb, bPrevCb, fPrevCb, fadePrev, fadeCfg.x);
                float bilinCb = 0.0;
                if (bPrevC > 0.0)
                {
                    float bNextC = BilinearCurvFast(gCbNext, w, h, invDims, qNextCb, bNextCb, fNextCb, fadeNext, fadeCfg.z);
                    bilinCb = min(bPrevC, bNextC);
                }
                float gradCb = (regCfg.x > 0.0) ? SobelGradDirect(gCbCur, w, h, hx, hy) : 0.0;
                float regCb = regCfg.x * gradCb;
                float varCb = hasGrain ? diffVar : noiseCfg.y;
                float sigmaCb = sqrt(max(varCb + regCb * regCb + bilinCb * bilinCb, 1e-24));
                zCb = rCb / sigmaCb;
            }

            float curCr = gCrCur.Load(int3(hx, hy, 0));
            float prevCr, nextCr;
            float4 qPrevCr, qNextCr;
            int2 bPrevCr, bNextCr;
            float2 fPrevCr, fNextCr;
            SampleBilinFast(gCrPrev, w, h, invDims, hPrev.x, hPrev.y, fadePrev, fadeCfg.x, fadeCfg.y, prevCr, qPrevCr, bPrevCr, fPrevCr);
            SampleBilinFast(gCrNext, w, h, invDims, hNext.x, hNext.y, fadeNext, fadeCfg.z, fadeCfg.w, nextCr, qNextCr, bNextCr, fNextCr);
            float rCr = SamePolarityResidual(curCr - prevCr, curCr - nextCr);
            if (rCr != 0.0)
            {
                if (hasGrain && !grainLoaded)
                {
                    int gx = clamp((int)floor(sx * regCfg.y), 0, (int)grids.x - 1);
                    int gy = clamp((int)floor(sy * regCfg.y), 0, (int)grids.y - 1);
                    float gv = max(0.0, gGrainVar.Load(int3(gx, gy, 0)));
                    diffVar = 2.0 * gv + noiseCfg.w;
                    grainLoaded = true;
                }
                float bPrevR = BilinearCurvFast(gCrPrev, w, h, invDims, qPrevCr, bPrevCr, fPrevCr, fadePrev, fadeCfg.x);
                float bilinCr = 0.0;
                if (bPrevR > 0.0)
                {
                    float bNextR = BilinearCurvFast(gCrNext, w, h, invDims, qNextCr, bNextCr, fNextCr, fadeNext, fadeCfg.z);
                    bilinCr = min(bPrevR, bNextR);
                }
                float gradCr = (regCfg.x > 0.0) ? SobelGradDirect(gCrCur, w, h, hx, hy) : 0.0;
                float regCr = regCfg.x * gradCr;
                float varCr = hasGrain ? diffVar : noiseCfg.z;
                float sigmaCr = sqrt(max(varCr + regCr * regCr + bilinCr * bilinCr, 1e-24));
                zCr = rCr / sigmaCr;
            }

            chi2Val = zY * zY + zCb * zCb + zCr * zCr;
            if (chi2Val >= threshCfg.w)
                isGrow = 1u;
        }
#endif
    }

#if !FAST_OBJ_DORMANT
    if (inBounds && writeZ)
    {
        gZMapUav[int2(hx, hy)] = zY;
    }
#endif

    // 4x4 sub-tile 2-step reduction (4x2 = 8 tiles per 16x8 threadgroup).
    uint subX = gtid.x >> 2u;
    uint subY = gtid.y >> 2u;
    uint subId = subY * 4u + subX;
    uint lane = (gtid.y & 3u) * 4u + (gtid.x & 3u);
    uint subBase = subId * 16u;
    uint idx = subBase + lane;
    sMaxZ[idx] = absZY;
    sSumZ[idx] = zY;
#if FAST_OBJ_HAS_CHROMA
    sMaxChi2[idx] = chi2Val;
    sSumZCb[idx] = zCb;
    sSumZCr[idx] = zCr;
#endif
    sActGrow[idx] = (isValid << 16) | isGrow;
    GroupMemoryBarrierWithGroupSync();

    // Step 1: 4 threads per cell reduce 4 columns into lanes [0..3].
    if (lane < 4u)
    {
        uint b0 = idx;
        uint b1 = idx + 4u;
        uint b2 = idx + 8u;
        uint b3 = idx + 12u;
        sMaxZ[b0] = max(max(sMaxZ[b0], sMaxZ[b1]), max(sMaxZ[b2], sMaxZ[b3]));
        sSumZ[b0] = (sSumZ[b0] + sSumZ[b1]) + (sSumZ[b2] + sSumZ[b3]);
#if FAST_OBJ_HAS_CHROMA
        sMaxChi2[b0] = max(max(sMaxChi2[b0], sMaxChi2[b1]), max(sMaxChi2[b2], sMaxChi2[b3]));
        sSumZCb[b0] = (sSumZCb[b0] + sSumZCb[b1]) + (sSumZCb[b2] + sSumZCb[b3]);
        sSumZCr[b0] = (sSumZCr[b0] + sSumZCr[b1]) + (sSumZCr[b2] + sSumZCr[b3]);
#endif
        sActGrow[b0] = (sActGrow[b0] + sActGrow[b1]) + (sActGrow[b2] + sActGrow[b3]);
    }
    GroupMemoryBarrierWithGroupSync();

    // Step 2: lane 0 reduces lanes [0..3] and writes cell + fused InterlockedAdd compaction.
    if (lane == 0u)
    {
        uint outCellX = gid.x * 4u + subX;
        uint outCellY = gid.y * 2u + subY;
        if (outCellX < limits.z && outCellY < limits.w)
        {
            float cellMaxZ = max(max(sMaxZ[subBase], sMaxZ[subBase + 1u]), max(sMaxZ[subBase + 2u], sMaxZ[subBase + 3u]));
            float sumZ = (sSumZ[subBase] + sSumZ[subBase + 1u]) + (sSumZ[subBase + 2u] + sSumZ[subBase + 3u]);
#if FAST_OBJ_HAS_CHROMA
            float cellMaxChi2 = max(max(sMaxChi2[subBase], sMaxChi2[subBase + 1u]), max(sMaxChi2[subBase + 2u], sMaxChi2[subBase + 3u]));
            float sumZCb = (sSumZCb[subBase] + sSumZCb[subBase + 1u]) + (sSumZCb[subBase + 2u] + sSumZCb[subBase + 3u]);
            float sumZCr = (sSumZCr[subBase] + sSumZCr[subBase + 1u]) + (sSumZCr[subBase + 2u] + sSumZCr[subBase + 3u]);
#endif
            uint ag = (sActGrow[subBase] + sActGrow[subBase + 1u]) + (sActGrow[subBase + 2u] + sActGrow[subBase + 3u]);
            uint act = ag >> 16;
            uint grow = ag & 0xFFFFu;

            float zCell = 0.0;
            uint cellFlags = 0u;
            if (act > 0u)
            {
                float invSqrtN = rsqrt((float)act);
                zCell = sumZ * invSqrtN;
#if FAST_OBJ_HAS_CHROMA
                float cellChi2 = zCell * zCell;
                if (hasChroma)
                {
                    float zCellCb = sumZCb * invSqrtN;
                    float zCellCr = sumZCr * invSqrtN;
                    cellChi2 += zCellCb * zCellCb + zCellCr * zCellCr;
                }
                bool peakTrig = (cellMaxZ >= threshCfg.x) || (hasChroma && cellMaxChi2 >= threshCfg.z);
                bool streakTrig = (abs(zCell) >= threshCfg.x) || (hasChroma && cellChi2 >= threshCfg.z);
#else
                bool peakTrig = (cellMaxZ >= threshCfg.x);
                bool streakTrig = (abs(zCell) >= threshCfg.x);
#endif
                cellFlags = 4u; // kCellGuardOk
                if (peakTrig || streakTrig)
                {
                    cellFlags |= 1u; // kCellActive
                    if (streakTrig)
                        cellFlags |= 2u; // kCellStreakTrigger
                }
            }
#if FAST_OBJ_HAS_CHROMA
            float outMaxZ = hasChroma ? max(cellMaxZ, sqrt(max(cellMaxChi2, 0.0))) : cellMaxZ;
#else
            float outMaxZ = cellMaxZ;
#endif
            gCellGridUav[int2(outCellX, outCellY)] = float4(outMaxZ, zCell, (float)grow, (float)cellFlags);

            // Fused bounded InterlockedAdd active-cell compaction (eliminates Pass 2 full-grid readback).
            if ((cellFlags & 1u) != 0u)
            {
                uint slot = 0u;
                InterlockedAdd(gCompactUav[0].cellXY, 1u, slot);
                if (slot < limits.x)
                {
                    GpuActiveCell rec;
                    rec.cellXY = (outCellX & 0xFFFFu) | ((outCellY & 0xFFFFu) << 16);
                    rec.flagsGrow = (grow & 0xFFFFu) | ((cellFlags & 0xFFFFu) << 16);
                    rec.maxAbsZ = outMaxZ;
                    rec.zCell = zCell;
                    gCompactUav[1u + slot] = rec;
                }
            }
        }
    }
}

// Pass 2: Compact active cells from coarse 4x4 cell grid via InterlockedAdd into bounded <= 4 KB buffer.
[numthreads(8, 8, 1)]
void CSCompactActiveCells(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= limits.z || dtid.y >= limits.w)
        return;

    float4 c = gCellGridSrv.Load(int3(dtid.x, dtid.y, 0));
    uint cellFlags = (uint)(c.w + 0.5);
    if ((cellFlags & 1u) == 0u)
        return;

    uint slot = 0u;
    InterlockedAdd(gCompactUav[0].cellXY, 1u, slot);
    if (slot < limits.x)
    {
        GpuActiveCell rec;
        rec.cellXY = (dtid.x & 0xFFFFu) | ((dtid.y & 0xFFFFu) << 16);
        rec.flagsGrow = (((uint)(c.z + 0.5)) & 0xFFFFu) | ((cellFlags & 0xFFFFu) << 16);
        rec.maxAbsZ = c.x;
        rec.zCell = c.y;
        gCompactUav[1u + slot] = rec;
    }
}
)HLSL";

		struct GpuParamsCB
		{
			uint32_t dims[4];
			uint32_t flags[4];
			uint32_t grids[4];
			uint32_t limits[4];

			float picRect[4];
			float picScale[4];
			float noiseCfg[4];
			float threshCfg[4];
			float regCfg[4];
			float fadeCfg[4];

			float mapPrev0[4];
			float mapPrev1[4];
			float mapPrev2[4];

			float mapNext0[4];
			float mapNext1[4];
			float mapNext2[4];
		};
		static_assert(sizeof(GpuParamsCB) == 256, "FastObjectCB must be 256 bytes (16-byte aligned)");

		bool CompareGpuActiveCell(const GpuActiveCell& a, const GpuActiveCell& b)
		{
			const uint32_t ay = a.CellY(), by = b.CellY();
			if (ay != by)
				return ay < by;
			return a.CellX() < b.CellX();
		}

		// Local CPU math helpers shared by ExtractBlobsFromGpuCells (sparse patch evaluation).
		inline double BilinearSafeCpu(const float* img, int w, int h, double sx, double sy)
		{
			if (sx < 0.0) sx = 0.0;
			else if (sx > static_cast<double>(w - 1)) sx = static_cast<double>(w - 1);
			if (sy < 0.0) sy = 0.0;
			else if (sy > static_cast<double>(h - 1)) sy = static_cast<double>(h - 1);
			const int x0 = static_cast<int>(std::floor(sx));
			const int y0 = static_cast<int>(std::floor(sy));
			const int x1 = (x0 + 1 < w) ? (x0 + 1) : (w - 1);
			const int y1 = (y0 + 1 < h) ? (y0 + 1) : (h - 1);
			const double fx = sx - static_cast<double>(x0);
			const double fy = sy - static_cast<double>(y0);
			const size_t r0 = static_cast<size_t>(y0) * static_cast<size_t>(w);
			const size_t r1 = static_cast<size_t>(y1) * static_cast<size_t>(w);
			const double a = static_cast<double>(img[r0 + static_cast<size_t>(x0)]);
			const double b = static_cast<double>(img[r0 + static_cast<size_t>(x1)]);
			const double c = static_cast<double>(img[r1 + static_cast<size_t>(x0)]);
			const double d = static_cast<double>(img[r1 + static_cast<size_t>(x1)]);
			return (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + d * fx) * fy;
		}

		inline double BilinearCurvCpu(const float* img, int w, int h, double sx, double sy, bool fade, double gain)
		{
			if (sx < 0.0) sx = 0.0;
			else if (sx > static_cast<double>(w - 1)) sx = static_cast<double>(w - 1);
			if (sy < 0.0) sy = 0.0;
			else if (sy > static_cast<double>(h - 1)) sy = static_cast<double>(h - 1);
			const int x0 = static_cast<int>(std::floor(sx));
			const int y0 = static_cast<int>(std::floor(sy));
			const double fx = sx - static_cast<double>(x0);
			const double fy = sy - static_cast<double>(y0);
			const double wx = 0.5 * std::fmin(fx, 1.0 - fx);
			const double wy = 0.5 * std::fmin(fy, 1.0 - fy);
			if (!(wx > 1e-9) && !(wy > 1e-9))
				return 0.0;
			const int x1 = (x0 + 1 < w) ? (x0 + 1) : (w - 1);
			const int y1 = (y0 + 1 < h) ? (y0 + 1) : (h - 1);
			auto at = [&](int ix, int iy) -> double
			{
				const int cx = std::min(std::max(ix, 0), w - 1);
				const int cy = std::min(std::max(iy, 0), h - 1);
				return static_cast<double>(img[static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx)]);
			};
			double maxD2x = 0.0, maxD2y = 0.0;
			const int xs[2] = { x0, x1 };
			const int ys[2] = { y0, y1 };
			for (int yi = 0; yi < 2; ++yi)
			{
				for (int xi = 0; xi < 2; ++xi)
				{
					const double c = at(xs[xi], ys[yi]);
					if (wx > 1e-9)
						maxD2x = std::fmax(maxD2x, std::fabs(at(xs[xi] - 1, ys[yi]) - 2.0 * c + at(xs[xi] + 1, ys[yi])));
					if (wy > 1e-9)
						maxD2y = std::fmax(maxD2y, std::fabs(at(xs[xi], ys[yi] - 1) - 2.0 * c + at(xs[xi], ys[yi] + 1)));
				}
			}
			double bnd = wx * maxD2x + wy * maxD2y;
			if (fade && std::fabs(gain) > 1e-6)
				bnd /= std::fabs(gain);
			return bnd;
		}

		inline double SobelGradCpu(const float* img, int w, int h, int x, int y)
		{
			const int xm = (x > 0) ? (x - 1) : 0;
			const int xp = (x + 1 < w) ? (x + 1) : (w - 1);
			const int ym = (y > 0) ? (y - 1) : 0;
			const int yp = (y + 1 < h) ? (y + 1) : (h - 1);
			const size_t rm = static_cast<size_t>(ym) * static_cast<size_t>(w);
			const size_t r0 = static_cast<size_t>(y) * static_cast<size_t>(w);
			const size_t rp = static_cast<size_t>(yp) * static_cast<size_t>(w);
			const double gx = ((img[rm + xp] - img[rm + xm]) + 2.0 * (img[r0 + xp] - img[r0 + xm]) + (img[rp + xp] - img[rp + xm])) * 0.125;
			const double gy = ((img[rp + xm] - img[rm + xm]) + 2.0 * (img[rp + x] - img[rm + x]) + (img[rp + xp] - img[rm + xp])) * 0.125;
			return std::sqrt(gx * gx + gy * gy);
		}

		inline double ApplyFadeCpu(double v, bool fade, double gain, double offset)
		{
			if (!fade || !(std::fabs(gain) > 1e-6))
				return v;
			return (v - offset) / gain;
		}

		inline double SamePolarityCpu(double dm, double dp)
		{
			if (dm * dp <= 0.0)
				return 0.0;
			const double m = std::fmin(std::fabs(dm), std::fabs(dp));
			return (dm > 0.0) ? m : -m;
		}

		bool CompareBlobBySnr(const BlobCandidate& a, const BlobCandidate& b)
		{
			if (a.intSnr != b.intSnr)
				return a.intSnr > b.intSnr;
			if (a.cy != b.cy)
				return a.cy < b.cy;
			return a.cx < b.cx;
		}
	}

	std::string FastObjectGpuHlslSource()
	{
		std::string s(kFastObjectGpuHlslPart1);
		s += kFastObjectGpuHlslPart2;
		s += kFastObjectGpuHlslPart3;
		return s;
	}

	void BuildGpuGuardGrid(
		const ResidualInput& in,
		double maxMinorAxisFrac,
		std::vector<uint32_t>& outGuardGrid)
	{
		const int gw = in.guardGridW;
		const int gh = in.guardGridH;
		const size_t gcount = (gw > 0 && gh > 0) ? static_cast<size_t>(gw) * static_cast<size_t>(gh) : 0;
		outGuardGrid.assign(gcount, 0u);
		if (gcount == 0 || !(in.guardBlockSrcPx > 0.0))
			return;

		CameraModel::Rect pic = in.picture;
		if (pic.Empty() && in.width > 0 && in.height > 0 && in.sourceScale > 0.0f)
		{
			pic.left = 0;
			pic.top = 0;
			pic.right = static_cast<int>(std::lround(static_cast<double>(in.width) * static_cast<double>(in.sourceScale)));
			pic.bottom = static_cast<int>(std::lround(static_cast<double>(in.height) * static_cast<double>(in.sourceScale)));
		}
		const double picH = pic.Empty() ? 1080.0 : static_cast<double>(pic.Height());
		const double maxMinorDiam = maxMinorAxisFrac * picH;
		const double maxLargeObjArea = maxMinorDiam * maxMinorDiam;

		// 1. Mark bar/text excluded, P8 Overlay, and CE-WATER >= 0.5 blocks.
		for (size_t i = 0; i < gcount; ++i)
		{
			uint32_t bits = 0u;
			if (in.blockExcluded && in.blockExcluded[i] != 0)
				bits |= GpuGuardBits::kExcluded;
			if (in.blockLabels && in.blockLabels[i] == static_cast<uint8_t>(CameraModel::Label::Overlay))
				bits |= (GpuGuardBits::kExcluded | GpuGuardBits::kOverlay);
			if (in.waterProb && static_cast<double>(in.waterProb[i]) >= kWaterProbThreshold)
				bits |= (GpuGuardBits::kExcluded | GpuGuardBits::kWater);
			outGuardGrid[i] = bits;
		}

		// 2. Large P8 Label::Object connected-component exclusion (> (maxMinorAxisFrac * picH)^2).
		if (!in.blockLabels)
			return;

		std::vector<uint8_t> visited(gcount, 0u);
		std::vector<uint8_t> largeObj(gcount, 0u);
		std::vector<int> queue;
		std::vector<int> comp;
		queue.reserve(64);
		comp.reserve(64);
		const double blockArea = in.guardBlockSrcPx * in.guardBlockSrcPx;
		const int dx4[4] = { -1, 1, 0, 0 };
		const int dy4[4] = { 0, 0, -1, 1 };

		for (int by = 0; by < gh; ++by)
		{
			for (int bx = 0; bx < gw; ++bx)
			{
				const size_t start = static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx);
				if (visited[start] != 0 || in.blockLabels[start] != static_cast<uint8_t>(CameraModel::Label::Object))
					continue;
				queue.clear();
				comp.clear();
				visited[start] = 1;
				queue.push_back(static_cast<int>(start));
				size_t head = 0;
				while (head < queue.size())
				{
					const int cur = queue[head++];
					comp.push_back(cur);
					const int cx = cur % gw;
					const int cy = cur / gw;
					for (int k = 0; k < 4; ++k)
					{
						const int nx = cx + dx4[k];
						const int ny = cy + dy4[k];
						if (nx < 0 || nx >= gw || ny < 0 || ny >= gh)
							continue;
						const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(gw) + static_cast<size_t>(nx);
						if (visited[nidx] != 0 || in.blockLabels[nidx] != static_cast<uint8_t>(CameraModel::Label::Object))
							continue;
						visited[nidx] = 1;
						queue.push_back(static_cast<int>(nidx));
					}
				}
				if (static_cast<double>(comp.size()) * blockArea > maxLargeObjArea)
				{
					for (int idx : comp)
					{
						largeObj[static_cast<size_t>(idx)] = 1;
						outGuardGrid[static_cast<size_t>(idx)] |= GpuGuardBits::kExcluded;
					}
				}
			}
		}

		// Dilate large objects by 1 block for Stage B active-strike contact validation.
		for (int by = 0; by < gh; ++by)
		{
			for (int bx = 0; bx < gw; ++bx)
			{
				if (largeObj[static_cast<size_t>(by) * static_cast<size_t>(gw) + static_cast<size_t>(bx)] == 0)
					continue;
				for (int oy = -1; oy <= 1; ++oy)
				{
					const int ny = by + oy;
					if (ny < 0 || ny >= gh) continue;
					for (int ox = -1; ox <= 1; ++ox)
					{
						const int nx = bx + ox;
						if (nx < 0 || nx >= gw) continue;
						outGuardGrid[static_cast<size_t>(ny) * static_cast<size_t>(gw) + static_cast<size_t>(nx)] |= GpuGuardBits::kNearLargeObj;
					}
				}
			}
		}
	}

	bool ExtractBlobsFromGpuCells(
		const Config& cfg,
		const ResidualInput& in,
		const GpuReadbackResult& gpuRes,
		ResidualWorkspace& ws,
		ResidualOutput& out)
	{
		out.Clear();
		out.activeCells = gpuRes.totalActiveCells;
		out.chromaUsed = gpuRes.chromaUsed;
		out.causalMode = gpuRes.causalMode;
		out.noiseSigmaEst = gpuRes.noiseSigmaEst;
		out.abstainReason = gpuRes.abstainReason;

		if (gpuRes.clutterOverflow || gpuRes.totalActiveCells > cfg.maxActiveCells)
		{
			out.clutterOverflow = true;
			out.abstainReason |= AbstainReason::ClutterOverflow;
			return false;
		}
		if ((gpuRes.abstainReason & (AbstainReason::SceneDiscontinuity | AbstainReason::NoUsableCamera)) != 0u)
			return false;
		if (gpuRes.cellCount == 0 || gpuRes.totalActiveCells == 0)
			return true;

		if (!in.lumaPrev || !in.lumaCur || !in.lumaNext || in.width <= 0 || in.height <= 0 || !(in.sourceScale > 0.0f))
		{
			out.abstainReason |= AbstainReason::NoUsableCamera;
			return false;
		}

		bool invPrevOk = false;
		const CameraModel::Params mapToPrev = InvertHomography(in.camPrevToCur, &invPrevOk);
		if (!invPrevOk)
		{
			out.abstainReason |= AbstainReason::NoUsableCamera;
			return false;
		}
		CameraModel::Params mapToNext = in.camCurToNext;
		if (in.causalTwoBack)
		{
			bool compOk = false;
			const CameraModel::Params hPrev2ToCur = ComposeHomography(in.camPrevToCur, in.camCurToNext, &compOk);
			bool inv2Ok = false;
			if (compOk)
				mapToNext = InvertHomography(hPrev2ToCur, &inv2Ok);
			if (!compOk || !inv2Ok)
			{
				out.abstainReason |= AbstainReason::NoUsableCamera;
				return false;
			}
		}

		const int w = in.width;
		const int h = in.height;
		const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
		const double scale = static_cast<double>(in.sourceScale);
		CameraModel::Rect pic = in.picture;
		if (pic.Empty())
		{
			pic.left = 0;
			pic.top = 0;
			pic.right = static_cast<int>(std::lround(static_cast<double>(w) * scale));
			pic.bottom = static_cast<int>(std::lround(static_cast<double>(h) * scale));
		}
		const double picCx = pic.CentreX();
		const double picCy = pic.CentreY();
		const double maxMinorDiamSrcPx = cfg.maxMinorAxisFrac * static_cast<double>(pic.Height());

		const bool hasChroma = cfg.enableChromaResidual && gpuRes.chromaUsed &&
			in.cbPrev != nullptr && in.cbCur != nullptr && in.cbNext != nullptr &&
			in.crPrev != nullptr && in.crCur != nullptr && in.crNext != nullptr;

		const double sigmaQ = cfg.QuantizationSigma();
		const double twoSigmaQ2 = 2.0 * sigmaQ * sigmaQ;
		double baseDiffVarLuma = twoSigmaQ2;
		if (in.uniformGrainSigma >= 0.0)
			baseDiffVarLuma = 2.0 * in.uniformGrainSigma * in.uniformGrainSigma + twoSigmaQ2;
		else if (gpuRes.noiseSigmaEst > 0.0f)
			baseDiffVarLuma = std::fmax(static_cast<double>(gpuRes.noiseSigmaEst) * static_cast<double>(gpuRes.noiseSigmaEst), twoSigmaQ2);
		const double baseDiffVarCb = (in.uniformChromaGrainSigma >= 0.0)
			? (2.0 * in.uniformChromaGrainSigma * in.uniformChromaGrainSigma + twoSigmaQ2)
			: baseDiffVarLuma;
		const double baseDiffVarCr = baseDiffVarCb;

		const double seedSigma = cfg.seedSigma;
		const double growSigma = cfg.growSigma;
		const double streakSampleSigma = seedSigma / static_cast<double>(kCellSizeHalfPx);
		const double seedChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(seedSigma) : 0.0;
		const double growChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(growSigma) : 0.0;
		const double streakChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(streakSampleSigma) : 0.0;
		const double sigmaHInSamples = std::fmax(0.0, in.camSigmaPx) / scale;

		const bool hasGrainGrid = (in.grainVarMap != nullptr && in.grainGridW > 0 && in.grainGridH > 0 && in.grainBlockSrcPx > 0.0);
		const double invGrainBlock = hasGrainGrid ? (1.0 / in.grainBlockSrcPx) : 0.0;
		const bool hasGuardGrid = (in.guardGridW > 0 && in.guardGridH > 0 && in.guardBlockSrcPx > 0.0);
		const double invGuardBlock = hasGuardGrid ? (1.0 / in.guardBlockSrcPx) : 0.0;

		std::vector<uint32_t> guardGrid;
		if (hasGuardGrid)
			BuildGpuGuardGrid(in, cfg.maxMinorAxisFrac, guardGrid);

		const int cellW = (w + kCellSizeHalfPx - 1) / kCellSizeHalfPx;
		const int cellH = (h + kCellSizeHalfPx - 1) / kCellSizeHalfPx;
		const size_t cellCountTotal = static_cast<size_t>(cellW) * static_cast<size_t>(cellH);

		// Ensure workspace storage (touched ONLY on sparse halo cells — zero full-frame scan!).
		if (ws.sampleMask.size() < n)
		{
			ws.signedResY.resize(n, 0.0f);
			ws.zMapY.resize(n, 0.0f);
			ws.sigmaDiffY.resize(n, 0.0f);
			ws.bgLuma.resize(n, 0.0f);
			ws.sampleMask.resize(n, 0);
			ws.visited.resize(n, 0);
		}
		if (hasChroma && ws.chi2Map.size() < n)
		{
			ws.signedResCb.resize(n, 0.0f);
			ws.signedResCr.resize(n, 0.0f);
			ws.chi2Map.resize(n, 0.0f);
		}
		if (ws.cells.size() < cellCountTotal)
			ws.cells.resize(cellCountTotal);

		// Collect unique sparse cells covering each GPU active cell + 1-cell border halo for BFS connectivity.
		ws.activeCellIndices.clear();
		std::vector<uint32_t> sparseCells;
		sparseCells.reserve(static_cast<size_t>(gpuRes.cellCount) * 9u);
		const uint32_t cellScale = gpuRes.dormantCoarse ? 2u : 1u;

		for (uint32_t i = 0; i < gpuRes.cellCount; ++i)
		{
			const GpuActiveCell& gc = gpuRes.cells[i];
			const int baseCx = static_cast<int>(gc.CellX() * cellScale);
			const int baseCy = static_cast<int>(gc.CellY() * cellScale);
			for (int dy = 0; dy < static_cast<int>(cellScale); ++dy)
			{
				for (int dx = 0; dx < static_cast<int>(cellScale); ++dx)
				{
					const int acx = baseCx + dx;
					const int acy = baseCy + dy;
					if (acx < 0 || acx >= cellW || acy < 0 || acy >= cellH)
						continue;
					const uint32_t acIdx = static_cast<uint32_t>(acy * cellW + acx);
					ws.activeCellIndices.push_back(acIdx);
					for (int oy = -1; oy <= 1; ++oy)
					{
						const int ny = acy + oy;
						if (ny < 0 || ny >= cellH) continue;
						for (int ox = -1; ox <= 1; ++ox)
						{
							const int nx = acx + ox;
							if (nx < 0 || nx >= cellW) continue;
							sparseCells.push_back(static_cast<uint32_t>(ny * cellW + nx));
						}
					}
				}
			}
		}

		std::sort(sparseCells.begin(), sparseCells.end());
		sparseCells.erase(std::unique(sparseCells.begin(), sparseCells.end()), sparseCells.end());

		// Clear sparse cell metadata before populating active-cell descriptors (zero stale flags across frames).
		for (uint32_t sc : sparseCells)
		{
			ws.cells[sc].flags = 0;
			ws.cells[sc].maxAbsZ = 0.0f;
			ws.cells[sc].zCell = 0.0f;
		}
		for (uint32_t i = 0; i < gpuRes.cellCount; ++i)
		{
			const GpuActiveCell& gc = gpuRes.cells[i];
			const int baseCx = static_cast<int>(gc.CellX() * cellScale);
			const int baseCy = static_cast<int>(gc.CellY() * cellScale);
			for (int dy = 0; dy < static_cast<int>(cellScale); ++dy)
			{
				for (int dx = 0; dx < static_cast<int>(cellScale); ++dx)
				{
					const int acx = baseCx + dx;
					const int acy = baseCy + dy;
					if (acx < 0 || acx >= cellW || acy < 0 || acy >= cellH)
						continue;
					const uint32_t acIdx = static_cast<uint32_t>(acy * cellW + acx);
					ws.cells[acIdx].flags = static_cast<uint8_t>(gc.Flags() & 0xFFu);
					ws.cells[acIdx].maxAbsZ = gc.maxAbsZ;
					ws.cells[acIdx].zCell = gc.zCell;
				}
			}
		}

		constexpr uint8_t kValid = 1u << 0;
		constexpr uint8_t kSeed = 1u << 1;
		constexpr uint8_t kGrow = 1u << 2;
		constexpr uint8_t kStreak = 1u << 3;
		constexpr uint8_t kNearLarge = 1u << 4;
		const double maxValidX = static_cast<double>(w - 1);
		const double maxValidY = static_cast<double>(h - 1);

		// Populate sparse residual & mask values ONLY inside the <= 9 * 256 halo cells!
		for (uint32_t cidx : sparseCells)
		{
			const int cy = static_cast<int>(cidx) / cellW;
			const int cx = static_cast<int>(cidx) % cellW;
			const int y0 = cy * kCellSizeHalfPx;
			const int y1 = std::min(y0 + kCellSizeHalfPx, h);
			const int x0 = cx * kCellSizeHalfPx;
			const int x1 = std::min(x0 + kCellSizeHalfPx, w);

			for (int hy = y0; hy < y1; ++hy)
			{
				const double sy = (static_cast<double>(hy) + 0.5) * scale;
				const bool yInPic = (sy >= static_cast<double>(pic.top) && sy < static_cast<double>(pic.bottom));
				const int gby = hasGuardGrid ? std::min(std::max(static_cast<int>(std::floor(sy * invGuardBlock)), 0), in.guardGridH - 1) : 0;
				const int grby = hasGrainGrid ? std::min(std::max(static_cast<int>(std::floor(sy * invGrainBlock)), 0), in.grainGridH - 1) : 0;

				for (int hx = x0; hx < x1; ++hx)
				{
					const size_t idx = static_cast<size_t>(hy) * static_cast<size_t>(w) + static_cast<size_t>(hx);
					ws.sampleMask[idx] = 0;
					ws.visited[idx] = 0;
					if (!yInPic) continue;
					const double sx = (static_cast<double>(hx) + 0.5) * scale;
					if (sx < static_cast<double>(pic.left) || sx >= static_cast<double>(pic.right))
						continue;

					uint8_t sflags = 0;
					if (hasGuardGrid)
					{
						const int gbx = std::min(std::max(static_cast<int>(std::floor(sx * invGuardBlock)), 0), in.guardGridW - 1);
						const uint32_t gbits = guardGrid[static_cast<size_t>(gby) * static_cast<size_t>(in.guardGridW) + static_cast<size_t>(gbx)];
						if ((gbits & GpuGuardBits::kExcluded) != 0u)
							continue;
						if ((gbits & GpuGuardBits::kNearLargeObj) != 0u)
							sflags |= kNearLarge;
					}

					const double xc = sx - picCx;
					const double yc = sy - picCy;
					double pxC = 0.0, pyC = 0.0, nxC = 0.0, nyC = 0.0;
					CameraModel::Apply(mapToPrev, xc, yc, &pxC, &pyC);
					CameraModel::Apply(mapToNext, xc, yc, &nxC, &nyC);
					const double hPrevX = (pxC + picCx) / scale - 0.5;
					const double hPrevY = (pyC + picCy) / scale - 0.5;
					const double hNextX = (nxC + picCx) / scale - 0.5;
					const double hNextY = (nyC + picCy) / scale - 0.5;
					if (!CameraModel::IsFiniteBits(hPrevX) || !CameraModel::IsFiniteBits(hPrevY) ||
						!CameraModel::IsFiniteBits(hNextX) || !CameraModel::IsFiniteBits(hNextY) ||
						hPrevX < 0.0 || hPrevX > maxValidX || hPrevY < 0.0 || hPrevY > maxValidY ||
						hNextX < 0.0 || hNextX > maxValidX || hNextY < 0.0 || hNextY > maxValidY)
						continue;

					const double curY = static_cast<double>(in.lumaCur[idx]);
					const double prevY = ApplyFadeCpu(BilinearSafeCpu(in.lumaPrev, w, h, hPrevX, hPrevY), in.fadePrev, in.fadeGainPrev, in.fadeOffsetPrev);
					const double nextY = ApplyFadeCpu(BilinearSafeCpu(in.lumaNext, w, h, hNextX, hNextY), in.fadeNext, in.fadeGainNext, in.fadeOffsetNext);
					const double rY = SamePolarityCpu(curY - prevY, curY - nextY);
					const double bY = std::fmin(
						BilinearCurvCpu(in.lumaPrev, w, h, hPrevX, hPrevY, in.fadePrev, in.fadeGainPrev),
						BilinearCurvCpu(in.lumaNext, w, h, hNextX, hNextY, in.fadeNext, in.fadeGainNext));

					double diffVar = baseDiffVarLuma;
					if (hasGrainGrid)
					{
						const int grbx = std::min(std::max(static_cast<int>(std::floor(sx * invGrainBlock)), 0), in.grainGridW - 1);
						const double gv = std::fmax(0.0, static_cast<double>(in.grainVarMap[static_cast<size_t>(grby) * static_cast<size_t>(in.grainGridW) + static_cast<size_t>(grbx)]));
						diffVar = 2.0 * gv + twoSigmaQ2;
					}
					const double gradY = in.edgeMagMap ? static_cast<double>(in.edgeMagMap[idx]) : SobelGradCpu(in.lumaCur, w, h, hx, hy);
					const double regY = sigmaHInSamples * gradY;
					const double sigmaY = std::sqrt(std::fmax(diffVar + regY * regY + bY * bY, 1e-24));
					const double zY = rY / sigmaY;
					const double absZY = std::fabs(zY);

					ws.signedResY[idx] = static_cast<float>(rY);
					ws.zMapY[idx] = static_cast<float>(zY);
					ws.sigmaDiffY[idx] = static_cast<float>(sigmaY);
					ws.bgLuma[idx] = static_cast<float>(0.5 * (prevY + nextY));

					bool isSeed = (absZY >= seedSigma);
					bool isGrow = (absZY >= growSigma);
					bool isStreakPx = (absZY >= streakSampleSigma);

					if (hasChroma)
					{
						const double curCb = static_cast<double>(in.cbCur[idx]);
						const double prevCb = ApplyFadeCpu(BilinearSafeCpu(in.cbPrev, w, h, hPrevX, hPrevY), in.fadePrev, in.fadeGainPrev, in.fadeOffsetPrev);
						const double nextCb = ApplyFadeCpu(BilinearSafeCpu(in.cbNext, w, h, hNextX, hNextY), in.fadeNext, in.fadeGainNext, in.fadeOffsetNext);
						const double rCb = SamePolarityCpu(curCb - prevCb, curCb - nextCb);
						const double bCb = std::fmin(
							BilinearCurvCpu(in.cbPrev, w, h, hPrevX, hPrevY, in.fadePrev, in.fadeGainPrev),
							BilinearCurvCpu(in.cbNext, w, h, hNextX, hNextY, in.fadeNext, in.fadeGainNext));
						const double regCb = sigmaHInSamples * SobelGradCpu(in.cbCur, w, h, hx, hy);
						const double sigmaCb = std::sqrt(std::fmax((hasGrainGrid ? diffVar : baseDiffVarCb) + regCb * regCb + bCb * bCb, 1e-24));
						const double zCb = rCb / sigmaCb;

						const double curCr = static_cast<double>(in.crCur[idx]);
						const double prevCr = ApplyFadeCpu(BilinearSafeCpu(in.crPrev, w, h, hPrevX, hPrevY), in.fadePrev, in.fadeGainPrev, in.fadeOffsetPrev);
						const double nextCr = ApplyFadeCpu(BilinearSafeCpu(in.crNext, w, h, hNextX, hNextY), in.fadeNext, in.fadeGainNext, in.fadeOffsetNext);
						const double rCr = SamePolarityCpu(curCr - prevCr, curCr - nextCr);
						const double bCr = std::fmin(
							BilinearCurvCpu(in.crPrev, w, h, hPrevX, hPrevY, in.fadePrev, in.fadeGainPrev),
							BilinearCurvCpu(in.crNext, w, h, hNextX, hNextY, in.fadeNext, in.fadeGainNext));
						const double regCr = sigmaHInSamples * SobelGradCpu(in.crCur, w, h, hx, hy);
						const double sigmaCr = std::sqrt(std::fmax((hasGrainGrid ? diffVar : baseDiffVarCr) + regCr * regCr + bCr * bCr, 1e-24));
						const double zCr = rCr / sigmaCr;

						const double chi2 = zY * zY + zCb * zCb + zCr * zCr;
						ws.signedResCb[idx] = static_cast<float>(rCb);
						ws.signedResCr[idx] = static_cast<float>(rCr);
						ws.chi2Map[idx] = static_cast<float>(chi2);
						if (chi2 >= seedChi2Dof3) isSeed = true;
						if (chi2 >= growChi2Dof3) isGrow = true;
						if (chi2 >= streakChi2Dof3) isStreakPx = true;
					}

					uint8_t mask = static_cast<uint8_t>(sflags | kValid);
					if (isSeed) mask |= kSeed;
					if (isGrow) mask |= kGrow;
					if (isStreakPx) mask |= kStreak;
					ws.sampleMask[idx] = mask;
				}
			}
		}

		// Sparse 8-connected hysteresis BFS & exact second-moment deconvolution (matching FastObjectResidualRef.cpp).
		ws.candidateScratch.clear();
		const int dx8[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
		const int dy8[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
		const double sheppardVar = (scale * scale) / 12.0;

		for (uint32_t cidx : ws.activeCellIndices)
		{
			const int cy = static_cast<int>(cidx) / cellW;
			const int cx = static_cast<int>(cidx) % cellW;
			const int y0 = cy * kCellSizeHalfPx;
			const int y1 = std::min(y0 + kCellSizeHalfPx, h);
			const int x0 = cx * kCellSizeHalfPx;
			const int x1 = std::min(x0 + kCellSizeHalfPx, w);
			const bool isStreakCell = (ws.cells[cidx].flags & 2u) != 0u;

			for (int sy = y0; sy < y1; ++sy)
			{
				for (int sx = x0; sx < x1; ++sx)
				{
					const size_t seedIdx = static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx);
					if (ws.visited[seedIdx] != 0)
						continue;
					const uint8_t sm = ws.sampleMask[seedIdx];
					if (((sm & kSeed) == 0) && !(isStreakCell && ((sm & kStreak) != 0)))
						continue;

					ws.bfsQueue.clear();
					ws.componentIndices.clear();
					ws.visited[seedIdx] = 1;
					ws.bfsQueue.push_back(static_cast<int>(seedIdx));

					size_t head = 0;
					while (head < ws.bfsQueue.size())
					{
						const int cur = ws.bfsQueue[head++];
						ws.componentIndices.push_back(cur);
						const int px = cur % w;
						const int py = cur / w;
						for (int k = 0; k < 8; ++k)
						{
							const int nx = px + dx8[k];
							const int ny = py + dy8[k];
							if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
							const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx);
							if (ws.visited[nidx] != 0) continue;
							const uint8_t nm = ws.sampleMask[nidx];
							if ((nm & kValid) == 0) continue;
							const size_t ncidx = static_cast<size_t>(ny / kCellSizeHalfPx) * static_cast<size_t>(cellW) +
								static_cast<size_t>(nx / kCellSizeHalfPx);
							const bool nInStreak = (ws.cells[ncidx].flags & 2u) != 0u;
							bool accept = ((nm & kGrow) != 0);
							if (!accept && nInStreak && ((nm & kStreak) != 0))
							{
								const CellDescriptor& nc = ws.cells[ncidx];
								const bool pureLumaStreak =
									(nc.maxAbsZ < static_cast<float>(seedSigma)) &&
									(std::fabs(nc.zCell) >= static_cast<float>(seedSigma));
								if (!pureLumaStreak || (ws.zMapY[nidx] * nc.zCell > 0.0f))
									accept = true;
							}
							if (!accept) continue;
							ws.visited[nidx] = 1;
							ws.bfsQueue.push_back(static_cast<int>(nidx));
						}
					}

					const size_t area = ws.componentIndices.size();
					if (static_cast<double>(area) < cfg.minBlobAreaHalfPx)
						continue;

					double sumW = 0.0, sumWX = 0.0, sumWY = 0.0, sumFluxY = 0.0, sumSigma2Y = 0.0;
					double sumCb = 0.0, sumCr = 0.0, sumBg = 0.0, peakSnr = 0.0;
					bool nearLarge = false;
					int minHx = w, maxHx = -1, minHy = h, maxHy = -1;

					for (int idxInt : ws.componentIndices)
					{
						const size_t idx = static_cast<size_t>(idxInt);
						const int hx = idxInt % w;
						const int hy = idxInt / w;
						if (hx < minHx) minHx = hx;
						if (hx > maxHx) maxHx = hx;
						if (hy < minHy) minHy = hy;
						if (hy > maxHy) maxHy = hy;

						const double sampleSnr = hasChroma
							? std::sqrt(std::fmax(static_cast<double>(ws.chi2Map[idx]), 0.0))
							: std::fabs(static_cast<double>(ws.zMapY[idx]));
						if (sampleSnr > peakSnr) peakSnr = sampleSnr;
						const double weight = std::fmax(sampleSnr, 1e-6);
						sumW += weight;
						sumWX += weight * (static_cast<double>(hx) + 0.5) * scale;
						sumWY += weight * (static_cast<double>(hy) + 0.5) * scale;
						sumFluxY += static_cast<double>(ws.signedResY[idx]);
						const double sY = static_cast<double>(ws.sigmaDiffY[idx]);
						sumSigma2Y += sY * sY;
						sumBg += static_cast<double>(ws.bgLuma[idx]);
						if (hasChroma)
						{
							sumCb += static_cast<double>(ws.signedResCb[idx]);
							sumCr += static_cast<double>(ws.signedResCr[idx]);
						}
						if ((ws.sampleMask[idx] & kNearLarge) != 0)
							nearLarge = true;
					}
					if (!(sumW > 0.0)) continue;

					const double centX = sumWX / sumW;
					const double centY = sumWY / sumW;
					double mxx = 0.0, myy = 0.0, mxy = 0.0;
					for (int idxInt : ws.componentIndices)
					{
						const size_t idx = static_cast<size_t>(idxInt);
						const int hx = idxInt % w;
						const int hy = idxInt / w;
						const double weight = std::fmax(hasChroma
							? std::sqrt(std::fmax(static_cast<double>(ws.chi2Map[idx]), 0.0))
							: std::fabs(static_cast<double>(ws.zMapY[idx])), 1e-6);
						const double dx = (static_cast<double>(hx) + 0.5) * scale - centX;
						const double dy = (static_cast<double>(hy) + 0.5) * scale - centY;
						mxx += weight * dx * dx;
						myy += weight * dy * dy;
						mxy += weight * dx * dy;
					}
					mxx = mxx / sumW + sheppardVar;
					myy = myy / sumW + sheppardVar;
					mxy = mxy / sumW;
					const double tr = mxx + myy;
					const double diff = mxx - myy;
					const double disc = std::sqrt(diff * diff + 4.0 * mxy * mxy);
					const double lambda1 = 0.5 * (tr + disc);
					const double lambda2 = std::fmax(0.0, 0.5 * (tr - disc));
					const double rEst = 2.0 * std::sqrt(lambda2);
					const double lEst = std::sqrt(12.0 * std::fmax(0.0, lambda1 - lambda2));
					if (2.0 * rEst > maxMinorDiamSrcPx) continue;

					const double rmsSigmaY = std::sqrt(std::fmax(sumSigma2Y / static_cast<double>(area), 1e-24));
					const double lumaIntSnr = std::fabs(sumFluxY) / (rmsSigmaY * std::sqrt(static_cast<double>(area)));
					double intSnr = lumaIntSnr;
					if (hasChroma)
					{
						const double cbSnr = std::fabs(sumCb) / (std::sqrt(std::fmax(baseDiffVarCb, 1e-24)) * std::sqrt(static_cast<double>(area)));
						const double crSnr = std::fabs(sumCr) / (std::sqrt(std::fmax(baseDiffVarCr, 1e-24)) * std::sqrt(static_cast<double>(area)));
						intSnr = std::sqrt(lumaIntSnr * lumaIntSnr + cbSnr * cbSnr + crSnr * crSnr);
					}
					if (peakSnr < seedSigma && intSnr < seedSigma) continue;

					BlobCandidate blob;
					blob.cx = static_cast<float>(centX);
					blob.cy = static_cast<float>(centY);
					blob.areaHalfPx = static_cast<float>(area);
					blob.radiusPx = static_cast<float>(rEst);
					blob.streakLenPx = static_cast<float>(lEst);
					blob.streakAngleRad = static_cast<float>(0.5 * std::atan2(2.0 * mxy, diff));
					blob.flux = static_cast<float>(sumFluxY);
					blob.polarity = (lumaIntSnr >= growSigma) ? ((sumFluxY >= 0.0) ? static_cast<int8_t>(1) : static_cast<int8_t>(-1)) : static_cast<int8_t>(0);
					blob.peakSnr = static_cast<float>(peakSnr);
					blob.intSnr = static_cast<float>(intSnr);
					blob.lumaIntSnr = static_cast<float>(lumaIntSnr);
					blob.hasChroma = hasChroma;
					if (hasChroma)
					{
						blob.cbContrast = static_cast<float>(sumCb / static_cast<double>(area));
						blob.crContrast = static_cast<float>(sumCr / static_cast<double>(area));
						blob.chromaFlux = static_cast<float>(std::hypot(sumCb, sumCr));
						blob.chromaSigma = static_cast<float>(std::sqrt(0.5 * (baseDiffVarCb + baseDiffVarCr)));
					}
					blob.meanBgLuma = static_cast<float>(sumBg / static_cast<double>(area));
					blob.nearLargeObject = nearLarge;
					if (hasGuardGrid)
					{
						const int cgbx = std::min(std::max(static_cast<int>(std::floor(centX * invGuardBlock)), 0), in.guardGridW - 1);
						const int cgby = std::min(std::max(static_cast<int>(std::floor(centY * invGuardBlock)), 0), in.guardGridH - 1);
						const uint32_t cgbits = guardGrid[static_cast<size_t>(cgby) * static_cast<size_t>(in.guardGridW) + static_cast<size_t>(cgbx)];
						blob.overlapsOverlay = (cgbits & GpuGuardBits::kOverlay) != 0u;
						blob.overlapsWater = (cgbits & GpuGuardBits::kWater) != 0u;
					}
					blob.maskBox.left = static_cast<int>(std::floor(static_cast<double>(minHx) * scale));
					blob.maskBox.top = static_cast<int>(std::floor(static_cast<double>(minHy) * scale));
					blob.maskBox.right = static_cast<int>(std::ceil(static_cast<double>(maxHx + 1) * scale));
					blob.maskBox.bottom = static_cast<int>(std::ceil(static_cast<double>(maxHy + 1) * scale));
					MaskClear(blob.maskBits);
					const int spanX = std::max(1, maxHx - minHx + 1);
					const int spanY = std::max(1, maxHy - minHy + 1);
					for (int idxInt : ws.componentIndices)
					{
						const int hx = idxInt % w;
						const int hy = idxInt / w;
						const int mx0 = ((hx - minHx) * kMaskDim) / spanX;
						const int mx1 = std::max(mx0 + 1, ((hx - minHx + 1) * kMaskDim) / spanX);
						const int my0 = ((hy - minHy) * kMaskDim) / spanY;
						const int my1 = std::max(my0 + 1, ((hy - minHy + 1) * kMaskDim) / spanY);
						for (int my = my0; my < my1 && my < kMaskDim; ++my)
							for (int mx = mx0; mx < mx1 && mx < kMaskDim; ++mx)
								MaskSetBit(blob.maskBits, mx, my);
					}
					blob.hasMaskBits = true;
					ws.candidateScratch.push_back(blob);
				}
			}
		}

		// Clean sparse mask/cell flags so next frame starts strictly zeroed with O(sparseCells) cost!
		for (uint32_t cidx : sparseCells)
		{
			ws.cells[cidx].flags = 0;
			const int cy = static_cast<int>(cidx) / cellW;
			const int cx = static_cast<int>(cidx) % cellW;
			const int y0 = cy * kCellSizeHalfPx;
			const int y1 = std::min(y0 + kCellSizeHalfPx, h);
			const int x0 = cx * kCellSizeHalfPx;
			const int x1 = std::min(x0 + kCellSizeHalfPx, w);
			for (int hy = y0; hy < y1; ++hy)
			{
				const size_t row = static_cast<size_t>(hy) * static_cast<size_t>(w);
				for (int hx = x0; hx < x1; ++hx)
				{
					ws.sampleMask[row + static_cast<size_t>(hx)] = 0;
					ws.visited[row + static_cast<size_t>(hx)] = 0;
				}
			}
		}

		std::sort(ws.candidateScratch.begin(), ws.candidateScratch.end(), CompareBlobBySnr);
		const uint32_t keep = static_cast<uint32_t>(std::min<size_t>(ws.candidateScratch.size(), kMaxBlobs));
		out.blobCount = keep;
		out.droppedBlobs = static_cast<uint32_t>(ws.candidateScratch.size()) - keep;
		for (uint32_t i = 0; i < keep; ++i)
			out.blobs[i] = ws.candidateScratch[i];
		return true;
	}

	size_t FastObjectGpu::EstimatedVramBytes(int width, int height, bool writeZMap, bool enabled)
	{
		if (!enabled || width <= 0 || height <= 0)
			return 0;
		const size_t w = static_cast<size_t>(width);
		const size_t h = static_cast<size_t>(height);
		const size_t cellW = (w + static_cast<size_t>(kCellSizeHalfPx) - 1u) / static_cast<size_t>(kCellSizeHalfPx);
		const size_t cellH = (h + static_cast<size_t>(kCellSizeHalfPx) - 1u) / static_cast<size_t>(kCellSizeHalfPx);
		const size_t zMapBytes = writeZMap ? (w * h * 4u) : 0u;           // DXGI_FORMAT_R32_FLOAT
		const size_t cellGridBytes = cellW * cellH * 16u;                 // DXGI_FORMAT_R32G32B32A32_FLOAT
		const size_t compactBytes = static_cast<size_t>(kMaxActiveCells + 1u) * sizeof(GpuActiveCell); // 4,112 B
		const size_t stagingBytes = compactBytes * static_cast<size_t>(kStagingRingSlots);
		const size_t cbBytes = sizeof(GpuParamsCB);
		return zMapBytes + cellGridBytes + compactBytes + stagingBytes + cbBytes;
	}

#ifdef _WIN32
	using Microsoft::WRL::ComPtr;

	struct FastObjectGpu::Impl
	{
		struct RingSlot
		{
			ComPtr<ID3D11Buffer> staging;
			ComPtr<ID3D11Query> eventQuery;
			ComPtr<ID3D11Query> tsDisjoint;
			ComPtr<ID3D11Query> tsStart;
			ComPtr<ID3D11Query> tsEnd;
			bool inFlight = false;
			bool timingQueried = false;
			uint64_t frameIndex = 0;
			uint32_t cellW = 0;
			uint32_t cellH = 0;
			uint32_t maxActiveCells = kMaxActiveCells;
			uint32_t abstainReason = AbstainReason::None;
			bool chromaUsed = false;
			bool causalMode = false;
			bool dormantCoarse = false;
			float noiseSigmaEst = 0.0f;
		};

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11ComputeShader> csClear;
		ComPtr<ID3D11ComputeShader> csResidualLuma;
		ComPtr<ID3D11ComputeShader> csResidualLumaTile;
		ComPtr<ID3D11ComputeShader> csResidualChroma;
		ComPtr<ID3D11ComputeShader> csResidualDormant;
		ComPtr<ID3D11ComputeShader> csCompact;
		ComPtr<ID3D11SamplerState> pointClampSampler;
		ComPtr<ID3D11Buffer> cbParams;
		ComPtr<ID3D11Buffer> compactBuf;
		ComPtr<ID3D11UnorderedAccessView> compactUav;

		ComPtr<ID3D11Texture2D> zMapTex;
		ComPtr<ID3D11UnorderedAccessView> zMapUav;
		ComPtr<ID3D11ShaderResourceView> zMapSrv;

		ComPtr<ID3D11Texture2D> cellGridTex;
		ComPtr<ID3D11UnorderedAccessView> cellGridUav;
		ComPtr<ID3D11ShaderResourceView> cellGridSrv;

		int allocW = 0;
		int allocH = 0;
		bool allocZMap = false;

		RingSlot ring[kStagingRingSlots];
		int writeHead = 0;
		int readHead = 0;
		int pending = 0;

		void ReleaseFrameResources()
		{
			zMapSrv.Reset();
			zMapUav.Reset();
			zMapTex.Reset();
			cellGridSrv.Reset();
			cellGridUav.Reset();
			cellGridTex.Reset();
			allocW = 0;
			allocH = 0;
			allocZMap = false;
		}

		void ReleaseAll()
		{
			ReleaseFrameResources();
			for (int i = 0; i < kStagingRingSlots; ++i)
			{
				ring[i].staging.Reset();
				ring[i].eventQuery.Reset();
				ring[i].tsDisjoint.Reset();
				ring[i].tsStart.Reset();
				ring[i].tsEnd.Reset();
				ring[i].inFlight = false;
				ring[i].timingQueried = false;
			}
			compactUav.Reset();
			compactBuf.Reset();
			cbParams.Reset();
			pointClampSampler.Reset();
			csCompact.Reset();
			csResidualDormant.Reset();
			csResidualChroma.Reset();
			csResidualLumaTile.Reset();
			csResidualLuma.Reset();
			csClear.Reset();
			device.Reset();
			writeHead = 0;
			readHead = 0;
			pending = 0;
		}
	};

	FastObjectGpu::FastObjectGpu() : impl_(new Impl()) {}
	FastObjectGpu::~FastObjectGpu()
	{
		Release();
		delete impl_;
		impl_ = nullptr;
	}

	void FastObjectGpu::SetFailure(GpuFailure f, const std::string& msg)
	{
		lastFailure_ = f;
		lastError_ = msg;
	}

	void FastObjectGpu::Release()
	{
		if (impl_)
			impl_->ReleaseAll();
		ready_ = false;
		allocatedVramBytes_ = 0;
	}

	bool FastObjectGpu::Init(ID3D11Device* device, std::string* why)
	{
		Release();
		lastFailure_ = GpuFailure::None;
		lastError_.clear();

		if (!device)
		{
			SetFailure(GpuFailure::Unavailable, "null ID3D11Device");
			if (why) *why = lastError_;
			return false;
		}
		if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
		{
			SetFailure(GpuFailure::Unavailable, "device below D3D_FEATURE_LEVEL_11_0 (CS 5.0 required)");
			if (why) *why = lastError_;
			return false;
		}

		impl_->device = device;

		const std::string hlslSrc = FastObjectGpuHlslSource();
		auto compileKernel = [&](const char* entry, const D3D_SHADER_MACRO* macros, ComPtr<ID3D11ComputeShader>& outShader) -> bool
		{
			ComPtr<ID3DBlob> code;
			ComPtr<ID3DBlob> err;
			const UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS;
			const HRESULT hr = D3DCompile(
				hlslSrc.c_str(),
				hlslSrc.size(),
				"FastObjectGpu.hlsl",
				macros,
				nullptr,
				entry,
				"cs_5_0",
				compileFlags,
				0,
				&code,
				&err);
			if (FAILED(hr) || !code)
			{
				std::string msg = std::string("CS 5.0 compile failed for ") + entry;
				if (err && err->GetBufferPointer() && err->GetBufferSize() > 0)
				{
					msg += ": ";
					msg.append(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
				}
				SetFailure(GpuFailure::ShaderCompile, msg);
				return false;
			}
			const HRESULT shr = device->CreateComputeShader(
				code->GetBufferPointer(),
				code->GetBufferSize(),
				nullptr,
				&outShader);
			if (FAILED(shr) || !outShader)
			{
				SetFailure(GpuFailure::ResourceCreate, std::string("CreateComputeShader failed for ") + entry);
				return false;
			}
			return true;
		};

		const D3D_SHADER_MACRO macrosLuma[] = {
			{ "FAST_OBJ_HAS_CHROMA", "0" },
			{ "FAST_OBJ_DORMANT", "0" },
			{ "FAST_OBJ_TILE_FAST", "0" },
			{ nullptr, nullptr }
		};
		const D3D_SHADER_MACRO macrosLumaTile[] = {
			{ "FAST_OBJ_HAS_CHROMA", "0" },
			{ "FAST_OBJ_DORMANT", "0" },
			{ "FAST_OBJ_TILE_FAST", "1" },
			{ nullptr, nullptr }
		};
		const D3D_SHADER_MACRO macrosChroma[] = {
			{ "FAST_OBJ_HAS_CHROMA", "1" },
			{ "FAST_OBJ_DORMANT", "0" },
			{ "FAST_OBJ_TILE_FAST", "0" },
			{ nullptr, nullptr }
		};
		const D3D_SHADER_MACRO macrosDormant[] = {
			{ "FAST_OBJ_HAS_CHROMA", "0" },
			{ "FAST_OBJ_DORMANT", "1" },
			{ "FAST_OBJ_TILE_FAST", "0" },
			{ nullptr, nullptr }
		};

		if (!compileKernel("CSClearHeader", nullptr, impl_->csClear) ||
			!compileKernel("CSResidualAndReduce", macrosLuma, impl_->csResidualLuma) ||
			!compileKernel("CSResidualAndReduce", macrosLumaTile, impl_->csResidualLumaTile) ||
			!compileKernel("CSResidualAndReduce", macrosChroma, impl_->csResidualChroma) ||
			!compileKernel("CSResidualAndReduce", macrosDormant, impl_->csResidualDormant) ||
			!compileKernel("CSCompactActiveCells", nullptr, impl_->csCompact))
		{
			Release();
			if (why) *why = lastError_;
			return false;
		}

		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sd, &impl_->pointClampSampler)))
		{
			SetFailure(GpuFailure::ResourceCreate, "CreateSamplerState(pointClamp) failed");
			Release();
			if (why) *why = lastError_;
			return false;
		}

		// Note: frame-sized UAVs and staging buffers are deferred until first enabled Submit (0 VRAM when OFF!).
		ready_ = true;
		return true;
	}

	bool FastObjectGpu::EnsureFrameResources(int width, int height, bool writeZMap)
	{
		if (!ready_ || !impl_->device || width <= 0 || height <= 0)
			return false;

		ID3D11Device* dev = impl_->device.Get();

		// Allocate constant buffer, 4,112-byte compact RWStructuredBuffer, and 3-slot staging ring once.
		if (!impl_->cbParams)
		{
			D3D11_BUFFER_DESC cbd = {};
			cbd.ByteWidth = sizeof(GpuParamsCB);
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(dev->CreateBuffer(&cbd, nullptr, &impl_->cbParams)))
			{
				SetFailure(GpuFailure::ResourceCreate, "CreateBuffer(cbParams) failed");
				return false;
			}

			const UINT elemCount = kMaxActiveCells + 1u;
			D3D11_BUFFER_DESC sbd = {};
			sbd.ByteWidth = elemCount * sizeof(GpuActiveCell);
			sbd.Usage = D3D11_USAGE_DEFAULT;
			sbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			sbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			sbd.StructureByteStride = sizeof(GpuActiveCell);
			if (FAILED(dev->CreateBuffer(&sbd, nullptr, &impl_->compactBuf)))
			{
				SetFailure(GpuFailure::ResourceCreate, "CreateBuffer(compactBuf) failed");
				return false;
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {};
			uavd.Format = DXGI_FORMAT_UNKNOWN;
			uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavd.Buffer.FirstElement = 0;
			uavd.Buffer.NumElements = elemCount;
			if (FAILED(dev->CreateUnorderedAccessView(impl_->compactBuf.Get(), &uavd, &impl_->compactUav)))
			{
				SetFailure(GpuFailure::ResourceCreate, "CreateUnorderedAccessView(compactBuf) failed");
				return false;
			}

			D3D11_BUFFER_DESC stgDesc = {};
			stgDesc.ByteWidth = elemCount * sizeof(GpuActiveCell);
			stgDesc.Usage = D3D11_USAGE_STAGING;
			stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stgDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			stgDesc.StructureByteStride = sizeof(GpuActiveCell);

			D3D11_QUERY_DESC evDesc = {};
			evDesc.Query = D3D11_QUERY_EVENT;
			D3D11_QUERY_DESC disjDesc = {};
			disjDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
			D3D11_QUERY_DESC tsDesc = {};
			tsDesc.Query = D3D11_QUERY_TIMESTAMP;

			for (int i = 0; i < kStagingRingSlots; ++i)
			{
				if (FAILED(dev->CreateBuffer(&stgDesc, nullptr, &impl_->ring[i].staging)) ||
					FAILED(dev->CreateQuery(&evDesc, &impl_->ring[i].eventQuery)) ||
					FAILED(dev->CreateQuery(&disjDesc, &impl_->ring[i].tsDisjoint)) ||
					FAILED(dev->CreateQuery(&tsDesc, &impl_->ring[i].tsStart)) ||
					FAILED(dev->CreateQuery(&tsDesc, &impl_->ring[i].tsEnd)))
				{
					SetFailure(GpuFailure::ResourceCreate, "CreateBuffer/CreateQuery(stagingRing) failed");
					return false;
				}
			}
		}

		if (impl_->allocW == width && impl_->allocH == height && impl_->allocZMap == writeZMap)
			return true;

		impl_->ReleaseFrameResources();

		const int cellW = (width + kCellSizeHalfPx - 1) / kCellSizeHalfPx;
		const int cellH = (height + kCellSizeHalfPx - 1) / kCellSizeHalfPx;

		if (writeZMap)
		{
			D3D11_TEXTURE2D_DESC zd = {};
			zd.Width = static_cast<UINT>(width);
			zd.Height = static_cast<UINT>(height);
			zd.MipLevels = 1;
			zd.ArraySize = 1;
			zd.Format = DXGI_FORMAT_R32_FLOAT;
			zd.SampleDesc.Count = 1;
			zd.Usage = D3D11_USAGE_DEFAULT;
			zd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(dev->CreateTexture2D(&zd, nullptr, &impl_->zMapTex)) ||
				FAILED(dev->CreateUnorderedAccessView(impl_->zMapTex.Get(), nullptr, &impl_->zMapUav)) ||
				FAILED(dev->CreateShaderResourceView(impl_->zMapTex.Get(), nullptr, &impl_->zMapSrv)))
			{
				impl_->ReleaseFrameResources();
				SetFailure(GpuFailure::ResourceCreate, "CreateTexture2D(zMap) failed");
				return false;
			}
		}

		D3D11_TEXTURE2D_DESC cd = {};
		cd.Width = static_cast<UINT>(cellW);
		cd.Height = static_cast<UINT>(cellH);
		cd.MipLevels = 1;
		cd.ArraySize = 1;
		cd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		cd.SampleDesc.Count = 1;
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(dev->CreateTexture2D(&cd, nullptr, &impl_->cellGridTex)) ||
			FAILED(dev->CreateUnorderedAccessView(impl_->cellGridTex.Get(), nullptr, &impl_->cellGridUav)) ||
			FAILED(dev->CreateShaderResourceView(impl_->cellGridTex.Get(), nullptr, &impl_->cellGridSrv)))
		{
			impl_->ReleaseFrameResources();
			SetFailure(GpuFailure::ResourceCreate, "CreateTexture2D(cellGrid) failed");
			return false;
		}

		impl_->allocW = width;
		impl_->allocH = height;
		impl_->allocZMap = writeZMap;
		allocatedVramBytes_ = EstimatedVramBytes(width, height, writeZMap, true);
		return true;
	}

	bool FastObjectGpu::Submit(
		ID3D11DeviceContext* context,
		const Config& cfg,
		const GpuResidualInput& in,
		uint64_t frameIndex)
	{
		// Zero-cost OFF / OriginalPathway invariant: 0 allocations, 0 dispatches, 0 VRAM.
		if (!cfg.enabled || cfg.originalPathway)
			return false;

		if (!ready_ || !context)
		{
			SetFailure(GpuFailure::Unavailable, "FastObjectGpu not initialized or null context");
			return false;
		}
		if (in.sceneDiscontinuity || !in.camPrevUsable || !in.camNextUsable ||
			!in.lumaPrevSrv || !in.lumaCurSrv || !in.lumaNextSrv ||
			in.width <= 0 || in.height <= 0 || !(in.sourceScale > 0.0f))
		{
			SetFailure(GpuFailure::InvalidInput, "unusable camera, discontinuity, or missing luma SRVs");
			return false;
		}

		bool invPrevOk = false;
		const CameraModel::Params mapToPrev = InvertHomography(in.camPrevToCur, &invPrevOk);
		if (!invPrevOk)
		{
			SetFailure(GpuFailure::InvalidInput, "singular camPrevToCur homography");
			return false;
		}

		CameraModel::Params mapToNext = in.camCurToNext;
		uint32_t abstain = AbstainReason::None;
		if (in.causalTwoBack)
		{
			bool compOk = false;
			const CameraModel::Params hPrev2ToCur = ComposeHomography(in.camPrevToCur, in.camCurToNext, &compOk);
			bool inv2Ok = false;
			if (compOk)
				mapToNext = InvertHomography(hPrev2ToCur, &inv2Ok);
			if (!compOk || !inv2Ok)
			{
				SetFailure(GpuFailure::InvalidInput, "singular causal two-back homography");
				return false;
			}
			abstain |= AbstainReason::CausalNoLookAhead;
		}

		if (!EnsureFrameResources(in.width, in.height, in.writeZMap))
			return false;

		// If the 3-slot staging ring is full, drop the oldest slot cleanly to prevent stall/overwrite.
		if (impl_->pending >= kStagingRingSlots)
		{
			impl_->ring[impl_->readHead].inFlight = false;
			impl_->readHead = (impl_->readHead + 1) % kStagingRingSlots;
			--impl_->pending;
		}

		const uint32_t stride = in.dormantCoarseMode ? 2u : 1u;
		const uint32_t effW = (static_cast<uint32_t>(in.width) + stride - 1u) / stride;
		const uint32_t effH = (static_cast<uint32_t>(in.height) + stride - 1u) / stride;
		const uint32_t fullCellW = (static_cast<uint32_t>(in.width) + static_cast<uint32_t>(kCellSizeHalfPx) - 1u) / static_cast<uint32_t>(kCellSizeHalfPx);
		const uint32_t fullCellH = (static_cast<uint32_t>(in.height) + static_cast<uint32_t>(kCellSizeHalfPx) - 1u) / static_cast<uint32_t>(kCellSizeHalfPx);
		const uint32_t outCellW = (effW + static_cast<uint32_t>(kCellSizeHalfPx) - 1u) / static_cast<uint32_t>(kCellSizeHalfPx);
		const uint32_t outCellH = (effH + static_cast<uint32_t>(kCellSizeHalfPx) - 1u) / static_cast<uint32_t>(kCellSizeHalfPx);

		const bool hasChroma = cfg.enableChromaResidual &&
			in.cbPrevSrv != nullptr && in.cbCurSrv != nullptr && in.cbNextSrv != nullptr &&
			in.crPrevSrv != nullptr && in.crCurSrv != nullptr && in.crNextSrv != nullptr;
		const bool hasGrain = (in.grainVarSrv != nullptr && in.grainGridW > 0 && in.grainGridH > 0 && in.grainBlockSrcPx > 0.0);
		const bool hasGuard = (in.guardMapSrv != nullptr && in.guardGridW > 0 && in.guardGridH > 0 && in.guardBlockSrcPx > 0.0);

		const double scale = static_cast<double>(in.sourceScale);
		CameraModel::Rect pic = in.picture;
		if (pic.Empty())
		{
			pic.left = 0;
			pic.top = 0;
			pic.right = static_cast<int>(std::lround(static_cast<double>(in.width) * scale));
			pic.bottom = static_cast<int>(std::lround(static_cast<double>(in.height) * scale));
		}

		const double sigmaQ = cfg.QuantizationSigma();
		const double twoSigmaQ2 = 2.0 * sigmaQ * sigmaQ;
		const double grainSig = std::fmax(0.0, in.uniformGrainSigma);
		const double baseDiffVarLuma = 2.0 * grainSig * grainSig + twoSigmaQ2;
		const double cGrainSig = (in.uniformChromaGrainSigma >= 0.0) ? in.uniformChromaGrainSigma : grainSig;
		const double baseDiffVarChroma = 2.0 * cGrainSig * cGrainSig + twoSigmaQ2;

		const double seedChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(cfg.seedSigma) : 0.0;
		const double growChi2Dof3 = hasChroma ? Chi2Dof3ThresholdForSigma(cfg.growSigma) : 0.0;
		const uint32_t maxCap = std::min(cfg.maxActiveCells, kMaxActiveCells);

		GpuParamsCB cb = {};
		cb.dims[0] = static_cast<uint32_t>(in.width);
		cb.dims[1] = static_cast<uint32_t>(in.height);
		cb.dims[2] = fullCellW;
		cb.dims[3] = fullCellH;
		cb.flags[0] = hasChroma ? 1u : 0u;
		cb.flags[1] = hasGrain ? 1u : 0u;
		cb.flags[2] = hasGuard ? 1u : 0u;
		cb.flags[3] = (in.dormantCoarseMode ? 1u : 0u) |
			(in.writeZMap ? 2u : 0u) |
			(in.fadePrev ? 4u : 0u) |
			(in.fadeNext ? 8u : 0u);
		cb.grids[0] = hasGrain ? static_cast<uint32_t>(in.grainGridW) : 1u;
		cb.grids[1] = hasGrain ? static_cast<uint32_t>(in.grainGridH) : 1u;
		cb.grids[2] = hasGuard ? static_cast<uint32_t>(in.guardGridW) : 1u;
		cb.grids[3] = hasGuard ? static_cast<uint32_t>(in.guardGridH) : 1u;
		cb.limits[0] = maxCap;
		cb.limits[1] = stride;
		cb.limits[2] = outCellW;
		cb.limits[3] = outCellH;

		cb.picRect[0] = static_cast<float>(pic.left);
		cb.picRect[1] = static_cast<float>(pic.top);
		cb.picRect[2] = static_cast<float>(pic.right);
		cb.picRect[3] = static_cast<float>(pic.bottom);

		cb.picScale[0] = static_cast<float>(pic.CentreX());
		cb.picScale[1] = static_cast<float>(pic.CentreY());
		cb.picScale[2] = static_cast<float>(scale);
		cb.picScale[3] = static_cast<float>(1.0 / scale);

		cb.noiseCfg[0] = static_cast<float>(baseDiffVarLuma);
		cb.noiseCfg[1] = static_cast<float>(baseDiffVarChroma);
		cb.noiseCfg[2] = static_cast<float>(baseDiffVarChroma);
		cb.noiseCfg[3] = static_cast<float>(twoSigmaQ2);

		cb.threshCfg[0] = static_cast<float>(cfg.seedSigma);
		cb.threshCfg[1] = static_cast<float>(cfg.growSigma);
		cb.threshCfg[2] = static_cast<float>(seedChi2Dof3);
		cb.threshCfg[3] = static_cast<float>(growChi2Dof3);

		cb.regCfg[0] = static_cast<float>(std::fmax(0.0, in.camSigmaPx) / scale);
		cb.regCfg[1] = hasGrain ? static_cast<float>(1.0 / in.grainBlockSrcPx) : 0.0f;
		cb.regCfg[2] = hasGuard ? static_cast<float>(1.0 / in.guardBlockSrcPx) : 0.0f;
		cb.regCfg[3] = static_cast<float>(cfg.seedSigma / static_cast<double>(kCellSizeHalfPx));

		cb.fadeCfg[0] = static_cast<float>(in.fadeGainPrev);
		cb.fadeCfg[1] = static_cast<float>(in.fadeOffsetPrev);
		cb.fadeCfg[2] = static_cast<float>(in.fadeGainNext);
		cb.fadeCfg[3] = static_cast<float>(in.fadeOffsetNext);

		for (int i = 0; i < 4; ++i) cb.mapPrev0[i] = static_cast<float>(mapToPrev.h[i]);
		for (int i = 0; i < 4; ++i) cb.mapPrev1[i] = static_cast<float>(mapToPrev.h[4 + i]);
		cb.mapPrev2[0] = static_cast<float>(mapToPrev.h[8]);
		cb.mapPrev2[1] = 1.0f / static_cast<float>(in.width);
		cb.mapPrev2[2] = 1.0f / static_cast<float>(in.height);

		for (int i = 0; i < 4; ++i) cb.mapNext0[i] = static_cast<float>(mapToNext.h[i]);
		for (int i = 0; i < 4; ++i) cb.mapNext1[i] = static_cast<float>(mapToNext.h[4 + i]);
		cb.mapNext2[0] = static_cast<float>(mapToNext.h[8]);

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(context->Map(impl_->cbParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			SetFailure(GpuFailure::ResourceCreate, "Map(cbParams) failed");
			return false;
		}
		std::memcpy(mapped.pData, &cb, sizeof(cb));
		context->Unmap(impl_->cbParams.Get(), 0);

		Impl::RingSlot& slot = impl_->ring[impl_->writeHead];
		slot.timingQueried = timingEnabled_;
		if (timingEnabled_)
		{
			context->Begin(slot.tsDisjoint.Get());
			context->End(slot.tsStart.Get());
		}

		ID3D11Buffer* cbs[1] = { impl_->cbParams.Get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		// Pass 0: Clear active-cell counter header.
		ID3D11UnorderedAccessView* uavs0[3] = { nullptr, nullptr, impl_->compactUav.Get() };
		context->CSSetUnorderedAccessViews(0, 3, uavs0, nullptr);
		context->CSSetShader(impl_->csClear.Get(), nullptr, 0);
		context->Dispatch(1, 1, 1);

		// Pass 1 (fused): 3-frame residual + Sobel + bilinear curvature + 4x4 groupshared reduce
		// + inline bounded InterlockedAdd active-cell compaction (<= 4,112 B, zero Pass 2 stall).
		ID3D11ShaderResourceView* srvs1[12] = {
			in.lumaPrevSrv,
			in.lumaCurSrv,
			in.lumaNextSrv,
			hasChroma ? in.cbPrevSrv : nullptr,
			hasChroma ? in.cbCurSrv : nullptr,
			hasChroma ? in.cbNextSrv : nullptr,
			hasChroma ? in.crPrevSrv : nullptr,
			hasChroma ? in.crCurSrv : nullptr,
			hasChroma ? in.crNextSrv : nullptr,
			hasGrain ? in.grainVarSrv : nullptr,
			hasGuard ? in.guardMapSrv : nullptr,
			nullptr
		};
		ID3D11UnorderedAccessView* uavs1[3] = {
			(in.writeZMap && !in.dormantCoarseMode) ? impl_->zMapUav.Get() : nullptr,
			impl_->cellGridUav.Get(),
			impl_->compactUav.Get()
		};
		ID3D11SamplerState* samplers1[1] = { impl_->pointClampSampler.Get() };
		context->CSSetSamplers(0, 1, samplers1);
		context->CSSetShaderResources(0, 12, srvs1);
		context->CSSetUnorderedAccessViews(0, 3, uavs1, nullptr);
		// Tight mathematical check: does every 16x8 tile map strictly inside its 22x14 shared tile?
		// For affine (h6==h7==0) with |a-1|<=0.04, |b|<=0.04, |d|<=0.04, |e-1|<=0.04,
		// max drift across a 16x8 tile is <= 15*0.04 + 7*0.04 = 0.88 px (< 1 px margin on every side).
		auto fitsSharedTile = [](const CameraModel::Params& hm) -> bool
		{
			const double w8 = hm.h[8];
			if (std::fabs(w8) < 1e-12) return false;
			const double invW = 1.0 / w8;
			if (std::fabs(hm.h[6] * invW) > 1e-8 || std::fabs(hm.h[7] * invW) > 1e-8) return false;
			if (std::fabs(hm.h[0] * invW - 1.0) > 0.04 || std::fabs(hm.h[4] * invW - 1.0) > 0.04) return false;
			if (std::fabs(hm.h[1] * invW) > 0.04 || std::fabs(hm.h[3] * invW) > 0.04) return false;
			return true;
		};
		const bool useTileFast = !in.dormantCoarseMode && !hasChroma &&
			fitsSharedTile(mapToPrev) && fitsSharedTile(mapToNext);

		ID3D11ComputeShader* pass1Cs = in.dormantCoarseMode
			? impl_->csResidualDormant.Get()
			: (hasChroma ? impl_->csResidualChroma.Get()
				: (useTileFast ? impl_->csResidualLumaTile.Get() : impl_->csResidualLuma.Get()));
		context->CSSetShader(pass1Cs, nullptr, 0);
		const UINT groupsX1 = (effW + 15u) / 16u;
		const UINT groupsY1 = (effH + 7u) / 8u;
		context->Dispatch(groupsX1, groupsY1, 1);

		// Cleanly unbind all samplers, SRVs & UAVs so caller pipeline sees zero lingering state.
		ID3D11UnorderedAccessView* nullUavs[3] = { nullptr, nullptr, nullptr };
		ID3D11SamplerState* nullSamplers[1] = { nullptr };
		ID3D11ShaderResourceView* nullSrvs[12] = {};
		context->CSSetSamplers(0, 1, nullSamplers);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShaderResources(0, 12, nullSrvs);
		context->CSSetShader(nullptr, nullptr, 0);

		if (timingEnabled_)
		{
			context->End(slot.tsEnd.Get());
			context->End(slot.tsDisjoint.Get());
		}

		// Async copy <= 4,112 B into staging ring slot and record completion event query.
		context->CopyResource(slot.staging.Get(), impl_->compactBuf.Get());
		context->End(slot.eventQuery.Get());

		slot.inFlight = true;
		slot.frameIndex = frameIndex;
		slot.cellW = outCellW;
		slot.cellH = outCellH;
		slot.maxActiveCells = cfg.maxActiveCells;
		slot.abstainReason = abstain;
		slot.chromaUsed = hasChroma;
		slot.causalMode = in.causalTwoBack;
		slot.dormantCoarse = in.dormantCoarseMode;
		slot.noiseSigmaEst = static_cast<float>(std::sqrt(baseDiffVarLuma));

		impl_->writeHead = (impl_->writeHead + 1) % kStagingRingSlots;
		++impl_->pending;
		return true;
	}

	bool FastObjectGpu::Collect(ID3D11DeviceContext* context, bool wait, GpuReadbackResult& out)
	{
		out.Clear();
		if (!ready_ || !context || impl_->pending <= 0)
			return false;

		Impl::RingSlot& slot = impl_->ring[impl_->readHead];
		if (!slot.inFlight)
			return false;

		// Non-blocking readiness check when wait == false.
		if (!wait)
		{
			BOOL done = FALSE;
			const HRESULT qhr = context->GetData(slot.eventQuery.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (qhr != S_OK || !done)
				return false;
		}

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		const UINT mapFlags = wait ? 0u : D3D11_MAP_FLAG_DO_NOT_WAIT;
		const HRESULT mhr = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, mapFlags, &mapped);
		if (mhr == DXGI_ERROR_WAS_STILL_DRAWING)
			return false;
		if (FAILED(mhr) || !mapped.pData)
		{
			SetFailure(GpuFailure::Readback, "Map(staging) failed");
			slot.inFlight = false;
			impl_->readHead = (impl_->readHead + 1) % kStagingRingSlots;
			--impl_->pending;
			return false;
		}

		const GpuActiveCell* raw = static_cast<const GpuActiveCell*>(mapped.pData);
		const uint32_t totalActive = raw[0].cellXY;
		const uint32_t stored = std::min(totalActive, std::min(slot.maxActiveCells, kMaxActiveCells));

		out.frameIndex = slot.frameIndex;
		out.totalActiveCells = totalActive;
		out.cellCount = stored;
		out.cellW = slot.cellW;
		out.cellH = slot.cellH;
		out.abstainReason = slot.abstainReason;
		out.chromaUsed = slot.chromaUsed;
		out.causalMode = slot.causalMode;
		out.dormantCoarse = slot.dormantCoarse;
		out.noiseSigmaEst = slot.noiseSigmaEst;

		if (totalActive > slot.maxActiveCells)
		{
			out.clutterOverflow = true;
			out.abstainReason |= AbstainReason::ClutterOverflow;
			out.cellCount = 0;
		}
		else if (stored > 0)
		{
			std::memcpy(out.cells, raw + 1, static_cast<size_t>(stored) * sizeof(GpuActiveCell));
			// Deterministic raster order independent of GPU InterlockedAdd thread scheduling.
			std::sort(out.cells, out.cells + stored, CompareGpuActiveCell);
		}
		context->Unmap(slot.staging.Get(), 0);

		if (slot.timingQueried)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disj = {};
			UINT64 tStart = 0, tEnd = 0;
			HRESULT hrD = S_FALSE, hrS = S_FALSE, hrE = S_FALSE;
			for (int tries = 0; tries < (wait ? 1000 : 1); ++tries)
			{
				hrD = context->GetData(slot.tsDisjoint.Get(), &disj, sizeof(disj), 0);
				hrS = context->GetData(slot.tsStart.Get(), &tStart, sizeof(tStart), 0);
				hrE = context->GetData(slot.tsEnd.Get(), &tEnd, sizeof(tEnd), 0);
				if (hrD == S_OK && hrS == S_OK && hrE == S_OK)
					break;
			}
			if (hrD == S_OK && hrS == S_OK && hrE == S_OK && !disj.Disjoint && disj.Frequency > 0 && tEnd >= tStart)
			{
				out.timing.valid = true;
				out.timing.gpuMs = static_cast<double>(tEnd - tStart) * 1000.0 / static_cast<double>(disj.Frequency);
			}
		}

		slot.inFlight = false;
		impl_->readHead = (impl_->readHead + 1) % kStagingRingSlots;
		--impl_->pending;
		return true;
	}

	bool FastObjectGpu::RunSync(
		ID3D11DeviceContext* context,
		const Config& cfg,
		const GpuResidualInput& in,
		GpuReadbackResult& out,
		uint64_t frameIndex)
	{
		out.Clear();
		out.frameIndex = frameIndex;
		if (cfg.originalPathway)
		{
			out.abstainReason = AbstainReason::OriginalPathway;
			return false;
		}
		if (!cfg.enabled)
		{
			out.abstainReason = AbstainReason::Disabled;
			return false;
		}
		if (in.sceneDiscontinuity)
		{
			out.abstainReason = AbstainReason::SceneDiscontinuity;
			return false;
		}
		if (!in.camPrevUsable || !in.camNextUsable)
		{
			out.abstainReason = AbstainReason::NoUsableCamera;
			return false;
		}
		if (!Submit(context, cfg, in, frameIndex))
			return false;
		return Collect(context, true, out);
	}

	int FastObjectGpu::PendingCount() const
	{
		return impl_ ? impl_->pending : 0;
	}

	ID3D11ShaderResourceView* FastObjectGpu::ZMapSrv() const
	{
		return (impl_ && impl_->zMapSrv) ? impl_->zMapSrv.Get() : nullptr;
	}

	ID3D11ShaderResourceView* FastObjectGpu::CellGridSrv() const
	{
		return (impl_ && impl_->cellGridSrv) ? impl_->cellGridSrv.Get() : nullptr;
	}

	// =========================================================================
	// FastObjectGpuUploader (clean CPU float plane -> D3D11 SRV upload helper)
	// =========================================================================
	struct FastObjectGpuUploader::Impl
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11Texture2D> texLuma[3];
		ComPtr<ID3D11ShaderResourceView> srvLuma[3];
		ComPtr<ID3D11Texture2D> texCb[3];
		ComPtr<ID3D11ShaderResourceView> srvCb[3];
		ComPtr<ID3D11Texture2D> texCr[3];
		ComPtr<ID3D11ShaderResourceView> srvCr[3];
		ComPtr<ID3D11Texture2D> texGrain;
		ComPtr<ID3D11ShaderResourceView> srvGrain;
		ComPtr<ID3D11Texture2D> texGuard;
		ComPtr<ID3D11ShaderResourceView> srvGuard;

		int w = 0, h = 0;
		bool hasChroma = false;
		int grainW = 0, grainH = 0;
		int guardW = 0, guardH = 0;
		std::vector<uint32_t> guardScratch;

		void ReleaseAll()
		{
			for (int i = 0; i < 3; ++i)
			{
				srvLuma[i].Reset(); texLuma[i].Reset();
				srvCb[i].Reset();   texCb[i].Reset();
				srvCr[i].Reset();   texCr[i].Reset();
			}
			srvGrain.Reset(); texGrain.Reset();
			srvGuard.Reset(); texGuard.Reset();
			w = h = grainW = grainH = guardW = guardH = 0;
			hasChroma = false;
			device.Reset();
		}
	};

	FastObjectGpuUploader::FastObjectGpuUploader() : impl_(new Impl()) {}
	FastObjectGpuUploader::~FastObjectGpuUploader()
	{
		Release();
		delete impl_;
		impl_ = nullptr;
	}

	bool FastObjectGpuUploader::Init(ID3D11Device* device, std::string* why)
	{
		Release();
		if (!device)
		{
			if (why) *why = "null ID3D11Device";
			return false;
		}
		impl_->device = device;
		return true;
	}

	void FastObjectGpuUploader::Release()
	{
		if (impl_)
			impl_->ReleaseAll();
		allocatedVramBytes_ = 0;
	}

	bool FastObjectGpuUploader::Upload(
		ID3D11DeviceContext* context,
		const Config& cfg,
		const ResidualInput& cpuIn,
		GpuResidualInput& outGpuIn,
		std::string* why)
	{
		outGpuIn = GpuResidualInput{};
		if (!impl_->device || !context || !cpuIn.lumaPrev || !cpuIn.lumaCur || !cpuIn.lumaNext ||
			cpuIn.width <= 0 || cpuIn.height <= 0)
		{
			if (why) *why = "invalid uploader state or missing luma pointers";
			return false;
		}

		ID3D11Device* dev = impl_->device.Get();
		const bool wantChroma = cfg.enableChromaResidual &&
			cpuIn.cbPrev != nullptr && cpuIn.cbCur != nullptr && cpuIn.cbNext != nullptr &&
			cpuIn.crPrev != nullptr && cpuIn.crCur != nullptr && cpuIn.crNext != nullptr;

		auto makeTexSrv = [&](int tw, int th, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv) -> bool
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = static_cast<UINT>(tw);
			td.Height = static_cast<UINT>(th);
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = fmt;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) &&
				SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv));
		};

		if (impl_->w != cpuIn.width || impl_->h != cpuIn.height || impl_->hasChroma != wantChroma)
		{
			for (int i = 0; i < 3; ++i)
			{
				impl_->srvLuma[i].Reset(); impl_->texLuma[i].Reset();
				impl_->srvCb[i].Reset();   impl_->texCb[i].Reset();
				impl_->srvCr[i].Reset();   impl_->texCr[i].Reset();
				if (!makeTexSrv(cpuIn.width, cpuIn.height, DXGI_FORMAT_R32_FLOAT, impl_->texLuma[i], impl_->srvLuma[i]))
				{
					if (why) *why = "CreateTexture2D(luma) failed";
					return false;
				}
				if (wantChroma)
				{
					if (!makeTexSrv(cpuIn.width, cpuIn.height, DXGI_FORMAT_R32_FLOAT, impl_->texCb[i], impl_->srvCb[i]) ||
						!makeTexSrv(cpuIn.width, cpuIn.height, DXGI_FORMAT_R32_FLOAT, impl_->texCr[i], impl_->srvCr[i]))
					{
						if (why) *why = "CreateTexture2D(chroma) failed";
						return false;
					}
				}
			}
			impl_->w = cpuIn.width;
			impl_->h = cpuIn.height;
			impl_->hasChroma = wantChroma;
		}

		const UINT rowPitchFloat = static_cast<UINT>(cpuIn.width) * sizeof(float);
		context->UpdateSubresource(impl_->texLuma[0].Get(), 0, nullptr, cpuIn.lumaPrev, rowPitchFloat, 0);
		context->UpdateSubresource(impl_->texLuma[1].Get(), 0, nullptr, cpuIn.lumaCur, rowPitchFloat, 0);
		context->UpdateSubresource(impl_->texLuma[2].Get(), 0, nullptr, cpuIn.lumaNext, rowPitchFloat, 0);

		if (wantChroma)
		{
			context->UpdateSubresource(impl_->texCb[0].Get(), 0, nullptr, cpuIn.cbPrev, rowPitchFloat, 0);
			context->UpdateSubresource(impl_->texCb[1].Get(), 0, nullptr, cpuIn.cbCur, rowPitchFloat, 0);
			context->UpdateSubresource(impl_->texCb[2].Get(), 0, nullptr, cpuIn.cbNext, rowPitchFloat, 0);
			context->UpdateSubresource(impl_->texCr[0].Get(), 0, nullptr, cpuIn.crPrev, rowPitchFloat, 0);
			context->UpdateSubresource(impl_->texCr[1].Get(), 0, nullptr, cpuIn.crCur, rowPitchFloat, 0);
			context->UpdateSubresource(impl_->texCr[2].Get(), 0, nullptr, cpuIn.crNext, rowPitchFloat, 0);
		}

		const bool wantGrain = (cpuIn.grainVarMap != nullptr && cpuIn.grainGridW > 0 && cpuIn.grainGridH > 0);
		if (wantGrain)
		{
			if (impl_->grainW != cpuIn.grainGridW || impl_->grainH != cpuIn.grainGridH)
			{
				impl_->srvGrain.Reset(); impl_->texGrain.Reset();
				if (!makeTexSrv(cpuIn.grainGridW, cpuIn.grainGridH, DXGI_FORMAT_R32_FLOAT, impl_->texGrain, impl_->srvGrain))
				{
					if (why) *why = "CreateTexture2D(grainVarMap) failed";
					return false;
				}
				impl_->grainW = cpuIn.grainGridW;
				impl_->grainH = cpuIn.grainGridH;
			}
			context->UpdateSubresource(impl_->texGrain.Get(), 0, nullptr, cpuIn.grainVarMap,
				static_cast<UINT>(cpuIn.grainGridW) * sizeof(float), 0);
		}

		const bool wantGuard = (cpuIn.guardGridW > 0 && cpuIn.guardGridH > 0 &&
			(cpuIn.blockLabels != nullptr || cpuIn.blockExcluded != nullptr || cpuIn.waterProb != nullptr));
		if (wantGuard)
		{
			if (impl_->guardW != cpuIn.guardGridW || impl_->guardH != cpuIn.guardGridH)
			{
				impl_->srvGuard.Reset(); impl_->texGuard.Reset();
				if (!makeTexSrv(cpuIn.guardGridW, cpuIn.guardGridH, DXGI_FORMAT_R32_UINT, impl_->texGuard, impl_->srvGuard))
				{
					if (why) *why = "CreateTexture2D(guardMap) failed";
					return false;
				}
				impl_->guardW = cpuIn.guardGridW;
				impl_->guardH = cpuIn.guardGridH;
			}
			BuildGpuGuardGrid(cpuIn, cfg.maxMinorAxisFrac, impl_->guardScratch);
			context->UpdateSubresource(impl_->texGuard.Get(), 0, nullptr, impl_->guardScratch.data(),
				static_cast<UINT>(cpuIn.guardGridW) * sizeof(uint32_t), 0);
		}

		size_t vram = static_cast<size_t>(impl_->w) * static_cast<size_t>(impl_->h) * sizeof(float) * (impl_->hasChroma ? 9u : 3u);
		if (impl_->grainW > 0 && impl_->grainH > 0)
			vram += static_cast<size_t>(impl_->grainW) * static_cast<size_t>(impl_->grainH) * sizeof(float);
		if (impl_->guardW > 0 && impl_->guardH > 0)
			vram += static_cast<size_t>(impl_->guardW) * static_cast<size_t>(impl_->guardH) * sizeof(uint32_t);
		allocatedVramBytes_ = vram;

		outGpuIn.lumaPrevSrv = impl_->srvLuma[0].Get();
		outGpuIn.lumaCurSrv = impl_->srvLuma[1].Get();
		outGpuIn.lumaNextSrv = impl_->srvLuma[2].Get();
		outGpuIn.width = cpuIn.width;
		outGpuIn.height = cpuIn.height;
		outGpuIn.sourceScale = cpuIn.sourceScale;
		if (wantChroma)
		{
			outGpuIn.cbPrevSrv = impl_->srvCb[0].Get();
			outGpuIn.cbCurSrv = impl_->srvCb[1].Get();
			outGpuIn.cbNextSrv = impl_->srvCb[2].Get();
			outGpuIn.crPrevSrv = impl_->srvCr[0].Get();
			outGpuIn.crCurSrv = impl_->srvCr[1].Get();
			outGpuIn.crNextSrv = impl_->srvCr[2].Get();
		}
		outGpuIn.camPrevToCur = cpuIn.camPrevToCur;
		outGpuIn.camCurToNext = cpuIn.camCurToNext;
		outGpuIn.camPrevUsable = cpuIn.camPrevUsable;
		outGpuIn.camNextUsable = cpuIn.camNextUsable;
		outGpuIn.camSigmaPx = cpuIn.camSigmaPx;
		outGpuIn.picture = cpuIn.picture;
		outGpuIn.sceneDiscontinuity = cpuIn.sceneDiscontinuity;
		outGpuIn.causalTwoBack = cpuIn.causalTwoBack;
		outGpuIn.fadePrev = cpuIn.fadePrev;
		outGpuIn.fadeGainPrev = cpuIn.fadeGainPrev;
		outGpuIn.fadeOffsetPrev = cpuIn.fadeOffsetPrev;
		outGpuIn.fadeNext = cpuIn.fadeNext;
		outGpuIn.fadeGainNext = cpuIn.fadeGainNext;
		outGpuIn.fadeOffsetNext = cpuIn.fadeOffsetNext;
		if (wantGrain)
		{
			outGpuIn.grainVarSrv = impl_->srvGrain.Get();
			outGpuIn.grainGridW = cpuIn.grainGridW;
			outGpuIn.grainGridH = cpuIn.grainGridH;
			outGpuIn.grainBlockSrcPx = cpuIn.grainBlockSrcPx;
		}
		outGpuIn.uniformGrainSigma = (cpuIn.uniformGrainSigma >= 0.0) ? cpuIn.uniformGrainSigma : 0.0;
		outGpuIn.uniformChromaGrainSigma = cpuIn.uniformChromaGrainSigma;
		if (wantGuard)
		{
			outGpuIn.guardMapSrv = impl_->srvGuard.Get();
			outGpuIn.guardGridW = cpuIn.guardGridW;
			outGpuIn.guardGridH = cpuIn.guardGridH;
			outGpuIn.guardBlockSrcPx = cpuIn.guardBlockSrcPx;
		}
		return true;
	}
#else
	struct FastObjectGpu::Impl {};
	FastObjectGpu::FastObjectGpu() : impl_(nullptr) {}
	FastObjectGpu::~FastObjectGpu() {}
	void FastObjectGpu::SetFailure(GpuFailure f, const std::string& msg) { lastFailure_ = f; lastError_ = msg; }
	void FastObjectGpu::Release() { ready_ = false; allocatedVramBytes_ = 0; }
	bool FastObjectGpu::Init(ID3D11Device*, std::string* why)
	{
		SetFailure(GpuFailure::Unavailable, "D3D11 requires Windows");
		if (why) *why = lastError_;
		return false;
	}
	bool FastObjectGpu::EnsureFrameResources(int, int, bool) { return false; }
	bool FastObjectGpu::Submit(ID3D11DeviceContext*, const Config&, const GpuResidualInput&, uint64_t) { return false; }
	bool FastObjectGpu::Collect(ID3D11DeviceContext*, bool, GpuReadbackResult& out) { out.Clear(); return false; }
	bool FastObjectGpu::RunSync(ID3D11DeviceContext*, const Config&, const GpuResidualInput&, GpuReadbackResult& out, uint64_t) { out.Clear(); return false; }
	int FastObjectGpu::PendingCount() const { return 0; }
	ID3D11ShaderResourceView* FastObjectGpu::ZMapSrv() const { return nullptr; }
	ID3D11ShaderResourceView* FastObjectGpu::CellGridSrv() const { return nullptr; }

	struct FastObjectGpuUploader::Impl {};
	FastObjectGpuUploader::FastObjectGpuUploader() : impl_(nullptr) {}
	FastObjectGpuUploader::~FastObjectGpuUploader() {}
	bool FastObjectGpuUploader::Init(ID3D11Device*, std::string* why) { if (why) *why = "D3D11 requires Windows"; return false; }
	void FastObjectGpuUploader::Release() { allocatedVramBytes_ = 0; }
	bool FastObjectGpuUploader::Upload(ID3D11DeviceContext*, const Config&, const ResidualInput&, GpuResidualInput&, std::string* why)
	{
		if (why) *why = "D3D11 requires Windows";
		return false;
	}
#endif
}
