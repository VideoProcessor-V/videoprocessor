/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// S-2a consumer identity: the consumers that moved onto the shared analysis axis (AnalysisAxis10.h) keep their ten-bit
// results bit for bit (SourceStats histogram and percentiles, LedLookahead's table), the LightLevel fetch carries
// 12-bit fractions on the same axis, and sigma-delta stays 8/10-bit (owner 2026-11-14 08:30: 12-bit is the DV tunnel,
// non-DV stays 10-bit + sigma-delta). CPU only; new file, no existing test edited.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelLut.h>
#include <VP-V/modules/output/dither/SigmaDeltaDomain.h>

#include <AnalysisLumaSource.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(AnalysisAxisConsumerTests)
	{
	public:
		TEST_METHOD(SourceStatsKeepsTheLegacyBinAndCentreLaw)
		{
			// A ten-bit-origin plane (code << 6) and a 12-bit plane (code12 << 4) through ComputeRecord; the histogram and the
			// percentile centres equal the legacy `s >> 6` / `(bin << 6) | 32` computed independently here.
			const uint32_t hw = 64, hh = 32;
			std::vector<uint16_t> half(hw * hh);
			uint32_t seed = 4242u;
			for (int plane = 0; plane < 2; ++plane)
			{
				for (uint16_t& s : half)
				{
					seed = seed * 1664525u + 1013904223u;
					const uint32_t v = (seed >> 8) & (plane == 0 ? 1023u : 4095u);
					s = static_cast<uint16_t>(plane == 0 ? v << 6 : v << 4);
				}
				std::vector<uint8_t> out(VpUnit::SourceStats::kRecordBytes);
				Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), hw, hh, out.data(), out.size()));
				VpUnit::SourceStats::Record rec;
				std::memcpy(&rec, out.data(), sizeof(rec));
				std::vector<uint32_t> bins(1024, 0u);
				for (const uint16_t s : half) ++bins[s >> 6];
				for (uint32_t b = 0; b < 1024; ++b) Assert::AreEqual(bins[b], rec.hist[b]);
				auto legacy = [&](uint64_t target)
				{
					uint64_t run = 0;
					for (uint32_t b = 0; b < 1024; ++b)
					{
						run += bins[b];
						if (run >= target) return (b << 6) | 32u;
					}
					return (1023u << 6) | 32u;
				};
				const uint64_t n = static_cast<uint64_t>(hw) * hh;
				Assert::AreEqual(legacy((n + 99u) / 100u), rec.p01);
				Assert::AreEqual(legacy((n + 1u) / 2u), rec.p50);
				Assert::AreEqual(legacy((n * 99u + 99u) / 100u), rec.p99);
			}
		}

		TEST_METHOD(LedLookaheadTableKeepsTheTenBitMapping)
		{
			const AnalysisLumaSource luma;   // Bill default: codes as read, not remapped
			const std::vector<float> transfer = LedDrive::IdentityRawTransfer();
			const LedDrive::LumaValueTable full = LedDrive::PeakValueTable(luma, transfer, 1);
			const LedDrive::LumaValueTable limited = LedDrive::PeakValueTable(luma, transfer, 0);
			for (int code = 0; code < LedDrive::LUMA_CODES; ++code)
			{
				const double sf = static_cast<double>(code) / 1023.0;
				const int fi = static_cast<int>(std::lround(std::max(0.0, std::min(sf, 1.0)) * (LedDrive::RAW_TRANSFER_SIZE - 1)));
				Assert::AreEqual(transfer[fi], full[code]);
				const double sl = (static_cast<double>(code) - 64.0) / 876.0;
				const int li = static_cast<int>(std::lround(std::max(0.0, std::min(sl, 1.0)) * (LedDrive::RAW_TRANSFER_SIZE - 1)));
				Assert::AreEqual(transfer[li], limited[code]);
			}
		}

		TEST_METHOD(LightLevelFetchCarriesTwelveBitFractions)
		{
			// A ramp LUT (J = code): an integer axis value returns the code, a 12-bit Q6 axis value its fraction.
			std::vector<float> texels(static_cast<size_t>(LightLevel::kLutSize) * 4u, 0.0f);
			for (int i = 0; i < LightLevel::kLutSize; ++i) texels[static_cast<size_t>(i) * 4u] = static_cast<float>(i);
			for (uint32_t code10 = 0; code10 <= 1023; code10 += 7)
				Assert::AreEqual(static_cast<double>(code10),
					LightLevel::FetchPacked<double>(texels.data(), Vpv::AnalysisDomain::ToAxis10(code10, 1023)).J, 1e-9);
			for (uint32_t code12 = 0; code12 < 4092; code12 += 5)
				Assert::AreEqual(static_cast<double>(code12) / 4.0,
					LightLevel::FetchPacked<double>(texels.data(), Vpv::AnalysisDomain::ToAxis10(code12, 4095)).J, 1e-6);
		}

		TEST_METHOD(SigmaDeltaStaysEightAndTenBit)
		{
			pl_color_repr repr{};
			repr.levels = PL_COLOR_LEVELS_LIMITED;
			repr.bits.sample_depth = 10;
			repr.bits.color_depth = 10;
			Assert::IsTrue(SigmaDeltaDomain::FromTarget(repr).supported);
			repr.bits.sample_depth = 12;
			repr.bits.color_depth = 12;
			Assert::IsFalse(SigmaDeltaDomain::FromTarget(repr).supported);   // 12-bit is the DV tunnel, not sigma-delta
		}
	};
}
