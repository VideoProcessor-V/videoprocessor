/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Proving synthetic-physics unit tests for ChainEdgeStats (SID Stage S1, EXP-8BIT MUST-1/2, SHOULD-1/2/3, Pitfall #1).
// CPU only, synthetic only, deterministic (fixed SplitMix64 PRNG, zero randomness).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainEdgeStats.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace EdgeStatsFx
	{
		using namespace SourceIdentity::Chain;

		inline EdgeStatsSettings Enabled()
		{
			EdgeStatsSettings s;
			s.enabled = true;
			return s;
		}

		// Deterministic PRNG (SplitMix64) for reproducible synthetic noise/dither tests.
		struct DetRng
		{
			uint64_t state;
			explicit DetRng(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			// Uniform in [-0.5, 0.5)
			double UniformCentered()
			{
				const double u = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				return u - 0.5;
			}

			// TPDF in [-1.0, 1.0): sum of two independent Uniform[-0.5, 0.5) variates (sd = 1 / sqrt(6))
			double TpdfUnit()
			{
				return UniformCentered() + UniformCentered();
			}

			// Box-Muller normal(0, 1)
			double Normal01()
			{
				double u1 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				double u2 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				if (u1 < 1e-12) u1 = 1e-12;
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
			}
		};
	}

	TEST_CLASS(ChainEdgeStatsTests)
	{
	public:
		TEST_METHOD(DefaultSettingsAreOffAndVisiblyDisabled)
		{
			using namespace EdgeStatsFx;
			EdgeStatsSettings def;
			Assert::IsFalse(def.enabled);

			LineSet ls;
			ls.Resize(2, 512);
			Assert::IsTrue(EstimateEmpiricalFloor(ls, 4.0, def).reason == EdgeStatsReason::Disabled);
			Assert::IsTrue(ComputeExcessVarianceDip(1.8, 5.0, 1.633, def).reason == EdgeStatsReason::Disabled);

			CandidateResult cand;
			cand.valid = true;
			cand.lineCount = 1;
			cand.segCount = 1;
			Assert::IsTrue(ComputeSegmentGatedStat(cand, 0.5, 1.633, def).reason == EdgeStatsReason::Disabled);

			double buf[16] = {};
			Assert::IsTrue(ComputeGradientMaskedHuberRms(buf, buf, 16, 1.633, 0.5, def).reason == EdgeStatsReason::Disabled);
			Assert::IsTrue(ComputeDitherAwareDetailAbove(10.0, 1.0, 1.0, def).reason == EdgeStatsReason::Disabled);

			std::vector<uint8_t> mask;
			Assert::IsTrue(BuildEdgeGateMask(buf, 16, 1.633, mask, def).reason == EdgeStatsReason::Disabled);
			Assert::IsTrue(ComputePhaseSecondDiffContrast(buf, 16, 2, nullptr, def).reason == EdgeStatsReason::Disabled);

			const auto comb = ClassifyHalfNotchWithCombCheck(1920, 3840, -0.5, true, 0.0, 0.0, 0.0, def);
			Assert::IsTrue(comb.reason == EdgeStatsReason::Disabled);
			Assert::AreEqual(kClassK8, comb.resolvedNullClass);
		}

		TEST_METHOD(SegmentGatedAndGradientMaskedHuberRecoverMoreThan2xDipSeparationOn8BitDitheredScene)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();
			const double floor8Tpdf = TpdfDitherFloorCodes(4.0); // 1.633 codes
			const double dofRatio = 0.5;                         // 1080p -> 4K (1/2)

			// Part A: Segment-gated T_ev on a synthetic 8-bit TPDF-dithered scene:
			// 16 lines x 10 segments per line = 160 segments: 80% (8/10) flat dithered background (base = 1.70, s = 1.65)
			// and 20% (2/10) sharp edge segments where true 1080p->4K upscale has baseline = 12.0 and candidate residual = 2.0
			// (signal residual ~1.15 + floor 1.633 in quadrature).
			CandidateResult cand;
			cand.valid = true;
			cand.lineCount = 16;
			cand.segCount = 10;
			const size_t total = static_cast<size_t>(cand.lineCount * cand.segCount);
			cand.segSum.resize(total);
			cand.segRows.assign(total, kSegPx);
			cand.segBase.resize(total);

			double wholeLineLogDipSum = 0.0;
			for (int li = 0; li < cand.lineCount; ++li)
			{
				double lineRss = 0.0;
				double lineBaseSq = 0.0;
				for (int sg = 0; sg < cand.segCount; ++sg)
				{
					const size_t idx = static_cast<size_t>(li * cand.segCount + sg);
					const bool isEdge = (sg < 2); // 20% edge segments, 80% flat background
					const double segRms = isEdge ? 1.85 : 1.65;
					const double segBase = isEdge ? 6.0 : 1.70;
					cand.segSum[idx] = segRms * segRms * static_cast<double>(kSegPx) * (1.0 - dofRatio);
					cand.segBase[idx] = segBase;
					lineRss += cand.segSum[idx];
					lineBaseSq += segBase * segBase;
				}
				const double wholeRms = std::sqrt(lineRss / (static_cast<double>(cand.segCount * kSegPx) * (1.0 - dofRatio)));
				const double wholeBase = std::sqrt(lineBaseSq / static_cast<double>(cand.segCount));
				wholeLineLogDipSum += std::log(wholeRms / wholeBase);
			}
			const double wholeLineTev = wholeLineLogDipSum / static_cast<double>(cand.lineCount);

			const SegmentGatedResult gated = ComputeSegmentGatedStat(cand, dofRatio, floor8Tpdf, s);
			Assert::IsTrue(gated.valid);
			Assert::AreEqual(32, gated.informativeSegments);  // exact 20% edge segments kept
			Assert::AreEqual(128, gated.flatSegmentsSkipped); // exact 80% flat segments skipped
			// Whole-line T_ev is heavily diluted (~ -1.16), while segment-gated excess-variance T_ev recovers > 2x dip!
			Assert::IsTrue(gated.stat < 2.0 * wholeLineTev);

			// Part B: Pixel-level Gradient-Masked Huber RMS on a 1024-px synthetic line (80% flat dither + 20% sharp edges)
			// plus a single sharp sharpening/impulse outlier spike (+40 codes) on an edge pixel.
			DetRng rng(42);
			const int len = 1024;
			std::vector<double> line(static_cast<size_t>(len), 512.0);
			std::vector<double> candRes(static_cast<size_t>(len), 0.0);
			std::vector<double> baseRes(static_cast<size_t>(len), 0.0);

			for (int i = 0; i < len; ++i)
			{
				const bool edgeZone = (i >= 100 && i < 305); // ~20% textured edge zone
				double sig = 512.0;
				if (edgeZone)
					sig = 512.0 + 50.0 * std::sin(0.8 * static_cast<double>(i)); // active high-gradient texture throughout 20% zone
				line[static_cast<size_t>(i)] = sig + 4.0 * rng.TpdfUnit();
				// True upscale residual on edges is ~floor; wrong-size baseline residual on edges is ~15 codes.
				candRes[static_cast<size_t>(i)] = 4.0 * rng.TpdfUnit();
				baseRes[static_cast<size_t>(i)] = edgeZone ? (15.0 + 4.0 * rng.TpdfUnit()) : (4.0 * rng.TpdfUnit());
			}
			// Inject 2 strong impulse/sharpening outliers on edge pixels in candidate residual.
			candRes[150] = 60.0;
			candRes[200] = -60.0;

			const MaskedHuberResult mCand = ComputeGradientMaskedHuberRms(line.data(), candRes.data(), len, floor8Tpdf, dofRatio, s);
			const MaskedHuberResult mBase = ComputeGradientMaskedHuberRms(line.data(), baseRes.data(), len, floor8Tpdf, dofRatio, s);
			Assert::IsTrue(mCand.valid);
			Assert::IsTrue(mBase.valid);
			Assert::IsTrue(mCand.huberClippedPixels >= 2);
			// Huber dramatically resists the impulse outliers compared to standard masked RMS!
			Assert::IsTrue(mCand.huberRms < 0.75 * mCand.standardMaskedRms);
			// And recovers a strong negative dip between candidate and baseline!
			const double huberDip = std::log(mCand.huberRms / mBase.huberRms);
			Assert::IsTrue(huberDip < -1.0);
		}

		TEST_METHOD(ExcessVarianceDipAligns8BitAnd10BitWithin15PercentAndIsFiniteOnWhiteNoise)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();

			const double floor10 = QuantizationFloorCodes(1.0);  // 0.2887
			const double floor8 = TpdfDitherFloorCodes(4.0);     // 1.6330

			// Same underlying synthetic upscale signal: s_sig = 0.90 codes, base_sig = 4.50 codes (true ln dip = ln(0.2) = -1.6094).
			const double sSig = 0.90;
			const double bSig = 4.50;
			const double trueDip = std::log(sSig / bSig);

			const double s10 = std::sqrt(sSig * sSig + floor10 * floor10);
			const double b10 = std::sqrt(bSig * bSig + floor10 * floor10);
			const double s8 = std::sqrt(sSig * sSig + floor8 * floor8);
			const double b8 = std::sqrt(bSig * bSig + floor8 * floor8);

			// Plain log dip is compressed on 8-bit by > 30% relative to 10-bit!
			const double plain10 = std::log(s10 / b10);
			const double plain8 = std::log(s8 / b8);
			Assert::IsTrue(std::fabs(plain8 - plain10) / std::fabs(plain10) > 0.30);

			// Excess-variance dip places 8-bit and 10-bit onto the exact same scale (well within +-15%)!
			const ExcessDipResult ex10 = ComputeExcessVarianceDip(s10, b10, floor10, s);
			const ExcessDipResult ex8 = ComputeExcessVarianceDip(s8, b8, floor8, s);
			Assert::IsTrue(ex10.valid && ex8.valid);
			Assert::IsTrue(std::fabs(ex8.dip - ex10.dip) / std::fabs(ex10.dip) < 0.05);
			Assert::IsTrue(std::fabs(ex8.dip - trueDip) < 0.05);

			// Native white noise (s == b == floor, or s < floor): never blows up to -inf or NaN!
			const ExcessDipResult whiteEq = ComputeExcessVarianceDip(floor8, floor8, floor8, s);
			Assert::IsTrue(whiteEq.valid);
			Assert::IsTrue(std::isfinite(whiteEq.dip));
			Assert::IsTrue(std::fabs(whiteEq.dip) < 1e-12);
			Assert::IsTrue(whiteEq.clampedNumerator && whiteEq.clampedDenominator);
			Assert::IsTrue(whiteEq.reason == EdgeStatsReason::BaselineBelowFloor);

			const ExcessDipResult subFloor = ComputeExcessVarianceDip(0.5 * floor8, 5.0, floor8, s);
			Assert::IsTrue(subFloor.valid);
			Assert::IsTrue(std::isfinite(subFloor.dip));
			Assert::IsTrue(subFloor.clampedNumerator);
			Assert::IsTrue(subFloor.reason == EdgeStatsReason::ClampedToFloorGuard);
		}

		TEST_METHOD(EmpiricalFloorRecovers10Bit8BitTpdfAndRequantized8In10Within10Percent)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();

			auto makeLines = [&](uint64_t seed, int mode, double extraSigma) {
				DetRng rng(seed);
				LineSet ls;
				ls.Resize(16, 2048); // 16 lines x 8 segments = 128 segments
				for (int li = 0; li < ls.count; ++li)
				{
					double* y = ls.Line(li);
					for (int i = 0; i < ls.length; ++i)
					{
						// Gentle linear ramp (annihilated by 2nd difference) + 12.5% textured edge segment at tail
						double base = 400.0 + 0.05 * static_cast<double>(i);
						if (i >= 1792)
							base += 40.0 * std::sin(0.4 * static_cast<double>(i));
						double noise = 0.0;
						if (mode == 0) noise = 1.0 * rng.UniformCentered();     // 10-bit uniform (0.289)
						else if (mode == 1) noise = 4.0 * rng.UniformCentered();// 8-bit uniform (1.155)
						else if (mode == 2) noise = 4.0 * rng.TpdfUnit();       // 8-bit TPDF (1.633)
						else noise = extraSigma * rng.Normal01();               // re-quantized 8-in-10 link (~1.3)
						y[i] = base + noise;
					}
				}
				return ls;
			};

			// 1. 10-bit uniform rounding: theoretical 0.2887
			{
				const auto res = EstimateEmpiricalFloor(makeLines(101, 0, 0.0), 1.0, s);
				Assert::IsTrue(res.valid);
				const double target = QuantizationFloorCodes(1.0);
				Assert::IsTrue(std::fabs(res.floorCodes - target) / target <= 0.10);
			}
			// 2. 8-bit uniform rounding: theoretical 1.1547
			{
				const auto res = EstimateEmpiricalFloor(makeLines(202, 1, 0.0), 4.0, s);
				Assert::IsTrue(res.valid);
				const double target = QuantizationFloorCodes(4.0);
				Assert::IsTrue(std::fabs(res.floorCodes - target) / target <= 0.10);
			}
			// 3. 8-bit TPDF dither: theoretical 1.6330
			{
				const auto res = EstimateEmpiricalFloor(makeLines(303, 2, 0.0), 4.0, s);
				Assert::IsTrue(res.valid);
				Assert::IsFalse(res.usedTheoreticalFallback);
				const double target = TpdfDitherFloorCodes(4.0);
				Assert::IsTrue(std::fabs(res.floorCodes - target) / target <= 0.10);
			}
			// 4. Simulated HDMI loop re-quantized 8-bit-in-10-bit: stepCodes == 1, true noise == 1.30
			{
				const auto res = EstimateEmpiricalFloor(makeLines(404, 3, 1.30), 1.0, s);
				Assert::IsTrue(res.valid);
				Assert::IsFalse(res.usedTheoreticalFallback);
				Assert::IsTrue(std::fabs(res.floorCodes - 1.30) / 1.30 <= 0.10);
			}
			// 5. Constant clipped flat lines: explicit visible fallback to theoretical step / sqrt(12) (Rule 4).
			{
				LineSet flat;
				flat.Resize(4, 512);
				for (int li = 0; li < flat.count; ++li)
					for (int i = 0; i < flat.length; ++i)
						flat.Line(li)[i] = 512.0;
				const auto res = EstimateEmpiricalFloor(flat, 4.0, s);
				Assert::IsTrue(res.valid);
				Assert::IsTrue(res.usedTheoreticalFallback);
				Assert::IsTrue(res.reason == EdgeStatsReason::TheoreticalFloorFallback);
				Assert::IsTrue(std::fabs(res.floorCodes - QuantizationFloorCodes(4.0)) < 1e-12);
			}
		}

		TEST_METHOD(DitherAwareDetailAboveDistinguishesDitherFloorFromRealHighFreqDetail)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();

			// Synthetic 256-pt FFT spectrum (129 bins, ratio = 0.5 => cut = 64).
			// Floor power per bin = 1.0 (representing 8-bit TPDF dither floor).
			const double floorP = 1.0;

			// Case 1: 8-bit dithered 1080p->4K upscale: below band has signal (power = 8.0),
			// above band sits at dither floor (power = 1.2 <= 2.25 * 1.0).
			std::vector<double> ditheredUpscale(129, 1.2);
			for (int k = 1; k <= 64; ++k)
				ditheredUpscale[static_cast<size_t>(k)] = 8.0;

			const auto rUp = ComputeDitherAwareDetailAboveSpectrum(ditheredUpscale, 0.5, floorP, s);
			Assert::IsTrue(rUp.valid);
			Assert::IsTrue(rUp.aboveIsNoiseFloor);
			Assert::IsFalse(rUp.hasRealDetailAbove);
			Assert::IsTrue(rUp.reason == EdgeStatsReason::AboveBandAtDitherFloor);

			// Case 2: Genuine native 4K: above band has strong real detail (power = 6.0 >> 2.25 * 1.0).
			std::vector<double> native4k(129, 6.0);
			for (int k = 1; k <= 64; ++k)
				native4k[static_cast<size_t>(k)] = 10.0;

			const auto rNat = ComputeDitherAwareDetailAboveSpectrum(native4k, 0.5, floorP, s);
			Assert::IsTrue(rNat.valid);
			Assert::IsFalse(rNat.aboveIsNoiseFloor);
			Assert::IsTrue(rNat.hasRealDetailAbove);
			Assert::IsTrue(rNat.reason == EdgeStatsReason::Ok);
		}

		TEST_METHOD(EdgeGatedProposalHelperBoostsPeriod2ContrastOn80PercentFlat8BitDitheredLine)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();
			const double floor8Tpdf = TpdfDitherFloorCodes(4.0);

			// Build a 1024-px synthetic 2:1 bilinear-upscaled line:
			// 80% flat background + 20% high-contrast edge texture, corrupted by 8-bit TPDF dither.
			// Bilinear 2:1 puts latent samples on even indices (sharp curvature) and exact midpoints on odd indices
			// (zero 2nd-difference on noiseless signal!), creating strong period-2 phase asymmetry on edges.
			DetRng rng(777);
			const int halfLen = 512;
			std::vector<double> latent(static_cast<size_t>(halfLen), 512.0);
			for (int j = 50; j < 152; ++j) // 20% active edge region
				latent[static_cast<size_t>(j)] = 512.0 + ((j % 2 == 0) ? 80.0 : -80.0);

			const int len = 2 * halfLen;
			std::vector<double> line(static_cast<size_t>(len), 512.0);
			for (int j = 0; j < halfLen; ++j)
			{
				const double a = latent[static_cast<size_t>(j)];
				const double b = latent[static_cast<size_t>((std::min)(j + 1, halfLen - 1))];
				line[static_cast<size_t>(2 * j)] = a + 4.0 * rng.TpdfUnit();
				line[static_cast<size_t>(2 * j + 1)] = 0.5 * (a + b) + 4.0 * rng.TpdfUnit();
			}

			std::vector<uint8_t> mask;
			const EdgeGateSummary gate = BuildEdgeGateMask(line.data(), len, floor8Tpdf, mask, s);
			Assert::IsTrue(gate.valid);
			Assert::IsTrue(gate.activeFraction > 0.10 && gate.activeFraction < 0.35);

			const PhaseContrastResult ungated = ComputePhaseSecondDiffContrast(line.data(), len, 2, nullptr, s);
			const PhaseContrastResult gated = ComputePhaseSecondDiffContrast(line.data(), len, 2, &mask, s);
			Assert::IsTrue(ungated.valid && gated.valid);
			// Gating out 80% isotropic flat dither markedly boosts the period-2 phase contrast!
			Assert::IsTrue(gated.phaseContrastRatio > 1.5 * ungated.phaseContrastRatio);
		}

		TEST_METHOD(HalfCombCheckKeeps8x8CodecGridInK8AndIsolatesHalfNotch)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();

			// 1. Native 4K with 8x8 HEVC/H.264 block comb (dips at 3/8, 5/8, 7/8): MUST stay in harsh kClassK8 (0 FP).
			const auto comb = ClassifyHalfNotchWithCombCheck(1920, 3840, -0.14, true, -0.08, -0.10, -0.18, s);
			Assert::IsTrue(comb.isHalfCandidate);
			Assert::IsTrue(comb.isCodecGridComb);
			Assert::IsFalse(comb.isIsolatedHalfNotch);
			Assert::AreEqual(3, comb.oddCombDipsPresent);
			Assert::AreEqual(kClassK8, comb.resolvedNullClass);
			Assert::IsTrue(comb.reason == EdgeStatsReason::CombGridDetected);

			// 2. Isolated 2:1 upscale (strong 1/2 dip at -0.42, flat 3/8, 5/8, 7/8 near 0.0): reclassified to kClassRational!
			const auto isolated = ClassifyHalfNotchWithCombCheck(1920, 3840, -0.42, true, -0.01, 0.005, -0.005, s);
			Assert::IsTrue(isolated.isHalfCandidate);
			Assert::IsFalse(isolated.isCodecGridComb);
			Assert::IsTrue(isolated.isIsolatedHalfNotch);
			Assert::AreEqual(0, isolated.oddCombDipsPresent);
			Assert::AreEqual(kClassRational, isolated.resolvedNullClass);
			Assert::IsTrue(isolated.reason == EdgeStatsReason::IsolatedHalfNotch);

			// 3. Missing comb probes: MUST fail safe to kClassK8 visibly (Rule 4).
			const auto missing = ClassifyHalfNotchWithCombCheck(1920, 3840, -0.50, false, 0.0, 0.0, 0.0, s);
			Assert::AreEqual(kClassK8, missing.resolvedNullClass);
			Assert::IsTrue(missing.reason == EdgeStatsReason::CombProbesUnavailable);

			// 4. Non-1/2 candidate (1280 of 3840 = 1/3): standard ClassifyLatent (kClassRational).
			const auto oneThird = ClassifyHalfNotchWithCombCheck(1280, 3840, -0.40, true, -0.10, -0.10, -0.15, s);
			Assert::IsFalse(oneThird.isHalfCandidate);
			Assert::AreEqual(kClassRational, oneThird.resolvedNullClass);
			Assert::IsTrue(oneThird.reason == EdgeStatsReason::NotHalfCandidate);
		}

		TEST_METHOD(EmptyInvalidInputsSafeAndBitExactDeterministic)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();
			const double nan = std::numeric_limits<double>::quiet_NaN();

			// Zero-division / empty / NaN / out-of-range guards never crash or emit NaN.
			Assert::AreEqual(0.0, QuantizationFloorCodes(0.0));
			Assert::AreEqual(0.0, QuantizationFloorCodes(-4.0));
			Assert::AreEqual(0.0, TpdfDitherFloorCodes(nan));

			LineSet empty;
			Assert::IsFalse(EstimateEmpiricalFloor(empty, 4.0, s).valid);
			Assert::IsFalse(ComputeExcessVarianceDip(-1.0, 5.0, 1.0, s).valid);
			Assert::IsFalse(ComputeExcessVarianceDip(nan, 5.0, 1.0, s).valid);

			CandidateResult emptyCand;
			Assert::IsFalse(ComputeSegmentGatedStat(emptyCand, 0.5, 1.0, s).valid);
			Assert::IsFalse(ComputeSegmentGatedStat(emptyCand, 1.0, 1.0, s).valid); // dofRatio == 1.0 (zero div guard)

			double tiny[3] = { 1.0, 2.0, 3.0 };
			Assert::IsFalse(ComputeGradientMaskedHuberRms(nullptr, tiny, 3, 1.0, 0.5, s).valid);
			Assert::IsFalse(ComputeGradientMaskedHuberRms(tiny, tiny, 3, 1.0, 1.0, s).valid);
			Assert::IsFalse(ComputeDitherAwareDetailAbove(0.0, 1.0, 1.0, s).valid);

			std::vector<uint8_t> mask;
			Assert::IsFalse(BuildEdgeGateMask(tiny, 3, 1.0, mask, s).valid);
			Assert::IsFalse(ComputePhaseSecondDiffContrast(tiny, 3, 2, nullptr, s).valid);
			Assert::IsTrue(ClassifyHalfNotchWithCombCheck(0, 3840, -0.5, true, 0.0, 0.0, 0.0, s).reason == EdgeStatsReason::InvalidInput);

			// Bit-exact determinism across repeated runs.
			DetRng rng(999);
			LineSet ls;
			ls.Resize(4, 1024);
			for (int li = 0; li < ls.count; ++li)
				for (int i = 0; i < ls.length; ++i)
					ls.Line(li)[i] = 500.0 + 4.0 * rng.TpdfUnit();

			const auto r1 = EstimateEmpiricalFloor(ls, 4.0, s);
			const auto r2 = EstimateEmpiricalFloor(ls, 4.0, s);
			Assert::AreEqual(r1.floorCodes, r2.floorCodes);
			Assert::AreEqual(r1.empiricalRawCodes, r2.empiricalRawCodes);
		}

		TEST_METHOD(AuditRegressionsBoundsNanCombFlatNativeAndZeroPhaseEnergy)
		{
			using namespace EdgeStatsFx;
			const EdgeStatsSettings s = Enabled();
			const double nan = std::numeric_limits<double>::quiet_NaN();

			// HIGH-4 + LOW-8: NaN ratio / refDetailDecades / detailMarginDecades cleanly rejected (no UB).
			{
				std::vector<double> bins(129, 2.0);
				Assert::IsFalse(ComputeDitherAwareDetailAboveSpectrum(bins, nan, 1.0, s).valid);
				Assert::IsTrue(ComputeDitherAwareDetailAboveSpectrum(bins, nan, 1.0, s).reason == EdgeStatsReason::InvalidInput);
				Assert::IsFalse(ComputeDitherAwareDetailAbove(10.0, 5.0, 1.0, s, nan, 0.07).valid);
				Assert::IsFalse(ComputeDitherAwareDetailAbove(10.0, 5.0, 1.0, s, 0.0, nan).valid);
			}

			// MEDIUM-1: undersized lines.data rejected cleanly without OOB read.
			{
				LineSet bad;
				bad.count = 4;
				bad.length = 512;
				bad.data.assign(10, 500.0); // intentionally undersized!
				const auto res = EstimateEmpiricalFloor(bad, 4.0, s);
				Assert::IsFalse(res.valid);
				Assert::IsTrue(res.reason == EdgeStatsReason::InvalidInput);
			}

			// MEDIUM-5: flat native 4K (no 1/2 dip, no odd k/8 comb) => NoHalfNotch (NEVER CombGridDetected!).
			{
				const auto flatNat = ClassifyHalfNotchWithCombCheck(1920, 3840, -0.005, true, 0.0, -0.004, 0.002, s);
				Assert::AreEqual(kClassK8, flatNat.resolvedNullClass);
				Assert::IsFalse(flatNat.isCodecGridComb);
				Assert::IsFalse(flatNat.isIsolatedHalfNotch);
				Assert::IsTrue(flatNat.reason == EdgeStatsReason::NoHalfNotch);

				// Undominated single odd comb dip (3/8 active, 1/2 weak) => isCodecGridComb == true, CombGridDetected.
				const auto weakOdd = ClassifyHalfNotchWithCombCheck(1920, 3840, -0.05, true, -0.06, 0.0, 0.0, s);
				Assert::AreEqual(kClassK8, weakOdd.resolvedNullClass);
				Assert::IsTrue(weakOdd.isCodecGridComb);
				Assert::IsTrue(weakOdd.reason == EdgeStatsReason::CombGridDetected);
			}

			// LOW-1: 1/16 block harmonic + +-1 Hann side-lobes excluded in ComputeDitherAwareDetailAboveSpectrum.
			{
				// n = 256 (bins = 129), period = n / 16 = 16; above band for ratio=0.5 is [74..98].
				// Bin 80 (5/16) and side-lobes 79, 81 plus bin 96 (6/16) and 95, 97 are block harmonics!
				std::vector<double> bins(129, 1.2);
				for (int k = 1; k <= 64; ++k) bins[static_cast<size_t>(k)] = 8.0;
				bins[79] = 1e6; bins[80] = 1e6; bins[81] = 1e6;
				bins[95] = 1e6; bins[96] = 1e6; bins[97] = 1e6;
				const auto r = ComputeDitherAwareDetailAboveSpectrum(bins, 0.5, 1.0, s);
				Assert::IsTrue(r.valid);
				Assert::IsTrue(r.aboveIsNoiseFloor);
				Assert::IsFalse(r.hasRealDetailAbove);
			}

			// LOW-2: overflow in ComputeExcessVarianceDip returns InvalidInput (never Ok).
			{
				const auto ov = ComputeExcessVarianceDip(1e200, 1e200, 1.0, s);
				Assert::IsFalse(ov.valid);
				Assert::IsTrue(ov.reason == EdgeStatsReason::InvalidInput);
			}

			// LOW-3: NaN in line[i+1] is NOT masked by std::max in BuildEdgeGateMask / ComputeGradientMaskedHuberRms.
			{
				std::vector<double> line(16, 500.0);
				std::vector<double> res(16, 1.0);
				line[5] = 600.0; // sharp step at 5
				line[6] = nan;   // poisons gR at i=5 and gL at i=6!
				std::vector<uint8_t> mask;
				const auto gate = BuildEdgeGateMask(line.data(), 16, 1.0, mask, s);
				Assert::AreEqual(uint8_t(0), mask[5]);
				Assert::AreEqual(uint8_t(0), mask[6]);
				(void)gate;
			}

			// LOW-4: constant zero-curvature line returns ZeroPhaseEnergy with valid == false and ratio == 1.0.
			{
				std::vector<double> flat(32, 512.0);
				const auto pc = ComputePhaseSecondDiffContrast(flat.data(), 32, 2, nullptr, s);
				Assert::IsFalse(pc.valid);
				Assert::AreEqual(1.0, pc.phaseContrastRatio);
				Assert::IsTrue(pc.reason == EdgeStatsReason::ZeroPhaseEnergy);
			}
		}
	};
}
