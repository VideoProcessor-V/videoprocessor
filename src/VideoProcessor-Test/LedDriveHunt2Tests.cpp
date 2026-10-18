#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedController.h>
#include <led/LedDriveConfig.h>
#include <led/LedDriveModule.h>
#include <led/LedMeasurement.h>

#include "LedDriveTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

// Bug hunt 2 (2026-10-17), Dynamic Drive: settings applied mid-tick, failed
// restore writes and the render-ahead command frame. CPU only (a fake port).
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

	// internal_script with the light held at the ceiling (only CNTR moves).
	LedDriveConfig Script()
	{
		LedDriveConfig config;
		config.enabled = true;
		config.mode = Mode::Script;
		config.scriptPacing = false;
		config.tickMs = 400;
		ParseCurve("0:255, 255:255", config.curve);
		return config;
	}

	void SetWriteFails(FakeProjector& projector, bool fails)
	{
		std::lock_guard<std::mutex> guard(projector.mutex);
		projector.writeFails = fails;
	}

	size_t Count(const std::vector<std::string>& writes, size_t from, const char* command)
	{
		return static_cast<size_t>(std::count(writes.begin() + std::min(from, writes.size()), writes.end(),
			std::string(command)));
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveHunt2Tests)
	{
	public:
		// #1: Apply between a script tick's decision (the controller already
		// believes 50) and its CNTR write. The projector stays at 51 unless the
		// cut-short schedule is completed: CNTR must end at the need, once.
		TEST_METHOD(ScriptScheduleCutByNewSettingsEndsAtTheNeed)
		{
			for (const bool pacing : { true, false })
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->contrast = 50;
				LedDriveConfig config = Script();
				config.scriptPacing = pacing;
				// script_pacing: the decision comes 1.2 s before its sends; the tick
				// loop: the CNTR goes out 200 ms after the decision, ticks 1 s apart.
				config.scriptMeasureMs = 1200;
				config.scriptEchoMs = 50;
				config.tickMs = 1000;
				TestClock clock;
				LedDriveModule module(Port(), config,
					[projector] { return std::make_unique<FakePort>(projector); }, clock);
				Bind(clock, module, projector);
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
				module.SubmitMeasurement(Frame(50.0));   // APL 45-60, no highlight: CNTR 51
				Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }, 5000, 5.0),
					L"the script raised CNTR to 51");
				const double raised = clock.Ms();
				module.SubmitMeasurement(Frame(50.0, 100, 235));   // a highlight: the next tick decides 50
				// Inside the next tick's window between its decision and its CNTR write.
				Assert::IsTrue(clock.Run(pacing ? 700 : 900, 5.0));
				Assert::AreEqual(raised + (pacing ? 700 : 900), clock.Ms(), TestClock::TOLERANCE_MS);
				const size_t before = projector->Writes().size();
				const int atApply = projector->contrast;
				LedDriveConfig tuned = config;
				tuned.highlightToleranceStops = 0.5;   // a compensate-only value: the script is unchanged
				Assert::IsTrue(module.Reconfigure(Port(), tuned));
				Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }, 1500),
					pacing ? L"script pacing: CNTR 50 sent after the cut-short tick" :
						L"tick loop: CNTR 50 sent after the cut-short tick");
				Assert::IsTrue(clock.Run(1500));
				const auto writes = projector->Writes();
				Assert::AreEqual(static_cast<size_t>(atApply == 51 ? 1 : 0), Count(writes, before, ":CNTR50\r"),
					L"CNTR 50 written once after the apply");
				Assert::AreEqual(50, projector->contrast);
				module.Stop();
			}
		}

		// #2: the idle restore's CNTR 50 fails to write; it is tried again while
		// video is still away (the script believes 50 and would never send it).
		TEST_METHOD(ScriptIdleRestoreRetriesAFailedWrite)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Script();
			config.idleRestoreS = 1;
			TestClock clock;
			LedDriveModule module(Port(), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			uint64_t sequence = 0;
			module.SubmitMeasurement(Frame(50.0));
			// Frames every 50 ms on the clock until the script has raised CNTR.
			for (int frame = 0; frame < 80 && projector->contrast != 51; ++frame)
			{
				++sequence;
				module.BeginFrameAt(sequence, sequence * FRAME_MS);
				Assert::IsTrue(clock.Run(50.0));
			}
			Assert::AreEqual(51, projector->contrast, L"the script raised CNTR to 51");
			// Video stops; the restore (after 1 s) fails to write.
			SetWriteFails(*projector, true);
			Assert::IsTrue(clock.Run(1500));
			Assert::AreEqual(51, projector->contrast, L"the restore's write failed");
			SetWriteFails(*projector, false);
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }, 1500),
				L"the idle restore tried CNTR 50 again");
			const size_t after = projector->Writes().size();
			Assert::IsTrue(clock.Run(1000));
			Assert::AreEqual(static_cast<size_t>(0), Count(projector->Writes(), after, ":CNTR50\r"),
				L"not repeated once written");
			module.Stop();
		}

		// #2: compensate -> script, the CNTR 50 that leaves the assist fails to
		// write: it is re-sent at the next script tick.
		TEST_METHOD(LeavingCompensateResendsAFailedContrastWrite)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config;
			config.enabled = true;
			config.mode = Mode::Compensate;
			config.ledFloor = 60;
			config.ledFloorReactive = 60;
			ParseCurve("0:60, 20:60, 40:200, 70:230, 100:255, 255:255", config.curve);
			config.maxCutDip = 0.0;
			config.shapedRecovery = false;
			config.blackPreliftMax = 255;
			config.cntrAssist = true;
			ApplyRatePreset(config, true);
			ProjectorConfig port = Port();
			port.ledLatencyMs = 80;
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			for (int frame = 1; frame <= 600 && projector->contrast == 50; ++frame)
			{
				module.SubmitMeasurement(Frame(18.0, 100, 230));
				clock.To(frame * FRAME_MS);
				module.BeginFrameAt(0, frame * FRAME_MS);
				clock.Settle();
			}
			Assert::IsTrue(WaitFor([&] { return projector->contrast > 50; }), L"the CNTR assist raised CNTR");
			const int assisted = projector->contrast;
			// The script will target 50 (APL 45-60 with a highlight).
			module.SubmitMeasurement(Frame(50.0, 100, 235));
			SetWriteFails(*projector, true);
			LedDriveConfig script = Script();
			Assert::IsTrue(module.Reconfigure(port, script));
			Assert::IsTrue(clock.Run(60));
			SetWriteFails(*projector, false);
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }, 2000),
				(L"CNTR re-sent to 50 (was " + std::to_wstring(assisted) + L")").c_str());
			module.Stop();
		}

		// #2: script -> compensate (cntr_assist off), the CNTR 50 compensate
		// assumes fails to write: the driver's queue repeats it until written.
		TEST_METHOD(LeavingScriptRepeatsAFailedContrastWrite)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			LedDriveConfig config = Script();
			TestClock clock;
			LedDriveModule module(Port(), config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			Assert::IsTrue(WaitFor([&] { return module.DrivingProjector() && projector->light == 255; }));
			module.SubmitMeasurement(Frame(50.0));
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 51; }), L"the script raised CNTR to 51");
			Assert::IsTrue(clock.Run(100));
			SetWriteFails(*projector, true);
			LedDriveConfig compensate = config;
			compensate.mode = Mode::Compensate;
			compensate.cntrAssist = false;
			Assert::IsTrue(module.Reconfigure(Port(), compensate));
			Assert::IsTrue(clock.Run(100));
			SetWriteFails(*projector, false);
			Assert::IsTrue(clock.RunUntil([&] { return projector->contrast == 50; }, 3000), L"CNTR 50 written after all");
			module.Stop();
		}

		// #6: under render-ahead the command is for the frame shown led_latency_ms
		// after now, not after the frame's display time; floored at the current frame.
		TEST_METHOD(RenderAheadCommandFrameCountsFromNow)
		{
			Assert::AreEqual(80.0, CommandLeadMs(80.0, 0.0));
			Assert::AreEqual(40.0, CommandLeadMs(80.0, 40.0));
			Assert::AreEqual(0.0, CommandLeadMs(80.0, 120.0));
			Assert::AreEqual(80.0, CommandLeadMs(80.0, -5.0));
			for (const double fps : { 24.0, 25.0, 30.0, 50.0, 60.0 })
			{
				ProjectorConfig port;
				port.ledLatencyMs = 80;
				LedDriveConfig config;
				config.mode = Mode::Compensate;
				LedDriveModule module(port, config);
				module.BeginFrame(1, fps);
				Assert::AreEqual(static_cast<int>(std::lround(80.0 * fps / 1000.0)), module.CommandLatencySourceFrames(),
					L"no lead: led_latency_ms in source frames");
				const double ahead = 1000.0 / fps;   // one frame rendered ahead
				module.BeginFrameAhead(2, ahead, fps);
				Assert::AreEqual(static_cast<int>(std::lround((80.0 - ahead) * fps / 1000.0)),
					module.CommandLatencySourceFrames(), L"the lead is taken off");
				module.BeginFrameAhead(3, 200.0, fps);
				Assert::AreEqual(0, module.CommandLatencySourceFrames(), L"never before the current frame");
				module.BeginFrame(4, fps);
				Assert::AreEqual(static_cast<int>(std::lround(80.0 * fps / 1000.0)), module.CommandLatencySourceFrames(),
					L"the lead is its own frame's only");
			}
		}

		// #7: the driver reports its next send slot, from which a queued LPW1 lands.
		TEST_METHOD(DriverReportsItsNextSendSlot)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			ProjectorConfig port = Port();
			port.minCommandGapMs = 400;
			LedDriveConfig config = Script();
			TestClock clock;
			LedDriveModule module(port, config,
				[projector] { return std::make_unique<FakePort>(projector); }, clock);
			Bind(clock, module, projector);
			module.Start();
			// The start-up keeps the 400 ms gap, on the clock.
			Assert::IsTrue(clock.RunUntil([&] { return module.DrivingProjector() && projector->light == 255; }, 8000));
			const DriverStatus status = module.ProjectorStatus();
			Assert::IsTrue(status.nextSendAt != std::chrono::steady_clock::time_point(), L"known after a write");
			Assert::IsTrue(status.nextSendAt >= status.lastLightSentAt, L"at or after the last write");
			module.Stop();
		}
	};
}
