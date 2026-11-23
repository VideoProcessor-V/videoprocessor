// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/colour/display_mapping/DvMath.h>
#include <VP-V/modules/colour/display_mapping/DolbyVisionTrims.h>
#include <VP-V/modules/colour/display_mapping/DvDolbyStyle.h>

#include <cmath>
#include <cstring>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 P2: DvMath is the one PQ / ICtCp implementation; the old names forward with identical
// arithmetic (compared bit for bit against a literal copy of the pre-fold formulas).
namespace Tests
{
	namespace
	{
		double OldPqFromNits(double nits)
		{
			const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
			const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
			const double y = (nits < 0.0 ? 0.0 : nits) / 10000.0;
			const double p = std::pow(y, m1);
			return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
		}
		double OldNitsFromPq(double pq)
		{
			const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
			const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
			const double e = std::pow(pq < 0.0 ? 0.0 : pq, 1.0 / m2);
			const double num = e - c1 > 0.0 ? e - c1 : 0.0;
			return 10000.0 * std::pow(num / (c2 - c3 * e), 1.0 / m1);
		}
		bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
	}

	TEST_CLASS(DvMathTests)
	{
	public:
		TEST_METHOD(PqForwardersAreBitIdenticalToTheOldFormulas)
		{
			for (int code = 0; code <= 4095; ++code)
			{
				const double pq = code / 4095.0;
				Assert::IsTrue(SameBits(DvMath::NitsFromPq(pq), OldNitsFromPq(pq)));
				Assert::IsTrue(SameBits(DolbyVisionTrims::NitsFromPq(pq), OldNitsFromPq(pq)));
				const double nits = OldNitsFromPq(pq);
				Assert::IsTrue(SameBits(DvMath::PqFromNits(nits), OldPqFromNits(nits)));
				Assert::IsTrue(SameBits(DolbyVisionTrims::PqFromNits(nits), OldPqFromNits(nits)));
			}
			Assert::IsTrue(SameBits(DvMath::PqFromNits(-5.0), OldPqFromNits(-5.0)));   // below black clamps
		}

		TEST_METHOD(PqRoundTripIsBelowHalfA12BitCode)
		{
			for (int code = 1; code <= 4095; ++code)
			{
				const double pq = code / 4095.0;
				const double back = DvMath::PqFromNits(DvMath::NitsFromPq(pq));
				Assert::IsTrue(std::fabs(back - pq) * 4095.0 < 1e-6);
			}
			Assert::IsTrue(std::fabs(DvMath::NitsFromPq(1.0) - 10000.0) < 1e-6);
		}

		TEST_METHOD(IctcpMatricesAreTheBt2100Table7Values)
		{
			const double rgb[3][3] = { { 1688.0 / 4096, 2146.0 / 4096, 262.0 / 4096 },
				{ 683.0 / 4096, 2951.0 / 4096, 462.0 / 4096 }, { 99.0 / 4096, 309.0 / 4096, 3688.0 / 4096 } };
			const double ict[3][3] = { { 0.5, 0.5, 0.0 }, { 6610.0 / 4096, -13613.0 / 4096, 7003.0 / 4096 },
				{ 17933.0 / 4096, -17390.0 / 4096, -543.0 / 4096 } };
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
				{
					Assert::IsTrue(SameBits(DvMath::kRgb2020ToLms[r][c], rgb[r][c]));
					Assert::IsTrue(SameBits(DvMath::kLmsToIctcp[r][c], ict[r][c]));
					Assert::IsTrue(SameBits(DvDolbyStyle::kRgb2020ToLms[r][c], rgb[r][c]));
					Assert::IsTrue(SameBits(DvDolbyStyle::kLmsToIctcp[r][c], ict[r][c]));
				}
		}

		TEST_METHOD(InvertTimesMatrixIsIdentity)
		{
			double inv[3][3];
			Assert::IsTrue(DvMath::Invert(DvMath::kLmsToIctcp, inv));
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
				{
					double s = 0.0;
					for (int k = 0; k < 3; ++k)
						s += inv[r][k] * DvMath::kLmsToIctcp[k][c];
					Assert::IsTrue(std::fabs(s - (r == c ? 1.0 : 0.0)) < 1e-12);
				}
		}
	};
}
