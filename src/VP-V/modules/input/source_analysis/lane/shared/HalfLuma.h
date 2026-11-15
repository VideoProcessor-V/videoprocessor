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
//   engine views of P1 (C9, additive): half = the top byte of the P1 half sample of a 10-bit source (exact, see
//              Engine8FromP1); quarter = (a + b + c + d + 2) / 4 on the 8-bit HALF view (the producer's own law)
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
	// The floating-point form of the same mean, the arithmetic of the GLSL accessor vpu_half_luma ((a + b + c + d) x 1/4,
	// same tap order). Additive (grain split v3, refs/116): grid, taps and the integer rounding above are unchanged.
	template <class F>
	inline F MeanF(F a, F b, F c, F d) { return (a + b + c + d) * F(0.25); }
	// Engine view finishing: 10-bit codes to the 8-bit mean code / 4.
	inline uint8_t FinishEngine8(uint32_t sum) { return static_cast<uint8_t>((std::min)(255u, sum >> 4)); }

	// Engine views of P1 (C9 commit A, doc 93 C9P s2a). Additive: the grid, the taps and the rounding above are unchanged,
	// so kUnitVersion stays. For a 10-bit source (codes << 6, msbShift 6: every lane tap view) the P1 half sample is
	// FinishP1(64 * S) = 16 * S exactly (S = a + b + c + d of the codes, 0..4092), so its top byte is S >> 4 =
	// FinishEngine8(S): the engine half comes from P1 with no second rounding.
	inline uint8_t Engine8FromP1(uint16_t p1) { return static_cast<uint8_t>(p1 >> 8); }
	// The engine quarter: the rounded mean of four 8-bit engine HALF samples, fixed order (moved from the optical-flow
	// producer's de-judder size). It is NOT the top byte of the P1 quarter: half sums S = (16, 16, 0, 0) give engine half
	// (1, 1, 0, 0) and quarter (1 + 1 + 0 + 0 + 2) / 4 = 1, while the P1 quarter is (256 + 256 + 2) >> 2 = 128, top byte 0.
	inline uint8_t QuarterEngine8(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
	{
		return static_cast<uint8_t>((a + b + c + d + 2u) / 4u);
	}

	// GLSL accessors (the contract). Both read an R-channel luma texture with texelFetch, q in output coordinates, second
	// taps clamped to the texture edge. vpu_quarter_luma takes the HALF texture. GlslComponent() adds the same law for any
	// channel (vpu_half_luma_comp; used by the grain split v3 HALF pass, which serves luma and semi-planar chroma).
	//
	// ONE GLSL body (U3 one copy, gate-u3fix SHOULD): every GLSL entry is emitted by Detail::GlslBox from the same tap
	// lines, the fixed tap order a, b, c, d and the one mean expression Detail::kGlslMean (the GLSL form of MeanF). The
	// entries differ only on three axes: the value type and channel select (.r, [comp], all four), the source origin, and
	// the edge rule (min to the last sample, or clamp to [0, size - 1] when an origin is added). The text each entry emits
	// is byte-identical to its former hand-written body (HalfLumaUnitTests pins it), so kUnitVersion stays.
	namespace Detail
	{
		// The one GLSL mean of the four taps: the sum in the fixed order a, b, c, d, then x 1/4 (MeanF).
		constexpr const char* kGlslMean = "(a + b + c + d) * 0.25";

		// `type signature { hi; p0 = tap0; p1 = tap1; a..d = texelFetch(...)select; return kGlslMean; }`.
		inline std::string GlslBox(const char* type, const char* signature, const char* tap0, const char* tap1,
			const char* select)
		{
			const char* const names[4] = { "a", "b", "c", "d" };
			const char* const taps[4] = { "p0.x, p0.y", "p1.x, p0.y", "p0.x, p1.y", "p1.x, p1.y" };
			std::string s = std::string(type) + " " + signature + "\n{\n"
				"    ivec2 hi = textureSize(src, 0) - ivec2(1);\n"
				"    ivec2 p0 = " + tap0 + ";\n"
				"    ivec2 p1 = " + tap1 + ";\n";
			for (int i = 0; i < 4; ++i)
				s += std::string("    ") + type + " " + names[i] + " = texelFetch(src, ivec2(" + taps[i] + "), 0)" + select + ";\n";
			return s + "    return " + kGlslMean + ";\n}\n";
		}
	}

	inline std::string Glsl()
	{
		return std::string("#define VPU_HALF_LUMA_VERSION ") + std::to_string(kUnitVersion) + "\n" +
			Detail::GlslBox("float", "vpu_half_luma(sampler2D src, ivec2 q)", "min(q * 2, hi)", "min(q * 2 + ivec2(1), hi)", ".r") +
			"float vpu_quarter_luma(sampler2D halfSrc, ivec2 q)\n{\n"
			"    return vpu_half_luma(halfSrc, q);\n}\n";
	}
	// Additive (grain split v3, refs/116): the same taps, order and mean on channel `comp` (0..3) of the texture.
	// The existing entries above are unchanged, so kUnitVersion stays.
	inline std::string GlslComponent()
	{
		return Detail::GlslBox("float", "vpu_half_luma_comp(sampler2D src, ivec2 q, int comp)", "min(q * 2, hi)",
			"min(q * 2 + ivec2(1), hi)", "[comp]");
	}
	// Additive (U3 one copy, owner 2026-11-15): the same taps, order and mean on all four channels, output sample q read
	// at source offset `origin` (origin + q * 2), taps clamped to [0, size - 1]. Moved from the output downscaler's
	// private doubler-halving pass (AdaptiveDownscale HALVE_BODY, which summed a, b, c, d in this order from 0 and scaled
	// by 0.25). Consumers: AdaptiveDownscale HALVE_BODY, the interpolator's LOW_HALF_BODY (origin 0). The existing
	// entries above are unchanged, so kUnitVersion stays.
	inline std::string GlslRgbaAt()
	{
		return Detail::GlslBox("vec4", "vpu_half_luma_rgba_at(sampler2D src, ivec2 origin, ivec2 q)",
			"clamp(origin + q * 2, ivec2(0), hi)", "clamp(origin + q * 2 + ivec2(1), ivec2(0), hi)", "");
	}
	// Additive (U3 one copy, owner 2026-11-15): the mean alone, for a consumer whose four taps are not plain texture
	// fetches (texture restore's DetailPulse reads them through its own picture-clamped code_at / quad_codes; the taps
	// stay the consumer's, the 2x2 law is this one). Same order and mean as every entry above (kGlslMean); moved from
	// DetailPulse load_low (`0.25 * (a + b + c + d)`) and LOW_BODY_FAST (`0.25 * (q.x + q.y + q.z + q.w)`): x * 0.25 and
	// 0.25 * x are the same IEEE product, so the move is bit-identical. Additive, kUnitVersion stays.
	inline std::string GlslMean4()
	{
		return std::string("float vpu_half_luma_mean4(float a, float b, float c, float d)\n{\n"
			"    return ") + Detail::kGlslMean + ";\n}\n";
	}
}
}
