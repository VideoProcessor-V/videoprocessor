#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <led/BarcoAsciiDriver.h>
#include <led/ConnectionTest.h>
#include <led/DeviceProfile.h>
#include <led/LedController.h>
#include <led/LedDriveConfig.h>
#include <led/LedDriveModule.h>
#include <led/ProjectorDriver.h>

#include "LedDriveReferenceDriver.h"
#include "LedDriveTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LedDrive;

// Dynamic Drive device profiles (feat/dd-projector-profiles, 2026-10-17):
// - the shipped profiles\dynamic-drive\barco-fl35.vpprojector loads to exactly
//   the compiled-in FL35 profile and the configuration VP read before profiles;
// - with it, the profile-driven driver writes byte-identical command streams to
//   a frozen copy of the pre-profile BarcoAsciiDriver (LedDriveReferenceDriver)
//   on the LED test sequences (script stream, full ranges, custom dim, split
//   drops, startup and exit restore, every framing);
// - the strict validator refuses bad profiles with clear messages, and a refused
//   profile never reaches the port;
// - the restore-on-exit values come from the profile;
// - a second, synthetic projector drives the driver and the planner with its own
//   syntax, ranges, latency, settle time and step limits.
// CPU only.
namespace
{
	std::string Repository()
	{
		std::string repository = __FILE__;
		for (int level = 0; level < 3; ++level)
			repository.resize(repository.find_last_of("\\/"));
		return repository;
	}

	std::string Fl35File() { return Repository() + "\\profiles\\dynamic-drive\\barco-fl35.vpprojector"; }
	std::string SyntheticFile() { return Repository() + "\\test-fixtures\\dynamic-drive\\synthetic-percent.vpprojector"; }

	std::string TempFile(const std::string& text, const char* extension = ".vpprojector")
	{
		char directory[MAX_PATH]{}, file[MAX_PATH]{};
		GetTempPathA(MAX_PATH, directory);
		GetTempFileNameA(directory, "vdd", 0, file);
		DeleteFileA(file);
		const std::string path = std::string(file) + extension;
		std::ofstream(path, std::ios::trunc | std::ios::binary) << text;
		return path;
	}

	std::string ReadAll(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

	bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 5000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return condition();
	}

	DeviceProfile LoadOrFail(const std::string& path)
	{
		DeviceProfile profile;
		std::string error;
		Assert::IsTrue(LoadDeviceProfile(path, profile, error), Wide(error).c_str());
		return profile;
	}

	// Every ProjectorConfig field: "" when equal, else the first difference.
	std::string ConfigDifference(const ProjectorConfig& a, const ProjectorConfig& b)
	{
#define VP_DD_FIELD(field) if (!(a.field == b.field)) return #field;
		VP_DD_FIELD(protocol) VP_DD_FIELD(port) VP_DD_FIELD(baud) VP_DD_FIELD(lightMin) VP_DD_FIELD(lightMax)
		VP_DD_FIELD(ackTimeoutMs) VP_DD_FIELD(minCommandGapMs) VP_DD_FIELD(maxLevelsPerCommand)
		VP_DD_FIELD(burstCommands) VP_DD_FIELD(burstPauseMs) VP_DD_FIELD(readBack) VP_DD_FIELD(echo)
		VP_DD_FIELD(terminator) VP_DD_FIELD(ledLatencyMs) VP_DD_FIELD(ledSettleMs) VP_DD_FIELD(cntrLatencyMs)
		VP_DD_FIELD(cntrSettleMs) VP_DD_FIELD(cntrMaxUsable) VP_DD_FIELD(dmdClipCntr) VP_DD_FIELD(fluxTable)
		VP_DD_FIELD(deviceProfileError)
#undef VP_DD_FIELD
		if (a.TerminatorName() != b.TerminatorName()) return "terminatorName";
		const std::string device = DeviceProfileDifference(a.device, b.device);
		return device.empty() ? std::string() : "device." + device;
	}

	ConfigFile LoadConfig(const std::string& text, std::string& path)
	{
		path = TempFile(text, ".cfg");
		ConfigFile config;
		Assert::IsTrue(config.Load(path, ConfigFile::ReadPolicy::Fresh));
		return config;
	}

	// The script's stream on a varied programme (ramps, blacks, highlights,
	// cuts): VP's internal_script copy, paced as the module paces it.
	std::vector<std::pair<std::string, int>> ScriptStream()
	{
		LedDriveConfig config;
		config.mode = Mode::Script;
		ScriptController controller(config);
		controller.Reset(255, 50);
		std::vector<std::pair<std::string, int>> stream;
		for (int tick = 0; tick < 240; ++tick)
		{
			FrameStats stats;
			const double drift = 60.0 + 45.0 * std::sin(tick / 9.0);
			stats.apl = (tick / 30) % 3 == 1 ? 17.0 : drift;
			stats.sustainedApl = stats.apl;
			stats.maximum = (tick / 7) % 2 ? 230.0 : 150.0;
			stats.highlight = std::min(stats.maximum, std::max(stats.apl, 16.0));
			stats.cut = tick % 30 == 0;
			stats.blocks = 100;
			const ControllerOutput out = controller.Tick(stats);
			std::vector<ScriptSend> sends;
			PaceScriptTick(out, config.scriptMeasureMs, config.scriptEchoMs, sends);
			for (const ScriptSend& send : sends)
				stream.push_back({ send.light ? "LPW1" : "CNTR", send.value });
		}
		return stream;
	}

	enum class Responder { Cr, LiteralCr, Silent };

	struct Variant
	{
		const wchar_t* name;
		bool readBack;
		Terminator terminator;
		bool echo;
		Responder responder;
		bool full;   // the full sweep (fast variants only)
	};

	const Variant VARIANTS[] = {
		{ L"read_back off, auto (the old script's bytes)", false, Terminator::Auto, true, Responder::Cr, true },
		{ L"read_back off, cr", false, Terminator::Cr, true, Responder::Cr, true },
		{ L"read_back off, literal_cr", false, Terminator::LiteralCr, true, Responder::Cr, true },
		{ L"read_back on, auto, answers cr", true, Terminator::Auto, true, Responder::Cr, true },
		{ L"read_back on, auto, answers literal cr", true, Terminator::Auto, true, Responder::LiteralCr, false },
		{ L"read_back on, cr, echo off", true, Terminator::Cr, false, Responder::Cr, false },
		{ L"read_back on, script", true, Terminator::Script, true, Responder::Cr, false },
		{ L"read_back on, auto, silent (falls back to the script's bytes)", true, Terminator::Auto, true, Responder::Silent, false },
	};

	// One run of the LED test sequence against a driver; returns the bytes the
	// projector received.
	std::vector<std::string> RunSequence(bool reference, const ProjectorConfig& configIn, const Variant& variant)
	{
		ProjectorConfig config = configIn;
		config.protocol = "barco_ascii";
		config.readBack = variant.readBack;
		config.terminator = variant.terminator;
		config.echo = variant.echo;
		config.ackTimeoutMs = 40;
		config.minCommandGapMs = 0;
		auto projector = std::make_shared<FakeProjector>();
		projector->acceptedSuffix = variant.responder == Responder::LiteralCr ? "cr\r" : "\r";
		projector->silent = variant.responder == Responder::Silent;
		projector->light = 200;
		projector->contrast = 74;
		std::unique_ptr<IProjectorDriver> driver;
		if (reference) driver = std::make_unique<ReferenceBarcoAsciiDriver>(config, std::make_unique<FakePort>(projector));
		else driver = std::make_unique<BarcoAsciiDriver>(config, std::make_unique<FakePort>(projector));
		std::string error;
		Assert::IsTrue(driver->Open(error), Wide(error).c_str());
		BringToKnownState(*driver, 255, true, true);
		// The script's stream, then (fast variants) every level of each command.
		for (const auto& send : ScriptStream()) driver->SendScript(send.first, send.second);
		if (variant.full)
		{
			for (int level = 0; level <= 255; ++level) driver->SendScript("LPW1", level);
			for (int contrast = 50; contrast <= 100; ++contrast) driver->SendScript("CNTR", contrast);
		}
		// The experimental custom dim (in, turns, out).
		const int done = driver->Status().sequencesDone;
		std::vector<std::pair<std::string, int>> dim;
		for (const char* channel : { "LDCR", "LDCG", "LDCB" }) dim.push_back({ channel, 40 });
		dim.push_back({ "LDMM", 1 });
		for (int level = 35; level >= 0; level -= 5)
			for (const char* channel : { "LDCR", "LDCG", "LDCB" }) dim.push_back({ channel, level });
		Assert::IsTrue(driver->QueueSequence(dim, 1));
		Assert::IsTrue(WaitFor([&] { return driver->Status().sequencesDone > done && !driver->Status().sequenceBusy; }, 20000));
		Assert::IsTrue(driver->QueueSequenceNow({ { "LDMM", 0 } }, 1));
		Assert::IsTrue(WaitFor([&] { return driver->Status().sequencesDone > done + 1; }, 20000));
		// Compensate's targets: a split dimming drop, a cut jump, a CNTR crossing.
		driver->SetLightNow(255, 0);
		driver->SetLight(150);
		Assert::IsTrue(WaitFor([&] { return driver->Status().lastSentLight == 150 && !driver->Status().busy; }, 20000));
		driver->SetContrastAsync(61);
		Assert::IsTrue(WaitFor([&] { return driver->Status().lastSentContrast == 61 && !driver->Status().busy; }, 20000));
		driver->SetLightJump(90);
		Assert::IsTrue(WaitFor([&] { return driver->Status().lastSentLight == 90 && !driver->Status().busy; }, 20000));
		driver->SendNow("LDMM", 0, 0);
		driver->SetContrast(50);
		// The exit restore.
		RestoreFullLight(*driver, 255, true, true);
		driver->Close();
		return projector->Writes();
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(DeviceProfileTests)
	{
	public:
		// The shipped FL35 file = the compiled-in profile = what VP read before
		// profiles (ProjectorConfig and LedDriveConfig, with and without user
		// overrides).
		TEST_METHOD(Fl35FileEqualsTheBuiltInProfileAndConfiguration)
		{
			const DeviceProfile file = LoadOrFail(Fl35File());
			const DeviceProfile builtIn;
			Assert::AreEqual(std::string(), DeviceProfileDifference(builtIn, file),
				L"barco-fl35.vpprojector differs from the compiled-in FL35 profile in this field");
			Assert::AreEqual(std::string("Barco"), file.maker);
			Assert::AreEqual(1, file.schema);
			Assert::IsFalse(file.BuiltIn());

			ProjectorConfig applied;
			ApplyDeviceProfile(builtIn, applied);
			Assert::AreEqual(std::string(), ConfigDifference(ProjectorConfig(), applied),
				L"the built-in profile changes no ProjectorConfig default");

			for (const char* overrides : { "", "baud: 9600\nmin_command_gap_ms: 500\nled_latency_ms: 120\nread_back: yes\n"
				"terminator: literal_cr\nmax_levels_per_command: 6\ncntr_max_usable: 70\nflux_table: 60:0.5, 255:1\n" })
			{
				std::string plainPath, filePath;
				const std::string base = std::string("[projector.Default]\nprotocol: barco_ascii\nport: COM3\n") + overrides;
				const ConfigFile plain = LoadConfig(base, plainPath);
				const ConfigFile withFile = LoadConfig(base + "device_profile: " + Fl35File() + "\n", filePath);
				std::string error;
				Assert::IsTrue(Validate(withFile, error), Wide(error).c_str());
				const ProjectorConfig a = ReadProjectorConfig(plain);
				ProjectorConfig b = ReadProjectorConfig(withFile);
				Assert::IsTrue(b.deviceProfileError.empty(), Wide(b.deviceProfileError).c_str());
				Assert::AreEqual(std::string("barco_ascii"), b.protocol);
				b.device.path.clear();   // the loaded file's own path is the only difference
				b.device.maker = a.device.maker;
				Assert::AreEqual(std::string(), ConfigDifference(a, b), L"configuration read with the FL35 file");
				const LedDriveConfig ledA = ReadLedDriveConfig(plain);
				const LedDriveConfig ledB = ReadLedDriveConfig(withFile);
				Assert::AreEqual(ledA.ledFloor, ledB.ledFloor);
				Assert::AreEqual(ledA.ledFloorReactive, ledB.ledFloorReactive);
				Assert::AreEqual(ledA.ledFloorBlack, ledB.ledFloorBlack);
				Assert::AreEqual(ledA.cntrAssist, ledB.cntrAssist);
				Assert::IsTrue(ledA.ftbCustomDim == ledB.ftbCustomDim);
				Assert::AreEqual(255, b.RestoreLight(255));
				Assert::AreEqual(230, b.RestoreLight(230), L"FL35: restore = led_ceiling, as before");
				DeleteFileA(plainPath.c_str());
				DeleteFileA(filePath.c_str());
			}
		}

		// The FL35 reply grammar ([reply] pattern) accepts exactly what the
		// driver's ParseReply accepted.
		TEST_METHOD(Fl35ReplyPatternMatchesTheBuiltInParser)
		{
			const DeviceProfile profile = LoadOrFail(Fl35File());
			const char* lines[] = { "%001 LPW1 000255", "%001 LPW1 255", " %001  CNTR\t000074 ", "%001 ECHO 000001",
				"%001 LPW1 000255 x", "%01 LPW1 000255", "%0011 LPW1 1", "001 LPW1 1", "%001 LPW 1", "%001 LPW1X 1",
				"%001 LP-1 1", "%001 LPW1 ", "%001 LPW1 1234567890", "%001 LPW1 123456789", "%001 LPW1 -1",
				"%001 LPW1 1a", "", "%abc LPW1 1", "%001 ldmm 000001", "%001 LPW1 000255\n", "random text" };
			for (const char* line : lines)
			{
				std::string oldName, newName;
				int oldValue = -7, newValue = -7;
				const bool oldOk = BarcoAsciiDriver::ParseReply(line, oldName, oldValue);
				const bool newOk = MatchDeviceReply(profile, line, newName, newValue);
				Assert::AreEqual(oldOk, newOk, Wide(std::string("reply \"") + line + "\"").c_str());
				if (oldOk)
				{
					Assert::AreEqual(oldName, newName);
					Assert::AreEqual(oldValue, newValue);
				}
			}
		}

		// The core equivalence: the profile-driven driver with the shipped FL35
		// file writes exactly the bytes the pre-profile driver wrote.
		TEST_METHOD(Fl35ProfileCommandStreamIsByteIdenticalToTheBuiltInDriver)
		{
			ProjectorConfig fl35;
			ApplyDeviceProfile(LoadOrFail(Fl35File()), fl35);
			for (const Variant& variant : VARIANTS)
			{
				const std::vector<std::string> before = RunSequence(true, ProjectorConfig(), variant);
				const std::vector<std::string> after = RunSequence(false, fl35, variant);
				Assert::IsTrue(before.size() > 60, (std::wstring(L"the sequence ran: ") + variant.name).c_str());
				const size_t common = std::min(before.size(), after.size());
				for (size_t i = 0; i < common; ++i)
					if (before[i] != after[i])
						Assert::Fail((std::wstring(L"command ") + std::to_wstring(i) + L" differs (" + variant.name + L"): \"" +
							Wide(DeviceProfileDetail::Printable(before[i])) + L"\" vs \"" +
							Wide(DeviceProfileDetail::Printable(after[i])) + L"\"").c_str());
				Assert::AreEqual(before.size(), after.size(), variant.name);
			}
		}

		// The compiled-in path (no device_profile) is unchanged as well.
		TEST_METHOD(BuiltInProfilePathIsByteIdenticalToTheBuiltInDriver)
		{
			for (const Variant& variant : VARIANTS)
			{
				if (variant.responder == Responder::Silent) continue;   // covered above; slow
				Assert::IsTrue(RunSequence(true, ProjectorConfig(), variant) == RunSequence(false, ProjectorConfig(), variant),
					variant.name);
			}
		}

		// The strict schema: each bad profile is refused with a message naming
		// the problem.
		TEST_METHOD(BadProfilesAreRefusedWithClearMessages)
		{
			std::string good = ReadAll(SyntheticFile());
			good.erase(std::remove(good.begin(), good.end(), '\r'), good.end());   // a CRLF checkout
			Assert::IsFalse(good.empty());
			auto replace = [&](const std::string& from, const std::string& to)
			{
				std::string text = good;
				const size_t at = text.find(from);
				Assert::IsTrue(at != std::string::npos, Wide("fixture has " + from).c_str());
				text.replace(at, from.size(), to);
				return text;
			};
			struct Case { std::string text; const char* expected; };
			const Case cases[] = {
				{ replace("schema = 1", "schema = 2"), "[profile] key 'schema'" },
				{ replace("maker = Example\n", ""), "[profile] maker is required" },
				{ good + "\n[power]\nset = POWR0\n", "unknown section [power]" },
				{ replace("baud = 9600", "baud = 9600\ncolour = blue"), "unknown key 'colour'" },
				{ replace("baud = 9600", "baud = 300"), "[transport] key 'baud'" },
				{ replace("min = 0\nmax = 100", "min = 100\nmax = 100"), "[light] needs min < max" },
				{ replace("floor = 40", "floor = 140"), "floor 140 is outside min..max" },
				{ replace("floor_black = 20", "floor_black = 45"), "floor_black <= floor" },
				{ replace("light = 90", "light = 101"), "[restore] light 101 is outside" },
				{ replace("max_step = 4", "max_step = 200"), "max_step is larger than the whole [light] range" },
				{ replace("set = LIGHT={v:03d}", "set = LIGHT=050"), "[command.light] key 'set'" },
				{ replace("set = LIGHT={v:03d}", "set = LIGHT={x}"), "[command.light] key 'set'" },
				{ replace("set = LIGHT={v:03d}", "set = LIGHT=\\q{v}"), "[command.light] key 'set'" },
				{ replace("set = LIGHT={v:03d}", "set = POWR{v}"), "never sends power" },
				{ replace("send_only = plain", "send_only = other"), "send_only names no [framing.other]" },
				{ replace("probe = no", "probe = yes"), "probe = yes needs [transport] read_back_supported = yes" },
				{ replace("read_back = no", "read_back = yes"), "read_back = yes needs read_back_supported = yes" },
				{ good + "\n[reply]\npattern = ^(\\w+) (\\d+)$\n", "need [transport] read_back_supported = yes" },
				{ good + "\n[contrast]\nmin = 0\nmax = 10\n", "[contrast] given without [command.contrast]" },
				{ good + "\n[command.contrast]\nset = CON{v}\n", "needs [contrast] min and max and [restore] contrast" },
				{ good + "\n[command.contrast]\nset = CON{v}\n[contrast]\nmin = 0\nmax = 10\n[restore]\ncontrast = 5\n",
					"duplicate section [restore]" },
				{ good + "\n[command.custom_red]\nset = R{v}\nmin = 0\nmax = 40\n", "the custom dim needs all of" },
				{ replace("min_command_gap_ms = 350", "min_command_gap_ms = 350\nmin_command_gap_ms = 300"), "duplicate" },
				{ replace("[timing]", "[timing]\nlatency_ms = abc"), "duplicate" },
				{ replace("latency_ms = 250", "latency_ms = 5000"), "[timing] key 'latency_ms'" },
			};
			for (const Case& c : cases)
			{
				const std::string path = TempFile(c.text);
				DeviceProfile profile;
				std::string error;
				const bool loaded = LoadDeviceProfile(path, profile, error);
				DeleteFileA(path.c_str());
				Assert::IsFalse(loaded, Wide(std::string("accepted; expected: ") + c.expected).c_str());
				Assert::IsTrue(error.find(c.expected) != std::string::npos,
					Wide(std::string("expected \"") + c.expected + "\" in: " + error).c_str());
				Assert::IsTrue(error.find("device profile ") == 0, Wide(error).c_str());
			}
			// A missing file.
			DeviceProfile profile;
			std::string error;
			Assert::IsFalse(LoadDeviceProfile(Repository() + "\\no-such.vpprojector", profile, error));
			Assert::IsTrue(error.find("cannot read") != std::string::npos);
			// The offline report.
			std::string report;
			Assert::IsTrue(ValidateDeviceProfileReport(Fl35File(), report), Wide(report).c_str());
			Assert::IsTrue(report.rfind("OK ", 0) == 0);
			Assert::IsTrue(report.find("\":lpw1255cr\\r\\n\"") != std::string::npos, Wide(report).c_str());
			const std::string bad = TempFile(replace("min = 0\nmax = 100", "min = 100\nmax = 100"));
			Assert::IsFalse(ValidateDeviceProfileReport(bad, report));
			Assert::IsTrue(report.rfind("INVALID ", 0) == 0, Wide(report).c_str());
			DeleteFileA(bad.c_str());
		}

		// A refused profile: no Dynamic Drive - protocol none, the reason kept,
		// the port never opened, the connection test refuses too.
		TEST_METHOD(RefusedProfileNeverReachesTheProjector)
		{
			const std::string badProfile = TempFile(ReadAll(SyntheticFile()) + "\n[lamp]\nset = LAMP0\n");
			std::string cfgPath;
			const ConfigFile config = LoadConfig("[projector.Default]\nprotocol: barco_ascii\ndevice_profile: " + badProfile +
				"\n[led_drive.Default]\nenabled: yes\ncommand_source: compensate\n", cfgPath);
			std::string error;
			Assert::IsTrue(Validate(config, error), L"the main configuration stays valid (VP starts)");
			const ProjectorConfig projector = ReadProjectorConfig(config);
			Assert::AreEqual(std::string("none"), projector.protocol);
			Assert::IsTrue(projector.deviceProfileError.find("unknown section [lamp]") != std::string::npos,
				Wide(projector.deviceProfileError).c_str());
			Assert::AreEqual(projector.deviceProfileError, DeviceProfileProblem(config));
			const LedDriveConfig led = ReadLedDriveConfig(config);
			Assert::AreEqual(std::string("none"), EffectiveProtocol(projector, led));
			auto fake = std::make_shared<FakeProjector>();
			{
				LedDriveModule module(projector, led, [fake] { return std::make_unique<FakePort>(fake); });
				module.Start();
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				Assert::IsFalse(module.DrivingProjector());
				module.Stop();
			}
			Assert::AreEqual(0, fake->opens, L"the port is never opened");
			Assert::IsTrue(fake->Writes().empty(), L"nothing is sent");
			const ConnectionTestResult test = RunConnectionTestOnNewConnection(projector,
				[fake] { return std::make_unique<FakePort>(fake); });
			Assert::IsFalse(test.pass);
			Assert::IsTrue(test.summary.find("device profile refused") != std::string::npos, Wide(test.summary).c_str());
			Assert::AreEqual(0, fake->opens);
			// A terminator the profile does not have, and read-back it cannot do.
			const std::string synthetic = SyntheticFile();
			for (const char* extra : { "terminator: cr\n", "read_back: yes\n" })
			{
				std::string path;
				const ConfigFile mismatch = LoadConfig(std::string("[projector.Default]\nprotocol: barco_ascii\ndevice_profile: ") +
					synthetic + "\n" + extra, path);
				const ProjectorConfig refused = ReadProjectorConfig(mismatch);
				Assert::AreEqual(std::string("none"), refused.protocol, Wide(extra).c_str());
				Assert::IsFalse(refused.deviceProfileError.empty());
				DeleteFileA(path.c_str());
			}
			// Out of the declared range: rendered nothing, sent nothing (FL35:
			// CNTR is 50-100, the custom dim 0-40, dim mode 0/1).
			ProjectorConfig fl35;
			ApplyDeviceProfile(LoadOrFail(Fl35File()), fl35);
			fl35.protocol = "barco_ascii";
			fl35.minCommandGapMs = 0;
			auto range = std::make_shared<FakeProjector>();
			BarcoAsciiDriver driver(fl35, std::make_unique<FakePort>(range));
			Assert::IsTrue(driver.Open(error));
			const size_t before = range->Writes().size();
			bool written = true;
			Assert::IsFalse(driver.SendScript("CNTR", 101, &written));
			Assert::IsFalse(written);
			Assert::IsFalse(driver.SendScript("CNTR", 49, &written));
			Assert::IsFalse(driver.SendNow("LDCR", 41, 0));
			Assert::IsFalse(driver.SendNow("LDMM", 2, 0));
			Assert::IsFalse(driver.SendNow("POWR", 0, 0), L"an unknown command id");
			Assert::AreEqual(before, range->Writes().size(), L"nothing out of range was written");
			Assert::IsTrue(driver.SendScript("CNTR", 100, &written) && written);
			driver.Close();
			DeleteFileA(badProfile.c_str());
			DeleteFileA(cfgPath.c_str());
		}

		// The restore-on-exit values: the profile's (FL35: led_ceiling and CNTR
		// 50; the synthetic projector: its 90 %), sent by the module's exit.
		TEST_METHOD(ExitRestoresTheProfilesValues)
		{
			// FL35 file, driver level: LDMM 0, the ceiling, CNTR 50 - its bytes.
			ProjectorConfig fl35;
			ApplyDeviceProfile(LoadOrFail(Fl35File()), fl35);
			fl35.protocol = "barco_ascii";
			fl35.minCommandGapMs = 0;
			{
				auto fake = std::make_shared<FakeProjector>();
				BarcoAsciiDriver driver(fl35, std::make_unique<FakePort>(fake));
				std::string error;
				Assert::IsTrue(driver.Open(error));
				driver.SendScript("CNTR", 62);
				const size_t mark = fake->Writes().size();
				RestoreOutcome outcome;
				RestoreFullLight(driver, fl35.RestoreLight(255), true, true, &outcome);
				Assert::IsTrue(outcome.Confirmed());
				const auto writes = fake->Writes();
				const std::vector<std::string> expected = { ":ldmm0cr\r\n", ":lpw1255cr\r\n", ":cntr50cr\r\n" };
				Assert::IsTrue(std::vector<std::string>(writes.begin() + mark, writes.end()) == expected);
				driver.Close();
			}
			// The synthetic projector, through the module's start and exit.
			std::string cfgPath;
			const ConfigFile config = LoadConfig("[projector.Default]\nprotocol: barco_ascii\nport: COM4\ndevice_profile: " +
				SyntheticFile() + "\nmin_command_gap_ms: 0\n[led_drive.Default]\nenabled: yes\ncommand_source: compensate\n"
				"ftb_custom_dim: together\n", cfgPath);
			const ProjectorConfig projector = ReadProjectorConfig(config);
			Assert::IsTrue(projector.deviceProfileError.empty(), Wide(projector.deviceProfileError).c_str());
			Assert::AreEqual(230, projector.RestoreLight(255), L"90 % in VP levels");
			const LedDriveConfig led = ReadLedDriveConfig(config);
			Assert::IsFalse(led.cntrAssist, L"no contrast command: no CNTR assist");
			Assert::IsTrue(led.ftbCustomDim == FtbCustomDim::Off, L"no custom dim commands: custom dim off");
			auto fake = std::make_shared<FakeProjector>();
			{
				LedDriveModule module(projector, led, [fake] { return std::make_unique<FakePort>(fake); });
				module.Start();
				Assert::IsTrue(WaitFor([&] { return module.DrivingProjector(); }));
				Assert::IsTrue(module.Stop(), L"the restore was written");
			}
			const auto writes = fake->Writes();
			Assert::IsTrue(writes.size() >= 2, L"start and exit");
			Assert::AreEqual(std::string("*LIGHT=100\r"), writes.front(), L"start: the ceiling, 100 %");
			Assert::AreEqual(std::string("*LIGHT=090\r"), writes.back(), L"exit: the profile's restore value");
			for (const std::string& write : writes)
				Assert::IsTrue(write.rfind("*LIGHT=", 0) == 0, Wide("only light commands: " + write).c_str());
			DeleteFileA(cfgPath.c_str());
		}

		// A second projector: its own syntax, ranges, timing and step limit in the
		// configuration, the driver and the script planner.
		TEST_METHOD(SyntheticProfileDrivesTheDriverAndPlanner)
		{
			const DeviceProfile synthetic = LoadOrFail(SyntheticFile());
			Assert::AreEqual(9600, synthetic.baud);
			Assert::AreEqual(std::string("even"), synthetic.parity);
			Assert::IsFalse(synthetic.Has(DeviceCommandId::Contrast));
			std::string cfgPath;
			const ConfigFile config = LoadConfig("[projector.Default]\nprotocol: barco_ascii\ndevice_profile: " +
				SyntheticFile() + "\n", cfgPath);
			const ProjectorConfig projector = ReadProjectorConfig(config);
			Assert::IsTrue(projector.deviceProfileError.empty(), Wide(projector.deviceProfileError).c_str());
			Assert::AreEqual(9600, projector.baud);
			Assert::AreEqual(250, projector.ledLatencyMs);
			Assert::AreEqual(600, projector.ledSettleMs);
			Assert::AreEqual(350, projector.minCommandGapMs);
			Assert::AreEqual(10, projector.burstCommands);
			Assert::AreEqual(2000, projector.burstPauseMs);
			Assert::AreEqual(10, projector.maxLevelsPerCommand, L"4 % = 10 VP levels");
			Assert::IsFalse(projector.readBack);
			const LedDriveConfig led = ReadLedDriveConfig(config);
			Assert::AreEqual(102, led.ledFloor, L"40 % floor in VP levels");
			Assert::AreEqual(128, led.ledFloorReactive);
			Assert::AreEqual(51, led.ledFloorBlack);
			// An explicit [led_drive] value still wins over the profile.
			{
				std::string overridePath;
				const ConfigFile overridden = LoadConfig("[projector.Default]\ndevice_profile: " + SyntheticFile() +
					"\nled_latency_ms: 300\n[led_drive.Default]\nled_floor: 110\n", overridePath);
				Assert::AreEqual(110, ReadLedDriveConfig(overridden).ledFloor);
				Assert::AreEqual(300, ReadProjectorConfig(overridden).ledLatencyMs);
				DeleteFileA(overridePath.c_str());
			}

			// The driver: its bytes, its percent scale, its step limit.
			ProjectorConfig fast = projector;
			fast.minCommandGapMs = 0;
			fast.burstCommands = 0;
			auto fake = std::make_shared<FakeProjector>();
			fake->silent = true;
			{
				BarcoAsciiDriver driver(fast, std::make_unique<FakePort>(fake));
				std::string error;
				Assert::IsTrue(driver.Open(error));
				Assert::AreEqual(std::string("plain"), driver.Status().framing);
				Assert::IsFalse(driver.Status().acknowledged);
				BringToKnownState(driver, 255, true, true);   // no dim mode, no contrast: light only
				Assert::IsTrue(driver.SendScript("LPW1", 128));
				driver.SetLightNow(255, 0);
				driver.SetLight(200);
				Assert::IsTrue(WaitFor([&] { return driver.Status().lastSentLight == 200 && !driver.Status().busy; }));
				bool written = true;
				Assert::IsFalse(driver.SendScript("CNTR", 60, &written), L"no contrast command");
				Assert::IsFalse(written);
				RestoreFullLight(driver, fast.RestoreLight(255), true, true);
				driver.Close();
			}
			const std::vector<std::string> expected = {
				"*LIGHT=100\r",                      // start: ceiling
				"*LIGHT=050\r",                      // VP 128 = 50 %
				"*LIGHT=100\r",                      // SetLightNow(255)
				"*LIGHT=096\r", "*LIGHT=092\r", "*LIGHT=088\r", "*LIGHT=084\r", "*LIGHT=080\r",   // 10-level steps
				"*LIGHT=078\r",                      // 200
				"*LIGHT=090\r" };                    // exit: restore 90 %
			const auto writes = fake->Writes();
			std::wstring got;
			for (const std::string& w : writes) got += Wide(DeviceProfileDetail::Printable(w)) + L" ";
			Assert::IsTrue(writes == expected, got.c_str());

			// The script planner with the profile's latency and settle: a fall lands
			// latency after the send, a rise latency + settle after it - other frames
			// than the FL35's, and coverage needs the longer queue.
			auto planFor = [](const ProjectorConfig& p, bool rise, size_t queued, ScriptLookaheadPlan& plan)
			{
				LedDriveConfig config;
				config.mode = Mode::Script;
				ScriptController controller(config);
				FrameStats current;
				current.apl = 50.0;
				current.maximum = 150.0;
				ScriptLookaheadInput in;
				in.onScreenSequence = 100;
				in.sourcePeriodMs = 1000.0 / 24.0;
				in.sendOffsetMs = 150.0;
				in.ledLatencyMs = p.ledLatencyMs;
				in.ledSettleMs = p.ledSettleMs;
				in.cntrLatencyMs = p.cntrLatencyMs;
				in.commandGapMs = p.minCommandGapMs;
				for (uint64_t s = 101; s <= 100 + queued; ++s)
				{
					FrameStats f = current;
					f.apl = rise ? 30.0 + static_cast<double>(s - 100) : static_cast<double>(150 - std::min<uint64_t>(s, 140));
					in.ahead.push_back({ s, f });
				}
				plan = PlanScriptLookahead(config, controller, current, in);
				const double period = in.sourcePeriodMs;
				return static_cast<uint64_t>(100 + std::floor((in.sendOffsetMs + p.ledLatencyMs +
					(rise ? p.ledSettleMs : 0)) / period + 1e-6));
			};
			const ProjectorConfig fl35;
			for (const ProjectorConfig* p : { &fl35, &projector })
			{
				const std::wstring who = p == &fl35 ? L"FL35" : L"synthetic";
				ScriptLookaheadPlan plan;
				uint64_t expectedFrame = planFor(*p, false, 40, plan);
				Assert::IsTrue(plan.kind == ScriptLookaheadPlan::Kind::Ahead, (who + L": fall looked ahead").c_str());
				Assert::AreEqual(expectedFrame, plan.targetSequence, (who + L": a fall lands latency after the send").c_str());
				Assert::IsTrue(plan.covered, (who + L": 40 frames cover latency + settle").c_str());
				expectedFrame = planFor(*p, true, 40, plan);
				Assert::AreEqual(expectedFrame, plan.targetSequence, (who + L": a rise lands when settled").c_str());
			}
			ScriptLookaheadPlan fl35Plan, syntheticPlan;
			planFor(fl35, false, 40, fl35Plan);
			planFor(projector, false, 40, syntheticPlan);
			Assert::IsTrue(syntheticPlan.targetSequence > fl35Plan.targetSequence, L"the slower projector looks further ahead");
			// 20 frames (833 ms) cover the FL35's 150 + 80 + 300 ms but not the synthetic 150 + 250 + 600 ms.
			planFor(fl35, false, 20, fl35Plan);
			planFor(projector, false, 20, syntheticPlan);
			Assert::IsTrue(fl35Plan.covered && fl35Plan.floor == ScriptFloor::Covered, L"FL35 covered by 20 frames");
			Assert::IsTrue(!syntheticPlan.covered && syntheticPlan.floor == ScriptFloor::Reactive,
				L"synthetic not covered by 20 frames: reactive floor");
			DeleteFileA(cfgPath.c_str());
		}
	};
}
