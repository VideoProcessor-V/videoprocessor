#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <led/BarcoAsciiDriver.h>
#include <led/LedDriveConfig.h>
#include <led/ProjectorDriver.h>
#include <led/ConnectionTest.h>

#include "LedDriveTestSupport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

namespace
{
	ProjectorConfig FastConfig()
	{
		ProjectorConfig config;
		config.protocol = "barco_ascii";
		config.ackTimeoutMs = 60;
		config.minCommandGapMs = 0;
		config.maxLevelsPerCommand = 0;
		config.readBack = true;
		return config;
	}

	std::unique_ptr<IProjectorDriver> OpenDriver(std::shared_ptr<FakeProjector> projector,
		ProjectorConfig config = FastConfig())
	{
		auto driver = CreateProjectorDriver("barco_ascii", config,
			[projector] { return std::make_unique<FakePort>(projector); });
		std::string error;
		Assert::IsTrue(driver->Open(error), std::wstring(error.begin(), error.end()).c_str());
		return driver;
	}

	std::vector<std::string> LightCommands(const std::vector<std::string>& writes)
	{
		std::vector<std::string> lights;
		for (const auto& write : writes)
			if ((write.rfind(":LPW1", 0) == 0 || write.rfind(":lpw1", 0) == 0) &&
				write.find('?') == std::string::npos)
				lights.push_back(write);
		return lights;
	}

	struct TemporaryConfig
	{
		std::string path;
		explicit TemporaryConfig(const std::string& contents)
		{
			char directory[MAX_PATH]{}, file[MAX_PATH]{};
			GetTempPathA(MAX_PATH, directory);
			GetTempFileNameA(directory, "vpl", 0, file);
			path = file;
			std::ofstream(path, std::ios::trunc) << contents;
		}
		~TemporaryConfig() { DeleteFileA(path.c_str()); }
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(LedDriveProtocolTests)
	{
		// RS-232 connection test (LED drive page): LED 60, read back, wait,
		// LED 255 (or the projector's maximum), read back, pass/fail.
		TEST_METHOD(ConnectionTestPassesFailsAndExplains)
		{
			ConnectionTestPlan plan;
			plan.holdMs = 20;
			plan.commandTimeoutMs = 500;
			auto run = [&](std::shared_ptr<FakeProjector> projector, ProjectorConfig config)
			{
				return RunConnectionTestOnNewConnection(config,
					[projector] { return std::make_unique<FakePort>(projector); }, plan);
			};
			ProjectorConfig config = FastConfig();
			config.readBack = false;   // the saved setting; the test turns it on itself
			{
				auto projector = std::make_shared<FakeProjector>();
				const ConnectionTestResult result = run(projector, config);
				Assert::IsTrue(result.pass, std::wstring(result.summary.begin(), result.summary.end()).c_str());
				Assert::IsTrue(result.summary.rfind("PASS", 0) == 0);
				const auto lights = LightCommands(projector->Writes());
				Assert::IsTrue(lights.size() >= 2);
				Assert::IsTrue(lights.front().rfind(":LPW160", 0) == 0 && lights.back().rfind(":LPW1255", 0) == 0);
				Assert::AreEqual(255, projector->light, L"The light must end at 255");
			}
			{
				// A projector whose maximum is below 255: the test ends there.
				auto projector = std::make_shared<FakeProjector>();
				projector->maximum = 200;
				const ConnectionTestResult result = run(projector, config);
				Assert::IsTrue(result.pass && projector->light == 200,
					std::wstring(result.summary.begin(), result.summary.end()).c_str());
			}
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->silent = true;
				const ConnectionTestResult result = run(projector, config);
				Assert::IsFalse(result.pass);
				Assert::IsTrue(result.summary.find("did not answer") != std::string::npos,
					std::wstring(result.summary.begin(), result.summary.end()).c_str());
			}
			{
				auto projector = std::make_shared<FakeProjector>();
				projector->openFails = true;
				const ConnectionTestResult result = run(projector, config);
				Assert::IsFalse(result.pass);
				Assert::IsTrue(result.summary.find("could not open") != std::string::npos,
					std::wstring(result.summary.begin(), result.summary.end()).c_str());
			}
			{
				// A projector that acknowledges but reports another level.
				struct Stubborn : IProjectorDriver
				{
					int light = 150;
					const char* Protocol() const override { return "test"; }
					bool Open(std::string&) override { return true; }
					LightRange QueryLightRange() override { return {}; }
					ProjectorState ReadState() override { ProjectorState st; st.lightKnown = true; st.light = light; return st; }
					void SetLight(int) override {}
					bool SetLightNow(int, int) override { return true; }
					bool SetContrast(int) override { return true; }
					void Close() override {}
					DriverStatus Status() const override { return {}; }
				} stubborn;
				int slept = 0;
				const ConnectionTestResult result = RunConnectionTest(stubborn, [&](int ms) { slept += ms; }, plan);
				Assert::IsFalse(result.pass);
				Assert::IsTrue(result.summary.find("reports 150") != std::string::npos,
					std::wstring(result.summary.begin(), result.summary.end()).c_str());
				Assert::AreEqual(0, slept, L"No wait after a failed first step");
			}
		}

	public:
		TEST_METHOD(ReplyParserAcceptsOnlyBarcoReplies)
		{
			std::string mnemonic;
			int value = 0;
			Assert::IsTrue(BarcoAsciiDriver::ParseReply("%001 LPW1 000255", mnemonic, value));
			Assert::AreEqual(std::string("LPW1"), mnemonic);
			Assert::AreEqual(255, value);
			Assert::IsTrue(BarcoAsciiDriver::ParseReply("%001 CNTR 000050", mnemonic, value));
			Assert::AreEqual(50, value);
			for (const char* bad : { "", "LPW1 000255", "%01 LPW1 000255", "%001 LPW1",
				"%001 LPW1 00x255", "%001 LPW12 000255", "%001 LPW1 000255 extra" })
				Assert::IsFalse(BarcoAsciiDriver::ParseReply(bad, mnemonic, value));
		}

		TEST_METHOD(HandshakeFindsCrFramingRangeAndStateThenRestoresKnownState)
		{
			auto projector = std::make_shared<FakeProjector>();
			auto driver = OpenDriver(projector);
			const auto status = driver->Status();
			Assert::AreEqual(std::string("cr"), status.framing);
			Assert::IsTrue(status.acknowledged);
			const auto range = driver->QueryLightRange();
			Assert::IsTrue(range.known);
			Assert::AreEqual(60, range.minimum);
			Assert::AreEqual(255, range.maximum);
			const auto state = driver->ReadState();
			Assert::IsTrue(state.lightKnown && state.contrastKnown);
			Assert::AreEqual(120, state.light);
			Assert::AreEqual(74, state.contrast, L"The old script can leave contrast at 74");

			const std::string summary = BringToKnownState(*driver, 255);
			Assert::AreEqual(255, projector->light);
			Assert::AreEqual(50, projector->contrast, L"Startup must put CNTR back to 50");
			Assert::IsTrue(summary.find("contrast=74") != std::string::npos);
			const auto writes = projector->Writes();
			Assert::AreEqual(std::string(":ECHO1\r"), writes.front());
			Assert::IsTrue(std::find(writes.begin(), writes.end(), ":LPW1255\r") != writes.end());
			Assert::IsTrue(std::find(writes.begin(), writes.end(), ":CNTR50\r") != writes.end());
		}

		TEST_METHOD(ContrastIsOnlyWrittenAtStartupAndOnlyWhenNot50)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			auto driver = OpenDriver(projector);
			BringToKnownState(*driver, 255);
			for (int level : { 200, 150, 100 }) driver->SetLightNow(level, 500);
			for (const auto& write : projector->Writes())
				Assert::IsFalse(write.rfind(":CNTR", 0) == 0 && write != ":CNTR?\r",
					L"CNTR must not be driven");
		}

		TEST_METHOD(LiteralCrFramingIsDetectedWhenPlainCrIsNotAnswered)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->acceptedSuffix = "cr\r";
			auto driver = OpenDriver(projector);
			Assert::AreEqual(std::string("literal_cr"), driver->Status().framing);
			Assert::IsTrue(driver->SetLightNow(200, 500));
			Assert::AreEqual(200, projector->light);
			Assert::AreEqual(std::string(":LPW1200cr\r"), LightCommands(projector->Writes()).back());
		}

		TEST_METHOD(SilentProjectorGetsTheOldScriptsExactBytes)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->silent = true;
			auto driver = OpenDriver(projector);
			const auto status = driver->Status();
			Assert::AreEqual(std::string("script"), status.framing);
			Assert::IsFalse(status.acknowledged);
			Assert::IsFalse(driver->QueryLightRange().known);
			Assert::IsTrue(driver->SetLightNow(200, 1000));
			const auto lights = LightCommands(projector->Writes());
			Assert::IsFalse(lights.empty());
			Assert::AreEqual(std::string(":lpw1200cr\r\n"), lights.back());
		}

		TEST_METHOD(LevelsAreClampedToTheReportedRangeAndOverrides)
		{
			auto projector = std::make_shared<FakeProjector>();
			auto driver = OpenDriver(projector);
			driver->SetLightNow(10, 500);
			Assert::AreEqual(60, projector->light);
			driver->SetLightNow(400, 500);
			Assert::AreEqual(255, projector->light);

			auto overridden = std::make_shared<FakeProjector>();
			ProjectorConfig config = FastConfig();
			config.lightMin = 80;
			config.lightMax = 230;
			auto clamped = OpenDriver(overridden, config);
			clamped->SetLightNow(60, 500);
			Assert::AreEqual(80, overridden->light);
			clamped->SetLightNow(255, 500);
			Assert::AreEqual(230, overridden->light);
			for (const auto& write : overridden->Writes())
				Assert::IsTrue(write.find(" ?N") == std::string::npos && write.find(" ?M") == std::string::npos,
					L"Configured limits replace the range queries");
		}

		TEST_METHOD(NewerTargetsReplaceOnesNotYetSent)
		{
			auto projector = std::make_shared<FakeProjector>();
			auto driver = OpenDriver(projector);
			const size_t before = projector->Writes().size();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->holdReplies = true;
			}
			driver->SetLight(150);
			Assert::IsTrue(projector->WaitForWrites(before + 1));
			driver->SetLight(140);
			driver->SetLight(130);
			projector->Release();
			Assert::IsTrue(projector->WaitForWrites(before + 2));
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			const auto lights = LightCommands(projector->Writes());
			Assert::AreEqual(size_t{ 2 }, lights.size(), L"The stale 140 must be skipped");
			Assert::AreEqual(std::string(":LPW1150\r"), lights[0]);
			Assert::AreEqual(std::string(":LPW1130\r"), lights[1]);
			Assert::AreEqual(130, driver->Status().lastLight);
		}

		TEST_METHOD(ReadBackOffOnlySendsTheScriptsBytes)
		{
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = FastConfig();
			config.readBack = false;
			auto driver = OpenDriver(projector, config);
			Assert::IsTrue(projector->Writes().empty(), L"No ECHO, no queries when read_back is off");
			Assert::IsFalse(driver->Status().acknowledged);
			BringToKnownState(*driver, 255);
			const auto writes = projector->Writes();
			Assert::AreEqual(size_t{ 2 }, writes.size());
			Assert::AreEqual(std::string(":lpw1255cr\r\n"), writes[0], L"The old script's exact bytes");
			Assert::AreEqual(std::string(":cntr50cr\r\n"), writes[1], L"Contrast reset sent blind, as the script did");
			Assert::IsFalse(driver->ReadState().lightKnown, L"Never asked");
			driver->SetLight(120);
			Assert::IsTrue(projector->WaitForWrites(3));
			for (const auto& write : projector->Writes())
				Assert::IsTrue(write.find('?') == std::string::npos);
			Assert::AreEqual(120, driver->Status().lastSentLight);
			Assert::AreEqual(2, driver->Status().lightSends);
		}

		TEST_METHOD(CommandsKeepTheGapAndTheOptionalPause)
		{
			auto measure = [](int burst)
			{
				auto projector = std::make_shared<FakeProjector>();
				ProjectorConfig config = FastConfig();
				config.readBack = false;
				config.minCommandGapMs = 120;
				config.burstCommands = burst;
				config.burstPauseMs = 600;
				auto driver = OpenDriver(projector, config);
				// The controller asks every 10 ms; only some targets go out.
				const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1700);
				for (int level = 100; std::chrono::steady_clock::now() < end; ++level)
				{
					driver->SetLight(100 + level % 100);
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				driver->Close();
				return projector->WriteTimes();
			};
			auto gapMs = [](const std::vector<std::chrono::steady_clock::time_point>& times, size_t i)
			{
				return std::chrono::duration<double, std::milli>(times[i] - times[i - 1]).count();
			};
			const auto steady = measure(0);
			Assert::IsTrue(steady.size() >= 10, std::to_wstring(steady.size()).c_str());
			for (size_t i = 1; i < steady.size(); ++i)
				Assert::IsTrue(gapMs(steady, i) >= 118.0, L"Never closer than min_command_gap_ms");
			const auto paused = measure(3);
			Assert::IsTrue(paused.size() >= 5);
			for (size_t i = 1; i < paused.size(); ++i)
				Assert::IsTrue(gapMs(paused, i) >= (i % 3 == 0 ? 598.0 : 118.0),
					(L"gap " + std::to_wstring(i) + L" = " + std::to_wstring(gapMs(paused, i))).c_str());
		}

		TEST_METHOD(ClosingDoesNotWaitOutAPause)
		{
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = FastConfig();
			config.readBack = false;
			config.burstCommands = 1;
			config.burstPauseMs = 5000;
			auto driver = OpenDriver(projector, config);
			driver->SetLight(150);
			Assert::IsTrue(projector->WaitForWrites(1));
			driver->SetLight(140);
			const auto start = std::chrono::steady_clock::now();
			driver->Close();
			Assert::IsTrue(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
			Assert::AreEqual(size_t{ 1 }, projector->Writes().size(), L"140 was still inside the pause");
		}

		TEST_METHOD(ExitRestoreIsNeverOvertakenByADimCommand)
		{
			// Audit (stage7-step1-protocol.md): a dim target the driver thread is
			// sending, or still holds, must not land after the full-light restore,
			// and the restore stays within its second even with a long reply wait.
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = FastConfig();
			config.ackTimeoutMs = 3000;
			auto driver = OpenDriver(projector, config);
			const size_t before = projector->Writes().size();
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				projector->holdReplies = true;
			}
			driver->SetLight(100);
			Assert::IsTrue(projector->WaitForWrites(before + 1), L"100 is on the wire, its reply held back");
			driver->SetLight(90); // queued behind it
			const auto start = std::chrono::steady_clock::now();
			RestoreFullLight(*driver, 255);
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			Assert::IsTrue(elapsed < 1100, (L"restore took " + std::to_wstring(elapsed) + L" ms").c_str());
			projector->Release();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			driver->Close();
			const auto lights = LightCommands(projector->Writes());
			Assert::AreEqual(std::string(":LPW1255\r"), lights.back(), L"Full light is the last level sent");
			for (const auto& light : lights)
				Assert::IsTrue(light != ":LPW190\r", L"The queued dim target was dropped");
		}

		TEST_METHOD(MissingReplyIsCountedAndTheDriverCarriesOn)
		{
			auto projector = std::make_shared<FakeProjector>();
			auto driver = OpenDriver(projector);
			projector->silent = true;
			Assert::IsFalse(driver->SetLightNow(200, 500));
			Assert::IsTrue(driver->Status().timeouts >= 1);
			projector->silent = false;
			Assert::IsTrue(driver->SetLightNow(210, 500));
			Assert::AreEqual(210, projector->light);
			Assert::AreEqual(210, driver->Status().lastLight);
		}

		TEST_METHOD(FailedOpenNeverSendsAnything)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->openFails = true;
			auto driver = CreateProjectorDriver("barco_ascii", FastConfig(),
				[projector] { return std::make_unique<FakePort>(projector); });
			std::string error;
			Assert::IsFalse(driver->Open(error));
			Assert::IsFalse(error.empty());
			driver->SetLight(200);
			Assert::IsFalse(driver->SetLightNow(200, 100));
			Assert::IsFalse(driver->SetContrast(50));
			Assert::IsTrue(projector->Writes().empty());
		}

		TEST_METHOD(NoneProtocolLogOnlyAndDisabledNeverOpenAPort)
		{
			ProjectorConfig projectorConfig = FastConfig();
			LedDriveConfig led;
			led.enabled = true;
			led.mode = Mode::LogOnly;
			Assert::AreEqual(std::string("none"), EffectiveProtocol(projectorConfig, led));
			led.mode = Mode::Script;
			led.enabled = false;
			Assert::AreEqual(std::string("none"), EffectiveProtocol(projectorConfig, led));
			led.enabled = true;
			Assert::AreEqual(std::string("barco_ascii"), EffectiveProtocol(projectorConfig, led));
			projectorConfig.protocol = "none";
			Assert::AreEqual(std::string("none"), EffectiveProtocol(projectorConfig, led));

			int portsCreated = 0;
			auto driver = CreateProjectorDriver("none", projectorConfig,
				[&portsCreated] { ++portsCreated; return std::unique_ptr<ISerialPort>(); });
			std::string error;
			Assert::IsTrue(driver->Open(error));
			driver->SetLight(180);
			Assert::IsTrue(BringToKnownState(*driver, 255).find("light set to 255") != std::string::npos);
			Assert::IsTrue(RestoreFullLight(*driver, 255));
			driver->Close();
			Assert::AreEqual(0, portsCreated);
			Assert::AreEqual(std::string("dry-run"), driver->Status().framing);
			Assert::AreEqual(255, driver->Status().lastLight);
		}

		// Step 7b: script mode's commands are synchronous and in the script's
		// own timing - no min_command_gap_ms or burst pause - and its CNTR
		// commands really reach the projector.
		TEST_METHOD(ScriptSendsIgnoreTheCommandGapAndStopAtShutdown)
		{
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = FastConfig();
			config.minCommandGapMs = 500;
			config.burstCommands = 2;
			config.burstPauseMs = 5000;
			// Send-only, so Open itself sends nothing (its queries would sit
			// out the gap and pauses); the range comes from light_min.
			config.readBack = false;
			config.terminator = Terminator::Cr;
			config.lightMin = 60;
			auto driver = OpenDriver(projector, config);
			const auto start = std::chrono::steady_clock::now();
			for (const int contrast : { 73, 72, 71, 70 })
				Assert::IsTrue(driver->SendScript("CNTR", contrast));
			Assert::IsTrue(driver->SendScript("LPW1", 30)); // clamped to the projector's minimum
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			Assert::IsTrue(elapsed < 400, (L"Script sends waited for the gap: " + std::to_wstring(elapsed)).c_str());
			Assert::AreEqual(70, projector->contrast);
			Assert::AreEqual(60, projector->light);
			Assert::AreEqual(60, driver->Status().lastSentLight);
			const auto writes = projector->Writes();
			Assert::AreEqual(std::string(":CNTR73\r"), writes[writes.size() - 5]);
			Assert::AreEqual(std::string(":LPW160\r"), writes.back());
			// Repeats are sent, not coalesced.
			Assert::IsTrue(driver->SendScript("CNTR", 70));
			Assert::AreEqual(std::string(":CNTR70\r"), projector->Writes().back());
			driver->BeginShutdown();
			const size_t before = projector->Writes().size();
			Assert::IsFalse(driver->SendScript("CNTR", 69));
			Assert::AreEqual(before, projector->Writes().size(), L"Nothing after the exit restore began");
			driver->Close();
		}

		TEST_METHOD(ScriptSendsUseTheScriptsBytes)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->acceptedSuffix = "cr\r\n";
			ProjectorConfig config = FastConfig();
			config.readBack = false;
			config.terminator = Terminator::Script;
			auto driver = OpenDriver(projector, config);
			Assert::IsTrue(driver->SendScript("CNTR", 61));
			Assert::IsTrue(driver->SendScript("LPW1", 230));
			const auto writes = projector->Writes();
			Assert::AreEqual(std::string(":cntr61cr\r\n"), writes[writes.size() - 2]);
			Assert::AreEqual(std::string(":lpw1230cr\r\n"), writes.back());
			driver->Close();
		}

		TEST_METHOD(ScriptStartAndExitSetContrastTo50)
		{
			auto projector = std::make_shared<FakeProjector>();
			projector->contrast = 50;
			auto driver = OpenDriver(projector);
			BringToKnownState(*driver, 255);
			for (const auto& write : projector->Writes())
				Assert::AreNotEqual(std::string(":CNTR50\r"), write, L"compensate: CNTR left alone at 50");
			BringToKnownState(*driver, 255, false, true);
			Assert::AreEqual(std::string(":CNTR50\r"), projector->Writes().back(), L"script: CNTR 50 as the script did");
			Assert::IsTrue(driver->SendScript("CNTR", 66));
			Assert::IsTrue(RestoreFullLight(*driver, 255, false, true));
			const auto writes = projector->Writes();
			Assert::AreEqual(std::string(":LPW1255\r"), writes[writes.size() - 2], L"Light first");
			Assert::AreEqual(std::string(":CNTR50\r"), writes.back(), L"then contrast back to 50");
			Assert::AreEqual(50, projector->contrast);
			driver->Close();
		}

		TEST_METHOD(RestoreOnExitNeverBlocksForLong)
		{
			auto projector = std::make_shared<FakeProjector>();
			ProjectorConfig config = FastConfig();
			config.ackTimeoutMs = 5000;
			config.minCommandGapMs = 5000;
			auto driver = OpenDriver(projector, config);
			projector->silent = true;
			const auto start = std::chrono::steady_clock::now();
			Assert::IsFalse(RestoreFullLight(*driver, 255));
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			Assert::IsTrue(elapsed < 1150, L"Exit restore must give up within a second");
			Assert::AreEqual(std::string(":LPW1255\r"), LightCommands(projector->Writes()).back());
		}

		TEST_METHOD(ConfigurationDefaultsMatchTheScriptAndValidationIsStrict)
		{
			const LedDriveConfig defaults;
			Assert::IsFalse(defaults.enabled);
			Assert::IsTrue(defaults.mode == Mode::LogOnly);
			Assert::AreEqual(7, defaults.stepsUpDark.StepFor(100));
			Assert::AreEqual(5, defaults.stepsUpDark.StepFor(99));
			Assert::AreEqual(4, defaults.stepsUpBright.StepFor(50));
			Assert::AreEqual(2, defaults.stepsUpBright.StepFor(25));
			Assert::AreEqual(1, defaults.stepsUpDark.StepFor(24), L"The script steps 1 for distance 10-24");
			Assert::AreEqual(1, defaults.stepsUpBright.StepFor(3));
			Assert::AreEqual(5, defaults.stepsDown.StepFor(150));
			Assert::AreEqual(3, defaults.stepsDown.StepFor(60));
			Assert::AreEqual(2, defaults.stepsDown.StepFor(30));
			Assert::AreEqual(1, defaults.stepsDown.StepFor(0));
			Assert::AreEqual(0.114, defaults.aplWeights[0], 1e-12);
			Assert::AreEqual(0.299, defaults.aplWeights[2], 1e-12);

			TemporaryConfig good("[projector.Default]\nprotocol: barco_ascii\nport: COM3\nbaud: 19200\n"
				"light_min: auto\nlight_max: 240\nack_timeout_ms: 300\nmin_command_gap_ms: 100\necho: on\n"
				"read_back: on\nburst_commands: 20\nburst_pause_ms: 5000\n"
				"terminator: literal_cr\n"
				"[led_drive.Default]\nenabled: false\nmode: log_only\ntick_ms: 400\nled_floor: 100\n"
				"led_floor_black: 60\nled_ceiling: 255\ncurve: 0:100, 20:100, 40:200, 70:230, 100:255, 255:255\n"
				"steps_up_dark: 100:7, 50:5, 25:4, 10:1, 0:1\nsteps_down: 100:5, 50:3, 25:2, 10:1, 0:1\n"
				"apl_weights: 0.299, 0.587, 0.114\ncompensation_eotf: 2.2\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(good.path));
			std::string error;
			Assert::IsTrue(MainConfigSchema::Validate(config, error),
				std::wstring(error.begin(), error.end()).c_str());
			Assert::IsTrue(MainConfigSchema::OwnsSection("projector.default"));
			Assert::IsTrue(MainConfigSchema::OwnsSection("led_drive.default"));
			const ProjectorConfig projector = ReadProjectorConfig(config);
			Assert::AreEqual(std::string("COM3"), projector.port);
			Assert::AreEqual(-1, projector.lightMin);
			Assert::AreEqual(240, projector.lightMax);
			Assert::IsTrue(projector.terminator == Terminator::LiteralCr);
			Assert::IsTrue(projector.readBack);
			Assert::AreEqual(20, projector.burstCommands);
			Assert::AreEqual(5000, projector.burstPauseMs);
			const ProjectorConfig projectorDefaults;
			Assert::AreEqual(200, projectorDefaults.minCommandGapMs, L"The old script's proven pace (Barco documents 500)");
			Assert::AreEqual(0, projectorDefaults.burstCommands, L"No pause unless asked for");
			Assert::IsFalse(projectorDefaults.readBack, L"Send-only, like the old script");
			const LedDriveConfig led = ReadLedDriveConfig(config);
			Assert::IsTrue(led.mode == Mode::LogOnly);
			Assert::AreEqual(0.299, led.aplWeights[0], 1e-12);
			Assert::AreEqual(size_t{ 6 }, led.curve.size());

			TemporaryConfig commented("[projector.Default]\nprotocol: barco_ascii        # barco_ascii | none\n");
			ConfigFile withComment;
			Assert::IsTrue(withComment.Load(commented.path));
			Assert::IsTrue(MainConfigSchema::Validate(withComment, error), L"Inline comments as in the guide");

			for (const char* bad : {
				"[led_drive.Default]\nmode: contrast\n",
				"[led_drive.Default]\nled_floor: 200\nled_ceiling: 150\n",
				"[led_drive.Default]\nsteps_down: 100:5, 50:3\n",
				"[led_drive.Default]\nsteps_down: 10:1, 50:3, 0:1\n",
				"[led_drive.Default]\napl_weights: 0.5, 0.5, 0.5\n",
				"[led_drive.Default]\nunknown_key: 1\n",
				"[projector.Default]\nport: LPT1\n" })
			{
				TemporaryConfig file(bad);
				ConfigFile rejected;
				Assert::IsTrue(rejected.Load(file.path));
				Assert::IsFalse(MainConfigSchema::Validate(rejected, error),
					std::wstring(bad, bad + strlen(bad)).c_str());
			}
		}
	};
}
