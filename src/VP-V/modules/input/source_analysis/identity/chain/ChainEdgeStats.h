// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainEdgeStats (SID Stage S1, EXP-8BIT MUST-1 / MUST-2 / SHOULD-1 / SHOULD-2 / SHOULD-3 + Pitfall #1):
// Pure C++14 8-bit and L4-hardened residual, floor, detail, edge-proposal, and k/8 comb discrimination helpers.
//
// Motivation (2026-11-20 8-bit audit & 2026-11-23 SourceID plan FM-1 / FM-4 / FM-7):
//   - On 8-bit & TPDF-dithered content, whole-line RMS averages 70-85% flat pixels (carrying 16x higher rounding
//     variance than 10-bit) with 15-30% sharp edges, swamping upscale dips and compressing ln(s / base).
//   - Hardcoded binary step detection ((v[i] & 3) == 0) misclassifies re-quantized 8-bit-in-10-bit HDMI streams.
//   - Unconditionally placing 1/2 = 4/8 in the harsh K8 codec-grid null class double-penalizes isolated 1080p->4K
//     notches even when the 3/8, 5/8, 7/8 block-comb harmonics are absent.
//
// Rule 1 (Every constant derived from physics, quantization, geometry, or existing contract constants):
//   - Uniform quantization floor: sigma_q(step) = step / sqrt(12)  (variance = step^2 / 12).
//   - TPDF dither floor:          sigma_tpdf(step) = sqrt(2) * step / sqrt(12) = step / sqrt(6)
//                                 (sum of two independent uniform [-step/2, step/2] variates).
//   - Second-difference variance: d_i = y_{i-1} - 2*y_i + y_{i+1} annihilates DC and linear slope; for i.i.d. white
//                                 floor noise, Var(d_i) = (1^2 + (-2)^2 + 1^2) * sigma^2 = 6 * sigma^2,
//                                 so sigma_hat = (RobustStats::kMadToSigma * MAD(d)) / sqrt(6).
//   - Huber delta = 3 * floorCodes: 3-sigma Gaussian tail (99.73% of inlier floor residuals sit in the quadratic
//                                 region; sharp sharpening/impulse outliers grow linearly, never quadratically).
//   - Gradient edge gate:         for white floor noise, SD(y_i - y_{i-1}) = SD(y_{i+1} - y_i) = sqrt(2) * floorCodes.
//                                 Using g_i = max(|y_i - y_{i-1}|, |y_{i+1} - y_i|) detects both slopes and local
//                                 extrema (peaks/valleys where central difference y_{i+1}-y_{i-1} cancels to 0) while
//                                 keeping the exact sqrt(2)*floorCodes noise scale (3-sigma gate = 3 * sqrt(2) * floor).
//   - Excess-variance clamp:      eps = floorCodes / 4  (1/4 floor prevents ln(0) = -inf on white noise while keeping
//                                 ~2.77 nats of dynamic range below sigma_q; power clamp = floorPower / 16 = (1/4)^2).
//   - Detail / dip gates:         lineGateFactor = 1.5 (ChainEvidence.h:86), trimFraction = 0.2 (ChainEvidence.h:97),
//                                 majorityDip = -0.03 (ChainEvidence.h:91), minGateCount = 3 (ChainEvidence.h:88),
//                                 kSegPx = 256 (ChainCommon.h:27).
//
// Pure C++14, header-only, deterministic, no clock, no threads, no Windows headers. Default OFF (enabled = false).
// Uses RobustStats (core/RobustStats.h) for all median / MAD / quantile / trimmed-mean operations.
// Zero silent fallbacks: every degraded, clamped, abstained, or fallback path carries an explicit EdgeStatsReason.
#include "ChainCommon.h"
#include "ChainNullTables.h"
#include "DescaleVerifier.h"

#include "../../../../../core/RobustStats.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		enum class EdgeStatsReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			TheoreticalFloorFallback,    // All segments were exact-zero/clipped; returned theoretical step / sqrt(12)
			InsufficientDetailedSegments,// Fewer than minInformativeSegments passed lineGateFactor * floor
			InsufficientEdgePixels,      // Fewer than minEdgePixels passed 3-sigma gradient gate
			BaselineBelowFloor,          // Baseline variance does not exceed floor variance + eps^2
			ClampedToFloorGuard,         // Numerator and/or denominator clamped to eps^2 = (floor/4)^2
			AboveBandAtDitherFloor,      // SHOULD-2: above-band power <= lineGateFactor^2 * floorPower (dither floor, not real detail)
			IsolatedHalfNotch,           // Pitfall #1: 1/2 dips with quiet 3/8, 5/8, 7/8 comb => kClassRational
			CombGridDetected,            // Pitfall #1: 8x8 codec-grid comb dips at odd k/8 => stays kClassK8
			CombProbesUnavailable,       // Pitfall #1: missing comb probes => fail safe to kClassK8
			NoHalfNotch,                 // Pitfall #1 (MEDIUM-5): flat native 4K (no 1/2 dip and no odd k/8 comb) => stays kClassK8
			ZeroPhaseEnergy,             // LOW-4: zero second-difference energy across all phases (constant line)
			NotHalfCandidate             // Candidate is not near N/2 => standard ClassifyLatent
		};

		inline const char* EdgeStatsReasonText(EdgeStatsReason r)
		{
			switch (r)
			{
			case EdgeStatsReason::Ok: return "ok";
			case EdgeStatsReason::Disabled: return "edge stats disabled";
			case EdgeStatsReason::InvalidInput: return "invalid input";
			case EdgeStatsReason::TheoreticalFloorFallback: return "empirical floor fallback to theoretical step/sqrt(12)";
			case EdgeStatsReason::InsufficientDetailedSegments: return "abstain (insufficient detailed segments)";
			case EdgeStatsReason::InsufficientEdgePixels: return "abstain (insufficient edge pixels)";
			case EdgeStatsReason::BaselineBelowFloor: return "abstain (baseline at or below floor)";
			case EdgeStatsReason::ClampedToFloorGuard: return "clamped to eps = floor/4 guard";
			case EdgeStatsReason::AboveBandAtDitherFloor: return "above-band power at dither floor (no real detail above)";
			case EdgeStatsReason::IsolatedHalfNotch: return "isolated 1/2 notch (odd k/8 comb quiet)";
			case EdgeStatsReason::CombGridDetected: return "8x8 codec-grid comb detected (kept in K8)";
			case EdgeStatsReason::CombProbesUnavailable: return "comb probes unavailable (fail safe to K8)";
			case EdgeStatsReason::NoHalfNotch: return "no significant 1/2 dip (kept in K8)";
			case EdgeStatsReason::ZeroPhaseEnergy: return "abstain (zero second-difference energy across all phases)";
			case EdgeStatsReason::NotHalfCandidate: return "not a 1/2 candidate (standard class)";
			default: return "unknown";
			}
		}

		struct EdgeStatsSettings
		{
			bool enabled = false;  // OFF by default (Stage S1 safe contract; promoted only after G-8bit)

			// Derived from ChainEvidence.h contract constants:
			double lineGateFactor = 1.5;   // ChainEvidence.h:86: detail >= 1.5 * floorCodes (power >= 2.25 * floorPower)
			double trimFraction = 0.2;     // ChainEvidence.h:97 / DescaleVerifier.h:87: 20% trimmed mean
			double combDipThreshold = -0.03; // ChainEvidence.h:91: majorityDip (-0.03 ln units)
			int minInformativeSegments = 3;  // ChainEvidence.h:88: minGateLinesFloor = 3
			int minEdgePixels = 3;           // Minimal sample count for variance/MAD stability (ChainEvidence.h:88)
			int segPx = kSegPx;              // ChainCommon.h:27: 256-px segment width

			// Derived from normal 3-sigma tail (99.73% inlier coverage):
			// Huber transition delta = huberSigma * floorCodes; gradient mask gate = gradientGateSigma * floorCodes.
			double huberSigma = 3.0;
			double gradientGateSigma = 3.0;

			// Derived from dynamic-range / singularity trade-off (Plan s5.2):
			// eps = floorCodes / floorGuardDivisor (= floorCodes / 4), giving 2.77 nats dynamic range below sigma_q.
			double floorGuardDivisor = 4.0;
			// Lower quartile (1/4 of quietest non-zero segments) isolates quiet background from textured edges.
			double quietSegmentQuantile = 1.0 / 4.0;
			// Pitfall #1: tolerance in pixels matching ChainNullTables::ClassifyLatent (tol = 2.0 px),
			// and elementary 2:1 dominance factor when a single odd comb probe dips weakly.
			double halfPixelTol = 2.0;
			double combDominanceFactor = 2.0;
			// Whether segment-gated T_ev applies excess-variance dip subtraction (MUST-2) vs plain log dip.
			bool useExcessVarianceDip = true;
		};

		// ============================================================================================================
		// 1. Theoretical & Empirical Quantization / Dither Floor (SHOULD-1 / PKG-SRCID-FLOOR)
		// ============================================================================================================

		// Uniform rounding standard deviation in 10-bit code units: step / sqrt(12).
		// (10-bit step = 1 => 0.288675; 8-bit step = 4 => 1.154701.)
		inline double QuantizationFloorCodes(double stepCodes)
		{
			if (!(stepCodes > 0.0) || !std::isfinite(stepCodes))
				return 0.0;
			return stepCodes / std::sqrt(12.0);
		}

		// Triangular-PDF dither standard deviation (sum of two independent uniform [-step/2, step/2] variates):
		// sqrt(2) * step / sqrt(12) = step / sqrt(6). (8-bit TPDF step = 4 => 1.632993.)
		inline double TpdfDitherFloorCodes(double stepCodes)
		{
			if (!(stepCodes > 0.0) || !std::isfinite(stepCodes))
				return 0.0;
			return stepCodes / std::sqrt(6.0);
		}

		struct EmpiricalFloorResult
		{
			bool valid = false;
			double floorCodes = 0.0;         // max(empiricalQuietSigma, theoreticalFloor)
			double empiricalRawCodes = 0.0;  // measured lower-quartile quiet sigma before theoretical floor clamp
			double theoreticalCodes = 0.0;   // stepCodes / sqrt(12)
			int totalSegments = 0;
			int positiveSegments = 0;
			int zeroSegmentsSkipped = 0;
			bool usedTheoreticalFallback = false;
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Measures the empirical high-frequency floor from sampled lines using second differences
		// d_i = y_{i-1} - 2*y_i + y_{i+1} over segPx-pixel segments:
		//   sigma_seg = (RobustStats::kMadToSigma * RobustStats::Mad(d)) / sqrt(6).
		// Works uniformly for 10-bit (0.289), 8-bit plain (1.155), 8-bit TPDF (1.633), and re-quantized
		// 8-bit-in-10-bit HDMI links where stepCodes == 1 but true floor is ~1.3 codes.
		inline EmpiricalFloorResult EstimateEmpiricalFloor(
			const LineSet& lines,
			double stepCodes,
			const EdgeStatsSettings& s = EdgeStatsSettings())
		{
			EmpiricalFloorResult res;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			if (!std::isfinite(stepCodes) || !(stepCodes > 0.0) || lines.count <= 0 || lines.length < 5)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			// MEDIUM-1: verify backing vector holds all count * length samples before indexing lines.Line(li).
			if (lines.data.size() / static_cast<size_t>(lines.length) < static_cast<size_t>(lines.count))
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			const double theo = QuantizationFloorCodes(stepCodes);
			res.theoreticalCodes = theo;

			const int segLen = s.segPx >= 8 ? s.segPx : kSegPx;
			const double sqrt6 = std::sqrt(6.0);
			std::vector<double> segSigmas;
			std::vector<double> secondDiffs;
			secondDiffs.reserve(static_cast<size_t>(segLen));

			for (int li = 0; li < lines.count; ++li)
			{
				const double* y = lines.Line(li);
				for (int start = 0; start < lines.length; start += segLen)
				{
					int end = start + segLen;
					if (end > lines.length)
						end = lines.length;
					// Merge short tail into previous segment only if fewer than 5 samples; skip tiny orphan tail.
					if (end - start < 5)
						continue;

					++res.totalSegments;
					secondDiffs.clear();
					bool finiteOk = true;
					for (int i = start + 1; i + 1 < end; ++i)
					{
						const double d = y[i - 1] - 2.0 * y[i] + y[i + 1];
						if (!std::isfinite(d))
						{
							finiteOk = false;
							break;
						}
						secondDiffs.push_back(d);
					}
					if (!finiteOk || secondDiffs.size() < 3)
						continue;

					const double mad = RobustStats::Mad(secondDiffs, RobustStats::MedianRule::MeanOfMiddle, 0.0);
					if (!(mad > 0.0))
					{
						++res.zeroSegmentsSkipped;
						continue;
					}
					const double sig = (RobustStats::kMadToSigma * mad) / sqrt6;
					segSigmas.push_back(sig);
				}
			}

			res.positiveSegments = static_cast<int>(segSigmas.size());
			if (segSigmas.empty())
			{
				// All segments exact-constant / clipped: visibly fall back to theoretical step / sqrt(12) (Rule 4).
				res.valid = true;
				res.floorCodes = theo;
				res.empiricalRawCodes = 0.0;
				res.usedTheoreticalFallback = true;
				res.reason = EdgeStatsReason::TheoreticalFloorFallback;
				return res;
			}

			// LOW-8: guard against NaN in quietSegmentQuantile before clamping.
			const double q = std::isfinite(s.quietSegmentQuantile) ? ClampD(s.quietSegmentQuantile, 0.0, 0.5) : (1.0 / 4.0);
			const double quietSig = RobustStats::Quantile(segSigmas, q, RobustStats::RankRule::Interpolated, theo);
			res.valid = true;
			res.empiricalRawCodes = quietSig;
			if (quietSig < theo)
			{
				res.floorCodes = theo;
				res.usedTheoreticalFallback = true;
				res.reason = EdgeStatsReason::TheoreticalFloorFallback;
			}
			else
			{
				res.floorCodes = quietSig;
				res.usedTheoreticalFallback = false;
				res.reason = EdgeStatsReason::Ok;
			}
			return res;
		}

		// ============================================================================================================
		// 2. Excess-Variance Dip (EXP-8BIT MUST-2)
		// ============================================================================================================

		struct ExcessDipResult
		{
			bool valid = false;
			double dip = 0.0;              // 0.5 * ln(max(s^2 - floor^2, eps^2) / max(base^2 - floor^2, eps^2))
			bool clampedNumerator = false;
			bool clampedDenominator = false;
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Subtracts floor variance sigma_q^2 from both candidate residual s^2 and baseline b^2 before taking the
		// log ratio, guarded by eps = floor / floorGuardDivisor (floor / 4) so white noise never blows up to -inf/NaN.
		inline ExcessDipResult ComputeExcessVarianceDip(
			double residualCodes,
			double baselineCodes,
			double floorCodes,
			const EdgeStatsSettings& s = EdgeStatsSettings())
		{
			ExcessDipResult res;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			if (!std::isfinite(residualCodes) || !std::isfinite(baselineCodes) || !std::isfinite(floorCodes) ||
				residualCodes < 0.0 || baselineCodes < 0.0 || floorCodes < 0.0)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			const double div = (std::isfinite(s.floorGuardDivisor) && s.floorGuardDivisor > 1.0) ? s.floorGuardDivisor : 4.0;
			// Strictly numerical minimum guard (1e-12) when caller passes floorCodes == 0.
			const double eps = (std::max)(floorCodes / div, 1e-6);
			const double eps2 = eps * eps;
			const double floor2 = floorCodes * floorCodes;

			const double rawNum = residualCodes * residualCodes - floor2;
			const double rawDen = baselineCodes * baselineCodes - floor2;
			if (!std::isfinite(rawNum) || !std::isfinite(rawDen) || !std::isfinite(eps2))
			{
				// LOW-2: overflow to inf/NaN must fail cleanly with InvalidInput, never Ok.
				res.valid = false;
				res.dip = 0.0;
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}

			double num = rawNum;
			double den = rawDen;
			if (num <= eps2)
			{
				num = eps2;
				res.clampedNumerator = true;
			}
			if (den <= eps2)
			{
				den = eps2;
				res.clampedDenominator = true;
			}

			res.dip = 0.5 * std::log(num / den);
			res.valid = std::isfinite(res.dip);
			if (!res.valid)
			{
				res.dip = 0.0;
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			if (res.clampedDenominator)
				res.reason = EdgeStatsReason::BaselineBelowFloor;
			else if (res.clampedNumerator)
				res.reason = EdgeStatsReason::ClampedToFloorGuard;
			else
				res.reason = EdgeStatsReason::Ok;
			return res;
		}

		// ============================================================================================================
		// 3. Segment-Gated T_ev from CandidateResult::segSum / segRows / segBase (EXP-8BIT MUST-1a)
		// ============================================================================================================

		struct SegmentGatedResult
		{
			bool valid = false;
			double stat = 0.0;             // Trimmed mean over informative segments (ln units)
			int totalSegments = 0;
			int informativeSegments = 0;
			int flatSegmentsSkipped = 0;
			int clampedSegments = 0;
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Computes segment-gated T_ev using DescaleVerifier's already-computed per-256-px segSum / segRows / segBase.
		// Skips flat segments whose baseline < lineGateFactor * floorCodes so 70-85% flat pixels cannot drown edges.
		inline SegmentGatedResult ComputeSegmentGatedStat(
			const CandidateResult& cand,
			double dofRatio,
			double floorCodes,
			const EdgeStatsSettings& s = EdgeStatsSettings(),
			std::vector<double>* outSegmentDips = nullptr)
		{
			SegmentGatedResult res;
			if (outSegmentDips != nullptr)
				outSegmentDips->clear();
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			if (!cand.valid || cand.lineCount <= 0 || cand.segCount <= 0 ||
				!std::isfinite(dofRatio) || !(dofRatio >= 0.0) || !(dofRatio < 1.0) ||
				!std::isfinite(floorCodes) || floorCodes < 0.0)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			const size_t expected = static_cast<size_t>(cand.lineCount) * static_cast<size_t>(cand.segCount);
			if (cand.segSum.size() < expected || cand.segRows.size() < expected || cand.segBase.size() < expected)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}

			const double dofDenom = 1.0 - dofRatio;
			const double gateCodes = s.lineGateFactor * floorCodes;
			const double eps = (std::max)(floorCodes / (s.floorGuardDivisor > 1.0 ? s.floorGuardDivisor : 4.0), 1e-6);

			std::vector<double> dips;
			dips.reserve(expected);

			for (size_t idx = 0; idx < expected; ++idx)
			{
				const int rows = cand.segRows[idx];
				if (rows <= 0)
					continue;
				++res.totalSegments;
				const double base = cand.segBase[idx];
				const double rss = cand.segSum[idx];
				if (!std::isfinite(base) || !std::isfinite(rss) || rss < 0.0 || base < gateCodes || !(base > 0.0))
				{
					++res.flatSegmentsSkipped;
					continue;
				}
				const double segRms = std::sqrt(rss / (static_cast<double>(rows) * dofDenom));
				double dip = 0.0;
				if (s.useExcessVarianceDip)
				{
					const ExcessDipResult ed = ComputeExcessVarianceDip(segRms, base, floorCodes, s);
					if (!ed.valid)
					{
						++res.flatSegmentsSkipped;
						continue;
					}
					if (ed.clampedNumerator || ed.clampedDenominator)
						++res.clampedSegments;
					dip = ed.dip;
				}
				else
				{
					const double safeRms = (std::max)(segRms, eps);
					const double safeBase = (std::max)(base, eps);
					dip = std::log(safeRms / safeBase);
				}
				dips.push_back(dip);
			}

			res.informativeSegments = static_cast<int>(dips.size());
			const int minSegs = s.minInformativeSegments > 1 ? s.minInformativeSegments : 1;
			if (res.informativeSegments < minSegs)
			{
				res.valid = false;
				res.reason = EdgeStatsReason::InsufficientDetailedSegments;
				return res;
			}

			const double trimSafe = std::isfinite(s.trimFraction) ? ClampD(s.trimFraction, 0.0, 0.5) : 0.2;
			res.stat = RobustStats::TrimmedMeanOf(dips.data(), res.informativeSegments, trimSafe);
			res.valid = std::isfinite(res.stat);
			if (res.valid && outSegmentDips != nullptr)
				*outSegmentDips = dips;
			res.reason = res.valid ? EdgeStatsReason::Ok : EdgeStatsReason::InvalidInput;
			return res;
		}

		// ============================================================================================================
		// 4. Pixel-Level Gradient-Masked + Huber-Clipped Residual RMS (EXP-8BIT MUST-1b)
		// ============================================================================================================

		struct MaskedHuberResult
		{
			bool valid = false;
			double huberRms = 0.0;        // Scale-consistent Huber RMS (equals standard RMS when all |r| <= delta), DOF-normalized
			double standardMaskedRms = 0.0;
			int totalInteriorPixels = 0;
			int edgePixels = 0;
			int huberClippedPixels = 0;
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Standard Huber rho_delta(r): 0.5*r^2 for |r| <= delta, delta*(|r| - 0.5*delta) for |r| > delta.
		inline double HuberRho(double r, double delta)
		{
			const double a = std::fabs(r);
			if (a <= delta)
				return 0.5 * a * a;
			return delta * (a - 0.5 * delta);
		}

		// Computes scale-consistent Huber RMS of residuals restricted to pixels with significant local gradient:
		//   max(|y[i] - y[i-1]|, |y[i+1] - y[i]|) / sqrt(2) >= gradientGateSigma * floorCodes  (3-sigma normal tail gate)
		// and Huber threshold delta = huberSigma * floorCodes (3 * floorCodes).
		// Because 2 * rho_delta(r) == r^2 for |r| <= delta, huberRms equals exact RMS on clean inliers while
		// bounding sharpening/ringing/impulse outliers linearly instead of quadratically.
		inline MaskedHuberResult ComputeGradientMaskedHuberRms(
			const double* line,
			const double* residual,
			int length,
			double floorCodes,
			double dofRatio = 0.0,
			const EdgeStatsSettings& s = EdgeStatsSettings())
		{
			MaskedHuberResult res;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			if (!line || !residual || length < 5 ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(dofRatio) || !(dofRatio >= 0.0) || !(dofRatio < 1.0))
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}

			const double gradGate = s.gradientGateSigma * std::sqrt(2.0) * floorCodes;
			const double delta = (std::max)(s.huberSigma * floorCodes, 1e-6);
			double sumTwoRho = 0.0;
			double sumSq = 0.0;

			for (int i = 1; i + 1 < length; ++i)
			{
				++res.totalInteriorPixels;
				const double gL = std::fabs(line[i] - line[i - 1]);
				const double gR = std::fabs(line[i + 1] - line[i]);
				// LOW-3: require BOTH one-step differences finite before (std::max) so NaN in gR is never masked.
				if (!std::isfinite(gL) || !std::isfinite(gR))
					continue;
				const double g = (std::max)(gL, gR);
				if (g < gradGate)
					continue;
				const double r = residual[i];
				if (!std::isfinite(r))
					continue;
				++res.edgePixels;
				if (std::fabs(r) > delta)
					++res.huberClippedPixels;
				sumTwoRho += 2.0 * HuberRho(r, delta);
				sumSq += r * r;
			}

			const int minPx = s.minEdgePixels > 1 ? s.minEdgePixels : 1;
			if (res.edgePixels < minPx)
			{
				res.valid = false;
				res.reason = EdgeStatsReason::InsufficientEdgePixels;
				return res;
			}

			const double denom = static_cast<double>(res.edgePixels) * (1.0 - dofRatio);
			res.huberRms = std::sqrt(sumTwoRho / denom);
			res.standardMaskedRms = std::sqrt(sumSq / denom);
			res.valid = std::isfinite(res.huberRms);
			res.reason = res.valid ? EdgeStatsReason::Ok : EdgeStatsReason::InvalidInput;
			return res;
		}

		// ============================================================================================================
		// 5. Dither-Floor-Aware DetailAbove Helper (EXP-8BIT SHOULD-2)
		// ============================================================================================================

		struct DitherDetailAboveResult
		{
			bool valid = false;
			bool aboveIsNoiseFloor = false;      // true when above-band power <= lineGateFactor^2 * floorPower
			bool hasRealDetailAbove = false;     // true iff above-band power exceeds lineGateFactor^2 * floorPower (and optional ref + margin)
			double rawDecades = 0.0;             // log10(abovePower / belowPower)
			double floorSubtractedDecades = 0.0; // log10(max(above - floor, epsP) / max(below - floor, epsP))
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Evaluates whether above-band FFT power reflects genuine high-frequency picture detail or merely white
		// 8-bit quantization / TPDF dither floor power.
		// Gate derivation:
		//   - ChainEvidence lineGateFactor = 1.5 in amplitude => 1.5^2 = 2.25 in power. If abovePower <= 2.25 * floorPower,
		//     the above band sits in the dither/quantization floor (aboveIsNoiseFloor = true, hasRealDetailAbove = false).
		//   - Power floor clamp: (floorCodes / 4)^2 = floorPower / 16.
		//   - Optional reference comparison (when refDetailDecades is finite): matches SourceResolutionDetector.h:411
		//     (detailMarginDecades = 0.07): requires floorSubtractedDecades >= refDetailDecades + detailMarginDecades.
		inline DitherDetailAboveResult ComputeDitherAwareDetailAbove(
			double belowPower,
			double abovePower,
			double floorPowerPerBin,
			const EdgeStatsSettings& s = EdgeStatsSettings(),
			double refDetailDecades = -1e300,
			double detailMarginDecades = 0.07)
		{
			DitherDetailAboveResult res;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			if (!std::isfinite(belowPower) || !std::isfinite(abovePower) || !std::isfinite(floorPowerPerBin) ||
				!(belowPower > 0.0) || !(abovePower > 0.0) || floorPowerPerBin < 0.0 ||
				std::isnan(refDetailDecades) || !std::isfinite(detailMarginDecades))
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}

			res.rawDecades = std::log10(abovePower / belowPower);
			const double powerGateMult = s.lineGateFactor * s.lineGateFactor; // 1.5^2 = 2.25
			const double div = s.floorGuardDivisor > 1.0 ? s.floorGuardDivisor : 4.0;
			const double epsP = (std::max)(floorPowerPerBin / (div * div), 1e-20);

			if (floorPowerPerBin > 0.0 && abovePower <= powerGateMult * floorPowerPerBin)
			{
				res.valid = true;
				res.aboveIsNoiseFloor = true;
				res.hasRealDetailAbove = false;
				const double exBelow = (std::max)(belowPower - floorPowerPerBin, epsP);
				res.floorSubtractedDecades = std::log10(epsP / exBelow);
				res.reason = EdgeStatsReason::AboveBandAtDitherFloor;
				return res;
			}

			const double exAbove = (std::max)(abovePower - floorPowerPerBin, epsP);
			const double exBelow = (std::max)(belowPower - floorPowerPerBin, epsP);
			res.floorSubtractedDecades = std::log10(exAbove / exBelow);
			res.valid = std::isfinite(res.floorSubtractedDecades);
			res.aboveIsNoiseFloor = false;
			if (res.valid)
			{
				if (refDetailDecades > -1e299)
					res.hasRealDetailAbove = res.floorSubtractedDecades >= refDetailDecades + detailMarginDecades;
				else
					res.hasRealDetailAbove = true;
			}
			res.reason = res.valid ? EdgeStatsReason::Ok : EdgeStatsReason::InvalidInput;
			return res;
		}

		// Spectrum-bin overload matching SourceResolutionDetector::DetailAboveSpectra band limits
		// ([0.55*cut, 0.9*cut] below, [1.15*cut, min(0.97*half, a0 + max(4, b1-b0))] above, excluding 8-px block harmonics).
		inline DitherDetailAboveResult ComputeDitherAwareDetailAboveSpectrum(
			const std::vector<double>& powerBins,
			double ratio,
			double floorPowerPerBin,
			const EdgeStatsSettings& s = EdgeStatsSettings(),
			double refDetailDecades = -1e300,
			double detailMarginDecades = 0.07)
		{
			DitherDetailAboveResult res;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			const size_t bins = powerBins.size();
			// HIGH-4 + LOW-8: require finite ratio in (0, 1) BEFORE any cast/indexing!
			if (!std::isfinite(ratio) || !(ratio > 0.0) || !(ratio < 1.0) || bins < 33 ||
				!std::isfinite(floorPowerPerBin) || floorPowerPerBin < 0.0 ||
				std::isnan(refDetailDecades) || !std::isfinite(detailMarginDecades))
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			const int half = static_cast<int>(bins) - 1;
			const int n = 2 * half;
			const double cut = ratio * static_cast<double>(half);
			// Band bounds verbatim from SourceResolutionDetector.cpp:829-830 (existing contract).
			const int b0 = static_cast<int>(std::ceil(0.55 * cut));
			const int b1 = static_cast<int>(std::floor(0.9 * cut));
			const int a0 = static_cast<int>(std::ceil(1.15 * cut));
			const int a1 = (std::min)(static_cast<int>(std::floor(0.97 * static_cast<double>(half))), a0 + (std::max)(4, b1 - b0));
			if (a1 - a0 < 3 || b1 - b0 < 3 || b0 < 1 || a1 > half)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}

			// LOW-1: verbatim from SourceResolutionDetector.cpp:54-60 (BlockHarmonic):
			// excludes multiples of 1/8 and 1/16 cycles/pixel AND +-1 Hann window side-lobes.
			auto isBlockHarm = [&](int k) {
				const int period = n / 16;
				if (period < 2)
					return false;
				const int r = k % period;
				return r == 0 || r == 1 || r == period - 1;
			};

			auto meanLinearBand = [&](int from, int to, double& outMean) {
				double sum = 0.0;
				int cnt = 0;
				for (int k = from; k <= to; ++k)
				{
					if (isBlockHarm(k))
						continue;
					const double p = powerBins[static_cast<size_t>(k)];
					if (!std::isfinite(p) || p < 0.0)
						return false;
					sum += (std::max)(p, 1e-20);
					++cnt;
				}
				if (cnt <= 0)
					return false;
				outMean = sum / static_cast<double>(cnt);
				return true;
			};

			double pBelow = 0.0, pAbove = 0.0;
			if (!meanLinearBand(b0, b1, pBelow) || !meanLinearBand(a0, a1, pAbove))
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			return ComputeDitherAwareDetailAbove(pBelow, pAbove, floorPowerPerBin, s, refDetailDecades, detailMarginDecades);
		}

		// ============================================================================================================
		// 6. Edge-Gated Proposal Helper (EXP-8BIT SHOULD-3)
		// ============================================================================================================

		struct EdgeGateSummary
		{
			bool valid = false;
			int totalStencilSamples = 0;
			int activeEdgeSamples = 0;
			double activeFraction = 0.0;
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Builds a deterministic per-sample edge mask over the 5-tap predictor stencil [3, length - 3]
		// (matching PolyphaseProposer's k in [3, length - 3]): mask[k] = 1 iff the adjacent gradient
		// max(|y[k] - y[k-1]|, |y[k+1] - y[k]|) / sqrt(2) >= gradientGateSigma * floorCodes (symmetric across
		// both slopes and local peaks/valleys so all sub-sample phases remain balanced).
		inline EdgeGateSummary BuildEdgeGateMask(
			const double* line,
			int length,
			double floorCodes,
			std::vector<uint8_t>& mask,
			const EdgeStatsSettings& s = EdgeStatsSettings())
		{
			EdgeGateSummary sum;
			mask.clear();
			if (!s.enabled)
			{
				sum.reason = EdgeStatsReason::Disabled;
				return sum;
			}
			if (!line || length < 7 || !std::isfinite(floorCodes) || !(floorCodes > 0.0))
			{
				sum.reason = EdgeStatsReason::InvalidInput;
				return sum;
			}

			mask.assign(static_cast<size_t>(length), 0u);
			const double gradGate = s.gradientGateSigma * std::sqrt(2.0) * floorCodes;
			for (int k = 3; k <= length - 3; ++k)
			{
				++sum.totalStencilSamples;
				const double gL = std::fabs(line[k] - line[k - 1]);
				const double gR = std::fabs(line[k + 1] - line[k]);
				// LOW-3: require BOTH one-step differences finite before (std::max) so NaN in gR is never masked.
				if (!std::isfinite(gL) || !std::isfinite(gR))
					continue;
				const double g = (std::max)(gL, gR);
				if (g >= gradGate)
				{
					mask[static_cast<size_t>(k)] = 1u;
					++sum.activeEdgeSamples;
				}
			}

			sum.activeFraction = sum.totalStencilSamples > 0
				? static_cast<double>(sum.activeEdgeSamples) / static_cast<double>(sum.totalStencilSamples)
				: 0.0;
			const int minPx = s.minEdgePixels > 1 ? s.minEdgePixels : 1;
			if (sum.activeEdgeSamples < minPx)
			{
				sum.valid = false;
				sum.reason = EdgeStatsReason::InsufficientEdgePixels;
				return sum;
			}
			sum.valid = true;
			sum.reason = EdgeStatsReason::Ok;
			return sum;
		}

		struct PhaseContrastResult
		{
			bool valid = false;
			double pooledSecondDiffRms = 0.0;
			double minPhaseSecondDiffRms = 0.0;
			double maxPhaseSecondDiffRms = 0.0;
			double phaseContrastRatio = 1.0;  // maxPhaseRms / minPhaseRms (>= 1; higher = stronger period-q phase asymmetry)
			int samplesUsed = 0;
			EdgeStatsReason reason = EdgeStatsReason::InvalidInput;
		};

		// Measures period-q sub-sample phase asymmetry on second-difference energy, optionally restricted by an
		// edge gate mask (pass mask == nullptr for ungated baseline comparison).
		// On a 2:1 upscaled line (q = 2) embedded in 80% flat 8-bit dithered background, gating to edge pixels
		// markedly increases phaseContrastRatio by excluding isotropic flat-pixel dither.
		inline PhaseContrastResult ComputePhaseSecondDiffContrast(
			const double* line,
			int length,
			int periodQ,
			const std::vector<uint8_t>* edgeMask = nullptr,
			const EdgeStatsSettings& s = EdgeStatsSettings())
		{
			PhaseContrastResult res;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}
			if (!line || length < 7 || periodQ < 2 || periodQ > 32)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			if (edgeMask && static_cast<int>(edgeMask->size()) != length)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}

			std::vector<double> sumSq(static_cast<size_t>(periodQ), 0.0);
			std::vector<int> counts(static_cast<size_t>(periodQ), 0);
			double totalSq = 0.0;
			int totalCnt = 0;

			for (int k = 3; k <= length - 3; ++k)
			{
				if (edgeMask && (*edgeMask)[static_cast<size_t>(k)] == 0u)
					continue;
				const double d = line[k - 1] - 2.0 * line[k] + line[k + 1];
				if (!std::isfinite(d))
					continue;
				const size_t ph = static_cast<size_t>(k % periodQ);
				sumSq[ph] += d * d;
				++counts[ph];
				totalSq += d * d;
				++totalCnt;
			}

			const int minPerPhase = s.minEdgePixels > 1 ? s.minEdgePixels : 1;
			for (int p = 0; p < periodQ; ++p)
			{
				if (counts[static_cast<size_t>(p)] < minPerPhase)
				{
					res.valid = false;
					res.samplesUsed = totalCnt;
					res.reason = EdgeStatsReason::InsufficientEdgePixels;
					return res;
				}
			}

			double minRms = 1e300;
			double maxRms = 0.0;
			for (int p = 0; p < periodQ; ++p)
			{
				const double rms = std::sqrt(sumSq[static_cast<size_t>(p)] / static_cast<double>(counts[static_cast<size_t>(p)]));
				if (rms < minRms) minRms = rms;
				if (rms > maxRms) maxRms = rms;
			}

			res.pooledSecondDiffRms = std::sqrt(totalSq / static_cast<double>(totalCnt));
			res.minPhaseSecondDiffRms = minRms;
			res.maxPhaseSecondDiffRms = maxRms;
			res.samplesUsed = totalCnt;
			if (!(maxRms > 0.0))
			{
				// LOW-4: zero second-difference energy across all phases (constant/linear line): abstain visibly
				// rather than emitting a bogus phaseContrastRatio == 0.0 (< 1.0).
				res.valid = false;
				res.phaseContrastRatio = 1.0;
				res.reason = EdgeStatsReason::ZeroPhaseEnergy;
				return res;
			}
			res.phaseContrastRatio = (std::max)(1.0, maxRms / (std::max)(minRms, 1e-12));
			res.valid = std::isfinite(res.phaseContrastRatio);
			res.reason = res.valid ? EdgeStatsReason::Ok : EdgeStatsReason::InvalidInput;
			return res;
		}

		// ============================================================================================================
		// 7. Isolated-1/2 k/8 Comb Check (2026-11-20 Pitfall #1 / FM-4)
		// ============================================================================================================

		struct HalfCombCheckResult
		{
			int baseNullClass = kClassGeneric;     // Standard ClassifyLatent(n, N)
			int resolvedNullClass = kClassGeneric; // kClassK8 if codec comb or missing probes; kClassRational if isolated 1/2
			bool isHalfCandidate = false;
			bool isCodecGridComb = false;
			bool isIsolatedHalfNotch = false;
			int oddCombDipsPresent = 0;            // Count among {3/8, 5/8, 7/8} with stat < combDipThreshold (-0.03)
			EdgeStatsReason reason = EdgeStatsReason::NotHalfCandidate;
		};

		// Mirrors SourceResolutionDetector::allowedOnGrid for the 1/2 = 4/8 candidate (1080p -> 4K):
		//   - Genuine 8x8 codec grids produce comb dips across the odd k/8 harmonics (3/8, 5/8, 7/8, strongest at 7/8).
		//     When >= 2 odd harmonics dip, OR 7/8 dips without 1/2 standing >= 2x deeper, stay in harsh kClassK8.
		//   - An isolated 2:1 upscale dips strongly at 1/2 while 3/8, 5/8, 7/8 remain flat (>= majorityDip -0.03)
		//     (or at most one weak odd dip < half as deep as 1/2) => reclassify to kClassRational.
		//   - Missing/non-finite comb probes ALWAYS fail safe to kClassK8 with explicit CombProbesUnavailable (Rule 4).
		inline HalfCombCheckResult ClassifyHalfNotchWithCombCheck(
			int n,
			int N,
			double statHalf,
			bool haveCombProbes,
			double stat3of8,
			double stat5of8,
			double stat7of8,
			const EdgeStatsSettings& s = EdgeStatsSettings())
		{
			HalfCombCheckResult res;
			if (n <= 0 || N <= 0)
			{
				res.reason = EdgeStatsReason::InvalidInput;
				return res;
			}
			res.baseNullClass = ClassifyLatent(n, N, 6, s.halfPixelTol);
			res.resolvedNullClass = res.baseNullClass;
			if (!s.enabled)
			{
				res.reason = EdgeStatsReason::Disabled;
				return res;
			}

			const bool nearHalf = std::fabs(static_cast<double>(n) - 0.5 * static_cast<double>(N)) <= s.halfPixelTol;
			res.isHalfCandidate = nearHalf;
			if (!nearHalf)
			{
				res.reason = EdgeStatsReason::NotHalfCandidate;
				return res;
			}

			if (!haveCombProbes || !std::isfinite(statHalf) || !std::isfinite(stat3of8) ||
				!std::isfinite(stat5of8) || !std::isfinite(stat7of8))
			{
				// Fail safe: never relax K8 without valid comb probes.
				res.resolvedNullClass = kClassK8;
				res.reason = EdgeStatsReason::CombProbesUnavailable;
				return res;
			}

			const double dipThr = s.combDipThreshold; // -0.03 (ChainEvidence::EvidenceSettings::majorityDip)
			const int d3 = (stat3of8 < dipThr) ? 1 : 0;
			const int d5 = (stat5of8 < dipThr) ? 1 : 0;
			const int d7 = (stat7of8 < dipThr) ? 1 : 0;
			res.oddCombDipsPresent = d3 + d5 + d7;

			const double deepestOddComb = (std::min)(stat3of8, (std::min)(stat5of8, stat7of8));
			const bool halfDips = statHalf < dipThr;
			const bool dominatesComb = halfDips && (deepestOddComb >= 0.0 || statHalf <= s.combDominanceFactor * deepestOddComb);

			if (res.oddCombDipsPresent >= 2)
			{
				// Classic 8x8 codec-grid comb across multiple odd harmonics => keep harsh K8 null!
				res.isCodecGridComb = true;
				res.isIsolatedHalfNotch = false;
				res.resolvedNullClass = kClassK8;
				res.reason = EdgeStatsReason::CombGridDetected;
				return res;
			}

			if (d7 != 0 && !dominatesComb)
			{
				// 7/8 signature harmonic active and 1/2 does not stand >= 2x deeper => keep harsh K8 null!
				res.isCodecGridComb = true;
				res.isIsolatedHalfNotch = false;
				res.resolvedNullClass = kClassK8;
				res.reason = EdgeStatsReason::CombGridDetected;
				return res;
			}

			if (halfDips && (res.oddCombDipsPresent == 0 || dominatesComb))
			{
				// Isolated 1/2 notch (odd k/8 comb quiet or dominated >= 2x) => kClassRational.
				res.isCodecGridComb = false;
				res.isIsolatedHalfNotch = true;
				res.resolvedNullClass = kClassRational;
				res.reason = EdgeStatsReason::IsolatedHalfNotch;
				return res;
			}

			// MEDIUM-5: distinguish an undominated odd comb dip (e.g. 3/8 or 5/8 active without 1/2 dominating)
			// from flat native 4K (no 1/2 dip AND no odd k/8 comb at all): never mislabel flat native as a comb!
			res.resolvedNullClass = kClassK8;
			res.isIsolatedHalfNotch = false;
			if (res.oddCombDipsPresent > 0)
			{
				res.isCodecGridComb = true;
				res.reason = EdgeStatsReason::CombGridDetected;
			}
			else
			{
				res.isCodecGridComb = false;
				res.reason = EdgeStatsReason::NoHalfNotch;
			}
			return res;
		}
	}
}
