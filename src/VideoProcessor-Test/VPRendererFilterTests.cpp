#include "pch.h"
#include "CppUnitTest.h"

// VP Renderer DirectShow filter: the pure parts (media-type acceptance, stride
// repack, plugin refusal, registration identity, timing arithmetic). The
// filter's .cpp files compiled here are listed in VideoProcessor-Test's
// project; nothing is registered and no plugin is loaded.

#include "../VideoProcessor-VPRendererFilter/FilterTimingGlueMath.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterMediaTypes.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterPluginCheck.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterRegistration.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterRepack.h"

#include <DisplayMode.h>
#include <VideoState.h>

#include <dvdmedia.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VPRendererFilter;


namespace
{
	// A media type over a format block owned by the caller.
	struct TestMediaType
	{
		AM_MEDIA_TYPE mt = {};
		VIDEOINFOHEADER2 vih2 = {};
		VIDEOINFOHEADER vih = {};

		TestMediaType(const GUID& subtype, LONG width, LONG height, bool videoInfo2 = true,
			WORD bitCount = 0)
		{
			mt.majortype = kMediaTypeVideo;
			mt.subtype = subtype;
			BITMAPINFOHEADER* bmi = nullptr;
			if (videoInfo2)
			{
				mt.formattype = kFormatVideoInfo2;
				mt.cbFormat = sizeof(vih2);
				mt.pbFormat = reinterpret_cast<BYTE*>(&vih2);
				vih2.AvgTimePerFrame = 417083;
				bmi = &vih2.bmiHeader;
			}
			else
			{
				mt.formattype = kFormatVideoInfo;
				mt.cbFormat = sizeof(vih);
				mt.pbFormat = reinterpret_cast<BYTE*>(&vih);
				vih.AvgTimePerFrame = 400000;
				bmi = &vih.bmiHeader;
			}
			bmi->biSize = sizeof(BITMAPINFOHEADER);
			bmi->biWidth = width;
			bmi->biHeight = height;
			bmi->biPlanes = 1;
			bmi->biBitCount = bitCount ? bitCount :
				(IsEqualGUID(subtype, kSubtypeRGB32) ? 32 : 12);
		}
	};

	const GUID kSubtypeYUY2 = { 0x32595559, 0x0000, 0x0010,
		{ 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
	// MEDIASUBTYPE_RGB48 is not a standard DirectShow GUID; 'b48r' is what LAV uses.
	const GUID kSubtypeB48r = { 0x72383462, 0x0000, 0x0010,
		{ 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
	const GUID kMediaTypeAudio = { 0x73647561, 0x0000, 0x0010,
		{ 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

	std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
	{
		std::vector<uint8_t> v(size);
		for (size_t i = 0; i < size; ++i)
			v[i] = static_cast<uint8_t>(seed + i * 7);
		return v;
	}
}


namespace Tests
{
	TEST_CLASS(VPRendererFilterMediaTypeTests)
	{
	public:
		TEST_METHOD(ProposalOrderIsP010P016Nv12V210Rgb32)
		{
			const std::vector<SubtypeInfo>& s = AcceptedSubtypes();
			Assert::AreEqual<size_t>(5, s.size());
			Assert::IsTrue(IsEqualGUID(s[0].subtype, kSubtypeP010) == TRUE);
			Assert::IsTrue(IsEqualGUID(s[1].subtype, kSubtypeP016) == TRUE);
			Assert::IsTrue(IsEqualGUID(s[2].subtype, kSubtypeNV12) == TRUE);
			Assert::IsTrue(IsEqualGUID(s[3].subtype, kSubtypeV210) == TRUE);
			Assert::IsTrue(IsEqualGUID(s[4].subtype, kSubtypeRGB32) == TRUE);
			Assert::IsTrue(s[0].encoding == VideoFrameEncoding::P010);
			Assert::IsTrue(s[1].encoding == VideoFrameEncoding::P016);
			Assert::IsTrue(s[2].encoding == VideoFrameEncoding::NV12);
			Assert::IsTrue(s[3].encoding == VideoFrameEncoding::V210);
			Assert::IsTrue(s[4].encoding == VideoFrameEncoding::BGRA_8BIT);
		}

		TEST_METHOD(FourccSubtypesHaveTheStandardGuids)
		{
			// {30313050-0000-0010-8000-00AA00389B71} = MEDIASUBTYPE_P010.
			Assert::AreEqual<unsigned long>(0x30313050ul, kSubtypeP010.Data1);
			Assert::AreEqual<unsigned long>(0x36313050ul, kSubtypeP016.Data1);
			Assert::AreEqual<unsigned long>(0x3231564Eul, kSubtypeNV12.Data1);
			Assert::AreEqual<unsigned long>(0x30313276ul, kSubtypeV210.Data1);
			Assert::IsTrue(IsEqualGUID(kSubtypeRGB32, MEDIASUBTYPE_RGB32) == TRUE);
			Assert::IsTrue(IsEqualGUID(kMediaTypeVideo, MEDIATYPE_Video) == TRUE);
			Assert::IsTrue(IsEqualGUID(kFormatVideoInfo2, FORMAT_VideoInfo2) == TRUE);
			Assert::IsTrue(IsEqualGUID(kFormatVideoInfo, FORMAT_VideoInfo) == TRUE);
		}

		TEST_METHOD(AcceptsEveryContractSubtypeWithBothFormatBlocks)
		{
			for (const SubtypeInfo& info : AcceptedSubtypes())
			{
				for (int vih2 = 0; vih2 < 2; ++vih2)
				{
					TestMediaType t(info.subtype, 1920, 1080, vih2 != 0);
					VideoFormat f;
					std::string reason;
					Assert::IsTrue(ParseVideoMediaType(t.mt, f, reason), ToWide(reason).c_str());
					Assert::IsTrue(f.encoding == info.encoding);
					Assert::AreEqual<uint32_t>(1920, f.width);
					Assert::AreEqual<uint32_t>(1080, f.height);
					Assert::AreEqual(vih2 != 0, f.hasVideoInfo2);
				}
			}
		}

		TEST_METHOD(RejectsYuy2Rgb48AudioAndBadFormatBlocks)
		{
			VideoFormat f;
			std::string reason;
			TestMediaType yuy2(kSubtypeYUY2, 1920, 1080);
			Assert::IsFalse(ParseVideoMediaType(yuy2.mt, f, reason));
			TestMediaType rgb48(kSubtypeB48r, 1920, 1080);
			Assert::IsFalse(ParseVideoMediaType(rgb48.mt, f, reason));

			TestMediaType audio(kSubtypeNV12, 1920, 1080);
			audio.mt.majortype = kMediaTypeAudio;
			Assert::IsFalse(ParseVideoMediaType(audio.mt, f, reason));

			TestMediaType shortBlock(kSubtypeNV12, 1920, 1080);
			shortBlock.mt.cbFormat = sizeof(VIDEOINFOHEADER2) - 1;
			Assert::IsFalse(ParseVideoMediaType(shortBlock.mt, f, reason));

			TestMediaType noBlock(kSubtypeNV12, 1920, 1080);
			noBlock.mt.pbFormat = nullptr;
			Assert::IsFalse(ParseVideoMediaType(noBlock.mt, f, reason));

			TestMediaType partial(kSubtypeNV12, 1920, 1080);
			partial.mt.formattype = GUID_NULL;
			Assert::IsFalse(ParseVideoMediaType(partial.mt, f, reason));

			TestMediaType rgb24(kSubtypeRGB32, 1920, 1080, true, 24);
			Assert::IsFalse(ParseVideoMediaType(rgb24.mt, f, reason));

			TestMediaType zero(kSubtypeNV12, 0, 1080);
			Assert::IsFalse(ParseVideoMediaType(zero.mt, f, reason));
		}

		TEST_METHOD(ParsesPaddedStrideRcSourceAndAspect)
		{
			// LAV-style: coded width padded to 1984, visible 1920x1080, 16:9.
			TestMediaType t(kSubtypeP010, 1984, 1088);
			t.vih2.rcSource = { 0, 0, 1920, 1080 };
			t.vih2.dwPictAspectRatioX = 16;
			t.vih2.dwPictAspectRatioY = 9;
			VideoFormat f;
			std::string reason;
			Assert::IsTrue(ParseVideoMediaType(t.mt, f, reason));
			Assert::AreEqual<uint32_t>(1920, f.width);
			Assert::AreEqual<uint32_t>(1080, f.height);
			Assert::AreEqual<uint32_t>(1984, f.codedWidth);
			Assert::AreEqual<uint32_t>(1088, f.codedHeight);
			Assert::AreEqual<uint32_t>(1984 * 2, f.sourceStrideBytes);
			Assert::AreEqual<uint32_t>(16, f.aspectX);
			Assert::AreEqual<uint32_t>(9, f.aspectY);
			Assert::AreEqual<int64_t>(417083, f.avgTimePerFrame);

			// Without a picture aspect the visible size is used (reduced).
			TestMediaType square(kSubtypeNV12, 1440, 1080, false);
			Assert::IsTrue(ParseVideoMediaType(square.mt, f, reason));
			Assert::AreEqual<uint32_t>(4, f.aspectX);
			Assert::AreEqual<uint32_t>(3, f.aspectY);

			// rcSource outside the coded frame, or a 4:2:0 origin off the chroma grid.
			TestMediaType outside(kSubtypeNV12, 1920, 1080);
			outside.vih2.rcSource = { 0, 0, 1922, 1080 };
			Assert::IsFalse(ParseVideoMediaType(outside.mt, f, reason));
			TestMediaType odd(kSubtypeNV12, 1920, 1080);
			odd.vih2.rcSource = { 1, 0, 1919, 1080 };
			Assert::IsFalse(ParseVideoMediaType(odd.mt, f, reason));
		}

		TEST_METHOD(BottomUpOnlyForPositiveHeightRgb)
		{
			VideoFormat f;
			std::string reason;
			TestMediaType up(kSubtypeRGB32, 1280, 720);
			Assert::IsTrue(ParseVideoMediaType(up.mt, f, reason));
			Assert::IsTrue(f.bottomUp);
			Assert::AreEqual<uint32_t>(1280 * 4, f.sourceStrideBytes);

			TestMediaType down(kSubtypeRGB32, 1280, -720);
			Assert::IsTrue(ParseVideoMediaType(down.mt, f, reason));
			Assert::IsFalse(f.bottomUp);
			Assert::AreEqual<uint32_t>(720, f.codedHeight);

			TestMediaType yuv(kSubtypeNV12, 1280, 720);
			Assert::IsTrue(ParseVideoMediaType(yuv.mt, f, reason));
			Assert::IsFalse(f.bottomUp);
		}

		TEST_METHOD(DecodesExtendedColorimetry)
		{
			// BT.2020 primaries (9), PQ (15), BT.2020 matrix (4), limited range (2).
			const DWORD flags = AMCONTROL_COLORINFO_PRESENT | AMCONTROL_USED |
				(2u << 12) | (4u << 15) | (9u << 22) | (15u << 27);
			const Colorimetry c = DecodeColorimetry(flags);
			Assert::IsTrue(c.present);
			Assert::AreEqual<uint32_t>(2, c.nominalRange);
			Assert::AreEqual<uint32_t>(4, c.transferMatrix);
			Assert::AreEqual<uint32_t>(9, c.primaries);
			Assert::AreEqual<uint32_t>(15, c.transferFunction);
			Assert::IsTrue(ColorSpaceFromColorimetry(c) == ColorSpace::BT_2020);
			Assert::IsTrue(EotfFromColorimetry(c) == EOTF::PQ);

			Colorimetry hlg = c;
			hlg.transferFunction = 16;
			Assert::IsTrue(EotfFromColorimetry(hlg) == EOTF::HLG);
			Colorimetry sdr = c;
			sdr.primaries = 2;
			sdr.transferFunction = 4;   // BT.709
			Assert::IsTrue(ColorSpaceFromColorimetry(sdr) == ColorSpace::REC_709);
			Assert::IsTrue(EotfFromColorimetry(sdr) == EOTF::SDR);

			const Colorimetry absent = DecodeColorimetry(AMCONTROL_USED | (9u << 22));
			Assert::IsFalse(absent.present);
			Assert::IsTrue(ColorSpaceFromColorimetry(absent) == ColorSpace::UNKNOWN);
			Assert::IsTrue(EotfFromColorimetry(absent) == EOTF::UNKNOWN);
		}

		TEST_METHOD(TightLayoutMatchesVideoState)
		{
			const VideoFrameEncoding encodings[] = { VideoFrameEncoding::NV12,
				VideoFrameEncoding::P010, VideoFrameEncoding::P016, VideoFrameEncoding::V210,
				VideoFrameEncoding::BGRA_8BIT };
			const uint32_t sizes[][2] = { { 1920, 1080 }, { 1279, 719 }, { 3840, 2160 },
				{ 721, 481 } };
			for (VideoFrameEncoding e : encodings)
			{
				for (const auto& size : sizes)
				{
					VideoState state;
					state.videoFrameEncoding = e;
					state.displayMode = std::make_shared<DisplayMode>(size[0], size[1], false,
						24000, 1001);
					Assert::AreEqual(state.BytesPerRow(), TightRowBytes(e, size[0]));
					Assert::AreEqual(static_cast<size_t>(state.BytesPerFrame()),
						TightFrameBytes(e, size[0], size[1]));
				}
			}
		}

		TEST_METHOD(ExactRateFromFrameDuration)
		{
			struct Case { int64_t duration; uint32_t num; uint32_t den; };
			const Case cases[] = {
				{ 417083, 24000, 1001 }, { 417084, 24000, 1001 },
				{ 416666, 24, 1 }, { 416667, 24, 1 },
				{ 400000, 25, 1 }, { 333667, 30000, 1001 }, { 333333, 30, 1 },
				{ 200000, 50, 1 }, { 166833, 60000, 1001 }, { 166667, 60, 1 },
				{ 83333, 120, 1 }, { 1428571, 7, 1 },
			};
			for (const Case& c : cases)
			{
				const RationalRate r = RationalFromFrameDuration(c.duration);
				Assert::AreEqual(c.num, r.numerator, std::to_wstring(c.duration).c_str());
				Assert::AreEqual(c.den, r.denominator, std::to_wstring(c.duration).c_str());
			}
			const RationalRate none = RationalFromFrameDuration(0);
			Assert::AreEqual<uint32_t>(0, none.numerator);
			Assert::AreEqual<uint32_t>(0, none.denominator);
			// Neither an integer nor a 1001-family rate: the simplest convergent
			// that reproduces the duration (23.5 Hz).
			const RationalRate half = RationalFromFrameDuration(425532);
			Assert::AreEqual<uint32_t>(47, half.numerator);
			Assert::AreEqual<uint32_t>(2, half.denominator);
		}

	private:
		static std::wstring ToWide(const std::string& s)
		{
			return std::wstring(s.begin(), s.end());
		}
	};


	TEST_CLASS(VPRendererFilterRepackTests)
	{
	public:
		TEST_METHOD(Nv12PaddedStrideOddHeight)
		{
			const uint32_t width = 6, height = 5, stride = 16;
			const uint32_t chromaRows = (height + 1) / 2;
			std::vector<uint8_t> src = Pattern(stride * (height + chromaRows), 3);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = height;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::NV12, width, height));
			Assert::AreEqual<size_t>(6 * (5 + 3), dst.size());
			Assert::IsTrue(RepackFrame(VideoFrameEncoding::NV12, width, height, s, dst.data(), dst.size()));
			for (uint32_t r = 0; r < height; ++r)
				Assert::AreEqual(0, memcmp(&dst[r * 6], &src[r * stride], 6));
			for (uint32_t r = 0; r < chromaRows; ++r)
				Assert::AreEqual(0, memcmp(&dst[(height + r) * 6], &src[(height + r) * stride], 6));
		}

		TEST_METHOD(Nv12OddWidthUsesEvenRows)
		{
			const uint32_t width = 5, height = 4, stride = 8;
			std::vector<uint8_t> src = Pattern(stride * (height + 2), 11);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = height;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::NV12, width, height));
			Assert::AreEqual<size_t>(6 * 6, dst.size());
			Assert::IsTrue(RepackFrame(VideoFrameEncoding::NV12, width, height, s, dst.data(), dst.size()));
			Assert::AreEqual(0, memcmp(&dst[6], &src[stride], 6));
			Assert::AreEqual(0, memcmp(&dst[4 * 6], &src[4 * stride], 6));
		}

		TEST_METHOD(P010PaddedStrideAndOrigin)
		{
			// Coded 8x6 P010 (stride 20 bytes, padded), visible 4x3 at (2,2).
			const uint32_t codedHeight = 6, stride = 20, width = 4, height = 3;
			std::vector<uint8_t> src = Pattern(stride * (codedHeight + codedHeight / 2), 29);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = codedHeight;
			s.originX = 2;
			s.originY = 2;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::P010, width, height));
			const size_t row = 8;   // 4 samples * 2 bytes
			Assert::AreEqual<size_t>(row * (3 + 2), dst.size());
			Assert::IsTrue(RepackFrame(VideoFrameEncoding::P010, width, height, s, dst.data(), dst.size()));
			for (uint32_t r = 0; r < height; ++r)
				Assert::AreEqual(0, memcmp(&dst[r * row], &src[(2 + r) * stride + 4], row));
			const size_t chroma = static_cast<size_t>(stride) * codedHeight;
			for (uint32_t r = 0; r < 2; ++r)
				Assert::AreEqual(0, memcmp(&dst[(height + r) * row],
					&src[chroma + (1 + r) * stride + 4], row));
		}

		TEST_METHOD(Rgb32BottomUpIsFlipped)
		{
			const uint32_t width = 3, height = 4, stride = 12;
			std::vector<uint8_t> src = Pattern(stride * height, 5);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = height;
			s.bottomUp = true;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::BGRA_8BIT, width, height));
			Assert::IsTrue(RepackFrame(VideoFrameEncoding::BGRA_8BIT, width, height, s, dst.data(), dst.size()));
			for (uint32_t r = 0; r < height; ++r)
				Assert::AreEqual(0, memcmp(&dst[r * 12], &src[(height - 1 - r) * stride], 12));
		}

		TEST_METHOD(Rgb32BottomUpWithPaddingAndOrigin)
		{
			// Coded 4x5 with a 24-byte stride, visible 2x3 at (1,1), bottom-up.
			const uint32_t codedHeight = 5, stride = 24, width = 2, height = 3;
			std::vector<uint8_t> src = Pattern(stride * codedHeight, 17);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = codedHeight;
			s.bottomUp = true;
			s.originX = 1;
			s.originY = 1;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::BGRA_8BIT, width, height));
			Assert::IsTrue(RepackFrame(VideoFrameEncoding::BGRA_8BIT, width, height, s, dst.data(), dst.size()));
			for (uint32_t r = 0; r < height; ++r)
			{
				const size_t srcRow = codedHeight - 1 - (1 + r);
				Assert::AreEqual(0, memcmp(&dst[r * 8], &src[srcRow * stride + 4], 8));
			}
		}

		TEST_METHOD(V210CopiesTightRowsFromPaddedStride)
		{
			const uint32_t width = 6, height = 3, stride = 256;
			std::vector<uint8_t> src = Pattern(stride * height, 41);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = height;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::V210, width, height));
			Assert::AreEqual<size_t>(128 * 3, dst.size());
			Assert::IsTrue(RepackFrame(VideoFrameEncoding::V210, width, height, s, dst.data(), dst.size()));
			for (uint32_t r = 0; r < height; ++r)
				Assert::AreEqual(0, memcmp(&dst[r * 128], &src[r * stride], 128));
		}

		TEST_METHOD(RejectsShortSamplesNarrowStridesAndWrongDestination)
		{
			const uint32_t width = 8, height = 4, stride = 8;
			std::vector<uint8_t> src = Pattern(stride * (height + 2), 1);
			RepackSource s;
			s.data = src.data();
			s.size = src.size();
			s.strideBytes = stride;
			s.codedHeight = height;
			std::vector<uint8_t> dst(TightFrameBytes(VideoFrameEncoding::NV12, width, height));
			std::string reason;

			RepackSource shortSample = s;
			shortSample.size = src.size() - 1;
			Assert::IsFalse(RepackFrame(VideoFrameEncoding::NV12, width, height, shortSample,
				dst.data(), dst.size(), &reason));
			Assert::IsFalse(reason.empty());

			RepackSource narrow = s;
			narrow.strideBytes = 6;
			Assert::IsFalse(RepackFrame(VideoFrameEncoding::NV12, width, height, narrow,
				dst.data(), dst.size()));

			Assert::IsFalse(RepackFrame(VideoFrameEncoding::NV12, width, height, s,
				dst.data(), dst.size() - 1));

			RepackSource tooTall = s;
			tooTall.originY = 2;
			Assert::IsFalse(RepackFrame(VideoFrameEncoding::NV12, width, height, tooTall,
				dst.data(), dst.size()));

			Assert::IsFalse(RepackFrame(VideoFrameEncoding::UYVY, width, height, s,
				dst.data(), dst.size()));
		}
	};


	TEST_CLASS(VPRendererFilterPluginCheckTests)
	{
	public:
		TEST_METHOD(AcceptsMatchingVersionAndFlavor)
		{
			PluginExportProbe probe = Complete(35, false);
			std::string reason;
			Assert::IsTrue(CheckPluginExports(probe, 35, false, reason) == PluginCheck::Ok);
		}

		TEST_METHOD(RefusesApiMismatchBeforeAnyOtherCall)
		{
			bool flavorCalled = false;
			PluginExportProbe probe = Complete(34, false);
			probe.isBeta = [&flavorCalled]() { flavorCalled = true; return false; };
			std::string reason;
			Assert::IsTrue(CheckPluginExports(probe, 35, false, reason) == PluginCheck::ApiMismatch);
			Assert::IsFalse(flavorCalled);
			Assert::IsTrue(reason.find("expects 35") != std::string::npos);
			Assert::IsTrue(reason.find("reports 34") != std::string::npos);

			probe = Complete(36, false);
			Assert::IsTrue(CheckPluginExports(probe, 35, false, reason) == PluginCheck::ApiMismatch);
		}

		TEST_METHOD(RefusesMissingExports)
		{
			std::string reason;
			PluginExportProbe noVersion = Complete(35, false);
			noVersion.getApiVersion = nullptr;
			Assert::IsTrue(CheckPluginExports(noVersion, 35, false, reason) == PluginCheck::MissingExports);
			PluginExportProbe noCreate = Complete(35, false);
			noCreate.hasCreate = false;
			Assert::IsTrue(CheckPluginExports(noCreate, 35, false, reason) == PluginCheck::MissingExports);
			PluginExportProbe noDestroy = Complete(35, false);
			noDestroy.hasDestroy = false;
			Assert::IsTrue(CheckPluginExports(noDestroy, 35, false, reason) == PluginCheck::MissingExports);
		}

		TEST_METHOD(RefusesWrongOrMissingFlavor)
		{
			std::string reason;
			PluginExportProbe beta = Complete(35, true);
			Assert::IsTrue(CheckPluginExports(beta, 35, false, reason) == PluginCheck::FlavorMismatch);
			Assert::IsTrue(CheckPluginExports(beta, 35, true, reason) == PluginCheck::Ok);
			PluginExportProbe none = Complete(35, false);
			none.isBeta = nullptr;
			Assert::IsTrue(CheckPluginExports(none, 35, false, reason) == PluginCheck::FlavorMissing);
		}

		TEST_METHOD(CandidatePathsPreferTheInstallLayout)
		{
			const std::vector<std::wstring> paths = PluginCandidatePaths(L"C:\\VP", false);
			Assert::AreEqual<size_t>(2, paths.size());
			Assert::AreEqual(std::wstring(L"C:\\VP\\vprenderer\\VideoProcessorVPRenderer.dll"), paths[0]);
			Assert::AreEqual(std::wstring(L"C:\\VP\\VideoProcessorVPRenderer.dll"), paths[1]);
			const std::vector<std::wstring> beta = PluginCandidatePaths(L"C:\\VP", true);
			Assert::AreEqual(std::wstring(L"C:\\VP\\vprenderer-beta\\VideoProcessorVPRendererBeta.dll"), beta[0]);
			Assert::IsTrue(PluginCandidatePaths(L"", false).empty());
		}

	private:
		static PluginExportProbe Complete(uint32_t version, bool beta)
		{
			PluginExportProbe probe;
			probe.hasCreate = true;
			probe.hasDestroy = true;
			probe.getApiVersion = [version]() { return version; };
			probe.isBeta = [beta]() { return beta; };
			return probe;
		}
	};


	TEST_CLASS(VPRendererFilterRegistrationTests)
	{
	public:
		TEST_METHOD(ClsidIsTheGeneratedOneAndNoOtherRenderers)
		{
			Assert::AreEqual(std::wstring(L"{54DEA624-D69C-43BF-BAB2-6E04D51F1B95}"),
				GuidString(kClsidVPRendererFilter));
			// madVR and MPC Video Renderer.
			const CLSID madVR = { 0xe1a8b82a, 0x32ce, 0x4b0d,
				{ 0xbe, 0x0d, 0xaa, 0x68, 0xc7, 0x72, 0xe4, 0x23 } };
			const CLSID mpcVr = { 0x71f080aa, 0x8661, 0x4093,
				{ 0xb1, 0x5e, 0x4f, 0x69, 0x03, 0xe7, 0x7d, 0x0a } };
			Assert::IsFalse(IsEqualGUID(kClsidVPRendererFilter, madVR) == TRUE);
			Assert::IsFalse(IsEqualGUID(kClsidVPRendererFilter, mpcVr) == TRUE);
		}

		TEST_METHOD(KeysAreUserScoped)
		{
			Assert::AreEqual(std::wstring(L"Software\\Classes\\CLSID\\{54DEA624-D69C-43BF-BAB2-6E04D51F1B95}"),
				ClsidKeyPath());
			Assert::AreEqual(std::wstring(L"Software\\Classes\\CLSID\\{54DEA624-D69C-43BF-BAB2-6E04D51F1B95}\\InprocServer32"),
				InprocServer32KeyPath());
			Assert::AreEqual(std::wstring(L"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance"),
				LegacyCategoryInstanceKeyPath());
			Assert::AreEqual(std::wstring(L"Software\\Classes\\CLSID\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\Instance\\{54DEA624-D69C-43BF-BAB2-6E04D51F1B95}"),
				FilterInstanceKeyPath());
		}

		TEST_METHOD(NameAndMerit)
		{
			Assert::AreEqual(std::wstring(L"VP Renderer"), std::wstring(kFilterName));
			Assert::AreEqual<DWORD>(MERIT_UNLIKELY, kFilterMerit);
		}

		TEST_METHOD(FilterDataListsOneRenderedPinWithEveryAcceptedType)
		{
			FilterRegistrationData data;
			Assert::AreEqual<DWORD>(2, data.filter.dwVersion);
			Assert::AreEqual<DWORD>(kFilterMerit, data.filter.dwMerit);
			Assert::AreEqual<ULONG>(1, data.filter.cPins2);
			Assert::IsTrue(data.filter.rgPins2 == &data.pin);
			Assert::AreEqual<DWORD>(REG_PINFLAG_B_RENDERER, data.pin.dwFlags);
			Assert::AreEqual<UINT>(1, data.pin.cInstances);
			const std::vector<SubtypeInfo>& subtypes = AcceptedSubtypes();
			Assert::AreEqual<UINT>(static_cast<UINT>(subtypes.size()), data.pin.nMediaTypes);
			for (size_t i = 0; i < subtypes.size(); ++i)
			{
				Assert::IsTrue(IsEqualGUID(*data.pin.lpMediaType[i].clsMajorType, kMediaTypeVideo) == TRUE);
				Assert::IsTrue(IsEqualGUID(*data.pin.lpMediaType[i].clsMinorType, subtypes[i].subtype) == TRUE);
			}
		}
	};


	TEST_CLASS(VPRendererFilterTimingGlueTests)
	{
	public:
		TEST_METHOD(ReferenceTimeMapsToQpc)
		{
			// 10 MHz QPC: 100 ns units map one to one.
			Assert::AreEqual<int64_t>(1000 + 5000000,
				ReferenceTimeToQpc(25000000, 20000000, 1000, 10000000));
			// 3 MHz QPC, due one and a half seconds ahead.
			Assert::AreEqual<int64_t>(77 + 4500000,
				ReferenceTimeToQpc(15000000, 0, 77, 3000000));
			// Late frames map to the past.
			Assert::AreEqual<int64_t>(1000000 - 300000,
				ReferenceTimeToQpc(0, 1000000, 1000000, 3000000));
			// Hours ahead without overflow (24 MHz QPC, 10 h).
			const int64_t tenHours = 10ll * 3600 * 10000000;
			Assert::AreEqual<int64_t>(5 + 10ll * 3600 * 24000000,
				ReferenceTimeToQpc(tenHours, 0, 5, 24000000));
			Assert::AreEqual<int64_t>(0, ReferenceTimeToQpc(1, 0, 5, 0));
		}
	};
}
