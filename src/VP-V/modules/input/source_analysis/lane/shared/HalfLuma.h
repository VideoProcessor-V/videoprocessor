// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared unit U3 "half / quarter luma pyramid" (doc 62 section 1.2): the one 2x2 box law behind product P1.
// The lane's CPU producer (LaneCpuProducts.h), the optical-flow producer's 8-bit engine view
// (MotionAheadProducer.cpp HalfLuma8) and the GLSL accessors `vpu_half_luma` / `vpu_quarter_luma` all take their
// grid sizes, edge replication and rounding from here.
//
// Law (moved unchanged from the private copies):
//   grid       half = ceil(w/2) x ceil(h/2), quarter = ceil(half/2): ceil(w/4) x ceil(h/4) for the P1 layout
//   taps       source index 2x and 2x+1, the second clamped to the last sample (odd edges replicate)
//   P1 finish  (a + b + c + d + 2) >> 2 on 16-bit-scaled samples (half and quarter)
//   engine 8   min(255, (a + b + c + d) >> 4) on 10-bit codes (the mean code / 4), the NVOF input
// Determinism: integer arithmetic only, fixed tap order a, b, c, d = (x0,y0) (x1,y0) (x0,y1) (x1,y1).
//
// Header-only, C++14, no Windows or GPU headers.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace VpUnit
{
namespace HalfLuma
{
	// Bump on ANY change of the grid, the tap order, the rounding or the GLSL entry names (P1's ProductDecl and every
	// published slot carry it).
	constexpr uint32_t kUnitVersion = 1;

	inline uint32_t HalfDim(uint32_t v) { return (v + 1u) / 2u; }
	inline uint32_t QuarterDim(uint32_t v) { return (v + 3u) / 4u; }
	// P1 buffer: the half plane (uint16) then the quarter plane (uint16), tightly packed.
	inline size_t HalfBytes(uint32_t w, uint32_t h) { return size_t(HalfDim(w)) * HalfDim(h) * 2u; }
	inline size_t QuarterBytes(uint32_t w, uint32_t h) { return size_t(QuarterDim(w)) * QuarterDim(h) * 2u; }
	inline size_t TotalBytes(uint32_t w, uint32_t h) { return HalfBytes(w, h) + QuarterBytes(w, h); }

	// Source tap indices of output sample x along an axis of n samples: 2x and min(2x + 1, n - 1).
	template <class I>
	inline I Tap0(I x, I n) { return (std::min)(I(2) * x, n - I(1)); }
	template <class I>
	inline I Tap1(I x, I n) { return (std::min)(I(2) * x + I(1), n - I(1)); }

	// The 2x2 box sum of four samples (already scaled to the working width), fixed order.
	inline uint32_t Box4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a + b + c + d; }
	// P1 finishing (half and quarter): the rounded mean.
	inline uint16_t FinishP1(uint32_t sum) { return uint16_t((sum + 2u) >> 2); }
	// Engine view finishing: 10-bit codes to the 8-bit mean code / 4.
	inline uint8_t FinishEngine8(uint32_t sum) { return static_cast<uint8_t>((std::min)(255u, sum >> 4)); }

	// GLSL accessors (the contract; not wired into a pass yet). Both read an R-channel luma texture with texelFetch,
	// q in output coordinates, second taps clamped to the texture edge. vpu_quarter_luma takes the HALF texture.
	inline std::string Glsl()
	{
		return std::string("#define VPU_HALF_LUMA_VERSION ") + std::to_string(kUnitVersion) + "\n"
			"float vpu_half_luma(sampler2D src, ivec2 q)\n{\n"
			"    ivec2 hi = textureSize(src, 0) - ivec2(1);\n"
			"    ivec2 p0 = min(q * 2, hi);\n"
			"    ivec2 p1 = min(q * 2 + ivec2(1), hi);\n"
			"    float a = texelFetch(src, ivec2(p0.x, p0.y), 0).r;\n"
			"    float b = texelFetch(src, ivec2(p1.x, p0.y), 0).r;\n"
			"    float c = texelFetch(src, ivec2(p0.x, p1.y), 0).r;\n"
			"    float d = texelFetch(src, ivec2(p1.x, p1.y), 0).r;\n"
			"    return (a + b + c + d) * 0.25;\n}\n"
			"float vpu_quarter_luma(sampler2D halfSrc, ivec2 q)\n{\n"
			"    return vpu_half_luma(halfSrc, q);\n}\n";
	}
}
}
