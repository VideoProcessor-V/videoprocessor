/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for SID-SOTA-1:
// Heteroscedastic HDR10 PQ / HLG quantization noise-floor modeling & Gauss-Markov linearized WLS descale
// (Rule 3 mandatory same-input negative controls, zero oracle-only tests, zero sleeps, pure CPU C++14 /W4).
//
// Covers:
//   1. Sota1PqAndHlgEotfOetfRoundTripDerivativesBitDepthAndDetectorFloor
//   2. Sota1HighContrastPqAndHlg10BitAnd12BitUpscaledEdgesBeatNaiveAndNativeAbstains
//   3. Sota1DarkToeClampHighlightCeilingNonFiniteWorkspaceReuseOriginalAndW1BitIdentity
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"
#include "W1DescaleRef.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/input/source_analysis/identity/chain/HdrNoiseFloor.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PowerProbe.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace Sota1HdrFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;
		namespace SR = SourceResolution;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kBicubic = 1;
		constexpr int kLanczos3 = 5;

		struct SplitMix64
		{
			uint64_t state;
			explicit SplitMix64(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double Uniform01()
			{
				return static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
			}

			double Normal01()
			{
				double u1 = Uniform01();
				if (u1 < 1e-12)
					u1 = 1e-12;
				const double u2 = Uniform01();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * C::kPi * u2);
			}
		};

		inline double ApplyOetf(C::HdrTransfer tr, double yLin)
		{
			return (tr == C::HdrTransfer::Pq) ? C::PqOetf(yLin) : C::HlgOetf(yLin);
		}

		// Quantizes normalized nonlinear signal E' in [0, 1] at exact 10-bit or 12-bit BT.2100 precision,
		// returning LineSet units (10-bit code scale: integer codes for 10-bit, 0.25-code steps for 12-bit).
		inline double QuantizeHdrCode10(double ep, int bitDepth, bool limitedRange)
		{
			const double e = C::ClampD(ep, 0.0, 1.0);
			if (limitedRange)
			{
				const int scaleShift = bitDepth - 8;
				const double black = static_cast<double>(16 * (1 << scaleShift));
				const double span = static_cast<double>(219 * (1 << scaleShift));
				const double qNative = std::floor(black + e * span + 0.5);
				const double toCode10 = std::pow(2.0, 10.0 - static_cast<double>(bitDepth));
				return qNative * toCode10;
			}
			const double maxNative = static_cast<double>((1u << static_cast<unsigned>(bitDepth)) - 1u);
			const double qNative = std::floor(e * maxNative + 0.5);
			return (qNative / maxNative) * 1023.0;
		}

		// Synthesizes a realistic high-dynamic-range linear-light 1-D scanline (dark shadow texture + high-contrast
		// specular highlight plateaus, kept strictly inside (0, 1) so linear-light Lanczos3/Bicubic ringing never clips),
		// upscales in linear light from nIn -> nOut (or keeps native 1-px detail when nIn == nOut), applies exact
		// SMPTE ST 2084 PQ or ARIB STD-B67 HLG OETF, and quantizes to 10-bit or 12-bit codes.
		inline void SynthHdrLinearUpscaledLine(
			int nIn,
			int nOut,
			int kernel,
			C::HdrTransfer tr,
			int bitDepth,
			bool limitedRange,
			SplitMix64& rng,
			std::vector<double>& outCode10)
		{
			std::vector<double> srcLin(static_cast<size_t>(nIn), 0.0);
			const double yShadowBase = (tr == C::HdrTransfer::Pq) ? 0.0028 : 0.015;  // ~28 nits PQ / dark HLG shadow (> code 64)
			const double yShadowTex = (tr == C::HdrTransfer::Pq) ? 0.00055 : 0.0035; // fine shadow detail (unclipped)
			const double yMidLit = (tr == C::HdrTransfer::Pq) ? 0.095 : 0.11;        // lit pedestal (> max Lanczos3 negative sidelobe)
			const double ySpecHi = 0.72;                                             // ~7200 nits PQ / bright HLG specular

			const int period = (std::max)(36, nIn / 12);
			const int ramp = 6;
			const int hiWidth = (std::max)(14, period / 3);
			const int phaseJit = static_cast<int>(rng.NextU64() % static_cast<uint64_t>((std::max)(1, period / 2)));

			for (int i = 0; i < nIn; ++i)
			{
				const int pos = (i + phaseJit) % period;
				if (pos < ramp)
				{
					// Smooth log-light optical transition from dark shadow into lit zone so Lanczos3 never clips <= 0.
					const double u = (static_cast<double>(pos) + 0.5) / static_cast<double>(ramp);
					const double w = 0.5 - 0.5 * std::cos(C::kPi * u);
					srcLin[static_cast<size_t>(i)] = std::exp(
						std::log(yShadowBase) + (std::log(yMidLit) - std::log(yShadowBase)) * w);
				}
				else if (pos < ramp + hiWidth)
				{
					// Sharp 1-px high-contrast edges between lit pedestal and specular highlight (unclipped ringing + strong OETF curvature).
					const int sub = (pos - ramp) % 9;
					const double target = (sub < 4)
						? (ySpecHi + 0.015 * rng.Normal01())
						: (yMidLit + 0.008 * rng.Normal01());
					srcLin[static_cast<size_t>(i)] = C::ClampD(target, 0.085, 0.85);
				}
				else if (pos < 2 * ramp + hiWidth)
				{
					const double u = (static_cast<double>(pos - (ramp + hiWidth)) + 0.5) / static_cast<double>(ramp);
					const double w = 0.5 - 0.5 * std::cos(C::kPi * u);
					srcLin[static_cast<size_t>(i)] = std::exp(
						std::log(yMidLit) + (std::log(yShadowBase) - std::log(yMidLit)) * w);
				}
				else
				{
					// Rich bandlimited dark-shadow texture strictly above limited-range code 64.
					const double minShadow = (tr == C::HdrTransfer::Pq) ? 0.0016 : 0.008;
					const double fine = yShadowTex * rng.Normal01();
					srcLin[static_cast<size_t>(i)] = C::ClampD(yShadowBase + fine, minShadow, 0.04);
				}
			}

			std::vector<double> upLin(static_cast<size_t>(nOut), 0.0);
			if (nIn == nOut)
			{
				upLin = srcLin;
			}
			else
			{
				const int r = C::Kernel(kernel).radius;
				const int taps = 2 * r;
				std::vector<double> wt(static_cast<size_t>(taps));
				for (int i = 0; i < nOut; ++i)
				{
					const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(nIn) / static_cast<double>(nOut) - 0.5;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, wt.data());
					double acc = 0.0;
					for (int s = 0; s < taps; ++s)
					{
						int c = static_cast<int>(fl) - r + 1 + s;
						if (c < 0)
							c = 0;
						if (c > nIn - 1)
							c = nIn - 1;
						acc += wt[static_cast<size_t>(s)] * srcLin[static_cast<size_t>(c)];
					}
					upLin[static_cast<size_t>(i)] = C::ClampD(acc, 0.0, 1.0);
				}
			}

			outCode10.resize(static_cast<size_t>(nOut));
			for (int i = 0; i < nOut; ++i)
			{
				const double ep = ApplyOetf(tr, upLin[static_cast<size_t>(i)]);
				outCode10[static_cast<size_t>(i)] = QuantizeHdrCode10(ep, bitDepth, limitedRange);
			}
		}

		inline C::LineSet BuildHdrLineSet(
			int axis,
			int axisLen,
			int srcLen,
			int lineCount,
			int margin,
			int kernel,
			C::HdrTransfer tr,
			int bitDepth,
			bool limitedRange,
			uint64_t seed)
		{
			C::LineSet ls;
			ls.axis = axis;
			ls.axisLen = axisLen;
			ls.origin = margin;
			const int length = axisLen - 2 * margin;
			ls.Resize(lineCount, length);
			SplitMix64 rng(seed);
			std::vector<double> fullLine;
			for (int j = 0; j < lineCount; ++j)
			{
				SynthHdrLinearUpscaledLine(srcLen, axisLen, kernel, tr, bitDepth, limitedRange, rng, fullLine);
				double* dst = ls.Line(j);
				for (int i = 0; i < length; ++i)
					dst[i] = fullLine[static_cast<size_t>(margin + i)];
				ls.cross[static_cast<size_t>(j)] = margin + j * 32;
			}
			return ls;
		}

		inline C::LineSnapshot BuildHdrSnapshot10Bit(
			int rasterW,
			int rasterH,
			int srcW,
			int srcH,
			int kernel,
			C::HdrTransfer tr,
			uint64_t seq,
			int64_t timeUs,
			uint64_t seed,
			bool cut)
		{
			C::LineRect active;
			active.x = 0;
			active.y = 0;
			active.w = rasterW;
			active.h = rasterH;
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"fixture: valid LinePlan");

			C::LineSnapshot snap;
			snap.seq = seq;
			snap.timeUs = timeUs;
			snap.rasterW = rasterW;
			snap.rasterH = rasterH;
			snap.active = plan.active;
			snap.inner = plan.inner;
			snap.rowCount = plan.rowCount;
			snap.colCount = plan.colCount;
			snap.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
			snap.colX.assign(plan.colX, plan.colX + plan.colCount);
			snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 64);
			snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 64);
			if (cut)
			{
				snap.flags = C::kEvCut;
				snap.cuts = 1;
				snap.eventTimeUs = timeUs;
			}

			SplitMix64 rng(seed ^ (seq * 0x9e3779b97f4a7c15ull));
			std::vector<double> fullLine;
			for (int j = 0; j < plan.rowCount; ++j)
			{
				SynthHdrLinearUpscaledLine(srcW, rasterW, kernel, tr, 10, true, rng, fullLine);
				uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = static_cast<uint16_t>(std::floor(fullLine[static_cast<size_t>(plan.inner.x + i)] + 0.5));
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				SynthHdrLinearUpscaledLine(srcH, rasterH, kernel, tr, 10, true, rng, fullLine);
				uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = static_cast<uint16_t>(std::floor(fullLine[static_cast<size_t>(plan.inner.y + i)] + 0.5));
			}
			return snap;
		}
	}

	TEST_CLASS(SourceIdSota1HdrTests)
	{
	public:
		// 1. Exact SMPTE ST 2084 (PQ) & ARIB STD-B67 (HLG) EOTF/OETF round-trip, analytical derivative accuracy
		//    against central finite differences, 10-bit vs 12-bit quantization step & sigma scaling, and
		//    SourceResolution::ReadFrame perceptual HDR quantization floor behavior vs SDR negative control.
		TEST_METHOD(Sota1PqAndHlgEotfOetfRoundTripDerivativesBitDepthAndDetectorFloor)
		{
			using namespace Sota1HdrFx;

			// Default-OFF settings check:
			const C::HdrNoiseSettings defCfg;
			Assert::IsFalse(defCfg.enabled, L"HdrNoiseSettings must default to OFF");
			Assert::IsTrue(defCfg.transfer == C::HdrTransfer::Sdr);
			Assert::AreEqual(10, defCfg.bitDepth);
			Assert::IsTrue(defCfg.limitedRange);
			Assert::IsTrue(defCfg.linearizeForDescale);

			// PQ EOTF <-> OETF round-trip and monotonicity across 0.01 nit .. 10000 nits:
			Assert::AreEqual(0.0, C::PqEotf(0.0), 0.0);
			Assert::AreEqual(1.0, C::PqEotf(1.0), 0.0);
			Assert::AreEqual(0.0, C::PqOetf(0.0), 0.0);
			Assert::AreEqual(1.0, C::PqOetf(1.0), 0.0);
			const double pqTestY[8] = { 1e-6, 1e-5, 1e-4, 1e-3, 0.01, 0.1, 0.45, 0.95 };
			double prevEp = 0.0;
			for (int k = 0; k < 8; ++k)
			{
				const double ep = C::PqOetf(pqTestY[k]);
				Assert::IsTrue(ep > prevEp && ep < 1.0);
				prevEp = ep;
				const double yBack = C::PqEotf(ep);
				Assert::AreEqual(pqTestY[k], yBack, pqTestY[k] * 1e-11, L"PQ EOTF(OETF(Y)) round-trip must be exact");
			}

			// HLG InvOETF <-> OETF round-trip + exact C0 (1/12) and C1 (1/3) continuity at E' = 0.5:
			Assert::AreEqual(0.0, C::HlgInvOetf(0.0), 0.0);
			Assert::AreEqual(1.0, C::HlgInvOetf(1.0), 1e-15);
			Assert::AreEqual(1.0 / 12.0, C::HlgInvOetf(0.5), 1e-15);
			Assert::AreEqual(1.0 / 12.0, C::HlgInvOetf(0.5 + 1e-13), 1e-12, L"HLG C0 continuity across E'=0.5");
			Assert::AreEqual(1.0 / 3.0, C::HlgInvOetfDerivative(0.5), 1e-15);
			Assert::AreEqual(1.0 / 3.0, C::HlgInvOetfDerivative(0.5 + 1e-13), 1e-11, L"HLG C1 derivative continuity across E'=0.5");
			const double hlgTestE[7] = { 1e-5, 0.01, 1.0 / 12.0, 0.15, 0.35, 0.70, 0.98 };
			for (int k = 0; k < 7; ++k)
			{
				const double ep = C::HlgOetf(hlgTestE[k]);
				const double eBack = C::HlgInvOetf(ep);
				Assert::AreEqual(hlgTestE[k], eBack, 1e-12, L"HLG InvOETF(OETF(E)) round-trip must be exact");
			}

			// Analytical derivatives vs central finite differences across [0.02, 0.98]:
			const double epsFd = 1e-7;
			const double probeEp[7] = { 0.02, 0.10, 0.25, 0.499, 0.501, 0.75, 0.95 };
			for (int k = 0; k < 7; ++k)
			{
				const double ep = probeEp[k];
				const double fdPq = (C::PqEotf(ep + epsFd) - C::PqEotf(ep - epsFd)) / (2.0 * epsFd);
				const double anPq = C::PqEotfDerivative(ep);
				Assert::AreEqual(fdPq, anPq, std::fabs(fdPq) * 1e-6 + 1e-14, L"PQ analytical derivative matches finite difference");

				const double fdHlg = (C::HlgInvOetf(ep + epsFd) - C::HlgInvOetf(ep - epsFd)) / (2.0 * epsFd);
				const double anHlg = C::HlgInvOetfDerivative(ep);
				Assert::AreEqual(fdHlg, anHlg, std::fabs(fdHlg) * 1e-6 + 1e-14, L"HLG analytical derivative matches finite difference");
			}

			// 10-bit vs 12-bit exact quantization step & local linear sigma scaling:
			Assert::AreEqual(1.0 / 876.0, C::QuantStepNorm(10, true), 1e-15);
			Assert::AreEqual(1.0 / 3504.0, C::QuantStepNorm(12, true), 1e-15);
			Assert::AreEqual(1.0 / 1023.0, C::QuantStepNorm(10, false), 1e-15);
			Assert::AreEqual(1.0 / 4095.0, C::QuantStepNorm(12, false), 1e-15);

			const double sigPq10Mid = C::LocalQuantSigmaLinear(C::HdrTransfer::Pq, 502.0, 10, true);
			const double sigPq12Mid = C::LocalQuantSigmaLinear(C::HdrTransfer::Pq, 502.0, 12, true);
			Assert::AreEqual(4.0, sigPq10Mid / sigPq12Mid, 1e-12,
				L"12-bit limited-range PQ local linear sigma at E'=0.5 must be 4x finer than 10-bit");

			// PerceptualCodeStepRatio: < 1 in dark toe, == 1 at E'=0.5 (code 502), > 1 in highlights, == 1.0 on SDR negative control:
			Assert::AreEqual(1.0, C::PerceptualCodeStepRatio(C::HdrTransfer::Sdr, 120.0, true), 0.0);
			Assert::AreEqual(1.0, C::PerceptualCodeStepRatio(C::HdrTransfer::Pq, 502.0, true), 1e-12);
			Assert::AreEqual(1.0, C::PerceptualCodeStepRatio(C::HdrTransfer::Hlg, 502.0, true), 1e-12);
			Assert::IsTrue(C::PerceptualCodeStepRatio(C::HdrTransfer::Pq, 160.0, true) < 0.05);
			Assert::IsTrue(C::PerceptualCodeStepRatio(C::HdrTransfer::Pq, 880.0, true) > 10.0);
			Assert::IsTrue(C::PerceptualCodeStepRatio(C::HdrTransfer::Hlg, 160.0, true) < 0.5);
			Assert::IsTrue(C::PerceptualCodeStepRatio(C::HdrTransfer::Hlg, 880.0, true) > 4.0);

			// SourceResolution::ReadFrame perceptual HDR floor vs default-OFF negative control on identical tiles:
			std::vector<float> darkTilePx(256u * 256u, 0.0f);
			std::vector<float> brightTilePx(256u * 256u, 0.0f);
			SplitMix64 rng(0x5101ull);
			for (size_t i = 0; i < darkTilePx.size(); ++i)
			{
				darkTilePx[i] = static_cast<float>(C::ClampD(0.16 + 0.03 * rng.Normal01(), 0.07, 0.90));
				brightTilePx[i] = static_cast<float>(C::ClampD(0.84 + 0.03 * rng.Normal01(), 0.07, 0.92));
			}
			SR::Spectrum spDark, spBright;
			Assert::IsTrue(SR::Analyse(darkTilePx.data(), 256, 256, spDark, 1023));
			Assert::IsTrue(SR::Analyse(brightTilePx.data(), 256, 256, spBright, 1023));
			const std::vector<SR::Spectrum> darkVec(4, spDark);
			const std::vector<SR::Spectrum> brightVec(4, spBright);

			SR::Thresholds thrOff; // negative control: hdrPerceptualNoiseFloor == false
			Assert::IsFalse(thrOff.hdrPerceptualNoiseFloor);
			const SR::FrameReading frDarkOff = SR::ReadFrame(darkVec, thrOff);
			const SR::FrameReading frBrightOff = SR::ReadFrame(brightVec, thrOff);
			Assert::AreEqual(frDarkOff.quantisationFloor, frBrightOff.quantisationFloor, 1e-12,
				L"negative control: with hdrPerceptualNoiseFloor=false, uniform floor ignores dark vs highlight APL");

			SR::Thresholds thrPq = thrOff;
			thrPq.hdrPerceptualNoiseFloor = true;
			thrPq.hdrTransfer = 1; // PQ
			thrPq.hdrBitDepth = 10;
			thrPq.hdrLimitedRange = true;
			const SR::FrameReading frDarkPq = SR::ReadFrame(darkVec, thrPq);
			const SR::FrameReading frBrightPq = SR::ReadFrame(brightVec, thrPq);
			Assert::IsTrue(frDarkPq.quantisationFloor < frDarkOff.quantisationFloor - 1.5,
				L"HDR PQ dark-shadow tile must lower perceptual quantization floor well below uniform midtone floor");
			Assert::IsTrue(frBrightPq.quantisationFloor > frBrightOff.quantisationFloor + 1.5,
				L"HDR PQ highlight tile must raise perceptual quantization floor well above uniform midtone floor");
		}

		// 2. High-contrast 10-bit & 12-bit HDR10 PQ and HLG upscaled edges spanning dark toe to bright highlights:
		//    prove hdrNoise.enabled=true (Gauss-Markov linearized WLS descale) beats BOTH naive uniform-code descale
		//    (enabled=false negative control) AND unweighted linear-light MSE on identical inputs -- both in
		//    DescaleVerifier AND end-to-end through ChainEstimator -- while native HDR10 PQ/HLG fine detail does NOT
		//    false-trigger an upscale!
		TEST_METHOD(Sota1HighContrastPqAndHlg10BitAnd12BitUpscaledEdgesBeatNaiveAndNativeAbstains)
		{
			using namespace Sota1HdrFx;

			const C::HdrTransfer transfers[2] = { C::HdrTransfer::Pq, C::HdrTransfer::Hlg };
			const int bitDepths[2] = { 10, 12 };

			for (int ti = 0; ti < 2; ++ti)
			{
				const C::HdrTransfer tr = transfers[ti];
				for (int bi = 0; bi < 2; ++bi)
				{
					const int bd = bitDepths[bi];
					const uint64_t seedBase = 0x7100ull + static_cast<uint64_t>(ti * 37 + bi * 13);

					// Synthesize 2x upscaled (1920 -> 3840) high-contrast HDR scanlines in linear light, encoded with PQ/HLG:
					const C::LineSet upLs = BuildHdrLineSet(0, kUhdW, 1920, 16, 48, kLanczos3, tr, bd, true, seedBase);

					// Synthesize native 3840x2160 high-contrast HDR scanlines with genuine 1-px native fine detail:
					const C::LineSet natLs = BuildHdrLineSet(0, kUhdW, kUhdW, 16, 48, kLanczos3, tr, bd, true, seedBase + 999ull);

					C::HdrNoiseSettings hdrCfg;
					hdrCfg.enabled = true;
					hdrCfg.transfer = tr;
					hdrCfg.bitDepth = bd;
					hdrCfg.limitedRange = true;
					hdrCfg.linearizeForDescale = true;

					// (A) Direct VerifyLinearizedHeteroscedastic on upscaled vs native + unweighted linear negative control:
					C::HdrNoiseWorkspace ws;
					const C::HdrVerifyResult hrUp = C::VerifyLinearizedHeteroscedastic(upLs, 1920, hdrCfg, ws);
					Assert::IsTrue(hrUp.valid, L"VerifyLinearizedHeteroscedastic must succeed on upscaled HDR lines");
					Assert::AreEqual(kLanczos3, hrUp.bestKernel, L"Gauss-Markov WLS must identify true Lanczos3 kernel");
					Assert::IsTrue(hrUp.stat < -0.50, L"Gauss-Markov WLS must expose a deep negative upscale dip on HDR edges");

					// Same-input Negative Control A1: unweighted linear-light MSE (uniformLinearStat) is swamped by
					// highlight quantization variance and degraded relative to Gauss-Markov WLS!
					Assert::IsTrue(hrUp.stat < hrUp.uniformLinearStat - 0.15,
						L"negative control: Gauss-Markov WLS must beat unweighted linear MSE on heteroscedastic HDR lines");

					// Same-input Negative Control A2: linearizeForDescale = false (code-space WLS without EOTF) fails to undo OETF distortion!
					C::HdrNoiseSettings noLinCfg = hdrCfg;
					noLinCfg.linearizeForDescale = false;
					const C::HdrVerifyResult hrNoLin = C::VerifyLinearizedHeteroscedastic(upLs, 1920, noLinCfg, ws);
					Assert::IsTrue(hrNoLin.valid);
					Assert::IsTrue(hrUp.stat < hrNoLin.stat - 0.25,
						L"negative control: EOTF linearization must markedly improve dip over code-space residual");

					// Native HDR10 PQ / HLG false-positive trap: MUST NOT show an upscale dip!
					const C::HdrVerifyResult hrNat = C::VerifyLinearizedHeteroscedastic(natLs, 1920, hdrCfg, ws);
					Assert::IsTrue(hrNat.valid);
					Assert::IsTrue(hrNat.stat > -0.05,
						L"native HDR10 PQ/HLG fine detail must NOT false-trigger a linearized descale dip");

					// (B) DescaleVerifier comparison: hdrNoise.enabled = true vs enabled = false (naive code-space negative control)
					//     including CoarseScan + ExactStage on identical input lines!
					C::VerifierSettings vsNaive;
					Assert::IsFalse(vsNaive.hdrNoise.enabled);
					C::DescaleVerifier verNaive(vsNaive);

					C::VerifierSettings vsHdr;
					vsHdr.hdrNoise = hdrCfg;
					C::DescaleVerifier verHdr(vsHdr);

					const int cands[2] = { 1920, 2560 };
					C::VerifyOptions vOpt;
					C::VerifyResult vrNaive, vrHdr, vrHdrNat;
					Assert::IsTrue(verNaive.Evaluate(upLs, cands, 2, vOpt, vrNaive));
					Assert::IsTrue(verHdr.Evaluate(upLs, cands, 2, vOpt, vrHdr));
					Assert::IsTrue(verHdr.Evaluate(natLs, cands, 2, vOpt, vrHdrNat));

					Assert::IsTrue(vrNaive.candidates[0].valid && vrNaive.candidates[0].exactDone);
					Assert::IsTrue(vrHdr.candidates[0].valid && vrHdr.candidates[0].exactDone);
					Assert::IsFalse(vrNaive.candidates[0].hdrLinearizedUsed);
					Assert::IsTrue(vrHdr.candidates[0].hdrLinearizedUsed);
					Assert::AreEqual(1920, vrHdr.candidates[0].n,
						L"DescaleVerifier with hdrNoise enabled must refine to exact n=1920 without code-space integer drift");
					Assert::IsTrue(vrHdr.candidates[0].stat < vrNaive.candidates[0].stat - 0.25,
						L"DescaleVerifier hdrNoise=true must achieve markedly deeper dip than naive code-space verifier on identical HDR lines");
					Assert::IsTrue(vrHdrNat.candidates[0].valid && vrHdrNat.candidates[0].stat > -0.05,
						L"DescaleVerifier hdrNoise=true must NOT false-dip on native HDR fine detail");
				}
			}

			// (C) End-to-end ChainEstimator proof on high-contrast 10-bit HDR10 PQ 1080p->2160p multi-shot stream
			//     vs naive default-OFF negative control AND native HDR10 PQ stream negative control:
			std::vector<C::LineSnapshot> pqUpStream;
			std::vector<C::LineSnapshot> pqNatStream;
			for (int s = 0; s < 9; ++s)
			{
				const bool isCut = (s % 3) == 0; // 3 shots x 3 evaluations (1.6 s cadence -> 3.2 s dwell per shot >= 3.0 s)
				pqUpStream.push_back(BuildHdrSnapshot10Bit(kUhdW, kUhdH, 1920, 1080, kLanczos3, C::HdrTransfer::Pq,
					static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1600000, 0x8200ull + static_cast<uint64_t>(s * 29), isCut));
				pqNatStream.push_back(BuildHdrSnapshot10Bit(kUhdW, kUhdH, kUhdW, kUhdH, kLanczos3, C::HdrTransfer::Pq,
					static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1600000, 0x9200ull + static_cast<uint64_t>(s * 29), isCut));
			}

			auto runChain = [&](const std::vector<C::LineSnapshot>& stream, bool enableHdr,
				G::Record& outRec, C::EvaluationDetail& outDet, C::EstimatorCounters& outCnt) {
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				if (enableHdr)
				{
					cfg.hdrNoise.enabled = true;
					cfg.hdrNoise.transfer = C::HdrTransfer::Pq;
					cfg.hdrNoise.bitDepth = 10;
					cfg.hdrNoise.limitedRange = true;
					cfg.hdrNoise.linearizeForDescale = true;
				}
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { outDet = d; });
				est.SetMode(C::EstimatorMode::On);
				for (size_t i = 0; i < stream.size(); ++i)
				{
					clk.Set(static_cast<double>(i) * 1600.0);
					Assert::IsTrue(est.SubmitLines(stream[i]));
				}
				Assert::IsTrue(est.Held().ReadLatest(0, outRec));
				outCnt = est.GetCounters();
				est.Stop();
			};

			G::Record recNaive, recHdr, recHdrNat;
			C::EvaluationDetail detNaive, detHdr, detHdrNat;
			C::EstimatorCounters cntNaive, cntHdr, cntHdrNat;
			runChain(pqUpStream, false, recNaive, detNaive, cntNaive);
			runChain(pqUpStream, true, recHdr, detHdr, cntHdr);
			runChain(pqNatStream, true, recHdrNat, detHdrNat, cntHdrNat);

			Assert::AreEqual(uint64_t(0), cntNaive.hdrLinearizedEvals, L"negative control: hdrLinearizedEvals == 0 when disabled");
			Assert::IsTrue(cntHdr.hdrLinearizedEvals >= 9u, L"hdrLinearizedEvals must increment on HDR evaluations");

			// Find 1920 candidate evidence stat on axis 0 in both runs: ChainEvidence::Judge MUST receive the
			// linearized Gauss-Markov per-line statistics (not stale un-linearized code-space lineStat)!
			double evNaive1920 = 0.0, evHdr1920 = 0.0;
			bool foundNaive = false, foundHdr = false;
			for (size_t ci = 0; ci < detNaive.axis[0].cands.size(); ++ci)
			{
				if (std::abs(detNaive.axis[0].cands[ci].n - 1920) <= 2 && detNaive.axis[0].cands[ci].evJudged)
				{
					evNaive1920 = detNaive.axis[0].cands[ci].evStat;
					foundNaive = true;
				}
			}
			for (size_t ci = 0; ci < detHdr.axis[0].cands.size(); ++ci)
			{
				if (detHdr.axis[0].cands[ci].n == 1920 && detHdr.axis[0].cands[ci].evJudged)
				{
					evHdr1920 = detHdr.axis[0].cands[ci].evStat;
					foundHdr = true;
				}
			}
			Assert::IsTrue(foundHdr, L"ChainEstimator hdrNoise=true must judge exact n=1920 in ChainEvidence");
			Assert::IsTrue(!foundNaive || evHdr1920 < evNaive1920 - 0.25,
				L"ChainEvidence::Judge evStat on HDR PQ stream must reflect Gauss-Markov linearized WLS improvement over naive code-space descale");
			Assert::IsTrue(recHdr.measured[0].llr > recNaive.measured[0].llr ||
				(recHdr.applied[0].state == G::LatchState::Latched && recHdr.applied[0].n == 1920),
				L"ChainEstimator hdrNoise=true must improve upscale LLR / confident latch over naive code-space path");

			// Native HDR10 PQ stream MUST NOT false-latch an upscale:
			const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };
			const SourceGeometry::ConfidentGeometry cgNat = SourceGeometry::ConfidentOf(recHdrNat, deliv);
			Assert::IsFalse(cgNat.axis[0].confident && cgNat.axis[0].n < kUhdW,
				L"native HDR10 PQ fine detail must NEVER false-latch as upscaled on H");
			Assert::IsFalse(cgNat.axis[1].confident && cgNat.axis[1].n < kUhdH,
				L"native HDR10 PQ fine detail must NEVER false-latch as upscaled on V");

			// SOTA1-HIGH-1: probeVerifier on pqNatStream must evaluate PowerProbe's code-space 0.61x synthetic
			// injection in code space (never linearizing it), so probeSeen == true and kReasonNoPower is unset:
			Assert::IsTrue(detHdrNat.axis[0].probeRan && detHdrNat.axis[0].probeSeen,
				L"SOTA1-HIGH-1: PowerProbe must see its code-space synthetic injection on high-contrast native HDR10 PQ (H)");
			Assert::IsTrue(detHdrNat.axis[1].probeRan && detHdrNat.axis[1].probeSeen,
				L"SOTA1-HIGH-1: PowerProbe must see its code-space synthetic injection on high-contrast native HDR10 PQ (V)");
			Assert::AreEqual(uint32_t(0), recHdrNat.measured[0].reasonBits & C::kReasonNoPower,
				L"SOTA1-HIGH-1: high-contrast native HDR10 PQ must NOT set kReasonNoPower on measured H");
			Assert::AreEqual(uint32_t(0), recHdrNat.measured[1].reasonBits & C::kReasonNoPower,
				L"SOTA1-HIGH-1: high-contrast native HDR10 PQ must NOT set kReasonNoPower on measured V");
			Assert::AreEqual(uint32_t(0), recHdrNat.applied[0].reasonBits & C::kReasonNoPower,
				L"SOTA1-HIGH-1: high-contrast native HDR10 PQ must NOT set kReasonNoPower on applied H");
			Assert::AreEqual(uint32_t(0), recHdrNat.applied[1].reasonBits & C::kReasonNoPower,
				L"SOTA1-HIGH-1: high-contrast native HDR10 PQ must NOT set kReasonNoPower on applied V");
		}

		// 3. Dark-toe clamp (0..64 codes), highlight ceiling (940..1023 codes), non-finite inputs, workspace reuse,
		//    Original pathway forces OFF, and default-OFF W1DescaleRef bit-identity (std::memcmp == 0).
		TEST_METHOD(Sota1DarkToeClampHighlightCeilingNonFiniteWorkspaceReuseOriginalAndW1BitIdentity)
		{
			using namespace Sota1HdrFx;

			// (A) Dark-toe 1-LSB slope clamp across every integer code 0..64 in limited range (both PQ and HLG):
			for (int code = 0; code <= 64; ++code)
			{
				bool clampedPq = false;
				const double sigPq = C::LocalQuantSigmaLinear(C::HdrTransfer::Pq, static_cast<double>(code), 10, true, &clampedPq);
				Assert::IsTrue(clampedPq, L"limited-range code 0..64 must engage dark-toe 1-LSB derivative clamp on PQ");
				Assert::IsTrue(std::isfinite(sigPq) && sigPq > 0.0, L"clamped dark-toe PQ sigma must be finite and strictly positive");

				bool clampedHlg = false;
				const double sigHlg = C::LocalQuantSigmaLinear(C::HdrTransfer::Hlg, static_cast<double>(code), 10, true, &clampedHlg);
				Assert::IsTrue(clampedHlg, L"limited-range code 0..64 must engage dark-toe 1-LSB derivative clamp on HLG");
				Assert::IsTrue(std::isfinite(sigHlg) && sigHlg > 0.0, L"clamped dark-toe HLG sigma must be finite and strictly positive");
			}
			// Midtone negative control: code 502 (E'=0.5) must NOT clamp!
			{
				bool clampedMid = true;
				const double sigMid = C::LocalQuantSigmaLinear(C::HdrTransfer::Pq, 502.0, 10, true, &clampedMid);
				Assert::IsFalse(clampedMid, L"negative control: midtone code 502 must not trigger dark-toe clamp");
				Assert::IsTrue(sigMid > 1e-5);
			}

			// (B) Highlight ceiling (940..1023 super-white codes): finite saturation to Y=1.0 and positive sigma, zero NaN/Inf:
			for (int code = 940; code <= 1023; ++code)
			{
				const double ep = C::Code10ToNorm(static_cast<double>(code), true);
				Assert::AreEqual(1.0, ep, 0.0);
				Assert::AreEqual(1.0, C::PqEotf(ep), 0.0);
				Assert::AreEqual(1.0, C::HlgInvOetf(ep), 1e-15);
				bool clampedHi = true;
				const double sigHi = C::LocalQuantSigmaLinear(C::HdrTransfer::Pq, static_cast<double>(code), 10, true, &clampedHi);
				Assert::IsFalse(clampedHi);
				Assert::IsTrue(std::isfinite(sigHi) && sigHi > 0.0);
			}

			// Verify LinearizeLineSet & ChainEstimator hdrDarkToeClamps counter on dark-toe + highlight lines vs midtone control:
			C::LineSet toeHiLs = BuildHdrLineSet(0, 1024, 512, 8, 32, kBicubic, C::HdrTransfer::Pq, 10, true, 0xA301ull);
			C::LineSet midOnlyLs = toeHiLs;
			for (int j = 0; j < midOnlyLs.count; ++j)
			{
				double* row = midOnlyLs.Line(j);
				for (int i = 0; i < midOnlyLs.length; ++i)
					row[i] = 420.0 + 0.1 * (row[i] - 420.0);
			}
			for (int j = 0; j < toeHiLs.count; ++j)
			{
				double* row = toeHiLs.Line(j);
				for (int i = 0; i < 20; ++i)
					row[i] = static_cast<double>(i * 3); // codes 0..57 in dark toe
				for (int i = 20; i < 32; ++i)
					row[i] = 940.0 + static_cast<double>((i - 20) * 6); // codes 940..1012 at/above highlight ceiling
			}

			C::HdrNoiseSettings pqCfg;
			pqCfg.enabled = true;
			pqCfg.transfer = C::HdrTransfer::Pq;
			pqCfg.bitDepth = 10;
			pqCfg.limitedRange = true;
			C::HdrNoiseWorkspace wsA, wsFresh;
			C::HdrNoiseReason linReason = C::HdrNoiseReason::Disabled;
			int clampedCnt = 0;
			Assert::IsTrue(C::LinearizeLineSet(toeHiLs, pqCfg, wsA, linReason, clampedCnt));
			Assert::IsTrue(linReason == C::HdrNoiseReason::DarkToeClamped);
			Assert::AreEqual(8 * 20, clampedCnt, L"exact count of 0..64 dark-toe samples clamped across 8 lines");

			int clampedMidCnt = -1;
			Assert::IsTrue(C::LinearizeLineSet(midOnlyLs, pqCfg, wsA, linReason, clampedMidCnt));
			Assert::IsTrue(linReason == C::HdrNoiseReason::Ok);
			Assert::AreEqual(0, clampedMidCnt, L"negative control: midtone-only lines must report 0 dark-toe clamps");

			// (C) Non-finite inputs (NaN, +Inf, -Inf in samples and settings):
			const double badVals[3] = {
				std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::infinity(),
				-std::numeric_limits<double>::infinity()
			};
			for (int b = 0; b < 3; ++b)
			{
				C::LineSet badLs = toeHiLs;
				badLs.Line(3)[55] = badVals[b];
				const C::HdrVerifyResult badRes = C::VerifyLinearizedHeteroscedastic(badLs, 512, pqCfg, wsA);
				Assert::IsFalse(badRes.valid, L"non-finite sample must be rejected cleanly");
				Assert::IsTrue(badRes.reason == C::HdrNoiseReason::InvalidInput);
			}
			{
				C::HdrNoiseSettings badCfg = pqCfg;
				badCfg.floorGuardDivisor = std::numeric_limits<double>::quiet_NaN();
				const C::HdrVerifyResult badCfgRes = C::VerifyLinearizedHeteroscedastic(toeHiLs, 512, badCfg, wsA);
				Assert::IsFalse(badCfgRes.valid);
				Assert::IsTrue(badCfgRes.reason == C::HdrNoiseReason::InvalidInput);
			}

			// (D) Workspace reuse: dirty wsA (after different sizes and NaN failures) vs brand-new wsFresh must be bit-identical:
			const C::HdrVerifyResult resReused = C::VerifyLinearizedHeteroscedastic(toeHiLs, 512, pqCfg, wsA);
			const C::HdrVerifyResult resFresh = C::VerifyLinearizedHeteroscedastic(toeHiLs, 512, pqCfg, wsFresh);
			Assert::IsTrue(resReused.valid && resFresh.valid);
			Assert::IsTrue(W1DescaleRef::MemEq(resReused.stat, resFresh.stat),
				L"reused HdrNoiseWorkspace must produce bit-identical stat to a fresh workspace");
			Assert::IsTrue(W1DescaleRef::MemEq(resReused.whitenedRms, resFresh.whitenedRms));
			Assert::IsTrue(W1DescaleRef::MemEq(resReused.uniformLinearStat, resFresh.uniformLinearStat));
			Assert::AreEqual(resFresh.bestKernel, resReused.bestKernel);
			Assert::AreEqual(resFresh.darkToeClampedSamples, resReused.darkToeClampedSamples);

			// (E) Original pathway forces OFF (and dark-toe counter increments when active):
			C::LineSnapshot toeSnap = BuildHdrSnapshot10Bit(kUhdW, kUhdH, 1920, 1080, kLanczos3, C::HdrTransfer::Pq,
				1, 1000000, 0xA309ull, true);
			for (size_t i = 0; i < 64 && i < toeSnap.rows.size(); ++i)
				toeSnap.rows[i] = 32u; // below limited-range black 64 -> triggers dark-toe clamp

			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.hdrNoise = pqCfg;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetOriginalPathway(true);
				Assert::IsFalse(est.Running(), L"Original pathway must force ChainEstimator off even when hdrNoise is enabled");
				Assert::IsFalse(est.SubmitLines(toeSnap), L"SubmitLines must return false while Original pathway is active");
				Assert::AreEqual(uint64_t(0), est.GetCounters().hdrLinearizedEvals);
				Assert::AreEqual(uint64_t(0), est.GetCounters().hdrDarkToeClamps);
				Assert::IsTrue(est.StatusLine().find("Original pathway") != std::string::npos);

				// Negative control: leaving Original pathway resumes HDR evaluation and counts dark-toe clamps:
				est.SetOriginalPathway(false);
				Assert::IsTrue(est.Running());
				Assert::IsTrue(est.SubmitLines(toeSnap));
				Assert::IsTrue(est.GetCounters().hdrLinearizedEvals > 0u);
				Assert::IsTrue(est.GetCounters().hdrDarkToeClamps >= 64u,
					L"ChainEstimator must count dark-toe clamped samples in EstimatorCounters::hdrDarkToeClamps");
				est.Stop();
			}

			// (F) Default-OFF W1DescaleRef bit-identity (std::memcmp == 0) + 1-ULP perturbation negative control:
			C::VerifierSettings defVs;
			Assert::IsFalse(defVs.hdrNoise.enabled);
			Assert::IsFalse(defVs.gaussNewtonPhaseRefine);
			C::DescaleVerifier liveVer(defVs);
			W1DescaleRef::DescaleVerifierW1 w1Ver(defVs);
			C::DescaleVerifier liveProbeVer(defVs);
			W1DescaleRef::DescaleVerifierW1 w1ProbeVer(defVs);
			const C::ProbeSettings defProbe;
			C::PowerProbe::Workspace probeWs;
			const int candsW1[2] = { 1920, 2560 };
			const C::VerifyOptions defOpt;

			C::LineSet lsH;
			Assert::IsTrue(C::ToLineSet(toeSnap, 0, lsH));
			C::VerifyResult vrLive, vrW1;
			Assert::IsTrue(liveVer.Evaluate(lsH, candsW1, 2, defOpt, vrLive));
			Assert::IsTrue(w1Ver.Evaluate(lsH, candsW1, 2, defOpt, vrW1));
			Assert::IsTrue(W1DescaleRef::MemEq(vrLive.reason, vrW1.reason));
			Assert::AreEqual(vrW1.candidates.size(), vrLive.candidates.size());
			for (size_t ci = 0; ci < vrLive.candidates.size(); ++ci)
			{
				const C::CandidateResult& a = vrLive.candidates[ci];
				const C::CandidateResult& b = vrW1.candidates[ci];
				Assert::IsFalse(a.hdrLinearizedUsed);
				Assert::AreEqual(0, a.hdrDarkToeClampedSamples);
				Assert::IsTrue(W1DescaleRef::MemEq(a.stat, b.stat));
				Assert::IsTrue(W1DescaleRef::MemEq(a.floorCodes, b.floorCodes));
				Assert::IsTrue(W1DescaleRef::MemEq(a.baselineMedian, b.baselineMedian));
				Assert::AreEqual(b.lineStat.size(), a.lineStat.size());
				for (size_t li = 0; li < a.lineStat.size(); ++li)
					Assert::IsTrue(W1DescaleRef::MemEq(a.lineStat[li], b.lineStat[li]));
			}

			const double floorCodes = 1.0 / std::sqrt(12.0);
			C::ProbeResult prLive, prW1;
			Assert::IsTrue(C::PowerProbe::Run(liveProbeVer, lsH, floorCodes, 1.0, defProbe, prLive, &probeWs));
			Assert::IsTrue(W1DescaleRef::PowerProbeW1::Run(w1ProbeVer, lsH, floorCodes, 1.0, defProbe, prW1));
			Assert::IsTrue(W1DescaleRef::MemEq(prLive.stat, prW1.stat));

			// 1-ULP negative control proving MemEq catches single-ULP drift:
			const double perturbed = std::nextafter(vrLive.candidates[0].stat, vrLive.candidates[0].stat + 1.0);
			Assert::IsFalse(W1DescaleRef::MemEq(vrLive.candidates[0].stat, perturbed),
				L"negative control: 1-ULP drift must fail W1DescaleRef::MemEq");
		}

		// 4. SOTA1-HIGH-1 regression: high-contrast native HDR10 PQ stream confirms NativeCoded with probeSeen == true
		//    and zero kReasonNoPower across every evaluation (vs same-input linearized-probe negative control that
		//    warps PowerProbe's code-space 0.61x synthetic upscale and destroys probeSeen), plus VerifyOptions::onlyKernel
		//    honored in linearized HDR WLS verification.
		TEST_METHOD(Sota1HighContrastNativeHdrPowerProbeSeenAndConfirmsNativeCoded)
		{
			using namespace Sota1HdrFx;

			C::HdrNoiseSettings pqCfg;
			pqCfg.enabled = true;
			pqCfg.transfer = C::HdrTransfer::Pq;
			pqCfg.bitDepth = 10;
			pqCfg.limitedRange = true;
			pqCfg.linearizeForDescale = true;

			// Build 3-shot x 6-eval (18 evaluations at 0.8 s cadence -> 4.0 s dwell/shot, all 3 shots inside 12.75 s window)
			// high-contrast aperiodic native 3840x2160 HDR10 PQ stream (variable cycle widths avoid synthetic fixed-period
			// harmonic Fourier-comb dips on rational sub-grids while preserving exact PQ shadow/pedestal/specular physics):
			auto buildAperiodicNativePqSnap = [](uint64_t seq, int64_t timeUs, uint64_t seed, bool cut) -> C::LineSnapshot {
				C::LineRect active{ 0, 0, kUhdW, kUhdH };
				const C::LinePlan plan = C::LineSampler::Plan(active, seq);
				Assert::IsTrue(plan.valid, L"fixture: valid LinePlan");

				C::LineSnapshot snap;
				snap.seq = seq;
				snap.timeUs = timeUs;
				snap.rasterW = kUhdW;
				snap.rasterH = kUhdH;
				snap.active = plan.active;
				snap.inner = plan.inner;
				snap.rowCount = plan.rowCount;
				snap.colCount = plan.colCount;
				snap.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
				snap.colX.assign(plan.colX, plan.colX + plan.colCount);
				snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 64u);
				snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 64u);
				if (cut)
				{
					snap.flags = C::kEvCut;
					snap.cuts = 1;
					snap.eventTimeUs = timeUs;
				}

				SplitMix64 rng(seed ^ (seq * 0x9e3779b97f4a7c15ull));
				auto synthAperiodicLine = [&rng](int nLen, std::vector<double>& outCode10) {
					const double yShadowBase = 0.0028;
					const double yShadowTex = 0.00055;
					const double yMidLit = 0.095;
					const double ySpecHi = 0.72;
					const int ramp = 6;
					outCode10.resize(static_cast<size_t>(nLen));
					int i = 0;
					while (i < nLen)
					{
						for (int r = 0; r < ramp && i < nLen; ++r, ++i)
						{
							const double u = (static_cast<double>(r) + 0.5) / static_cast<double>(ramp);
							const double w = 0.5 - 0.5 * std::cos(C::kPi * u);
							const double yLin = std::exp(std::log(yShadowBase) + (std::log(yMidLit) - std::log(yShadowBase)) * w);
							outCode10[static_cast<size_t>(i)] = QuantizeHdrCode10(C::PqOetf(yLin), 10, true);
						}
						const int hiW = 15 + static_cast<int>(rng.NextU64() % 14ull);
						const int endHi = (std::min)(nLen, i + hiW);
						while (i < endHi)
						{
							const int runLen = 2 + static_cast<int>(rng.NextU64() % 5ull);
							const bool isHi = (rng.NextU64() & 1ull) != 0ull;
							for (int k = 0; k < runLen && i < endHi; ++k, ++i)
							{
								const double target = isHi
									? (ySpecHi + 0.020 * rng.Normal01())
									: (yMidLit + 0.012 * rng.Normal01());
								const double yLin = C::ClampD(target, 0.085, 0.85);
								outCode10[static_cast<size_t>(i)] = QuantizeHdrCode10(C::PqOetf(yLin), 10, true);
							}
						}
						for (int r = 0; r < ramp && i < nLen; ++r, ++i)
						{
							const double u = (static_cast<double>(r) + 0.5) / static_cast<double>(ramp);
							const double w = 0.5 - 0.5 * std::cos(C::kPi * u);
							const double yLin = std::exp(std::log(yMidLit) + (std::log(yShadowBase) - std::log(yMidLit)) * w);
							outCode10[static_cast<size_t>(i)] = QuantizeHdrCode10(C::PqOetf(yLin), 10, true);
						}
						const int shW = 13 + static_cast<int>(rng.NextU64() % 18ull);
						const int endSh = (std::min)(nLen, i + shW);
						while (i < endSh)
						{
							const double yLin = C::ClampD(yShadowBase + yShadowTex * rng.Normal01(), 0.0016, 0.04);
							outCode10[static_cast<size_t>(i)] = QuantizeHdrCode10(C::PqOetf(yLin), 10, true);
							++i;
						}
					}
				};

				std::vector<double> fullLine;
				for (int j = 0; j < plan.rowCount; ++j)
				{
					synthAperiodicLine(kUhdW, fullLine);
					uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
					for (int i = 0; i < plan.inner.w; ++i)
						dst[i] = static_cast<uint16_t>(std::floor(fullLine[static_cast<size_t>(plan.inner.x + i)] + 0.5));
				}
				for (int j = 0; j < plan.colCount; ++j)
				{
					synthAperiodicLine(kUhdH, fullLine);
					uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
					for (int i = 0; i < plan.inner.h; ++i)
						dst[i] = static_cast<uint16_t>(std::floor(fullLine[static_cast<size_t>(plan.inner.y + i)] + 0.5));
				}
				return snap;
			};

			std::vector<C::LineSnapshot> pqNatStream;
			pqNatStream.reserve(18u);
			for (int s = 0; s < 18; ++s)
			{
				const bool isCut = (s % 6) == 0;
				pqNatStream.push_back(buildAperiodicNativePqSnap(
					static_cast<uint64_t>(100 + s), static_cast<int64_t>(s) * 800000,
					0x9400ull + static_cast<uint64_t>(s * 37), isCut));
			}

			// (A) Mandatory Same-Input Negative Control on PowerProbe:
			//     A verifier that keeps hdrNoise enabled (pre-fix SOTA1-HIGH-1 bug) linearizes PowerProbe's code-space
			//     0.61x bicubic injection across specular PQ edges, destroying the synthetic probe dip (seen == false,
			//     kReasonNoPower), whereas a code-space probeVerifier sees its synthetic injection cleanly!
			{
				C::LineSet natLsH;
				Assert::IsTrue(C::ToLineSet(pqNatStream[0], 0, natLsH));
				const double floorCodes = 1.0 / std::sqrt(12.0);
				const C::ProbeSettings defProbe;

				C::VerifierSettings vsCode;
				Assert::IsFalse(vsCode.hdrNoise.enabled);
				C::DescaleVerifier verCode(vsCode);

				C::VerifierSettings vsBrokenLinearProbe;
				vsBrokenLinearProbe.hdrNoise = pqCfg;
				C::DescaleVerifier verBrokenLinearProbe(vsBrokenLinearProbe);

				C::ProbeResult prCode, prBroken;
				Assert::IsTrue(C::PowerProbe::Run(verCode, natLsH, floorCodes, 1.0, defProbe, prCode));
				Assert::IsTrue(C::PowerProbe::Run(verBrokenLinearProbe, natLsH, floorCodes, 1.0, defProbe, prBroken));

				Assert::IsTrue(prCode.ran && prCode.seen,
					L"code-space probeVerifier must see PowerProbe's synthetic 0.61x upscale on high-contrast native HDR");
				Assert::AreEqual(uint32_t(0), prCode.reason & C::kReasonNoPower);
				Assert::IsFalse(prBroken.seen,
					L"hazard proof (negative control): linearizing PowerProbe's code-space injection destroys probeSeen on high-contrast HDR");
				Assert::IsTrue((prBroken.reason & C::kReasonNoPower) != 0u);
				Assert::IsTrue(prCode.stat < prBroken.stat - 1.0);
			}

			// (B) VerifyOptions::onlyKernel honored when hdrNoise.enabled == true:
			{
				const C::LineSet upLs = BuildHdrLineSet(0, 1024, 512, 8, 32, kLanczos3, C::HdrTransfer::Pq, 10, true, 0xB401ull);
				C::VerifierSettings vsHdr;
				vsHdr.hdrNoise = pqCfg;
				C::DescaleVerifier verHdr(vsHdr);
				const int cand512[1] = { 512 };

				C::VerifyOptions optAll;
				optAll.exactOnly = true;
				optAll.exactHalfWidth = 0;
				C::VerifyResult vrAll;
				Assert::IsTrue(verHdr.Evaluate(upLs, cand512, 1, optAll, vrAll));
				Assert::IsTrue(vrAll.candidates[0].valid && vrAll.candidates[0].hdrLinearizedUsed);
				Assert::AreEqual(kLanczos3, vrAll.candidates[0].kernel);

				C::VerifyOptions optOnlyBicubic = optAll;
				optOnlyBicubic.onlyKernel = kBicubic;
				C::VerifyResult vrOnly;
				Assert::IsTrue(verHdr.Evaluate(upLs, cand512, 1, optOnlyBicubic, vrOnly));
				Assert::IsTrue(vrOnly.candidates[0].valid && vrOnly.candidates[0].hdrLinearizedUsed);
				Assert::AreEqual(kBicubic, vrOnly.candidates[0].kernel,
					L"SOTA1-HIGH-1: DescaleVerifier linearized HDR WLS path must honor VerifyOptions::onlyKernel");
			}

			// (C) End-to-end ChainEstimator with hdrNoise enabled on pqNatStream:
			//     probeSeen == true on EVERY evaluation, zero kReasonNoPower, and Confident NativeCoded confirmation!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.hdrNoise = pqCfg;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				int evalCount = 0;
				int probeSeenCountH = 0;
				int probeSeenCountV = 0;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) {
					++evalCount;
					if (d.axis[0].probeRan && d.axis[0].probeSeen)
						++probeSeenCountH;
					if (d.axis[1].probeRan && d.axis[1].probeSeen)
						++probeSeenCountV;
				});
				est.SetMode(C::EstimatorMode::On);
				for (size_t i = 0; i < pqNatStream.size(); ++i)
				{
					clk.Set(static_cast<double>(i) * 800.0);
					Assert::IsTrue(est.SubmitLines(pqNatStream[i]));
				}

				Assert::AreEqual(18, evalCount);
				Assert::AreEqual(18, probeSeenCountH,
					L"SOTA1-HIGH-1: probeSeen must be true on every evaluation of sharp native HDR10 PQ (H)");
				Assert::AreEqual(18, probeSeenCountV,
					L"SOTA1-HIGH-1: probeSeen must be true on every evaluation of sharp native HDR10 PQ (V)");

				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				Assert::AreEqual(uint32_t(0), rec.measured[0].reasonBits & C::kReasonNoPower);
				Assert::AreEqual(uint32_t(0), rec.measured[1].reasonBits & C::kReasonNoPower);
				Assert::AreEqual(uint32_t(0), rec.applied[0].reasonBits & C::kReasonNoPower);
				Assert::AreEqual(uint32_t(0), rec.applied[1].reasonBits & C::kReasonNoPower);
				Assert::IsTrue(rec.codedRaster == G::CodedRaster::NativeCoded,
					L"SOTA1-HIGH-1: sharp native HDR10 PQ stream must confirm CodedRaster::NativeCoded");

				const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };
				const SourceGeometry::ConfidentGeometry cg = SourceGeometry::ConfidentOf(rec, deliv);
				Assert::IsTrue(cg.axis[0].confident && cg.axis[0].n == kUhdW);
				Assert::IsTrue(cg.axis[1].confident && cg.axis[1].n == kUhdH);
				Assert::IsTrue(est.StatusLine().find("native, confident") != std::string::npos);
				est.Stop();
			}
		}
	};
}
