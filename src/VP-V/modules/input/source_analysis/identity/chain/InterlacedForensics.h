// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// InterlacedForensics (SID-I, Broadcast Interlaced 1080i Forensics):
// Pure C++14 standalone forensics to distinguish broadcast interlaced 1080i -> 2160p (both
// motion-bobbed fields and weave-combed interlaced frames, as well as native 1080i) from
// genuine progressive 540p/720p/1080p -> 2160p upscales and native 2160p fine horizontal gratings.
//
// Physics & Rule 1 constant derivations (zero invented magic numbers):
//   - Second-difference white-noise floor:
//       d(y; s) = I(x, y - s) - 2*I(x, y) + I(x, y + s) annihilates DC and linear vertical slope.
//       For i.i.d. white noise of std `floorCodes`, Var(d) = (1^2 + (-2)^2 + 1^2) * floorCodes^2
//       = 6 * floorCodes^2 => RMS floor = sqrt(6) * floorCodes (matching ChainEdgeStats.h:20).
//   - Amplitude & power detail gate:
//       lineGateFactor = 1.5 (ChainEvidence.h:87 EvidenceSettings::lineGateFactor):
//       amplitude gate = 1.5 * noiseRms, power SNR gate = 1.5^2 = 2.25.
//   - Horizontal edge gate for weave-comb isolation:
//       horizEdgeSigma = 3.0 (3-sigma Gaussian tail, 99.73% inlier coverage; ChainEdgeStats.h:110).
//       Same-row horizontal gradient gxSame(x, r) = 0.5 * (I(x + 1, r) - I(x - 1, r)) is strictly
//       intra-row (100% invariant to vertical field shift / horizontal gratings) and excludes I(x, r)
//       (zero noise selection correlation with cComb/cSame). Gated at |gx| >= 3 * sqrt(2) * floorCodes
//       (exact 3-sigma scale of 1-px white-noise differences, and 6-sigma on central difference gxSame),
//       ensuring native 2160p pure horizontal gratings where dx I == 0 never trigger.
//   - Weave-comb to same-field curvature ratio:
//       weaveCombToSameFieldRatio = 2.0: for a smooth quadratic vertical profile sampled at step s,
//       |cComb(s)| / |cSame(2s)| = |s^2 * I''| / |(2s)^2 * I''| = 1/4; for a sharp progressive step
//       edge or impulse, RMS(cComb) / RMS(cSame) <= 1.0; for white noise = 1.0; for alternating
//       even/odd field weave combing, same-field curvature cancels while cross-field comb doubles,
//       yielding ratio >> 2.0. Threshold 2.0 gives exact 2x clearance above white noise and 8x above
//       smooth progressive curvature.
//       Denominator floor guard = (1 / 4) * sqrt(6) * floorCodes (derived from ChainEdgeStats.h:114
//       floorGuardDivisor = 4.0) prevents division by zero on clean comb teeth where cSame == 0.
//   - Non-grating anisotropy guard:
//       weaveMaxUniformGratingShare = 0.5 (= 1 / 2, reciprocal of 2:1 dominance factor; matching
//       ChainEdgeStats.h:120 combDominanceFactor = 2.0). By optical flow, moving weave comb satisfies
//       cComb ~= +-2 * v_x * (dI/dx), so steep-H edge comb variance strictly dominates flat-H comb
//       variance by >= 2x (flatCombVar / edgeCombVar <= 0.5). On a uniform horizontal grating (even
//       with textured overlays), flat-H comb variance equals or exceeds steep-H comb variance (>= 1.0).
//   - Minimum active weave pixels:
//       minActiveWeavePixels = 32 (= kSegPx / 8 = 256 / 8, ChainCommon.h:27; power-of-two sample
//       floor above CLT N >= 30 for stable RMS variance ratio estimation).
//   - Motion-bob vertical upscale confirmation:
//       kBobVerticalDipThreshold = -0.09 (= 3 * EvidenceSettings::majorityDip = 3 * (-0.03),
//       ChainEvidence.h:92 & ChainResidualSpectrum.h:45).
//       kBobMaxVerticalRatio = 0.55 (= 0.50 + 0.05: exact 1/2 field height plus elementary 1/20 = 0.05
//       guard matching ReadingSettings::kernelMarginScale = 0.05 & ChainResidualSpectrum.h:15).
//   - Motion-bob 2:1 anisotropic field-height split:
//       bobAxisRatioTolerance = 0.06 (= 4 * EvidenceSettings::agreementTolerance = 4 * 0.015,
//       ChainEvidence.h:83; equivalently 10 * SourceGeometrySet::kConventionTolerance = 10 * 0.006),
//       bracketing exact rV / rH = 0.50 in [0.44, 0.56] while staying >0.10 below 2:3/anamorphic
//       (0.667) and >0.44 below progressive isotropic (1.0).
//   - Motion-bob horizontal passband corroboration:
//       Hann-windowed segment FFT (up to kSegPx = 256) normalized so white noise of std `floorCodes`
//       has E[psd[k]] = floorCodes^2 on interior bins (ChainResidualSpectrum.h:10-12). Mean per-bin
//       SNR is measured across Nyquist-normalized [0.55 * rH, 0.90 * rH] (matching W1 below-cutoff
//       band ChainResidualSpectrum.h:14; strictly above rV = 0.5 * rH Nyquist and below rH Nyquist)
//       and gated at lineGateFactor^2 = 2.25.
//   - Motion-bob temporal field parity flip:
//       Exact top/bottom field parity shift is 0.50 latent field lines; [bobPhaseFlipMin, bobPhaseFlipMax]
//       = [0.25, 0.75] brackets 0.50 +- 0.25 (quarter latent field line).
//
// Pure C++14, header-only, deterministic doubles, fixed loop order, no Windows headers, no threads/clocks,
// zero static/thread_local mutable state (all buffers/history live in caller-owned InterlacedWorkspace).
// Default OFF (enabled = false); every degraded/rejected/abstained path exposes explicit InterlacedReason.

#include "ChainCommon.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#pragma fp_contract(off)

namespace SourceIdentity
{
	namespace Chain
	{
		enum class InterlacedKind : int
		{
			None = 0,
			MotionBob = 1,
			WeaveComb = 2,
			Abstain = 3
		};

		inline const char* InterlacedKindName(InterlacedKind k)
		{
			switch (k)
			{
			case InterlacedKind::None: return "progressive";
			case InterlacedKind::MotionBob: return "motion_bob";
			case InterlacedKind::WeaveComb: return "weave_comb";
			case InterlacedKind::Abstain: return "abstain";
			default: return "abstain";
			}
		}

		enum class InterlacedReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			InsufficientEdgeActivity,
			ProgressiveIsotropic,
			NativeHorizontalGratingRejected,
			WeaveCombConfirmed,
			MotionBobAnisotropicSplit,
			MotionBobFieldParityFlip,
			Ambiguous
		};

		inline const char* InterlacedReasonText(InterlacedReason r)
		{
			switch (r)
			{
			case InterlacedReason::Ok: return "ok";
			case InterlacedReason::Disabled: return "interlaced forensics disabled";
			case InterlacedReason::InvalidInput: return "invalid input";
			case InterlacedReason::InsufficientEdgeActivity: return "abstain (insufficient horizontal edge activity)";
			case InterlacedReason::ProgressiveIsotropic: return "progressive isotropic (no interlaced signature)";
			case InterlacedReason::NativeHorizontalGratingRejected: return "native horizontal grating rejected (comb not edge-localized)";
			case InterlacedReason::WeaveCombConfirmed: return "weave-comb field alternation confirmed";
			case InterlacedReason::MotionBobAnisotropicSplit: return "motion-bob 2:1 vertical/horizontal field split confirmed";
			case InterlacedReason::MotionBobFieldParityFlip: return "motion-bob 2:1 split with temporal field parity flip confirmed";
			case InterlacedReason::Ambiguous: return "ambiguous interlaced evidence";
			default: return "unknown";
			}
		}

		struct InterlacedSettings
		{
			bool enabled = false;                       // Strictly OFF by default (W1 bit-exact contract)
			double lineGateFactor = 1.5;                // ChainEvidence.h:87: amplitude gate 1.5x floor (power gate 2.25x)
			double horizEdgeSigma = 3.0;                // 3-sigma Gaussian tail: |gx| >= 3 * sqrt(2) * floorCodes
			double weaveCombToSameFieldRatio = 2.0;     // 2x above white noise (1.0) and 8x above smooth progressive (0.25)
			double weaveMaxUniformGratingShare = 0.5;   // Elementary 1/2 dominance: flat-H comb var <= 0.5 * steep-H edge comb var
			double bobAxisRatioTolerance = 0.06;        // 4 * ChainEvidence::agreementTolerance (0.015) around exact rV / rH = 0.50
			double bobPhaseFlipMin = 0.25;              // Quarter latent field line: 0.50 - 0.25
			double bobPhaseFlipMax = 0.75;              // Three-quarter latent field line: 0.50 + 0.25
			int minActiveWeavePixels = 32;              // kSegPx / 8 = 32 minimum edge samples for stable RMS ratio
		};

		struct BobAxisInput
		{
			bool validH = false;
			bool validV = false;
			int nH = 0;
			int NH = 0;
			int nV = 0;
			int NV = 0;
			double statH = 0.0;             // Exact descale dip on H (negative = upscale dip)
			double statV = 0.0;             // Exact descale dip on V (negative = upscale dip)
			double phaseV = 0.0;            // Vertical sub-sample phase in latent samples
			double horizMidBandPower = 0.0; // Optional precomputed SNR in [0.55*rH, 0.90*rH] / floorCodes^2 (used when lsH == nullptr and > 0)
		};

		// Caller-owned bounded temporal parity state and FFT scratch (Rule 2: zero static/thread_local mutable state).
		struct InterlacedWorkspace
		{
			static constexpr int kParityHistory = 4;

			double phaseHistory[kParityHistory] = { 0.0, 0.0, 0.0, 0.0 };
			uint64_t seqHistory[kParityHistory] = { 0u, 0u, 0u, 0u };
			int count = 0;
			std::vector<double> scratch;

			void Reset()
			{
				for (int i = 0; i < kParityHistory; ++i)
				{
					phaseHistory[i] = 0.0;
					seqHistory[i] = 0u;
				}
				count = 0;
				scratch.clear();
			}
		};

		struct InterlacedResult
		{
			bool interlacedDetected = false;
			InterlacedKind kind = InterlacedKind::None;
			InterlacedReason reason = InterlacedReason::Ok;
			int weaveStride = 0;
			double weaveCombRatio = 0.0;
			double flatToEdgeCombRatio = 0.0;
			double axisRatioVOverH = 1.0;
			double horizAboveHalfSnr = 0.0;
			bool fieldParityFlipObserved = false;
		};

		namespace detail_interlaced
		{
			// Strictly numerical singularity guard against division by zero.
			constexpr double kNumericEps = 1e-20;

			// Derived from ChainEdgeStats.h:20: Var(1, -2, 1) = 6 * sigma^2.
			inline double SecondDiffNoiseScale() { return std::sqrt(6.0); }

			// Derived from ChainEdgeStats.h:114: floorGuardDivisor = 4.0 => eps = floor / 4.
			constexpr double kFloorGuardScale = 0.25;

			// Derived from ChainEvidence.h:92 (majorityDip = -0.03) & ChainResidualSpectrum.h:45: 3 * majorityDip.
			constexpr double kBobVerticalDipThreshold = -0.09;

			// Derived from exact half-field height 0.50 + elementary 1/20 = 0.05 guard (ChainReading.h:48 kernelMarginScale).
			constexpr double kBobMaxVerticalRatio = 0.55;

			// Derived from W1 below-cutoff passband (ChainResidualSpectrum.h:14): [0.55 * rH, 0.90 * rH].
			constexpr double kHorizBandLoFrac = 0.55;
			constexpr double kHorizBandHiFrac = 0.90;

			// Elementary midpoint 1/2 separating flat-H pixels (< 0.5 * gate) from steep-H edges (>= 1.0 * gate).
			constexpr double kFlatEdgeGateFrac = 0.5;

			inline bool ValidSettings(const InterlacedSettings& cfg)
			{
				if (!std::isfinite(cfg.lineGateFactor) || !(cfg.lineGateFactor > 0.0))
					return false;
				if (!std::isfinite(cfg.horizEdgeSigma) || !(cfg.horizEdgeSigma > 0.0))
					return false;
				if (!std::isfinite(cfg.weaveCombToSameFieldRatio) || !(cfg.weaveCombToSameFieldRatio > 0.0))
					return false;
				if (!std::isfinite(cfg.weaveMaxUniformGratingShare) || !(cfg.weaveMaxUniformGratingShare > 0.0))
					return false;
				if (!std::isfinite(cfg.bobAxisRatioTolerance) || !(cfg.bobAxisRatioTolerance > 0.0))
					return false;
				if (!std::isfinite(cfg.bobPhaseFlipMin) || !std::isfinite(cfg.bobPhaseFlipMax))
					return false;
				if (cfg.bobPhaseFlipMin < 0.0 || cfg.bobPhaseFlipMax > 1.0 || cfg.bobPhaseFlipMin > cfg.bobPhaseFlipMax)
					return false;
				if (cfg.minActiveWeavePixels < 1)
					return false;
				return true;
			}

			inline bool IsFiniteSample(double v) { return std::isfinite(v); }
			inline bool IsFiniteSample(uint16_t) { return true; }

			struct WeaveStrideEval
			{
				bool valid = false;
				int strideS = 0;
				InterlacedKind kind = InterlacedKind::Abstain;
				InterlacedReason reason = InterlacedReason::InsufficientEdgeActivity;
				double combRatio = 0.0;
				double flatToEdgeRatio = 0.0;
				double flatCombRms = 0.0;
			};

			template <class Pix>
			inline bool EvaluateWeaveStride(
				const Pix* plane,
				int width,
				int height,
				int stride,
				int s,
				double floorCodes,
				const InterlacedSettings& cfg,
				double sampleScale,
				WeaveStrideEval& out)
			{
				out = WeaveStrideEval();
				if (height < 4 * s + 5 || width < 5)
					return true;

				const double secondDiffNoise = SecondDiffNoiseScale() * floorCodes;
				const double combNoiseGate = cfg.lineGateFactor * secondDiffNoise;
				const double denomGuard = kFloorGuardScale * secondDiffNoise;
				const double horizEdgeGate = cfg.horizEdgeSigma * std::sqrt(2.0) * floorCodes;
				const double flatEdgeGate = kFlatEdgeGateFrac * horizEdgeGate;

				double edgeCombSq = 0.0;
				double edgeSameSq = 0.0;
				int64_t edgeCount = 0;
				double flatCombSq = 0.0;
				int64_t flatCount = 0;

				const std::ptrdiff_t rowStride = static_cast<std::ptrdiff_t>(stride);
				const std::ptrdiff_t sOff = static_cast<std::ptrdiff_t>(s) * rowStride;
				const std::ptrdiff_t twoSOff = static_cast<std::ptrdiff_t>(2 * s) * rowStride;

				const int yBegin = 2 * s;
				const int yEnd = height - 2 * s;
				for (int y = yBegin; y < yEnd; ++y)
				{
					const Pix* rowCur = plane + static_cast<std::ptrdiff_t>(y) * rowStride;
					const Pix* rowPrev = rowCur - sOff;
					const Pix* rowNext = rowCur + sOff;
					const Pix* rowPrev2 = rowCur - twoSOff;
					const Pix* rowNext2 = rowCur + twoSOff;

					for (int x = 1; x < width - 1; ++x)
					{
						if (!IsFiniteSample(rowCur[x - 1]) || !IsFiniteSample(rowCur[x]) || !IsFiniteSample(rowCur[x + 1]) ||
							!IsFiniteSample(rowPrev[x - 1]) || !IsFiniteSample(rowPrev[x]) || !IsFiniteSample(rowPrev[x + 1]) ||
							!IsFiniteSample(rowNext[x - 1]) || !IsFiniteSample(rowNext[x]) || !IsFiniteSample(rowNext[x + 1]) ||
							!IsFiniteSample(rowPrev2[x]) || !IsFiniteSample(rowNext2[x]))
						{
							return false;
						}

						const double cur = static_cast<double>(rowCur[x]) * sampleScale;
						const double prev = static_cast<double>(rowPrev[x]) * sampleScale;
						const double next = static_cast<double>(rowNext[x]) * sampleScale;
						const double prev2 = static_cast<double>(rowPrev2[x]) * sampleScale;
						const double next2 = static_cast<double>(rowNext2[x]) * sampleScale;

						// Strictly intra-row horizontal gradients (invariant to vertical field shift / horizontal gratings,
						// and excluding center column x so white noise has zero selection bias with cComb / cSame).
						// Taking the max magnitude over {y - s, y, y + s} preserves moving vertical extrema / 1-px lines
						// shifted between adjacent fields while remaining identically 0 on pure horizontal gratings.
						const double gxCur = std::fabs(0.5 * (static_cast<double>(rowCur[x + 1]) - static_cast<double>(rowCur[x - 1])) * sampleScale);
						const double gxPrev = std::fabs(0.5 * (static_cast<double>(rowPrev[x + 1]) - static_cast<double>(rowPrev[x - 1])) * sampleScale);
						const double gxNext = std::fabs(0.5 * (static_cast<double>(rowNext[x + 1]) - static_cast<double>(rowNext[x - 1])) * sampleScale);
						const double gxLocal = (std::max)(gxCur, (std::max)(gxPrev, gxNext));

						const double cComb = prev - 2.0 * cur + next;
						const double cSame = prev2 - 2.0 * cur + next2;

						if (gxLocal >= horizEdgeGate)
						{
							edgeCombSq += cComb * cComb;
							edgeSameSq += cSame * cSame;
							++edgeCount;
						}
						else if (gxLocal < flatEdgeGate)
						{
							flatCombSq += cComb * cComb;
							++flatCount;
						}
					}
				}

				out.valid = true;
				out.strideS = s;

				const double flatCombVar = flatCount > 0 ? (flatCombSq / static_cast<double>(flatCount)) : 0.0;
				const double flatCombRms = std::sqrt(flatCombVar);
				out.flatCombRms = flatCombRms;

				if (edgeCount < static_cast<int64_t>(cfg.minActiveWeavePixels))
				{
					if (flatCount > 0 && flatCombRms > combNoiseGate)
					{
						out.kind = InterlacedKind::None;
						out.reason = InterlacedReason::NativeHorizontalGratingRejected;
						out.flatToEdgeRatio = 1.0;
					}
					else
					{
						out.kind = InterlacedKind::Abstain;
						out.reason = InterlacedReason::InsufficientEdgeActivity;
					}
					return true;
				}

				const double edgeCombVar = edgeCombSq / static_cast<double>(edgeCount);
				const double edgeSameVar = edgeSameSq / static_cast<double>(edgeCount);
				const double edgeCombRms = std::sqrt(edgeCombVar);
				const double edgeSameRms = std::sqrt(edgeSameVar);

				out.combRatio = edgeCombRms / (edgeSameRms + denomGuard);
				out.flatToEdgeRatio = (flatCount > 0 && edgeCombVar > kNumericEps) ? (flatCombVar / edgeCombVar) : 0.0;

				const bool flatGratingActive = (flatCount >= static_cast<int64_t>(cfg.minActiveWeavePixels))
					&& (flatCombRms > combNoiseGate)
					&& (flatCombVar > cfg.weaveMaxUniformGratingShare * edgeCombVar);

				if (flatGratingActive)
				{
					out.kind = InterlacedKind::None;
					out.reason = InterlacedReason::NativeHorizontalGratingRejected;
					return true;
				}

				const bool aboveNoise = edgeCombRms > combNoiseGate;
				const bool combDominates = out.combRatio >= cfg.weaveCombToSameFieldRatio;

				if (aboveNoise && combDominates)
				{
					out.kind = InterlacedKind::WeaveComb;
					out.reason = InterlacedReason::WeaveCombConfirmed;
					return true;
				}

				out.kind = InterlacedKind::None;
				out.reason = InterlacedReason::ProgressiveIsotropic;
				return true;
			}

			inline int StrideDecisionRank(const WeaveStrideEval& e)
			{
				if (!e.valid)
					return 0;
				if (e.kind == InterlacedKind::WeaveComb)
					return 4;
				if (e.reason == InterlacedReason::NativeHorizontalGratingRejected)
					return 3;
				if (e.reason == InterlacedReason::ProgressiveIsotropic)
					return 2;
				return 1;
			}

			template <class Pix>
			inline InterlacedResult AnalyzeWeaveCombImpl(
				const Pix* plane,
				int width,
				int height,
				int stride,
				double floorCodes,
				const InterlacedSettings& cfg,
				double sampleScale)
			{
				InterlacedResult res;
				if (!cfg.enabled)
				{
					res.kind = InterlacedKind::None;
					res.reason = InterlacedReason::Disabled;
					return res;
				}

				if (plane == nullptr || width < 5 || height < 9 || stride < width ||
					!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
					!std::isfinite(sampleScale) || !(sampleScale > 0.0) || !ValidSettings(cfg))
				{
					res.kind = InterlacedKind::Abstain;
					res.reason = InterlacedReason::InvalidInput;
					return res;
				}

				WeaveStrideEval eval1;
				WeaveStrideEval eval2;
				if (!EvaluateWeaveStride(plane, width, height, stride, 1, floorCodes, cfg, sampleScale, eval1) ||
					!EvaluateWeaveStride(plane, width, height, stride, 2, floorCodes, cfg, sampleScale, eval2))
				{
					res.kind = InterlacedKind::Abstain;
					res.reason = InterlacedReason::InvalidInput;
					return res;
				}

				const int rank1 = StrideDecisionRank(eval1);
				const int rank2 = StrideDecisionRank(eval2);
				bool pick2 = false;
				if (rank2 > rank1)
				{
					pick2 = true;
				}
				else if (rank2 == rank1 && eval2.valid)
				{
					if (rank2 == 3)
					{
						// Both strides rejected a horizontal grating: report the stronger grating stride.
						if (eval2.flatCombRms > eval1.flatCombRms)
							pick2 = true;
					}
					else if (eval2.combRatio > eval1.combRatio)
					{
						pick2 = true;
					}
				}

				const WeaveStrideEval& best = pick2 ? eval2 : eval1;
				res.interlacedDetected = (best.kind == InterlacedKind::WeaveComb);
				res.kind = best.kind;
				res.reason = best.reason;
				res.weaveStride = best.strideS;
				res.weaveCombRatio = best.combRatio;
				res.flatToEdgeCombRatio = best.flatToEdgeRatio;
				return res;
			}

			// Wraps phase difference (a - b) onto [0.0, 1.0) in latent field-line units.
			inline double WrapUnitPhaseDiff(double a, double b)
			{
				double d = std::fmod(a - b, 1.0);
				if (d < 0.0)
					d += 1.0;
				return d;
			}

			inline int ChooseHorizFftLength(int length)
			{
				if (length < 32)
					return 0;
				if (length >= kSegPx)
					return kSegPx;
				if (length >= 128)
					return 128;
				return 64;
			}

			inline bool IsValidLineSet(const LineSet& ls)
			{
				if (ls.count <= 0 || ls.length < 32)
					return false;
				if (ls.data.size() / static_cast<size_t>(ls.length) < static_cast<size_t>(ls.count))
					return false;
				for (int li = 0; li < ls.count; ++li)
				{
					const double* line = ls.Line(li);
					for (int i = 0; i < ls.length; ++i)
					{
						if (!std::isfinite(line[i]))
							return false;
					}
				}
				return true;
			}

			// Deterministic iterative radix-2 Cooley-Tukey FFT (power-of-two n <= kSegPx).
			inline void FftRadix2InPlace(
				double* re,
				double* im,
				const double* twRe,
				const double* twIm,
				const size_t* bitRev,
				size_t n)
			{
				for (size_t i = 1; i < n; ++i)
				{
					const size_t j = bitRev[i];
					if (i < j)
					{
						std::swap(re[i], re[j]);
						std::swap(im[i], im[j]);
					}
				}
				for (size_t len = 2; len <= n; len <<= 1)
				{
					const size_t halfLen = len >> 1;
					const size_t offset = halfLen - 1;
					const double* stageRe = twRe + offset;
					const double* stageIm = twIm + offset;
					for (size_t start = 0; start < n; start += len)
					{
						for (size_t j = 0; j < halfLen; ++j)
						{
							const double wr = stageRe[j];
							const double wi = stageIm[j];
							const size_t uIdx = start + j;
							const size_t vIdx = uIdx + halfLen;
							const double vr = re[vIdx] * wr - im[vIdx] * wi;
							const double vi = re[vIdx] * wi + im[vIdx] * wr;
							const double ur = re[uIdx];
							const double ui = im[uIdx];
							re[uIdx] = ur + vr;
							im[uIdx] = ui + vi;
							re[vIdx] = ur - vr;
							im[vIdx] = ui - vi;
						}
					}
				}
			}

			inline void RecordParityPhase(InterlacedWorkspace& ws, double phaseV, uint64_t frameSeq)
			{
				if (ws.count > 0 && frameSeq == ws.seqHistory[ws.count - 1])
					return;
				if (ws.count < InterlacedWorkspace::kParityHistory)
				{
					ws.phaseHistory[ws.count] = phaseV;
					ws.seqHistory[ws.count] = frameSeq;
					++ws.count;
					return;
				}
				for (int i = 1; i < InterlacedWorkspace::kParityHistory; ++i)
				{
					ws.phaseHistory[i - 1] = ws.phaseHistory[i];
					ws.seqHistory[i - 1] = ws.seqHistory[i];
				}
				ws.phaseHistory[InterlacedWorkspace::kParityHistory - 1] = phaseV;
				ws.seqHistory[InterlacedWorkspace::kParityHistory - 1] = frameSeq;
			}
		}

		// Part A — 2-D Weave-Comb Forensics (double plane overload, 10-bit code units after `sampleScale`).
		// Automatically evaluates field strides s = 1 (native 1080i weave) and s = 2 (2x upscaled 1080i -> 2160p weave).
		inline InterlacedResult AnalyzeWeaveComb(
			const double* plane,
			int width,
			int height,
			int stride,
			double floorCodes,
			const InterlacedSettings& cfg = InterlacedSettings(),
			double sampleScale = 1.0)
		{
			return detail_interlaced::AnalyzeWeaveCombImpl(plane, width, height, stride, floorCodes, cfg, sampleScale);
		}

		// Part A — 2-D Weave-Comb Forensics (uint16_t plane overload, scales inline by `sampleScale` = 2^(msbShift - 6) into 10-bit codes).
		inline InterlacedResult AnalyzeWeaveComb(
			const uint16_t* plane,
			int width,
			int height,
			int stride,
			double floorCodes,
			const InterlacedSettings& cfg = InterlacedSettings(),
			double sampleScale = 1.0)
		{
			return detail_interlaced::AnalyzeWeaveCombImpl(plane, width, height, stride, floorCodes, cfg, sampleScale);
		}

		// Part B helper — Hann-windowed segment FFT (up to 256-pt) measuring mean per-bin horizontal power in
		// Nyquist-normalized [0.55 * rH, 0.90 * rH] (strictly above rV = 0.5 * rH Nyquist and below rH Nyquist),
		// divided by floorCodes^2. White noise of std `floorCodes` yields ~1.0; genuine detail above rV yields >> 2.25.
		// All working buffers live inside caller-owned `scratch`.
		inline double MeasureHorizBandwidthAboveHalf(
			const LineSet& lsH,
			double rH,
			double floorCodes,
			std::vector<double>& scratch)
		{
			if (!detail_interlaced::IsValidLineSet(lsH) ||
				!std::isfinite(rH) || !(rH > 0.0) || rH > 1.0 ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0))
			{
				return 0.0;
			}

			const int fftN = detail_interlaced::ChooseHorizFftLength(lsH.length);
			if (fftN < 64)
				return 0.0;

			const int half = fftN / 2;
			const double cutBin = rH * static_cast<double>(half);
			int kLo = static_cast<int>(std::ceil(detail_interlaced::kHorizBandLoFrac * cutBin));
			int kHi = static_cast<int>(std::floor(detail_interlaced::kHorizBandHiFrac * cutBin));
			kLo = (std::max)(2, kLo);
			kHi = (std::min)(half - 1, kHi);
			if (kLo > kHi)
				return 0.0;

			const int winLen = (std::min)(lsH.length, fftN);
			const size_t n = static_cast<size_t>(fftN);
			const size_t psdLen = static_cast<size_t>(half + 1);
			scratch.assign(5u * n + psdLen, 0.0);

			double* win = scratch.data();
			double* twRe = win + n;
			double* twIm = twRe + n;
			double* re = twIm + n;
			double* im = re + n;
			double* psd = im + n;

			double winEnergy = 0.0;
			for (int i = 0; i < winLen; ++i)
			{
				const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(winLen));
				win[static_cast<size_t>(i)] = w;
				winEnergy += w * w;
			}
			if (!(winEnergy > 0.0))
				return 0.0;

			size_t bitRev[kSegPx];
			bitRev[0] = 0u;
			for (size_t i = 1, j = 0; i < n; ++i)
			{
				size_t bit = n >> 1;
				for (; (j & bit) != 0u; bit >>= 1)
					j ^= bit;
				j ^= bit;
				bitRev[i] = j;
			}
			for (size_t len = 2; len <= n; len <<= 1)
			{
				const size_t halfLen = len >> 1;
				const size_t offset = halfLen - 1;
				const double angStep = -2.0 * kPi / static_cast<double>(len);
				for (size_t j = 0; j < halfLen; ++j)
				{
					const double ang = angStep * static_cast<double>(j);
					twRe[offset + j] = std::cos(ang);
					twIm[offset + j] = std::sin(ang);
				}
			}

			int totalSegs = 0;
			for (int li = 0; li < lsH.count; ++li)
			{
				const double* line = lsH.Line(li);
				int start = 0;
				bool done = false;
				while (!done)
				{
					double mean = 0.0;
					for (int i = 0; i < winLen; ++i)
						mean += line[start + i];
					mean /= static_cast<double>(winLen);

					for (int i = 0; i < fftN; ++i)
					{
						re[static_cast<size_t>(i)] = (i < winLen) ? (line[start + i] - mean) * win[static_cast<size_t>(i)] : 0.0;
						im[static_cast<size_t>(i)] = 0.0;
					}
					detail_interlaced::FftRadix2InPlace(re, im, twRe, twIm, bitRev, n);
					for (int k = kLo; k <= kHi; ++k)
					{
						const double rk = re[static_cast<size_t>(k)];
						const double ik = im[static_cast<size_t>(k)];
						psd[static_cast<size_t>(k)] += rk * rk + ik * ik;
					}
					++totalSegs;

					if (lsH.length <= fftN)
					{
						done = true;
					}
					else
					{
						const int nextStart = start + fftN;
						const int tailStart = lsH.length - fftN;
						if (start == tailStart)
							done = true;
						else if (nextStart + fftN <= lsH.length)
							start = nextStart;
						else
							start = tailStart;
					}
				}
			}

			if (totalSegs <= 0)
				return 0.0;

			double bandSum = 0.0;
			for (int k = kLo; k <= kHi; ++k)
				bandSum += psd[static_cast<size_t>(k)];

			const int binCount = kHi - kLo + 1;
			const double meanBinPower = bandSum / (static_cast<double>(binCount) * static_cast<double>(totalSegs) * winEnergy);
			const double floorPower = floorCodes * floorCodes;
			return meanBinPower / floorPower;
		}

		// Part B — Motion-Bobbed 1080i -> 2160p (and native 1080i) Forensics with caller-owned temporal parity tracking.
		inline InterlacedResult AnalyzeMotionBob(
			const BobAxisInput& in,
			const LineSet* lsH,
			double floorCodes,
			uint64_t frameSeq,
			const InterlacedSettings& cfg,
			InterlacedWorkspace& ws)
		{
			InterlacedResult res;
			if (!cfg.enabled)
			{
				res.kind = InterlacedKind::None;
				res.reason = InterlacedReason::Disabled;
				return res;
			}

			if (!detail_interlaced::ValidSettings(cfg) ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(in.statH) || !std::isfinite(in.statV) ||
				!std::isfinite(in.phaseV) || !std::isfinite(in.horizMidBandPower) ||
				in.horizMidBandPower < 0.0)
			{
				res.kind = InterlacedKind::Abstain;
				res.reason = InterlacedReason::InvalidInput;
				return res;
			}

			if (!in.validH || !in.validV ||
				in.nH <= 0 || in.NH <= 0 || in.nH > in.NH ||
				in.nV <= 0 || in.NV <= 0 || in.nV > in.NV)
			{
				res.kind = InterlacedKind::Abstain;
				res.reason = InterlacedReason::InvalidInput;
				return res;
			}

			if (lsH != nullptr && !detail_interlaced::IsValidLineSet(*lsH))
			{
				res.kind = InterlacedKind::Abstain;
				res.reason = InterlacedReason::InvalidInput;
				return res;
			}

			const double rH = static_cast<double>(in.nH) / static_cast<double>(in.NH);
			const double rV = static_cast<double>(in.nV) / static_cast<double>(in.NV);
			const double axisRatio = rV / rH;
			res.axisRatioVOverH = axisRatio;

			// Out-of-order sequence (seek / stream reset): clear stale temporal parity history.
			if (ws.count > 0 && frameSeq < ws.seqHistory[ws.count - 1])
				ws.Reset();

			const bool verticalUpscaled = (in.statV < detail_interlaced::kBobVerticalDipThreshold) &&
				(rV <= detail_interlaced::kBobMaxVerticalRatio);
			const bool hasTwoToOneSplit = std::fabs(axisRatio - 0.5) <= cfg.bobAxisRatioTolerance;
			const bool isIsotropic = std::fabs(axisRatio - 1.0) <= cfg.bobAxisRatioTolerance;

			if (!verticalUpscaled)
			{
				// Native or unverified vertical axis: clear stale bob parity history and report progressive.
				ws.count = 0;
				res.interlacedDetected = false;
				res.kind = InterlacedKind::None;
				res.reason = InterlacedReason::ProgressiveIsotropic;
				return res;
			}

			if (!hasTwoToOneSplit)
			{
				ws.count = 0;
				res.interlacedDetected = false;
				res.kind = InterlacedKind::None;
				res.reason = isIsotropic ? InterlacedReason::ProgressiveIsotropic : InterlacedReason::Ambiguous;
				return res;
			}

			// Optional horizontal bandwidth corroboration above rV = 0.5 * rH.
			const double powerGate = cfg.lineGateFactor * cfg.lineGateFactor;
			bool horizCorroborated = true;
			if (lsH != nullptr)
			{
				res.horizAboveHalfSnr = MeasureHorizBandwidthAboveHalf(*lsH, rH, floorCodes, ws.scratch);
				horizCorroborated = res.horizAboveHalfSnr > powerGate;
			}
			else if (in.horizMidBandPower > 0.0)
			{
				res.horizAboveHalfSnr = in.horizMidBandPower;
				horizCorroborated = res.horizAboveHalfSnr > powerGate;
			}

			if (!horizCorroborated)
			{
				res.interlacedDetected = false;
				res.kind = InterlacedKind::Abstain;
				res.reason = InterlacedReason::Ambiguous;
				return res;
			}

			// Track temporal vertical phase flip across consecutive frames (seq > prevSeq).
			bool parityFlip = false;
			if (ws.count > 0 && frameSeq > ws.seqHistory[ws.count - 1])
			{
				const double prevPhaseV = ws.phaseHistory[ws.count - 1];
				const double dPhi = detail_interlaced::WrapUnitPhaseDiff(in.phaseV, prevPhaseV);
				if (dPhi >= cfg.bobPhaseFlipMin && dPhi <= cfg.bobPhaseFlipMax)
					parityFlip = true;
			}
			detail_interlaced::RecordParityPhase(ws, in.phaseV, frameSeq);

			res.interlacedDetected = true;
			res.kind = InterlacedKind::MotionBob;
			res.fieldParityFlipObserved = parityFlip;
			res.reason = parityFlip ? InterlacedReason::MotionBobFieldParityFlip : InterlacedReason::MotionBobAnisotropicSplit;
			return res;
		}
	}
}

#pragma fp_contract(on)
