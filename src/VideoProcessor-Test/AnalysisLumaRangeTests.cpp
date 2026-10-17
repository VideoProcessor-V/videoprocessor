#include "pch.h"

#include <AnalysisLumaSource.h>
#include "CppUnitTest.h"

#include <array>
#include <cstdlib>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// audit 2026-10-16: AnalysisLumaSource::Sample() reports every source in the
// limited 10-bit code domain (black 64, white 940, chroma 512 +/- 448), so
// full-range RGB / YCbCr sources match the equivalent limited r210 sample.
namespace Tests
{
	namespace
	{
		struct Rgb
		{
			uint16_t red, green, blue;
		};

		AnalysisLumaSample SampleR210(Rgb code,
			ColorSpace colorspace = ColorSpace::REC_709)
		{
			const uint32_t word = (static_cast<uint32_t>(code.red) << 20) |
				(static_cast<uint32_t>(code.green) << 10) | code.blue;
			const uint8_t bytes[4] = {
				static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
				static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word) };
			const AnalysisLumaSource source = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, VideoFrameEncoding::R210,
				colorspace, 1 };
			AnalysisLumaSample sample;
			Assert::IsTrue(source.Sample(0, 0, sample));
			return sample;
		}

		AnalysisLumaSample SampleR10b(Rgb code,
			ColorSpace colorspace = ColorSpace::REC_709)
		{
			const uint32_t word = (static_cast<uint32_t>(code.red) << 22) |
				(static_cast<uint32_t>(code.green) << 12) |
				(static_cast<uint32_t>(code.blue) << 2);
			const uint8_t bytes[4] = {
				static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
				static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word) };
			const AnalysisLumaSource source = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, VideoFrameEncoding::R10b,
				colorspace, 1 };
			AnalysisLumaSample sample;
			Assert::IsTrue(source.Sample(0, 0, sample));
			return sample;
		}

		AnalysisLumaSample Sample8(VideoFrameEncoding encoding, uint8_t red,
			uint8_t green, uint8_t blue, ColorSpace colorspace = ColorSpace::REC_709)
		{
			uint8_t bytes[4] = {};
			if (encoding == VideoFrameEncoding::BGRA_8BIT)
			{
				bytes[0] = blue; bytes[1] = green; bytes[2] = red; bytes[3] = 255;
			}
			else
			{
				bytes[0] = 255; bytes[1] = red; bytes[2] = green; bytes[3] = blue;
			}
			const AnalysisLumaSource source = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, encoding, colorspace, 1 };
			AnalysisLumaSample sample;
			Assert::IsTrue(source.Sample(0, 0, sample));
			return sample;
		}

		// R12L: two pixels per nine bytes; eight pixels make one 36-byte row.
		AnalysisLumaSample SampleR12L(uint16_t red, uint16_t green, uint16_t blue)
		{
			std::array<uint8_t, 36> row{};
			for (size_t pair = 0; pair < 4; ++pair)
			{
				uint8_t* d = row.data() + pair * 9;
				d[0] = static_cast<uint8_t>(red);
				d[1] = static_cast<uint8_t>((red >> 8) | (green << 4));
				d[2] = static_cast<uint8_t>(green >> 4);
				d[3] = static_cast<uint8_t>(blue);
				d[4] = static_cast<uint8_t>((blue >> 8) | (red << 4));
				d[5] = static_cast<uint8_t>(red >> 4);
				d[6] = static_cast<uint8_t>(green);
				d[7] = static_cast<uint8_t>((green >> 8) | (blue << 4));
				d[8] = static_cast<uint8_t>(blue >> 4);
			}
			const AnalysisLumaSource source = { row.data(), row.size(), 8, 1,
				row.size(), 0, AnalysisLumaFormat::NativeRgb,
				VideoFrameEncoding::R12L, ColorSpace::REC_709, 1 };
			AnalysisLumaSample even, odd;
			Assert::IsTrue(source.Sample(0, 0, even));
			Assert::IsTrue(source.Sample(1, 0, odd));
			Assert::AreEqual(static_cast<int>(even.luma), static_cast<int>(odd.luma));
			return even;
		}

		void AssertNear(const AnalysisLumaSample& expected,
			const AnalysisLumaSample& actual, const wchar_t* message)
		{
			Assert::IsTrue(std::abs(static_cast<int>(expected.luma) -
				static_cast<int>(actual.luma)) <= 1, message);
			Assert::IsTrue(std::abs(static_cast<int>(expected.chromaU) -
				static_cast<int>(actual.chromaU)) <= 1, message);
			Assert::IsTrue(std::abs(static_cast<int>(expected.chromaV) -
				static_cast<int>(actual.chromaV)) <= 1, message);
		}

		void AssertExact(int luma, int chromaU, int chromaV,
			const AnalysisLumaSample& actual)
		{
			Assert::AreEqual(luma, static_cast<int>(actual.luma));
			Assert::AreEqual(chromaU, static_cast<int>(actual.chromaU));
			Assert::AreEqual(chromaV, static_cast<int>(actual.chromaV));
		}

		// 4:2:2 16-bit planes (P210 layout): one luma row, one chroma row.
		AnalysisLumaSample SampleP210(uint16_t luma, uint16_t u, uint16_t v,
			bool fullRange)
		{
			const uint16_t plane[] = {
				static_cast<uint16_t>(luma << 6), static_cast<uint16_t>(luma << 6),
				static_cast<uint16_t>(u << 6), static_cast<uint16_t>(v << 6) };
			AnalysisLumaSource source = {
				reinterpret_cast<const uint8_t*>(plane), sizeof(plane), 2, 1,
				4, 4, AnalysisLumaFormat::P210, VideoFrameEncoding::V210,
				ColorSpace::REC_709, 1 };
			source.fullRangeYuv = fullRange;
			AnalysisLumaSample sample;
			Assert::IsTrue(source.Sample(0, 0, sample));
			return sample;
		}
	}

	TEST_CLASS(AnalysisLumaRangeTests)
	{
	public:
		TEST_METHOD(FullRangeEightBitRgbMatchesLimitedR210)
		{
			Assert::IsTrue(kAnalysisLumaSampleLimitedDomain);
			for (const auto encoding : { VideoFrameEncoding::BGRA_8BIT,
				VideoFrameEncoding::ARGB_8BIT })
			{
				// Black, white, mid grey (128/255 -> r210 64 + 0.502 * 896 = 514).
				AssertNear(SampleR210({ 64, 64, 64 }),
					Sample8(encoding, 0, 0, 0), L"8-bit black");
				AssertNear(SampleR210({ 960, 960, 960 }),
					Sample8(encoding, 255, 255, 255), L"8-bit white");
				AssertNear(SampleR210({ 514, 514, 514 }),
					Sample8(encoding, 128, 128, 128), L"8-bit mid grey");
				AssertNear(SampleR210({ 960, 64, 64 }),
					Sample8(encoding, 255, 0, 0), L"8-bit red");
				AssertExact(64, 512, 512, Sample8(encoding, 0, 0, 0));
				AssertExact(940, 512, 512, Sample8(encoding, 255, 255, 255));
				AssertExact(504, 512, 512, Sample8(encoding, 128, 128, 128));
				AssertExact(250, 409, 960, Sample8(encoding, 255, 0, 0));
				AssertExact(127, 960, 471, Sample8(encoding, 0, 0, 255));
			}
		}

		TEST_METHOD(FullRangeTwelveBitRgbMatchesLimitedR210)
		{
			AssertNear(SampleR210({ 64, 64, 64 }), SampleR12L(0, 0, 0),
				L"12-bit black");
			AssertNear(SampleR210({ 960, 960, 960 }), SampleR12L(4095, 4095, 4095),
				L"12-bit white");
			// 2048/4095 -> r210 64 + 0.5001 * 896 = 512.
			AssertNear(SampleR210({ 512, 512, 512 }), SampleR12L(2048, 2048, 2048),
				L"12-bit mid grey");
			AssertExact(64, 512, 512, SampleR12L(0, 0, 0));
			AssertExact(940, 512, 512, SampleR12L(4095, 4095, 4095));
			AssertExact(502, 512, 512, SampleR12L(2048, 2048, 2048));
		}

		TEST_METHOD(BlacksAndWhitesBeyondNominalAreNotClamped)
		{
			// Limited r210: BTB / WTW codes stay below 64 / above 940.
			const auto btb = SampleR210({ 40, 40, 40 });
			const auto wtw = SampleR210({ 1000, 1000, 1000 });
			Assert::AreEqual(41, static_cast<int>(btb.luma));
			Assert::AreEqual(979, static_cast<int>(wtw.luma));
			Assert::IsTrue(btb.luma < 64 && wtw.luma > 940);
			Assert::AreEqual(1, static_cast<int>(SampleR210({ 0, 0, 0 }).luma));
			Assert::AreEqual(1002,
				static_cast<int>(SampleR210({ 1023, 1023, 1023 }).luma));

			// Full-range RGB: the mapping is monotonic; a limited signal carried
			// in a full-range container keeps every code below its black distinct.
			const auto zero = Sample8(VideoFrameEncoding::BGRA_8BIT, 0, 0, 0);
			const auto one = Sample8(VideoFrameEncoding::BGRA_8BIT, 1, 1, 1);
			const auto sixteen = Sample8(VideoFrameEncoding::BGRA_8BIT, 16, 16, 16);
			Assert::AreEqual(67, static_cast<int>(one.luma));
			Assert::IsTrue(zero.luma < one.luma && one.luma < sixteen.luma);

			// Limited YCbCr planes are untouched, BTB / WTW included.
			AssertExact(20, 40, 1000, SampleP210(20, 40, 1000, false));
			AssertExact(1010, 512, 512, SampleP210(1010, 512, 512, false));
		}

		TEST_METHOD(FullRangeYuvPlanesMapToTheLimitedDomain)
		{
			// The decoded DV full-range path: upper ten bits, then mapped.
			AssertExact(64, 64, 960, SampleP210(0, 0, 1023, true));
			AssertExact(940, 512, 512, SampleP210(1023, 512, 512, true));
			AssertExact(652, 665, 127, SampleP210(687, 687, 72, true));
			AssertExact(0, 0, 1023, SampleP210(0, 0, 1023, false));
		}

		TEST_METHOD(Bt601TaggedRgbUsesBt601Coefficients)
		{
			for (const auto colorspace : { ColorSpace::REC_601_525,
				ColorSpace::REC_601_576, ColorSpace::REC_601_625 })
			{
				AssertExact(326, 361, 960, SampleR210({ 960, 64, 64 }, colorspace));
				AssertExact(164, 960, 439, SampleR210({ 64, 64, 960 }, colorspace));
				AssertExact(326, 361, 960, SampleR10b({ 940, 64, 64 }, colorspace));
				AssertExact(164, 960, 439, SampleR10b({ 64, 64, 940 }, colorspace));
				AssertExact(326, 361, 960,
					Sample8(VideoFrameEncoding::BGRA_8BIT, 255, 0, 0, colorspace));
				AssertExact(164, 960, 439,
					Sample8(VideoFrameEncoding::ARGB_8BIT, 0, 0, 255, colorspace));
				// Neutral greys are coefficient-independent.
				AssertExact(504, 512, 512, SampleR210({ 514, 514, 514 }, colorspace));
			}
			// BT.709 (and untagged) selection is unchanged.
			for (const auto colorspace : { ColorSpace::REC_709, ColorSpace::UNKNOWN })
			{
				AssertExact(250, 409, 960, SampleR210({ 960, 64, 64 }, colorspace));
				AssertExact(250, 409, 960,
					Sample8(VideoFrameEncoding::BGRA_8BIT, 255, 0, 0, colorspace));
			}
			Assert::AreNotEqual(
				static_cast<int>(SampleR210({ 960, 64, 64 }, ColorSpace::BT_2020).luma),
				static_cast<int>(SampleR210({ 960, 64, 64 }, ColorSpace::REC_709).luma));
		}

		TEST_METHOD(LimitedRgbPathIsUnchanged)
		{
			// Reference values from the pre-audit limited r210 / R10 path.
			AssertExact(64, 512, 512, SampleR210({ 64, 64, 64 }));
			AssertExact(940, 512, 512, SampleR210({ 960, 960, 960 }));
			AssertExact(504, 512, 512, SampleR210({ 514, 514, 514 }));
			AssertExact(502, 512, 512, SampleR210({ 512, 512, 512 }));
			AssertExact(250, 409, 960, SampleR210({ 960, 64, 64 }));
			AssertExact(127, 960, 471, SampleR210({ 64, 64, 960 }));
			AssertExact(250, 409, 960, SampleR10b({ 940, 64, 64 }));
			AssertExact(64, 512, 512, SampleR10b({ 64, 64, 64 }));
			AssertExact(940, 512, 512, SampleR10b({ 940, 940, 940 }));
		}
	};
}
