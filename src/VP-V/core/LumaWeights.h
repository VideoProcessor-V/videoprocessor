// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <libplacebo/renderer.h>

// ONE source of luma (Y') weights (rule 20, ND02). The BT.709 literals 0.2126 / 0.7152 / 0.0722 were repeated in
// the source-resolution monitor's shader, the net runner's options, the RGB-luma pass and DetailPulse; the
// YCbCr-system table (DetailPulse::LumaWeights, hunt 3 F8) is moved here unchanged and DetailPulse forwards to it.
//
//   ForSystem   Kr, Kg, Kb of a YCbCr colour system (the luma of an RGB picture); an unknown tag follows the
//               primaries (BT.2020 primaries -> BT.2020 weights), else BT.709.
//   kBt709*     the BT.709 constants; the *Text macros are the same numbers as string literals so a GLSL / mpv
//               pass built from adjacent literals takes them from here (a test checks text == constant).
//
//   LumaOf      Y' = kr R' + (1 - kr - kb) G' + kb B' in float (moved verbatim from VpNets RgbLumaDelta); its GLSL twin
//               is VP_LUMA_DOT_PARAM_GLSL_TEXT over the mpv parameters vp_kr / vp_kb (a test checks the text).
//
// Kept apart (display / grading primaries, not a YCbCr tag): DvHostStage's primaries-derived luminance (its
// BT.709 fallback reads the kBt709* constants here; the primaries path stays its own), DolbyVisionTrims.h:204,
// calibration patterns. AdaptiveDownscale's GLSL form is owned by L-B5 and calls ForSystem through DetailPulse::LumaWeights.
#define VP_LUMA_BT709_KR_TEXT "0.2126"
#define VP_LUMA_BT709_KG_TEXT "0.7152"
#define VP_LUMA_BT709_KB_TEXT "0.0722"
#define VP_LUMA_BT709_VEC3_TEXT "vec3(" VP_LUMA_BT709_KR_TEXT ", " VP_LUMA_BT709_KG_TEXT ", " VP_LUMA_BT709_KB_TEXT ")"
// GLSL twin of LumaCoeff::LumaOf over a vec4 c and the mpv parameters vp_kr / vp_kb.
#define VP_LUMA_DOT_PARAM_GLSL_TEXT "vp_kr * c.x + (1.0 - vp_kr - vp_kb) * c.y + vp_kb * c.z"

namespace LumaCoeff
{
	constexpr double kBt709Kr = 0.2126;
	constexpr double kBt709Kg = 0.7152;
	constexpr double kBt709Kb = 0.0722;

	inline void ForSystem(enum pl_color_system system, enum pl_color_primaries primaries, double out[3])
	{
		double kr = kBt709Kr, kb = kBt709Kb;
		switch (system)
		{
		case PL_COLOR_SYSTEM_BT_601: kr = 0.299; kb = 0.114; break;
		case PL_COLOR_SYSTEM_BT_2020_NC:
		case PL_COLOR_SYSTEM_BT_2020_C: kr = 0.2627; kb = 0.0593; break;
		case PL_COLOR_SYSTEM_SMPTE_240M: kr = 0.212; kb = 0.087; break;
		case PL_COLOR_SYSTEM_BT_709: break;
		default:
			if (primaries == PL_COLOR_PRIM_BT_2020) { kr = 0.2627; kb = 0.0593; }
			break;
		}
		out[0] = kr;
		out[1] = 1.0 - kr - kb;
		out[2] = kb;
	}

	// Y' from R'G'B' with the matrix coefficients (BT.709: kr 0.2126, kb 0.0722).
	inline float LumaOf(float r, float g, float b, float kr, float kb)
	{
		return kr * r + (1.0f - kr - kb) * g + kb * b;
	}
}
