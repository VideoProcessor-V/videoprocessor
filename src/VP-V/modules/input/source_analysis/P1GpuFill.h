// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P1 GPU form (L-B4, iface-L-CORE section 7): the kernel that fills the P1 half plane on the render device, and the exact CPU
// emulation of it that the identity test compares with the lane's law (VpLaneProducts::HalfLumaRows).
//   kPrecisionUnorm16  R16 unorm, FULL 16-bit scale, msb-aligned codes: stored k = (a + b + c + d + 2) >> 2 on the msb-aligned
//                      16-bit codes, so GPU == CPU bit-exact (the P1 default variant).
//   kPrecisionFloat32  R32F, the unrounded 2x2 mean in the plane's normalised units, (a + b + c + d) * 0.25 in the accessor's
//                      tap order (vpu_half_luma): the variant the bands (L-B3) and the matcher read. The matcher multiplies by its
//                      `codes` on READ; a bit-identical vector run therefore needs THIS variant (the R16 unorm mean differs from
//                      it by < 0.5 / 65535 of full scale, about 0.002 codes at 255). No R16F 8-bit-code variant is made.
// The source plane is read through texelFetch with the second tap clamped to the last texel (HalfLuma.h taps). Bits and the
// msb shift come in as uniforms: src_max = 2^bits - 1 (the normalised-to-code scale), shift_mul = 2^(16 - bits).
// Header-only, C++14, no Windows or GPU headers (the shader is text; the render device runs it).
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

namespace VpFrameAnalysis
{
namespace P1Gpu
{
	// The compute body (libplacebo custom-shader style: `src` sampled, `dst` storage). Defines: P1_UNORM16 selects the exact
	// integer law, otherwise the Float32 mean. The accessor header is VpUnit::HalfLuma::Glsl().
	inline const char* FillBody()
	{
		return
			"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"ivec2 size = imageSize(dst);\n"
			"if (p.x < size.x && p.y < size.y) {\n"
			"#ifdef P1_UNORM16\n"
			"    ivec2 hi = textureSize(src, 0) - ivec2(1);\n"
			"    ivec2 p0 = min(p * 2, hi);\n"
			"    ivec2 p1 = min(p * 2 + ivec2(1), hi);\n"
			"    float a = floor(texelFetch(src, ivec2(p0.x, p0.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float b = floor(texelFetch(src, ivec2(p1.x, p0.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float c = floor(texelFetch(src, ivec2(p0.x, p1.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float d = floor(texelFetch(src, ivec2(p1.x, p1.y), 0).r * src_max + 0.5) * shift_mul;\n"
			"    float k = floor((a + b + c + d + 2.0) * 0.25);\n"
			"    imageStore(dst, p, vec4(k / 65535.0, 0.0, 0.0, 1.0));\n"
			"#else\n"
			"    imageStore(dst, p, vec4(vpu_half_luma(src, p), 0.0, 0.0, 1.0));\n"
			"#endif\n"
			"}\n";
	}

	// CPU emulation of the kernel in 32-bit float arithmetic, one output sample. `s` are the four normalised source samples in
	// the tap order a, b, c, d. Returns the stored R16 unorm code (Unorm16) .
	inline uint16_t EmulateUnorm16(float s0, float s1, float s2, float s3, int bits)
	{
		const float srcMax = float((1u << bits) - 1u);
		const float shiftMul = float(1u << (16 - bits));
		const float a = std::floor(s0 * srcMax + 0.5f) * shiftMul, b = std::floor(s1 * srcMax + 0.5f) * shiftMul;
		const float c = std::floor(s2 * srcMax + 0.5f) * shiftMul, d = std::floor(s3 * srcMax + 0.5f) * shiftMul;
		const float k = std::floor((a + b + c + d + 2.0f) * 0.25f);
		// R16 unorm store: round(k / 65535 * 65535) = k exactly.
		return static_cast<uint16_t>(std::floor((k / 65535.0f) * 65535.0f + 0.5f));
	}
	// The Float32 variant: the unrounded mean, the accessor's arithmetic.
	inline float EmulateFloat32(float s0, float s1, float s2, float s3) { return VpUnit::HalfLuma::MeanF<float>(s0, s1, s2, s3); }
}
}
