// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// NeuralSrWitness (SID-C4 / Neural Super-Resolution Positive Witness):
// Pure C++14 positive witness distinguishing nonlinear AI / Neural Super-Resolution (DLSS, FSR, Topaz,
// GAN, shock-filter edge-steepening upscalers) from both:
//   (a) classical LTI resampling (Lanczos, Bicubic, Spline, Bilinear), and
//   (b) native sharp photographic / CG 1/f^2 broadband content.
//
// Physics & Rule 1 constant derivations (zero invented magic numbers):
//   - Pillar 1 (Classical LTI Veto):
//       ltiDipVeto = -0.09 = 3.0 * EvidenceSettings::majorityDip (-0.03, ChainEvidence.h:92; matching
//       ChainResidualSpectrum.h:45). Any candidate whose best classical LTI descale statistic clears
//       this 3x majority dip is already explained by LTI resampling => NotNeural / ClassicalLtiConfirmed.
//   - Disjoint Spectral Split around candidate Nyquist f_c = 0.5 * (candidateN / axisLen):
//       * Low-pass y_L:  bins 1 .. floor(0.90 * cut), matching W1 SourceResolutionDetector.cpp 0.90 * f_c.
//       * Below-band y_Below: bins ceil(0.55 * cut) .. floor(0.90 * cut), matching W1 [0.55*f_c, 0.90*f_c]
//         with band midpoint fBelow = 0.5 * (0.55 + 0.90) * f_c.
//       * High-pass y_H: bins ceil(1.05 * cut) .. floor(0.95 * half), strictly disjoint from y_L
//         (1.05 * cut > 0.90 * cut guard matching ReadingSettings::kernelMarginScale = 0.05; 0.95 * half
//         Nyquist trim matching ChainResidualSpectrum.h:18), with midpoint fAbove = 0.5 * (1.05*f_c + 0.95*0.5).
//       * Raised-cosine spectral tapers inside each band preserve exact disjointness (H_L[k] * H_H[k] == 0
//         for all k) while suppressing 1/d brick-wall Gibbs ringing into soft interiors to 1/d^3.
//       * Parseval band normalization: for a symmetric real band mask H[k] (k = 1..half-1), white noise of
//         variance floorP = floorCodes^2 produces E[y_band^2] = bandFrac * floorP where
//         bandFrac = (2.0 / fftN) * sum(H[k]^2). Dividing spatial mean square by bandFrac yields exact
//         per-bin power in floorCodes^2 units across every upscale ratio.
//       * Spatial Tukey taper on the outer margin = fftN / 8 (= 32 px for fftN = 256) with unity gain on
//         [margin, fftN - margin) prevents circular wrap-around leakage without distorting interior samples;
//         segments tile [margin, length - margin) with zero duplicate pixel counting.
//   - Pillar 2 (Cross-Scale Edge-Phase Bicoherence + Spatial HF Localization):
//       * Edge gradient threshold: edgeGradientSigma = 3.0 (3-sigma Gaussian tail from ChainEdgeStats.h:110)
//         gives noise floor gate 3 * sqrt(2) * floorCodes on low-pass gradient gL, combined with relative
//         prominence gate lineGateFactor * rms(gL) (lineGateFactor = 1.5, ChainEvidence.h:87) so gentle
//         interior textures are separated from strong edges.
//       * Edge-crest phase lock (non-maximum-suppressed local maxima of |gL| inside edgeMask):
//         for any sharp or nonlinearly steepened step centered at sub-pixel offset |delta| <= 0.5 from its
//         nearest integer crest pixel, 2*pi*f*|delta| <= pi/2 for every f in (0, 0.5), so cos(2*pi*f*delta) >= 0
//         across all above-cutoff frequencies and gH * gL > 0 without adjacent-pixel (-1/+1) phase reversal.
//         On stationary random-phase native 1/f^2 fields, disjoint Fourier bands y_L and y_H are independent,
//         so E[gH * gL | gL] == 0 regardless of the gL-only crest mask.
//       * minBicoherence = 0.25 (elementary 1/4 positive cross-scale correlation threshold).
//       * Edge HF energy gate: per-bin edge HF power > lineGateFactor^2 * floorCodes^2 = 2.25 * floorP
//         (ChainEvidence.h:87 / ChainResidualSpectrum.h:20).
//       * minEdgeToInteriorHfRatio = 4.0 (elementary 2^2 = 6 dB concentration of above-cutoff HF power on
//         strong edges vs +-2-px-dilated-edge-excluded textured interiors).
//       * minEdgeKurtosisRatio = 2.0 (elementary 2x above Gaussian baseline kurtosis 3.0 => hfKurtosis >= 6.0,
//         evaluated strictly on active non-flat pixels so flat sky/letterbox regions cannot inflate kurtosis).
//   - Pillar 3 (Interior PSD Step Discontinuity vs Natural 1/f^2):
//       * Conservative natural 1/f^2 spectral drop across f_c: expectedDrop = -2.0 * log10(fAbove / fBelow)
//         (SourceResolutionDetector.h:354).
//       * interiorDropDecadesMargin = 0.5 (nativeFloorDecades = 0.5, SourceResolutionDetector.h:361).
//   - Sample / line consensus floors:
//       * fftSize = kSegPx = 256 (ChainCommon.h:27).
//       * minValidLines = kMaxLines / 16 = 4 (ChainCommon.h:25).
//       * minEdgeSamples = fftSize / 16 = 16; minInteriorSamples = fftSize / 8 = 32 (protects flat-interior
//         vector graphics from ever being misclassified as neural SR).
//   - Strictly numerical singularity guard: kNumericEps = 1e-18 (never a physical threshold).
//
// Pure C++14, header-only, deterministic doubles, fixed loop order, no Windows headers, no threads/clocks,
// zero static/thread_local mutable state (all caches live in caller-owned NeuralSrWorkspace).
// OFF by default (enabled = false); every degraded or abstained path returns an explicit NeuralSrReason.

#include "ChainCommon.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#pragma fp_contract(off)

namespace SourceIdentity
{
	namespace Chain
	{
		enum class NeuralSrVerdict : int
		{
			Unknown = 0,
			NotNeural = 1,
			NeuralSrDetected = 2,
			Abstain = 3
		};

		inline const char* NeuralSrVerdictName(NeuralSrVerdict v)
		{
			switch (v)
			{
			case NeuralSrVerdict::Unknown: return "unknown";
			case NeuralSrVerdict::NotNeural: return "not_neural";
			case NeuralSrVerdict::NeuralSrDetected: return "neural_sr_detected";
			case NeuralSrVerdict::Abstain: return "abstain";
			default: return "unknown";
			}
		}

		enum class NeuralSrReason : int
		{
			Ok = 0,
			Disabled,
			InvalidInput,
			ClassicalLtiConfirmed,
			InsufficientEdgeActivity,
			InsufficientInteriorTexture,
			NaturalBroadbandSpectrum,
			NeuralEdgeLockAndInteriorDrop,
			AmbiguousSignal
		};

		inline const char* NeuralSrReasonText(NeuralSrReason r)
		{
			switch (r)
			{
			case NeuralSrReason::Ok: return "ok";
			case NeuralSrReason::Disabled: return "neural SR witness disabled";
			case NeuralSrReason::InvalidInput: return "invalid input";
			case NeuralSrReason::ClassicalLtiConfirmed: return "classical LTI resampling confirmed (LTI veto)";
			case NeuralSrReason::InsufficientEdgeActivity: return "abstain (insufficient edge activity)";
			case NeuralSrReason::InsufficientInteriorTexture: return "abstain (insufficient interior texture)";
			case NeuralSrReason::NaturalBroadbandSpectrum: return "natural broadband spectrum (not neural SR)";
			case NeuralSrReason::NeuralEdgeLockAndInteriorDrop: return "AI-upscaled (skip LTI inverse): edge phase-lock and interior HF drop";
			case NeuralSrReason::AmbiguousSignal: return "ambiguous signal (insufficient consensus or edge HF below floor)";
			default: return "unknown";
			}
		}

		struct NeuralSrSettings
		{
			bool enabled = false;                       // Strictly OFF by default (W1 bit-exact preservation)
			double ltiDipVeto = -0.09;                  // Derived: 3.0 * EvidenceSettings::majorityDip (-0.03, ChainEvidence.h:92)
			double lineGateFactor = 1.5;                // ChainEvidence.h:87 contract: amplitude gate 1.5x => power gate 2.25x
			double edgeGradientSigma = 3.0;             // ChainEdgeStats.h:110 contract: 3-sigma Gaussian tail on floor gradient
			double minBicoherence = 0.25;               // Elementary 1/4 cross-scale edge-gradient phase-lock threshold (E[rho]=0 on 1/f^2)
			double minEdgeToInteriorHfRatio = 4.0;      // Derived: 2^2 = 4.0 (6 dB HF energy concentration on edges vs textured interiors)
			double interiorDropDecadesMargin = 0.5;     // Derived: nativeFloorDecades = 0.5 (SourceResolutionDetector.h:361)
			double minEdgeKurtosisRatio = 2.0;          // Derived: 2x Gaussian baseline kurtosis (3.0) => hfKurtosis >= 6.0
			int fftSize = kSegPx;                       // ChainCommon.h:27: 256-sample power-of-two segment FFT
			int minValidLines = kMaxLines / 16;         // Derived: kMaxLines (64) / 16 = 4 lines minimum multi-line consensus
			int minEdgeSamples = kSegPx / 16;           // Derived: fftSize / 16 = 16 strong-edge samples minimum
			int minInteriorSamples = kSegPx / 8;        // Derived: fftSize / 8 = 32 textured interior samples minimum
		};

		// Caller-owned bounded reusable workspace (Rule 2: zero static/thread_local mutable caches).
		struct NeuralSrWorkspace
		{
			// Precomputed radix-2 FFT twiddle tables and spatial Tukey taper for size fftN.
			int fftN = 0;
			std::vector<double> cosTab;
			std::vector<double> sinTab;
			std::vector<size_t> bitRev;
			std::vector<double> taperWin;
			double winEnergy = 0.0;

			// Reusable per-segment FFT and band buffers.
			std::vector<double> re;
			std::vector<double> im;
			std::vector<double> specRe;
			std::vector<double> specIm;
			std::vector<double> segLow;
			std::vector<double> segHigh;
			std::vector<double> segBelow;
			std::vector<double> lowWeights;
			std::vector<double> highWeights;
			std::vector<double> belowWeights;

			// Reusable per-line interior signal, gradient, and mask buffers.
			std::vector<double> yLow;
			std::vector<double> yHigh;
			std::vector<double> yBelow;
			std::vector<double> gradLow;
			std::vector<double> gradHigh;
			std::vector<uint8_t> edgeMask;

			// Reusable diagnostic PSD and per-line witness buffers.
			std::vector<double> linePsd;
			std::vector<double> psdEdge;
			std::vector<double> psdInterior;
			std::vector<double> lineBi;
			std::vector<double> lineRatio;

			void Reset()
			{
				fftN = 0;
				winEnergy = 0.0;
				cosTab.clear();
				sinTab.clear();
				bitRev.clear();
				taperWin.clear();
				re.clear();
				im.clear();
				specRe.clear();
				specIm.clear();
				segLow.clear();
				segHigh.clear();
				segBelow.clear();
				lowWeights.clear();
				highWeights.clear();
				belowWeights.clear();
				yLow.clear();
				yHigh.clear();
				yBelow.clear();
				gradLow.clear();
				gradHigh.clear();
				edgeMask.clear();
				linePsd.clear();
				psdEdge.clear();
				psdInterior.clear();
				lineBi.clear();
				lineRatio.clear();
			}
		};

		struct NeuralSrResult
		{
			NeuralSrVerdict verdict = NeuralSrVerdict::Unknown;
			NeuralSrReason reason = NeuralSrReason::Disabled;
			bool neuralUpscaled = false;
			double edgePhaseBicoherence = 0.0;
			double edgeToInteriorHfRatio = 0.0;
			double hfKurtosis = 0.0;
			double interiorSpectralDropExcessDecades = 0.0;
			int validLines = 0;
			int edgeSamples = 0;
			int interiorSamples = 0;
		};

		namespace detail_neural_sr
		{
			constexpr double kNumericEps = 1e-18; // Strictly numerical denominator guard (Rule 1)
			constexpr uint8_t kMaskEdge = 1u;     // Strong edge pixel: |gL| >= edgeGate
			constexpr uint8_t kMaskEdgeZone = 2u; // +-2 px dilated edge zone excluded from interior
			constexpr uint8_t kMaskInterior = 4u; // Non-edgeZone textured interior (5-px local |gL| > floorCodes)
			constexpr uint8_t kMaskCrest = 8u;    // Local maximum of |gL| within strong edge (sub-pixel phase-aligned crest)

			inline bool IsValidFftSize(int n)
			{
				// Power-of-two segment FFT in [kSegPx / 4 (64), 16 * kSegPx (4096)], keeping workspace memory bounded.
				return n >= (kSegPx / 4) && n <= (16 * kSegPx) && (n & (n - 1)) == 0;
			}

			inline bool ValidateSettings(const NeuralSrSettings& cfg)
			{
				if (!IsValidFftSize(cfg.fftSize))
					return false;
				if (!std::isfinite(cfg.ltiDipVeto))
					return false;
				if (!std::isfinite(cfg.lineGateFactor) || !(cfg.lineGateFactor > 0.0))
					return false;
				if (!std::isfinite(cfg.edgeGradientSigma) || !(cfg.edgeGradientSigma > 0.0))
					return false;
				if (!std::isfinite(cfg.minBicoherence))
					return false;
				if (!std::isfinite(cfg.minEdgeToInteriorHfRatio) || !(cfg.minEdgeToInteriorHfRatio > 0.0))
					return false;
				if (!std::isfinite(cfg.interiorDropDecadesMargin))
					return false;
				if (!std::isfinite(cfg.minEdgeKurtosisRatio) || !(cfg.minEdgeKurtosisRatio > 0.0))
					return false;
				if (cfg.minValidLines < 1 || cfg.minEdgeSamples < 1 || cfg.minInteriorSamples < 1)
					return false;
				return true;
			}

			inline bool ValidateLineSet(const LineSet& ls, int minLen)
			{
				if (ls.count <= 0 || ls.axisLen <= 0 || ls.length < minLen)
					return false;
				if (ls.data.size() / static_cast<size_t>(ls.length) < static_cast<size_t>(ls.count))
					return false;
				const size_t total = static_cast<size_t>(ls.count) * static_cast<size_t>(ls.length);
				for (size_t idx = 0; idx < total; ++idx)
				{
					if (!std::isfinite(ls.data[idx]))
						return false;
				}
				return true;
			}

			// Builds radix-2 FFT twiddle tables, bit-reversal table, and raised-cosine Tukey taper
			// (unity on [margin, fftN - margin) with margin = fftN / 8) inside caller-owned workspace.
			inline void EnsureWorkspaceFft(NeuralSrWorkspace& ws, int fftN)
			{
				if (ws.fftN == fftN &&
					ws.cosTab.size() == static_cast<size_t>(fftN / 2) &&
					ws.sinTab.size() == static_cast<size_t>(fftN / 2) &&
					ws.bitRev.size() == static_cast<size_t>(fftN) &&
					ws.taperWin.size() == static_cast<size_t>(fftN))
				{
					return;
				}

				const size_t n = static_cast<size_t>(fftN);
				const size_t half = n >> 1;
				ws.fftN = fftN;

				ws.bitRev.assign(n, 0u);
				for (size_t i = 1, j = 0; i < n; ++i)
				{
					size_t bit = half;
					for (; (j & bit) != 0u; bit >>= 1)
						j ^= bit;
					j ^= bit;
					ws.bitRev[i] = j;
				}

				ws.cosTab.assign(half, 0.0);
				ws.sinTab.assign(half, 0.0);
				const double angStep = -2.0 * kPi / static_cast<double>(fftN);
				for (size_t k = 0; k < half; ++k)
				{
					const double ang = angStep * static_cast<double>(k);
					ws.cosTab[k] = std::cos(ang);
					ws.sinTab[k] = std::sin(ang);
				}

				const int margin = fftN / 8;
				ws.taperWin.assign(n, 1.0);
				double energy = 0.0;
				for (int i = 0; i < fftN; ++i)
				{
					double w = 1.0;
					if (i < margin)
					{
						w = 0.5 * (1.0 - std::cos(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(margin)));
					}
					else if (i >= fftN - margin)
					{
						const double d = static_cast<double>(fftN) - 0.5 - static_cast<double>(i);
						w = 0.5 * (1.0 - std::cos(kPi * d / static_cast<double>(margin)));
					}
					ws.taperWin[static_cast<size_t>(i)] = w;
					energy += w * w;
				}
				ws.winEnergy = energy;
			}

			// Deterministic iterative radix-2 Cooley-Tukey FFT / IFFT using workspace twiddle tables.
			inline void FftRadix2(std::vector<double>& re, std::vector<double>& im, const NeuralSrWorkspace& ws, bool inverse)
			{
				const size_t n = static_cast<size_t>(ws.fftN);
				for (size_t i = 1; i < n; ++i)
				{
					const size_t j = ws.bitRev[i];
					if (i < j)
					{
						std::swap(re[i], re[j]);
						std::swap(im[i], im[j]);
					}
				}

				const double* cosPtr = ws.cosTab.data();
				const double* sinPtr = ws.sinTab.data();
				const double sinSign = inverse ? -1.0 : 1.0;

				for (size_t len = 2; len <= n; len <<= 1)
				{
					const size_t halfLen = len >> 1;
					const size_t stride = n / len;
					for (size_t start = 0; start < n; start += len)
					{
						for (size_t j = 0; j < halfLen; ++j)
						{
							const size_t twIdx = j * stride;
							const double wr = cosPtr[twIdx];
							const double wi = sinSign * sinPtr[twIdx];
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

				if (inverse)
				{
					const double invN = 1.0 / static_cast<double>(ws.fftN);
					for (size_t i = 0; i < n; ++i)
					{
						re[i] *= invN;
						im[i] *= invN;
					}
				}
			}

			// Builds strictly band-supported raised-cosine spectral weights H[k] on [kLo, kHi]
			// (zero everywhere outside [kLo, kHi], guaranteeing zero overlap between disjoint bands)
			// and returns exact Parseval white-noise variance fraction (2 / fftN) * sum(H[k]^2).
			inline double BuildBandWeights(
				int fftN,
				int kLo,
				int kHi,
				bool taperLeft,
				bool taperRight,
				std::vector<double>& weights)
			{
				const int half = fftN / 2;
				weights.assign(static_cast<size_t>(half + 1), 0.0);
				if (kLo < 1 || kHi >= half || kLo > kHi)
					return 0.0;

				const int nBins = kHi - kLo + 1;
				// Smooth C^1 roll-off: <= 1/4 of band width, capped at margin / 4 = fftN / 32 (= 8 bins at fftN = 256).
				const int maxTaper = (std::max)(1, fftN / 32);
				const int taperLen = (std::min)(nBins / 4, maxTaper);
				double sumSq = 0.0;
				for (int k = kLo; k <= kHi; ++k)
				{
					double w = 1.0;
					if (taperLeft && taperLen > 0)
					{
						const int distL = k - kLo;
						if (distL < taperLen)
							w *= 0.5 * (1.0 - std::cos(kPi * (static_cast<double>(distL) + 0.5) / static_cast<double>(taperLen)));
					}
					if (taperRight && taperLen > 0)
					{
						const int distR = kHi - k;
						if (distR < taperLen)
							w *= 0.5 * (1.0 - std::cos(kPi * (static_cast<double>(distR) + 0.5) / static_cast<double>(taperLen)));
					}
					weights[static_cast<size_t>(k)] = w;
					sumSq += w * w;
				}
				return (2.0 / static_cast<double>(fftN)) * sumSq;
			}

			// Reconstructs zero-phase real band-pass signal from cached Hermitian spectrum (specRe, specIm).
			inline void InverseBandSignal(
				NeuralSrWorkspace& ws,
				int kLo,
				int kHi,
				const std::vector<double>& weights,
				std::vector<double>& outSignal)
			{
				const int fftN = ws.fftN;
				const size_t n = static_cast<size_t>(fftN);
				ws.re.assign(n, 0.0);
				ws.im.assign(n, 0.0);
				for (int k = kLo; k <= kHi; ++k)
				{
					const size_t ku = static_cast<size_t>(k);
					const size_t km = static_cast<size_t>(fftN - k);
					const double w = weights[ku];
					ws.re[ku] = ws.specRe[ku] * w;
					ws.im[ku] = ws.specIm[ku] * w;
					ws.re[km] = ws.specRe[km] * w;
					ws.im[km] = ws.specIm[km] * w;
				}
				FftRadix2(ws.re, ws.im, ws, true);
				outSignal.resize(n);
				for (size_t i = 0; i < n; ++i)
					outSignal[i] = ws.re[i];
			}
		}

		class NeuralSrWitness
		{
		public:
			// Exact 3-Pillar Neural SR Witness:
			//   Pillar 1: Classical LTI Veto (bestLtiStat <= ltiDipVeto => NotNeural / ClassicalLtiConfirmed).
			//   Pillar 2: Disjoint spectral split + cross-scale edge-crest phase bicoherence + edge-vs-interior HF localization + active-pixel HF kurtosis.
			//   Pillar 3: Non-edge textured interior PSD step collapse across f_c relative to natural -2*log10(fAbove/fBelow) 1/f^2 slope.
			static NeuralSrResult Evaluate(
				const LineSet& ls,
				int candidateN,
				double bestLtiStat,
				double floorCodes,
				const NeuralSrSettings& cfg,
				NeuralSrWorkspace& ws)
			{
				NeuralSrResult res;
				if (!cfg.enabled)
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::Disabled;
					return res;
				}

				if (!detail_neural_sr::ValidateSettings(cfg) ||
					!detail_neural_sr::ValidateLineSet(ls, cfg.fftSize) ||
					candidateN < 16 || candidateN >= ls.axisLen ||
					!std::isfinite(floorCodes) || !(floorCodes > 0.0))
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::InvalidInput;
					return res;
				}

				const int fftN = cfg.fftSize;
				const int half = fftN / 2;
				const double ratio = static_cast<double>(candidateN) / static_cast<double>(ls.axisLen);
				const double cut = ratio * static_cast<double>(half);

				// Disjoint spectral band bin bounds (matching W1 0.55..0.90 below-band and 1.05..0.95*half above-band).
				const int lowLo = 1;
				const int lowHi = static_cast<int>(std::floor(0.90 * cut));
				const int belowLo = (std::max)(1, static_cast<int>(std::ceil(0.55 * cut)));
				const int belowHi = lowHi;
				const int highLo = static_cast<int>(std::ceil(1.05 * cut));
				const int highHi = static_cast<int>(std::floor(0.95 * static_cast<double>(half)));

				if (lowHi < lowLo || belowHi < belowLo || highLo >= highHi || highLo <= lowHi)
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::InvalidInput;
					return res;
				}

				// Pillar 1: Classical LTI Veto.
				if (std::isfinite(bestLtiStat) && bestLtiStat <= cfg.ltiDipVeto)
				{
					res.verdict = NeuralSrVerdict::NotNeural;
					res.reason = NeuralSrReason::ClassicalLtiConfirmed;
					res.neuralUpscaled = false;
					return res;
				}

				detail_neural_sr::EnsureWorkspaceFft(ws, fftN);
				const double bandFracLow = detail_neural_sr::BuildBandWeights(fftN, lowLo, lowHi, false, true, ws.lowWeights);
				const double bandFracBelow = detail_neural_sr::BuildBandWeights(fftN, belowLo, belowHi, true, true, ws.belowWeights);
				const double bandFracHigh = detail_neural_sr::BuildBandWeights(fftN, highLo, highHi, true, true, ws.highWeights);
				if (!(bandFracLow > 0.0) || !(bandFracBelow > 0.0) || !(bandFracHigh > 0.0))
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::InvalidInput;
					return res;
				}

				const int margin = fftN / 8;
				const int innerLen = fftN - 2 * margin;
				const int evalTotal = ls.length - 2 * margin;
				if (innerLen <= 0 || evalTotal <= 0)
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::InvalidInput;
					return res;
				}

				const size_t nFft = static_cast<size_t>(fftN);
				const size_t nHalfPlus1 = static_cast<size_t>(half + 1);
				const size_t nEval = static_cast<size_t>(evalTotal);

				ws.re.resize(nFft);
				ws.im.resize(nFft);
				ws.specRe.resize(nFft);
				ws.specIm.resize(nFft);
				ws.segLow.resize(nFft);
				ws.segHigh.resize(nFft);
				ws.segBelow.resize(nFft);

				ws.yLow.resize(nEval);
				ws.yHigh.resize(nEval);
				ws.yBelow.resize(nEval);
				ws.gradLow.resize(nEval);
				ws.gradHigh.resize(nEval);
				ws.edgeMask.resize(nEval);

				ws.linePsd.assign(nHalfPlus1, 0.0);
				ws.psdEdge.assign(nHalfPlus1, 0.0);
				ws.psdInterior.assign(nHalfPlus1, 0.0);
				ws.lineBi.clear();
				ws.lineRatio.clear();

				const double floorP = floorCodes * floorCodes;
				const double noiseEdgeGate = cfg.edgeGradientSigma * std::sqrt(2.0) * floorCodes;
				const double psdNormDenom = static_cast<double>(fftN) * ws.winEnergy;

				double totalCrestCross = 0.0;
				double totalCrestGH2 = 0.0;
				double totalCrestGL2 = 0.0;
				double totalEdgeYH2 = 0.0;
				double totalEdgeYBelow2 = 0.0;
				double totalIntYH2 = 0.0;
				double totalIntYBelow2 = 0.0;
				double totalActiveYH2 = 0.0;
				double totalActiveYH4 = 0.0;
				int totalEdgeSamples = 0;
				int totalInteriorSamples = 0;
				int totalActiveSamples = 0;
				int validLines = 0;

				for (int lineIdx = 0; lineIdx < ls.count; ++lineIdx)
				{
					const double* line = ls.Line(lineIdx);
					ws.linePsd.assign(nHalfPlus1, 0.0);
					int segCount = 0;

					// Tile [margin, ls.length - margin) strictly once per pixel using fftN-sample segments
					// with margin = fftN / 8 context on both ends so circular boundary effects never leak.
					int coveredEnd = margin;
					for (int segStart = 0; ; )
					{
						double segMean = 0.0;
						for (int s = 0; s < fftN; ++s)
							segMean += line[segStart + s];
						segMean /= static_cast<double>(fftN);

						for (int s = 0; s < fftN; ++s)
						{
							ws.re[static_cast<size_t>(s)] = (line[segStart + s] - segMean) * ws.taperWin[static_cast<size_t>(s)];
							ws.im[static_cast<size_t>(s)] = 0.0;
						}

						detail_neural_sr::FftRadix2(ws.re, ws.im, ws, false);
						for (size_t k = 0; k < nFft; ++k)
						{
							ws.specRe[k] = ws.re[k];
							ws.specIm[k] = ws.im[k];
						}

						if (psdNormDenom > 0.0)
						{
							const double invDen = 1.0 / psdNormDenom;
							ws.linePsd[0] += (ws.specRe[0] * ws.specRe[0] + ws.specIm[0] * ws.specIm[0]) * invDen;
							for (int k = 1; k < half; ++k)
							{
								const size_t ku = static_cast<size_t>(k);
								ws.linePsd[ku] += 2.0 * (ws.specRe[ku] * ws.specRe[ku] + ws.specIm[ku] * ws.specIm[ku]) * invDen;
							}
							const size_t kh = static_cast<size_t>(half);
							ws.linePsd[kh] += (ws.specRe[kh] * ws.specRe[kh] + ws.specIm[kh] * ws.specIm[kh]) * invDen;
						}
						++segCount;

						detail_neural_sr::InverseBandSignal(ws, lowLo, lowHi, ws.lowWeights, ws.segLow);
						detail_neural_sr::InverseBandSignal(ws, highLo, highHi, ws.highWeights, ws.segHigh);
						detail_neural_sr::InverseBandSignal(ws, belowLo, belowHi, ws.belowWeights, ws.segBelow);

						const int evalLo = (std::max)(margin, coveredEnd - segStart);
						const int evalHi = fftN - margin;
						for (int s = evalLo; s < evalHi; ++s)
						{
							const size_t su = static_cast<size_t>(s);
							const size_t outIdx = static_cast<size_t>((segStart + s) - margin);
							ws.yLow[outIdx] = ws.segLow[su];
							ws.yHigh[outIdx] = ws.segHigh[su];
							ws.yBelow[outIdx] = ws.segBelow[su];
							ws.gradLow[outIdx] = 0.5 * (ws.segLow[su + 1u] - ws.segLow[su - 1u]);
							ws.gradHigh[outIdx] = 0.5 * (ws.segHigh[su + 1u] - ws.segHigh[su - 1u]);
						}
						coveredEnd = segStart + evalHi;

						if (coveredEnd >= ls.length - margin)
							break;
						const int nextStart = segStart + innerLen;
						if (nextStart + fftN <= ls.length)
							segStart = nextStart;
						else
							segStart = ls.length - fftN;
					}

					if (segCount > 0)
					{
						const double invSegs = 1.0 / static_cast<double>(segCount);
						for (size_t k = 0; k < nHalfPlus1; ++k)
							ws.linePsd[k] *= invSegs;
					}

					// Pillar 2: Strong edge mask, edge-crest phase lock, +-2 px edge dilation, and textured interior mask.
					double sumGL2 = 0.0;
					for (size_t idx = 0; idx < nEval; ++idx)
						sumGL2 += ws.gradLow[idx] * ws.gradLow[idx];
					const double rmsGL = std::sqrt(sumGL2 / static_cast<double>(evalTotal));
					const double edgeGate = (std::max)(noiseEdgeGate, cfg.lineGateFactor * rmsGL);

					ws.edgeMask.assign(nEval, 0u);
					for (int idx = 0; idx < evalTotal; ++idx)
					{
						const size_t u = static_cast<size_t>(idx);
						if (std::fabs(ws.gradLow[u]) >= edgeGate)
							ws.edgeMask[u] |= detail_neural_sr::kMaskEdge;
					}

					// Mark dilated edgeZone (1.5 latent cycles = KernelBank support margin, >= 2 px)
					// and non-maximum-suppressed edge crests.
					const int edgeHalo = (std::max)(2, static_cast<int>(std::ceil(1.5 / ratio)));
					for (int idx = 0; idx < evalTotal; ++idx)
					{
						const size_t u = static_cast<size_t>(idx);
						if ((ws.edgeMask[u] & detail_neural_sr::kMaskEdge) != 0u)
						{
							const int dilLo = (std::max)(0, idx - edgeHalo);
							const int dilHi = (std::min)(evalTotal - 1, idx + edgeHalo);
							for (int d = dilLo; d <= dilHi; ++d)
								ws.edgeMask[static_cast<size_t>(d)] |= detail_neural_sr::kMaskEdgeZone;

							const double absGL = std::fabs(ws.gradLow[u]);
							const bool leftOk = (idx == 0) || (absGL >= std::fabs(ws.gradLow[u - 1u]));
							const bool rightOk = (idx + 1 == evalTotal) || (absGL >= std::fabs(ws.gradLow[u + 1u]));
							if (leftOk && rightOk)
								ws.edgeMask[u] |= detail_neural_sr::kMaskCrest;
						}
					}

					// Identify textured non-edge interior samples: outside dilated edgeZone, requiring BOTH
					// low-pass gradient activity AND raw-signal local activity > floorCodes so FFT Gibbs ringing
					// from crisp step edges on a flat vector-graphics background can never fabricate fake interior texture.
					for (int idx = 0; idx < evalTotal; ++idx)
					{
						const size_t u = static_cast<size_t>(idx);
						if ((ws.edgeMask[u] & detail_neural_sr::kMaskEdgeZone) != 0u)
							continue;
						const int winLo = (std::max)(0, idx - 2);
						const int winHi = (std::min)(evalTotal - 1, idx + 2);
						double sumAbsGL = 0.0;
						double sumAbsRaw = 0.0;
						for (int w = winLo; w <= winHi; ++w)
						{
							sumAbsGL += std::fabs(ws.gradLow[static_cast<size_t>(w)]);
							const int rawPos = margin + w;
							sumAbsRaw += 0.5 * std::fabs(line[rawPos + 1] - line[rawPos - 1]);
						}
						const double invWin = 1.0 / static_cast<double>(winHi - winLo + 1);
						const double localActivity = sumAbsGL * invWin;
						const double rawActivity = sumAbsRaw * invWin;
						if (localActivity > floorCodes && rawActivity > floorCodes)
							ws.edgeMask[u] |= detail_neural_sr::kMaskInterior;
					}

					double lineCrestCross = 0.0;
					double lineCrestGH2 = 0.0;
					double lineCrestGL2 = 0.0;
					double lineEdgeYH2 = 0.0;
					double lineEdgeYBelow2 = 0.0;
					double lineIntYH2 = 0.0;
					double lineIntYBelow2 = 0.0;
					int lineEdgeCount = 0;
					int lineInteriorCount = 0;

					for (size_t idx = 0; idx < nEval; ++idx)
					{
						const uint8_t m = ws.edgeMask[idx];
						const double yh = ws.yHigh[idx];
						const double yh2 = yh * yh;

						if ((m & detail_neural_sr::kMaskEdge) != 0u)
						{
							const double yb = ws.yBelow[idx];
							lineEdgeYH2 += yh2;
							lineEdgeYBelow2 += yb * yb;
							++lineEdgeCount;
						}
						if ((m & detail_neural_sr::kMaskCrest) != 0u)
						{
							const double gL = ws.gradLow[idx];
							const double gH = ws.gradHigh[idx];
							lineCrestCross += gH * gL;
							lineCrestGH2 += gH * gH;
							lineCrestGL2 += gL * gL;
						}
						if ((m & detail_neural_sr::kMaskInterior) != 0u)
						{
							const double yb = ws.yBelow[idx];
							lineIntYH2 += yh2;
							lineIntYBelow2 += yb * yb;
							++lineInteriorCount;
						}
						if ((m & (detail_neural_sr::kMaskEdgeZone | detail_neural_sr::kMaskInterior)) != 0u)
						{
							totalActiveYH2 += yh2;
							totalActiveYH4 += yh2 * yh2;
							++totalActiveSamples;
						}
					}

					totalCrestCross += lineCrestCross;
					totalCrestGH2 += lineCrestGH2;
					totalCrestGL2 += lineCrestGL2;
					totalEdgeYH2 += lineEdgeYH2;
					totalEdgeYBelow2 += lineEdgeYBelow2;
					totalIntYH2 += lineIntYH2;
					totalIntYBelow2 += lineIntYBelow2;
					totalEdgeSamples += lineEdgeCount;
					totalInteriorSamples += lineInteriorCount;

					if (lineEdgeCount > 0)
					{
						const double wEdge = static_cast<double>(lineEdgeCount);
						for (size_t k = 0; k < nHalfPlus1; ++k)
							ws.psdEdge[k] += wEdge * ws.linePsd[k];
					}
					if (lineInteriorCount > 0)
					{
						const double wInt = static_cast<double>(lineInteriorCount);
						for (size_t k = 0; k < nHalfPlus1; ++k)
							ws.psdInterior[k] += wInt * ws.linePsd[k];
					}

					if (lineEdgeCount > 0 && lineInteriorCount > 0)
					{
						const double lBi = ClampD(
							lineCrestCross / std::sqrt(lineCrestGH2 * lineCrestGL2 + detail_neural_sr::kNumericEps),
							-1.0,
							1.0);
						const double lEdgeP = (lineEdgeYH2 / static_cast<double>(lineEdgeCount)) / bandFracHigh;
						const double lIntP = (lineIntYH2 / static_cast<double>(lineInteriorCount)) / bandFracHigh;
						const double lRatio = lEdgeP / (std::max)(lIntP, floorP);
						ws.lineBi.push_back(lBi);
						ws.lineRatio.push_back(lRatio);
						++validLines;
					}
				}

				if (totalEdgeSamples > 0)
				{
					const double invEdge = 1.0 / static_cast<double>(totalEdgeSamples);
					for (size_t k = 0; k < nHalfPlus1; ++k)
						ws.psdEdge[k] *= invEdge;
				}
				if (totalInteriorSamples > 0)
				{
					const double invInt = 1.0 / static_cast<double>(totalInteriorSamples);
					for (size_t k = 0; k < nHalfPlus1; ++k)
						ws.psdInterior[k] *= invInt;
				}

				res.validLines = validLines;
				res.edgeSamples = totalEdgeSamples;
				res.interiorSamples = totalInteriorSamples;

				// Populate diagnostics whenever samples exist so callers always see full telemetry (Rule 4).
				if (totalEdgeSamples > 0)
				{
					res.edgePhaseBicoherence = ClampD(
						totalCrestCross / std::sqrt(totalCrestGH2 * totalCrestGL2 + detail_neural_sr::kNumericEps),
						-1.0,
						1.0);
				}

				if (totalActiveSamples > 0)
				{
					const double meanYH2 = totalActiveYH2 / static_cast<double>(totalActiveSamples);
					const double meanYH4 = totalActiveYH4 / static_cast<double>(totalActiveSamples);
					res.hfKurtosis = meanYH4 / (meanYH2 * meanYH2 + detail_neural_sr::kNumericEps);
				}

				const double edgeHfEnergy = totalEdgeSamples > 0
					? (totalEdgeYH2 / static_cast<double>(totalEdgeSamples))
					: 0.0;
				const double edgeBelowEnergy = totalEdgeSamples > 0
					? (totalEdgeYBelow2 / static_cast<double>(totalEdgeSamples))
					: 0.0;
				const double interiorHfEnergy = totalInteriorSamples > 0
					? (totalIntYH2 / static_cast<double>(totalInteriorSamples))
					: 0.0;
				const double interiorBelowEnergy = totalInteriorSamples > 0
					? (totalIntYBelow2 / static_cast<double>(totalInteriorSamples))
					: 0.0;

				const double pHighEdge = edgeHfEnergy / bandFracHigh;
				const double pBelowEdge = edgeBelowEnergy / bandFracBelow;
				const double pHighInterior = interiorHfEnergy / bandFracHigh;
				const double pBelowInterior = interiorBelowEnergy / bandFracBelow;

				double edgeDropExcessDecades = 0.0;
				if (totalEdgeSamples > 0 && totalInteriorSamples > 0)
				{
					res.edgeToInteriorHfRatio = pHighEdge / (std::max)(pHighInterior, floorP);

					// Pillar 3: Interior PSD step discontinuity across f_c relative to natural -2*log10(fAbove/fBelow),
					// paired with edge spectral continuity across f_c so classical bandlimited LTI edges (whose own
					// spectrum collapses across f_c) never fire even if Pillar 1 LTI veto is bypassed.
					const double fc = 0.5 * ratio;
					const double fBelow = 0.5 * (0.55 + 0.90) * fc;
					const double fAbove = 0.5 * (1.05 * fc + 0.95 * 0.5);
					const double expectedDrop = -2.0 * std::log10(fAbove / fBelow);
					const double actualInteriorDecades = std::log10(
						(std::max)(pHighInterior, floorP) / (std::max)(pBelowInterior, floorP));
					res.interiorSpectralDropExcessDecades = expectedDrop - actualInteriorDecades;
					const double actualEdgeDecades = std::log10(
						(std::max)(pHighEdge, floorP) / (std::max)(pBelowEdge, floorP));
					edgeDropExcessDecades = expectedDrop - actualEdgeDecades;
				}

				// Decision Rule (strict sample/consensus gates + 3-pillar joint detection):
				if (totalEdgeSamples < cfg.minEdgeSamples)
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::InsufficientEdgeActivity;
					res.neuralUpscaled = false;
					return res;
				}

				if (totalInteriorSamples < cfg.minInteriorSamples)
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::InsufficientInteriorTexture;
					res.neuralUpscaled = false;
					return res;
				}

				if (validLines < cfg.minValidLines)
				{
					res.verdict = NeuralSrVerdict::Abstain;
					res.reason = NeuralSrReason::AmbiguousSignal;
					res.neuralUpscaled = false;
					return res;
				}

				// Require genuine above-cutoff HF power on strong edges above the 1.5x amplitude (2.25x power) floor gate.
				const double gateP = (cfg.lineGateFactor * cfg.lineGateFactor) * floorP;
				if (!(pHighEdge > gateP))
				{
					res.verdict = NeuralSrVerdict::NotNeural;
					res.reason = NeuralSrReason::AmbiguousSignal;
					res.neuralUpscaled = false;
					return res;
				}

				const bool hasEdgePhaseLock = res.edgePhaseBicoherence >= cfg.minBicoherence;
				const bool hasEdgeHfLocalization = res.edgeToInteriorHfRatio >= cfg.minEdgeToInteriorHfRatio;
				const bool hasInteriorPsdDrop = res.interiorSpectralDropExcessDecades >= cfg.interiorDropDecadesMargin;
				// Genuine neural/shock-sharpened edges synthesize broadband energy across f_c rather than dropping off
				// like a bandlimited LTI upscaler (edgeDropExcessDecades < interiorDropDecadesMargin).
				const bool hasEdgeBroadband = edgeDropExcessDecades < cfg.interiorDropDecadesMargin;

				// Require genuine interior PSD collapse above f_c AND broadband edge energy across f_c.
				if (hasEdgePhaseLock && hasEdgeHfLocalization && hasInteriorPsdDrop && hasEdgeBroadband)
				{
					res.verdict = NeuralSrVerdict::NeuralSrDetected;
					res.reason = NeuralSrReason::NeuralEdgeLockAndInteriorDrop;
					res.neuralUpscaled = true;
					return res;
				}

				res.verdict = NeuralSrVerdict::NotNeural;
				res.reason = NeuralSrReason::NaturalBroadbandSpectrum;
				res.neuralUpscaled = false;
				return res;
			}
		};

		// Convenience free-function entry point forwarding to NeuralSrWitness::Evaluate.
		inline NeuralSrResult Evaluate(
			const LineSet& ls,
			int candidateN,
			double bestLtiStat,
			double floorCodes,
			const NeuralSrSettings& cfg,
			NeuralSrWorkspace& ws)
		{
			return NeuralSrWitness::Evaluate(ls, candidateN, bestLtiStat, floorCodes, cfg, ws);
		}
	}
}

#pragma fp_contract(on)
