// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S1 tests (2026-11-23_TD-consolidated-plan.md s3.2):
// Analytical and synthetic proofs for refs/111 quantization-cell blend math:
//   - Exact unbiased w* = C^-1 c against closed-form MSE optimum (< 1e-5)
//   - FP32 centered moment stability at 12-bit code levels
//   - Shared-error copy block gets zero weight (fixes independent Wiener F1 flaw)
//   - Convex weight clip + 3-sigma Huber outlier taper
//   - Spatial-only weight interpolation with zero temporal ramp
//   - A1 grain-corrected Three-Cornered Hat, A2 Laplacian rounding cells (uniform limits & exact), A3 = min(A1, A2)
//   - STRICT vs CELL residual projection (1D + 8x8 DCT) + visible "transform unknown" pixel bound
//   - Non-finite evidence closes all weights/deltas to zero visibly. CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

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
				//   (matches inverse-variance optimal 1/16 : 1/4 : 1/4 = 1 : 4 : 4 -> 1/9, 4/9, 4/9!)
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
				// If frame t copied ref 1 (Sigma_01 = Sigma_00), then c_1 = Sigma_00 - Sigma_01 = 0 -> w_1 = 0!
				// Meanwhile ref 2 is an independent sharper peak (Sigma_02 = 0, Sigma_22 = 1).
				double C[kMaxBlendRefs][kMaxBlendRefs] = {};
				double c[kMaxBlendRefs] = {};
				// Ref 1 has some warp error (var = 2) on top of copied error -> C_11 = 2, C_12 = 0, c_1 = 0.
				// Ref 2 has independent error -> C_22 = 16 + 1 = 17, c_2 = 16.
				C[0][0] = 2.0;  C[0][1] = 0.0;
				C[1][0] = 0.0;  C[1][1] = 17.0;
				c[0] = 0.0;
				c[1] = 16.0;

				const auto res = SolveConvexWeights(C, c, 2, 0.0);
				Assert::AreEqual(0.0, res.weights[0], 1e-12, L"Copied reference must receive exact zero weight");
				Assert::AreEqual(16.0 / 17.0, res.weights[1], 1e-9, L"Independent peak receives optimal weight");
				Assert::AreEqual(16.0 / (17.0 + kSolveNumericalFloor), res.weights[1], 1e-14);

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
				Assert::AreEqual(1.0, HuberPixelTaper(12.0, diagC), 1e-12);      // exactly 3*sigma (r = 9)
				Assert::AreEqual(0.25, HuberPixelTaper(24.0, diagC), 1e-12);     // 6*sigma (r = 36 -> 9/36 = 0.25)
				Assert::AreEqual(0.0, HuberPixelTaper(std::numeric_limits<double>::quiet_NaN(), diagC), 1e-12);
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
			}

			TEST_METHOD(A1GrainCorrectionRejectsOwnGrainAndA3IsConservativeMin)
			{
				// Pure own grain scenario (refs/111 s3.2):
				// Three frames have ZERO coding error, only independent own grain of variance N = 25.
				// In the fine band (share beta_H = kBandGrainShareFine), E[d_p d_n] = Var(g_0,H) = N * beta_H,
				// C_pp = C_nn = 2 * N * beta_H. Grain-corrected A1 MUST subtract N * beta_H -> c_A1 = 0!
				const double grainVar = 25.0;
				const double fineGrainVar = grainVar * kBandGrainShareFine;
				const double cA1PureGrain = EstimateC_A1(fineGrainVar, 2.0 * fineGrainVar, 2.0 * fineGrainVar, grainVar, kBandGrainShareFine);
				Assert::AreEqual(0.0, cA1PureGrain, 1e-12, L"A1 must reject pure own grain as coding error");

				// Now add real independent coding error var = 12 on current frame:
				// E[d_p d_n] = 12 + fineGrainVar -> c_A1 recovers exact 12!
				const double cA1WithCoding = EstimateC_A1(
					12.0 + fineGrainVar, 12.0 + 2.0 * fineGrainVar, 12.0 + 2.0 * fineGrainVar,
					grainVar, kBandGrainShareFine);
				Assert::AreEqual(12.0, cA1WithCoding, 1e-12, L"A1 must recover true coding variance after grain subtraction");

				// Verify A3 <= min(A1, A2) everywhere.
				Assert::AreEqual(7.5, EstimateC_A3(12.0, 7.5), 1e-12);
				Assert::AreEqual(5.0, EstimateC_A3(5.0, 9.0), 1e-12);
				Assert::AreEqual(0.0, EstimateC_A3(-2.0, 9.0), 1e-12);
			}

			TEST_METHOD(A2RoundingCellsMatchUniformLimitsAndExactFormula)
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

				// Sharing adjustment: exact copy (zeta = 1) drops sigmaZeroSq, keeping only sigmaSentSq.
				Assert::AreEqual(inter.totalVar, EstimateC_A2(inter, false), 1e-12);
				Assert::AreEqual(inter.sigmaSentSq, EstimateC_A2(inter, true), 1e-12);
				Assert::IsTrue(EstimateC_A2(inter, true) < EstimateC_A2(inter, false));
			}

			TEST_METHOD(StrictKeepsSentCoeffExactAndCellStaysInsideIk)
			{
				const double delta = 10.0;
				const double phi = kDeadZonePhiInter; // 1/6, theta = 50/6 = 8.3333...
				const double theta = (1.0 - phi) * delta;

				// Zeroed coefficient (k = 0): both STRICT and CELL clip to [-theta, +theta].
				Assert::AreEqual(theta, ProjectTransformCoeff(0.0, 20.0, delta, phi, ResidualMode::Strict), 1e-12);
				Assert::AreEqual(-theta, ProjectTransformCoeff(0.0, -20.0, delta, phi, ResidualMode::Cell), 1e-12);
				Assert::AreEqual(4.0, ProjectTransformCoeff(0.0, 4.0, delta, phi, ResidualMode::Strict), 1e-12);

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

			TEST_METHOD(NaNInfEvidenceClosesAllWeightsToZero)
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();
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

				Assert::AreEqual(0.0, EstimateC_A1(qnan, 10.0, 10.0, 1.0, kBandGrainShareFine), 1e-15);
				Assert::IsFalse(ComputeRoundingCellStats(qnan, kDeadZonePhiInter, 0.5).valid);
				Assert::AreEqual(0.0, ApplyTransformUnknownBound(qnan, 16.0, &reg), 1e-15);
			}
		};
	}
}
