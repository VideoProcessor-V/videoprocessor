#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <led/BarcoAsciiDriver.h>
#include <led/LedDriveConfig.h>
#include <led/ProjectorDriver.h>

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
	// A stand-in FL35 behind a fake serial port. It answers ":MNEM..." commands
	// framed with acceptedSuffix with "%001 MNEM 000nnn" replies.
	struct FakeProjector
	{
		std::mutex mutex;
		std::condition_variable changed;
		std::vector<std::string> writes;
		std::deque<std::string> replies;
		std::string acceptedSuffix = "\r";
		bool silent = false;
		bool holdReplies = false;
		bool openFails = false;
		int opens = 0;
		int light = 120;
		int contrast = 74;
		int minimum = 60;
		int maximum = 255;

		static std::string Reply(const char* mnemonic, int value)
		{
			char text[32];
			sprintf_s(text, "%%001 %s %06d", mnemonic, value);
			return text;
		}

		void Receive(const std::string& bytes)
		{
			std::lock_guard<std::mutex> guard(mutex);
			writes.push_back(bytes);
			changed.notify_all();
			if (silent || bytes.size() < acceptedSuffix.size() ||
				bytes.compare(bytes.size() - acceptedSuffix.size(), acceptedSuffix.size(), acceptedSuffix) != 0)
				return;
			const std::string body = bytes.substr(0, bytes.size() - acceptedSuffix.size());
			// "\r" framing must not also accept the literal "cr" variant.
			if (acceptedSuffix == "\r" && body.size() >= 2 && body.compare(body.size() - 2, 2, "cr") == 0)
				return;
			if (body == ":ECHO1") replies.push_back(Reply("ECHO", 1));
			else if (body == ":LPW1?") replies.push_back(Reply("LPW1", light));
			else if (body == ":LPW1 ?N") replies.push_back(Reply("LPW1", minimum));
			else if (body == ":LPW1 ?M") replies.push_back(Reply("LPW1", maximum));
			else if (body == ":CNTR?") replies.push_back(Reply("CNTR", contrast));
			else if (body.rfind(":LPW1", 0) == 0) { light = std::stoi(body.substr(5)); replies.push_back(Reply("LPW1", light)); }
			else if (body.rfind(":CNTR", 0) == 0) { contrast = std::stoi(body.substr(5)); replies.push_back(Reply("CNTR", contrast)); }
			changed.notify_all();
		}

		std::vector<std::string> Writes()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return writes;
		}

		bool WaitForWrites(size_t count, int timeoutMs = 2000)
		{
			std::unique_lock<std::mutex> lock(mutex);
			return changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return writes.size() >= count; });
		}

		void Release()
		{
			std::lock_guard<std::mutex> guard(mutex);
			holdReplies = false;
			changed.notify_all();
		}
	};

	class FakePort : public ISerialPort
	{
	public:
		explicit FakePort(std::shared_ptr<FakeProjector> projectorIn) : projector(std::move(projectorIn)) {}
		bool Open(const std::string&, int, std::string& error) override
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			++projector->opens;
			if (projector->openFails) { error = "cannot open COM9"; return false; }
			open = true;
			return true;
		}
		void Close() override { open = false; }
		bool IsOpen() const override { return open; }
		void DiscardInput() override
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			projector->replies.clear();
		}
		bool Write(const std::string& bytes, std::string&) override
		{
			projector->Receive(bytes);
			return true;
		}
		bool ReadLine(int timeoutMs, std::string& line) override
		{
			std::unique_lock<std::mutex> lock(projector->mutex);
			if (!projector->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return !projector->holdReplies && !projector->replies.empty(); }))
				return false;
			line = projector->replies.front();
			projector->replies.pop_front();
			return true;
		}

	private:
		std::shared_ptr<FakeProjector> projector;
		bool open = false;
	};

	ProjectorConfig FastConfig()
	{
		ProjectorConfig config;
		config.protocol = "barco_ascii";
		config.ackTimeoutMs = 60;
		config.minCommandGapMs = 0;
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
