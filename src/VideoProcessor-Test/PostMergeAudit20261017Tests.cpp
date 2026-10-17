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

	// B4: the dequeue / producer adjacency test across a pulldown lock change compares
	// the film clock (QPC) with the capture clock: not judged, so no hitch.
	TEST_CLASS(PulldownLockChangeAdjacencyTests)
	{
	public:
		TEST_METHOD(LockChangeIsNotJudged)
		{
			using CaptureTimestampGap::AdjacentPairTimes;
			// A on the film clock (QPC seconds since boot), B on the capture clock.
			Assert::IsTrue(AdjacentPairTimes(86400.0, true, 12.5, false, 24.0), L"lock lost: accepted");
			Assert::IsTrue(AdjacentPairTimes(12.5, false, 86400.0, true, 24.0), L"lock gained: accepted");
			// The same clock is still judged.
			Assert::IsTrue(AdjacentPairTimes(10.0, true, 10.0 + 1.0 / 24.0, true, 24.0));
			Assert::IsFalse(AdjacentPairTimes(10.0, true, 11.0, true, 24.0));
			Assert::IsFalse(AdjacentPairTimes(10.0, false, 11.0, false, 24.0));

			DejudderPolicy::MotionAheadFrame a, b;
			a.time = 86400.0; a.filmClock = true; a.kind = DejudderPolicy::DolbyVisionFrameKind::Picture;
			b.time = 12.5; b.filmClock = false; b.kind = DejudderPolicy::DolbyVisionFrameKind::Picture;
			Assert::IsFalse(DejudderPolicy::MotionAheadBreakBefore(false, false, true, 24.0, false, true, a, b),
				L"the producer: no break at the lock change");
			b.filmClock = true;
			Assert::IsTrue(DejudderPolicy::MotionAheadBreakBefore(false, false, true, 24.0, false, true, a, b),
				L"the same clock far apart: a break");
		}
	};

	// C6: Missed is defined by time, so the same loss behaves alike at 24, 50 and 120 Hz.
	TEST_CLASS(MissedFramesByTimeTests)
	{
	public:
		TEST_METHOD(MissedSpanIsTheSameAtEveryRate)
		{
			using CaptureTimestampGap::Classify;
			using CaptureTimestampGap::Kind;
			const long long tps = 10000000;   // 100 ns ticks
			for (double hz : { 24.0, 50.0, 120.0 })
			{
				const long long period = static_cast<long long>(tps / hz);
				const std::wstring at = L" at " + std::to_wstring(hz) + L" Hz";
				Assert::IsTrue(Classify(period, period, tps).kind == Kind::Normal, (L"one period" + at).c_str());
				const auto three = Classify(3 * period, period, tps);
				Assert::IsTrue(three.kind == Kind::Missed, (L"3 periods" + at).c_str());
				Assert::AreEqual(2LL, three.missed);
				// About 300 ms lost: missed frames at every rate (7 / 15 / 36 periods).
				const long long p300 = static_cast<long long>(0.3 * hz + 0.5);
				const auto loss300 = Classify(p300 * period, period, tps);
				Assert::IsTrue(loss300.kind == Kind::Missed, (L"300 ms" + at).c_str());
				Assert::AreEqual(p300 - 1, loss300.missed);
				// Half a second lost: a break at every rate (12 / 25 / 60 periods).
				const long long p500 = static_cast<long long>(0.5 * hz + 0.5);
				Assert::IsTrue(Classify(p500 * period, period, tps).kind == Kind::Break, (L"500 ms" + at).c_str());
				// Between whole periods is still a break.
				Assert::IsTrue(Classify(3 * period + period / 2, period, tps).kind == Kind::Break, (L"3.5 periods" + at).c_str());
			}
			// 8 periods of 23.976 film stay missed frames (the old span).
			const long long film = static_cast<long long>(tps * 1001 / 24000);
			Assert::IsTrue(Classify(8 * film, film, tps).kind == Kind::Missed);
			// Without the clock's rate: the old 2-8 periods.
			Assert::IsTrue(Classify(8 * 1000, 1000).kind == Kind::Missed);
			Assert::IsTrue(Classify(9 * 1000, 1000).kind == Kind::Break);
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
