#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

// LED mode "listen" (owner 2026-10-13, docs/VP-led-listen-script-host-plan.md):
// VP starts the owner's script itself and supervises it. In listen mode, with
// listen_script set, the LED worker calls Update() every tick: the script
// runs as `<python> -u <listen_script>` without a window, in its own folder,
// inside a job object (it ends with VP, even after a crash); after an exit it
// is started again after 2, 4, 8 ... 60 s (2 s again once it ran a minute).
// Its output goes to logs\led-script.log (the previous run's kept past
// 10 MB as led-script.prev.log); logs\led-script-now.txt holds its state for
// the Config monitor. The script drives the serial port and tells VP over UDP
// as before. Its line settings (the beta 22 script opened Tera Term for them)
// VP sets before each start from [projector.Default] port / baud, 8N1: the
// port is opened, set and closed at once (VP never holds it); a failure is
// logged and shown, and the script starts anyway.
namespace LedDrive
{
	class ScriptHost
	{
	public:
		enum class State { Off, Running, Waiting };

		ScriptHost() = default;
		~ScriptHost();
		ScriptHost(const ScriptHost&) = delete;
		ScriptHost& operator=(const ScriptHost&) = delete;

		// script empty = Off (the owner starts the script); python empty =
		// "python" on PATH. logDirectory empty = beside vp.log (tests: a folder).
		// port empty = the line settings are left alone.
		void Update(const std::string& script, const std::string& python, const std::string& logDirectory,
			const std::string& port = std::string(), int baud = 0);
		// Ends the script (the mode left listen, or VP stops).
		void Stop();

		State GetState() const { return state; }
		DWORD Pid() const { return pid; }
		int Starts() const { return starts; }
		// Seconds until the next start while Waiting.
		int SecondsToRestart() const;
		const std::string& LastError() const { return lastError; }
		// The last port set-up's failure (empty: set, or not asked).
		const std::string& PortError() const { return portError; }
		// The last start's port set-up (planner Q5): Applied, Skipped (no
		// port configured), Failed; None before the first start.
		enum class PortSetup { None, Applied, Skipped, Failed };
		PortSetup GetPortSetup() const { return portSetup; }
		// Opens `port`, sets baud and 8N1 (no flow control), closes it.
		static bool ConfigurePort(const std::string& port, int baud, std::string& error);

	private:
		bool Start(const std::string& script, const std::string& python, const std::string& logDirectory);
		void Reap();   // the process exited: close it and plan the restart
		void WriteNow(const std::string& logDirectory);

		State state = State::Off;
		HANDLE process = nullptr;
		HANDLE job = nullptr;
		HANDLE outputRead = nullptr;
		std::thread reader;
		DWORD pid = 0;
		int starts = 0;
		int backoffSeconds = 2;
		std::chrono::steady_clock::time_point startedAt{};
		std::chrono::steady_clock::time_point restartAt{};
		std::string configuredScript, configuredPython;
		std::string lastError;
		std::string portError;
		PortSetup portSetup = PortSetup::None;
		std::string nowPath;
		std::string writtenNow;
	};
}
