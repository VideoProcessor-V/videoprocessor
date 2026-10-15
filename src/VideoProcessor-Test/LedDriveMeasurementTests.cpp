#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	// A uniform picture at `code`, with `brightBlocks` of the blocks at `bright`.
	RawMeasurement Uniform(double code, uint32_t blocks = 1000, uint32_t brightBlocks = 0,
		int bright = 235)
	{
		RawMeasurement raw{};
		const uint32_t plain = blocks - brightBlocks;
		raw.histogram[static_cast<int>(code)] += plain;
		raw.histogram[bright] += brightBlocks;
		raw.blocks = blocks;
		raw.sum = static_cast<uint32_t>(code * SUM_SCALE * plain + bright * SUM_SCALE * brightBlocks);
		raw.maximum = static_cast<uint32_t>((brightBlocks > 0 ? bright : code) * SUM_SCALE);
		for (int i = 0; i < GRID_W * GRID_H; ++i)
		{
			raw.cellCount[i] = 2;
			raw.cellSum[i] = static_cast<uint32_t>(2 * code * SUM_SCALE);
		}
		return raw;
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

	LedDriveConfig FastScript()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Script;
		config.tickMs = 50;
		// script_pacing (the default) with near-zero screenshot and echo
		// times: the paced loop runs, quiet loops in 10 ms.
		config.scriptMeasureMs = 10;
		config.scriptEchoMs = 0;
		return config;
	}

	ProjectorConfig FastProjector()
	{
		ProjectorConfig config;
		config.protocol = "barco_ascii";
		config.ackTimeoutMs = 60;
		config.minCommandGapMs = 0;
		config.readBack = true;
		return config;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveMeasurementTests)
	{
	public:
		TEST_METHOD(MeasurementFollowsTheTrustedActivePicture)
		{
			// Scope picture inside a 1920x1080 16:9 raster, all of it displayed.
			const auto scope = ActivePictureRect(true, 0, 138, 1920, 942, 0, 0, 1920, 1080);
			Assert::AreEqual(0.0, scope.x0, 1e-9);
			Assert::AreEqual(1.0, scope.x1, 1e-9);
			Assert::AreEqual(138.0 / 1080.0, scope.y0, 1e-9);
			Assert::AreEqual(942.0 / 1080.0, scope.y1, 1e-9);
			// Relative to a displayed crop.
			const auto cropped = ActivePictureRect(true, 240, 0, 1680, 1080, 240, 0, 1680, 1080);
			Assert::AreEqual(0.0, cropped.x0, 1e-9);
			Assert::AreEqual(1.0, cropped.x1, 1e-9);
			// No trusted picture, or a sliver: measure everything.
			for (const auto rect : { ActivePictureRect(false, 0, 138, 1920, 942, 0, 0, 1920, 1080),
				ActivePictureRect(true, 0, 500, 1920, 520, 0, 0, 1920, 1080),
				ActivePictureRect(true, 0, 0, 0, 0, 0, 0, 1920, 1080) })
			{
				Assert::AreEqual(0.0, rect.x0, 1e-9);
				Assert::AreEqual(1.0, rect.y1, 1e-9);
			}
		}

		TEST_METHOD(StatisticsAreInTheScriptsCodeUnits)
		{
			StatsBuilder builder{ LedDriveConfig() };
			const FrameStats grey = builder.Build(Uniform(60.0));
			Assert::AreEqual(60.0, grey.apl, 1e-9);
			Assert::AreEqual(60.0, grey.highlight, 1e-9);
			Assert::AreEqual(0.0, grey.highlightArea, 1e-9);
			Assert::AreEqual(1000u, grey.blocks);

			// 1% of blocks at white: above the 99.5th percentile, so the highlight
			// statistic sees them; 0.2% would not.
			const FrameStats lamp = builder.Build(Uniform(30.0, 1000, 10));
			Assert::AreEqual(235.0, lamp.highlight, 1e-9);
			Assert::AreEqual(0.01, lamp.highlightArea, 1e-9);
			Assert::AreEqual((30.0 * 990 + 235.0 * 10) / 1000.0, lamp.apl, 1e-6);
			const FrameStats speck = builder.Build(Uniform(30.0, 1000, 2));
			Assert::AreEqual(30.0, speck.highlight, 1e-9);
			Assert::AreEqual(235.0, speck.maximum, 1e-9,
				L"A 0.2% lamp is missed by the percentile but is the block maximum (script mode)");

			uint32_t histogram[256] = {};
			histogram[16] = 500;
			histogram[100] = 495;
			histogram[200] = 5;
			Assert::AreEqual(100.0, HistogramPercentile(histogram, 1000, 99.5), 1e-9);
			Assert::AreEqual(200.0, HistogramPercentile(histogram, 1000, 99.6), 1e-9);
			Assert::AreEqual(16.0, HistogramPercentile(histogram, 0, 99.5), 1e-9);
		}

		TEST_METHOD(CutsAreLargeBlockChangesAndFadesAreSteadyDeclines)
		{
			LedDriveConfig config;
			StatsBuilder builder(config);
			Assert::IsFalse(builder.Build(Uniform(100.0)).cut, L"No previous frame");
			Assert::IsFalse(builder.Build(Uniform(110.0)).cut, L"10 codes is a scene change, not a cut");
			Assert::IsTrue(builder.Build(Uniform(30.0)).cut, L"80 codes is a cut");
			Assert::AreEqual(80.0, builder.LastDifference(), 1e-9);

			// The fade is in time (general): the same at 24p and 60p.
			for (double fps : { 24.0, 60.0 })
			{
				const double step = 1000.0 / fps;
				const int frames = static_cast<int>(std::ceil(config.fadeMs / step - 1e-9));
				StatsBuilder fading(config);
				double apl = 120.0;
				FrameStats stats;
				for (int frame = 0; frame < frames; ++frame)
				{
					stats = fading.Build(Uniform(apl), frame * step);
					Assert::IsFalse(stats.fade, L"Needs fade_ms of declines");
					apl -= 2.0 * 24.0 / fps;   // 48 APL per second
				}
				stats = fading.Build(Uniform(apl), frames * step);
				Assert::IsTrue(stats.fade, L"A fade after fade_ms");
				stats = fading.Build(Uniform(apl), (frames + 1) * step);
				Assert::IsFalse(stats.fade, L"A held frame ends the fade");

				StatsBuilder slow(config);
				apl = 120.0;
				for (int frame = 0; frame <= frames + 2; ++frame)
				{
					stats = slow.Build(Uniform(apl), frame * step);
					apl -= 6.0 / fps;   // 6 APL per second: under fade_min_slope
				}
				Assert::IsFalse(stats.fade, L"Below fade_min_slope is drift, not a fade");
			}
		}

		// Step 7b: script mode is the script - real LPW1 and CNTR, VP's picture
		// untouched (script_smoothing off), CNTR 50 at start and exit.
		TEST_METHOD(ScriptModeSendsTheScriptsLightAndContrast)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			{
				LedDriveModule module(FastProjector(), FastScript(),
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
				Assert::IsTrue(WaitFor([&]
					{
						const auto writes = projector->Writes();
						return projector->light == 255 &&
							std::find(writes.begin(), writes.end(), std::string(":CNTR50\r")) != writes.end();
					}), L"LPW1 255 and CNTR 50 at start (even though the projector was at 50), as the script did");
				for (int frame = 0; frame < 5; ++frame)
				{
					module.SubmitMeasurement(Uniform(10.0));
					module.BeginFrame(static_cast<uint64_t>(frame + 1));
				}
				Assert::IsTrue(WaitFor([&] { return projector->light < 240; }), L"Dark scene dims the LED");
				Assert::IsTrue(WaitFor([&] { return projector->contrast > 51; }, 5000),
					L"Dark scene raises the projector's real CNTR");
				module.BeginFrame(6);
				Assert::IsFalse(module.CurrentGain().active, L"script_smoothing off: VP's picture untouched");
				Assert::AreEqual(1.0, module.CurrentGain().gain, 1e-12);
			}
			Assert::AreEqual(255, projector->light, L"Stopping restores full light");
			Assert::AreEqual(50, projector->contrast, L"and the contrast to 50");
		}

		// Audit stage7-step7ab, Should fix 6: a live switch from script to
		// compensate puts CNTR back to 50 before compensate does anything; the
		// rest of the script's tick is not sent.
		TEST_METHOD(SwitchFromScriptToCompensateResetsContrastFirst)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			const ProjectorConfig port = FastProjector();
			LedDriveModule module(port, FastScript(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			for (int frame = 0; frame < 5; ++frame)
				module.SubmitMeasurement(Uniform(10.0));
			Assert::IsTrue(WaitFor([&] { return projector->contrast >= 53; }, 5000), L"the script raised CNTR");

			LedDriveConfig compensate = FastScript();
			compensate.mode = Mode::Compensate;
			const size_t before = projector->Writes().size();
			Assert::IsTrue(module.Reconfigure(port, compensate));
			auto resetSeen = [&]
			{
				const auto writes = projector->Writes();
				return std::find(writes.begin() + static_cast<std::ptrdiff_t>(std::min(before, writes.size())),
					writes.end(), std::string(":CNTR50\r")) != writes.end();
			};
			bool compensated = false;
			for (uint64_t frame = 1; frame <= 200 && !compensated; ++frame)
			{
				module.SubmitMeasurement(Uniform(10.0), frame);
				const bool reset = resetSeen();
				const CompensationGain gain = module.BeginFrame(frame);
				if (!reset)
					Assert::IsFalse(gain.active, L"compensate touched the picture before CNTR was back at 50");
				compensated = reset && gain.active;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			Assert::IsTrue(compensated, L"compensate starts once CNTR is back at 50");
			const auto writes = projector->Writes();
			const auto reset = std::find(writes.begin() + static_cast<std::ptrdiff_t>(before), writes.end(),
				std::string(":CNTR50\r"));
			Assert::IsTrue(reset != writes.end());
			Assert::IsTrue(reset - (writes.begin() + static_cast<std::ptrdiff_t>(before)) <= 1,
				L"at most the command already in flight goes out before the reset");
			Assert::AreEqual(50, projector->contrast);
			module.Stop();
		}

		TEST_METHOD(ScriptSmoothingBridgesTheRealContrastSteps)
		{
			auto projector = std::make_shared<FakeProjector>();
			LedDriveConfig config = FastScript();
			config.scriptSmoothing = true;
			{
				LedDriveModule module(FastProjector(), config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(Uniform(10.0));
				bool bridged = false;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				for (uint64_t frame = 1; std::chrono::steady_clock::now() < deadline && projector->contrast < 54; ++frame)
				{
					module.SubmitMeasurement(Uniform(10.0));
					const CompensationGain gain = module.BeginFrame(frame);
					if (!gain.active) continue;
					Assert::IsTrue(gain.domain == GainDomain::Encoded);
					Assert::IsTrue(gain.gain <= 1.0 + 1e-12, L"Never brighter than the projector's own CNTR");
					bridged = bridged || gain.gain < 1.0 - 1e-6;
					std::this_thread::sleep_for(std::chrono::milliseconds(4));
				}
				Assert::IsTrue(projector->contrast >= 54, L"The script raised the real CNTR");
				Assert::IsTrue(bridged, L"VP's gain bridged the steps");
			}
			Assert::AreEqual(50, projector->contrast);
		}

		TEST_METHOD(LogOnlyNeverOpensAPortOrTouchesThePicture)
		{
			int ports = 0;
			LedDriveConfig config = FastScript();
			config.mode = Mode::LogOnly;
			LedDriveModule module(FastProjector(), config,
				[&ports] { ++ports; return std::unique_ptr<ISerialPort>(); });
			module.Start();
			for (int frame = 0; frame < 5; ++frame)
				module.SubmitMeasurement(Uniform(10.0));
			Assert::IsTrue(WaitFor([&] { return module.Ticks() >= 5; }));
			Assert::IsFalse(module.DrivingProjector());
			Assert::IsFalse(module.CurrentGain().active);
			Assert::AreEqual(1.0, module.CurrentGain().gain, 1e-12);
			Assert::IsTrue(module.ProjectorStatus().lastLight < 255, L"The dry run still logs the levels");
			module.Stop();
			Assert::AreEqual(0, ports);
		}

		TEST_METHOD(UnavailablePortDegradesToADryRun)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;
			LedDriveModule module(FastProjector(), FastScript(),
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			for (int frame = 0; frame < 5; ++frame)
				module.SubmitMeasurement(Uniform(10.0));
			Assert::IsTrue(WaitFor([&] { return module.Ticks() >= 5; }));
			Assert::IsFalse(module.DrivingProjector());
			Assert::IsFalse(module.CurrentGain().active, L"Without the LED dimming, no compensating gain");
			module.Stop();
			Assert::IsTrue(projector->Writes().empty());
		}
	};
}
