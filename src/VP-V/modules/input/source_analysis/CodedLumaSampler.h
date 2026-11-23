// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// W21-2 (slice 21, 12-bit analysis widening): a luma sampler that declares its own code range. The source-resolution look-ahead
// (SourceResolutionMonitor::Monitor::LookAheadFrame) normalises a sampled code by the range it is told, so a 12-bit DV tap (4095)
// is read at its own resolution instead of being forced onto 1023. The look-ahead's axis-level thresholds keep their meaning (they
// are fractions of the range). Callable like a plain sampler (`sampler(x, y)`), so a caller that only reads samples keeps its call
// shape; a distinct type so the plain-sampler overload (1023) is unchanged.
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>

#include <cstdint>
#include <functional>

namespace SourceResolutionMonitor
{
	// A sampled code on 0..1 of the sampler's declared range (1023: 10-bit, exactly code / 1023.0 as before; 4095: a 12-bit tap).
	inline double NormalisedLuma(int code, uint32_t codeMaximum)
	{
		return static_cast<double>(code) / static_cast<double>(codeMaximum == 0 ? 1u : codeMaximum);
	}

	struct CodedLumaSampler
	{
		std::function<int(int x, int y)> read;
		uint32_t codeMaximum = Vpv::AnalysisDomain::kAxisMax;   // the largest code read() returns (default: the 10-bit axis)
		int operator()(int x, int y) const { return read(x, y); }
	};
}
