// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainChromaEstimator (SID-F, Stage S5):
// Pure C++14 chroma origin (Native444 / Subsampled422 / Subsampled420), player upsampling kernel,
// chroma grid siting (Center / Left / TopLeft), and 1/4-res luma-upscale corroboration estimator.
//
// Physics & Rule 1 constant derivations:
//   - Subsampling detection via 2:1 DescaleOperator at n = N/2 compared against adjacent non-2:1
//     baselines nLo = round(0.92 * N/2) and nHi = round(1.08 * N/2):
//       dip = ln(s_best / s_baseline).
//     On native 4:4:4, E[dip] ~= 0.0; dipConfirmThreshold = -0.20 (PROVISIONAL pending Stage S5
//     G-gate calibration on real 4:4:4 vs 4:2:0 corpus; conservative multi-fold clearance above
//     EvidenceSettings::majorityDip = -0.03, requiring >= 18% DOF-normalized residual reduction
//     below adjacent baselines to maintain 0 false calls on native 4:4:4).
//   - Above-Nyquist native 4:4:4 confirmation gate: lineGateFactor = 1.5 (ChainEvidence.h) =>
//     nativeAboveRatioGate = 1.5^2 = 2.25 * floorCodes^2. Chroma FFT bands [0.08, 0.22] and
//     [0.28, 0.48] cyc/px are derived from the 2:1 chroma Nyquist f_c = 0.25 cyc/px with the
//     SourceResolutionDetector.cpp:857-858 transition guard around f_c (below [0.32*f_c, 0.88*f_c],
//     above [1.12*f_c, 0.96*0.5]).
//   - Siting physics (exact KernelBank centre-aligned mapping p_i = (i + 0.5) * 0.5 - 0.5 + phi):
//       * Centered (chroma sample m midway between 2m and 2m+1): p_{2m} = m - 0.25 => phi = 0.0.
//       * Cosited (chroma sample m directly on even pixel 2m):   p_{2m} = m        => phi = +0.25.
//     Fused with exact symmetric-kernel parity asymmetry (second-difference even/odd excess energy
//     ratio peaks for Cosited; first-difference odd-to-even/even-to-odd excess energy ratio peaks
//     for Centered, optionally edge-weighted by co-located luma gradients when provided).
//   - Separation margins (Rule 1 — derived from existing contract constants, PROVISIONAL pending
//     Stage S5 G-gate calibration):
//       * kernelMarginMin = 0.05 (matches ChainReading.h:48 ReadingSettings::kernelMarginScale = 0.05,
//         the 1 - 1/e kernel-family confidence scale, before naming a classical kernel).
//       * sitingMarginMin = 0.03 (matches ChainEvidence.h:92 |EvidenceSettings::majorityDip| = 0.03
//         contract separation between Cosited and Centered grid siting).
//   - 1/4-res corroboration: evaluated whenever raw demeaned chroma std > lineGateFactor * floorCodes
//     (even if N/2 baseline is quiet, as on 1080p-in-4K 4:2:0 where chroma is 960x540 = N/4).
//
// Pure C++14, header-only, deterministic doubles, fixed loop order, no Windows headers, no threads/clocks.
// OFF by default (enabled = false); every degraded/abstained/unsure path carries an explicit reason.

#include "ChainCommon.h"
#include "KernelBank.h"
#include "../SourceGeometryProduct.h"

#include <VP-V/core/RobustStats.h>

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
		enum class DeliveredChromaSampling : int
		{
			Yuv444 = 0, // Default (backward compatible): full-width & full-height chroma planes delivered
			Yuv422 = 1, // 4:2:2 HDMI transport (e.g. Apple TV / Shield / Blu-ray 12-bit): H already N/2, V is full N
			Yuv420 = 2  // Native 4:2:0 delivered directly (both H & V already N/2; not player-upsampled)
		};

		inline const char* DeliveredChromaSamplingName(DeliveredChromaSampling s)
		{
			switch (s)
			{
			case DeliveredChromaSampling::Yuv444: return "yuv444";
			case DeliveredChromaSampling::Yuv422: return "yuv422";
			case DeliveredChromaSampling::Yuv420: return "yuv420";
			default: return "unknown";
			}
		}

		enum class ChromaUpsamplerFamily : int
		{
			Unknown = 0,
			Bilinear = 1,
			Bicubic = 2,
			Lanczos = 3,
			Spline = 4,
			PointNearest = 5, // Hardware nearest-neighbor / line duplication (zero-order hold)
			MpegLinear = 6    // MPEG-2/4 2-tap/3-tap linear FIR ([3,1]/4 centered or field-shifted, [1,2,1]/4 cosited)
		};

		inline const char* ChromaUpsamplerFamilyName(ChromaUpsamplerFamily f)
		{
			switch (f)
			{
			case ChromaUpsamplerFamily::Unknown: return "unknown";
			case ChromaUpsamplerFamily::Bilinear: return "bilinear";
			case ChromaUpsamplerFamily::Bicubic: return "bicubic";
			case ChromaUpsamplerFamily::Lanczos: return "lanczos";
			case ChromaUpsamplerFamily::Spline: return "spline";
			case ChromaUpsamplerFamily::PointNearest: return "point_nearest";
			case ChromaUpsamplerFamily::MpegLinear: return "mpeg_linear";
			default: return "unknown";
			}
		}

		enum class ChromaOrigin : int
		{
			Unknown = 0,
			Native444 = 1,
			Subsampled422 = 2,
			Subsampled420 = 3
		};

		enum class ChromaGridSiting : int
		{
			Unknown = -1,
			Center = 0,
			Left = 1,
			TopLeft = 2
		};

		inline const char* ChromaOriginName(ChromaOrigin o)
		{
			switch (o)
			{
			case ChromaOrigin::Unknown: return "unknown";
			case ChromaOrigin::Native444: return "native_444";
			case ChromaOrigin::Subsampled422: return "subsampled_422";
			case ChromaOrigin::Subsampled420: return "subsampled_420";
			default: return "unknown";
			}
		}

		inline const char* ChromaGridSitingName(ChromaGridSiting s)
		{
			switch (s)
			{
			case ChromaGridSiting::Unknown: return "unknown";
			case ChromaGridSiting::Center: return "center";
			case ChromaGridSiting::Left: return "left";
			case ChromaGridSiting::TopLeft: return "top_left";
			default: return "unknown";
			}
		}

		enum class ChromaEstimatorReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			LowChromaDetail,
			Native444Confirmed,
			PlayerUpsampled420Confirmed,
			Subsampled422Confirmed,
			KernelUnsure,
			SitingUnsure,
			QuarterCorroborated,
			QuarterNotSupported,
			NonStandardSubsampling,
			// SID-SOTA-6 (strictly appended for ABI/enum stability):
			Native422Confirmed,
			Native420Delivered,
			PointNearestUpsampledConfirmed,
			MpegLinearUpsampledConfirmed,
			LumaCoupledChromaCorroborated,
			LumaCoupledChromaNotSupported
		};

		inline const char* ChromaEstimatorReasonText(ChromaEstimatorReason r)
		{
			switch (r)
			{
			case ChromaEstimatorReason::Ok: return "ok";
			case ChromaEstimatorReason::Disabled: return "chroma estimator disabled";
			case ChromaEstimatorReason::InvalidInput: return "invalid input";
			case ChromaEstimatorReason::LowChromaDetail: return "abstain (low chroma detail)";
			case ChromaEstimatorReason::Native444Confirmed: return "native 4:4:4 chroma confirmed";
			case ChromaEstimatorReason::PlayerUpsampled420Confirmed: return "player-upsampled 4:2:0 chroma confirmed";
			case ChromaEstimatorReason::Subsampled422Confirmed: return "subsampled 4:2:2 chroma confirmed";
			case ChromaEstimatorReason::KernelUnsure: return "chroma origin confirmed (kernel unsure)";
			case ChromaEstimatorReason::SitingUnsure: return "chroma origin confirmed (siting unsure)";
			case ChromaEstimatorReason::QuarterCorroborated: return "1/4-res chroma dip corroborates 1/2-res luma upscale";
			case ChromaEstimatorReason::QuarterNotSupported: return "1/4-res chroma dip not supported";
			case ChromaEstimatorReason::NonStandardSubsampling: return "abstain (non-standard vertical-only chroma subsampling)";
			case ChromaEstimatorReason::Native422Confirmed: return "native 4:2:2 chroma confirmed";
			case ChromaEstimatorReason::Native420Delivered: return "delivered 4:2:0 chroma confirmed (not player-upsampled)";
			case ChromaEstimatorReason::PointNearestUpsampledConfirmed: return "point/nearest-neighbor upsampled chroma confirmed";
			case ChromaEstimatorReason::MpegLinearUpsampledConfirmed: return "MPEG-2/4 linear FIR upsampled chroma confirmed";
			case ChromaEstimatorReason::LumaCoupledChromaCorroborated: return "luma-coupled chroma grid corroborates luma upscale";
			case ChromaEstimatorReason::LumaCoupledChromaNotSupported: return "luma-coupled chroma grid not supported";
			default: return "unknown";
			}
		}

		struct ChromaEstimatorSettings
		{
			bool enabled = false;               // OFF by default (Stage S5 safe contract)
			double lineGateFactor = 1.5;        // ChainEvidence.h:87 contract: detail >= 1.5 * floorCodes
			double dipConfirmThreshold = -0.20; // PROVISIONAL (pending Stage S5 G-gate calibration): multi-fold clearance above majorityDip (-0.03); >= 18% residual reduction
			double nativeAboveRatioGate = 2.25; // Derived: lineGateFactor^2 = 1.5^2 = 2.25 (above-0.25 cyc/px chroma power gate)
			double kernelMarginMin = 0.05;      // Derived from ReadingSettings::kernelMarginScale (ChainReading.h:48 = 0.05); PROVISIONAL pending Stage S5 G-gate
			double sitingMarginMin = 0.03;      // Derived from |EvidenceSettings::majorityDip| (ChainEvidence.h:92 = 0.03); PROVISIONAL pending Stage S5 G-gate
			bool detectBoxAndMpegChroma = false;// SID-SOTA-6 (default OFF): algebraic nearest-neighbor (line-duplication) & MPEG-2/4 [3,1]/4 / [1,2,1]/4 linear FIR probe
		};

		struct ChromaSampleInput
		{
			LineSet lumaH;
			LineSet lumaV;
			LineSet cbH;
			LineSet cbV;
			LineSet crH;
			LineSet crV;
			double floorCodes = 0.289; // 10-bit uniform quantization floor: 1 / sqrt(12) ~= 0.288675
			// SID-SOTA-6 (backward-compatible defaults):
			DeliveredChromaSampling deliveredSampling = DeliveredChromaSampling::Yuv444;
			int knownLumaNH = 0;       // Optional (>0): known horizontal luma source size (tests coupled chroma nChromaH = knownLumaNH / 2)
			int knownLumaNV = 0;       // Optional (>0): known vertical luma source size (tests coupled chroma nChromaV = knownLumaNV / 2)
		};

		struct ChromaQuarterCorroboration
		{
			bool valid = false;
			bool supportsTwoToOneLumaUpscale = false;
			double dipH = 0.0;
			double dipV = 0.0;
			double confidence = 0.0;
			ChromaEstimatorReason reason = ChromaEstimatorReason::Disabled;
		};

		struct ChromaLumaCoupledCorroboration
		{
			bool valid = false;
			bool corroborated = false;
			int expectedChromaNH = 0;
			int expectedChromaNV = 0;
			double dipH = 0.0;
			double dipV = 0.0;
			double confidence = 0.0;
			ChromaEstimatorReason reason = ChromaEstimatorReason::Disabled;
		};

		struct ChromaChainEstimate
		{
			bool valid = false;
			ChromaOrigin origin = ChromaOrigin::Unknown;
			bool playerUpsampled = false;
			int kernelIndex = -1;
			int kernelFamily = -1;
			ChromaUpsamplerFamily upsamplerFamily = ChromaUpsamplerFamily::Unknown;
			double kernelMargin = 0.0;
			ChromaGridSiting siting = ChromaGridSiting::Unknown;
			double sitingMargin = 0.0;
			double dipH = 0.0;
			double dipV = 0.0;
			double confidence = 0.0;
			ChromaEstimatorReason reason = ChromaEstimatorReason::Disabled;
			ChromaQuarterCorroboration quarter;
			ChromaLumaCoupledCorroboration lumaCoupled;

			Geometry::ChromaChain ToProductChromaChain() const
			{
				Geometry::ChromaChain out;
				out.originSampling = static_cast<uint8_t>(origin);
				out.playerUpsampled = valid ? static_cast<uint8_t>(playerUpsampled ? 2u : 1u) : static_cast<uint8_t>(0u);
				out.kernel = static_cast<int8_t>(kernelIndex);
				out.siting = static_cast<int8_t>(siting);
				out.conf = static_cast<float>(ClampD(confidence, 0.0, 1.0));
				return out;
			}
		};

		namespace detail_chroma_estimator
		{
			constexpr double kNumericEps = 1e-20; // Strictly numerical singularity guard
			constexpr double kTrimFraction = 0.2; // Existing ChainEvidence.h:97 20% trimmed mean
			constexpr double kFloorGuardDiv = 4.0;// Existing ChainEdgeStats.h:114 eps = floor / 4

			inline bool ValidLineSet(const LineSet& ls, int minLen)
			{
				if (ls.count <= 0 || ls.length < minLen || ls.axisLen <= 0)
					return false;
				if (ls.data.size() / static_cast<size_t>(ls.length) < static_cast<size_t>(ls.count))
					return false;
				return true;
			}

			inline bool AllFinite(const LineSet& ls)
			{
				for (int li = 0; li < ls.count; ++li)
				{
					const double* line = ls.Line(li);
					for (int j = 0; j < ls.length; ++j)
					{
						if (!std::isfinite(line[j]))
							return false;
					}
				}
				return true;
			}

			// Computes pooled demeaned signal std across cb (and optional cr) before any N/2 baseline gating,
			// so 1/4-res chroma (1080p-in-4K 4:2:0) is distinguished from flat/achromatic grayscale chroma.
			inline double ComputeAxisSignalStd(const LineSet& cb, const LineSet* cr)
			{
				double sumSq = 0.0;
				size_t totalSamples = 0;
				auto accumStd = [&](const LineSet& ls) {
					for (int li = 0; li < ls.count; ++li)
					{
						const double* line = ls.Line(li);
						double mean = 0.0;
						for (int j = 0; j < ls.length; ++j)
							mean += line[j];
						mean /= static_cast<double>(ls.length);
						for (int j = 0; j < ls.length; ++j)
						{
							const double d = line[j] - mean;
							sumSq += d * d;
						}
						totalSamples += static_cast<size_t>(ls.length);
					}
				};
				accumStd(cb);
				if (cr != nullptr)
					accumStd(*cr);
				if (totalSamples == 0)
					return 0.0;
				return std::sqrt(sumSq / static_cast<double>(totalSamples));
			}

			struct FftTwiddles
			{
				size_t fftN = 0;
				std::vector<size_t> bitRev;
				std::vector<double> re; // size fftN - 1: stage offset = halfLen - 1
				std::vector<double> im;

				void Build(size_t n)
				{
					fftN = n;
					bitRev.assign(n, 0u);
					for (size_t i = 1, j = 0; i < n; ++i)
					{
						size_t bit = n >> 1;
						for (; (j & bit) != 0u; bit >>= 1)
							j ^= bit;
						j ^= bit;
						bitRev[i] = j;
					}
					re.assign(n > 0 ? n - 1 : 0, 0.0);
					im.assign(n > 0 ? n - 1 : 0, 0.0);
					for (size_t len = 2; len <= n; len <<= 1)
					{
						const size_t halfLen = len >> 1;
						const size_t offset = halfLen - 1;
						const double angStep = -2.0 * kPi / static_cast<double>(len);
						for (size_t j = 0; j < halfLen; ++j)
						{
							const double ang = angStep * static_cast<double>(j);
							re[offset + j] = std::cos(ang);
							im[offset + j] = std::sin(ang);
						}
					}
				}
			};

			inline void FftRadix2(std::vector<double>& re, std::vector<double>& im, const FftTwiddles& tw)
			{
				const size_t n = re.size();
				for (size_t i = 1; i < n; ++i)
				{
					const size_t j = tw.bitRev[i];
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
					const double* twRe = &tw.re[offset];
					const double* twIm = &tw.im[offset];
					for (size_t start = 0; start < n; start += len)
					{
						for (size_t j = 0; j < halfLen; ++j)
						{
							const double wr = twRe[j];
							const double wi = twIm[j];
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

			// Computes mean PSD in below-Nyquist [0.08, 0.22] and above-Nyquist [0.28, 0.48] cyc/px bands
			// across cb (and optional cr) with exact white-noise variance normalization.
			inline bool ComputeChromaBandPowers(
				const LineSet& cb,
				const LineSet* cr,
				double& outBelowPower,
				double& outAbovePower)
			{
				outBelowPower = 0.0;
				outAbovePower = 0.0;
				int fftN = 64;
				while (fftN * 2 <= cb.length && fftN * 2 <= 1024)
					fftN *= 2;
				if (cb.length < 32)
					return false;
				const int winLen = cb.length < fftN ? cb.length : fftN;
				std::vector<double> win(static_cast<size_t>(winLen), 0.0);
				double winEnergy = 0.0;
				for (int i = 0; i < winLen; ++i)
				{
					const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(winLen));
					win[static_cast<size_t>(i)] = w;
					winEnergy += w * w;
				}
				if (!(winEnergy > 0.0))
					return false;

				FftTwiddles tw;
				tw.Build(static_cast<size_t>(fftN));

				const int half = fftN / 2;
				std::vector<double> psd(static_cast<size_t>(half + 1), 0.0);
				std::vector<double> re(static_cast<size_t>(fftN), 0.0);
				std::vector<double> im(static_cast<size_t>(fftN), 0.0);
				int totalSegs = 0;

				auto accumPlane = [&](const LineSet& ls) {
					std::vector<int> segStarts;
					if (ls.length <= fftN)
					{
						segStarts.push_back(0);
					}
					else
					{
						for (int start = 0; start + fftN <= ls.length; start += fftN)
							segStarts.push_back(start);
						const int tailStart = ls.length - fftN;
						if (segStarts.empty() || segStarts.back() != tailStart)
							segStarts.push_back(tailStart);
					}
					const int curWinLen = ls.length < fftN ? ls.length : fftN;
					for (int li = 0; li < ls.count; ++li)
					{
						const double* line = ls.Line(li);
						for (size_t si = 0; si < segStarts.size(); ++si)
						{
							const int start = segStarts[si];
							double mean = 0.0;
							for (int i = 0; i < curWinLen; ++i)
								mean += line[start + i];
							mean /= static_cast<double>(curWinLen);
							for (int i = 0; i < fftN; ++i)
							{
								re[static_cast<size_t>(i)] = (i < curWinLen) ? (line[start + i] - mean) * win[static_cast<size_t>(i)] : 0.0;
								im[static_cast<size_t>(i)] = 0.0;
							}
							FftRadix2(re, im, tw);
							for (int k = 0; k <= half; ++k)
							{
								const double rk = re[static_cast<size_t>(k)];
								const double ik = im[static_cast<size_t>(k)];
								const double weight = (k == 0 || k == half) ? 1.0 : 2.0;
								psd[static_cast<size_t>(k)] += (rk * rk + ik * ik) * weight;
							}
							++totalSegs;
						}
					}
				};

				accumPlane(cb);
				if (cr != nullptr && cr->length == cb.length)
					accumPlane(*cr);
				if (totalSegs <= 0)
					return false;

				const double norm = static_cast<double>(totalSegs) * winEnergy * 2.0;
				for (size_t k = 0; k < psd.size(); ++k)
					psd[k] /= norm;

				const int bLo = (std::max)(1, static_cast<int>(std::ceil(0.08 * static_cast<double>(fftN))));
				const int bHi = (std::min)(half - 1, static_cast<int>(std::floor(0.22 * static_cast<double>(fftN))));
				const int aLo = (std::max)(1, static_cast<int>(std::ceil(0.28 * static_cast<double>(fftN))));
				const int aHi = (std::min)(half - 1, static_cast<int>(std::floor(0.48 * static_cast<double>(fftN))));
				if (bLo > bHi || aLo > aHi)
					return false;

				double bSum = 0.0;
				int bCnt = 0;
				for (int k = bLo; k <= bHi; ++k)
				{
					bSum += psd[static_cast<size_t>(k)];
					++bCnt;
				}
				double aSum = 0.0;
				int aCnt = 0;
				for (int k = aLo; k <= aHi; ++k)
				{
					aSum += psd[static_cast<size_t>(k)];
					++aCnt;
				}
				if (bCnt <= 0 || aCnt <= 0)
					return false;
				outBelowPower = bSum / static_cast<double>(bCnt);
				outAbovePower = aSum / static_cast<double>(aCnt);
				return true;
			}

			// Computes signed parity siting score in [-1, +1] (positive => Cosited +0.25, negative => Centered 0.0)
			// from exact symmetric-kernel parity asymmetry:
			//   - Cosited:  E[d2(2m)^2] >> E[d2(2m+1)^2], while E[d1(2m+1->2m+2)^2] == E[d1(2m->2m+1)^2].
			//   - Centered: E[d1(2m+1->2m+2)^2] >> E[d1(2m->2m+1)^2], while E[d2(2m)^2] == E[d2(2m+1)^2].
			// Optionally weighted by co-located luma edge gradients when `luma` matches `cb` geometry.
			inline double ComputeAxisParitySitingScore(
				const LineSet& cb,
				const LineSet* cr,
				const LineSet* luma,
				double floorCodes)
			{
				const bool useLuma =
					luma != nullptr &&
					ValidLineSet(*luma, cb.length) &&
					luma->count == cb.count &&
					luma->length == cb.length &&
					luma->origin == cb.origin &&
					AllFinite(*luma);

				double d2EvenSum = 0.0, d2EvenW = 0.0;
				double d2OddSum = 0.0, d2OddW = 0.0;
				double d1EOSum = 0.0, d1EOW = 0.0;
				double d1OESum = 0.0, d1OEW = 0.0;

				auto accumParity = [&](const LineSet& ls, bool allowLumaWeight) {
					for (int li = 0; li < ls.count; ++li)
					{
						const double* y = ls.Line(li);
						const double* yLuma = (allowLumaWeight && useLuma) ? luma->Line(li) : nullptr;
						for (int j = 1; j + 1 < ls.length; ++j)
						{
							const int coord = ls.origin + j;
							const int parity = ((coord % 2) + 2) % 2;
							double w = 1.0;
							if (yLuma != nullptr)
							{
								const double gL = 0.5 * std::fabs(yLuma[j + 1] - yLuma[j - 1]);
								w = 1.0 + gL / floorCodes;
							}
							const double d2 = y[j - 1] - 2.0 * y[j] + y[j + 1];
							const double d2Sq = d2 * d2;
							if (parity == 0)
							{
								d2EvenSum += w * d2Sq;
								d2EvenW += w;
							}
							else
							{
								d2OddSum += w * d2Sq;
								d2OddW += w;
							}
							const double d1 = y[j + 1] - y[j];
							const double d1Sq = d1 * d1;
							if (parity == 0)
							{
								d1EOSum += w * d1Sq;
								d1EOW += w;
							}
							else
							{
								d1OESum += w * d1Sq;
								d1OEW += w;
							}
						}
					}
				};

				accumParity(cb, true);
				if (cr != nullptr)
					accumParity(*cr, cr->count == cb.count && cr->length == cb.length && cr->origin == cb.origin);

				if (!(d2EvenW > 0.0) || !(d2OddW > 0.0) || !(d1EOW > 0.0) || !(d1OEW > 0.0))
					return 0.0;

				const double floorP = floorCodes * floorCodes;
				const double epsP = (std::max)(floorP / (kFloorGuardDiv * kFloorGuardDiv), kNumericEps);
				// Second-diff noise variance = (1^2 + (-2)^2 + 1^2) * floor^2 = 6 * floor^2;
				// First-diff noise variance  = (1^2 + (-1)^2) * floor^2 = 2 * floor^2.
				const double exD2Even = (std::max)(d2EvenSum / d2EvenW - 6.0 * floorP, epsP);
				const double exD2Odd = (std::max)(d2OddSum / d2OddW - 6.0 * floorP, epsP);
				const double exD1EO = (std::max)(d1EOSum / d1EOW - 2.0 * floorP, epsP);
				const double exD1OE = (std::max)(d1OESum / d1OEW - 2.0 * floorP, epsP);

				const double cositedLogRatio = 0.5 * std::log(exD2Even / exD2Odd);
				const double centeredLogRatio = 0.5 * std::log(exD1OE / exD1EO);
				const double parityDiff = cositedLogRatio - centeredLogRatio;
				return std::tanh(0.5 * parityDiff);
			}

			// Pre-factored DescaleOperator bundle for one axis geometry (N, origin, length):
			// holds both N/2 (all 10 kernels x 2 siting phases + 2x4 baselines) and N/4 (4 reps x 4 phases + 2x4 baselines).
			struct ChromaAxisOperators
			{
				int N = 0;
				int origin = 0;
				int length = 0;
				uint64_t lastUsedTick = 0;

				bool halfOk = false;
				int nHalf = 0;
				int nLoHalf = 0;
				int nHiHalf = 0;
				DescaleOperator baseLoHalf[kFamilyCount];
				DescaleOperator baseHiHalf[kFamilyCount];
				DescaleOperator halfOps[kKernelCount][2];

				bool quarterOk = false;
				int nQ = 0;
				int nLoQ = 0;
				int nHiQ = 0;
				DescaleOperator baseLoQ[kFamilyCount];
				DescaleOperator baseHiQ[kFamilyCount];
				DescaleOperator qOps[kFamilyCount][4];

				void Build(int axisLen, int orig, int len, TapMemo& memo)
				{
					N = axisLen;
					origin = orig;
					length = len;
					halfOk = false;
					quarterOk = false;

					int repCount = 0;
					const int* reps = RepresentativeKernels(repCount);

					// Half-resolution (n = N / 2) operators.
					nHalf = N / 2;
					nLoHalf = RoundToInt(0.92 * static_cast<double>(nHalf));
					nHiHalf = RoundToInt(1.08 * static_cast<double>(nHalf));
					if (N >= 32 && nHalf >= 16 && nLoHalf > 1 && nLoHalf < nHalf && nHiHalf > nHalf && nHiHalf < N)
					{
						bool ok = true;
						for (int ri = 0; ri < repCount && ok; ++ri)
						{
							if (!baseLoHalf[ri].Build(N, nLoHalf, reps[ri], 0.0, origin, length, &memo) ||
								!baseHiHalf[ri].Build(N, nHiHalf, reps[ri], 0.0, origin, length, &memo))
							{
								ok = false;
							}
						}
						const double phases[2] = { 0.0, 0.25 };
						for (int k = 0; k < kKernelCount && ok; ++k)
						{
							for (int pi = 0; pi < 2 && ok; ++pi)
							{
								if (!halfOps[k][pi].Build(N, nHalf, k, phases[pi], origin, length, &memo))
									ok = false;
							}
						}
						halfOk = ok;
					}

					// Quarter-resolution (n = N / 4) operators.
					nQ = N / 4;
					nLoQ = RoundToInt(0.92 * static_cast<double>(nQ));
					nHiQ = RoundToInt(1.08 * static_cast<double>(nQ));
					if (N >= 64 && nQ >= 16 && nLoQ > 1 && nLoQ < nQ && nHiQ > nQ && nHiQ < N)
					{
						bool ok = true;
						for (int ri = 0; ri < repCount && ok; ++ri)
						{
							if (!baseLoQ[ri].Build(N, nLoQ, reps[ri], 0.0, origin, length, &memo) ||
								!baseHiQ[ri].Build(N, nHiQ, reps[ri], 0.0, origin, length, &memo))
							{
								ok = false;
							}
						}
						const double qPhases[4] = { 0.0, 0.125, 0.25, 0.375 };
						for (int ri = 0; ri < repCount && ok; ++ri)
						{
							for (int pi = 0; pi < 4 && ok; ++pi)
							{
								if (!qOps[ri][pi].Build(N, nQ, reps[ri], qPhases[pi], origin, length, &memo))
									ok = false;
							}
						}
						quarterOk = ok;
					}
				}

				size_t Bytes() const
				{
					size_t total = sizeof(ChromaAxisOperators);
					for (int ri = 0; ri < kFamilyCount; ++ri)
					{
						total += baseLoHalf[ri].Bytes() + baseHiHalf[ri].Bytes();
						total += baseLoQ[ri].Bytes() + baseHiQ[ri].Bytes();
						for (int pi = 0; pi < 4; ++pi)
							total += qOps[ri][pi].Bytes();
					}
					for (int k = 0; k < kKernelCount; ++k)
						for (int pi = 0; pi < 2; ++pi)
							total += halfOps[k][pi].Bytes();
					return total;
				}
			};

			struct AxisHalfAnalysis
			{
				bool valid = false;
				bool hasDetail = false;
				int informativeLines = 0;
				double bestDip = 0.0;
				double kernelRms[kKernelCount][2] = {}; // [kernel][0 = centered 0.0, 1 = cosited +0.25]
				double bestRmsCentered = 0.0;
				double bestRmsCosited = 0.0;
				double sitingScore = 0.0;               // in [-1, +1]: >0 Cosited (+0.25), <0 Centered (0.0)
				double belowPower = 0.0;
				double abovePower = 0.0;
			};

			// Evaluates 2:1 descale dips across all 10 KernelBank kernels and both physical phases (0.0 vs +0.25)
			// against adjacent non-2:1 baselines (0.92 * N/2 and 1.08 * N/2) using bit-exact ResidualBatch.
			inline AxisHalfAnalysis AnalyzeAxisHalf(
				const LineSet& cb,
				const LineSet* cr,
				const LineSet* luma,
				double floorCodes,
				const ChromaEstimatorSettings& s,
				const ChromaAxisOperators& ops)
			{
				AxisHalfAnalysis out;
				if (!ops.halfOk)
					return out;

				const int N = ops.N;
				int repCount = 0;
				(void)RepresentativeKernels(repCount);

				const double dofLo = 1.0 - static_cast<double>(ops.nLoHalf) / static_cast<double>(N);
				const double dofHi = 1.0 - static_cast<double>(ops.nHiHalf) / static_cast<double>(N);
				const double dofHalf = 1.0 - static_cast<double>(ops.nHalf) / static_cast<double>(N);
				const double gateCodes = s.lineGateFactor * floorCodes;
				const double eps = (std::max)(floorCodes / kFloorGuardDiv, 1e-6);

				std::vector<const double*> allLines;
				allLines.reserve(static_cast<size_t>(cb.count) + (cr != nullptr ? static_cast<size_t>(cr->count) : 0u));
				for (int li = 0; li < cb.count; ++li)
					allLines.push_back(cb.Line(li));
				if (cr != nullptr && cr->axisLen == N && cr->origin == cb.origin && cr->length == cb.length)
				{
					for (int li = 0; li < cr->count; ++li)
						allLines.push_back(cr->Line(li));
				}

				std::vector<double> lineDips[kKernelCount][2];
				std::vector<double> lineLogRms[kKernelCount][2];
				std::vector<double> scratch;
				double rssBatch[kResidualBatch];
				double segSumBatch[kResidualBatch * kMaxSegs];
				int segRows[kMaxSegs];

				for (size_t base = 0; base < allLines.size(); base += static_cast<size_t>(kResidualBatch))
				{
					const int batchN = static_cast<int>((std::min)(static_cast<size_t>(kResidualBatch), allLines.size() - base));
					const double* const* batchPtrs = &allLines[base];

					double bestLo[kResidualBatch];
					double bestHi[kResidualBatch];
					for (int g = 0; g < batchN; ++g)
					{
						bestLo[g] = 1e300;
						bestHi[g] = 1e300;
					}

					for (int ri = 0; ri < repCount; ++ri)
					{
						int rowsLo = 0;
						ops.baseLoHalf[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsLo, segSumBatch, segRows);
						if (rowsLo > 0)
						{
							const double denomLo = static_cast<double>(rowsLo) * dofLo;
							for (int g = 0; g < batchN; ++g)
							{
								const double rmsLo = std::sqrt(rssBatch[g] / denomLo);
								if (rmsLo < bestLo[g]) bestLo[g] = rmsLo;
							}
						}
						int rowsHi = 0;
						ops.baseHiHalf[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsHi, segSumBatch, segRows);
						if (rowsHi > 0)
						{
							const double denomHi = static_cast<double>(rowsHi) * dofHi;
							for (int g = 0; g < batchN; ++g)
							{
								const double rmsHi = std::sqrt(rssBatch[g] / denomHi);
								if (rmsHi < bestHi[g]) bestHi[g] = rmsHi;
							}
						}
					}

					const double* infoPtrs[kResidualBatch];
					double infoSafeBase[kResidualBatch];
					int infoN = 0;
					for (int g = 0; g < batchN; ++g)
					{
						if (!(bestLo[g] < 1e299) || !(bestHi[g] < 1e299))
							continue;
						const double baseRms = std::sqrt(bestLo[g] * bestHi[g]);
						if (!std::isfinite(baseRms) || baseRms < gateCodes)
							continue;
						infoPtrs[infoN] = batchPtrs[g];
						infoSafeBase[infoN] = (std::max)(baseRms, eps);
						++infoN;
					}

					if (infoN <= 0)
						continue;
					out.informativeLines += infoN;

					for (int k = 0; k < kKernelCount; ++k)
					{
						for (int pi = 0; pi < 2; ++pi)
						{
							int rows = 0;
							ops.halfOps[k][pi].ResidualBatch(infoPtrs, infoN, scratch, rssBatch, rows, segSumBatch, segRows);
							if (rows <= 0)
								continue;
							const double denomHalf = static_cast<double>(rows) * dofHalf;
							for (int g = 0; g < infoN; ++g)
							{
								const double rms = std::sqrt(rssBatch[g] / denomHalf);
								const double safeDipRms = (std::max)(rms, eps);
								const double safeKernelRms = (std::max)(rms, 1e-12);
								lineDips[k][pi].push_back(std::log(safeDipRms / infoSafeBase[g]));
								lineLogRms[k][pi].push_back(std::log(safeKernelRms));
							}
						}
					}
				}

				out.valid = true;
				ComputeChromaBandPowers(cb, cr, out.belowPower, out.abovePower);

				if (out.informativeLines <= 0)
				{
					out.hasDetail = false;
					return out;
				}
				out.hasDetail = true;

				double bestDip = 1e300;
				double bestCentered = 1e300;
				double bestCosited = 1e300;
				for (int k = 0; k < kKernelCount; ++k)
				{
					for (int pi = 0; pi < 2; ++pi)
					{
						const int cnt = static_cast<int>(lineDips[k][pi].size());
						if (cnt <= 0)
							continue;
						const double dip = RobustStats::TrimmedMeanOf(lineDips[k][pi].data(), cnt, kTrimFraction);
						const double rms = std::exp(RobustStats::TrimmedMeanOf(lineLogRms[k][pi].data(), cnt, kTrimFraction));
						out.kernelRms[k][pi] = rms;
						if (dip < bestDip)
							bestDip = dip;
						if (pi == 0 && rms < bestCentered)
							bestCentered = rms;
						if (pi == 1 && rms < bestCosited)
							bestCosited = rms;
					}
				}

				out.bestDip = bestDip < 1e299 ? bestDip : 0.0;
				out.bestRmsCentered = bestCentered < 1e299 ? bestCentered : 0.0;
				out.bestRmsCosited = bestCosited < 1e299 ? bestCosited : 0.0;

				const double denomPhase = (std::max)((std::max)(out.bestRmsCentered, out.bestRmsCosited), 1e-12);
				const double descaleRel = (out.bestRmsCentered - out.bestRmsCosited) / denomPhase;
				const double parityRel = ComputeAxisParitySitingScore(cb, cr, luma, floorCodes);
				// Fuse DescaleOperator phase residual separation with symmetric-kernel parity asymmetry.
				// Agreement reinforces; contradiction cancels toward 0 so ambiguous siting yields Unknown.
				out.sitingScore = 0.5 * (descaleRel + parityRel);
				return out;
			}

			// Evaluates 1/4-res dip (n = N/4) across representative kernels and two-stage siting phases
			// {0.0, 0.125, 0.25, 0.375} vs adjacent baselines (0.92 * N/4 and 1.08 * N/4) using ResidualBatch.
			inline bool EvaluateAxisQuarterDip(
				const LineSet& cb,
				const LineSet* cr,
				double floorCodes,
				const ChromaEstimatorSettings& s,
				const ChromaAxisOperators& ops,
				double& outDip)
			{
				outDip = 0.0;
				if (!ops.quarterOk)
					return false;

				const int N = ops.N;
				int repCount = 0;
				(void)RepresentativeKernels(repCount);

				const double dofLo = 1.0 - static_cast<double>(ops.nLoQ) / static_cast<double>(N);
				const double dofHi = 1.0 - static_cast<double>(ops.nHiQ) / static_cast<double>(N);
				const double dofQ = 1.0 - static_cast<double>(ops.nQ) / static_cast<double>(N);
				const double gateCodes = s.lineGateFactor * floorCodes;
				const double eps = (std::max)(floorCodes / kFloorGuardDiv, 1e-6);

				std::vector<const double*> allLines;
				allLines.reserve(static_cast<size_t>(cb.count) + (cr != nullptr ? static_cast<size_t>(cr->count) : 0u));
				for (int li = 0; li < cb.count; ++li)
					allLines.push_back(cb.Line(li));
				if (cr != nullptr && cr->axisLen == N && cr->origin == cb.origin && cr->length == cb.length)
				{
					for (int li = 0; li < cr->count; ++li)
						allLines.push_back(cr->Line(li));
				}

				std::vector<double> dips[kFamilyCount][4];
				std::vector<double> scratch;
				double rssBatch[kResidualBatch];
				double segSumBatch[kResidualBatch * kMaxSegs];
				int segRows[kMaxSegs];
				int infoLines = 0;

				for (size_t base = 0; base < allLines.size(); base += static_cast<size_t>(kResidualBatch))
				{
					const int batchN = static_cast<int>((std::min)(static_cast<size_t>(kResidualBatch), allLines.size() - base));
					const double* const* batchPtrs = &allLines[base];

					double bestLo[kResidualBatch];
					double bestHi[kResidualBatch];
					for (int g = 0; g < batchN; ++g)
					{
						bestLo[g] = 1e300;
						bestHi[g] = 1e300;
					}

					for (int ri = 0; ri < repCount; ++ri)
					{
						int rowsLo = 0;
						ops.baseLoQ[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsLo, segSumBatch, segRows);
						if (rowsLo > 0)
						{
							const double denomLo = static_cast<double>(rowsLo) * dofLo;
							for (int g = 0; g < batchN; ++g)
							{
								const double rmsLo = std::sqrt(rssBatch[g] / denomLo);
								if (rmsLo < bestLo[g]) bestLo[g] = rmsLo;
							}
						}
						int rowsHi = 0;
						ops.baseHiQ[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsHi, segSumBatch, segRows);
						if (rowsHi > 0)
						{
							const double denomHi = static_cast<double>(rowsHi) * dofHi;
							for (int g = 0; g < batchN; ++g)
							{
								const double rmsHi = std::sqrt(rssBatch[g] / denomHi);
								if (rmsHi < bestHi[g]) bestHi[g] = rmsHi;
							}
						}
					}

					const double* infoPtrs[kResidualBatch];
					double infoSafeBase[kResidualBatch];
					int infoN = 0;
					for (int g = 0; g < batchN; ++g)
					{
						if (!(bestLo[g] < 1e299) || !(bestHi[g] < 1e299))
							continue;
						const double baseRms = std::sqrt(bestLo[g] * bestHi[g]);
						if (!std::isfinite(baseRms) || baseRms < gateCodes)
							continue;
						infoPtrs[infoN] = batchPtrs[g];
						infoSafeBase[infoN] = (std::max)(baseRms, eps);
						++infoN;
					}

					if (infoN <= 0)
						continue;
					infoLines += infoN;

					for (int ri = 0; ri < repCount; ++ri)
					{
						for (int pi = 0; pi < 4; ++pi)
						{
							int rows = 0;
							ops.qOps[ri][pi].ResidualBatch(infoPtrs, infoN, scratch, rssBatch, rows, segSumBatch, segRows);
							if (rows <= 0)
								continue;
							const double denomQ = static_cast<double>(rows) * dofQ;
							for (int g = 0; g < infoN; ++g)
							{
								const double rms = std::sqrt(rssBatch[g] / denomQ);
								const double safeRms = (std::max)(rms, eps);
								dips[ri][pi].push_back(std::log(safeRms / infoSafeBase[g]));
							}
						}
					}
				}

				if (infoLines <= 0)
					return false;

				double bestDip = 1e300;
				for (int ri = 0; ri < repCount; ++ri)
				{
					for (int pi = 0; pi < 4; ++pi)
					{
						const int cnt = static_cast<int>(dips[ri][pi].size());
						if (cnt <= 0)
							continue;
						const double d = RobustStats::TrimmedMeanOf(dips[ri][pi].data(), cnt, kTrimFraction);
						if (d < bestDip)
							bestDip = d;
					}
				}
				if (!(bestDip < 1e299))
					return false;
				outDip = bestDip;
				return true;
			}

			inline ChromaUpsamplerFamily UpsamplerFamilyFromKernelFamily(int kernelFamily)
			{
				switch (kernelFamily)
				{
				case 0: return ChromaUpsamplerFamily::Bilinear;
				case 1: return ChromaUpsamplerFamily::Bicubic;
				case 2: return ChromaUpsamplerFamily::Lanczos;
				case 3: return ChromaUpsamplerFamily::Spline;
				default: return ChromaUpsamplerFamily::Unknown;
				}
			}

			// SID-SOTA-6: Algebraic Point/Nearest-Neighbor (zero-order hold) & MPEG-2/4 Linear FIR
			// ([3,1]/4 centered / field-shifted, [1,2,1]/4 cosited) chroma upsampler probe.
			struct BoxMpegProbeResult
			{
				bool detected = false;
				ChromaUpsamplerFamily family = ChromaUpsamplerFamily::Unknown;
				double dip = 0.0;
				double rms = 0.0;
				double margin = 0.0;
				bool sitingKnown = false;
				bool cosited = false; // true = cosited ([1,2,1]/4), false = centered ([3,1]/4)
				double sitingMargin = 0.0;
			};

			inline BoxMpegProbeResult ProbeAxisBoxAndMpeg(
				const LineSet& cb,
				const LineSet* cr,
				double floorCodes,
				const ChromaEstimatorSettings& s,
				const ChromaAxisOperators& ops,
				const AxisHalfAnalysis& axHalf,
				TapMemo& memo)
			{
				BoxMpegProbeResult out;
				const double eps = (std::max)(floorCodes / kFloorGuardDiv, 1e-6);

				// 1. Point / Nearest-Neighbor (line-duplication / zero-order hold) exact algebraic probe:
				//    c[2m+1] == c[2m] => same-pair diff RMS <= lineGateFactor * floorCodes while
				//    cross-pair step RMS >= 2 * lineGateFactor * floorCodes and cross/same >= 4 (12 dB).
				//    Tests both even-origin and odd-origin pairing so crop/origin shifts never blind detection.
				double eoSq = 0.0, oeSq = 0.0;
				int64_t eoCnt = 0, oeCnt = 0;
				auto accumPairs = [&](const LineSet& ls) {
					for (int li = 0; li < ls.count; ++li)
					{
						const double* y = ls.Line(li);
						for (int j = 0; j + 1 < ls.length; ++j)
						{
							const double d = y[j + 1] - y[j];
							const int parity = (((ls.origin + j) % 2) + 2) % 2;
							if (parity == 0)
							{
								eoSq += d * d;
								++eoCnt;
							}
							else
							{
								oeSq += d * d;
								++oeCnt;
							}
						}
					}
				};
				accumPairs(cb);
				if (cr != nullptr && cr->axisLen == cb.axisLen && cr->origin == cb.origin && cr->length == cb.length)
					accumPairs(*cr);

				if (eoCnt >= 16 && oeCnt >= 16)
				{
					const double rmsEO = std::sqrt(eoSq / static_cast<double>(eoCnt));
					const double rmsOE = std::sqrt(oeSq / static_cast<double>(oeCnt));
					const double rmsSame = (std::min)(rmsEO, rmsOE);
					const double rmsCross = (std::max)(rmsEO, rmsOE);
					// Exact first-difference noise std for independent pixel noise sigma = floorCodes is sqrt(2) * floorCodes.
					const double diffNoiseFloor = std::sqrt(2.0) * floorCodes;
					const double sameGate = s.lineGateFactor * diffNoiseFloor;
					const double crossGate = 2.0 * sameGate;
					const double sepRatio = rmsCross / (std::max)(rmsSame, eps);
					// Elementary 2^2 = 4.0 (12 dB) ratio prevents smooth gradients, bilinear (ratio <= 2), or native textures from triggering.
					if (rmsSame <= sameGate && rmsCross >= crossGate && sepRatio >= 4.0)
					{
						out.detected = true;
						out.family = ChromaUpsamplerFamily::PointNearest;
						out.rms = rmsSame;
						out.dip = std::log((std::max)(rmsSame, eps) / rmsCross);
						out.margin = ClampD((rmsCross - rmsSame) / (std::max)(rmsCross, 1e-12), 0.0, 1.0);
						out.sitingKnown = false;
						return out;
					}
				}

				// 2. MPEG-2/4 Linear FIR probe: evaluates bilinear (kernel 0) across all 4 physical MPEG phases:
				//    0.0 = centered [3,1]/4, +0.25 = cosited [1,2,1]/4, +-0.125 = MPEG-2 field-shifted [3,1]/4.
				if (!ops.halfOk)
					return out;

				const int N = ops.N;
				int repCount = 0;
				(void)RepresentativeKernels(repCount);
				const double dofLo = 1.0 - static_cast<double>(ops.nLoHalf) / static_cast<double>(N);
				const double dofHi = 1.0 - static_cast<double>(ops.nHiHalf) / static_cast<double>(N);
				const double dofHalf = 1.0 - static_cast<double>(ops.nHalf) / static_cast<double>(N);
				const double gateCodes = s.lineGateFactor * floorCodes;

				DescaleOperator opFieldPos;
				DescaleOperator opFieldNeg;
				const bool fieldPosOk = opFieldPos.Build(N, ops.nHalf, 0, 0.125, ops.origin, ops.length, &memo);
				const bool fieldNegOk = opFieldNeg.Build(N, ops.nHalf, 0, -0.125, ops.origin, ops.length, &memo);

				const DescaleOperator* mpegOps[4] = {
					&ops.halfOps[0][0],
					&ops.halfOps[0][1],
					fieldPosOk ? &opFieldPos : nullptr,
					fieldNegOk ? &opFieldNeg : nullptr
				};

				std::vector<const double*> allLines;
				allLines.reserve(static_cast<size_t>(cb.count) + (cr != nullptr ? static_cast<size_t>(cr->count) : 0u));
				for (int li = 0; li < cb.count; ++li)
					allLines.push_back(cb.Line(li));
				if (cr != nullptr && cr->axisLen == N && cr->origin == cb.origin && cr->length == cb.length)
				{
					for (int li = 0; li < cr->count; ++li)
						allLines.push_back(cr->Line(li));
				}

				std::vector<double> mpegDips[4];
				std::vector<double> mpegLogRms[4];
				std::vector<double> scratch;
				double rssBatch[kResidualBatch];
				double segSumBatch[kResidualBatch * kMaxSegs];
				int segRows[kMaxSegs];

				for (size_t base = 0; base < allLines.size(); base += static_cast<size_t>(kResidualBatch))
				{
					const int batchN = static_cast<int>((std::min)(static_cast<size_t>(kResidualBatch), allLines.size() - base));
					const double* const* batchPtrs = &allLines[base];
					double bestLo[kResidualBatch];
					double bestHi[kResidualBatch];
					for (int g = 0; g < batchN; ++g)
					{
						bestLo[g] = 1e300;
						bestHi[g] = 1e300;
					}
					for (int ri = 0; ri < repCount; ++ri)
					{
						int rowsLo = 0;
						ops.baseLoHalf[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsLo, segSumBatch, segRows);
						if (rowsLo > 0)
						{
							const double denomLo = static_cast<double>(rowsLo) * dofLo;
							for (int g = 0; g < batchN; ++g)
							{
								const double r = std::sqrt(rssBatch[g] / denomLo);
								if (r < bestLo[g]) bestLo[g] = r;
							}
						}
						int rowsHi = 0;
						ops.baseHiHalf[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsHi, segSumBatch, segRows);
						if (rowsHi > 0)
						{
							const double denomHi = static_cast<double>(rowsHi) * dofHi;
							for (int g = 0; g < batchN; ++g)
							{
								const double r = std::sqrt(rssBatch[g] / denomHi);
								if (r < bestHi[g]) bestHi[g] = r;
							}
						}
					}

					const double* infoPtrs[kResidualBatch];
					double infoSafeBase[kResidualBatch];
					int infoN = 0;
					for (int g = 0; g < batchN; ++g)
					{
						if (!(bestLo[g] < 1e299) || !(bestHi[g] < 1e299))
							continue;
						const double baseRms = std::sqrt(bestLo[g] * bestHi[g]);
						if (!std::isfinite(baseRms) || baseRms < gateCodes)
							continue;
						infoPtrs[infoN] = batchPtrs[g];
						infoSafeBase[infoN] = (std::max)(baseRms, eps);
						++infoN;
					}
					if (infoN <= 0)
						continue;

					for (int pi = 0; pi < 4; ++pi)
					{
						if (mpegOps[pi] == nullptr)
							continue;
						int rows = 0;
						mpegOps[pi]->ResidualBatch(infoPtrs, infoN, scratch, rssBatch, rows, segSumBatch, segRows);
						if (rows <= 0)
							continue;
						const double denomHalf = static_cast<double>(rows) * dofHalf;
						for (int g = 0; g < infoN; ++g)
						{
							const double rms = std::sqrt(rssBatch[g] / denomHalf);
							mpegDips[pi].push_back(std::log((std::max)(rms, eps) / infoSafeBase[g]));
							mpegLogRms[pi].push_back(std::log((std::max)(rms, 1e-12)));
						}
					}
				}

				int bestPi = -1;
				double bestDip = 1e300;
				double bestRms = 1e300;
				double phaseDip[4] = { 1e300, 1e300, 1e300, 1e300 };
				for (int pi = 0; pi < 4; ++pi)
				{
					const int cnt = static_cast<int>(mpegDips[pi].size());
					if (cnt <= 0)
						continue;
					const double d = RobustStats::TrimmedMeanOf(mpegDips[pi].data(), cnt, kTrimFraction);
					const double r = std::exp(RobustStats::TrimmedMeanOf(mpegLogRms[pi].data(), cnt, kTrimFraction));
					phaseDip[pi] = d;
					if (d < bestDip)
					{
						bestDip = d;
						bestRms = r;
						bestPi = pi;
					}
				}

				if (bestPi < 0 || !(bestDip <= s.dipConfirmThreshold))
					return out;

				// Require MPEG linear (bilinear family) to beat every higher-order kernel (Bicubic/Lanczos/Spline)
				// by at least kernelMarginMin so higher-order Lanczos/Bicubic chroma never misclassifies as MpegLinear.
				double bestHigherOrderRms = 1e300;
				for (int k = 1; k < kKernelCount; ++k)
				{
					for (int pi = 0; pi < 2; ++pi)
					{
						const double rk = axHalf.kernelRms[k][pi];
						if (rk > 0.0 && rk < bestHigherOrderRms)
							bestHigherOrderRms = rk;
					}
				}

				if (bestHigherOrderRms < 1e299)
				{
					const double margin = ClampD((bestHigherOrderRms - bestRms) / (std::max)(bestHigherOrderRms, 1e-12), 0.0, 1.0);
					if (margin >= s.kernelMarginMin)
					{
						out.detected = true;
						out.family = ChromaUpsamplerFamily::MpegLinear;
						out.dip = bestDip;
						out.rms = bestRms;
						out.margin = margin;
						if ((bestPi == 0 || bestPi == 1) && phaseDip[0] < 1e299 && phaseDip[1] < 1e299)
						{
							const double sep = std::fabs(phaseDip[0] - phaseDip[1]);
							out.sitingMargin = sep;
							if (sep >= s.sitingMarginMin)
							{
								out.sitingKnown = true;
								out.cosited = (bestPi == 1);
							}
						}
						else
						{
							out.sitingKnown = false;
						}
					}
				}
				return out;
			}

			// SID-SOTA-6: Arbitrary luma-coupled chroma grid corroboration (e.g., 720p -> 2160p or 1440p -> 2160p
			// where expected 4:2:0 chroma size is targetN = knownLumaN / 2). Strictly gated on signal std, baseline
			// detail, and absence of above-targetRatio high-band native chroma dominance.
			inline bool EvaluateAxisTargetDip(
				const LineSet& cb,
				const LineSet* cr,
				int targetN,
				double floorCodes,
				const ChromaEstimatorSettings& s,
				TapMemo& memo,
				double& outDip,
				bool& outHighDom)
			{
				outDip = 0.0;
				outHighDom = false;
				const int N = cb.axisLen;
				if (targetN < 16 || targetN >= N)
					return false;

				const int nLo = RoundToInt(0.92 * static_cast<double>(targetN));
				const int nHi = RoundToInt(1.08 * static_cast<double>(targetN));
				if (nLo <= 1 || nLo >= targetN || nHi <= targetN || nHi >= N)
					return false;

				int repCount = 0;
				const int* reps = RepresentativeKernels(repCount);
				DescaleOperator baseLo[kFamilyCount];
				DescaleOperator baseHi[kFamilyCount];
				DescaleOperator targetOps[kFamilyCount][4];
				const double phases[4] = { 0.0, 0.125, 0.25, 0.375 };

				for (int ri = 0; ri < repCount; ++ri)
				{
					if (!baseLo[ri].Build(N, nLo, reps[ri], 0.0, cb.origin, cb.length, &memo) ||
						!baseHi[ri].Build(N, nHi, reps[ri], 0.0, cb.origin, cb.length, &memo))
					{
						return false;
					}
					for (int pi = 0; pi < 4; ++pi)
					{
						if (!targetOps[ri][pi].Build(N, targetN, reps[ri], phases[pi], cb.origin, cb.length, &memo))
							return false;
					}
				}

				// Check above-targetRatio spectral dominance so native broadband chroma never falsely corroborates.
				const double ratio = static_cast<double>(targetN) / static_cast<double>(N);
				const double fc = 0.5 * ratio;
				int fftN = 64;
				while (fftN * 2 <= cb.length && fftN * 2 <= 1024)
					fftN *= 2;
				if (cb.length >= 32)
				{
					const int winLen = cb.length < fftN ? cb.length : fftN;
					std::vector<double> win(static_cast<size_t>(winLen), 0.0);
					double winEnergy = 0.0;
					for (int i = 0; i < winLen; ++i)
					{
						const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(winLen));
						win[static_cast<size_t>(i)] = w;
						winEnergy += w * w;
					}
					if (winEnergy > 0.0)
					{
						FftTwiddles tw;
						tw.Build(static_cast<size_t>(fftN));
						const int half = fftN / 2;
						std::vector<double> psd(static_cast<size_t>(half + 1), 0.0);
						std::vector<double> re(static_cast<size_t>(fftN), 0.0);
						std::vector<double> im(static_cast<size_t>(fftN), 0.0);
						int totalSegs = 0;
						auto accumPlane = [&](const LineSet& ls) {
							const int curWinLen = ls.length < fftN ? ls.length : fftN;
							for (int li = 0; li < ls.count; ++li)
							{
								const double* line = ls.Line(li);
								for (int start = 0; start + curWinLen <= ls.length; start += curWinLen)
								{
									double mean = 0.0;
									for (int i = 0; i < curWinLen; ++i)
										mean += line[start + i];
									mean /= static_cast<double>(curWinLen);
									for (int i = 0; i < fftN; ++i)
									{
										re[static_cast<size_t>(i)] = (i < curWinLen) ? (line[start + i] - mean) * win[static_cast<size_t>(i)] : 0.0;
										im[static_cast<size_t>(i)] = 0.0;
									}
									FftRadix2(re, im, tw);
									for (int k = 0; k <= half; ++k)
									{
										const double rk = re[static_cast<size_t>(k)];
										const double ik = im[static_cast<size_t>(k)];
										const double weight = (k == 0 || k == half) ? 1.0 : 2.0;
										psd[static_cast<size_t>(k)] += (rk * rk + ik * ik) * weight;
									}
									++totalSegs;
								}
							}
						};
						accumPlane(cb);
						if (cr != nullptr && cr->length == cb.length)
							accumPlane(*cr);
						if (totalSegs > 0)
						{
							const double norm = static_cast<double>(totalSegs) * winEnergy * 2.0;
							for (size_t k = 0; k < psd.size(); ++k)
								psd[k] /= norm;
							const int bLo = (std::max)(1, static_cast<int>(std::ceil(0.32 * fc * static_cast<double>(fftN))));
							const int bHi = (std::min)(half - 1, static_cast<int>(std::floor(0.88 * fc * static_cast<double>(fftN))));
							const int aLo = (std::max)(1, static_cast<int>(std::ceil(1.12 * fc * static_cast<double>(fftN))));
							const int aHi = (std::min)(half - 1, static_cast<int>(std::floor(0.48 * static_cast<double>(fftN))));
							if (bLo <= bHi && aLo <= aHi)
							{
								double bSum = 0.0, aSum = 0.0;
								for (int k = bLo; k <= bHi; ++k) bSum += psd[static_cast<size_t>(k)];
								for (int k = aLo; k <= aHi; ++k) aSum += psd[static_cast<size_t>(k)];
								const double bMean = bSum / static_cast<double>(bHi - bLo + 1);
								const double aMean = aSum / static_cast<double>(aHi - aLo + 1);
								const double nativeAboveGateP = s.nativeAboveRatioGate * floorCodes * floorCodes;
								if (aMean > nativeAboveGateP && aMean >= bMean)
									outHighDom = true;
							}
						}
					}
				}

				const double dofLo = 1.0 - static_cast<double>(nLo) / static_cast<double>(N);
				const double dofHi = 1.0 - static_cast<double>(nHi) / static_cast<double>(N);
				const double dofT = 1.0 - static_cast<double>(targetN) / static_cast<double>(N);
				const double gateCodes = s.lineGateFactor * floorCodes;
				const double eps = (std::max)(floorCodes / kFloorGuardDiv, 1e-6);

				std::vector<const double*> allLines;
				allLines.reserve(static_cast<size_t>(cb.count) + (cr != nullptr ? static_cast<size_t>(cr->count) : 0u));
				for (int li = 0; li < cb.count; ++li)
					allLines.push_back(cb.Line(li));
				if (cr != nullptr && cr->axisLen == N && cr->origin == cb.origin && cr->length == cb.length)
				{
					for (int li = 0; li < cr->count; ++li)
						allLines.push_back(cr->Line(li));
				}

				std::vector<double> dips[kFamilyCount][4];
				std::vector<double> scratch;
				double rssBatch[kResidualBatch];
				double segSumBatch[kResidualBatch * kMaxSegs];
				int segRows[kMaxSegs];
				int infoLines = 0;

				for (size_t base = 0; base < allLines.size(); base += static_cast<size_t>(kResidualBatch))
				{
					const int batchN = static_cast<int>((std::min)(static_cast<size_t>(kResidualBatch), allLines.size() - base));
					const double* const* batchPtrs = &allLines[base];
					double bestLo[kResidualBatch];
					double bestHi[kResidualBatch];
					for (int g = 0; g < batchN; ++g)
					{
						bestLo[g] = 1e300;
						bestHi[g] = 1e300;
					}
					for (int ri = 0; ri < repCount; ++ri)
					{
						int rowsLo = 0;
						baseLo[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsLo, segSumBatch, segRows);
						if (rowsLo > 0)
						{
							const double denomLo = static_cast<double>(rowsLo) * dofLo;
							for (int g = 0; g < batchN; ++g)
							{
								const double r = std::sqrt(rssBatch[g] / denomLo);
								if (r < bestLo[g]) bestLo[g] = r;
							}
						}
						int rowsHi = 0;
						baseHi[ri].ResidualBatch(batchPtrs, batchN, scratch, rssBatch, rowsHi, segSumBatch, segRows);
						if (rowsHi > 0)
						{
							const double denomHi = static_cast<double>(rowsHi) * dofHi;
							for (int g = 0; g < batchN; ++g)
							{
								const double r = std::sqrt(rssBatch[g] / denomHi);
								if (r < bestHi[g]) bestHi[g] = r;
							}
						}
					}

					const double* infoPtrs[kResidualBatch];
					double infoSafeBase[kResidualBatch];
					int infoN = 0;
					for (int g = 0; g < batchN; ++g)
					{
						if (!(bestLo[g] < 1e299) || !(bestHi[g] < 1e299))
							continue;
						const double baseRms = std::sqrt(bestLo[g] * bestHi[g]);
						if (!std::isfinite(baseRms) || baseRms < gateCodes)
							continue;
						infoPtrs[infoN] = batchPtrs[g];
						infoSafeBase[infoN] = (std::max)(baseRms, eps);
						++infoN;
					}
					if (infoN <= 0)
						continue;
					infoLines += infoN;

					for (int ri = 0; ri < repCount; ++ri)
					{
						for (int pi = 0; pi < 4; ++pi)
						{
							int rows = 0;
							targetOps[ri][pi].ResidualBatch(infoPtrs, infoN, scratch, rssBatch, rows, segSumBatch, segRows);
							if (rows <= 0)
								continue;
							const double denomT = static_cast<double>(rows) * dofT;
							for (int g = 0; g < infoN; ++g)
							{
								const double rms = std::sqrt(rssBatch[g] / denomT);
								dips[ri][pi].push_back(std::log((std::max)(rms, eps) / infoSafeBase[g]));
							}
						}
					}
				}

				if (infoLines <= 0)
					return false;

				double bestDip = 1e300;
				for (int ri = 0; ri < repCount; ++ri)
				{
					for (int pi = 0; pi < 4; ++pi)
					{
						const int cnt = static_cast<int>(dips[ri][pi].size());
						if (cnt <= 0)
							continue;
						const double d = RobustStats::TrimmedMeanOf(dips[ri][pi].data(), cnt, kTrimFraction);
						if (d < bestDip)
							bestDip = d;
					}
				}
				if (!(bestDip < 1e299))
					return false;
				outDip = bestDip;
				return true;
			}
		} // namespace detail_chroma_estimator

		// Bounded deterministic cache of pre-factored chroma DescaleOperators keyed by (N, origin, length).
		// Capacity is strictly bounded to [1, 16] entries with deterministic LRU eviction and visible stats (Rule 2 & 4).
		struct ChromaOperatorCacheStats
		{
			uint64_t hits = 0;
			uint64_t misses = 0;
			uint64_t evictions = 0;
			size_t entryCount = 0;
			size_t totalBytes = 0;
		};

		class ChromaOperatorCache
		{
		public:
			explicit ChromaOperatorCache(size_t maxEntries = 4)
				: maxEntries_(maxEntries < 1 ? 1 : (maxEntries > 16 ? 16 : maxEntries))
				, memo_(4096)
			{
				entries_.reserve(maxEntries_);
			}

			const ChromaOperatorCacheStats& Stats() const { return stats_; }
			size_t EntryCount() const { return entries_.size(); }
			size_t MaxEntries() const { return maxEntries_; }

			void Clear()
			{
				entries_.clear();
				memo_ = TapMemo(4096);
				accessTick_ = 0;
				stats_ = ChromaOperatorCacheStats{};
			}

			const detail_chroma_estimator::ChromaAxisOperators& GetOrBuild(int N, int origin, int length)
			{
				for (size_t i = 0; i < entries_.size(); ++i)
				{
					if (entries_[i].N == N && entries_[i].origin == origin && entries_[i].length == length)
					{
						entries_[i].lastUsedTick = ++accessTick_;
						++stats_.hits;
						return entries_[i];
					}
				}

				++stats_.misses;
				size_t slot = entries_.size();
				if (entries_.size() >= maxEntries_)
				{
					// Deterministic LRU eviction: replace entry with smallest lastUsedTick.
					slot = 0;
					for (size_t i = 1; i < entries_.size(); ++i)
					{
						if (entries_[i].lastUsedTick < entries_[slot].lastUsedTick)
							slot = i;
					}
					++stats_.evictions;
				}
				else
				{
					entries_.emplace_back();
				}

				entries_[slot].Build(N, origin, length, memo_);
				entries_[slot].lastUsedTick = ++accessTick_;
				UpdateAccounting();
				return entries_[slot];
			}

		private:
			void UpdateAccounting()
			{
				stats_.entryCount = entries_.size();
				size_t bytes = memo_.Bytes();
				for (size_t i = 0; i < entries_.size(); ++i)
					bytes += entries_[i].Bytes();
				stats_.totalBytes = bytes;
			}

			size_t maxEntries_ = 4;
			TapMemo memo_;
			uint64_t accessTick_ = 0;
			std::vector<detail_chroma_estimator::ChromaAxisOperators> entries_;
			ChromaOperatorCacheStats stats_;
		};

		// ============================================================================================================
		// EstimateChromaChain: Origin (4:4:4 / 4:2:2 / 4:2:0), Kernel, Siting, and 1/4-Res Corroboration
		// ============================================================================================================
		inline ChromaChainEstimate EstimateChromaChain(
			const ChromaSampleInput& in,
			const ChromaEstimatorSettings& s = ChromaEstimatorSettings(),
			ChromaOperatorCache* cache = nullptr)
		{
			ChromaChainEstimate est;
			if (!s.enabled)
			{
				est.reason = ChromaEstimatorReason::Disabled;
				est.quarter.reason = ChromaEstimatorReason::Disabled;
				est.lumaCoupled.reason = ChromaEstimatorReason::Disabled;
				return est;
			}

			const bool is422 = (in.deliveredSampling == DeliveredChromaSampling::Yuv422);
			const bool is420 = (in.deliveredSampling == DeliveredChromaSampling::Yuv420);
			const bool hasCbH = (in.cbH.count > 0 || !in.cbH.data.empty());
			const bool hasCbV = (in.cbV.count > 0 || !in.cbV.data.empty());

			if ((in.deliveredSampling != DeliveredChromaSampling::Yuv444 && !is422 && !is420) ||
				!std::isfinite(in.floorCodes) || !(in.floorCodes > 0.0) ||
				!std::isfinite(s.lineGateFactor) || !(s.lineGateFactor > 1.0) ||
				!std::isfinite(s.dipConfirmThreshold) || !(s.dipConfirmThreshold < 0.0) ||
				!std::isfinite(s.nativeAboveRatioGate) || !(s.nativeAboveRatioGate > 1.0) ||
				!std::isfinite(s.kernelMarginMin) || s.kernelMarginMin < 0.0 ||
				!std::isfinite(s.sitingMarginMin) || s.sitingMarginMin < 0.0)
			{
				est.reason = ChromaEstimatorReason::InvalidInput;
				est.quarter.reason = ChromaEstimatorReason::InvalidInput;
				est.lumaCoupled.reason = ChromaEstimatorReason::InvalidInput;
				return est;
			}

			// Validate chroma LineSets according to delivered transport sampling:
			//   - Yuv444: both cbH and cbV required.
			//   - Yuv422: cbV required (H is already N/2 on transport; cbH validated if supplied).
			//   - Yuv420: at least one of cbH/cbV valid (both validated if supplied).
			const bool cbHOk = hasCbH && detail_chroma_estimator::ValidLineSet(in.cbH, 32) && detail_chroma_estimator::AllFinite(in.cbH);
			const bool cbVOk = hasCbV && detail_chroma_estimator::ValidLineSet(in.cbV, 32) && detail_chroma_estimator::AllFinite(in.cbV);
			if ((!is422 && !is420 && (!cbHOk || !cbVOk)) ||
				(is422 && (!cbVOk || (hasCbH && !cbHOk))) ||
				(is420 && ((!cbHOk && !cbVOk) || (hasCbH && !cbHOk) || (hasCbV && !cbVOk))))
			{
				est.reason = ChromaEstimatorReason::InvalidInput;
				est.quarter.reason = ChromaEstimatorReason::InvalidInput;
				est.lumaCoupled.reason = ChromaEstimatorReason::InvalidInput;
				return est;
			}

			const LineSet* crH = nullptr;
			if (in.crH.count > 0 || !in.crH.data.empty())
			{
				if (!cbHOk ||
					!detail_chroma_estimator::ValidLineSet(in.crH, 32) ||
					!detail_chroma_estimator::AllFinite(in.crH) ||
					in.crH.axisLen != in.cbH.axisLen ||
					in.crH.length != in.cbH.length ||
					in.crH.origin != in.cbH.origin)
				{
					est.reason = ChromaEstimatorReason::InvalidInput;
					est.quarter.reason = ChromaEstimatorReason::InvalidInput;
					est.lumaCoupled.reason = ChromaEstimatorReason::InvalidInput;
					return est;
				}
				crH = &in.crH;
			}
			const LineSet* crV = nullptr;
			if (in.crV.count > 0 || !in.crV.data.empty())
			{
				if (!cbVOk ||
					!detail_chroma_estimator::ValidLineSet(in.crV, 32) ||
					!detail_chroma_estimator::AllFinite(in.crV) ||
					in.crV.axisLen != in.cbV.axisLen ||
					in.crV.length != in.cbV.length ||
					in.crV.origin != in.cbV.origin)
				{
					est.reason = ChromaEstimatorReason::InvalidInput;
					est.quarter.reason = ChromaEstimatorReason::InvalidInput;
					est.lumaCoupled.reason = ChromaEstimatorReason::InvalidInput;
					return est;
				}
				crV = &in.crV;
			}

			const LineSet* lumaH = nullptr;
			if (in.lumaH.count > 0 || !in.lumaH.data.empty())
			{
				if (!cbHOk ||
					!detail_chroma_estimator::ValidLineSet(in.lumaH, 32) ||
					!detail_chroma_estimator::AllFinite(in.lumaH) ||
					in.lumaH.count != in.cbH.count ||
					in.lumaH.length != in.cbH.length ||
					in.lumaH.origin != in.cbH.origin)
				{
					est.reason = ChromaEstimatorReason::InvalidInput;
					est.quarter.reason = ChromaEstimatorReason::InvalidInput;
					est.lumaCoupled.reason = ChromaEstimatorReason::InvalidInput;
					return est;
				}
				lumaH = &in.lumaH;
			}
			const LineSet* lumaV = nullptr;
			if (in.lumaV.count > 0 || !in.lumaV.data.empty())
			{
				if (!cbVOk ||
					!detail_chroma_estimator::ValidLineSet(in.lumaV, 32) ||
					!detail_chroma_estimator::AllFinite(in.lumaV) ||
					in.lumaV.count != in.cbV.count ||
					in.lumaV.length != in.cbV.length ||
					in.lumaV.origin != in.cbV.origin)
				{
					est.reason = ChromaEstimatorReason::InvalidInput;
					est.quarter.reason = ChromaEstimatorReason::InvalidInput;
					est.lumaCoupled.reason = ChromaEstimatorReason::InvalidInput;
					return est;
				}
				lumaV = &in.lumaV;
			}

			// Helper: arbitrary luma-coupled 4:2:0 chroma grid corroboration (SID-SOTA-6, opt-in via knownLumaNH/NV > 0).
			TapMemo localMemo(4096);
			const double sigStdGate = s.lineGateFactor * in.floorCodes;
			const double sigStdH = cbHOk ? detail_chroma_estimator::ComputeAxisSignalStd(in.cbH, crH) : 0.0;
			const double sigStdV = cbVOk ? detail_chroma_estimator::ComputeAxisSignalStd(in.cbV, crV) : 0.0;

			auto evaluateLumaCoupled = [&](bool& outHighDomAny) {
				outHighDomAny = false;
				const bool wantH = (in.knownLumaNH > 0) && cbHOk;
				const bool wantV = (in.knownLumaNV > 0) && cbVOk;
				if (!wantH && !wantV)
				{
					est.lumaCoupled.reason = ChromaEstimatorReason::Disabled;
					return;
				}
				const int targetH = wantH ? (in.knownLumaNH / 2) : 0;
				const int targetV = wantV ? (in.knownLumaNV / 2) : 0;
				est.lumaCoupled.expectedChromaNH = targetH;
				est.lumaCoupled.expectedChromaNV = targetV;

				bool okH = !wantH;
				bool okV = !wantV;
				double dH = 0.0, dV = 0.0;
				bool hdH = false, hdV = false;
				if (wantH && sigStdH > sigStdGate)
					okH = detail_chroma_estimator::EvaluateAxisTargetDip(in.cbH, crH, targetH, in.floorCodes, s, localMemo, dH, hdH);
				if (wantV && sigStdV > sigStdGate)
					okV = detail_chroma_estimator::EvaluateAxisTargetDip(in.cbV, crV, targetV, in.floorCodes, s, localMemo, dV, hdV);
				outHighDomAny = hdH || hdV;

				if (okH && okV)
				{
					est.lumaCoupled.valid = true;
					est.lumaCoupled.dipH = dH;
					est.lumaCoupled.dipV = dV;
					const bool passH = !wantH || (!hdH && dH <= s.dipConfirmThreshold);
					const bool passV = !wantV || (!hdV && dV <= s.dipConfirmThreshold);
					if (passH && passV)
					{
						est.lumaCoupled.corroborated = true;
						const double worstDip = (wantH && wantV) ? (std::max)(dH, dV) : (wantH ? dH : dV);
						est.lumaCoupled.confidence = ClampD(1.0 - std::exp(worstDip), 0.0, 1.0);
						est.lumaCoupled.reason = ChromaEstimatorReason::LumaCoupledChromaCorroborated;
					}
					else
					{
						est.lumaCoupled.corroborated = false;
						est.lumaCoupled.confidence = 0.0;
						est.lumaCoupled.reason = ChromaEstimatorReason::LumaCoupledChromaNotSupported;
					}
				}
				else
				{
					est.lumaCoupled.valid = false;
					est.lumaCoupled.corroborated = false;
					est.lumaCoupled.reason = ChromaEstimatorReason::LumaCoupledChromaNotSupported;
				}
			};

			// SID-SOTA-6 Case 1: Native 4:2:0 delivered directly (not player-upsampled).
			if (is420)
			{
				bool lumaCoupledHighDom = false;
				evaluateLumaCoupled(lumaCoupledHighDom);
				(void)lumaCoupledHighDom;
				est.valid = true;
				est.origin = ChromaOrigin::Subsampled420;
				est.playerUpsampled = false;
				est.kernelIndex = -1;
				est.kernelFamily = -1;
				est.upsamplerFamily = ChromaUpsamplerFamily::Unknown;
				est.siting = ChromaGridSiting::Unknown;
				est.confidence = 1.0;
				est.reason = ChromaEstimatorReason::Native420Delivered;
				est.quarter.reason = ChromaEstimatorReason::QuarterNotSupported;
				return est;
			}

			// Helper: resolve winning kernel and family margin from per-kernel pooled RMS scores.
			auto selectKernel = [&](const double pooledRms[kKernelCount]) {
				double bestFamRms[kFamilyCount] = { 1e300, 1e300, 1e300, 1e300 };
				int bestFamKernel[kFamilyCount] = { -1, -1, -1, -1 };
				for (int k = 0; k < kKernelCount; ++k)
				{
					const int fam = static_cast<int>(Kernel(k).family);
					if (fam >= 0 && fam < kFamilyCount && pooledRms[k] > 0.0 && pooledRms[k] < bestFamRms[fam])
					{
						bestFamRms[fam] = pooledRms[k];
						bestFamKernel[fam] = k;
					}
				}
				int winFam = -1;
				double firstRms = 1e300;
				double secondRms = 1e300;
				for (int f = 0; f < kFamilyCount; ++f)
				{
					if (bestFamRms[f] < firstRms)
					{
						secondRms = firstRms;
						firstRms = bestFamRms[f];
						winFam = f;
					}
					else if (bestFamRms[f] < secondRms)
					{
						secondRms = bestFamRms[f];
					}
				}
				if (winFam >= 0 && secondRms < 1e299)
				{
					est.kernelMargin = ClampD((secondRms - firstRms) / (std::max)(secondRms, 1e-12), 0.0, 1.0);
					if (est.kernelMargin >= s.kernelMarginMin)
					{
						est.kernelIndex = bestFamKernel[winFam];
						est.kernelFamily = winFam;
						est.upsamplerFamily = detail_chroma_estimator::UpsamplerFamilyFromKernelFamily(winFam);
					}
					else
					{
						est.kernelIndex = -1;
						est.kernelFamily = -1;
						est.upsamplerFamily = ChromaUpsamplerFamily::Unknown;
					}
				}
			};

			const double floorP = in.floorCodes * in.floorCodes;
			const double nativeAboveGateP = s.nativeAboveRatioGate * floorP;

			// SID-SOTA-6 Case 2: Delivered 4:2:2 HDMI transport (Apple TV / Shield / Blu-ray 12-bit HDR):
			// H is already N/2 on transport; vertical axis alone decides player-upsampled 4:2:0 vs native 4:2:2.
			if (is422)
			{
				if (!(sigStdV > sigStdGate))
				{
					est.valid = false;
					est.origin = ChromaOrigin::Unknown;
					est.reason = ChromaEstimatorReason::LowChromaDetail;
					est.quarter.reason = ChromaEstimatorReason::LowChromaDetail;
					est.lumaCoupled.reason = ChromaEstimatorReason::LowChromaDetail;
					return est;
				}

				bool lumaCoupledHighDom = false;
				evaluateLumaCoupled(lumaCoupledHighDom);

				detail_chroma_estimator::ChromaAxisOperators localOpsV;
				const detail_chroma_estimator::ChromaAxisOperators* opsV = nullptr;
				if (cache != nullptr)
				{
					opsV = &cache->GetOrBuild(in.cbV.axisLen, in.cbV.origin, in.cbV.length);
				}
				else
				{
					localOpsV.Build(in.cbV.axisLen, in.cbV.origin, in.cbV.length, localMemo);
					opsV = &localOpsV;
				}

				const detail_chroma_estimator::AxisHalfAnalysis axV =
					detail_chroma_estimator::AnalyzeAxisHalf(in.cbV, crV, lumaV, in.floorCodes, s, *opsV);
				if (!axV.valid)
				{
					est.reason = ChromaEstimatorReason::InvalidInput;
					return est;
				}

				detail_chroma_estimator::BoxMpegProbeResult bmV;
				if (s.detectBoxAndMpegChroma)
					bmV = detail_chroma_estimator::ProbeAxisBoxAndMpeg(in.cbV, crV, in.floorCodes, s, *opsV, axV, localMemo);

				est.dipV = axV.bestDip;
				est.quarter.reason = ChromaEstimatorReason::QuarterNotSupported;

				const bool highDomV = (axV.abovePower > nativeAboveGateP) && (axV.abovePower >= axV.belowPower);
				const bool pointV = s.detectBoxAndMpegChroma && bmV.detected && (bmV.family == ChromaUpsamplerFamily::PointNearest);
				const bool mpegV = s.detectBoxAndMpegChroma && bmV.detected && (bmV.family == ChromaUpsamplerFamily::MpegLinear) && !highDomV;
				const bool subV = pointV || mpegV || (axV.hasDetail && (axV.bestDip <= s.dipConfirmThreshold) && !highDomV);

				if (est.lumaCoupled.valid && est.lumaCoupled.corroborated && !subV && !highDomV && !lumaCoupledHighDom)
				{
					est.valid = true;
					est.origin = ChromaOrigin::Subsampled420;
					est.playerUpsampled = true;
					est.dipH = est.lumaCoupled.dipH;
					est.dipV = est.lumaCoupled.dipV;
					est.confidence = est.lumaCoupled.confidence;
					est.reason = ChromaEstimatorReason::LumaCoupledChromaCorroborated;
					return est;
				}

				if (subV)
				{
					est.valid = true;
					est.origin = ChromaOrigin::Subsampled420;
					est.playerUpsampled = true;
					if (pointV)
					{
						est.dipV = bmV.dip;
						est.kernelIndex = -1;
						est.kernelFamily = -1;
						est.upsamplerFamily = ChromaUpsamplerFamily::PointNearest;
						est.kernelMargin = bmV.margin;
						est.siting = ChromaGridSiting::Unknown;
						est.confidence = ClampD(1.0 - std::exp(bmV.dip), 0.0, 1.0);
						est.reason = ChromaEstimatorReason::PointNearestUpsampledConfirmed;
						return est;
					}
					if (mpegV)
					{
						est.dipV = bmV.dip;
						est.kernelIndex = 0; // bilinear / linear FIR family
						est.kernelFamily = 0;
						est.upsamplerFamily = ChromaUpsamplerFamily::MpegLinear;
						est.kernelMargin = bmV.margin;
						est.siting = bmV.sitingKnown ? (bmV.cosited ? ChromaGridSiting::TopLeft : ChromaGridSiting::Left) : ChromaGridSiting::Unknown;
						est.sitingMargin = bmV.sitingKnown ? bmV.sitingMargin : std::fabs(axV.sitingScore);
						est.confidence = ClampD(1.0 - std::exp(bmV.dip), 0.0, 1.0);
						est.reason = ChromaEstimatorReason::MpegLinearUpsampledConfirmed;
						return est;
					}

					const double magV = std::fabs(axV.sitingScore);
					const bool sureV = magV >= s.sitingMarginMin;
					const bool cositedV = axV.sitingScore > 0.0;
					est.sitingMargin = magV;
					// On 4:2:2 transport horizontal is already cosited/4:2:2: cosited V -> TopLeft, centered V -> Left.
					if (sureV)
						est.siting = cositedV ? ChromaGridSiting::TopLeft : ChromaGridSiting::Left;
					else
						est.siting = ChromaGridSiting::Unknown;

					const int phaseIdxV = sureV ? (cositedV ? 1 : 0) : -1;
					double pooledRms[kKernelCount] = {};
					for (int k = 0; k < kKernelCount; ++k)
					{
						pooledRms[k] = phaseIdxV >= 0 ? axV.kernelRms[k][phaseIdxV]
							: (std::min)(axV.kernelRms[k][0], axV.kernelRms[k][1]);
					}
					selectKernel(pooledRms);

					est.confidence = ClampD(1.0 - std::exp(axV.bestDip), 0.0, 1.0);
					if (est.kernelIndex < 0)
						est.reason = ChromaEstimatorReason::KernelUnsure;
					else if (est.siting == ChromaGridSiting::Unknown)
						est.reason = ChromaEstimatorReason::SitingUnsure;
					else
						est.reason = ChromaEstimatorReason::PlayerUpsampled420Confirmed;
					return est;
				}

				// !subV on delivered 4:2:2: confirm Native 4:2:2 ONLY when genuine vertical high-band chroma detail exists.
				if (axV.hasDetail && axV.abovePower > nativeAboveGateP)
				{
					est.valid = true;
					est.origin = ChromaOrigin::Subsampled422;
					est.playerUpsampled = false;
					est.kernelIndex = -1;
					est.kernelFamily = -1;
					est.upsamplerFamily = ChromaUpsamplerFamily::Unknown;
					est.siting = ChromaGridSiting::Unknown;
					est.confidence = ClampD(1.0 - nativeAboveGateP / (axV.abovePower + nativeAboveGateP), 0.0, 1.0);
					est.reason = ChromaEstimatorReason::Native422Confirmed;
					return est;
				}
				est.valid = false;
				est.origin = ChromaOrigin::Unknown;
				est.playerUpsampled = false;
				est.reason = ChromaEstimatorReason::LowChromaDetail;
				return est;
			}

			// Default Case 3: Delivered 4:4:4 (100% backward compatible when detectBoxAndMpegChroma == false && knownLumaN == 0).
			// Gate truly flat/achromatic chroma on raw demeaned signal std BEFORE N/2 baseline gating,
			// so 1/4-res chroma (1080p-in-4K 4:2:0, quiet near N/2) always reaches 1/4-res corroboration.
			if (!(sigStdH > sigStdGate) || !(sigStdV > sigStdGate))
			{
				est.valid = false;
				est.origin = ChromaOrigin::Unknown;
				est.reason = ChromaEstimatorReason::LowChromaDetail;
				est.quarter.reason = ChromaEstimatorReason::LowChromaDetail;
				est.lumaCoupled.reason = ChromaEstimatorReason::LowChromaDetail;
				return est;
			}

			bool lumaCoupledHighDom = false;
			evaluateLumaCoupled(lumaCoupledHighDom);

			// Evaluate H axis completely before acquiring V operators so even a 1-slot cache (maxEntries == 1)
			// never invalidates a live operator reference (Rule 2 lifetime invariant).
			detail_chroma_estimator::ChromaAxisOperators localOps;

			const detail_chroma_estimator::ChromaAxisOperators* opsH = nullptr;
			if (cache != nullptr)
			{
				opsH = &cache->GetOrBuild(in.cbH.axisLen, in.cbH.origin, in.cbH.length);
			}
			else
			{
				localOps.Build(in.cbH.axisLen, in.cbH.origin, in.cbH.length, localMemo);
				opsH = &localOps;
			}
			double qDipH = 0.0;
			const bool qOkH = detail_chroma_estimator::EvaluateAxisQuarterDip(in.cbH, crH, in.floorCodes, s, *opsH, qDipH);
			const detail_chroma_estimator::AxisHalfAnalysis axH =
				detail_chroma_estimator::AnalyzeAxisHalf(in.cbH, crH, lumaH, in.floorCodes, s, *opsH);
			detail_chroma_estimator::BoxMpegProbeResult bmH;
			if (s.detectBoxAndMpegChroma)
				bmH = detail_chroma_estimator::ProbeAxisBoxAndMpeg(in.cbH, crH, in.floorCodes, s, *opsH, axH, localMemo);

			const detail_chroma_estimator::ChromaAxisOperators* opsV = nullptr;
			if (cache != nullptr)
			{
				opsV = &cache->GetOrBuild(in.cbV.axisLen, in.cbV.origin, in.cbV.length);
			}
			else
			{
				localOps.Build(in.cbV.axisLen, in.cbV.origin, in.cbV.length, localMemo);
				opsV = &localOps;
			}
			double qDipV = 0.0;
			const bool qOkV = detail_chroma_estimator::EvaluateAxisQuarterDip(in.cbV, crV, in.floorCodes, s, *opsV, qDipV);
			const detail_chroma_estimator::AxisHalfAnalysis axV =
				detail_chroma_estimator::AnalyzeAxisHalf(in.cbV, crV, lumaV, in.floorCodes, s, *opsV);
			detail_chroma_estimator::BoxMpegProbeResult bmV;
			if (s.detectBoxAndMpegChroma)
				bmV = detail_chroma_estimator::ProbeAxisBoxAndMpeg(in.cbV, crV, in.floorCodes, s, *opsV, axV, localMemo);

			if (qOkH && qOkV)
			{
				est.quarter.valid = true;
				est.quarter.dipH = qDipH;
				est.quarter.dipV = qDipV;
				if (qDipH <= s.dipConfirmThreshold && qDipV <= s.dipConfirmThreshold)
				{
					est.quarter.supportsTwoToOneLumaUpscale = true;
					est.quarter.confidence = ClampD(1.0 - std::exp((std::max)(qDipH, qDipV)), 0.0, 1.0);
					est.quarter.reason = ChromaEstimatorReason::QuarterCorroborated;
				}
				else
				{
					est.quarter.supportsTwoToOneLumaUpscale = false;
					est.quarter.confidence = 0.0;
					est.quarter.reason = ChromaEstimatorReason::QuarterNotSupported;
				}
			}
			else
			{
				est.quarter.valid = false;
				est.quarter.supportsTwoToOneLumaUpscale = false;
				est.quarter.reason = ChromaEstimatorReason::QuarterNotSupported;
			}

			if (!axH.valid || !axV.valid)
			{
				est.reason = ChromaEstimatorReason::InvalidInput;
				return est;
			}

			est.dipH = axH.bestDip;
			est.dipV = axV.bestDip;

			const bool highDomH = (axH.abovePower > nativeAboveGateP) && (axH.abovePower >= axH.belowPower);
			const bool highDomV = (axV.abovePower > nativeAboveGateP) && (axV.abovePower >= axV.belowPower);
			const bool pointH = s.detectBoxAndMpegChroma && bmH.detected && (bmH.family == ChromaUpsamplerFamily::PointNearest);
			const bool pointV = s.detectBoxAndMpegChroma && bmV.detected && (bmV.family == ChromaUpsamplerFamily::PointNearest);
			const bool mpegH = s.detectBoxAndMpegChroma && bmH.detected && (bmH.family == ChromaUpsamplerFamily::MpegLinear) && !highDomH;
			const bool mpegV = s.detectBoxAndMpegChroma && bmV.detected && (bmV.family == ChromaUpsamplerFamily::MpegLinear) && !highDomV;
			const bool subH = pointH || mpegH || (axH.hasDetail && (axH.bestDip <= s.dipConfirmThreshold) && !highDomH);
			const bool subV = pointV || mpegV || (axV.hasDetail && (axV.bestDip <= s.dipConfirmThreshold) && !highDomV);

			// 1080p-in-4K 4:2:0 path: 1/4-res chroma corroborated on BOTH axes when N/2 is quiet or non-dipping.
			if (est.quarter.valid && est.quarter.supportsTwoToOneLumaUpscale && !(subH && subV) && !(highDomH || highDomV))
			{
				est.valid = true;
				est.origin = ChromaOrigin::Subsampled420;
				est.playerUpsampled = true;
				est.dipH = est.quarter.dipH;
				est.dipV = est.quarter.dipV;
				est.confidence = est.quarter.confidence;
				est.reason = ChromaEstimatorReason::QuarterCorroborated;
				return est;
			}

			// SID-SOTA-6: arbitrary luma-coupled 4:2:0 chroma corroboration (e.g. 720p/1440p -> 2160p) when N/2 is quiet or non-dipping.
			if (est.lumaCoupled.valid && est.lumaCoupled.corroborated && !(subH && subV) && !(highDomH || highDomV) && !lumaCoupledHighDom)
			{
				est.valid = true;
				est.origin = ChromaOrigin::Subsampled420;
				est.playerUpsampled = true;
				est.dipH = est.lumaCoupled.dipH;
				est.dipV = est.lumaCoupled.dipV;
				est.confidence = est.lumaCoupled.confidence;
				est.reason = ChromaEstimatorReason::LumaCoupledChromaCorroborated;
				return est;
			}

			if ((!axH.hasDetail && !pointH) || (!axV.hasDetail && !pointV))
			{
				est.valid = false;
				est.origin = ChromaOrigin::Unknown;
				est.reason = ChromaEstimatorReason::LowChromaDetail;
				return est;
			}

			if (subH && subV)
			{
				// Subsampled 4:2:0 confirmed on BOTH axes.
				est.valid = true;
				est.origin = ChromaOrigin::Subsampled420;
				est.playerUpsampled = true;

				if (pointH && pointV)
				{
					est.dipH = bmH.dip;
					est.dipV = bmV.dip;
					est.kernelIndex = -1;
					est.kernelFamily = -1;
					est.upsamplerFamily = ChromaUpsamplerFamily::PointNearest;
					est.kernelMargin = (std::min)(bmH.margin, bmV.margin);
					est.siting = ChromaGridSiting::Unknown;
					est.confidence = ClampD(1.0 - std::exp((std::max)(bmH.dip, bmV.dip)), 0.0, 1.0);
					est.reason = ChromaEstimatorReason::PointNearestUpsampledConfirmed;
					return est;
				}
				if (mpegH && mpegV)
				{
					est.dipH = bmH.dip;
					est.dipV = bmV.dip;
					est.kernelIndex = 0;
					est.kernelFamily = 0;
					est.upsamplerFamily = ChromaUpsamplerFamily::MpegLinear;
					est.kernelMargin = (std::min)(bmH.margin, bmV.margin);
					if (bmH.sitingKnown && bmV.sitingKnown)
					{
						if (bmH.cosited && bmV.cosited)
							est.siting = ChromaGridSiting::TopLeft;
						else if (bmH.cosited && !bmV.cosited)
							est.siting = ChromaGridSiting::Left;
						else if (!bmH.cosited && !bmV.cosited)
							est.siting = ChromaGridSiting::Center;
						else
							est.siting = ChromaGridSiting::Unknown;
					}
					else
					{
						est.siting = ChromaGridSiting::Unknown;
					}
					est.sitingMargin = (bmH.sitingKnown && bmV.sitingKnown)
						? (std::min)(bmH.sitingMargin, bmV.sitingMargin)
						: (std::min)(std::fabs(axH.sitingScore), std::fabs(axV.sitingScore));
					est.confidence = ClampD(1.0 - std::exp((std::max)(bmH.dip, bmV.dip)), 0.0, 1.0);
					est.reason = ChromaEstimatorReason::MpegLinearUpsampledConfirmed;
					return est;
				}

				const double magH = std::fabs(axH.sitingScore);
				const double magV = std::fabs(axV.sitingScore);
				const bool sureH = magH >= s.sitingMarginMin;
				const bool sureV = magV >= s.sitingMarginMin;
				const bool cositedH = axH.sitingScore > 0.0;
				const bool cositedV = axV.sitingScore > 0.0;
				est.sitingMargin = (std::min)(magH, magV);

				if (sureH && sureV)
				{
					if (cositedH && cositedV)
						est.siting = ChromaGridSiting::TopLeft;
					else if (cositedH && !cositedV)
						est.siting = ChromaGridSiting::Left;
					else if (!cositedH && !cositedV)
						est.siting = ChromaGridSiting::Center;
					else
						est.siting = ChromaGridSiting::Unknown;
				}
				else
				{
					est.siting = ChromaGridSiting::Unknown;
				}

				const int phaseIdxH = sureH ? (cositedH ? 1 : 0) : -1;
				const int phaseIdxV = sureV ? (cositedV ? 1 : 0) : -1;
				double pooledRms[kKernelCount] = {};
				for (int k = 0; k < kKernelCount; ++k)
				{
					const double rH = phaseIdxH >= 0 ? axH.kernelRms[k][phaseIdxH]
						: (std::min)(axH.kernelRms[k][0], axH.kernelRms[k][1]);
					const double rV = phaseIdxV >= 0 ? axV.kernelRms[k][phaseIdxV]
						: (std::min)(axV.kernelRms[k][0], axV.kernelRms[k][1]);
					pooledRms[k] = 0.5 * (rH + rV);
				}
				selectKernel(pooledRms);

				// Confidence derived from worst-axis residual reduction 1 - exp(max(dipH, dipV)).
				est.confidence = ClampD(1.0 - std::exp((std::max)(axH.bestDip, axV.bestDip)), 0.0, 1.0);
				if (est.kernelIndex < 0)
					est.reason = ChromaEstimatorReason::KernelUnsure;
				else if (est.siting == ChromaGridSiting::Unknown)
					est.reason = ChromaEstimatorReason::SitingUnsure;
				else
					est.reason = ChromaEstimatorReason::PlayerUpsampled420Confirmed;
				return est;
			}

			if (subH && !subV)
			{
				// Subsampled 4:2:2 (horizontal 2:1 subsampling only, full vertical chroma resolution).
				est.valid = true;
				est.origin = ChromaOrigin::Subsampled422;
				est.playerUpsampled = true;

				if (pointH)
				{
					est.dipH = bmH.dip;
					est.kernelIndex = -1;
					est.kernelFamily = -1;
					est.upsamplerFamily = ChromaUpsamplerFamily::PointNearest;
					est.kernelMargin = bmH.margin;
					est.siting = ChromaGridSiting::Unknown;
					est.confidence = ClampD(1.0 - std::exp(bmH.dip), 0.0, 1.0);
					est.reason = ChromaEstimatorReason::PointNearestUpsampledConfirmed;
					return est;
				}
				if (mpegH)
				{
					est.dipH = bmH.dip;
					est.kernelIndex = 0;
					est.kernelFamily = 0;
					est.upsamplerFamily = ChromaUpsamplerFamily::MpegLinear;
					est.kernelMargin = bmH.margin;
					est.siting = bmH.sitingKnown ? (bmH.cosited ? ChromaGridSiting::Left : ChromaGridSiting::Center) : ChromaGridSiting::Unknown;
					est.sitingMargin = bmH.sitingKnown ? bmH.sitingMargin : std::fabs(axH.sitingScore);
					est.confidence = ClampD(1.0 - std::exp(bmH.dip), 0.0, 1.0);
					est.reason = ChromaEstimatorReason::MpegLinearUpsampledConfirmed;
					return est;
				}

				const double magH = std::fabs(axH.sitingScore);
				const bool sureH = magH >= s.sitingMarginMin;
				const bool cositedH = axH.sitingScore > 0.0;
				est.sitingMargin = magH;
				if (sureH)
					est.siting = cositedH ? ChromaGridSiting::Left : ChromaGridSiting::Center;
				else
					est.siting = ChromaGridSiting::Unknown;

				const int phaseIdxH = sureH ? (cositedH ? 1 : 0) : -1;
				double pooledRms[kKernelCount] = {};
				for (int k = 0; k < kKernelCount; ++k)
				{
					pooledRms[k] = phaseIdxH >= 0 ? axH.kernelRms[k][phaseIdxH]
						: (std::min)(axH.kernelRms[k][0], axH.kernelRms[k][1]);
				}
				selectKernel(pooledRms);

				est.confidence = ClampD(1.0 - std::exp(axH.bestDip), 0.0, 1.0);
				if (est.kernelIndex < 0)
					est.reason = ChromaEstimatorReason::KernelUnsure;
				else if (est.siting == ChromaGridSiting::Unknown)
					est.reason = ChromaEstimatorReason::SitingUnsure;
				else
					est.reason = ChromaEstimatorReason::Subsampled422Confirmed;
				return est;
			}

			if (!subH && !subV)
			{
				// Neither axis dips at 2:1: confirm Native444 only when genuine above-0.25 cyc/px chroma detail exists.
				const bool hasNativeHighBand = (axH.abovePower > nativeAboveGateP) || (axV.abovePower > nativeAboveGateP);
				if (hasNativeHighBand)
				{
					est.valid = true;
					est.origin = ChromaOrigin::Native444;
					est.playerUpsampled = false;
					est.kernelIndex = -1;
					est.kernelFamily = -1;
					est.siting = ChromaGridSiting::Unknown;
					const double peakAbove = (std::max)(axH.abovePower, axV.abovePower);
					est.confidence = ClampD(1.0 - nativeAboveGateP / (peakAbove + nativeAboveGateP), 0.0, 1.0);
					est.reason = ChromaEstimatorReason::Native444Confirmed;
					return est;
				}
				est.valid = false;
				est.origin = ChromaOrigin::Unknown;
				est.playerUpsampled = false;
				est.reason = ChromaEstimatorReason::LowChromaDetail;
				return est;
			}

			// Non-standard vertical-only subsampling (!subH && subV): abstain with explicit reason (Rule 4).
			est.valid = false;
			est.origin = ChromaOrigin::Unknown;
			est.playerUpsampled = false;
			est.reason = ChromaEstimatorReason::NonStandardSubsampling;
			return est;
		}
	}
}

#pragma fp_contract(on)
