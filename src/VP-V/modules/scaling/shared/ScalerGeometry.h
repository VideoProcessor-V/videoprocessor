// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: geometry of one scaler pass. The ratio of each axis is DERIVED from the source span and the
// destination size, never a stored constant (general-not-fixed rule); an SD picture with a non-square pixel
// aspect simply has two different ratios. Header-only, C++14.
#pragma once

#include <algorithm>
#include <cmath>

namespace ScalerGeometry
{
	// One axis: the source span [lo, hi) in source texels and the destination size in texels.
	struct Span
	{
		float lo = 0.0f;
		float hi = 0.0f;
		int dst = 0;
		// source texels per destination texel (< 1: upscale; > 1: downscale)
		double Ratio() const { return dst > 0 ? (static_cast<double>(hi) - lo) / dst : 1.0; }
		bool Scales() const { return std::fabs(Ratio() - 1.0) > 1e-6; }
	};

	// The whole-texel window that contains [lo, hi): what the unscaled axis of a first separable pass reads.
	struct Window
	{
		int origin = 0;
		int size = 1;
	};
	inline Window IntegerWindow(float lo, float hi)
	{
		Window w;
		w.origin = static_cast<int>(std::floor(lo));
		w.size = (std::max)(1, static_cast<int>(std::ceil(hi)) - w.origin);
		return w;
	}

	// The axes' ratios differ by more than `tolerance` (the faithful branch's own 10 % rule).
	inline bool Unequal(double rx, double ry, double tolerance = 1.1)
	{
		return (std::max)(rx, ry) > tolerance * (std::max)(1e-6, (std::min)(rx, ry));
	}
}
