// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/colour/display_mapping/DmCurve.h>
#include <VP-V/modules/colour/display_mapping/DvMath.h>
#include <VP-V/modules/input/source_analysis/identity/SourceQualityView.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 P4: DmCurve picks the tone LUT size from a measured interpolation error and takes the contrast-recovery
// need from the P14 view. CPU only; the GPU ramp row (no step > 1 code at PRE_OUTPUT) is P6.
namespace Tests
{
	namespace
	{
		DvDolbyStyle::Curve SceneCurve(double sMaxNits, double sAvgNits, double tMaxNits)
		{
			return DvDolbyStyle::Build(DvMath::PqFromNits(0.005), DvMath::PqFromNits(sAvgNits), DvMath::PqFromNits(sMaxNits),
				DvMath::PqFromNits(0.05), DvMath::PqFromNits(tMaxNits));
		}
	}

	TEST_CLASS(DmCurveLutSizeTests)
	{
	public:
		TEST_METHOD(ErrorFallsAsTheLutGrowsAndIsZeroForIdentity)
		{
			const DvDolbyStyle::Curve c = SceneCurve(4000.0, 200.0, 600.0);
			Assert::IsFalse(c.identity, L"a 4000-nit scene on a 600-nit screen is compressed");
			double previous = 1e9;
			for (size_t size : DmCurve::Candidates())
			{
				const double e = DmCurve::LutInterpolationError(c, size);
				Assert::IsTrue(e <= previous + 1e-9, L"the error never rises with the size");
				previous = e;
			}
			const DvDolbyStyle::Curve fits = SceneCurve(500.0, 100.0, 1000.0);
			Assert::IsTrue(fits.identity || DmCurve::LutInterpolationError(fits, 256) < 1.0);
			Assert::AreEqual(0.0, DmCurve::LutInterpolationError(DvDolbyStyle::Curve(), 256));
		}

		TEST_METHOD(ChoosesTheSmallestCandidateUnderTheBudget)
		{
			for (double target : { 120.0, 300.0, 600.0, 1000.0 })
			{
				const DvDolbyStyle::Curve c = SceneCurve(4000.0, 150.0, target);
				const DmCurve::LutChoice r = DmCurve::ChooseLutSize(c);
				Assert::IsTrue(r.size > 0);
				if (!r.withinBudget)
				{
					Assert::AreEqual(DmCurve::Candidates().back(), r.size, L"no candidate passed: the largest is kept and reported");
					continue;
				}
				Assert::IsTrue(r.errorCodes <= DmCurve::kMaxErrorCodes, L"within budget");
				Assert::IsTrue(DmCurve::LutInterpolationError(c, r.size) == r.errorCodes);
				// Smallest: the candidate below it (if any) misses the budget.
				const auto& ladder = DmCurve::Candidates();
				for (size_t i = 1; i < ladder.size(); ++i)
					if (ladder[i] == r.size)
						Assert::IsTrue(DmCurve::LutInterpolationError(c, ladder[i - 1]) > DmCurve::kMaxErrorCodes, L"the smaller size fails");
			}
		}

		TEST_METHOD(ABudgetNoCandidateMeetsKeepsTheLargestAndSaysSo)
		{
			const DvDolbyStyle::Curve c = SceneCurve(4000.0, 150.0, 300.0);
			const DmCurve::LutChoice r = DmCurve::ChooseLutSize(c, 0.0);
			Assert::IsFalse(r.withinBudget);
			Assert::AreEqual(DmCurve::Candidates().back(), r.size);
		}

		TEST_METHOD(IdentityOrNoRangeLeavesLibplaceboSize)
		{
			Assert::AreEqual<size_t>(0, DmCurve::ChooseLutSize(DvDolbyStyle::Curve()).size);
			Assert::AreEqual<size_t>(0, DmCurve::ChooseLutSize(DvDolbyStyle::Build(0.5, 0.5, 0.5, 0.0, 1.0)).size);
		}

		TEST_METHOD(TheBudgetIsStatedInTwelveBitCodes)
		{
			Assert::AreEqual(4095.0, DmCurve::kCodeScale);
			Assert::AreEqual(12, DmCurve::kCodeBits);
		}
	};

	TEST_CLASS(DmContrastNeedTests)
	{
	public:
		static SourceIdentity::QualityView View(double quality, bool consensusOn)
		{
			SourceIdentity::ClassicalQuality c;
			c.reading = true;
			c.bitrateQuality = quality;
			return SourceIdentity::View(nullptr, c, consensusOn, 0);
		}

		TEST_METHOD(AStrugglingSourceKeepsTheConfiguredRecovery)
		{
			const DmCurve::Recovery r = DmCurve::RecoveryFromNeed(View(0.2, false), 0.35);
			Assert::IsTrue(r.needed);
			Assert::AreEqual(0.35, r.value);
			Assert::IsTrue(r.status.empty(), L"consensus off is not a fallback");
		}

		TEST_METHOD(ACleanSourceGetsNoRecovery)
		{
			const DmCurve::Recovery r = DmCurve::RecoveryFromNeed(View(0.99, false), 0.35);
			Assert::IsFalse(r.needed);
			Assert::AreEqual(0.0, r.value);
		}

		TEST_METHOD(NeedAbsentUsesTheExistingClassicalQualityLine)
		{
			// Consensus requested but no record: the classical value stands in and the line is the existing wording.
			const DmCurve::Recovery r = DmCurve::RecoveryFromNeed(View(0.2, true), 0.35);
			Assert::IsTrue(r.status.find("contrast recovery: classical quality") == 0, L"the existing wording");
			Assert::IsTrue(r.needed, L"the classical value still decides");
			Assert::AreEqual(0.35, r.value);
		}

		TEST_METHOD(TheNeedComesFromTheSharedHelperOnly)
		{
			for (double q : { 0.0, 0.3, 0.6, 0.9, 1.0 })
			{
				const SourceIdentity::QualityView v = View(q, false);
				Assert::AreEqual(SourceIdentity::ContrastRecoveryActive(v), DmCurve::RecoveryFromNeed(v, 1.0).needed);
			}
		}
	};
}
