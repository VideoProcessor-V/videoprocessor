#include <pch.h>

#include "CalibrationController.h"
#include "AmbientCheck.h"
#include "CalibrationSimulation.h"
#include "HdrVerify.h"
#include "TranscriptRecorder.h"

#include <AmbientConfig.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>

#include <ConfigFile.h>
#include <IRenderer.h>

#include <sstream>

namespace Calibration
{
	namespace
	{
		std::string Escape(const std::string& text)
		{
			std::string out;
			for (const char c : text)
			{
				if (c == '\\') out += "\\\\";
				else if (c == '\n') out += "\\n";
				else if (c != '\r') out.push_back(c);
			}
			return out;
		}

		std::wstring Wide(const std::string& text)
		{
			if (text.empty()) return std::wstring();
			int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
			const UINT page = n > 0 ? CP_UTF8 : CP_ACP;
			n = MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>((std::max)(n, 0)), L'\0');
			if (n > 0) MultiByteToWideChar(page, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
			return out;
		}

		std::string FolderOf(const std::string& path)
		{
			const size_t slash = path.find_last_of("\\/");
			return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
		}

		bool MeasuringStep(SessionStep step)
		{
			return step == SessionStep::OpeningMeter || step == SessionStep::MeterCalibration ||
				step == SessionStep::Placement || step == SessionStep::WarmUp || step == SessionStep::Measuring ||
				step == SessionStep::Verifying || step == SessionStep::UserAction;
		}
	}

	// ------------------------------------------------------------- sink

	RendererPatternSink::RendererPatternSink(RendererFn r) : renderer(std::move(r)) {}

	bool RendererPatternSink::GetOutput(PatternOutputInfo& info)
	{
		std::lock_guard<std::mutex> guard(mutex);
		info = PatternOutputInfo();
		if (blocked)
		{
			strncpy_s(info.reason, sizeof(info.reason), "the VP Renderer restarted", _TRUNCATE);
			return false;
		}
		const std::shared_ptr<IVideoRenderer> current = renderer ? renderer() : nullptr;
		if (!current)
		{
			strncpy_s(info.reason, sizeof(info.reason), "no renderer is running (start VP with any source)", _TRUNCATE);
			return false;
		}
		const bool ok = current->GetCalibrationOutput(info);
		format = info;
		return ok;
	}

	bool RendererPatternSink::Show(const PatternRequest& request, std::string& error)
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (blocked) { error = "the VP Renderer restarted"; return !request.active; }
		const std::shared_ptr<IVideoRenderer> current = renderer ? renderer() : nullptr;
		if (!current) { error = "no renderer is running"; return !request.active; }
		CString reason;
		const bool ok = current->SetCalibrationPattern(request, reason);
		if (!ok) error = std::string(CStringA(reason).GetString());
		else last = request;
		return ok;
	}

	void RendererPatternSink::Block()
	{
		std::lock_guard<std::mutex> guard(mutex);
		blocked = true;
	}

	bool RendererPatternSink::Showing(Vec3& device)
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (!last.active || !(format.codeBits == 8 || format.codeBits == 10)) return false;
		SimulatedScreen screen(format);
		device = screen.DeviceForContainer(last.patch);
		return true;
	}

	// ------------------------------------------------------- controller

	CalibrationController::CalibrationController(Hooks h) : hooks(std::move(h))
	{
		if (!hooks.clock)
		{
			systemClock = CreateSystemClock();
			hooks.clock = systemClock.get();
		}
	}

	CalibrationController::~CalibrationController()
	{
		Shutdown();
	}

	bool CalibrationController::Active() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return session && !session->Finished();
	}

	void CalibrationController::JoinWorker()
	{
		if (worker.joinable()) worker.join();
	}

	void CalibrationController::Shutdown()
	{
		stopping.store(true);
		std::shared_ptr<ICalibrationRun> current;
		std::shared_ptr<RendererPatternSink> currentSink;
		{
			std::lock_guard<std::mutex> guard(mutex);
			current = session;
			currentSink = sink;
		}
		if (current) current->Cancel();
		JoinWorker();
		// The session's destructor returns the renderer to video when it can.
		{
			std::lock_guard<std::mutex> guard(mutex);
			session.reset();
		}
		if (currentSink) currentSink->Block();
		std::thread probeWorker;
		{
			std::lock_guard<std::mutex> guard(probeMutex);
			probeWorker.swap(probeThread);
		}
		if (probeWorker.joinable()) probeWorker.join();
	}

	void CalibrationController::RendererGoing()
	{
		std::shared_ptr<ICalibrationRun> current;
		std::shared_ptr<RendererPatternSink> currentSink;
		{
			std::lock_guard<std::mutex> guard(mutex);
			current = session;
			currentSink = sink;
		}
		if (current && !current->Finished())
		{
			DebugLog::Log("Calibration: the renderer is being replaced; the run is cancelled");
			current->Cancel();
		}
		if (currentSink) currentSink->Block();   // waits for a call in progress
	}

	void CalibrationController::StartProbe(const std::string& argyllPath)
	{
		std::lock_guard<std::mutex> guard(probeMutex);
		if (probeRunning) return;
		if (probeThread.joinable()) probeThread.join();
		probeRunning = true;
		probeDone = false;
		probeThread = std::thread([this, argyllPath]()
		{
			const ArgyllProbe result = ProbeArgyll(Wide(argyllPath));
			DebugLog::Log("Calibration: Argyll probe found=%d ran=%d version=%s instruments=%zu",
				result.found ? 1 : 0, result.ran ? 1 : 0, result.usage.version.c_str(), result.usage.instruments.size());
			std::lock_guard<std::mutex> g(probeMutex);
			probe = result;
			probeRunning = false;
			probeDone = true;
		});
	}

	std::string CalibrationController::StatusText()
	{
		SessionStatus status;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (session)
			{
				session->Touch();
				status = session->Status();
			}
		}
		std::string text = FormatStatus(status);
		std::lock_guard<std::mutex> guard(probeMutex);
		text += std::string("probe: ") + (probeRunning ? "running" : probeDone ? "done" : "idle") + "\n";
		if (probeDone)
		{
			text += "probe_found: " + std::string(probe.found ? "1" : "0") + "\n";
			text += "probe_text: " + Escape(probe.Summary()) + "\n";
			std::string instruments;
			for (const SpotreadInstrument& i : probe.usage.instruments)
				instruments += (instruments.empty() ? "" : "\n") + std::to_string(i.port) + "|" + i.name;
			text += "instruments: " + Escape(instruments) + "\n";
			std::string types;
			for (const SpotreadDisplayType& t : probe.usage.displayTypes)
				types += (types.empty() ? "" : "\n") + t.selector + "|" + t.description;
			text += "display_types: " + Escape(types) + "\n";
		}
		return text;
	}

	std::unique_ptr<IMeter> CalibrationController::CreateMeter(const CalibrationSettings& settings, IPatternSink& patternSink,
		double& floorNits, std::string& error)
	{
		if (hooks.meterFactory)
			return hooks.meterFactory(settings, patternSink, floorNits, error);
		if (settings.meterModel == "simulated")
		{
			floorNits = 0.002;
			RendererPatternSink* rendererSink = dynamic_cast<RendererPatternSink*>(&patternSink);
			SimulatedMeterParams meterParams;
			meterParams.matrixError = SimulatedMeter::MatrixError(0.01, 7u);
			return std::unique_ptr<IMeter>(new SimulatedScreenMeter(
				[rendererSink](Vec3& device) { return rendererSink && rendererSink->Showing(device); },
				SimulatedDisplayParams(), meterParams, hooks.clock));
		}
		std::string searched;
		const std::wstring spotread = FindSpotread(SystemSearchEnvironment(Wide(settings.argyllPath)), searched);
		if (spotread.empty())
		{
			error = ArgyllInstallGuidance(searched);
			return nullptr;
		}
		if (!ConPtyAvailable())
		{
			error = "This Windows has no pseudo-console (Windows 10 version 1809 or newer is needed for spotread).";
			return nullptr;
		}
		std::string description;
		{
			std::lock_guard<std::mutex> guard(probeMutex);
			for (const SpotreadInstrument& i : probe.usage.instruments)
				if (settings.meterPort == 0 ? description.empty() : i.port == settings.meterPort)
					description = i.name;
		}
		const MeterModel* model = ResolveMeterModel(settings.meterModel, description);
		floorNits = model ? model->floorNits : GenericFloorNits();
		SpotreadMeterOptions options;
		options.spotread = spotread;
		options.arguments.port = settings.meterPort;
		options.arguments.displayType = settings.displayType;
		options.arguments.spectralCorrection = settings.spectralCorrection;
		options.arguments.refreshMode = settings.refreshMode == RefreshMode::Refresh ? 1 :
			settings.refreshMode == RefreshMode::NonRefresh ? 2 : 0;
		options.readTimeoutSeconds = settings.readTimeoutSeconds;
		options.openTimeoutSeconds = (std::max)(60.0, static_cast<double>(settings.readTimeoutSeconds));
		options.instrumentName = model ? model->label : description.empty() ? std::string("ArgyllCMS meter") : description;
		if (!settings.spectralCorrection.empty() && model && !model->acceptsCcss)
		{
			const std::string lower = settings.spectralCorrection.size() >= 5 ?
				settings.spectralCorrection.substr(settings.spectralCorrection.size() - 5) : std::string();
			if (lower == ".ccss" || lower == ".CCSS")
			{
				error = std::string(model->label) + " takes a .ccmx correction matrix, not a .ccss spectral sample set.";
				return nullptr;
			}
		}
		if (settings.recordTranscripts)
		{
			// Phase 2: the real meter's session in the test-data script format.
			const std::string folder = !settings.transcriptFolder.empty() ? settings.transcriptFolder :
				(currentVpRoot.empty() ? std::string(".") : currentVpRoot) + "\\luts\\calibration\\transcripts";
			ISessionClock* clock = hooks.clock;
			const std::string meterName = options.instrumentName;
			std::string version;
			{
				std::lock_guard<std::mutex> guard(probeMutex);
				version = probe.usage.version;
			}
			auto counter = std::make_shared<int>(0);
			return std::unique_ptr<IMeter>(new SpotreadMeter(options, [folder, clock, meterName, version, counter]()
			{
				const RunTimestamp stamp = clock->LocalTime();
				std::string name = TranscriptFileName(meterName, stamp.Format());
				if ((*counter)++ > 0) name.insert(name.size() - 4, "-" + std::to_string(*counter));
				std::vector<std::string> header = {
					"Recorded by VideoProcessor (record_transcripts) " + stamp.Format(),
					"meter: " + meterName,
					"ArgyllCMS spotread version: " + (version.empty() ? std::string("unknown (press Detect first)") : version) };
				return std::unique_ptr<ITerminal>(new RecordingTerminal(CreateConPtyTerminal(), folder + "\\" + name, header));
			}));
		}
		return std::unique_ptr<IMeter>(new SpotreadMeter(options, [] { return CreateConPtyTerminal(); }));
	}

	std::string CalibrationController::Start(const std::string& arguments, std::string& error)
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (session && !session->Finished())
			{
				error = "A calibration is already running.";
				return std::string();
			}
		}
		JoinWorker();
		std::string kind = "calibration";
		ArgumentValue(arguments, "kind", kind);
		if (kind != "calibration" && kind != "led" && kind != "ambient" && kind != "hdr_verify")
		{
			error = "Unknown calibration run '" + kind + "' (calibration, led, ambient or hdr_verify).";
			return std::string();
		}
		ConfigFile config;
		const std::string requested = hooks.configPath ? hooks.configPath() : std::string();
		if (!config.Load(requested.empty() ? ConfigFile::DEFAULT_FILENAME : requested, ConfigFile::ReadPolicy::Fresh))
		{
			error = "VideoProcessor could not read its configuration.";
			return std::string();
		}
		std::string validation;
		if (!ValidateConfig(config, validation))
		{
			error = "The [calibration] settings are invalid: " + validation;
			return std::string();
		}
		const CalibrationSettings settings = ReadSettings(config);
		SessionEnvironment environment;
		environment.configPath = config.GetLoadedPath().empty() ? requested : config.GetLoadedPath();
		environment.vpRoot = FolderOf(environment.configPath);
		environment.activeColorSection = hooks.activeColorProfile ? hooks.activeColorProfile() : std::string();
		environment.displayName = !settings.displayName.empty() ? settings.displayName :
			hooks.displayName ? hooks.displayName() : std::string();
		currentVpRoot = environment.vpRoot;

		// Phase 2: the Dynamic Drive run needs the projector (and optionally the
		// fast sensor) before anything starts.
		std::unique_ptr<IProjectorControl> projector;
		std::unique_ptr<ITimedLightSensor> fastSensor;
		LedDrive::ProjectorConfig projectorConfig;
		LedDrive::LedDriveConfig ledConfig;
		if (kind == "led")
		{
			projectorConfig = LedDrive::ReadProjectorConfig(config);
			ledConfig = LedDrive::ReadLedDriveConfig(config);
			if (hooks.projectorFactory)
				projector = hooks.projectorFactory(projectorConfig, error);
			else if (ConfigFile::NormalizeName(projectorConfig.protocol) != "barco_ascii")
				error = "The Dynamic Drive characterisation needs the projector's RS-232 connection: set [projector.default] "
					"protocol to barco_ascii and its port (Config > Dynamic Drive).";
			else
				projector.reset(new DriverProjectorControl(projectorConfig, [] { return LedDrive::CreateWin32SerialPort(); }));
			if (!projector) return std::string();
			if (!settings.ledSensorPort.empty())
			{
				Ambient::Settings ambient;
				std::string ambientError;
				Ambient::Read(config, ambient, ambientError);
				if (ConfigFile::NormalizeName(settings.ledSensorPort) == ConfigFile::NormalizeName(projectorConfig.port))
				{
					error = "led_sensor_port " + settings.ledSensorPort + " is the projector's RS-232 port; the fast light "
						"sensor needs its own port.";
					return std::string();
				}
				if (ambient.mode == Ambient::Mode::Sensor &&
					ConfigFile::NormalizeName(settings.ledSensorPort) == ConfigFile::NormalizeName(ambient.port))
				{
					error = "led_sensor_port " + settings.ledSensorPort + " is the room light sensor's port, which VP holds "
						"while [ambient] mode is sensor. Use another port, or set [ambient] mode to manual for this run.";
					return std::string();
				}
				if (hooks.sensorFactory)
					fastSensor = hooks.sensorFactory(settings, error);
				else
					fastSensor.reset(new SerialTimedLightSensor(settings.ledSensorPort, settings.ledSensorBaud,
						[] { return LedDrive::CreateWin32SerialPort(); }, SerialTimedLightSensor::Clock()));
				if (!fastSensor && !error.empty()) return std::string();
			}
		}

		auto newSink = std::make_shared<RendererPatternSink>(hooks.renderer);
		double floor = 0.0;
		std::unique_ptr<IMeter> meter = CreateMeter(settings, *newSink, floor, error);
		if (!meter) return std::string();
		std::shared_ptr<ICalibrationRun> newSession;
		if (kind == "led")
			newSession = std::make_shared<LedCharacterizationRun>(settings, environment, std::move(meter), floor, *newSink,
				*hooks.clock, std::move(projector), std::move(fastSensor), projectorConfig, ledConfig);
		else if (kind == "ambient")
			newSession = std::make_shared<AmbientCheckRun>(settings, environment, std::move(meter), floor, *newSink,
				*hooks.clock, hooks.roomLux);
		else if (kind == "hdr_verify")
			newSession = std::make_shared<HdrVerifyRun>(settings, environment, std::move(meter), floor, *newSink, *hooks.clock);
		else
			newSession = std::make_shared<CalibrationSession>(settings, environment, std::move(meter), floor, *newSink,
				*hooks.clock);
		newSession->Start();
		if (newSession->Finished())
		{
			error = newSession->Status().message;
			return std::string();
		}
		{
			std::lock_guard<std::mutex> guard(mutex);
			// The old session first: its destructor may still use its own sink.
			session = newSession;
			sink = newSink;
		}
		stopping.store(false);
		DebugLog::Log("Calibration: started %s (plan %s, meter %s, display kind %s)", kind.c_str(), ToString(settings.plan),
			settings.meterModel.c_str(), ToString(settings.displayKind));
		worker = std::thread([this, newSession, newSink]()
		{
			while (!stopping.load())
			{
				if (!newSession->Step()) break;
				const SessionStep step = newSession->Status().step;
				if (MeasuringStep(step) && newSession->SecondsSinceTouch() > 60.0)
				{
					DebugLog::Log("Calibration: Config has been silent for 60 s; the run is cancelled");
					newSession->Cancel();
				}
			}
			// VP closing mid-run: the run still finishes (LED restore, patterns off).
			if (!newSession->Finished())
			{
				newSession->Cancel();
				newSession->Step();
			}
			const SessionStatus final = newSession->Status();
			DebugLog::Log("Calibration: finished %s (%s) %s", final.kind.c_str(), ToString(final.step), final.message.c_str());
			if (final.step == SessionStep::Applied && hooks.applied) hooks.applied();
		});
		return StatusText();
	}

	std::string CalibrationController::Handle(const std::string& command, const std::string& arguments)
	{
		std::string error;
		if (command == "status") return StatusText();
		if (command == "detect")
		{
			ConfigFile config;
			std::string argyllPath;
			const std::string requested = hooks.configPath ? hooks.configPath() : std::string();
			if (config.Load(requested.empty() ? ConfigFile::DEFAULT_FILENAME : requested, ConfigFile::ReadPolicy::Fresh))
				argyllPath = ReadSettings(config).argyllPath;
			StartProbe(argyllPath);
			return StatusText();
		}
		if (command == "start")
		{
			const std::string text = Start(arguments, error);
			return error.empty() ? text : "error: " + Escape(error) + "\n" + StatusText();
		}
		std::shared_ptr<ICalibrationRun> current;
		{
			std::lock_guard<std::mutex> guard(mutex);
			current = session;
		}
		if (!current) return "error: No calibration is running.\n" + StatusText();
		current->SetArguments(arguments);
		if (command == "continue") current->Continue();
		else if (command == "skip") current->Skip();
		else if (command == "cancel") current->Cancel();
		else if (command == "apply") current->Apply();
		else if (command == "discard") current->Discard();
		else return "error: Unknown calibration command.\n" + StatusText();
		return StatusText();
	}
}
