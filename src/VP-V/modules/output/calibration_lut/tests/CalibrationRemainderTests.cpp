#include "pch.h"
#include "CppUnitTest.h"

#include "CalibrationTestSupport.h"

#include <VP-V/modules/output/calibration_lut/calibration/ArgyllInstall.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationConfig.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationController.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationPattern.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationRun.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationSession.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationSimulation.h>
#include <VP-V/modules/output/calibration_lut/calibration/Meter.h>
#include <VP-V/modules/output/calibration_lut/calibration/MeterCatalog.h>
#include <VP-V/modules/output/calibration_lut/calibration/PatternCodes.h>
#include <VP-V/modules/output/calibration_lut/calibration/Spotread.h>
#include <ConfigEditorCore.h>
#include <ConfigFile.h>
#include <MainConfigSchema.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Calibration;
using namespace CalibrationTest;

// Automatic calibration, the remainder (docs/calibration/AUTO-CALIBRATION.md):
// spotread's text (terminal, parser, usage, command line), the meter's state
// machine on scripted transcripts, Argyll discovery, pattern code selection,
// the near-black strategy, the session from start to Apply / Discard / Cancel
// on a simulated display and meter, the run files and the key-only config
// edits with their backups, the [calibration] schema and the controller.
// CPU only: no GPU, no meter, no spawned processes (CalibrationMeterTests
// runs the fake spotread under the real pseudo-console).
namespace
{
	std::vector<SpotreadEvent> ParseAll(const std::string& text, size_t chunk = 7)
	{
		// Fed in small chunks: escape sequences and lines split anywhere.
		SpotreadParser parser;
		std::vector<SpotreadEvent> events;
		for (size_t i = 0; i < text.size(); i += chunk)
			for (const SpotreadEvent& e : parser.Feed(text.substr(i, chunk)))
				events.push_back(e);
		for (const SpotreadEvent& e : parser.Feed("\n"))
			events.push_back(e);
		return events;
	}

	std::vector<SpotreadEventKind> Kinds(const std::vector<SpotreadEvent>& events)
	{
		std::vector<SpotreadEventKind> out;
		for (const SpotreadEvent& e : events) out.push_back(e.kind);
		return out;
	}

	std::wstring Kinds(const std::vector<SpotreadEventKind>& kinds)
	{
		std::wstring out;
		for (SpotreadEventKind k : kinds)
		{
			const std::string s = ToString(k);
			out += std::wstring(s.begin(), s.end()) + L" ";
		}
		return out;
	}

	void AssertKinds(const std::vector<SpotreadEventKind>& want, const std::vector<SpotreadEvent>& got)
	{
		Assert::AreEqual(Kinds(want), Kinds(Kinds(got)));
	}

	struct MeterRig
	{
		double now = 100.0;
		ScriptedTerminal* terminal = nullptr;
		std::unique_ptr<SpotreadMeter> meter;

		explicit MeterRig(const std::string& script, double readTimeout = 5.0)
		{
			SpotreadMeterOptions o;
			o.spotread = L"C:\\Argyll\\bin\\spotread.exe";
			o.readTimeoutSeconds = readTimeout;
			o.openTimeoutSeconds = readTimeout;
			o.closeTimeoutSeconds = 1.0;
			meter.reset(new SpotreadMeter(o, [this, script]()
			{
				auto t = std::unique_ptr<ScriptedTerminal>(new ScriptedTerminal(script, &now));
				terminal = t.get();
				return std::unique_ptr<ITerminal>(std::move(t));
			}, [this]() { return now; }));
		}
	};

	PatternOutputInfo Format(int container, int code, bool limited)
	{
		PatternOutputInfo f;
		f.available = true;
		f.containerBits = static_cast<uint32_t>(container);
		f.codeBits = static_cast<uint32_t>(code);
		f.limitedRange = limited;
		f.width = 3840;
		f.height = 2160;
		f.refreshHz = 59.94;
		return f;
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::stringstream s;
		s << file.rdbuf();
		return s.str();
	}

	void WriteFile(const std::string& path, const std::string& text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
	}

	std::string TempFolder(const std::string& name)
	{
		char base[MAX_PATH];
		GetTempPathA(MAX_PATH, base);
		const std::string folder = std::string(base) + "vp-calibration-tests\\" + name + "-" +
			std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
		std::string error;
		CreateFolders(folder, error);
		return folder;
	}

	const char* kConfig =
		"# VP test configuration\r\n"
		"[general]\r\n"
		"fullscreen: false\r\n"
		"\r\n"
		"[vprenderer.color.Rec709]\r\n"
		"# One profile owns the display reference and its calibration files.\r\n"
		"calibration_lut_enabled: false\r\n"
		"calibration_lut_bt709: none\r\n"
		"calibration_lut_p3_d65: none\r\n"
		"calibration_lut_bt2020: none\r\n"
		"output_gamma: 2.2\r\n"
		"target_primaries: REC709\r\n"
		"\r\n"
		"[calibration]\r\n"
		"meter_model: simulated   # dry run\r\n"
		"plan: cube5\r\n"
		"warmup_minutes: 0\r\n"
		"display_kind: projector\r\n";

	struct SessionRig
	{
		std::string folder;
		std::string config;
		ManualClock clock;
		std::unique_ptr<SimulatedScreen> screen;
		SimulatedScreenMeter* meter = nullptr;
		std::unique_ptr<CalibrationSession> session;
		CalibrationSettings settings;

		SessionRig(const std::string& name, PlanChoice plan, const PatternOutputInfo& format,
			const SimulatedDisplayParams& display = SimulatedDisplayParams())
		{
			folder = TempFolder(name);
			config = folder + "\\VideoProcessor.cfg";
			WriteFile(config, kConfig);
			screen.reset(new SimulatedScreen(format));
			settings.plan = plan;
			settings.warmupMinutes = 0;
			settings.lutSize = 17;
			settings.displayKind = DisplayKind::Projector;
			SimulatedMeterParams mp;
			mp.matrixError = SimulatedMeter::MatrixError(0.01, 7u);
			mp.proportionalNoise = 0.001;
			mp.seed = 11u;
			SimulatedScreen* s = screen.get();
			auto m = std::unique_ptr<SimulatedScreenMeter>(new SimulatedScreenMeter(
				[s](Vec3& device) { return s->Showing(device); }, display, mp, &clock));
			meter = m.get();
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = config;
			env.activeColorSection = "Rec709";
			env.displayName = "Test Projector";
			session.reset(new CalibrationSession(settings, env, std::move(m), 0.002, *screen, clock));
		}

		// Steps until the step is reached (or a final one); false on a bound.
		bool RunUntil(SessionStep wanted, int bound = 20000)
		{
			for (int i = 0; i < bound; ++i)
			{
				const SessionStep step = session->Status().step;
				if (step == wanted) return true;
				if (IsFinal(step)) return false;
				if (step == SessionStep::Placement && session->Status().haveReading) session->Continue();
				session->Step();
			}
			return session->Status().step == wanted;
		}
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(CalibrationSpotreadTextTests)
	{
	public:
		TEST_METHOD(TerminalTextStripsVtAndKeepsSpacing)
		{
			TerminalText t;
			// Split mid-sequence; cursor-forward / erase-characters become spaces;
			// cursor positioning ends a line; a bare CR redraws.
			t.Append("\x1b[?25l\x1b[2J\x1b[HFirst li");
			t.Append("ne\x1b[K\x1b[2;1HA\x1b[3Cb\x1b[2Xc\r\nold text\rnew text\n\x1b]0;title\x07tail");
			const std::vector<std::string> lines = t.TakeLines();
			Assert::AreEqual(size_t(3), lines.size());
			Assert::AreEqual(std::string("First line"), lines[0]);
			Assert::AreEqual(std::string("A   b  c"), lines[1]);
			Assert::AreEqual(std::string("new text"), lines[2]);
			Assert::AreEqual(std::string("tail"), t.Pending());
			Assert::AreEqual(std::string("a\nb"), StripVt("\x1b[1ma\x1b[0m\r\n\x1b[31mb"));
		}

		TEST_METHOD(ResultLinesParseInEveryLayout)
		{
			Vec3 xyz;
			Assert::IsTrue(ParseResultLine(" Result is XYZ: 95.047000 100.000000 108.883000, D50 Lab: 100.0 0.0 0.0", xyz));
			Assert::AreEqual(95.047, xyz[0], 1e-9);
			Assert::AreEqual(100.0, xyz[1], 1e-9);
			Assert::AreEqual(108.883, xyz[2], 1e-9);
			Assert::IsTrue(ParseResultLine("Result is XYZ: 1.2e-3 1.3E-03 -0.0001, Yxy: 0.0013 0.3 0.3", xyz));
			Assert::AreEqual(0.0012, xyz[0], 1e-12);
			Assert::AreEqual(-0.0001, xyz[2], 1e-12);
			// Yxy only (an older layout): converted to XYZ.
			Assert::IsTrue(ParseResultLine("Result is Y: 50.0, Yxy: 50.000000 0.312700 0.329000", xyz));
			Assert::AreEqual(50.0, xyz[1], 1e-9);
			Assert::AreEqual(0.3127 * 50.0 / 0.329, xyz[0], 1e-9);
			Assert::IsFalse(ParseResultLine("Result is XYZ: n/a", xyz));
			Assert::IsFalse(ParseResultLine("Place instrument on spot to be measured,", xyz));
			// The decimal point is never the locale's.
			size_t pos = 0;
			double v = 0;
			Assert::IsTrue(ParseNumberAt("123.456789", pos, v));
			Assert::AreEqual(123.456789, v, 1e-12);
			Assert::AreEqual(size_t(10), pos);
		}

		TEST_METHOD(TranscriptsGiveTheExpectedEvents)
		{
			using K = SpotreadEventKind;
			std::vector<SpotreadEvent> e = ParseAll(PlayAll("i1d3_session.txt"));
			AssertKinds({ K::ReadyPrompt, K::Reading, K::ReadyPrompt, K::Reading, K::ReadyPrompt, K::Reading, K::ReadyPrompt }, e);
			Assert::AreEqual(95.047, e[1].xyz[0], 1e-6);
			Assert::AreEqual(21.26, e[3].xyz[1], 1e-6);
			Assert::AreEqual(0.0130, e[5].xyz[1], 1e-6);

			e = ParseAll(PlayAll("i1pro_calibration.txt"));
			AssertKinds({ K::NeedsCalibration, K::CalibrationPrompt, K::CalibrationComplete, K::ReadyPrompt, K::Reading,
				K::ReadyPrompt }, e);
			Assert::IsTrue(e[1].text.find("white calibration reference") != std::string::npos);

			e = ParseAll(PlayAll("read_failure.txt"));
			AssertKinds({ K::ReadyPrompt, K::ReadFailed, K::ReadyPrompt, K::Reading, K::ReadyPrompt }, e);
			Assert::IsTrue(e[1].error == SpotreadErrorKind::TooDark);

			e = ParseAll(PlayAll("no_instrument.txt"));
			AssertKinds({ K::FatalError }, e);
			Assert::IsTrue(e[0].error == SpotreadErrorKind::NoInstrument);

			e = ParseAll(PlayAll("init_failed.txt"));
			AssertKinds({ K::FatalError }, e);
			Assert::IsTrue(e[0].error == SpotreadErrorKind::InitFailed);

			// A redraw repeats the result line: the parser reports both; the meter
			// takes only the one it asked for (SpotreadMeterTests).
			e = ParseAll(PlayAll("redraw.txt"), 3);
			AssertKinds({ K::ReadyPrompt, K::Reading, K::Reading, K::ReadyPrompt }, e);
			Assert::AreEqual(12.5, e[1].xyz[0], 1e-9);
			Assert::AreEqual(14.0, e[1].xyz[2], 1e-9);

			// The same transcripts in one piece.
			Assert::AreEqual(size_t(7), ParseAll(PlayAll("i1d3_session.txt"), 100000).size());
		}

		TEST_METHOD(UsageGivesVersionInstrumentsAndDisplayTypes)
		{
			const std::string usage =
				"Read Print Spot values, Version 3.5.0\n"
				"usage: spotread [-options] [logfile]\n"
				" -c listno            Set communication port from the following list (default 1)\n"
				"    1 = 'usb:/bus5/dev4/ (X-Rite i1 DisplayPro, ColorMunki Display)'\n"
				"    2 = 'usb:/bus5/dev7/ (Datacolor SpyderX)'\n"
				" -e                   Emissive for display\n"
				" -y c|l               Display type - instrument specific list to choose from:\n"
				"    l   LCD (generic)\n"
				"    c   CRT (generic)\n"
				"    1   Projector (custom)\n"
				" -x                   Display Yxy instead of Lab\n";
			const SpotreadUsage u = ParseSpotreadUsage(usage);
			Assert::IsTrue(u.recognised);
			Assert::AreEqual(std::string("3.5.0"), u.version);
			Assert::IsTrue(VersionAtLeast(u, 3, 0));
			Assert::IsFalse(VersionAtLeast(u, 3, 6));
			Assert::AreEqual(size_t(2), u.instruments.size());
			Assert::AreEqual(1, u.instruments[0].port);
			Assert::AreEqual(std::string("X-Rite i1 DisplayPro, ColorMunki Display"), u.instruments[0].name);
			Assert::AreEqual(std::string("Datacolor SpyderX"), u.instruments[1].name);
			Assert::AreEqual(size_t(3), u.displayTypes.size());
			Assert::AreEqual(std::string("1"), u.displayTypes[2].selector);
			Assert::AreEqual(std::string("Projector (custom)"), u.displayTypes[2].description);
			Assert::IsFalse(ParseSpotreadUsage("'spot' is not recognized as a command").instruments.size() > 0);
		}

		TEST_METHOD(MeterModelsMatchArgyllNames)
		{
			Assert::AreEqual(std::string("i1d3"), std::string(MatchInstrument("X-Rite i1 DisplayPro, ColorMunki Display")->id));
			Assert::AreEqual(std::string("i1d3"), std::string(MatchInstrument("Calibrite Display Plus HL")->id));
			Assert::AreEqual(std::string("spyderx"), std::string(MatchInstrument("Datacolor SpyderX")->id));
			Assert::AreEqual(std::string("spyderx2"), std::string(MatchInstrument("Datacolor SpyderX2")->id));
			Assert::AreEqual(std::string("i1pro2"), std::string(MatchInstrument("X-Rite i1 Pro 2")->id));
			Assert::AreEqual(std::string("i1pro"), std::string(MatchInstrument("GretagMacbeth i1 Pro")->id));
			Assert::AreEqual(std::string("colormunki_photo"), std::string(MatchInstrument("X-Rite ColorMunki")->id));
			Assert::AreEqual(std::string("colormunki_display"), std::string(MatchInstrument("X-Rite ColorMunki Display")->id));
			Assert::AreEqual(std::string("jeti"), std::string(MatchInstrument("JETI specbos 1211")->id));
			Assert::IsNull(MatchInstrument("Unknown Meter 9000"));
			// The configured model wins; auto / other follow the instrument.
			Assert::AreEqual(std::string("i1pro3"), std::string(ResolveMeterModel("i1pro3", "Datacolor SpyderX")->id));
			Assert::AreEqual(std::string("spyderx"), std::string(ResolveMeterModel("auto", "Datacolor SpyderX")->id));
			Assert::IsTrue(FindMeterModel("i1pro")->needsSensorCalibration);
			Assert::IsFalse(FindMeterModel("i1d3")->needsSensorCalibration);
			Assert::IsTrue(FindMeterModel("i1d3")->floorNits < FindMeterModel("i1pro")->floorNits);
		}

		TEST_METHOD(CommandLineIsQuotedAndComplete)
		{
			SpotreadOptions o;
			o.port = 2;
			o.displayType = "l";
			o.spectralCorrection = "C:\\Corrections\\RGB laser \"projector\".ccss";
			o.refreshMode = 2;
			const std::vector<std::string> args = SpotreadArguments(o);
			const std::vector<std::string> want = { "-c", "2", "-e", "-x", "-y", "l", "-Y", "n", "-X",
				"C:\\Corrections\\RGB laser \"projector\".ccss" };
			Assert::IsTrue(args == want);
			const std::wstring line = BuildCommandLine(L"C:\\Program Files\\Argyll\\bin\\spotread.exe", args);
			Assert::AreEqual(std::wstring(L"\"C:\\Program Files\\Argyll\\bin\\spotread.exe\" -c 2 -e -x -y l -Y n -X "
				L"\"C:\\Corrections\\RGB laser \\\"projector\\\".ccss\""), line);
			// CommandLineToArgvW gives the arguments back exactly.
			int argc = 0;
			LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
			Assert::AreEqual(11, argc);
			Assert::AreEqual(std::wstring(L"C:\\Corrections\\RGB laser \"projector\".ccss"), std::wstring(argv[10]));
			LocalFree(argv);
			// No space or quote: left as it is; with one, trailing backslashes are doubled.
			Assert::AreEqual(std::wstring(L"a\\\\b\\"), QuoteArgument(L"a\\\\b\\"));
			Assert::AreEqual(std::wstring(L"\"a b\\\\\""), QuoteArgument(L"a b\\"));
			// Defaults: emissive absolute, Yxy; nothing else.
			const std::vector<std::string> plain = SpotreadArguments(SpotreadOptions());
			Assert::IsTrue(plain == std::vector<std::string>({ "-e", "-x" }));
		}

		TEST_METHOD(ArgyllIsFoundOrExplained)
		{
			std::set<std::wstring> files;
			ArgyllSearchEnvironment e;
			e.fileExists = [&files](const std::wstring& p) { return files.count(p) > 0; };
			std::map<std::wstring, std::vector<std::wstring>> folders;
			e.listArgyllFolders = [&folders](const std::wstring& root) { return folders[root]; };
			e.installRoots = { L"C:\\Program Files", L"C:\\" };
			std::string searched;

			// Nothing installed: empty, and the guidance names the official site
			// and says VP does not bundle or download Argyll.
			Assert::IsTrue(FindSpotread(e, searched).empty());
			const std::string guidance = ArgyllInstallGuidance(searched);
			Assert::IsTrue(guidance.find("argyllcms.com") != std::string::npos);
			Assert::IsTrue(guidance.find("does not include or download") != std::string::npos);
			Assert::IsTrue(guidance.find("C:\\Program Files\\Argyll*") != std::string::npos);

			// Install folders: the highest version wins.
			files.insert(L"C:\\Program Files\\Argyll_V2.3.1\\bin\\spotread.exe");
			files.insert(L"C:\\Program Files\\Argyll_V3.5.0\\bin\\spotread.exe");
			files.insert(L"C:\\Argyll_V3.10.2\\bin\\spotread.exe");
			folders[L"C:\\Program Files"] = { L"Argyll_V2.3.1", L"Argyll_V3.5.0" };
			folders[L"C:\\"] = { L"Argyll_V3.10.2", L"ArgyllEmpty" };
			Assert::AreEqual(std::wstring(L"C:\\Argyll_V3.10.2\\bin\\spotread.exe"), FindSpotread(e, searched));

			// PATH before the install folders.
			files.insert(L"D:\\tools\\argyll\\bin\\spotread.exe");
			e.path = L"C:\\Windows;\"D:\\tools\\argyll\\bin\";";
			Assert::AreEqual(std::wstring(L"D:\\tools\\argyll\\bin\\spotread.exe"), FindSpotread(e, searched));

			// The setting: the install folder, its bin, or spotread.exe itself.
			files.insert(L"E:\\Argyll\\bin\\spotread.exe");
			e.configured = L"E:\\Argyll";
			Assert::AreEqual(std::wstring(L"E:\\Argyll\\bin\\spotread.exe"), FindSpotread(e, searched));
			e.configured = L"\"E:\\Argyll\\bin\\\"";
			Assert::AreEqual(std::wstring(L"E:\\Argyll\\bin\\spotread.exe"), FindSpotread(e, searched));
			e.configured = L"E:\\Argyll\\bin\\spotread.exe";
			Assert::AreEqual(std::wstring(L"E:\\Argyll\\bin\\spotread.exe"), FindSpotread(e, searched));
			// A wrong setting is reported, never silently replaced by another install.
			e.configured = L"F:\\Nowhere";
			Assert::IsTrue(FindSpotread(e, searched).empty());
			Assert::IsTrue(searched.find("F:\\Nowhere") != std::string::npos);

			int v[3];
			Assert::IsTrue(FolderVersion(L"Argyll_V3.5.0", v));
			Assert::AreEqual(3, v[0]); Assert::AreEqual(5, v[1]); Assert::AreEqual(0, v[2]);
			Assert::IsFalse(FolderVersion(L"ArgyllCMS", v));
		}
	};

	TEST_CLASS(CalibrationMeterStateTests)
	{
	public:
		TEST_METHOD(ColorimeterSessionReadsAndQuits)
		{
			MeterRig rig("i1d3_session.txt");
			MeterResult r = rig.meter->Open();
			Assert::IsTrue(r.status == MeterStatus::Ok, std::wstring(r.message.begin(), r.message.end()).c_str());
			Assert::IsTrue(rig.terminal->startedCommandLine.find(L" -e -x") != std::wstring::npos);
			r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Ok);
			Assert::AreEqual(100.0, r.xyz[1], 1e-6);
			r = rig.meter->Read();
			Assert::AreEqual(21.26, r.xyz[1], 1e-6);
			r = rig.meter->Read();
			Assert::AreEqual(0.013, r.xyz[1], 1e-9);
			rig.meter->Close();
			Assert::IsFalse(rig.meter->IsOpen());
			Assert::AreEqual(std::string("   q"), rig.meter->KeysSent());
		}

		TEST_METHOD(SensorCalibrationPromptIsPassedToTheUser)
		{
			MeterRig rig("i1pro_calibration.txt");
			MeterResult r = rig.meter->Open();
			Assert::IsTrue(r.status == MeterStatus::NeedsUserAction);
			Assert::IsTrue(r.message.find("white calibration reference") != std::string::npos);
			// Reading before the user acts asks again rather than sending a key.
			Assert::IsTrue(rig.meter->Read().status == MeterStatus::NeedsUserAction);
			r = rig.meter->Continue();
			Assert::IsTrue(r.status == MeterStatus::Ok);
			r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Ok);
			Assert::AreEqual(50.0, r.xyz[1], 1e-6);
		}

		TEST_METHOD(FailedReadingIsRecoverable)
		{
			MeterRig rig("read_failure.txt");
			Assert::IsTrue(rig.meter->Open().status == MeterStatus::Ok);
			MeterResult r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::ReadFailed);
			Assert::IsTrue(r.error == SpotreadErrorKind::TooDark);
			r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Ok);
			Assert::AreEqual(0.047, r.xyz[1], 1e-9);
		}

		TEST_METHOD(MissingInstrumentAndInitFailureAreExplained)
		{
			MeterRig none("no_instrument.txt");
			MeterResult r = none.meter->Open();
			Assert::IsTrue(r.status == MeterStatus::Fatal);
			Assert::IsTrue(r.error == SpotreadErrorKind::NoInstrument);
			Assert::IsTrue(r.message.find("Plug it in") != std::string::npos);
			Assert::IsFalse(none.meter->IsOpen());
			MeterRig init("init_failed.txt");
			r = init.meter->Open();
			Assert::IsTrue(r.status == MeterStatus::Fatal);
			Assert::IsTrue(r.error == SpotreadErrorKind::InitFailed);
			Assert::IsTrue(r.message.find("USB driver") != std::string::npos);
		}

		TEST_METHOD(HungInstrumentTimesOutAndIsEnded)
		{
			MeterRig rig("hang.txt", 3.0);
			Assert::IsTrue(rig.meter->Open().status == MeterStatus::Ok);
			const double before = rig.now;
			const MeterResult r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Timeout);
			Assert::IsTrue(rig.now - before >= 3.0);
			Assert::IsFalse(rig.meter->IsOpen());
			Assert::IsTrue(r.message.find("did not answer") != std::string::npos);
		}

		TEST_METHOD(UnexpectedExitIsFatal)
		{
			MeterRig rig("crash.txt");
			Assert::IsTrue(rig.meter->Open().status == MeterStatus::Ok);
			const MeterResult r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Fatal);
			Assert::IsTrue(r.message.find("exit code 3") != std::string::npos);
			Assert::IsTrue(r.message.find("unexpected reply") != std::string::npos);
		}

		TEST_METHOD(RedrawnResultIsNotTakenTwice)
		{
			MeterRig rig("redraw.txt");
			Assert::IsTrue(rig.meter->Open().status == MeterStatus::Ok);
			MeterResult r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Ok);
			Assert::AreEqual(13.0, r.xyz[1], 1e-9);
			// The next Read waits for the prompt (the duplicate result is dropped)
			// and then sends its own key.
			rig.meter->Close();
			Assert::AreEqual(std::string(" q"), rig.meter->KeysSent());
		}

		TEST_METHOD(AbortEndsAWaitAtOnce)
		{
			MeterRig rig("hang.txt", 30.0);
			Assert::IsTrue(rig.meter->Open().status == MeterStatus::Ok);
			rig.meter->Abort();
			const double before = rig.now;
			const MeterResult r = rig.meter->Read();
			Assert::IsTrue(r.status == MeterStatus::Fatal);
			Assert::IsTrue(r.error == SpotreadErrorKind::UserAbort);
			Assert::IsTrue(rig.now - before < 2.0);
		}
	};

	TEST_CLASS(CalibrationPatternTests)
	{
	public:
		TEST_METHOD(EightBitLimitedCodes)
		{
			const PatternOutputInfo f = Format(8, 8, true);
			std::string reason;
			Assert::IsTrue(PatternFormatSupported(f, reason));
			Assert::AreEqual(16, BlackCode(f));
			Assert::AreEqual(235, WhiteCode(f));
			Assert::AreEqual(1, LowestLegalCode(f));
			Assert::AreEqual(254, HighestLegalCode(f));
			PatchCodes p = CodesForSignal(f, { 0.0, 0.5, 1.0 });
			Assert::AreEqual(16, p.code[0]);
			Assert::AreEqual(126, p.code[1]);   // 16 + 109.5 rounds to 126 (half away from zero)
			Assert::AreEqual(235, p.code[2]);
			Assert::AreEqual(double(126 - 16) / 219.0, p.device[1], 1e-12);   // the code's exact signal
			Assert::IsFalse(p.clipped);
			// The plan's BTB / WTW codes are kept exactly.
			for (const int code : { 4, 8, 12, 15, 240, 254 })
			{
				const double s = LimitedCodeToSignal(code);
				p = CodesForSignal(f, { s, s, s });
				Assert::AreEqual(code, p.code[0]);
				Assert::AreEqual(static_cast<uint16_t>(code), p.container[0]);
				Assert::IsFalse(p.clipped);
			}
			// Below code 1 / above 254: clamped and flagged (0 and 255 are sync on a limited link).
			p = CodesForSignal(f, { LimitedCodeToSignal(0), 0, LimitedCodeToSignal(255) });
			Assert::AreEqual(1, p.code[0]);
			Assert::AreEqual(254, p.code[2]);
			Assert::IsTrue(p.clipped);
		}

		TEST_METHOD(EightInTenMatchesTheSigmaDeltaDomain)
		{
			// Limited: 4k / 1023 (the code in the top 8 bits); full: k / 255.
			const PatternOutputInfo limited = Format(10, 8, true);
			Assert::AreEqual(static_cast<uint16_t>(64), ContainerValue(limited, 16));
			Assert::AreEqual(static_cast<uint16_t>(940), ContainerValue(limited, 235));
			Assert::AreEqual(static_cast<uint16_t>(16), ContainerValue(limited, 4));
			const PatternOutputInfo full = Format(10, 8, false);
			Assert::AreEqual(static_cast<uint16_t>(0), ContainerValue(full, 0));
			Assert::AreEqual(static_cast<uint16_t>(1023), ContainerValue(full, 255));
			Assert::AreEqual(static_cast<uint16_t>(514), ContainerValue(full, 128));   // round(128 * 1023 / 255)
			for (int k = 0; k <= 255; ++k)
			{
				Assert::AreEqual(k, ContainerValue(limited, k) >> 2);
				Assert::AreEqual(k, ContainerValue(full, k) >> 2);   // the top 8 bits are the code
			}
			// The simulated screen reads them back.
			SimulatedScreen screen(limited);
			const uint16_t c[3] = { 64, 940, 16 };
			const Vec3 d = screen.DeviceForContainer(c);
			Assert::AreEqual(0.0, d[0], 1e-12);
			Assert::AreEqual(1.0, d[1], 1e-12);
			Assert::AreEqual(LimitedCodeToSignal(4), d[2], 1e-12);
		}

		TEST_METHOD(TenBitCodesAndFullRange)
		{
			const PatternOutputInfo ten = Format(10, 10, true);
			Assert::AreEqual(64, BlackCode(ten));
			Assert::AreEqual(940, WhiteCode(ten));
			Assert::AreEqual(4, LowestLegalCode(ten));
			Assert::AreEqual(1019, HighestLegalCode(ten));
			PatchCodes p = CodesForSignal(ten, { 0.25, 0.25, 0.25 });
			Assert::AreEqual(283, p.code[0]);   // 64 + 219
			Assert::AreEqual(static_cast<uint16_t>(283), p.container[0]);
			// Finer near black than 8 bits: 1% grey is 9 codes above black, not 2.
			p = CodesForSignal(ten, { 0.01, 0.01, 0.01 });
			Assert::AreEqual(73, p.code[0]);
			const PatternOutputInfo full = Format(8, 8, false);
			Assert::AreEqual(0, BlackCode(full));
			Assert::AreEqual(255, WhiteCode(full));
			Patch btb;
			btb.rgb[0] = btb.rgb[1] = btb.rgb[2] = LimitedCodeToSignal(8);
			btb.kind = PatchKind::Btb;
			Assert::IsFalse(PatchRepresentable(full, btb));
			Assert::IsTrue(PatchRepresentable(Format(8, 8, true), btb));
			std::string reason;
			Assert::IsFalse(PatternFormatSupported(Format(16, 16, false), reason));
			Assert::IsTrue(reason.find("FP16") != std::string::npos);
			PatternOutputInfo off;
			Assert::IsFalse(PatternFormatSupported(off, reason));
		}

		TEST_METHOD(PqKnownValues)
		{
			// SMPTE ST 2084 reference points.
			Assert::AreEqual(1.0, PqEncode(10000.0), 1e-12);
			Assert::AreEqual(0.508078, PqEncode(100.0), 1e-5);
			Assert::AreEqual(0.751827, PqEncode(1000.0), 1e-5);
			Assert::AreEqual(7.3e-7, PqEncode(0.0), 1e-7);
			for (const double nits : { 0.005, 0.1, 1.0, 48.0, 203.0, 4000.0 })
				Assert::AreEqual(nits, PqDecode(PqEncode(nits)), nits * 1e-9);
			// A 10-bit limited PQ code for 100 nits: 64 + 876 * 0.508078 = 509.
			Assert::AreEqual(509, SignalToCode(Format(10, 10, true), HdrSignalForNits(Format(10, 10, true), 100.0)));
		}

		TEST_METHOD(WindowIsCentredAtTheRequestedArea)
		{
			for (const auto& size : { std::make_pair(3840, 2160), std::make_pair(4096, 2160), std::make_pair(1920, 1080),
				std::make_pair(2560, 1080), std::make_pair(1279, 721) })
				for (const double percent : { 1.0, 10.0, 18.0, 50.0 })
				{
					const PixelRect r = PatchWindowRect(size.first, size.second, percent);
					const double area = 100.0 * r.Width() * r.Height() / (double(size.first) * size.second);
					Assert::AreEqual(percent, area, percent * 0.05 + 0.05);
					Assert::IsTrue(std::abs((r.left + r.right) - size.first) <= 3);
					Assert::IsTrue(std::abs((r.top + r.bottom) - size.second) <= 3);
					Assert::AreEqual(0, r.left % 2);
					Assert::AreEqual(0, r.top % 2);
					// The screen's aspect ratio.
					Assert::AreEqual(double(size.first) / size.second, double(r.Width()) / r.Height(), 0.02);
				}
			const PixelRect full = PatchWindowRect(3840, 2160, 100.0);
			Assert::AreEqual(3840, full.right);
			Assert::AreEqual(2160, full.bottom);
			Assert::AreEqual(0, PatchWindowRect(0, 0, 10.0).Width());
		}

		TEST_METHOD(SettleFollowsDisplayKindAndDrops)
		{
			SettleOptions o;
			o.displayKind = static_cast<int>(DisplayKind::Projector);
			Assert::AreEqual(0.5, SettleSeconds(o, 50.0, 40.0, 50.0), 1e-12);
			Assert::AreEqual(1.0, SettleSeconds(o, 50.0, 0.1, 50.0), 1e-12);   // a large drop waits longer
			o.displayKind = static_cast<int>(DisplayKind::Lcd);
			Assert::AreEqual(1.0, SettleSeconds(o, 50.0, 40.0, 50.0), 1e-12);
			o.configuredMs = 300;
			o.extraLatencySeconds = 0.05;
			Assert::AreEqual(0.35, SettleSeconds(o, 50.0, 40.0, 50.0), 1e-12);
			Assert::AreEqual(0.5, SettleSeconds(o, 50.0, 1.0, 50.0), 1e-12);
		}

		TEST_METHOD(NearBlackStrategyAveragesAndFlagsTheFloor)
		{
			ReadingPolicy p;
			p.floorNits = 0.002;
			p.nearBlackMaxReads = 4;
			p.tolerance = 0.01;
			// Bright: one reading.
			ReadingAccumulator bright(p, 50.0);
			bright.Add({ 20, 21, 22 });
			Assert::IsTrue(bright.Done());
			Assert::IsFalse(bright.NearBlack());
			// Near black, disagreeing: up to four, averaged.
			ReadingAccumulator dark(p, 50.0);
			for (const double y : { 0.10, 0.12, 0.09 })
			{
				dark.Add({ y, y, y });
				Assert::IsFalse(dark.Done());
			}
			dark.Add({ 0.11, 0.11, 0.11 });
			Assert::IsTrue(dark.Done());
			Assert::AreEqual(0.105, dark.Mean()[1], 1e-12);
			Assert::IsTrue(dark.SpreadY() > 0.2);
			// Near black, agreeing: stops at two.
			ReadingAccumulator steady(p, 50.0);
			steady.Add({ 0.1, 0.1000, 0.1 });
			steady.Add({ 0.1, 0.1005, 0.1 });
			Assert::IsTrue(steady.Done());
			// Below the meter's floor: all four readings, and flagged.
			ReadingAccumulator floor(p, 50.0);
			for (int i = 0; i < 3; ++i) { floor.Add({ 0.001, 0.001, 0.001 }); Assert::IsFalse(floor.Done()); }
			floor.Add({ 0.001, 0.001, 0.001 });
			Assert::IsTrue(floor.Done());
			Assert::IsTrue(floor.FloorLimited());
		}

		TEST_METHOD(SessionPlanOrder)
		{
			const std::vector<Patch> plan = BuildSessionPlan(PlanChoice::Cube5, 50);
			Assert::IsTrue(plan.front().kind == PatchKind::DriftReference);
			Assert::IsTrue(plan.back().kind == PatchKind::DriftReference);
			// The greyscale from white down, then the rest.
			size_t i = 1;
			Assert::AreEqual(1.0, plan[i].rgb[0], 1e-12);
			double last = 2.0;
			int greys = 0;
			for (; i < plan.size(); ++i)
			{
				const Patch& p = plan[i];
				if (p.kind == PatchKind::DriftReference) continue;
				const bool neutral = p.kind == PatchKind::Black || p.kind == PatchKind::Greyscale ||
					p.kind == PatchKind::NearBlack || p.kind == PatchKind::Btb || p.kind == PatchKind::Wtw;
				if (!neutral) break;
				if (p.kind != PatchKind::Wtw || greys > 0)
				{
					Assert::IsTrue(p.rgb[0] <= last + 1e-12 || p.kind == PatchKind::Wtw);
					last = p.rgb[0];
				}
				++greys;
			}
			Assert::IsTrue(greys > 25);
			// A drift reference after every 50 patches.
			int since = 0;
			for (size_t k = 1; k + 1 < plan.size(); ++k)
			{
				if (plan[k].kind == PatchKind::DriftReference) { Assert::AreEqual(50, since); since = 0; }
				else ++since;
			}
			// Every patch of the plan is there once.
			const std::vector<Patch> base = BuildPlan(PlanKind::Cube5, 0);
			size_t baseCount = 0, sessionCount = 0;
			for (const Patch& p : base) if (p.kind != PatchKind::DriftReference) ++baseCount;
			for (const Patch& p : plan) if (p.kind != PatchKind::DriftReference) ++sessionCount;
			Assert::AreEqual(baseCount, sessionCount);
		}
	};

	TEST_CLASS(CalibrationSessionTests)
	{
	public:
		TEST_METHOD(FullRunAppliesOnlyTheCalibrationKeysAfterBackups)
		{
			SessionRig rig("apply", PlanChoice::Cube5, Format(10, 8, true));
			const std::string original = ReadFile(rig.config);
			rig.session->Start();
			const bool reached = rig.RunUntil(SessionStep::Review);
			const std::string why = std::string(ToString(rig.session->Status().step)) + ": " + rig.session->Status().message;
			Assert::IsTrue(reached, (L"did not reach review: " + std::wstring(why.begin(), why.end())).c_str());
			const SessionStatus review = rig.session->Status();
			Logger::WriteMessage(("review:\n" + review.review + "\ndiff:\n" + review.diff + "\n").c_str());
			Assert::IsFalse(rig.screen->PatternActive());   // video is back while the owner reviews
			Assert::IsTrue(review.verifyMean >= 0.0 && review.verifyMean < 1.5);
			Assert::IsTrue(review.verifyMax < 4.0);
			Assert::IsTrue(review.measured > 150);
			Assert::AreEqual(0, review.skipped);
			Assert::IsTrue(review.format.find("8-in-10 limited SDR") != std::string::npos);
			Assert::IsTrue(review.diff.find("[vprenderer.color.Rec709] calibration_lut_bt709: none -> luts/calibration/") !=
				std::string::npos);
			Assert::IsTrue(review.diff.find("calibration_lut_enabled: false -> true") != std::string::npos);
			Assert::AreEqual(original, ReadFile(rig.config));   // nothing written before Apply

			// The run folder: journal, cubes, profile, verify, report.
			const RunFolder& f = rig.session->Folder();
			const std::string journal = ReadFile(f.rawJournal);
			Assert::IsTrue(std::count(journal.begin(), journal.end(), '\n') > 200);
			Assert::IsTrue(journal.find("\"kind\":\"btb\"") != std::string::npos);
			Assert::IsTrue(journal.find("\"container\":[16,16,16]") != std::string::npos);   // BTB code 4 in 8-in-10
			for (int s = 0; s < 3; ++s)
				Assert::IsTrue(ReadFile(f.cube[s]).find("LUT_3D_SIZE 17") != std::string::npos);
			Assert::IsTrue(ReadFile(f.displayProfile).find("peak_nits") != std::string::npos);
			Assert::IsTrue(ReadFile(f.verifyJson).find("mean_de2000") != std::string::npos);
			Assert::IsTrue(ReadFile(f.reportHtml).find("Verification") != std::string::npos);

			rig.session->Apply();
			for (int i = 0; i < 10 && !rig.session->Finished(); ++i) rig.session->Step();
			const SessionStatus done = rig.session->Status();
			Assert::IsTrue(done.step == SessionStep::Applied, std::wstring(done.message.begin(), done.message.end()).c_str());

			// Backups: beside the configuration (timestamped) and in the run folder, both byte copies.
			const size_t at = done.backup.find("Backup: ");
			const size_t end = done.backup.find('\n');
			const std::string backup = done.backup.substr(at + 8, end - at - 8);
			Assert::IsTrue(backup.find("VideoProcessor.cfg.before-calibration.") != std::string::npos);
			Assert::AreEqual(original, ReadFile(backup));
			Assert::AreEqual(original, ReadFile(BeforeCopyPath(f, rig.config)));

			// Key-only: every other line, comment and the inline comment are byte-identical.
			const std::string edited = ReadFile(rig.config);
			std::istringstream a(original), b(edited);
			std::string la, lb;
			int changed = 0;
			while (std::getline(a, la) && std::getline(b, lb))
				if (la != lb)
				{
					++changed;
					Assert::IsTrue(lb.find("calibration_lut_") == 0);
				}
			Assert::AreEqual(4, changed);
			Assert::IsTrue(edited.find("meter_model: simulated   # dry run") != std::string::npos);
			Assert::IsTrue(edited.find("calibration_lut_bt709: " + f.cubeConfigValue[0]) != std::string::npos);
			std::wstring error;
			ConfigEditorCore::ConfigDocument document;
			Assert::IsTrue(document.Load(std::wstring(rig.config.begin(), rig.config.end()), error));
			Assert::IsTrue(ConfigEditorCore::ValidateCandidate(document, error), error.c_str());
			Assert::IsTrue(ReadFile(f.indexJson).find("\"status\":\"applied\"") != std::string::npos);
		}

		TEST_METHOD(DiscardLeavesTheConfigurationUntouched)
		{
			SessionRig rig("discard", PlanChoice::Quick, Format(8, 8, true));
			const std::string original = ReadFile(rig.config);
			rig.session->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Review));
			rig.session->Discard();
			for (int i = 0; i < 10 && !rig.session->Finished(); ++i) rig.session->Step();
			Assert::IsTrue(rig.session->Status().step == SessionStep::Discarded);
			Assert::AreEqual(original, ReadFile(rig.config));
			Assert::IsTrue(ReadFile(rig.session->Folder().indexJson).find("\"status\":\"discarded\"") != std::string::npos);
			Assert::IsFalse(rig.screen->PatternActive());
			Assert::IsFalse(rig.meter->IsOpen());
		}

		TEST_METHOD(CancelDuringMeasurementRestoresVideo)
		{
			SessionRig rig("cancel", PlanChoice::Cube5, Format(10, 10, true));
			const std::string original = ReadFile(rig.config);
			rig.session->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
			for (int i = 0; i < 20; ++i) rig.session->Step();
			Assert::IsTrue(rig.screen->PatternActive());
			rig.session->Cancel();
			rig.session->Step();
			Assert::IsTrue(rig.session->Status().step == SessionStep::Cancelled);
			Assert::IsFalse(rig.screen->PatternActive());
			Assert::IsFalse(rig.meter->IsOpen());
			Assert::AreEqual(original, ReadFile(rig.config));
			Assert::IsFalse(rig.session->Step());
		}

		TEST_METHOD(MeterCalibrationPromptPausesAndResumes)
		{
			SessionRig rig("metercal", PlanChoice::Quick, Format(8, 8, true));
			rig.meter->RequireCalibrationOnOpen(true);
			rig.session->Start();
			rig.session->Step();
			SessionStatus s = rig.session->Status();
			Assert::IsTrue(s.step == SessionStep::MeterCalibration);
			Assert::IsTrue(s.instruction.find("calibration position") != std::string::npos);
			for (int i = 0; i < 5; ++i) rig.session->Step();   // waits for the user
			Assert::IsTrue(rig.session->Status().step == SessionStep::MeterCalibration);
			rig.session->Continue();
			rig.session->Step();
			Assert::IsTrue(rig.session->Status().step == SessionStep::Placement);
			Assert::IsTrue(rig.RunUntil(SessionStep::Review));
		}

		TEST_METHOD(ReadFailuresAndATimeoutAreRetried)
		{
			SessionRig rig("retry", PlanChoice::Quick, Format(8, 8, true));
			rig.session->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
			rig.meter->FailNextReads(2, SpotreadErrorKind::Misread);
			rig.session->Step();   // two failures, then a good reading of the same patch
			Assert::AreEqual(0, rig.session->Status().skipped);
			Assert::AreEqual(1, rig.session->Status().measured);
			rig.session->Step();   // white (the fit needs it)
			Assert::AreEqual(2, rig.session->Status().measured);
			rig.meter->FailNextReads(3, SpotreadErrorKind::TooDark);
			rig.session->Step();   // three: the patch (WTW 254) is unreadable and skipped
			Assert::AreEqual(1, rig.session->Status().skipped);
			Assert::IsTrue(rig.session->Journal().back().status == "unreadable");
			Assert::IsTrue(rig.session->Journal().back().note.find("too dark") != std::string::npos);
			rig.meter->TimeoutNextRead();
			rig.session->Step();   // reopened once and measured
			const SessionStatus after = rig.session->Status();
			Logger::WriteMessage(("after the timeout: " + std::string(ToString(after.step)) + " measured " +
				std::to_string(after.measured) + " skipped " + std::to_string(after.skipped) + " " + after.message + "\n").c_str());
			Assert::AreEqual(3, after.measured);
			const bool reached = rig.RunUntil(SessionStep::Review);
			const std::string why = std::string(ToString(rig.session->Status().step)) + ": " + rig.session->Status().message;
			Assert::IsTrue(reached, std::wstring(why.begin(), why.end()).c_str());
		}

		TEST_METHOD(FullRangeOutputSkipsBtbAndWtw)
		{
			SimulatedDisplayParams display;
			SessionRig rig("fullrange", PlanChoice::Quick, Format(8, 8, false), display);
			rig.session->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Review));
			const SessionStatus s = rig.session->Status();
			Assert::AreEqual(6, s.skipped);   // BTB 4/8/12/15, WTW 240/254
			Assert::IsTrue(s.format.find("full") != std::string::npos);
			for (const JournalEntry& e : rig.session->Journal())
				if (e.patch.kind == PatchKind::Btb || e.patch.kind == PatchKind::Wtw)
					Assert::IsTrue(e.status == "skipped");
		}

		TEST_METHOD(UnsupportedOutputFailsClearly)
		{
			SessionRig rig("fp16", PlanChoice::Quick, Format(16, 16, false));
			const std::string original = ReadFile(rig.config);
			rig.session->Start();
			rig.session->Step();
			const SessionStatus s = rig.session->Status();
			Assert::IsTrue(s.step == SessionStep::Failed);
			Assert::IsTrue(s.message.find("FP16") != std::string::npos);
			Assert::AreEqual(original, ReadFile(rig.config));
		}

		TEST_METHOD(DriftIsMeasuredAndCorrected)
		{
			SessionRig rig("drift", PlanChoice::Cube5, Format(10, 8, true));
			rig.session->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
			// The display dims 3% while measuring (a lamp / LED warming): the
			// references every 50 patches see it.
			int steps = 0;
			while (rig.session->Status().step == SessionStep::Measuring && steps < 5000)
			{
				const int index = rig.session->Status().patchIndex;
				rig.meter->Display().SetLpw(255 - std::min(index / 20, 8));
				rig.session->Step();
				++steps;
			}
			Assert::IsTrue(rig.RunUntil(SessionStep::Review));
			Assert::IsTrue(std::fabs(rig.session->Status().driftPercent) > 0.5);
		}

		TEST_METHOD(StatusTextRoundTrips)
		{
			SessionStatus s;
			s.step = SessionStep::Review;
			s.instruction = "line one\nline two \\ with a backslash";
			s.review = "Display: white 48 cd/m2";
			s.diff = "[a] b: c -> d\n[a] e: f -> g\n";
			s.patchIndex = 12; s.patchCount = 230;
			s.haveReading = true; s.lastXyz = { 1.5, 2.25, 3.125 };
			s.verifyMean = 0.812; s.verifyMax = 2.3; s.verifyPass = true;
			s.driftPercent = -1.25;
			SessionStatus back;
			Assert::IsTrue(ParseStatus(FormatStatus(s), back));
			Assert::IsTrue(back.step == SessionStep::Review);
			Assert::AreEqual(s.instruction, back.instruction);
			Assert::AreEqual(s.diff, back.diff);
			Assert::AreEqual(230, back.patchCount);
			Assert::AreEqual(2.25, back.lastXyz[1], 1e-6);
			Assert::AreEqual(0.812, back.verifyMean, 1e-3);
			Assert::IsTrue(back.verifyPass);
			Assert::AreEqual(-1.25, back.driftPercent, 1e-6);
			Assert::IsFalse(ParseStatus("garbage", back));
		}
	};

	TEST_CLASS(CalibrationConfigTests)
	{
	public:
		TEST_METHOD(SchemaAcceptsEveryKeyAndRejectsBadValues)
		{
			ConfigFile config;
			const std::string path = TempFolder("schema") + "\\VideoProcessor.cfg";
			WriteFile(path,
				"[calibration]\n"
				"argyll_path: C:\\Program Files\\ArgyllCMS\n"
				"meter_model: i1d3\nmeter_port: 1\ndisplay_type: l\nspectral_correction: none\n"
				"refresh_mode: non_refresh\ndisplay_kind: oled\ndisplay_name: Living room TV\nplan: thorough\n"
				"patch_window_percent: 10\nbackground_percent: 0\nsettle_ms: 1200\nnear_black_max_reads: 6\n"
				"repeat_tolerance_percent: 0.5\nread_timeout_s: 90\nwarmup_minutes: 20\ndrift_interval: 40\n"
				"target_transfer: gamma\ntarget_gamma: 2.2\ntarget_white_percent: 90\nlut_size: 65\nverify_after: false\n");
			Assert::IsTrue(config.Load(path, ConfigFile::ReadPolicy::Fresh));
			std::string error;
			Assert::IsTrue(ValidateConfig(config, error), std::wstring(error.begin(), error.end()).c_str());
			const CalibrationSettings s = ReadSettings(config);
			Assert::AreEqual(std::string("C:\\Program Files\\ArgyllCMS"), s.argyllPath);
			Assert::AreEqual(std::string("i1d3"), s.meterModel);
			Assert::AreEqual(1, s.meterPort);
			Assert::IsTrue(s.refreshMode == RefreshMode::NonRefresh);
			Assert::IsTrue(s.displayKind == DisplayKind::Oled);
			Assert::IsTrue(s.plan == PlanChoice::Thorough);
			Assert::AreEqual(10.0, EffectiveWindowPercent(s), 1e-12);
			Assert::AreEqual(6, s.nearBlackMaxReads);
			Assert::IsTrue(s.targetTransfer == TargetTransferChoice::Gamma);
			Assert::AreEqual(65, s.lutSize);
			Assert::IsFalse(s.verifyAfter);
			Assert::IsTrue(s.spectralCorrection.empty());
			// Every rule key appears exactly once in the schema.
			std::set<std::string> keys;
			for (const ConfigSchema::KeyRule& r : ConfigRules()) Assert::IsTrue(keys.insert(r.key).second);
			Assert::AreEqual(size_t(34), keys.size());   // 22 + 12 from calibration phase 2

			for (const char* bad : { "plan: huge\n", "meter_port: -1\n", "lut_size: 32\n", "display_type: l; rm\n",
				"patch_window_percent: 120\n", "unknown_key: 1\n", "verify_after: maybe\n" })
			{
				WriteFile(path, std::string("[calibration]\n") + bad);
				ConfigFile badConfig;
				Assert::IsTrue(badConfig.Load(path, ConfigFile::ReadPolicy::Fresh));
				Assert::IsFalse(ValidateConfig(badConfig, error), std::wstring(bad, bad + strlen(bad)).c_str());
			}
		}

		TEST_METHOD(DefaultsDeriveTheWindowFromTheDisplayKind)
		{
			CalibrationSettings s;
			Assert::AreEqual(100.0, EffectiveWindowPercent(s), 1e-12);   // projector: full field
			s.displayKind = DisplayKind::Oled;
			Assert::AreEqual(10.0, EffectiveWindowPercent(s), 1e-12);
			s.displayKind = DisplayKind::Lcd;
			Assert::AreEqual(18.0, EffectiveWindowPercent(s), 1e-12);
			s.patchWindowPercent = 25.0;
			Assert::AreEqual(25.0, EffectiveWindowPercent(s), 1e-12);
		}

		TEST_METHOD(MainSchemaOwnsTheCalibrationSection)
		{
			const std::string folder = TempFolder("mainschema");
			const std::string path = folder + "\\VideoProcessor.cfg";
			WriteFile(path, std::string(kConfig));
			std::wstring error;
			ConfigEditorCore::ConfigDocument document;
			Assert::IsTrue(document.Load(std::wstring(path.begin(), path.end()), error));
			Assert::IsTrue(ConfigEditorCore::ValidateCandidate(document, error), error.c_str());
			Assert::IsTrue(MainConfigSchema::OwnsSection("calibration"));
			document.SetKnown("calibration", "plan", "enormous");
			Assert::IsFalse(ConfigEditorCore::ValidateCandidate(document, error));
		}

		TEST_METHOD(ColorProfileSectionChoice)
		{
			const std::vector<std::string> sections = { "general", "vprenderer.color.Rec709", "vprenderer.color.BT2020" };
			Assert::AreEqual(std::string("vprenderer.color.BT2020"), ChooseColorProfileSection(sections, "BT2020"));
			Assert::AreEqual(std::string("vprenderer.color.BT2020"), ChooseColorProfileSection(sections, "color.bt2020"));
			Assert::AreEqual(std::string("vprenderer.color.BT2020"), ChooseColorProfileSection(sections, "vprenderer.color.BT2020"));
			Assert::AreEqual(std::string("vprenderer.color.Rec709"), ChooseColorProfileSection(sections, "Gone"));
			Assert::AreEqual(std::string(), ChooseColorProfileSection({ "general" }, "Rec709"));
		}

		TEST_METHOD(IndexAndJournalFiles)
		{
			const std::string folder = TempFolder("index");
			const std::string index = folder + "\\luts\\calibration\\index.json";
			std::string error;
			IndexEntry e;
			e.display = "FL35"; e.timestamp = "20261017-210000"; e.run = "luts/calibration/FL35/20261017-210000";
			e.status = "applied"; e.verifyMean = 0.8; e.verifyMax = 2.1;
			Assert::IsTrue(AppendIndex(index, e, error));
			e.status = "discarded"; e.verifyMean = -1.0;
			Assert::IsTrue(AppendIndex(index, e, error));
			const std::string text = ReadFile(index);
			Assert::AreEqual(size_t(1), static_cast<size_t>(std::count(text.begin(), text.end(), '[')));
			Assert::IsTrue(text.find("\"status\":\"applied\"") < text.find("\"status\":\"discarded\""));
			Assert::IsTrue(text.find("},\n  {") != std::string::npos);
			JournalEntry j;
			j.index = 3; j.phase = "measure"; j.patch.label = "grey \"50\"%"; j.status = "ok";
			const std::string line = FormatJournalLine(j);
			Assert::IsTrue(line.find("\"label\":\"grey \\\"50\\\"%\"") != std::string::npos);
			Assert::IsTrue(line.front() == '{' && line.back() == '}');
		}

		TEST_METHOD(ControllerAnswersWithoutARun)
		{
			Calibration::CalibrationController::Hooks hooks;
			const std::string folder = TempFolder("controller");
			const std::string path = folder + "\\VideoProcessor.cfg";
			WriteFile(path, std::string(kConfig) + "argyll_path: Z:\\No Argyll Here\n");
			hooks.configPath = [path]() { return path; };
			hooks.renderer = []() { return std::shared_ptr<IVideoRenderer>(); };
			// A meter factory that refuses (as the real one does without Argyll).
			hooks.meterFactory = [](const CalibrationSettings&, IPatternSink&, double&, std::string& error)
			{
				error = ArgyllInstallGuidance("Z:\\No Argyll Here\\bin\\spotread.exe");
				return std::unique_ptr<IMeter>();
			};
			ManualClock clock;
			hooks.clock = &clock;
			Calibration::CalibrationController controller(hooks);
			SessionStatus s;
			Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
			Assert::IsTrue(s.step == SessionStep::Idle);
			// WP-12 (module findings item 7): the meter is opened on the worker, so a missing
			// ArgyllCMS arrives as the run's status (Failed), not as the start answer.
			const std::string started = controller.Handle("start", "");
			Assert::IsTrue(started.find("error:") == std::string::npos, std::wstring(started.begin(), started.end()).c_str());
			for (int i = 0; i < 300; ++i)
			{
				Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
				if (IsFinal(s.step)) break;
				Sleep(10);
			}
			Assert::IsTrue(s.step == SessionStep::Failed);
			Assert::IsTrue(s.message.find("ArgyllCMS was not found") == 0, std::wstring(s.message.begin(), s.message.end()).c_str());
			Assert::IsTrue(controller.Handle("continue", "").find("error: No calibration is running.") == 0);
			Assert::IsTrue(controller.Handle("bogus", "").find("error:") == 0);
			Assert::IsFalse(controller.Active());
			controller.Shutdown();
		}

		// WP-12 (module findings item 7): `start` answers at once while the worker searches for
		// spotread and opens the meter; the RPC thread never waits for it. A cancel sent while the
		// run is still starting is kept and applied when it starts.
		TEST_METHOD(ControllerStartDoesNotBlockTheRpcThread)
		{
			Calibration::CalibrationController::Hooks hooks;
			const std::string folder = TempFolder("controller-async");
			const std::string path = folder + "\\VideoProcessor.cfg";
			WriteFile(path, kConfig);
			hooks.configPath = [path]() { return path; };
			hooks.renderer = []() { return std::shared_ptr<IVideoRenderer>(); };
			auto release = std::make_shared<std::atomic<bool>>(false);
			hooks.meterFactory = [release](const CalibrationSettings&, IPatternSink&, double&, std::string& error)
			{
				for (int i = 0; i < 500 && !release->load(); ++i) Sleep(10);   // a slow spotread search
				error = "no meter (test)";
				return std::unique_ptr<IMeter>();
			};
			ManualClock clock;
			hooks.clock = &clock;
			Calibration::CalibrationController controller(hooks);
			const ULONGLONG before = GetTickCount64();
			const std::string started = controller.Handle("start", "");
			Assert::IsTrue(GetTickCount64() - before < 1000, L"start returned without waiting for the meter");
			SessionStatus s;
			Assert::IsTrue(ParseStatus(started, s));
			Assert::IsTrue(s.step == SessionStep::OpeningMeter);
			Assert::AreEqual(std::string("starting"), s.message);
			Assert::IsTrue(controller.Active());
			Assert::IsTrue(controller.Handle("start", "").find("error: A calibration is already running.") == 0);
			Assert::IsTrue(controller.Handle("continue", "").find("error: The calibration is still starting.") == 0);
			release->store(true);
			for (int i = 0; i < 300; ++i)
			{
				Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
				if (IsFinal(s.step)) break;
				Sleep(10);
			}
			Assert::IsTrue(s.step == SessionStep::Failed);
			Assert::AreEqual(std::string("no meter (test)"), s.message);
			Assert::IsFalse(controller.Active());
			controller.Shutdown();
		}

		TEST_METHOD(ControllerRunsADryRunWithoutARenderer)
		{
			// No renderer: the session opens the meter, then fails clearly (patterns
			// need VP's renderer); nothing is written to the configuration.
			Calibration::CalibrationController::Hooks hooks;
			const std::string folder = TempFolder("controller-dry");
			const std::string path = folder + "\\VideoProcessor.cfg";
			WriteFile(path, kConfig);
			const std::string original = ReadFile(path);
			hooks.configPath = [path]() { return path; };
			hooks.renderer = []() { return std::shared_ptr<IVideoRenderer>(); };
			Calibration::CalibrationController controller(hooks);
			controller.Handle("start", "");
			SessionStatus s;
			for (int i = 0; i < 200; ++i)
			{
				Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
				if (IsFinal(s.step)) break;
				Sleep(10);
			}
			Assert::IsTrue(s.step == SessionStep::Failed);
			Assert::IsTrue(s.message.find("cannot show calibration patches") != std::string::npos);
			Assert::AreEqual(original, ReadFile(path));
			controller.Shutdown();
		}

		// Module findings 2026-10-24 item 7: the .ccss refusal ignores letter case.
		TEST_METHOD(SpectralSampleFileIsRecognisedInAnyCase)
		{
			using Calibration::IsSpectralSampleFile;
			Assert::IsTrue(IsSpectralSampleFile("C:/x/laser.ccss"));
			Assert::IsTrue(IsSpectralSampleFile("C:/x/laser.CCSS"));
			Assert::IsTrue(IsSpectralSampleFile("C:/x/laser.Ccss"));
			Assert::IsFalse(IsSpectralSampleFile("C:/x/laser.ccmx"));
			Assert::IsFalse(IsSpectralSampleFile("ccss"));
			Assert::IsFalse(IsSpectralSampleFile(""));
		}
	};
}
