// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Light-level LUT, shader side (`input.light_level`, DRAFT). Header-only, no Windows or libplacebo headers. The GLSL is
// meant for a libplacebo custom shader HEADER; the host declares:
//
// light_level_lut : sampler2D (rgba32f, 1024 x 1, nearest, no mipmaps), the PackTexels() layout of LightLevelLut.h:
//                   (J, S, V, L / 10000) per integer 10-bit code.
// Uniforms: none (the context lives in the LUT; the host rebuilds it when LightLevel::Fingerprint changes).
//
// light_level_lut_fetch(code10) mirrors LightLevel::Fetch: index clamped to [0, 1023], linear interpolation between
// integer codes written as a + (b - a) * t (same arithmetic as the CPU). Pixel data are never clamped; only the index.
#pragma once

namespace LightLevel
{
	inline const char* LightLevelGlsl()
	{
		return R"GLSL(
vec4 light_level_lut_fetch(float code10)
{
    float idx = clamp(code10, 0.0, 1023.0);
    float f = floor(idx);
    int i0 = int(f);
    int i1 = min(i0 + 1, 1023);
    float t = idx - f;
    vec4 a = texelFetch(light_level_lut, ivec2(i0, 0), 0);
    vec4 b = texelFetch(light_level_lut, ivec2(i1, 0), 0);
    return a + (b - a) * t;
}

float light_level_j(float code10)
{
    return light_level_lut_fetch(code10).x;
}
)GLSL";
	}
}
