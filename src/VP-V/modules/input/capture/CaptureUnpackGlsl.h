// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The one copy of the capture unpack GLSL helpers shared by GpuCaptureUnpack (V210 / UYVY to
// P210 luma + chroma) and SourceUnpackGpu (the P13 luma plane). The texture is named `raw`.
// Lead ruling (CG-1): the V210 / UYVY field extraction of the two kernels is the same law
// (see README "V210 / UYVY twin ruling"); the helpers are one text, the invocation shape differs.
namespace CaptureUnpackGlsl
{
	// The raw frame is uploaded as rgba8: one texel per 4 source bytes
	// (V210: one 32-bit little-endian word; UYVY: U, Y0, V, Y1). unorm8 is
	// exact for 0..255, so the bytes come back exactly.
	constexpr const char* kHeader = R"GLSL(
uvec4 cu_bytes(int i, int y) { return uvec4(round(texelFetch(raw, ivec2(i, y), 0) * 255.0)); }
uint cu_word(int i, int y) { uvec4 b = cu_bytes(i, y); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }
// A 16-bit sample (the value already in the high bits) for a unorm16 store.
float cu_u16(uint v) { return float(v) / 65535.0; }
)GLSL";
}
