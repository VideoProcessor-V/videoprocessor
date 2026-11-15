// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The GPU twins of RobustStats.h (C6-2 U24, owner core/RobustStats): ONE text per shader-side rule, concatenated
// into a shader's header by its host. No other shader may carry its own copy.
//   kMedianUpper9Glsl  GLSL `float vpu_median_upper(float a[9], int n)`: the MedianRule::Upper median of the first
//                      n (1..9) values (insertion sort, a[n / 2]). The body is the one of the matcher's former
//                      `median_` copies (DejudderWarp.cpp, two identical copies), byte for byte; only the name
//                      carries the owner prefix. The array is an in-parameter (a copy), so the caller's values
//                      are untouched. n < 1 is the caller's guard (as before); no NaN handling (see RobustStats.h).
//   kMadToSigmaHlsl    HLSL declaration of VP_MAD_TO_SIGMA: the text form of RobustStats::kMadToSigma (a double
//                      literal, the same digits as the PhotometricRefine GPU shader's former inline constant).
// The unit test compares the GLSL text with the pre-change matcher text and VP_MAD_TO_SIGMA with kMadToSigma.
namespace RobustStats
{
	static constexpr const char kMedianUpper9Glsl[] =
		"\n"
		"float vpu_median_upper(float a[9], int n)\n"
		"{\n"
		"    for (int i = 1; i < 9; i++) {\n"
		"        if (i >= n) break;\n"
		"        float x = a[i];\n"
		"        int j = i - 1;\n"
		"        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }\n"
		"        a[j + 1] = x;\n"
		"    }\n"
		"    return a[n / 2];\n"
		"}\n";

	static constexpr const char kMadToSigmaHlsl[] = "static const double VP_MAD_TO_SIGMA = 1.4826L;\n";
}
