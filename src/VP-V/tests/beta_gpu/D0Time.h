// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camfix R8D (test code only): the GPU-time tolerance of the d = 0 acceptance (BetaGpuTests.cpp WriteD0Summary), as a pure function so a
// unit test can pin it. The tolerance is max(2% of off, 0.1 ms, the measured A/A spread), CAPPED at max(5% of off, 0.25 ms); an A/A
// spread above the cap is a noisy run: the time grade is INCONCLUSIVE (never PASS), because the limit would be meaningless.
#pragma once

#include <algorithm>

namespace CameraAccuracy
{
	struct GpuTimeTolerance
	{
		double tolerance = 0.0;   // ms added to the off median for the limit
		bool noisy = false;       // the A/A spread exceeds the cap: the time comparison cannot be trusted
	};

	inline GpuTimeTolerance GpuTimeToleranceFor(double offGpuMs, double aaGpuMs)
	{
		const double cap = (std::max)(0.05 * offGpuMs, 0.25);
		GpuTimeTolerance t;
		t.tolerance = (std::min)((std::max)((std::max)(0.02 * offGpuMs, 0.1), aaGpuMs), cap);
		t.noisy = aaGpuMs > cap;
		return t;
	}
}
