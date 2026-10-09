#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <led/LedController.h>
#include <led/LedDriveModule.h>
#include <led/LedLookahead.h>
#include <led/LedMeasurement.h>
#include <led/LedRawTransfer.h>
#include <vprenderer/AlphaQueuePolicy.h>

#include "LedDriveTestSupport.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

// docs/VP-led-drive-delta-1.md item 6 (as amended by delta-1-answers): flux
// table, latency timing, ramps and deadband, the early-ramp bound, the raw
// lookahead path and its correction, and the three-frame lookahead clamp.
namespace
{
	constexpr double FRAME_MS = 1000.0 / 60.0;

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

	LedDriveConfig Compensate()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Compensate;
		config.ledFloor = 60;
		ParseCurve("0:60, 20:60, 40:200, 70:230, 100:255, 255:255", config.curve);
		return config;
	}

	bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 3000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	ProjectorConfig Port(int latencyMs)
	{
		ProjectorConfig port;
		port.protocol = "barco_ascii";
		port.ackTimeoutMs = 60;
		port.minCommandGapMs = 0;
		port.ledLatencyMs = latencyMs;
		return port;
	}

	std::string Write(const std::string& text)
	{
		char directory[MAX_PATH]{}, file[MAX_PATH]{};
		GetTempPathA(MAX_PATH, directory);
		GetTempFileNameA(directory, "vpd", 0, file);
		std::ofstream(file, std::ios::trunc) << text;
		return file;
	}

	bool Validates(const std::string& text, std::string& error)
	{
		const std::string file = Write(text);
		ConfigFile config;
		const bool loaded = config.Load(file.c_str());
		const bool valid = loaded && Validate(config, error);
		DeleteFileA(file.c_str());
		return valid;
	}

	// Lookahead predictions for sequences [from, to): `bright` before `cut`,
	// near black from `cut` on.
	void Predict(LedDriveModule& module, uint64_t from, uint64_t to, uint64_t cut)
	{
		StatsBuilder builder(module.Config());
		for (uint64_t sequence = from; sequence < to; ++sequence)
			module.SubmitPrediction(sequence, builder.Build(Frame(sequence < cut ? 120.0 : 18.0), sequence * FRAME_MS));
	}

	// Renders sequences 1.. at 60 Hz until `until`; returns the modelled LED
	// level seen on each sequence.
	std::vector<double> Render(LedDriveModule& module, uint64_t until, std::vector<double>* gains = nullptr)
	{
		std::vector<double> levels(1, 255.0);
		for (uint64_t sequence = 1; sequence <= until; ++sequence)
		{
			const CompensationGain gain = module.BeginFrameAt(sequence, sequence * FRAME_MS);
			levels.push_back(module.ModelledLight());
			if (gains) gains->push_back(gain.gain);
		}
		return levels;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveDelta1Tests)
	{
	public:
		TEST_METHOD(FluxTableInterpolatesClampsAndDefaultsToLinear)
		{
			std::vector<std::pair<double, double>> table;
			Assert::IsTrue(ParseFluxTable("60:0.55, 80:0.78, 100:1.00", table));
			const FluxModel flux(table);
			Assert::IsFalse(flux.Linear());
			Assert::AreEqual(0.665, flux.Flux(70), 1e-12, L"Piecewise-linear between points");
			Assert::AreEqual(0.55, flux.Flux(40), 1e-12, L"Clamped below the table");
			Assert::AreEqual(1.0, flux.Flux(200), 1e-12, L"Clamped above the table");
			Assert::AreEqual(1.0 / 0.78, flux.Gain(100, 80), 1e-12, L"gain = flux(ceiling) / flux(level)");
			Assert::AreEqual(80, flux.LevelFor(0.78, 0, 100));
			Assert::AreEqual(81, flux.LevelFor(0.79, 0, 100));

			const FluxModel linear;
			Assert::IsTrue(linear.Linear());
			Assert::AreEqual(255.0 / 120.0, linear.Gain(255, 120), 1e-12);
			Assert::AreEqual(255.0 / 120.0, LedGain(255, 120.0), 1e-12);
			Assert::AreEqual(128, linear.LevelFor(0.5, 0, 255));

			for (const char* bad : { "80:0.78, 60:0.55", "60:0", "300:1", "60:0.5, 60:0.6", "lots" })
				Assert::IsFalse(ParseFluxTable(bad, table), std::wstring(bad, bad + strlen(bad)).c_str());

			// The controller's gain and headroom use the flux ratio.
			LedDriveConfig config = Compensate();
			CompensateController controller(config, flux);
			StatsBuilder builder(config);
			ControllerOutput out;
			for (int frame = 0; frame < 600; ++frame)
				out = controller.Tick(builder.Build(Frame(18.0), frame * FRAME_MS), FRAME_MS);
			Assert::AreEqual(60, out.light);
			Assert::AreEqual(1.0 / 0.55, out.gain, 1e-9, L"Dark scene at the floor: flux ratio, not 255/60");
		}

		TEST_METHOD(LatencyMovesToTheProjectorAndOldKeysAreRejected)
		{
			std::string error;
			const std::string good = "[projector.Default]\nled_latency_ms: 120\nflux_table: 60:0.55, 100:1.0\n"
				"[led_drive.Default]\nrate_up: 300\nrate_down: 90\ndeadband: 4\nearly_ramp: false\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const ProjectorConfig projector = ReadProjectorConfig(config);
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::AreEqual(120, projector.ledLatencyMs);
			Assert::AreEqual<size_t>(2, projector.fluxTable.size());
			Assert::AreEqual(300.0, led.rateUp, 1e-12);
			Assert::AreEqual(90.0, led.rateDown, 1e-12);
			Assert::AreEqual(4, led.deadband);
			Assert::IsFalse(led.earlyRamp);
			Assert::AreEqual(80, ProjectorConfig().ledLatencyMs, L"Default led_latency_ms");

			Assert::IsFalse(Validates("[led_drive.Default]\nled_response_ms: 80\n", error));
			Assert::IsTrue(error.find("led_latency_ms") != std::string::npos &&
				error.find("[projector.Default]") != std::string::npos, L"The message names the new place");
			Assert::IsFalse(Validates("[led_drive.Default]\nlead_ms: 40\n", error));
			Assert::IsFalse(Validates("[projector.Default]\nflux_table: 80:0.7, 60:0.5\n", error));
			Assert::IsFalse(Validates("[projector.Default]\nled_latency_ms: -1\n", error));
		}

		TEST_METHOD(CompensateRampsAtTheRateLimitsAndHoldsInsideTheDeadband)
		{
			LedDriveConfig config = Compensate();
			config.deadband = 10;
			CompensateController controller(config);
			StatsBuilder builder(config);
			double now = 0.0;
			auto step = [&](double code)
			{
				const ControllerOutput out = controller.Tick(builder.Build(Frame(code), now), FRAME_MS);
				now += FRAME_MS;
				return out;
			};
			for (int frame = 0; frame < 30; ++frame) step(120.0);
			Assert::AreEqual(255, controller.Light());

			// A step down to APL 30 (target 130): at most rate_down / 60 per frame.
			double previous = controller.Level();
			int frames = 0;
			while (controller.Light() != 130 && frames < 1000)
			{
				step(30.0);
				Assert::IsTrue(previous - controller.Level() <= 120.0 / 60.0 + 1e-9, L"rate_down");
				Assert::IsTrue(controller.Level() <= previous, L"Only down");
				previous = controller.Level();
				++frames;
			}
			Assert::AreEqual(130, controller.Light());
			Assert::IsTrue(frames >= 62 && frames <= 64, std::to_wstring(frames).c_str());

			// Inside the deadband nothing moves: APL 31 wants 137.
			for (int frame = 0; frame < 120; ++frame) step(31.0);
			Assert::AreEqual(130, controller.Light(), L"7 levels is inside a deadband of 10");
			// Outside it, it moves: APL 32 wants 144.
			for (int frame = 0; frame < 120; ++frame) step(32.0);
			Assert::AreEqual(144, controller.Light());

			// Back up: at most rate_up / 60 per frame.
			previous = controller.Level();
			for (int frame = 0; frame < 120; ++frame)
			{
				step(120.0);
				Assert::IsTrue(controller.Level() - previous <= 400.0 / 60.0 + 1e-9, L"rate_up");
				previous = controller.Level();
			}
			Assert::AreEqual(255, controller.Light());
		}

		TEST_METHOD(CutsAreRampedAndFadesAreCappedByRateDown)
		{
			const LedDriveConfig config = Compensate();
			CompensateController controller(config);
			StatsBuilder builder(config);
			double now = 0.0;
			for (int frame = 0; frame < 30; ++frame, now += FRAME_MS)
				controller.Tick(builder.Build(Frame(120.0), now), FRAME_MS);
			const FrameStats cut = builder.Build(Frame(17.0), now);
			Assert::IsTrue(cut.cut);
			const ControllerOutput out = controller.Tick(cut, FRAME_MS);
			Assert::AreEqual(config.ledFloorBlack, out.lightTarget, L"A cut to black targets led_floor_black");
			Assert::AreEqual(253, out.light, L"An unseen cut is ramped like any change");

			// A fast fade: the APL halves every frame, but the LED moves at most
			// rate_down / 60 per frame.
			CompensateController fading(config);
			StatsBuilder fadeStats(config);
			double t = 0.0;
			for (int frame = 0; frame < 30; ++frame, t += FRAME_MS)
				fading.Tick(fadeStats.Build(Frame(120.0), t), FRAME_MS);
			double apl = 120.0, previous = fading.Level();
			bool faded = false;
			for (int frame = 0; frame < 40; ++frame, t += FRAME_MS)
			{
				apl = 16.0 + (apl - 16.0) * 0.9;
				const FrameStats stats = fadeStats.Build(Frame(apl), t);
				const ControllerOutput o = fading.Tick(stats, FRAME_MS);
				faded = faded || stats.fade;
				Assert::IsTrue(previous - fading.Level() <= 120.0 / 60.0 + 1e-9);
				if (stats.fade) Assert::IsTrue(o.holdGain);
				previous = fading.Level();
			}
			Assert::IsTrue(faded);
		}

		TEST_METHOD(EarlyRampDimsOnlyToTheCurrentHighlightLimitAndBrightensFreely)
		{
			const LedDriveConfig config = Compensate();
			auto settleThenLookAhead = [&config](const RawMeasurement& current, double aheadCode, int frames)
			{
				CompensateController controller(config);
				StatsBuilder builder(config);
				StatsBuilder future(config);
				double now = 0.0;
				for (int frame = 0; frame < 60; ++frame, now += FRAME_MS)
					controller.Tick(builder.Build(current, now), FRAME_MS);
				Lookahead ahead;
				ahead.stats = future.Build(Frame(aheadCode), 0.0);
				ahead.cutAhead = true;
				ahead.framesAhead = 3;
				double previous = controller.Level();
				for (int frame = 0; frame < frames; ++frame, now += FRAME_MS)
				{
					controller.Tick(builder.Build(current, now), FRAME_MS, &ahead);
					const double moved = std::abs(controller.Level() - previous);
					Assert::IsTrue(moved <= 400.0 / 60.0 + 1e-9, L"Early ramps keep the rate limits");
					previous = controller.Level();
				}
				return controller.Light();
			};

			// Bright scene, nothing bright in it: dims all the way to the next
			// scene's level ahead of the cut.
			Assert::AreEqual(config.ledFloorBlack, settleThenLookAhead(Frame(120.0), 18.0, 600));
			// Bright scene with 1% of the picture at 230: dimming stops where those
			// highlights would pass highlight_tolerance_stops.
			const double h = std::pow((230.0 - 16.0) / 219.0, config.compensationEotf);
			const int limit = static_cast<int>(std::ceil(h / std::pow(2.0, config.highlightToleranceStops) * 255.0));
			Assert::AreEqual(limit, settleThenLookAhead(Frame(120.0, 100, 230), 18.0, 600));
			// Dark scene, a bright one ahead: brightens to it, unconstrained.
			Assert::AreEqual(255, settleThenLookAhead(Frame(18.0), 120.0, 60));

			// early_ramp: false ignores the lookahead.
			LedDriveConfig off = config;
			off.earlyRamp = false;
			CompensateController controller(off);
			StatsBuilder builder(off);
			Lookahead ahead;
			ahead.stats = StatsBuilder(off).Build(Frame(18.0), 0.0);
			ahead.cutAhead = true;
			for (int frame = 0; frame < 120; ++frame)
				controller.Tick(builder.Build(Frame(120.0), frame * FRAME_MS), FRAME_MS, &ahead);
			Assert::AreEqual(255, controller.Light());
		}

		TEST_METHOD(WithoutLookaheadTheGainLandsLedLatencyAfterTheCommand)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveModule module(Port(80), Compensate(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(18.0));
			std::vector<double> gains;
			// Sequence 0: no lookahead for these frames.
			std::vector<double> levels(1, 255.0);
			for (int frame = 1; frame <= 8; ++frame)
			{
				gains.push_back(module.BeginFrameAt(0, frame * FRAME_MS).gain);
				levels.push_back(module.ModelledLight());
			}
			Assert::AreEqual(5, module.LatencyFrames(), L"80 ms at 60 Hz, in whole frames");
			// The first command (253) went out on frame 1; LED and gain land on 6.
			for (int frame = 1; frame <= 5; ++frame)
			{
				Assert::AreEqual(255.0, levels[frame], 1e-12);
				Assert::AreEqual(1.0, gains[frame - 1], 1e-12);
			}
			Assert::AreEqual(253.0, levels[6], 1e-12);
			Assert::AreEqual(255.0 / 253.0, gains[5], 1e-12);
			Assert::IsTrue(WaitFor([&] { return projector->light < 255; }), L"The command reached the projector");
		}

		TEST_METHOD(WithLookaheadTheCommandPrecedesItsFrameByLedLatency)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveModule module(Port(50), Compensate(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(120.0));
			module.SetLookaheadDepth(3);
			const uint64_t cut = 20;
			Predict(module, 1, 40, cut);
			std::vector<double> gains;
			const std::vector<double> levels = Render(module, cut + 2, &gains);
			Assert::AreEqual(3, module.LatencyFrames());
			for (uint64_t sequence = 1; sequence < cut; ++sequence)
			{
				Assert::AreEqual(255.0, levels[sequence], 1e-12, L"Nothing lands before the cut");
				Assert::AreEqual(1.0, gains[sequence - 1], 1e-12);
			}
			Assert::IsTrue(levels[cut] < 255.0, L"Sent 3 frames ahead, the LED lands on the cut frame");
			Assert::AreEqual(255.0 / levels[cut], gains[cut - 1], 1e-9, L"and so does the gain");
		}

		TEST_METHOD(EffectiveLookaheadIsClampedToTheFramesActuallyQueued)
		{
			// Until Stage 5 the queue holds the current frame plus target_frames
			// (3): an 8-frame setting still sees only 3 frames ahead.
			struct Frame { bool cadenceRepeat = false; };
			const std::deque<Frame> queue(4);
			const auto window = AlphaQueuePolicy::SelectActivePicturePreview(queue, 8, 8);
			Assert::AreEqual<size_t>(3, window.effectiveFutureFrames);
			Assert::AreEqual<size_t>(4, window.indices.size());

			// The module only looks as far as the walk saw, even with predictions
			// further out: a cut 8 frames away is acted on only once in range.
			auto firstLanding = [](size_t depth)
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				LedDriveModule module(Port(50), Compensate(),
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(::Frame(120.0));
				module.SetLookaheadDepth(depth);
				Assert::AreEqual(depth, module.LookaheadDepth());
				Predict(module, 1, 60, 30);
				const std::vector<double> levels = Render(module, 30);
				for (uint64_t sequence = 1; sequence < levels.size(); ++sequence)
					if (levels[sequence] < 255.0) return sequence;
				return uint64_t{ 0 };
			};
			// Commands land 3 frames (50 ms) after they are sent.
			Assert::AreEqual<uint64_t>(30, firstLanding(window.effectiveFutureFrames),
				L"Three frames ahead: the ramp starts 3 frames early and lands on the cut");
			Assert::AreEqual<uint64_t>(25, firstLanding(8),
				L"With a deeper queue (Stage 5) the same cut is seen 8 frames ahead");
		}

		TEST_METHOD(RawFramesAreMeasuredThroughTheTransferLikeTheGpuPass)
		{
			// A UYVY frame at luma code 126 (10-bit 504), with a bright square.
			const int width = 96, height = 54;
			std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 2);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					uint8_t* p = &pixels[(static_cast<size_t>(y) * width + x) * 2];
					p[0] = 128;
					p[1] = (x < 12 && y < 12) ? 235 : 126;
				}
			AnalysisLumaSource luma;
			luma.data = pixels.data();
			luma.dataBytes = pixels.size();
			luma.width = width;
			luma.height = height;
			luma.rowBytes = static_cast<size_t>(width) * 2;
			luma.format = AnalysisLumaFormat::NativeYuv422;
			luma.encoding = VideoFrameEncoding::UYVY;
			luma.colorspace = ColorSpace::REC_709;
			Assert::IsTrue(luma.IsValid());
			Assert::IsFalse(LumaIsFullRange(luma));

			QueuedFrameStatistics::PictureRect rect;
			rect.left = 12;
			rect.right = width;
			rect.bottom = height;
			RawMeasurement raw;
			Assert::IsTrue(MeasureRawFrame(luma, rect, IdentityRawTransfer(), raw));
			Assert::AreEqual<uint32_t>(28 * 18, raw.blocks);
			const double signal = (504.0 - 64.0) / 876.0;
			const double identity = std::lround(signal * 1023) / 1023.0;
			Assert::AreEqual(16.0 + 219.0 * identity, static_cast<double>(raw.sum) / SUM_SCALE / raw.blocks, 0.05,
				L"Identity transfer: the code comes back, the square outside the rect is not seen");

			// SDR BT.1886 source to a 2.2 target. The table itself is checked
			// against libplacebo rendering in LedRawTransferMatchesTheRenderedOutput.
			pl_color_space source{}, target{};
			source.primaries = target.primaries = PL_COLOR_PRIM_BT_709;
			source.transfer = PL_COLOR_TRC_BT_1886;
			target.transfer = PL_COLOR_TRC_GAMMA22;
			const RawTransfer transfer = BuildRawTransfer(source, target, &pl_color_map_default_params);
			Assert::IsTrue(transfer[64] > 64.0 / 1023.0 + 0.01, L"BT.1886 black level lifts the shadows");
			Assert::IsTrue(MeasureRawFrame(luma, rect, transfer, raw));
			Assert::AreEqual(16.0 + 219.0 * transfer[514],
				static_cast<double>(raw.sum) / SUM_SCALE / raw.blocks, 0.05);
			Assert::AreNotEqual(RawTransferSignature(source, target, &pl_color_map_default_params),
				RawTransferSignature(source, source, &pl_color_map_default_params), L"A profile change rebuilds");
		}

		TEST_METHOD(CorrectionRatioConvergesOnASyntheticMismatch)
		{
			LedDriveModule module(ProjectorConfig(), Compensate());
			StatsBuilder builder(module.Config());
			// The raw path under-reads by 20% above black from frame 100 on.
			for (uint64_t sequence = 1; sequence <= 1500; ++sequence)
			{
				const double exact = 100.0;
				const double predicted = sequence < 100 ? exact : 16.0 + (exact - 16.0) / 1.2;
				module.SubmitPrediction(sequence, builder.Build(Frame(predicted), sequence * FRAME_MS));
				module.SubmitMeasurement(Frame(exact), sequence);
				if (sequence == 99) Assert::AreEqual(1.0, module.CorrectionRatio(), 1e-9);
			}
			Assert::AreEqual(1.2, module.CorrectionRatio(), 0.01);
			// A new transfer (profile change) starts over.
			module.SetRawTransfer(std::make_shared<const RawTransfer>(IdentityRawTransfer()));
			Assert::AreEqual(1.0, module.CorrectionRatio(), 1e-12);
		}

		TEST_METHOD(ScriptModeIgnoresTheLookahead)
		{
			LedDriveConfig script;
			script.enabled = true;
			script.mode = Mode::Script;
			LedDriveModule module(ProjectorConfig(), script);
			Assert::IsFalse(module.WantsPredictions());
			// The walk's observer measures nothing for script mode.
			std::vector<uint8_t> pixels(64 * 32 * 2, 128);
			AnalysisLumaSource luma;
			luma.data = pixels.data();
			luma.dataBytes = pixels.size();
			luma.width = 64;
			luma.height = 32;
			luma.rowBytes = 128;
			luma.format = AnalysisLumaFormat::NativeYuv422;
			luma.encoding = VideoFrameEncoding::UYVY;
			QueuedFrameStatistics::PictureRect rect;
			rect.right = 64;
			rect.bottom = 32;
			module.Lookahead().BeginWalk(3);
			module.Lookahead().ObserveFrame(1, 0, luma, rect);
			Assert::AreEqual(1.0, module.CorrectionRatio(), 1e-12);
			Assert::IsTrue(LedDriveModule(ProjectorConfig(), Compensate()).WantsPredictions());
		}
	};
}
