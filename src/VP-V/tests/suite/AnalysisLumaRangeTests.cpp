#include "pch.h"

#include <AnalysisLumaSource.h>
#include <AnalysisFrame.h>
#include <SubtitleBoxDetector.h>
#include "CppUnitTest.h"

#include <array>
#include <cstdlib>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// audit 2026-10-16 (VP-V DIV-AN-01, either/or since the merge of Bill's v1.4.02):
// with AnalysisLumaSource::limitedDomain (VP-V pathway, source_coding_fixes=on)
// Sample() reports every source in the limited 10-bit code domain (black 64,
// white 940, chroma 512 +/- 448), so full-range RGB / YCbCr sources match the
// equivalent limited r210 sample. Without it Sample() is Bill's sampler exactly.
namespace Tests
{
	namespace
	{
		// VP-V DIV-AN-01 is an either/or (merge 2026-10-31): these helpers sample in the
		// VP-V pathway's limited domain unless a test selects Bill's (Original) sampler.
		bool vpLimitedDomain = true;
		struct OriginalSampler
		{
			OriginalSampler() { vpLimitedDomain = false; }
			~OriginalSampler() { vpLimitedDomain = true; }
		};

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
			AnalysisLumaSource source = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, VideoFrameEncoding::R210,
				colorspace, 1 };
			source.limitedDomain = vpLimitedDomain;
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
			AnalysisLumaSource source = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, VideoFrameEncoding::R10b,
				colorspace, 1 };
			source.limitedDomain = vpLimitedDomain;
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
			AnalysisLumaSource source = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, encoding, colorspace, 1 };
			source.limitedDomain = vpLimitedDomain;
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
			AnalysisLumaSource source = { row.data(), row.size(), 8, 1,
				row.size(), 0, AnalysisLumaFormat::NativeRgb,
				VideoFrameEncoding::R12L, ColorSpace::REC_709, 1 };
			source.limitedDomain = vpLimitedDomain;
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
			source.limitedDomain = vpLimitedDomain;
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
		// ---- VP-V DIV-AN-01 either/or (merge of Bill's v1.4.02, 2026-10-31) ----

		TEST_METHOD(OriginalSamplerIsBillsDeliveredCodes)
		{
			// Original pathway (limitedDomain clear): Bill's v1.4.02 sampler exactly.
			OriginalSampler original;
			for (const auto encoding : { VideoFrameEncoding::BGRA_8BIT,
				VideoFrameEncoding::ARGB_8BIT })
			{
				AssertExact(0, 512, 512, Sample8(encoding, 0, 0, 0));
				AssertExact(1023, 512, 512, Sample8(encoding, 255, 255, 255));
				AssertExact(514, 512, 512, Sample8(encoding, 128, 128, 128));
				AssertExact(217, 395, 1023, Sample8(encoding, 255, 0, 0));
				// Bill has no BT.601 tables: BT.601-tagged RGB uses BT.709.
				AssertExact(217, 395, 1023, Sample8(encoding, 255, 0, 0, ColorSpace::REC_601_525));
			}
			AssertExact(250, 409, 960, SampleR210({ 960, 64, 64 }, ColorSpace::REC_601_625));
			AssertExact(0, 512, 512, SampleR12L(0, 0, 0));
			AssertExact(1023, 512, 512, SampleR12L(4095, 4095, 4095));
		}

		TEST_METHOD(DomainQueriesGiveOneAuthorityPerSource)
		{
			const uint8_t bytes[4] = {};
			AnalysisLumaSource rgb8 = { bytes, sizeof(bytes), 1, 1, 4, 0,
				AnalysisLumaFormat::NativeRgb, VideoFrameEncoding::BGRA_8BIT, ColorSpace::REC_709, 1 };
			rgb8.range = AnalysisLumaRange::Full;
			// Original: delivered full-range codes; Bill's range tag is the authority.
			Assert::IsTrue(rgb8.FullRangeLevels());
			Assert::IsFalse(rgb8.MapsToLimitedDomain());
			Assert::IsTrue(rgb8.CodesFullRange());
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Full), static_cast<int>(rgb8.CodesRange()));
			// VP-V: the sampler mapped the codes; nobody interprets them as full range again.
			rgb8.limitedDomain = true;
			Assert::IsTrue(rgb8.MapsToLimitedDomain());
			Assert::IsFalse(rgb8.CodesFullRange());
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Limited), static_cast<int>(rgb8.CodesRange()));

			AnalysisLumaSource r210 = rgb8;
			r210.encoding = VideoFrameEncoding::R210;
			r210.range = AnalysisLumaRange::Limited;
			Assert::IsFalse(r210.FullRangeLevels());
			Assert::IsFalse(r210.MapsToLimitedDomain());
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Limited), static_cast<int>(r210.CodesRange()));

			AnalysisLumaSource dv;
			dv.format = AnalysisLumaFormat::DecodedDolbyVision422;
			dv.range = AnalysisLumaRange::Full;
			Assert::IsTrue(dv.CodesFullRange());            // Original: Bill's raw full-range planes
			dv.limitedDomain = true;
			dv.fullRangeYuv = true;
			Assert::IsFalse(dv.CodesFullRange());           // VP-V: mapped
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Limited), static_cast<int>(dv.CodesRange()));
		}

		TEST_METHOD(PreparedFramesFollowTheEitherOr)
		{
			constexpr int width = 4;
			constexpr int height = 2;
			auto dvPixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4, 0);
			auto* dvWords = reinterpret_cast<uint16_t*>(dvPixels->data());
			for (int pixel = 0; pixel < width * height; ++pixel)
			{
				dvWords[pixel] = static_cast<uint16_t>(200 << 4);
				dvWords[width * height + pixel] = static_cast<uint16_t>(2048 << 4);
			}
			AnalysisFrameInput dv;
			dv.convertedData = dvPixels->data();
			dv.convertedBytes = dvPixels->size();
			dv.convertedRowBytes = width * 2;
			dv.convertedChromaRowBytes = width * 2;
			dv.convertedOwner = dvPixels;
			dv.converted422 = true;
			dv.dolbyVisionTunnel = true;
			dv.dolby.valid = true;
			dv.dolby.fullRange = true;
			dv.width = width;
			dv.height = height;
			dv.encoding = VideoFrameEncoding::R210;
			dv.colorspace = ColorSpace::BT_2020;
			dv.sourceGeneration = 17;
			dv.convertedContract = { VideoFrameSampleRange::FULL, 12, 4 };
			const auto original = PrepareAnalysisFrame(dv);
			dv.vpLimitedDomain = true;
			const auto vp = PrepareAnalysisFrame(dv);
			Assert::IsTrue(original.IsValid() && vp.IsValid());
			Assert::IsFalse(original.source.limitedDomain || original.source.fullRangeYuv);
			Assert::IsTrue(vp.source.limitedDomain && vp.source.fullRangeYuv);
			Assert::AreNotEqual(original.domainKey, vp.domainKey);
			AnalysisLumaSample sample;
			Assert::IsTrue(original.source.Sample(2, 1, sample));
			AssertExact(50, 512, 512, sample);                 // Bill: delivered codes, range Full
			Assert::IsTrue(original.source.CodesFullRange());
			Assert::IsTrue(vp.source.Sample(2, 1, sample));
			AssertExact(107, 512, 512, sample);                // 64 + 50 * 876 / 1023
			Assert::IsFalse(vp.source.CodesFullRange());
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Full), static_cast<int>(vp.source.range));
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Limited), static_cast<int>(vp.source.CodesRange()));

			auto rgbPixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4, 0);
			AnalysisFrameInput native;
			native.nativeData = rgbPixels->data();
			native.nativeBytes = rgbPixels->size();
			native.nativeRowBytes = width * 4;
			native.nativeOwner = rgbPixels;
			native.width = width;
			native.height = height;
			native.encoding = VideoFrameEncoding::BGRA_8BIT;
			native.colorspace = ColorSpace::REC_709;
			native.sourceGeneration = 3;
			const auto rgbOriginal = PrepareAnalysisFrame(native);
			native.vpLimitedDomain = true;
			const auto rgbVp = PrepareAnalysisFrame(native);
			Assert::IsTrue(rgbOriginal.IsValid() && rgbVp.IsValid());
			Assert::IsTrue(rgbOriginal.source.Sample(1, 1, sample));
			Assert::AreEqual(0, static_cast<int>(sample.luma));
			Assert::IsTrue(rgbVp.source.Sample(1, 1, sample));
			Assert::AreEqual(64, static_cast<int>(sample.luma));
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Full), static_cast<int>(rgbOriginal.source.CodesRange()));
			Assert::AreEqual(static_cast<int>(AnalysisLumaRange::Limited), static_cast<int>(rgbVp.source.CodesRange()));
			Assert::AreNotEqual(rgbOriginal.domainKey, rgbVp.domainKey);
		}

		TEST_METHOD(SharedLumaPlaneIsBoundToTheDomain)
		{
			const uint8_t bytes[8] = {};
			AnalysisLumaSource vp = { bytes, sizeof(bytes), 2, 1, 8, 0,
				AnalysisLumaFormat::NativeRgb, VideoFrameEncoding::BGRA_8BIT, ColorSpace::REC_709, 1 };
			vp.limitedDomain = true;
			AnalysisLumaPlane plane;
			plane.Bind(vp);
			Assert::IsTrue(plane.Matches(vp));
			AnalysisLumaSource original = vp;
			original.limitedDomain = false;
			Assert::IsFalse(plane.Matches(original), L"A plane decoded in one domain never serves the other.");
		}

		TEST_METHOD(SubtitleDetectorFindsTheSameCueInTheLimitedDomain)
		{
			// The VP-V pathway: a full-range RGB capture (black 0) of the limited P010 fixture
			// that Bill's NativeRgbAndP210HaveEquivalentBoxes uses is mapped back to the same
			// limited codes, so Bill's detector (absolute thresholds included) finds the same box.
			constexpr int W = 640, H = 360;
			std::vector<uint16_t> f(W * H * 3 / 2, static_cast<uint16_t>(512 << 6));
			auto fill = [&](int l, int t, int r, int b, int value)
			{
				for (int y = t; y < b; ++y) for (int x = l; x < r; ++x) f[y * W + x] = static_cast<uint16_t>(value << 6);
			};
			fill(0, 0, W, H, 64); fill(0, 45, W, 315, 200);
			fill(190, 300, 470, 315, 64);               // panel to the bar
			for (int n = 0; n < 20; ++n)
			{
				const int x = 200 + n * 13, t = 310;
				fill(x - 1, t - 1, x + 9, t + 15, 64);
				fill(x, t, x + 8, t + 14, 510); fill(x + 2, t + 2, x + 6, t + 12, 64);
			}
			AnalysisLumaSource p010; p010.data = reinterpret_cast<const uint8_t*>(f.data());
			p010.dataBytes = f.size() * 2; p010.width = W; p010.height = H; p010.rowBytes = p010.chromaRowBytes = W * 2;
			p010.format = AnalysisLumaFormat::P010; p010.generation = 1;
			SubtitleBoxDetector reference;
			const auto expected = reference.Analyze(p010, 45, 315, 0, 1);
			Assert::IsTrue(expected.detected);
			std::vector<uint8_t> rgb(W * H * 4, 255);
			for (int i = 0; i < W * H; ++i) for (int c = 0; c < 3; ++c) rgb[i * 4 + c] = static_cast<uint8_t>(
				(std::max)(0, (std::min)(255, ((static_cast<int>(f[i] >> 6) - 64) * 255 + 438) / 876)));
			AnalysisLumaSource s; s.data = rgb.data(); s.dataBytes = rgb.size(); s.width = W; s.height = H;
			s.rowBytes = W * 4; s.format = AnalysisLumaFormat::NativeRgb; s.encoding = VideoFrameEncoding::BGRA_8BIT;
			s.colorspace = ColorSpace::REC_709; s.generation = 1; s.range = AnalysisLumaRange::Full;
			s.limitedDomain = true;
			SubtitleBoxDetector native;
			const auto actual = native.Analyze(s, 45, 315, 0, 1);
			Assert::IsTrue(actual.detected);
			Assert::AreEqual(expected.bounds.left, actual.bounds.left);
			Assert::AreEqual(expected.bounds.top, actual.bounds.top);
			Assert::AreEqual(expected.bounds.right, actual.bounds.right);
			Assert::AreEqual(expected.bounds.bottom, actual.bounds.bottom);
		}
	};
}
