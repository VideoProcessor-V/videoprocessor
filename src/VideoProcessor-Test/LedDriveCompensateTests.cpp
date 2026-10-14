#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <led/LedController.h>
#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>
#include <led/ScriptListener.h>
#include <led/ScriptHost.h>

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
				LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }), L"compensate started");
				double time = 0.0;
				for (int frame = 0; frame < 60; ++frame)
				{
					module.SubmitMeasurement(Frame(18.0));
					time += 1000.0 / 60.0;
					module.BeginFrameAt(0, time);
					WaitFor([&] { return !module.ProjectorStatus().busy; });
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

		TEST_METHOD(ListenMessagesParse)
		{
			ScriptMessage m;
			Assert::IsTrue(ParseScriptMessage("VP1 TICK -1 t=12.5 mean=41 max=230 hl=1 tp=210 tc=58", m));
			Assert::AreEqual(std::string("TICK"), m.kind);
			Assert::AreEqual(41, m.mean);
			Assert::AreEqual(230, m.maximum);
			Assert::AreEqual(1, m.highlight);
			Assert::AreEqual(210, m.targetPower);
			Assert::AreEqual(58, m.targetContrast);
			Assert::IsTrue(ParseScriptMessage("VP1 CNTR 55 t=1.0 mean=-1 max=-1 hl=-1 tp=-1 tc=-1", m));
			Assert::AreEqual(55, m.value);
			Assert::IsFalse(ParseScriptMessage("VP2 CNTR 55", m));
			Assert::IsFalse(ParseScriptMessage("VP1 LDMM 0", m));
			Assert::IsFalse(ParseScriptMessage("garbage", m));
		}

		TEST_METHOD(ListenModeFollowsTheScriptWithoutThePort)
		{
			// mode: listen (owner 2026-10-13): the script drives COM1 and tells
			// VP over UDP. VP never opens the port; a heard CNTR step is eased
			// in the picture as if VP had sent it (the script smoothing); a TICK
			// is compared with VP's own copy of the rules; both are logged.
			auto projector = std::make_shared<FakeProjector>();
			LedDriveConfig config = Compensate();
			config.mode = Mode::Listen;
			config.scriptSmoothing = true;
			config.listenPort = 47923;
			config.listenLog = true;
			// Beside the test binary (writable wherever the tests build).
			char here[MAX_PATH] = {};
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&ParseScriptMessage), &self);
			GetModuleFileNameA(self, here, MAX_PATH);
			const std::string binary(here);
			config.listenLogDirectory = binary.substr(0, binary.find_last_of("\\/") + 1) + "vp-listen-test";
			DeleteFileA(ListenCsvPath(config.listenLogDirectory).c_str());
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			port.cntrLatencyMs = 100;
			Assert::AreEqual(std::string("none"), EffectiveProtocol(port, config));
			LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.Listening().listening; }), L"the UDP port is bound");
			module.SubmitMeasurement(Frame(40.0, 100, 230));
			module.BeginFrame(0);
			Assert::IsTrue(SendScriptMessageForTest(config.listenPort, "VP1 TICK -1 t=1.0 mean=40 max=230 hl=1 tp=-1 tc=-1"));
			Assert::IsTrue(SendScriptMessageForTest(config.listenPort, "VP1 LPW1 200 t=1.1 mean=40 max=230 hl=1 tp=-1 tc=-1"));
			Assert::IsTrue(WaitFor([&] { return module.Listening().ticks == 1 && module.Listening().lightCommands == 1; }));
			const ListenAccuracy heard = module.Listening();
			Assert::IsTrue(heard.meanDifference <= 2.0, (L"mean2 vs VP's APL: " + std::to_wstring(heard.meanDifference)).c_str());
			Assert::AreEqual(1.0, heard.highlightAgreement, 1e-9);
			// A heard CNTR step: the picture gain moves off 1 around the step
			// and comes back (the smoother eases the projector's step).
			Assert::IsTrue(SendScriptMessageForTest(config.listenPort, "VP1 CNTR 56 t=1.2 mean=40 max=230 hl=1 tp=-1 tc=-1"));
			Assert::IsTrue(WaitFor([&] { return module.Listening().contrastCommands == 1; }));
			double furthest = 0.0, last = 1.0;
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
			while (std::chrono::steady_clock::now() < until)
			{
				const CompensationGain gain = module.BeginFrame(0);
				const double g = gain.active ? gain.gain : 1.0;
				furthest = std::max(furthest, std::fabs(g - 1.0));
				last = g;
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
			}
			Assert::AreEqual(56, module.Listening().lastContrast);
			Assert::IsTrue(furthest > 0.005, (L"the step was eased: furthest gain " + std::to_wstring(furthest)).c_str());
			Assert::AreEqual(1.0, last, 0.002, L"and the gain is back at 1 after the step");
			module.Stop();
			Assert::AreEqual(0, projector->opens, L"listen must not open the port");
			Assert::IsTrue(projector->Writes().empty(), L"listen must not write to the port");
			// The data log has the TICK row with VP's measurement next to it.
			std::ifstream csv(ListenCsvPath(config.listenLogDirectory));
			std::string line, tick;
			while (std::getline(csv, line))
				if (line.find(",TICK,") != std::string::npos)
					tick = line;
			Assert::IsFalse(tick.empty(), L"the TICK row is logged");
			Assert::IsTrue(tick.find(",40,230,1,") != std::string::npos, std::wstring(tick.begin(), tick.end()).c_str());
			// The monitor's line: the heard commands and the agreement.
			std::ifstream nowFile(ListenNowPath(config.listenLogDirectory));
			std::string now;
			std::getline(nowFile, now);
			Assert::IsTrue(now.find("light=200") != std::string::npos && now.find("contrast=56") != std::string::npos &&
				now.find("ticks=1") != std::string::npos, std::wstring(now.begin(), now.end()).c_str());
		}

		TEST_METHOD(ScriptHostRunsWatchesRestartsAndStopsTheScript)
		{
			// Owner 2026-10-13: VP starts the listen script itself. A dummy
			// script (its folder's mode.txt: "hang" prints and waits, "exit"
			// prints and exits with code 3) under the real Python.
			wchar_t python[MAX_PATH] = {};
			if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, python, nullptr) == 0)
			{
				Logger::WriteMessage(L"ScriptHost test: no python.exe on PATH; not run");
				return;
			}
			char here[MAX_PATH] = {};
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&ParseScriptMessage), &self);
			GetModuleFileNameA(self, here, MAX_PATH);
			const std::string binary(here);
			const std::string folder = binary.substr(0, binary.find_last_of("\\/") + 1) + "vp-script-host-test";
			const std::string logs = folder + "\\logs";
			CreateDirectoryA(folder.c_str(), nullptr);
			CreateDirectoryA(logs.c_str(), nullptr);
			const std::string script = folder + "\\dummy.py";
			{
				std::ofstream out(script, std::ios::trunc);
				out << "import sys, time\n"
					"mode = open('mode.txt').read().strip()\n"
					"print('hello from the dummy script, mode ' + mode)\n"
					"if mode == 'exit':\n"
					"    sys.exit(3)\n"
					"while True:\n"
					"    time.sleep(0.2)\n";
			}
			auto setMode = [&](const char* mode) { std::ofstream(folder + "\\mode.txt", std::ios::trunc) << mode << "\n"; };
			const std::string logPath = logs + "\\led-script.log", nowPath = logs + "\\led-script-now.txt";
			DeleteFileA(logPath.c_str());
			auto read = [](const std::string& path)
			{
				std::ifstream in(path);
				return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			};
			auto alive = [](DWORD pid)
			{
				HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
				if (!h) return false;
				const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
				CloseHandle(h);
				return running;
			};
			char pythonPath[MAX_PATH] = {};
			WideCharToMultiByte(CP_UTF8, 0, python, -1, pythonPath, MAX_PATH, nullptr, nullptr);
			// The test's logs folder stands for "beside vp.log": ListenNowPath(directory).
			const std::string directory = logs + "\\";
			DWORD last = 0;
			{
				ScriptHost host;
				// No script: nothing runs.
				host.Update("", pythonPath, directory);
				Assert::IsTrue(host.GetState() == ScriptHost::State::Off && host.Pid() == 0, L"no listen_script: off");
				// Runs; its output reaches the log; the monitor's file says so.
				setMode("hang");
				host.Update(script, pythonPath, directory);
				Assert::IsTrue(host.GetState() == ScriptHost::State::Running && host.Pid() != 0,
					(L"started: " + std::wstring(host.LastError().begin(), host.LastError().end())).c_str());
				const DWORD first = host.Pid();
				Assert::IsTrue(WaitFor([&] { return read(logPath).find("hello from the dummy script, mode hang") != std::string::npos; }),
					L"the script's output is in logs\\led-script.log");
				Assert::IsTrue(read(nowPath).find("state=running pid=" + std::to_string(first)) != std::string::npos,
					L"led-script-now.txt: running with its pid");
				// Watched: still running a moment later, not restarted.
				Sleep(500);
				host.Update(script, pythonPath, directory);
				Assert::IsTrue(host.Pid() == first && host.Starts() == 1, L"a running script is left alone");
				// Stop (the mode left listen): the process ends.
				host.Stop();
				Assert::IsTrue(host.GetState() == ScriptHost::State::Off);
				Assert::IsTrue(WaitFor([&] { return !alive(first); }), L"stopped: the process ended");
				Assert::IsTrue(read(nowPath).find("state=stopped") != std::string::npos, L"led-script-now.txt: stopped");
				// A script that exits: restarted after 2 s, then 4 s.
				setMode("exit");
				host.Update(script + "", pythonPath + std::string(), directory);   // same values: a start
				const int startsBefore = host.Starts();
				Assert::IsTrue(WaitFor([&] { host.Update(script, pythonPath, directory); return host.GetState() == ScriptHost::State::Waiting; }),
					L"the exit is seen");
				Assert::IsTrue(host.LastError() == "exited with code 3", std::wstring(host.LastError().begin(), host.LastError().end()).c_str());
				Assert::IsTrue(host.SecondsToRestart() >= 1 && host.SecondsToRestart() <= 2, L"restart in 2 s");
				Assert::IsTrue(WaitFor([&] { host.Update(script, pythonPath, directory); return host.Starts() == startsBefore + 1; }, 4000),
					L"started again after 2 s");
				Assert::IsTrue(WaitFor([&] { host.Update(script, pythonPath, directory); return host.GetState() == ScriptHost::State::Waiting; }),
					L"the second exit is seen");
				Assert::IsTrue(host.SecondsToRestart() >= 3 && host.SecondsToRestart() <= 4, L"then in 4 s (the back-off doubles)");
				// The port set-up: a port that is not there is reported and the
				// script starts anyway (VP never holds the port).
				setMode("hang");
				host.Update("", pythonPath, directory);   // a new script value: start at once
				host.Update(script, pythonPath, directory, "COM250", 19200);
				Assert::IsTrue(host.GetState() == ScriptHost::State::Running, L"a port that cannot be set up: the script still starts");
				Assert::IsTrue(host.PortError().find("cannot open COM250") == 0,
					std::wstring(host.PortError().begin(), host.PortError().end()).c_str());
				Assert::IsTrue(read(nowPath).find("port_error=cannot_open_COM250") != std::string::npos, L"led-script-now.txt: the port error");
				Assert::IsTrue(host.GetPortSetup() == ScriptHost::PortSetup::Failed && read(nowPath).find("port=failed") != std::string::npos,
					L"the port set-up reported as failed");
				host.Stop();
				host.Update("", pythonPath, directory);
				host.Update(script, pythonPath, directory);   // no port configured
				Assert::IsTrue(host.GetPortSetup() == ScriptHost::PortSetup::Skipped && read(nowPath).find("port=skipped") != std::string::npos,
					L"no port: reported as skipped");
				host.Stop();
				setMode("exit");
				// A missing script: waiting, with the reason.
				host.Update(folder + "\\missing.py", pythonPath, directory);
				Assert::IsTrue(host.GetState() == ScriptHost::State::Waiting && host.LastError().find("script not found") == 0,
					L"a missing script waits with the reason");
				// Destroyed while running (VP exits): no orphan.
				setMode("hang");
				host.Update(script, pythonPath, directory);
				Assert::IsTrue(host.GetState() == ScriptHost::State::Running);
				last = host.Pid();
				Assert::IsTrue(alive(last));
			}
			// The host is gone: its script is too.
			Assert::IsTrue(WaitFor([&] { return !alive(last); }), L"no orphan after the host is destroyed");
		}

		TEST_METHOD(ListenLogOffWritesNoCsv)
		{
			// listen_log off (the default): the monitor's line only, no data log.
			auto projector = std::make_shared<FakeProjector>();
			LedDriveConfig config = Compensate();
			config.mode = Mode::Listen;
			config.listenPort = 47924;
			config.listenLog = false;
			char here[MAX_PATH] = {};
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&ParseScriptMessage), &self);
			GetModuleFileNameA(self, here, MAX_PATH);
			const std::string binary(here);
			config.listenLogDirectory = binary.substr(0, binary.find_last_of("\\/") + 1) + "vp-listen-off-test";
			CreateDirectoryA(config.listenLogDirectory.c_str(), nullptr);
			DeleteFileA(ListenCsvPath(config.listenLogDirectory).c_str());
			DeleteFileA(ListenNowPath(config.listenLogDirectory).c_str());
			ProjectorConfig port;
			port.protocol = "barco_ascii";
			LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); });
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.Listening().listening; }), L"the UDP port is bound");
			Assert::IsTrue(SendScriptMessageForTest(config.listenPort, "VP1 LPW1 210 t=1.0 mean=40 max=230 hl=1 tp=-1 tc=-1"));
			Assert::IsTrue(WaitFor([&] { return module.Listening().lightCommands == 1; }));
			Assert::IsTrue(WaitFor([&] { return GetFileAttributesA(ListenNowPath(config.listenLogDirectory).c_str()) != INVALID_FILE_ATTRIBUTES; }),
				L"the monitor's line is written");
			module.Stop();
			Assert::AreEqual(static_cast<DWORD>(INVALID_FILE_ATTRIBUTES), GetFileAttributesA(ListenCsvPath(config.listenLogDirectory).c_str()),
				L"no data log with listen_log off");
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
		TEST_METHOD(ModuleBrightCutDumpsContrastAndFadesUp)
		{
			for (const bool dump : { true, false })
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				LedDriveConfig config = Assisted();
				ApplyRatePreset(config, true);
				config.maxCutDip = 0.3;
				config.shapedRecovery = true;
				config.cutCntrDump = dump;
				config.cutCntrLevel = 55; // below where the dim scene's CNTR sits, so a dump shows
				ProjectorConfig port;
				port.protocol = "barco_ascii";
				port.ackTimeoutMs = 60;
				port.minCommandGapMs = 0;
				port.readBack = true;
				LedDriveModule module(port, config,
					[projector] { return std::make_unique<FakePort>(projector); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				double time = 0.0;
				auto render = [&](const RawMeasurement& raw)
				{
					time += 1000.0 / 60.0;
					module.SubmitMeasurement(raw);
					const CompensationGain gain = module.BeginFrameAt(0, time);
					WaitFor([&] { return !module.ProjectorStatus().busy; });
					return gain;
				};
				for (int frame = 0; frame < 600; ++frame) render(Frame(18.0, 100, 200));
				const int before = module.AssistContrast();
				Assert::IsTrue(before > 50, L"the dark scene raised CNTR");
				Assert::IsTrue(WaitFor([&] { return projector->contrast == before; }));
				const size_t writesBefore = projector->Writes().size();

				Assert::IsTrue(before <= 59, L"with highlights CNTR stops at dmd_clip_cntr (59)");
				// The cut: a bright scene with highlights at white (ceiling 2^tolerance).
				const double ceiling = std::pow(2.0, config.highlightToleranceStops);
				CompensationGain gain = render(Frame(120.0, 100, 235));
				Assert::IsTrue(module.Recovering(), L"the fade-up starts on the cut");
				Assert::IsTrue(gain.gain <= ceiling + 1e-6, L"the gain goes no higher than the new scene's ceiling");
				double previousDisplayed = module.RecoveryDisplayed();
				for (int frame = 0; frame < 900 && module.Recovering(); ++frame)
				{
					gain = render(Frame(120.0, 100, 235));
					Assert::IsTrue(gain.gain <= ceiling + 1e-6);
					Assert::IsTrue(module.RecoveryDisplayed() >= previousDisplayed - 1e-9, L"the fade-up only climbs");
					previousDisplayed = module.RecoveryDisplayed();
				}
				Assert::IsFalse(module.Recovering(), L"the fade-up ends");
				// Let a held CNTR finish its paired descent.
				for (int frame = 0; frame < 300 && module.AssistContrast() > 50; ++frame)
					render(Frame(120.0, 100, 235));
				std::vector<int> after;
				const auto writes = projector->Writes();
				for (size_t i = writesBefore; i < writes.size(); ++i)
					if (writes[i].rfind(":CNTR", 0) == 0 && writes[i] != ":CNTR?\r")
						after.push_back(std::stoi(writes[i].substr(5)));
				Assert::IsFalse(after.empty());
				for (size_t i = 1; i < after.size(); ++i)
					Assert::IsTrue(after[i] <= after[i - 1], L"CNTR never rises during the fade-up");
				if (dump)
				{
					Assert::AreEqual(55, after.front(), L"one command to cut_cntr_level, never to 50 because of the cut");
					// Audit stage7-step7c: the cut's CNTR goes out before the LED's jump.
					size_t firstContrast = writes.size(), firstLight = writes.size();
					for (size_t i = writesBefore; i < writes.size(); ++i)
					{
						if (firstContrast == writes.size() && writes[i] == ":CNTR55\r") firstContrast = i;
						if (firstLight == writes.size() && writes[i].rfind(":LPW1", 0) == 0 && writes[i] != ":LPW1?\r")
							firstLight = i;
					}
					Assert::IsTrue(firstContrast < firstLight, L"CNTR first, then the LPW1 a command gap later");
				}
				else
					Assert::IsTrue(after.front() > 55 && after.size() > 1, L"held through the cut, descending paired");
				Assert::AreEqual(50, module.AssistContrast(), L"back to 50 in the bright scene");
				module.Stop();
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
