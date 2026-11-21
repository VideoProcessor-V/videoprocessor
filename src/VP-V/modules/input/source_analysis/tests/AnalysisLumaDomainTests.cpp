/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// S-2a W1: the shared analysis-luma decode unit (AnalysisAxis10.h, re-exported by AnalysisLumaDomain.h). CPU only.
// 12-bit rides as Q6 fractions on the 10-bit axis (coordinator 2026-11-14 08:40, Q1 a): 10-bit sources stay integers
// on the axis (bit/decision identical), a 12-bit ramp is monotonic over all 4096 codes. TapCarriesTwelveBits pins
// DvTunnelTapLaw's tap to the same axis (a separate class: DvTunnelTapLawTests is not edited, rule 28).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Vpv::AnalysisDomain;

namespace Tests
{
	TEST_CLASS(AnalysisLumaDomainTests)
	{
	public:
		TEST_METHOD(DecodeIsExactForEveryFormat)
		{
			// Shift law: every depth 8..12 is code << (16 - bits), exactly, and its whole axis code (q6 / 64) is the legacy value.
			for (int bits = 8; bits <= 12; ++bits)
			{
				const uint32_t maximum = CodeMaximum(bits);
				Assert::AreEqual(maximum, (1u << bits) - 1u);
				Assert::AreEqual(bits, BitsOf(maximum));
				for (uint32_t code = 0; code <= maximum; ++code)
				{
					const uint32_t q6 = AxisQ6(code, maximum, AxisLaw::Shift);
					Assert::AreEqual(code << (16 - bits), q6);
					Assert::AreEqual(bits >= 10 ? code >> (bits - 10) : code << (10 - bits), q6 / 64u);
				}
			}
			// Scale law, 12-bit RGB: within half a ten-bit code of Bill's integer Round12To10 for every code.
			for (uint32_t code = 0; code <= 4095; ++code)
			{
				const int64_t q6 = AxisQ6(code, 4095, AxisLaw::Scale);
				const int64_t legacy = static_cast<int64_t>(Round12To10(static_cast<uint16_t>(code))) * 64;
				const int64_t gap = q6 > legacy ? q6 - legacy : legacy - q6;
				Assert::IsTrue(gap <= 33);
				// Exact law, independent arithmetic: round(code * 65472 / 4095) in Q6.
				Assert::AreEqual(static_cast<int64_t>((static_cast<uint64_t>(code) * 65472ull + 2047ull) / 4095ull), q6);
			}
			// The GLSL twin carries the same constant as the CPU law.
			const std::string glsl = AxisGlsl();
			Assert::IsTrue(glsl.find("65472") != std::string::npos);
			Assert::AreEqual(65472u, kAxisQ6Max);
		}

		TEST_METHOD(TenBitSourcesStayIntegerOnTheAxis)
		{
			for (uint32_t code = 0; code <= 1023; ++code)
			{
				for (int law = 0; law < 2; ++law)
				{
					const uint32_t q6 = AxisQ6(code, 1023, law ? AxisLaw::Scale : AxisLaw::Shift);
					Assert::AreEqual(code * 64u, q6);
					Assert::AreEqual(code, q6 / 64u);
					Assert::AreEqual(0u, q6 % 64u, L"a ten-bit code carries no fraction on the axis");
					Assert::AreEqual(static_cast<float>(code), ToAxis10(code, 1023, law ? AxisLaw::Scale : AxisLaw::Shift));
				}
			}
			// The Q6 centre of a whole-code bin is `(bin << 6) | 32`.
			for (uint32_t bin = 0; bin < kAxisBins; ++bin)
				Assert::AreEqual((bin << 6) | 32u, BinCenterQ6(bin));
		}

		TEST_METHOD(TwelveBitRampMonotonic4096)
		{
			uint32_t previousShift = 0, previousScale = 0;
			for (uint32_t code = 0; code <= 4095; ++code)
			{
				const uint32_t s = AxisQ6(code, 4095, AxisLaw::Shift);
				const uint32_t c = AxisQ6(code, 4095, AxisLaw::Scale);
				if (code > 0)
				{
					Assert::IsTrue(s > previousShift);   // every 12-bit code is distinct on the axis
					Assert::IsTrue(c >= previousScale);
				}
				previousShift = s;
				previousScale = c;
			}
			Assert::AreEqual(0u, AxisQ6(0, 4095, AxisLaw::Scale));
			Assert::AreEqual(kAxisQ6Max, AxisQ6(4095, 4095, AxisLaw::Scale));
			Assert::IsTrue(ToAxis10(4095, 4095, AxisLaw::Shift) <= 1023.99f);
			// 12-bit codes are fractions on the axis: four per whole code.
			Assert::AreEqual(2.5f, ToAxis10(10, 4095, AxisLaw::Shift));
		}
	};

	TEST_CLASS(DvTunnelTapAxisTests)
	{
	public:
		TEST_METHOD(TapCarriesTwelveBits)
		{
			const int width = 64, height = 8;
			const VideoFrameEncoding containers[] = { VideoFrameEncoding::R210, VideoFrameEncoding::R10b, VideoFrameEncoding::R10l };
			bool sawFraction = false;
			size_t samples = 0, fractional = 0;
			for (const VideoFrameEncoding c : containers)
			{
				std::vector<uint8_t> r210(static_cast<size_t>(width) * height * 4), bytes(r210.size());
				uint32_t seed = 777u;
				for (size_t i = 0; i < r210.size(); i += 4)
				{
					seed = seed * 1664525u + 1013904223u;
					const uint32_t w = seed & 0x3FFFFFFFu;
					for (unsigned k = 0; k < 4; ++k) r210[i + k] = static_cast<uint8_t>(w >> (24 - 8 * k));
				}
				DolbyVisionTunnelUnpackReference::RepackR210(r210.data(), bytes.data(), r210.size(), c, 1u);
				for (size_t i = 0; i < bytes.size(); i += 4)
				{
					const uint8_t* pixel = bytes.data() + i;
					for (int fullRange = 0; fullRange < 2; ++fullRange)
					{
						const uint32_t q6 = DvTunnelTapLaw::TapAxisQ6(DvTunnelTapLaw::TapCode(pixel, c, 1, fullRange != 0));
						// Independent expected value: the 12-bit tap code times 16 (literal), not the shared shift.
						Assert::AreEqual(static_cast<uint32_t>(DvTunnelTapLaw::TapCode(pixel, c, 1, fullRange != 0)) * 16u, q6);
						++samples;
						Assert::AreEqual(static_cast<uint32_t>(DvTunnelTapLaw::TapCode(pixel, c, 1, fullRange != 0)) >> 2, q6 / 64u);
						if ((q6 & 63u) != 0u) { sawFraction = true; ++fractional; }
					}
				}
			}
			Assert::IsTrue(sawFraction);   // the tunnel tap is 12 bits, not the old 10
			Assert::IsTrue(fractional * 2 > samples);   // random 12-bit codes: about 3/4 carry a fraction
		}
	};
}
