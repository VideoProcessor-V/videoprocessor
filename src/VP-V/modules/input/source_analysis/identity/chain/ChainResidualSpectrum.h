// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainResidualSpectrum (SID-C3, Stage S5):
// Pure C++14 spectral residual classification, effective bandwidth estimation, double-scaling
// first-stage detection (OD-4 E=(1)), and separable piecewise-linear tent-basis kernel fitting.
//
// Physics & Rule 1 constant derivations:
//   - Hann-windowed averaged PSD normalization: power[k] /= (segments * windowEnergy * 2.0), matching
//     SourceResolutionDetector.cpp:177-179 so white noise of std `floorCodes` has E[power[k]] = floorCodes^2
//     per interior bin (0.5 * floorCodes^2 at the 1-DOF Nyquist bin).
//   - Band boundaries relative to candidate cutoff cut = (candidateN / axisLen) * (fftN / 2), derived
//     algebraically from W1 SourceResolutionDetector.cpp:857-858 (kW1BelowLo = 0.55, kW1BelowHi = 0.90,
//     width = 0.35, midpoint = 0.50, elementary 1/20 = 0.05 guard matching ReadingSettings::kernelMarginScale):
//       * low  = [0.15*cut, 0.50*cut]  (0.50 - 0.35 = 0.15; equal width to W1 below-band, clears DC/Hann lobe)
//       * halo = [0.55*cut, 1.35*cut]  (W1 0.55..1.0 + symmetric 0.35 reflection above cutoff)
//       * far  = [1.40*cut, 0.95*half] (1.35 + 0.05 symmetric guard to 1.0 - 0.05 Nyquist edge trim)
//     excluding DC, Nyquist, and 8/16-px block harmonics.
//   - Power gate: lineGateFactor = 1.5 (ChainEvidence.h) => gateMult = 1.5^2 = 2.25, gateP = 2.25 * floorP.
//   - Physical TPDF dither variance upper bound: uniform rounding floorP = floorCodes^2 = step^2 / 12;
//     additive TPDF dither variance = 2 * floorP (quantized Schuchman TPDF = 3 * floorP); with gateMult = 2.25,
//     tpdfUpperP = gateMult * 2.0 * floorP = 4.5 * floorP. Flat far-stopband energy in (gateP, tpdfUpperP] is
//     DitherDominated (neverRebase = false); flat far-stopband energy > tpdfUpperP (e.g. 4K / 2K-DI film grain)
//     is NativeCodedStudioUpscale with codedRaster = NativeCoded, neverRebase = true, and explicit reason
//     NativeCodedGrainAboveDitherBound.
//   - Power guard: floorGuardDivisor = 4.0 (ChainEdgeStats.h) => epsP = floorCodes^2 / 16.
//   - Spectral flatness (Wiener entropy): SFM = exp(mean(ln P_k)) / mean(P_k) in (0, 1]; midpoint 0.5
//     (PROVISIONAL pending Stage S5 G-gate calibration) separates flat white noise/grain (>= 0.5) from
//     structured / harmonic high-band detail (< 0.5).
//   - Alias-pair USM inversion: on LS descale residual e = (I - P_U) M U z with M(f) = 1 + a(1 - G_sigma(f)),
//     G_sigma(f) = exp(-2 * pi^2 * sigma^2 * f^2), at transition bin f' in [1.02*f_c, 1.35*f_c] (2% Nyquist
//     clearance avoiding the exact f' = f_c mirror singularity where dG = 0; PROVISIONAL pending Stage S5
//     G-gate calibration) with mirror partner f = 2*f_c - f',
//     |E(f')|/|Y(f')| ~= a*(G_sigma(f) - G_sigma(f')) / (1 + a*(1 - G_sigma(f'))).
//     Physical gain cap kMaxUsmGain = 8.0 (2^3 = 3-bit / 18 dB dynamic-range headroom before highlight/shadow
//     clipping; PROVISIONAL pending S5 G-gate) yields exact algebraic denominator lower bound
//     1 / (1 + kMaxUsmGain) = 1/9 (never reusing floorGuardDivisor).
//     Falls back to passband Wiener ratio and Gaussian half-power inversion sigma = sqrt(ln(2)/(2*pi^2))/f_half
//     when fewer than 2 transition bins are active above gateP.
//   - Double scaling (OD-4 E=(1)): always reports lastStageN as primary size; skips n1 / lastStageN > 0.90
//     (derived from W1 Tracker::Settings::nativeAbove = 0.9 and W1 0.90*f_c2 passband limit); requires valid
//     finite descaleStat candidates and flags firstStageN < lastStageN when power collapses by >= 1.0 decade
//     (10x) between [0.55*f_c1, 0.90*f_c1] and [1.15*f_c1, 0.90*f_c2], prioritizing deepest confirmed
//     descaleStat < 3 * EvidenceSettings::majorityDip (-0.09; PROVISIONAL pending Stage S5 G-gate calibration).
//   - Kernel fit: symmetric half-pixel tent basis h(t_b) at t_b = 0.5 * b (b = 0 .. 2*halfSupport - 1,
//     h(halfSupport) = 0) with partition-of-unity integral 0.5*c_0 + sum_{b>=1} c_b = 1 and strictly
//     numerical Tikhonov guard kernelFitRidge = 1e-6.
//
// Pure C++14, header-only, deterministic doubles, fixed loop order, no Windows headers, no threads/clocks.
// OFF by default (enabled = false); every degraded/abstained path carries an explicit ResidualSpectrumReason.

#include "ChainCommon.h"
#include "KernelBank.h"
#include "../SourceGeometryProduct.h"

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
		enum class ResidualClass : int
		{
			Unknown = 0,
			CleanUpscale = 1,
			UsmSharpened = 2,
			DitherDominated = 3,
			NativeCodedStudioUpscale = 4,
			Abstain = 5
		};

		inline const char* ResidualClassName(ResidualClass c)
		{
			switch (c)
			{
			case ResidualClass::Unknown: return "unknown";
			case ResidualClass::CleanUpscale: return "clean_upscale";
			case ResidualClass::UsmSharpened: return "usm_sharpened";
			case ResidualClass::DitherDominated: return "dither_dominated";
			case ResidualClass::NativeCodedStudioUpscale: return "native_coded_studio_upscale";
			case ResidualClass::Abstain: return "abstain";
			default: return "unknown";
			}
		}

		enum class ResidualSpectrumReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			LowSignalDetail,
			InsufficientBandwidthBins,
			FlatWhiteDitherFloor,
			BandShapedUsmHalo,
			NativeCodedHighBandEnergy,
			NativeCodedGrainAboveDitherBound,
			DoubleScaleConfirmed,
			SingleStageOnly,
			KernelFitSingular,
			AmbiguousSpectrum
		};

		inline const char* ResidualSpectrumReasonText(ResidualSpectrumReason r)
		{
			switch (r)
			{
			case ResidualSpectrumReason::Ok: return "ok";
			case ResidualSpectrumReason::Disabled: return "residual spectrum disabled";
			case ResidualSpectrumReason::InvalidInput: return "invalid input";
			case ResidualSpectrumReason::LowSignalDetail: return "abstain (low signal detail)";
			case ResidualSpectrumReason::InsufficientBandwidthBins: return "abstain (insufficient bandwidth bins)";
			case ResidualSpectrumReason::FlatWhiteDitherFloor: return "flat white dither floor above cutoff";
			case ResidualSpectrumReason::BandShapedUsmHalo: return "band-shaped USM sharpening halo";
			case ResidualSpectrumReason::NativeCodedHighBandEnergy: return "native-coded high-band energy (never rebase)";
			case ResidualSpectrumReason::NativeCodedGrainAboveDitherBound: return "native-coded flat high-band energy above TPDF dither bound (never rebase)";
			case ResidualSpectrumReason::DoubleScaleConfirmed: return "double-scale first-stage cutoff confirmed";
			case ResidualSpectrumReason::SingleStageOnly: return "single-stage upscale only";
			case ResidualSpectrumReason::KernelFitSingular: return "separable kernel normal equations singular";
			case ResidualSpectrumReason::AmbiguousSpectrum: return "abstain (ambiguous residual spectrum)";
			default: return "unknown";
			}
		}

		struct ResidualSpectrumSettings
		{
			bool enabled = false;                 // OFF by default (Stage S5 safe contract)
			double lineGateFactor = 1.5;          // ChainEvidence.h contract: amplitude gate 1.5x => power gate 2.25x
			double floorGuardDivisor = 4.0;       // ChainEdgeStats.h contract: eps = floor / 4, power guard floor^2 / 16
			double bandwidthEnergyQuantile = 0.95;// Standard 95% cumulative excess-energy containment bandwidth
			double doubleScaleDropDecades = 1.0;  // 10x (1-decade) power collapse between first-stage f_c1 and last-stage f_c2
			double kernelFitRidge = 1e-6;         // Strictly numerical Tikhonov singularity guard
			int kernelFitIterations = 2;          // Alternating latent/kernel refinement passes, clamped 1..4
			int kernelFitHalfSupport = 3;         // 3-px half-support = 6-tap support covering Bilinear/Bicubic/Lanczos3
		};

		struct ResidualSpectrumResult
		{
			bool valid = false;
			ResidualClass classification = ResidualClass::Unknown;
			Geometry::CodedRaster codedRaster = Geometry::CodedRaster::Unknown;
			bool neverRebase = false;
			double lowBandPower = 0.0;
			double midBandPower = 0.0;
			double highBandPower = 0.0;
			double signalHighBandPower = 0.0;
			double highBandFlatness = 0.0;
			double usmAmount = 0.0;
			double usmRadiusPx = 0.0;
			double usmConfidence = 0.0;
			double confidence = 0.0;
			ResidualSpectrumReason reason = ResidualSpectrumReason::Disabled;

			Geometry::SharpenGeom ToSharpenGeom() const
			{
				Geometry::SharpenGeom g;
				if (valid && classification == ResidualClass::UsmSharpened)
				{
					g.present = true;
					g.amount = static_cast<float>(usmAmount);
					g.radius = static_cast<float>(usmRadiusPx);
					g.conf = static_cast<float>(ClampD(usmConfidence, 0.0, 1.0));
				}
				return g;
			}
		};

		struct EffectiveBandwidthResult
		{
			bool valid = false;
			double cyclesPerPixel = 0.0;
			double rollOff10dBCyclesPerPixel = 0.0;
			double totalExcessPower = 0.0;
			ResidualSpectrumReason reason = ResidualSpectrumReason::Disabled;
		};

		struct DoubleScaleCandidate
		{
			int n = 0;
			double descaleStat = 0.0;
			bool valid = false;
		};

		struct DoubleScaleResult
		{
			bool valid = false;
			bool doubleScalePresent = false;
			int reportedLastStageN = 0;
			int firstStageN = 0;
			double bandDropDecades = 0.0;
			double confidence = 0.0;
			ResidualSpectrumReason reason = ResidualSpectrumReason::Disabled;

			Geometry::DoubleGeom ToDoubleGeom() const
			{
				Geometry::DoubleGeom g;
				if (valid && doubleScalePresent && firstStageN > 0)
				{
					g.present = true;
					g.n1 = static_cast<int32_t>(firstStageN);
					g.conf = static_cast<float>(ClampD(confidence, 0.0, 1.0));
				}
				return g;
			}
		};

		struct SeparableKernelFitResult
		{
			bool valid = false;
			int n = 0;
			int N = 0;
			double phase = 0.0;
			int halfSupport = 3;
			std::vector<double> knotDistances;
			std::vector<double> knotWeights;
			double fitRms = 0.0;
			double baselineRms = 0.0;
			double rSquared = 0.0;
			int nearestBankKernel = -1;
			int nearestBankFamily = -1;
			ResidualSpectrumReason reason = ResidualSpectrumReason::Disabled;

			double EvaluateKernel(double absDist) const
			{
				if (!valid || knotDistances.empty() || knotDistances.size() != knotWeights.size() || !std::isfinite(absDist))
					return 0.0;
				const double d = std::fabs(absDist);
				const double maxD = static_cast<double>(halfSupport);
				if (!(d < maxD))
					return 0.0;
				const size_t knotCount = knotDistances.size();
				if (d <= knotDistances[0])
					return knotWeights[0];
				for (size_t b = 0; b + 1 < knotCount; ++b)
				{
					if (d <= knotDistances[b + 1])
					{
						const double span = knotDistances[b + 1] - knotDistances[b];
						const double t = span > 0.0 ? (d - knotDistances[b]) / span : 0.0;
						return knotWeights[b] * (1.0 - t) + knotWeights[b + 1] * t;
					}
				}
				const double lastD = knotDistances[knotCount - 1];
				const double span = maxD - lastD;
				const double t = span > 0.0 ? (d - lastD) / span : 1.0;
				return knotWeights[knotCount - 1] * (1.0 - t);
			}
		};

		struct PrecomputedLinePsd
		{
			bool valid = false;
			int fftN = 0;
			int lineCount = 0;
			int axisLen = 0;
			std::vector<double> power;
		};

		namespace detail_residual_spectrum
		{
			constexpr double kNumericEps = 1e-20; // Strictly numerical guard against log(0) / div-by-zero

			inline bool IsValidFftLength(int n)
			{
				return n >= 64 && (n & (n - 1)) == 0;
			}

			inline bool ValidLineSet(const LineSet& ls, int minLen, bool requireAxisLen)
			{
				if (ls.count <= 0 || ls.length < minLen)
					return false;
				if (requireAxisLen && ls.axisLen <= 0)
					return false;
				if (ls.data.size() / static_cast<size_t>(ls.length) < static_cast<size_t>(ls.count))
					return false;
				return true;
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

			// Deterministic iterative radix-2 Cooley-Tukey FFT using precomputed twiddle tables (N a power of two >= 2).
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

			// Selects FFT length in {64, 128, 256, 512, 1024}: largest power of two <= min(length, 1024),
			// or 64 (zero-padded with Hann window over length) when 32 <= length < 64.
			inline int ChooseFftLength(int length)
			{
				if (length < 32)
					return 0;
				if (length < 64)
					return 64;
				int fftN = 64;
				while (fftN * 2 <= length && fftN * 2 <= 1024)
					fftN *= 2;
				return fftN;
			}

			// Computes Hann-windowed averaged one-sided PSD at an explicit power-of-two `forcedFftN` (or ChooseFftLength if <= 0),
			// normalized so white noise of variance sigma^2 has E[psd[k]] = sigma^2 for interior bins 1 .. fftN/2 - 1.
			// Returns false if any sample is non-finite.
			inline bool ComputeAveragedPsdWithFftN(const LineSet& ls, int forcedFftN, int& outFftN, std::vector<double>& outPsd)
			{
				outPsd.clear();
				outFftN = forcedFftN > 0 ? forcedFftN : ChooseFftLength(ls.length);
				if (!IsValidFftLength(outFftN) || ls.length < 32)
					return false;

				const int winLen = ls.length < outFftN ? ls.length : outFftN;
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
				tw.Build(static_cast<size_t>(outFftN));

				// Deterministic segment start offsets covering the full line.
				std::vector<int> segStarts;
				if (ls.length <= outFftN)
				{
					segStarts.push_back(0);
				}
				else
				{
					for (int start = 0; start + outFftN <= ls.length; start += outFftN)
						segStarts.push_back(start);
					const int tailStart = ls.length - outFftN;
					if (segStarts.empty() || segStarts.back() != tailStart)
						segStarts.push_back(tailStart);
				}

				const int half = outFftN / 2;
				outPsd.assign(static_cast<size_t>(half + 1), 0.0);
				std::vector<double> re(static_cast<size_t>(outFftN), 0.0);
				std::vector<double> im(static_cast<size_t>(outFftN), 0.0);
				int totalSegs = 0;

				for (int li = 0; li < ls.count; ++li)
				{
					const double* line = ls.Line(li);
					for (int i = 0; i < ls.length; ++i)
					{
						if (!std::isfinite(line[i]))
							return false;
					}
					for (size_t si = 0; si < segStarts.size(); ++si)
					{
						const int start = segStarts[si];
						double mean = 0.0;
						for (int i = 0; i < winLen; ++i)
							mean += line[start + i];
						mean /= static_cast<double>(winLen);

						for (int i = 0; i < outFftN; ++i)
						{
							re[static_cast<size_t>(i)] = (i < winLen) ? (line[start + i] - mean) * win[static_cast<size_t>(i)] : 0.0;
							im[static_cast<size_t>(i)] = 0.0;
						}
						FftRadix2(re, im, tw);
						for (int k = 0; k <= half; ++k)
						{
							const double rk = re[static_cast<size_t>(k)];
							const double ik = im[static_cast<size_t>(k)];
							const double weight = (k == 0 || k == half) ? 1.0 : 2.0;
							outPsd[static_cast<size_t>(k)] += (rk * rk + ik * ik) * weight;
						}
						++totalSegs;
					}
				}

				if (totalSegs <= 0)
					return false;
				const double norm = static_cast<double>(totalSegs) * winEnergy * 2.0;
				for (size_t k = 0; k < outPsd.size(); ++k)
					outPsd[k] /= norm;
				return true;
			}

			inline bool ComputeAveragedPsd(const LineSet& ls, int& outFftN, std::vector<double>& outPsd)
			{
				return ComputeAveragedPsdWithFftN(ls, 0, outFftN, outPsd);
			}

			// Strict Rule 4 validation of a caller-supplied shared PSD against its target LineSet.
			// Enforces exact FFT size and line count match so cropped windows or mismatched inputs never alias.
			inline bool ValidateSharedPsd(const PrecomputedLinePsd& shared, const LineSet& ls)
			{
				if (!shared.valid ||
					!IsValidFftLength(shared.fftN) ||
					shared.fftN != ChooseFftLength(ls.length) ||
					shared.lineCount != ls.count ||
					shared.axisLen != ls.axisLen ||
					static_cast<int>(shared.power.size()) != shared.fftN / 2 + 1)
				{
					return false;
				}
				for (size_t k = 0; k < shared.power.size(); ++k)
				{
					if (!std::isfinite(shared.power[k]) || shared.power[k] < 0.0)
						return false;
				}
				return true;
			}

			// Block-harmonic exclusion verbatim from SourceResolutionDetector.cpp:54-60 (multiples of 1/16 cyc/px
			// plus +-1 Hann window side-lobes). If full exclusion leaves fewer than 2 bins in a narrow band,
			// falls back to exact harmonic exclusion only (matching SourceResolutionDetector.cpp:207).
			inline bool IsBlockHarmonic(int k, int fftN, bool exactOnly)
			{
				const int period = fftN / 16;
				if (period < 2)
					return false;
				const int r = k % period;
				if (exactOnly)
					return r == 0;
				return r == 0 || r == 1 || r == period - 1;
			}

			inline bool ComputeBandStats(
				const std::vector<double>& psd,
				int fftN,
				int kFrom,
				int kTo,
				double& outMean,
				double* outFlatness = nullptr,
				int* outBins = nullptr)
			{
				outMean = 0.0;
				if (outFlatness != nullptr)
					*outFlatness = 0.0;
				if (outBins != nullptr)
					*outBins = 0;
				const int half = fftN / 2;
				const int lo = kFrom < 1 ? 1 : kFrom;
				const int hi = kTo > half - 1 ? half - 1 : kTo;
				if (lo > hi)
					return false;

				int keptFull = 0;
				for (int k = lo; k <= hi; ++k)
				{
					if (!IsBlockHarmonic(k, fftN, false))
						++keptFull;
				}
				const bool exactOnly = keptFull < 2;

				double sum = 0.0;
				double logSum = 0.0;
				int count = 0;
				for (int k = lo; k <= hi; ++k)
				{
					if (IsBlockHarmonic(k, fftN, exactOnly))
						continue;
					const double p = (std::max)(psd[static_cast<size_t>(k)], kNumericEps);
					sum += p;
					logSum += std::log(p);
					++count;
				}
				if (count < 2)
					return false;
				outMean = sum / static_cast<double>(count);
				if (outFlatness != nullptr)
				{
					const double geomMean = std::exp(logSum / static_cast<double>(count));
					*outFlatness = ClampD(geomMean / (std::max)(outMean, kNumericEps), 0.0, 1.0);
				}
				if (outBins != nullptr)
					*outBins = count;
				return true;
			}

			// Evaluates any KernelBank kernel (including natural cubic splines type == 3) at distance |d|.
			inline double BankKernelValueAt(int kernelIndex, double absDist)
			{
				const KernelInfo& ki = Kernel(kernelIndex);
				const double d = std::fabs(absDist);
				if (!(d < static_cast<double>(ki.radius)))
					return 0.0;
				if (ki.type != 3)
					return KernelValue(ki, d);
				const int idx = static_cast<int>(std::floor(d));
				if (idx < 0 || idx >= ki.radius)
					return 0.0;
				const double frac = d - static_cast<double>(idx);
				double w[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
				NaturalSplineWeights(ki.radius, frac, w);
				return w[ki.radius - 1 - idx];
			}

			// Evaluates piecewise-linear symmetric tent basis at distance d in [0, halfSupport).
			// Sets knot indices b0 (and optional b1 = b0 + 1 < K) with weights w0, w1.
			inline void TentBasis(double absDist, int halfSupport, int& b0, double& w0, int& b1, double& w1)
			{
				b0 = -1;
				b1 = -1;
				w0 = 0.0;
				w1 = 0.0;
				const int knotCount = 2 * halfSupport;
				if (!(absDist >= 0.0) || !(absDist < static_cast<double>(halfSupport)))
					return;
				const double u = 2.0 * absDist;
				const int idx = static_cast<int>(std::floor(u));
				if (idx < 0 || idx >= knotCount)
					return;
				const double frac = u - static_cast<double>(idx);
				b0 = idx;
				w0 = 1.0 - frac;
				if (idx + 1 < knotCount)
				{
					b1 = idx + 1;
					w1 = frac;
				}
			}

			// Small dense linear system solver (Gaussian elimination with partial pivoting, dim <= 8).
			inline bool SolveSmallDense(int dim, std::vector<double> mat, std::vector<double> rhs, std::vector<double>& outX)
			{
				outX.assign(static_cast<size_t>(dim), 0.0);
				double maxAbs = 0.0;
				for (size_t i = 0; i < mat.size(); ++i)
				{
					const double a = std::fabs(mat[i]);
					if (a > maxAbs)
						maxAbs = a;
				}
				if (!(maxAbs > 0.0) || !std::isfinite(maxAbs))
					return false;
				const double pivotTol = 1e-18 * maxAbs;

				for (int col = 0; col < dim; ++col)
				{
					int piv = col;
					double bestVal = std::fabs(mat[static_cast<size_t>(col) * static_cast<size_t>(dim) + static_cast<size_t>(col)]);
					for (int row = col + 1; row < dim; ++row)
					{
						const double v = std::fabs(mat[static_cast<size_t>(row) * static_cast<size_t>(dim) + static_cast<size_t>(col)]);
						if (v > bestVal)
						{
							bestVal = v;
							piv = row;
						}
					}
					if (!(bestVal > pivotTol) || !std::isfinite(bestVal))
						return false;
					if (piv != col)
					{
						for (int c = col; c < dim; ++c)
							std::swap(
								mat[static_cast<size_t>(col) * static_cast<size_t>(dim) + static_cast<size_t>(c)],
								mat[static_cast<size_t>(piv) * static_cast<size_t>(dim) + static_cast<size_t>(c)]);
						std::swap(rhs[static_cast<size_t>(col)], rhs[static_cast<size_t>(piv)]);
					}
					const double diag = mat[static_cast<size_t>(col) * static_cast<size_t>(dim) + static_cast<size_t>(col)];
					for (int row = col + 1; row < dim; ++row)
					{
						const double f = mat[static_cast<size_t>(row) * static_cast<size_t>(dim) + static_cast<size_t>(col)] / diag;
						mat[static_cast<size_t>(row) * static_cast<size_t>(dim) + static_cast<size_t>(col)] = 0.0;
						for (int c = col + 1; c < dim; ++c)
							mat[static_cast<size_t>(row) * static_cast<size_t>(dim) + static_cast<size_t>(c)] -=
								f * mat[static_cast<size_t>(col) * static_cast<size_t>(dim) + static_cast<size_t>(c)];
						rhs[static_cast<size_t>(row)] -= f * rhs[static_cast<size_t>(col)];
					}
				}
				for (int row = dim - 1; row >= 0; --row)
				{
					double sum = rhs[static_cast<size_t>(row)];
					for (int c = row + 1; c < dim; ++c)
						sum -= mat[static_cast<size_t>(row) * static_cast<size_t>(dim) + static_cast<size_t>(c)] * outX[static_cast<size_t>(c)];
					const double diag = mat[static_cast<size_t>(row) * static_cast<size_t>(dim) + static_cast<size_t>(row)];
					if (!(std::fabs(diag) > pivotTol))
						return false;
					outX[static_cast<size_t>(row)] = sum / diag;
					if (!std::isfinite(outX[static_cast<size_t>(row)]))
						return false;
				}
				return true;
			}

			// Banded Cholesky factor & solve for symmetric positive-definite matrix A(i, i-d) stored at [i * bw + d].
			inline bool FactorBandedCholesky(int m, int u, std::vector<double>& chol)
			{
				const size_t bw = static_cast<size_t>(u + 1);
				for (int j = 0; j < m; ++j)
				{
					double s = chol[static_cast<size_t>(j) * bw];
					const int k0 = j - u > 0 ? j - u : 0;
					for (int k = k0; k < j; ++k)
					{
						const double l = chol[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						s -= l * l;
					}
					if (!(s > 1e-300))
						return false;
					const double inv = 1.0 / std::sqrt(s);
					chol[static_cast<size_t>(j) * bw] = inv;
					const int i1 = j + u < m - 1 ? j + u : m - 1;
					for (int i = j + 1; i <= i1; ++i)
					{
						double v = chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)];
						const int kk0 = i - u > 0 ? i - u : 0;
						for (int k = kk0; k < j; ++k)
							v -= chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] *
								chol[static_cast<size_t>(j) * bw + static_cast<size_t>(j - k)];
						chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - j)] = v * inv;
					}
				}
				return true;
			}

			inline void SolveBandedCholesky(int m, int u, const std::vector<double>& chol, double* z)
			{
				const size_t bw = static_cast<size_t>(u + 1);
				for (int i = 0; i < m; ++i)
				{
					double v = z[i];
					const int k0 = i - u > 0 ? i - u : 0;
					for (int k = k0; k < i; ++k)
						v -= chol[static_cast<size_t>(i) * bw + static_cast<size_t>(i - k)] * z[k];
					z[i] = v * chol[static_cast<size_t>(i) * bw];
				}
				for (int i = m - 1; i >= 0; --i)
				{
					double v = z[i];
					const int k1 = i + u < m - 1 ? i + u : m - 1;
					for (int k = i + 1; k <= k1; ++k)
						v -= chol[static_cast<size_t>(k) * bw + static_cast<size_t>(k - i)] * z[k];
					z[i] = v * chol[static_cast<size_t>(i) * bw];
				}
			}
		} // namespace detail_residual_spectrum

		// Precomputes a reusable Hann-windowed averaged PSD for a LineSet so ClassifyResidualSpectrum,
		// EstimateEffectiveBandwidth, and EvaluateDoubleScale can share a single signal FFT pass.
		inline bool ComputeLineSetPsd(const LineSet& ls, PrecomputedLinePsd& out)
		{
			out = PrecomputedLinePsd{};
			if (!detail_residual_spectrum::ValidLineSet(ls, 32, false))
				return false;
			int fftN = 0;
			std::vector<double> power;
			if (!detail_residual_spectrum::ComputeAveragedPsd(ls, fftN, power))
				return false;
			out.valid = true;
			out.fftN = fftN;
			out.lineCount = ls.count;
			out.axisLen = ls.axisLen;
			out.power = std::move(power);
			return true;
		}

		// ============================================================================================================
		// 1. Spectral Residual Classification (SID-C3: Clean / USM / Dither / NativeCodedStudioUpscale / Abstain)
		// ============================================================================================================
		inline ResidualSpectrumResult ClassifyResidualSpectrum(
			const LineSet& signalLines,
			const LineSet& residualLines,
			int candidateN,
			double floorCodes,
			const ResidualSpectrumSettings& s = ResidualSpectrumSettings(),
			const PrecomputedLinePsd* sharedSigPsd = nullptr)
		{
			ResidualSpectrumResult res;
			if (!s.enabled)
			{
				res.reason = ResidualSpectrumReason::Disabled;
				return res;
			}
			if (!detail_residual_spectrum::ValidLineSet(signalLines, 32, true) ||
				!detail_residual_spectrum::ValidLineSet(residualLines, 32, false) ||
				signalLines.count != residualLines.count ||
				signalLines.length != residualLines.length ||
				candidateN <= 0 || candidateN >= signalLines.axisLen ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(s.lineGateFactor) || !(s.lineGateFactor > 1.0) ||
				!std::isfinite(s.floorGuardDivisor) || !(s.floorGuardDivisor > 1.0))
			{
				res.reason = ResidualSpectrumReason::InvalidInput;
				return res;
			}

			int fftN = 0;
			int fftNRes = 0;
			std::vector<double> localSigPsd;
			std::vector<double> resPsd;
			const std::vector<double>* sigPsdPtr = nullptr;
			if (sharedSigPsd != nullptr)
			{
				if (!detail_residual_spectrum::ValidateSharedPsd(*sharedSigPsd, signalLines))
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
				fftN = sharedSigPsd->fftN;
				sigPsdPtr = &sharedSigPsd->power;
				if (!detail_residual_spectrum::ComputeAveragedPsdWithFftN(residualLines, fftN, fftNRes, resPsd) ||
					fftN != fftNRes)
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
			}
			else
			{
				if (!detail_residual_spectrum::ComputeAveragedPsd(signalLines, fftN, localSigPsd) ||
					!detail_residual_spectrum::ComputeAveragedPsd(residualLines, fftNRes, resPsd) ||
					fftN != fftNRes)
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
				sigPsdPtr = &localSigPsd;
			}
			const std::vector<double>& sigPsd = *sigPsdPtr;

			const int half = fftN / 2;
			const double ratio = static_cast<double>(candidateN) / static_cast<double>(signalLines.axisLen);
			const double cut = ratio * static_cast<double>(half);
			const double fCut = 0.5 * ratio; // cycles per pixel at candidate Nyquist

			// Band limits around candidate Nyquist `cut`, derived from W1 SourceResolutionDetector.cpp:857-858
			// (kW1BelowLo = 0.55, kW1BelowHi = 0.90, width = 0.35), passband midpoint 0.50, and elementary 1/20 = 0.05 guard
			// (matching ReadingSettings::kernelMarginScale = 0.05):
			//   - low:  [0.15*cut, 0.50*cut]  (passband lower half, equal 0.35 width to W1 below-band)
			//   - halo: [0.55*cut, 1.35*cut]  (W1 0.55..1.0 + symmetric 0.35 reflection above cutoff)
			//   - far:  [1.40*cut, 0.95*half] (halo upper + 0.05 symmetric guard to 1.0 - 0.05 Nyquist edge trim)
			constexpr double kW1BelowLo = 0.55;
			constexpr double kW1BelowHi = 0.90;
			constexpr double kW1BelowWidth = kW1BelowHi - kW1BelowLo;   // 0.35
			constexpr double kBandGuard = 0.05;                         // 1/20 elementary transition/Nyquist guard
			constexpr double kLowHiFrac = 0.50;                         // exact passband midpoint 1/2
			constexpr double kLowLoFrac = kLowHiFrac - kW1BelowWidth;   // 0.15
			constexpr double kHaloLoFrac = kW1BelowLo;                  // 0.55
			constexpr double kHaloHiFrac = 1.0 + kW1BelowWidth;         // 1.35
			constexpr double kFarLoFrac = kHaloHiFrac + kBandGuard;     // 1.40
			constexpr double kFarHiFrac = 1.0 - kBandGuard;             // 0.95
			const int low0 = static_cast<int>(std::ceil(kLowLoFrac * cut));
			const int low1 = static_cast<int>(std::floor(kLowHiFrac * cut));
			const int halo0 = static_cast<int>(std::ceil(kHaloLoFrac * cut));
			const int halo1 = (std::min)(half - 1, static_cast<int>(std::floor(kHaloHiFrac * cut)));
			const int far0 = static_cast<int>(std::ceil(kFarLoFrac * cut));
			const int far1 = static_cast<int>(std::floor(kFarHiFrac * static_cast<double>(half)));

			double resLow = 0.0;
			double resHalo = 0.0;
			double resFar = 0.0, resFarFlat = 0.0;
			double sigLow = 0.0;
			double sigHalo = 0.0;
			double sigFar = 0.0;

			if (!detail_residual_spectrum::ComputeBandStats(resPsd, fftN, low0, low1, resLow) ||
				!detail_residual_spectrum::ComputeBandStats(resPsd, fftN, halo0, halo1, resHalo) ||
				!detail_residual_spectrum::ComputeBandStats(resPsd, fftN, far0, far1, resFar, &resFarFlat) ||
				!detail_residual_spectrum::ComputeBandStats(sigPsd, fftN, low0, low1, sigLow) ||
				!detail_residual_spectrum::ComputeBandStats(sigPsd, fftN, halo0, halo1, sigHalo) ||
				!detail_residual_spectrum::ComputeBandStats(sigPsd, fftN, far0, far1, sigFar))
			{
				res.classification = ResidualClass::Abstain;
				res.reason = ResidualSpectrumReason::InsufficientBandwidthBins;
				return res;
			}

			res.lowBandPower = resLow;
			res.midBandPower = resHalo;
			res.highBandPower = resFar;
			res.signalHighBandPower = sigFar;
			res.highBandFlatness = resFarFlat;

			const double floorP = floorCodes * floorCodes;
			const double gateMult = s.lineGateFactor * s.lineGateFactor; // 1.5^2 = 2.25
			const double gateP = gateMult * floorP;
			const double epsP = (std::max)(floorP / (s.floorGuardDivisor * s.floorGuardDivisor), detail_residual_spectrum::kNumericEps);

			// Total signal detail check across low + halo + far bands.
			const double sigTotalBandPower = sigLow + sigHalo + sigFar;
			if (!(sigTotalBandPower > gateP))
			{
				res.valid = false;
				res.classification = ResidualClass::Abstain;
				res.reason = ResidualSpectrumReason::LowSignalDetail;
				return res;
			}

			// Measure 8-px block-harmonic peak power in the far stopband (native 4K coding grid signature).
			double blockHarmResSum = 0.0;
			double blockHarmSigSum = 0.0;
			int blockHarmCount = 0;
			const int period8 = fftN / 8;
			if (period8 >= 2)
			{
				const int fLo = far0 < 1 ? 1 : far0;
				const int fHi = far1 > half - 1 ? half - 1 : far1;
				for (int k = fLo; k <= fHi; ++k)
				{
					if (k % period8 == 0)
					{
						blockHarmResSum += resPsd[static_cast<size_t>(k)];
						blockHarmSigSum += sigPsd[static_cast<size_t>(k)];
						++blockHarmCount;
					}
				}
			}
			const double blockHarmResMean = blockHarmCount > 0 ? blockHarmResSum / static_cast<double>(blockHarmCount) : 0.0;
			const double blockHarmSigMean = blockHarmCount > 0 ? blockHarmSigSum / static_cast<double>(blockHarmCount) : 0.0;
			const double blockHarmRatio = blockHarmResMean / (std::max)(resFar, floorP);

			// Physical TPDF dither variance upper bound (H4 / SID-C3):
			//   - Uniform rounding variance floorP = floorCodes^2 = step^2 / 12.
			//   - Additive TPDF dither variance = 2 * floorP (quantized Schuchman TPDF = 3 * floorP).
			//   - With power gate multiplier gateMult = lineGateFactor^2 = 2.25:
			//     tpdfUpperP = gateMult * 2.0 * floorP = 4.5 * floorP (at 10-bit: 0.375, sigma <= 0.612 codes).
			//   - Quantized 10-bit TPDF (3 * floorP = 0.25) sits cleanly inside (gateP, tpdfUpperP] = (0.1875, 0.375];
			//     real 4K / 2K-DI film grain (e.g. sigma ~= 2.0 codes => resFar ~= 4.0 >> 0.375) exceeds tpdfUpperP
			//     and MUST classify as NativeCodedStudioUpscale (codedRaster = NativeCoded, neverRebase = true).
			const double tpdfUpperP = gateMult * 2.0 * floorP;
			// Exact midpoint 1/2 of the Wiener entropy (SFM) domain (0, 1]; PROVISIONAL pending Stage S5 G-gate calibration.
			constexpr double kFlatnessWhiteThresh = 0.5;

			// 1. NativeCodedStudioUpscale (C8 / OD-4 C=(1)): structured far-stopband detail (SFM < kFlatnessWhiteThresh)
			//    or 8-px native codec harmonic peak >= lineGateFactor^2. USM leaves far stopband at floor.
			const bool structuredFarBand = (sigFar > gateP) && (resFar > gateP) && (resFarFlat < kFlatnessWhiteThresh);
			const bool blockHarmonicFarBand =
				(blockHarmCount > 0) &&
				(blockHarmResMean > gateP) &&
				(blockHarmSigMean > gateP) &&
				(blockHarmRatio >= gateMult);

			if (structuredFarBand || blockHarmonicFarBand)
			{
				res.valid = true;
				res.classification = ResidualClass::NativeCodedStudioUpscale;
				res.codedRaster = Geometry::CodedRaster::NativeCoded;
				res.neverRebase = true;
				res.reason = ResidualSpectrumReason::NativeCodedHighBandEnergy;
				const double domPower = structuredFarBand ? resFar : blockHarmResMean;
				const double snrScore = 1.0 - gateP / (domPower + gateP);
				const double structScore = blockHarmonicFarBand
					? (1.0 - gateMult / (blockHarmRatio + gateMult))
					: ClampD(1.0 - resFarFlat, 0.0, 1.0);
				res.confidence = ClampD(0.5 * (snrScore + structScore), 0.0, 1.0);
				return res;
			}

			// 2a. NativeCodedStudioUpscale (H4 / fake-4K film-grain boundary): flat far-stopband energy EXCEEDING the
			//     physical TPDF dither upper bound tpdfUpperP = gateMult * 2 * (step^2 / 12) is genuine native-coded
			//     high-frequency grain/texture (never re-base), NOT dither — even if USM sharpening is also present
			//     in the halo band (USM leaves the far stopband near floorP).
			if (resFar > tpdfUpperP &&
				resFarFlat >= kFlatnessWhiteThresh)
			{
				res.valid = true;
				res.classification = ResidualClass::NativeCodedStudioUpscale;
				res.codedRaster = Geometry::CodedRaster::NativeCoded;
				res.neverRebase = true;
				res.reason = ResidualSpectrumReason::NativeCodedGrainAboveDitherBound;
				res.confidence = ClampD(resFarFlat * (1.0 - tpdfUpperP / (resFar + tpdfUpperP)), 0.0, 1.0);
				return res;
			}

			// 2b. DitherDominated: flat white TPDF dither floor within physical TPDF bound (gateP, tpdfUpperP],
			//     with no band-shaped halo peak.
			if (resFar > gateP &&
				resFar <= tpdfUpperP &&
				resFarFlat >= kFlatnessWhiteThresh &&
				resHalo < gateMult * resFar)
			{
				res.valid = true;
				res.classification = ResidualClass::DitherDominated;
				res.codedRaster = Geometry::CodedRaster::Unknown;
				res.neverRebase = false;
				res.reason = ResidualSpectrumReason::FlatWhiteDitherFloor;
				res.confidence = ClampD(resFarFlat * (1.0 - gateP / (resFar + gateP)), 0.0, 1.0);
				return res;
			}

			// 3. UsmSharpened: band-shaped transition halo peaking sharply above both low band and far stopband
			//    by the dimensionally consistent power gate gateMult = lineGateFactor^2 (2.25x in power).
			if (resHalo > gateP &&
				resHalo >= gateMult * (std::max)(resLow, epsP) &&
				resHalo >= gateMult * (std::max)(resFar, epsP))
			{
				res.valid = true;
				res.classification = ResidualClass::UsmSharpened;
				res.codedRaster = Geometry::CodedRaster::PlayerUpscaled;
				res.neverRebase = false;
				res.reason = ResidualSpectrumReason::BandShapedUsmHalo;

				// Exact analytical alias-pair USM inversion over above-cutoff transition bins [(1 + kTransNyquistGuard)*cut, halo1]:
				// r_obs(f') ~= a * (G_sigma(f) - G_sigma(f')) / (1 + a * (1 - G_sigma(f'))), f = 2*f_c - f'.
				// kTransNyquistGuard = 1/50 = 0.02 clears the exact f' = f_c mirror singularity (where f = f' => dG = 0)
				// and Hann main-lobe leakage at cut (PROVISIONAL pending Stage S5 G-gate calibration).
				constexpr double kTransNyquistGuard = 0.02;
				const int trans0 = (std::max)(1, static_cast<int>(std::ceil((1.0 + kTransNyquistGuard) * cut)));
				const int trans1 = halo1;
				std::vector<double> transFreq;
				std::vector<double> transMirrorFreq;
				std::vector<double> transObsRatio;
				std::vector<double> transWeight;
				for (int k = trans0; k <= trans1; ++k)
				{
					if (detail_residual_spectrum::IsBlockHarmonic(k, fftN, true))
						continue;
					const double pe = resPsd[static_cast<size_t>(k)];
					const double py = sigPsd[static_cast<size_t>(k)];
					if (!(pe > gateP) || !(py > gateP))
						continue;
					const double fPrime = static_cast<double>(k) / static_cast<double>(fftN);
					const double fMirror = 2.0 * fCut - fPrime;
					if (!(fMirror > 0.0) || !(fMirror < fPrime))
						continue;
					const double exE = (std::max)(pe - floorP, 0.0);
					const double exY = (std::max)(py - floorP, epsP);
					transFreq.push_back(fPrime);
					transMirrorFreq.push_back(fMirror);
					transObsRatio.push_back(std::sqrt(exE / exY));
					transWeight.push_back(exE);
				}

				bool aliasPairFitOk = false;
				if (transFreq.size() >= 2)
				{
					const double twoPiSq = 2.0 * kPi * kPi;
					// Physical USM contrast-boost stability bound: kMaxUsmGain = 8.0 (2^3 = 3 bits / 18 dB dynamic-range
					// headroom before highlight/shadow clipping; PROVISIONAL pending Stage S5 G-gate calibration).
					// Exact algebraic lower bound on 1 - a_eff*(1 - gBar) = 1 / (1 + a*(1 - gBar)) >= 1 / (1 + kMaxUsmGain)
					// for all physical a <= kMaxUsmGain and gBar in [0, 1] (no reuse of floorGuardDivisor).
					constexpr double kMaxUsmGain = 8.0;
					constexpr double kInvDenomFloor = 1.0 / (1.0 + kMaxUsmGain);
					double bestSse = 1e300;
					double bestSigma = 1.0;
					double bestAmount = 0.0;
					// Deterministic physical sigma grid in [0.4, 3.0] px (0.1-px step):
					// lower bound 0.4 px = discrete Nyquist-resolvable Gaussian half-width;
					// upper bound 3.0 px = default kernel half-support (kernelFitHalfSupport = 3).
					for (int si = 4; si <= 30; ++si)
					{
						const double sigma = 0.1 * static_cast<double>(si);
						const double coeff = -twoPiSq * sigma * sigma;
						double num = 0.0;
						double den = 0.0;
						double gBarNum = 0.0;
						double wSum = 0.0;
						for (size_t ti = 0; ti < transFreq.size(); ++ti)
						{
							const double fP = transFreq[ti];
							const double fM = transMirrorFreq[ti];
							const double gM = std::exp(coeff * fM * fM);
							const double gP = std::exp(coeff * fP * fP);
							const double dG = gM - gP;
							const double w = transWeight[ti];
							num += w * transObsRatio[ti] * dG;
							den += w * dG * dG;
							gBarNum += w * gP;
							wSum += w;
						}
						if (!(den > detail_residual_spectrum::kNumericEps) || !(num > 0.0) || !(wSum > 0.0))
							continue;
						const double aEff = num / den;
						const double gBar = gBarNum / wSum;
						const double denomInv = (std::max)(1.0 - aEff * (1.0 - gBar), kInvDenomFloor);
						const double aCand = ClampD(aEff / denomInv, 0.0, kMaxUsmGain);
						if (!(aCand > 0.0) || !std::isfinite(aCand))
							continue;
						double sse = 0.0;
						for (size_t ti = 0; ti < transFreq.size(); ++ti)
						{
							const double fP = transFreq[ti];
							const double fM = transMirrorFreq[ti];
							const double gM = std::exp(coeff * fM * fM);
							const double gP = std::exp(coeff * fP * fP);
							const double dG = gM - gP;
							const double pred = (aCand * dG) / (1.0 + aCand * (1.0 - gP));
							const double diff = transObsRatio[ti] - pred;
							sse += transWeight[ti] * diff * diff;
						}
						if (sse < bestSse)
						{
							bestSse = sse;
							bestSigma = sigma;
							bestAmount = aCand;
							aliasPairFitOk = true;
						}
					}
					if (aliasPairFitOk)
					{
						res.usmAmount = bestAmount;
						res.usmRadiusPx = bestSigma;
					}
				}

				if (!aliasPairFitOk)
				{
					// Fallback when transition band has < 2 active bins: passband Wiener ratio + Gaussian half-power inversion.
					const double exHaloE = (std::max)(resHalo - floorP, 0.0);
					const double exHaloY = (std::max)(sigHalo - floorP, epsP);
					res.usmAmount = std::sqrt(exHaloE / exHaloY);

					const double targetRatio = 0.5 * res.usmAmount;
					double fHalf = static_cast<double>(halo0 + halo1) / (2.0 * static_cast<double>(fftN));
					double prevRatio = 0.0;
					int prevBin = 0;
					for (int k = 1; k <= halo1; ++k)
					{
						if (detail_residual_spectrum::IsBlockHarmonic(k, fftN, true))
							continue;
						if (!(sigPsd[static_cast<size_t>(k)] > gateP))
							continue;
						const double exE = (std::max)(resPsd[static_cast<size_t>(k)] - floorP, 0.0);
						const double exY = (std::max)(sigPsd[static_cast<size_t>(k)] - floorP, epsP);
						const double curRatio = std::sqrt(exE / exY);
						if (curRatio >= targetRatio && targetRatio > 0.0)
						{
							if (prevBin > 0 && curRatio > prevRatio)
							{
								const double frac = ClampD((targetRatio - prevRatio) / (curRatio - prevRatio), 0.0, 1.0);
								fHalf = (static_cast<double>(prevBin) + frac * static_cast<double>(k - prevBin)) / static_cast<double>(fftN);
							}
							else
							{
								fHalf = static_cast<double>(k) / static_cast<double>(fftN);
							}
							break;
						}
						prevRatio = curRatio;
						prevBin = k;
					}
					const double gaussConst = std::sqrt(std::log(2.0) / (2.0 * kPi * kPi));
					res.usmRadiusPx = fHalf > 0.0 ? (gaussConst / fHalf) : 0.0;
				}

				const double lowDom = resHalo / (gateMult * (std::max)(resLow, epsP));
				const double farDom = resHalo / (gateMult * (std::max)(resFar, epsP));
				const double snrScore = 1.0 - gateP / (resHalo + gateP);
				const double shapeScore = 1.0 - 1.0 / (1.0 + (std::min)(lowDom, farDom));
				res.usmConfidence = ClampD(snrScore * shapeScore, 0.0, 1.0);
				res.confidence = res.usmConfidence;
				return res;
			}

			// 4. CleanUpscale: residual sits at/within noise floor across halo & far bands while signal has passband detail.
			if (resHalo <= gateP && resFar <= gateP && (sigLow + sigHalo) > gateP)
			{
				res.valid = true;
				res.classification = ResidualClass::CleanUpscale;
				res.codedRaster = Geometry::CodedRaster::PlayerUpscaled;
				res.neverRebase = false;
				res.reason = ResidualSpectrumReason::Ok;
				const double maxRes = (std::max)(resHalo, resFar);
				const double quietness = 1.0 - 0.5 * (maxRes / gateP);
				const double sigSnr = 1.0 - gateP / ((sigLow + sigHalo) + gateP);
				res.confidence = ClampD(quietness * sigSnr, 0.0, 1.0);
				return res;
			}

			// 5. Ambiguous spectral shape: explicit Abstain with AmbiguousSpectrum reason (Rule 4).
			res.valid = true;
			res.classification = ResidualClass::Abstain;
			res.codedRaster = Geometry::CodedRaster::Unknown;
			res.neverRebase = false;
			res.confidence = 0.0;
			res.reason = ResidualSpectrumReason::AmbiguousSpectrum;
			return res;
		}

		// ============================================================================================================
		// 2. Effective Bandwidth Estimator (95% Cumulative Excess Energy & -10 dB Roll-Off)
		// ============================================================================================================
		inline EffectiveBandwidthResult EstimateEffectiveBandwidth(
			const LineSet& ls,
			double floorCodes,
			const ResidualSpectrumSettings& s = ResidualSpectrumSettings(),
			const PrecomputedLinePsd* sharedSigPsd = nullptr)
		{
			EffectiveBandwidthResult res;
			if (!s.enabled)
			{
				res.reason = ResidualSpectrumReason::Disabled;
				return res;
			}
			if (!detail_residual_spectrum::ValidLineSet(ls, 32, false) ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(s.lineGateFactor) || !(s.lineGateFactor > 1.0) ||
				!std::isfinite(s.bandwidthEnergyQuantile) ||
				!(s.bandwidthEnergyQuantile > 0.0) || !(s.bandwidthEnergyQuantile < 1.0))
			{
				res.reason = ResidualSpectrumReason::InvalidInput;
				return res;
			}

			int fftN = 0;
			std::vector<double> localPsd;
			const std::vector<double>* psdPtr = nullptr;
			if (sharedSigPsd != nullptr)
			{
				if (!detail_residual_spectrum::ValidateSharedPsd(*sharedSigPsd, ls))
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
				fftN = sharedSigPsd->fftN;
				psdPtr = &sharedSigPsd->power;
			}
			else
			{
				if (!detail_residual_spectrum::ComputeAveragedPsd(ls, fftN, localPsd))
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
				psdPtr = &localPsd;
			}
			const std::vector<double>& psd = *psdPtr;

			const int half = fftN / 2;
			const double floorP = floorCodes * floorCodes;
			std::vector<double> excess(static_cast<size_t>(half + 1), 0.0);
			double totalExcess = 0.0;
			for (int k = 1; k <= half; ++k)
			{
				// Nyquist bin carries 1 real DOF vs 2 for complex interior bins.
				const double binFloor = (k == half) ? (0.5 * floorP) : floorP;
				const double ex = (std::max)(psd[static_cast<size_t>(k)] - binFloor, 0.0);
				excess[static_cast<size_t>(k)] = ex;
				totalExcess += ex;
			}

			const double meanExcess = totalExcess / static_cast<double>(half);
			const double minMeanExcess = (s.lineGateFactor * s.lineGateFactor - 1.0) * floorP;
			if (!(meanExcess >= minMeanExcess) || !(totalExcess > 0.0))
			{
				res.valid = false;
				res.totalExcessPower = totalExcess;
				res.reason = ResidualSpectrumReason::LowSignalDetail;
				return res;
			}

			// Exact cumulative excess-energy quantile frequency.
			const double target = s.bandwidthEnergyQuantile * totalExcess;
			double cum = 0.0;
			double bwBin = static_cast<double>(half);
			for (int k = 1; k <= half; ++k)
			{
				const double ex = excess[static_cast<size_t>(k)];
				const double nextCum = cum + ex;
				if (nextCum >= target)
				{
					const double frac = ex > 0.0 ? ClampD((target - cum) / ex, 0.0, 1.0) : 1.0;
					bwBin = static_cast<double>(k - 1) + frac;
					if (bwBin < 1.0)
						bwBin = 1.0;
					break;
				}
				cum = nextCum;
			}

			// 3-bin smoothed excess spectrum for stable -10 dB (0.1x power) upper roll-off frequency.
			std::vector<double> smoothed(static_cast<size_t>(half + 1), 0.0);
			double peakSmooth = 0.0;
			for (int k = 1; k <= half; ++k)
			{
				const double left = k > 1 ? excess[static_cast<size_t>(k - 1)] : excess[static_cast<size_t>(k)];
				const double mid = excess[static_cast<size_t>(k)];
				const double right = k < half ? excess[static_cast<size_t>(k + 1)] : excess[static_cast<size_t>(k)];
				const double sm = (left + mid + right) / 3.0;
				smoothed[static_cast<size_t>(k)] = sm;
				if (sm > peakSmooth)
					peakSmooth = sm;
			}
			const double thresh10dB = 0.1 * peakSmooth; // -10 dB = 10^(-1) = 0.1 in power
			int rollBin = 1;
			for (int k = half; k >= 1; --k)
			{
				if (smoothed[static_cast<size_t>(k)] >= thresh10dB)
				{
					rollBin = k;
					break;
				}
			}

			res.valid = true;
			res.cyclesPerPixel = ClampD(bwBin / static_cast<double>(fftN), 0.0, 0.5);
			res.rollOff10dBCyclesPerPixel = ClampD(static_cast<double>(rollBin) / static_cast<double>(fftN), 0.0, 0.5);
			res.totalExcessPower = totalExcess;
			res.reason = ResidualSpectrumReason::Ok;
			return res;
		}

		// ============================================================================================================
		// 3. Double-Scaling First-Stage Detector (OD-4 E=(1): Always Report Last-Stage Size)
		// ============================================================================================================
		inline DoubleScaleResult EvaluateDoubleScale(
			const LineSet& ls,
			int lastStageN,
			const std::vector<DoubleScaleCandidate>& candidates,
			double floorCodes,
			const ResidualSpectrumSettings& s = ResidualSpectrumSettings(),
			const PrecomputedLinePsd* sharedSigPsd = nullptr)
		{
			DoubleScaleResult res;
			if (!s.enabled)
			{
				res.reason = ResidualSpectrumReason::Disabled;
				return res;
			}
			if (!detail_residual_spectrum::ValidLineSet(ls, 32, true) ||
				lastStageN < 16 || lastStageN > ls.axisLen ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(s.lineGateFactor) || !(s.lineGateFactor > 1.0) ||
				!std::isfinite(s.doubleScaleDropDecades) || !(s.doubleScaleDropDecades > 0.0))
			{
				res.reason = ResidualSpectrumReason::InvalidInput;
				return res;
			}

			// OD-4 E=(1): ALWAYS report lastStageN as the primary reported size.
			res.reportedLastStageN = lastStageN;

			int fftN = 0;
			std::vector<double> localPsd;
			const std::vector<double>* psdPtr = nullptr;
			if (sharedSigPsd != nullptr)
			{
				if (!detail_residual_spectrum::ValidateSharedPsd(*sharedSigPsd, ls))
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
				fftN = sharedSigPsd->fftN;
				psdPtr = &sharedSigPsd->power;
			}
			else
			{
				if (!detail_residual_spectrum::ComputeAveragedPsd(ls, fftN, localPsd))
				{
					res.reason = ResidualSpectrumReason::InvalidInput;
					return res;
				}
				psdPtr = &localPsd;
			}
			const std::vector<double>& psd = *psdPtr;

			const int half = fftN / 2;
			const double floorP = floorCodes * floorCodes;
			const double gateP = s.lineGateFactor * s.lineGateFactor * floorP;
			const double fC2 = (static_cast<double>(lastStageN) / static_cast<double>(ls.axisLen)) * static_cast<double>(half);

			// Verify overall passband signal detail below fC2.
			double passMean = 0.0;
			const int passHi = static_cast<int>(std::floor(0.90 * fC2));
			if (!detail_residual_spectrum::ComputeBandStats(psd, fftN, 1, passHi, passMean) ||
				!(passMean > gateP))
			{
				res.valid = false;
				res.reason = ResidualSpectrumReason::LowSignalDetail;
				return res;
			}

			// Derived from W1 contract constants:
			//   - kDoubleScaleMaxStageRatio = 0.90 (W1 SourceResolution::Tracker::Settings::nativeAbove = 0.9 and
			//     W1 SourceResolutionDetector.cpp:857 passband upper limit 0.90*cut; n1/n2 > 0.90 collapses [1.15*f_c1, 0.90*f_c2]).
			//   - kConfirmedDescaleDip = 3.0 * EvidenceSettings::majorityDip (-0.03 * 3 = -0.09 ln units;
			//     PROVISIONAL pending Stage S5 G-gate calibration) distinguishes genuine first-stage descale notches
			//     from 1/f^2 low-frequency spectral bias.
			constexpr double kDoubleScaleMaxStageRatio = 0.90;
			constexpr double kMajorityDipContract = -0.03; // ChainEvidence.h:92 EvidenceSettings::majorityDip
			constexpr double kConfirmedDescaleDip = 3.0 * kMajorityDipContract;
			int bestN1 = 0;
			bool bestHasConfirmedDip = false;
			double bestDescaleStat = 1e300;
			double bestDrop = -1e300;
			double bestJointScore = -1e300;
			double bestPBelow = 0.0;
			double maxObservedDrop = 0.0;

			for (size_t ci = 0; ci < candidates.size(); ++ci)
			{
				const DoubleScaleCandidate& c = candidates[ci];
				if (!c.valid || !std::isfinite(c.descaleStat))
					continue;
				if (c.n < 16 || c.n >= lastStageN)
					continue;
				if (static_cast<double>(c.n) / static_cast<double>(lastStageN) > kDoubleScaleMaxStageRatio)
					continue;
				// Positive descaleStat contradicts a first-stage scaling lattice at c.n.
				if (c.descaleStat > 0.0)
					continue;

				const double fC1 = (static_cast<double>(c.n) / static_cast<double>(ls.axisLen)) * static_cast<double>(half);
				const int b1Lo = static_cast<int>(std::ceil(0.55 * fC1));
				const int b1Hi = static_cast<int>(std::floor(0.90 * fC1));
				const int b2Lo = static_cast<int>(std::ceil(1.15 * fC1));
				const int b2Hi = static_cast<int>(std::floor(0.90 * fC2));

				double pBelow1 = 0.0;
				double pBetween12 = 0.0;
				if (!detail_residual_spectrum::ComputeBandStats(psd, fftN, b1Lo, b1Hi, pBelow1) ||
					!detail_residual_spectrum::ComputeBandStats(psd, fftN, b2Lo, b2Hi, pBetween12))
				{
					continue;
				}
				if (!(pBelow1 > gateP))
					continue;

				const double dropDecades = std::log10(pBelow1 / (std::max)(pBetween12, floorP));
				if (dropDecades > maxObservedDrop)
					maxObservedDrop = dropDecades;

				if (dropDecades >= s.doubleScaleDropDecades)
				{
					const bool hasConfirmedDip = (c.descaleStat < kConfirmedDescaleDip);
					const double jointScore = dropDecades - c.descaleStat;
					bool better = false;
					if (bestN1 == 0)
					{
						better = true;
					}
					else if (hasConfirmedDip != bestHasConfirmedDip)
					{
						better = hasConfirmedDip;
					}
					else if (hasConfirmedDip)
					{
						// Prefer deepest confirmed descaleStat < -0.10; break exact ties by larger spectral drop.
						better = (c.descaleStat < bestDescaleStat) ||
							(c.descaleStat == bestDescaleStat && dropDecades > bestDrop);
					}
					else
					{
						better = jointScore > bestJointScore;
					}

					if (better)
					{
						bestN1 = c.n;
						bestHasConfirmedDip = hasConfirmedDip;
						bestDescaleStat = c.descaleStat;
						bestDrop = dropDecades;
						bestJointScore = jointScore;
						bestPBelow = pBelow1;
					}
				}
			}

			res.valid = true;
			if (bestN1 > 0)
			{
				res.doubleScalePresent = true;
				res.firstStageN = bestN1;
				res.bandDropDecades = bestDrop;
				const double collapseFrac = 1.0 - std::pow(10.0, -bestDrop);
				const double snrFrac = 1.0 - gateP / (bestPBelow + gateP);
				res.confidence = ClampD(collapseFrac * snrFrac, 0.0, 1.0);
				res.reason = ResidualSpectrumReason::DoubleScaleConfirmed;
			}
			else
			{
				res.doubleScalePresent = false;
				res.firstStageN = 0;
				res.bandDropDecades = maxObservedDrop;
				res.confidence = 0.0;
				res.reason = ResidualSpectrumReason::SingleStageOnly;
			}
			return res;
		}

		// ============================================================================================================
		// 4. Separable Non-Parametric Tent-Basis Kernel Fit (Alternating Latent / Kernel LS)
		// ============================================================================================================
		inline SeparableKernelFitResult FitSeparableKernel(
			const LineSet& ls,
			int n,
			double phase,
			double floorCodes,
			const ResidualSpectrumSettings& s = ResidualSpectrumSettings())
		{
			SeparableKernelFitResult res;
			if (!s.enabled)
			{
				res.reason = ResidualSpectrumReason::Disabled;
				return res;
			}
			if (!detail_residual_spectrum::ValidLineSet(ls, 32, true) ||
				n <= 1 || n >= ls.axisLen ||
				!std::isfinite(phase) ||
				!std::isfinite(floorCodes) || !(floorCodes > 0.0) ||
				!std::isfinite(s.kernelFitRidge) || s.kernelFitRidge < 0.0)
			{
				res.reason = ResidualSpectrumReason::InvalidInput;
				return res;
			}

			for (int li = 0; li < ls.count; ++li)
			{
				const double* line = ls.Line(li);
				for (int j = 0; j < ls.length; ++j)
				{
					if (!std::isfinite(line[j]))
					{
						res.reason = ResidualSpectrumReason::InvalidInput;
						return res;
					}
				}
			}

			const int halfSup = s.kernelFitHalfSupport < 1 ? 1 : (s.kernelFitHalfSupport > 4 ? 4 : s.kernelFitHalfSupport);
			const int iters = s.kernelFitIterations < 1 ? 1 : (s.kernelFitIterations > 4 ? 4 : s.kernelFitIterations);
			const int taps = 2 * halfSup;
			const int knotCount = 2 * halfSup; // Half-pixel knots: 0.0, 0.5, ..., halfSupport - 0.5 (h(halfSupport) = 0)
			const int L = ls.length;
			const int margin = OperatorMargin(ls.axisLen, n, halfSup);
			if (L - 2 * margin < 16)
			{
				res.reason = ResidualSpectrumReason::InvalidInput;
				return res;
			}

			res.n = n;
			res.N = ls.axisLen;
			res.phase = phase;
			res.halfSupport = halfSup;
			res.knotDistances.resize(static_cast<size_t>(knotCount));
			std::vector<double> knots(static_cast<size_t>(knotCount), 0.0);
			// Initialize from neutral Bicubic (0, 0.5) (or Bilinear when halfSupport == 1).
			const int initBankKernel = halfSup == 1 ? 0 : 1;
			for (int b = 0; b < knotCount; ++b)
			{
				const double dist = 0.5 * static_cast<double>(b);
				res.knotDistances[static_cast<size_t>(b)] = dist;
				knots[static_cast<size_t>(b)] = detail_residual_spectrum::BankKernelValueAt(initBankKernel, dist);
			}

			// Precompute latent coordinates and node indices (exact KernelBank centre-aligned convention).
			const double ratio = static_cast<double>(n) / static_cast<double>(ls.axisLen);
			std::vector<double> pPos(static_cast<size_t>(L), 0.0);
			std::vector<int> cl(static_cast<size_t>(L), 0);
			int cmin = 0;
			int cmax = 0;
			for (int j = 0; j < L; ++j)
			{
				const double p = (static_cast<double>(ls.origin + j) + 0.5) * ratio - 0.5 + phase;
				const int fl = static_cast<int>(std::floor(p));
				const int c = fl - halfSup + 1;
				pPos[static_cast<size_t>(j)] = p;
				cl[static_cast<size_t>(j)] = c;
				if (j == 0 || c < cmin) cmin = c;
				if (j == 0 || c > cmax) cmax = c;
			}
			const int m = cmax + taps - cmin;
			for (int j = 0; j < L; ++j)
				cl[static_cast<size_t>(j)] -= cmin;

			// Compute per-line mean and baseline inner-pixel AC RMS first so flat lines abstain immediately.
			// Subtracting line mean eliminates DC-level conditioning bloat in Phi^T Phi (since normalized W preserves constants).
			std::vector<double> lineMeans(static_cast<size_t>(ls.count), 0.0);
			double baselineSumSq = 0.0;
			int totalInnerRows = 0;
			for (int li = 0; li < ls.count; ++li)
			{
				const double* line = ls.Line(li);
				double mean = 0.0;
				const int innerCount = L - 2 * margin;
				for (int j = margin; j < L - margin; ++j)
					mean += line[j];
				mean /= static_cast<double>(innerCount);
				lineMeans[static_cast<size_t>(li)] = mean;
				for (int j = margin; j < L - margin; ++j)
				{
					const double d = line[j] - mean;
					baselineSumSq += d * d;
				}
				totalInnerRows += innerCount;
			}
			const double baselineRms = std::sqrt(baselineSumSq / static_cast<double>(totalInnerRows));
			if (!(baselineRms > floorCodes))
			{
				res.baselineRms = baselineRms;
				res.reason = ResidualSpectrumReason::LowSignalDetail;
				return res;
			}

			// Hoist TentBasis knot indices & weights once per (j, tap) outside all line & iteration loops.
			const size_t totalTaps = static_cast<size_t>(L) * static_cast<size_t>(taps);
			std::vector<int> tapB0(totalTaps, -1);
			std::vector<int> tapB1(totalTaps, -1);
			std::vector<double> tapW0(totalTaps, 0.0);
			std::vector<double> tapW1(totalTaps, 0.0);
			for (int j = 0; j < L; ++j)
			{
				const double p = pPos[static_cast<size_t>(j)];
				const int baseNode = cl[static_cast<size_t>(j)] + cmin;
				const size_t rowBase = static_cast<size_t>(j) * static_cast<size_t>(taps);
				for (int tap = 0; tap < taps; ++tap)
				{
					const double dist = std::fabs(p - static_cast<double>(baseNode + tap));
					const size_t idx = rowBase + static_cast<size_t>(tap);
					detail_residual_spectrum::TentBasis(dist, halfSup, tapB0[idx], tapW0[idx], tapB1[idx], tapW1[idx]);
				}
			}

			// Flat contiguous buffers reused across lines & iterations (zero per-line / per-iteration heap churn).
			std::vector<double> W(totalTaps, 0.0);
			std::vector<double> latents(static_cast<size_t>(ls.count) * static_cast<size_t>(m), 0.0);
			const int bandU = taps - 1;
			const size_t bw = static_cast<size_t>(bandU + 1);
			std::vector<double> A(static_cast<size_t>(m) * bw, 0.0);
			std::vector<double> chol(static_cast<size_t>(m) * bw, 0.0);

			auto solveLatents = [&]() -> bool {
				for (int j = 0; j < L; ++j)
				{
					const size_t rowBase = static_cast<size_t>(j) * static_cast<size_t>(taps);
					double* row = &W[rowBase];
					double sumW = 0.0;
					for (int tap = 0; tap < taps; ++tap)
					{
						const size_t idx = rowBase + static_cast<size_t>(tap);
						const int b0 = tapB0[idx];
						const int b1 = tapB1[idx];
						double val = 0.0;
						if (b0 >= 0) val += tapW0[idx] * knots[static_cast<size_t>(b0)];
						if (b1 >= 0) val += tapW1[idx] * knots[static_cast<size_t>(b1)];
						row[tap] = val;
						sumW += val;
					}
					if (!(std::fabs(sumW) > 1e-12))
						return false;
					for (int tap = 0; tap < taps; ++tap)
						row[tap] /= sumW;
				}
				std::fill(A.begin(), A.end(), 0.0);
				for (int j = 0; j < L; ++j)
				{
					const double* row = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const int c = cl[static_cast<size_t>(j)];
					for (int s0 = 0; s0 < taps; ++s0)
						for (int t0 = s0; t0 < taps; ++t0)
							A[static_cast<size_t>(c + t0) * bw + static_cast<size_t>(t0 - s0)] += row[s0] * row[t0];
				}
				double maxDiag = 0.0;
				for (int i = 0; i < m; ++i)
					if (A[static_cast<size_t>(i) * bw] > maxDiag)
						maxDiag = A[static_cast<size_t>(i) * bw];
				if (!(maxDiag > 0.0))
					return false;

				double ridge = (std::max)(1e-12, s.kernelFitRidge * 1e-3) * maxDiag;
				bool factored = false;
				for (int attempt = 0; attempt < 3; ++attempt)
				{
					std::copy(A.begin(), A.end(), chol.begin());
					for (int i = 0; i < m; ++i)
						chol[static_cast<size_t>(i) * bw] += ridge;
					if (detail_residual_spectrum::FactorBandedCholesky(m, bandU, chol))
					{
						factored = true;
						break;
					}
					ridge *= 1000.0;
				}
				if (!factored)
					return false;

				for (int li = 0; li < ls.count; ++li)
				{
					const double* line = ls.Line(li);
					const double mu = lineMeans[static_cast<size_t>(li)];
					double* z = &latents[static_cast<size_t>(li) * static_cast<size_t>(m)];
					std::fill(z, z + m, 0.0);
					for (int j = 0; j < L; ++j)
					{
						const double* row = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
						const int c = cl[static_cast<size_t>(j)];
						const double yj = line[j] - mu;
						for (int tap = 0; tap < taps; ++tap)
							z[c + tap] += row[tap] * yj;
					}
					detail_residual_spectrum::SolveBandedCholesky(m, bandU, chol, z);
				}
				return true;
			};

			// Alternating refinement: solve zero-mean latent x given h, then solve kernel knots c given x.
			std::vector<double> phiRow(static_cast<size_t>(knotCount), 0.0);
			std::vector<double> normalMat(static_cast<size_t>(knotCount) * static_cast<size_t>(knotCount), 0.0);
			std::vector<double> rhs(static_cast<size_t>(knotCount), 0.0);
			std::vector<double> newKnots(static_cast<size_t>(knotCount), 0.0);
			for (int iter = 0; iter < iters; ++iter)
			{
				if (!solveLatents())
				{
					res.reason = ResidualSpectrumReason::KernelFitSingular;
					return res;
				}

				std::fill(normalMat.begin(), normalMat.end(), 0.0);
				std::fill(rhs.begin(), rhs.end(), 0.0);
				for (int li = 0; li < ls.count; ++li)
				{
					const double* line = ls.Line(li);
					const double mu = lineMeans[static_cast<size_t>(li)];
					const double* x = &latents[static_cast<size_t>(li) * static_cast<size_t>(m)];
					for (int j = margin; j < L - margin; ++j)
					{
						for (int b = 0; b < knotCount; ++b)
							phiRow[static_cast<size_t>(b)] = 0.0;
						const int c = cl[static_cast<size_t>(j)];
						const size_t rowBase = static_cast<size_t>(j) * static_cast<size_t>(taps);
						for (int tap = 0; tap < taps; ++tap)
						{
							const size_t idx = rowBase + static_cast<size_t>(tap);
							const int b0 = tapB0[idx];
							const int b1 = tapB1[idx];
							const double xv = x[c + tap];
							if (b0 >= 0) phiRow[static_cast<size_t>(b0)] += tapW0[idx] * xv;
							if (b1 >= 0) phiRow[static_cast<size_t>(b1)] += tapW1[idx] * xv;
						}
						const double yj = line[j] - mu;
						for (int rIdx = 0; rIdx < knotCount; ++rIdx)
						{
							const double pr = phiRow[static_cast<size_t>(rIdx)];
							rhs[static_cast<size_t>(rIdx)] += pr * yj;
							for (int cIdx = 0; cIdx < knotCount; ++cIdx)
								normalMat[static_cast<size_t>(rIdx) * static_cast<size_t>(knotCount) + static_cast<size_t>(cIdx)] +=
									pr * phiRow[static_cast<size_t>(cIdx)];
						}
					}
				}

				double maxDiag = 0.0;
				for (int b = 0; b < knotCount; ++b)
				{
					const double d = normalMat[static_cast<size_t>(b) * static_cast<size_t>(knotCount) + static_cast<size_t>(b)];
					if (d > maxDiag)
						maxDiag = d;
				}
				if (!(maxDiag > 0.0))
				{
					res.reason = ResidualSpectrumReason::KernelFitSingular;
					return res;
				}
				const double reg = s.kernelFitRidge * maxDiag;
				for (int b = 0; b < knotCount; ++b)
					normalMat[static_cast<size_t>(b) * static_cast<size_t>(knotCount) + static_cast<size_t>(b)] += reg;

				if (!detail_residual_spectrum::SolveSmallDense(knotCount, normalMat, rhs, newKnots))
				{
					res.reason = ResidualSpectrumReason::KernelFitSingular;
					return res;
				}

				// DC partition-of-unity normalization: integral_{-R}^{R} h(t) dt = 0.5 * c_0 + sum_{b>=1} c_b = 1.
				double integral = 0.5 * newKnots[0];
				for (int b = 1; b < knotCount; ++b)
					integral += newKnots[static_cast<size_t>(b)];
				if (!(std::fabs(integral) > 1e-12))
				{
					res.reason = ResidualSpectrumReason::KernelFitSingular;
					return res;
				}
				for (int b = 0; b < knotCount; ++b)
					knots[static_cast<size_t>(b)] = newKnots[static_cast<size_t>(b)] / integral;
			}

			// Final latent pass with converged normalized kernel to compute exact DOF-normalized fitRms.
			if (!solveLatents())
			{
				res.reason = ResidualSpectrumReason::KernelFitSingular;
				return res;
			}

			double rss = 0.0;
			for (int li = 0; li < ls.count; ++li)
			{
				const double* line = ls.Line(li);
				const double mu = lineMeans[static_cast<size_t>(li)];
				const double* x = &latents[static_cast<size_t>(li) * static_cast<size_t>(m)];
				for (int j = margin; j < L - margin; ++j)
				{
					const double* row = &W[static_cast<size_t>(j) * static_cast<size_t>(taps)];
					const int c = cl[static_cast<size_t>(j)];
					double pred = 0.0;
					for (int tap = 0; tap < taps; ++tap)
						pred += row[tap] * x[c + tap];
					const double err = (line[j] - mu) - pred;
					rss += err * err;
				}
			}

			const double dofDenom = 1.0 - ratio;
			res.valid = true;
			res.knotWeights = knots;
			res.fitRms = std::sqrt(rss / (static_cast<double>(totalInnerRows) * dofDenom));
			res.baselineRms = baselineRms;
			res.rSquared = ClampD(1.0 - (res.fitRms * res.fitRms) / (baselineRms * baselineRms), 0.0, 1.0);

			// Nearest KernelBank kernel by L2 distance on quarter-pixel evaluation grid in [0, halfSupport).
			int bestBank = -1;
			double bestDistSq = 1e300;
			const int gridSteps = 4 * halfSup;
			for (int bk = 0; bk < kKernelCount; ++bk)
			{
				double distSq = 0.0;
				for (int g = 0; g < gridSteps; ++g)
				{
					const double d = static_cast<double>(g) / 4.0;
					const double diff = res.EvaluateKernel(d) - detail_residual_spectrum::BankKernelValueAt(bk, d);
					distSq += diff * diff;
				}
				if (distSq < bestDistSq)
				{
					bestDistSq = distSq;
					bestBank = bk;
				}
			}
			res.nearestBankKernel = bestBank;
			res.nearestBankFamily = bestBank >= 0 ? static_cast<int>(Kernel(bestBank).family) : -1;
			res.reason = ResidualSpectrumReason::Ok;
			return res;
		}
	}
}

#pragma fp_contract(on)
