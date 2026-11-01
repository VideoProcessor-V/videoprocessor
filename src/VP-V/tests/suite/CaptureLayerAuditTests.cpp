#include "pch.h"
#include "CppUnitTest.h"

#include <blackmagic_decklink/BlackMagicDeckLinkTranslate.h>
#ifndef VP_NO_MAGEWELL
#include <magewell/MagewellCaptureDevice.h>
#endif
#include <DisplayMode.h>
#include <VideoFrameEncoding.h>

#include <cmath>
#include <stdexcept>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Audit 2026-10-16 (capture-card readiness): CPU checks for the capture-layer
// fixes - DeckLink mode table rates (DCI 29.97 = 30000/1001, DCI 60 present),
// modes missing from the table built from the card's own description, the
// shared refresh-rate policy, NanosecondsPerFrame, bits-per-pixel coverage,
// Magewell HDR primaries ordered by chromaticity and missed frames counted
// from frame timestamps.
namespace VideoProcessorTest
{
	TEST_CLASS(CaptureLayerAuditTests)
	{
	public:

		TEST_METHOD(DeckLinkDci2997IsThirtyThousandOver1001)
		{
			const DisplayModeSharedPtr mode = Translate(bmdMode4kDCI2997);
			Assert::AreEqual(4096U, mode->FrameWidth());
			Assert::AreEqual(2160U, mode->FrameHeight());
			Assert::AreEqual(30000U, mode->TimeScale());
			Assert::AreEqual(1001U, mode->FrameDuration());
			Assert::AreEqual(30000.0 / 1001.0, FPS(bmdMode4kDCI2997), 1e-9);
		}

		TEST_METHOD(DeckLinkDci60IsInTheTable)
		{
			const DisplayModeSharedPtr mode = Translate(bmdMode4kDCI60);
			Assert::AreEqual(4096U, mode->FrameWidth());
			Assert::AreEqual(60000U, mode->TimeScale());
			Assert::AreEqual(1000U, mode->FrameDuration());
			Assert::IsFalse(mode->IsInterlaced());
		}

		TEST_METHOD(DeckLinkTableRatesMatchTheirModeNames)
		{
			struct Expected { BMDDisplayMode mode; double hz; };
			const Expected expected[] = {
				{ bmdModeHD1080p2398, 24000.0 / 1001.0 }, { bmdModeHD1080p24, 24.0 },
				{ bmdModeHD1080p25, 25.0 }, { bmdModeHD1080p2997, 30000.0 / 1001.0 },
				{ bmdModeHD1080p30, 30.0 }, { bmdModeHD1080p50, 50.0 },
				{ bmdModeHD1080p5994, 60000.0 / 1001.0 }, { bmdModeHD1080p6000, 60.0 },
				{ bmdModeHD1080i50, 25.0 }, { bmdModeHD1080i5994, 30000.0 / 1001.0 },
				{ bmdMode4K2160p2398, 24000.0 / 1001.0 }, { bmdMode4K2160p2997, 30000.0 / 1001.0 },
				{ bmdMode4K2160p5994, 60000.0 / 1001.0 }, { bmdMode4K2160p60, 60.0 },
				{ bmdMode4kDCI2398, 24000.0 / 1001.0 }, { bmdMode4kDCI24, 24.0 },
				{ bmdMode4kDCI2997, 30000.0 / 1001.0 }, { bmdMode4kDCI30, 30.0 },
				{ bmdMode4kDCI5994, 60000.0 / 1001.0 }, { bmdMode4kDCI60, 60.0 },
				{ bmdMode4K2160p11988, 120000.0 / 1001.0 }, { bmdMode4K2160p120, 120.0 },
			};
			for (const auto& e : expected)
			{
				BMDTimeValue duration = 0;
				BMDTimeScale scale = 0;
				Assert::IsTrue(DeckLinkModeTableFrameRate(e.mode, duration, scale));
				Assert::AreEqual(e.hz, (double)scale / (double)duration, 1e-9);
				Assert::AreEqual(e.hz, Translate(e.mode)->RefreshRateHz(), 1e-9);
			}
		}

		TEST_METHOD(DeckLinkModeMissingFromTableIsBuiltFromTheCard)
		{
			const BMDDisplayMode unknown = static_cast<BMDDisplayMode>(0x7a7a7a7a);
			BMDTimeValue duration = 0;
			BMDTimeScale scale = 0;
			Assert::IsFalse(DeckLinkModeTableFrameRate(unknown, duration, scale));
			Assert::ExpectException<std::runtime_error>([&]() { Translate(unknown); });

			const DisplayModeSharedPtr progressive = TranslateDeckLinkDisplayMode(
				unknown, 5120, 2160, 1001, 60000, bmdProgressiveFrame);
			Assert::AreEqual(5120U, progressive->FrameWidth());
			Assert::AreEqual(2160U, progressive->FrameHeight());
			Assert::AreEqual(60000.0 / 1001.0, progressive->RefreshRateHz(), 1e-9);
			Assert::IsFalse(progressive->IsInterlaced());

			const DisplayModeSharedPtr interlaced = TranslateDeckLinkDisplayMode(
				unknown, 1920, 1080, 1000, 25000, bmdUpperFieldFirst);
			Assert::IsTrue(interlaced->IsInterlaced());
			Assert::IsTrue(interlaced->FirstFieldOrder() == DisplayMode::FieldOrder::TopFirst);
			Assert::AreEqual(25.0, interlaced->RefreshRateHz(), 1e-9);
		}

		TEST_METHOD(DeckLinkModeMissingFromTableWithoutRateIsRejected)
		{
			const BMDDisplayMode unknown = static_cast<BMDDisplayMode>(0x7a7a7a7a);
			Assert::ExpectException<std::runtime_error>([&]() {
				TranslateDeckLinkDisplayMode(unknown, 1920, 1080, 0, 0, bmdProgressiveFrame);
			});
			Assert::ExpectException<std::runtime_error>([&]() {
				TranslateDeckLinkDisplayMode(unknown, 0, 1080, 1000, 60000, bmdProgressiveFrame);
			});
		}

		TEST_METHOD(DeckLinkTableStaysAuthoritativeForKnownModes)
		{
			// The card's own (here deliberately different) description does not
			// override the exact table rational for a known mode.
			const DisplayModeSharedPtr mode = TranslateDeckLinkDisplayMode(
				bmdMode4kDCI2997, 4096, 2160, 1000, 30000, bmdProgressiveFrame);
			Assert::AreEqual(1001U, mode->FrameDuration());
			Assert::AreEqual(30000U, mode->TimeScale());
		}

		TEST_METHOD(RefreshPolicyAcceptsTenTo240Hz)
		{
			Assert::IsTrue(DisplayMode::IsRefreshRateSupported(10.0));
			Assert::IsTrue(DisplayMode::IsRefreshRateSupported(23.976));
			Assert::IsTrue(DisplayMode::IsRefreshRateSupported(144.0));
			Assert::IsTrue(DisplayMode::IsRefreshRateSupported(240.0));
			Assert::IsFalse(DisplayMode::IsRefreshRateSupported(9.9));
			Assert::IsFalse(DisplayMode::IsRefreshRateSupported(240.1));

			Assert::IsFalse(DisplayMode::IsRefreshRateUnusual(24000.0 / 1001.0));
			Assert::IsFalse(DisplayMode::IsRefreshRateUnusual(60.0));
			Assert::IsFalse(DisplayMode::IsRefreshRateUnusual(120000.0 / 1001.0));
			Assert::IsFalse(DisplayMode::IsRefreshRateUnusual(120.0));
			Assert::IsTrue(DisplayMode::IsRefreshRateUnusual(15.0));
			Assert::IsTrue(DisplayMode::IsRefreshRateUnusual(144.0));
			Assert::IsTrue(DisplayMode::IsRefreshRateUnusual(240.0));
		}

		TEST_METHOD(DisplayModeConstructorFollowsRefreshPolicy)
		{
			// Formerly rejected (outside 23-120 Hz), now accepted.
			DisplayMode hz144(1920, 1080, false, 144000, 1000);
			DisplayMode hz240(1920, 1080, false, 240000, 1000);
			DisplayMode hz15(1920, 1080, false, 15000, 1000);
			Assert::AreEqual(240.0, hz240.RefreshRateHz(), 1e-9);
			Assert::AreEqual(144.0, hz144.RefreshRateHz(), 1e-9);
			Assert::AreEqual(15.0, hz15.RefreshRateHz(), 1e-9);
			// Magewell timebase (100ns ticks).
			DisplayMode magewell2398(3840, 2160, false, 10000000, 417083);
			Assert::AreEqual(23.976, magewell2398.RefreshRateHz(), 1e-3);

			Assert::ExpectException<std::runtime_error>([]() {
				DisplayMode tooSlow(1920, 1080, false, 9000, 1000);
			});
			Assert::ExpectException<std::runtime_error>([]() {
				DisplayMode tooFast(1920, 1080, false, 300000, 1000);
			});
		}

		TEST_METHOD(NanosecondsPerFrameReturnsTheRoundedPeriod)
		{
			Assert::AreEqual<uint64_t>(41708333ULL,
				DisplayMode(1920, 1080, false, 24000, 1001).NanosecondsPerFrame());
			Assert::AreEqual<uint64_t>(16666667ULL,
				DisplayMode(1920, 1080, false, 60000, 1000).NanosecondsPerFrame());
			Assert::AreEqual<uint64_t>(40000000ULL,
				DisplayMode(1920, 1080, true, 25000, 1000).NanosecondsPerFrame());
			Assert::AreEqual<uint64_t>(41708300ULL,
				DisplayMode(3840, 2160, false, 10000000, 417083).NanosecondsPerFrame());
		}

		TEST_METHOD(BitsPerPixelCoversEveryDeckLinkRgbPacking)
		{
			Assert::AreEqual(30U, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::R210));
			Assert::AreEqual(30U, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::R10b));
			Assert::AreEqual(30U, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::R10l));
			Assert::AreEqual(36U, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::R12B));
			Assert::AreEqual(36U, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::R12L));
			Assert::AreEqual(32U, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::ARGB_8BIT));
		}

#ifndef VP_NO_MAGEWELL  // Magewell device helpers (CI builds without the SDK skip them)
		TEST_METHOD(HdrPrimariesAreOrderedByChromaticity)
		{
			// BT.2020 primaries: R(0.708,0.292) G(0.170,0.797) B(0.131,0.046).
			const double rx = 0.708, ry = 0.292, gx = 0.170, gy = 0.797, bx = 0.131, by = 0.046;
			const double px[3][2] = { { rx, ry }, { gx, gy }, { bx, by } };
			// Every wire order (R,G,B / G,B,R as ST 2086 commonly sends / ...).
			const int orders[6][3] = {
				{ 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 0, 2, 1 }, { 1, 0, 2 }, { 2, 1, 0 } };
			for (const auto& order : orders)
			{
				double x[3], y[3];
				for (int slot = 0; slot < 3; ++slot)
				{
					x[slot] = px[order[slot]][0];
					y[slot] = px[order[slot]][1];
				}
				int red = -1, green = -1, blue = -1;
				MagewellCaptureDevice::OrderPrimariesByChromaticity(x, y, red, green, blue);
				Assert::AreEqual(rx, x[red], 1e-12);
				Assert::AreEqual(ry, y[red], 1e-12);
				Assert::AreEqual(gx, x[green], 1e-12);
				Assert::AreEqual(gy, y[green], 1e-12);
				Assert::AreEqual(bx, x[blue], 1e-12);
				Assert::AreEqual(by, y[blue], 1e-12);
			}

			// DCI-P3 in G,B,R wire order: R(0.680,0.320) G(0.265,0.690) B(0.150,0.060).
			const double x[3] = { 0.265, 0.150, 0.680 };
			const double y[3] = { 0.690, 0.060, 0.320 };
			int red = -1, green = -1, blue = -1;
			MagewellCaptureDevice::OrderPrimariesByChromaticity(x, y, red, green, blue);
			Assert::AreEqual(2, red);
			Assert::AreEqual(0, green);
			Assert::AreEqual(1, blue);

			// Degenerate (all zero) keeps the wire order and distinct indices.
			const double zero[3] = { 0.0, 0.0, 0.0 };
			MagewellCaptureDevice::OrderPrimariesByChromaticity(zero, zero, red, green, blue);
			Assert::AreEqual(0, red);
			Assert::AreEqual(1, green);
			Assert::AreEqual(2, blue);
		}

		TEST_METHOD(MagewellMissedFramesCountedFromTimestamps)
		{
			const uint32_t period = 166833;  // 59.94 Hz in 100ns ticks
			Assert::AreEqual<uint64_t>(0, MagewellCaptureDevice::MissedFramesBetween(1000, 1000 + period, period));
			Assert::AreEqual<uint64_t>(1, MagewellCaptureDevice::MissedFramesBetween(1000, 1000 + 2 * period, period));
			Assert::AreEqual<uint64_t>(3, MagewellCaptureDevice::MissedFramesBetween(1000, 1000 + 4 * period, period));
			// Jitter below half a period does not count.
			Assert::AreEqual<uint64_t>(0, MagewellCaptureDevice::MissedFramesBetween(1000, 1000 + period + period / 3, period));
			Assert::AreEqual<uint64_t>(1, MagewellCaptureDevice::MissedFramesBetween(1000, 1000 + 2 * period - period / 3, period));
			// Repeated, older, unknown period or discontinuity: nothing counted.
			Assert::AreEqual<uint64_t>(0, MagewellCaptureDevice::MissedFramesBetween(5000, 5000, period));
			Assert::AreEqual<uint64_t>(0, MagewellCaptureDevice::MissedFramesBetween(5000, 4000, period));
			Assert::AreEqual<uint64_t>(0, MagewellCaptureDevice::MissedFramesBetween(1000, 1000 + 4 * period, 0));
			Assert::AreEqual<uint64_t>(0, MagewellCaptureDevice::MissedFramesBetween(0, 2000LL * period, period));
		}
#endif
	};
}
