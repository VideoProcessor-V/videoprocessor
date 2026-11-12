/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// refs/116b: the 12-bit DV tunnel analysis tap (DvTunnelTapLaw.h) and VpFrameAnalysis::SourceViewOf12.
// CPU only. Code12 against the unpack reference for the three containers; Legacy10 against the CPU tap
// (AnalysisLumaSource::Sample): exact when limited, within one code when full range; the full-range map's endpoints;
// the GPU range table; SourceTap12 refusals; the SourceViewOf12 view fields.
#include "pch.h"
#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace DvTapLaw
	{
		// A frame of pseudo-random r210 words, rewritten into `container`.
		inline std::vector<uint8_t> Frame(int width, int height, VideoFrameEncoding container)
		{
			std::vector<uint8_t> r210(static_cast<size_t>(width) * height * 4), out(r210.size());
			uint32_t seed = 12345u;
			for (size_t i = 0; i < r210.size(); i += 4)
			{
				seed = seed * 1664525u + 1013904223u;
				const uint32_t w = seed & 0x3FFFFFFFu;
				for (unsigned k = 0; k < 4; ++k) r210[i + k] = static_cast<uint8_t>(w >> (24 - 8 * k));
			}
			DolbyVisionTunnelUnpackReference::RepackR210(r210.data(), out.data(), r210.size(), container, 1u);
			return out;
		}

		inline AnalysisLumaSource Source(const std::vector<uint8_t>& bytes, int width, int height, VideoFrameEncoding container,
			int alignment, bool fullRange)
		{
			AnalysisLumaSource s;
			s.format = AnalysisLumaFormat::PackedDolbyVision422;
			s.encoding = container;
			s.width = width;
			s.height = height;
			s.rgbBitAlignment = alignment;
			s.fullRangeYuv = fullRange;
			s.data = bytes.data();
			s.dataBytes = bytes.size();
			s.rowBytes = static_cast<size_t>(width) * 4;
			return s;
		}
	}

	TEST_CLASS(DvTunnelTapLawTests)
	{
	public:
		TEST_METHOD(Code12MatchesTheUnpackReferenceForAllContainers)
		{
			const VideoFrameEncoding containers[] = { VideoFrameEncoding::R210, VideoFrameEncoding::R10b, VideoFrameEncoding::R10l };
			const int width = 64, height = 16;
			for (VideoFrameEncoding c : containers)
				for (int alignment = 1; alignment <= 2; ++alignment)
				{
					const std::vector<uint8_t> bytes = DvTapLaw::Frame(width, height, c);
					std::vector<uint16_t> ref(static_cast<size_t>(width) * height * 2);
					DolbyVisionTunnelUnpackReference::UnpackFrame(bytes.data(), static_cast<size_t>(width) * 4, width, height, c, alignment == 1, ref.data());
					for (int y = 0; y < height; ++y)
						for (int x = 0; x < width; ++x)
						{
							const uint8_t* p = bytes.data() + (static_cast<size_t>(y) * width + x) * 4;
							Assert::AreEqual(static_cast<int>(ref[static_cast<size_t>(y) * width + x] >> 4),
								static_cast<int>(DvTunnelTapLaw::Code12(p, c, alignment)), L"Code12 == reference intensity >> 4");
						}
				}
		}

		TEST_METHOD(LimitedRangeLegacy10EqualsTheCpuTapLuma)
		{
			const int width = 64, height = 16;
			const std::vector<uint8_t> bytes = DvTapLaw::Frame(width, height, VideoFrameEncoding::R210);
			const AnalysisLumaSource s = DvTapLaw::Source(bytes, width, height, VideoFrameEncoding::R210, 2, false);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					uint16_t tap = 0;
					Assert::IsTrue(DvTunnelTapLaw::SourceTap12(s, x, y, tap), L"tap");
					AnalysisLumaSample sample;
					Assert::IsTrue(s.Sample(x, y, sample), L"cpu sample");
					Assert::AreEqual(static_cast<int>(sample.luma), static_cast<int>(DvTunnelTapLaw::Legacy10(tap)), L"limited: exact");
				}
		}

		// All 4096 12-bit codes through an r210 word (alignment 2, big endian: I = hi8 << 4 | lo4).
		TEST_METHOD(FullRangeLegacy10IsWithinOneCodeOfTheCpuTapForAllCodes)
		{
			const int width = 64, height = 64;   // 4096 pixels
			std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * 4);
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const uint32_t word = ((code >> 4) << 10) | (code & 15u);
				for (unsigned k = 0; k < 4; ++k) bytes[code * 4 + k] = static_cast<uint8_t>(word >> (24 - 8 * k));
			}
			const AnalysisLumaSource s = DvTapLaw::Source(bytes, width, height, VideoFrameEncoding::R210, 2, true);
			int maxDiff = 0;
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const int x = static_cast<int>(code % width), y = static_cast<int>(code / width);
				uint16_t tap = 0;
				Assert::IsTrue(DvTunnelTapLaw::SourceTap12(s, x, y, tap), L"tap");
				Assert::AreEqual(static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(static_cast<int>(code))), static_cast<int>(tap), L"tap uses the map");
				AnalysisLumaSample sample;
				Assert::IsTrue(s.Sample(x, y, sample), L"cpu sample");
				const int diff = std::abs(static_cast<int>(sample.luma) - static_cast<int>(DvTunnelTapLaw::Legacy10(tap)));
				if (diff > maxDiff) maxDiff = diff;
			}
			Assert::IsTrue(maxDiff <= 1, L"full range: Legacy10 within one code of the CPU tap");
		}

		TEST_METHOD(FullLumaToLimited12Endpoints)
		{
			Assert::AreEqual(256, static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(0)), L"black");
			Assert::AreEqual(3760, static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(4095)), L"white");
		}

		TEST_METHOD(RangeTableEqualsTheFunction)
		{
			uint16_t table[DvTunnelTapLaw::kLevels];
			DvTunnelTapLaw::BuildRangeTable(table);
			for (int c = 0; c < DvTunnelTapLaw::kLevels; ++c)
				Assert::AreEqual(static_cast<int>(DvTunnelTapLaw::FullLumaToLimited12(c)), static_cast<int>(table[c]), L"table entry");
		}

		TEST_METHOD(SourceTap12Refusals)
		{
			const int width = 8, height = 4;
			const std::vector<uint8_t> bytes = DvTapLaw::Frame(width, height, VideoFrameEncoding::R210);
			uint16_t tap = 0;
			AnalysisLumaSource s = DvTapLaw::Source(bytes, width, height, VideoFrameEncoding::R210, 2, false);
			Assert::IsTrue(DvTunnelTapLaw::SourceTap12(s, 0, 0, tap), L"valid");
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(s, -1, 0, tap), L"x < 0");
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(s, width, 0, tap), L"x >= width");
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(s, 0, height, tap), L"y >= height");
			AnalysisLumaSource noData = s;
			noData.data = nullptr;
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(noData, 0, 0, tap), L"no data");
			AnalysisLumaSource badAlign = s;
			badAlign.rgbBitAlignment = 0;
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(badAlign, 0, 0, tap), L"alignment");
			AnalysisLumaSource badContainer = s;
			badContainer.encoding = VideoFrameEncoding::BGRA_8BIT;
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(badContainer, 0, 0, tap), L"container");
			AnalysisLumaSource odd = s;
			odd.width = 7;
			Assert::IsFalse(DvTunnelTapLaw::SourceTap12(odd, 0, 0, tap), L"odd width");
		}

		TEST_METHOD(SourceViewOf12FillsTheView)
		{
			const int width = 16, height = 8;
			const std::vector<uint8_t> bytes = DvTapLaw::Frame(width, height, VideoFrameEncoding::R210);
			AnalysisLumaSource s = DvTapLaw::Source(bytes, width, height, VideoFrameEncoding::R210, 2, false);
			VpSkeleton::SourceFrameView view;
			Assert::IsTrue(VpFrameAnalysis::SourceViewOf12(s, view), L"view");
			Assert::IsTrue(view.Valid() && view.width == 16u && view.height == 8u && view.strideSamples == 16u, L"geometry");
			Assert::AreEqual(4, static_cast<int>(view.msbShift), L"12-bit codes in the low bits");
			Assert::IsTrue(view.keepAlive != nullptr, L"owns its codes");
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					uint16_t tap = 0;
					DvTunnelTapLaw::SourceTap12(s, x, y, tap);
					Assert::AreEqual(static_cast<int>(tap), static_cast<int>(view.luma[static_cast<size_t>(y) * width + x]), L"sample");
				}
			// Refusals: another format, a short buffer.
			AnalysisLumaSource other = s;
			other.format = AnalysisLumaFormat::P010;
			Assert::IsFalse(VpFrameAnalysis::SourceViewOf12(other, view), L"not a packed DV tunnel");
			AnalysisLumaSource shortBuffer = s;
			shortBuffer.dataBytes = bytes.size() - 4;
			Assert::IsFalse(VpFrameAnalysis::SourceViewOf12(shortBuffer, view), L"buffer shorter than rows x height");
		}
	};
}
