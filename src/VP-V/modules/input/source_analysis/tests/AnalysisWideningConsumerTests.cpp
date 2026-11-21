/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Slice 21 W21-2: the CPU analysis consumers on the 12-bit tap. Reduce (histogram bins on the 1024-code axis, Q6 mean / min / max /
// percentile companions), the source-resolution look-ahead's normalisation by the sampler's code range, the CodingEvidence step-derived
// constants (R1/R2) and the LED value table read at a Q6 value. 10-bit (and lower) sources are bit-identical by construction: the
// scale factors are exactly 1 there. New classes in a new file (rule 28: no existing test, tolerance or baseline is edited). CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/CodedLumaSampler.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/regions/CodingEvidence.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AnalysisWideningConsumerTestsNs
{
	using namespace SyntheticAnalysisFrames;
	namespace Axis = Vpv::AnalysisDomain;

	// 4096 r210 words (alignment 2, big endian): pixel `code` carries intensity code, I = hi8 << 4 | lo4. Limited range, so the tap is the code.
	struct Ramp
	{
		std::vector<uint8_t> bytes;
		AnalysisLumaSource source;
		Ramp()
		{
			const int width = 64, height = 64;
			bytes.assign(static_cast<size_t>(width) * height * 4, 0);
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const uint32_t word = ((code >> 4) << 10) | (code & 15u);
				for (unsigned k = 0; k < 4; ++k) bytes[code * 4 + k] = static_cast<uint8_t>(word >> (24 - 8 * k));
			}
			source.format = AnalysisLumaFormat::PackedDolbyVision422;
			source.encoding = VideoFrameEncoding::R210;
			source.width = width;
			source.height = height;
			source.rgbBitAlignment = 2;
			source.fullRangeYuv = false;
			source.data = bytes.data();
			source.dataBytes = bytes.size();
			source.rowBytes = static_cast<size_t>(width) * 4;
		}
	};

	// One 64 x 64 tile of banded content: every 8 x 8 block holds two levels `step` axis codes apart; block bases climb 16 axis codes per
	// block. `bits` 10 writes the axis codes; 12 writes the same tile at four times the codes (the same content on the deeper axis).
	std::vector<float> BandedTile(int bits)
	{
		const int n = 64, scale = bits == 12 ? 4 : 1;
		const float maximum = bits == 12 ? 4095.0f : 1023.0f;
		std::vector<float> tile(static_cast<size_t>(n) * n);
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const int base = 200 + 16 * ((x / 8) + (y / 8));
				const int code10 = base + (x % 8 >= 4 ? 2 : 0);
				tile[static_cast<size_t>(y) * n + x] = static_cast<float>(code10 * scale) / maximum;
			}
		return tile;
	}

	// The same tile geometry with a chosen native step between the two levels of every block (step native codes = step / scale axis codes).
	std::vector<float> StepTile(int bits, int stepNative)
	{
		const int n = 64, scale = bits == 12 ? 4 : 1;
		const float maximum = bits == 12 ? 4095.0f : 1023.0f;
		std::vector<float> tile(static_cast<size_t>(n) * n);
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const int base = (200 + 16 * ((x / 8) + (y / 8))) * scale;
				tile[static_cast<size_t>(y) * n + x] = static_cast<float>(base + (x % 8 >= 4 ? stepNative : 0)) / maximum;
			}
		return tile;
	}

	TEST_CLASS(AnalysisWideningConsumerTests)
	{
	public:
		// Frozen literal oracle (not derived through the production path): a P010 frame whose luma is 64 + 8 * (x mod 16), 16 levels of
		// 24 * 216 samples each. Reduce must read exactly these literals.
		TEST_METHOD(TenBitFrozenLiteralStats)
		{
			Frame frame = Make(Kind::P010, 0);
			std::vector<uint8_t>& data = *frame.bytes;
			for (int y = 0; y < kHeight; ++y)
				for (int x = 0; x < kWidth; ++x)
					Put16(&data[static_cast<size_t>(y) * frame.source.rowBytes + static_cast<size_t>(x) * 2],
						static_cast<uint16_t>((64 + 8 * (x % 16)) << 6));
			VpFrameAnalysis::Measured m;
			Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, FrameAnalysis::Rect{ 0, 0, kWidth, kHeight }, m));
			Assert::AreEqual<uint64_t>(82944, m.luma.samples);
			for (int bin = 0; bin < 1024; ++bin)
			{
				const bool used = bin >= 64 && bin <= 184 && ((bin - 64) % 8) == 0;
				Assert::AreEqual<uint32_t>(used ? 5184u : 0u, m.luma.histogram[static_cast<size_t>(bin)]);
			}
			Assert::AreEqual<int>(64, m.luma.minimum);
			Assert::AreEqual<int>(184, m.luma.maximum);
			Assert::AreEqual(124.0, m.luma.mean, 1e-12);
			Assert::AreEqual(60.0 / 876.0, m.luma.apl, 1e-12);
			Assert::AreEqual<uint32_t>(64u << 6, m.luma.minQ6);
			Assert::AreEqual<uint32_t>(184u << 6, m.luma.maxQ6);
			Assert::AreEqual(124.0 * 64.0, m.luma.meanQ6, 1e-9);
		}

		// The discriminating thresholds: a band is a 2..8 AXIS-code step. At 12 bit one axis code is 4 native codes: a 4-native step is
		// NOT a band, an 8-native (2 axis code) step IS; the 10-bit tile reads the same (1 code no, 2 codes yes).
		TEST_METHOD(TwelveBitStepThresholdsAreAxisCodes)
		{
			const int n = 64;
			const auto edges = [n](int bits, int step) {
				const std::vector<float> t = StepTile(bits, step);
				return VpUnit::CodingEvidence::MeasureTile(t.data(), n, n, bits == 12 ? 4095 : 1023).bandEdges;
			};
			Assert::AreEqual(0, edges(10, 1), L"one axis code is no band at 10 bits");
			Assert::AreEqual(64, edges(10, 2), L"two axis codes are a band at 10 bits");
			Assert::AreEqual(0, edges(12, 4), L"one axis code (4 native) is no band at 12 bits");
			Assert::AreEqual(64, edges(12, 8), L"two axis codes (8 native) are a band at 12 bits");
			Assert::AreEqual(64, edges(12, 32), L"eight axis codes (32 native) are still a band");
			Assert::AreEqual(0, edges(12, 36), L"nine axis codes are above the band range");
		}

		// The DV ramp through Reduce: the 12-bit view Reduce reads is the one whose tap codes use all 4096 values (12 effective bits),
		// the 1024-bin axis histogram Reduce publishes reads 10, and its Q6 extremes are that view's own codes << 4.
		TEST_METHOD(EffectiveBitsThroughReduceOnDvRamp)
		{
			Ramp ramp;
			VpSkeleton::SourceFrameView view;
			Assert::IsTrue(VpFrameAnalysis::SourceViewOf12(ramp.source, view));
			VpFrameAnalysis::Measured m;
			Assert::IsTrue(VpFrameAnalysis::Reduce(ramp.source, FrameAnalysis::Rect{ 0, 0, 64, 64 }, m, &view));
			std::vector<unsigned long long> h12(4096, 0), axis(m.luma.histogram.begin(), m.luma.histogram.end());
			uint16_t lo = 0xffff, hi = 0;
			for (uint32_t i = 0; i < 4096u; ++i)
			{
				++h12[view.luma[i]];
				if (view.luma[i] < lo) lo = view.luma[i];
				if (view.luma[i] > hi) hi = view.luma[i];
			}
			Assert::AreEqual(12.0, VpUnit::CodingEvidence::EffectiveBitsOf(h12), 1e-9);
			Assert::AreEqual(10.0, VpUnit::CodingEvidence::EffectiveBitsOf(axis), 1e-9);
			Assert::AreEqual<uint32_t>(static_cast<uint32_t>(lo) << 4, m.luma.minQ6);
			Assert::AreEqual<uint32_t>(static_cast<uint32_t>(hi) << 4, m.luma.maxQ6);
		}

		// Every non-DV synthetic source: Reduce equals the frozen former path (Reducer::AddRow over LumaRowReader rows) and its Q6
		// companions are exactly code * 64 (an integer-code source has no sub-code part).
		TEST_METHOD(TenBitSourcesBitIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (kind == Kind::PackedDvR210) continue;
				Frame frame = Make(kind, 3);
				const FrameAnalysis::Rect picture = { 0, 0, kWidth, kHeight };
				VpFrameAnalysis::Measured want, got;
				{
					VpFrameAnalysis::Reducer reducer(picture, VpFrameAnalysis::StudioLumaMap(frame.source));
					VpFrameAnalysis::LumaRowReader reader(frame.source);
					for (int y = 0; y < kHeight; ++y)
					{
						const uint16_t* row = reader.Row(y, 0, kWidth);
						Assert::IsNotNull(row, KindName(kind));
						reducer.AddRow(y, row);
					}
					want = reducer.Finish();
				}
				Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, picture, got), KindName(kind));
				Assert::IsTrue(want.luma.histogram == got.luma.histogram, KindName(kind));
				Assert::IsTrue(want.luma.mean == got.luma.mean, KindName(kind));
				Assert::IsTrue(want.luma.apl == got.luma.apl, KindName(kind));
				Assert::IsTrue(want.luma.minimum == got.luma.minimum && want.luma.maximum == got.luma.maximum, KindName(kind));
				Assert::IsTrue(want.luma.p1 == got.luma.p1 && want.luma.p50 == got.luma.p50 && want.luma.p99 == got.luma.p99, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(got.luma.minimum) << 6, got.luma.minQ6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(got.luma.maximum) << 6, got.luma.maxQ6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(got.luma.p1) << 6, got.luma.p1Q6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(got.luma.p50) << 6, got.luma.p50Q6, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(got.luma.p99) << 6, got.luma.p99Q6, KindName(kind));
				Assert::IsTrue(got.luma.meanQ6 == got.luma.mean * 64.0, KindName(kind));
			}
			// The look-ahead's normalisation at 1023 is exactly the former `code / 1023.0`.
			for (int code = 0; code <= 1023; ++code)
				Assert::IsTrue(SourceResolutionMonitor::NormalisedLuma(code, 1023) == static_cast<double>(code) / 1023.0);
		}

		// The 12-bit DV ramp (one pixel per tap code, 4096 codes): the
		// histogram is one bin per whole axis code (four tap codes each), the Q6 mean is the exact sub-code mean, min / max are exact,
		// and the percentiles rise with the picture.
		TEST_METHOD(TwelveBitRampMonotonic)
		{
			Ramp ramp;
			VpFrameAnalysis::Measured m;
			Assert::IsTrue(VpFrameAnalysis::Reduce(ramp.source, FrameAnalysis::Rect{ 0, 0, 64, 64 }, m));
			Assert::AreEqual<uint64_t>(4096, m.luma.samples);
			for (int bin = 0; bin < 1024; ++bin)
				Assert::AreEqual<uint32_t>(4, m.luma.histogram[static_cast<size_t>(bin)], L"four tap codes per axis code");
			Assert::AreEqual<uint32_t>(0, m.luma.minQ6);
			Assert::AreEqual<uint32_t>(4095u << 4, m.luma.maxQ6);
			Assert::AreEqual(32760.0, m.luma.meanQ6, 1e-9);                 // (0 + ... + 4095) / 4096 = 2047.5 tap codes, x 16
			Assert::AreEqual(32760.0 / 64.0, m.luma.mean, 1e-9);            // the axis-code mean carries the sub-code part
			Assert::AreEqual((32760.0 / 64.0 - 64.0) / 876.0, m.luma.apl, 1e-9);
			Assert::IsTrue(m.luma.minQ6 <= m.luma.p1Q6 && m.luma.p1Q6 <= m.luma.p50Q6 && m.luma.p50Q6 <= m.luma.p99Q6 &&
				m.luma.p99Q6 <= m.luma.maxQ6);
			Assert::IsTrue(std::abs(static_cast<int>(m.luma.p1) - 10) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(m.luma.p50) - 511) <= 1);
			Assert::IsTrue(std::abs(static_cast<int>(m.luma.p99) - 1013) <= 1);
			// Each percentile's Q6 value is the centre of the bin it falls in.
			Assert::AreEqual<uint32_t>(Axis::BinCenterQ6(m.luma.p50), m.luma.p50Q6);

			// The look-ahead's normalisation: monotone across the whole 12-bit range, white = 1.
			double last = -1.0;
			for (int code = 0; code <= 4095; ++code)
			{
				const double v = SourceResolutionMonitor::NormalisedLuma(code, 4095);
				Assert::IsTrue(v > last);
				last = v;
			}
			Assert::AreEqual(1.0, SourceResolutionMonitor::NormalisedLuma(4095, 4095), 1e-15);

			// CodingEvidence: a tile that grows tap code by tap code reads a monotone gradient.
			const int n = 64;
			double lastGradient = -1.0;
			for (int slope = 1; slope <= 4; ++slope)
			{
				std::vector<float> tile(static_cast<size_t>(n) * n);
				for (int y = 0; y < n; ++y)
					for (int x = 0; x < n; ++x)
						tile[static_cast<size_t>(y) * n + x] = static_cast<float>(1000 + slope * x) / 4095.0f;
				const double g = VpUnit::CodingEvidence::MeasureTile(tile.data(), n, n, 4095).gradient;
				Assert::IsTrue(g > lastGradient);
				lastGradient = g;
			}
		}

		// R1/R2: the same banded content written at 10-bit and at four times the codes at 12-bit reads the same blocks, the same band
		// edges (minimum 2 / maximum 8 axis codes = 8 / 32 native) and, scaled by four, the same gradient; the blockiness ratio is equal
		// (the quarter-code floor scales with the codes). Nothing about 10-bit moves: the scale is exactly 1 there.
		TEST_METHOD(TwelveBitScaleKeepsAxisMeaning)
		{
			const int n = 64;
			const std::vector<float> t10 = BandedTile(10), t12 = BandedTile(12);
			const VpUnit::CodingEvidence::TileMeasure a = VpUnit::CodingEvidence::MeasureTile(t10.data(), n, n, 1023);
			const VpUnit::CodingEvidence::TileMeasure b = VpUnit::CodingEvidence::MeasureTile(t12.data(), n, n, 4095);
			Assert::AreEqual(64, a.blocks);
			Assert::AreEqual(a.blocks, b.blocks);
			Assert::AreEqual(64, a.bandEdges, L"every block is a two-level band edge");
			Assert::AreEqual(a.bandEdges, b.bandEdges);
			Assert::AreEqual(a.flatBlocks, b.flatBlocks);
			Assert::AreEqual(a.bandEdgesRobust, b.bandEdgesRobust);
			Assert::AreEqual(a.gradient * 4.0, b.gradient, 1e-9);
			Assert::AreEqual(a.blockiness, b.blockiness, 1e-9);
		}

		// EffectiveBitsReportsTwelveOnDvRamp: a DV ramp whose every 12-bit code is used equally reads 12 effective bits from its tap
		// histogram and 10 from the same frame's ten-bit floor; a ramp that only uses every 16th tap code (an 8-bit source) reads 8.
		TEST_METHOD(EffectiveBitsReportsTwelveOnDvRamp)
		{
			Ramp ramp;
			VpSkeleton::SourceFrameView view;
			bool refused = true;
			Assert::IsTrue(VpFrameAnalysis::TapViewOf(ramp.source, view, &refused));
			Assert::IsFalse(refused);
			Assert::AreEqual<int>(4, view.msbShift);
			std::vector<unsigned long long> h12(4096, 0), h10(1024, 0), h8(4096, 0);
			for (int repeat = 0; repeat < 64; ++repeat)
				for (uint32_t i = 0; i < 4096u; ++i)
				{
					const uint32_t code = view.luma[i];
					++h12[code];
					++h10[Axis::Floor10FromQ6(view.MsbAlign(view.luma[i]))];
					if ((code & 15u) == 0) h8[code] += 16;
				}
			Assert::AreEqual(12.0, VpUnit::CodingEvidence::EffectiveBitsOf(h12), 1e-9);
			Assert::AreEqual(10.0, VpUnit::CodingEvidence::EffectiveBitsOf(h10), 1e-9);
			Assert::AreEqual(8.0, VpUnit::CodingEvidence::EffectiveBitsOf(h8), 1e-9);
		}
	};

	TEST_CLASS(LedWideTableTests)
	{
	public:
		// LedInterpolationIntegerCodesUnchanged: the table read at an integer axis code (Q6 = code * 64, or a 12-bit code that is a multiple
		// of four) is the table's own entry bit for bit; the 10-bit lane (shift 6) reproduces the table; a 12-bit code between two whole
		// codes lands between their entries, monotone when the table is.
		TEST_METHOD(LedInterpolationIntegerCodesUnchanged)
		{
			LedDrive::LumaValueTable table{};
			for (int code = 0; code < LedDrive::LUMA_CODES; ++code)
				table[static_cast<size_t>(code)] = static_cast<float>(std::pow(code / 1023.0, 1.7));
			const std::vector<float> lane10 = LedDrive::WideValueTable(table, 1023, 6);
			Assert::AreEqual<size_t>(1024, lane10.size());
			for (int code = 0; code < 1024; ++code)
			{
				Assert::IsTrue(lane10[static_cast<size_t>(code)] == table[static_cast<size_t>(code)]);
				Assert::IsTrue(LedDrive::TableValueQ6(table, static_cast<uint32_t>(code) << 6) == table[static_cast<size_t>(code)]);
			}
			const std::vector<float> tap12 = LedDrive::WideValueTable(table, 4095, 4);
			Assert::AreEqual<size_t>(4096, tap12.size());
			for (int code12 = 0; code12 < 4096; ++code12)
			{
				const float v = tap12[static_cast<size_t>(code12)];
				const int whole = code12 >> 2;
				if ((code12 & 3) == 0)
					Assert::IsTrue(v == table[static_cast<size_t>(whole)]);
				else
					Assert::IsTrue(v >= table[static_cast<size_t>(whole)] && v <= table[static_cast<size_t>(whole + 1)]);
				if (code12 > 0)
					Assert::IsTrue(v >= tap12[static_cast<size_t>(code12 - 1)]);
			}
			// Past the last whole code the last entry is held.
			Assert::IsTrue(LedDrive::TableValueQ6(table, Axis::kAxisQ6Max + 63u) == table[1023]);
		}
	};
}
