#include "pch.h"
#include "CppUnitTest.h"

// ABI 37 (VP Renderer DirectShow filter): the renderer side's pure parts -
// the two-plane 4:2:0 encodings, the file film clock, the slot wait and the
// file formatter.

#include <VideoFrame.h>
#include <VideoState.h>
#include <VideoFrameEncoding.h>
#include <FileFrameTiming.h>
#include <video_frame_formatter/CNoopVideoFrameFormatter.h>
#include <video_frame_formatter/CTwoPlane420VideoFrameFormatter.h>
#include "../VideoProcessor-Lib/vprenderer/FileFrameClock.h"
#include "../VideoProcessor-Lib/vprenderer/FileFrameSlotGate.h"
#include "../VideoProcessor-Lib/vprenderer/LibplaceboRendererPluginApi.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	VideoStateComPtr FileState(VideoFrameEncoding encoding, uint32_t width, uint32_t height)
	{
		VideoStateComPtr state = new VideoState();
		state->valid = true;
		state->displayMode = std::make_shared<DisplayMode>(width, height, false, 24000, 1001);
		state->videoFrameEncoding = encoding;
		return state;
	}

	FileFrameTiming Timing(uint32_t numerator, uint32_t denominator, double measuredHz, double rate)
	{
		FileFrameTiming timing;
		timing.sourceRateNumerator = numerator;
		timing.sourceRateDenominator = denominator;
		timing.sourceRateHz = measuredHz;
		timing.playbackRate = rate;
		return timing;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(FileFrameEncodingTests)
	{
	public:
		TEST_METHOD(BytesPerRowAndFrameForTwoPlane420)
		{
			// 1920x1080: luma 1080 rows + 540 chroma rows.
			auto nv12 = FileState(VideoFrameEncoding::NV12, 1920, 1080);
			Assert::AreEqual(1920u, nv12->BytesPerRow());
			Assert::AreEqual(1920u * (1080u + 540u), nv12->BytesPerFrame());
			auto p010 = FileState(VideoFrameEncoding::P010, 1920, 1080);
			Assert::AreEqual(3840u, p010->BytesPerRow());
			Assert::AreEqual(3840u * (1080u + 540u), p010->BytesPerFrame());
			auto p016 = FileState(VideoFrameEncoding::P016, 3840, 2160);
			Assert::AreEqual(7680u, p016->BytesPerRow());
			Assert::AreEqual(7680u * (2160u + 1080u), p016->BytesPerFrame());
		}

		TEST_METHOD(OddHeightsRoundTheChromaRowsUp)
		{
			// 720x481: ceil(481 / 2) = 241 chroma rows.
			auto nv12 = FileState(VideoFrameEncoding::NV12, 720, 481);
			Assert::AreEqual(720u * (481u + 241u), nv12->BytesPerFrame());
			auto p010 = FileState(VideoFrameEncoding::P010, 720, 481);
			Assert::AreEqual(1440u * (481u + 241u), p010->BytesPerFrame());
			// DisplayMode's smallest size (100 lines) with an odd height.
			auto p016 = FileState(VideoFrameEncoding::P016, 100, 101);
			Assert::AreEqual(200u * (101u + 51u), p016->BytesPerFrame());
		}

		TEST_METHOD(OddWidthsRoundTheStrideUpToEven)
		{
			auto nv12 = FileState(VideoFrameEncoding::NV12, 719, 480);
			Assert::AreEqual(720u, nv12->BytesPerRow());
			auto p010 = FileState(VideoFrameEncoding::P010, 719, 480);
			Assert::AreEqual(1440u, p010->BytesPerRow());
			Assert::AreEqual(1440u * (480u + 240u), p010->BytesPerFrame());
		}

		TEST_METHOD(ToStringFourCCAndBitsPerPixel)
		{
			Assert::AreEqual(L"YUV 4:2:0 8-bit (NV12)", ToString(VideoFrameEncoding::NV12));
			Assert::AreEqual(L"YUV 4:2:0 10-bit (P010)", ToString(VideoFrameEncoding::P010));
			Assert::AreEqual(L"YUV 4:2:0 16-bit (P016)", ToString(VideoFrameEncoding::P016));
			Assert::AreEqual(static_cast<uint32_t>(MAKEFOURCC('N', 'V', '1', '2')), VideoFrameEncodingFourCC(VideoFrameEncoding::NV12));
			Assert::AreEqual(static_cast<uint32_t>(MAKEFOURCC('P', '0', '1', '0')), VideoFrameEncodingFourCC(VideoFrameEncoding::P010));
			Assert::AreEqual(static_cast<uint32_t>(MAKEFOURCC('P', '0', '1', '6')), VideoFrameEncodingFourCC(VideoFrameEncoding::P016));
			Assert::AreEqual(12u, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::NV12));
			Assert::AreEqual(24u, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::P010));
			Assert::AreEqual(24u, VideoFrameEncodingBitsPerPixel(VideoFrameEncoding::P016));
		}

		TEST_METHOD(OnlyTheNewEncodingsAreTwoPlane420AndEarlierValuesKeepTheirNumbers)
		{
			Assert::IsTrue(VideoFrameEncodingIsTwoPlane420(VideoFrameEncoding::NV12));
			Assert::IsTrue(VideoFrameEncodingIsTwoPlane420(VideoFrameEncoding::P010));
			Assert::IsTrue(VideoFrameEncodingIsTwoPlane420(VideoFrameEncoding::P016));
			Assert::IsFalse(VideoFrameEncodingIsTwoPlane420(VideoFrameEncoding::V210));
			Assert::IsFalse(VideoFrameEncodingIsTwoPlane420(VideoFrameEncoding::UYVY));
			Assert::IsFalse(VideoFrameEncodingIsTwoPlane420(VideoFrameEncoding::UNKNOWN));
			Assert::AreEqual(static_cast<int>(VideoFrameEncoding::DNxHR) + 1, static_cast<int>(VideoFrameEncoding::NV12));
			Assert::AreEqual(static_cast<int>(VideoFrameEncoding::DNxHR) + 3, static_cast<int>(VideoFrameEncoding::P016));
		}

		TEST_METHOD(PluginApiVersionIs35)
		{
			Assert::AreEqual(static_cast<uint32_t>(35), VP_LIBPLACEBO_PLUGIN_API_VERSION);
		}
	};

	TEST_CLASS(FileFrameClockTests)
	{
	public:
		TEST_METHOD(FilmRateIsTheRationalTimesThePlaybackRate)
		{
			Assert::AreEqual(24000.0 / 1001.0, FileFrameClock::FilmHz(Timing(24000, 1001, 0.0, 1.0)), 1e-12);
			Assert::AreEqual(2.0 * 24000.0 / 1001.0, FileFrameClock::FilmHz(Timing(24000, 1001, 0.0, 2.0)), 1e-12);
			Assert::AreEqual(25.0 * 0.5, FileFrameClock::FilmHz(Timing(25, 1, 0.0, 0.5)), 1e-12);
			// The rational wins over the measured rate.
			Assert::AreEqual(30000.0 / 1001.0, FileFrameClock::FilmHz(Timing(30000, 1001, 29.0, 1.0)), 1e-12);
		}

		TEST_METHOD(MeasuredRateWhenNoRational)
		{
			Assert::AreEqual(23.9, FileFrameClock::FilmHz(Timing(0, 0, 23.9, 1.0)), 1e-12);
			// Half a rational is no rational.
			Assert::AreEqual(50.0, FileFrameClock::FilmHz(Timing(24000, 0, 50.0, 1.0)), 1e-12);
			Assert::AreEqual(50.0, FileFrameClock::FilmHz(Timing(0, 1001, 50.0, 1.0)), 1e-12);
		}

		TEST_METHOD(UnknownOrInvalidRatesGiveZero)
		{
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(0, 0, 0.0, 1.0)));
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(0, 0, -24.0, 1.0)));
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(0, 0, std::numeric_limits<double>::quiet_NaN(), 1.0)));
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(0, 0, std::numeric_limits<double>::infinity(), 1.0)));
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(24, 1, 0.0, 0.0)));
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(24, 1, 0.0, -1.0)));
			Assert::AreEqual(0.0, FileFrameClock::FilmHz(Timing(24, 1, 0.0, std::numeric_limits<double>::quiet_NaN())));
		}

		TEST_METHOD(MotionOnlyAtRateOneNotPausedWithAKnownRate)
		{
			Assert::IsTrue(FileFrameClock::MotionAllowed(Timing(24000, 1001, 0.0, 1.0)));
			Assert::IsFalse(FileFrameClock::MotionAllowed(Timing(24000, 1001, 0.0, 2.0)));
			Assert::IsFalse(FileFrameClock::MotionAllowed(Timing(24000, 1001, 0.0, 0.5)));
			Assert::IsFalse(FileFrameClock::MotionAllowed(Timing(24000, 1001, 0.0, 1.0000001)));
			Assert::IsFalse(FileFrameClock::MotionAllowed(Timing(0, 0, 0.0, 1.0)));
			FileFrameTiming paused = Timing(24000, 1001, 0.0, 1.0);
			paused.paused = true;
			Assert::IsFalse(FileFrameClock::MotionAllowed(paused));
		}

		TEST_METHOD(ArrivalIsThePresentationTimeWhenKnown)
		{
			FileFrameTiming timing;
			Assert::AreEqual(int64_t{ 77 }, FileFrameClock::ArrivalQpc(timing, 77));
			timing.presentationQpc = 123456789;
			Assert::AreEqual(int64_t{ 123456789 }, FileFrameClock::ArrivalQpc(timing, 77));
		}

		TEST_METHOD(DueAtPresentationLessLeadPausedOrUnknownAtOnce)
		{
			Assert::IsFalse(FileFrameClock::Due(900, 1000, 50, false));
			Assert::AreEqual(int64_t{ 50 }, FileFrameClock::TicksUntilDue(900, 1000, 50, false));
			Assert::IsTrue(FileFrameClock::Due(950, 1000, 50, false));
			Assert::IsTrue(FileFrameClock::Due(5000, 1000, 50, false));   // late: at once, never dropped here
			Assert::AreEqual(int64_t{ 0 }, FileFrameClock::TicksUntilDue(5000, 1000, 50, false));
			Assert::IsTrue(FileFrameClock::Due(0, 1000, 0, true));        // paused: shown and held
			Assert::IsTrue(FileFrameClock::Due(0, 0, 0, false));          // unknown due time
			Assert::AreEqual(int64_t{ 100 }, FileFrameClock::TicksUntilDue(900, 1000, -5, false));   // negative lead ignored
		}

		TEST_METHOD(SmallerTimingBlocksAreRefused)
		{
			FileFrameTiming timing;
			Assert::IsTrue(FileFrameClock::TimingUsable(timing));
			timing.structSize = sizeof(FileFrameTiming) - 1;
			Assert::IsFalse(FileFrameClock::TimingUsable(timing));
			timing.structSize = sizeof(FileFrameTiming) + 8;   // a newer sender
			Assert::IsTrue(FileFrameClock::TimingUsable(timing));
		}
	};

	TEST_CLASS(FileFrameSlotGateTests)
	{
	public:
		TEST_METHOD(RoomAtOnceReturnsTrueWithoutWaiting)
		{
			FileFrameSlotGate gate;
			const auto started = std::chrono::steady_clock::now();
			Assert::IsTrue(gate.Wait(5000, []() { return true; }));
			Assert::IsTrue(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
		}

		TEST_METHOD(TimesOutWhenNoRoomAppears)
		{
			FileFrameSlotGate gate;
			int calls = 0;
			const auto started = std::chrono::steady_clock::now();
			Assert::IsFalse(gate.Wait(30, [&calls]() { ++calls; return false; }));
			Assert::IsTrue(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(25));
			Assert::IsTrue(calls >= 1);
			Assert::IsFalse(gate.Wait(0, []() { return false; }));
		}

		TEST_METHOD(WakesWhenSpaceAppears)
		{
			FileFrameSlotGate gate;
			std::atomic<bool> room{ false };
			std::thread consumer([&]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					room = true;
					gate.Notify();
				});
			const auto started = std::chrono::steady_clock::now();
			const bool result = gate.Wait(10000, [&room]() { return room.load(); });
			const auto waited = std::chrono::steady_clock::now() - started;
			consumer.join();
			Assert::IsTrue(result);
			Assert::IsTrue(waited < std::chrono::seconds(5));
		}

		TEST_METHOD(InfiniteWaitWakesOnNotify)
		{
			FileFrameSlotGate gate;
			std::atomic<bool> room{ false };
			std::thread consumer([&]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					room = true;
					gate.Notify();
				});
			Assert::IsTrue(gate.Wait(FileFrameSlotGate::kInfinite, [&room]() { return room.load(); }));
			consumer.join();
		}

		TEST_METHOD(NoLostWakeupBetweenTestAndSleep)
		{
			// Room appears (with its Notify) while the room test runs: the
			// sequence number makes the waiter test again instead of sleeping.
			FileFrameSlotGate gate;
			int calls = 0;
			Assert::IsTrue(gate.Wait(10000, [&]()
				{
					if (++calls == 1)
					{
						gate.Notify();
						return false;
					}
					return true;
				}));
			Assert::AreEqual(2, calls);
		}

		TEST_METHOD(CancelWakesABlockedWaiterWithFalse)
		{
			FileFrameSlotGate gate;
			std::thread flusher([&]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					gate.Cancel();
				});
			const auto started = std::chrono::steady_clock::now();
			const bool result = gate.Wait(FileFrameSlotGate::kInfinite, []() { return false; });
			const auto waited = std::chrono::steady_clock::now() - started;
			flusher.join();
			Assert::IsFalse(result);
			Assert::IsTrue(waited < std::chrono::seconds(5));
		}

		TEST_METHOD(CancelIsStickyUntilRearmed)
		{
			FileFrameSlotGate gate;
			gate.Cancel();
			Assert::IsTrue(gate.Cancelled());
			// Even with room: false until the next timed frame re-arms the gate.
			Assert::IsFalse(gate.Wait(1000, []() { return true; }));
			Assert::IsFalse(gate.Wait(FileFrameSlotGate::kInfinite, []() { return true; }));
			gate.Notify();
			Assert::IsFalse(gate.Wait(0, []() { return true; }));
			gate.Arm();
			Assert::IsFalse(gate.Cancelled());
			Assert::IsTrue(gate.Wait(1000, []() { return true; }));
		}

		TEST_METHOD(CancelDuringTheRoomTestWins)
		{
			FileFrameSlotGate gate;
			Assert::IsFalse(gate.Wait(1000, [&gate]() { gate.Cancel(); return true; }));
		}
	};

	TEST_CLASS(FileFrameFormatterTests)
	{
	public:
		TEST_METHOD(Nv12IsWidenedIntoSixteenBitPlanes)
		{
			// 104x101 (DisplayMode needs >= 100): luma 101 rows, chroma ceil(101/2) = 51 rows.
			auto state = FileState(VideoFrameEncoding::NV12, 104, 101);
			std::vector<uint8_t> in(state->BytesPerFrame());
			for (size_t i = 0; i < in.size(); ++i)
				in[i] = static_cast<uint8_t>(16 + i * 11);
			CTwoPlane420VideoFrameFormatter formatter;
			formatter.OnVideoState(state);
			Assert::AreEqual(static_cast<LONG>(104 * 2 * (101 + 51)), formatter.GetOutFrameSize());
			std::vector<uint8_t> out(formatter.GetOutFrameSize(), 0xCD);
			VideoFrame frame(in.data(), 1, 0, nullptr);
			Assert::IsTrue(formatter.FormatVideoFrame(frame, out.data()));
			const uint16_t* words = reinterpret_cast<const uint16_t*>(out.data());
			for (size_t i = 0; i < in.size(); ++i)
				Assert::AreEqual(static_cast<uint16_t>(in[i] << 8), words[i]);
			const auto contract = formatter.GetOutputContract();
			Assert::IsTrue(contract.sampleRange == VideoFrameSampleRange::LIMITED);
			Assert::AreEqual(static_cast<uint8_t>(8), contract.colorDepth);
			Assert::AreEqual(static_cast<uint8_t>(8), contract.bitShift);
		}

		TEST_METHOD(P010AndP016AreCopiedUnchanged)
		{
			const VideoFrameEncoding encodings[] = { VideoFrameEncoding::P010, VideoFrameEncoding::P016 };
			for (const VideoFrameEncoding encoding : encodings)
			{
				auto state = FileState(encoding, 106, 101);   // odd height: 51 chroma rows
				std::vector<uint16_t> in(state->BytesPerFrame() / 2);
				for (size_t i = 0; i < in.size(); ++i)
					in[i] = static_cast<uint16_t>(encoding == VideoFrameEncoding::P010 ? ((64 + i * 7) & 0x3FF) << 6 : 4096 + i * 997);
				CTwoPlane420VideoFrameFormatter formatter;
				formatter.OnVideoState(state);
				Assert::AreEqual(static_cast<LONG>(106 * 2 * (101 + 51)), formatter.GetOutFrameSize());
				std::vector<uint16_t> out(formatter.GetOutFrameSize() / 2, 0);
				VideoFrame frame(in.data(), 1, 0, nullptr);
				Assert::IsTrue(formatter.FormatVideoFrame(frame, reinterpret_cast<BYTE*>(out.data())));
				Assert::IsTrue(in == out);
			}
			const auto p010 = CTwoPlane420VideoFrameFormatter::ContractFor(VideoFrameEncoding::P010);
			Assert::AreEqual(static_cast<uint8_t>(10), p010.colorDepth);
			Assert::AreEqual(static_cast<uint8_t>(6), p010.bitShift);
			const auto p016 = CTwoPlane420VideoFrameFormatter::ContractFor(VideoFrameEncoding::P016);
			Assert::AreEqual(static_cast<uint8_t>(16), p016.colorDepth);
			Assert::AreEqual(static_cast<uint8_t>(0), p016.bitShift);
			Assert::IsFalse(CTwoPlane420VideoFrameFormatter::ContractFor(VideoFrameEncoding::V210).IsValid());
		}

		TEST_METHOD(OddWidthsAndOtherEncodingsAreRefusedCleanly)
		{
			CTwoPlane420VideoFrameFormatter formatter;
			auto odd = FileState(VideoFrameEncoding::P010, 719, 480);
			bool refused = false;
			try { formatter.OnVideoState(odd); }
			catch (const std::runtime_error&) { refused = true; }
			Assert::IsTrue(refused);
			auto v210 = FileState(VideoFrameEncoding::V210, 1920, 1080);
			refused = false;
			try { formatter.OnVideoState(v210); }
			catch (const std::runtime_error&) { refused = true; }
			Assert::IsTrue(refused);
		}

		TEST_METHOD(NoopFormatterDeclaresTheFileContracts)
		{
			const VideoFrameEncoding encodings[] = { VideoFrameEncoding::NV12, VideoFrameEncoding::P010, VideoFrameEncoding::P016 };
			const uint8_t depths[] = { 8, 10, 16 };
			for (size_t i = 0; i < 3; ++i)
			{
				CNoopVideoFrameFormatter formatter;
				auto state = FileState(encodings[i], 1920, 1081);
				formatter.OnVideoState(state);
				Assert::AreEqual(static_cast<LONG>(state->BytesPerFrame()), formatter.GetOutFrameSize());
				const auto contract = formatter.GetOutputContract();
				Assert::IsTrue(contract.IsValid());
				Assert::AreEqual(depths[i], contract.colorDepth);
			}
		}
	};
}
