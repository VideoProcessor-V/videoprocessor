// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// HLSL (cs_5_0) of the GPU photometric refinement (PhotometricRefineGpu.h), compiled at run time with
// D3DCompile (IEEE strictness). Doubles use only the SM 5.0 base double ops (add, mul, min, max, compare,
// float <-> double conversion); division, reciprocal, square root and floor are built from float seeds with Newton
// steps / exact corrections, so no device needs the 11.1 extended double instructions (checked at Init on the
// disassembly). Each piece is one raw literal (the MSVC literal limit); they are concatenated at compile time.
static const char* const kRefineHlsl[] = {
R"HLSL(
cbuffer RefineCB : register(b0)
{
	uint gW, gH, gUseMask, gGridW;
	uint gBicubic, gNGeo, gNPar, gFade;
	uint gTarget, gMinPixels, gMaxIter, gUnit;
	uint gNumGroups, gPad0, gPad1, gPad2;
	double gS, gInvS;
	double gInv2S, gK;
	double gCx, gCy;
	double gL, gInvL;
	double gDiag, gInvDiagPic;
	double gMinGrad, gHuberK;
	double gMinScale, gConvThr;
	double gHw, gHh;
};

Texture2D<uint> tA : register(t0);
Texture2D<uint> tB : register(t1);
StructuredBuffer<int> sColBx : register(t2);
StructuredBuffer<int> sRowBy : register(t3);
StructuredBuffer<uint> sCell : register(t4);

RWStructuredBuffer<uint> uBin : register(u0);
RWStructuredBuffer<uint> uHist : register(u1);
RWStructuredBuffer<uint4> uRow : register(u2);
RWStructuredBuffer<uint> uPts : register(u3);
RWStructuredBuffer<uint4> uRes : register(u4);
RWStructuredBuffer<uint2> uPart : register(u5);
RWStructuredBuffer<uint2> uState : register(u6);

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
	w0 = ((-0.5L * t + 1.0L) * t - 0.5L) * t;
	w1 = (1.5L * t - 2.5L) * t * t + 1.0L;
	w2 = ((-1.5L * t + 2.0L) * t + 0.5L) * t;
	w3 = (0.5L * t - 0.5L) * t * t;
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
		Keys(u - ToDi(x0), wx0, wx1, wx2, wx3);
		Keys(v - ToDi(y0), wy[0], wy[1], wy[2], wy[3]);
		double acc = 0.0L;
		[unroll] for (int r = 0; r < 4; ++r)
		{
			int yy = y0 - 1 + r;
			acc += wy[r] * (wx0 * CodeB(x0 - 1, yy) + wx1 * CodeB(x0, yy) + wx2 * CodeB(x0 + 1, yy) + wx3 * CodeB(x0 + 2, yy));
		}
		val = acc * gK;
		return true;
	}
	if (!(u >= 0.0L && v >= 0.0L && u < ToD(gW) - 1.0L && v < ToD(gH) - 1.0L))
		return false;
	int bx0 = FloorD(u), by0 = FloorD(v);
	double fx = u - ToDi(bx0), fy = v - ToDi(by0);
	double a = (1.0L - fx) * CodeB(bx0, by0) + fx * CodeB(bx0 + 1, by0);
	double b = (1.0L - fx) * CodeB(bx0, by0 + 1) + fx * CodeB(bx0 + 1, by0 + 1);
	val = ((1.0L - fy) * a + fy * b) * gK;
	return true;
}

void LoadP(out double h[9])
{
	[unroll] for (uint q = 0; q < 9; ++q) h[q] = GetD(S_P + q);
}

void ApplyP(double h[9], double x, double y, out double ox, out double oy)
{
	double w = h[6] * x + h[7] * y + h[8];
	double iw = (w != 0.0L) ? RcpD(w) : 0.0L;
	ox = (h[0] * x + h[1] * y + h[2]) * iw;
	oy = (h[3] * x + h[4] * y + h[5]) * iw;
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
	X = gS * (ToDi(i) + 0.5L) - gCx;
	Y = gS * (ToDi(j) + 0.5L) - gCy;
	T = CodeA(i, j) * gK;
	GX = (CodeA(i + 1, j) * gK - CodeA(i - 1, j) * gK) * gInv2S;
	GY = (CodeA(i, j + 1) * gK - CodeA(i, j - 1) * gK) * gInv2S;
}

[numthreads(64, 1, 1)]
void CSClear(uint3 dt : SV_DispatchThreadID)
{
	if (dt.x < 4096) uHist[dt.x] = 0;
}

// 1. Candidates: inlier-block pixels inside the picture margin with |dx| + |dy| >= minGradient; quarter-code bins.
groupshared uint gsCandHist[4096];
[numthreads(16, 16, 1)]
void CSCand(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	for (uint b0 = gi; b0 < 4096; b0 += 256) gsCandHist[b0] = 0;
	GroupMemoryBarrierWithGroupSync();
	for (uint ya = 0; ya < 4; ++ya)
		for (uint xa = 0; xa < 4; ++xa)
		{
			uint i = gid.x * 64 + tid.x + 16 * xa, j = gid.y * 64 + tid.y + 16 * ya;
			if (i >= gW || j >= gH) continue;
			uint bin = 0xFFFFu;
			if (i >= 1 && j >= 1 && i + 1 < gW && j + 1 < gH)
			{
				int bx = sColBx[i], by = sRowBy[j];
				if (bx >= 0 && by >= 0 && (gUseMask == 0 || sCell[(uint)by * gGridW + (uint)bx] != 0))
				{
					uint l = tA.Load(int3(i - 1, j, 0)), r = tA.Load(int3(i + 1, j, 0));
					uint t = tA.Load(int3(i, j - 1, 0)), d = tA.Load(int3(i, j + 1, 0));
					if (gUnit != 0)
					{
						uint mag = (r > l ? r - l : l - r) + (d > t ? d - t : t - d);
						if (ToD(mag) >= gMinGrad) bin = min(4095u, mag * 4u);
					}
					else
					{
						double mag = abs(ToD(r) * gK - ToD(l) * gK) + abs(ToD(d) * gK - ToD(t) * gK);
						if (mag >= gMinGrad) bin = (uint)FloorD(min(4095.0L, mag * 4.0L));
					}
					if (bin != 0xFFFFu) InterlockedAdd(gsCandHist[bin], 1u);
				}
			}
			uBin[j * gW + i] = bin;
		}
	GroupMemoryBarrierWithGroupSync();
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
			uint b = uBin[j * gW + i];
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
			uint b = uBin[j * gW + i];
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
		uint b2 = uBin[j * gW + i2];
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

// 6. Residuals r = gain * T + offset - I(W(x; p)) per selected point (pass 0 also keeps r0 / ok0).
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
	double X, Y, T, GX, GY;
	PointOf(packed, X, Y, T, GX, GY);
	double qx, qy;
	ApplyP(h, X, Y, qx, qy);
	double u = (qx + gCx) * gInvS - 0.5L, v = (qy + gCy) * gInvS - 0.5L;
	double iv;
	bool ok = SampleB(u, v, iv);
	double r = ok ? gain * T + offset - iv : 0.0L;
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
// 7. Robust sigma: exact median (element count / 2 of the sorted |r|, the CPU's nth_element) by a 64-bit radix
// select of the double bits (6 digits of 11 / 11 / 10 bits per word), one group.
groupshared uint gsMedHist[2048];
groupshared uint gsMedScan[1024];
groupshared uint gsMedSel[8];   // 0 prefix hi, 1 prefix lo, 2 rank, 3 count, 4 mask hi, 5 mask lo
[numthreads(1024, 1, 1)]
void CSMedian(uint gi : SV_GroupIndex)
{
	bool live = uState[S_STOP].x == 0;
	uint n = uState[S_COUNTS].y;
	if (gi == 0) { gsMedSel[0] = 0; gsMedSel[1] = 0; gsMedSel[3] = 0; gsMedSel[4] = 0; gsMedSel[5] = 0; }
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
	for (uint d = 0; d < 6; ++d)
	{
		uint part = d % 3;
		uint shift = part == 0 ? 21u : (part == 1 ? 10u : 0u);
		uint width = part == 2 ? 10u : 11u;
		uint dmask = (1u << width) - 1u;
		gsMedHist[2 * gi] = 0;
		gsMedHist[2 * gi + 1] = 0;
		GroupMemoryBarrierWithGroupSync();
		uint pHi = gsMedSel[0], pLo = gsMedSel[1], mHi = gsMedSel[4], mLo = gsMedSel[5];
		if (live && enough)
			for (uint k2 = gi; k2 < n; k2 += 1024)
			{
				if ((uPts[k2] & OK_BIT) == 0) continue;
				uint4 rr = uRes[k2];
				uint hi = rr.y & 0x7FFFFFFFu, lo = rr.x;
				if ((hi & mHi) != pHi || (lo & mLo) != pLo) continue;
				uint word = d < 3 ? hi : lo;
				InterlockedAdd(gsMedHist[(word >> shift) & dmask], 1u);
			}
		GroupMemoryBarrierWithGroupSync();
		uint h0 = gsMedHist[2 * gi], h1 = gsMedHist[2 * gi + 1];
		gsMedScan[gi] = h0 + h1;
		GroupMemoryBarrierWithGroupSync();
		for (uint off = 1; off < 1024; off <<= 1)
		{
			uint v = gsMedScan[gi];
			if (gi >= off) v += gsMedScan[gi - off];
			GroupMemoryBarrierWithGroupSync();
			gsMedScan[gi] = v;
			GroupMemoryBarrierWithGroupSync();
		}
		uint incl = gsMedScan[gi], excl = incl - (h0 + h1);
		uint rank = gsMedSel[2];
		GroupMemoryBarrierWithGroupSync();
		if (excl <= rank && rank < incl)
		{
			uint bin = rank < excl + h0 ? 2 * gi : 2 * gi + 1;
			uint newRank = rank < excl + h0 ? rank - excl : rank - excl - h0;
			if (d < 3) { gsMedSel[0] = pHi | (bin << shift); gsMedSel[4] = mHi | (dmask << shift); }
			else { gsMedSel[1] = pLo | (bin << shift); gsMedSel[5] = mLo | (dmask << shift); }
			gsMedSel[2] = newRank;
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi != 0 || !live) return;
	if (!enough)
	{
		uState[S_FLAGS] = uint2(uState[S_FLAGS].x, 1);
		uState[S_STOP] = uint2(1, uState[S_STOP].y);
		return;
	}
	double med = asdouble(gsMedSel[1], gsMedSel[0]);
	PutD(S_SIGMA, max(gMinScale, 1.4826L * med));
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
			double X, Y, T, GX, GY;
			PointOf(packed, X, Y, T, GX, GY);
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
