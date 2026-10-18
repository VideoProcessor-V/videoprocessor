#pragma once

// Automatic calibration: VP's side of the wizard. The Config app sends text
// commands over the configuration RPC (Operation::Calibration); the
// controller answers at once (never blocks the UI thread that serves the
// RPC) and runs the session on its own worker thread.
//
// Commands (first line), arguments as "key: value" lines:
//   status    the session status (FormatStatus) plus the Argyll probe state
//   detect    find ArgyllCMS and run `spotread -?` in the background
//   start     a new session from the saved [calibration] settings
//   continue | skip | cancel | apply | discard
//
// Safety: the renderer going away (restart, new signal, exit) cancels the
// run; Config silent for 60 s during a measuring step cancels it; the
// session always returns the renderer to video and closes the meter.

#include "CalibrationSession.h"
#include "ArgyllInstall.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IVideoRenderer;

namespace Calibration
{
	// The renderer as a pattern sink; calls are refused once Block() ran (the
	// renderer is being destroyed). Block waits for a call in progress.
	class RendererPatternSink final : public IPatternSink
	{
	public:
		using RendererFn = std::function<std::shared_ptr<IVideoRenderer>()>;
		explicit RendererPatternSink(RendererFn renderer);
		bool GetOutput(PatternOutputInfo& info) override;
		bool Show(const PatternRequest& request, std::string& error) override;
		void Block();
		// The device signal of the patch last shown (the simulated meter's input).
		bool Showing(Vec3& device);

	private:
		RendererFn renderer;
		std::mutex mutex;
		bool blocked = false;
		PatternRequest last;
		PatternOutputInfo format;
	};

	class CalibrationController
	{
	public:
		struct Hooks
		{
			std::function<std::shared_ptr<IVideoRenderer>()> renderer;   // the current renderer (atomic load)
			std::function<std::string()> activeColorProfile;            // the live Color / Output profile name
			std::function<std::string()> displayName;                    // the output monitor's name
			std::function<void()> applied;                               // the configuration was changed (UI thread: reload)
			std::function<std::string()> configPath;                     // VP's active configuration file
			// Tests: replace the terminal / clock / meter. Empty = the real ones.
			std::function<std::unique_ptr<IMeter>(const CalibrationSettings&, IPatternSink&, double& floorNits,
				std::string& error)> meterFactory;
			ISessionClock* clock = nullptr;
		};

		explicit CalibrationController(Hooks hooks);
		~CalibrationController();

		// Text in, text out (see above). Never blocks for a measurement.
		std::string Handle(const std::string& command, const std::string& arguments);
		// The renderer is about to be destroyed: patterns stop, a run is cancelled.
		void RendererGoing();
		// VP is closing: cancel and join.
		void Shutdown();
		bool Active() const;

	private:
		std::string StatusText();
		std::string Start(std::string& error);
		void JoinWorker();
		void StartProbe(const std::string& argyllPath);
		std::unique_ptr<IMeter> CreateMeter(const CalibrationSettings& settings, IPatternSink& sink, double& floorNits,
			std::string& error);

		Hooks hooks;
		std::unique_ptr<ISessionClock> systemClock;
		mutable std::mutex mutex;
		std::shared_ptr<RendererPatternSink> sink;
		std::shared_ptr<CalibrationSession> session;
		std::thread worker;
		std::atomic_bool stopping{ false };
		std::atomic_bool appliedNotified{ false };

		// The Argyll probe (detect).
		std::mutex probeMutex;
		std::thread probeThread;
		bool probeRunning = false;
		bool probeDone = false;
		ArgyllProbe probe;
	};
}
