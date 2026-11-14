// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

// ONE set of robust-statistics helpers (rule 20, ND06): median, median absolute deviation (MAD), the MAD
// -> standard-deviation factor and a quantile. About ten private Median copies and four MAD-sigma
// computations existed; they differ only in the RANK RULE, which is an axis here:
//   MedianRule  Upper         even n: the element at n / 2 of the sorted values (std::nth_element); the
//                             monitor, frame-quality and refresh-rate estimators use this one.
//               MeanOfMiddle  even n: the mean of the two middle values (the textbook median).
//   RankRule    FloorIndex    sorted[floor(q * (n - 1))] (lower rank, no interpolation)
//               NearestRank   sorted[ceil(q * n) - 1] (the nearest-rank percentile)
//               Interpolated  linear interpolation between the neighbouring ranks (type 7)
// The two median rules give different values on even n by design of their callers (a registered
// difference, not merged away: merging them would change picture-independent decisions and needs its own
// FIX with a before/after table). Every function copies its input (the caller's vector is untouched);
// an empty input returns `ifEmpty` (default 0).
namespace RobustStats
{
	// A MAD is scaled by this to estimate a standard deviation of a normal distribution.
	constexpr double kMadToSigma = 1.4826;

	enum class MedianRule { Upper, MeanOfMiddle };
	enum class RankRule { FloorIndex, NearestRank, Interpolated };

	// The k-th smallest value (0-based, k < size; larger k is clamped to the last).
	template <class T> inline T NthSmallest(std::vector<T> v, size_t k)
	{
		if (v.empty()) return T();
		if (k >= v.size()) k = v.size() - 1;
		std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
		return v[k];
	}

	template <class T> inline double Median(std::vector<T> v, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		if (v.empty()) return ifEmpty;
		const size_t n = v.size();
		if (rule == MedianRule::Upper)
		{
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2), v.end());
			return static_cast<double>(v[n / 2]);
		}
		std::sort(v.begin(), v.end());
		return (n % 2 == 1) ? static_cast<double>(v[n / 2]) : 0.5 * (static_cast<double>(v[n / 2 - 1]) + static_cast<double>(v[n / 2]));
	}

	// The median of n doubles (n <= 0: ifEmpty).
	inline double MedianOf(const double* v, int n, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		if (n <= 0) return ifEmpty;
		return Median(std::vector<double>(v, v + n), rule, ifEmpty);
	}

	// The median absolute deviation about a given centre (usually the median already computed).
	template <class T> inline double MadAbout(const std::vector<T>& v, double centre, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		std::vector<double> d;
		d.reserve(v.size());
		for (const T& x : v) d.push_back(std::fabs(static_cast<double>(x) - centre));
		return Median(std::move(d), rule, ifEmpty);
	}

	// The MAD about the values' own median.
	template <class T> inline double Mad(const std::vector<T>& v, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		if (v.empty()) return ifEmpty;
		return MadAbout(v, Median(v, rule, ifEmpty), rule, ifEmpty);
	}

	// A MAD as a standard deviation estimate.
	inline double MadSigma(double mad) { return kMadToSigma * mad; }

	// The q-quantile (q in 0..1) under a rank rule.
	template <class T> inline double Quantile(std::vector<T> v, double q, RankRule rule = RankRule::FloorIndex, double ifEmpty = 0.0)
	{
		if (v.empty()) return ifEmpty;
		const size_t n = v.size();
		if (rule == RankRule::Interpolated)
		{
			std::sort(v.begin(), v.end());
			const double pos = (q < 0.0 ? 0.0 : (q > 1.0 ? 1.0 : q)) * static_cast<double>(n - 1);
			const size_t lo = static_cast<size_t>(std::floor(pos));
			const size_t hi = lo + 1 < n ? lo + 1 : lo;
			const double f = pos - static_cast<double>(lo);
			return static_cast<double>(v[lo]) + f * (static_cast<double>(v[hi]) - static_cast<double>(v[lo]));
		}
		size_t k = 0;
		if (rule == RankRule::FloorIndex)
			k = static_cast<size_t>(q * static_cast<double>(n - 1));
		else
		{
			const size_t c = static_cast<size_t>(std::ceil(q * static_cast<double>(n)));
			k = c > 0 ? c - 1 : 0;
		}
		return static_cast<double>(NthSmallest(std::move(v), k));
	}
}
