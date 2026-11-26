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
#include <VP-V/modules/motion/estimation/composed/WarpSampler.h>
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

	// Pixel-domain band bound multiplier kappa_c when codec transform is unknown (TD-U3 / Owner D3):
	// strict default 1.0, adjustable range [1.0, 3.0].
	constexpr double kPixelBoundKappaMin = 1.0;
	constexpr double kPixelBoundKappaMax = 3.0;
	constexpr double kPixelBoundKappaDefault = 1.0;
	constexpr double kPixelBoundSigma = kPixelBoundKappaDefault;

	inline double ClampPixelBoundKappa(double kappa)
	{
		if (!std::isfinite(kappa))
			return kPixelBoundKappaDefault;
		if (kappa < kPixelBoundKappaMin)
			return kPixelBoundKappaMin;
		if (kappa > kPixelBoundKappaMax)
			return kPixelBoundKappaMax;
		return kappa;
	}

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

	// ---- SOTA #1: Per-Cell AQ & ST.2084 PQ Step Scaling (Rule 1: Derived Codec & SMPTE ST 2084 Physics) ----
	// Physical step-scale bounds: +/- 4 octaves = +/- 24 QP steps (exact 6-QP doubling law 2^(dQP/6)), spanning [1/16, 16].
	constexpr double kCellStepScaleMin = 1.0 / 16.0;
	constexpr double kCellStepScaleMax = 16.0;
	// ST.2084 PQ shadow sensitivity weight bounds (TD-U3 / M-6): PQ perceptual factor may ONLY tighten
	// down to 1/4 in deep shadows relative to ITU-R BT.2408 diffuse white (203 nits), NEVER widen beyond 1.0.
	constexpr double kPqStepWeightMin = 0.25;
	constexpr double kPqStepWeightMax = 1.0;

	// Compute per-cell quantization step scale factor combining:
	//   1) Optional HEVC / AVC / AV1 standard 6-QP step doubling law: 2^(cellQpDelta / 6) (clamped to +/- 24 QP => [1/16, 16]),
	//      gated behind `enableAqStepScaling` (TD-U3 / M-6: OFF by default until a live HDMI AQ producer exists).
	//   2) Exact SMPTE ST 2084 PQ EOTF logarithmic slope (Weber contrast per unit PQ code) relative to
	//      ITU-R BT.2408 HDR diffuse white (203 nits) when `isPqHdr == true`:
	//        S(e) = d(ln L)/de = (1/M1) * (1/(p - C1) + C3/(C2 - C3*p)) * (p / (M2 * e)),  p = e^(1/M2)
	//        pqWeight(e) = clamp(S(e_203) / S(e), 1/4, 1.0)
	//      At >= 203 nits: pqWeight == 1.0 (never widens); in deep PQ shadows where 1 code step causes a larger relative
	//      luminance jump, pqWeight tightens smoothly down to 0.25 to respect PQ shadow JND sensitivity.
	// When !enableAqStepScaling and !isPqHdr, returns exact 1.0 (100% baseline identity).
	inline double ComputeCellStepScale(
		double cellQpDelta,
		double cellMeanPqCode,
		bool isPqHdr,
		double sourceBits = 10.0,
		bool enableAqStepScaling = false)
	{
		if (!std::isfinite(cellQpDelta))
			return 1.0;
		const double clampedQp = cellQpDelta < -24.0 ? -24.0 : (cellQpDelta > 24.0 ? 24.0 : cellQpDelta);
		const double aqScale = enableAqStepScaling ? std::exp2(clampedQp / 6.0) : 1.0;

		double pqWeight = 1.0;
		if (isPqHdr)
		{
			if (!std::isfinite(cellMeanPqCode) || !std::isfinite(sourceBits) || !(sourceBits > 0.0) || sourceBits > 24.0)
				return 1.0;
			const double peakCode = std::exp2(sourceBits) - 1.0;
			if (!(peakCode > 0.0))
				return 1.0;

			// Exact SMPTE ST 2084 rational constants and ITU-R BT.2408 diffuse white reference (203 nits / 10000 nits).
			constexpr double kPqM1 = 2610.0 / 16384.0;
			constexpr double kPqM2 = (2523.0 / 4096.0) * 128.0;
			constexpr double kPqC1 = 3424.0 / 4096.0;
			constexpr double kPqC2 = (2413.0 / 4096.0) * 32.0;
			constexpr double kPqC3 = (2392.0 / 4096.0) * 32.0;
			constexpr double kPqCeilingNits = 10000.0;
			constexpr double kBt2408DiffuseWhiteNits = 203.0;

			auto pqLogSlope = [&](double e) -> double
			{
				if (!(e > 0.0))
					return 0.0;
				const double ec = (e < 1.0) ? e : 1.0;
				const double p = std::pow(ec, 1.0 / kPqM2);
				const double numGap = p - kPqC1;
				const double denGap = kPqC2 - kPqC3 * p;
				if (!(numGap > kSolvePivotFloor) || !(denGap > kSolvePivotFloor))
					return 0.0;
				return (1.0 / kPqM1) * (1.0 / numGap + kPqC3 / denGap) * (p / (kPqM2 * ec));
			};

			const double yRef = std::pow(kBt2408DiffuseWhiteNits / kPqCeilingNits, kPqM1);
			const double eRef = std::pow((kPqC1 + kPqC2 * yRef) / (1.0 + kPqC3 * yRef), kPqM2);
			const double sRef = pqLogSlope(eRef);

			const double e = RampMath::Clamp01(cellMeanPqCode / peakCode);
			const double sCur = pqLogSlope(e);
			if (sCur > kSolvePivotFloor && sRef > kSolvePivotFloor)
			{
				const double rawPq = sRef / sCur;
				pqWeight = rawPq < kPqStepWeightMin ? kPqStepWeightMin : (rawPq > kPqStepWeightMax ? kPqStepWeightMax : rawPq);
			}
			else
			{
				// Pure black (p <= C1, L == 0): Weber contrast per code step diverges -> tighten to shadow floor.
				pqWeight = kPqStepWeightMin;
			}
		}

		const double maxScale = enableAqStepScaling ? kCellStepScaleMax : 1.0;
		const double combined = aqScale * pqWeight;
		if (!std::isfinite(combined))
			return 1.0;
		return combined < kCellStepScaleMin ? kCellStepScaleMin : (combined > maxScale ? maxScale : combined);
	}

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
		StandInRegistry* standIns = nullptr,
		SolverMode solverMode = SolverMode::ActiveSetExact)
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

		if (solverMode == SolverMode::LegacyClipRescale)
		{
			// Legacy refs/111 s3.5 unconstrained solve + clip-then-rescale (retained strictly for ablation).
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

		// ---- TD-U2 (H-2): Exact Active-Set Constrained QP Solver ----
		// Minimize Q(w) = w' A w - 2 w' c  subject to  w_k >= 0, sum_k w_k <= 1,
		// where A = 0.5 * (C + C') + reg * I (K <= 4 -> at most 15 non-empty subsets + w = 0).
		double A[kMaxBlendRefs][kMaxBlendRefs] = {};
		for (int i = 0; i < refCount; ++i)
		{
			for (int j = 0; j < refCount; ++j)
				A[i][j] = 0.5 * (C[i][j] + C[j][i]) + (i == j ? reg : 0.0);
		}

		// Verify full regularized matrix A is strictly SPD via Cholesky; singular or indefinite matrices fail closed visibly.
		{
			double Lfull[kMaxBlendRefs][kMaxBlendRefs] = {};
			for (int i = 0; i < refCount; ++i)
			{
				for (int j = 0; j < i; ++j)
				{
					double s = A[i][j];
					for (int p = 0; p < j; ++p)
						s -= Lfull[i][p] * Lfull[j][p];
					Lfull[i][j] = s / Lfull[j][j];
				}
				double d = A[i][i];
				for (int p = 0; p < i; ++p)
					d -= Lfull[i][p] * Lfull[i][p];
				if (!std::isfinite(d) || d <= pivotFloor)
				{
					res.singular = true;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::SingularSolveClosure, "SolveConvexWeights singular/non-PSD regularized covariance");
					return res;
				}
				Lfull[i][i] = std::sqrt(d);
			}
		}

		// Helper: solve L L^T x = rhs for SPD Cholesky factor L of size sz <= 4.
		auto choleskySolve = [](const double L[kMaxBlendRefs][kMaxBlendRefs], const double rhs[kMaxBlendRefs],
		                        int sz, double outX[kMaxBlendRefs]) -> bool
		{
			double y[kMaxBlendRefs] = {};
			for (int i = 0; i < sz; ++i)
			{
				double s = rhs[i];
				for (int p = 0; p < i; ++p)
					s -= L[i][p] * y[p];
				y[i] = s / L[i][i];
				if (!std::isfinite(y[i]))
					return false;
			}
			for (int i = sz - 1; i >= 0; --i)
			{
				double s = y[i];
				for (int p = i + 1; p < sz; ++p)
					s -= L[p][i] * outX[p];
				outX[i] = s / L[i][i];
				if (!std::isfinite(outX[i]))
					return false;
			}
			return true;
		};

		// Candidate w = 0 is ALWAYS feasible with Q(0) = 0.0.
		std::array<double, kMaxBlendRefs> bestW{};
		double bestObj = 0.0;
		double bestSum = 0.0;

		auto tryCandidate = [&](const double cand[kMaxBlendRefs])
		{
			double w[kMaxBlendRefs] = {};
			double sumW = 0.0;
			for (int i = 0; i < refCount; ++i)
			{
				if (!std::isfinite(cand[i]) || cand[i] < -kSolvePivotFloor)
					return;
				w[i] = cand[i] > 0.0 ? cand[i] : 0.0;
				sumW += w[i];
			}
			if (sumW > 1.0 + kSolveNumericalFloor)
				return;
			if (sumW > 1.0)
			{
				const double invSum = 1.0 / sumW;
				for (int i = 0; i < refCount; ++i)
					w[i] *= invSum;
				sumW = 1.0;
			}
			double quad = 0.0;
			double lin = 0.0;
			for (int i = 0; i < refCount; ++i)
			{
				lin += w[i] * c[i];
				for (int j = 0; j < refCount; ++j)
					quad += w[i] * A[i][j] * w[j];
			}
			const double obj = quad - 2.0 * lin;
			if (std::isfinite(obj) && obj < bestObj)
			{
				bestObj = obj;
				bestSum = sumW;
				for (int i = 0; i < refCount; ++i)
					bestW[i] = w[i];
			}
		};

		const int totalMasks = (1 << refCount);
		for (int mask = 1; mask < totalMasks; ++mask)
		{
			int idx[kMaxBlendRefs] = {};
			int sz = 0;
			for (int i = 0; i < refCount; ++i)
			{
				if ((mask & (1 << i)) != 0)
					idx[sz++] = i;
			}

			// Cholesky factor of active submatrix A_SS.
			double Ls[kMaxBlendRefs][kMaxBlendRefs] = {};
			bool subOk = true;
			for (int i = 0; i < sz && subOk; ++i)
			{
				for (int j = 0; j < i; ++j)
				{
					double s = A[idx[i]][idx[j]];
					for (int p = 0; p < j; ++p)
						s -= Ls[i][p] * Ls[j][p];
					Ls[i][j] = s / Ls[j][j];
				}
				double d = A[idx[i]][idx[i]];
				for (int p = 0; p < i; ++p)
					d -= Ls[i][p] * Ls[i][p];
				if (!std::isfinite(d) || d <= pivotFloor)
				{
					subOk = false;
					break;
				}
				Ls[i][i] = std::sqrt(d);
			}
			if (!subOk)
				continue;

			double cSub[kMaxBlendRefs] = {};
			for (int i = 0; i < sz; ++i)
				cSub[i] = c[idx[i]];

			double uSub[kMaxBlendRefs] = {};
			if (!choleskySolve(Ls, cSub, sz, uSub))
				continue;

			// Candidate A: unconstrained optimum on active subset S (feasible if u >= 0 and sum(u) <= 1).
			double candU[kMaxBlendRefs] = {};
			double sumU = 0.0;
			for (int i = 0; i < sz; ++i)
			{
				candU[idx[i]] = uSub[i];
				sumU += uSub[i];
			}
			tryCandidate(candU);

			// Candidate B: sum == 1 KKT simplex border on active subset S (needed when sumU > 1 or boundary optimal).
			if (sz == 1)
			{
				double candB[kMaxBlendRefs] = {};
				candB[idx[0]] = 1.0;
				tryCandidate(candB);
			}
			else if (sumU > 1.0 - kSolveNumericalFloor)
			{
				double onesSub[kMaxBlendRefs] = {};
				for (int i = 0; i < sz; ++i)
					onesSub[i] = 1.0;
				double vSub[kMaxBlendRefs] = {};
				if (choleskySolve(Ls, onesSub, sz, vSub))
				{
					double sumV = 0.0;
					for (int i = 0; i < sz; ++i)
						sumV += vSub[i];
					if (std::isfinite(sumV) && sumV > pivotFloor)
					{
						const double mu = (sumU - 1.0) / sumV;
						double candB[kMaxBlendRefs] = {};
						for (int i = 0; i < sz; ++i)
							candB[idx[i]] = uSub[i] - mu * vSub[i];
						tryCandidate(candB);
					}
				}
			}
		}

		res.weights = bestW;
		res.weightSum = bestSum;
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

	// ---- SOTA #5: Grain-Free 3x3 Neighborhood C^1 Soft Clamp on M/H Bands (Rule 1: Zero Invented Constants) ----
	// Clamps warped reference band value `refVal` smoothly into [mean - gamma*sigma, mean + gamma*sigma]
	// where gamma = sqrt(kHuberKneeSq) = 3.0 (derived 3-sigma Gaussian bound).
	// By Samuelson's inequality for N = 9 samples in a 3x3 neighborhood, every current neighborhood pixel satisfies
	//   |v_i - mean| <= sqrt(N - 1) * sigma = sqrt(8) * sigma < 3 * sigma.
	// Keeping exact identity for |d| <= sqrt(8) * sigma and C^1 rational saturation across the remaining
	// (3 - sqrt(8)) * sigma shoulder guarantees:
	//   1) Exact bit-identity on 100% of valid static/rigid detail (refVal == curVal never biased),
	//   2) C^1 continuity (value and slope == 1.0 at |d| = sqrt(8) * sigma),
	//   3) Strict containment inside [mean - 3*sigma, mean + 3*sigma] for all unmasked micro-motion outliers.
	inline double SoftClampToNeighborhood3Sigma(double refVal, double mean, double sigma)
	{
		if (!std::isfinite(refVal) || !std::isfinite(mean) || !std::isfinite(sigma))
			return std::isfinite(mean) ? mean : 0.0;
		if (!(sigma > kSolveNumericalFloor))
			return mean;
		const double gamma = std::sqrt(kHuberKneeSq);  // 3.0 (derived 3-sigma Gaussian bound)
		const double r0 = std::sqrt(8.0) * sigma;      // Exact Samuelson bound sqrt(9 - 1) * sigma for 3x3 patch
		const double d = refVal - mean;
		const double absD = std::fabs(d);
		if (absD <= r0)
			return refVal;
		const double shoulder = (gamma - std::sqrt(8.0)) * sigma;
		const double excess = absD - r0;
		const double clampedMag = r0 + shoulder * (excess / (excess + shoulder));
		return mean + (d >= 0.0 ? clampedMag : -clampedMag);
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

	// ---- SOTA #2: Exact Parseval Fine-Band (H = I - G_fine) Bicubic Warp Variance Transfer Gain ----
	// Computes beta_HP(vx, vy) = ||H * W(vx, vy)||_2^2 / ||H||_2^2 using shared VpComposed::CubicWeights
	// and U2 VpUnit::BandFromBase::kSigmaFine taps (Rule 1: zero invented constants; exact 1.0 at integer shifts).
	inline double FineBandBicubicWarpVarianceGain(double vx, double vy)
	{
		if (!std::isfinite(vx) || !std::isfinite(vy))
			return 1.0;
		const auto g = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
		if (g.empty())
			return 1.0;
		const int gLen = static_cast<int>(g.size());
		const int r = gLen / 2;

		auto axisFactors = [&](double v, double& outWW, double& outUU, double& outWU, double& outGG, double& outG0)
		{
			const double f = v - std::floor(v);
			double w[4] = {};
			VpComposed::CubicWeights(f, w);
			outWW = 0.0;
			for (int i = 0; i < 4; ++i)
				outWW += w[i] * w[i];
			outGG = 0.0;
			for (int j = 0; j < gLen; ++j)
				outGG += g[static_cast<size_t>(j)] * g[static_cast<size_t>(j)];
			outG0 = g[static_cast<size_t>(r)];
			outUU = 0.0;
			outWU = 0.0;
			const int mMin = -r - 1;
			const int mMax = (gLen - 1 - r) + 2;
			for (int m = mMin; m <= mMax; ++m)
			{
				double u = 0.0;
				for (int i = 0; i < 4; ++i)
				{
					const int idx = (m - (i - 1)) + r;
					if (idx >= 0 && idx < gLen)
						u += w[i] * g[static_cast<size_t>(idx)];
				}
				outUU += u * u;
				if (m >= -1 && m <= 2)
					outWU += w[m + 1] * u;
			}
		};

		double wwX = 0.0, uuX = 0.0, wuX = 0.0, ggX = 0.0, g0X = 0.0;
		double wwY = 0.0, uuY = 0.0, wuY = 0.0, ggY = 0.0, g0Y = 0.0;
		axisFactors(vx, wwX, uuX, wuX, ggX, g0X);
		axisFactors(vy, wwY, uuY, wuY, ggY, g0Y);

		const double den = 1.0 - 2.0 * g0X * g0Y + ggX * ggY;
		if (!(den > kSolveNumericalFloor))
			return RampMath::Clamp01(wwX * wwY);
		const double num = wwX * wwY - 2.0 * wuX * wuY + uuX * uuY;
		const double ratio = num / den;
		if (!std::isfinite(ratio))
			return RampMath::Clamp01(wwX * wwY);
		return RampMath::Clamp01(ratio);
	}

	// Shared sub-pixel bicubic H-band attenuation bias in E[d_p d_q] for partner pair (p, q):
	//   alpha_p = sqrt(FineBandBicubicWarpVarianceGain(vx_p, vy_p)),  alpha_q = sqrt(FineBandBicubicWarpVarianceGain(vx_q, vy_q)),
	//   bias    = (1 - alpha_p) * (1 - alpha_q) * max(0, curHVar - max(0, grainHVar)).
	// Identically 0.0 whenever either partner is integer-shifted (alpha == 1.0).
	inline double ComputeSubpixelHAttenuationBias(
		double vxP, double vyP,
		double vxQ, double vyQ,
		double curHVar, double grainHVar)
	{
		if (!std::isfinite(vxP) || !std::isfinite(vyP) || !std::isfinite(vxQ) || !std::isfinite(vyQ) ||
		    !std::isfinite(curHVar) || !std::isfinite(grainHVar))
		{
			return 0.0;
		}
		const double alphaP = std::sqrt(FineBandBicubicWarpVarianceGain(vxP, vyP));
		const double alphaQ = std::sqrt(FineBandBicubicWarpVarianceGain(vxQ, vyQ));
		const double cleanTexHVar = (std::max)(0.0, curHVar - (std::max)(0.0, grainHVar));
		const double bias = (1.0 - alphaP) * (1.0 - alphaQ) * cleanTexHVar;
		return (std::isfinite(bias) && bias > 0.0) ? bias : 0.0;
	}

	// ---- SOTA #6: P10g Spectral Grain PSD Shape Multipliers (1.0 = exact white-noise identity) ----
	// Scales EstimateC_A1's band grain subtraction N_g,b = retainedGrainVar * bandTransfer[b] * psdScale[b]
	// so EstimateC_A1 remains unbiased under clumpy AR(2) / AV1-AFGS1 spectrally coloured film grain.
	struct PsdShape
	{
		double bandShareLow = 1.0;   // Low band PSD multiplier relative to white noise (default 1.0 neutral)
		double bandShareMid = 1.0;   // Mid band PSD multiplier relative to white noise (default 1.0 neutral)
		double bandShareFine = 1.0;  // Fine band PSD multiplier relative to white noise (default 1.0 neutral)

		static PsdShape White() { return PsdShape{ 1.0, 1.0, 1.0 }; }

		double ForBand(int b) const
		{
			if (b == 0) return bandShareLow;
			if (b == 1) return bandShareMid;
			return bandShareFine;
		}

		bool IsValid() const
		{
			return std::isfinite(bandShareLow) && bandShareLow >= 0.0 &&
			       std::isfinite(bandShareMid) && bandShareMid >= 0.0 &&
			       std::isfinite(bandShareFine) && bandShareFine >= 0.0;
		}
	};

	// ---- Blind Estimator A1: Grain-Corrected Three-Cornered Hat (refs/111 s3.2 + SOTA #2 H-band debias) ----
	// c_A1,j = clip(E_b[d_p d_n] - kappa_g * N_g,j - subpixelAttenBias, 0, min(C_pp, C_nn))
	// where N_g,j = retainedGrainVar * bandShare * psdScale (retainedGrainVar = mean(f'^2) * N_b from P10g).
	inline double EstimateC_A1(
		double crossPrevNext,
		double cPrevPrev,
		double cNextNext,
		double retainedGrainVar,
		double bandShare,
		double kappaG = 1.0,
		double psdScale = 1.0,
		double subpixelAttenBias = 0.0)
	{
		if (!std::isfinite(crossPrevNext) || !std::isfinite(cPrevPrev) || !std::isfinite(cNextNext) ||
		    !std::isfinite(retainedGrainVar) || !std::isfinite(bandShare) ||
		    !std::isfinite(kappaG) || !std::isfinite(psdScale) || !std::isfinite(subpixelAttenBias))
		{
			return 0.0;
		}
		const double upper = (std::max)(0.0, (std::min)(cPrevPrev, cNextNext));
		const double grainTerm = (std::max)(0.0, retainedGrainVar) * (std::max)(0.0, bandShare) *
		                         (std::max)(0.0, kappaG) * (std::max)(0.0, psdScale);
		const double biasTerm = (std::max)(0.0, subpixelAttenBias);
		const double raw = crossPrevNext - grainTerm - biasTerm;
		if (raw <= 0.0)
			return 0.0;
		return raw < upper ? raw : upper;
	}

	// ---- TD-U4 (H-3): Derived Effective Independent Samples & Empirical-Bayes Shrinkage of c_IV ----
	// Effective independent sample count for band b over `sampleCountPx` pixels (Rule 1: derived from BandFromBase sigmas):
	//   sigma_L = kSigmaBase (2.5 px) -> 2*pi*sigma_L^2 ~= 39.27 px^2 (~6.52/cell, ~163 per 5x5 pool)
	//   sigma_M = kSigmaFine (1.0 px) -> 2*pi*sigma_M^2 ~=  6.28 px^2 (~40.7/cell, ~367 per 3x3 pool)
	//   sigma_H = 0.5 px (Nyquist half-pixel correlation floor) -> 2*pi*(0.5)^2 = pi/2 (~163/cell)
	inline double EffectiveSamplesForBand(int band, double sampleCountPx)
	{
		if (!std::isfinite(sampleCountPx) || sampleCountPx <= 0.0)
			return 1.0;
		const double sigma = (band == 0)
			? VpUnit::BandFromBase::kSigmaBase
			: ((band == 1) ? VpUnit::BandFromBase::kSigmaFine : 0.5);
		const double corrArea = 2.0 * kDctPi * sigma * sigma;
		const double nEff = sampleCountPx / corrArea;
		return (std::isfinite(nEff) && nEff > 1.0) ? nEff : 1.0;
	}

	// Wishart sampling variance of the sample cross-moment c_raw = (1/N) sum(d_p * d_n):
	//   se^2 = (C_pp * C_nn + C_pn^2) / N_eff  (TD-U4 / Plan Phase 2c).
	inline double SamplingVarianceOfCrossMoment(double cPP, double cNN, double cPN, double nEff)
	{
		if (!std::isfinite(cPP) || !std::isfinite(cNN) || !std::isfinite(cPN) || !std::isfinite(nEff))
			return 0.0;
		const double effN = nEff > 1.0 ? nEff : 1.0;
		const double pp = cPP > 0.0 ? cPP : 0.0;
		const double nn = cNN > 0.0 ? cNN : 0.0;
		const double v = (pp * nn + cPN * cPN) / effN;
		return (std::isfinite(v) && v > 0.0) ? v : 0.0;
	}

	// Empirical-Bayes James-Stein / normal-prior shrinkage of grain-corrected c_raw toward frame prior U_j(class):
	//   B = tau^2 / (tau^2 + se^2),   c_hat = clip(U + B * (c_raw - U), 0, min(C_pp, C_nn)).
	// Grain correction is ALWAYS applied BEFORE calling this; exact-copy cells stay strictly 0 outside this helper.
	inline double EmpiricalBayesShrinkC(
		double cRaw,
		double seSq,
		double priorU,
		double tauSq,
		double upperCap)
	{
		if (!std::isfinite(cRaw) || !std::isfinite(seSq) || !std::isfinite(priorU) ||
		    !std::isfinite(tauSq) || !std::isfinite(upperCap))
		{
			return 0.0;
		}
		const double cap = upperCap > 0.0 ? upperCap : 0.0;
		const double u = priorU > 0.0 ? priorU : 0.0;
		const double t2 = tauSq > 0.0 ? tauSq : 0.0;
		const double s2 = seSq > 0.0 ? seSq : 0.0;
		const double denom = t2 + s2;
		const double B = (denom > kSolveNumericalFloor) ? RampMath::Clamp01(t2 / denom) : 0.0;
		const double shrunk = u + B * (cRaw - u);
		if (!std::isfinite(shrunk) || shrunk <= 0.0)
			return 0.0;
		return shrunk < cap ? shrunk : cap;
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

	// Transform-unknown pixel-domain band bound (HEVC / AV1 over HDMI, TD-U3 / Owner D3, refs/111 s5):
	//   |Delta_j(p)| <= kappa_c * sqrt(U_j(block))
	// Always engages StandInKind::TransformUnknownBound visibly so the statistical limit is NEVER silent.
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

	// Joint cross-band transform-unknown limit (TD-U3 / H-1 / Owner D3):
	//   |sum_j Delta_j(p)| <= kappa_c * sqrt(sum_j U_j(block))
	// Prevents the three bands (L, M, H) from each independently spending the full allowance.
	inline double ApplyJointCrossBandBound(
		double sumDelta,
		double sumBandVar,
		StandInRegistry* standIns,
		double kappaC = kPixelBoundSigma)
	{
		if (standIns != nullptr)
			standIns->Engage(StandInKind::TransformUnknownBound);
		if (!std::isfinite(sumDelta) || !std::isfinite(sumBandVar) || !std::isfinite(kappaC) ||
		    sumBandVar < 0.0 || kappaC <= 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "ApplyJointCrossBandBound non-finite");
			return 0.0;
		}
		const double bound = kappaC * std::sqrt(sumBandVar);
		return (std::max)(-bound, (std::min)(bound, sumDelta));
	}

	// Resolve upstream U7 gradual fadeKeep in [0, 1] (Rule 1 + Rule 4):
	// Non-finite fails closed to 0.0 (sets outNonFinite = true); finite values clamp to [0.0, 1.0].
	inline double ResolveFadeKeep(double rawFadeKeep, bool& outNonFinite)
	{
		outNonFinite = false;
		if (!std::isfinite(rawFadeKeep))
		{
			outNonFinite = true;
			return 0.0;
		}
		return RampMath::Clamp01(rawFadeKeep);
	}

	// P7 3x3 projective camera homography mapping current luma pixel (x, y) -> reference luma pixel (sx, sy):
	//   w  = h[6]*x + h[7]*y + h[8]
	//   sx = (h[0]*x + h[1]*y + h[2]) / w
	//   sy = (h[3]*x + h[4]*y + h[5]) / w
	// Applied strictly on RegionMotionClass::Camera (rigid background) cells when valid == true;
	// NEVER overrides moving foreground Object vectors. Default valid == false -> 100% bit-identical baseline.
	struct Homography3x3
	{
		double h[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
		bool valid = false;

		static Homography3x3 Identity(bool isValid = true)
		{
			Homography3x3 out;
			out.valid = isValid;
			return out;
		}

		// Single-resample composition H_cur_to_key = H_prev_to_key * H_cur_to_prev (normalized by h[8]).
		// Maps current-frame coordinates directly into keyframe-bank coordinates in ONE resample over 8-32+ frames.
		// Fails closed (valid = false) on any non-finite entry or vanishing projective scale h[8].
		static Homography3x3 Compose(const Homography3x3& hPrevToKey, const Homography3x3& hCurToPrev)
		{
			Homography3x3 out;
			out.valid = false;
			if (!hPrevToKey.valid || !hCurToPrev.valid || !hPrevToKey.AllFinite() || !hCurToPrev.AllFinite())
				return out;
			double m[9] = {};
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
				{
					m[r * 3 + c] = hPrevToKey.h[r * 3 + 0] * hCurToPrev.h[0 * 3 + c] +
					               hPrevToKey.h[r * 3 + 1] * hCurToPrev.h[1 * 3 + c] +
					               hPrevToKey.h[r * 3 + 2] * hCurToPrev.h[2 * 3 + c];
				}
			}
			const double s = m[8];
			if (!std::isfinite(s) || std::fabs(s) <= kSolveNumericalFloor)
				return out;
			const double invS = 1.0 / s;
			for (int i = 0; i < 9; ++i)
			{
				out.h[i] = m[i] * invS;
				if (!std::isfinite(out.h[i]))
					return Homography3x3{};
			}
			out.h[8] = 1.0;
			out.valid = true;
			return out;
		}

		bool AllFinite() const
		{
			for (int i = 0; i < 9; ++i)
				if (!std::isfinite(h[i]))
					return false;
			return true;
		}

		bool Project(double x, double y, double& outSx, double& outSy) const
		{
			if (!valid || !AllFinite())
				return false;
			const double w = h[6] * x + h[7] * y + h[8];
			if (!std::isfinite(w) || std::fabs(w) <= kSolveNumericalFloor)
				return false;
			const double sx = (h[0] * x + h[1] * y + h[2]) / w;
			const double sy = (h[3] * x + h[4] * y + h[5]) / w;
			if (!std::isfinite(sx) || !std::isfinite(sy))
				return false;
			outSx = sx;
			outSy = sy;
			return true;
		}

		// Exact affine-extremum check for zero-copy (!readbackCellDiagnostics) paths:
		// true iff valid && (!AllFinite() || w(x,y) vanishes/crosses zero anywhere inside [x0,x1)x[y0,y1)).
		bool HasDegenerateProjection(int x0, int y0, int x1, int y1) const
		{
			if (!valid)
				return false;
			if (!AllFinite())
				return true;
			if (x1 <= x0 || y1 <= y0)
				return false;
			const double xs[3] = { static_cast<double>(x0), 0.5 * static_cast<double>(x0 + x1 - 1), static_cast<double>(x1 - 1) };
			const double ys[3] = { static_cast<double>(y0), 0.5 * static_cast<double>(y0 + y1 - 1), static_cast<double>(y1 - 1) };
			double sx = 0.0, sy = 0.0;
			for (double y : ys)
				for (double x : xs)
					if (!Project(x, y, sx, sy))
						return true;
			const double w00 = h[6] * xs[0] + h[7] * ys[0] + h[8];
			const double w10 = h[6] * xs[2] + h[7] * ys[0] + h[8];
			const double w01 = h[6] * xs[0] + h[7] * ys[2] + h[8];
			const double w11 = h[6] * xs[2] + h[7] * ys[2] + h[8];
			const bool hasPos = (w00 > 0.0) || (w10 > 0.0) || (w01 > 0.0) || (w11 > 0.0);
			const bool hasNeg = (w00 < 0.0) || (w10 < 0.0) || (w01 < 0.0) || (w11 < 0.0);
			return hasPos && hasNeg;
		}
	};

	// ---- Stage A4-1: Continuous C^1 Hermite Smoothstep & Per-Region Descriptor Math (Rule 1) ----

	// Canonical C^1 cubic Hermite smoothstep delegating to shared U25 RampMath::Smoothstep (StepWhenEqual),
	// safely returning 0.0 on non-finite inputs and matching GLSL td_smoothstep_c1 1:1.
	inline double SmoothstepC1(double edge0, double edge1, double x)
	{
		if (!std::isfinite(edge0) || !std::isfinite(edge1) || !std::isfinite(x))
			return 0.0;
		const double span = edge1 - edge0;
		if (!(std::fabs(span) > kSolvePivotFloor))
			return (x >= edge1) ? 1.0 : 0.0;
		return RampMath::Smoothstep(edge0, edge1, x, RampMath::SmoothRule::StepWhenEqual);
	}

	// Clamp matched-focus bokeh tolerance to [0.0, kBokehMatchDeltaMax] (0.0 = OFF / disabled).
	inline double ClampBokehMatchDelta(double delta)
	{
		if (!std::isfinite(delta) || !(delta > 0.0))
			return 0.0;
		return delta > kBokehMatchDeltaMax ? kBokehMatchDeltaMax : delta;
	}

	// Per-cell continuous region descriptors derived from P11 (focus confidence), P12 (texture density),
	// kEdgeMap (edge strength), and P10g (grain level), with physics-derived internal fallbacks when absent.
	struct CellRegionDescriptors
	{
		double focusConf = 1.0;          // P11 in [0, 1]: 0 = defocused bokeh, 1 = crisp in-focus (or flat neutral)
		double textureDensity = 0.0;     // P12 in [0, 1]: 0 = flat/smooth, 1 = dense real texture
		double edgeStrength = 0.0;       // kEdgeMap in [0, 1]: 0 = smooth interior, 1 = strong structural edge
		double p10gShare = 0.0;          // P10g in [0, 1]: 0 = grain-free, 1 = strong film grain
		bool curIsOutOfFocus = false;    // True when current cell is optically defocused or explicit bokeh
		std::array<double, kMaxBlendRefs> focusDelta{}; // |focus_ref_k - focus_cur| in [0, 1] per reference
	};

	struct CellRegionInputs
	{
		double curFDebiased = 1.0;
		double curEnergy = 0.0;
		std::array<double, kMaxBlendRefs> refFDebiased = { 1.0, 1.0, 1.0, 1.0 };
		std::array<double, kMaxBlendRefs> refEnergy = {};
		int refCount = 0;
		double curMVar = 0.0;
		double curHVar = 0.0;
		double cellRetainedGrainVar = 0.0;
		double cellOwnGrainVar = 0.0;
		double cellStepScaleSq = 1.0;
		double effNoiseVar = 1.0 / 12.0;
		double baseQuantVar = 1.0 / 12.0;
		double fDefocus = 0.0;
		double fSharp = 1.0;
		double gainMid = 0.0;
		double gainFine = 0.0;
		double gainD3 = 0.0;
		double psdMid = 1.0;
		double psdFine = 1.0;
		bool isExplicitBokeh = false;
		bool opticalIsDefocused = false;
		// Optional external shared maps (< 0.0 means absent => derive internally from clean bands).
		double extCurFocus = -1.0;
		std::array<double, kMaxBlendRefs> extRefFocus = { -1.0, -1.0, -1.0, -1.0 };
		double extTextureDensity = -1.0;
		double extCellEdge = -1.0;
	};

	// Evaluate continuous [0, 1] per-cell region descriptors using exact +/- 3 dB (0.5x..2x) and 1-sigma..3-sigma
	// C^1 Hermite smoothsteps derived from BandFromBase noise transfer gains and quantization physics (Rule 1).
	inline CellRegionDescriptors EvaluateCellRegionDescriptors(const CellRegionInputs& in)
	{
		CellRegionDescriptors out;
		const double stepSq = (std::isfinite(in.cellStepScaleSq) && in.cellStepScaleSq > 0.0) ? in.cellStepScaleSq : 1.0;
		const double effNoise = (std::isfinite(in.effNoiseVar) && in.effNoiseVar > 0.0) ? in.effNoiseVar : in.baseQuantVar;
		const double gainMD3 = in.gainMid + in.gainD3;
		const double cellStructFloor = 2.0 * gainMD3 * effNoise * stepSq;

		// 1) P11 focus confidence in [0, 1]: 0 = defocused bokeh, 1 = sharp or flat neutral.
		const double rawCurF = SmoothstepC1(in.fDefocus, in.fSharp, in.curFDebiased);
		const bool hasExtCurF = std::isfinite(in.extCurFocus) && in.extCurFocus >= 0.0;
		if (hasExtCurF)
		{
			out.focusConf = RampMath::Clamp01(in.extCurFocus);
		}
		else if (in.isExplicitBokeh)
		{
			out.focusConf = 0.0;
		}
		else
		{
			const double eConf = SmoothstepC1(0.5 * cellStructFloor, 2.0 * cellStructFloor, in.curEnergy);
			out.focusConf = 1.0 - eConf * (1.0 - rawCurF);
		}
		// LOW-1: Authoritative external P11 (extCurFocus >= 0) is never overridden by clean-band stand-in opticalIsDefocused.
		out.curIsOutOfFocus = in.isExplicitBokeh || (!hasExtCurF && in.opticalIsDefocused) || (out.focusConf < 0.5);

		// 2) Per-reference |focus_ref_k - focus_cur| in [0, 1] focus space (for matched-focus bokeh tailoring).
		const int K = in.refCount < 0 ? 0 : (in.refCount > kMaxBlendRefs ? kMaxBlendRefs : in.refCount);
		for (int k = 0; k < K; ++k)
		{
			const bool hasExtRefF = std::isfinite(in.extRefFocus[k]) && in.extRefFocus[k] >= 0.0;
			if (hasExtCurF && hasExtRefF)
			{
				out.focusDelta[k] = std::fabs(RampMath::Clamp01(in.extRefFocus[k]) - RampMath::Clamp01(in.extCurFocus));
			}
			else
			{
				const double pairE = in.curEnergy > in.refEnergy[k] ? in.curEnergy : in.refEnergy[k];
				const double pairConf = SmoothstepC1(0.5 * cellStructFloor, 2.0 * cellStructFloor, pairE);
				const double fCur = hasExtCurF ? RampMath::Clamp01(in.extCurFocus) : rawCurF;
				const double fRef = hasExtRefF ? RampMath::Clamp01(in.extRefFocus[k])
				                               : SmoothstepC1(in.fDefocus, in.fSharp, in.refFDebiased[k]);
				out.focusDelta[k] = pairConf * std::fabs(fRef - fCur);
			}
		}

		// 3) P12 texture/detail density in [0, 1] (+/- 3 dB around 0-dB (M+H) quantization floor).
		if (std::isfinite(in.extTextureDensity) && in.extTextureDensity >= 0.0)
		{
			out.textureDensity = RampMath::Clamp01(in.extTextureDensity);
		}
		else
		{
			const double retG = (std::isfinite(in.cellRetainedGrainVar) && in.cellRetainedGrainVar > 0.0) ? in.cellRetainedGrainVar : 0.0;
			const double cleanM = (in.curMVar - retG * in.gainMid * in.psdMid) > 0.0 ? (in.curMVar - retG * in.gainMid * in.psdMid) : 0.0;
			const double cleanH = (in.curHVar - retG * in.gainFine * in.psdFine) > 0.0 ? (in.curHVar - retG * in.gainFine * in.psdFine) : 0.0;
			const double texFloor = 2.0 * (in.gainMid + in.gainFine) * effNoise * stepSq;
			out.textureDensity = SmoothstepC1(0.5 * texFloor, 2.0 * texFloor, cleanM + cleanH);
		}

		// 4) kEdgeMap cell edge strength in [0, 1] (1-sigma to 3-sigma (M+D3) step-edge amplitude above noise floor).
		if (std::isfinite(in.extCellEdge) && in.extCellEdge >= 0.0)
		{
			out.edgeStrength = RampMath::Clamp01(in.extCellEdge);
		}
		else
		{
			const double md3NoiseVar = gainMD3 * effNoise * stepSq;
			const double edgeFloor = std::sqrt(md3NoiseVar > kSolveNumericalFloor ? md3NoiseVar : kSolveNumericalFloor);
			const double edgeProxy = std::sqrt((in.curEnergy - md3NoiseVar) > 0.0 ? (in.curEnergy - md3NoiseVar) : 0.0);
			out.edgeStrength = SmoothstepC1(edgeFloor, std::sqrt(kHuberKneeSq) * edgeFloor, edgeProxy);
		}

		// 5) P10g grain share in [0, 1] (0.5x to 9x base 1-LSB quantization variance = 0.7x to 3-sigma RMS).
		const double ownG = (std::isfinite(in.cellOwnGrainVar) && in.cellOwnGrainVar > 0.0) ? in.cellOwnGrainVar : 0.0;
		const double retG = (std::isfinite(in.cellRetainedGrainVar) && in.cellRetainedGrainVar > 0.0) ? in.cellRetainedGrainVar : 0.0;
		out.p10gShare = SmoothstepC1(0.5 * in.baseQuantVar, kHuberKneeSq * in.baseQuantVar, ownG + retG);

		return out;
	}

	// ---- Pass 3C Layer 1: Coarse-Band (L) 2-vs-1 Agreement Voter (Rule 1, 1:1 CPU/GPU parity) ----
	// Compares carried/bank anchor coarse-band RMS difference `rmsCarryL` against local peak consensus
	// `rmsLocalL` (normalized by coarse-band noise/quantization floor `sigmaL`).
	// When local peak(s) agree with X_clean while carried anchor disagrees beyond 2-to-3 sigma above
	// 2 * rmsLocalL, smoothly drives g_agree -> 0.0 BEFORE solving w* = C^-1 c!
	inline double ComputeCoarse2vs1VoterGate(double rmsCarryL, double rmsLocalL, double sigmaL)
	{
		if (!std::isfinite(rmsCarryL) || !std::isfinite(rmsLocalL) || !std::isfinite(sigmaL))
			return 0.0;
		const double sig = (sigmaL > kSolveNumericalFloor) ? sigmaL : kSolveNumericalFloor;
		const double excess = rmsCarryL - 2.0 * (rmsLocalL > 0.0 ? rmsLocalL : 0.0);
		// Derived 2-sigma to 3-sigma tail (sqrt(kHuberKneeSq) = 3.0):
		// g_agree = 1.0 when rmsCarryL <= 2*rmsLocalL + 2*sig; 0.0 when rmsCarryL >= 2*rmsLocalL + 3*sig.
		const double hiSigma = std::sqrt(kHuberKneeSq);
		return 1.0 - SmoothstepC1(2.0 * sig, hiSigma * sig, excess);
	}

	// ---- Pass 3C Layer 3: Per-Pixel Grain-Free 3x3 Salvi TAA Neighborhood Envelope Clamp (Rule 1) ----
	// Intersects the 3x3 min/max AABB and 3-sigma variance interval around X_clean, admitting quantization-cell
	// slack `qFloor = sqrt(max(baseQuantVar * bandGain, cellOwnU))` so legitimate quantization recovery passes
	// while rogue ghost edges outside the local 3x3 envelope are physically impossible.
	inline double ClampToSalvi3x3Envelope(
		double refV,
		double min3x3,
		double max3x3,
		double mu3x3,
		double sig3x3,
		double qFloor)
	{
		if (!std::isfinite(refV) || !std::isfinite(min3x3) || !std::isfinite(max3x3) ||
		    !std::isfinite(mu3x3) || !std::isfinite(sig3x3) || !std::isfinite(qFloor))
		{
			return std::isfinite(mu3x3) ? mu3x3 : 0.0;
		}
		const double q = qFloor > 0.0 ? qFloor : 0.0;
		const double gamma = std::sqrt(kHuberKneeSq); // Exact 3.0-sigma Gaussian tail
		const double boxLo = min3x3 - q;
		const double boxHi = max3x3 + q;
		const double varLo = mu3x3 - gamma * (sig3x3 > 0.0 ? sig3x3 : 0.0) - q;
		const double varHi = mu3x3 + gamma * (sig3x3 > 0.0 ? sig3x3 : 0.0) + q;
		double lo = (boxLo > varLo) ? boxLo : varLo;
		double hi = (boxHi < varHi) ? boxHi : varHi;
		if (lo > hi)
		{
			lo = boxLo;
			hi = boxHi;
		}
		if (refV < lo) return lo;
		if (refV > hi) return hi;
		return refV;
	}
}
