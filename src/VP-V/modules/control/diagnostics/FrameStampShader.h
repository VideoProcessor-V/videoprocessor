// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// vp-stamp/1, GPU side: the libplacebo (mpv-style) user shader that draws the OUTPUT stamp, plus
// the CPU twin of its per-pixel formula so a test can prove the shader geometry equals
// FrameStamp.h's Write(). Debug only: the hook exists only when the environment variable
// VP_DEBUG_OUTPUT_STAMP equals 1 (no config key; default off). Off = no hook text at all (empty
// string), so the normal chain is byte-identical.
//
// The hook is OutputHookOrder::Kind::DebugStamp (rank 500, after the quantiser): it overwrites the
// block's pixels with 0.0 / 1.0 in the OUTPUT encoding, after sigma-delta, so the stamp survives
// untouched. The CPU computes the 64-bit word (VpFrameStamp::Word(kOutput, source seq & 63, index));
// hi = top 32 bits, lo = low 32 bits, handed over as the DYNAMIC uint params stamp_hi / stamp_lo.
// GPU proof still needed: levels (limited vs full) of the 0.0 / 1.0 values at the real output.

#include "FrameStamp.h"

#include <cstdlib>
#include <string>

namespace VpFrameStamp
{
	// True when VP_DEBUG_OUTPUT_STAMP is exactly "1".
	inline bool OutputStampEnabled()
	{
#ifdef _MSC_VER
		char* value = nullptr;
		size_t length = 0;
		bool on = false;
		if (_dupenv_s(&value, &length, "VP_DEBUG_OUTPUT_STAMP") == 0 && value)
		{
			on = std::string(value) == "1";
			std::free(value);
		}
		return on;
#else
		const char* value = std::getenv("VP_DEBUG_OUTPUT_STAMP");
		return value && std::string(value) == "1";
#endif
	}

	// The shader's per-pixel formula in double: 0.0 / 1.0 for a pixel inside the OUTPUT stamp block of a
	// W x H frame, -1.0 when the pixel is not in the block (the shader returns the picture unchanged).
	// The cell is found by floor(offset / u) and corrected against the cell edges, so it agrees with
	// Write()'s exact "pixel centre inside the cell rectangle" rule.
	inline double CpuReference(int x, int y, int w, int h, Corner corner, uint64_t word)
	{
		const double u = UnitOf(h);
		double bx, by;
		BlockOrigin(w, h, corner, bx, by);
		const double cxm = x + 0.5, cym = y + 0.5;
		if (cxm < bx || cym < by || cxm >= bx + kBlockW * u || cym >= by + kBlockH * u)
			return -1.0;
		int kx = static_cast<int>(std::floor((cxm - bx) / u));
		if (cxm < bx + kx * u) --kx;
		else if (cxm >= bx + (kx + 1) * u) ++kx;
		int ky = static_cast<int>(std::floor((cym - by) / u));
		if (cym < by + ky * u) --ky;
		else if (cym >= by + (ky + 1) * u) ++ky;
		const int col = kx - 1, row = ky - 1;
		if (col < 0 || col >= kCols || row < 0 || row >= kRows)
			return 0.0;
		return static_cast<double>(BitAt(word, col, row));
	}

	// The OUTPUT-stage user shader text (libplacebo mpv syntax, as shaders/NLS.glsl).
	inline std::string BuildShaderText()
	{
		return std::string(R"GLSL(//!PARAM stamp_hi
//!DESC VideoProcessor debug output stamp: top 32 bits of the vp-stamp/1 word
//!TYPE DYNAMIC uint
0

//!PARAM stamp_lo
//!DESC VideoProcessor debug output stamp: low 32 bits of the vp-stamp/1 word
//!TYPE DYNAMIC uint
0

//!HOOK OUTPUT
//!BIND HOOKED
//!DESC VideoProcessor debug output stamp (vp-stamp/1)

// Same geometry as FrameStamp.h: unit u = H / 270, 18 x 7 block (1u dark border, reference row,
// 4 data rows of 16 cells), anchored top-right at (W - 2u - 18u, 2u). Debug only.
vec4 hook()
{
    vec4 color = HOOKED_tex(HOOKED_pos);
    vec2 centre = HOOKED_pos * HOOKED_size;   // x + 0.5, y + 0.5
    float u = HOOKED_size.y / 270.0;
    float bx = HOOKED_size.x - 2.0 * u - 18.0 * u;
    float by = 2.0 * u;
    if (centre.x < bx || centre.y < by || centre.x >= bx + 18.0 * u || centre.y >= by + 7.0 * u)
        return color;
    int col = int(floor((centre.x - bx) / u)) - 1;
    int row = int(floor((centre.y - by) / u)) - 1;
    float v = 0.0;
    if (col >= 0 && col < 16 && row >= 0 && row < 5) {
        if (row == 0) {
            v = (col % 2 == 0) ? 1.0 : 0.0;
        } else {
            int idx = (row - 1) * 16 + col;
            uint bits = idx < 32 ? stamp_hi : stamp_lo;
            int shift = 31 - (idx % 32);
            v = float((bits >> uint(shift)) & 1u);
        }
    }
    return vec4(v, v, v, color.a);
}
)GLSL");
	}

	// The hook text when `enabled`, otherwise the EMPTY string (off = no hook, byte-identical).
	inline std::string HookTextIfEnabled(bool enabled)
	{
		return enabled ? BuildShaderText() : std::string();
	}
}
