// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// WAIVER-S21-O3 test reference: a packed DV tunnel source (Kind::PackedDvR210) is measured by VpFrameAnalysis::Reduce on its 12-bit
// tap, not on the ten-bit Sample().luma. This is the independent reference for that read, written from the in-force law and not from the
// Reducer: DvTunnelTapLaw code12 = Intensity >> 4 (SourceTap12), tap16 = tap12 << 4, and "a 12-bit sample is Q6 on the 10-bit axis"
// (q6 = code12 << 4; one histogram bin per whole axis code = q6 >> 6 = code12 >> 2, clamped to 1023). Mean, min and max are exact in Q6;
// a percentile is the first bin whose cumulative count reaches max(1, q * samples). Tests only. CPU only.

#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace DvWideReference
{
	// The studio map is the identity (the tap is already limited, or the frame is limited): no extra map.
	inline bool MapIsIdentity(const std::array<uint16_t, 1024>& studio)
	{
		for (size_t code = 0; code < studio.size(); ++code)
			if (studio[code] != static_cast<uint16_t>(code))
				return false;
		return true;
	}

	inline uint16_t FirstBinReaching(const std::array<uint32_t, FrameAnalysis::kHistogramBins>& histogram, double target)
	{
		uint64_t cumulative = 0;
		for (int bin = 0; bin < FrameAnalysis::kHistogramBins; ++bin)
		{
			cumulative += histogram[static_cast<size_t>(bin)];
			if (static_cast<double>(cumulative) >= target)
				return static_cast<uint16_t>(bin);
		}
		return static_cast<uint16_t>(FrameAnalysis::kHistogramBins - 1);
	}

	// `mapFull`: the frame's studio map is not the identity (the Original pathway's full -> limited map): applied to the tap at 12-bit
	// precision (DvTunnelTapLaw::FullLumaToLimited12), as the law's range table. False when a pixel of `picture` cannot be tapped.
	inline bool Reduce12(const AnalysisLumaSource& source, const FrameAnalysis::Rect& picture, bool mapFull, FrameAnalysis::LumaStats& luma,
		FrameAnalysis::RangeOccupancy& range)
	{
		luma = FrameAnalysis::LumaStats{};
		range = FrameAnalysis::RangeOccupancy{};
		uint64_t sumQ6 = 0, samples = 0;
		uint32_t minQ6 = 0xffffffffu, maxQ6 = 0;
		for (int y = picture.top; y < picture.bottom; ++y)
			for (int x = picture.left; x < picture.right; ++x)
			{
				uint16_t tap12 = 0;
				// One assert per pixel (as the ten-bit reference asserted source.Sample() per pixel): the first unreadable pixel is named.
				Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(DvTunnelTapLaw::SourceTap12(source, x, y, tap12),
					(L"packed DV 12-bit tap unreadable at " + std::to_wstring(x) + L"," + std::to_wstring(y)).c_str());
				const uint32_t code12 = mapFull ? DvTunnelTapLaw::FullLumaToLimited12(tap12) : tap12;
				const uint32_t q6 = code12 << 4;
				const uint32_t bin = (std::min)(q6 >> 6, static_cast<uint32_t>(FrameAnalysis::kHistogramBins - 1));
				++luma.histogram[bin];
				sumQ6 += q6;
				minQ6 = (std::min)(minQ6, q6);
				maxQ6 = (std::max)(maxQ6, q6);
				++samples;
			}
		if (samples == 0)
			return false;
		luma.samples = samples;
		luma.mean = static_cast<double>(sumQ6) / 64.0 / static_cast<double>(samples);
		luma.apl = (luma.mean - 64.0) / (940.0 - 64.0);
		luma.meanQ6 = luma.mean * 64.0;
		luma.minQ6 = minQ6;
		luma.maxQ6 = maxQ6;
		int first = 0, last = FrameAnalysis::kHistogramBins - 1;
		while (first < last && !luma.histogram[static_cast<size_t>(first)]) ++first;
		while (last > first && !luma.histogram[static_cast<size_t>(last)]) --last;
		luma.minimum = static_cast<uint16_t>(first);
		luma.maximum = static_cast<uint16_t>(last);
		const double n = static_cast<double>(samples);
		luma.p1 = FirstBinReaching(luma.histogram, (std::max)(1.0, 0.01 * n));
		luma.p50 = FirstBinReaching(luma.histogram, (std::max)(1.0, 0.50 * n));
		luma.p99 = FirstBinReaching(luma.histogram, (std::max)(1.0, 0.99 * n));
		luma.p1Q6 = (static_cast<uint32_t>(luma.p1) << 6) | 32u;
		luma.p50Q6 = (static_cast<uint32_t>(luma.p50) << 6) | 32u;
		luma.p99Q6 = (static_cast<uint32_t>(luma.p99) << 6) | 32u;
		uint64_t btb = 0, wtw = 0, nearBlack = 0, clipped = 0;
		for (int bin = 0; bin < FrameAnalysis::kHistogramBins; ++bin)
		{
			const uint64_t count = luma.histogram[static_cast<size_t>(bin)];
			if (bin < 64) btb += count;
			if (bin > 940) wtw += count;
			if (bin <= 81) nearBlack += count;
			if (bin >= 1019) clipped += count;
		}
		range.btb = btb / n;
		range.wtw = wtw / n;
		range.nearBlack = nearBlack / n;
		range.clipped = clipped / n;
		return true;
	}
}
