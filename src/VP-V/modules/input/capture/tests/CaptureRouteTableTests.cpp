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
	};
}
