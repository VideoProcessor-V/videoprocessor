#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedDriveConfig.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>

#include <cmath>
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

	// B5: FilmClock::SetRebaseOnRateChange toggled mid-lock starts the clock over (the
	// caller then marks a pulldown discontinuity); before, a re-based ideal survived
	// into the plain form and the clock jumped.
	TEST_CLASS(FilmClockToggleTests)
	{
	public:
		TEST_METHOD(ToggleMidLockResetsTheClock)
		{
			Pulldown::FilmClock clock;
			Assert::IsFalse(clock.SetRebaseOnRateChange(true), L"nothing timed yet: no discontinuity");
			Assert::IsFalse(clock.SetRebaseOnRateChange(true), L"unchanged");
			const uint64_t start = 200000;
			const double t0 = 5000.0;
			double arrival = t0;
			for (int i = 0; i < 100; ++i)
			{
				const double hz = i < 50 ? 24.0 : 23.976;   // a rate change re-bases
				arrival = t0 + i / 23.976;
				clock.Time(start + i, arrival, hz);
			}
			Assert::AreEqual(1ULL, clock.Rebases());
			Assert::IsTrue(clock.InUse());
			Assert::IsTrue(clock.SetRebaseOnRateChange(false), L"toggled mid-lock: a discontinuity");
			Assert::IsFalse(clock.InUse(), L"the clock started over");
			// Fresh: the next frame is timed at its arrival, not from the stale re-based base.
			const double next = t0 + 100 / 23.976;
			Assert::AreEqual(next, clock.Time(start + 100, next, 23.976), 1e-9);
		}
	};

	// C6: the residual grain guard's code -> linear conversion follows the source's
	// transfer (the shader's rs_slope, mirrored on the CPU), not an assumed 2.2.
	TEST_CLASS(GrainGuardTransferTests)
	{
	public:
		TEST_METHOD(SlopeFollowsTheSourceTransfer)
		{
			using namespace AdaptiveDownscale;
			// Gamma 2.2: the earlier formula, 2.2 x code^1.2.
			pl_color_space g22{};
			g22.primaries = PL_COLOR_PRIM_BT_709;
			g22.transfer = PL_COLOR_TRC_GAMMA22;
			const GuardTransferParams t22 = GuardTransfer(g22);
			for (double y : { 0.01, 0.1, 0.5, 0.9 })
				Assert::AreEqual(2.2 * std::pow(std::pow(y, 1.0 / 2.2), 1.2), GuardSlope(t22, y), 1e-2 * GuardSlope(t22, y));
			// Gamma 2.4 differs from it (the 2.2 assumption was wrong there).
			pl_color_space g24 = g22;
			g24.transfer = PL_COLOR_TRC_GAMMA24;
			const GuardTransferParams t24 = GuardTransfer(g24);
			Assert::AreEqual(2.4 * std::pow(std::pow(0.1, 1.0 / 2.4), 1.4), GuardSlope(t24, 0.1), 1e-2);
			// Code and linear invert each other for every mode.
			pl_color_space bt1886 = g22, pq = g22, hlg = g22;
			bt1886.transfer = PL_COLOR_TRC_BT_1886;
			pq.primaries = hlg.primaries = PL_COLOR_PRIM_BT_2020;
			pq.transfer = PL_COLOR_TRC_PQ;
			hlg.transfer = PL_COLOR_TRC_HLG;
			for (const pl_color_space& csp : { g22, g24, bt1886, pq, hlg })
			{
				const GuardTransferParams t = GuardTransfer(csp);
				for (double c : { 0.1, 0.3, 0.5, 0.75, 0.9 })
				{
					const double y = GuardLinear(t, c);
					Assert::AreEqual(c, GuardCode(t, y), 1e-4, L"code(linear(c)) = c");
					Assert::IsTrue(GuardSlope(t, y) > 0.0);
				}
			}
			// PQ: the slope is PQ's own (finite difference of the ST 2084 curve), far
			// from a 2.2 power at the same code.
			const GuardTransferParams tpq = GuardTransfer(pq);
			Assert::AreEqual(1.0f, tpq.mode, 0.0f);
			const double c = 0.58;   // about 203 nits
			const double y = GuardLinear(tpq, c);
			Assert::AreEqual(1.0, y, 0.05, L"PQ 0.58 is near SDR white");
			const double numeric = (GuardLinear(tpq, c + 1e-4) - GuardLinear(tpq, c - 1e-4)) / 2e-4;
			Assert::AreEqual(numeric, GuardSlope(tpq, y), numeric * 1e-2);
			Assert::IsTrue(std::abs(GuardSlope(tpq, y) - 2.2 * std::pow(std::pow(y, 1.0 / 2.2), 1.2)) > 1.0);
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
