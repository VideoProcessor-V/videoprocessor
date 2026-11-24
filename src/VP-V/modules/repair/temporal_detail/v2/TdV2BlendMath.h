// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S1 (2026-11-23_TD-consolidated-plan.md s1.1, s1.2, s1.7, s3.2):
// Exact refs/111 quantization-cell blend mathematics:
//   - FP32 centered second-moment covariance accumulation (never FP16, avoiding 10/12-bit cancellation)
//   - Closed-form unbiased optimal blend w* = (C + eps I)^-1 c with convex clip (w_k >= 0, sum w_k <= 1)
//   - Per-pixel 3-sigma Huber outlier taper psi(r) = min(1, 9 / r)
//   - Spatial-only block weight interpolation (ZERO temporal weight ramps)
//   - Blind c estimators: A1 (grain-corrected Three-Cornered Hat), A2 (Laplacian rounding cells), A3 = min(A1, A2)
//   - Both STRICT and CELL residual projection (8x8 DCT transform-identified + visible transform-unknown pixel bound)
//   - Non-finite fail-closed behavior with visible StandInRegistry telemetry
//
// Every constant is mathematically or physically derived and documented inline (Rule 1).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/Ramp.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TdV2
{
	// Maximum simultaneous references supported by closed-form K x K covariance solve:
	//   y_1 = prev peak, y_2 = next peak, y_3 = recurrent carry-forward, y_4 = optional long-reach I-anchor.
	constexpr int kMaxBlendRefs = 4;

	// ---- Rule-1 Derived Mathematical & Physical Constants ----

	// Huber pixel outlier knee: r = d^2 / C_kk > 3^2 = 9 (standard 3-sigma Gaussian tail P(|z| > 3) = 0.27%, refs/111:255).
	constexpr double kHuberKneeSq = 9.0;

	// Pixel-domain band bound multiplier when codec transform is unknown: 3-sigma Gaussian bound (consistent with Huber knee).
	constexpr double kPixelBoundSigma = 3.0;

	// Zero temporal weight ramp invariant (99 s2.1 M2, refs/111:212): weights are interpolated spatially only.
	constexpr double kTemporalWeightRamp = 0.0;

	// Exact 2D separable white-noise variance transfer gains ||H_j||_2^2 of U2 BandFromBase filters:
	//   L  = G(kSigmaBase), M = G(kSigmaFine) - G(kSigmaBase), H = I - G(kSigmaFine),
	//   D3 = G(kSigmaBase) - G(kSigmaLow) (low-mid focus band).
	// For unit-variance uncorrelated white pixel noise (grain or white quantization error), the exact
	// per-band variance is sigma^2 * gain_j (NO re-normalization by 0.809, because D1+D2+L=I has nonzero
	// cross-covariance terms between overlapping filter bands; re-normalizing would over-subtract by +23.6%).
	struct BandWhiteNoiseGains
	{
		double low = 0.0;        // ||G(2.5)||_2^2 ~= 0.0127
		double mid = 0.0;        // ||G(1.0) - G(2.5)||_2^2 ~= 0.0485
		double fine = 0.0;       // ||I - G(1.0)||_2^2 ~= 0.7613
		double d3 = 0.0;         // ||G(2.5) - G(6.0)||_2^2 ~= 0.0074
		// Exact 2D white-noise projection <H_b G, G> / Var(G) = h_b[0, 0] (partition of unity: sum == 1.0).
		double originLow = 0.0;  // g2(0)^2 ~= 0.0255
		double originMid = 0.0;  // g1(0)^2 - g2(0)^2 ~= 0.1338
		double originFine = 0.0; // 1 - g1(0)^2 ~= 0.8408
	};

	inline BandWhiteNoiseGains ComputeBandWhiteNoiseGains()
	{
		const auto g1 = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
		const auto g2 = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaBase, 3.0, true, 0.0);
		const auto g3 = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaLow,  3.0, true, 0.0);
		const int r1 = static_cast<int>(g1.size() / 2);
		const int r2 = static_cast<int>(g2.size() / 2);
		const int r3 = static_cast<int>(g3.size() / 2);

		double sum1Sq = 0.0;
		for (double t : g1) sum1Sq += t * t;
		double sum2Sq = 0.0;
		for (double t : g2) sum2Sq += t * t;
		double sum3Sq = 0.0;
		for (double t : g3) sum3Sq += t * t;

		const int rMin12 = (std::min)(r1, r2);
		double dot12 = 0.0;
		for (int k = -rMin12; k <= rMin12; ++k)
			dot12 += g1[static_cast<size_t>(k + r1)] * g2[static_cast<size_t>(k + r2)];

		const int rMin23 = (std::min)(r2, r3);
		double dot23 = 0.0;
		for (int k = -rMin23; k <= rMin23; ++k)
			dot23 += g2[static_cast<size_t>(k + r2)] * g3[static_cast<size_t>(k + r3)];

		const double g1_0 = g1[static_cast<size_t>(r1)];
		const double g2_0 = g2[static_cast<size_t>(r2)];
		const double normG1Sq = sum1Sq * sum1Sq;
		const double normG2Sq = sum2Sq * sum2Sq;
		const double normG3Sq = sum3Sq * sum3Sq;
		const double cross12Sq = dot12 * dot12;
		const double cross23Sq = dot23 * dot23;

		BandWhiteNoiseGains out;
		out.low = normG2Sq;
		out.mid = normG1Sq - 2.0 * cross12Sq + normG2Sq;
		out.fine = 1.0 - 2.0 * (g1_0 * g1_0) + normG1Sq;
		out.d3 = normG2Sq - 2.0 * cross23Sq + normG3Sq;
		out.originLow = g2_0 * g2_0;
		out.originMid = g1_0 * g1_0 - g2_0 * g2_0;
		out.originFine = 1.0 - g1_0 * g1_0;
		return out;
	}

	// Nominal codec dead-zone rounding offsets phi (refs/111:180: standard x264 / HM / HEVC inter 1/6, intra 1/3)
	// and unbiased uniform integer output rounding (phi = 1/2, exact 1-LSB code step = 1.0 at any bit depth).
	constexpr double kDeadZonePhiInter = 1.0 / 6.0;
	constexpr double kDeadZonePhiIntra = 1.0 / 3.0;
	constexpr double kUniformRoundingPhi = 0.5;
	constexpr double kDefaultStepPriorLsb = 1.0;

	// Strictly numerical singularity & conditioning guards (Rule 1: never physical thresholds).
	// Regularization floor (1e-9) and Gaussian-elimination pivot singularity floor (1e-12) are strictly separated
	// so well-conditioned tiny-scale matrices never falsely trip pivot singularity and rank-deficient matrices do.
	constexpr double kSolveNumericalFloor = 1e-9;
	constexpr double kSolvePivotFloor = 1e-12;
	constexpr double kDefaultTraceRegScale = 1e-2;   // refs/111:211 diagonal conditioning 0.01 * mean(diag C)
	constexpr double kSmallArgTaylorCutoff = 1e-3;   // Switch to 4th-order Taylor series to prevent 0/0 cancellation
	constexpr double kDctPi = 3.14159265358979323846;

	// ---- FP32 Centered Second-Moment Accumulator (2026-11-20 CVQE review s3) ----
	// Accumulates exact centered second moments E[(d_k - mu_k)(d_l - mu_l)] and raw E[d_k d_l]
	// strictly in FP32 (float), never FP16, so 10/12-bit code levels avoid float cancellation.
	struct MomentAccumulatorFp32
	{
		int refCount = 0;
		uint32_t sampleCount = 0;
		uint32_t nonFiniteCount = 0;
		// Two-pass or shifted-origin (first finite sample) FP32 accumulation avoids catastrophic cancellation
		// when input differences carry a DC offset (e.g., local luminance shift).
		bool hasAnchor = false;
		std::array<float, kMaxBlendRefs> anchor{};
		std::array<float, kMaxBlendRefs> sumShifted{};
		std::array<std::array<float, kMaxBlendRefs>, kMaxBlendRefs> sumProdShifted{};

		explicit MomentAccumulatorFp32(int k = 0)
			: refCount(k > kMaxBlendRefs ? kMaxBlendRefs : (k < 0 ? 0 : k))
		{
		}

		void Reset(int k)
		{
			refCount = k > kMaxBlendRefs ? kMaxBlendRefs : (k < 0 ? 0 : k);
			sampleCount = 0;
			nonFiniteCount = 0;
			hasAnchor = false;
			anchor.fill(0.0f);
			sumShifted.fill(0.0f);
			for (int i = 0; i < kMaxBlendRefs; ++i)
			{
				sumProdShifted[i].fill(0.0f);
			}
		}

		bool AddSample(const float* diffs)
		{
			if (diffs == nullptr || refCount <= 0)
				return false;
			for (int i = 0; i < refCount; ++i)
			{
				if (!std::isfinite(diffs[i]))
				{
					++nonFiniteCount;
					return false;
				}
			}
			if (!hasAnchor)
			{
				for (int i = 0; i < refCount; ++i)
					anchor[i] = diffs[i];
				hasAnchor = true;
			}
			for (int i = 0; i < refCount; ++i)
			{
				const float si = diffs[i] - anchor[i];
				sumShifted[i] += si;
				for (int j = i; j < refCount; ++j)
				{
					const float sj = diffs[j] - anchor[j];
					sumProdShifted[i][j] += si * sj;
				}
			}
			++sampleCount;
			return true;
		}

		// Centered covariance E[(d_i - mean_i)(d_j - mean_j)] in FP32, lifted to double for solve.
		void CenteredCovariance(double outC[kMaxBlendRefs][kMaxBlendRefs]) const
		{
			for (int i = 0; i < kMaxBlendRefs; ++i)
				for (int j = 0; j < kMaxBlendRefs; ++j)
					outC[i][j] = 0.0;
			if (sampleCount == 0 || refCount <= 0)
				return;
			const float invN = 1.0f / static_cast<float>(sampleCount);
			for (int i = 0; i < refCount; ++i)
			{
				const float mi = sumShifted[i] * invN;
				for (int j = i; j < refCount; ++j)
				{
					const float mj = sumShifted[j] * invN;
					const float cov = sumProdShifted[i][j] * invN - mi * mj;
					const float val = (i == j && cov < 0.0f) ? 0.0f : cov;
					outC[i][j] = static_cast<double>(val);
					outC[j][i] = static_cast<double>(val);
				}
			}
		}

		// Second moment E[d_i d_j] = Cov(d_i, d_j) + mean_i * mean_j computed stably via shifted origin in FP32.
		void SecondMoment(double outC[kMaxBlendRefs][kMaxBlendRefs]) const
		{
			for (int i = 0; i < kMaxBlendRefs; ++i)
				for (int j = 0; j < kMaxBlendRefs; ++j)
					outC[i][j] = 0.0;
			if (sampleCount == 0 || refCount <= 0)
				return;
			const float invN = 1.0f / static_cast<float>(sampleCount);
			for (int i = 0; i < refCount; ++i)
			{
				const float meanI = anchor[i] + sumShifted[i] * invN;
				const float mi = sumShifted[i] * invN;
				for (int j = i; j < refCount; ++j)
				{
					const float meanJ = anchor[j] + sumShifted[j] * invN;
					const float mj = sumShifted[j] * invN;
					const float cov = sumProdShifted[i][j] * invN - mi * mj;
					const float m2 = cov + meanI * meanJ;
					const float val = (i == j && m2 < 0.0f) ? 0.0f : m2;
					outC[i][j] = static_cast<double>(val);
					outC[j][i] = static_cast<double>(val);
				}
			}
		}
	};

	// ---- Closed-Form Optimal Blend Solve w* = (C + eps I)^-1 c + Convex Clip (refs/111 s2.2 & s3.5) ----
	struct BlendSolveResult
	{
		std::array<double, kMaxBlendRefs> weights{};
		double weightSum = 0.0;
		bool singular = false;
		bool nonFiniteClosed = false;
	};

	inline BlendSolveResult SolveConvexWeights(
		const double C[kMaxBlendRefs][kMaxBlendRefs],
		const double c[kMaxBlendRefs],
		int refCount,
		double traceRegScale = kDefaultTraceRegScale,
		StandInRegistry* standIns = nullptr)
	{
		BlendSolveResult res;
		if (refCount <= 0 || refCount > kMaxBlendRefs || !std::isfinite(traceRegScale) || traceRegScale < 0.0)
		{
			res.nonFiniteClosed = true;
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "SolveConvexWeights invalid args");
			return res;
		}

		// Strict IEEE-754 finite guard: any NaN/Inf closes ALL weights to 0 visibly.
		double diagSum = 0.0;
		for (int i = 0; i < refCount; ++i)
		{
			if (!std::isfinite(c[i]))
			{
				res.nonFiniteClosed = true;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "SolveConvexWeights non-finite c");
				return res;
			}
			for (int j = 0; j < refCount; ++j)
			{
				if (!std::isfinite(C[i][j]))
				{
					res.nonFiniteClosed = true;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "SolveConvexWeights non-finite C");
					return res;
				}
			}
			diagSum += (C[i][i] > 0.0 ? C[i][i] : 0.0);
		}

		if (diagSum <= kSolveNumericalFloor)
		{
			// Zero difference energy across all references -> references identical to current; weight 0 is exact.
			return res;
		}

		const double meanDiag = diagSum / static_cast<double>(refCount);
		const double reg = (std::max)(traceRegScale * meanDiag, kSolveNumericalFloor);
		const double pivotFloor = (std::max)(kSolvePivotFloor, kSolvePivotFloor * meanDiag);

		// Build augmented matrix [A | b] for exact Gaussian elimination with partial pivoting (K <= 4).
		double a[kMaxBlendRefs][kMaxBlendRefs + 1] = {};
		for (int i = 0; i < refCount; ++i)
		{
			for (int j = 0; j < refCount; ++j)
				a[i][j] = C[i][j] + (i == j ? reg : 0.0);
			a[i][refCount] = c[i];
		}

		for (int col = 0; col < refCount; ++col)
		{
			int pivotRow = col;
			double pivotAbs = std::fabs(a[col][col]);
			for (int r = col + 1; r < refCount; ++r)
			{
				const double cand = std::fabs(a[r][col]);
				if (cand > pivotAbs)
				{
					pivotAbs = cand;
					pivotRow = r;
				}
			}
			if (pivotAbs <= pivotFloor)
			{
				res.singular = true;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::SingularSolveClosure, "SolveConvexWeights singular pivot");
				return res;
			}
			if (pivotRow != col)
			{
				for (int j = col; j <= refCount; ++j)
					std::swap(a[col][j], a[pivotRow][j]);
			}
			const double invPivot = 1.0 / a[col][col];
			for (int r = col + 1; r < refCount; ++r)
			{
				const double factor = a[r][col] * invPivot;
				for (int j = col; j <= refCount; ++j)
					a[r][j] -= factor * a[col][j];
			}
		}

		double w[kMaxBlendRefs] = {};
		for (int i = refCount - 1; i >= 0; --i)
		{
			double rhs = a[i][refCount];
			for (int j = i + 1; j < refCount; ++j)
				rhs -= a[i][j] * w[j];
			w[i] = rhs / a[i][i];
			if (!std::isfinite(w[i]))
			{
				res.nonFiniteClosed = true;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "SolveConvexWeights non-finite solution");
				return res;
			}
		}

		// Convex clip (refs/111 s3.5): w_k >= 0, and if sum(w_k) > 1 scale w_k /= sum(w_k).
		// Note: current frame weight is w_0 = 1 - sum_{k>=1} w_k >= 0, guaranteeing affine convex hull.
		double sumW = 0.0;
		for (int i = 0; i < refCount; ++i)
		{
			res.weights[i] = w[i] > 0.0 ? w[i] : 0.0;
			sumW += res.weights[i];
		}
		if (sumW > 1.0)
		{
			const double invSum = 1.0 / sumW;
			for (int i = 0; i < refCount; ++i)
				res.weights[i] *= invSum;
			sumW = 1.0;
		}
		res.weightSum = sumW;
		return res;
	}

	// ---- Per-Pixel 3-Sigma Huber Robust M-Estimator Taper (refs/111:254-255) ----
	// psi(r) = min(1, 9 / r) where r = d^2 / max(C_kk, eps_num). Non-finite closes to 0.
	inline double HuberPixelTaper(double diff, double diagC)
	{
		if (!std::isfinite(diff) || !std::isfinite(diagC))
			return 0.0;
		const double denom = diagC > kSolveNumericalFloor ? diagC : kSolveNumericalFloor;
		const double r = (diff * diff) / denom;
		if (r <= kHuberKneeSq)
			return 1.0;
		return kHuberKneeSq / r;
	}

	// ---- Spatial-Only Weight Interpolation (99 s2.1 M2, refs/111:212: ZERO temporal weight ramps) ----
	inline double SpatialBilinearWeight(
		const double* cellWeights,
		int cellsW,
		int cellsH,
		double pxX,
		double pxY,
		int cellPx)
	{
		if (cellWeights == nullptr || cellsW <= 0 || cellsH <= 0 || cellPx <= 0 ||
		    !std::isfinite(pxX) || !std::isfinite(pxY))
		{
			return 0.0;
		}
		// Cell centers sit at (c + 0.5) * cellPx.
		const double gx = (pxX + 0.5) / static_cast<double>(cellPx) - 0.5;
		const double gy = (pxY + 0.5) / static_cast<double>(cellPx) - 0.5;
		const int x0 = (std::max)(0, (std::min)(cellsW - 1, static_cast<int>(std::floor(gx))));
		const int y0 = (std::max)(0, (std::min)(cellsH - 1, static_cast<int>(std::floor(gy))));
		const int x1 = (std::min)(cellsW - 1, x0 + 1);
		const int y1 = (std::min)(cellsH - 1, y0 + 1);
		const double tx = RampMath::Clamp01(gx - static_cast<double>(x0));
		const double ty = RampMath::Clamp01(gy - static_cast<double>(y0));

		const double w00 = cellWeights[static_cast<size_t>(y0) * cellsW + x0];
		const double w10 = cellWeights[static_cast<size_t>(y0) * cellsW + x1];
		const double w01 = cellWeights[static_cast<size_t>(y1) * cellsW + x0];
		const double w11 = cellWeights[static_cast<size_t>(y1) * cellsW + x1];
		if (!std::isfinite(w00) || !std::isfinite(w10) || !std::isfinite(w01) || !std::isfinite(w11))
			return 0.0;

		const double top = w00 + (w10 - w00) * tx;
		const double bot = w01 + (w11 - w01) * tx;
		return top + (bot - top) * ty;
	}

	// ---- Blind Estimator A1: Grain-Corrected Three-Cornered Hat (refs/111 s3.2) ----
	// c_A1,j = clip(E_b[d_p d_n] - kappa_g * N_g,j, 0, min(C_pp, C_nn))
	// where N_g,j = retainedGrainVar * bandShare * psdScale (retainedGrainVar = mean(f'^2) * N_b from P10g).
	inline double EstimateC_A1(
		double crossPrevNext,
		double cPrevPrev,
		double cNextNext,
		double retainedGrainVar,
		double bandShare,
		double kappaG = 1.0,
		double psdScale = 1.0)
	{
		if (!std::isfinite(crossPrevNext) || !std::isfinite(cPrevPrev) || !std::isfinite(cNextNext) ||
		    !std::isfinite(retainedGrainVar) || !std::isfinite(bandShare) ||
		    !std::isfinite(kappaG) || !std::isfinite(psdScale))
		{
			return 0.0;
		}
		const double upper = (std::max)(0.0, (std::min)(cPrevPrev, cNextNext));
		const double grainTerm = (std::max)(0.0, retainedGrainVar) * (std::max)(0.0, bandShare) *
		                         (std::max)(0.0, kappaG) * (std::max)(0.0, psdScale);
		const double raw = crossPrevNext - grainTerm;
		if (raw <= 0.0)
			return 0.0;
		return raw < upper ? raw : upper;
	}

	// ---- Blind Estimator A2: Codec Rounding Prior (Two-Sided Exponential / Laplacian Quantization Cells, refs/111 s3.3) ----
	// For step Delta, dead-zone offset phi (1/6 inter, 1/3 intra), dead-zone half-width theta = (1 - phi) * Delta,
	// and zero-coefficient fraction pi0 in (0, 1):
	//   MLE decay rate: lambda = -ln(1 - pi0) / theta
	//   V0(lambda, theta) = (2 / lambda^2) * (1 - exp(-b) * (1 + b + 0.5 b^2)) / (1 - exp(-b)),  b = lambda * theta
	//     Uniform limit (b -> 0): theta^2 / 3
	//   V1(lambda, Delta, phi) = v + (m - phi * Delta)^2
	//     m = Delta * (1/a - 1/(exp(a) - 1)),  v = Delta^2 * (1/a^2 - exp(a)/(exp(a) - 1)^2),  a = lambda * Delta
	//     Uniform limit (a -> 0, phi = 1/2): Delta^2 / 12
	struct RoundingCellStats
	{
		double stepDelta = 0.0;
		double phi = kDeadZonePhiInter;
		double theta = 0.0;
		double zeroFraction = 0.0;
		double expDecayRate = 0.0; // lambda
		double varZero = 0.0;      // V0: conditional MSE on zeroed coeffs (k = 0)
		double varSent = 0.0;      // V1: conditional MSE on sent coeffs (k != 0)
		double sigmaZeroSq = 0.0;  // pi0 * V0
		double sigmaSentSq = 0.0;  // (1 - pi0) * V1
		double totalVar = 0.0;     // sigmaZeroSq + sigmaSentSq
		bool valid = false;
	};

	inline RoundingCellStats ComputeRoundingCellStats(
		double stepDelta,
		double phi,
		double zeroFraction)
	{
		RoundingCellStats st;
		if (!std::isfinite(stepDelta) || !std::isfinite(phi) || !std::isfinite(zeroFraction) ||
		    stepDelta <= 0.0 || phi <= 0.0 || phi >= 1.0 || zeroFraction < 0.0 || zeroFraction > 1.0)
		{
			return st;
		}
		st.stepDelta = stepDelta;
		st.phi = phi;
		st.theta = (1.0 - phi) * stepDelta;
		// Clamp pi0 strictly inside [0, 1 - 1e-9] so ln(1 - pi0) stays finite.
		const double pi0 = zeroFraction < 0.0 ? 0.0 : (zeroFraction > (1.0 - kSolveNumericalFloor) ? (1.0 - kSolveNumericalFloor) : zeroFraction);
		st.zeroFraction = pi0;

		const double lambda = (pi0 > 0.0 && st.theta > kSolveNumericalFloor)
			? (-std::log(1.0 - pi0) / st.theta)
			: 0.0;
		st.expDecayRate = lambda;

		// Conditional MSE on k = 0: V0(lambda, theta).
		const double b = lambda * st.theta;
		if (b < kSmallArgTaylorCutoff)
		{
			// Exact 4th-order Taylor expansion around b = 0: theta^2 * (1/3 - b/12 + b^2/360).
			st.varZero = st.theta * st.theta * (1.0 / 3.0 - b / 12.0 + (b * b) / 360.0);
		}
		else
		{
			const double eb = std::exp(-b);
			const double num = 1.0 - eb * (1.0 + b + 0.5 * b * b);
			const double den = 1.0 - eb;
			st.varZero = (2.0 / (lambda * lambda)) * (num / den);
		}

		// Conditional MSE on k != 0: V1(lambda, Delta, phi) = v + (m - phi * Delta)^2.
		const double a = lambda * stepDelta;
		double m = 0.0;
		double v = 0.0;
		if (a < kSmallArgTaylorCutoff)
		{
			// Exact Taylor limits: m/Delta = 1/2 - a/12 + a^3/720; v/Delta^2 = 1/12 - a^2/240.
			m = stepDelta * (0.5 - a / 12.0 + (a * a * a) / 720.0);
			v = stepDelta * stepDelta * (1.0 / 12.0 - (a * a) / 240.0);
		}
		else if (a > 40.0)
		{
			// IEEE-754 FP64 asymptotic cutoff: exp(-40) ~= 4.25e-18 < DBL_EPSILON / 2 (1.11e-16),
			// so 1/(exp(a) - 1) and exp(a)/(exp(a) - 1)^2 vanish below FP64 machine precision:
			// m -> 1 / lambda, v -> 1 / lambda^2 exactly to 53-bit significand precision.
			m = 1.0 / lambda;
			v = 1.0 / (lambda * lambda);
		}
		else
		{
			const double ea = std::exp(a);
			const double eam1 = ea - 1.0;
			m = stepDelta * (1.0 / a - 1.0 / eam1);
			v = stepDelta * stepDelta * (1.0 / (a * a) - ea / (eam1 * eam1));
		}
		if (v < 0.0)
			v = 0.0;
		const double bias = m - phi * stepDelta;
		st.varSent = v + bias * bias;

		st.sigmaZeroSq = pi0 * st.varZero;
		st.sigmaSentSq = (1.0 - pi0) * st.varSent;
		st.totalVar = st.sigmaZeroSq + st.sigmaSentSq;
		st.valid = std::isfinite(st.totalVar);
		return st;
	}

	// Sharing-adjusted Estimator A2 (refs/111 s3.3):
	// c_A2,kj = sigma_s^2 + (1 - zeta_kj) * sigma_z^2, where zeta_kj = 1 on exact copies of ref k.
	inline double EstimateC_A2(const RoundingCellStats& st, bool isExactCopyOfRef)
	{
		if (!st.valid)
			return 0.0;
		const double zeta = isExactCopyOfRef ? 1.0 : 0.0;
		return st.sigmaSentSq + (1.0 - zeta) * st.sigmaZeroSq;
	}

	// Conservative Primary Estimator A3 (refs/111 s3.4): min(A1, A2).
	inline double EstimateC_A3(double cA1, double cA2)
	{
		if (!std::isfinite(cA1) || !std::isfinite(cA2))
			return 0.0;
		const double lo = (std::min)(cA1, cA2);
		return lo > 0.0 ? lo : 0.0;
	}

	// Dispatch blind c estimation per configured VarianceEstimator.
	inline double SelectBlindC(VarianceEstimator est, double cA1, double cA2)
	{
		switch (est)
		{
		case VarianceEstimator::A1_ThreeCorneredHat:
			return (std::isfinite(cA1) && cA1 > 0.0) ? cA1 : 0.0;
		case VarianceEstimator::A2_RoundingCells:
			return (std::isfinite(cA2) && cA2 > 0.0) ? cA2 : 0.0;
		case VarianceEstimator::A3_ConservativeMin:
			return EstimateC_A3(cA1, cA2);
		}
		return 0.0;
	}

	// ---- STRICT & CELL Residual Projection (refs/111 s5; Owner Q1) ----

	// Project one transform coefficient `blendedCoeff` against its received quantized residual `sentCoeff`
	// given quantization step `stepDelta` and dead-zone offset `phi`.
	//   - Level k == 0 (zeroed dead-zone): clipped to [-theta, +theta] in BOTH STRICT and CELL modes.
	//   - Level k != 0 (sent coefficient):
	//       STRICT -> returns `sentCoeff` unchanged (exact bitstream residual preserved).
	//       CELL   -> clipped to rounding cell I_k:
	//                 k > 0: [(k - phi) * Delta, (k + 1 - phi) * Delta]
	//                 k < 0: [(k - 1 + phi) * Delta, (k + phi) * Delta]
	inline double ProjectTransformCoeff(
		double sentCoeff,
		double blendedCoeff,
		double stepDelta,
		double phi,
		ResidualMode mode)
	{
		if (!std::isfinite(sentCoeff) || !std::isfinite(blendedCoeff) ||
		    !std::isfinite(stepDelta) || !std::isfinite(phi) ||
		    stepDelta <= 0.0 || phi < 0.0 || phi >= 1.0)
		{
			return std::isfinite(sentCoeff) ? sentCoeff : 0.0;
		}
		const double theta = (1.0 - phi) * stepDelta;
		// Exact dead-zone quantizer cell index:
		//   |x| < theta => k = 0;  |x| >= theta => |k| = floor(|x| / Delta + phi).
		// Guarantees sentCoeff always lies inside its own cell I_k and maps k * Delta -> k identically.
		const double absSent = std::fabs(sentCoeff);
		long long level = 0;
		if (absSent >= theta)
		{
			const long long mag = static_cast<long long>(std::floor(absSent / stepDelta + phi));
			level = (sentCoeff >= 0.0) ? mag : -mag;
		}
		if (level == 0)
		{
			return (std::max)(-theta, (std::min)(theta, blendedCoeff));
		}
		if (mode == ResidualMode::Strict)
		{
			return sentCoeff;
		}
		// CELL mode: keep coefficient strictly inside its own quantization cell I_k.
		const double k = static_cast<double>(level);
		double lo = 0.0, hi = 0.0;
		if (level > 0)
		{
			lo = (k - phi) * stepDelta;
			hi = (k + 1.0 - phi) * stepDelta;
		}
		else
		{
			lo = (k - 1.0 + phi) * stepDelta;
			hi = (k + phi) * stepDelta;
		}
		return (std::max)(lo, (std::min)(hi, blendedCoeff));
	}

	// Orthonormal 8x8 separable DCT-II (for identified AVC/MPEG transform-domain STRICT/CELL projection).
	inline void ForwardDct8x8(const double in[64], double out[64])
	{
		double tmp[64] = {};
		const double invSqrt8 = 1.0 / std::sqrt(8.0);
		const double scaleAc = 0.5; // sqrt(2 / 8)
		for (int y = 0; y < 8; ++y)
		{
			for (int u = 0; u < 8; ++u)
			{
				const double cu = (u == 0) ? invSqrt8 : scaleAc;
				double sum = 0.0;
				for (int x = 0; x < 8; ++x)
					sum += in[y * 8 + x] * std::cos(kDctPi * (2.0 * x + 1.0) * u / 16.0);
				tmp[y * 8 + u] = cu * sum;
			}
		}
		for (int u = 0; u < 8; ++u)
		{
			for (int v = 0; v < 8; ++v)
			{
				const double cv = (v == 0) ? invSqrt8 : scaleAc;
				double sum = 0.0;
				for (int y = 0; y < 8; ++y)
					sum += tmp[y * 8 + u] * std::cos(kDctPi * (2.0 * y + 1.0) * v / 16.0);
				out[v * 8 + u] = cv * sum;
			}
		}
	}

	inline void InverseDct8x8(const double in[64], double out[64])
	{
		double tmp[64] = {};
		const double invSqrt8 = 1.0 / std::sqrt(8.0);
		const double scaleAc = 0.5;
		for (int v = 0; v < 8; ++v)
		{
			for (int x = 0; x < 8; ++x)
			{
				double sum = 0.0;
				for (int u = 0; u < 8; ++u)
				{
					const double cu = (u == 0) ? invSqrt8 : scaleAc;
					sum += cu * in[v * 8 + u] * std::cos(kDctPi * (2.0 * x + 1.0) * u / 16.0);
				}
				tmp[v * 8 + x] = sum;
			}
		}
		for (int x = 0; x < 8; ++x)
		{
			for (int y = 0; y < 8; ++y)
			{
				double sum = 0.0;
				for (int v = 0; v < 8; ++v)
				{
					const double cv = (v == 0) ? invSqrt8 : scaleAc;
					sum += cv * tmp[v * 8 + x] * std::cos(kDctPi * (2.0 * y + 1.0) * v / 16.0);
				}
				out[y * 8 + x] = sum;
			}
		}
	}

	// Transform-domain 8x8 DCT residual projection for an identified codec transform (AVC/MPEG 8x8).
	// `sentResidual` = X - P_B; `blendedResidual` = z - P_B; writes projected residual into `outResidual`.
	inline void ProjectResidual8x8Dct(
		const double sentResidual[64],
		const double blendedResidual[64],
		double stepDelta,
		double phi,
		ResidualMode mode,
		double outResidual[64])
	{
		double sentDct[64] = {};
		double blendDct[64] = {};
		double projDct[64] = {};
		ForwardDct8x8(sentResidual, sentDct);
		ForwardDct8x8(blendedResidual, blendDct);
		for (int i = 0; i < 64; ++i)
			projDct[i] = ProjectTransformCoeff(sentDct[i], blendDct[i], stepDelta, phi, mode);
		InverseDct8x8(projDct, outResidual);
	}

	// Transform-unknown pixel-domain band bound (HEVC / AV1 over HDMI, plan s1.2 #2 & refs/111:244-245):
	//   |Delta_j(p)| <= kappa_c * sqrt(sigma_0,j^2)
	// Always engages StandInKind::TransformUnknownBound visibly so the pixel-domain bound is NEVER silent.
	inline double ApplyTransformUnknownBound(
		double rawDelta,
		double totalStepVar,
		StandInRegistry* standIns,
		double kappaC = kPixelBoundSigma)
	{
		if (standIns != nullptr)
			standIns->Engage(StandInKind::TransformUnknownBound);
		if (!std::isfinite(rawDelta) || !std::isfinite(totalStepVar) || !std::isfinite(kappaC) ||
		    totalStepVar < 0.0 || kappaC <= 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "ApplyTransformUnknownBound non-finite");
			return 0.0;
		}
		const double bound = kappaC * std::sqrt(totalStepVar);
		return (std::max)(-bound, (std::min)(bound, rawDelta));
	}
}
