#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/drive/projector_control/LedController.h>
#include <led/LedDriveConfig.h>
#include <led/LedDriveModule.h>
#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <VP-V/modules/drive/led_measure/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

// Bug hunt 3 (2026-10-18, hunter D), Dynamic Drive: the look-ahead's clock and
// stream changes, the exit restore order, Apply inside script_pacing's measure
// window and the compensating / applyGain flags. CPU only (a fake port, the
// manual clock at real frame rates).
namespace
{
	constexpr double FILM_FPS = 24000.0 / 1001.0;    // 23.976
	constexpr double VIDEO_FPS = 60000.0 / 1001.0;   // 59.94

	RawMeasurement Frame(double code, uint32_t bright = 0, int highlight = 235, uint32_t blocks = 10000)
	{
		RawMeasurement raw{};
		raw.histogram[static_cast<int>(code)] += blocks - bright;
		raw.histogram[highlight] += bright;
		raw.blocks = blocks;
		raw.sum = static_cast<uint32_t>((code * (blocks - bright) + highlight * bright) * SUM_SCALE);
		for (int i = 0; i < GRID_W * GRID_H; ++i)
		{
			raw.cellCount[i] = 4;
			raw.cellSum[i] = static_cast<uint32_t>(4 * code * SUM_SCALE);
		}
		return raw;
	}

	bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 10000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	ProjectorConfig Port()
	{
		ProjectorConfig port;
		port.protocol = "barco_ascii";
		port.ackTimeoutMs = 60;
		port.minCommandGapMs = 0;
		port.readBack = true;
		port.ledLatencyMs = 50;
		port.ledSettleMs = 0;
		return port;
	}

	LedDriveConfig Compensate()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Compensate;
		config.ledFloor = 60;
		config.ledFloorReactive = 60;
		ParseCurve("0:60, 20:60, 40:200, 70:230, 100:255, 255:255", config.curve);
		config.rateGainUp = config.rateGainDown = 0.0;
		config.cntrAssist = false;
		config.maxCutDip = 0.0;
		config.shapedRecovery = false;
		config.blackPreliftMax = 255;
		return config;
	}

	// A captured UYVY frame: luma `y` everywhere, with the top `brightRows`
	// rows at luma 235 (a highlight).
	struct LumaFrame
	{
		static constexpr int W = 64, H = 32;
		std::vector<uint8_t> pixels;
		AnalysisLumaSource luma;
		QueuedFrameStatistics::PictureRect rect;

		LumaFrame(uint8_t y, int brightRows)
			: pixels(static_cast<size_t>(W) * H * 2, 128)
		{
			for (int row = 0; row < H; ++row)
				for (int x = 0; x < W; ++x)
					pixels[static_cast<size_t>(row) * W * 2 + x * 2 + 1] = row < brightRows ? 235 : y;
			luma.data = pixels.data();
			luma.dataBytes = pixels.size();
			luma.width = W;
			luma.height = H;
			luma.rowBytes = W * 2;
			luma.format = AnalysisLumaFormat::NativeYuv422;
			luma.encoding = VideoFrameEncoding::UYVY;
			rect.right = W;
			rect.bottom = H;
		}
	};

	// One output frame showing `sequence` at `fps`, with the walk measuring it
	// (the renderer's order: BeginFrame, then the queued-frame walk).
	void ShowAndObserve(LedDriveModule& module, uint64_t sequence, double& timeMs, double fps, const LumaFrame& frame)
	{
		module.BeginFrameAt(sequence, timeMs, fps);
		module.Lookahead().BeginWalk(1);
		module.Lookahead().ObserveFrame(sequence, 0, frame.luma, frame.rect);
		timeMs += 1000.0 / fps;
	}

	size_t Count(const std::vector<std::string>& writes, size_t from, const std::string& command)
	{
		return static_cast<size_t>(std::count(writes.begin() + std::min(from, writes.size()), writes.end(), command));
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveHunt3Tests)
	{
	public:
		// LED1: pulldown lock lost after 3000 film frames - the source sequence
		// carries on, but the rate (and so sequence x period) drops from 23.976
		// to 59.94. One black frame, then highlights: the sustained highlight
		// recovers within highlight_persist_ms, as it would on a steady stream.
		TEST_METHOD(LookaheadClockSurvivesARateChange)
		{
			LedDriveConfig config = Compensate();
			LedDriveModule module(ProjectorConfig(), config);
			Assert::IsTrue(module.WantsPredictions());
			const LumaFrame bright(120, 8), black(16, 0);
			double timeMs = 0.0;
			uint64_t sequence = 0;
			for (int i = 0; i < 3000; ++i)
				ShowAndObserve(module, ++sequence, timeMs, FILM_FPS, bright);
			FrameStats steady;
			Assert::IsTrue(module.PredictionFor(sequence, steady));
			Assert::AreEqual(steady.highlight, steady.sustainedHighlight, 1e-9, L"film: highlights sustained");
			// The lock is lost: capture frames at 59.94 from here.
			ShowAndObserve(module, ++sequence, timeMs, VIDEO_FPS, black);
			FrameStats dark;
			Assert::IsTrue(module.PredictionFor(sequence, dark));
			Assert::IsTrue(dark.highlight < steady.highlight - 50.0, L"the black frame has no highlight");
			const int persistFrames = static_cast<int>(std::ceil(config.highlightPersistMs * VIDEO_FPS / 1000.0)) + 2;
			for (int i = 0; i < persistFrames; ++i)
				ShowAndObserve(module, ++sequence, timeMs, VIDEO_FPS, bright);
			FrameStats recovered;
			Assert::IsTrue(module.PredictionFor(sequence, recovered));
			Assert::AreEqual(recovered.highlight, recovered.sustainedHighlight, 1e-9,
				L"the sustained highlight recovered within highlight_persist_ms after the rate change");
			// And back to film (lock regained): still monotonic, still recovering.
			ShowAndObserve(module, ++sequence, timeMs, FILM_FPS, black);
			for (int i = 0; i < persistFrames; ++i)
				ShowAndObserve(module, ++sequence, timeMs, FILM_FPS, bright);
			Assert::IsTrue(module.PredictionFor(sequence, recovered));
			Assert::AreEqual(recovered.highlight, recovered.sustainedHighlight, 1e-9, L"after the lock came back");
		}

		// LED1: the persistence windows restart when time goes backwards and are
		// bounded when it stalls.
		TEST_METHOD(StatsBuilderWindowsRestartAndStayBounded)
		{
			LedDriveConfig config;
			StatsBuilder builder(config);
			const RawMeasurement bright = Frame(100.0, 500, 235), black = Frame(16.0);
			for (int i = 0; i < 3000; ++i)
				builder.Build(bright, i * 1000.0 / FILM_FPS);
			// Backwards (an old seq x period clock): one black frame, then highlights.
			const double back = 3001 * 1000.0 / VIDEO_FPS;
			builder.Build(black, back);
			FrameStats stats;
			for (int i = 1; i <= 20; ++i)
				stats = builder.Build(bright, back + i * 1000.0 / VIDEO_FPS);
			Assert::AreEqual(stats.highlight, stats.sustainedHighlight, 1e-9, L"recovered after time went back");
			// A stalled clock never grows the window without limit.
			for (int i = 0; i < 10000; ++i)
				builder.Build(bright, 1.0e6);
			Assert::IsTrue(builder.HighlightHistorySize() <= StatsBuilder::MAX_WINDOW_SAMPLES);
		}

		// LED2: a hold-over drive adopted by the next file (its sequence restarts
		// at 1 after a short file): the new file is looked ahead from its first
		// frame, with no stale predictions or windows from the last one.
		TEST_METHOD(AdoptedDriveLooksAheadAtTheNewFileAtOnce)
		{
			for (const bool explicitReset : { true, false })
			{
				LedDriveModule module(ProjectorConfig(), Compensate());
				const LumaFrame bright(120, 8), dark(30, 0);
				double timeMs = 0.0;
				for (uint64_t sequence = 1; sequence <= 300; ++sequence)
					ShowAndObserve(module, sequence, timeMs, FILM_FPS, bright);
				const uint64_t epoch = module.LookaheadEpoch();
				if (explicitReset)
					module.ResetLookahead();   // the adopting renderer
				// The new file: its first frame shown, the walk measures it and the queue.
				module.BeginFrameAt(1, timeMs, FILM_FPS);
				Assert::IsTrue(module.LookaheadEpoch() > epoch, explicitReset ?
					L"ResetLookahead starts a new stream" : L"the sequence restart starts a new stream");
				module.Lookahead().BeginWalk(4);
				for (uint64_t sequence = 1; sequence <= 5; ++sequence)
					module.Lookahead().ObserveFrame(sequence, static_cast<size_t>(sequence - 1), dark.luma, dark.rect);
				for (uint64_t sequence = 1; sequence <= 5; ++sequence)
				{
					FrameStats predicted;
					Assert::IsTrue(module.PredictionFor(sequence, predicted), L"the new file's frames are measured");
					Assert::IsTrue(predicted.apl < 60.0, L"from the new file's pictures");
					Assert::IsTrue(predicted.sustainedHighlight <= predicted.highlight + 1e-9,
						L"no window carried over from the last file");
				}
				FrameStats stale;
				Assert::IsFalse(module.PredictionFor(250, stale), L"the last file's predictions are gone");
			}
		}

		// LED2: a small step back (a repeated earlier frame) is not a new stream.
		TEST_METHOD(SmallSequenceStepBackKeepsTheLookahead)
		{
			LedDriveModule module(ProjectorConfig(), Compensate());
			const LumaFrame bright(120, 8);
			double timeMs = 0.0;
			for (uint64_t sequence = 1; sequence <= 100; ++sequence)
				ShowAndObserve(module, sequence, timeMs, FILM_FPS, bright);
			const uint64_t epoch = module.LookaheadEpoch();
			module.BeginFrameAt(98, timeMs, FILM_FPS);
			Assert::AreEqual(epoch, module.LookaheadEpoch());
			FrameStats kept;
			Assert::IsTrue(module.PredictionFor(100, kept));
		}

		// LED4: Apply inside script_pacing's measure window of a dark step (LPW1
		// down, then CNTR up): the completed schedule keeps the script's order,
		// LPW1 first.
		TEST_METHOD(ApplyInTheMeasureWindowKeepsTheDarkStepOrder)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config;
			config.enabled = true;
			config.mode = Mode::Script;
			config.scriptPacing = true;
			config.scriptMeasureMs = 1200;
			config.scriptEchoMs = 50;
			config.tickMs = 1000;
			// Bright pictures at the ceiling; APL 50 a step down.
			ParseCurve("0:150, 40:150, 60:255, 255:255", config.curve);
			TestClock clock;
			LedDriveModule module(Port(), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(80.0));   // bright: light 255, CNTR 50 - nothing to send
			Assert::IsTrue(clock.Run(3000, 10.0));
			Assert::AreEqual(255, projector->light);
			Assert::AreEqual(50, projector->contrast);
			// A tick has just decided (nothing sent: its count rises at once); the
			// next decides scriptMeasureMs later and sends scriptMeasureMs after that.
			const int ticks = module.Ticks();
			Assert::IsTrue(clock.RunUntil([&] { return module.Ticks() > ticks; }, 3000, 5.0));
			module.SubmitMeasurement(Frame(50.0));   // APL 50, no highlight: LPW1 down, CNTR 51
			Assert::IsTrue(clock.Run(1800, 5.0));   // inside the next tick's measure window
			const size_t before = projector->Writes().size();
			Assert::AreEqual(255, projector->light, L"no LPW1 yet");
			LedDriveConfig tuned = config;
			tuned.highlightToleranceStops = 0.5;   // a compensate-only value: the script is unchanged
			Assert::IsTrue(module.Reconfigure(Port(), tuned));
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }, 4000, 5.0), L"CNTR 51 sent");
			Assert::IsTrue(clock.Run(500, 10.0));
			const auto writes = projector->Writes();
			size_t firstLight = writes.size(), firstContrast = writes.size();
			for (size_t i = before; i < writes.size(); ++i)
			{
				if (firstLight == writes.size() && writes[i].rfind(":LPW1", 0) == 0 && writes[i] != ":LPW1?\r")
					firstLight = i;
				if (firstContrast == writes.size() && writes[i] == ":CNTR51\r")
					firstContrast = i;
			}
			Assert::IsTrue(firstLight < writes.size(), L"an LPW1 was sent");
			Assert::IsTrue(firstLight < firstContrast, L"LPW1 before CNTR (the dark step's order)");
			Assert::AreEqual(static_cast<size_t>(1), Count(writes, before, ":CNTR51\r"), L"CNTR 51 once");
			Assert::IsTrue(projector->light < 255);
			module.Stop();
		}

		// LED5: compensating / applyGain are read on the LED thread while
		// Reconfigure() writes them; a smoke test under switches (the fields are
		// atomic, so the reads cannot tear or race).
		TEST_METHOD(ModeSwitchesWhileTheScriptThreadRuns)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig script;
			script.enabled = true;
			script.mode = Mode::Script;
			script.scriptPacing = false;
			script.tickMs = 50;
			script.scriptSmoothing = true;
			LedDriveConfig compensate = Compensate();
			TestClock clock;
			LedDriveModule module(Port(), script,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
			module.SubmitMeasurement(Frame(50.0));
			std::atomic<bool> done{ false };
			std::thread switcher([&]
			{
				for (int i = 0; i < 200; ++i)
					module.Reconfigure(Port(), i % 2 ? compensate : script);
				done.store(true);
			});
			double timeMs = 0.0;
			uint64_t sequence = 0;
			while (!done.load())
			{
				timeMs += 1000.0 / VIDEO_FPS;
				clock.To(timeMs);
				module.BeginFrameAt(++sequence, timeMs, VIDEO_FPS);
				module.SubmitMeasurement(Frame(50.0), sequence);
			}
			switcher.join();
			Assert::IsTrue(module.Reconfigure(Port(), script));
			Assert::IsTrue(clock.Run(500, 10.0));
			Assert::IsTrue(module.DrivingProjector());
			module.Stop();
		}
	};
}
