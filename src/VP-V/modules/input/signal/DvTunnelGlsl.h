// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The one copy of the DV tunnel word / intensity / chroma GLSL (HDMI 1.4a 6.5.4).
// Used by GpuDolbyVisionTunnelUnpack (the DV kernel) and SourceUnpackGpu (the P13
// luma kernel). The texture is named `raw` and the uniform `big_endian`; both
// kernels declare them. Keep in lockstep with DolbyVisionTunnelUnpackReference.h.
namespace DvTunnelGlsl
{
	// The raw frame is uploaded as rgba8: one texel per pixel (its 32-bit
	// word's bytes in x, y, z, w; big endian for r210 / R10b, little for
	// R10l). unorm8 is exact for 0..255, so the bytes come back exactly.
	constexpr const char* kHeader = R"GLSL(
uint dv_word(int i, int y)
{
    uvec4 b = uvec4(round(texelFetch(raw, ivec2(i, y), 0) * 255.0));
    return big_endian != 0 ? (b.x << 24) | (b.y << 16) | (b.z << 8) | b.w :
        (b.w << 24) | (b.z << 16) | (b.y << 8) | b.x;
}
// HDMI 1.4a 6.5.4: G8 = I[11:4], R8 = C[11:4], B8 = C[3:0] I[3:0]; s = 2 when
// RGB8 sits in the high eight bits of the 10-bit channels, else 0, plus 2 for
// R10b / R10l (their word is the r210 word << 2). The 12-bit codes land in the
// high bits of the 16-bit sample (code << 4).
uint dv_intensity(uint p, uint s) { return ((p >> (2u + s)) & 0xFF00u) | ((p << (4u - s)) & 0xF0u); }
uint dv_chroma(uint p, uint s) { return ((p >> (12u + s)) & 0xFF00u) | ((p >> s) & 0xF0u); }
float dv_u16(uint v) { return float(v) / 65535.0; }
)GLSL";
}
