// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P10 D5 phase D1 (12-bit, owner ruling 2026-11-12): DvTunnelTapLaw carries the full 12-bit intensity,
// bit-exact with DolbyVisionTunnelUnpackReference for every container x alignment x range and all 4096
// values in both pair positions, plus random frames. The legacy ten-bit CPU tap (AnalysisLumaSource,
// PackedDolbyVision422) stays provable: its result is the 12-bit result >> 2. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		const VideoFrameEncoding kContainers[] = { VideoFrameEncoding::R210, VideoFrameEncoding::R10b, VideoFrameEncoding::R10l };

		inline uint32_t NextRandom(uint32_t& seed)
		{
			seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
			return seed;
		}

		inline void StoreWord(uint8_t* pixel, uint32_t word, bool bigEndian)
		{
			for (int i = 0; i < 4; ++i)
				pixel[i] = static_cast<uint8_t>(word >> (bigEndian ? 24 - 8 * i : 8 * i));
		}

		// A word whose 12-bit intensity is i12: G8 = I[11:4] and B's low nibble = I[3:0] land at p bits
		// (10 + s .. 17 + s) and (s .. s + 3); every other bit (chroma, padding) is garbage.
		inline uint32_t WordWithIntensity(uint32_t i12, uint32_t s, uint32_t& seed)
		{
			const uint32_t fields = (((i12 >> 4) & 0xFFu) << (10 + s)) | ((i12 & 0xFu) << s);
			const uint32_t mask = (0xFFu << (10 + s)) | (0xFu << s);
			return fields | (NextRandom(seed) & ~mask);
		}

		inline AnalysisLumaSource Source(const std::vector<uint8_t>& data, int width, int height, size_t rowBytes,
			VideoFrameEncoding container, int alignment, bool fullRange)
		{
			AnalysisLumaSource s;
			s.data = data.data();
			s.dataBytes = data.size();
			s.width = width;
			s.height = height;
			s.rowBytes = rowBytes;
			s.format = AnalysisLumaFormat::PackedDolbyVision422;
			s.encoding = container;
			s.rgbBitAlignment = static_cast<uint8_t>(alignment);
			s.fullRangeYuv = fullRange;
			return s;
		}

		inline std::wstring Name(VideoFrameEncoding container, int alignment, bool fullRange)
		{
			const wchar_t* c = container == VideoFrameEncoding::R210 ? L"r210" :
				container == VideoFrameEncoding::R10b ? L"R10b" : L"R10l";
			return std::wstring(c) + (alignment == 1 ? L" high" : L" low") + (fullRange ? L" full" : L" limited");
		}
	}

	TEST_CLASS(DvTunnelTapLawTests)
	{
	public:
		TEST_METHOD(EveryIntensityInEveryContainerAlignmentAndRangeEqualsTheUnpackReference)
		{
			namespace R = DolbyVisionTunnelUnpackReference;
			Assert::AreEqual(12, DvTunnelTapLaw::kBits);
			uint32_t seed = 0x2545f491u;
			for (VideoFrameEncoding container : kContainers)
				for (int alignment = 1; alignment <= 2; ++alignment)
					for (int range = 0; range < 2; ++range)
					{
						const bool full = range == 1;
						const bool bigEndian = R::ContainerBigEndian(container);
						const uint32_t s = R::ContainerShift(container, alignment == 1);
						const std::wstring what = Name(container, alignment, full);
						for (int position = 0; position < 2; ++position)
						{
							const size_t rowBytes = 12; // 8 B of pixels + 4 B padding
							std::vector<uint8_t> data(rowBytes * 4096, 0xA5);
							for (int i12 = 0; i12 < 4096; ++i12)
							{
								uint8_t* row = &data[static_cast<size_t>(i12) * rowBytes];
								StoreWord(row + position * 4, WordWithIntensity(static_cast<uint32_t>(i12), s, seed), bigEndian);
								StoreWord(row + (1 - position) * 4, NextRandom(seed), bigEndian);
							}
							for (int i12 = 0; i12 < 4096; ++i12)
							{
								const uint8_t* pixel = &data[static_cast<size_t>(i12) * rowBytes + position * 4];
								// The reference's own decode (word -> Intensity), 12 bits.
								const uint32_t word = R::Word(pixel, bigEndian);
								const int reference = static_cast<int>(R::Intensity(word, s) >> 4);
								Assert::AreEqual(i12, reference, what.c_str());
								Assert::AreEqual(i12, static_cast<int>(DvTunnelTapLaw::Code12(pixel, container, alignment)), what.c_str());
								const int expected = full ? static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(i12)) : i12;
								const int tap = DvTunnelTapLaw::TapCode(pixel, container, alignment, full);
								Assert::AreEqual(expected, tap, what.c_str());
								Assert::AreEqual(tap << 4, static_cast<int>(DvTunnelTapLaw::Tap16(pixel, container, alignment, full)), what.c_str());
								Assert::IsTrue(tap >= 0 && tap < DvTunnelTapLaw::kLevels, what.c_str());
							}
						}
					}
		}

		TEST_METHOD(TheLegacyTenBitTapIsTheTwelveBitTapShiftedRightByTwo)
		{
			// The old path stays provable against the real CPU tap (AnalysisLumaSource): limited range is exactly
			// tap12 >> 2; full range maps at another precision, so it agrees within one ten-bit code.
			uint32_t seed = 0x51ed270bu;
			for (VideoFrameEncoding container : kContainers)
				for (int alignment = 1; alignment <= 2; ++alignment)
					for (int range = 0; range < 2; ++range)
					{
						const bool full = range == 1;
						const bool bigEndian = DolbyVisionTunnelUnpackReference::ContainerBigEndian(container);
						const uint32_t s = DolbyVisionTunnelUnpackReference::ContainerShift(container, alignment == 1);
						const std::wstring what = Name(container, alignment, full);
						const size_t rowBytes = 12;
						std::vector<uint8_t> data(rowBytes * 4096, 0);
						for (int i12 = 0; i12 < 4096; ++i12)
						{
							uint8_t* row = &data[static_cast<size_t>(i12) * rowBytes];
							StoreWord(row, WordWithIntensity(static_cast<uint32_t>(i12), s, seed), bigEndian);
							StoreWord(row + 4, NextRandom(seed), bigEndian);
						}
						const AnalysisLumaSource source = Source(data, 2, 4096, rowBytes, container, alignment, full);
						Assert::IsTrue(source.IsValid(), what.c_str());
						for (int i12 = 0; i12 < 4096; ++i12)
						{
							AnalysisLumaSample cpu;
							Assert::IsTrue(source.Sample(0, i12, cpu), what.c_str());
							const uint8_t* pixel = &data[static_cast<size_t>(i12) * rowBytes];
							const int legacy = DvTunnelTapLaw::Legacy10(DvTunnelTapLaw::TapCode(pixel, container, alignment, full));
							if (!full)
								Assert::AreEqual(static_cast<int>(cpu.luma), legacy, what.c_str());
							else
							{
								const int diff = static_cast<int>(cpu.luma) - legacy;
								Assert::IsTrue(diff >= -1 && diff <= 1, what.c_str());
							}
						}
					}
		}

		TEST_METHOD(RandomPaddedFramesEqualTheUnpackReferenceEverywhere)
		{
			namespace R = DolbyVisionTunnelUnpackReference;
			uint32_t seed = 0x9e3779b9u;
			for (VideoFrameEncoding container : kContainers)
				for (int alignment = 1; alignment <= 2; ++alignment)
					for (int range = 0; range < 2; ++range)
						for (int width : { 2, 6, 64, 130 })
						{
							const bool full = range == 1;
							const int height = 7;
							const size_t rowBytes = static_cast<size_t>(width) * 4 + 28;
							std::vector<uint8_t> data(rowBytes * height);
							for (uint8_t& b : data)
								b = static_cast<uint8_t>(NextRandom(seed));
							const std::wstring what = Name(container, alignment, full);
							const bool bigEndian = R::ContainerBigEndian(container);
							const uint32_t s = R::ContainerShift(container, alignment == 1);
							for (int y = 0; y < height; ++y)
								for (int x = 0; x < width; ++x)
								{
									const uint8_t* pixel = &data[static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4];
									const int i12 = static_cast<int>(R::Intensity(R::Word(pixel, bigEndian), s) >> 4);
									const int expected = full ? static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(i12)) : i12;
									Assert::AreEqual(expected, static_cast<int>(DvTunnelTapLaw::TapCode(pixel, container, alignment, full)), what.c_str());
								}
						}
		}

		TEST_METHOD(TheRangeTableIsTheTwelveBitMapAndLimitedIsTheIdentity)
		{
			static uint16_t table[DvTunnelTapLaw::kLevels];
			DvTunnelTapLaw::BuildRangeTable(table);
			int previous = -1;
			for (int code = 0; code < DvTunnelTapLaw::kLevels; ++code)
			{
				Assert::AreEqual(static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(code)), static_cast<int>(table[code]));
				Assert::AreEqual(static_cast<int>(table[code]), static_cast<int>(DvTunnelTapLaw::ToLimited(static_cast<uint16_t>(code), true)));
				Assert::AreEqual(code, static_cast<int>(DvTunnelTapLaw::ToLimited(static_cast<uint16_t>(code), false)));
				Assert::IsTrue(static_cast<int>(table[code]) >= previous, L"table must be monotonic");
				previous = table[code];
			}
			Assert::AreEqual(256, static_cast<int>(table[0]));
			Assert::AreEqual(3760, static_cast<int>(table[4095]));
			// The studio endpoints agree with the ten-bit map's (64 / 940, times four).
			Assert::AreEqual(static_cast<int>(Vpv::AnalysisDomain::FullLumaToLimited(0, 1)) * 4, static_cast<int>(table[0]));
			Assert::AreEqual(static_cast<int>(Vpv::AnalysisDomain::FullLumaToLimited(1023, 1)) * 4, static_cast<int>(table[4095]));
		}

		TEST_METHOD(SupportsMirrorsTheCpuTapAcceptanceRules)
		{
			Assert::IsTrue(DvTunnelTapLaw::Supports(VideoFrameEncoding::R210, 1, 1920));
			Assert::IsTrue(DvTunnelTapLaw::Supports(VideoFrameEncoding::R10b, 2, 3840));
			Assert::IsTrue(DvTunnelTapLaw::Supports(VideoFrameEncoding::R10l, 1, 2));
			Assert::IsFalse(DvTunnelTapLaw::Supports(VideoFrameEncoding::R12B, 1, 1920));
			Assert::IsFalse(DvTunnelTapLaw::Supports(VideoFrameEncoding::R12L, 2, 1920));
			Assert::IsFalse(DvTunnelTapLaw::Supports(VideoFrameEncoding::R210, 0, 1920));
			Assert::IsFalse(DvTunnelTapLaw::Supports(VideoFrameEncoding::R210, 3, 1920));
			Assert::IsFalse(DvTunnelTapLaw::Supports(VideoFrameEncoding::R210, 1, 1921));
			// The same verdicts as AnalysisLumaSource::IsValid for the geometry it checks.
			std::vector<uint8_t> data(64 * 4, 0);
			for (VideoFrameEncoding container : { VideoFrameEncoding::R210, VideoFrameEncoding::R12B })
				for (int alignment = 0; alignment <= 3; ++alignment)
					for (int width : { 2, 3, 8 })
						Assert::AreEqual(DvTunnelTapLaw::Supports(container, alignment, width),
							Source(data, width, 4, static_cast<size_t>(width) * 4, container, alignment, false).IsValid());
		}
	};
}
