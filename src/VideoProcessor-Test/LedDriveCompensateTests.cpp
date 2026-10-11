#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <led/LedController.h>
#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
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

	// The regression recipe (planner's 7c-1 answer 1): cntr_assist off,
	// rate_gain_up/down 0 (the fixed rate_up / rate_down) and, in Port() and
	// the module tests, led_settle_ms 0. The compensate tests written before
	// 7c-1 run under it as the proof that it reproduces the previous build.
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
		return config;
	}

	// The same scene setup with 7c-1's defaults (rate law, CNTR assist).
	LedDriveConfig Assisted()
	{
		LedDriveConfig config = Compensate();
		const LedDriveConfig defaults;
		config.rateGainUp = defaults.rateGainUp;
		config.rateGainDown = defaults.rateGainDown;
		config.cntrAssist = true;
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
			port.ledSettleMs = 0;
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

		// 7c-1 rate law (control policy 3.2): rate = clamp(k x distance,
		// rate_min, rate_max) per second; k = 0 is the fixed rate.
		TEST_METHOD(RateLawIsProportionalBetweenMinAndMax)
		{
			Assert::AreEqual(40.0, RampRate(10, 4.0, 20.0, 400.0), 1e-12);
			Assert::AreEqual(200.0, RampRate(50, 4.0, 20.0, 400.0), 1e-12);
			Assert::AreEqual(400.0, RampRate(150, 4.0, 20.0, 400.0), 1e-12, L"capped at rate_max");
			Assert::AreEqual(20.0, RampRate(2, 4.0, 20.0, 400.0), 1e-12, L"never below rate_min: no creep");
			Assert::AreEqual(12.0, RampRate(-10, 1.2, 10.0, 120.0), 1e-12);
			Assert::AreEqual(60.0, RampRate(50, 1.2, 10.0, 120.0), 1e-12);
			Assert::AreEqual(120.0, RampRate(150, 1.2, 10.0, 120.0), 1e-12);
			Assert::AreEqual(120.0, RampRate(3, 0.0, 10.0, 120.0), 1e-12, L"rate_gain 0 = the fixed rate");

			// The defaults match the script's tables (planner's 7c-1 answer 2),
			// per second at one step per 400 ms tick: up between the bright and
			// dark tables, down within 1.25 levels/s of its table.
			const LedDriveConfig defaults;
			const struct { double distance, upBright, upDark, down; } script[] = {
				{ 25, 5.0, 10.0, 5.0 }, { 50, 10.0, 12.5, 7.5 }, { 100, 17.5, 17.5, 12.5 }, { 150, 17.5, 17.5, 12.5 } };
			for (const auto& row : script)
			{
				const double up = RampRate(row.distance, defaults.rateGainUp, defaults.rateMinUp, defaults.rateMaxUp);
				const double down = RampRate(row.distance, defaults.rateGainDown, defaults.rateMinDown, defaults.rateMaxDown);
				Assert::IsTrue(up >= row.upBright - 1e-9 && up <= row.upDark + 1e-9,
					(L"up at distance " + std::to_wstring(row.distance)).c_str());
				Assert::IsTrue(std::abs(down - row.down) <= 1.25 + 1e-9,
					(L"down at distance " + std::to_wstring(row.distance)).c_str());
			}
			Assert::AreEqual(2.5, RampRate(5, defaults.rateGainUp, defaults.rateMinUp, defaults.rateMaxUp), 1e-12,
				L"one level per tick near the target, as the script");

			// A dim from 255 to the floor (60) at 60 Hz: every frame's step is the
			// law's, the ramp lands exactly on the target and stays there.
			const LedDriveConfig config = Assisted();
			StatsBuilder builder(config);
			CompensateController controller(config);
			double now = 0.0;
			const double frame = 1000.0 / 60.0;
			double previous = controller.Level();
			bool slowedDown = false;
			double lastStep = 1e9;
			for (int tick = 0; tick < 4000 && controller.Level() > 60.0; ++tick)
			{
				controller.Tick(Play(builder, now, frame, Frame(18.0)), frame);
				const double distance = previous - 60.0;
				const double expected = std::min(distance, RampRate(distance, config.rateGainDown,
					config.rateMinDown, config.rateMaxDown) * frame / 1000.0);
				const double step = previous - controller.Level();
				Assert::AreEqual(expected, step, 1e-9, (L"step at level " + std::to_wstring(previous)).c_str());
				slowedDown = slowedDown || step < lastStep - 1e-9;
				lastStep = step;
				previous = controller.Level();
			}
			Assert::AreEqual(60.0, controller.Level(), 1e-12, L"lands exactly on the target");
			Assert::IsTrue(slowedDown, L"the ramp slows as it arrives");
			for (int tick = 0; tick < 60; ++tick)
				controller.Tick(Play(builder, now, frame, Frame(18.0)), frame);
			Assert::AreEqual(60.0, controller.Level(), 1e-12, L"no residual creep");
		}

		TEST_METHOD(SettleModelIsDeadTimeThenFirstOrderLag)
		{
			SettleModel model(300.0, 1.0);
			model.Land(0.4);
			Assert::AreEqual(1.0, model.Value(), 1e-12, L"nothing moves on landing");
			model.Step(300.0);
			Assert::AreEqual(0.4 + 0.6 * std::exp(-1.0), model.Value(), 1e-12, L"63% after led_settle_ms");
			const double now = model.Value();
			Assert::AreEqual(0.4 + (now - 0.4) * std::exp(-0.5), model.Predict(0.4, 150.0), 1e-12);
			SettleModel instant(0.0, 1.0);
			instant.Land(0.4);
			Assert::AreEqual(0.4, instant.Value(), 1e-12, L"led_settle_ms 0 = dead time only");
			Assert::AreEqual(300, ProjectorConfig().ledSettleMs, L"fast-biased default (planner Q2)");
		}

		// 7c-1 CNTR assist: software first, whole values, commands only on
		// integer crossings, no chatter, bounded by cntr_max_usable.
		TEST_METHOD(CntrPairingIsSoftwareFirstAndChangesOnlyOnCrossings)
		{
			CntrPairing pairing;
			pairing.Configure(2.41, 74, true, true);
			Assert::AreEqual(-1, pairing.Update(2.0, 1e9), L"no highlights: VP's gain does it all");
			Assert::AreEqual(-1, pairing.Update(2.0, 2.0), L"within VP's headroom: CNTR stays at 50");
			const int first = pairing.Update(4.0, 2.0); // CNTR must supply 2x
			Assert::AreEqual(67, first, L"the smallest whole CNTR with (CNTR/50)^E >= 2");
			Assert::IsTrue(pairing.Light(67) >= 2.0 && pairing.Light(66) < 2.0);
			Assert::AreEqual(-1, pairing.Update(4.0, 2.0), L"no command while the value holds");

			// Hovering at a step boundary: one command, then quiet.
			pairing.Reset(50);
			const double edge = pairing.Light(60);
			Assert::AreEqual(61, pairing.Update(edge * 1.001, 1.0));
			int commands = 0;
			for (int i = 0; i < 100; ++i)
				commands += pairing.Update(edge * (i % 2 ? 1.001 : 0.999), 1.0) >= 0 ? 1 : 0;
			Assert::AreEqual(0, commands, L"no chatter at a boundary");
			Assert::AreEqual(60, pairing.Update(edge / 1.03, 1.0), L"down once a step lower covers it with margin");
			Assert::AreEqual(50, pairing.Update(1.0, 2.0), L"no need left (VP has room): back to 50");

			Assert::AreEqual(74, pairing.Update(100.0, 1.0), L"never above cntr_max_usable");
			pairing.Configure(2.41, 74, false, true);
			Assert::AreEqual(50, pairing.Update(100.0, 1.0), L"cntr_assist off: back to 50");
			Assert::AreEqual(-1, pairing.Update(100.0, 1.0));

			CntrPairing first_;
			first_.Configure(2.41, 74, true, false);
			Assert::AreEqual(67, first_.Update(2.0, 1e9), L"software_first off: CNTR goes first");
		}

		// Control policy 9, pairing: over a floor ramp, LED light x CNTR light
		// x VP gain stays exact, VP's gain never exceeds its headroom, CNTR
		// only rises (one command per whole step crossed).
		TEST_METHOD(CntrPairingFollowsAFloorRamp)
		{
			CntrPairing pairing;
			pairing.Configure(2.41, 74, true, true);
			const double headroom = 1.5;
			int previous = 50, commands = 0;
			for (int i = 0; i <= 400; ++i)
			{
				const double led = 1.0 - 0.7 * i / 400.0; // LED light 1.0 -> 0.3
				const int command = pairing.Update(1.0 / led, headroom);
				if (command >= 0)
				{
					++commands;
					Assert::IsTrue(command > previous, L"CNTR only rises while the LED dims");
					previous = command;
				}
				const double vp = (1.0 / led) / pairing.Light(pairing.Contrast());
				Assert::IsTrue(vp <= headroom + 1e-9, (L"VP gain over its headroom at step " + std::to_wstring(i)).c_str());
				Assert::AreEqual(1.0, led * pairing.Light(pairing.Contrast()) * vp, 1e-12);
			}
			Assert::IsTrue(previous > 50 && commands <= previous - 50, L"at most one command per whole value");
		}

		// With CNTR assist the highlight floor may go lower by CNTR's
		// headroom, and the controller reports VP's own headroom.
		TEST_METHOD(ContrastHeadroomLowersTheHighlightFloor)
		{
			const LedDriveConfig config = Assisted();
			auto settle = [&config](double contrastHeadroom, ControllerOutput& out)
			{
				StatsBuilder builder(config);
				CompensateController controller(config);
				controller.SetContrastHeadroom(contrastHeadroom);
				double now = 0.0;
				for (int tick = 0; tick < 120; ++tick)
					out = controller.Tick(Play(builder, now, 100.0, Frame(18.0, 100, 230)), 100);
				return out.lightTarget;
			};
			ControllerOutput plain, assisted;
			const int without = settle(1.0, plain);
			const int with = settle(std::pow(74.0 / 50.0, config.compensationEotf), assisted);
			Assert::IsTrue(without > config.ledFloor, L"the highlight holds the LED up without CNTR");
			Assert::IsTrue(with < without, L"CNTR's headroom lets the LED dim further");
			const double h = std::pow((230.0 - 16.0) / 219.0, config.compensationEotf);
			Assert::IsTrue(plain.vpHeadroom < 1e9 && plain.vpHeadroom >= 1.0 / h,
				L"VP's headroom: the highlight may rise by the allowed stops");
			Assert::AreEqual(1e9, CompensateController(config).Tick(FrameStats(), 16.0).vpHeadroom, 1e-3,
				L"no sustained highlight: unlimited");
		}

		// The module: CNTR assist pairs a real CNTR with the modelled LED,
		// sends it only on changes, VP's gain divides by the modelled CNTR
		// light, and the exit puts CNTR back to 50.
		TEST_METHOD(ModuleCntrAssistPairsWithTheModelledLed)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Assisted();
			ApplyRatePreset(config, true); // the faster preset keeps the test short
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			port.ackTimeoutMs = 60;
			port.minCommandGapMs = 0;
			port.readBack = true;
			{
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				// A dark scene with a sustained 1% highlight: the LED dims as far
				// as CNTR's headroom allows; VP's gain cannot cover it alone.
				CompensationGain gain;
				double time = 0.0;
				for (int frame = 1; frame <= 600; ++frame)
				{
					time = frame * 1000.0 / 60.0;
					module.SubmitMeasurement(Frame(18.0, 100, 230));
					gain = module.BeginFrameAt(0, time);
					WaitFor([&] { return !module.ProjectorStatus().busy; });
				}
				Assert::IsTrue(module.AssistContrast() > 50, L"CNTR assists");
				Assert::IsTrue(module.AssistContrast() <= config.cntrAssistMax,
					L"never above cntr_assist_max (62 until 7c-2)");
				Assert::IsTrue(WaitFor([&] { return projector->contrast == module.AssistContrast(); }),
					L"the projector got the paired CNTR");
				Assert::AreEqual(std::pow(module.AssistContrast() / 50.0, config.compensationEotf),
					module.ModelledContrastLight(), 1e-9, L"the CNTR model landed");
				// VP's gain = LED compensation / CNTR light, less at most the
				// highlight cap's small share at a 1% highlight area.
				const double paired = 255.0 / module.ModelledLight() / module.ModelledContrastLight();
				Assert::IsTrue(gain.gain <= paired + 1e-9 && gain.gain >= 0.97 * paired,
					(L"VP's gain " + std::to_wstring(gain.gain) + L" vs paired " + std::to_wstring(paired)).c_str());
				int changes = 0, sends = 0, last = 50;
				for (const auto& write : projector->Writes())
				{
					if (write.rfind(":CNTR", 0) != 0 || write == ":CNTR?\r" || write == ":CNTR50\r") continue;
					const int value = std::stoi(write.substr(5));
					++sends;
					changes += value != last ? 1 : 0;
					last = value;
				}
				Assert::IsTrue(sends > 0 && sends == changes, L"CNTR only on integer changes, never repeated");
				Assert::IsTrue(module.ProjectorStatus().contrastSends >= sends);
			}
			Assert::AreEqual(255, projector->light);
			Assert::AreEqual(50, projector->contrast, L"the exit puts CNTR back to 50");
		}

		TEST_METHOD(CntrAssistOffNeverSendsContrast)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Assisted();
			config.cntrAssist = false;
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			port.ackTimeoutMs = 60;
			port.minCommandGapMs = 0;
			port.readBack = true;
			{
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				for (int frame = 1; frame <= 300; ++frame)
				{
					module.SubmitMeasurement(Frame(18.0, 100, 230));
					module.BeginFrameAt(0, frame * 1000.0 / 60.0);
					WaitFor([&] { return !module.ProjectorStatus().busy; });
				}
				Assert::IsTrue(projector->light < 255);
				Assert::AreEqual(50, module.AssistContrast());
			}
			for (const auto& write : projector->Writes())
				Assert::IsFalse(write.rfind(":CNTR", 0) == 0 && write != ":CNTR?\r",
					L"cntr_assist off never touches the projector's contrast");
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
