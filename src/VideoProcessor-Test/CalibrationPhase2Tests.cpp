#include "pch.h"
#include "CppUnitTest.h"

#include "CalibrationTestSupport.h"

#include <calibration/AmbientCheck.h>
#include <calibration/ArgyllInstall.h>
#include <calibration/CalibrationConfig.h>
#include <calibration/CalibrationController.h>
#include <calibration/CalibrationRunBase.h>
#include <calibration/CalibrationSimulation.h>
#include <calibration/HdrVerify.h>
#include <calibration/LedRun.h>
#include <calibration/Meter.h>
#include <calibration/PatternCodes.h>
#include <calibration/TranscriptRecorder.h>
#include <ConfigEditorCore.h>
#include <ConfigFile.h>
#include <led/LedDriveConfig.h>
#include <led/SerialPort.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Calibration;
using namespace CalibrationTest;

// Automatic calibration phase 2 (docs/agent-reports/calibration-phase2.md),
// CPU only: the Dynamic Drive (LED) characterisation run's state machine on a
// simulated FL35 (flux with hysteresis, CNTR gain with the DMD clip, LED / CNTR
// dynamics) with a fake projector and fast sensor - every command within the
// limits, full light and CNTR 50 restored on completion, cancel, failures and
// destruction, the crash marker and its recovery through the real driver on a
// fake port; the map fitting; the ambient-light check; the PQ / ICtCp / dE ITP
// / BT.2390 maths and the HDR verify run on a simulated PQ display; transcript
// recording and replay; the new settings and the controller's run kinds.
namespace
{
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

	bool Exists(const std::string& path)
	{
		return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	std::string TempFolder(const std::string& name)
	{
		char base[MAX_PATH];
		GetTempPathA(MAX_PATH, base);
		const std::string folder = std::string(base) + "vp-calibration-tests\\p2-" + name + "-" +
			std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
		std::string error;
		CreateFolders(folder, error);
		return folder;
	}

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	PatternOutputInfo Format(int container, int code, bool limited, SignalTransfer transfer = SignalTransfer::Sdr)
	{
		PatternOutputInfo f;
		f.available = true;
		f.containerBits = static_cast<uint32_t>(container);
		f.codeBits = static_cast<uint32_t>(code);
		f.limitedRange = limited;
		f.transfer = transfer;
		f.bt2020 = transfer == SignalTransfer::Pq;
		f.width = 3840;
		f.height = 2160;
		f.refreshHz = 59.94;
		return f;
	}

	const char* kConfig =
		"# VP test configuration\r\n"
		"[general]\r\n"
		"fullscreen: false\r\n"
		"\r\n"
		"[vprenderer.color.Rec709]\r\n"
		"calibration_lut_enabled: false\r\n"
		"calibration_lut_bt709: none\r\n"
		"output_gamma: 2.2\r\n"
		"\r\n"
		"[projector.default]\r\n"
		"protocol: barco_ascii   # FL35\r\n"
		"port: COM7\r\n"
		"cntr_max_usable: 74\r\n"
		"dmd_clip_cntr: 59\r\n"
		"\r\n"
		"[led_drive.default]\r\n"
		"# the Dynamic Drive's own settings (none needed for the run)\r\n"
		"\r\n"
		"[calibration]\r\n"
		"meter_model: simulated   # dry run\r\n"
		"warmup_minutes: 0\r\n";

	// ------------------------------------------------- a simulated FL35

	// What the projector was told, shared with the test after the run is gone.
	struct ProjectorLog
	{
		struct Command { char kind; int value; double at; };   // 'L' LPW1, 'C' CNTR, 'R' restore
		std::vector<Command> commands;
		bool open = false;
		int opens = 0, restores = 0, closes = 0;
		int light = 255, contrast = 50;
		bool restoreOk = true;
		bool failOpen = false;
		int failLightAfter = -1;      // the n-th SetLight fails
		int lights = 0;
	};

	class FakeProjector final : public IProjectorControl
	{
	public:
		FakeProjector(std::shared_ptr<ProjectorLog> l, SimulatedDisplay* d, ISessionClock* c) : log(std::move(l)), display(d), clock(c) {}
		bool Open(std::string& error) override
		{
			if (log->failOpen) { error = "COM7 is busy (test)"; return false; }
			log->open = true;
			++log->opens;
			return true;
		}
		bool SetLight(int level, double& at) override
		{
			if (!log->open) return false;
			++log->lights;
			if (log->failLightAfter >= 0 && log->lights > log->failLightAfter) return false;
			at = clock->Now();
			log->light = level;
			log->commands.push_back({ 'L', level, at });
			if (display) display->SetLpw(level);
			return true;
		}
		bool SetContrast(int value, double& at) override
		{
			if (!log->open) return false;
			at = clock->Now();
			log->contrast = value;
			log->commands.push_back({ 'C', value, at });
			if (display) display->SetCntr(value);
			return true;
		}
		bool Restore(int ceiling, std::string& report) override
		{
			if (!log->open) { report = "closed"; return false; }
			++log->restores;
			log->light = ceiling;
			log->contrast = 50;
			log->commands.push_back({ 'R', ceiling, clock->Now() });
			if (display) { display->SetLpw(ceiling); display->SetCntr(50); }
			report = log->restoreOk ? "LPW1 sent, LDMM 0 sent, CNTR 50 sent" : "CNTR 50 NOT confirmed (test)";
			return log->restoreOk;
		}
		void Close() override { if (log->open) ++log->closes; log->open = false; }
		bool IsOpen() const override { return log->open; }

	private:
		std::shared_ptr<ProjectorLog> log;
		SimulatedDisplay* display;
		ISessionClock* clock;
	};

	// The screen, remembering when the picture changed (the picture steps).
	class TimedScreen final : public IPatternSink
	{
	public:
		TimedScreen(const PatternOutputInfo& f, ISessionClock* c) : inner(f), clock(c) {}
		bool GetOutput(PatternOutputInfo& info) override { return inner.GetOutput(info); }
		bool Show(const PatternRequest& r, std::string& error) override
		{
			if (!inner.Show(r, error)) return false;
			Vec3 d;
			if (inner.Showing(d)) changes.push_back(std::make_pair(clock->Now(), (d[0] + d[1] + d[2]) / 3.0));
			return true;
		}
		SimulatedScreen inner;
		ISessionClock* clock;
		std::vector<std::pair<double, double>> changes;
	};

	// A fast light sensor on the simulated projector: the LED (display
	// params' dead time / tau), CNTR and picture each a dead time plus a
	// first-order lag of what was commanded, multiplied together.
	struct SensorModel
	{
		double cntrDead = 0.060, cntrTau = 0.030;
		double pictureDead = 0.030, pictureTau = 0.004;
		double rateHz = 250.0;
	};

	class FakeFastSensor final : public ITimedLightSensor
	{
	public:
		FakeFastSensor(std::shared_ptr<ProjectorLog> l, TimedScreen* s, const SimulatedDisplayParams& p, ISessionClock* c,
			SensorModel m = SensorModel())
			: log(std::move(l)), screen(s), display(p), params(p), clock(c), model(m), rng(5u) {}
		bool Open(std::string&) override { open = true; return true; }
		void Begin() override { start = clock->Now(); }
		std::vector<TimedSample> Take() override
		{
			std::vector<TimedSample> out;
			const double end = clock->Now();
			std::normal_distribution<double> noise(0.0, 0.002);
			for (double t = start; t <= end; t += 1.0 / model.rateHz)
			{
				TimedSample s;
				s.seconds = t;
				s.value = Light(t) * (1.0 + noise(rng));
				out.push_back(s);
			}
			return out;
		}
		void Close() override { open = false; }
		bool open = false;

	private:
		double Channel(double t, char kind, double initial, double dead, double tau, double ramp) const
		{
			double value = initial, previous = initial;
			for (const ProjectorLog::Command& c : log->commands)
			{
				if (c.at > t) break;
				double target = previous;
				if (kind == 'L' && (c.kind == 'L' || c.kind == 'R')) target = display.RelativeFlux(c.value, false);
				else if (kind == 'C' && (c.kind == 'C' || c.kind == 'R'))
					target = display.Gain(c.kind == 'R' ? 50 : c.value) / display.Gain(50);
				else continue;
				value += (target - previous) * StepModel(t - c.at, dead, tau, ramp);
				previous = target;
			}
			return value;
		}
		double Picture(double t) const
		{
			double value = 0.02, previous = 0.02;
			for (const auto& c : screen->changes)
			{
				if (c.first > t) break;
				const double target = 0.02 + 0.98 * (std::min)((std::max)(c.second, 0.0), 1.0);
				value += (target - previous) * StepModel(t - c.first, model.pictureDead, model.pictureTau, 0.0);
				previous = target;
			}
			return value;
		}
		double Light(double t) const
		{
			return Channel(t, 'L', 1.0, params.ledDeadTimeSeconds, params.ledTauSeconds, params.ledRampSeconds) *
				Channel(t, 'C', 1.0, model.cntrDead, model.cntrTau, 0.0) * Picture(t);
		}

		std::shared_ptr<ProjectorLog> log;
		TimedScreen* screen;
		SimulatedDisplay display;
		SimulatedDisplayParams params;
		ISessionClock* clock;
		SensorModel model;
		double start = 0.0;
		mutable std::mt19937 rng;
	};

	// A meter that can be told to die (a fatal error) after n readings.
	class FlakyMeter final : public IMeter
	{
	public:
		explicit FlakyMeter(std::unique_ptr<SimulatedScreenMeter> m) : inner(std::move(m)) {}
		MeterResult Open() override { return inner->Open(); }
		MeterResult Continue() override { return inner->Continue(); }
		MeterResult Read() override
		{
			if (fatalAfter >= 0 && reads++ >= fatalAfter)
			{
				MeterResult r;
				r.status = MeterStatus::Fatal;
				r.message = "The meter was unplugged (test).";
				return r;
			}
			MeterResult r = inner->Read();
			if (r.status == MeterStatus::Ok && veil) r.xyz = Add(r.xyz, veil());
			return r;
		}
		void Close() override { inner->Close(); }
		bool IsOpen() const override { return inner->IsOpen(); }
		std::string Description() const override { return "Simulated meter (test)"; }
		std::unique_ptr<SimulatedScreenMeter> inner;
		int fatalAfter = -1;
		int reads = 0;
		std::function<Vec3()> veil;   // room light reflected by the screen
	};

	struct LedRig
	{
		std::string folder, config;
		ManualClock clock;
		std::unique_ptr<TimedScreen> screen;
		FlakyMeter* meter = nullptr;
		std::shared_ptr<ProjectorLog> log = std::make_shared<ProjectorLog>();
		FakeFastSensor* sensor = nullptr;
		std::unique_ptr<LedCharacterizationRun> run;
		CalibrationSettings settings;
		SimulatedDisplayParams displayParams;

		LedRig(const std::string& name, bool withSensor, const PatternOutputInfo& format = Format(10, 8, true),
			std::function<void(CalibrationSettings&)> tweak = nullptr)
		{
			folder = TempFolder(name);
			config = folder + "\\VideoProcessor.cfg";
			WriteFile(config, kConfig);
			screen.reset(new TimedScreen(format, &clock));
			settings.warmupMinutes = 0;
			settings.displayKind = DisplayKind::Projector;
			settings.ledFluxStep = 5;
			settings.ledDwellMs = 3000;
			settings.ledReadsPerLevel = 2;
			settings.ledCntrSweepMax = 74;
			settings.ledStepRepeats = 3;
			if (tweak) tweak(settings);
			SimulatedMeterParams mp;
			mp.proportionalNoise = 0.0015;
			mp.seed = 21u;
			TimedScreen* s = screen.get();
			auto sim = std::unique_ptr<SimulatedScreenMeter>(new SimulatedScreenMeter(
				[s](Vec3& device) { return s->inner.Showing(device); }, displayParams, mp, &clock));
			SimulatedDisplay* display = &sim->Display();
			auto flaky = std::unique_ptr<FlakyMeter>(new FlakyMeter(std::move(sim)));
			meter = flaky.get();
			std::unique_ptr<ITimedLightSensor> fast;
			if (withSensor)
			{
				auto f = std::unique_ptr<FakeFastSensor>(new FakeFastSensor(log, screen.get(), displayParams, &clock));
				sensor = f.get();
				fast = std::move(f);
			}
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = config;
			env.activeColorSection = "Rec709";
			env.displayName = "FL35";
			ConfigFile cfg;
			cfg.Load(config, ConfigFile::ReadPolicy::Fresh);
			run.reset(new LedCharacterizationRun(settings, env, std::move(flaky), 0.002, *screen, clock,
				std::unique_ptr<IProjectorControl>(new FakeProjector(log, display, &clock)), std::move(fast),
				LedDrive::ReadProjectorConfig(cfg), LedDrive::ReadLedDriveConfig(cfg)));
		}

		bool RunUntil(SessionStep wanted, int bound = 200000)
		{
			for (int i = 0; i < bound; ++i)
			{
				const SessionStep step = run->Status().step;
				if (step == wanted) return true;
				if (IsFinal(step)) return false;
				if (step == SessionStep::Placement && run->Status().haveReading) run->Continue();
				run->Step();
			}
			return run->Status().step == wanted;
		}

		std::wstring Why() const
		{
			const SessionStatus s = run->Status();
			return W(std::string(ToString(s.step)) + ": " + s.message);
		}

		// Every command the run sent was inside the limits.
		void AssertCommandsInLimits() const
		{
			const LedLimits& l = run->Limits();
			for (const ProjectorLog::Command& c : log->commands)
			{
				if (c.kind == 'L')
				{
					Assert::IsTrue(c.value >= l.sweepLow && c.value <= l.ceiling, (L"LPW1 out of range: " + std::to_wstring(c.value)).c_str());
					Assert::IsTrue(c.value >= l.lightMin && c.value <= l.lightMax);
				}
				if (c.kind == 'C')
					Assert::IsTrue(c.value >= 50 && c.value <= l.contrastMax, (L"CNTR out of range: " + std::to_wstring(c.value)).c_str());
			}
		}

		void AssertRestored() const
		{
			Assert::IsTrue(log->restores >= 1, L"full light / CNTR 50 restored");
			Assert::IsFalse(log->commands.empty());
			Assert::IsTrue(log->commands.back().kind == 'R', L"the restore was the projector's last command");
			Assert::AreEqual(255, log->light);
			Assert::AreEqual(50, log->contrast);
			Assert::IsFalse(log->open, L"the projector connection is closed");
		}
	};

	// A fake COM port for the real Barco driver (RecoverInterruptedLedRun).
	struct PortLog
	{
		std::string written;
		bool open = false;
	};

	class FakeSerialPort final : public LedDrive::ISerialPort
	{
	public:
		explicit FakeSerialPort(std::shared_ptr<PortLog> l) : log(std::move(l)) {}
		bool Open(const std::string&, int, std::string&) override { log->open = true; return true; }
		void Close() override { log->open = false; }
		bool IsOpen() const override { return log->open; }
		void DiscardInput() override {}
		bool Write(const std::string& bytes, std::string&) override { log->written += bytes; return true; }
		bool ReadLine(int, std::string& line) override { line.clear(); return false; }
	private:
		std::shared_ptr<PortLog> log;
	};

	std::string Upper(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return s;
	}

	// ------------------------------------------------------- PQ display

	// A display that tracks PQ in BT.2020 with a per-channel gain error, a black
	// floor and a peak clip.
	class PqDisplayMeter final : public IMeter
	{
	public:
		PqDisplayMeter(SimulatedScreen* s, double peak, Vec3 gain, ISessionClock* c) : screen(s), peakNits(peak), gains(gain), clock(c) {}
		MeterResult Open() override { open = true; MeterResult r; r.status = MeterStatus::Ok; return r; }
		MeterResult Continue() override { MeterResult r; r.status = MeterStatus::Ok; return r; }
		MeterResult Read() override
		{
			MeterResult r;
			Vec3 d;
			if (!screen->Showing(d)) d = { 0.5, 0.5, 0.5 };
			Vec3 rgb;
			for (int k = 0; k < 3; ++k)
				rgb[k] = (std::min)(PqDecode((std::min)((std::max)(d[k], 0.0), 1.0)) * gains[k], peakNits) + 0.0005;
			r.xyz = RgbToXyz(Gamuts::Bt2020()) * rgb;
			r.status = MeterStatus::Ok;
			if (clock) clock->Sleep(0.8);
			++reads;
			return r;
		}
		void Close() override { open = false; }
		bool IsOpen() const override { return open; }
		std::string Description() const override { return "PQ display (test)"; }
		int reads = 0;
	private:
		SimulatedScreen* screen;
		double peakNits;
		Vec3 gains;
		ISessionClock* clock;
		bool open = false;
	};

	void FillToneCurve(PatternOutputInfo& f, double sourcePeak, double targetPeak)
	{
		f.toneCurveCount = 64;
		for (uint32_t i = 0; i < 64; ++i)
		{
			// As the renderer samples: evenly in PQ.
			const double nits = PqDecode(PqEncode(0.005) + (PqEncode(sourcePeak) - PqEncode(0.005)) * i / 63.0);
			f.toneCurveSourceNits[i] = static_cast<float>(nits);
			f.toneCurveOutputNits[i] = static_cast<float>(Bt2390Eetf(nits, 0.0, sourcePeak, 0.0, targetPeak));
		}
		f.sourcePeakNits = static_cast<float>(sourcePeak);
		f.targetPeakNits = static_cast<float>(targetPeak);
		f.toneMappingActive = targetPeak < sourcePeak;
		strncpy_s(f.toneFunction, sizeof(f.toneFunction), "bt2390", _TRUNCATE);
		f.sentMaxLuma = 1000.0f;
		f.sentMinLuma = 0.005f;
		f.sentMaxCll = 1000.0f;
		f.sentMaxFall = 400.0f;
	}

	template <typename Run>
	bool StepUntil(Run& run, SessionStep wanted, int bound = 100000)
	{
		for (int i = 0; i < bound; ++i)
		{
			const SessionStep step = run.Status().step;
			if (step == wanted) return true;
			if (IsFinal(step)) return false;
			if (step == SessionStep::Placement && run.Status().haveReading) run.Continue();
			run.Step();
		}
		return run.Status().step == wanted;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(CalibrationPhase2Tests)
	{
	public:
		// ------------------------------------------------------- settings

		TEST_METHOD(Phase2SettingsHaveDefaultsAndRanges)
		{
			ConfigFile config;
			CalibrationSettings defaults = ReadSettings(config);
			Assert::IsFalse(defaults.recordTranscripts);
			Assert::AreEqual(5, defaults.ledFluxStep);
			Assert::AreEqual(3000, defaults.ledDwellMs);
			Assert::AreEqual(74, defaults.ledCntrSweepMax);
			Assert::IsTrue(defaults.ledSensorPort.empty());
			Assert::AreEqual(std::string("vp"), defaults.hdrVerifyToneCurve);
			const std::string folder = TempFolder("settings");
			const std::string path = folder + "\\VideoProcessor.cfg";
			WriteFile(path, "[calibration]\nrecord_transcripts: on\ntranscript_folder: D:\\rec\nled_flux_step: 10\n"
				"led_dwell_ms: 2500\nled_reads_per_level: 3\nled_cntr_sweep_max: 70\nled_cntr_dwell_ms: 900\n"
				"led_sensor_port: COM9\nled_sensor_baud: 921600\nled_step_repeats: 2\nhdr_verify_source_peak: 4000\n"
				"hdr_verify_tone_curve: bt2390\n");
			Assert::IsTrue(config.Load(path, ConfigFile::ReadPolicy::Fresh));
			std::string error;
			Assert::IsTrue(ValidateConfig(config, error), W(error).c_str());
			const CalibrationSettings s = ReadSettings(config);
			Assert::IsTrue(s.recordTranscripts);
			Assert::AreEqual(std::string("D:\\rec"), s.transcriptFolder);
			Assert::AreEqual(10, s.ledFluxStep);
			Assert::AreEqual(2500, s.ledDwellMs);
			Assert::AreEqual(3, s.ledReadsPerLevel);
			Assert::AreEqual(70, s.ledCntrSweepMax);
			Assert::AreEqual(900, s.ledCntrDwellMs);
			Assert::AreEqual(std::string("COM9"), s.ledSensorPort);
			Assert::AreEqual(921600, s.ledSensorBaud);
			Assert::AreEqual(2, s.ledStepRepeats);
			Assert::AreEqual(4000.0, s.hdrVerifySourcePeak, 1e-9);
			Assert::AreEqual(std::string("bt2390"), s.hdrVerifyToneCurve);
			for (const char* bad : { "led_cntr_sweep_max: 101", "led_cntr_sweep_max: 49", "led_sensor_port: LPT1",
				"led_flux_step: 0", "hdr_verify_tone_curve: spline", "led_dwell_ms: 100" })
			{
				WriteFile(path, std::string("[calibration]\n") + bad + "\n");
				Assert::IsTrue(config.Load(path, ConfigFile::ReadPolicy::Fresh));
				Assert::IsFalse(ValidateConfig(config, error), W(bad).c_str());
			}
		}

		// ------------------------------------------------- limits and plan

		TEST_METHOD(LedLimitsStayInsideTheProjectorAndProfileRanges)
		{
			LedDrive::ProjectorConfig projector;
			LedDrive::LedDriveConfig led;
			CalibrationSettings s;
			LedLimits l = ResolveLedLimits(projector, led, s, LedDrive::LightRange());
			Assert::AreEqual(0, l.lightMin);
			Assert::AreEqual(255, l.lightMax);
			Assert::AreEqual(60, l.sweepLow);       // led_floor_black
			Assert::AreEqual(255, l.ceiling);
			Assert::AreEqual(74, l.contrastMax);
			Assert::AreEqual(60, l.ClampLight(10));
			Assert::AreEqual(255, l.ClampLight(400));
			Assert::AreEqual(50, l.ClampContrast(20));
			Assert::AreEqual(74, l.ClampContrast(90));
			// The projector reports a narrower range; the ceiling is lowered.
			LedDrive::LightRange reported;
			reported.known = true;
			reported.minimum = 80;
			reported.maximum = 240;
			led.ledCeiling = 250;
			s.ledCntrSweepMax = 120;
			l = ResolveLedLimits(projector, led, s, reported);
			Assert::AreEqual(80, l.lightMin);
			Assert::AreEqual(240, l.lightMax);
			Assert::AreEqual(80, l.sweepLow);
			Assert::AreEqual(240, l.ceiling);
			Assert::AreEqual(100, l.contrastMax);   // never above the device maximum
			// Configured light_min / light_max narrow it too.
			projector.lightMin = 100;
			projector.lightMax = 200;
			l = ResolveLedLimits(projector, led, s, LedDrive::LightRange());
			Assert::AreEqual(100, l.sweepLow);
			Assert::AreEqual(200, l.ceiling);
		}

		TEST_METHOD(LedPlanSweepsBothWaysAndCountsUnits)
		{
			LedLimits l;
			CalibrationSettings s;
			LedPlan p = BuildLedPlan(l, s, false);
			Assert::AreEqual(size_t(11), p.greys.size());
			Assert::AreEqual(255, p.fluxDown.front());
			Assert::AreEqual(60, p.fluxDown.back());
			Assert::AreEqual(65, p.fluxUp.front());
			Assert::AreEqual(255, p.fluxUp.back());
			for (size_t i = 1; i < p.fluxDown.size(); ++i) Assert::IsTrue(p.fluxDown[i] < p.fluxDown[i - 1]);
			for (size_t i = 1; i < p.fluxUp.size(); ++i) Assert::IsTrue(p.fluxUp[i] > p.fluxUp[i - 1]);
			Assert::AreEqual(size_t(25), p.cntr.size());
			Assert::IsTrue(p.steps.empty());
			Assert::AreEqual(static_cast<int>(11 + p.fluxDown.size() + p.fluxUp.size() + 25), p.Units());
			p = BuildLedPlan(l, s, true);
			Assert::AreEqual(size_t(18), p.steps.size());   // 3 x (LED down/up, CNTR up/down, picture up/down)
			const double minutes = EstimateLedSeconds(p, s) / 60.0;
			Logger::WriteMessage(("estimated LED run: " + std::to_string(minutes) + " min\n").c_str());
			Assert::IsTrue(minutes > 5.0 && minutes < 25.0);
			Assert::IsTrue(LedBlackLevel(p, 0) && LedBlackLevel(p, p.fluxDown.size() - 1) && LedBlackLevel(p, 4) &&
				!LedBlackLevel(p, 1));
		}

		// ----------------------------------------------------- map fitting

		TEST_METHOD(GreyscaleGammaFitRecoversThePower)
		{
			std::vector<std::pair<double, double>> greys;
			for (int i = 10; i >= 0; --i)
			{
				const double v = i / 10.0;
				greys.push_back(std::make_pair(v, 0.05 + 47.95 * std::pow(v, 2.35)));
			}
			Assert::AreEqual(2.35, FitGreyscaleGamma(greys), 1e-6);
			Assert::AreEqual(0.0, FitGreyscaleGamma({ { 1.0, 10.0 } }), 1e-12);
		}

		TEST_METHOD(LedResultEditsAndProfilePatch)
		{
			LedResults r;
			std::vector<FluxStep> steps;
			for (int lpw : { 60, 100, 180, 255 })
				for (StepDirection d : { StepDirection::Up, StepDirection::Down })
				{
					FluxStep s;
					s.lpw = lpw;
					s.direction = d;
					const double f = std::pow(lpw / 255.0, 0.85) * (d == StepDirection::Down ? 1.01 : 1.0);
					s.samples = { { 1.0, f }, { 2.0, f } };
					steps.push_back(s);
				}
			FluxFitOptions o;
			o.settledFraction = 1.0;
			o.referenceLpw = 255;
			r.flux = FitFluxTable(steps, o);
			Assert::IsTrue(r.flux.ok);
			r.cntr.ok = true;
			r.cntr.cntrMaxUsable = 61;
			r.cntr.dmdClipCntr = 61;
			r.cntr.clipReached = true;
			r.led.ok = true;
			r.led.latencyMs = 82;
			r.led.settleMs = 118;
			r.compensationEotf = 2.31;
			const std::vector<ConfigEdit> edits = LedResultEdits(r);
			std::map<std::string, std::string> byKey;
			for (const ConfigEdit& e : edits) byKey[e.section + "/" + e.key] = e.value;
			Assert::AreEqual(std::string("82"), byKey["projector.default/led_latency_ms"]);
			Assert::AreEqual(std::string("118"), byKey["projector.default/led_settle_ms"]);
			Assert::AreEqual(std::string("61"), byKey["projector.default/cntr_max_usable"]);
			Assert::AreEqual(std::string("61"), byKey["projector.default/dmd_clip_cntr"]);
			Assert::AreEqual(std::string("2.31"), byKey["led_drive.default/compensation_eotf"]);
			Assert::IsTrue(byKey["projector.default/flux_table"].find("255:1.000000") != std::string::npos);
			Assert::IsTrue(byKey.find("projector.default/cntr_latency_ms") == byKey.end());   // not measured: unchanged
			// The flux table is a valid flux_table and a valid profile curve.
			std::vector<std::pair<double, double>> parsed;
			Assert::IsTrue(LedDrive::ParseFluxTable(byKey["projector.default/flux_table"], parsed));
			LedLimits l;
			const std::string patch = FormatProjectorProfilePatch(r, l);
			Assert::IsTrue(patch.find("[light]\ncurve = " + r.flux.ConfigValue()) != std::string::npos);
			Assert::IsTrue(patch.find("max_usable = 61") != std::string::npos);
			Assert::IsTrue(patch.find("latency_ms = 82") != std::string::npos);
			Assert::IsTrue(patch.find("# contrast_latency_ms") != std::string::npos);
			Assert::IsTrue(FormatLedCfg(r).find("[projector.default]\nflux_table: ") != std::string::npos);
		}

		// -------------------------------------------------- the LED run

		TEST_METHOD(LedRunMeasuresTheMapsAndRestoresTheProjector)
		{
			LedRig rig("led-full", true);
			const std::string original = ReadFile(rig.config);
			rig.run->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring), rig.Why().c_str());
			Assert::IsTrue(rig.log->open, L"the projector opens once the patterns show");
			Assert::IsTrue(Exists(LedMarkerPath(rig.folder)), L"the crash marker exists while the run drives the projector");
			Assert::IsTrue(rig.RunUntil(SessionStep::Review), rig.Why().c_str());
			const SessionStatus review = rig.run->Status();
			Logger::WriteMessage(("review:\n" + review.review + "\ndiff:\n" + review.diff + "\n").c_str());
			rig.AssertCommandsInLimits();
			rig.AssertRestored();
			Assert::IsFalse(Exists(LedMarkerPath(rig.folder)), L"the marker is removed after a confirmed restore");
			Assert::IsFalse(rig.screen->inner.PatternActive(), L"video is back for the review");
			Assert::AreEqual(original, ReadFile(rig.config), L"nothing written before Apply");

			const LedResults& r = rig.run->Results();
			const SimulatedDisplay model(rig.displayParams);
			// Flux table: the mean of the two directions, relative to full light.
			Assert::IsTrue(r.flux.ok, W(r.flux.error).c_str());
			const double reference = model.RelativeFlux(255, false);
			for (const FluxPoint& p : r.flux.points)
			{
				const double up = model.RelativeFlux(p.lpw, false) / reference;
				const double down = model.RelativeFlux(p.lpw, true) / reference;
				const double want = p.haveUp && p.haveDown ? 0.5 * (up + down) : p.haveUp ? up : down;
				Assert::AreEqual(want, p.flux, 0.015 * want, (L"flux at LPW1 " + std::to_wstring(p.lpw)).c_str());
			}
			Assert::IsTrue(r.flux.maxHysteresis > 0.004 && r.flux.maxHysteresis < 0.03, L"the hysteresis is seen");
			// CNTR: code 235 reaches the DMD maximum where gain(c) = 1.
			const double clip = 50.0 + (1.0 / rig.displayParams.gainAtReference - 1.0) / rig.displayParams.gainPerStep;
			Assert::IsTrue(r.cntr.ok, W(r.cntr.error).c_str());
			Assert::IsTrue(r.cntr.clipReached);
			Assert::AreEqual(clip, static_cast<double>(r.cntr.dmdClipCntr), 1.0);
			Assert::IsTrue(r.cntr.cntrMaxUsable >= r.cntr.dmdClipCntr - 1 && r.cntr.cntrMaxUsable <= 74);
			Assert::IsTrue(r.blackByLight.size() >= 10);
			Assert::IsTrue(r.blackByLight.front().second > r.blackByLight.back().second, L"black falls with the LED");
			Assert::IsTrue(r.blackByContrast.size() >= 6);
			Assert::IsTrue(r.compensationEotf > 2.0 && r.compensationEotf < 2.5);
			// Dead time within 10 ms, settle (tau) within 10%.
			Assert::IsTrue(r.led.ok);
			Assert::AreEqual(rig.displayParams.ledDeadTimeSeconds * 1000.0, static_cast<double>(r.led.latencyMs), 10.0);
			Assert::AreEqual(rig.displayParams.ledTauSeconds * 1000.0, static_cast<double>(r.led.settleMs),
				0.1 * rig.displayParams.ledTauSeconds * 1000.0);
			Assert::IsTrue(r.contrast.ok);
			Assert::AreEqual(60.0, static_cast<double>(r.contrast.latencyMs), 10.0);
			Assert::AreEqual(30.0, static_cast<double>(r.contrast.settleMs), 5.0);
			Assert::IsTrue(r.havePictureOffset);
			Assert::AreEqual(50.0, r.ledAfterPictureSeconds * 1000.0, 10.0);   // 80 ms LED - 30 ms picture
			Assert::IsTrue(r.restoreConfirmed);

			// Files in the run folder.
			const RunFolder& f = rig.run->Folder();
			Assert::IsTrue(f.runDir.find("-led") != std::string::npos);
			Assert::IsTrue(ReadFile(f.runDir + "\\led_characterisation.cfg").find("flux_table: ") != std::string::npos);
			Assert::IsTrue(ReadFile(f.runDir + "\\projector_profile_measured.txt").find("[light]") != std::string::npos);
			Assert::IsTrue(ReadFile(f.runDir + "\\led.json").find("\"restore_confirmed\": true") != std::string::npos);
			Assert::IsTrue(ReadFile(f.rawJournal).find("\"phase\":\"flux\"") != std::string::npos);
			Assert::IsTrue(review.diff.find("[projector.default] flux_table: (absent) -> 60:") != std::string::npos);
			Assert::IsTrue(review.diff.find("[projector.default] dmd_clip_cntr: 59 -> ") != std::string::npos);

			// Apply: backups first, key-only edits, still valid.
			rig.run->Apply();
			for (int i = 0; i < 10 && !rig.run->Finished(); ++i) rig.run->Step();
			const SessionStatus done = rig.run->Status();
			Assert::IsTrue(done.step == SessionStep::Applied, W(done.message).c_str());
			const size_t at = done.backup.find("Backup: ");
			const std::string backup = done.backup.substr(at + 8, done.backup.find('\n') - at - 8);
			Assert::AreEqual(original, ReadFile(backup));
			Assert::AreEqual(original, ReadFile(BeforeCopyPath(f, rig.config)));
			const std::string edited = ReadFile(rig.config);
			Assert::IsTrue(edited.find("protocol: barco_ascii   # FL35") != std::string::npos, L"comments kept");
			Assert::IsTrue(edited.find("meter_model: simulated   # dry run") != std::string::npos);
			Assert::IsTrue(edited.find("led_latency_ms: ") != std::string::npos);
			Assert::IsTrue(edited.find("compensation_eotf: ") != std::string::npos);
			std::wstring error;
			ConfigEditorCore::ConfigDocument document;
			Assert::IsTrue(document.Load(std::wstring(rig.config.begin(), rig.config.end()), error));
			Assert::IsTrue(ConfigEditorCore::ValidateCandidate(document, error), error.c_str());
			ConfigFile reloaded;
			Assert::IsTrue(reloaded.Load(rig.config, ConfigFile::ReadPolicy::Fresh));
			const LedDrive::ProjectorConfig pc = LedDrive::ReadProjectorConfig(reloaded);
			Assert::AreEqual(r.cntr.dmdClipCntr, pc.dmdClipCntr);
			Assert::AreEqual(r.flux.table.size(), pc.fluxTable.size());
		}

		TEST_METHOD(LedRunWithoutAFastSensorSkipsTheTimings)
		{
			LedRig rig("led-nosensor", false, Format(8, 8, true), [](CalibrationSettings& s)
			{
				s.ledFluxStep = 15;
				s.ledReadsPerLevel = 1;
				s.ledCntrSweepMax = 66;
			});
			rig.run->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Review), rig.Why().c_str());
			const LedResults& r = rig.run->Results();
			Assert::IsTrue(r.flux.ok);
			Assert::IsFalse(r.led.ok);
			Assert::IsFalse(r.contrast.ok);
			Assert::IsTrue(r.lightSteps.empty());
			Assert::IsTrue(rig.run->Status().review.find("not measured (set led_sensor_port") != std::string::npos);
			const std::string diff = rig.run->Status().diff;
			Assert::IsTrue(diff.find("led_latency_ms") == std::string::npos, L"latency left as it is");
			rig.AssertCommandsInLimits();
			rig.AssertRestored();
		}

		TEST_METHOD(LedRunCancelledMidSweepRestoresAndChangesNothing)
		{
			LedRig rig("led-cancel", true);
			const std::string original = ReadFile(rig.config);
			rig.run->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
			for (int i = 0; i < 25; ++i) rig.run->Step();   // greys, then into the flux sweep
			Assert::IsTrue(rig.log->light < 255, L"the sweep had dimmed the LED");
			rig.run->Cancel();
			rig.run->Step();
			Assert::IsTrue(rig.run->Status().step == SessionStep::Cancelled, rig.Why().c_str());
			rig.AssertRestored();
			Assert::IsFalse(Exists(LedMarkerPath(rig.folder)));
			Assert::IsFalse(rig.screen->inner.PatternActive());
			Assert::AreEqual(original, ReadFile(rig.config));
			Assert::IsTrue(ReadFile(rig.run->Folder().indexJson).find("\"status\":\"cancelled\"") != std::string::npos);
		}

		TEST_METHOD(LedRunCancelledDuringTheCntrSweepPutsCntrBack)
		{
			LedRig rig("led-cancel-cntr", false, Format(10, 10, true), [](CalibrationSettings& s)
			{
				s.ledFluxStep = 50;
				s.ledReadsPerLevel = 1;
			});
			rig.run->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
			for (int i = 0; i < 400 && rig.log->contrast < 58; ++i) rig.run->Step();
			Assert::IsTrue(rig.log->contrast >= 58, L"the CNTR sweep was under way");
			rig.run->Cancel();
			rig.run->Step();
			Assert::IsTrue(rig.run->Status().step == SessionStep::Cancelled);
			rig.AssertRestored();
		}

		TEST_METHOD(LedRunMeterFailureRestoresTheProjector)
		{
			LedRig rig("led-meter", true);
			rig.meter->fatalAfter = 60;
			rig.run->Start();
			Assert::IsFalse(rig.RunUntil(SessionStep::Review));
			const SessionStatus s = rig.run->Status();
			Assert::IsTrue(s.step == SessionStep::Failed, rig.Why().c_str());
			Assert::IsTrue(s.message.find("unplugged") != std::string::npos);
			rig.AssertRestored();
			Assert::IsFalse(rig.screen->inner.PatternActive());
			Assert::IsFalse(Exists(LedMarkerPath(rig.folder)));
		}

		TEST_METHOD(LedRunProjectorCommandFailureRestores)
		{
			LedRig rig("led-command", false);
			rig.log->failLightAfter = 6;
			rig.run->Start();
			Assert::IsFalse(rig.RunUntil(SessionStep::Review));
			const SessionStatus s = rig.run->Status();
			Assert::IsTrue(s.step == SessionStep::Failed, rig.Why().c_str());
			Assert::IsTrue(s.message.find("did not take LPW1") != std::string::npos);
			Assert::IsTrue(rig.log->restores >= 2, L"restored at the start and after the failure");
			Assert::AreEqual(50, rig.log->contrast);
			Assert::AreEqual(255, rig.log->light);
		}

		TEST_METHOD(LedRunProjectorThatWillNotOpenSendsNothing)
		{
			LedRig rig("led-open", false);
			rig.log->failOpen = true;
			rig.run->Start();
			Assert::IsFalse(rig.RunUntil(SessionStep::Measuring));
			const SessionStatus s = rig.run->Status();
			Assert::IsTrue(s.step == SessionStep::Failed, rig.Why().c_str());
			Assert::IsTrue(s.message.find("could not be opened") != std::string::npos);
			Assert::IsTrue(rig.log->commands.empty());
			Assert::IsFalse(rig.screen->inner.PatternActive());
			Assert::IsFalse(Exists(LedMarkerPath(rig.folder)));
		}

		TEST_METHOD(LedRunDestroyedMidRunStillRestores)
		{
			std::shared_ptr<ProjectorLog> log;
			std::string folder;
			{
				LedRig rig("led-destroy", false);
				log = rig.log;
				folder = rig.folder;
				rig.run->Start();
				Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
				for (int i = 0; i < 20; ++i) rig.run->Step();
				Assert::IsTrue(log->open);
				rig.run.reset();   // VP closing without a cancel
				Assert::IsFalse(rig.screen->inner.PatternActive());
			}
			Assert::IsTrue(log->restores >= 2);
			Assert::IsTrue(log->commands.back().kind == 'R');
			Assert::IsFalse(log->open);
			Assert::IsFalse(Exists(LedMarkerPath(folder)));
		}

		TEST_METHOD(UnconfirmedRestoreKeepsTheMarkerAndVpRecoversAtStart)
		{
			LedRig rig("led-marker", false, Format(8, 8, true), [](CalibrationSettings& s) { s.ledFluxStep = 50; });
			rig.run->Start();
			Assert::IsTrue(rig.RunUntil(SessionStep::Measuring));
			rig.log->restoreOk = false;
			rig.run->Cancel();
			rig.run->Step();
			Assert::IsTrue(rig.run->Status().step == SessionStep::Cancelled);
			Assert::IsTrue(Exists(LedMarkerPath(rig.folder)), L"the marker stays when the restore was not confirmed");
			Assert::IsTrue(rig.log->restores >= 3, L"the restore was retried");
			// VP's next start: full light, LDMM 0 and CNTR 50 through the real driver.
			auto port = std::make_shared<PortLog>();
			LedDrive::ProjectorConfig projector;
			projector.port = "COM7";
			projector.minCommandGapMs = 0;
			const std::string line = RecoverInterruptedLedRun(rig.folder, projector, 255,
				[port]() { return std::unique_ptr<LedDrive::ISerialPort>(new FakeSerialPort(port)); });
			Logger::WriteMessage((line + "\nwritten: " + port->written + "\n").c_str());
			Assert::IsTrue(line.find("projector restored") != std::string::npos);
			const std::string bytes = Upper(port->written);
			Assert::IsTrue(bytes.find("LPW1255") != std::string::npos);
			Assert::IsTrue(bytes.find("LDMM0") != std::string::npos);
			Assert::IsTrue(bytes.find("CNTR50") != std::string::npos);
			Assert::IsFalse(Exists(LedMarkerPath(rig.folder)), L"the marker is removed after the recovery");
			// No marker: nothing is opened.
			auto quiet = std::make_shared<PortLog>();
			Assert::IsTrue(RecoverInterruptedLedRun(rig.folder, projector, 255,
				[quiet]() { return std::unique_ptr<LedDrive::ISerialPort>(new FakeSerialPort(quiet)); }).empty());
			Assert::IsTrue(quiet->written.empty());
		}

		TEST_METHOD(TimedSensorLinesAndDeviceClockAlignment)
		{
			bool device = false;
			double t = 0.0, v = 0.0;
			Assert::IsTrue(ParseTimedSensorLine("123.5", device, t, v));
			Assert::IsFalse(device);
			Assert::AreEqual(123.5, v, 1e-12);
			Assert::IsTrue(ParseTimedSensorLine("1500000 0.75", device, t, v));
			Assert::IsTrue(device);
			Assert::AreEqual(1.5, t, 1e-12);
			Assert::AreEqual(0.75, v, 1e-12);
			Assert::IsTrue(ParseTimedSensorLine("t=2000 v=12", device, t, v));
			Assert::AreEqual(0.002, t, 1e-12);
			Assert::IsFalse(ParseTimedSensorLine("VP light sensor ready", device, t, v));
			Assert::IsFalse(ParseTimedSensorLine("1 2 3", device, t, v));
			std::vector<TimedSample> samples = { { 1.000, 1 }, { 1.004, 1 }, { 1.008, 1 } };
			const std::vector<double> arrivals = { 50.0105, 50.0123, 50.0185 };   // USB jitter on arrival
			AlignDeviceTimes(samples, arrivals);
			Assert::AreEqual(50.0123, samples[1].seconds, 1e-9);   // shifted by the least-delayed arrival (offset 49.0083)
			Assert::AreEqual(0.004, samples[1].seconds - samples[0].seconds, 1e-12);
		}

		// ------------------------------------------------- ambient check

		TEST_METHOD(ScreenReflectMaths)
		{
			AmbientMeasurement dark, lit;
			dark.have = lit.have = true;
			dark.lux = 0.5;
			lit.lux = 120.5;
			dark.black = { 0.0, 0.010, 0.0 };
			lit.black = { 0.0, 1.210, 0.0 };
			dark.white = { 0.0, 48.0, 0.0 };
			lit.white = { 0.0, 49.2, 0.0 };
			AmbientResult r = ComputeScreenReflect(dark, lit);
			Assert::IsTrue(r.ok);
			Assert::AreEqual(0.01, r.reflect, 1e-9);
			Assert::AreEqual(0.01, r.reflectFromWhite, 1e-9);
			Assert::AreEqual(1.2, r.veilNits, 1e-9);
			Assert::AreEqual(std::string("0.01"), FormatReflectValue(r.reflect));
			const std::vector<ConfigEdit> edits = AmbientEdits(r);
			Assert::AreEqual(size_t(1), edits.size());
			Assert::AreEqual(std::string("vprenderer"), edits[0].section);
			Assert::AreEqual(std::string("ambient_screen_reflect"), edits[0].key);
			lit.lux = 3.0;
			r = ComputeScreenReflect(dark, lit);
			Assert::IsFalse(r.ok, L"too little lux difference");
			lit.lux = 120.5;
			lit.black[1] = 0.005;
			r = ComputeScreenReflect(dark, lit);
			Assert::IsTrue(r.ok);
			Assert::AreEqual(0.0, r.reflect, 1e-12);
			Assert::IsFalse(r.notes.empty());
		}

		TEST_METHOD(AmbientCheckRunSuggestsTheReflectance)
		{
			const std::string folder = TempFolder("ambient");
			const std::string config = folder + "\\VideoProcessor.cfg";
			WriteFile(config, kConfig);
			const std::string original = ReadFile(config);
			ManualClock clock;
			SimulatedScreen screen(Format(10, 8, true));
			SimulatedMeterParams mp;
			mp.proportionalNoise = 0.001;
			SimulatedScreen* s = &screen;
			auto flaky = std::unique_ptr<FlakyMeter>(new FlakyMeter(std::unique_ptr<SimulatedScreenMeter>(new SimulatedScreenMeter(
				[s](Vec3& d) { return s->Showing(d); }, SimulatedDisplayParams(), mp, &clock))));
			double room = 0.4;            // lux
			const double reflect = 0.012; // cd/m2 per lux, the screen
			flaky->veil = [&room, reflect]() { return XyToXyz(Gamuts::D65(), room * reflect); };
			bool haveSensor = true;
			CalibrationSettings settings;
			settings.warmupMinutes = 0;
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = config;
			env.displayName = "Screen";
			AmbientCheckRun run(settings, env, std::move(flaky), 0.002, screen, clock,
				[&room, &haveSensor](double& lux) { if (!haveSensor) return false; lux = room; return true; });
			run.Start();
			Assert::IsTrue(StepUntil(run, SessionStep::UserAction));
			Assert::IsTrue(run.Status().instruction.find("lights OFF") != std::string::npos);
			run.Continue();
			Assert::IsTrue(StepUntil(run, SessionStep::Measuring));
			Assert::IsTrue(StepUntil(run, SessionStep::UserAction));
			Assert::IsTrue(run.Status().instruction.find("lights ON") != std::string::npos);
			// No sensor now and no value entered: the run asks and waits.
			haveSensor = false;
			room = 150.0;
			run.Continue();
			run.Step();
			Assert::IsTrue(run.Status().step == SessionStep::UserAction);
			Assert::IsTrue(run.Status().message.find("enter the room's lux") != std::string::npos);
			run.SetArguments("lux: 150.4");
			run.Continue();
			Assert::IsTrue(StepUntil(run, SessionStep::Review), W(run.Status().message).c_str());
			const AmbientResult& r = run.Result();
			Logger::WriteMessage((run.Status().review + "\n" + run.Status().diff + "\n").c_str());
			Assert::IsTrue(r.ok, W(r.error).c_str());
			Assert::AreEqual(reflect, r.reflect, 0.05 * reflect);
			Assert::IsTrue(run.Dark().luxFromSensor);
			Assert::IsFalse(run.Lit().luxFromSensor);
			// This build has no intent ambient adaptation: Apply is refused, the value is kept.
			Assert::IsTrue(run.Status().diff.find("Apply is not possible") == 0, W(run.Status().diff).c_str());
			Assert::IsTrue(run.Status().diff.find("feat/intent-remainder") != std::string::npos);
			Assert::IsTrue(ReadFile(run.Folder().runDir + "\\ambient.cfg").find("ambient_screen_reflect: ") != std::string::npos);
			run.Apply();
			run.Step();
			Assert::IsTrue(run.Status().step == SessionStep::Review);
			Assert::IsTrue(run.Status().message.find("Apply failed") != std::string::npos);
			Assert::AreEqual(original, ReadFile(config));
			run.Discard();
			run.Step();
			Assert::IsTrue(run.Status().step == SessionStep::Discarded);
			Assert::AreEqual(original, ReadFile(config));
		}

		TEST_METHOD(AmbientCheckAppliesWhereTheKeyIsKnown)
		{
			// A build with the intent ambient adaptation (modelled by the validator).
			const std::string folder = TempFolder("ambient-apply");
			const std::string config = folder + "\\VideoProcessor.cfg";
			WriteFile(config, kConfig);
			const std::string original = ReadFile(config);
			ManualClock clock;
			SimulatedScreen screen(Format(8, 8, true));
			SimulatedScreen* s = &screen;
			auto flaky = std::unique_ptr<FlakyMeter>(new FlakyMeter(std::unique_ptr<SimulatedScreenMeter>(new SimulatedScreenMeter(
				[s](Vec3& d) { return s->Showing(d); }, SimulatedDisplayParams(), SimulatedMeterParams(), &clock))));
			double room = 0.0;
			flaky->veil = [&room]() { return XyToXyz(Gamuts::D65(), room * 0.02); };
			CalibrationSettings settings;
			settings.warmupMinutes = 0;
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = config;
			AmbientCheckRun run(settings, env, std::move(flaky), 0.002, screen, clock, [](double&) { return false; });
			run.SetValidatorForTests([](const ConfigEditorCore::ConfigDocument&, std::string&) { return true; });
			run.Start();
			Assert::IsTrue(StepUntil(run, SessionStep::UserAction));
			run.Continue();   // dark: 0 lux without a sensor
			Assert::IsTrue(StepUntil(run, SessionStep::Measuring));
			Assert::IsTrue(StepUntil(run, SessionStep::UserAction));
			room = 80.0;
			run.SetArguments("lux: 80");
			run.Continue();
			Assert::IsTrue(StepUntil(run, SessionStep::Review));
			Assert::IsTrue(run.Status().diff.find("[vprenderer] ambient_screen_reflect: (absent) -> 0.02") == 0,
				W(run.Status().diff).c_str());
			// SaveSafely validates again with VP's own schema: on this build it still
			// refuses the unknown key, so nothing is written (the last guard).
			run.Apply();
			run.Step();
			Assert::IsTrue(run.Status().step == SessionStep::Review);
			Assert::IsTrue(run.Status().message.find("Apply failed") != std::string::npos);
			Assert::AreEqual(original, ReadFile(config));
		}

		TEST_METHOD(SuggestedEditsApplyKeyOnlyAfterBackups)
		{
			// The phase 2 apply path (LED and ambient results) with keys this build knows.
			const std::string folder = TempFolder("apply-keys");
			const std::string config = folder + "\\VideoProcessor.cfg";
			WriteFile(config, kConfig);
			const std::string original = ReadFile(config);
			const std::vector<ConfigEdit> edits = {
				{ "projector.default", "led_latency_ms", "91" },
				{ "projector.default", "dmd_clip_cntr", "61" },
				{ "led_drive.default", "compensation_eotf", "2.30" } };
			std::string error;
			const ConfigDiff preview = PreviewKeyEdits(config, edits, DefaultCandidateValidator(), error);
			Assert::IsTrue(preview.ok, W(error).c_str());
			Assert::IsTrue(preview.Format().find("[projector.default] dmd_clip_cntr: 59 -> 61") != std::string::npos);
			Assert::AreEqual(original, ReadFile(config), L"a preview writes nothing");
			RunTimestamp stamp;
			stamp.year = 2026; stamp.month = 10; stamp.day = 18; stamp.hour = 12;
			const RunFolder f = BuildPhase2RunFolder(folder, "FL35", stamp, "led");
			Assert::IsTrue(f.runDir.find("20261018-120000-led") != std::string::npos);
			const ApplyOutcome o = ApplyKeyEdits(config, edits, f, stamp, DefaultCandidateValidator());
			Assert::IsTrue(o.ok, W(o.error).c_str());
			Assert::AreEqual(original, ReadFile(o.backupPath));
			Assert::AreEqual(original, ReadFile(o.runCopyPath));
			const std::string edited = ReadFile(config);
			// Without the two added lines, the file is the original with dmd_clip_cntr changed in place.
			std::string stripped;
			{
				std::istringstream b(edited);
				for (std::string line; std::getline(b, line);)
					if (line.find("led_latency_ms: 91") != 0 && line.find("compensation_eotf: 2.30") != 0) stripped += line + "\n";
			}
			std::string expected = original;
			expected.replace(expected.find("dmd_clip_cntr: 59"), 17, "dmd_clip_cntr: 61");
			Assert::AreEqual(expected, stripped);
			Assert::IsTrue(edited.find("led_latency_ms: 91") != std::string::npos);
			Assert::IsTrue(edited.find("compensation_eotf: 2.30") != std::string::npos);
			Assert::IsTrue(edited.find("protocol: barco_ascii   # FL35") != std::string::npos);
			// A value VP would reject is refused before any backup or write.
			const ApplyOutcome bad = ApplyKeyEdits(config, { { "projector.default", "led_latency_ms", "99999" } }, f, stamp,
				DefaultCandidateValidator());
			Assert::IsFalse(bad.ok);
			Assert::IsTrue(bad.backupPath.empty());
			Assert::AreEqual(edited, ReadFile(config));
		}

		// --------------------------------------------------- PQ maths

		TEST_METHOD(IctcpAndDeltaEItp)
		{
			const Vec3 white = XyToXyz(Gamuts::D65(), 100.0);
			const Vec3 i = XyzToIctcp(white);
			Assert::AreEqual(0.0, i[1], 1e-4);   // neutral: no Ct / Cp
			Assert::AreEqual(0.0, i[2], 1e-4);
			Assert::AreEqual(PqEncode(100.0), i[0], 2e-4);
			Assert::AreEqual(0.0, DeltaEItp(white, white), 1e-12);
			// 1% luminance at 100 cd/m2 is about one JND in ITP.
			const double one = DeltaEItp(white, Scale(white, 1.01));
			Assert::IsTrue(one > 0.5 && one < 3.0, std::to_wstring(one).c_str());
			// A colour shift is larger than the same-size luminance shift.
			const Vec3 tinted = XyToXyz({ 0.3227, 0.3290 }, 100.0);
			Assert::IsTrue(DeltaEItp(white, tinted) > one);
			// Symmetric and growing with the error.
			Assert::AreEqual(DeltaEItp(white, Scale(white, 1.05)), DeltaEItp(Scale(white, 1.05), white), 1e-12);
			Assert::IsTrue(DeltaEItp(white, Scale(white, 1.05)) > one);
		}

		TEST_METHOD(Bt2390EetfBehaves)
		{
			// Target at or above the source: identity.
			for (const double y : { 0.01, 1.0, 100.0, 999.0 })
				Assert::AreEqual(y, Bt2390Eetf(y, 0.0, 1000.0, 0.0, 1000.0), 1e-6 * (y + 1));
			// Below the knee: identity; the source peak lands on the target peak.
			Assert::AreEqual(10.0, Bt2390Eetf(10.0, 0.0, 1000.0, 0.0, 600.0), 1e-6);
			Assert::AreEqual(600.0, Bt2390Eetf(1000.0, 0.0, 1000.0, 0.0, 600.0), 0.5);
			double previous = 0.0;
			for (double y = 0.01; y <= 1000.0; y *= 1.3)
			{
				const double m = Bt2390Eetf(y, 0.0, 1000.0, 0.0, 600.0);
				Assert::IsTrue(m >= previous && m <= 600.5 && m <= y + 1e-6);
				previous = m;
			}
			// The sampled curve interpolates the same function.
			PatternOutputInfo f = Format(10, 10, true, SignalTransfer::Pq);
			FillToneCurve(f, 1000.0, 600.0);
			CalibrationSettings s;
			const ToneCurve vp = ChooseToneCurve(f, s, 0.0);
			Assert::IsTrue(vp.kind == ToneCurve::Kind::Samples);
			for (const double y : { 0.05, 3.0, 120.0, 450.0, 800.0 })
				Assert::AreEqual(Bt2390Eetf(y, 0.0, 1000.0, 0.0, 600.0), vp.Map(y), 0.01 * Bt2390Eetf(y, 0.0, 1000.0, 0.0, 600.0));
			s.hdrVerifyToneCurve = "none";
			Assert::IsTrue(ChooseToneCurve(f, s, 0.0).kind == ToneCurve::Kind::Identity);
			s.hdrVerifyToneCurve = "vp";
			f.toneCurveCount = 0;
			f.sourcePeakNits = 0.0f;
			f.targetPeakNits = 0.0f;
			s.hdrVerifySourcePeak = 4000.0;
			const ToneCurve model = ChooseToneCurve(f, s, 700.0);
			Assert::IsTrue(model.kind == ToneCurve::Kind::Bt2390);
			Assert::AreEqual(4000.0, model.sourcePeak, 1e-9);
			Assert::AreEqual(700.0, model.targetPeak, 1e-9);   // the measured display peak
			Assert::IsTrue(model.description.find("not reported") != std::string::npos);
			FillToneCurve(f, 1000.0, 1000.0);
			Assert::IsTrue(ChooseToneCurve(f, s, 0.0).kind == ToneCurve::Kind::Identity, L"VP passes the source through");
		}

		TEST_METHOD(HdrTargetsUseTheExactPqCodes)
		{
			const PatternOutputInfo f = Format(10, 10, true, SignalTransfer::Pq);
			ToneCurve identity;
			HdrPatch grey;
			grey.sourceRgbNits = { 100.0, 100.0, 100.0 };
			PatchCodes codes;
			Vec3 target;
			HdrTarget(grey, identity, f, codes, target);
			// PQ(100) = 0.5081 of black..white: limited 10-bit code 64 + 876 x 0.5081.
			Assert::AreEqual(static_cast<int>(std::lround(64 + 876 * PqEncode(100.0))), codes.code[0]);
			Assert::AreEqual(PqDecode(codes.device[0]), target[1], 1e-6 * target[1]);
			Assert::AreEqual(100.0, target[1], 1.0);   // within a code's step
			const std::vector<HdrPatch> plan = BuildHdrPlan(1000.0);
			Assert::IsTrue(plan.front().peakProbe);
			int greys = 0, colours = 0;
			for (const HdrPatch& p : plan)
			{
				if (p.peakProbe) continue;
				if (p.label.rfind("Grey", 0) == 0) ++greys; else ++colours;
				Assert::IsTrue((std::max)((std::max)(p.sourceRgbNits[0], p.sourceRgbNits[1]), p.sourceRgbNits[2]) <= 1000.0);
			}
			Assert::IsTrue(greys >= 15);
			Assert::AreEqual(12, colours);
		}

		TEST_METHOD(HdrVerifyRunReportsTrackingAndDeltaEItp)
		{
			const std::string folder = TempFolder("hdr");
			const std::string config = folder + "\\VideoProcessor.cfg";
			WriteFile(config, kConfig);
			const std::string original = ReadFile(config);
			ManualClock clock;
			PatternOutputInfo f = Format(10, 10, true, SignalTransfer::Pq);
			FillToneCurve(f, 1000.0, 600.0);   // VP maps 1000 cd/m2 content to a 600 cd/m2 display
			SimulatedScreen screen(f);
			CalibrationSettings settings;
			settings.warmupMinutes = 0;
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = config;
			env.displayName = "OLED";
			HdrVerifyRun run(settings, env, std::unique_ptr<IMeter>(new PqDisplayMeter(&screen, 650.0, { 1.004, 0.997, 1.002 },
				&clock)), 0.002, screen, clock);
			run.Start();
			Assert::IsTrue(StepUntil(run, SessionStep::Done), W(run.Status().message).c_str());
			const HdrReport& r = run.Report();
			Logger::WriteMessage((run.Status().review + "\n").c_str());
			Assert::AreEqual(650.0, r.measuredPeak, 1.0);
			Assert::IsTrue(run.Curve().kind == ToneCurve::Kind::Samples);
			Assert::IsTrue(r.counted > 25);
			Assert::IsTrue(r.meanItp < 1.0, std::to_wstring(r.meanItp).c_str());
			Assert::IsTrue(r.pass);
			bool sawTop = false;
			for (const HdrEntry& e : r.entries)
			{
				Assert::IsFalse(e.aboveDisplayPeak, W(e.label).c_str());   // VP mapped everything below 650
				if (e.label == "Grey 1000 cd/m2") { sawTop = true; Assert::AreEqual(600.0, e.targetXyz[1], 3.0); }
				if (e.grey && !e.informative && e.targetXyz[1] > 1.0) Assert::AreEqual(0.0, e.trackingPercent, 1.0);
			}
			Assert::IsTrue(sawTop, L"the 1000 cd/m2 grey targets VP's 600 cd/m2");
			Assert::IsTrue(ReadFile(run.Folder().runDir + "\\hdr_verify.json").find("\"de_itp\":") != std::string::npos);
			Assert::IsTrue(ReadFile(run.Folder().reportHtml).find("HDR (PQ) verify") != std::string::npos);
			Assert::IsTrue(run.Status().review.find("Metadata VP sends: mastering max 1000") != std::string::npos);
			Assert::IsTrue(run.Status().review.find("Target: VP's tone mapping (libplacebo bt2390, source peak 1000") != std::string::npos);
			Assert::IsFalse(screen.PatternActive());
			Assert::AreEqual(original, ReadFile(config), L"verify only");
		}

		TEST_METHOD(HdrVerifyMarksPatchesAboveTheDisplayPeak)
		{
			const std::string folder = TempFolder("hdr-clip");
			ManualClock clock;
			PatternOutputInfo f = Format(10, 10, true, SignalTransfer::Pq);
			FillToneCurve(f, 1000.0, 1000.0);   // VP passes 1000 cd/m2 through ...
			SimulatedScreen screen(f);
			CalibrationSettings settings;
			settings.warmupMinutes = 0;
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = folder + "\\VideoProcessor.cfg";
			WriteFile(env.configPath, kConfig);
			HdrVerifyRun run(settings, env, std::unique_ptr<IMeter>(new PqDisplayMeter(&screen, 400.0, { 1, 1, 1 }, &clock)),
				0.002, screen, clock);   // ... to a 400 cd/m2 display
			run.Start();
			Assert::IsTrue(StepUntil(run, SessionStep::Done));
			int above = 0;
			for (const HdrEntry& e : run.Report().entries) if (e.aboveDisplayPeak) ++above;
			Assert::IsTrue(above >= 3);   // the 600, 800 and 1000 cd/m2 greys
			Assert::IsTrue(run.Report().pass, L"patches the display cannot reach are not counted");
		}

		TEST_METHOD(HdrVerifyRefusesAnSdrOutput)
		{
			const std::string folder = TempFolder("hdr-sdr");
			ManualClock clock;
			SimulatedScreen screen(Format(10, 10, true));
			CalibrationSettings settings;
			SessionEnvironment env;
			env.vpRoot = folder;
			env.configPath = folder + "\\VideoProcessor.cfg";
			HdrVerifyRun run(settings, env, std::unique_ptr<IMeter>(new PqDisplayMeter(&screen, 400.0, { 1, 1, 1 }, &clock)),
				0.002, screen, clock);
			run.Start();
			Assert::IsFalse(StepUntil(run, SessionStep::Measuring));
			Assert::IsTrue(run.Status().step == SessionStep::Failed);
			Assert::IsTrue(run.Status().message.find("needs VP's output in HDR (PQ)") != std::string::npos);
			Assert::IsFalse(screen.PatternActive());
		}

		// ------------------------------------------------- transcripts

		TEST_METHOD(TranscriptScriptFromEvents)
		{
			std::vector<TranscriptEvent> e;
			auto out = [&e](double t, const std::string& s) { TranscriptEvent x; x.seconds = t; x.text = s; e.push_back(x); };
			auto key = [&e](double t, const std::string& s)
			{
				TranscriptEvent x;
				x.kind = TranscriptEvent::Kind::Key;
				x.seconds = t;
				x.text = s;
				e.push_back(x);
			};
			out(0.0, "\r\nPlace instrument on spot to be measured,\r\nHit ESC or Q to exit, any");
			out(0.1, " other key to take a reading: ");
			key(1.0, " ");
			out(1.85, "\r\n Result is XYZ: 95.047000 100.000000 108.883000, \x1b[0KYxy: 100 0.3127 0.329\r\n\r\n");
			out(1.9, "Hit ESC or Q to exit: ");
			key(2.0, "q");
			TranscriptEvent x;
			x.kind = TranscriptEvent::Kind::Exit;
			x.exitCode = 0;
			e.push_back(x);
			const std::string script = FormatTranscriptScript(e, { "meter: i1Display Pro" });
			Logger::WriteMessage(script.c_str());
			const std::string want =
				"# meter: i1Display Pro\n"
				"OUT\n"
				"OUT Place instrument on spot to be measured,\n"
				"PROMPT Hit ESC or Q to exit, any other key to take a reading: \n"
				"KEY\n"
				"SLEEP 850\n"
				"OUT\n"
				"OUT  Result is XYZ: 95.047000 100.000000 108.883000, \\e[0KYxy: 100 0.3127 0.329\n"
				"OUT\n"
				"PROMPT Hit ESC or Q to exit: \n"
				"KEY\n"
				"EXIT 0\n";
			Assert::AreEqual(want, script);
			Assert::AreEqual(std::string("recorded_i1display_pro_20261018-101500.txt"),
				TranscriptFileName("i1Display Pro", "20261018-101500"));
		}

		TEST_METHOD(RecordedTranscriptReplaysToTheSameReadings)
		{
			double now = 100.0;
			std::string script;
			std::vector<Vec3> first;
			{
				SpotreadMeterOptions o;
				o.spotread = L"C:\\Argyll\\bin\\spotread.exe";
				o.arguments.displayType = "l";
				o.readTimeoutSeconds = 5.0;
				o.openTimeoutSeconds = 5.0;
				o.closeTimeoutSeconds = 1.0;
				RecordingTerminal* recorder = nullptr;
				SpotreadMeter meter(o, [&now, &recorder]()
				{
					auto r = std::unique_ptr<RecordingTerminal>(new RecordingTerminal(
						std::unique_ptr<ITerminal>(new ScriptedTerminal("i1d3_session.txt", &now)), std::string(),
						{ "meter: test" }, [&now]() { return now; }));
					recorder = r.get();
					return std::unique_ptr<ITerminal>(std::move(r));
				}, [&now]() { return now; });
				Assert::IsTrue(meter.Open().status == MeterStatus::Ok);
				for (int i = 0; i < 3; ++i)
				{
					const MeterResult r = meter.Read();
					Assert::IsTrue(r.status == MeterStatus::Ok);
					first.push_back(r.xyz);
				}
				script = recorder->Script();
				meter.Close();
			}
			Logger::WriteMessage(script.c_str());
			Assert::IsTrue(script.find("# spotread arguments: -e -x -y l") != std::string::npos);
			Assert::IsTrue(script.find("KEY") != std::string::npos);
			std::vector<std::string> lines;
			std::istringstream in(script);
			for (std::string line; std::getline(in, line);) lines.push_back(line);
			// Replayed by the tests' scripted terminal, the recording gives the same readings.
			SpotreadMeterOptions o;
			o.spotread = L"C:\\Argyll\\bin\\spotread.exe";
			o.readTimeoutSeconds = 5.0;
			o.openTimeoutSeconds = 5.0;
			o.closeTimeoutSeconds = 1.0;
			SpotreadMeter replay(o, [&lines, &now]() { return std::unique_ptr<ITerminal>(new ScriptedTerminal(lines, &now)); },
				[&now]() { return now; });
			Assert::IsTrue(replay.Open().status == MeterStatus::Ok);
			for (int i = 0; i < 3; ++i)
			{
				const MeterResult r = replay.Read();
				Assert::IsTrue(r.status == MeterStatus::Ok);
				for (int k = 0; k < 3; ++k) Assert::AreEqual(first[i][k], r.xyz[k], 1e-6);
			}
			replay.Close();
			// A file is written when the terminal goes (the recording mode).
			const std::string folder = TempFolder("transcript");
			const std::string path = folder + "\\" + TranscriptFileName("i1d3", "20261018-120000");
			{
				RecordingTerminal t(std::unique_ptr<ITerminal>(new ScriptedTerminal("i1d3_session.txt", &now)), path, { "x" });
				std::string error, output;
				t.Start(L"spotread.exe", L"spotread.exe -e -x", L"", error);
				t.Read(output, 10);
			}
			Assert::IsTrue(ReadFile(path).find("OUT Place instrument on spot to be measured,") != std::string::npos);
		}

		// -------------------------------------------------- controller

		TEST_METHOD(ControllerStartsTheRunKinds)
		{
			const std::string folder = TempFolder("controller-kinds");
			const std::string path = folder + "\\VideoProcessor.cfg";
			WriteFile(path, kConfig);
			const std::string original = ReadFile(path);
			auto log = std::make_shared<ProjectorLog>();
			Calibration::CalibrationController::Hooks hooks;
			hooks.configPath = [path]() { return path; };
			hooks.renderer = []() { return std::shared_ptr<IVideoRenderer>(); };
			ManualClock clock;
			hooks.clock = &clock;
			hooks.projectorFactory = [log, &clock](const LedDrive::ProjectorConfig& pc, std::string&)
			{
				Assert::AreEqual(std::string("COM7"), pc.port);
				return std::unique_ptr<IProjectorControl>(new FakeProjector(log, nullptr, &clock));
			};
			Calibration::CalibrationController controller(hooks);
			Assert::IsTrue(controller.Handle("start", "kind: bogus").find("error: Unknown calibration run") == 0);
			// No renderer: the LED run stops before the projector is ever opened.
			controller.Handle("start", "kind: led");
			SessionStatus s;
			for (int i = 0; i < 300; ++i)
			{
				Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
				if (IsFinal(s.step)) break;
				Sleep(10);
			}
			Assert::AreEqual(std::string("led"), s.kind);
			Assert::IsTrue(s.step == SessionStep::Failed, W(s.message).c_str());
			Assert::IsTrue(s.message.find("cannot show calibration patches") != std::string::npos);
			Assert::AreEqual(0, log->opens, L"no patterns: the projector is never touched");
			controller.Handle("start", "kind: ambient");
			for (int i = 0; i < 300; ++i)
			{
				Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
				if (IsFinal(s.step)) break;
				Sleep(10);
			}
			Assert::AreEqual(std::string("ambient"), s.kind);
			controller.Handle("start", "kind: hdr_verify");
			for (int i = 0; i < 300; ++i)
			{
				Assert::IsTrue(ParseStatus(controller.Handle("status", ""), s));
				if (IsFinal(s.step)) break;
				Sleep(10);
			}
			Assert::AreEqual(std::string("hdr_verify"), s.kind);
			Assert::AreEqual(original, ReadFile(path));
			controller.Shutdown();

			// Without the projector's RS-232 protocol the LED run is refused at once.
			WriteFile(path, std::string(kConfig).replace(std::string(kConfig).find("barco_ascii"), 11, "none"));
			Calibration::CalibrationController::Hooks plain;
			plain.configPath = [path]() { return path; };
			plain.renderer = []() { return std::shared_ptr<IVideoRenderer>(); };
			plain.clock = &clock;
			Calibration::CalibrationController refusing(plain);
			Assert::IsTrue(refusing.Handle("start", "kind: led").find("error: The Dynamic Drive characterisation needs the "
				"projector's RS-232 connection") == 0);
			// A fast sensor on the projector's own port is refused.
			WriteFile(path, std::string(kConfig) + "led_sensor_port: COM7\n");
			Assert::IsTrue(refusing.Handle("start", "kind: led").find("error:") == 0);
			refusing.Shutdown();
		}

		TEST_METHOD(StatusCarriesTheRunKindAndNewSteps)
		{
			SessionStatus s;
			s.kind = "led";
			s.step = SessionStep::UserAction;
			SessionStatus back;
			Assert::IsTrue(ParseStatus(FormatStatus(s), back));
			Assert::AreEqual(std::string("led"), back.kind);
			Assert::IsTrue(back.step == SessionStep::UserAction);
			s.step = SessionStep::Done;
			Assert::IsTrue(ParseStatus(FormatStatus(s), back));
			Assert::IsTrue(back.step == SessionStep::Done);
			Assert::IsTrue(IsFinal(SessionStep::Done));
			Assert::IsFalse(IsFinal(SessionStep::UserAction));
			std::string v;
			Assert::IsTrue(ArgumentValue("kind: led\nlux: 12.5\n", "lux", v));
			Assert::AreEqual(std::string("12.5"), v);
			Assert::IsFalse(ArgumentValue("kind: led", "lux", v));
		}
	};

	// Gated: the Dynamic Drive restore on the real projector port
	// (VP_TEST_PROJECTOR=COMn, the FL35 on and idle; VP not running). Sets LPW1
	// 200 and CNTR 52 for two seconds, then restores full light, LDMM 0 and
	// CNTR 50 and checks the restore was confirmed (read-back on).
	TEST_CLASS(CalibrationLedHardwareTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(ProjectorRestoreOnTheRealPort)
			TEST_METHOD_ATTRIBUTE(L"Category", L"ProjectorHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ProjectorRestoreOnTheRealPort)
		{
			char port[32] = {};
			if (GetEnvironmentVariableA("VP_TEST_PROJECTOR", port, sizeof(port)) == 0)
			{
				Logger::WriteMessage("SKIP-NO-PROJECTOR ProjectorRestoreOnTheRealPort (set VP_TEST_PROJECTOR=COMn)\n");
				return;
			}
			LedDrive::ProjectorConfig projector;
			projector.port = port;
			projector.readBack = true;
			DriverProjectorControl control(projector, [] { return LedDrive::CreateWin32SerialPort(); });
			std::string error, report;
			Assert::IsTrue(control.Open(error), W(error).c_str());
			double at = 0.0;
			Assert::IsTrue(control.SetLight(200, at));
			Assert::IsTrue(control.SetContrast(52, at));
			Sleep(2000);
			const bool ok = control.Restore(255, report);
			Logger::WriteMessage(("restore: " + report + "\n").c_str());
			control.Close();
			Assert::IsTrue(ok, W(report).c_str());
		}
	};

	// The recording mode under the real pseudo-console: VP's meter drives the
	// fake spotread through a RecordingTerminal; the saved file, played by the
	// fake spotread again, gives the same readings and keys (CPU, no meter).
	TEST_CLASS(CalibrationTranscriptConPtyTests)
	{
	public:
		TEST_METHOD(FakeSpotreadSessionRecordsAndReplays)
		{
			Assert::IsTrue(ConPtyAvailable(), L"this Windows has no pseudo-console");
			const std::string folder = TempFolder("record conpty");
			std::string error;
			CreateFolders(folder + "\\bin", error);
			const std::string fake = FakeSpotreadPath();
			Assert::IsTrue(Exists(fake), L"x64\\Release\\test-tools\\fake_spotread.exe is missing: build VideoProcessor-FakeSpotread");
			Assert::IsTrue(CopyFileA(fake.c_str(), (folder + "\\bin\\spotread.exe").c_str(), FALSE) != FALSE);
			const std::string recorded = folder + "\\" + TranscriptFileName("fake i1d3", "20261018-130000");
			auto session = [&folder](const std::string& script, const std::string& recordTo, std::vector<Vec3>& readings,
				std::string& keys)
			{
				const std::string log = folder + "\\fake.log";
				DeleteFileA(log.c_str());
				SetEnvironmentVariableA("FAKE_SPOTREAD_SCRIPT", script.c_str());
				SetEnvironmentVariableA("FAKE_SPOTREAD_LOG", log.c_str());
				SpotreadMeterOptions o;
				const std::string exe = folder + "\\bin\\spotread.exe";
				o.spotread = std::wstring(exe.begin(), exe.end());
				o.readTimeoutSeconds = 10.0;
				o.openTimeoutSeconds = 15.0;
				o.closeTimeoutSeconds = 2.0;
				{
					SpotreadMeter meter(o, [recordTo]() -> std::unique_ptr<ITerminal>
					{
						if (recordTo.empty()) return CreateConPtyTerminal();
						return std::unique_ptr<ITerminal>(new RecordingTerminal(CreateConPtyTerminal(), recordTo,
							{ "meter: fake i1d3 (test)" }));
					});
					Assert::IsTrue(meter.Open().status == MeterStatus::Ok);
					for (int i = 0; i < 3; ++i)
					{
						const MeterResult r = meter.Read();
						Assert::IsTrue(r.status == MeterStatus::Ok, W(r.message).c_str());
						readings.push_back(r.xyz);
					}
					meter.Close();
				}
				keys = ReadFile(log);
				SetEnvironmentVariableA("FAKE_SPOTREAD_SCRIPT", nullptr);
				SetEnvironmentVariableA("FAKE_SPOTREAD_LOG", nullptr);
			};
			std::vector<Vec3> first, second;
			std::string keysFirst, keysSecond;
			session(TranscriptPath("i1d3_session.txt"), recorded, first, keysFirst);
			const std::string script = ReadFile(recorded);
			Logger::WriteMessage(("recorded:\n" + script).c_str());
			Assert::IsTrue(script.find("# meter: fake i1d3 (test)") != std::string::npos);
			Assert::IsTrue(script.find("Result is XYZ:") != std::string::npos);
			Assert::IsTrue(script.find("\nKEY\n") != std::string::npos);
			session(recorded, std::string(), second, keysSecond);
			for (int i = 0; i < 3; ++i)
				for (int k = 0; k < 3; ++k) Assert::AreEqual(first[i][k], second[i][k], 1e-6);
			auto keyLines = [](const std::string& log)
			{
				std::string out;
				std::istringstream in(log);
				for (std::string line; std::getline(in, line);) if (line.rfind("key:", 0) == 0) out += line + "\n";
				return out;
			};
			Assert::AreEqual(keyLines(keysFirst), keyLines(keysSecond), L"the replay reads the same keys");
		}
	};

	// Gated: records a real meter session (VP_TEST_METER=1; optional
	// VP_TEST_ARGYLL, VP_TEST_METER_PORT) in the test-data format to
	// %TEMP%\vp-recorded-transcript.txt, then replays it through the parser.
	TEST_CLASS(CalibrationTranscriptHardwareTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(RecordRealSpotreadTranscript)
			TEST_METHOD_ATTRIBUTE(L"Category", L"MeterHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RecordRealSpotreadTranscript)
		{
			char flag[8] = {};
			if (GetEnvironmentVariableA("VP_TEST_METER", flag, sizeof(flag)) == 0 || std::string(flag) != "1")
			{
				Logger::WriteMessage("SKIP-NO-METER RecordRealSpotreadTranscript (set VP_TEST_METER=1)\n");
				return;
			}
			char argyll[MAX_PATH] = {}, portText[16] = {};
			GetEnvironmentVariableA("VP_TEST_ARGYLL", argyll, sizeof(argyll));
			GetEnvironmentVariableA("VP_TEST_METER_PORT", portText, sizeof(portText));
			const std::string argyllPath = argyll;
			const ArgyllProbe probe = ProbeArgyll(std::wstring(argyllPath.begin(), argyllPath.end()));
			Assert::IsTrue(probe.found && probe.ran && !probe.usage.instruments.empty(), W(probe.Summary()).c_str());
			char base[MAX_PATH];
			GetTempPathA(MAX_PATH, base);
			const std::string path = std::string(base) + "vp-recorded-transcript.txt";
			DeleteFileA(path.c_str());
			SpotreadMeterOptions o;
			o.spotread = probe.spotread;
			o.arguments.port = std::atoi(portText);
			o.readTimeoutSeconds = 60.0;
			o.openTimeoutSeconds = 90.0;
			const std::string name = probe.usage.instruments[0].name;
			{
				SpotreadMeter meter(o, [path, name, &probe]()
				{
					return std::unique_ptr<ITerminal>(new RecordingTerminal(CreateConPtyTerminal(), path,
						{ "meter: " + name, "ArgyllCMS spotread version: " + probe.usage.version }));
				});
				MeterResult r = meter.Open();
				Assert::IsTrue(r.status == MeterStatus::Ok || r.status == MeterStatus::NeedsUserAction, W(r.message).c_str());
				if (r.status == MeterStatus::Ok)
					for (int i = 0; i < 3; ++i) meter.Read();
				meter.Close();
			}
			const std::string script = ReadFile(path);
			Logger::WriteMessage(("recorded to " + path + ":\n" + script).c_str());
			Assert::IsTrue(script.find("OUT") != std::string::npos);
			Assert::IsTrue(script.find("KEY") != std::string::npos);
		}
	};
}
