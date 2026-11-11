// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 5a: the GPU twin of the optical-flow producer's engine luma (MotionAhead::HalfLuma8 + the producer's
// quarter step, MotionAheadProducer.cpp). Compute shader, cs_5_0, compiled at run time by EngineLumaGpu.cpp, which
// #includes this file as one C++ raw string literal (the R"VPHLSL( ... )VPHLSL" markers below). Integer arithmetic only:
// the bytes equal the CPU law by construction (EngineLumaGpuTwinTests pins it).
//
// Input  t0: the raw frame bytes as an R8G8B8A8_UNORM texture (rowBytes / 4 texels per row, one texture row per frame
//            row; an EarlyUpload slot). A texel's four channels are bytes 0..3 of a little-endian 32-bit word.
// Output u0: R8_UNORM (the NVOF input format), outW x outH.
// Law (VpUnit::HalfLuma U3 + MotionAheadProducer.cpp):
//   code     P010 / P210 luma plane: word16 >> 6; v210: the 10-bit Y field; UYVY / HDYC: byte << 2
//   half     taps 2x and min(2x + 1, n - 1) on both axes, min(255, (a + b + c + d) >> 4)
//   quarter  the same taps on the half grid, (a + b + c + d + 2) / 4
R"VPHLSL(
cbuffer EngineLumaParams : register(b0)
{
	uint srcW;      // frame width in pixels
	uint srcH;      // frame height in rows
	uint packing;   // 0 planar 16-bit (P010 / P210 luma plane), 1 v210, 2 UYVY / HDYC (8-bit)
	uint quarter;   // 0: half output; 1: quarter output (half of the half grid)
	uint halfW;
	uint halfH;
	uint outW;
	uint outH;
};

Texture2D<float4> rawBytes : register(t0);
RWTexture2D<unorm float> engineOut : register(u0);

// The little-endian 32-bit word at byte offset `byteOffset` (a multiple of 4) of row `row`.
uint WordAt(uint byteOffset, uint row)
{
	const uint4 b = (uint4)(rawBytes.Load(int3(int(byteOffset >> 2), int(row), 0)) * 255.0f + 0.5f);
	return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}

// The 10-bit luma code of pixel (x, y), as MotionAhead::HalfLuma8 decodes it.
uint CodeAt(uint x, uint y)
{
	if (packing == 0u)
	{
		const uint word = WordAt((x * 2u) & ~3u, y);
		const uint sample16 = (x & 1u) != 0u ? (word >> 16) : (word & 0xffffu);
		return sample16 >> 6;
	}
	if (packing == 1u)
	{
		// Six pixels per 16 bytes: Y0 = w0 >> 10, Y1 = w1, Y2 = w1 >> 20, Y3 = w2 >> 10, Y4 = w3, Y5 = w3 >> 20.
		const uint group = x / 6u;
		const uint k = x - group * 6u;
		const uint wordIndex = (k * 2u + 1u) / 3u;
		const uint phase = k % 3u;
		const uint shift = phase == 0u ? 10u : (phase == 1u ? 0u : 20u);
		return (WordAt(group * 16u + wordIndex * 4u, y) >> shift) & 0x3ffu;
	}
	// UYVY / HDYC: U Y0 V Y1 per 4 bytes, 8-bit, to 10-bit by << 2.
	const uint word = WordAt((x / 2u) * 4u, y);
	const uint luma8 = (x & 1u) != 0u ? (word >> 24) : ((word >> 8) & 0xffu);
	return luma8 << 2;
}

uint HalfAt(uint hx, uint hy)
{
	const uint x0 = min(2u * hx, srcW - 1u), x1 = min(2u * hx + 1u, srcW - 1u);
	const uint y0 = min(2u * hy, srcH - 1u), y1 = min(2u * hy + 1u, srcH - 1u);
	const uint sum = CodeAt(x0, y0) + CodeAt(x1, y0) + CodeAt(x0, y1) + CodeAt(x1, y1);
	return min(255u, sum >> 4);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= outW || id.y >= outH)
		return;
	uint value;
	if (quarter == 0u)
		value = HalfAt(id.x, id.y);
	else
	{
		const uint x0 = min(2u * id.x, halfW - 1u), x1 = min(2u * id.x + 1u, halfW - 1u);
		const uint y0 = min(2u * id.y, halfH - 1u), y1 = min(2u * id.y + 1u, halfH - 1u);
		value = (HalfAt(x0, y0) + HalfAt(x1, y0) + HalfAt(x0, y1) + HalfAt(x1, y1) + 2u) / 4u;
	}
	engineOut[id.xy] = float(value) / 255.0f;
}
)VPHLSL"
