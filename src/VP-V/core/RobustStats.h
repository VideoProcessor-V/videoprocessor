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
// FIX with a before/after table). Every function copies its input (the caller's vector is untouched), except
// MedianInPlace, MadInPlace, and QuantileInPlace, which reorder/overwrite their argument in place;
// an empty input returns `ifEmpty` (default 0); `NthSmallest` of an empty input returns T(). Callers whose
// input can never be empty (the private copies were undefined there) assert(!empty) at the call, with no
// fallback (C6-2 U24 plan (d)).
// NaN policy: NONE here. The helpers neither filter nor detect NaN / inf: with a NaN in the input the
// ordering is unspecified and so is the result. Callers filter non-finite values before the call, exactly
// as they did before the consolidation (a NaN must never reach pixels; masking it here would be a silent
// fallback). The GPU twins of the Upper median and of kMadToSigma are text in RobustStatsShaders.h.
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

	// The median of v IN PLACE: v is reordered (Upper: nth_element, MeanOfMiddle: sorted), nothing is allocated. For a
	// caller's persistent scratch buffer that is refilled every pass (PhotometricRefine |r|), where a copy or a move
	// would cost an allocation per pass. Same values as Median().
	template <class T> inline double MedianInPlace(std::vector<T>& v, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
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

	template <class T> inline double Median(std::vector<T> v, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		return MedianInPlace(v, rule, ifEmpty);
	}

	// The median of n doubles IN PLACE (reorders `data`, O(n), zero heap allocations). Same values as MedianOf().
	inline double MedianInPlace(double* data, int n, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		if (!data || n <= 0) return ifEmpty;
		const size_t cnt = static_cast<size_t>(n);
		const size_t mid = cnt / 2;
		std::nth_element(data, data + static_cast<std::ptrdiff_t>(mid), data + static_cast<std::ptrdiff_t>(cnt));
		const double hi = data[mid];
		if (rule == MedianRule::Upper || (cnt & 1u) != 0u)
			return hi;
		const double lo = *std::max_element(data, data + static_cast<std::ptrdiff_t>(mid));
		return 0.5 * (lo + hi);
	}

	// The MAD of n doubles IN PLACE (overwrites `data`, O(n), zero heap allocations). Same values as Mad().
	inline double MadInPlace(double* data, int n, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		if (!data || n <= 0) return ifEmpty;
		const double med = MedianInPlace(data, n, rule, ifEmpty);
		for (int i = 0; i < n; ++i)
			data[i] = std::fabs(data[i] - med);
		return MedianInPlace(data, n, rule, ifEmpty);
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

	// The MAD of n doubles about a given centre (n <= 0: ifEmpty): median of |v[i] - centre| under the rule.
	inline double MadAboutOf(const double* v, int n, double centre, MedianRule rule = MedianRule::MeanOfMiddle, double ifEmpty = 0.0)
	{
		if (n <= 0) return ifEmpty;
		std::vector<double> d(static_cast<size_t>(n));
		for (int i = 0; i < n; ++i) d[static_cast<size_t>(i)] = std::fabs(v[i] - centre);
		return Median(std::move(d), rule, ifEmpty);
	}

	// Mean after dropping floor(trim * n) values from each end of the sorted sample (ascending summation order).
	// Moved verbatim from identity/chain/ChainCommon.h (C6-2 U24); n <= 0 -> 0; n - 2k <= 0 -> no trimming.
	inline double TrimmedMeanOf(const double* v, int n, double trim)
	{
		if (n <= 0)
			return 0.0;
		std::vector<double> t(v, v + n);
		std::sort(t.begin(), t.end());
		int k = static_cast<int>(std::floor(trim * static_cast<double>(n)));
		if (k < 0)
			k = 0;
		if (n - 2 * k <= 0)
			k = 0;
		double s = 0.0;
		for (int i = k; i < n - k; ++i)
			s += t[static_cast<size_t>(i)];
		return s / static_cast<double>(n - 2 * k);
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

	// The q-quantile of n doubles IN PLACE (reorders `data`, O(n), zero heap allocations). Same values as Quantile().
	inline double QuantileInPlace(double* data, int n, double q, RankRule rule = RankRule::Interpolated, double ifEmpty = 0.0)
	{
		if (!data || n <= 0 || !std::isfinite(q)) return ifEmpty;
		const size_t cnt = static_cast<size_t>(n);
		const double cq = q < 0.0 ? 0.0 : (q > 1.0 ? 1.0 : q);
		if (rule == RankRule::Interpolated)
		{
			const double pos = cq * static_cast<double>(cnt - 1);
			const size_t lo = static_cast<size_t>(std::floor(pos));
			const size_t hi = lo + 1 < cnt ? lo + 1 : lo;
			const double f = pos - static_cast<double>(lo);
			std::nth_element(data, data + static_cast<std::ptrdiff_t>(lo), data + static_cast<std::ptrdiff_t>(cnt));
			const double vLo = data[lo];
			const double vHi = (hi > lo)
				? *std::min_element(data + static_cast<std::ptrdiff_t>(lo + 1), data + static_cast<std::ptrdiff_t>(cnt))
				: vLo;
			return vLo + f * (vHi - vLo);
		}
		size_t k = 0;
		if (rule == RankRule::FloorIndex)
			k = static_cast<size_t>(cq * static_cast<double>(cnt - 1));
		else
		{
			const size_t c = static_cast<size_t>(std::ceil(cq * static_cast<double>(cnt)));
			k = c > 0 ? c - 1 : 0;
		}
		if (k >= cnt) k = cnt - 1;
		std::nth_element(data, data + static_cast<std::ptrdiff_t>(k), data + static_cast<std::ptrdiff_t>(cnt));
		return data[k];
	}
}
