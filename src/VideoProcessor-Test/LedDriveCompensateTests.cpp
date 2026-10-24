#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <VP-V/modules/drive/projector_control/BarcoAsciiDriver.h>
#include <VP-V/modules/drive/projector_control/LedController.h>
#include <led/LedDriveModule.h>
#include <VP-V/modules/drive/led_measure/LedMeasurement.h>

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

	// Feeds frames at 60 Hz for `ms` and returns the last statistics.
	FrameStats Play(StatsBuilder& builder, double& now, double ms, const RawMeasurement& raw)
	{
		FrameStats stats;
		for (double end = now + ms; now < end; now += 1000.0 / 60.0)
			stats = builder.Build(raw, now);
		return stats;
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

	// cntr-cut fix 2026-10-18: a bright cut through the module at the real frame
	// rate. The module's clock moves with the frames (one frame period each), so
	// every time-based statistic (the sustained highlight's highlight_persist_ms,
	// the LED's settle) ages as it does on the projector.
	struct CutOptions
	{
		double fps = 24.0;
		bool dump = true;          // cut_cntr_dump
		bool lookahead = false;    // a cut seen in the queue (predictions over half a second)
		bool pairPredict = true;   // cntr_pair_predict
		bool legacyPort = false;   // the original test's port: no command gap, read-back on
	};

	struct CutRun
	{
		CutOptions options;
		bool started = false;
		int before = 0;              // the assist's CNTR a second before the cut
		std::vector<int> after;      // CNTR written from then on
		bool contrastBeforeLight = false;
		bool recoveryStarted = false;
		bool recovered = false;
		bool climbed = true;
		bool gainUnderCeiling = true;
		bool neverRises = true;
		int final = 0;
		int projector = 0;
		std::wstring aboveNeed;
		std::wstring trace;

		std::wstring Where() const
		{
			return L"(" + std::to_wstring(options.fps) + L" fps, " + (options.lookahead ? L"look-ahead, " : L"reactive, ") +
				(options.dump ? L"dump) " : L"held) ");
		}
	};

	double MsOn(const TestClock& clock, LedDrive::Clock::time_point when)
	{
		return std::chrono::duration<double, std::milli>(when - clock.origin).count();
	}

	CutRun RunCompensateCut(const CutOptions& options)
	{
		CutRun run;
		run.options = options;
		auto projector = std::make_shared<FakeProjector>();
		projector->contrast = 50;
		LedDriveConfig config = Assisted();
		ApplyRatePreset(config, true);
		config.maxCutDip = 0.3;
		config.shapedRecovery = true;
		config.cutCntrDump = options.dump;
		config.cutCntrLevel = 55;   // below where the dim scene's CNTR sits, so a dump shows
		config.lookahead = options.lookahead;
		config.cntrPairPredict = options.pairPredict;
		ProjectorConfig port;       // the owner's defaults: led/cntr latency 80 ms, settle 300 ms, gap 200 ms
		port.protocol = "barco_ascii";
		port.terminator = Terminator::Cr;   // the fake projector's framing (with read-back off Auto sends the script's)
		if (options.legacyPort)
		{
			port.ackTimeoutMs = 60;
			port.minCommandGapMs = 0;
			port.readBack = true;
		}
		TestClock clock;
		LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); }, clock);
		Bind(clock, module, projector);
		module.Start();
		if (!clock.RunUntil([&] { return module.DrivingProjector(); }, 5000))
			return run;
		run.started = true;

		const double period = 1000.0 / options.fps;
		const double time0 = clock.Ms();
		const uint64_t cut = static_cast<uint64_t>(std::lround(10.0 * options.fps)) + 1;
		const uint64_t mark = cut - static_cast<uint64_t>(std::lround(options.fps));
		const uint64_t last = cut + static_cast<uint64_t>(std::lround(8.0 * options.fps));
		const size_t depth = static_cast<size_t>(std::ceil(0.5 * options.fps));
		auto rawFor = [&](uint64_t s) { return s < cut ? Frame(18.0, 100, 200) : Frame(120.0, 100, 235); };
		if (options.lookahead)
		{
			StatsBuilder builder(module.Config());
			for (uint64_t s = 1; s <= last + depth; ++s)
				module.SubmitPrediction(s, builder.Build(rawFor(s), time0 + s * period));
		}
		// The new scene (1% of the picture at white): the fade-up's gain ceiling, and
		// the software headroom left by the large-area share; with the LED at or above
		// `carried` (and the recovery bias) VP's gain alone compensates, so any CNTR
		// above 50 is above need.
		const double ceiling = std::pow(2.0, config.highlightToleranceStops);
		double large = std::max(0.0, std::min((0.01 - config.highlightAreaSmall) /
			(config.highlightAreaLarge - config.highlightAreaSmall), 1.0));
		large = large * large * (3.0 - 2.0 * large);
		const double headroom = std::pow(2.0, config.highlightToleranceStops * (1.0 - large));
		const double carried = (1.0 + config.recoveryBiasPct / 100.0) / headroom;

		size_t writesBefore = 0;
		int previous = 0;
		double previousDisplayed = 0.0;
		std::wstring rises;
		for (uint64_t s = 1; s <= last; ++s)
		{
			if (s == mark)
			{
				run.before = previous = module.AssistContrast();
				writesBefore = projector->Writes().size();
			}
			module.SubmitMeasurement(rawFor(s), s);
			const double t = time0 + s * period;
			clock.To(t);
			if (options.lookahead) module.SetLookaheadDepth(depth);
			const CompensationGain gain = module.BeginFrameAt(s, t, options.fps);
			clock.Settle();
			if (s < mark) continue;
			const int assist = module.AssistContrast();
			if (assist > previous)
			{
				run.neverRises = false;
				rises += L" " + std::to_wstring(previous) + L"->" + std::to_wstring(assist) + L"@" +
					std::to_wstring(static_cast<int>(std::lround(t - (time0 + cut * period)))) + L"ms";
			}
			previous = assist;
			if (s < cut) continue;
			if (module.Recovering())
			{
				run.recoveryStarted = true;
				if (gain.gain > ceiling + 1e-6) run.gainUnderCeiling = false;
				if (module.RecoveryDisplayed() < previousDisplayed - 1e-9) run.climbed = false;
				previousDisplayed = module.RecoveryDisplayed();
			}
			else if (run.recoveryStarted)
				run.recovered = true;
			const double fraction = module.ModelledLight() / 255.0;
			if (fraction >= carried + 1e-9 && assist > 50)
				run.aboveNeed += L" CNTR " + std::to_wstring(assist) + L" with the LED at " + std::to_wstring(fraction) +
					L" @" + std::to_wstring(static_cast<int>(std::lround(t - (time0 + cut * period)))) + L"ms";
		}
		run.final = module.AssistContrast();
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			run.projector = projector->contrast;
		}
		const auto writes = projector->Writes();
		const auto times = projector->WriteTimes();
		size_t firstContrast = writes.size(), firstLight = writes.size();
		run.trace = run.Where() + (rises.empty() ? L"" : L"rises:" + rises + L"; ") + L"CNTR after the cut (ms):";
		for (size_t i = writesBefore; i < writes.size(); ++i)
		{
			const bool contrast = writes[i].rfind(":CNTR", 0) == 0 && writes[i].find('?') == std::string::npos;
			const bool light = writes[i].rfind(":LPW1", 0) == 0 && writes[i].find('?') == std::string::npos;
			if (contrast)
			{
				const int value = std::stoi(writes[i].substr(5));
				run.after.push_back(value);
				run.trace += L" " + std::to_wstring(value) + L"@" +
					std::to_wstring(static_cast<int>(std::lround(MsOn(clock, times[i]) - (time0 + cut * period))));
				if (firstContrast == writes.size()) firstContrast = i;
			}
			if (light && firstLight == writes.size()) firstLight = i;
		}
		run.contrastBeforeLight = firstContrast < firstLight;
		run.trace += L" (writes " + std::to_wstring(writes.size()) + L":";
		for (size_t i = 0; i < writes.size() && i < 8; ++i)
			run.trace += L" " + std::wstring(writes[i].begin(), writes[i].end() - 1);
		run.trace += L")";
		module.Stop();
		return run;
	}

	// internal_script through the module at the real frame rate: a dark scene
	// with highlights (the script's CNTR 74), then a bright cut (its CNTR 50).
	struct ScriptCutRun
	{
		bool started = false;
		int before = 0;
		std::vector<int> after;
		int projector = 0;
		double cutMs = 0.0;
		double firstLandsMs = 1e300;
		std::wstring trace;
	};

	ScriptCutRun RunScriptCut(double fps, bool lookahead)
	{
		ScriptCutRun run;
		auto projector = std::make_shared<FakeProjector>();
		projector->contrast = 50;
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Script;
		config.scriptLookahead = lookahead;
		ProjectorConfig port;   // the owner's defaults
		port.protocol = "barco_ascii";
		port.terminator = Terminator::Cr;   // the fake projector's framing
		TestClock clock;
		LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); }, clock);
		Bind(clock, module, projector);
		module.Start();
		if (!clock.RunUntil([&] { return module.DrivingProjector(); }, 5000))
			return run;
		run.started = true;
		const double period = 1000.0 / fps;
		const double time0 = clock.Ms();
		const uint64_t cut = static_cast<uint64_t>(std::lround(30.0 * fps)) + 1;
		const uint64_t mark = cut - static_cast<uint64_t>(std::lround(fps));
		const uint64_t last = cut + static_cast<uint64_t>(std::lround(10.0 * fps));
		// Deep enough to cover measure + latency + settle and the next tick.
		const size_t depth = static_cast<size_t>(std::ceil(1.2 * fps));
		auto rawFor = [&](uint64_t s) { return s < cut ? Frame(18.0, 100, 200) : Frame(120.0, 100, 235); };
		if (lookahead)
		{
			StatsBuilder builder(module.Config());
			for (uint64_t s = 1; s <= last + depth; ++s)
				module.SubmitPrediction(s, builder.Build(rawFor(s), time0 + s * period));
		}
		run.cutMs = time0 + cut * period;
		size_t writesBefore = 0;
		for (uint64_t s = 1; s <= last; ++s)
		{
			if (s == mark)
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				run.before = projector->contrast;
				writesBefore = projector->writes.size();
			}
			module.SubmitMeasurement(rawFor(s), s);
			const double t = time0 + s * period;
			clock.To(t);
			if (lookahead) module.SetLookaheadDepth(depth);
			module.BeginFrameAt(s, t, fps);
			clock.Settle();
		}
		clock.Run(1000.0);
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			run.projector = projector->contrast;
		}
		const auto writes = projector->Writes();
		const auto times = projector->WriteTimes();
		run.trace = L"CNTR from a second before the cut (ms from the cut):";
		for (size_t i = writesBefore; i < writes.size(); ++i)
			if (writes[i].rfind(":CNTR", 0) == 0 && writes[i].find('?') == std::string::npos)
			{
				const int value = std::stoi(writes[i].substr(5));
				const double at = MsOn(clock, times[i]);
				if (run.after.empty()) run.firstLandsMs = at + port.cntrLatencyMs;
				run.after.push_back(value);
				run.trace += L" " + std::to_wstring(value) + L"@" + std::to_wstring(static_cast<int>(std::lround(at - run.cutMs)));
			}
		module.Stop();
		return run;
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
				// 24p frames, 45 APL per second (the fade is in time: fade_ms).
				for (int frame = 0; frame < 6; ++frame)
				{
					apl -= 1.875;
					stats = builder.Build(Frame(apl), now);
					now += 1000.0 / 24.0;
				}
				out = controller.Tick(stats, 250);   // six 24p frames
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

		TEST_METHOD(ModuleVeilEstimateFollowsHighlightsSlewedAndResetsOnACut)
		{
			// Delta 11: the veil estimate is 0 on a dark frame without
			// highlights; a bright patch (20% of the blocks at 235) moves it up
			// at shadow_lift_ansi_rate (1/s), not at once; a cut sets it at once.
			LedDriveConfig config = Compensate();
			config.mode = Mode::LogOnly;
			config.shadowLiftAnsi = 0.3;
			ProjectorConfig port;
			auto projector = std::make_shared<FakeProjector>();
			LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); });
			double time = 0.0;
			auto frames = [&](const RawMeasurement& raw, int count)
			{
				for (int i = 0; i < count; ++i)
				{
					module.SubmitMeasurement(raw);
					time += 1000.0 / 60.0;
					module.BeginFrameAt(0, time);
				}
				return module.Veil();
			};
			const double dark = frames(Frame(30.0), 30);
			Assert::AreEqual(0.0, dark, 1e-9, L"no highlight: no veil");
			const double early = frames(Frame(30.0, 2000, 235), 6);
			const double late = frames(Frame(30.0, 2000, 235), 120);
			Assert::IsTrue(early < 0.2, (L"slewed: after 0.1 s " + std::to_wstring(early)).c_str());
			Assert::IsTrue(late > 0.95, (L"after 2 s " + std::to_wstring(late)).c_str());
			// Back to dark, then a cut to a bright scene: the cut sets it at once.
			frames(Frame(30.0), 120);
			const double before = module.Veil();
			const double atCut = frames(Frame(200.0, 2000, 235), 1);
			Assert::IsTrue(before < 0.05 && atCut > 0.9, (L"a cut resets: " + std::to_wstring(before) + L" -> " +
				std::to_wstring(atCut)).c_str());
		}

		TEST_METHOD(PictureOnlyNeverOpensThePortAndKeepsGainOne)
		{
			// Delta 12 (owner: the port stays free for the Python script):
			// picture_only never opens the RS-232 port and writes nothing under
			// any statistic (dark, bright, cuts); the gain is exactly 1 and the
			// veil estimate still follows the highlights (the picture terms).
			auto projector = std::make_shared<FakeProjector>();
			LedDriveConfig config = Compensate();
			config.mode = Mode::PictureOnly;
			config.shadowLiftAnsi = 0.3;
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			Assert::AreEqual(std::string("none"), EffectiveProtocol(port, config));
			LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			double time = 0.0;
			double veil = 0.0;
			for (int frame = 0; frame < 120; ++frame)
			{
				const RawMeasurement raw = frame < 40 ? Frame(18.0) : frame < 80 ? Frame(30.0, 2000, 235) : Frame(200.0 - frame, 3000, 250);
				module.SubmitMeasurement(raw);
				time += 1000.0 / 60.0;
				const CompensationGain gain = module.BeginFrameAt(0, time);
				Assert::IsFalse(gain.active, L"picture_only never applies a gain");
				Assert::AreEqual(1.0, gain.gain, 1e-12);
				veil = std::max(veil, gain.veil);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(600));   // ticks of the module's thread
			module.Stop();
			Assert::AreEqual(0, projector->opens, L"picture_only must not open the port");
			Assert::IsTrue(projector->Writes().empty(), L"picture_only must not write to the port");
			Assert::IsFalse(module.DrivingProjector());
			Assert::IsTrue(veil > 0.1, (L"the veil estimate follows the highlights: " + std::to_wstring(veil)).c_str());
		}

		TEST_METHOD(CompensateDrivesTheLedWithAdaptiveGammaOff)
		{
			// Audit 2026-10-14b blocker 2: adaptive gamma off keeps the picture's
			// gain at 1 but compensate still dims the LED.
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Compensate();
			config.adaptiveGamma = false;
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			port.ackTimeoutMs = 60;
			port.minCommandGapMs = 0;
			port.readBack = true;
			port.ledLatencyMs = 80;
			port.ledSettleMs = 0;
			TestClock clock;
			LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }), L"compensate started");
			double time = 0.0;
			bool anyGain = false;
			for (int frame = 0; frame < 60; ++frame)
			{
				module.SubmitMeasurement(Frame(18.0));
				time += 1000.0 / 60.0;
				const CompensationGain gain = BeginFrameOn(clock, module, 0, time);
				anyGain = anyGain || (gain.active && std::fabs(gain.gain - 1.0) > 1e-9);
				clock.Settle();
			}
			Assert::IsTrue(WaitFor([&] { return projector->light < 255; }), L"the LED is dimmed with adaptive gamma off");
			Assert::IsFalse(anyGain, L"the picture's gain stays at 1 with adaptive gamma off");
		}

		TEST_METHOD(PictureOnlySwitchFromCompensateRestoresAndReleasesThePort)
		{
			// Delta 12: compensate dims the LED; a switch to picture_only is not
			// applied live (the protocol changes to none), so the renderer
			// rebuilds the module: the compensate module's exit restores full
			// light and closes the port, and the picture_only module never
			// opens it.
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
			LedDriveConfig picture = config;
			picture.mode = Mode::PictureOnly;
			{
				TestClock clock;
				LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }), L"compensate started");
				double time = 0.0;
				for (int frame = 0; frame < 60; ++frame)
				{
					module.SubmitMeasurement(Frame(18.0));
					time += 1000.0 / 60.0;
					BeginFrameOn(clock, module, 0, time);
					clock.Settle();
				}
				Assert::IsTrue(WaitFor([&] { return projector->light < 255; }), L"compensate dimmed the LED");
				Assert::IsFalse(module.Reconfigure(port, picture), L"picture_only needs a new module (no port)");
			}
			Assert::AreEqual(255, projector->light, L"the compensate module's exit restored full light");
			const int opens = projector->opens;
			const size_t writes = projector->Writes().size();
			{
				LedDriveModule module(port, picture, [projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				double time = 0.0;
				for (int frame = 0; frame < 60; ++frame)
				{
					module.SubmitMeasurement(Frame(frame % 2 ? 18.0 : 200.0, 2000, 240));
					time += 1000.0 / 60.0;
					Assert::IsFalse(module.BeginFrameAt(0, time).active);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(400));
			}
			Assert::AreEqual(opens, projector->opens, L"picture_only did not open the port");
			Assert::AreEqual(writes, projector->Writes().size(), L"picture_only wrote nothing");
		}

		TEST_METHOD(CommandSourceSetsTheModeAndSwitches)
		{
			// The LED core (owner 2026-10-14): command_source picks the mode;
			// adaptive_gamma is off unless named; older `mode` files unchanged.
			auto read = [](const std::string& body)
			{
				char directory[MAX_PATH]{}, file[MAX_PATH]{};
				GetTempPathA(MAX_PATH, directory);
				GetTempFileNameA(directory, "vps", 0, file);
				std::ofstream(file, std::ios::trunc) << "[led_drive.Default]\nenabled: true\n" << body;
				ConfigFile config;
				Assert::IsTrue(config.Load(file));
				std::string error;
				Assert::IsTrue(Validate(config, error), std::wstring(error.begin(), error.end()).c_str());
				const LedDriveConfig result = ReadLedDriveConfig(config);
				DeleteFileA(file);
				return result;
			};
			// The Python source was removed (owner 2026-10-15): an older file naming
			// it runs VP's copy of the script, its retired keys accepted and ignored.
			const LedDriveConfig python = read("command_source: python\nlisten_script: C:\\scripts\\main_vp.py\npython: python.exe\n"
				"listen_port: 47811\nlisten_log: true\npython_drives_port: false\npython_feed: true\nshadow_lift_ansi: 0.3\n");
			Assert::IsTrue(python.mode == Mode::Script && python.pythonSourceRetired && !python.adaptiveGamma,
				L"command_source: python runs internal_script");
			const LedDriveConfig listen = read("mode: listen\nlisten_script: main_vp.py\n");
			Assert::IsTrue(listen.mode == Mode::Script && listen.pythonSourceRetired, L"mode: listen runs mode: script");
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			Assert::AreEqual(std::string("barco_ascii"), EffectiveProtocol(port, python), L"VP drives the port itself");
			Assert::IsFalse(read("command_source: internal_script\n").pythonSourceRetired);
			{
				// script_pacing (owner 2026-10-15): the default; read with its two times.
				Assert::IsTrue(python.scriptPacing && python.scriptMeasureMs == 150 && python.scriptEchoMs == 30);
				Assert::IsFalse(read("command_source: internal_script\nscript_pacing: tick\n").scriptPacing);
				const LedDriveConfig paced = read("command_source: internal_script\nscript_pacing: script\n"
					"script_measure_ms: 120\nscript_echo_ms: 25\n");
				Assert::IsTrue(paced.scriptPacing && paced.scriptMeasureMs == 120 && paced.scriptEchoMs == 25);
				// One tick in the script's timing: screenshot 120, LPW1, two echoes, its 200 ms
				// sleep, a fast step after 100 ms more, the last CNTR after 200 more.
				ControllerOutput out;
				out.lightCommand = 200;
				out.contrastSchedule = { { 300, 57 }, { 500, 56 } };
				out.durationMs = 500;
				std::vector<ScriptSend> sends;
				const int length = PaceScriptTick(out, 120, 25, sends);
				Assert::AreEqual(size_t(3), sends.size());
				Assert::IsTrue(sends[0].light && sends[0].offsetMs == 120 && sends[0].value == 200);
				Assert::IsTrue(!sends[1].light && sends[1].offsetMs == 120 + 50 + 300 && sends[1].value == 57);
				Assert::IsTrue(!sends[2].light && sends[2].offsetMs == 120 + 50 + 500 + 25 && sends[2].value == 56);
				Assert::AreEqual(120 + 50 + 500 + 50, length, L"the next loop starts after the last echo");
				Assert::AreEqual(120, PaceScriptTick(ControllerOutput{}, 120, 25, sends), L"a quiet loop: its screenshot only");
				Assert::IsTrue(sends.empty());
			}
			const LedDriveConfig internal = read("command_source: internal_script\nadaptive_gamma: true\nlookahead: false\n");
			Assert::IsTrue(internal.mode == Mode::Script && internal.adaptiveGamma && internal.scriptSmoothing && !internal.lookahead,
				L"internal script with adaptive gamma: the steps are eased");
			Assert::IsTrue(read("command_source: compensate\n").mode == Mode::Compensate);
			Assert::IsTrue(read("command_source: none\n").mode == Mode::LogOnly);
			Assert::IsTrue(read("command_source: none\nadaptive_gamma: true\n").mode == Mode::PictureOnly,
				L"no LED commands with adaptive gamma: the picture terms only");
			const LedDriveConfig legacy = read("mode: compensate\n");
			Assert::IsTrue(legacy.mode == Mode::Compensate && legacy.adaptiveGamma && legacy.lookahead,
				L"an older mode-only configuration behaves as before");
			Assert::IsFalse(read("mode: compensate\nadaptive_gamma: false\n").adaptiveGamma);
			Assert::IsFalse(read("").adaptiveGamma, L"a new file (no source, no mode): adaptive gamma off (audit 2026-10-14c)");
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
				TestClock clock;
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				Assert::AreEqual(1.0, module.CurrentGain().gain, 1e-9);
				module.SubmitMeasurement(Frame(18.0));
				CompensationGain gain;
				for (int frame = 1; frame <= 30; ++frame)
				{
					gain = BeginFrameOn(clock, module, 0, frame * 1000.0 / 60.0);
					// Let the driver send before the next frame looks.
					clock.Settle();
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

			// Hovering at a step boundary: down to the covering value once, then
			// quiet (the margin is on the way up).
			pairing.Reset(50);
			const double edge = pairing.Light(60);
			Assert::AreEqual(61, pairing.Update(edge * 1.001, 1.0));
			int commands = 0;
			for (int i = 0; i < 100; ++i)
				commands += pairing.Update(edge * (i % 2 ? 1.001 : 0.999), 1.0) >= 0 ? 1 : 0;
			Assert::AreEqual(1, commands, L"one step down to 60, then no chatter at the boundary");
			Assert::AreEqual(60, pairing.Contrast());
			Assert::AreEqual(59, pairing.Update(pairing.Light(59) * 0.999, 1.0),
				L"down at once to the smallest value that covers the need");
			Assert::AreEqual(-1, pairing.Update(pairing.Light(59) * 1.01, 1.0), L"up only past the 2% margin");
			Assert::AreEqual(60, pairing.Update(pairing.Light(59) * 1.03, 1.0));
			// Audit stage7-step7c1: never left above what is needed.
			pairing.Reset(51);
			Assert::AreEqual(50, pairing.Update(0.99, 1.0), L"a need of 0.99 at CNTR 51 sends 50");
			Assert::AreEqual(-1, pairing.Update(1.0, 2.0), L"no need left: stays at 50");

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
				Assert::IsTrue(vp <= headroom * 1.02 + 1e-9,
					(L"VP gain over its headroom (plus the 2% up-margin) at step " + std::to_wstring(i)).c_str());
				Assert::AreEqual(1.0, led * pairing.Light(pairing.Contrast()) * vp, 1e-12);
			}
			Assert::IsTrue(previous > 50 && commands <= previous - 50, L"at most one command per whole value");
		}

		// Rule test (owner: CNTR left above the need is a visible defect): over a need that
		// rises then falls (the LED dimming into a dark scene and back), after every update the
		// CNTR value is never above the smallest whole value covering the need; on the way down
		// it equals it exactly; on the way up it lags only inside the 2% anti-chatter margin.
		TEST_METHOD(CntrFollowsARisingThenFallingNeedAtTheSmallestCoveringValue)
		{
			const double eotf = 2.41;
			const int maxUsable = 74;
			CntrPairing pairing;
			pairing.Configure(eotf, maxUsable, true, true);
			const double headroom = 1.25;
			auto smallestCovering = [&](double need) {
				const double required = need > 1.0 ? need : 1.0;
				int value = 50;
				while (value < maxUsable && pairing.Light(value) < required - 1e-12) ++value;
				return value;
			};
			std::vector<double> needs;
			for (int i = 0; i <= 300; ++i) needs.push_back(0.8 + 2.4 * i / 300.0);    // rises to 3.2 (beyond the top)
			for (int i = 300; i >= 0; --i) needs.push_back(0.8 + 2.4 * i / 300.0);    // falls back
			int peak = 50, rises = 0, falls = 0;
			double previousNeed = 0.0;
			for (size_t step = 0; step < needs.size(); ++step)
			{
				const double compensation = needs[step] * headroom;   // the LED's light factor
				pairing.Update(compensation, headroom);
				const double need = compensation / headroom;
				const int covering = smallestCovering(need);
				const int contrast = pairing.Contrast();
				const std::wstring at = L" at step " + std::to_wstring(step) + L" (need " + std::to_wstring(need) +
					L", CNTR " + std::to_wstring(contrast) + L", covering " + std::to_wstring(covering) + L")";
				Assert::IsTrue(contrast <= covering, (L"CNTR left above the smallest covering value" + at).c_str());
				if (need < previousNeed)
				{
					Assert::AreEqual(covering, contrast, (L"falling need: CNTR exactly at the smallest covering value" + at).c_str());
					++falls;
				}
				else
				{
					// Rising: at most the anti-chatter margin below (VP's gain covers the 2%).
					Assert::IsTrue(contrast == covering || need <= pairing.Light(contrast) * 1.02 + 1e-12,
						(L"rising need: CNTR lags beyond the 2% margin" + at).c_str());
					++rises;
				}
				peak = (std::max)(peak, contrast);
				previousNeed = need;
			}
			Assert::AreEqual(maxUsable, peak, L"the need reached the top usable value");
			Assert::AreEqual(50, pairing.Contrast(), L"back to 50 once nothing is needed");
			Assert::IsTrue(rises > 100 && falls > 100, L"both directions exercised");
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
				TestClock clock;
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
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
					gain = BeginFrameOn(clock, module, 0, time);
					clock.Settle();
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
				TestClock clock;
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				for (int frame = 1; frame <= 300; ++frame)
				{
					module.SubmitMeasurement(Frame(18.0, 100, 230));
					BeginFrameOn(clock, module, 0, frame * 1000.0 / 60.0);
					clock.Settle();
				}
				Assert::IsTrue(projector->light < 255);
				Assert::AreEqual(50, module.AssistContrast());
			}
			for (const auto& write : projector->Writes())
				Assert::IsFalse(write.rfind(":CNTR", 0) == 0 && write != ":CNTR?\r",
					L"cntr_assist off never touches the projector's contrast");
		}

		// 7c-2 (planner's answer 3): the scene floor keeps a full dump's
		// fade-up start, f x 2^tolerance, at max_cut_dip; the higher of it
		// and led_floor applies.
		TEST_METHOD(MaxCutDipSetsTheSceneFloor)
		{
			auto settle = [](double dip, int ledFloor)
			{
				LedDriveConfig config = Compensate();
				config.maxCutDip = dip;
				config.ledFloor = config.ledFloorReactive = ledFloor;
				StatsBuilder builder(config);
				CompensateController controller(config);
				double now = 0.0;
				ControllerOutput out;
				for (int tick = 0; tick < 30; ++tick)
					out = controller.Tick(Play(builder, now, 100.0, Frame(18.0)), 100.0, nullptr, true, true);
				return out.lightTarget;
			};
			const int half = settle(0.5, 60);
			Assert::AreEqual(102, half, L"0.5 -> about 0.40 of the ceiling's flux, level 102");
			const double start = half / 255.0 * std::pow(2.0, LedDriveConfig().highlightToleranceStops);
			Assert::IsTrue(start >= 0.5, L"a full dump from that floor starts at max_cut_dip or above");
			Assert::IsTrue(settle(0.3, 60) < half, L"a lower max_cut_dip lowers the floor");
			Assert::AreEqual(120, settle(0.5, 120), L"led_floor above it still applies");
			Assert::AreEqual(60, settle(0.0, 60), L"0 = no floor from it");
		}

		// 7c-2 (planner's answer 1): the dump is to 50, or just enough CNTR to
		// keep the fade-up's start at max_cut_dip; never above the current.
		TEST_METHOD(CntrDumpIsFullOrJustEnough)
		{
			CntrPairing pairing;
			pairing.Configure(2.41, 74, true, true);
			pairing.Reset(70);
			Assert::AreEqual(50, pairing.DumpTarget(0.5, 1.26, 0.5), L"0.5 x 1.26 >= 0.5: full dump");
			const int partial = pairing.DumpTarget(0.3, 1.26, 0.5);
			Assert::IsTrue(partial > 50 && partial < 70, L"a full dump would start too dark: partial");
			Assert::IsTrue(0.3 * pairing.Light(partial) * 1.26 >= 0.5 &&
				0.3 * pairing.Light(partial - 1) * 1.26 < 0.5, L"the smallest value that holds max_cut_dip");
			Assert::AreEqual(50, pairing.DumpTarget(0.1, 1.26, 0.0), L"max_cut_dip 0: always full");
			pairing.Reset(52);
			Assert::AreEqual(52, pairing.DumpTarget(0.1, 1.26, 0.5), L"never above the current value");
			// Owner correction: the cut goes to cut_cntr_level (59), not 50.
			pairing.Reset(70);
			Assert::AreEqual(59, pairing.DumpTarget(0.5, 1.26, 0.5, 59), L"to the owner's level, not 50");
			const int held = pairing.DumpTarget(0.25, 1.26, 0.5, 59);
			Assert::IsTrue(held > 59 && 0.25 * pairing.Light(held) * 1.26 >= 0.5,
				L"higher than the level only to hold max_cut_dip");
			pairing.Reset(55);
			Assert::AreEqual(55, pairing.DumpTarget(0.5, 1.26, 0.5, 59), L"already below the level: kept");
			// During the fade-up CNTR never rises, and the need carries the bias.
			pairing.Reset(56);
			pairing.SetCeiling(56);
			Assert::AreEqual(-1, pairing.Update(10.0, 1.0), L"no rise above the dump point");
			pairing.SetBias(0.03);
			const double light55 = pairing.Light(55);
			Assert::AreEqual(-1, pairing.Update(light55 / 1.02, 1.0),
				L"the bias keeps the residual a step higher (bright side)");
			Assert::AreEqual(55, pairing.Update(light55 / 1.04, 1.0));
		}

		// 7c-2 (planner's answer 2, policy 9 "gain-shaped recovery"): with a
		// modelled LED step response, the displayed brightness follows the
		// rate-law ramp - monotonic, never above the ramp or the LED x ceiling
		// - and the gain never exceeds the ceiling; it lands on 1.
		TEST_METHOD(CutRecoveryFollowsTheRampUnderTheLed)
		{
			for (const bool slow : { false, true })
			{
				LedDriveConfig config;
				if (slow)
				{
					// As slow as the script's LED, to check the shape holds.
					config.recoveryRateGain = 0.25;
					config.recoveryRateMin = 2.5;
					config.recoveryRateMax = 17.5;
				}
				CutRecovery recovery;
				recovery.Configure(config, FluxModel());
				SettleModel led(300.0, 0.4);
				const double ceiling = 1.26;
				recovery.Start(led.Value(), 1.0, ceiling);
				Assert::IsTrue(recovery.Active());
				Assert::AreEqual(0.4 * ceiling, recovery.Displayed(), 0.005, L"starts at LED x ceiling");
				const double frame = 1000.0 / 60.0;
				double previous = 0.0;
				bool landed = false;
				for (int i = 0; i < 60 * 40 && recovery.Active(); ++i)
				{
					if (i == 5) led.Land(1.0); // the LED's jump lands after its dead time
					led.Step(frame);
					const double limit = recovery.Step(frame, led.Value(), 1.0);
					const double gain = std::min(1.0 / led.Value(), limit);
					const double displayed = led.Value() * gain;
					Assert::IsTrue(gain <= ceiling + 1e-9, L"never above the gain ceiling");
					Assert::IsTrue(displayed >= previous - 1e-9, L"the fade-up never goes backwards");
					Assert::IsTrue(displayed <= recovery.Displayed() + 1e-9 || !recovery.Active(),
						L"never above the ramp: the LED's step never shows");
					if (led.Value() * ceiling >= recovery.Displayed() + 1e-9 && recovery.Active())
						Assert::AreEqual(recovery.Displayed(), displayed, 1e-9, L"follows the ramp exactly");
					previous = displayed;
					landed = !recovery.Active();
				}
				Assert::IsTrue(landed, L"the fade-up ends");
				Assert::AreEqual(1e9, recovery.Step(frame, led.Value(), 1.0), 1.0, L"no limit once over");
			}
		}

		// The module: a bright cut after a CNTR-assisted dark scene dumps CNTR
		// once, the fade-up limits the gain to the new scene's ceiling, CNTR
		// never rises during it, and it ends with the gain back to normal.
		// cntr-cut fix 2026-10-18: at the real frame rate (the module's clock
		// moves with the frames). It was written against frames fed much faster
		// than real time, where the sustained highlight never caught up with the
		// new scene during the fade-up; at the real rate CNTR went 55 51 50 54 52
		// 51 50 (docs/agent-reports/cntr-bright-cut.md).
		TEST_METHOD(ModuleBrightCutDumpsContrastAndFadesUp)
		{
			for (const bool dump : { true, false })
			{
				CutOptions options;
				options.fps = 60.0;
				options.dump = dump;
				options.legacyPort = true;
				const CutRun run = RunCompensateCut(options);
				Logger::WriteMessage(run.trace.c_str());
				Assert::IsTrue(run.started, run.trace.c_str());
				Assert::IsTrue(run.before > 50 && run.before <= 59,
					L"the dark scene raised CNTR, with highlights no higher than dmd_clip_cntr (59)");
				Assert::IsTrue(run.recoveryStarted, L"the fade-up starts on the cut");
				Assert::IsTrue(run.gainUnderCeiling, L"the gain goes no higher than the new scene's ceiling");
				Assert::IsTrue(run.climbed, L"the fade-up only climbs");
				Assert::IsTrue(run.recovered, L"the fade-up ends");
				Assert::IsFalse(run.after.empty());
				Assert::IsTrue(run.neverRises, (L"CNTR never rises after the cut, " + run.trace).c_str());
				Assert::IsTrue(run.aboveNeed.empty(), (L"CNTR never above need: " + run.aboveNeed + L" " + run.trace).c_str());
				if (dump)
				{
					Assert::AreEqual(55, run.after.front(), L"one command to cut_cntr_level, never to 50 because of the cut");
					Assert::IsTrue(run.contrastBeforeLight, L"CNTR first, then the LPW1 a command gap later");
				}
				else
					Assert::IsTrue(run.after.front() > 55 && run.after.size() > 1, L"held through the cut, descending paired");
				Assert::AreEqual(50, run.final, L"back to 50 in the bright scene");
			}
		}

		// cntr-cut fix 2026-10-18 (owner rule: CNTR never above need): a bright
		// cut at the real frame rate, 23.976 / 24 / 50 / 59.94, reactive and with
		// the look-ahead (a seen cut), dumped and held, with the owner's port
		// defaults (200 ms command gap, no read-back). CNTR never rises from the
		// cut on, is 50 whenever the LED alone carries the new scene, and the
		// fade-up runs as designed.
		TEST_METHOD(BrightCutContrastNeverRisesAtRealFrameRates)
		{
			for (const double fps : { 24000.0 / 1001.0, 24.0, 50.0, 60000.0 / 1001.0 })
				for (const bool lookahead : { false, true })
					for (const bool dump : { true, false })
					{
						CutOptions options;
						options.fps = fps;
						options.dump = dump;
						options.lookahead = lookahead;
						const CutRun run = RunCompensateCut(options);
						const std::wstring where = run.Where();
						Logger::WriteMessage(run.trace.c_str());
						Assert::IsTrue(run.started, (where + run.trace).c_str());
						Assert::IsFalse(run.after.empty(), (L"CNTR commanded, " + where + run.trace).c_str());
						Assert::IsTrue(run.before > 50, (L"the dark scene raised CNTR, " + where).c_str());
						Assert::IsTrue(run.recoveryStarted && run.recovered, (L"the fade-up runs and ends, " + where).c_str());
						Assert::IsTrue(run.climbed, (L"the fade-up only climbs, " + where).c_str());
						Assert::IsTrue(run.gainUnderCeiling, (L"the gain stays under the ceiling, " + where).c_str());
						Assert::IsTrue(run.neverRises, (L"CNTR never rises after the cut, " + where + run.trace).c_str());
						Assert::IsTrue(run.aboveNeed.empty(),
							(L"CNTR never above need, " + where + run.aboveNeed + L" " + run.trace).c_str());
						Assert::AreEqual(50, run.final, (L"back to 50, " + where).c_str());
						Assert::AreEqual(50, run.projector, (L"the projector at 50, " + where).c_str());
						// The cut's CNTR: cut_cntr_level, or lower when the driver's next slot
						// already has the paired descent (the 200 ms gap keeps only the newest;
						// with the look-ahead CNTR may also descend a little before the cut,
						// paired with the LED's early ramp).
						if (dump)
						{
							const auto dumped = std::find_if(run.after.begin(), run.after.end(), [](int c) { return c <= 55; });
							Assert::IsTrue(dumped != run.after.end(),
								(L"the cut's CNTR goes to cut_cntr_level or below, " + where + run.trace).c_str());
						}
					}
		}

		// cntr_pair_predict off: the pairing as before the fix (the vpHeadroom of
		// the sustained highlight, the dump point as the only ceiling), which at
		// the real frame rate lets CNTR rise again during the fade-up.
		TEST_METHOD(BrightCutPairPredictOffIsThePreviousPairing)
		{
			for (const double fps : { 24.0, 60.0 })
			{
				CutOptions options;
				options.fps = fps;
				options.pairPredict = false;
				options.legacyPort = true;   // every CNTR the pairing asks for is written
				const CutRun run = RunCompensateCut(options);
				Assert::IsTrue(run.started && run.recovered, run.trace.c_str());
				Assert::IsFalse(run.neverRises, (L"off: CNTR rises again during the fade-up, " + run.trace).c_str());
				Assert::AreEqual(50, run.final);
			}
		}

		// internal_script (the owner's script as intent), with and without
		// script_lookahead, at the four rates: after a bright cut CNTR goes down
		// to the script's target for the new scene (50) and never rises again;
		// with the look-ahead it goes there in one command, landing by the cut's
		// first frame.
		TEST_METHOD(ScriptBrightCutContrastNeverRisesAtRealFrameRates)
		{
			for (const double fps : { 24000.0 / 1001.0, 24.0, 50.0, 60000.0 / 1001.0 })
				for (const bool lookahead : { false, true })
				{
					const ScriptCutRun run = RunScriptCut(fps, lookahead);
					Logger::WriteMessage(run.trace.c_str());
					const std::wstring where = L"(" + std::to_wstring(fps) + L" fps, script_lookahead " +
						(lookahead ? L"on) " : L"off) ");
					Assert::IsTrue(run.started, (where + run.trace).c_str());
					Assert::AreEqual(74, run.before, (L"the dark scene's script target, " + where + run.trace).c_str());
					Assert::IsFalse(run.after.empty(), (L"CNTR commanded after the cut, " + where).c_str());
					for (size_t i = 1; i < run.after.size(); ++i)
						Assert::IsTrue(run.after[i] <= run.after[i - 1], (L"CNTR never rises, " + where + run.trace).c_str());
					Assert::AreEqual(50, run.after.back(), (L"the script's target for the bright scene, " + where).c_str());
					Assert::AreEqual(50, run.projector, (L"the projector at 50, " + where).c_str());
					if (lookahead)
					{
						Assert::AreEqual(50, run.after.front(), (L"one command to the target, " + where + run.trace).c_str());
						Assert::IsTrue(run.firstLandsMs <= run.cutMs + 1000.0 / fps + 1e-6,
							(L"landing by the cut's first frame, " + where + run.trace).c_str());
					}
				}
		}

		// 7c-3 (policy 3.5; owner's black-frames note): on black with a bright
		// scene in the look-ahead the LED goes at once to black_prelift_max
		// (never further while black) and the controller asks for CNTR to be
		// pre-positioned; the cut then jumps to the scene's level. 255 = the
		// behaviour before 7c-3 (early ramp at the scene rate, no prelift).
		TEST_METHOD(ExitFromBlackPreliftsCappedThenJumpsOnTheCut)
		{
			for (const int cap : { 120, 255 })
			{
				LedDriveConfig config = Compensate();
				config.blackPreliftMax = cap;
				config.ledFloorBlack = 60;
				StatsBuilder black(config), bright(config);
				CompensateController controller(config);
				controller.Reset(60);
				double now = 0.0, ahead = 0.0;
				Lookahead look;
				look.stats = Play(bright, ahead, 100.0, Frame(120.0));
				look.cutAhead = true;
				look.framesAhead = 4;
				ControllerOutput out = controller.Tick(Play(black, now, 1000.0 / 60.0, Frame(16.0)), 1000.0 / 60.0,
					&look, true, true);
				if (cap == 120)
				{
					Assert::IsTrue(out.preposition, L"bright scene ahead on black: pre-position");
					Assert::AreEqual(120, out.lightCommand, L"one command to black_prelift_max");
					Assert::IsTrue(out.jump);
					for (int tick = 0; tick < 10; ++tick)
						out = controller.Tick(Play(black, now, 1000.0 / 60.0, Frame(16.0)), 1000.0 / 60.0, &look, true, true);
					Assert::AreEqual(120, controller.Light(), L"no further while the picture is still black");
				}
				else
				{
					Assert::IsFalse(out.preposition, L"255: no pre-positioning (before 7c-3)");
					Assert::IsTrue(out.lightCommand < 120 && !out.jump, L"255: the early ramp at the scene rate");
				}
				// The cut reaches the frame the LED lands on: the full jump.
				FrameStats cut = Play(black, now, 1000.0 / 60.0, Frame(120.0));
				Assert::IsTrue(cut.cut);
				out = controller.Tick(cut, 1000.0 / 60.0, &look, true, true);
				Assert::AreEqual(255, controller.Light(), L"the cut jumps to the scene's level");
				Assert::IsTrue(out.brightCut && !out.preposition);
			}
		}

		// Audit stage7-step7c, Should fix 2: no pre-lift during a fade's gain
		// hold unless the picture is truly black.
		TEST_METHOD(NoPreliftWhileAFadeHoldsTheGainUnlessBlack)
		{
			LedDriveConfig config = Compensate();
			config.blackPreliftMax = 120;
			StatsBuilder bright(config);
			double ahead = 0.0;
			Lookahead look;
			look.stats = Play(bright, ahead, 100.0, Frame(120.0));
			look.cutAhead = true;
			FrameStats fading;
			fading.apl = fading.sustainedApl = 18.0;
			fading.highlight = fading.sustainedHighlight = fading.maximum = 25.0;
			fading.fade = true;
			CompensateController dark(config);
			dark.Reset(60);
			Assert::IsFalse(dark.Tick(fading, 16.7, &look, true, true).preposition,
				L"a dark, not black, picture still fading: no pre-lift (the gain is held)");
			FrameStats black = fading;
			black.apl = black.sustainedApl = 16.0;
			black.highlight = black.sustainedHighlight = black.maximum = 16.0;
			CompensateController blackController(config);
			blackController.Reset(60);
			Assert::IsTrue(blackController.Tick(black, 16.7, &look, true, true).preposition,
				L"truly black: gain x black = black, so the pre-lift is invisible");
		}

		TEST_METHOD(CntrHoldRaisesAtOnceUntilReleased)
		{
			CntrPairing pairing;
			pairing.Configure(2.41, 74, true, true);
			pairing.SetHold(59);
			Assert::AreEqual(59, pairing.Update(1.0, 1e9), L"raised to the hold at once (no up-margin)");
			Assert::AreEqual(-1, pairing.Update(0.5, 1e9), L"held while the need is nil");
			pairing.SetHold(-1);
			Assert::AreEqual(50, pairing.Update(0.5, 1e9), L"released: back to what the pairing needs");
		}

		// Post-merge audit 2026-10-17 (C2, cntr_pair_predict): a compensate simulation -
		// LPW1 dead time, the LED's settle and the CNTR's own latency, one LED command per
		// frame. Each CNTR sent is checked when it lands against the smallest whole value
		// covering the need for the LED then. Software first with no VP headroom, so the
		// need is the whole compensation.
		static void SimulateCntrLandings(bool predict, bool rising, int& above, int& checked)
		{
			const double frameMs = 1000.0 / 60.0;
			const double ledLatencyMs = 5 * frameMs, settleMs = 100.0, cntrLatencyMs = 12 * frameMs;
			auto target = [rising](int k) {
				const double p = (std::min)(1.0, k / 120.0);
				return rising ? 0.5 + 0.5 * p : 1.0 - 0.5 * p;
			};
			SettleModel led(settleMs, target(0));
			CntrPairing pairing;
			pairing.Configure(2.41, 74, true, true);
			for (int i = 0; i < 4; ++i) pairing.Update(1.0 / led.Value(), 1.0);   // settled on the start
			auto covering = [&](double ledFraction) {
				const double need = (std::max)(1.0, 1.0 / ledFraction);
				int value = 50;
				while (value < 74 && pairing.Light(value) < need - 1e-12) ++value;
				return value;
			};
			std::vector<std::pair<double, double>> lpw1;   // (lands at ms, fraction)
			std::vector<std::pair<double, int>> cntr;      // (lands at ms, value)
			above = checked = 0;
			for (int k = 1; k <= 300; ++k)
			{
				const double now = k * frameMs;
				led.Step(frameMs);
				while (!lpw1.empty() && lpw1.front().first <= now + 1e-6)
				{
					led.Land(lpw1.front().second);
					lpw1.erase(lpw1.begin());
				}
				for (auto it = cntr.begin(); it != cntr.end();)
				{
					if (it->first > now + 1e-6) { ++it; continue; }
					++checked;
					if (it->second > covering(led.Value())) ++above;
					it = cntr.erase(it);
				}
				// This frame's LPW1, then the pairing (the module's order: LPW1 before CNTR).
				const double next = target(k);
				std::vector<std::pair<double, double>> landings;
				for (const auto& c : lpw1) landings.push_back({ c.first - now, c.second });
				landings.push_back({ ledLatencyMs, next });
				const double paired = CntrPairLedFraction(predict, led, landings, cntrLatencyMs);
				const int command = pairing.Update(1.0 / paired, 1.0);
				if (command >= 0) cntr.push_back({ now + cntrLatencyMs, command });
				lpw1.push_back({ now + ledLatencyMs, next });
			}
		}

		TEST_METHOD(CntrPairPredictNeverLandsAboveTheNeed)
		{
			int above = 0, checked = 0;
			SimulateCntrLandings(true, true, above, checked);
			Assert::IsTrue(checked >= 5, L"CNTR steps down while the LED rises");
			Assert::AreEqual(0, above, L"on, rising LED: never above the need when it lands");
			SimulateCntrLandings(true, false, above, checked);
			Assert::IsTrue(checked >= 5, L"CNTR steps up while the LED falls");
			Assert::AreEqual(0, above, L"on, falling LED: never above the need when it lands");
			// Off (the old pairing against the LED now): a rising LED lands CNTR above the need.
			SimulateCntrLandings(false, true, above, checked);
			Assert::IsTrue(above > 0, L"off: the defect the switch fixes");
			const LedDriveConfig defaults;
			Assert::IsTrue(defaults.cntrPairPredict, L"cntr_pair_predict on by default");
		}

		// Post-merge audit 2026-10-17 (B6): the limit (cut_lead window) caps the
		// pre-position hold; before, the hold overrode it.
		TEST_METHOD(CntrLimitCapsTheHold)
		{
			CntrPairing pairing;
			pairing.Configure(2.41, 74, true, true);
			pairing.SetHold(59);
			Assert::AreEqual(-1, pairing.Update(1.0, 1e9, 50), L"limit 50: no rise to the hold");
			Assert::AreEqual(50, pairing.Contrast());
			Assert::AreEqual(55, pairing.Update(1.0, 1e9, 55), L"the hold up to the limit");
			Assert::AreEqual(59, pairing.Update(1.0, 1e9, -1), L"no limit: the full hold");
			Assert::AreEqual(52, pairing.Update(1.0, 1e9, 52), L"a lower limit brings it down");
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

		// preposition_black_only (review 2026-10-17, default on): the CNTR
		// hold and the pre-lift only while truly black (IsTrueBlack); a dark
		// but visible picture gets the early ramp instead. Off = APL < 20.
		TEST_METHOD(PrepositionBlackOnlyHoldsOnlyOnTrueBlack)
		{
			for (const bool blackOnly : { true, false })
			{
				LedDriveConfig config = Compensate();
				config.blackPreliftMax = 120;
				config.prepositionBlackOnly = blackOnly;
				StatsBuilder bright(config);
				double ahead = 0.0;
				Lookahead look;
				look.stats = Play(bright, ahead, 100.0, Frame(120.0));
				look.cutAhead = true;
				look.framesAhead = 8;
				FrameStats dark;
				dark.apl = dark.sustainedApl = 18.5;
				dark.highlight = dark.sustainedHighlight = dark.maximum = 19.0;
				Assert::IsFalse(IsTrueBlack(dark));
				CompensateController visible(config);
				visible.Reset(60);
				const ControllerOutput out = visible.Tick(dark, 16.7, &look, true, true);
				if (blackOnly)
				{
					Assert::IsFalse(out.preposition, L"on: a dark, visible picture is not held (no release step later)");
					Assert::IsFalse(out.jump, L"on: the early ramp at the scene rate instead of the pre-lift jump");
					Assert::IsTrue(out.lightCommand > 60 && out.lightCommand < 120);
				}
				else
				{
					Assert::IsTrue(out.preposition, L"off: held while very dark (APL < 20), as before");
					Assert::AreEqual(120, out.lightCommand);
				}
				FrameStats black = dark;
				black.apl = black.sustainedApl = 16.0;
				black.highlight = black.sustainedHighlight = black.maximum = 16.0;
				Assert::IsTrue(IsTrueBlack(black));
				CompensateController onBlack(config);
				onBlack.Reset(60);
				const ControllerOutput blackOut = onBlack.Tick(black, 16.7, &look, true, true);
				Assert::IsTrue(blackOut.preposition, L"truly black: pre-positioned either way");
				Assert::AreEqual(120, blackOut.lightCommand);
			}
		}

		// cut_lead: window (review 2026-10-17, experimental): a seen bright cut's
		// jump goes out as soon as the cut enters the look-ahead window, then
		// the LED holds (in-scene sends wait) until the cut arrives; off = the
		// early ramp at the scene rate and the jump timed for the cut frame.
		TEST_METHOD(CutLeadWindowJumpsWhenTheCutEntersTheWindow)
		{
			for (const bool lead : { true, false })
			{
				LedDriveConfig config = Compensate();
				config.cutLeadWindow = lead;
				StatsBuilder scene(config), bright(config);
				double now = 0.0, ahead = 0.0;
				Lookahead look;
				look.stats = Play(bright, ahead, 100.0, Frame(120.0));
				look.cutAhead = true;
				look.framesAhead = 8;
				CompensateController controller(config);
				controller.Reset(130);   // the APL-30 scene's level
				const FrameStats dim = Play(scene, now, 1000.0, Frame(30.0));
				Assert::IsFalse(dim.cut);
				ControllerOutput out = controller.Tick(dim, 16.7, &look, true, true);
				if (lead)
				{
					Assert::AreEqual(255, out.lightCommand, L"the jump as the cut enters the window");
					Assert::IsTrue(out.jump && out.cutLeadHold);
					for (int tick = 0; tick < 5; ++tick)
					{
						out = controller.Tick(Play(scene, now, 1000.0 / 60.0, Frame(30.0)), 16.7, &look, true, true);
						Assert::AreEqual(-1, out.lightCommand, L"nothing more until the cut");
						Assert::IsTrue(out.cutLeadHold, L"in-scene sends wait (port reserve)");
					}
				}
				else
				{
					Assert::IsTrue(out.lightCommand > 130 && out.lightCommand < 255 && !out.jump,
						L"off: the early ramp at the scene rate");
					Assert::IsFalse(out.cutLeadHold);
				}
				const FrameStats cut = Play(scene, now, 1000.0 / 60.0, Frame(120.0));
				Assert::IsTrue(cut.cut);
				out = controller.Tick(cut, 16.7, &look, true, true);
				Assert::AreEqual(255, controller.Light(), L"at the scene's level on the cut");
				Assert::IsFalse(out.cutLeadHold, L"the hold ends with the cut");
				if (lead) Assert::AreEqual(-1, out.lightCommand, L"lead: already there, no second command");
				else Assert::AreEqual(255, out.lightCommand, L"off: the seen cut's jump");
			}
			// A cut that leaves the window without arriving ends the hold.
			LedDriveConfig config = Compensate();
			config.cutLeadWindow = true;
			StatsBuilder scene(config), bright(config);
			double now = 0.0, ahead = 0.0;
			Lookahead look;
			look.stats = Play(bright, ahead, 100.0, Frame(120.0));
			look.cutAhead = true;
			CompensateController controller(config);
			controller.Reset(130);
			Assert::IsTrue(controller.Tick(Play(scene, now, 1000.0, Frame(30.0)), 16.7, &look, true, true).cutLeadHold);
			look.cutAhead = false;
			look.stats = Play(bright, ahead, 100.0, Frame(30.0));
			const ControllerOutput after = controller.Tick(Play(scene, now, 1000.0 / 60.0, Frame(30.0)), 16.7,
				&look, true, true);
			Assert::IsFalse(after.cutLeadHold);
		}
	};
}
