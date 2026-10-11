#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <led/LedController.h>
#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	// One frame: most blocks at `code`, `bright` blocks at `highlight`.
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
		config.ledFloorReactive = 60;
		ParseCurve("0:60, 20:60, 40:200, 70:230, 100:255, 255:255", config.curve);
		return config;
	}

	// Feeds frames at 60 Hz for `ms` and returns the last statistics.
	FrameStats Play(StatsBuilder& builder, double& now, double ms, const RawMeasurement& raw)
	{
		FrameStats stats;
		for (double end = now + ms; now < end; now += 1000.0 / 60.0)
			stats = builder.Build(raw, now);
		return stats;
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
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveCompensateTests)
	{
	public:
		TEST_METHOD(DarkSceneDimsTheLedAndCompensatesExactly)
		{
			CompensateController controller(Compensate());
			StatsBuilder builder(Compensate());
			double now = 0.0;
			ControllerOutput out;
			for (int tick = 0; tick < 100; ++tick)
				out = controller.Tick(Play(builder, now, 400.0, Frame(18.0)), 400);
			Assert::AreEqual(60, out.light, L"Dark scene reaches led_floor");
			Assert::AreEqual(255.0 / 60.0, out.gain, 1e-9, L"gain = ceiling / LED level");
			Assert::IsTrue(out.gainDomain == GainDomain::Linear);
			Assert::AreEqual(0.0, out.capWeight, 1e-12);
			Assert::AreEqual(255.0 / 120.0, LedGain(255, 120.0), 1e-12);
		}

		TEST_METHOD(ThreeFrameFlashChangesNothing)
		{
			const LedDriveConfig config = Compensate();
			StatsBuilder plainStats(config), flashStats(config);
			CompensateController plain(config), flashed(config);
			double t1 = 0.0, t2 = 0.0;
			for (int tick = 0; tick < 60; ++tick)
			{
				plain.Tick(Play(plainStats, t1, 400.0, Frame(18.0)), 400);
				flashed.Tick(Play(flashStats, t2, 400.0, Frame(18.0)), 400);
			}
			// Lightning: three frames with a fifth of the picture at white.
			FrameStats flash;
			for (int frame = 0; frame < 3; ++frame)
			{
				flash = flashStats.Build(Frame(18.0, 2000), t2);
				t2 += 1000.0 / 60.0;
			}
			Assert::AreEqual(235.0, flash.highlight, 1e-9);
			Assert::IsTrue(flash.sustainedHighlight < 30.0, L"A flash is not a sustained highlight");
			const auto a = plain.Tick(Play(plainStats, t1, 50.0, Frame(18.0)), 400);
			const auto b = flashed.Tick(flash, 400);
			Assert::AreEqual(a.light, b.light);
			Assert::AreEqual(a.gain, b.gain, 1e-12, L"No gain change for a flash");
			Assert::AreEqual(0.0, flashed.Protection(), 1e-12);
		}

		TEST_METHOD(SustainedSmallHighlightEngagesSmoothlyThroughTheKnee)
		{
			const LedDriveConfig config = Compensate();
			StatsBuilder builder(config);
			CompensateController controller(config);
			double now = 0.0;
			for (int tick = 0; tick < 100; ++tick)
				controller.Tick(Play(builder, now, 400.0, Frame(18.0)), 400);
			Assert::AreEqual(60, controller.Light());
			// Headlights: 0.6% of the picture near white, held.
			const RawMeasurement headlights = Frame(18.0, 60, 230);
			int previousLight = controller.Light();
			double previousProtection = 0.0;
			ControllerOutput out;
			for (int tick = 0; tick < 80; ++tick)
			{
				out = controller.Tick(Play(builder, now, 100.0, headlights), 100);
				Assert::IsTrue(controller.Light() >= previousLight, L"The LED only comes up");
				Assert::IsTrue(controller.Protection() >= previousProtection, L"Protection engages monotonically");
				previousLight = controller.Light();
				previousProtection = controller.Protection();
			}
			Assert::AreEqual(1.0, controller.Protection(), 1e-9);
			const double h = std::pow((230.0 - 16.0) / 219.0, config.compensationEotf);   // the projector exponent (2.41 since delta 4)
			const int expectedFloor = static_cast<int>(std::ceil(h / std::pow(2.0, config.highlightToleranceStops) * 255));
			Assert::AreEqual(expectedFloor, out.lightTarget, L"Lowest LED within the tolerance");
			Assert::AreEqual(expectedFloor, out.light);
			Assert::IsTrue(out.capWeight < 0.05, L"Small highlights go through the knee, not the global gain");
			Assert::AreEqual(255.0 / expectedFloor, out.gain, 0.05 * 255.0 / expectedFloor,
				L"The dark surround stays fully compensated");
		}

		TEST_METHOD(HighlightsUnderHalfAPercentCostNothing)
		{
			const LedDriveConfig config = Compensate();
			auto settle = [&config](uint32_t brightBlocks)
			{
				StatsBuilder builder(config);
				CompensateController controller(config);
				double now = 0.0;
				for (int tick = 0; tick < 120; ++tick)
					controller.Tick(Play(builder, now, 200.0, Frame(18.0, brightBlocks, 230)), 200);
				return controller.Light();
			};
			const int lamp = settle(30);       // 0.3% of the picture
			const int headlights = settle(100); // 1%
			Assert::AreEqual(config.ledFloor, lamp, L"A small lamp lets a dark scene go to the floor");
			Assert::IsTrue(headlights > lamp, L"A larger sustained highlight raises the floor");
		}

		TEST_METHOD(LargeSustainedHighlightLowersTheGlobalGain)
		{
			const LedDriveConfig config = Compensate();
			StatsBuilder builder(config);
			CompensateController controller(config);
			double now = 0.0;
			for (int tick = 0; tick < 60; ++tick)
				controller.Tick(Play(builder, now, 400.0, Frame(18.0)), 400);
			// A lit window: 10% of the picture near white.
			ControllerOutput out;
			for (int tick = 0; tick < 200; ++tick)
				out = controller.Tick(Play(builder, now, 100.0, Frame(18.0, 1000, 230)), 100);
			const double h = std::pow((230.0 - 16.0) / 219.0, config.compensationEotf);   // the projector exponent (2.41 since delta 4)
			Assert::AreEqual(1.0, out.capWeight, 1e-9);
			Assert::IsTrue(out.gain <= 1.0 / h + 1e-9, L"The window is not pushed past white");
			Assert::AreEqual(static_cast<int>(std::ceil(h * 255)), out.lightTarget, L"No compression allowed");
		}

		TEST_METHOD(FadeToBlackTracksDownAndHoldsTheGain)
		{
			const LedDriveConfig config = Compensate();
			StatsBuilder builder(config);
			CompensateController controller(config);
			double now = 0.0;
			for (int tick = 0; tick < 40; ++tick)
				controller.Tick(Play(builder, now, 400.0, Frame(120.0)), 400);
			Assert::AreEqual(255, controller.Light());
			double apl = 120.0;
			ControllerOutput out;
			int fadeTicks = 0;
			while (apl > 20.0)
			{
				FrameStats stats;
				for (int frame = 0; frame < 6; ++frame)
				{
					apl -= 1.5;
					stats = builder.Build(Frame(apl), now);
					now += 1000.0 / 60.0;
				}
				out = controller.Tick(stats, 100);
				if (stats.fade)
				{
					++fadeTicks;
					Assert::IsTrue(out.holdGain);
					Assert::AreEqual(config.ledFloorBlack, out.lightTarget);
				}
			}
			Assert::IsTrue(fadeTicks > 5);
			Assert::IsTrue(out.light < 120, L"The LED follows the fade faster than the scene drift would");
		}

		TEST_METHOD(ModuleCompensatesAlongTheModelledLedLevel)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			port.ackTimeoutMs = 60;
			port.minCommandGapMs = 0;
			port.readBack = true;
			port.ledLatencyMs = 80;
			{
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				Assert::AreEqual(1.0, module.CurrentGain().gain, 1e-9);
				module.SubmitMeasurement(Frame(18.0));
				CompensationGain gain;
				for (int frame = 1; frame <= 30; ++frame)
				{
					gain = module.BeginFrameAt(0, frame * 1000.0 / 60.0);
					// Let the driver send before the next frame looks.
					WaitFor([&] { return !module.ProjectorStatus().busy; });
				}
				Assert::IsTrue(WaitFor([&] { return projector->light < 255; }));
				Assert::IsTrue(gain.active);
				Assert::IsTrue(gain.domain == GainDomain::Linear);
				const double modelled = module.ModelledLight();
				Assert::IsTrue(modelled < 255.0 && modelled > 60.0);
				Assert::AreEqual(255.0 / modelled, gain.gain, 1e-9, L"The gain follows the modelled level");
				Assert::AreEqual(module.CurrentGain().gain, gain.gain, 1e-12);
				for (const auto& write : projector->Writes())
					Assert::IsFalse(write.rfind(":CNTR", 0) == 0 && write != ":CNTR?\r",
						L"compensate never touches the projector's contrast");
			}
			Assert::AreEqual(255, projector->light);
		}

		// Delta 4: the boost's base is the output gamma VP encodes for, not the
		// projector's compensation_eotf (now 2.41). Before the fix the exponent
		// was compensation_eotf / gamma_boost, so 2.41 would have switched the
		// boost off (2.41 / 2.4 = 1.004).
		TEST_METHOD(GammaBoostIsIndependentOfTheProjectorExponent)
		{
			LedDriveConfig led;
			Assert::AreEqual(2.41, led.compensationEotf, 1e-12, L"compensation_eotf defaults to the FL35 fit");
			led.gammaBoost = 2.4;
			for (const double eotf : { 2.2, 2.41, 2.6 })
			{
				led.compensationEotf = eotf;
				Assert::AreEqual(2.2 / 2.4, BoostExponent(led), 1e-12);
				Assert::AreEqual(2.2 / 2.4, BoostExponent(led, 2.2), 1e-12);
			}
			Assert::AreEqual(1.0, BoostExponent(led, 2.4), 1e-12, L"A 2.4 output needs no boost to look 2.4");
			Assert::AreEqual(1.0, BoostExponent(0.0, 2.2), 1e-12, L"gamma_boost off");
		}

		TEST_METHOD(GammaBoostIsConfiguredInTheModule)
		{
			LedDriveConfig off;
			Assert::AreEqual(1.0, BoostExponent(off), 1e-12);
			LedDriveConfig boosted;
			boosted.gammaBoost = 2.4;
			Assert::AreEqual(2.2 / 2.4, BoostExponent(boosted), 1e-12);

			char directory[MAX_PATH]{}, file[MAX_PATH]{};
			GetTempPathA(MAX_PATH, directory);
			GetTempFileNameA(directory, "vpb", 0, file);
			{
				std::ofstream(file, std::ios::trunc) << "[led_drive.Default]\nenabled: false\ngamma_boost: 2.4\n"
					"compensation_eotf: 2.2\nhighlight_area_small: 0.01\nhighlight_area_large: 0.1\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(file));
			std::string error;
			Assert::IsTrue(Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
			const LedDriveConfig read = ReadLedDriveConfig(config);
			Assert::IsFalse(read.enabled, L"The boost does not need the LED module enabled");
			Assert::AreEqual(2.2 / 2.4, BoostExponent(read), 1e-12);
			// Owner decision (design review SF-1): switching the LED drive off
			// or changing its mode never changes the picture's gamma.
			for (const bool enabled : { false, true })
				for (const Mode mode : { Mode::LogOnly, Mode::Script, Mode::Compensate })
				{
					LedDriveConfig any = read;
					any.enabled = enabled;
					any.mode = mode;
					Assert::AreEqual(2.2 / 2.4, BoostExponent(any), 1e-12);
				}
			Assert::AreEqual(0.1, read.highlightAreaLarge, 1e-12);
			for (const char* bad : { "gamma_boost: 4\n", "gamma_boost: lots\n",
				"highlight_area_small: 0.2\nhighlight_area_large: 0.1\n" })
			{
				std::ofstream(file, std::ios::trunc) << "[led_drive.Default]\n" << bad;
				ConfigFile rejected;
				Assert::IsTrue(rejected.Load(file));
				Assert::IsFalse(Validate(rejected, error));
			}
			std::ofstream(file, std::ios::trunc) << "[led_drive.Default]\ngamma_boost: off\n";
			ConfigFile offConfig;
			Assert::IsTrue(offConfig.Load(file));
			Assert::IsTrue(Validate(offConfig, error));
			Assert::AreEqual(1.0, BoostExponent(ReadLedDriveConfig(offConfig)), 1e-12);
			DeleteFileA(file);
		}
	};
}
