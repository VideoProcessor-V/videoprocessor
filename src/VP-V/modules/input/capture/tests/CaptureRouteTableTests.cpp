// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-0 (doc 59 sections 1.5 and 5.3): the recorded capture route table. LegacyRoute below is a test-only,
// verbatim copy of the route decision ConversionLoop made inline before P10-3 (LibplaceboVideoRenderer.cpp at
// ed1eca65, the rgbRaw / dvRaw terms and the if / else-if chain), with each host read replaced by the matching
// fact. Vpv::Capture::ChooseRoute must give the same route for every combination of the 15 facts (32768 rows).
// CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/capture/CaptureRoutes.h>
#include <VP-V/modules/input/capture/CaptureRouteFacts.h>
#include <VP-V/modules/input/capture/CapturePayload.h>
#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>
#include <VideoFrame.h>

#include <memory>
#include <vector>

#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using Vpv::Capture::ConvertRoute;
		using Vpv::Capture::RouteFacts;

		// The pre-P10 text (names of the host reads kept in the comments).
		ConvertRoute LegacyRoute(const RouteFacts& f)
		{
			const bool rgbRaw = !f.dolbyVisionTunnel /* !state->dolbyVisionTunnel */ &&
				f.rgbWanted /* m_impl->rgbGpuWanted */ &&
				!f.rgbFailed /* !m_impl->rgbGpuFailed */ &&
				f.overrideNone /* m_videoConversionOverride == VIDEOCONVERSION_NONE */ &&
				f.rgbSupports /* GpuRgbUnpack::Supports(state->videoFrameEncoding) */ && f.displayMode /* state->displayMode */ &&
				(!f.twelveBitRgb /* encoding != R12B && encoding != R12L */ ||
					f.widthMultipleOf8 /* state->displayMode->FrameWidth() % 8 == 0 */);
			const bool dvRaw = f.dolbyVisionTunnel &&
				f.dvWanted /* m_impl->dvGpuWanted */ &&
				!f.dvFailed /* !m_impl->dvGpuFailed */ &&
				f.overrideNone &&
				f.dvSupports /* GpuDolbyVisionTunnelUnpack::Supports(state->videoFrameEncoding) */ && f.displayMode;
			if (!rgbRaw && f.nativeRgbUpload /* IsNativeRgbUpload(encoding, override, state->dolbyVisionTunnel) */)
				return ConvertRoute::NativeRgbEmpty;
			else if (rgbRaw ||
				(f.captureWanted /* m_impl->captureGpuWanted */ &&
				!f.captureFailed /* !m_impl->captureGpuFailed */ &&
				f.overrideNone &&
				f.captureSupports /* GpuCaptureUnpack::Supports(state->videoFrameEncoding) */))
				return rgbRaw ? ConvertRoute::RawRgb : ConvertRoute::RawCapture;   // makeRaw(); rgbRaw: the RGB contract
			else if (dvRaw)
				return ConvertRoute::DvAnalyse;
			else
				return ConvertRoute::CpuFormatter;
		}

		RouteFacts FactsOf(uint32_t bits)
		{
			RouteFacts f;
			f.dolbyVisionTunnel = (bits & (1u << 0)) != 0;
			f.overrideNone = (bits & (1u << 1)) != 0;
			f.displayMode = (bits & (1u << 2)) != 0;
			f.rgbWanted = (bits & (1u << 3)) != 0;
			f.rgbFailed = (bits & (1u << 4)) != 0;
			f.rgbSupports = (bits & (1u << 5)) != 0;
			f.twelveBitRgb = (bits & (1u << 6)) != 0;
			f.widthMultipleOf8 = (bits & (1u << 7)) != 0;
			f.nativeRgbUpload = (bits & (1u << 8)) != 0;
			f.captureWanted = (bits & (1u << 9)) != 0;
			f.captureFailed = (bits & (1u << 10)) != 0;
			f.captureSupports = (bits & (1u << 11)) != 0;
			f.dvWanted = (bits & (1u << 12)) != 0;
			f.dvFailed = (bits & (1u << 13)) != 0;
			f.dvSupports = (bits & (1u << 14)) != 0;
			return f;
		}
		constexpr uint32_t kRows = 1u << 15;
	}

	TEST_CLASS(CaptureRouteTableTests)
	{
	public:
		TEST_METHOD(ChooseRouteMatchesTheRecordedDecisionForEveryFactCombination)
		{
			uint32_t counts[5] = {};
			for (uint32_t bits = 0; bits < kRows; ++bits)
			{
				const RouteFacts f = FactsOf(bits);
				const ConvertRoute legacy = LegacyRoute(f);
				const ConvertRoute now = Vpv::Capture::ChooseRoute(f);
				if (legacy != now)
					Assert::Fail((L"route differs for fact bits " + std::to_wstring(bits)).c_str());
				++counts[static_cast<int>(now)];
			}
			// Plausibility: every route is reachable, and the table is the full product.
			uint32_t total = 0;
			for (uint32_t c : counts)
			{
				Assert::IsTrue(c > 0, L"every route occurs in the table");
				total += c;
			}
			Assert::AreEqual(kRows, total);
		}

		TEST_METHOD(OriginalPathwayFactsTakeTheHostFormatterOrTheNativeUpload)
		{
			// Original forces capture_path standard, native_rgb_ingest off and dv_tunnel_gpu_unpack off
			// (OriginalPathway.h): no raw route remains, whatever the encoding or the failure flags.
			for (uint32_t bits = 0; bits < kRows; ++bits)
			{
				RouteFacts f = FactsOf(bits);
				f.captureWanted = f.rgbWanted = f.dvWanted = false;
				const ConvertRoute r = Vpv::Capture::ChooseRoute(f);
				Assert::IsTrue(r == ConvertRoute::CpuFormatter || r == ConvertRoute::NativeRgbEmpty, L"Original: no raw route");
				Assert::IsTrue((r == ConvertRoute::NativeRgbEmpty) == f.nativeRgbUpload, L"the native upload exactly when libplacebo's");
			}
		}

		TEST_METHOD(DolbyVisionTunnelNeverTakesTheRgbRoute)
		{
			for (uint32_t bits = 0; bits < kRows; ++bits)
			{
				RouteFacts f = FactsOf(bits);
				f.dolbyVisionTunnel = true;
				Assert::IsFalse(Vpv::Capture::ChooseRoute(f) == ConvertRoute::RawRgb, L"the tunnel's r210 words are not RGB");
			}
		}

		TEST_METHOD(TwelveBitRgbNeedsAWidthMultipleOfEight)
		{
			RouteFacts f;
			f.rgbWanted = f.rgbSupports = f.displayMode = f.twelveBitRgb = true;
			f.widthMultipleOf8 = false;
			Assert::IsFalse(Vpv::Capture::ChooseRoute(f) == ConvertRoute::RawRgb);
			f.widthMultipleOf8 = true;
			Assert::IsTrue(Vpv::Capture::ChooseRoute(f) == ConvertRoute::RawRgb);
		}

		// Audit F3: FactsOf on real states, against the old expressions as they read the host (state->..., the
		// Supports functions, the width rule), for every encoding x tunnel x override x display mode x switches.
		TEST_METHOD(FactsOfMatchesTheOldExpressionsOnRealStates)
		{
			const VideoFrameEncoding encodings[] = { VideoFrameEncoding::UNKNOWN, VideoFrameEncoding::UYVY, VideoFrameEncoding::HDYC,
				VideoFrameEncoding::V210, VideoFrameEncoding::ARGB_8BIT, VideoFrameEncoding::BGRA_8BIT, VideoFrameEncoding::R210,
				VideoFrameEncoding::R10b, VideoFrameEncoding::R10l, VideoFrameEncoding::R12B, VideoFrameEncoding::R12L,
				VideoFrameEncoding::H265, VideoFrameEncoding::DNxHR, VideoFrameEncoding::P210, VideoFrameEncoding::P010,
				VideoFrameEncoding::NV12, VideoFrameEncoding::P016 };
			const VideoConversionOverride overrides[] = { VideoConversionOverride::VIDEOCONVERSION_NONE,
				VideoConversionOverride::VIDEOCONVERSION_V210_TO_P010 };
			const int widths[] = { 0 /* no display mode */, 1920, 1922 };
			uint32_t rows = 0, raw = 0;
			for (VideoFrameEncoding e : encodings)
				for (int dv = 0; dv < 2; ++dv)
					for (VideoConversionOverride o : overrides)
						for (int w : widths)
							for (uint32_t bits = 0; bits < 64; ++bits)
								for (int native = 0; native < 2; ++native)
								{
									VideoStateComPtr state = new VideoState();
									state->videoFrameEncoding = e;
									state->dolbyVisionTunnel = dv != 0;
									if (w > 0)
										state->displayMode = std::make_shared<DisplayMode>(w, 1080, false, 24000, 1001);
									Vpv::Capture::RouteFlags flags;
									flags.rgbWanted = (bits & 1) != 0; flags.rgbFailed = (bits & 2) != 0;
									flags.captureWanted = (bits & 4) != 0; flags.captureFailed = (bits & 8) != 0;
									flags.dvWanted = (bits & 16) != 0; flags.dvFailed = (bits & 32) != 0;
									// The pre-P10 terms, verbatim in their host form.
									const bool rgbRaw = !state->dolbyVisionTunnel && flags.rgbWanted && !flags.rgbFailed &&
										o == VideoConversionOverride::VIDEOCONVERSION_NONE &&
										GpuRgbUnpack::Supports(state->videoFrameEncoding) && state->displayMode &&
										((state->videoFrameEncoding != VideoFrameEncoding::R12B && state->videoFrameEncoding != VideoFrameEncoding::R12L) ||
											state->displayMode->FrameWidth() % 8 == 0);
									const bool dvRaw = state->dolbyVisionTunnel && flags.dvWanted && !flags.dvFailed &&
										o == VideoConversionOverride::VIDEOCONVERSION_NONE &&
										GpuDolbyVisionTunnelUnpack::Supports(state->videoFrameEncoding) && state->displayMode;
									ConvertRoute legacy = ConvertRoute::CpuFormatter;
									if (!rgbRaw && native != 0)
										legacy = ConvertRoute::NativeRgbEmpty;
									else if (rgbRaw || (flags.captureWanted && !flags.captureFailed &&
										o == VideoConversionOverride::VIDEOCONVERSION_NONE && GpuCaptureUnpack::Supports(state->videoFrameEncoding)))
										legacy = rgbRaw ? ConvertRoute::RawRgb : ConvertRoute::RawCapture;
									else if (dvRaw)
										legacy = ConvertRoute::DvAnalyse;
									const ConvertRoute now = Vpv::Capture::ChooseRoute(Vpv::Capture::FactsOf(*state, o, native != 0, flags));
									if (now != legacy)
										Assert::Fail((L"route differs: encoding " + std::to_wstring(static_cast<int>(e)) + L" bits " +
											std::to_wstring(bits)).c_str());
									++rows;
									raw += now == ConvertRoute::RawRgb || now == ConvertRoute::RawCapture || now == ConvertRoute::DvAnalyse ? 1 : 0;
								}
			Assert::AreEqual(17u * 2u * 2u * 3u * 64u * 2u, rows);
			Assert::IsTrue(raw > 0, L"plausibility: real states reach the raw routes");
		}

		// Audit F3: the AfterPublish gate (CaptureAfterPublish runs the early upload only for a slot with a hand-off and
		// its frame; a null slot, or one without them, is skipped).
		TEST_METHOD(EarlyUploadDueOnlyWithAHandoffAndItsFrame)
		{
			Assert::IsNull(Vpv::Capture::EarlyUploadDue(std::shared_ptr<void>()), L"null slot: skipped");
			auto e = std::make_shared<Vpv::Capture::PayloadExtension>();
			std::shared_ptr<void> slot = e;
			Assert::IsNull(Vpv::Capture::EarlyUploadDue(slot), L"no hand-off: skipped");
			e->earlyHandoff = std::make_shared<EarlyUploadHandoff>();
			Assert::IsNull(Vpv::Capture::EarlyUploadDue(slot), L"no frame: skipped");
			e->rawFrame = std::make_shared<VideoFrame>();
			Assert::IsTrue(Vpv::Capture::EarlyUploadDue(slot) == e.get(), L"both: due");
		}

		// R18 (r2/p10): the free readers on a fake payload ({pixels, extension}) answer as the former members did.
		struct FakePayload
		{
			std::vector<uint8_t> pixels;
			std::shared_ptr<void> extension;
		};

		TEST_METHOD(PayloadReadersAreNullSafeWithoutASlot)
		{
			FakePayload p;
			p.pixels.resize(37);
			Assert::IsFalse(Vpv::Capture::IsRaw(p));
			Assert::AreEqual(static_cast<size_t>(37), Vpv::Capture::BytesOf(p), L"Bytes = the formatted pixels");
			Assert::IsTrue(Vpv::Capture::RawEncodingOf(p) == VideoFrameEncoding::UNKNOWN);
			Assert::IsFalse(Vpv::Capture::DvTunnelRawOf(p));
			Assert::IsFalse(Vpv::Capture::DvTunnelHighAlignedOf(p));
			Assert::IsNull(Vpv::Capture::RawFrameOf(p));
			Assert::IsFalse(Vpv::Capture::HasEarlyUpload(p));
			Assert::IsNull(Vpv::Capture::RawOf(p));
			Assert::IsNull(Vpv::Capture::CaptureOf(p));
		}

		TEST_METHOD(PayloadReadersSeeARawFrameSlot)
		{
			FakePayload p;
			p.pixels.resize(5);
			auto e = std::make_shared<Vpv::Capture::PayloadExtension>();
			e->rawFrame = std::make_shared<VideoFrame>();
			e->rawBytes = 4096;
			e->rawEncoding = VideoFrameEncoding::V210;
			p.extension = e;
			Assert::IsTrue(Vpv::Capture::IsRaw(p));
			Assert::AreEqual(static_cast<size_t>(4096), Vpv::Capture::BytesOf(p), L"Bytes = the raw frame's size");
			Assert::IsTrue(Vpv::Capture::RawEncodingOf(p) == VideoFrameEncoding::V210);
			Assert::IsTrue(Vpv::Capture::RawFrameOf(p) == e->rawFrame.get());
			Assert::IsFalse(Vpv::Capture::DvTunnelRawOf(p), L"not the tunnel");
			Assert::IsFalse(Vpv::Capture::DvTunnelHighAlignedOf(p));
			Assert::IsFalse(Vpv::Capture::HasEarlyUpload(p), L"no early slot or hand-off");
			e->dvTunnelRaw = true;
			e->dvTunnelHighAligned = true;
			Assert::IsTrue(Vpv::Capture::DvTunnelRawOf(p));
			Assert::IsTrue(Vpv::Capture::DvTunnelHighAlignedOf(p));
			e->dvTunnelHighAligned = false;
			Assert::IsFalse(Vpv::Capture::DvTunnelHighAlignedOf(p));
		}

		TEST_METHOD(PayloadReadersSeeAHandoffOnlySlot)
		{
			FakePayload p;
			p.pixels.resize(5);
			auto e = std::make_shared<Vpv::Capture::PayloadExtension>();
			e->earlyHandoff = std::make_shared<EarlyUploadHandoff>();
			p.extension = e;
			Assert::IsTrue(Vpv::Capture::HasEarlyUpload(p));
			Assert::IsFalse(Vpv::Capture::IsRaw(p), L"a hand-off without a raw frame is not raw");
			Assert::AreEqual(static_cast<size_t>(5), Vpv::Capture::BytesOf(p));
			Assert::IsNull(Vpv::Capture::RawFrameOf(p));
			Assert::IsNotNull(Vpv::Capture::CaptureOf(p));
		}
	};
}
