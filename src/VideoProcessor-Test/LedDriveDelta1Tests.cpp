#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <VP-V/modules/drive/projector_control/LedController.h>
#include <led/LedDriveModule.h>
#include <led/LedLookahead.h>
#include <led/LedMeasurement.h>
#include <led/LedRawTransfer.h>
#include <VP-V/modules/drive/projector_control/LedShutdownRestore.h>
#include <vprenderer/AlphaQueuePolicy.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
#include <atomic>
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

	// The regression recipe (planner's 7c-1 answer 1, extended by 7c-2):
	// cntr_assist off, rate_gain_up/down 0 (the fixed rate_up / rate_down),
	// max_cut_dip 0, shaped_recovery off, black_prelift_max 255 (7c-3) and,
	// in Port() and the module tests, led_settle_ms 0. The compensate tests written before 7c-1 run under it
	// as the proof that it reproduces the previous build.
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

	// Real-time waits only for what needs no time on the drive's clock (a port
	// write and its echo); the bound is generous so machine load cannot fail it.
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

	ProjectorConfig Port(int latencyMs)
	{
		ProjectorConfig port;
		port.protocol = "barco_ascii";
		port.ackTimeoutMs = 60;
		port.minCommandGapMs = 0;
		port.readBack = true;
		port.ledLatencyMs = latencyMs;
		port.ledSettleMs = 0; // the dead-time model these tests were written for
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

	// One output frame at timeMs, the clock moved there first; depth > 0 is the
	// renderer's queued-frame walk, which comes with every frame.
	CompensationGain FrameAt(LedDriveModule& module, TestClock& clock, uint64_t sequence, double timeMs,
		size_t depth = 0, double sourceFps = 0.0)
	{
		clock.To(timeMs);
		if (depth > 0) module.SetLookaheadDepth(depth);
		return module.BeginFrameAt(sequence, timeMs, sourceFps);
	}

	// Renders sequences 1.. at 60 Hz until `until`, the clock at each frame's
	// time; returns the modelled LED level seen on each sequence.
	std::vector<double> Render(LedDriveModule& module, TestClock& clock, uint64_t until,
		std::vector<double>* gains = nullptr)
	{
		std::vector<double> levels(1, 255.0);
		for (uint64_t sequence = 1; sequence <= until; ++sequence)
		{
			clock.To(sequence * FRAME_MS);
			const CompensationGain gain = module.BeginFrameAt(sequence, sequence * FRAME_MS);
			levels.push_back(module.ModelledLight());
			if (gains) gains->push_back(gain.gain);
			// The driver sends between frames (at this frame's time).
			clock.Settle();
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

		// Planner's 7c-1 answers 2 and 3: rate_preset (script default,
		// compensated = the faster set; rate_* keys override) and the
		// cntr_assist_max cap (62 until 7c-2).
		TEST_METHOD(RatePresetAndAssistCapAreRead)
		{
			std::string error;
			const std::string good = "[led_drive.Default]\nrate_preset: compensated\nrate_max_up: 300\n"
				"cntr_assist_max: 70\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::AreEqual(4.0, led.rateGainUp, 1e-12);
			Assert::AreEqual(1.2, led.rateGainDown, 1e-12);
			Assert::AreEqual(120.0, led.rateMaxDown, 1e-12);
			Assert::AreEqual(300.0, led.rateMaxUp, 1e-12, L"an explicit key overrides the preset");
			Assert::AreEqual(4.0, led.recoveryRateGain, 1e-12, L"the preset keeps the fade-up fast");
			Assert::AreEqual(70, led.cntrAssistMax);
			const LedDriveConfig defaults;
			Assert::AreEqual(0.25, defaults.rateGainUp, 1e-12, L"default = script-matched");
			Assert::AreEqual(17.5, defaults.rateMaxUp, 1e-12);
			Assert::AreEqual(100, defaults.cntrAssistMax, L"7c-2 lifts the cap: cntr_max_usable governs");
			Assert::IsFalse(Validates("[led_drive.Default]\nrate_preset: fast\n", error));
			Assert::IsFalse(Validates("[led_drive.Default]\ncntr_assist_max: 40\n", error));
		}

		// The regression recipe uses the fixed rates even with the new
		// rate_max defaults.
		// Owner correction (2026-10-11): cut_cntr_level (default =
		// dmd_clip_cntr, 59), dmd_clip_cntr 59 until the fixed-LED white
		// sweep, fast recovery_rate_* for the fade-up's shortfall.
		TEST_METHOD(CutLevelClipAndRecoveryRatesAreRead)
		{
			Assert::AreEqual(59, ProjectorConfig().dmdClipCntr);
			const LedDriveConfig defaults;
			Assert::AreEqual(-1, defaults.cutCntrLevel, L"follows dmd_clip_cntr");
			Assert::AreEqual(4.0, defaults.recoveryRateGain, 1e-12);
			Assert::AreEqual(400.0, defaults.recoveryRateMax, 1e-12);
			std::string error;
			const std::string good = "[projector.Default]\ndmd_clip_cntr: 63\n"
				"[led_drive.Default]\ncut_cntr_level: 57\nrecovery_rate_gain: 2\nrecovery_rate_min: 10\n"
				"recovery_rate_max: 200\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const ProjectorConfig projector = ReadProjectorConfig(config);
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::AreEqual(63, projector.dmdClipCntr);
			Assert::AreEqual(57, led.cutCntrLevel);
			Assert::AreEqual(2.0, led.recoveryRateGain, 1e-12);
			Assert::AreEqual(10.0, led.recoveryRateMin, 1e-12);
			Assert::AreEqual(200.0, led.recoveryRateMax, 1e-12);
			Assert::IsFalse(Validates("[led_drive.Default]\ncut_cntr_level: 40\n", error));
		}

		TEST_METHOD(RegressionRecipeUsesTheFixedRates)
		{
			LedDriveConfig config = Compensate();
			Assert::AreEqual(0.0, config.rateGainDown, 1e-12);
			StatsBuilder builder(config);
			CompensateController controller(config);
			FrameStats stats;
			for (int frame = 0; frame < 3; ++frame) stats = builder.Build(Frame(18.0), frame * FRAME_MS);
			controller.Tick(stats, FRAME_MS);
			Assert::AreEqual(255.0 - config.rateDown * FRAME_MS / 1000.0, controller.Level(), 1e-9,
				L"rate_gain 0: the fixed rate_down, not rate_max_down");
		}

		TEST_METHOD(ScriptSmoothingAndContrastLatencyAreRead)
		{
			std::string error;
			const std::string good = "[projector.Default]\ncntr_latency_ms: 120\n"
				"[led_drive.Default]\nmode: script\nscript_smoothing: on\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const ProjectorConfig projector = ReadProjectorConfig(config);
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::AreEqual(120, projector.cntrLatencyMs);
			Assert::IsTrue(led.scriptSmoothing);
			Assert::IsFalse(LedDriveConfig().scriptSmoothing, L"Off by default: script mode is the script");
			Assert::AreEqual(80, ProjectorConfig().cntrLatencyMs);
			Assert::IsFalse(Validates("[projector.Default]\ncntr_latency_ms: -1\n", error));
			Assert::IsFalse(Validates("[led_drive.Default]\nscript_smoothing: maybe\n", error));
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

		TEST_METHOD(LedProfileFileFillsInWhatTheMainSettingsLeaveOut)
		{
			char directory[MAX_PATH]{};
			GetTempPathA(MAX_PATH, directory);
			const std::string profilePath = std::string(directory) + "vp-led-profile-test.cfg";
			std::ofstream(profilePath, std::ios::trunc) << "[led_drive.Default]\nrate_down: 60\nled_floor: 90\n"
				"curve: 0:100, 40:180, 100:255, 255:255\n";
			std::string error;
			const std::string main = Write("[led_drive.Default]\nenabled: true\nmode: compensate\n"
				"led_floor: 80\nprofile_file: vp-led-profile-test.cfg\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(main.c_str()));
			Assert::IsTrue(Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
			const LedDriveConfig read = ReadLedDriveConfig(config);
			Assert::AreEqual(60.0, read.rateDown, 1e-12, L"From the profile");
			Assert::AreEqual(80, read.ledFloor, L"The main settings (Config app page) win");
			Assert::AreEqual<size_t>(4, read.curve.size());
			Assert::AreEqual(400.0, read.rateUp, 1e-12, L"Left out of both: built-in");
			DeleteFileA(main.c_str());

			// Audit round 2: a broken or missing profile is reported but never
			// invalidates the configuration (VP must still start); the LED drive
			// leaves it out and runs on the built-in values plus the main settings.
			for (const char* bad : { "[led_drive.Default]\nenabled: true\n",
				"[led_drive.Default]\nrate_down: fast\nrate_up: 300\n",
				"[projector.Default]\nport: COM2\n", "[led_drive.Default]\nled_ceiling: 70\n" })
			{
				std::ofstream(profilePath, std::ios::trunc) << bad;
				const std::string withBad = Write("[led_drive.Default]\nled_floor: 80\nprofile_file: vp-led-profile-test.cfg\n");
				ConfigFile badConfig;
				Assert::IsTrue(badConfig.Load(withBad.c_str()));
				const std::wstring label(bad, bad + strlen(bad));
				Assert::IsTrue(Validate(badConfig, error), label.c_str());
				Assert::IsFalse(ValidateLedProfile(badConfig, error), label.c_str());
				Assert::IsFalse(error.empty());
				const LedDriveConfig fallback = ReadLedDriveConfig(badConfig);
				Assert::AreEqual(400.0, fallback.rateUp, 1e-12, L"Nothing from a broken profile is used");
				Assert::AreEqual(80, fallback.ledFloor, L"The main settings still apply");
				DeleteFileA(withBad.c_str());
			}
			DeleteFileA(profilePath.c_str());
			Assert::IsTrue(Validates("[led_drive.Default]\nprofile_file: vp-led-profile-test.cfg\n", error),
				L"A missing profile does not stop VP");
			const std::string missing = Write("[led_drive.Default]\nprofile_file: vp-led-profile-test.cfg\n");
			ConfigFile missingConfig;
			Assert::IsTrue(missingConfig.Load(missing.c_str()));
			Assert::IsFalse(ValidateLedProfile(missingConfig, error));
			Assert::IsTrue(error.find("cannot read") != std::string::npos);
			DeleteFileA(missing.c_str());
		}

		TEST_METHOD(ExperimentalFadeToBlackUsesTheCustomDimAndComesBack)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50); // 3 frames at 60 Hz
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			config.ftbCustomDim = FtbCustomDim::Turns;
			config.ftbStepMs = 100;
			config.ftbCustomStart = 40;
			config.ftbCustomFloor = 30;
			config.ftbTurnStep = 5;
			config.ftbBlackMs = 200;
			TestClock clock;
			{
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				const auto startup = projector->Writes();
				Assert::IsTrue(std::find(startup.begin(), startup.end(), std::string(":LDMM0\r")) != startup.end(),
					L"Startup puts the dim mode back to standard when the experiment is configured");

				// A black stretch, seen in the queue (3 frames ahead).
				module.SetLookaheadDepth(3);
				StatsBuilder builder(module.Config());
				auto predict = [&](uint64_t from, uint64_t to, double code)
				{
					for (uint64_t sequence = from; sequence < to; ++sequence)
						module.SubmitPrediction(sequence, builder.Build(Frame(code), sequence * FRAME_MS));
				};
				predict(1, 400, 16.0);
				module.SubmitMeasurement(Frame(16.0));
				uint64_t sequence = 0;
				auto render = [&](int frames)
				{
					for (int i = 0; i < frames; ++i)
					{
						++sequence;
						clock.To(sequence * FRAME_MS);
						module.SetLookaheadDepth(3);   // the renderer's walk, every frame
						module.BeginFrameAt(sequence, sequence * FRAME_MS);
						clock.Settle();
					}
				};
				for (int i = 0; i < 300 && !module.CustomDimEngaged(); ++i) render(1);
				Assert::IsTrue(module.CustomDimEngaged(), L"The custom dim engaged on a black picture");
				Assert::IsTrue(module.ModelledLight() <= config.ledFloorBlack, L"Only after the LED reached its floor");
				// The sequence's ftb_step_ms steps run on the clock: frames keep coming.
				for (int i = 0; i < 300 && module.ProjectorStatus().sequenceBusy; ++i) render(1);
				Assert::IsFalse(module.ProjectorStatus().sequenceBusy);

				std::vector<std::string> dim;
				std::vector<std::chrono::steady_clock::time_point> times;
				const auto writes = projector->Writes();
				const auto writeTimes = projector->WriteTimes();
				for (size_t i = 0; i < writes.size(); ++i)
					if (writes[i].rfind(":LDC", 0) == 0 || writes[i] == ":LDMM1\r")
					{
						dim.push_back(writes[i]);
						times.push_back(writeTimes[i]);
					}
				const std::vector<std::string> expected = { ":LDCR40\r", ":LDCG40\r", ":LDCB40\r", ":LDMM1\r",
					":LDCR35\r", ":LDCG35\r", ":LDCB35\r", ":LDCR30\r", ":LDCG30\r", ":LDCB30\r" };
				Assert::AreEqual(expected.size(), dim.size());
				for (size_t i = 0; i < expected.size(); ++i)
					Assert::AreEqual(expected[i], dim[i], L"Colours take turns, one small step each");
				for (size_t i = 1; i < times.size(); ++i)
					Assert::IsTrue(std::chrono::duration<double, std::milli>(times[i] - times[i - 1]).count() >= 95.0,
						L"One custom-dim command every ftb_step_ms (100 ms)");
				const size_t before = projector->Writes().size();

				// A bright scene comes into the queue: leave as soon as it is seen
				// - one LDMM 0 at once, then the LED straight to the new level.
				predict(sequence + 10, sequence + 400, 120.0);
				for (int i = 0; i < 40 && module.CustomDimEngaged(); ++i)
					render(1);
				Assert::IsFalse(module.CustomDimEngaged(), L"The custom dim ends before the bright scene");
				const auto after = projector->Writes();
				std::vector<std::string> added(after.begin() + before, after.end());
				Assert::IsTrue(added.size() >= 2);
				Assert::AreEqual(std::string(":LDMM0\r"), added[0], L"Leaving is one command, sent first");
				Assert::AreEqual(std::string(":LPW1255\r"), added[1], L"then the LED jumps to the bright scene, whole");
				for (const auto& w : added) Assert::IsTrue(w.rfind(":LDC", 0) != 0, L"No colour commands on the way out");
			}
			// Exit: standard dim mode, then full light.
			const auto final = projector->Writes();
			Assert::AreEqual(std::string(":LPW1255\r"), final.back());
			Assert::AreEqual(std::string(":LDMM0\r"), final[final.size() - 2]);
		}

		TEST_METHOD(SwitchingModeDuringTheCustomDimLeavesItAtOnce)
		{
			// Audit round 2: compensate -> script while the custom dim is on must
			// send LDMM 0 before any LPW1, or the screen stays dark.
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			config.ftbCustomDim = FtbCustomDim::Together;
			config.ftbStepMs = 100;
			config.ftbBlackMs = 200;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			StatsBuilder builder(module.Config());
			for (uint64_t s = 1; s < 400; ++s)
				module.SubmitPrediction(s, builder.Build(Frame(16.0), s * FRAME_MS));
			module.SubmitMeasurement(Frame(16.0));
			uint64_t sequence = 0;
			auto render = [&]
			{
				++sequence;
				clock.To(sequence * FRAME_MS);
				module.SetLookaheadDepth(3);   // the renderer's walk, every frame
				module.BeginFrameAt(sequence, sequence * FRAME_MS);
				clock.Settle();
			};
			for (int i = 0; i < 300 && !module.CustomDimEngaged(); ++i) render();
			Assert::IsTrue(module.CustomDimEngaged());
			for (int i = 0; i < 300 && module.ProjectorStatus().sequenceBusy; ++i) render();
			Assert::IsFalse(module.ProjectorStatus().sequenceBusy);
			const size_t before = projector->Writes().size();

			LedDriveConfig script = config;
			script.mode = Mode::Script;
			Assert::IsTrue(module.Reconfigure(port, script), L"A mode change is applied live");
			Assert::IsFalse(module.CustomDimEngaged(), L"Leaving the compensate mode leaves the custom dim");
			Assert::IsTrue(clock.RunUntil([&] { return projector->Writes().size() > before; }, 3000));
			// Let script mode run a little (on the clock); it may send LPW1, but only after LDMM 0.
			Assert::IsTrue(clock.Run(600));
			const auto after = projector->Writes();
			Assert::AreEqual(std::string(":LDMM0\r"), after[before], L"LDMM 0 is the first command after the switch");
			for (size_t i = before + 1; i < after.size(); ++i)
				Assert::IsTrue(after[i].rfind(":LDC", 0) != 0 && after[i] != ":LDMM1\r",
					L"Script mode never sends custom-dim commands");
		}

		TEST_METHOD(LookaheadOffIgnoresTheQueuedFrames)
		{
			// Owner 2026-10-14 (lookahead: off): VP reacts to the frame on
			// screen. The same queued black frames that engage the custom dim
			// with the look-ahead (above) are ignored, so it never engages.
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			config.ftbCustomDim = FtbCustomDim::Together;
			config.ftbStepMs = 100;
			config.ftbBlackMs = 200;
			config.lookahead = false;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			StatsBuilder builder(module.Config());
			for (uint64_t s = 1; s < 400; ++s)
				module.SubmitPrediction(s, builder.Build(Frame(16.0), s * FRAME_MS));
			module.SubmitMeasurement(Frame(16.0));
			uint64_t sequence = 0;
			for (int i = 0; i < 300; ++i)
			{
				++sequence;
				clock.To(sequence * FRAME_MS);
				module.SetLookaheadDepth(3);   // queued frames walked, every frame
				module.BeginFrameAt(sequence, sequence * FRAME_MS);
				clock.Settle();
			}
			Assert::IsFalse(module.CustomDimEngaged(), L"look-ahead off: the queued frames are not used");
			Assert::IsTrue(projector->light < 255, L"but the LED still dims on the frame on screen");
		}

		TEST_METHOD(ScriptModeWithTheExperimentConfiguredSendsNoCustomDim)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.ftbCustomDim = FtbCustomDim::Turns;
			config.ftbBlackMs = 100;
			{
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
				for (uint64_t frame = 1; frame <= 120; ++frame)
				{
					module.SubmitMeasurement(Frame(16.0), frame);
					module.BeginFrameAt(frame, frame * FRAME_MS);
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				Assert::IsFalse(module.CustomDimEngaged(), L"A black picture in script mode never engages it");
			}
			for (const auto& w : projector->Writes())
				Assert::IsTrue(w.rfind(":LDC", 0) != 0 && w != ":LDMM1\r",
					L"Only the LDMM 0 safety reset, never custom-dim levels, outside compensate");
		}

		// audit 2026-10-16 (second pass, 5): LDMM 0 at every start, so a crash
		// during the custom dim cannot leave the projector in custom mode once
		// the experiment is switched off.
		TEST_METHOD(StartupAlwaysResetsTheDimMode)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			Assert::IsTrue(config.ftbCustomDim == FtbCustomDim::Off);
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			const auto writes = projector->Writes();
			Assert::AreEqual<size_t>(1, static_cast<size_t>(std::count(writes.begin(), writes.end(), std::string(":LDMM0\r"))),
				L"One LDMM 0 at start with the custom dim off");
			const auto ldmm = std::find(writes.begin(), writes.end(), std::string(":LDMM0\r"));
			const auto light = std::find(writes.begin(), writes.end(), std::string(":LPW1255\r"));
			Assert::IsTrue(ldmm < light, L"Before the light is set");
		}

		// Renders dark frames (look-ahead off: the frame on screen) until the
		// LED has dimmed.
		static void DimOnDarkFrames(LedDriveModule& module, std::shared_ptr<FakeProjector> projector, TestClock& clock)
		{
			module.SubmitMeasurement(Frame(16.0));
			for (uint64_t sequence = 1; sequence <= 600 && projector->light >= 200; ++sequence)
			{
				clock.To(sequence * FRAME_MS);
				module.BeginFrameAt(sequence, sequence * FRAME_MS);
				clock.Settle();
			}
		}

		// audit 2026-10-16 (second pass, 9): led_idle_restore_s - no video
		// for that long (pause, stop) and compensate puts the LED back to the
		// ceiling and CNTR to 50, once.
		TEST_METHOD(IdleRestoresFullLightInCompensate)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Assisted();
			config.lookahead = false;
			config.idleRestoreS = 1;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			DimOnDarkFrames(module, projector, clock);
			Assert::IsTrue(projector->light < 255, L"The LED dimmed on the dark picture");
			// Video stops: within led_idle_restore_s (+ a module tick) full light.
			const double stopped = clock.Ms();
			Assert::IsTrue(clock.RunUntil([&] { return projector->light == 255 && projector->contrast == 50; }, 4000),
				L"Full light and CNTR 50 after the idle time");
			Assert::IsTrue(clock.Ms() - stopped >= 1000.0, L"not before led_idle_restore_s");
			Assert::AreEqual(255.0, module.ModelledLight(), 1e-9, L"The model follows");
			Assert::IsTrue(clock.Run(300));
			const size_t settled = projector->Writes().size();
			Assert::IsTrue(clock.Run(1500));
			Assert::AreEqual(settled, projector->Writes().size(), L"Restored once, not every tick");
			// Video returns: compensate carries on.
			module.SubmitMeasurement(Frame(16.0));
			const double resumed = clock.Ms();
			for (uint64_t sequence = 1000; sequence < 1300 && projector->light == 255; ++sequence)
			{
				clock.To(resumed + (sequence - 999) * FRAME_MS);
				module.BeginFrameAt(sequence, sequence * FRAME_MS);
				clock.Settle();
			}
			Assert::IsTrue(projector->light < 255, L"Dims again once frames return");
		}

		TEST_METHOD(IdleRestoreOffLeavesTheProjector)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			config.lookahead = false;
			config.idleRestoreS = 0;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			DimOnDarkFrames(module, projector, clock);
			const int dimmed = projector->light;
			Assert::IsTrue(dimmed < 255);
			Assert::IsTrue(clock.Run(1800));
			Assert::AreEqual(dimmed, projector->light, L"led_idle_restore_s 0: unchanged (as before)");
		}

		// audit 2026-10-16 (second pass, 9): internal_script restores full
		// light and stops acting on the last frame's statistics while no video
		// comes.
		TEST_METHOD(IdleRestoresFullLightInScriptModeAndStopsTicking)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.idleRestoreS = 1;
			TestClock clock;
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			Assert::IsTrue(clock.Settle());
			for (uint64_t frame = 1; projector->light == 255 && frame <= 300; ++frame)
			{
				module.SubmitMeasurement(Frame(16.0), frame);
				module.BeginFrameAt(frame, frame * FRAME_MS);
				Assert::IsTrue(clock.Run(FRAME_MS, FRAME_MS));
			}
			Assert::IsTrue(projector->light < 255, L"The script dimmed the LED");
			Assert::IsTrue(clock.RunUntil([&] { return projector->light == 255 && projector->contrast == 50; }, 4000),
				L"Full light and CNTR 50 after the idle time");
			Assert::IsTrue(clock.Run(300));
			const int ticks = module.Ticks();
			const size_t writes = projector->Writes().size();
			Assert::IsTrue(clock.Run(1200));
			Assert::AreEqual(ticks, module.Ticks(), L"No script ticks on the stale statistics");
			Assert::AreEqual(writes, projector->Writes().size(), L"Nothing more sent while idle");
		}

		TEST_METHOD(ExperimentalFadeToBlackNeedsTheLookahead)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			config.ftbCustomDim = FtbCustomDim::Together;
			config.ftbBlackMs = 100;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(16.0));
			for (uint64_t frame = 1; frame <= 200; ++frame)
			{
				FrameAt(module, clock, 0, frame * FRAME_MS); // no look-ahead (Low_Latency)
				clock.Settle();
			}
			Assert::IsTrue(module.ModelledLight() <= config.ledFloorBlack);
			Assert::IsFalse(module.CustomDimEngaged(), L"Without look-ahead the custom dim never engages");
			for (const auto& w : projector->Writes())
				Assert::IsTrue(w.rfind(":LDC", 0) != 0 && w != ":LDMM1\r");
		}

		TEST_METHOD(ReactiveModeUsesItsOwnFloor)
		{
			LedDriveConfig config = Compensate();
			config.ledFloor = 60;
			config.ledFloorReactive = 100;
			auto settle = [&config](bool covered)
			{
				CompensateController controller(config);
				StatsBuilder builder(config);
				for (int frame = 0; frame < 600; ++frame)
					controller.Tick(builder.Build(Frame(18.0), frame * FRAME_MS), FRAME_MS, nullptr, covered, covered);
				return controller.Light();
			};
			Assert::AreEqual(60, settle(true), L"With look-ahead: led_floor");
			Assert::AreEqual(100, settle(false), L"Without: led_floor_reactive");
			Assert::AreEqual(100, LedDriveConfig().ledFloorReactive, L"Default: the script's 100");
		}

		TEST_METHOD(SeenCutsJumpAndUnseenBrightCutsDump)
		{
			const LedDriveConfig config = Compensate();
			auto settled = [&config](double code, StatsBuilder& builder, double& now)
			{
				CompensateController controller(config);
				for (int frame = 0; frame < 600; ++frame, now += FRAME_MS)
					controller.Tick(builder.Build(Frame(code), now), FRAME_MS, nullptr, true, true);
				return controller;
			};
			// Seen dark-to-bright cut: one jump to the new level, sent whole.
			{
				StatsBuilder builder(config);
				double now = 0.0;
				CompensateController controller = settled(18.0, builder, now);
				Assert::AreEqual(60, controller.Light());
				const FrameStats cut = builder.Build(Frame(120.0), now);
				Assert::IsTrue(cut.cut);
				const ControllerOutput out = controller.Tick(cut, FRAME_MS, nullptr, true, true);
				Assert::AreEqual(255, out.light, L"A seen cut jumps");
				Assert::IsTrue(out.jump);
			}
			// Seen bright-to-dark cut: jumps down too (the cut hides it).
			{
				StatsBuilder builder(config);
				double now = 0.0;
				CompensateController controller = settled(120.0, builder, now);
				const FrameStats cut = builder.Build(Frame(30.0), now);
				const ControllerOutput out = controller.Tick(cut, FRAME_MS, nullptr, true, true);
				Assert::AreEqual(CurveLevel(config, 30), out.light);
				Assert::IsTrue(out.jump);
			}
			// Unseen dark-to-bright cut: straight up (the script's dump)...
			{
				StatsBuilder builder(config);
				double now = 0.0;
				CompensateController controller = settled(18.0, builder, now);
				const ControllerOutput out = controller.Tick(builder.Build(Frame(120.0), now), FRAME_MS);
				Assert::AreEqual(255, out.light, L"An unseen bright cut dumps straight up");
				Assert::IsTrue(out.jump);
			}
			// ...but an unseen bright-to-dark cut ramps, and the switches work.
			{
				StatsBuilder builder(config);
				double now = 0.0;
				CompensateController controller = settled(120.0, builder, now);
				const ControllerOutput out = controller.Tick(builder.Build(Frame(30.0), now), FRAME_MS);
				Assert::AreEqual(253, out.light, L"An unseen dimming cut ramps");
				Assert::IsFalse(out.jump);
			}
			{
				LedDriveConfig off = config;
				off.cutJump = false;
				off.brightCutDump = false;
				StatsBuilder builder(off);
				CompensateController controller(off);
				for (int frame = 0; frame < 600; ++frame)
					controller.Tick(builder.Build(Frame(18.0), frame * FRAME_MS), FRAME_MS, nullptr, true, true);
				const ControllerOutput out = controller.Tick(builder.Build(Frame(120.0), 600 * FRAME_MS),
					FRAME_MS, nullptr, true, true);
				Assert::IsTrue(out.light < 255 && !out.jump, L"cut_jump: false ramps instead");
			}
		}

		TEST_METHOD(CutJumpsAreNotSplitByTheDimmingCap)
		{
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = Port(80);
			config.readBack = false;
			config.minCommandGapMs = 20;
			config.maxLevelsPerCommand = 12;
			auto driver = CreateProjectorDriver("barco_ascii", config,
				[projector] { return std::make_unique<FakePort>(projector); });
			std::string error;
			Assert::IsTrue(driver->Open(error));
			Assert::IsTrue(driver->SetLightNow(255, 0));
			driver->SetLightJump(60);
			Assert::IsTrue(WaitFor([&] { return driver->Status().lastSentLight == 60; }));
			Assert::AreEqual<size_t>(2, projector->Writes().size(), L"255, then 60 in one command");
		}

		TEST_METHOD(ExperimentalFadeToBlackSettingsAreValidated)
		{
			std::string error;
			Assert::IsTrue(Validates("[led_drive.Default]\nftb_custom_dim: turns\nftb_step_ms: 100\n"
				"ftb_custom_start: 40\nftb_custom_floor: 0\n", error), std::wstring(error.begin(), error.end()).c_str());
			Assert::IsFalse(Validates("[led_drive.Default]\nftb_custom_start: 60\n", error),
				L"The FL35's custom dim stops at 40");
			Assert::AreEqual(40, LedDriveConfig().ftbCustomStart);
			Assert::AreEqual(0, LedDriveConfig().ftbCustomFloor, L"Deepest default: off");
			Assert::IsFalse(Validates("[led_drive.Default]\nftb_custom_dim: sometimes\n", error));
			Assert::IsFalse(Validates("[led_drive.Default]\nftb_custom_start: 40\nftb_custom_floor: 40\n", error));
			Assert::IsTrue(error.find("ftb_custom_floor") != std::string::npos);
			Assert::IsFalse(Validates("[led_drive.Default]\nftb_step_ms: 10\n", error));
			Assert::IsTrue(LedDriveConfig().ftbCustomDim == FtbCustomDim::Off, L"Off unless chosen");
		}

		TEST_METHOD(ShippedExampleProfileIsValidAndEqualsTheBuiltInValues)
		{
			std::string repository = __FILE__;
			for (int level = 0; level < 3; ++level)
				repository.resize(repository.find_last_of("\\/"));
			const std::string example = repository + "\\docs\\cinema\\led-profile-example.cfg";
			std::string error;
			const std::string main = Write("[led_drive.Default]\nprofile_file: " + example + "\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(main.c_str()));
			Assert::IsTrue(Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
			const LedDriveConfig fromExample = ReadLedDriveConfig(config);
			DeleteFileA(main.c_str());
			const LedDriveConfig builtIn;
			Assert::AreEqual(builtIn.curve.size(), fromExample.curve.size());
			for (size_t i = 0; i < builtIn.curve.size(); ++i)
			{
				Assert::AreEqual(builtIn.curve[i].apl, fromExample.curve[i].apl);
				Assert::AreEqual(builtIn.curve[i].level, fromExample.curve[i].level);
			}
			for (int distance : { 0, 9, 10, 24, 25, 49, 50, 99, 100, 200 })
			{
				Assert::AreEqual(builtIn.stepsUpDark.StepFor(distance), fromExample.stepsUpDark.StepFor(distance));
				Assert::AreEqual(builtIn.stepsUpBright.StepFor(distance), fromExample.stepsUpBright.StepFor(distance));
				Assert::AreEqual(builtIn.stepsDown.StepFor(distance), fromExample.stepsDown.StepFor(distance));
			}
			Assert::AreEqual(builtIn.rateUp, fromExample.rateUp, 1e-12);
			Assert::AreEqual(builtIn.rateDown, fromExample.rateDown, 1e-12);
			Assert::AreEqual(builtIn.deadband, fromExample.deadband);
			Assert::AreEqual(builtIn.knee, fromExample.knee, 1e-12);
			Assert::AreEqual(builtIn.highlightReleaseMs, fromExample.highlightReleaseMs);
			Assert::AreEqual(builtIn.aplPersistMs, fromExample.aplPersistMs);
			Assert::AreEqual(builtIn.cutThreshold, fromExample.cutThreshold, 1e-12);
			Assert::AreEqual(builtIn.aplWeights[0], fromExample.aplWeights[0], 1e-12);
			Assert::AreEqual(builtIn.compensationEotf, fromExample.compensationEotf, 1e-12);
		}

		TEST_METHOD(CompensateRampsAtTheRateLimitsAndHoldsInsideTheDeadband)
		{
			LedDriveConfig config = Compensate();
			config.deadband = 10;
			config.brightCutDump = false; // the ramps themselves
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
			TestClock clock;
			LedDriveModule module(Port(80), Compensate(),
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(18.0));
			std::vector<double> gains;
			// Sequence 0: no lookahead for these frames.
			std::vector<double> levels(1, 255.0);
			for (int frame = 1; frame <= 8; ++frame)
			{
				gains.push_back(FrameAt(module, clock, 0, frame * FRAME_MS).gain);
				levels.push_back(module.ModelledLight());
				clock.Settle();
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
			TestClock clock;
			LedDriveModule module(Port(50), Compensate(),
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(120.0));
			module.SetLookaheadDepth(3);
			const uint64_t cut = 20;
			Predict(module, 1, 40, cut);
			std::vector<double> gains;
			const std::vector<double> levels = Render(module, clock, cut + 2, &gains);
			Assert::AreEqual(3, module.LatencyFrames());
			for (uint64_t sequence = 1; sequence < cut; ++sequence)
			{
				Assert::AreEqual(255.0, levels[sequence], 1e-12, L"Nothing lands before the cut");
				Assert::AreEqual(1.0, gains[sequence - 1], 1e-12);
			}
			Assert::IsTrue(levels[cut] < 255.0, L"Sent 3 frames ahead, the LED lands on the cut frame");
			Assert::AreEqual(255.0 / levels[cut], gains[cut - 1], 1e-9, L"and so does the gain");
		}

		TEST_METHOD(TheGainFollowsOnlyTheLevelsActuallySent)
		{
			// A 300 ms command gap: most per-frame targets are skipped by the
			// driver, and the gain must step with the levels it really sent.
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.minCommandGapMs = 300;
			TestClock clock;
			LedDriveModule module(port, Compensate(),
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			// The start-up's commands keep the 300 ms gap too, on the clock.
			Assert::IsTrue(clock.RunUntil([&] { return module.DrivingProjector() && projector->light == 255; }));
			// The start-up's last command gap has passed.
			Assert::IsTrue(clock.Run(400));
			const double started = clock.Ms();
			module.SubmitMeasurement(Frame(18.0));
			std::vector<int> sentAt;
			int seen = module.ProjectorStatus().lightSends;
			for (uint64_t frame = 1; frame <= 60; ++frame)
			{
				const CompensationGain gain = FrameAt(module, clock, 0, started + frame * FRAME_MS);
				Assert::AreEqual(255.0 / module.ModelledLight(), gain.gain, 1e-9);
				clock.Settle();
				const DriverStatus status = module.ProjectorStatus();
				if (status.lightSends != seen) sentAt.push_back(status.lastSentLight);
				seen = status.lightSends;
			}
			// At most one send per 300 ms on the drive's clock (plus the first).
			const double elapsedMs = clock.Ms() - started;
			Assert::IsTrue(sentAt.size() >= 2 && sentAt.size() <= static_cast<size_t>(elapsedMs / 300.0) + 1,
				(L"sends in ~1.6 s: " + std::to_wstring(sentAt.size())).c_str());
			// The model holds one of the sent levels, never an unsent target.
			const double modelled = module.ModelledLight();
			bool wasSent = modelled == 255.0;
			for (int level : sentAt) wasSent = wasSent || modelled == level;
			Assert::IsTrue(wasSent, std::to_wstring(modelled).c_str());
		}

		TEST_METHOD(DimmingIsCappedPerCommandSoTheFloorNeverStepsFar)
		{
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = Port(80);
			config.readBack = false;
			config.minCommandGapMs = 20;
			config.maxLevelsPerCommand = 12;
			auto driver = CreateProjectorDriver("barco_ascii", config,
				[projector] { return std::make_unique<FakePort>(projector); });
			std::string error;
			Assert::IsTrue(driver->Open(error));
			Assert::IsTrue(driver->SetLightNow(255, 0));
			driver->SetLight(60);
			Assert::IsTrue(WaitFor([&] { return driver->Status().lastSentLight == 60; }));
			std::vector<int> sent;
			for (const auto& write : projector->Writes())
				sent.push_back(std::stoi(write.substr(5)));
			Assert::AreEqual(255, sent.front());
			for (size_t i = 1; i < sent.size(); ++i)
				Assert::IsTrue(sent[i - 1] - sent[i] <= 12 && sent[i] < sent[i - 1], L"At most 12 levels down per command");
			Assert::AreEqual<size_t>(1 + 17, sent.size(), L"255 to 60 in 12-level steps");
			// Brightening is not capped: a bright scene must not wait.
			driver->SetLight(200);
			Assert::IsTrue(WaitFor([&] { return driver->Status().lastSentLight == 200; }));
			Assert::AreEqual<size_t>(19, projector->Writes().size());
			Assert::AreEqual(200, ProjectorConfig().minCommandGapMs, L"Default gap 200 ms");
			Assert::AreEqual(12, ProjectorConfig().maxLevelsPerCommand);
		}

		TEST_METHOD(SustainedAplRejectsFlashesButFollowsACutAtOnce)
		{
			LedDriveConfig config = Compensate();
			Assert::AreEqual(100, config.aplPersistMs);
			StatsBuilder builder(config);
			double now = 0.0;
			FrameStats stats;
			for (int frame = 0; frame < 30; ++frame, now += FRAME_MS)
				stats = builder.Build(Frame(20.0), now);
			// A bright cut: every block changes, so the new APL counts at once.
			stats = builder.Build(Frame(120.0), now);
			Assert::IsTrue(stats.cut);
			Assert::AreEqual(120.0, stats.sustainedApl, 1e-9);
			now += FRAME_MS;
			stats = builder.Build(Frame(120.0), now);
			Assert::AreEqual(120.0, stats.sustainedApl, 1e-9, L"and keeps counting after the cut frame");
			// A flash without a cut (a fifth of the picture, blocks unchanged):
			// held back for apl_persist_ms.
			StatsBuilder flashes(config);
			double t = 0.0;
			for (int frame = 0; frame < 30; ++frame, t += FRAME_MS)
				stats = flashes.Build(Frame(20.0), t);
			for (int frame = 0; frame < 3; ++frame, t += FRAME_MS)
			{
				stats = flashes.Build(Frame(20.0, 2000), t);
				Assert::IsFalse(stats.cut);
				Assert::AreEqual(20.0, stats.sustainedApl, 1e-9, L"A 3-frame flash never lifts the LED target");
			}
		}

		TEST_METHOD(SettingChangesCarryOnFromTheCurrentLedLevel)
		{
			// Audit blocker (stage7-step4-compensate.md): tuning or switching
			// script/compensate must not restore full light.
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(18.0));
			uint64_t frame = 0;
			auto render = [&](int frames)
			{
				for (int i = 0; i < frames; ++i, ++frame)
				{
					FrameAt(module, clock, 0, frame * FRAME_MS);
					clock.Settle();
				}
			};
			render(60);
			Assert::IsTrue(projector->light < 200, L"Dimmed for the dark scene");
			auto fullLightWrites = [&]
			{
				int count = 0;
				for (const auto& write : projector->Writes())
					if (write == ":LPW1255\r") ++count;
				return count;
			};
			const int startupRestores = fullLightWrites();
			const int dimmed = projector->light;

			LedDriveConfig tuned = config;
			tuned.highlightToleranceStops = 0.5;
			Assert::IsTrue(module.Reconfigure(port, tuned));
			Assert::AreEqual(0.5, module.Config().highlightToleranceStops, 1e-12);
			render(10);
			Assert::AreEqual(startupRestores, fullLightWrites(), L"Tuning never restores full light");
			Assert::IsTrue(projector->light <= dimmed, L"The LED carried on from its level");

			LedDriveConfig script = tuned;
			script.mode = Mode::Script;
			script.scriptLookahead = false;   // the script copy without its look-ahead (owner 2026-10-17)
			Assert::IsTrue(module.Reconfigure(port, script));
			Assert::IsFalse(module.WantsPredictions());
			Assert::IsTrue(clock.Run(300));
			Assert::AreEqual(startupRestores, fullLightWrites(), L"compensate -> script: no restore");
			Assert::IsTrue(module.Reconfigure(port, tuned));
			Assert::IsTrue(module.WantsPredictions());
			render(10);
			Assert::AreEqual(startupRestores, fullLightWrites(), L"script -> compensate: no restore");

			// Changes that need a new module are refused (the renderer then
			// restarts it, restoring full light as before).
			ProjectorConfig otherPort = port;
			otherPort.port = "COM9";
			Assert::IsFalse(module.Reconfigure(otherPort, tuned));
			LedDriveConfig logOnly = tuned;
			logOnly.mode = Mode::LogOnly;
			Assert::IsFalse(module.Reconfigure(port, logOnly));
			LedDriveConfig disabled = tuned;
			disabled.enabled = false;
			Assert::IsFalse(module.Reconfigure(port, disabled));
			ProjectorConfig longer = port;
			longer.ledLatencyMs = 100;
			Assert::IsTrue(module.Reconfigure(longer, tuned), L"led_latency_ms and flux_table apply live");
			Assert::AreEqual(6, module.LatencyFrames());
		}

		TEST_METHOD(FilmIn60HzCountsTheLatencyInSourceFrames)
		{
			// Audit (stage7-step5-delta1.md): 24p in a 60 Hz output shows each
			// source frame for 3 or 2 output frames. 80 ms is 5 output frames but
			// 2 source frames: the command for a cut must go out about 2 source
			// frames ahead, not 5.
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(80);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Compensate();
			config.earlyRamp = false; // the command timing alone
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(120.0));
			module.SetLookaheadDepth(8);
			const uint64_t cut = 20;
			StatsBuilder builder(module.Config());
			for (uint64_t sequence = 1; sequence < 40; ++sequence)
				module.SubmitPrediction(sequence, builder.Build(Frame(sequence < cut ? 120.0 : 18.0),
					sequence * 1000.0 / 24.0));
			uint64_t frame = 0, landedAt = 0;
			for (uint64_t sequence = 1; sequence <= cut + 2 && landedAt == 0; ++sequence)
				for (int repeat = 0; repeat < (sequence % 2 ? 3 : 2) && landedAt == 0; ++repeat, ++frame)
				{
					FrameAt(module, clock, sequence, frame * FRAME_MS, 8, 24.0);
					if (module.ModelledLight() < 255.0) landedAt = sequence;
					clock.Settle();
				}
			Assert::AreEqual(5, module.LatencyFrames(), L"Dead time is in output frames");
			Assert::AreEqual(1000.0 / 24.0, module.SourceFramePeriodMs(), 1e-9);
			Assert::IsTrue(landedAt >= cut - 1 && landedAt <= cut,
				(L"the LED landed on source frame " + std::to_wstring(landedAt)).c_str());
		}

		// 7c-3, the module: black on screen, a bright scene coming in the
		// look-ahead. Before the cut the LED never goes above black_prelift_max
		// and CNTR goes to cut_cntr_level (59); after the cut the LED goes to
		// the scene's level and CNTR descends paired.
		TEST_METHOD(ExitFromBlackPrepositionsContrastAndCapsTheLed)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port(50);
			port.maxLevelsPerCommand = 0;
			LedDriveConfig config = Assisted();
			ApplyRatePreset(config, true); // reach the black floor quickly
			config.blackPreliftMax = 120;
			config.ledFloorBlack = 60;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SetLookaheadDepth(8);
			const uint64_t cut = 400;
			StatsBuilder builder(module.Config());
			for (uint64_t sequence = 1; sequence < cut + 60; ++sequence)
				module.SubmitPrediction(sequence, builder.Build(Frame(sequence < cut ? 16.0 : 120.0),
					sequence * FRAME_MS));
			bool contrastBeforeCut = false;
			for (uint64_t sequence = 1; sequence < cut + 60; ++sequence)
			{
				module.SubmitMeasurement(Frame(sequence < cut ? 16.0 : 120.0), sequence);
				FrameAt(module, clock, sequence, sequence * FRAME_MS, 8);
				clock.Settle();
				// Once at the black floor, until the cut's own jump goes out
				// (latency frames ahead of the cut).
				if (sequence > 300 && sequence + 5 < cut)
					Assert::IsTrue(projector->light <= 120,
						(L"the LED above black_prelift_max while black, sequence " + std::to_wstring(sequence)).c_str());
				if (sequence < cut && projector->contrast == 59) contrastBeforeCut = true;
			}
			Assert::IsTrue(contrastBeforeCut, L"CNTR pre-positioned to cut_cntr_level before the cut");
			Assert::AreEqual(255, projector->light, L"the scene's level after the cut");
			Assert::IsTrue(module.AssistContrast() <= 59, L"CNTR descends paired after the cut");
			module.Stop();
			Assert::AreEqual(50, projector->contrast);
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
				TestClock clock;
				LedDriveModule module(Port(50), Compensate(),
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(::Frame(120.0));
				module.SetLookaheadDepth(depth);
				Assert::AreEqual(depth, module.LookaheadDepth());
				Predict(module, 1, 60, 30);
				const std::vector<double> levels = Render(module, clock, 30);
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
			{
				// source_range (owner 2026-10-15): the captured signal's range as the
				// picture uses it - full reads the same codes as 0-1023; limited (or
				// -1, the format's range for UYVY) as 64-940.
				RawMeasurement full, limited;
				Assert::IsTrue(MeasureRawFrame(luma, rect, IdentityRawTransfer(), full, 1));
				Assert::IsTrue(MeasureRawFrame(luma, rect, IdentityRawTransfer(), limited, 0));
				const double fullSignal = std::lround(504.0 / 1023.0 * 1023) / 1023.0;
				Assert::AreEqual(16.0 + 219.0 * fullSignal, static_cast<double>(full.sum) / SUM_SCALE / full.blocks, 0.05,
					L"source_range full: the codes read as full range");
				Assert::AreEqual(static_cast<double>(raw.sum), static_cast<double>(limited.sum), 0.5,
					L"source_range limited: as the format's own range for UYVY");
				LedDriveModule module(ProjectorConfig(), Compensate());
				Assert::AreEqual(-1, module.SourceRange(), L"from the format until the renderer says otherwise");
				module.SetSourceRange(1);
				Assert::AreEqual(1, module.SourceRange());
			}

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

		// audit 2026-10-16: Sample() emits full-range RGB in the limited code
		// domain; the look-ahead must not convert it a second time (black read
		// ~6% with source_range auto).
		TEST_METHOD(FullRangeRgbLookaheadReadsBlackAsZeroAndWhiteAsFull)
		{
			const int width = 96, height = 54;
			auto frame = [&](uint8_t level)
			{
				return std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, level);
			};
			auto measure = [&](std::vector<uint8_t>& pixels, int range)
			{
				AnalysisLumaSource luma;
				luma.data = pixels.data();
				luma.dataBytes = pixels.size();
				luma.width = width;
				luma.height = height;
				luma.rowBytes = static_cast<size_t>(width) * 4;
				luma.format = AnalysisLumaFormat::NativeRgb;
				luma.encoding = VideoFrameEncoding::BGRA_8BIT;
				luma.colorspace = ColorSpace::REC_709;
				Assert::IsTrue(luma.IsValid());
				Assert::IsFalse(LumaIsFullRange(luma), L"Sample() returns limited codes");
				Assert::IsTrue(SampleRemapsFullRange(luma));
				QueuedFrameStatistics::PictureRect rect;
				rect.right = width;
				rect.bottom = height;
				RawMeasurement raw;
				Assert::IsTrue(MeasureRawFrame(luma, rect, IdentityRawTransfer(), raw, range));
				// The mean block code; APL % = (code - 16) / 219.
				return (static_cast<double>(raw.sum) / SUM_SCALE / raw.blocks - 16.0) / 219.0 * 100.0;
			};
			std::vector<uint8_t> black = frame(0), white = frame(255), grey = frame(128);
			for (const int range : { -1, 1 })
			{
				Assert::AreEqual(0.0, measure(black, range), 0.05, L"Full-range RGB black is 0% APL");
				Assert::AreEqual(100.0, measure(white, range), 0.05, L"Full-range RGB white is 100% APL");
				// 8-bit 128 = 10-bit 514 of 1023.
				Assert::AreEqual(514.0 / 1023.0 * 100.0, measure(grey, range), 0.15,
					L"Mid grey as the source's full-range code");
			}
			// source_range: limited forced on the same RGB: the source's own codes
			// read as 64-940 (514 -> (514 - 64) / 876), as before the sampler change.
			Assert::AreEqual((514.0 - 64.0) / 876.0 * 100.0, measure(grey, 0), 0.15,
				L"Forced limited reads the source codes, not the remapped ones");
			Assert::AreEqual(0.0, measure(black, 0), 0.05);
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

		// Audit 2026-10-16: correction_weight_by_time off keeps the earlier
		// weight (one output frame period per update, however many output
		// frames passed); on weights by the output frames since the update.
		TEST_METHOD(CorrectionWeightByTimeOffKeepsTheEarlierWeight)
		{
			for (const bool byTime : { false, true })
			{
				LedDriveConfig config = Compensate();
				config.correctionWeightByTime = byTime;
				LedDriveModule module(ProjectorConfig(), config);
				StatsBuilder builder(module.Config());
				const double exact = 100.0;
				auto submit = [&](uint64_t sequence, double ratio)
				{
					module.SubmitPrediction(sequence, builder.Build(Frame(16.0 + (exact - 16.0) / ratio),
						sequence * FRAME_MS));
					module.SubmitMeasurement(Frame(exact), sequence);
				};
				submit(1, 1.0);   // seeds the correction at 1
				Assert::AreEqual(1.0, module.CorrectionRatio(), 1e-9);
				// Three output frames (24p on 60 Hz averages 2.5) before the next
				// source frame's measurement.
				for (int frame = 1; frame <= 3; ++frame)
					module.BeginFrameAt(0, frame * FRAME_MS);
				const double period = module.FramePeriodMs();
				submit(2, 1.2);
				const double frames = byTime ? 3.0 : 1.0;
				Assert::AreEqual(1.0 + (1.2 - 1.0) * frames * period / 3000.0, module.CorrectionRatio(), 1e-4,
					byTime ? L"on: weighted by the frames since the last update" : L"off: one frame period per update");
			}
			LedDriveConfig defaults;
			Assert::IsTrue(defaults.correctionWeightByTime, L"On by default");
		}

		// script_lookahead off (owner 2026-10-17; on by default, when the script copy
		// looks ahead through the queue): script mode wants no predictions.
		TEST_METHOD(ScriptModeIgnoresTheLookahead)
		{
			LedDriveConfig script;
			script.enabled = true;
			script.mode = Mode::Script;
			Assert::IsTrue(LedDriveModule(ProjectorConfig(), script).WantsPredictions(), L"script_lookahead on by default");
			script.scriptLookahead = false;
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

		// Review 2026-10-17: the new keys, their defaults and validation.
		TEST_METHOD(Review20261017KeysAreReadWithTheirDefaults)
		{
			const LedDriveConfig defaults;
			Assert::IsTrue(defaults.prepositionBlackOnly, L"preposition_black_only on by default");
			Assert::IsTrue(defaults.ledLandingMs, L"led_landing_ms on by default");
			Assert::IsFalse(defaults.cutLeadWindow, L"cut_lead off by default (experimental)");
			Assert::AreEqual(0, defaults.ledLookaheadMs, L"led_lookahead_ms 0 = the queue walk's depth");
			std::string error;
			const std::string good = "[led_drive.Default]\npreposition_black_only: false\nled_landing_ms: false\n"
				"cut_lead: window\nled_lookahead_ms: 380\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::IsFalse(led.prepositionBlackOnly);
			Assert::IsFalse(led.ledLandingMs);
			Assert::IsTrue(led.cutLeadWindow);
			Assert::AreEqual(380, led.ledLookaheadMs);
			Assert::IsFalse(Validates("[led_drive.Default]\ncut_lead: early\n", error));
			Assert::IsFalse(Validates("[led_drive.Default]\nled_lookahead_ms: 5000\n", error));
		}

		// led_lookahead_ms caps the LED's horizon in time (source frames at
		// the measured rate) and is what the renderer would ask the walk for.
		TEST_METHOD(LedLookaheadMsCapsTheHorizonInTime)
		{
			LedDriveConfig config = Compensate();
			config.ledLookaheadMs = 100;
			LedDriveModule module(ProjectorConfig(), config);
			module.BeginFrameAt(0, 0.0, 24.0);
			Assert::AreEqual<size_t>(2, module.WantedLookaheadFrames(), L"100 ms at 24p");
			module.BeginFrameAt(0, FRAME_MS, 60.0);
			Assert::AreEqual<size_t>(6, module.WantedLookaheadFrames(), L"100 ms at 60p");
			config.ledLookaheadMs = 0;
			LedDriveModule plain(ProjectorConfig(), config);
			// Owner 2026-10-17: 0 = the whole queue (queue = look-ahead).
			Assert::AreEqual<size_t>(LedDriveModule::WHOLE_QUEUE, plain.WantedLookaheadFrames(), L"0: every queued frame");
			// A cut 8 frames away is acted on only once within 100 ms (6
			// frames at 60p), although the walk sees 8.
			auto firstLanding = [](int lookaheadMs)
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				LedDriveConfig led = Compensate();
				led.ledLookaheadMs = lookaheadMs;
				led.ledLandingMs = false;   // frame-exact landings for the comparison
				TestClock clock;
				LedDriveModule module(Port(50), led,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(::Frame(120.0));
				Predict(module, 1, 60, 30);
				std::vector<double> levels(1, 255.0);
				for (uint64_t sequence = 1; sequence <= 30; ++sequence)
				{
					FrameAt(module, clock, sequence, sequence * FRAME_MS, 8, 60.0);
					levels.push_back(module.ModelledLight());
					clock.Settle();
				}
				for (uint64_t sequence = 1; sequence < levels.size(); ++sequence)
					if (levels[sequence] < 255.0) return sequence;
				return uint64_t{ 0 };
			};
			Assert::AreEqual<uint64_t>(25, firstLanding(0), L"the walk's 8 frames");
			Assert::AreEqual<uint64_t>(27, firstLanding(100), L"capped at 6 frames (100 ms at 60p)");
		}

		// led_landing_ms (review 2026-10-17): a send the driver held back for
		// its command gap lands led_latency_ms after it was written (nearest
		// frame), not latency frames after the frame it was written during.
		TEST_METHOD(LandingIsTimedFromTheWriteInMilliseconds)
		{
			for (const bool landingMs : { true, false })
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				ProjectorConfig port = Port(100);
				port.minCommandGapMs = 85;   // writes drift against the 50 ms frames
				port.maxLevelsPerCommand = 0;
				LedDriveConfig config = Compensate();
				config.lookahead = false;
				config.ledLandingMs = landingMs;
				TestClock testClock;
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); }, testClock);
				Bind(testClock, module, projector);
				module.Start();
				// The start-up keeps the 85 ms gap too, on the clock.
				Assert::IsTrue(testClock.RunUntil([&] { return module.DrivingProjector() && projector->light == 255; }));
				const auto start = testClock.clock->Now();
				auto clock = [&] { return std::chrono::duration<double, std::milli>(testClock.clock->Now() - start).count(); };
				uint64_t sequence = 0;
				// Bright frames until the module's frame period has settled on 50 ms;
				// the driver's gap runs on the clock in 5 ms steps between frames.
				module.SubmitMeasurement(Frame(235.0));
				for (int frame = 0; frame < 40; ++frame)
				{
					module.BeginFrameAt(++sequence, clock());
					Assert::IsTrue(testClock.Run(50.0, 5.0));
				}
				const size_t firstWrite = projector->Writes().size();
				std::vector<std::pair<double, double>> modelled;   // (frame time, modelled level)
				module.SubmitMeasurement(Frame(16.0));
				for (int frame = 0; frame < 40; ++frame)
				{
					const double now = clock();
					module.BeginFrameAt(++sequence, now);
					modelled.push_back({ now, module.ModelledLight() });
					Assert::IsTrue(testClock.Run(50.0, 5.0));
				}
				const double period = module.FramePeriodMs();
				Assert::IsTrue(period > 40.0 && period < 75.0, (L"frame period " + std::to_wstring(period)).c_str());
				const auto writes = projector->Writes();
				const auto times = projector->WriteTimes();
				int checked = 0, early = 0, outside = 0;
				for (size_t i = firstWrite; i < writes.size(); ++i)
				{
					if (writes[i].rfind(":LPW1", 0) != 0 || writes[i].find('?') != std::string::npos) continue;
					const int level = std::stoi(writes[i].substr(5));
					const double written = std::chrono::duration<double, std::milli>(times[i] - start).count();
					for (const auto& entry : modelled)
						if (entry.second == level)
						{
							const double due = written + 100.0;
							++checked;
							if (entry.first < due - period / 2.0 - 3.0) ++early;
							if (entry.first < due - period * 0.6 - 5.0 || entry.first > due + 100.0) ++outside;   // jitter margin
							break;
						}
				}
				Assert::IsTrue(checked >= 5, (L"landings checked: " + std::to_wstring(checked)).c_str());
				// Off (whole frames from the frame each send was written during)
				// is only exercised: under load its landings may fall either side.
				Logger::WriteMessage((std::string(landingMs ? "on" : "off") + ": " + std::to_string(checked) +
					" landings, " + std::to_string(early) + " early, " + std::to_string(outside) + " outside").c_str());
				if (landingMs)
					Assert::AreEqual(0, outside, L"on: every send lands at the frame nearest write + led_latency_ms");
			}
		}

		// Bug hunt 2026-10-17 (internal_script): a CNTR whose write failed is
		// sent again at the next tick with the value the script controller
		// believes (it would never repeat it itself: CNTR stuck above need).
		TEST_METHOD(ScriptResendsAFailedWriteAtTheNextTick)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 400;
			ParseCurve("0:255, 255:255", config.curve);   // light stays at the ceiling
			TestClock clock;
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(50.0));   // APL 45-60, no highlight: CNTR 51
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }), L"the script raised CNTR to 51");
			Assert::IsTrue(clock.Settle());   // that tick has ended
			const int ticks = module.Ticks();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = true;
			}
			module.SubmitMeasurement(Frame(50.0, 100, 235));   // a highlight: back to 50
			Assert::IsTrue(clock.RunUntil([&] { return module.Ticks() > ticks; }));
			Assert::AreEqual(51, module.ProjectorStatus().lastSentContrast, L"the write of 50 failed");
			const size_t before = projector->Writes().size();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = false;
			}
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }, 3000),
				L"CNTR 50 re-sent at the next tick");
			const auto writes = projector->Writes();
			Assert::IsTrue(std::count(writes.begin() + before, writes.end(), std::string(":CNTR50\r")) == 1,
				L"once, with the controller's value");
			Assert::IsTrue(clock.Run(900));
			const auto later = projector->Writes();
			Assert::IsTrue(std::count(later.begin() + before, later.end(), std::string(":CNTR50\r")) == 1,
				L"not repeated once it went through");
			module.Stop();
		}

		// Post-merge audit 2026-10-17 (B1): a CNTR that was written but got no reply is
		// not re-sent (the projector may well have taken it); only a failed write is
		// (ScriptResendsAFailedWriteAtTheNextTick).
		TEST_METHOD(ScriptDroppedReplyIsNotResent)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 400;
			ParseCurve("0:255, 255:255", config.curve);
			TestClock clock;
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(50.0));
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }), L"the script raised CNTR to 51");
			Assert::IsTrue(clock.Settle());
			const size_t before = projector->Writes().size();
			projector->silent = true;   // written, never answered
			module.SubmitMeasurement(Frame(50.0, 100, 235));   // a highlight: back to 50
			auto count50 = [&]
			{
				const auto writes = projector->Writes();
				return std::count(writes.begin() + before, writes.end(), std::string(":CNTR50\r"));
			};
			Assert::IsTrue(clock.RunUntil([&] { return count50() == 1; }), L"CNTR 50 written");
			Assert::IsTrue(clock.Run(300));
			projector->silent = false;
			// Past Barco's 2 s after the missing reply and several ticks.
			Assert::IsTrue(clock.Run(3000));
			Assert::AreEqual(static_cast<std::ptrdiff_t>(1), static_cast<std::ptrdiff_t>(count50()),
				L"a dropped reply is not re-sent");
			module.Stop();
		}

		// Bug hunt 2026-10-17 (internal_script): the idle restore decides from
		// what the port last saw: CNTR 51 written, the write of 50 failed, so
		// the script believes 50 - the restore still sends CNTR 50.
		TEST_METHOD(ScriptIdleRestoreSendsContrastTheProjectorLastSaw)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 1500;
			config.idleRestoreS = 1;
			ParseCurve("0:255, 255:255", config.curve);
			TestClock clock;
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			uint64_t sequence = 0;
			// Frames keep coming every 50 ms (no idle restore) until the condition holds.
			auto playUntil = [&](const std::function<bool()>& condition)
			{
				for (int frame = 0; frame < 80 && !condition(); ++frame)
				{
					++sequence;
					module.BeginFrameAt(sequence, sequence * FRAME_MS);
					clock.Run(50.0);
				}
				return condition();
			};
			module.SubmitMeasurement(Frame(50.0));
			Assert::IsTrue(playUntil([&] { return projector->contrast == 51; }), L"the script raised CNTR to 51");
			Assert::IsTrue(clock.Settle());   // that tick has ended
			const int ticks = module.Ticks();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = true;
			}
			module.SubmitMeasurement(Frame(50.0, 100, 235));
			// Until the next tick has tried (and failed) 50.
			Assert::IsTrue(playUntil([&] { return module.Ticks() > ticks; }));
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = false;
			}
			Assert::AreEqual(51, module.ProjectorStatus().lastSentContrast, L"the port last saw 51");
			Assert::AreEqual(51, projector->contrast);
			// Video stops: the idle restore runs before any further tick.
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }, 4000),
				L"the idle restore sent CNTR 50 although the script believed 50");
			module.Stop();
		}
		// ---- Integration 2026-10-17: restore switches and the forced-shutdown restore ----

		TEST_METHOD(RestoreSwitchesAreReadWithTheirDefaults)
		{
			const LedDriveConfig defaults;
			Assert::IsTrue(defaults.scriptResendFailedWrites, L"script_resend_failed_writes on by default");
			Assert::IsTrue(defaults.wtwPassthrough, L"led_wtw_passthrough on by default");
			Assert::IsTrue(defaults.btbPassthrough, L"led_btb_passthrough on by default");
			std::string error;
			const std::string good = "[led_drive.Default]\nscript_resend_failed_writes: false\nled_wtw_passthrough: false\n"
				"led_btb_passthrough: false\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::IsFalse(led.scriptResendFailedWrites);
			Assert::IsFalse(led.wtwPassthrough);
			Assert::IsFalse(led.btbPassthrough);
			Assert::IsFalse(Validates("[led_drive.Default]\nscript_resend_failed_writes: maybe\n", error));
			Assert::IsFalse(Validates("[led_drive.Default]\nled_btb_passthrough: maybe\n", error));
		}

		// ---- Bug hunt 2 2026-10-17: "Dynamic Drive" - [dynamic_drive*] is an alias ----

		TEST_METHOD(DynamicDriveSectionIsAnAliasOfLedDrive)
		{
			Assert::IsTrue(OwnsSection("dynamic_drive") && OwnsSection("dynamic_drive.default") &&
				OwnsSection("led_drive.default") && OwnsSection("projector.default"));
			Assert::IsTrue(MainConfigSchema::OwnsSection("dynamic_drive.default"), L"the main schema owns the alias");
			Assert::IsFalse(OwnsSection("dynamic_drives") || OwnsSection("dynamic_driver.x"));
			Assert::AreEqual(std::string("dynamic_drive.default"), LedDriveSectionAlias("Led_Drive.Default"));
			Assert::AreEqual(std::string("led_drive"), LedDriveSectionAlias("dynamic_drive"));
			Assert::AreEqual(std::string(), LedDriveSectionAlias("projector.default"));

			// Read through the alias, with strict validation.
			std::string error;
			const std::string alias = "[dynamic_drive.Default]\nenabled: true\ncommand_source: compensate\n"
				"led_floor: 90\nled_btb_passthrough: false\n";
			Assert::IsTrue(Validates(alias, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::IsFalse(Validates("[dynamic_drive.Default]\nled_floor: lots\n", error), L"the alias is validated");
			Assert::IsFalse(Validates("[dynamic_drive.Default]\nno_such_key: 1\n", error), L"unknown keys in the alias");
			{
				const std::string file = Write(alias);
				ConfigFile config;
				Assert::IsTrue(config.Load(file.c_str()));
				DeleteFileA(file.c_str());
				Assert::AreEqual(std::string("dynamic_drive.default"), ResolveLedDriveSection(config));
				Assert::AreEqual(std::string(), LedDriveAliasConflict(config));
				const LedDriveConfig led = ReadLedDriveConfig(config);
				Assert::IsTrue(led.enabled && led.mode == Mode::Compensate);
				Assert::AreEqual(90, led.ledFloor);
				Assert::IsFalse(led.btbPassthrough);
			}
			// Both spellings: [led_drive.*] takes precedence, and it is reported.
			{
				const std::string both = "[led_drive.Default]\nenabled: false\nled_floor: 80\n"
					"[dynamic_drive.Default]\nenabled: true\nled_floor: 120\n";
				Assert::IsTrue(Validates(both, error), std::wstring(error.begin(), error.end()).c_str());
				const std::string file = Write(both);
				ConfigFile config;
				Assert::IsTrue(config.Load(file.c_str()));
				DeleteFileA(file.c_str());
				Assert::AreEqual(std::string("led_drive.default"), ResolveLedDriveSection(config));
				Assert::AreEqual(std::string("led_drive.default"), ResolveLedDriveSection(config, "dynamic_drive.default"));
				Assert::IsFalse(LedDriveAliasConflict(config).empty(), L"both spellings are reported");
				const LedDriveConfig led = ReadLedDriveConfig(config);
				Assert::IsFalse(led.enabled);
				Assert::AreEqual(80, led.ledFloor);
			}
			// Neither: the [led_drive.default] name (what the Config app writes).
			{
				const std::string file = Write("[general]\n");
				ConfigFile config;
				Assert::IsTrue(config.Load(file.c_str()));
				DeleteFileA(file.c_str());
				Assert::AreEqual(std::string(LED_DRIVE_SECTION), ResolveLedDriveSection(config));
			}
			// An LED profile file may use the alias too; profile_file is found in it.
			{
				const std::string profile = Write("[dynamic_drive.Default]\nled_floor: 110\n");
				const std::string file = Write("[dynamic_drive.Default]\nprofile_file: " + profile + "\n");
				ConfigFile config;
				Assert::IsTrue(config.Load(file.c_str()));
				Assert::AreEqual(profile, LedProfilePath(config));
				Assert::IsTrue(ValidateLedProfile(config, error), std::wstring(error.begin(), error.end()).c_str());
				Assert::AreEqual(110, ReadLedDriveConfig(config).ledFloor);
				DeleteFileA(file.c_str());
				DeleteFileA(profile.c_str());
			}
		}

		// ---- Bug hunt 2 2026-10-17: Stop() for concurrent callers, restore result ----

		// Several threads (the forced-shutdown restore and UpdateLedDrive / the
		// connection test's suspend) stop the module at once: only one joins
		// its thread; every caller returns only after the restore, with its
		// result.
		TEST_METHOD(ConcurrentStopJoinsOnceAndEveryCallerWaitsForTheRestore)
		{
			for (int round = 0; round < 3; ++round)
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				LedDriveConfig config = Compensate();
				config.mode = Mode::Script;
				config.scriptPacing = false;
				config.tickMs = 400;
				ParseCurve("0:120, 255:120", config.curve);   // the script dims the LED
				LedDriveModule module(Port(50), config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
				module.SubmitMeasurement(Frame(50.0));
				Assert::IsTrue(WaitFor([&] { return projector->light < 255; }, 4000), L"the script dimmed the LED");
				std::atomic<bool> go{ false };
				std::atomic<int> ready{ 0 };
				constexpr int CALLERS = 3;
				int lightSeen[CALLERS] = {};
				bool confirmed[CALLERS] = {};
				std::vector<std::thread> callers;
				for (int i = 0; i < CALLERS; ++i)
					callers.emplace_back([&, i]
						{
							++ready;
							while (!go.load()) std::this_thread::yield();
							confirmed[i] = module.Stop();
							std::lock_guard<std::mutex> guard(projector->mutex);
							lightSeen[i] = projector->light;
						});
				while (ready.load() < CALLERS) std::this_thread::yield();
				go.store(true);
				for (std::thread& caller : callers) caller.join();
				for (int i = 0; i < CALLERS; ++i)
				{
					Assert::IsTrue(confirmed[i], L"every caller reports the confirmed restore");
					Assert::AreEqual(255, lightSeen[i], L"no caller returns before full light is back");
				}
				Assert::IsTrue(module.Stop(), L"a later Stop is a no-op with the same result");
			}
		}

		// The exit restore fails (the adapter is unplugged): Stop() and the
		// bounded restore report it instead of "restored".
		TEST_METHOD(FailedExitRestoreIsReportedAsNotConfirmed)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 400;
			ParseCurve("0:120, 255:120", config.curve);
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
			module.SubmitMeasurement(Frame(50.0));
			Assert::IsTrue(WaitFor([&] { return projector->light < 255; }, 4000), L"the script dimmed the LED");
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = true;
			}
			const BoundedRestoreResult result = RunBoundedRestore([&module] { return module.Stop(); }, 5000);
			const std::string name = Name(result);
			Assert::IsTrue(result == BoundedRestoreResult::NotConfirmed,
				(L"a failed restore is reported as not confirmed, got " + std::wstring(name.begin(), name.end())).c_str());
			Assert::IsFalse(module.Stop(), L"the result stays with the stopped module");
			std::lock_guard<std::mutex> guard(projector->mutex);
			Assert::IsTrue(projector->light < 255, L"(the fake projector really stayed dim)");
		}

		// Bug hunt 2 (owner 2026-10-17): the peak from every pixel and picture-only
		// measurement are restore switches, on by default.
		TEST_METHOD(MeasurementSwitchesAreReadWithTheirDefaults)
		{
			const LedDriveConfig defaults;
			Assert::IsTrue(defaults.peakFullCoverage, L"led_peak_full_coverage on by default");
			Assert::IsTrue(defaults.pictureOnly, L"led_picture_only on by default");
			std::string error;
			const std::string good = "[led_drive.Default]\nled_peak_full_coverage: false\nled_picture_only: false\n";
			Assert::IsTrue(Validates(good, error), std::wstring(error.begin(), error.end()).c_str());
			const std::string file = Write(good);
			ConfigFile config;
			Assert::IsTrue(config.Load(file.c_str()));
			const LedDriveConfig led = ReadLedDriveConfig(config);
			DeleteFileA(file.c_str());
			Assert::IsFalse(led.peakFullCoverage);
			Assert::IsFalse(led.pictureOnly);
			Assert::IsFalse(Validates("[led_drive.Default]\nled_picture_only: sometimes\n", error));
			Assert::IsFalse(Validates("[led_drive.Default]\nled_peak_full_coverage: 2\n", error));
		}

		// script_resend_failed_writes off: the pure script stream - the failed
		// CNTR 50 is not repeated (the script believes 50 and never sends it again).
		TEST_METHOD(ScriptResendOffKeepsThePureScriptStream)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 400;
			config.scriptResendFailedWrites = false;
			ParseCurve("0:255, 255:255", config.curve);
			TestClock clock;
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(50.0));
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }), L"the script raised CNTR to 51");
			Assert::IsTrue(clock.Settle());
			const int ticks = module.Ticks();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = true;
			}
			module.SubmitMeasurement(Frame(50.0, 100, 235));
			Assert::IsTrue(clock.RunUntil([&] { return module.Ticks() > ticks; }));
			const size_t before = projector->Writes().size();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->writeFails = false;
			}
			Assert::IsTrue(clock.Run(1300));   // three more ticks
			const auto writes = projector->Writes();
			Assert::IsTrue(std::count(writes.begin() + before, writes.end(), std::string(":CNTR50\r")) == 0,
				L"off: a failed write is not repeated");
			Assert::AreEqual(51, projector->contrast, L"the projector still has the value that went through");
			module.Stop();
		}

		// With no failed write, the script's command stream is the same with the
		// resend switch on and off (the resend adds nothing of its own).
		TEST_METHOD(ScriptStreamIsUnchangedWhenNoWriteFails)
		{
			auto run = [](bool resend)
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				LedDriveConfig config = Compensate();
				config.mode = Mode::Script;
				config.scriptPacing = false;
				config.tickMs = 400;
				config.scriptResendFailedWrites = resend;
				ParseCurve("0:255, 255:255", config.curve);
				TestClock clock;
				LedDriveModule module(Port(50), config,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(Frame(50.0));
				Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }), L"CNTR 51");
				module.SubmitMeasurement(Frame(50.0, 100, 235));
				Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }), L"CNTR 50");
				Assert::IsTrue(clock.Run(900));
				module.Stop();
				return projector->Writes();
			};
			const auto on = run(true);
			const auto off = run(false);
			Assert::IsTrue(on == off, L"the same command stream with the resend on and off when nothing fails");
		}

		// The forced shutdown (a blocked renderer retirement) stops the LED drive on
		// its own thread, bounded: the projector gets full light and CNTR 50 back.
		TEST_METHOD(BoundedRestoreReturnsFullLightAndContrast)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.mode = Mode::Script;
			config.scriptPacing = false;
			config.tickMs = 400;
			ParseCurve("0:120, 255:120", config.curve);   // the script dims the LED
			LedDriveModule module(Port(50), config,
				[projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
			module.SubmitMeasurement(Frame(50.0));
			Assert::IsTrue(WaitFor([&] { return projector->contrast == 51 && projector->light < 255; }, 4000),
				L"the script dimmed the LED and raised CNTR");
			const BoundedRestoreResult result = RunBoundedRestore([&module] { return module.Stop(); }, 3000);
			Assert::IsTrue(result == BoundedRestoreResult::Restored, L"stopped within the bound");
			Assert::AreEqual(255, projector->light, L"full light restored");
			Assert::AreEqual(50, projector->contrast, L"CNTR never left high");
		}

		TEST_METHOD(BoundedRestoreGivesUpOnABlockedStop)
		{
			auto release = std::make_shared<std::atomic<bool>>(false);
			const auto started = std::chrono::steady_clock::now();
			const BoundedRestoreResult result = RunBoundedRestore([release]
				{
					while (!release->load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					return true;
				}, 200);
			const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			release->store(true);
			Assert::IsTrue(result == BoundedRestoreResult::TimedOut, L"a blocked stop times out");
			Assert::IsTrue(waited < 2000, L"the caller is not held");
			Assert::IsTrue(RunBoundedRestore([] { return false; }, 1000) == BoundedRestoreResult::NotConfirmed);
			Assert::IsTrue(RunBoundedRestore([]() -> bool { throw std::runtime_error("x"); }, 1000) ==
				BoundedRestoreResult::Failed);
		}
	};
}
