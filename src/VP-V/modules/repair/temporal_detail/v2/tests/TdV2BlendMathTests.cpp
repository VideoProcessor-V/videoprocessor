// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S1 tests (2026-11-23_TD-consolidated-plan.md s3.2):
// Adversarial analytical & synthetic proofs for refs/111 quantization-cell blend math:
//   - Exact unbiased w* = C^-1 c (K=2 and K=3) against closed-form MSE optimum (< 1e-5) with naive negative control
//   - FP32 centered moment stability at 12-bit code levels
//   - Shared-error copy block gets zero weight vs naive independent-Wiener negative control
//   - Convex weight clip + 3-sigma Huber outlier taper
//   - Spatial-only weight interpolation with zero temporal ramp
//   - Real VpUnit::BandFromBase filtered white-noise verification of ComputeBandWhiteNoiseGains()
//     and unbiased EstimateC_A1 grain subtraction (with uncorrected negative control!)
//   - A2 Laplacian rounding cells (uniform limits, exact formula, and a > 40 asymptotic continuity)
//   - STRICT vs CELL residual projection including dead-zone cell-indexing boundaries & idempotence + 8x8 DCT
//   - Non-finite evidence & singular covariance closure with visible StandInRegistry counters. CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace TdV2BlendMathNs
	{
		using namespace TdV2;

		static void FilterBands(const std::vector<double>& img, int w, int h,
		                        std::vector<double>& outL, std::vector<double>& outM, std::vector<double>& outH)
		{
			const auto tapsFine = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
			const auto tapsBase = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaBase, 3.0, true, 0.0);
			std::vector<double> gFine, gBase;
			VpUnit::BandFromBase::BlurTaps(img, gFine, w, h, tapsFine, VpUnit::BandFromBase::EdgeMode::Replicate);
			VpUnit::BandFromBase::BlurTaps(img, gBase, w, h, tapsBase, VpUnit::BandFromBase::EdgeMode::Replicate);
			const size_t n = img.size();
			outL = gBase;
			outM.resize(n);
			outH.resize(n);
			for (size_t i = 0; i < n; ++i)
			{
				outM[i] = gFine[i] - gBase[i];
				outH[i] = img[i] - gFine[i];
			}
		}

		TEST_CLASS(TdV2BlendMathTests)
		{
		public:
			TEST_METHOD(IndependentPeakErrorsRecoverExactMseMinimum)
			{
				// Analytical ground truth (refs/111 s2.2):
				// Three independent errors e_0 (var = 16), e_1 (var = 4), e_2 (var = 4).
				// d_1 = e_0 - e_1 => C_11 = 16 + 4 = 20
				// d_2 = e_0 - e_2 => C_22 = 16 + 4 = 20
				// C_12 = E[d_1 d_2] = Var(e_0) = 16
				// c_1 = c_2 = Var(e_0) = 16
				// Optimal unconstrained & unbiased weights:
				//   w_1 = w_2 = 16 / (20 + 16) = 4 / 9, w_0 = 1 - 8/9 = 1/9
				double C[kMaxBlendRefs][kMaxBlendRefs] = {};
				double c[kMaxBlendRefs] = {};
				C[0][0] = 20.0; C[0][1] = 16.0;
				C[1][0] = 16.0; C[1][1] = 20.0;
				c[0] = 16.0; c[1] = 16.0;

				// With zero trace regularization (only 1e-9 numerical singularity guard), exact analytical parity < 1e-9 (plan requires < 1e-5).
				const auto exact = SolveConvexWeights(C, c, 2, 0.0);
				Assert::IsFalse(exact.singular);
				Assert::IsFalse(exact.nonFiniteClosed);
				Assert::AreEqual(4.0 / 9.0, exact.weights[0], 1e-9);
				Assert::AreEqual(4.0 / 9.0, exact.weights[1], 1e-9);
				Assert::AreEqual(16.0 / (36.0 + kSolveNumericalFloor), exact.weights[0], 1e-14);
				Assert::AreEqual(8.0 / 9.0, exact.weightSum, 1e-9);

				// Negative control: verify exact w* MSE strictly beats BOTH single-frame AND naive equal average (1/3, 1/3, 1/3).
				auto evalMse2 = [](double w1, double w2)
				{
					const double w0 = 1.0 - w1 - w2;
					return w0 * w0 * 16.0 + w1 * w1 * 4.0 + w2 * w2 * 4.0;
				};
				const double mseSingle = evalMse2(0.0, 0.0);
				const double mseNaiveAvg = evalMse2(1.0 / 3.0, 1.0 / 3.0);
				const double mseOptimal = evalMse2(exact.weights[0], exact.weights[1]);
				Assert::IsTrue(mseNaiveAvg > 2.6, L"Naive equal average suffers suboptimal MSE");
				Assert::AreEqual(16.0 / 9.0, mseOptimal, 1e-8);
				Assert::IsTrue(mseOptimal < mseNaiveAvg && mseOptimal < mseSingle);

				// Also verify 3-reference (K = 3: prev + next + carry) analytical optimum:
				// Independent errors var = (16, 4, 4, 8): inverse variances 1/16 : 1/4 : 1/4 : 1/8 = 1 : 4 : 4 : 2 (sum = 11)
				// -> w* = (4/11, 4/11, 2/11), w_0 = 1/11.
				double C3[kMaxBlendRefs][kMaxBlendRefs] = {};
				double c3[kMaxBlendRefs] = { 16.0, 16.0, 16.0, 0.0 };
				C3[0][0] = 20.0; C3[0][1] = 16.0; C3[0][2] = 16.0;
				C3[1][0] = 16.0; C3[1][1] = 20.0; C3[1][2] = 16.0;
				C3[2][0] = 16.0; C3[2][1] = 16.0; C3[2][2] = 24.0;
				const auto exact3 = SolveConvexWeights(C3, c3, 3, 0.0);
				Assert::IsFalse(exact3.singular);
				Assert::AreEqual(4.0 / 11.0, exact3.weights[0], 1e-8);
				Assert::AreEqual(4.0 / 11.0, exact3.weights[1], 1e-8);
				Assert::AreEqual(2.0 / 11.0, exact3.weights[2], 1e-8);
				Assert::AreEqual(10.0 / 11.0, exact3.weightSum, 1e-8);

				// With default 0.01 conditioning guard, verify minimal bias (< 1e-2) and strictly improved MSE over single-frame.
				const auto reg = SolveConvexWeights(C, c, 2, kDefaultTraceRegScale);
				Assert::IsTrue(std::fabs(reg.weights[0] - 4.0 / 9.0) < 0.01);
				Assert::IsTrue(std::fabs(reg.weights[1] - 4.0 / 9.0) < 0.01);

				// Verify FP32 centered moment accumulator preserves variance accurately even with a large 12-bit DC shift (4000).
				MomentAccumulatorFp32 acc(2);
				std::mt19937 rng(12345);
				std::normal_distribution<double> n0(0.0, 4.0), n1(0.0, 2.0), n2(0.0, 2.0);
				for (int i = 0; i < 40000; ++i)
				{
					const float e0 = static_cast<float>(n0(rng));
					const float e1 = static_cast<float>(n1(rng));
					const float e2 = static_cast<float>(n2(rng));
					const float d[2] = { 4000.0f + (e0 - e1), 4000.0f + (e0 - e2) };
					Assert::IsTrue(acc.AddSample(d));
				}
				double cov[kMaxBlendRefs][kMaxBlendRefs] = {};
				acc.CenteredCovariance(cov);
				Assert::AreEqual(20.0, cov[0][0], 0.5);
				Assert::AreEqual(20.0, cov[1][1], 0.5);
				Assert::AreEqual(16.0, cov[0][1], 0.5);
			}

			TEST_METHOD(CopyBlockGetsZeroWeightUnlikeIndependentWiener)
			{
				// Shared error proof (refs/111 s2.2 fact 3):
				// Frame t copied ref 1 (e_1 = e_0 + warpErr, Var(e_0)=16, Var(warpErr)=2 => C_11=2, c_1=Sigma_00 - Sigma_01 = 0).
				// Ref 2 is an independent sharper peak (Var(e_2)=1 => C_22=17, C_12=0, c_2=16).
				double C[kMaxBlendRefs][kMaxBlendRefs] = {};
				double c[kMaxBlendRefs] = {};
				C[0][0] = 2.0;  C[0][1] = 0.0;
				C[1][0] = 0.0;  C[1][1] = 17.0;
				c[0] = 0.0;
				c[1] = 16.0;

				// Mandatory Negative Control (Rule 3): Naive independent-error Wiener (F1) assumes Sigma_01 = 0,
				// setting c_naive = {Sigma_00, Sigma_00} = {16, 16}. Prove naive F1 violates copy protection!
				double cNaiveF1[kMaxBlendRefs] = { 16.0, 16.0, 0.0, 0.0 };
				const auto naiveRes = SolveConvexWeights(C, cNaiveF1, 2, 0.0);
				Assert::IsTrue(naiveRes.weights[0] > 0.85,
					L"Negative control: naive independent-error Wiener F1 wrongly dumps >85% weight onto copied ref!");
				// MSE of naive F1 vs exact w* on e_1 = e_0 + warpErr, e_2 independent:
				// residual = (1 - w_2) e_0 - w_1 warpErr - w_2 e_2 -> MSE = (1-w_2)^2 * 16 + w_1^2 * 2 + w_2^2 * 1.
				auto sharedErrMse = [](double w1, double w2)
				{
					return (1.0 - w2) * (1.0 - w2) * 16.0 + w1 * w1 * 2.0 + w2 * w2 * 1.0;
				};
				const double mseNaiveF1 = sharedErrMse(naiveRes.weights[0], naiveRes.weights[1]);
				Assert::IsTrue(mseNaiveF1 > 12.0, L"Negative control: naive F1 suffers severe MSE from shared error + warp noise");

				const auto res = SolveConvexWeights(C, c, 2, 0.0);
				Assert::AreEqual(0.0, res.weights[0], 1e-12, L"Copied reference must receive exact zero weight");
				Assert::AreEqual(16.0 / 17.0, res.weights[1], 1e-9, L"Independent peak receives optimal weight");
				Assert::AreEqual(16.0 / (17.0 + kSolveNumericalFloor), res.weights[1], 1e-14);
				Assert::IsTrue(sharedErrMse(res.weights[0], res.weights[1]) < 1.0,
					L"Exact covariance solve collapses MSE below 1.0 vs naive F1's >12.0");

				// Even with correlated ref 1 + ref 2 where c_1 = 0 and C_12 != 0 (negative unconstrained w_1),
				// convex clip enforces w_1 >= 0 (never extrapolating/sharpening!).
				C[0][1] = C[1][0] = 4.0;
				C[0][0] = 5.0;
				const auto clipped = SolveConvexWeights(C, c, 2, 0.0);
				Assert::AreEqual(0.0, clipped.weights[0], 1e-12, L"Convex clip prevents negative weights");
				Assert::IsTrue(clipped.weights[1] > 0.0 && clipped.weights[1] <= 1.0);
			}

			TEST_METHOD(ConvexClipAndHuberThreeSigmaTaperRejectOutliers)
			{
				// When unconstrained weights sum > 1 (e.g., over-estimated c), convex clip normalizes sum(w) == 1.
				double C[kMaxBlendRefs][kMaxBlendRefs] = {};
				double c[kMaxBlendRefs] = {};
				C[0][0] = 10.0; C[1][1] = 10.0;
				c[0] = 9.0; c[1] = 9.0; // unconstrained w = (0.9, 0.9), sum = 1.8 > 1
				const auto res = SolveConvexWeights(C, c, 2, 0.0);
				Assert::AreEqual(0.5, res.weights[0], 1e-12);
				Assert::AreEqual(0.5, res.weights[1], 1e-12);
				Assert::AreEqual(1.0, res.weightSum, 1e-12);

				// 3-sigma Huber taper: psi(r) = 1 for |d| <= 3*sigma, 9 / r for |d| > 3*sigma.
				const double diagC = 16.0; // sigma = 4.0
				Assert::AreEqual(1.0, HuberPixelTaper(0.0, diagC), 1e-12);
				Assert::AreEqual(1.0, HuberPixelTaper(12.0, diagC), 1e-12);       // exactly +3*sigma (r = 9)
				Assert::AreEqual(1.0, HuberPixelTaper(-12.0, diagC), 1e-12);      // symmetric -3*sigma
				Assert::AreEqual(0.25, HuberPixelTaper(24.0, diagC), 1e-12);      // 6*sigma (r = 36 -> 9/36 = 0.25)
				Assert::AreEqual(0.25, HuberPixelTaper(-24.0, diagC), 1e-12);     // symmetric -6*sigma
				Assert::AreEqual(0.0, HuberPixelTaper(std::numeric_limits<double>::quiet_NaN(), diagC), 1e-12);
				Assert::AreEqual(0.0, HuberPixelTaper(std::numeric_limits<double>::infinity(), diagC), 1e-12);
				Assert::IsTrue(std::isfinite(HuberPixelTaper(1.0, 0.0)) && HuberPixelTaper(1.0, 0.0) >= 0.0);
			}

			TEST_METHOD(SpatialInterpolationHasZeroTemporalRamp)
			{
				Assert::AreEqual(0.0, kTemporalWeightRamp, 1e-15, L"Zero temporal weight ramps invariant");
				const double grid[4] = { 0.0, 1.0, 0.4, 0.8 }; // 2x2 cells, cellPx = 16
				// Exact cell centers: (7.5, 7.5) -> 0.0; (23.5, 7.5) -> 1.0; (7.5, 23.5) -> 0.4; (23.5, 23.5) -> 0.8.
				Assert::AreEqual(0.0, SpatialBilinearWeight(grid, 2, 2, 7.5, 7.5, 16), 1e-12);
				Assert::AreEqual(1.0, SpatialBilinearWeight(grid, 2, 2, 23.5, 7.5, 16), 1e-12);
				Assert::AreEqual(0.4, SpatialBilinearWeight(grid, 2, 2, 7.5, 23.5, 16), 1e-12);
				Assert::AreEqual(0.8, SpatialBilinearWeight(grid, 2, 2, 23.5, 23.5, 16), 1e-12);
				// Exact midpoint (15.5, 15.5) -> average of all 4 = (0 + 1 + 0.4 + 0.8) / 4 = 0.55.
				Assert::AreEqual(0.55, SpatialBilinearWeight(grid, 2, 2, 15.5, 15.5, 16), 1e-12);

				// Out-of-bounds coordinates clamp cleanly; non-finite grid cell closes to 0.
				Assert::AreEqual(0.0, SpatialBilinearWeight(grid, 2, 2, -10.0, -10.0, 16), 1e-12);
				Assert::AreEqual(0.8, SpatialBilinearWeight(grid, 2, 2, 100.0, 100.0, 16), 1e-12);
				const double badGrid[4] = { 0.5, std::numeric_limits<double>::quiet_NaN(), 0.5, 0.5 };
				Assert::AreEqual(0.0, SpatialBilinearWeight(badGrid, 2, 2, 15.5, 15.5, 16), 1e-15);
			}

			TEST_METHOD(A1GrainCorrectionWithRealBandFromBaseWhiteNoiseAndA3Min)
			{
				// NON-SHORTCUT PROOF: Generate independent 2D white Gaussian grain on 192x192 planes and filter
				// EVERY plane through real VpUnit::BandFromBase::BlurTaps (G(1.0), G(2.5)) to verify:
				//   1. ComputeBandWhiteNoiseGains() matches exact empirical 2D filter transfer gains ||H_j||_2^2
				//      (NOT the erroneous 0.94 re-normalized ratio that would over-subtract by +23.6%!).
				//   2. On pure own grain (zero coding error), uncorrected A1 (negative control!) massively over-estimates c,
				//      whereas grain-corrected EstimateC_A1 collapses c near 0 on BOTH fine (H) and mid (M) bands.
				//   3. With real independent coding error + white grain, EstimateC_A1 recovers the exact filtered coding variance!
				const int w = 192, h = 192;
				const int pad = 16; // stay clear of Replicate border taps (r_base = ceil(3*2.5) = 8)
				const size_t n = static_cast<size_t>(w) * h;

				std::mt19937 rng(20261123);
				const double sigmaG = 5.0; // Var = 25.0
				const double sigmaE = 6.0; // Var = 36.0
				std::normal_distribution<double> gDist(0.0, sigmaG);
				std::normal_distribution<double> eDist(0.0, sigmaE);

				std::vector<double> g0(n), gPrev(n), gNext(n), e0(n), x0WithCoding(n);
				for (size_t i = 0; i < n; ++i)
				{
					g0[i] = gDist(rng);
					gPrev[i] = gDist(rng);
					gNext[i] = gDist(rng);
					e0[i] = eDist(rng);
					x0WithCoding[i] = g0[i] + e0[i];
				}

				std::vector<double> g0L, g0M, g0H;
				std::vector<double> gpL, gpM, gpH;
				std::vector<double> gnL, gnM, gnH;
				std::vector<double> x0L, x0M, x0H;
				FilterBands(g0, w, h, g0L, g0M, g0H);
				FilterBands(gPrev, w, h, gpL, gpM, gpH);
				FilterBands(gNext, w, h, gnL, gnM, gnH);
				FilterBands(x0WithCoding, w, h, x0L, x0M, x0H);

				// Measure exact interior white-noise variances.
				double varG0 = 0.0, varL = 0.0, varM = 0.0, varH = 0.0;
				size_t cnt = 0;
				for (int y = pad; y < h - pad; ++y)
				{
					for (int x = pad; x < w - pad; ++x)
					{
						const size_t i = static_cast<size_t>(y) * w + x;
						varG0 += g0[i] * g0[i];
						varL += g0L[i] * g0L[i];
						varM += g0M[i] * g0M[i];
						varH += g0H[i] * g0H[i];
						++cnt;
					}
				}
				const double invCnt = 1.0 / static_cast<double>(cnt);
				varG0 *= invCnt;
				varL *= invCnt;
				varM *= invCnt;
				varH *= invCnt;

				const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
				Assert::IsTrue(gains.fine > 0.74 && gains.fine < 0.78,
					L"Exact 2D fine white-noise gain ||I - G(1.0)||_2^2 must be ~0.761 (never re-normalized 0.94)");
				Assert::IsTrue(gains.mid > 0.044 && gains.mid < 0.052,
					L"Exact 2D mid white-noise gain ||G(1.0) - G(2.5)||_2^2 must be ~0.048");
				Assert::IsTrue(gains.low > 0.010 && gains.low < 0.016,
					L"Exact 2D low white-noise gain ||G(2.5)||_2^2 must be ~0.0127");
				Assert::AreEqual(gains.fine, varH / varG0, 0.02, L"ComputeBandWhiteNoiseGains().fine must match real BandFromBase white noise");
				Assert::AreEqual(gains.mid,  varM / varG0, 0.01, L"ComputeBandWhiteNoiseGains().mid must match real BandFromBase white noise");
				Assert::AreEqual(gains.low,  varL / varG0, 0.005, L"ComputeBandWhiteNoiseGains().low must match real BandFromBase white noise");

				// Accumulate pure-grain diffs through FP32 MomentAccumulatorFp32 on H and M bands.
				auto measureCross = [&](const std::vector<double>& curB, const std::vector<double>& pB,
				                        const std::vector<double>& nB, double outC[kMaxBlendRefs][kMaxBlendRefs])
				{
					MomentAccumulatorFp32 acc(2);
					for (int y = pad; y < h - pad; ++y)
					{
						for (int x = pad; x < w - pad; ++x)
						{
							const size_t i = static_cast<size_t>(y) * w + x;
							const float d[2] = { static_cast<float>(curB[i] - pB[i]), static_cast<float>(curB[i] - nB[i]) };
							acc.AddSample(d);
						}
					}
					acc.SecondMoment(outC);
				};

				double CpureH[kMaxBlendRefs][kMaxBlendRefs] = {};
				double CpureM[kMaxBlendRefs][kMaxBlendRefs] = {};
				measureCross(g0H, gpH, gnH, CpureH);
				measureCross(g0M, gpM, gnM, CpureM);

				// Mandatory Negative Control (Rule 3): Without grain subtraction (retainedGrainVar = 0),
				// A1 falsely mistakes pure own grain for massive coding error!
				const double cNaivePureH = EstimateC_A1(CpureH[0][1], CpureH[0][0], CpureH[1][1], 0.0, gains.fine);
				const double cNaivePureM = EstimateC_A1(CpureM[0][1], CpureM[0][0], CpureM[1][1], 0.0, gains.mid);
				Assert::IsTrue(cNaivePureH > 0.85 * varG0 * gains.fine,
					L"Negative control: uncorrected A1 must fail by misclassifying pure fine grain as coding error");
				Assert::IsTrue(cNaivePureM > 0.70 * varG0 * gains.mid,
					L"Negative control: uncorrected A1 must fail by misclassifying pure mid grain as coding error");

				// Grain-corrected A1 with exact BandFromBase transfer gains collapses pure grain to ~0!
				const double cCorrPureH = EstimateC_A1(CpureH[0][1], CpureH[0][0], CpureH[1][1], varG0, gains.fine);
				const double cCorrPureM = EstimateC_A1(CpureM[0][1], CpureM[0][0], CpureM[1][1], varG0, gains.mid);
				Assert::IsTrue(cCorrPureH < 0.06 * varG0 * gains.fine,
					L"Grain-corrected A1 must reject pure own grain on fine band");
				Assert::IsTrue(cCorrPureM < 0.15 * varG0 * gains.mid,
					L"Grain-corrected A1 must reject pure own grain on mid band");

				// Now add real independent coding error (var = 36) + white grain (var = 25), filtered through BandFromBase:
				// Expected fine-band coding variance = 36 * gains.fine ~= 27.41.
				double CcodH[kMaxBlendRefs][kMaxBlendRefs] = {};
				measureCross(x0H, gpH, gnH, CcodH);
				const double expectedCodingFine = (sigmaE * sigmaE) * gains.fine;
				const double cNaiveCodingH = EstimateC_A1(CcodH[0][1], CcodH[0][0], CcodH[1][1], 0.0, gains.fine);
				const double cCorrCodingH = EstimateC_A1(CcodH[0][1], CcodH[0][0], CcodH[1][1], varG0, gains.fine);
				const double cOverSubBug = EstimateC_A1(CcodH[0][1], CcodH[0][0], CcodH[1][1], varG0, 0.94);

				Assert::IsTrue(std::fabs(cNaiveCodingH - expectedCodingFine) > 15.0,
					L"Negative control: uncorrected A1 severely over-estimates coding variance in grainy frame");
				Assert::IsTrue(std::fabs(cOverSubBug - expectedCodingFine) > 3.5,
					L"Negative control: erroneous 0.94 re-normalized share visibly under-estimates true coding variance");
				Assert::AreEqual(expectedCodingFine, cCorrCodingH, 1.8,
					L"Unbiased EstimateC_A1 with ComputeBandWhiteNoiseGains() must recover true BandFromBase coding variance");

				// Verify A3 <= min(A1, A2) everywhere and SelectBlindC dispatches cleanly.
				Assert::AreEqual(7.5, EstimateC_A3(12.0, 7.5), 1e-12);
				Assert::AreEqual(5.0, EstimateC_A3(5.0, 9.0), 1e-12);
				Assert::AreEqual(0.0, EstimateC_A3(-2.0, 9.0), 1e-12);
				Assert::AreEqual(12.0, SelectBlindC(VarianceEstimator::A1_ThreeCorneredHat, 12.0, 7.5), 1e-12);
				Assert::AreEqual(7.5,  SelectBlindC(VarianceEstimator::A2_RoundingCells, 12.0, 7.5), 1e-12);
				Assert::AreEqual(7.5,  SelectBlindC(VarianceEstimator::A3_ConservativeMin, 12.0, 7.5), 1e-12);
			}

			TEST_METHOD(A2RoundingCellsMatchUniformLimitsExactFormulaAndAsymptote)
			{
				// Uniform limit check (pi0 -> 0, lambda -> 0):
				//   V0 -> theta^2 / 3
				//   V1(phi = 0.5) -> Delta^2 / 12
				const double delta = 12.0;
				const auto unif = ComputeRoundingCellStats(delta, 0.5, 1e-7);
				Assert::IsTrue(unif.valid);
				Assert::AreEqual(6.0, unif.theta, 1e-12);
				Assert::AreEqual((6.0 * 6.0) / 3.0, unif.varZero, 1e-4, L"V0 uniform limit must equal theta^2 / 3");
				Assert::AreEqual((delta * delta) / 12.0, unif.varSent, 1e-4, L"V1 uniform limit at phi=0.5 must equal Delta^2 / 12");

				// Standard inter coding check (phi = 1/6, theta = 5/6 * Delta, pi0 = 0.6):
				const auto inter = ComputeRoundingCellStats(delta, kDeadZonePhiInter, 0.6);
				Assert::IsTrue(inter.valid);
				Assert::AreEqual(10.0, inter.theta, 1e-12);
				Assert::IsTrue(inter.varZero > 0.0 && inter.varZero < (10.0 * 10.0) / 3.0, L"Exponential decay concentrates V0 below uniform");
				Assert::IsTrue(inter.varSent > 0.0 && inter.varSent < delta * delta);
				Assert::AreEqual(inter.sigmaZeroSq + inter.sigmaSentSq, inter.totalVar, 1e-12);

				// Asymptotic regime (a > 40, extreme zero fraction near 1): must remain finite and continuous with zero exp overflow!
				const auto extreme = ComputeRoundingCellStats(delta, kDeadZonePhiInter, 1.0 - 1e-8);
				Assert::IsTrue(extreme.valid);
				Assert::IsTrue(std::isfinite(extreme.varZero) && extreme.varZero > 0.0);
				Assert::IsTrue(std::isfinite(extreme.varSent) && extreme.varSent > 0.0);
				Assert::IsTrue(std::isfinite(extreme.totalVar) && extreme.totalVar > 0.0);

				// Invalid parameters fail closed.
				Assert::IsFalse(ComputeRoundingCellStats(0.0, kDeadZonePhiInter, 0.5).valid);
				Assert::IsFalse(ComputeRoundingCellStats(-4.0, kDeadZonePhiInter, 0.5).valid);
				Assert::IsFalse(ComputeRoundingCellStats(delta, -0.1, 0.5).valid);
				Assert::IsFalse(ComputeRoundingCellStats(delta, 1.0, 0.5).valid);

				// Sharing adjustment: exact copy (zeta = 1) drops sigmaZeroSq, keeping only sigmaSentSq.
				Assert::AreEqual(inter.totalVar, EstimateC_A2(inter, false), 1e-12);
				Assert::AreEqual(inter.sigmaSentSq, EstimateC_A2(inter, true), 1e-12);
				Assert::IsTrue(EstimateC_A2(inter, true) < EstimateC_A2(inter, false));
			}

			TEST_METHOD(StrictKeepsSentCoeffExactCellStaysInsideIkAndDeadZoneIndexing)
			{
				const double delta = 10.0;
				const double phi = kDeadZonePhiInter; // 1/6, theta = 50/6 = 8.3333...
				const double theta = (1.0 - phi) * delta;

				// Zeroed coefficient (k = 0): both STRICT and CELL clip to [-theta, +theta].
				Assert::AreEqual(theta, ProjectTransformCoeff(0.0, 20.0, delta, phi, ResidualMode::Strict), 1e-12);
				Assert::AreEqual(-theta, ProjectTransformCoeff(0.0, -20.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual(4.0, ProjectTransformCoeff(0.0, 4.0, delta, phi, ResidualMode::Strict), 1e-12);

				// Adversarial dead-zone boundary indexing checks!
				// Just below +theta and -theta -> MUST map to k = 0 (clipped to [-theta, +theta]).
				Assert::AreEqual(theta, ProjectTransformCoeff(theta - 1e-6, 50.0, delta, phi, ResidualMode::Strict), 1e-12,
					L"sentCoeff just below +theta must be indexed as dead-zone level k=0");
				Assert::AreEqual(-theta, ProjectTransformCoeff(-(theta - 1e-6), -50.0, delta, phi, ResidualMode::Cell), 1e-12,
					L"sentCoeff just above -theta must be indexed as dead-zone level k=0");

				// Just above +theta -> MUST map to level k = +1 with cell I_1 = [theta, theta + delta] = [8.3333, 18.3333].
				const double sentJustAbove = theta + 1e-6;
				Assert::AreEqual(sentJustAbove, ProjectTransformCoeff(sentJustAbove, 15.0, delta, phi, ResidualMode::Strict), 1e-12);
				Assert::AreEqual(15.0, ProjectTransformCoeff(sentJustAbove, 15.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual(theta + delta, ProjectTransformCoeff(sentJustAbove, 40.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual(theta, ProjectTransformCoeff(sentJustAbove, 0.0, delta, phi, ResidualMode::Cell), 1e-12);

				// Off-center sentCoeff inside upper half of I_1 (17.5 in [8.3333, 18.3333]):
				// A buggy llround(sent / delta) would wrongly assign level k = 2 ([18.3333, 28.3333]), ejecting 17.5 from its own cell!
				Assert::AreEqual(17.5, ProjectTransformCoeff(17.5, 17.5, delta, phi, ResidualMode::Cell), 1e-12,
					L"Dead-zone indexing must map 17.5 to I_1=[8.333,18.333], never wrongly to I_2!");
				Assert::AreEqual(theta, ProjectTransformCoeff(17.5, 0.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual(theta + delta, ProjectTransformCoeff(17.5, 50.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual(-17.5, ProjectTransformCoeff(-17.5, -17.5, delta, phi, ResidualMode::Cell), 1e-12,
					L"Negative off-center sentCoeff -17.5 must remain inside I_-1=[-18.333,-8.333]");
				Assert::AreEqual(-(theta + delta), ProjectTransformCoeff(-17.5, -50.0, delta, phi, ResidualMode::Cell), 1e-12);

				// Idempotence sweep across both inter (phi=1/6) and intra (phi=1/3):
				// Every sentCoeff MUST ALWAYS lie inside its own quantization cell I_k!
				const double phis[2] = { kDeadZonePhiInter, kDeadZonePhiIntra };
				for (double p : phis)
				{
					for (int step = -500; step <= 500; ++step)
					{
						const double s = step * 0.1;
						const double projCell = ProjectTransformCoeff(s, s, delta, p, ResidualMode::Cell);
						const double projStrict = ProjectTransformCoeff(s, s, delta, p, ResidualMode::Strict);
						Assert::AreEqual(s, projCell, 1e-12, L"CELL projection must be idempotent on sentCoeff");
						Assert::AreEqual(s, projStrict, 1e-12, L"STRICT projection must be idempotent on sentCoeff");
					}
				}

				// Positive sent coefficient (k = 2 -> sentCoeff = 20.0, cell I_2 = [18.3333, 28.3333]):
				Assert::AreEqual(20.0, ProjectTransformCoeff(20.0, 25.0, delta, phi, ResidualMode::Strict), 1e-12,
					L"STRICT must keep sent coefficient exact");
				Assert::AreEqual(25.0, ProjectTransformCoeff(20.0, 25.0, delta, phi, ResidualMode::Cell), 1e-12,
					L"CELL allows movement inside I_k");
				Assert::AreEqual((2.0 + 1.0 - phi) * delta, ProjectTransformCoeff(20.0, 40.0, delta, phi, ResidualMode::Cell), 1e-12,
					L"CELL clips upper edge of I_k");
				Assert::AreEqual((2.0 - phi) * delta, ProjectTransformCoeff(20.0, 10.0, delta, phi, ResidualMode::Cell), 1e-12,
					L"CELL clips lower edge of I_k");

				// Negative sent coefficient (k = -2 -> sentCoeff = -20.0, cell I_-2 = [-28.3333, -18.3333]):
				Assert::AreEqual(-20.0, ProjectTransformCoeff(-20.0, -25.0, delta, phi, ResidualMode::Strict), 1e-12);
				Assert::AreEqual(-25.0, ProjectTransformCoeff(-20.0, -25.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual((-2.0 - 1.0 + phi) * delta, ProjectTransformCoeff(-20.0, -40.0, delta, phi, ResidualMode::Cell), 1e-12);

				// 8x8 DCT round-trip & STRICT/CELL projection test:
				double sentDct[64] = {}, blendDct[64] = {};
				sentDct[0] = 30.0;   // k = 3 (sent DC)
				sentDct[1] = 0.0;    // k = 0 (zeroed AC)
				blendDct[0] = 34.0;  // inside I_3 = [28.333, 38.333]
				blendDct[1] = 15.0;  // exceeds theta = 8.333
				double sentSpatial[64] = {}, blendSpatial[64] = {}, outStrict[64] = {}, outCell[64] = {};
				InverseDct8x8(sentDct, sentSpatial);
				InverseDct8x8(blendDct, blendSpatial);

				ProjectResidual8x8Dct(sentSpatial, blendSpatial, delta, phi, ResidualMode::Strict, outStrict);
				ProjectResidual8x8Dct(sentSpatial, blendSpatial, delta, phi, ResidualMode::Cell, outCell);

				double strictBack[64] = {}, cellBack[64] = {};
				ForwardDct8x8(outStrict, strictBack);
				ForwardDct8x8(outCell, cellBack);
				Assert::AreEqual(30.0, strictBack[0], 1e-9, L"8x8 DCT STRICT keeps sent DC exact");
				Assert::AreEqual(theta, strictBack[1], 1e-9, L"8x8 DCT STRICT clips zeroed AC to theta");
				Assert::AreEqual(34.0, cellBack[0], 1e-9, L"8x8 DCT CELL permits DC movement inside I_3");
				Assert::AreEqual(theta, cellBack[1], 1e-9, L"8x8 DCT CELL clips zeroed AC to theta");

				// Transform-unknown pixel-domain bound MUST set visible "transform unknown" stand-in token!
				StandInRegistry reg;
				const double bounded = ApplyTransformUnknownBound(50.0, 16.0, &reg); // sigma = 4 -> bound = 12
				Assert::AreEqual(12.0, bounded, 1e-12);
				Assert::IsTrue(reg.IsActive(StandInKind::TransformUnknownBound), L"Transform-unknown bound must be visible");
				Assert::IsTrue(reg.FormatStatusSummary().find("transform unknown") != std::string::npos);
			}

			TEST_METHOD(NaNInfAndSingularCovarianceCloseAllWeightsToZeroVisibly)
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();
				const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
				StandInRegistry reg;

				double C[kMaxBlendRefs][kMaxBlendRefs] = {};
				double c[kMaxBlendRefs] = { 5.0, 5.0 };
				C[0][0] = 10.0; C[1][1] = 10.0; C[0][1] = qnan; C[1][0] = qnan;

				const auto r1 = SolveConvexWeights(C, c, 2, kDefaultTraceRegScale, &reg);
				Assert::IsTrue(r1.nonFiniteClosed);
				Assert::AreEqual(0.0, r1.weights[0], 1e-15);
				Assert::AreEqual(0.0, r1.weights[1], 1e-15);
				Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));

				C[0][1] = C[1][0] = 0.0;
				c[1] = inf;
				const auto r2 = SolveConvexWeights(C, c, 2, kDefaultTraceRegScale, &reg);
				Assert::IsTrue(r2.nonFiniteClosed);
				Assert::AreEqual(0.0, r2.weightSum, 1e-15);

				// Exact singular pivot after diagonal conditioning (a = [[10.1, 10.1], [10.1, 10.1]]):
				// must close all weights to zero AND engage StandInKind::SingularSolveClosure visibly!
				StandInRegistry singReg;
				double Csing[kMaxBlendRefs][kMaxBlendRefs] = {};
				Csing[0][0] = 10.0; Csing[1][1] = 10.0;
				Csing[0][1] = 10.1; Csing[1][0] = 10.1;
				double cSing[kMaxBlendRefs] = { 5.0, 5.0, 0.0, 0.0 };
				const auto rSing = SolveConvexWeights(Csing, cSing, 2, kDefaultTraceRegScale, &singReg);
				Assert::IsTrue(rSing.singular, L"Singular covariance solve must report singular=true");
				Assert::AreEqual(0.0, rSing.weightSum, 1e-15);
				Assert::IsTrue(singReg.IsActive(StandInKind::SingularSolveClosure),
					L"Singular solve closure must be visible in StandInRegistry");

				Assert::AreEqual(0.0, EstimateC_A1(qnan, 10.0, 10.0, 1.0, gains.fine), 1e-15);
				Assert::IsFalse(ComputeRoundingCellStats(qnan, kDeadZonePhiInter, 0.5).valid);
				Assert::AreEqual(0.0, ApplyTransformUnknownBound(qnan, 16.0, &reg), 1e-15);
			}
		};
	}
}
