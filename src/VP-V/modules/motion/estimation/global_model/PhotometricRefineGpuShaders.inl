// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// HLSL (cs_5_0) of the GPU photometric refinement (PhotometricRefineGpu.h), compiled at run time with
// D3DCompile (IEEE strictness). Doubles use only the SM 5.0 base double ops (add, mul, min, max, compare,
// float <-> double conversion); division, reciprocal, square root and floor are built from float seeds with Newton
// steps / exact corrections, so no device needs the 11.1 extended double instructions (checked at Init on the
// disassembly). Each piece is one raw literal (the MSVC literal limit); they are concatenated at compile time,
// after RobustStats::kMadToSigmaHlsl (core/RobustStatsShaders.h: VP_MAD_TO_SIGMA, the MAD -> sigma factor; C6-2 U24).
static const char* const kRefineHlsl[] = {
R"HLSL(
cbuffer RefineCB : register(b0)
{
	uint gW, gH, gUseMask, gGridW;
	uint gBicubic, gNGeo, gNPar, gFade;
	uint gTarget, gMinPixels, gMaxIter, gUnit;
	uint gNumGroups, gCandUntiled, gPad1, gPad2;
	double gS, gInvS;
	double gInv2S, gK;
	double gCx, gCy;
	double gL, gInvL;
	double gDiag, gInvDiagPic;
	double gMinGrad, gHuberK;
	double gMinScale, gConvThr;
	double gHw, gHh;
};

struct PtGeom
{
	uint2 X, Y, T, GX, GY;
};

Texture2D<uint> tA : register(t0);
Texture2D<uint> tB : register(t1);
StructuredBuffer<int> sColBx : register(t2);
StructuredBuffer<int> sRowBy : register(t3);
StructuredBuffer<uint> sCell : register(t4);
// GEM-142: 16-bit R16_UINT candidate bin buffer is written via uBin (u0) in CSCand and read via tBin (t5) in
// CSRowCount / CSCompact (D3D11 FL 11.0 safe: no TypedUAVLoadAdditionalFormats required).
Buffer<uint> tBin : register(t5);

RWBuffer<uint> uBin : register(u0);
RWStructuredBuffer<uint> uHist : register(u1);
RWStructuredBuffer<uint4> uRow : register(u2);
RWStructuredBuffer<uint> uPts : register(u3);
RWStructuredBuffer<uint4> uRes : register(u4);
RWStructuredBuffer<uint2> uPart : register(u5);
RWStructuredBuffer<uint2> uState : register(u6);
RWStructuredBuffer<PtGeom> uPtGeom : register(u7);

// State slots (uint2 each; doubles as lo / hi bits).
#define S_STOP 0
#define S_FLAGS 1
#define S_COUNTS 2
#define S_THR 3
#define S_PASS 4
#define S_P 5
#define S_GAIN 14
#define S_OFFSET 15
#define S_SIGMA 16
#define S_LAST 17
#define S_BEFORE 18
#define S_AFTER 19
#define S_BOTH 20
#define S_OKCOUNT 21

#define OK_BIT 0x04000000u
#define OK0_BIT 0x08000000u
#define IDX_MASK 0x03FFFFFFu

double LD(uint2 v) { return asdouble(v.x, v.y); }
uint2 SD(double d) { uint lo, hi; asuint(d, lo, hi); return uint2(lo, hi); }
double GetD(uint slot) { return LD(uState[slot]); }
void PutD(uint slot, double d) { uState[slot] = SD(d); }
bool FiniteD(double d) { uint lo, hi; asuint(d, lo, hi); return (hi & 0x7FF00000u) != 0x7FF00000u; }
double ToD(uint v) { float f = (float)v; return (double)f; }
double ToDi(int v) { float f = (float)v; return (double)f; }
// floor of a double (|u| < 2^24): float seed, then an exact correction.
int FloorD(double u)
{
	float f = floor((float)u);
	double r = u - (double)f;
	if (r < 0.0L) f -= 1.0f;
	else if (r >= 1.0L) f += 1.0f;
	return (int)f;
}
// 1 / x: float seed and three Newton steps (to about 1 ulp of double).
double RcpD(double x)
{
	float xf = (float)x;
	double y = (double)(1.0f / xf);
	y = y * (2.0L - x * y);
	y = y * (2.0L - x * y);
	y = y * (2.0L - x * y);
	return y;
}
double SqrtD(double x)
{
	if (!(x > 0.0L)) return 0.0L;
	float xf = (float)x;
	if (!(xf > 0.0f)) return 0.0L;
	double y = (double)rsqrt(xf);
	y = y * (1.5L - 0.5L * x * y * y);
	y = y * (1.5L - 0.5L * x * y * y);
	y = y * (1.5L - 0.5L * x * y * y);
	return x * y;
}
double CodeA(int x, int y) { return ToD(tA.Load(int3(x, y, 0))); }
double CodeB(int x, int y) { return ToD(tB.Load(int3(x, y, 0))); }

void Keys(double t, out double w0, out double w1, out double w2, out double w3)
{
	precise double k0 = ((-0.5L * t + 1.0L) * t - 0.5L) * t;
	precise double k1 = (1.5L * t - 2.5L) * t * t + 1.0L;
	precise double k2 = ((-1.5L * t + 2.0L) * t + 0.5L) * t;
	precise double k3 = (0.5L * t - 0.5L) * t * t;
	w0 = k0; w1 = k1; w2 = k2; w3 = k3;
}

bool SampleB(double u, double v, out double val)
{
	val = 0.0L;
	if (gBicubic != 0)
	{
		if (!(u >= 1.0L && v >= 1.0L && u < ToD(gW) - 2.0L && v < ToD(gH) - 2.0L))
			return false;
		int x0 = FloorD(u), y0 = FloorD(v);
		double wx0, wx1, wx2, wx3, wy[4];
		precise double tx = u - ToDi(x0), ty = v - ToDi(y0);
		Keys(tx, wx0, wx1, wx2, wx3);
		Keys(ty, wy[0], wy[1], wy[2], wy[3]);
		precise double acc = 0.0L;
		[unroll] for (int r = 0; r < 4; ++r)
		{
			int yy = y0 - 1 + r;
			acc += wy[r] * (wx0 * CodeB(x0 - 1, yy) + wx1 * CodeB(x0, yy) + wx2 * CodeB(x0 + 1, yy) + wx3 * CodeB(x0 + 2, yy));
		}
		precise double vb = acc * gK;
		val = vb;
		return true;
	}
	if (!(u >= 0.0L && v >= 0.0L && u < ToD(gW) - 1.0L && v < ToD(gH) - 1.0L))
		return false;
	int bx0 = FloorD(u), by0 = FloorD(v);
	precise double fx = u - ToDi(bx0), fy = v - ToDi(by0);
	precise double a = (1.0L - fx) * CodeB(bx0, by0) + fx * CodeB(bx0 + 1, by0);
	precise double b = (1.0L - fx) * CodeB(bx0, by0 + 1) + fx * CodeB(bx0 + 1, by0 + 1);
	precise double vl = ((1.0L - fy) * a + fy * b) * gK;
	val = vl;
	return true;
}

void LoadP(out double h[9])
{
	[unroll] for (uint q = 0; q < 9; ++q) h[q] = GetD(S_P + q);
}

void ApplyP(double h[9], double x, double y, out double ox, out double oy)
{
	precise double w = h[6] * x + h[7] * y + h[8];
	double iw = (w != 0.0L) ? RcpD(w) : 0.0L;
	precise double px = (h[0] * x + h[1] * y + h[2]) * iw;
	precise double py = (h[3] * x + h[4] * y + h[5]) * iw;
	ox = px; oy = py;
}

double Huber(double r, double c)
{
	double a = abs(r);
	return a <= c ? 0.5L * r * r : c * (a - 0.5L * c);
}

// Per selected point: centred position, template value and its gradient (codes per source px), as the CPU.
void PointOf(uint packed, out double X, out double Y, out double T, out double GX, out double GY)
{
	int i = (int)(packed & 0x1FFFu), j = (int)((packed >> 13) & 0x1FFFu);
	precise double pX = gS * (ToDi(i) + 0.5L) - gCx;
	precise double pY = gS * (ToDi(j) + 0.5L) - gCy;
	precise double pT = CodeA(i, j) * gK;
	precise double pGX = (CodeA(i + 1, j) * gK - CodeA(i - 1, j) * gK) * gInv2S;
	precise double pGY = (CodeA(i, j + 1) * gK - CodeA(i, j - 1) * gK) * gInv2S;
	X = pX; Y = pY; T = pT; GX = pGX; GY = pGY;
}

[numthreads(64, 1, 1)]
void CSClear(uint3 dt : SV_DispatchThreadID)
{
	if (dt.x < 4096) uHist[dt.x] = 0;
}

// 1. Candidates: inlier-block pixels inside the picture margin with |dx| + |dy| >= minGradient; quarter-code bins.
// GEM-219 (MISS-motion-4): cooperative 18x18 shared tile per (xa, ya) sub-iteration; all 256 threads reach every barrier.
groupshared uint gsCandHist[4096];
groupshared uint gsTile[18][18];
[numthreads(16, 16, 1)]
void CSCand(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	for (uint b0 = gi; b0 < 4096; b0 += 256) gsCandHist[b0] = 0;
	GroupMemoryBarrierWithGroupSync();
	if (gCandUntiled == 0)
	{
		for (uint ya = 0; ya < 4; ++ya)
			for (uint xa = 0; xa < 4; ++xa)
			{
				int ox = (int)(gid.x * 64u + 16u * xa) - 1;
				int oy = (int)(gid.y * 64u + 16u * ya) - 1;
				for (uint idx = gi; idx < 324u; idx += 256u)
				{
					uint tx = idx % 18u;
					uint ty = idx / 18u;
					int sx = ox + (int)tx;
					int sy = oy + (int)ty;
					gsTile[ty][tx] = (sx >= 0 && sy >= 0 && sx < (int)gW && sy < (int)gH)
						? tA.Load(int3(sx, sy, 0)) : 0u;
				}
				GroupMemoryBarrierWithGroupSync();
				uint i = gid.x * 64u + tid.x + 16u * xa, j = gid.y * 64u + tid.y + 16u * ya;
				if (i < gW && j < gH)
				{
					uint bin = 0xFFFFu;
					if (i >= 1 && j >= 1 && i + 1 < gW && j + 1 < gH)
					{
						int bx = sColBx[i], by = sRowBy[j];
						if (bx >= 0 && by >= 0 && (gUseMask == 0 || sCell[(uint)by * gGridW + (uint)bx] != 0))
						{
							uint l = gsTile[tid.y + 1u][tid.x], r = gsTile[tid.y + 1u][tid.x + 2u];
							uint t = gsTile[tid.y][tid.x + 1u], d = gsTile[tid.y + 2u][tid.x + 1u];
							if (gUnit != 0)
							{
								uint mag = (r > l ? r - l : l - r) + (d > t ? d - t : t - d);
								if (ToD(mag) >= gMinGrad) bin = min(4095u, mag * 4u);
							}
							else
							{
								precise double mag = ToD((r > l ? r - l : l - r) + (d > t ? d - t : t - d)) * gK;   // integer sum, one multiply (= CandidateGradientCodes)
								if (mag >= gMinGrad) bin = (uint)FloorD(min(4095.0L, mag * 4.0L));
							}
							if (bin != 0xFFFFu) InterlockedAdd(gsCandHist[bin], 1u);
						}
					}
					uBin[j * gW + i] = bin;
				}
				GroupMemoryBarrierWithGroupSync();
			}
	}
	else
	{
		for (uint ya2 = 0; ya2 < 4; ++ya2)
			for (uint xa2 = 0; xa2 < 4; ++xa2)
			{
				uint i2 = gid.x * 64u + tid.x + 16u * xa2, j2 = gid.y * 64u + tid.y + 16u * ya2;
				if (i2 >= gW || j2 >= gH) continue;
				uint bin2 = 0xFFFFu;
				if (i2 >= 1 && j2 >= 1 && i2 + 1 < gW && j2 + 1 < gH)
				{
					int bx2 = sColBx[i2], by2 = sRowBy[j2];
					if (bx2 >= 0 && by2 >= 0 && (gUseMask == 0 || sCell[(uint)by2 * gGridW + (uint)bx2] != 0))
					{
						uint l2 = tA.Load(int3(i2 - 1, j2, 0)), r2 = tA.Load(int3(i2 + 1, j2, 0));
						uint t2 = tA.Load(int3(i2, j2 - 1, 0)), d2 = tA.Load(int3(i2, j2 + 1, 0));
						if (gUnit != 0)
						{
							uint mag2 = (r2 > l2 ? r2 - l2 : l2 - r2) + (d2 > t2 ? d2 - t2 : t2 - d2);
							if (ToD(mag2) >= gMinGrad) bin2 = min(4095u, mag2 * 4u);
						}
						else
						{
							precise double mag2 = ToD((r2 > l2 ? r2 - l2 : l2 - r2) + (d2 > t2 ? d2 - t2 : t2 - d2)) * gK;
							if (mag2 >= gMinGrad) bin2 = (uint)FloorD(min(4095.0L, mag2 * 4.0L));
						}
						if (bin2 != 0xFFFFu) InterlockedAdd(gsCandHist[bin2], 1u);
					}
				}
				uBin[j2 * gW + i2] = bin2;
			}
		GroupMemoryBarrierWithGroupSync();
	}
	for (uint b1 = gi; b1 < 4096; b1 += 256)
		if (gsCandHist[b1] != 0) InterlockedAdd(uHist[b1], gsCandHist[b1]);
}

// 2. Threshold bin from the top (ties in raster order later), as the CPU.
groupshared uint gsThrHist[4096];
[numthreads(1024, 1, 1)]
void CSThr(uint gi : SV_GroupIndex)
{
	for (uint b = gi; b < 4096; b += 1024) gsThrHist[b] = uHist[b];
	GroupMemoryBarrierWithGroupSync();
	if (gi != 0) return;
	uint cand = 0;
	for (uint k = 0; k < 4096; ++k) cand += gsThrHist[k];
	uState[S_COUNTS] = uint2(cand, 0);
	if (cand < gMinPixels)
	{
		uState[S_FLAGS] = uint2(1, 0);
		uState[S_STOP] = uint2(1, 0);
		return;
	}
	int thr = 4095;
	uint above = 0;
	[loop] while (thr > 0 && above + gsThrHist[thr] < gTarget)
	{
		above += gsThrHist[thr];
		--thr;
	}
	uState[S_THR] = uint2((uint)thr, gTarget > above ? gTarget - above : 0u);
}
)HLSL",
R"HLSL(
// 3. Per-row counts above the threshold bin and at it.
groupshared uint gsRowA, gsRowT;
[numthreads(256, 1, 1)]
void CSRowCount(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	uint j = gid.x;
	if (gi == 0) { gsRowA = 0; gsRowT = 0; }
	GroupMemoryBarrierWithGroupSync();
	uint thr = uState[S_THR].x;
	uint a = 0, t = 0;
	if (live)
		for (uint i = gi; i < gW; i += 256)
		{
			uint b = tBin[j * gW + i];
			if (b == 0xFFFFu) continue;
			if (b > thr) ++a;
			else if (b == thr) ++t;
		}
	InterlockedAdd(gsRowA, a);
	InterlockedAdd(gsRowT, t);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0 && live) uRow[j] = uint4(gsRowA, gsRowT, 0, 0);
}

// 4. Row prefix: ties allowed per row (the first tiesLeft in raster order) and each row's output offset.
groupshared uint gsPfxA[2304];
groupshared uint gsPfxT[2304];
[numthreads(1024, 1, 1)]
void CSRowPrefix(uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	for (uint j0 = gi; j0 < gH; j0 += 1024)
	{
		uint4 r = live ? uRow[j0] : uint4(0, 0, 0, 0);
		gsPfxA[j0] = r.x;
		gsPfxT[j0] = r.y;
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0 && live)
	{
		int tiesLeft = (int)uState[S_THR].y;
		int tieBase = 0;
		uint off = 0;
		for (uint j = 0; j < gH; ++j)
		{
			int t = (int)gsPfxT[j];
			int allowed = clamp(tiesLeft - tieBase, 0, t);
			uint sel = gsPfxA[j] + (uint)allowed;
			gsPfxA[j] = off;
			gsPfxT[j] = (uint)allowed;
			off += sel;
			tieBase += t;
		}
		uState[S_COUNTS] = uint2(uState[S_COUNTS].x, off);
	}
	GroupMemoryBarrierWithGroupSync();
	if (live)
		for (uint j1 = gi; j1 < gH; j1 += 1024)
			uRow[j1] = uint4(uRow[j1].x, uRow[j1].y, gsPfxA[j1], gsPfxT[j1]);
}

// 5. Compaction in raster order: contiguous segments per thread, serial scans of 256 counts.
groupshared uint gsSegT[256];
groupshared uint gsSegS[256];
[numthreads(256, 1, 1)]
void CSCompact(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	uint j = gid.x;
	uint thr = uState[S_THR].x;
	uint seg = (gW + 255) / 256;
	uint i0 = min(gi * seg, gW), i1 = min(i0 + seg, gW);
	uint a = 0, t = 0;
	if (live)
		for (uint i = i0; i < i1; ++i)
		{
			uint b = tBin[j * gW + i];
			if (b == 0xFFFFu) continue;
			if (b > thr) ++a;
			else if (b == thr) ++t;
		}
	gsSegT[gi] = t;
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
	{
		uint run = 0;
		for (uint q = 0; q < 256; ++q) { uint v = gsSegT[q]; gsSegT[q] = run; run += v; }
	}
	GroupMemoryBarrierWithGroupSync();
	uint4 row = live ? uRow[j] : uint4(0, 0, 0, 0);
	int allowed = clamp((int)row.w - (int)gsSegT[gi], 0, (int)t);
	gsSegS[gi] = a + (uint)allowed;
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
	{
		uint run2 = 0;
		for (uint q2 = 0; q2 < 256; ++q2) { uint v2 = gsSegS[q2]; gsSegS[q2] = run2; run2 += v2; }
	}
	GroupMemoryBarrierWithGroupSync();
	if (!live) return;
	uint idx = row.z + gsSegS[gi];
	int used = 0;
	for (uint i2 = i0; i2 < i1; ++i2)
	{
		uint b2 = tBin[j * gW + i2];
		if (b2 == 0xFFFFu || b2 < thr) continue;
		if (b2 == thr)
		{
			if (used >= allowed) continue;
			++used;
		}
		uPts[idx] = i2 | (j << 13);
		++idx;
	}
}

// 6. Residuals r = gain * T + offset - I(W(x; p)) per selected point (pass 0 also caches PtGeom & keeps r0 / ok0).
[numthreads(64, 1, 1)]
void CSResid(uint3 dt : SV_DispatchThreadID)
{
	if (uState[S_STOP].x != 0) return;
	uint n = uState[S_COUNTS].y;
	uint k = dt.x;
	if (k >= n) return;
	double h[9];
	LoadP(h);
	double gain = GetD(S_GAIN), offset = GetD(S_OFFSET);
	uint passIx = uState[S_PASS].x;
	uint packed = uPts[k];
	double X, Y, T;
	if (passIx == 0)
	{
		double GX, GY;
		PointOf(packed, X, Y, T, GX, GY);
		PtGeom pg;
		pg.X = SD(X); pg.Y = SD(Y); pg.T = SD(T); pg.GX = SD(GX); pg.GY = SD(GY);
		uPtGeom[k] = pg;
	}
	else
	{
		PtGeom pg = uPtGeom[k];
		X = LD(pg.X); Y = LD(pg.Y); T = LD(pg.T);
	}
	double qx, qy;
	ApplyP(h, X, Y, qx, qy);
	precise double u = (qx + gCx) * gInvS - 0.5L, v = (qy + gCy) * gInvS - 0.5L;
	double iv;
	bool ok = SampleB(u, v, iv);
	precise double r = ok ? gain * T + offset - iv : 0.0L;
	uint2 rb = SD(r);
	uint flags = (packed & IDX_MASK) | (ok ? OK_BIT : 0u);
	if (passIx == 0)
	{
		uRes[k] = uint4(rb, rb);
		flags |= ok ? OK0_BIT : 0u;
	}
	else
	{
		uint4 old = uRes[k];
		uRes[k] = uint4(rb, old.zw);
		flags |= packed & OK0_BIT;
	}
	uPts[k] = flags;
}
)HLSL",
R"HLSL(
// 7. Robust sigma: exact median (element count / 2 of the sorted |r|, the CPU's nth_element) by a 63-bit radix
// select of the non-negative double bits (GEM-142: 6 passes of 10-11 bit digits: hi 11+10+10, lo 11+11+10;
// exact 64-bit IEEE double result, 16 KB LDS, 6 passes instead of 8).
groupshared uint gsMedHist[2048];
groupshared uint gsMedScanA[1024];
groupshared uint gsMedScanB[1024];
groupshared uint gsMedSel[8];   // 0 prefix hi, 1 prefix lo, 2 rank, 3 count, 4 mask hi, 5 mask lo, 6 digits found
[numthreads(1024, 1, 1)]
void CSMedian(uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	uint n = uState[S_COUNTS].y;
	if (gi == 0) { gsMedSel[0] = 0; gsMedSel[1] = 0; gsMedSel[3] = 0; gsMedSel[4] = 0; gsMedSel[5] = 0; gsMedSel[6] = 0; }
	GroupMemoryBarrierWithGroupSync();
	uint cnt = 0;
	if (live)
		for (uint k = gi; k < n; k += 1024)
			if ((uPts[k] & OK_BIT) != 0) ++cnt;
	InterlockedAdd(gsMedSel[3], cnt);
	GroupMemoryBarrierWithGroupSync();
	uint count = gsMedSel[3];
	bool enough = count >= gMinPixels;
	if (gi == 0) gsMedSel[2] = count / 2;
	GroupMemoryBarrierWithGroupSync();
	static const uint kMedShift[6] = { 20u, 10u, 0u, 21u, 10u, 0u };
	static const uint kMedMask[6] = { 0x7FFu, 0x3FFu, 0x3FFu, 0x7FFu, 0x7FFu, 0x3FFu };
	[unroll] for (uint d = 0; d < 6; ++d)
	{
		const bool inHi = d < 3;
		const uint shift = kMedShift[d];
		const uint mask = kMedMask[d];
		const bool wide = mask > 0x3FFu;
		gsMedHist[gi] = 0;
		if (wide) gsMedHist[gi + 1024u] = 0;
		GroupMemoryBarrierWithGroupSync();
		uint pHi = gsMedSel[0], pLo = gsMedSel[1], mHi = gsMedSel[4], mLo = gsMedSel[5];
		if (live && enough)
			for (uint k2 = gi; k2 < n; k2 += 1024)
			{
				if ((uPts[k2] & OK_BIT) == 0) continue;
				uint4 rr = uRes[k2];
				uint hi = rr.y & 0x7FFFFFFFu, lo = rr.x;
				if ((hi & mHi) != pHi || (lo & mLo) != pLo) continue;
				uint word = inHi ? hi : lo;
				InterlockedAdd(gsMedHist[(word >> shift) & mask], 1u);
			}
		GroupMemoryBarrierWithGroupSync();
		uint h0 = wide ? gsMedHist[2u * gi] : gsMedHist[gi];
		uint h1 = wide ? gsMedHist[2u * gi + 1u] : 0u;
		uint pairSum = h0 + h1;
		gsMedScanA[gi] = pairSum;
		GroupMemoryBarrierWithGroupSync();
		[unroll] for (uint s = 0; s < 10; ++s)
		{
			const uint off = 1u << s;
			if ((s & 1u) == 0u)
				gsMedScanB[gi] = gsMedScanA[gi] + (gi >= off ? gsMedScanA[gi - off] : 0u);
			else
				gsMedScanA[gi] = gsMedScanB[gi] + (gi >= off ? gsMedScanB[gi - off] : 0u);
			GroupMemoryBarrierWithGroupSync();
		}
		uint incl = gsMedScanA[gi];
		uint excl = incl - pairSum;
		uint rank = gsMedSel[2];
		GroupMemoryBarrierWithGroupSync();
		if (wide)
		{
			if (h0 != 0 && excl <= rank && rank < excl + h0)
			{
				uint dig = 2u * gi;
				if (inHi) { gsMedSel[0] = pHi | (dig << shift); gsMedSel[4] = mHi | (mask << shift); }
				else { gsMedSel[1] = pLo | (dig << shift); gsMedSel[5] = mLo | (mask << shift); }
				gsMedSel[2] = rank - excl;
				gsMedSel[6] = gsMedSel[6] + 1u;
			}
			else if (h1 != 0 && excl + h0 <= rank && rank < incl)
			{
				uint dig = 2u * gi + 1u;
				if (inHi) { gsMedSel[0] = pHi | (dig << shift); gsMedSel[4] = mHi | (mask << shift); }
				else { gsMedSel[1] = pLo | (dig << shift); gsMedSel[5] = mLo | (mask << shift); }
				gsMedSel[2] = rank - (excl + h0);
				gsMedSel[6] = gsMedSel[6] + 1u;
			}
		}
		else if (h0 != 0 && excl <= rank && rank < incl)
		{
			if (inHi) { gsMedSel[0] = pHi | (gi << shift); gsMedSel[4] = mHi | (mask << shift); }
			else { gsMedSel[1] = pLo | (gi << shift); gsMedSel[5] = mLo | (mask << shift); }
			gsMedSel[2] = rank - excl;
			gsMedSel[6] = gsMedSel[6] + 1u;
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi != 0 || !live) return;
	if (!enough || gsMedSel[6] != 6u)
	{
		uState[S_FLAGS] = uint2(uState[S_FLAGS].x, 1);
		uState[S_STOP] = uint2(1, uState[S_STOP].y);
		return;
	}
	double med = asdouble(gsMedSel[1], gsMedSel[0]);
	PutD(S_SIGMA, max(gMinScale, VP_MAD_TO_SIGMA * med));
	uState[S_OKCOUNT] = uint2(count, 0);
	if (uState[S_STOP].y != 0) uState[S_STOP] = uint2(1, 1);
}

// 8. Huber-weighted normal equations: 64 threads x 16 contiguous points per group, fixed-order tree reduction.
// Partial m (27 per group): g[0..5], then H upper triangle row by row.
groupshared double gsAcc[27 * 64];
[numthreads(64, 1, 1)]
void CSNormal(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	uint n = uState[S_COUNTS].y;
	double acc[27];
	[unroll] for (uint m0 = 0; m0 < 27; ++m0) acc[m0] = 0.0L;
	if (live)
	{
		double gain = GetD(S_GAIN), sigma = GetD(S_SIGMA);
		double c = gHuberK * sigma;
		uint base = gid.x * 1024 + gi * 16;
		for (uint q = 0; q < 16; ++q)
		{
			uint k = base + q;
			if (k >= n) break;
			uint packed = uPts[k];
			if ((packed & OK_BIT) == 0) continue;
			PtGeom pg = uPtGeom[k];
			double X = LD(pg.X), Y = LD(pg.Y), T = LD(pg.T), GX = LD(pg.GX), GY = LD(pg.GY);
			double r = LD(uRes[k].xy);
			double a = abs(r);
			double wt = a <= c ? 1.0L : c * RcpD(a);
			double J[6];
			J[0] = gain * GX;
			J[1] = gain * GY;
			J[2] = 0.0L; J[3] = 0.0L; J[4] = 0.0L; J[5] = 0.0L;
			if (gNGeo == 4)
			{
				J[2] = gain * (GX * X + GY * Y) * gInvL;
				J[3] = gain * (GY * X - GX * Y) * gInvL;
				if (gFade != 0) { J[4] = T; J[5] = 1.0L; }
			}
			else if (gFade != 0) { J[2] = T; J[3] = 1.0L; }
			uint m = 6;
			[unroll] for (uint ia = 0; ia < 6; ++ia)
			{
				double wa = wt * J[ia];
				if (ia < gNPar) acc[ia] += wa * r;
				[unroll] for (uint ib = ia; ib < 6; ++ib)
				{
					if (ia < gNPar && ib < gNPar) acc[m] += wa * J[ib];
					++m;
				}
			}
		}
	}
	[unroll] for (uint m1 = 0; m1 < 27; ++m1) gsAcc[m1 * 64 + gi] = acc[m1];
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (gi < stride)
			for (uint m2 = 0; m2 < 27; ++m2) gsAcc[m2 * 64 + gi] += gsAcc[m2 * 64 + gi + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi < 27 && live) uPart[gid.x * 32 + gi] = SD(gsAcc[gi * 64]);
}
)HLSL",
R"HLSL(
// Gaussian elimination with partial pivoting (n <= 6), as the CPU Solve.
bool SolveN(inout double a[36], inout double b[6], uint n, out double x[6])
{
	[unroll] for (uint z = 0; z < 6; ++z) x[z] = 0.0L;
	double scale = 0.0L;
	for (uint i = 0; i < n; ++i) scale = max(scale, abs(a[i * 6 + i]));
	if (!(scale > 0.0L) || !FiniteD(scale)) return false;
	for (uint c = 0; c < n; ++c)
	{
		uint piv = c;
		for (uint r = c + 1; r < n; ++r)
			if (abs(a[r * 6 + c]) > abs(a[piv * 6 + c])) piv = r;
		if (!(abs(a[piv * 6 + c]) > 1e-12L * scale)) return false;
		if (piv != c)
		{
			for (uint k = 0; k < n; ++k) { double t = a[c * 6 + k]; a[c * 6 + k] = a[piv * 6 + k]; a[piv * 6 + k] = t; }
			double tb = b[c]; b[c] = b[piv]; b[piv] = tb;
		}
		double inv = RcpD(a[c * 6 + c]);
		for (uint r2 = c + 1; r2 < n; ++r2)
		{
			double f = a[r2 * 6 + c] * inv;
			for (uint k2 = c; k2 < n; ++k2) a[r2 * 6 + k2] -= f * a[c * 6 + k2];
			b[r2] -= f * b[c];
		}
	}
	for (int r3 = (int)n - 1; r3 >= 0; --r3)
	{
		double v = b[r3];
		for (uint k3 = (uint)r3 + 1; k3 < n; ++k3) v -= a[r3 * 6 + k3] * x[k3];
		x[r3] = v * RcpD(a[r3 * 6 + r3]);
		if (!FiniteD(x[r3])) return false;
	}
	return true;
}

bool AllFinite9(double h[9])
{
	bool ok = true;
	[unroll] for (uint q = 0; q < 9; ++q) ok = ok && FiniteD(h[q]);
	return ok;
}
double MaxAbs9(double h[9])
{
	double m = 0.0L;
	[unroll] for (uint q = 0; q < 9; ++q) m = max(m, abs(h[q]));
	return m;
}

// MaxCornerDifference(a, b, picture) x diag: corners of the picture and its centre (picture-centred), source px.
double CornerStep(double a[9], double b[9])
{
	double cxs[5] = { -gHw, gHw, gHw, -gHw, 0.0L };
	double cys[5] = { -gHh, -gHh, gHh, gHh, 0.0L };
	double worst = 0.0L;
	[unroll] for (uint q = 0; q < 5; ++q)
	{
		double ax, ay, bx, by;
		ApplyP(a, cxs[q], cys[q], ax, ay);
		ApplyP(b, cxs[q], cys[q], bx, by);
		double dax = ax - cxs[q], day = ay - cys[q], dbx = bx - cxs[q], dby = by - cys[q];
		double d = SqrtD((dax - dbx) * (dax - dbx) + (day - dby) * (day - dby));
		worst = max(worst, d);
	}
	return worst * gInvDiagPic * gDiag;
}

// 9. Solve + inverse-compositional update W(p) <- W(p) o W(d)^-1, convergence (one group: partial sums, then thread 0).
groupshared double gsSum[32];
[numthreads(32, 1, 1)]
void CSSolve(uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	double s = 0.0L;
	if (live && gi < 27)
		for (uint g = 0; g < gNumGroups; ++g) s += LD(uPart[g * 32 + gi]);
	gsSum[gi] = s;
	GroupMemoryBarrierWithGroupSync();
	if (gi != 0 || !live) return;
	uint n = gNPar;
	double H[36], gv[6], d[6];
	[unroll] for (uint z = 0; z < 36; ++z) H[z] = 0.0L;
	[unroll] for (uint z2 = 0; z2 < 6; ++z2) gv[z2] = gsSum[z2];
	uint m = 6;
	[unroll] for (uint ia = 0; ia < 6; ++ia)
		[unroll] for (uint ib = ia; ib < 6; ++ib)
		{
			H[ia * 6 + ib] = gsSum[m];
			++m;
		}
	for (uint ia2 = 0; ia2 < n; ++ia2)
	{
		gv[ia2] = -gv[ia2];
		for (uint ib2 = 0; ib2 < ia2; ++ib2) H[ia2 * 6 + ib2] = H[ib2 * 6 + ia2];
	}
	bool degenerate = !SolveN(H, gv, n, d);
	double p[9];
	LoadP(p);
	double next[9];
	[unroll] for (uint z3 = 0; z3 < 9; ++z3) next[z3] = p[z3];
	if (!degenerate)
	{
		// W(d): translation (d0, d1) and, for 4 parameters, the similarity (1 + a, -b; b, 1 + a), a = d2 / L, b = d3 / L.
		double sa = gNGeo == 4 ? d[2] * gInvL : 0.0L, sb = gNGeo == 4 ? d[3] * gInvL : 0.0L;
		double S[9] = { 1.0L + sa, -sb, d[0], sb, 1.0L + sa, d[1], 0.0L, 0.0L, 1.0L };
		// Invert (GlobalModelGeometry Invert, non-homography type).
		double c00 = S[4] * S[8] - S[5] * S[7];
		double c01 = S[5] * S[6] - S[3] * S[8];
		double c02 = S[3] * S[7] - S[4] * S[6];
		double det = S[0] * c00 + S[1] * c01 + S[2] * c02;
		double mS = MaxAbs9(S);
		if (!AllFinite9(S) || !FiniteD(det) || !(abs(det) > 1e-14L * mS * mS * mS))
			degenerate = true;
		else
		{
			double id = RcpD(det);
			double Si[9];
			Si[0] = c00 * id;
			Si[1] = (S[2] * S[7] - S[1] * S[8]) * id;
			Si[2] = (S[1] * S[5] - S[2] * S[4]) * id;
			Si[3] = c01 * id;
			Si[4] = (S[0] * S[8] - S[2] * S[6]) * id;
			Si[5] = (S[2] * S[3] - S[0] * S[5]) * id;
			Si[6] = c02 * id;
			Si[7] = (S[1] * S[6] - S[0] * S[7]) * id;
			Si[8] = (S[0] * S[4] - S[1] * S[3]) * id;
			if (!AllFinite9(Si))
				degenerate = true;
			else
			{
				Si[6] = 0.0L; Si[7] = 0.0L; Si[8] = 1.0L;
				// Compose: Mul3(p, Si), then NormaliseH8 (result ignored, as Compose).
				[unroll] for (uint r = 0; r < 3; ++r)
					[unroll] for (uint c = 0; c < 3; ++c)
						next[r * 3 + c] = p[r * 3 + 0] * Si[0 * 3 + c] + p[r * 3 + 1] * Si[1 * 3 + c] + p[r * 3 + 2] * Si[2 * 3 + c];
				double mN = MaxAbs9(next);
				if (abs(next[8]) > 1e-12L * mN)
				{
					double inv8 = RcpD(next[8]);
					[unroll] for (uint q = 0; q < 9; ++q) next[q] = next[q] * inv8;
					next[8] = 1.0L;
				}
				if (!AllFinite9(next))
					degenerate = true;
			}
		}
	}
	if (degenerate)
	{
		uState[S_FLAGS] = uint2(uState[S_FLAGS].x, 1);
		uState[S_STOP] = uint2(1, uState[S_STOP].y);
		return;
	}
	double step = CornerStep(next, p);
	PutD(S_LAST, step);
	[unroll] for (uint q2 = 0; q2 < 9; ++q2) PutD(S_P + q2, next[q2]);
	if (gFade != 0)
	{
		PutD(S_GAIN, GetD(S_GAIN) + d[gNGeo]);
		PutD(S_OFFSET, GetD(S_OFFSET) + d[gNGeo + 1]);
	}
	uint iterations = uState[S_PASS].y + 1;
	uState[S_PASS] = uint2(uState[S_PASS].x + 1, iterations);
	bool done = !(step > gConvThr) || iterations >= gMaxIter;
	uState[S_STOP] = uint2(0, done ? 1u : 0u);
}

// 10. Verification sums: mean Huber cost before / after over the points valid under both models, same sigma.
groupshared double gsVer[3 * 64];
[numthreads(64, 1, 1)]
void CSVerify(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	uint2 fl = uState[S_FLAGS];
	bool live = fl.x == 0 && fl.y == 0;
	uint n = uState[S_COUNTS].y;
	double before = 0.0L, after = 0.0L, both = 0.0L;
	if (live)
	{
		double c = gHuberK * GetD(S_SIGMA);
		uint base = gid.x * 1024 + gi * 16;
		for (uint q = 0; q < 16; ++q)
		{
			uint k = base + q;
			if (k >= n) break;
			uint packed = uPts[k];
			if ((packed & OK_BIT) == 0 || (packed & OK0_BIT) == 0) continue;
			uint4 rr = uRes[k];
			before += Huber(LD(rr.zw), c);
			after += Huber(LD(rr.xy), c);
			both += 1.0L;
		}
	}
	gsVer[gi] = before;
	gsVer[64 + gi] = after;
	gsVer[128 + gi] = both;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32; stride > 0; stride >>= 1)
	{
		if (gi < stride)
		{
			gsVer[gi] += gsVer[gi + stride];
			gsVer[64 + gi] += gsVer[64 + gi + stride];
			gsVer[128 + gi] += gsVer[128 + gi + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi < 3 && live) uPart[gid.x * 32 + gi] = SD(gsVer[gi * 64]);
}

[numthreads(32, 1, 1)]
void CSFinal(uint gi : SV_GroupIndex)
{
	uint2 fl = uState[S_FLAGS];
	if (fl.x != 0 || fl.y != 0 || gi >= 3) return;
	double s = 0.0L;
	for (uint g = 0; g < gNumGroups; ++g) s += LD(uPart[g * 32 + gi]);
	PutD(S_BEFORE + gi, s);
}
)HLSL"
};
