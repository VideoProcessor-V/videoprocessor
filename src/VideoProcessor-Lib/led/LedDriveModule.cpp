#include <pch.h>
#include "LedDriveModule.h"

#include "../DebugLog.h"

#include <algorithm>

namespace LedDrive
{
	namespace
	{
		const char* ModeName(Mode mode)
		{
			return mode == Mode::Compensate ? "compensate" : mode == Mode::Script ? "script" : "log_only";
		}
	}

	LedDriveModule::LedDriveModule(const ProjectorConfig& projectorIn, const LedDriveConfig& ledIn,
		SerialPortFactory portFactoryIn)
		: projector(projectorIn), led(ledIn), portFactory(std::move(portFactoryIn)), statsBuilder(ledIn)
	{
		gain.knee = led.knee;
		gain.eotf = led.compensationEotf;
	}

	LedDriveModule::~LedDriveModule()
	{
		Stop();
	}

	void LedDriveModule::Start()
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (started) return;
		started = true;
		stopping = false;
		thread = std::thread(&LedDriveModule::Run, this);
	}

	void LedDriveModule::Stop()
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (!started) return;
			stopping = true;
		}
		wake.notify_all();
		if (thread.joinable()) thread.join();
		std::lock_guard<std::mutex> guard(mutex);
		started = false;
	}

	void LedDriveModule::SubmitMeasurement(const RawMeasurement& raw)
	{
		std::lock_guard<std::mutex> guard(mutex);
		const FrameStats stats = statsBuilder.Build(raw);
		if (stats.blocks == 0) return;
		latest = stats;
		haveStats = true;
		// A cut or fade between two ticks must still reach the controller.
		pendingCut = pendingCut || stats.cut;
		pendingFade = pendingFade || stats.fade;
	}

	CompensationGain LedDriveModule::CurrentGain() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return gain;
	}

	bool LedDriveModule::DrivingProjector() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return live;
	}

	DriverStatus LedDriveModule::ProjectorStatus() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return driver ? driver->Status() : DriverStatus{};
	}

	int LedDriveModule::Ticks() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return ticks;
	}

	bool LedDriveModule::WaitUntil(std::chrono::steady_clock::time_point when)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return !wake.wait_until(lock, when, [this] { return stopping; });
	}

	void LedDriveModule::Run()
	{
		const std::string protocol = EffectiveProtocol(projector, led);
		std::unique_ptr<IProjectorDriver> created = CreateProjectorDriver(protocol, projector, portFactory);
		std::string error;
		bool driving = protocol != "none";
		if (!created->Open(error))
		{
			DebugLog::Log("LED drive: %s; continuing as a dry run", error.c_str());
			created = CreateProjectorDriver("none", projector, portFactory);
			created->Open(error);
			driving = false;
		}
		{
			std::lock_guard<std::mutex> guard(mutex);
			driver = std::move(created);
			live = driving;
		}
		BringToKnownState(*driver, led.ledCeiling);
		DebugLog::Log("LED drive: running mode=%s protocol=%s tick=%d ms%s", ModeName(led.mode),
			driving ? protocol.c_str() : "none (dry run)", led.tickMs,
			led.mode == Mode::Compensate ? " (compensate arrives in step 4; behaving as script)" : "");

		ScriptController controller(led);
		controller.Reset(led.ledCeiling, 50);
		// The emulated contrast is applied to the picture only while the
		// projector is really being driven; a dry run never changes the picture.
		const bool applyGain = driving && led.mode != Mode::LogOnly;
		std::string lastLog;
		auto lastLogTime = std::chrono::steady_clock::now();
		for (;;)
		{
			const auto tickStart = std::chrono::steady_clock::now();
			FrameStats stats;
			bool have = false;
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (stopping) break;
				have = haveStats;
				stats = latest;
				stats.cut = pendingCut;
				stats.fade = pendingFade;
				pendingCut = pendingFade = false;
			}
			int durationMs = 0;
			if (have)
			{
				const ControllerOutput out = controller.Tick(stats);
				if (out.lightCommand >= 0) driver->SetLight(out.lightCommand);
				for (const ContrastStep& step : out.contrastSchedule)
				{
					if (!WaitUntil(tickStart + std::chrono::milliseconds(step.offsetMs))) break;
					std::lock_guard<std::mutex> guard(mutex);
					gain.active = applyGain;
					gain.gain = applyGain ? step.contrast / 50.0 : 1.0;
					gain.domain = out.gainDomain;
				}
				durationMs = out.durationMs;
				const auto now = std::chrono::steady_clock::now();
				// Log changes, plus a heartbeat every ten seconds.
				if (out.lightCommand >= 0 || !out.contrastSchedule.empty() ||
					now - lastLogTime >= std::chrono::seconds(10))
				{
					if (out.log != lastLog || now - lastLogTime >= std::chrono::seconds(10))
						DebugLog::Log("%s%s", out.log.c_str(), applyGain ? "" : " (dry run)");
					lastLog = out.log;
					lastLogTime = now;
				}
				std::lock_guard<std::mutex> guard(mutex);
				++ticks;
			}
			if (!WaitUntil(tickStart + std::chrono::milliseconds(std::max(led.tickMs, durationMs))))
				break;
		}

		{
			std::lock_guard<std::mutex> guard(mutex);
			gain.active = false;
			gain.gain = 1.0;
		}
		if (driving) RestoreFullLight(*driver, led.ledCeiling);
		driver->Close();
	}
}
