#pragma once

#include <algorithm>

// RGB capture: repair the luma, then add the luma delta to R, G and B (barrier B1, owner
// 2026-10-24).
//
// Why: native RGB ingest (r210/R12B/R12L) is one rgba16 plane plus the matcher's r16 luma plane
// (GpuRgbUnpack.cpp), and libplacebo never fires a LUMA hook for RGB, so a LUMA-hooked net would
// silently never run on RGB capture. VP runs net A on the r16 luma plane instead and applies the
// result to RGB as a delta. Adding the same d to R, G and B changes Y' = Kr R + Kg G + Kb B by
// exactly d (Kr + Kg + Kb = 1) and leaves the colour differences (R - Y', B - Y') untouched: the
// repair stays luma-only, as the nets were trained (neutral chroma).
//
// The luma the net saw and the RGB must be in the SAME transfer domain (both gamma/PQ coded, as
// captured). No clamp by default: float targets keep BTB/WTW (owner rule vp-btb-wtw-preserve);
// an UNORM target clamps on store anyway.
//
// Two forms with ONE behaviour: the CPU helper below, and the GPU pass text (kRgbLumaDeltaPass)
// the pl_dispatch runner executes; NetRunnerTests runs the pass through the CPU executor and
// checks it against the helper. Header-only, C++14.
namespace VpNets
{
	struct RgbDeltaOptions
	{
		bool clampToUnit = false;
		// The delta is computed in the net's (trained) luma range and added in the RGB's range:
		// d_rgb = d_net * deltaScale. Full-range RGB with a limited-range-trained net: 1023 / 876.
		float deltaScale = 1.0f;
	};

	// Full-range normalised luma -> the limited 10-bit range the nets were trained on (code / 1023):
	// y_lim = (y_full * 876 + 64) / 1023. No clamp: below-black / above-white stay (BTB/WTW).
	constexpr float kFullToLimitedScale = 876.0f / 1023.0f;
	constexpr float kFullToLimitedOffset = 64.0f / 1023.0f;
	constexpr float kLimitedToFullDeltaScale = 1023.0f / 876.0f;
	inline float FullToLimited(float yFull) { return yFull * kFullToLimitedScale + kFullToLimitedOffset; }

	// rgba: `channels` floats per pixel (3 or 4), rows `rgbaStride` floats apart. Luma planes: one
	// float per pixel, rows `lumaStride` floats apart. Alpha is left as is.
	inline void AddLumaDeltaToRgb(const float* lumaBefore, const float* lumaAfter, int lumaStride,
		float* rgba, int rgbaStride, int channels, int width, int height, const RgbDeltaOptions& opt = RgbDeltaOptions())
	{
		for (int y = 0; y < height; ++y)
		{
			const float* lb = lumaBefore + static_cast<size_t>(y) * lumaStride;
			const float* la = lumaAfter + static_cast<size_t>(y) * lumaStride;
			float* px = rgba + static_cast<size_t>(y) * rgbaStride;
			for (int x = 0; x < width; ++x, px += channels)
			{
				const float d = (la[x] - lb[x]) * opt.deltaScale;
				for (int c = 0; c < 3; ++c)
				{
					float v = px[c] + d;
					if (opt.clampToUnit) v = std::min(std::max(v, 0.0f), 1.0f);
					px[c] = v;
				}
			}
		}
	}

	// Y' from R'G'B' with the matrix coefficients (BT.709: kr 0.2126, kb 0.0722).
	inline float LumaOf(float r, float g, float b, float kr, float kb)
	{
		return kr * r + (1.0f - kr - kb) * g + kb * b;
	}

	// The GPU form, in the exporter's mpv pass format: hook the RGB plane, bind the luma before
	// and after the net. Same-size planes; texel-exact reads (no filtering).
	constexpr const char* kRgbLumaDeltaPass =
		"//!PARAM vp_delta_scale\n"
		"//!TYPE float\n"
		"//!MINIMUM 0.0\n"
		"//!MAXIMUM 4.0\n"
		"1.0\n"
		"\n"
		"//!DESC VP RGB luma delta (repaired luma - luma, added to R, G and B)\n"
		"//!HOOK RGB\n"
		"//!BIND HOOKED\n"
		"//!BIND LUMA_BEFORE\n"
		"//!BIND LUMA_AFTER\n"
		"//!COMPONENTS 4\n"
		"\n"
		"vec4 hook() {\n"
		"    ivec2 p = ivec2(HOOKED_pos * HOOKED_size);\n"
		"    vec4 c = texelFetch(HOOKED_raw, p, 0) * HOOKED_mul;\n"
		"    float d = (texelFetch(LUMA_AFTER_raw, p, 0).x * LUMA_AFTER_mul - texelFetch(LUMA_BEFORE_raw, p, 0).x * LUMA_BEFORE_mul) * vp_delta_scale;\n"
		"    return vec4(c.x + d, c.y + d, c.z + d, c.w);\n"
		"}\n";

	// Luma from R'G'B' in the net's trained range: y = (kr R + kg G + kb B) * vp_range_scale +
	// vp_range_offset (full -> limited: 876/1023, 64/1023; native: 1, 0). For D's RGB splice in the
	// POST_KERNEL hook (and RGB front-end paths without a matcher luma). Same transfer domain as
	// the RGB it reads (the caller's responsibility; D's light domain is barrier R8).
	constexpr const char* kLumaFromRgbPass =
		"//!PARAM vp_kr\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 1.0\n0.2126\n\n"
		"//!PARAM vp_kb\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 1.0\n0.0722\n\n"
		"//!PARAM vp_range_scale\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 2.0\n1.0\n\n"
		"//!PARAM vp_range_offset\n//!TYPE float\n//!MINIMUM -1.0\n//!MAXIMUM 1.0\n0.0\n\n"
		"//!DESC VP luma from RGB (the net's trained range)\n"
		"//!HOOK RGB\n"
		"//!BIND HOOKED\n"
		"//!COMPONENTS 1\n"
		"\n"
		"vec4 hook() {\n"
		"    ivec2 p = ivec2(HOOKED_pos * HOOKED_size);\n"
		"    vec4 c = texelFetch(HOOKED_raw, p, 0) * HOOKED_mul;\n"
		"    float y = vp_kr * c.x + (1.0 - vp_kr - vp_kb) * c.y + vp_kb * c.z;\n"
		"    return vec4(y * vp_range_scale + vp_range_offset, 0.0, 0.0, 1.0);\n"
		"}\n";
}
