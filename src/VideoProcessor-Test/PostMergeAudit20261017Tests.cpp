#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedDriveConfig.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <vprenderer/DejudderPolicy.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Post-merge audit 2026-10-17 (v1.4.02 integration): CPU tests for the fixes.
namespace PostMergeAudit20261017Test
{
	// The fields of the renderer's QueuedFrame::ConvertedPayload the dequeue's DV pair
	// rule reads (that struct is private to the renderer).
	struct FakePayload
	{
		DolbyVisionTunnelFrameMetadata dolby;
		VideoFrameEncoding rawEncoding = VideoFrameEncoding::UNKNOWN;   // raw route only
		VideoFrameEncoding captureEncoding = VideoFrameEncoding::UNKNOWN;
	};

	FakePayload SdrFallback(VideoFrameEncoding capture)
	{
		// Not a raw payload: the SDR fallback goes through the CPU path (zero planes).
		FakePayload p;
		p.dolby.valid = false;
		p.dolby.allowSdrFallback = true;
		p.dolby.rgbBitAlignment = 1;
		p.captureEncoding = capture;
		return p;
	}

	// Owner 2026-10-17: projector read-back off by default (the original script never
	// read back); start-up reaches a known state by sending, not by asking.
	TEST_CLASS(LedReadBackDefaultTests)
	{
	public:
		TEST_METHOD(ReadBackIsOffByDefault)
		{
			const LedDrive::ProjectorConfig defaults;
			Assert::IsFalse(defaults.readBack);
		}
	};

	TEST_CLASS(DolbyVisionCaptureEncodingTests)
	{
	public:
		TEST_METHOD(R210SdrFallbackPairPairsThroughTheDequeueRule)
		{
			using namespace DejudderPolicy;
			const FakePayload a = SdrFallback(VideoFrameEncoding::R210);
			const FakePayload b = SdrFallback(VideoFrameEncoding::R210);
			// The regression: rawEncoding is UNKNOWN on these payloads.
			Assert::IsTrue(a.rawEncoding == VideoFrameEncoding::UNKNOWN);
			Assert::IsTrue(ClassifyDolbyVisionPayload(a) == DolbyVisionFrameKind::SdrFallback);
			Assert::IsTrue(DolbyVisionNextPairs(true, a, b));
		}

		TEST_METHOD(R10bSdrFallbackAndUntrustedDvDoNotPair)
		{
			using namespace DejudderPolicy;
			const FakePayload a = SdrFallback(VideoFrameEncoding::R10b);
			const FakePayload b = SdrFallback(VideoFrameEncoding::R10b);
			Assert::IsTrue(ClassifyDolbyVisionPayload(a) == DolbyVisionFrameKind::Untrusted);
			Assert::IsFalse(DolbyVisionNextPairs(true, a, b));

			FakePayload dv;
			dv.dolby.valid = true;
			dv.dolby.rgbBitAlignment = 1;
			dv.captureEncoding = VideoFrameEncoding::R210;
			FakePayload black = dv;
			black.dolby.requiresBlackFrame = true;
			Assert::IsTrue(DolbyVisionNextPairs(true, dv, dv));
			Assert::IsFalse(DolbyVisionNextPairs(true, dv, black));
			Assert::IsFalse(DolbyVisionNextPairs(true, black, black));
			// DV never pairs with an r210 SDR fallback.
			Assert::IsFalse(DolbyVisionNextPairs(true, dv, SdrFallback(VideoFrameEncoding::R210)));
			// Off the tunnel the rule does not apply.
			Assert::IsTrue(DolbyVisionNextPairs(false, a, b));
		}
	};
}
