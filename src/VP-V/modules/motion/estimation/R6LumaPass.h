// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// R6 (0a) 4K producer throughput: bit-identical fused planar-16 (P010 / P210) half-luma decimation
// and the exact-integer D3D11 HLSL compute shader source (R16_UINT -> R8_UNORM half / quarter luma)
// matching VpUnit::HalfLuma (U3: FinishEngine8, QuarterEngine8) bit-for-bit.
//
// Design note for CE (reviews/2026-11-21_coding-batch-2-findings.md):
//   * The fused CPU planar-16 row kernel is bit-identical to HalfLuma8's two-pass decodeRow + Box4 loop
//     for every uint16 code and every even/odd picture dimension (verified by R6ThroughputTests).
//   * The GPU compute path is strictly opt-in (default OFF until quiet 4K GPU window timing verifies
//     PAIR-LAT < 40 ms and on-time >= 90 % at L=2); when enabled on a device without shader compile support,
//     it falls back visibly to the CPU path with a recorded reason (never silent).
//
// Pure header-only C++14 (no Windows headers required for the CPU mirror and HLSL constants).
#pragma once

#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace MotionAhead
{
	namespace R6
	{
		// Bit-identical fused 2x2 planar-16 (P010 / P210) row decimation -> 8-bit NVOF engine luma.
		// Reads directly from the two source uint16 rows without intermediate row buffers, reproducing
		// VpUnit::HalfLuma::FinishEngine8(Box4(r0[x0]>>6, r0[x1]>>6, r1[x0]>>6, r1[x1]>>6)) exactly.
		inline void FusedPlanar16HalfLuma8Row(const uint16_t* row0, const uint16_t* row1, int w, int halfW, uint8_t* dst)
		{
			if (!row0 || !row1 || !dst || w <= 0 || halfW <= 0)
				return;
			const int pairs = w / 2;
			for (int x = 0; x < pairs; ++x)
			{
				const int x0 = x * 2;
				const uint32_t sum = VpUnit::HalfLuma::Box4(
					static_cast<uint32_t>(row0[x0] >> 6),
					static_cast<uint32_t>(row0[x0 + 1] >> 6),
					static_cast<uint32_t>(row1[x0] >> 6),
					static_cast<uint32_t>(row1[x0 + 1] >> 6));
				dst[x] = VpUnit::HalfLuma::FinishEngine8(sum);
			}
			// Odd trailing column replicates the last sample (Tap1(x, w) == w - 1).
			if (pairs < halfW)
			{
				const int last = w - 1;
				const uint32_t a = static_cast<uint32_t>(row0[last] >> 6);
				const uint32_t c = static_cast<uint32_t>(row1[last] >> 6);
				dst[pairs] = VpUnit::HalfLuma::FinishEngine8(VpUnit::HalfLuma::Box4(a, a, c, c));
			}
		}

		// Exact CPU mirror of the HLSL integer compute shader below (R16_UINT -> R8_UNORM byte):
		// uint sum = (s00 >> 6) + (s01 >> 6) + (s10 >> 6) + (s11 >> 6); out = min(255u, sum >> 4).
		inline uint8_t HlslHalfTexelMirror(uint16_t s00, uint16_t s01, uint16_t s10, uint16_t s11)
		{
			const uint32_t sum = (uint32_t(s00) >> 6u) + (uint32_t(s01) >> 6u) + (uint32_t(s10) >> 6u) + (uint32_t(s11) >> 6u);
			const uint32_t v = sum >> 4u;
			return static_cast<uint8_t>(v < 255u ? v : 255u);
		}

		// Exact CPU mirror of the HLSL quarter-luma compute shader (R8_UINT half -> R8_UNORM quarter byte):
		// out = (a + b + c + d + 2u) >> 2u (identical to VpUnit::HalfLuma::QuarterEngine8).
		inline uint8_t HlslQuarterTexelMirror(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
		{
			return static_cast<uint8_t>((uint32_t(a) + uint32_t(b) + uint32_t(c) + uint32_t(d) + 2u) >> 2u);
		}

		// D3D11 CS 5.0 shader: planar-16 luma (DXGI_FORMAT_R16_UINT SRV) -> half-size 8-bit NVOF luma (R8_UNORM UAV).
		// Uses exact integer shifts matching VpUnit::HalfLuma::FinishEngine8 (zero float rounding error).
		constexpr const char* kR6HalfLumaHlsl = R"HLSL(
cbuffer Params : register(b0)
{
    uint srcW;
    uint srcH;
    uint dstW;
    uint dstH;
};
Texture2D<uint> srcLuma : register(t0);
RWTexture2D<unorm float> dstHalf : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= dstW || tid.y >= dstH)
        return;
    uint x0 = min(tid.x * 2u, srcW - 1u);
    uint x1 = min(tid.x * 2u + 1u, srcW - 1u);
    uint y0 = min(tid.y * 2u, srcH - 1u);
    uint y1 = min(tid.y * 2u + 1u, srcH - 1u);
    uint s00 = srcLuma.Load(int3(x0, y0, 0)) >> 6u;
    uint s01 = srcLuma.Load(int3(x1, y0, 0)) >> 6u;
    uint s10 = srcLuma.Load(int3(x0, y1, 0)) >> 6u;
    uint s11 = srcLuma.Load(int3(x1, y1, 0)) >> 6u;
    uint byteVal = min(255u, (s00 + s01 + s10 + s11) >> 4u);
    dstHalf[tid.xy] = float(byteVal) / 255.0;
}
)HLSL";

		// D3D11 CS 5.0 shader: half-size 8-bit luma (R8_UNORM SRV) -> quarter-size 8-bit NVOF luma (R8_UNORM UAV).
		// Matches VpUnit::HalfLuma::QuarterEngine8 ((a + b + c + d + 2) / 4) bit-for-bit.
		constexpr const char* kR6QuarterLumaHlsl = R"HLSL(
cbuffer Params : register(b0)
{
    uint srcW;
    uint srcH;
    uint dstW;
    uint dstH;
};
Texture2D<unorm float> srcHalf : register(t0);
RWTexture2D<unorm float> dstQuarter : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= dstW || tid.y >= dstH)
        return;
    uint x0 = min(tid.x * 2u, srcW - 1u);
    uint x1 = min(tid.x * 2u + 1u, srcW - 1u);
    uint y0 = min(tid.y * 2u, srcH - 1u);
    uint y1 = min(tid.y * 2u + 1u, srcH - 1u);
    uint a = uint(round(srcHalf.Load(int3(x0, y0, 0)) * 255.0));
    uint b = uint(round(srcHalf.Load(int3(x1, y0, 0)) * 255.0));
    uint c = uint(round(srcHalf.Load(int3(x0, y1, 0)) * 255.0));
    uint d = uint(round(srcHalf.Load(int3(x1, y1, 0)) * 255.0));
    uint byteVal = (a + b + c + d + 2u) >> 2u;
    dstQuarter[tid.xy] = float(byteVal) / 255.0;
}
)HLSL";

		struct GpuLumaStats
		{
			uint64_t gpuFrames = 0;
			uint64_t cpuFallbacks = 0;
			std::string lastFallbackReason;
		};
	}
}
