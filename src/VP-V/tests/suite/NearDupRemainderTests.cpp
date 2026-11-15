// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Ramp.h>
#include <VP-V/core/RobustStats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Rule 20 near-duplicate remainder (ND06, ND07): the RenderAheadPolicy p95 window, the Regulator window percentile and
// the inline [0,1] clamps now routed to core/RobustStats.h and core/Ramp.h are compared bit for bit with the code they
// replaced (reproduced below as the reference), per site.
namespace NearDupRemainderTest
{
	bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
	bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

	// RenderAheadPolicy.h CostWindow::P95 (before): nearest rank with the index clamped to n - 1.
	double OldP95(std::vector<double> values)
	{
		if (values.empty()) return 0.0;
		const size_t k = std::min(values.size() - 1, static_cast<size_t>(std::ceil(0.95 * values.size())) - 1);
		std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
		return values[k];
	}

	// Regulator.h Percentile (before): FloorIndex on a sorted vector.
	double OldPercentile(const std::vector<double>& sorted, double q)
	{
		return sorted[size_t(std::floor(q * double(sorted.size() - 1)))];
	}

	std::vector<double> Series(size_t n, uint32_t seed)
	{
		std::vector<double> v;
		uint32_t s = seed;
		for (size_t i = 0; i < n; ++i)
		{
			s = s * 1664525u + 1013904223u;
			v.push_back(static_cast<double>(s >> 8) / 65536.0 - 40.0);
		}
		return v;
	}

	TEST_CLASS(NearDupRemainderTests)
	{
	public:
		TEST_METHOD(RenderAheadP95IsTheNearestRank)
		{
			for (size_t n = 1; n <= 300; ++n)
			{
				const std::vector<double> v = Series(n, static_cast<uint32_t>(n * 7919u));
				Assert::IsTrue(SameBits(OldP95(v), RobustStats::Quantile(v, 0.95, RobustStats::RankRule::NearestRank)));
			}
			Assert::IsTrue(SameBits(0.0, RobustStats::Quantile(std::vector<double>(), 0.95, RobustStats::RankRule::NearestRank)));
		}

		TEST_METHOD(RegulatorWindowPercentilesAreTheFloorIndex)
		{
			const double qs[] = { 0.05, 0.50, 0.99 };
			for (size_t n = 1; n <= 300; ++n)
			{
				const std::vector<double> v = Series(n, static_cast<uint32_t>(n * 104729u));
				std::vector<double> sorted(v);
				std::sort(sorted.begin(), sorted.end());
				for (double q : qs)
					Assert::IsTrue(SameBits(OldPercentile(sorted, q), RobustStats::Quantile(v, q, RobustStats::RankRule::FloorIndex)));
			}
		}

		// min(1, max(0, x)) is Clamp01NanToZero for every double / float including NaN, +-inf and -0.
		TEST_METHOD(MinMaxClampIsClamp01NanToZero)
		{
			const double nan = std::numeric_limits<double>::quiet_NaN();
			const double inf = std::numeric_limits<double>::infinity();
			const double xs[] = { nan, inf, -inf, 0.0, -0.0, 1.0, 0.5, -0.25, 1.75, 1e-300, 0.9999999999999999 };
			for (double x : xs)
			{
				Assert::IsTrue(SameBits((std::min)(1.0, (std::max)(0.0, x)), RampMath::Clamp01NanToZero(x)));
				const float f = static_cast<float>(x);
				Assert::IsTrue(SameBits((std::min)(1.0f, (std::max)(0.0f, f)), RampMath::Clamp01NanToZero(f)));
			}
		}
	};
}
