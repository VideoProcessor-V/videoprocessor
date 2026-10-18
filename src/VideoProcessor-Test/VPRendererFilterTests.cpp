#include "pch.h"
#include "CppUnitTest.h"

// VP Renderer DirectShow filter: the pure parts (media-type acceptance, stride
// repack, plugin refusal, registration identity, timing arithmetic). The
// filter's .cpp files compiled here are listed in VideoProcessor-Test's
// project; nothing is registered and no plugin is loaded.

#include "../VideoProcessor-VPRendererFilter/FilterTimingGlue.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterMediaTypes.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterPluginCheck.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterRegistration.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererFilterRepack.h"
#include "../VideoProcessor-VPRendererFilter/VPRendererLedGuard.h"

#include <DisplayMode.h>
#include <VideoState.h>
#include <VpBuildIdentity.h>

#include <dvdmedia.h>
#include <IMediaSideData.h>

#include <algorithm>
#include <cmath>
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
			VideoFormat f;
			f.width = 3840;
			f.height = 2160;
			f.colorimetry = c;
			FileColorDecision d = DecideColor(f);
			Assert::IsTrue(d.colorSpace == ColorSpace::BT_2020);
			Assert::IsTrue(d.eotf == EOTF::PQ);
			Assert::IsFalse(d.eotfAssumed);

			f.colorimetry.transferFunction = 16;
			Assert::IsTrue(DecideColor(f).eotf == EOTF::HLG);
			f.colorimetry.primaries = 2;
			f.colorimetry.transferFunction = 4;   // gamma 2.2 class -> SDR
			d = DecideColor(f);
			Assert::IsTrue(d.colorSpace == ColorSpace::REC_709);
			Assert::IsTrue(d.eotf == EOTF::SDR);

			// Nothing signalled: SDR (assumed) and the capture path's height rule.
			const Colorimetry absent = DecodeColorimetry(AMCONTROL_USED | (9u << 22));
			Assert::IsFalse(absent.present);
			f.colorimetry = absent;
			d = DecideColor(f);
			Assert::IsTrue(d.eotf == EOTF::SDR);
			Assert::IsTrue(d.eotfAssumed);
			Assert::IsTrue(d.colorSpace == ColorSpace::REC_709);
			Assert::IsTrue(d.colorSpaceFromHeight);
			f.width = 720;
			f.height = 576;
			Assert::IsTrue(DecideColor(f).colorSpace == ColorSpace::REC_601_625);
			f.height = 480;
			Assert::IsTrue(DecideColor(f).colorSpace == ColorSpace::REC_601_525);
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


	// A well-formed build ID shared by the plugin-check probes and the expected ID.
	static constexpr const char* kId = "0123456789abcdef0123456789abcdef";

	TEST_CLASS(VPRendererFilterPluginCheckTests)
	{
	public:
		TEST_METHOD(AcceptsMatchingVersionAndFlavor)
		{
			PluginExportProbe probe = Complete(38, false);
			std::string reason;
			Assert::IsTrue(CheckPluginExports(probe, 38, kId, false, reason) == PluginCheck::Ok);
		}

		TEST_METHOD(RefusesApiMismatchBeforeAnyOtherCall)
		{
			bool flavorCalled = false;
			PluginExportProbe probe = Complete(34, false);
			probe.isBeta = [&flavorCalled]() { flavorCalled = true; return false; };
			std::string reason;
			Assert::IsTrue(CheckPluginExports(probe, 38, kId, false, reason) == PluginCheck::ApiMismatch);
			Assert::IsFalse(flavorCalled);
			Assert::IsTrue(reason.find("expects 38") != std::string::npos);
			Assert::IsTrue(reason.find("reports 34") != std::string::npos);

			probe = Complete(37, false);
			Assert::IsTrue(CheckPluginExports(probe, 38, kId, false, reason) == PluginCheck::ApiMismatch);
		}

		TEST_METHOD(RefusesMissingExports)
		{
			std::string reason;
			PluginExportProbe noVersion = Complete(38, false);
			noVersion.getApiVersion = nullptr;
			Assert::IsTrue(CheckPluginExports(noVersion, 38, kId, false, reason) == PluginCheck::MissingExports);
			PluginExportProbe noCreate = Complete(38, false);
			noCreate.hasCreate = false;
			Assert::IsTrue(CheckPluginExports(noCreate, 38, kId, false, reason) == PluginCheck::MissingExports);
			PluginExportProbe noDestroy = Complete(38, false);
			noDestroy.hasDestroy = false;
			Assert::IsTrue(CheckPluginExports(noDestroy, 38, kId, false, reason) == PluginCheck::MissingExports);
		}

		TEST_METHOD(RefusesWrongOrMissingFlavor)
		{
			std::string reason;
			PluginExportProbe beta = Complete(38, true);
			Assert::IsTrue(CheckPluginExports(beta, 38, kId, false, reason) == PluginCheck::FlavorMismatch);
			Assert::IsTrue(CheckPluginExports(beta, 38, kId, true, reason) == PluginCheck::Ok);
			PluginExportProbe none = Complete(38, false);
			none.isBeta = nullptr;
			Assert::IsTrue(CheckPluginExports(none, 38, kId, false, reason) == PluginCheck::FlavorMissing);
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

		// Integration 2026-10-17: the filter applies the executable's build-ID pair
		// rule after the API version and before the flavor; no override.
		TEST_METHOD(RefusesAnotherBuildBeforeTheFlavor)
		{
			std::string reason;
			bool flavorCalled = false;
			PluginExportProbe other = Complete(38, false);
			other.getBuildId = []() { return "0123456789abcdef0123456789abcdee"; };
			other.isBeta = [&flavorCalled]() { flavorCalled = true; return false; };
			Assert::IsTrue(CheckPluginExports(other, 38, kId, false, reason) == PluginCheck::BuildIdMismatch);
			Assert::IsFalse(flavorCalled);
			Assert::IsTrue(reason.find("same build") != std::string::npos);
			PluginExportProbe older = Complete(38, false);
			older.getBuildId = nullptr;
			Assert::IsTrue(CheckPluginExports(older, 38, kId, false, reason) == PluginCheck::BuildIdMismatch);
			PluginExportProbe placeholder = Complete(38, false);
			placeholder.getBuildId = []() { return "unknown"; };
			Assert::IsTrue(CheckPluginExports(placeholder, 38, "unknown", false, reason) == PluginCheck::BuildIdMismatch);
			Assert::AreEqual(std::string("build-id-mismatch"), std::string(ToString(PluginCheck::BuildIdMismatch)));
			// The filter's loader passes its own VpBuildIdentity::Id(), a real ID.
			Assert::IsTrue(VpBuildIdentity::IsWellFormed(VpBuildIdentity::Id()));
		}

	private:
		static PluginExportProbe Complete(uint32_t version, bool beta)
		{
			PluginExportProbe probe;
			probe.hasCreate = true;
			probe.hasDestroy = true;
			probe.getApiVersion = [version]() { return version; };
			probe.getBuildId = []() { return kId; };
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


	// A reference clock that only answers GetTime (FileClockMapper's correlation).
	class FakeReferenceClock final : public IReferenceClock
	{
	public:
		REFERENCE_TIME now = 0;
		STDMETHODIMP QueryInterface(REFIID, void** ppv) override { *ppv = nullptr; return E_NOINTERFACE; }
		STDMETHODIMP_(ULONG) AddRef() override { return 2; }
		STDMETHODIMP_(ULONG) Release() override { return 1; }
		STDMETHODIMP GetTime(REFERENCE_TIME* time) override { *time = now; return S_OK; }
		STDMETHODIMP AdviseTime(REFERENCE_TIME, REFERENCE_TIME, HEVENT, DWORD_PTR*) override { return E_NOTIMPL; }
		STDMETHODIMP AdvisePeriodic(REFERENCE_TIME, REFERENCE_TIME, HSEMAPHORE, DWORD_PTR*) override { return E_NOTIMPL; }
		STDMETHODIMP Unadvise(DWORD_PTR) override { return E_NOTIMPL; }
	};

	// 10 MHz QPC so 100 ns reference units map one to one.
	filesource::ClockCorrelation Correlation(int64_t reference, int64_t qpc)
	{
		filesource::ClockCorrelation c;
		c.referenceTime100ns = reference;
		c.qpcTicks = qpc;
		c.qpcFrequency = 10000000;
		return c;
	}

	filesource::FileSampleTimes Times(int64_t start, int64_t duration, bool discontinuity = false)
	{
		filesource::FileSampleTimes t;
		t.hasStart = true;
		t.hasStop = true;
		t.start100ns = start;
		t.stop100ns = start + duration;
		t.discontinuityFlag = discontinuity;
		return t;
	}

	struct SideDataSource
	{
		std::vector<std::pair<GUID, std::vector<BYTE>>> items;
		static HRESULT Get(void* context, const GUID& type, const BYTE** data, size_t* size)
		{
			for (const auto& item : static_cast<SideDataSource*>(context)->items)
			{
				if (IsEqualGUID(item.first, type))
				{
					*data = item.second.data();
					*size = item.second.size();
					return S_OK;
				}
			}
			return E_FAIL;
		}
	};

	template <typename T>
	std::vector<BYTE> Bytes(const T& value)
	{
		const BYTE* p = reinterpret_cast<const BYTE*>(&value);
		return std::vector<BYTE>(p, p + sizeof(T));
	}

	TEST_CLASS(VPRendererFilterTimingGlueTests)
	{
	public:
		TEST_METHOD(VideoStateCarriesExactRateColorAndHdr)
		{
			TestMediaType t(kSubtypeP010, 3840, 2160);
			t.vih2.dwControlFlags = AMCONTROL_COLORINFO_PRESENT | AMCONTROL_USED |
				(2u << 12) | (4u << 15) | (9u << 22) | (15u << 27);
			VideoFormat f;
			std::string reason;
			Assert::IsTrue(ParseVideoMediaType(t.mt, f, reason));
			const RationalRate rate = filesource::SnapAvgTimePerFrame(f.avgTimePerFrame);
			Assert::AreEqual<uint32_t>(24000, rate.numerator);
			Assert::AreEqual<uint32_t>(1001, rate.denominator);

			HDRData hdr;
			hdr.masteringDisplayMaxLuminance = 1000;
			hdr.maxCll = 900;
			VideoStateComPtr state;
			Assert::IsTrue(BuildFileVideoState(f, rate, &hdr, state, reason));
			Assert::IsTrue(state->valid);
			Assert::IsTrue(state->videoFrameEncoding == VideoFrameEncoding::P010);
			Assert::IsTrue(state->eotf == EOTF::PQ);
			Assert::IsTrue(state->colorspace == ColorSpace::BT_2020);
			Assert::AreEqual(3840u, state->displayMode->FrameWidth());
			Assert::AreEqual(1000.0, state->hdrData->masteringDisplayMaxLuminance);
			Assert::AreEqual(900.0, state->hdrData->maxCll);

			// No HDR side data: an empty HDRData, never null.
			Assert::IsTrue(BuildFileVideoState(f, rate, nullptr, state, reason));
			Assert::IsNotNull(state->hdrData.get());
			Assert::IsFalse(state->hdrData->IsValid());

			// An unknown rate is refused with a reason (the filter waits for PTS).
			Assert::IsFalse(BuildFileVideoState(f, RationalRate(), nullptr, state, reason));
			Assert::IsFalse(reason.empty());
		}

		TEST_METHOD(RateFromHeaderOrTimestamps)
		{
			FileTimingModel model;
			// Header 23.976: valid at once, still valid while the PTS agree.
			RationalRate r = model.ObserveSample(true, 0, false, 417083);
			Assert::AreEqual<uint32_t>(24000, r.numerator);
			for (int i = 1; i < 12; ++i)
				r = model.ObserveSample(true, (i * 4170833LL + 5) / 10, false, 417083);
			Assert::AreEqual<uint32_t>(24000, r.numerator);
			Assert::AreEqual<uint32_t>(1001, r.denominator);

			// No header: unknown until the timestamps give 25 Hz.
			FileTimingModel noHeader;
			r = noHeader.ObserveSample(true, 0, false, 0);
			Assert::IsFalse(r.IsValid());
			for (int i = 1; i < 12; ++i)
				r = noHeader.ObserveSample(true, i * 400000LL, false, 0);
			Assert::AreEqual<uint32_t>(25, r.numerator);
			Assert::AreEqual<uint32_t>(1, r.denominator);

			// A header that disagrees with the stream (claims 25, runs 50): measured wins.
			FileTimingModel wrong;
			for (int i = 0; i < 12; ++i)
				r = wrong.ObserveSample(true, i * 200000LL, false, 400000);
			Assert::AreEqual<uint32_t>(50, r.numerator);
		}

		TEST_METHOD(PresentationQpcFollowsClockOffsetAndState)
		{
			FileTimingModel model;
			model.AddCorrelation(Correlation(100000000, 500000000));
			model.AddCorrelation(Correlation(110000000, 510000000));
			const RationalRate rate = filesource::SnapAvgTimePerFrame(400000);

			// Paused (pre-roll): held, shown at once, first frame is a discontinuity.
			model.Pause();
			FileFrameTiming t = model.BuildTiming(Times(0, 400000), rate);
			Assert::IsTrue(t.paused);
			Assert::AreEqual<int64_t>(0, t.presentationQpc);
			Assert::IsTrue(t.discontinuity);
			Assert::AreEqual<uint32_t>(25, t.sourceRateNumerator);

			// Running from tStart = 120000000: due at tStart + rtStart.
			model.Run(120000000);
			t = model.BuildTiming(Times(400000, 400000), rate);
			Assert::IsFalse(t.paused);
			Assert::IsFalse(t.discontinuity);
			Assert::AreEqual<int64_t>(500000000 + 20000000 + 400000, t.presentationQpc);
			Assert::IsTrue(t.isFileSource);

			// file_av_offset_ms = -40: 40 ms earlier.
			model.SetAvOffsetMs(-40);
			t = model.BuildTiming(Times(800000, 400000), rate);
			Assert::AreEqual<int64_t>(500000000 + 20000000 + 800000 - 400000, t.presentationQpc);
			model.SetAvOffsetMs(0);

			// A seek (flush + new segment) and a format change are discontinuities.
			model.BeginFlush();
			model.EndFlush();
			Assert::IsTrue(model.BuildTiming(Times(0, 400000), rate).discontinuity);
			Assert::IsFalse(model.BuildTiming(Times(400000, 400000), rate).discontinuity);
			model.MarkDiscontinuity();
			Assert::IsTrue(model.BuildTiming(Times(800000, 400000), rate).discontinuity);
			Assert::IsTrue(model.BuildTiming(Times(1200000, 400000, true), rate).discontinuity);

			// Rate 2.0: carried to VP, motion off, and the rate change is a discontinuity.
			Assert::IsTrue(model.MotionAllowed());
			Assert::IsTrue(model.NewSegment(0, 0, 2.0));
			t = model.BuildTiming(Times(0, 200000), rate);
			Assert::AreEqual(2.0, t.playbackRate);
			Assert::IsTrue(t.discontinuity);
			Assert::IsFalse(model.MotionAllowed());

			// A frame step: held and a discontinuity.
			model.FrameStep();
			t = model.BuildTiming(Times(200000, 200000), rate);
			Assert::IsTrue(t.paused);
			Assert::IsTrue(t.discontinuity);

			// Stop forgets the clock fit: nothing can be mapped until it is rebuilt.
			model.Stop();
			model.Run(0);
			Assert::AreEqual<int64_t>(0, model.BuildTiming(Times(0, 400000), rate).presentationQpc);
		}

		TEST_METHOD(LedGuardFiresOnlyForThePlayerWindowItself)
		{
			const HWND root = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
			const HWND other = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x5678));
			Assert::IsTrue(IsRootDestroyed(root, EVENT_OBJECT_DESTROY, root, OBJID_WINDOW, CHILDID_SELF));
			Assert::IsFalse(IsRootDestroyed(root, EVENT_OBJECT_DESTROY, other, OBJID_WINDOW, CHILDID_SELF));
			Assert::IsFalse(IsRootDestroyed(root, EVENT_OBJECT_DESTROY, root, OBJID_CARET, CHILDID_SELF));
			Assert::IsFalse(IsRootDestroyed(root, EVENT_OBJECT_DESTROY, root, OBJID_WINDOW, 3));
			Assert::IsFalse(IsRootDestroyed(root, EVENT_OBJECT_HIDE, root, OBJID_WINDOW, CHILDID_SELF));
			Assert::IsFalse(IsRootDestroyed(nullptr, EVENT_OBJECT_DESTROY, root, OBJID_WINDOW, CHILDID_SELF));
		}

		TEST_METHOD(CorrelationFromAReferenceClock)
		{
			FakeReferenceClock clock;
			clock.now = 123456789;
			filesource::ClockCorrelation c;
			Assert::IsTrue(filesource::SampleReferenceClockCorrelation(&clock, c));
			Assert::AreEqual<int64_t>(123456789, c.referenceTime100ns);
			Assert::IsTrue(c.qpcTicks > 0 && c.qpcFrequency > 0);
		}

		TEST_METHOD(SideDataTrackerReportsStaticHdrChangesOnly)
		{
			MediaSideDataHDR mastering = {};
			mastering.display_primaries_x[0] = 0.170;   // LAV order: G, B, R
			mastering.display_primaries_y[0] = 0.797;
			mastering.display_primaries_x[1] = 0.131;
			mastering.display_primaries_y[1] = 0.046;
			mastering.display_primaries_x[2] = 0.708;
			mastering.display_primaries_y[2] = 0.292;
			mastering.white_point_x = 0.3127;
			mastering.white_point_y = 0.3290;
			mastering.max_display_mastering_luminance = 1000.0;
			mastering.min_display_mastering_luminance = 0.005;
			MediaSideDataHDRContentLightLevel cll = {};
			cll.MaxCLL = 900;
			cll.MaxFALL = 300;

			SideDataSource source;
			source.items.push_back(std::make_pair(IID_MediaSideDataHDR, Bytes(mastering)));
			source.items.push_back(std::make_pair(IID_MediaSideDataHDRContentLightLevel, Bytes(cll)));

			FileSideDataTracker tracker;
			Assert::IsFalse(tracker.HasStaticHdr());
			Assert::IsTrue(tracker.Update(&SideDataSource::Get, &source));
			Assert::IsTrue(tracker.HasStaticHdr());
			Assert::AreEqual(1000.0, tracker.StaticHdr().masteringDisplayMaxLuminance);
			Assert::AreEqual(900.0, tracker.StaticHdr().maxCll);
			Assert::AreEqual(0.708, tracker.StaticHdr().displayPrimaryRedX, 1e-9);
			// The same metadata again: no change. None on a sample: kept.
			Assert::IsFalse(tracker.Update(&SideDataSource::Get, &source));
			SideDataSource empty;
			Assert::IsFalse(tracker.Update(&SideDataSource::Get, &empty));
			Assert::IsTrue(tracker.HasStaticHdr());
			// A new MaxCLL is a change; a truncated block is ignored.
			cll.MaxCLL = 1200;
			source.items[1].second = Bytes(cll);
			Assert::IsTrue(tracker.Update(&SideDataSource::Get, &source));
			Assert::AreEqual(1200.0, tracker.StaticHdr().maxCll);
			source.items[0].second.resize(8);
			Assert::IsFalse(tracker.Update(&SideDataSource::Get, &source));
			tracker.Reset();
			Assert::IsFalse(tracker.HasStaticHdr());
			Assert::AreEqual<uint64_t>(0, tracker.DolbyVisionFrames());
		}
	};


	// Bug hunt 3 (C1): the filter stalled for good after a media-type change,
	// a rate change or a seek. A stand-in renderer refuses a state in place
	// exactly as LibplaceboVideoRenderer::OnVideoState does (another display
	// mode - size, interlacing or rate - or pixel format; optionally any EOTF
	// change, as an effective profile change does); the filter model below
	// runs the steps of DeliverSample (ObserveSample, the state tracker's
	// Ensure, BuildTiming) for each sample.
	namespace
	{
		class StandInRenderer final : public IFileVideoStateSink
		{
		public:
			bool SendVideoState(VideoStateComPtr& state) override
			{
				++offered;
				if (accepted && (!state->displayMode || !accepted->displayMode ||
					*state->displayMode != *accepted->displayMode ||
					state->videoFrameEncoding != accepted->videoFrameEncoding ||
					(refuseEotfChange && state->eotf != accepted->eotf)))
					return false;
				accepted = state;
				return true;
			}
			bool RestartRenderer(std::string& reason) override
			{
				if (restartFails)
				{
					reason = "no renderer";
					return false;
				}
				++generation;
				accepted = nullptr;
				started = false;
				return true;
			}
			void StartRestartedRenderer() override { started = true; }

			VideoStateComPtr accepted;
			int generation = 1;
			int offered = 0;
			bool started = true;
			bool refuseEotfChange = false;
			bool restartFails = false;
		};

		// The filter's per-sample path over a stream with Matroska-style 1 ms
		// timestamps, rate-adjusted by the source as DirectShow sources do.
		struct FilterModel
		{
			FileTimingModel timing;
			FileVideoStateTracker tracker;
			StandInRenderer renderer;
			VideoFormat format;
			uint64_t generation = 1;
			RationalRate exact;
			double rate = 1.0;
			int64_t segmentStartFrame = 0;
			int64_t frame = 0;
			bool paused = false;
			int delivered = 0;
			int refused = 0;
			FileFrameTiming last;
			bool haveHdr = false;
			HDRData hdr;

			FilterModel()
			{
				exact.numerator = 24000;
				exact.denominator = 1001;
				format.encoding = VideoFrameEncoding::P010;
				format.width = 1920;
				format.height = 1080;
				format.avgTimePerFrame = 417083;
				timing.Run(0);
			}

			// Stream time of a frame in ms (rounded as Matroska stores it),
			// relative to the segment start and divided by the playback rate.
			int64_t SampleStart(int64_t n) const
			{
				const double ms = std::floor((double)n * 1000.0 * exact.denominator / exact.numerator + 0.5);
				const double segmentMs = std::floor((double)segmentStartFrame * 1000.0 * exact.denominator /
					exact.numerator + 0.5);
				return static_cast<int64_t>(std::llround((ms - segmentMs) * 10000.0 / rate));
			}

			bool Sample()
			{
				const int64_t start = SampleStart(frame++);
				const RationalRate observed = timing.ObserveSample(!paused, start, false,
					format.avgTimePerFrame, tracker.SentRate());
				std::string reason;
				const FileStateOutcome outcome = tracker.Ensure(format, generation, observed,
					haveHdr ? &hdr : nullptr, renderer, reason);
				if (outcome == FileStateOutcome::Deferred || outcome == FileStateOutcome::Failed ||
					!renderer.accepted || !renderer.started)
				{
					++refused;
					return false;
				}
				last = timing.BuildTiming(Times(start, 417083), tracker.SentRate());
				++delivered;
				return true;
			}

			// n samples; true when every one of the last `tail` was delivered.
			bool Play(int n, int tail = 10)
			{
				int run = 0;
				for (int i = 0; i < n; ++i)
					run = Sample() ? run + 1 : 0;
				return run >= tail;
			}

			// Seek: flush, then the new segment from the target frame.
			void Seek(int64_t toFrame)
			{
				timing.BeginFlush();
				timing.EndFlush();
				segmentStartFrame = frame = toFrame;
				timing.NewSegment(0, -1, rate);
			}

			// A rate change: the source flushes and starts a segment at the new rate.
			void SetRate(double newRate)
			{
				timing.BeginFlush();
				timing.EndFlush();
				rate = newRate;
				segmentStartFrame = frame;
				timing.NewSegment(0, -1, rate);
			}

			// A dynamic media-type change (SetMediaType on a sample).
			void ChangeFormat(uint32_t width, uint32_t height, VideoFrameEncoding encoding)
			{
				format.width = width;
				format.height = height;
				format.encoding = encoding;
				++generation;
				timing.MarkDiscontinuity();
			}

			void Pause() { paused = true; timing.Pause(); }
			void Resume() { paused = false; timing.Run(0); }
		};
	}

	TEST_CLASS(VPRendererFilterStreamChangeTests)
	{
	public:
		// The rate verifier: 1 ms container timestamps of a 23.976 stream are
		// 41 or 42 ms apart, yet agree with the 417083 header.
		TEST_METHOD(QuantizedTimestampsAgreeWithTheHeaderRate)
		{
			FilterModel m;
			RationalRate r;
			for (int i = 0; i < 60; ++i)
				r = m.timing.ObserveSample(true, m.SampleStart(i), false, 417083);
			Assert::AreEqual<uint32_t>(24000, r.numerator, L"the header rate stands");
			Assert::AreEqual<uint32_t>(1001, r.denominator);

			// Without a header the mean period over the 1 ms stamps finds 23.976.
			filesource::PtsRateVerifier v;
			for (int i = 0; i < 240; ++i)
				v.AddSampleStart(m.SampleStart(i));
			const RationalRate measured = v.MeasuredRate();
			Assert::AreEqual<uint32_t>(24000, measured.numerator);
			Assert::AreEqual<uint32_t>(1001, measured.denominator);
			// A 25 fps claim still disagrees.
			Assert::IsTrue(v.Check(filesource::SnapAvgTimePerFrame(400000)) ==
				filesource::PtsRateVerifier::Verdict::Disagrees);
		}

		TEST_METHOD(PlaybackRateChangesKeepTheSourceRateAndNeverRestart)
		{
			FilterModel m;
			Assert::IsTrue(m.Play(40), L"plays at 1x");
			for (const double rate : { 2.0, 1.0, 0.5, 1.0, 4.0, 1.0 })
			{
				m.SetRate(rate);
				Assert::IsTrue(m.Play(40), L"plays after the rate change");
				Assert::AreEqual(rate, m.last.playbackRate, L"the speed goes to VP");
				Assert::AreEqual<uint32_t>(24000, m.tracker.SentRate().numerator, L"the source rate is unchanged");
				Assert::AreEqual<uint32_t>(1001, m.tracker.SentRate().denominator);
			}
			Assert::AreEqual<uint32_t>(0, m.tracker.Restarts(), L"no renderer restart for a speed change");
			Assert::AreEqual(0, m.refused);
		}

		TEST_METHOD(MediaTypeChangesRestartTheRendererOnceAndKeepDelivering)
		{
			FilterModel m;
			Assert::IsTrue(m.Play(20));
			// A new size: refused in place, so one restart; frames flow again at once.
			m.ChangeFormat(3840, 2160, VideoFrameEncoding::P010);
			Assert::IsTrue(m.Sample(), L"the first sample after the change is delivered");
			Assert::AreEqual<uint32_t>(1, m.tracker.Restarts());
			Assert::AreEqual(2, m.renderer.generation);
			Assert::AreEqual(3840u, m.renderer.accepted->displayMode->FrameWidth());
			Assert::IsTrue(m.Play(20));
			// A new pixel format (P010 -> NV12): another restart.
			m.ChangeFormat(3840, 2160, VideoFrameEncoding::NV12);
			Assert::IsTrue(m.Play(20));
			Assert::AreEqual<uint32_t>(2, m.tracker.Restarts());
			Assert::IsTrue(m.renderer.accepted->videoFrameEncoding == VideoFrameEncoding::NV12);
			// A change the renderer takes in place (same mode and format): no restart.
			m.ChangeFormat(3840, 2160, VideoFrameEncoding::NV12);
			Assert::IsTrue(m.Play(20));
			Assert::AreEqual<uint32_t>(2, m.tracker.Restarts());
			Assert::AreEqual(0, m.refused);
		}

		TEST_METHOD(StaticHdrArrivingLaterIsSentAndAProfileChangeRestarts)
		{
			FilterModel m;
			m.renderer.refuseEotfChange = true;   // the effective profile differs for PQ
			Assert::IsTrue(m.Play(10));
			Assert::IsTrue(m.renderer.accepted->eotf == EOTF::SDR);
			m.haveHdr = true;
			m.hdr.masteringDisplayMaxLuminance = 1000;
			m.hdr.maxCll = 1000;
			Assert::IsTrue(m.Play(10));
			// No transfer signalled + HDR10 static metadata: PQ (C7), via a restart.
			Assert::IsTrue(m.renderer.accepted->eotf == EOTF::PQ);
			Assert::AreEqual<uint32_t>(1, m.tracker.Restarts());
			Assert::AreEqual(1000.0, m.renderer.accepted->hdrData->maxCll);
		}

		TEST_METHOD(SeeksAndFlushesInAnyOrderKeepDelivering)
		{
			FilterModel m;
			Assert::IsTrue(m.Play(30));
			// Seek forward, back, to the start and far ahead.
			for (const int64_t target : { 5000LL, 120LL, 0LL, 99999LL })
			{
				m.Seek(target);
				Assert::IsTrue(m.Sample(), L"the first sample after a seek is delivered");
				Assert::IsTrue(m.last.discontinuity, L"a seek is a discontinuity for VP");
				Assert::IsTrue(m.Play(30));
			}
			// NewSegment before the flush, and two segments in a row.
			m.timing.NewSegment(0, -1, 1.0);
			m.timing.BeginFlush();
			m.timing.EndFlush();
			Assert::IsTrue(m.Play(10));
			m.timing.NewSegment(0, -1, 1.0);
			m.timing.NewSegment(0, -1, 1.0);
			Assert::IsTrue(m.Play(10));
			Assert::AreEqual<uint32_t>(0, m.tracker.Restarts());
			Assert::AreEqual(0, m.refused);
		}

		TEST_METHOD(PauseAndResumeHoldThenPlayWithoutRestart)
		{
			FilterModel m;
			Assert::IsTrue(m.Play(20));
			m.Pause();
			Assert::IsTrue(m.Sample());
			Assert::IsTrue(m.last.paused, L"held while paused");
			Assert::AreEqual<int64_t>(0, m.last.presentationQpc);
			m.Resume();
			Assert::IsTrue(m.Play(20));
			Assert::IsFalse(m.last.paused);
			// Pause, seek while paused, resume.
			m.Pause();
			m.Seek(300);
			Assert::IsTrue(m.Sample());
			m.Resume();
			Assert::IsTrue(m.Play(20));
			Assert::AreEqual<uint32_t>(0, m.tracker.Restarts());
		}

		// Every order of the four events, twice over: never a permanent stall.
		TEST_METHOD(EveryOrderOfFormatRateSeekAndPauseKeepsDelivering)
		{
			int events[4] = { 0, 1, 2, 3 };
			int orders = 0;
			do
			{
				FilterModel m;
				Assert::IsTrue(m.Play(20));
				bool big = false;
				for (int round = 0; round < 2; ++round)
					for (const int e : events)
					{
						switch (e)
						{
						case 0:
							big = !big;
							m.ChangeFormat(big ? 3840 : 1920, big ? 2160 : 1080, VideoFrameEncoding::P010);
							break;
						case 1: m.SetRate(m.rate == 1.0 ? 2.0 : 1.0); break;
						case 2: m.Seek(m.frame / 2); break;
						case 3: m.Pause(); m.Sample(); m.Resume(); break;
						}
						Assert::IsTrue(m.Play(25), (L"stalled after event " + std::to_wstring(e) +
							L" in order " + std::to_wstring(orders)).c_str());
					}
				Assert::AreEqual<uint32_t>(2, m.tracker.Restarts(), L"restarts only for the size changes");
				Assert::AreEqual(0, m.refused);
				++orders;
			} while (std::next_permutation(events, events + 4));
			Assert::AreEqual(24, orders);
		}

		TEST_METHOD(HeaderlessStreamMeasuresOnceAndKeepsItsRate)
		{
			FilterModel m;
			m.format.avgTimePerFrame = 0;
			Assert::IsFalse(m.Sample(), L"no rate yet: deferred");
			Assert::IsTrue(m.Play(200));
			Assert::AreEqual<uint32_t>(24000, m.tracker.SentRate().numerator);
			Assert::AreEqual<uint32_t>(1001, m.tracker.SentRate().denominator);
			const uint32_t restarts = m.tracker.Restarts();
			Assert::IsTrue(restarts <= 1, L"at most one refinement of the first measurement");
			// A seek (no rate measured yet afterwards) keeps the sent rate.
			m.Seek(1000);
			Assert::IsTrue(m.Sample(), L"delivered at once after the seek");
			Assert::IsTrue(m.Play(100));
			Assert::AreEqual(restarts, m.tracker.Restarts());
		}

		TEST_METHOD(AFailedRestartRecoversOnALaterSample)
		{
			FilterModel m;
			Assert::IsTrue(m.Play(10));
			m.renderer.restartFails = true;
			m.ChangeFormat(1280, 720, VideoFrameEncoding::P010);
			Assert::IsFalse(m.Sample(), L"no renderer could be made");
			Assert::IsFalse(m.Sample());
			m.renderer.restartFails = false;
			Assert::IsTrue(m.Play(10), L"the next sample that can restart does");
			Assert::AreEqual(720u, m.renderer.accepted->displayMode->FrameHeight());
		}

		TEST_METHOD(UnsignalledTransferWithHdr10MetadataIsPq)
		{
			VideoFormat f;
			f.encoding = VideoFrameEncoding::P010;
			f.width = 3840;
			f.height = 2160;
			Assert::IsTrue(DecideColor(f).eotf == EOTF::SDR, L"no metadata: SDR (assumed)");
			const FileColorDecision d = DecideColor(f, true);
			Assert::IsTrue(d.eotf == EOTF::PQ, L"HDR10 static metadata: PQ");
			Assert::IsTrue(d.eotfAssumed);
		}
	};
}
