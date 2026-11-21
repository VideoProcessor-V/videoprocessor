// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The CPU model of the DitherNeedWarp GPU tests, shared by the native run (SigmaDeltaHookTests.cpp) and the Beta-host port
// (BetaGpuTests.cpp, RunBetaDitherNeedWarpForkTests): one model, no private copy. Pure C++; depends only on SigmaDeltaDomain.h.
#pragma once

#include <VP-V/modules/output/dither/SigmaDeltaDomain.h>

#include <cstddef>
#include <vector>

namespace DitherNeedWarpModel
{
	// Greys that are exact in the rgba16f OUTPUT texture (m / 2048, m in [1024, 2047]): the quantiser's input is then
	// x = grey * 1023 exactly (full range, true 10-bit), with a fraction well away from a whole code.
	inline std::vector<float> Greys() { return { 1100.0f / 2048.0f, 1357.0f / 2048.0f, 1601.0f / 2048.0f, 1999.0f / 2048.0f }; }

	// The CPU model of `frames` refreshes of the quantiser at one grey: SigmaDeltaDomain::QuantiseStep with the shader's
	// dither (frame / 5 = firstFrame + j), amplitude `amp` (float, as the shader's), x moved by dx. True when every code matches.
	inline bool ModelMatches(const int* got, int frames, float grey, int firstFrame, float amp, double dx)
	{
		const auto limits = SigmaDeltaDomain::Limits(10, true);
		const double x = static_cast<double>(grey * 1023.0f) + dx;
		double r = 0.0;
		for (int j = 0; j < frames; ++j)
		{
			const double d = static_cast<double>((SigmaDeltaDomain::Ign(0.0f, 0.0f, firstFrame + j) - 0.5f) * 2.0f * amp);
			if (static_cast<int>(SigmaDeltaDomain::QuantiseStep(x, r, d, limits, amp)) != got[j]) return false;
		}
		return true;
	}

	inline bool ModelMatchesAnyDx(const int* got, int frames, float grey, int firstFrame, float amp)
	{
		for (const double dx : { 0.0, 1e-4, -1e-4, 3e-4, -3e-4 })
			if (ModelMatches(got, frames, grey, firstFrame, amp, dx)) return true;
		return false;
	}

	inline std::vector<float> ConstantMap(int w, int h, float e)
	{
		std::vector<float> m(static_cast<size_t>(w) * h * 4, 0.0f);
		for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) m[i * 4] = e;
		return m;
	}
}
