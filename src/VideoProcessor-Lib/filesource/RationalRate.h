#pragma once

// VP Renderer (file sources): exact rational frame rates from DirectShow
// timing. Pure, header-only, graph-free; C++14.
//
// No table of fixed rates: AvgTimePerFrame (100 ns units) is the rounded (or
// truncated) period of an exact rational rate. Every rational whose exact
// period lies within one 100 ns unit of the given value is a
// candidate (inclusive for the integer and 1001 families, open for the
// general search). Preference order:
//   1. an integer rate (denominator 1),
//   2. the NTSC-style family k*1000/1001,
//   3. otherwise the simplest rational (smallest denominator, found with the
//      Stern-Brocot / continued-fraction walk) inside the tolerance interval.
// The same snapping is applied to measured median PTS deltas.

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

namespace filesource
{
	// 100 ns units per second (REFERENCE_TIME scale).
	static const int64_t kUnitsPerSecond = 10000000LL;

	struct RationalRate
	{
		uint32_t numerator = 0;
		uint32_t denominator = 0;

		bool IsValid() const { return numerator != 0 && denominator != 0; }

		double Hz() const
		{
			return IsValid() ? (double)numerator / (double)denominator : 0.0;
		}

		// Exact period in 100 ns units (fractional). 0 when invalid.
		double Period100ns() const
		{
			return IsValid() ? (double)kUnitsPerSecond * (double)denominator / (double)numerator : 0.0;
		}

		bool operator==(const RationalRate& o) const
		{
			return numerator == o.numerator && denominator == o.denominator;
		}
		bool operator!=(const RationalRate& o) const { return !(*this == o); }
	};

	namespace detail
	{
		inline int64_t Gcd(int64_t a, int64_t b)
		{
			if (a < 0) a = -a;
			if (b < 0) b = -b;
			while (b != 0)
			{
				const int64_t t = a % b;
				a = b;
				b = t;
			}
			return a;
		}

		inline RationalRate MakeReduced(int64_t num, int64_t den)
		{
			RationalRate r;
			if (num <= 0 || den <= 0)
				return r;
			const int64_t g = Gcd(num, den);
			num /= g;
			den /= g;
			if (num > 0xFFFFFFFFLL || den > 0xFFFFFFFFLL)
				return r;
			r.numerator = (uint32_t)num;
			r.denominator = (uint32_t)den;
			return r;
		}

		// |exact period of num/den - period100ns| in 100 ns units.
		inline double PeriodError(int64_t num, int64_t den, int64_t period100ns)
		{
			const double exact = (double)kUnitsPerSecond * (double)den / (double)num;
			return std::fabs(exact - (double)period100ns);
		}

		// Simplest rational p/q (smallest q, then smallest p) strictly inside the
		// open interval (an/ad, bn/bd), 0 <= an/ad < bn/bd. Continued-fraction
		// walk on exact integers; all inputs are positive and small (<= ~1e7
		// times a few), so int64 cannot overflow over the bounded iterations.
		inline bool SimplestBetween(int64_t an, int64_t ad, int64_t bn, int64_t bd,
			int64_t& outNum, int64_t& outDen)
		{
			// Convergent matrix: value = (h1 * x + h0) / (k1 * x + k0), x = remainder.
			int64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
			for (int iteration = 0; iteration < 64; ++iteration)
			{
				if (ad <= 0 || bd <= 0)
					return false;
				const int64_t fl = an / ad; // floor (an >= 0)
				// Smallest integer strictly greater than a is fl + 1.
				const int64_t candidate = fl + 1;
				// candidate < b  <=>  candidate * bd < bn
				if (candidate * bd < bn)
				{
					// Use the smallest integer strictly inside; if a itself is not an
					// integer, fl < a, so fl + 1 is the smallest > a.
					const int64_t p = h1 * candidate + h0;
					const int64_t q = k1 * candidate + k0;
					outNum = p;
					outDen = q;
					return true;
				}
				// Both endpoints share the integer part fl (b <= fl + 1).
				// Extend the convergent with fl.
				const int64_t nh = h1 * fl + h0;
				const int64_t nk = k1 * fl + k0;
				h0 = h1; h1 = nh;
				k0 = k1; k1 = nk;
				// Fractional parts: a' = (an - fl*ad)/ad, b' = (bn - fl*bd)/bd.
				const int64_t ar = an - fl * ad;
				const int64_t br = bn - fl * bd;
				if (br <= 0)
					return false; // b == fl exactly would mean an empty interval
				// Next interval: (1/b', 1/a') = (bd/br, ad/ar); a' == 0 -> infinity.
				if (ar == 0)
				{
					// Interval (bd/br, inf): simplest is floor(bd/br) + 1.
					const int64_t x = bd / br + 1;
					outNum = h1 * x + h0;
					outDen = k1 * x + k0;
					return true;
				}
				const int64_t nan_ = bd, nad = br, nbn = ad, nbd = ar;
				an = nan_; ad = nad; bn = nbn; bd = nbd;
			}
			return false;
		}
	}

	// Snap an AvgTimePerFrame / measured period (100 ns units) to an exact
	// rational rate. Returns an invalid rate for period <= 0.
	inline RationalRate SnapPeriodToRational(int64_t period100ns)
	{
		RationalRate none;
		if (period100ns <= 1)
			return none;

		// Integer and 1001 family: within one 100 ns unit inclusive (covers
		// rounding, truncation and a ceiling of the exact period).
		const double tolerance = 1.0;
		const double rateHz = (double)kUnitsPerSecond / (double)period100ns;

		// 1. Integer rate.
		bool haveBest = false;
		int64_t bestNum = 0, bestDen = 0;
		double bestErr = 0.0;
		{
			const int64_t n = (int64_t)std::llround(rateHz);
			if (n > 0)
			{
				const double err = detail::PeriodError(n, 1, period100ns);
				if (err <= tolerance)
				{
					haveBest = true; bestNum = n; bestDen = 1; bestErr = err;
				}
			}
		}
		// 2. k*1000/1001 family (the integer rate slowed by 1000/1001).
		{
			const int64_t k = (int64_t)std::llround(rateHz * 1001.0 / 1000.0);
			if (k > 0)
			{
				const int64_t n = k * 1000;
				const double err = detail::PeriodError(n, 1001, period100ns);
				if (err <= tolerance && (!haveBest || err < bestErr))
				{
					haveBest = true; bestNum = n; bestDen = 1001; bestErr = err;
				}
			}
		}
		if (haveBest)
			return detail::MakeReduced(bestNum, bestDen);

		// 3. Simplest rational inside (1e7/(T+1), 1e7/(T-1)).
		int64_t num = 0, den = 0;
		if (detail::SimplestBetween(kUnitsPerSecond, period100ns + 1,
			kUnitsPerSecond, period100ns - 1, num, den) && num > 0 && den > 0)
		{
			return detail::MakeReduced(num, den);
		}
		// Fallback: the exact rational 1e7 / T.
		return detail::MakeReduced(kUnitsPerSecond, period100ns);
	}

	// AvgTimePerFrame -> exact rational. 0 / negative -> invalid (unknown).
	inline RationalRate SnapAvgTimePerFrame(int64_t avgTimePerFrame)
	{
		return SnapPeriodToRational(avgTimePerFrame);
	}

	// True when the (integer) period lies within one 100 ns unit of the rate's
	// exact period, i.e. the period is a rounding of the rate.
	inline bool PeriodMatchesRate(int64_t period100ns, const RationalRate& rate)
	{
		if (!rate.IsValid() || period100ns <= 0)
			return false;
		return std::fabs(rate.Period100ns() - (double)period100ns) <= 1.0;
	}

	// Accumulates sample start deltas and checks the snapped rate against the
	// real timestamps. VFR-tolerant: decisions use the median delta over a
	// sliding window; a non-increasing timestamp or an explicit discontinuity
	// resets the history.
	class PtsRateVerifier
	{
	public:
		enum class Verdict { Insufficient, Agrees, Disagrees };

		explicit PtsRateVerifier(size_t window = 15, size_t minDeltas = 5) :
			m_window(window < 1 ? 1 : window),
			m_minDeltas(minDeltas < 1 ? 1 : minDeltas)
		{
			if (m_minDeltas > m_window)
				m_minDeltas = m_window;
		}

		void Reset()
		{
			m_deltas.clear();
			m_haveLast = false;
			m_last = 0;
			m_next = 0;
		}

		// Call on flush, seek, step, rate change or a sample discontinuity flag.
		void OnDiscontinuity() { Reset(); }

		void AddSampleStart(int64_t rtStart)
		{
			if (m_haveLast)
			{
				const int64_t delta = rtStart - m_last;
				if (delta <= 0)
				{
					// Out of order / jump backwards: treat as a discontinuity.
					Reset();
				}
				else if (m_deltas.size() < m_window)
				{
					m_deltas.push_back(delta);
				}
				else
				{
					m_deltas[m_next] = delta;
					m_next = (m_next + 1) % m_window;
				}
			}
			m_last = rtStart;
			m_haveLast = true;
		}

		size_t DeltaCount() const { return m_deltas.size(); }
		bool HasMeasurement() const { return m_deltas.size() >= m_minDeltas; }

		// Median delta (lower median for an even count). 0 without measurement.
		int64_t MedianDelta100ns() const
		{
			if (!HasMeasurement())
				return 0;
			std::vector<int64_t> sorted(m_deltas);
			std::sort(sorted.begin(), sorted.end());
			return sorted[(sorted.size() - 1) / 2];
		}

		// Rational snapped from the median delta. Invalid without measurement.
		RationalRate MeasuredRate() const
		{
			return SnapPeriodToRational(MedianDelta100ns());
		}

		Verdict Check(const RationalRate& snapped) const
		{
			if (!HasMeasurement())
				return Verdict::Insufficient;
			return PeriodMatchesRate(MedianDelta100ns(), snapped) ? Verdict::Agrees : Verdict::Disagrees;
		}

		// The rate to report: the snapped AvgTimePerFrame when it agrees with the
		// timestamps (or nothing is measured yet), the measured rational when the
		// header is 0 or disagrees.
		RationalRate EffectiveRate(int64_t avgTimePerFrame) const
		{
			const RationalRate snapped = SnapAvgTimePerFrame(avgTimePerFrame);
			if (!HasMeasurement())
				return snapped;
			if (snapped.IsValid() && Check(snapped) == Verdict::Agrees)
				return snapped;
			const RationalRate measured = MeasuredRate();
			return measured.IsValid() ? measured : snapped;
		}

	private:
		size_t m_window;
		size_t m_minDeltas;
		std::vector<int64_t> m_deltas;
		size_t m_next = 0;
		bool m_haveLast = false;
		int64_t m_last = 0;
	};
}
