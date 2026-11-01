#include "pch.h"
#include "CppUnitTest.h"

#include <blackmagic_decklink/BlackMagicDeckLinkTranslate.h>
#include <microsoft_directshow/live_source_filter/ALiveSourceVideoOutputPin.h>
#include <CaptureStatePolicy.h>
#include <IntegerMath.h>
#include <TimingClock.h>
#include <VideoTimingController.h>

#include <cstdint>
#include <limits>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Bug hunt 2 (2026-10-17), capture fixes: DeckLink Rec.601 on any raster and
// reserved EOTF values (M1/L10), the 12-bit YCbCr capture default (M4), the
// overflow-free clock conversions (M2/L11/L12) and Restart after a failed
// capture start (M3).
namespace VideoProcessorTest
{
	namespace
	{
		// Exposes the protected static conversion without instantiating the pin.
		struct LiveSourcePinProbe : ALiveSourceVideoOutputPin
		{
			static REFERENCE_TIME Convert(timingclocktime_t timestamp, timingclocktime_t ticksPerSecond)
			{
				return ConvertTimingClockToReferenceTime(timestamp, ticksPerSecond);
			}
		};

		constexpr int64_t kRefPerSecond = 10000000;

		// Reference: exact rounded ticks * 1e7 / tps through split quotient and
		// remainder (both parts exact for the inputs used here).
		int64_t SplitReference(int64_t ticks, int64_t tps)
		{
			const int64_t whole = (ticks / tps) * kRefPerSecond;
			const int64_t remainderScaled = (ticks % tps) * kRefPerSecond;   // < tps * 1e7
			int64_t fraction = remainderScaled / tps;
			if ((remainderScaled % tps) * 2 >= tps)
				++fraction;
			return whole + fraction;
		}
	}

	TEST_CLASS(BugHunt2CaptureTests)
	{
	public:

		TEST_METHOD(DeckLinkRec601OnHdAndUhdRastersIsCarriedThrough)
		{
			for (const bool fixes : { true, false })
			{
				bool other = false;
				Assert::IsTrue(Translate(bmdColorspaceRec601, 1080, fixes, 60000.0 / 1001.0, &other) ==
					ColorSpace::REC_601_525, L"1080p59.94: 525 primaries");
				Assert::IsTrue(other, L"the caller is told to log it");
				Assert::IsTrue(Translate(bmdColorspaceRec601, 2160, fixes, 50.0, &other) ==
					ColorSpace::REC_601_625, L"2160p50: 625 primaries");
				Assert::IsTrue(Translate(bmdColorspaceRec601, 720, fixes, 25.0) == ColorSpace::REC_601_625);
				Assert::IsTrue(Translate(bmdColorspaceRec601, 720, fixes, 60.0) == ColorSpace::REC_601_525);
				Assert::IsTrue(Translate(bmdColorspaceRec601, 1080, fixes, 24.0) == ColorSpace::REC_601_525);
				Assert::IsTrue(Translate(bmdColorspaceRec601, 1440, fixes, 0.0) == ColorSpace::REC_601_525,
					L"no measured rate: 525 (never throws)");
			}
			// The SD mapping is unchanged and does not report "other raster".
			bool other = true;
			Assert::IsTrue(Translate(bmdColorspaceRec601, 576, true, 50.0, &other) == ColorSpace::REC_601_625);
			Assert::IsFalse(other);
			Assert::IsTrue(Translate(bmdColorspaceRec601, 486, false, 59.94, &other) == ColorSpace::UNKNOWN,
				L"fixes off: the 486 guard is kept");
			Assert::IsFalse(other);
			Assert::IsTrue(Translate(bmdColorspaceRec709, 1080, true, 50.0, &other) == ColorSpace::REC_709);
			Assert::IsFalse(other);
		}

		TEST_METHOD(DeckLinkRateFamilyIsDerivedFromTheMeasuredRate)
		{
			Assert::IsTrue(DeckLinkRateIs625Family(25.0));
			Assert::IsTrue(DeckLinkRateIs625Family(50.0));
			Assert::IsTrue(DeckLinkRateIs625Family(100.0));
			Assert::IsFalse(DeckLinkRateIs625Family(30000.0 / 1001.0));
			Assert::IsFalse(DeckLinkRateIs625Family(24.0));
			Assert::IsFalse(DeckLinkRateIs625Family(60.0));
			Assert::IsFalse(DeckLinkRateIs625Family(120.0));
			Assert::IsFalse(DeckLinkRateIs625Family(0.0));
			Assert::IsFalse(DeckLinkRateIs625Family(std::numeric_limits<double>::quiet_NaN()));
		}

		TEST_METHOD(DeckLinkReservedEotfIsSdrNotRejected)
		{
			bool reserved = true;
			Assert::IsTrue(TranslateEOTF(0, &reserved) == EOTF::SDR);
			Assert::IsFalse(reserved);
			Assert::IsTrue(TranslateEOTF(2, &reserved) == EOTF::PQ);
			Assert::IsFalse(reserved);
			Assert::IsTrue(TranslateEOTF(3) == EOTF::HLG);
			for (LONGLONG value = 4; value <= 7; ++value)
			{
				reserved = false;
				Assert::IsTrue(TranslateEOTF(value, &reserved) == EOTF::SDR);
				Assert::IsTrue(reserved);
			}
		}

		TEST_METHOD(DeckLinkYCbCr12DefaultKeepsBtbWtw)
		{
			using Choice = DeckLinkCaptureFormatPreferences::YCbCr12Capture;
			Assert::IsTrue(DeckLinkCaptureFormatPreferences{}.ycbcr12 == Choice::V210, L"default v210");

			Choice choice = Choice::Off;
			Assert::IsTrue(ParseDeckLinkYCbCr12Capture("auto", choice));
			Assert::IsTrue(choice == Choice::V210, L"auto: v210");
			Assert::IsTrue(ParseDeckLinkYCbCr12Capture("V210", choice));
			Assert::IsTrue(choice == Choice::V210);
			Assert::IsTrue(ParseDeckLinkYCbCr12Capture("rgb12", choice));
			Assert::IsTrue(choice == Choice::Rgb12, L"rgb12 restores the earlier default");
			Assert::IsTrue(ParseDeckLinkYCbCr12Capture("off", choice));
			Assert::IsTrue(choice == Choice::Off);
			Assert::IsFalse(ParseDeckLinkYCbCr12Capture("rgb16", choice));

			// v210 captures the 12-bit 4:2:2 signal as 10-bit YCbCr (VP owns the
			// matrix, BTB/WTW kept); rgb12 as full-range 12-bit RGB (R12B).
			const auto flags = static_cast<BMDDetectedVideoInputFormatFlags>(
				bmdDetectedVideoInputYCbCr422 | bmdDetectedVideoInput12BitDepth);
			Assert::IsTrue(DeckLinkIsYCbCr12(flags));
			Assert::IsTrue(CanonicalDeckLinkCapturePixelFormat(DeckLinkYCbCr12AsV210(flags)) == bmdFormat10BitYUV);
			Assert::IsTrue(CanonicalDeckLinkCapturePixelFormat(DeckLinkYCbCr12AsRgb12(flags)) == bmdFormat12BitRGB);
		}

		TEST_METHOD(ClockConversionsAreExactAtLargeTickCounts)
		{
			// 1 MHz (DeckLink) and 10 MHz (Magewell, QPC) at 30+ days, past
			// INT64_MAX / 1e7 = 9.22e11 ticks where the earlier code overflowed
			// or truncated to whole seconds.
			const int64_t rates[] = { 1000000, 10000000, 24000, 27000000 };
			for (const int64_t tps : rates)
			{
				const int64_t thirtyDays = tps * 86400LL * 30LL;
				const int64_t ticks[] = { 0, 1, tps - 1, thirtyDays, thirtyDays + tps / 3 + 7,
					INT64_MAX / kRefPerSecond + 1, INT64_MAX / kRefPerSecond + tps / 2 + 1 };
				for (const int64_t t : ticks)
				{
					const int64_t expected = SplitReference(t, tps);
					Assert::AreEqual<int64_t>(expected, I64_MulDivRoundSaturating(t, kRefPerSecond, tps));
					Assert::AreEqual<int64_t>(expected, LiveSourcePinProbe::Convert(t, tps));
					Assert::AreEqual<int64_t>(expected, VideoTimingController::ClockToReferenceTime(
						static_cast<uint64_t>(t), static_cast<uint64_t>(tps)));
				}
			}
			// Sub-second precision survives past the old threshold (was whole seconds).
			const int64_t big = 1000000LL * 86400LL * 40LL + 123457;   // 40 days at 1 MHz + 0.123457 s
			Assert::AreEqual<int64_t>(86400LL * 40LL * kRefPerSecond + 1234570,
				LiveSourcePinProbe::Convert(big, 1000000));
			// Negative and saturating cases.
			Assert::AreEqual<int64_t>(-15, I64_MulDivRoundSaturating(-3, 5, 1));
			Assert::AreEqual<int64_t>(-2, I64_MulDivRoundSaturating(-3, 1, 2), L"round half away from zero");
			Assert::AreEqual<int64_t>(INT64_MAX, I64_MulDivRoundSaturating(INT64_MAX, kRefPerSecond, 1));
			Assert::AreEqual<int64_t>(INT64_MIN, I64_MulDivRoundSaturating(INT64_MIN, kRefPerSecond, 1));
			Assert::AreEqual<int64_t>(0, I64_MulDivRoundSaturating(5, 5, 0));
			Assert::AreEqual<uint64_t>(UINT64_MAX, U64_MulDivRoundSaturating(UINT64_MAX, 2, 1));
		}

		TEST_METHOD(TimingClockDiffKeepsSubMillisecondPrecision)
		{
			Assert::AreEqual(8.333, TimingClockDiffMs(0, 8333, 1000000), 1e-9, L"was 8");
			Assert::AreEqual(-8.333, TimingClockDiffMs(8333, 0, 1000000), 1e-9);
			Assert::AreEqual(0.0001, TimingClockDiffMs(0, 1, 10000000), 1e-12);
			// A diff whose diff * 1000 would overflow int64.
			const int64_t huge = 10000000LL * 86400LL * 20000LL;
			Assert::AreEqual(86400.0 * 20000.0 * 1000.0, TimingClockDiffMs(0, huge, 10000000), 1.0);
			Assert::AreEqual(0.0, TimingClockDiffMs(0, 100, 0));
		}

		TEST_METHOD(RestartStaysAvailableAfterAFailedCaptureStart)
		{
			using S = CaptureDeviceState;
			Assert::IsTrue(CaptureStatePolicy::StartFailed(S::CAPTUREDEVICESTATE_READY, S::CAPTUREDEVICESTATE_STARTING));
			Assert::IsTrue(CaptureStatePolicy::RestartAvailable(S::CAPTUREDEVICESTATE_READY, S::CAPTUREDEVICESTATE_STARTING),
				L"Magewell setup failure: Error() then READY after STARTING");
			Assert::IsTrue(CaptureStatePolicy::RestartAvailable(S::CAPTUREDEVICESTATE_READY, S::CAPTUREDEVICESTATE_CAPTURING));
			Assert::IsTrue(CaptureStatePolicy::RestartAvailable(S::CAPTUREDEVICESTATE_FAILED, S::CAPTUREDEVICESTATE_UNKNOWN));
			Assert::IsTrue(CaptureStatePolicy::RestartAvailable(S::CAPTUREDEVICESTATE_CAPTURING, S::CAPTUREDEVICESTATE_STARTING));
			Assert::IsFalse(CaptureStatePolicy::RestartAvailable(S::CAPTUREDEVICESTATE_READY, S::CAPTUREDEVICESTATE_UNKNOWN),
				L"a device that is simply ready (no start attempted)");
			Assert::IsFalse(CaptureStatePolicy::RestartAvailable(S::CAPTUREDEVICESTATE_READY, S::CAPTUREDEVICESTATE_STOPPING),
				L"a requested stop");
			Assert::IsFalse(CaptureStatePolicy::StartFailed(S::CAPTUREDEVICESTATE_CAPTURING, S::CAPTUREDEVICESTATE_STARTING));
		}
	};
}
