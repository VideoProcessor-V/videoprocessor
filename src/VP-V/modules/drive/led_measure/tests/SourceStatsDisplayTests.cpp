// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// O#3 (L2a): Red Team adversarial tests for CPU display-domain P4 variant SourceStatsDisplay.h.
// Pure CPU; verifies independent analytical ground truth, U6 percentile boundary parity,
// canonical VariantKey identity, and same-input negative controls for subtitle masking & gamma boost.

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/drive/led_measure/SourceStatsDisplay.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace VideoProcessorTest
{
	TEST_CLASS(SourceStatsDisplayTests)
	{
	public:
		// Canonical display-domain VariantKey identity vs default source-domain VariantKey (negative control 3).
		TEST_METHOD(VariantKeyIdentityIsCanonicalAndDistinctFromSourceDomain)
		{
			const VpSkeleton::VariantKey key = SourceStatsDisplay::VariantKeyFor();
			Assert::IsFalse(key.IsDefault(), L"Display-domain variant key must not be the default source-domain key");
			Assert::AreNotEqual<uint64_t>(0ull, key.Hash(), L"Display-domain variant key hash must be non-zero");
			Assert::AreEqual(
				static_cast<unsigned>(VpSkeleton::VariantKey::StageAfter(static_cast<uint8_t>(PipelineSlots::Id::ColourAppearance))),
				static_cast<unsigned>(key.stage), L"Display-domain variant must sit at StageAfter(ColourAppearance)");
			Assert::AreEqual(8u, static_cast<unsigned>(key.codeUnit), L"Display-domain code unit must be 8-bit limited code units");
			Assert::AreEqual(static_cast<unsigned>(VpSkeleton::kPrecisionFloat32), static_cast<unsigned>(key.precision),
				L"Default precision must be Float32");

			// Negative control: default source-domain VariantKey differs in identity and hash.
			const VpSkeleton::VariantKey sourceDefault{};
			Assert::IsTrue(sourceDefault.IsDefault());
			Assert::AreEqual<uint64_t>(0ull, sourceDefault.Hash());
			Assert::IsTrue(key != sourceDefault, L"Display-domain VariantKey must differ from default source-domain key");
			Assert::AreNotEqual(sourceDefault.Hash(), key.Hash());
		}

		// Independent analytical ground truth for DisplayPercentile (U6 FirstBinReaching boundary parity) and SummaryFromRaw.
		TEST_METHOD(DisplayPercentileExactBoundaryParityAndSummaryFromRaw)
		{
			uint32_t hist[HISTOGRAM_BINS] = {};
			hist[16] = 500;
			hist[100] = 495;
			hist[200] = 5;

			Assert::AreEqual(100.0, SourceStatsDisplay::DisplayPercentile(hist, 1000, 99.5), 1e-9, L"p99.5 exact boundary");
			Assert::AreEqual(200.0, SourceStatsDisplay::DisplayPercentile(hist, 1000, 99.6), 1e-9, L"p99.6 exact upper tail");
			Assert::AreEqual(0.0, SourceStatsDisplay::DisplayPercentile(hist, 1000, 0.0), 1e-9, L"p0 exact floor");
			Assert::AreEqual(200.0, SourceStatsDisplay::DisplayPercentile(hist, 1000, 100.0), 1e-9, L"p100 exact top bin");
			Assert::AreEqual(16.0, SourceStatsDisplay::DisplayPercentile(hist, 0, 99.5), 1e-9, L"total=0 returns limited black 16");
			Assert::AreEqual(16.0, SourceStatsDisplay::DisplayPercentile(nullptr, 1000, 99.5), 1e-9, L"null histogram returns 16");

			// Exact parity with LedDrive::HistogramPercentile delegation.
			for (double p : { 0.0, 1.0, 50.0, 99.0, 99.5, 99.6, 100.0 })
			{
				Assert::AreEqual(
					SourceStatsDisplay::DisplayPercentile(hist, 1000, p),
					HistogramPercentile(hist, 1000, p),
					1e-12, L"HistogramPercentile must delegate identically to SourceStatsDisplay::DisplayPercentile");
			}

			// SummaryFromRaw against independent analytical sums and ratios.
			RawMeasurement raw{};
			raw.histogram[16] = 500;
			raw.histogram[100] = 495;
			raw.histogram[220] = 5;
			raw.blocks = 1000;
			raw.sum = (500u * 16u + 495u * 100u + 5u * 220u) * SUM_SCALE;
			raw.maximum = 225u * SUM_SCALE;

			const double expectedApl = (500.0 * 16.0 + 495.0 * 100.0 + 5.0 * 220.0) / 1000.0;
			const SourceStatsDisplay::DisplayRecord rec = SourceStatsDisplay::SummaryFromRaw(raw, 99.5, 215);
			Assert::AreEqual(SourceStatsDisplay::kUnitVersion, rec.version);
			Assert::AreEqual(1000u, rec.blocks);
			Assert::AreEqual(expectedApl, rec.apl, 1e-9);
			Assert::AreEqual(100.0, rec.highlight, 1e-9);
			Assert::AreEqual(5.0 / 1000.0, rec.highlightArea, 1e-12);
			Assert::AreEqual(225.0, rec.maximum, 1e-9);
			Assert::AreEqual(16.0, rec.p01, 1e-9);
			Assert::AreEqual(16.0, rec.p50, 1e-9);
			Assert::AreEqual(100.0, rec.p99, 1e-9);

			// When raw.maximum == 0, SummaryFromRaw falls back to highlight percentile.
			RawMeasurement noMax = raw;
			noMax.maximum = 0;
			const SourceStatsDisplay::DisplayRecord noMaxRec = SourceStatsDisplay::SummaryFromRaw(noMax, 99.5, 215);
			Assert::AreEqual(noMaxRec.highlight, noMaxRec.maximum, 1e-9);
		}

		// Negative control 1 (subtitle mask hazard): dim scene (40), 2x2 specular (250), bright subtitle bar (230).
		// Runs BOTH naive unmasked and masked MeasureDisplayFrame on the EXACT same pixels.
		TEST_METHOD(MeasureDisplayFrameSubtitleMaskRejectsSubtitleContamination)
		{
			constexpr int kSize = 192;
			auto pixelCode8 = [](int x, int y) -> double
			{
				if (x >= 61 && x < 63 && y >= 61 && y < 63) return 250.0;       // 2x2 specular inside 3x3 box [60,63)^2
				if (x >= 40 && x < 150 && y >= 160 && y < 176) return 230.0;    // bright subtitle bar
				return 40.0;                                                     // dim picture background
			};
			auto sampleRgb = [&](int x, int y, double& r, double& g, double& b) -> bool
			{
				const double v = pixelCode8(x, y) / 255.0;
				r = g = b = v;
				return true;
			};

			// Screen 3x the script peak grid -> exact 3x3-pixel boxes tiling 192x192.
			SourceStatsDisplay::MeasureParams baseParams;
			baseParams.peakFullCoverage = true;
			baseParams.screenWidth = 3.0 * SCRIPT_PEAK_GRID_W;
			baseParams.screenHeight = 3.0 * SCRIPT_PEAK_GRID_H;

			// Analytical ground truth:
			const double analyticalBgCode = 16.0 + 219.0 * (40.0 / 255.0);
			const double analyticalSubCode = 16.0 + 219.0 * (230.0 / 255.0);
			const double analyticalSpecBoxCode = 16.0 + 219.0 * ((4.0 * 250.0 + 5.0 * 40.0) / (9.0 * 255.0));

			// Naive unmasked baseline on the exact same pixels: subtitle bar contaminates peak, highlight, and APL!
			const SourceStatsDisplay::DisplayRecord naiveUnmasked =
				SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleRgb, baseParams);
			Assert::AreEqual(64u * 64u, naiveUnmasked.blocks);
			Assert::AreEqual(analyticalSubCode, naiveUnmasked.maximum, 0.25,
				L"Negative control: naive unmasked baseline must be contaminated by the 230 subtitle bar peak");
			Assert::IsTrue(naiveUnmasked.maximum > analyticalSpecBoxCode + 60.0,
				L"Negative control: subtitle peak must dominate the 2x2 specular when unmasked");
			Assert::IsTrue(naiveUnmasked.apl > analyticalBgCode + 5.0,
				L"Negative control: subtitle bar must visibly inflate unmasked APL");

			// Masked measurement on the EXACT same pixels: recovers analytical specular peak and dim picture APL.
			const PixelRect subtitleMask{ 36.0, 156.0, 154.0, 180.0 };
			SourceStatsDisplay::MeasureParams maskedParams = baseParams;
			maskedParams.masks = &subtitleMask;
			maskedParams.maskCount = 1;

			const SourceStatsDisplay::DisplayRecord masked =
				SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleRgb, maskedParams);
			Assert::IsTrue(masked.blocks > 0 && masked.blocks < naiveUnmasked.blocks,
				L"Masked measurement must exclude subtitle blocks");
			Assert::AreEqual(analyticalSpecBoxCode, masked.maximum, 0.25,
				L"Masked measurement must recover exact analytical 2x2 specular box peak");
			// Picture APL: 1 block at specular box + (blocks - 1) blocks at background.
			const double analyticalMaskedApl =
				(analyticalSpecBoxCode + static_cast<double>(masked.blocks - 1u) * analyticalBgCode) /
				static_cast<double>(masked.blocks);
			Assert::AreEqual(analyticalMaskedApl, masked.apl, 0.25,
				L"Masked measurement must recover exact analytical picture APL");
			Assert::IsTrue(masked.apl < naiveUnmasked.apl - 5.0,
				L"Masked APL must be strictly below subtitle-contaminated baseline");
			Assert::AreEqual(std::floor(analyticalBgCode), masked.highlight, 1.0,
				L"Masked highlight percentile must see picture level, not subtitle bar");
		}

		// Negative control 2 (gamma boost hazard): mid-grey field (0.35 linear) with boost = 2.2 / 2.4.
		TEST_METHOD(MeasureDisplayFrameGammaBoostMatchesAnalyticalLaw)
		{
			constexpr int kSize = 96;
			constexpr double kLinearGrey = 0.35;
			constexpr double kBoost = 2.2 / 2.4;

			auto sampleGrey = [](int, int, double& r, double& g, double& b) -> bool
			{
				r = g = b = kLinearGrey;
				return true;
			};

			const double analyticalBoosted = 16.0 + 219.0 * std::pow(kLinearGrey, kBoost);
			const double analyticalUnboosted = 16.0 + 219.0 * kLinearGrey;

			SourceStatsDisplay::MeasureParams boostedParams;
			boostedParams.boost = kBoost;
			const SourceStatsDisplay::DisplayRecord boosted =
				SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleGrey, boostedParams);

			// Negative control on the EXACT same pixels with boost = 1.0.
			SourceStatsDisplay::MeasureParams unboostedParams;
			unboostedParams.boost = 1.0;
			const SourceStatsDisplay::DisplayRecord unboosted =
				SourceStatsDisplay::MeasureDisplayFrame(kSize, kSize, sampleGrey, unboostedParams);

			Assert::AreEqual(analyticalBoosted, boosted.apl, 0.1,
				L"Boosted MeasureDisplayFrame APL must match analytical 16 + 219 * pow(0.35, 2.2/2.4)");
			Assert::AreEqual(analyticalBoosted, boosted.maximum, 0.1,
				L"Boosted MeasureDisplayFrame maximum must match analytical boosted code");
			Assert::AreEqual(analyticalUnboosted, unboosted.apl, 0.1,
				L"Unboosted baseline APL must match 16 + 219 * 0.35");
			Assert::IsTrue(std::fabs(boosted.apl - unboosted.apl) > 5.0,
				L"Negative control: unboosted baseline must diverge from boosted measurement by > 5 codes");
		}
	};
}
