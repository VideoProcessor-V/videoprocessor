// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: the CPU reference sampler of ScalerPass (the specification the GPU pass is compared with).
// Exact taps in double, clamp-to-edge, weights normalised, support stretched by the ratio when downscaling.
// Output texel i sits at source position lo + (i + 0.5) * ratio; source texel k is centred at k + 0.5.
// Anti-ringing is NOT modelled (it is libplacebo's; compare with antiring 0). The polar (EWA) filter has no
// reference here. Header-only, C++14.
#pragma once

#include "KernelTable.h"
#include "ScalerGeometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ScalerReference
{
	struct Image
	{
		int w = 0;
		int h = 0;
		std::vector<double> v;
		Image() = default;
		Image(int width, int height, double fill = 0.0) : w(width), h(height), v(static_cast<size_t>(width) * height, fill) {}
		double& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
	};

	// The taps of output texel i on one axis: the first source index (before clamping) and the weights.
	inline void Taps(const ScalerKernels::Spec& spec, double lo, double ratio, int i, std::vector<double>& weights, int& first)
	{
		const double s = (std::max)(1.0, ratio);
		const double pos = lo + (i + 0.5) * ratio;
		const double radius = ScalerKernels::Radius(spec) * s;
		first = static_cast<int>(std::ceil(pos - radius - 0.5));
		const int last = static_cast<int>(std::floor(pos + radius - 0.5));
		weights.assign(static_cast<size_t>((std::max)(0, last - first + 1)), 0.0);
		double sum = 0.0;
		for (size_t t = 0; t < weights.size(); ++t)
		{
			weights[t] = ScalerKernels::Weight(spec, (first + static_cast<int>(t) + 0.5 - pos) / s);
			sum += weights[t];
		}
		if (sum != 0.0) for (double& w : weights) w /= sum;
	}

	// One axis (X: along rows; Y: along columns) over the span [lo, hi), `dst` output texels. `phase` is the
	// chroma siting offset in source texels (added to the span).
	inline Image ResampleAxis(const Image& in, bool alongX, double lo, double hi, int dst, const ScalerKernels::Spec& spec, double phase = 0.0)
	{
		const int n = alongX ? in.w : in.h;
		const double ratio = (hi - lo) / dst;
		Image out = alongX ? Image(dst, in.h) : Image(in.w, dst);
		std::vector<double> w;
		int first = 0;
		for (int i = 0; i < dst; ++i)
		{
			Taps(spec, lo + phase, ratio, i, w, first);
			const int lines = alongX ? in.h : in.w;
			for (int l = 0; l < lines; ++l)
			{
				double acc = 0.0;
				for (size_t t = 0; t < w.size(); ++t)
				{
					const int k = (std::min)(n - 1, (std::max)(0, first + static_cast<int>(t)));
					acc += w[t] * (alongX ? in.at(k, l) : in.at(l, k));
				}
				if (alongX) out.at(i, l) = acc; else out.at(l, i) = acc;
			}
		}
		return out;
	}

	// Both axes, separable, over the whole input (each axis' span is [0, size)).
	inline Image Resample(const Image& in, int dstW, int dstH, const ScalerKernels::Spec& spec, bool downFirst = false,
		double phaseX = 0.0, double phaseY = 0.0)
	{
		if (downFirst)
			return ResampleAxis(ResampleAxis(in, false, 0.0, in.h, dstH, spec, phaseY), true, 0.0, in.w, dstW, spec, phaseX);
		return ResampleAxis(ResampleAxis(in, true, 0.0, in.w, dstW, spec, phaseX), false, 0.0, in.h, dstH, spec, phaseY);
	}
}
