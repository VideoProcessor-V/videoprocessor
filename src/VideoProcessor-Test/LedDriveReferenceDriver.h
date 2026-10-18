// FROZEN REFERENCE (feat/dd-projector-profiles, 2026-10-17): BarcoAsciiDriver as it was
// before device profiles (integ/2026-10-17 0642f68c), renamed. Test-only: the
// FL35 equivalence tests compare its byte stream with the profile-driven
// driver loading profiles\dynamic-drivearco-fl35.vpprojector. Never edit it
// to follow later driver changes; it is the "current built-in path" baseline.
#pragma once

#include <led/ProjectorDriver.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Barco F(L)35 ASCII protocol (601-0381-00) over RS232: ":MNEM value" with a
// carriage return; replies "%001 MNEM 000nnn" while ECHO is on. No two
// commands are ever sent closer than min_command_gap_ms, with an optional
// burst_pause_ms after every burst_commands (Barco's timing rules: 500 ms,
// 5 s after 20). read_back: on also waits for each reply (ack_timeout_ms,
// then 2 s before the next command when none came); off only sends.
namespace LedDrive
{
	class ReferenceBarcoAsciiDriver : public IProjectorDriver
	{
	public:
		ReferenceBarcoAsciiDriver(const ProjectorConfig& config, std::unique_ptr<ISerialPort> port);
		~ReferenceBarcoAsciiDriver() override;

		const char* Protocol() const override { return "barco_ascii"; }
		bool Open(std::string& error) override;
		LightRange QueryLightRange() override;
		ProjectorState ReadState() override;
		void SetLight(int level) override;
		void SetLightJump(int level) override;
		void SetContrastAsync(int value) override;
		void CancelLight() override;
		bool SetLightNow(int level, int timeoutMs) override;
		void BeginShutdown() override;
		bool QueueSequence(const std::vector<std::pair<std::string, int>>& commands, int stepMs) override;
		bool QueueSequenceNow(const std::vector<std::pair<std::string, int>>& commands, int stepMs) override;
		bool SendNow(const std::string& mnemonic, int value, int timeoutMs) override;
		bool SendScript(const std::string& mnemonic, int value, bool* written = nullptr) override;
		bool SetContrast(int value) override;
		int CommandGapMs() const override;
		void Close() override;
		DriverStatus Status() const override;

		// "%001 LPW1 000255" -> ("LPW1", 255). False for anything else.
		static bool ParseReply(const std::string& line, std::string& mnemonic, int& value);

	private:
		struct Framing
		{
			const char* name;
			bool lowercase;
			const char* suffix;
			bool acknowledged;
		};

		std::string Format(const char* mnemonic, const std::string& argument) const;
		// Sends one command and waits for the matching reply. Caller holds
		// portMutex.
		// maxGapWaitMs < 0 waits the full gap or pause; sentLight >= 0 records
		// an LPW1 write.
		// abortable: the reply wait ends early once BeginShutdown() is called
		// (the driver thread's commands only).
		// sentContrast >= 0 records a CNTR write.
		// generation / expectedGeneration (post-merge audit 2026-10-17): a value the
		// caller took at expectedGeneration of *generation (lightGeneration or
		// contrastGeneration, guarded by stateMutex); when the counter moved
		// (CancelLight, SetLightNow, a hold, a script send) the command is not sent -
		// checked before and after the gap wait, which also ends on the change.
		// wrote (optional): whether the write itself happened.
		bool Transact(const std::string& command, const char* mnemonic, int* value,
			int timeoutMs, int maxGapWaitMs, int sentLight = -1, bool abortable = false,
			int sentContrast = -1, const int* generation = nullptr, int expectedGeneration = 0,
			bool* wrote = nullptr);
		// audit 2026-10-16: an exit command within budgetMs (after the port
		// wait): the command gap first, in full when the budget allows, then
		// the reply. Caller holds portMutex.
		bool BoundedTransactLocked(const std::string& command, const char* mnemonic, int* value,
			int budgetMs, int sentLight, int sentContrast);
		bool QueryLocked(const char* mnemonic, const std::string& argument, int& value);
		// audit 2026-10-16: reopens a port closed after repeated write
		// failures (or a failed read), with a back-off of 2 s doubling to
		// 30 s. True when the port is open. Caller holds portMutex.
		bool EnsurePortLocked(std::string& error);
		void SelectFraming();
		int Clamp(int level) const;
		void Worker();

		ProjectorConfig config;
		std::unique_ptr<ISerialPort> port;
		Framing framing;
		LightRange range;
		ProjectorState state;

		mutable std::mutex stateMutex;
		std::timed_mutex portMutex;
		std::condition_variable wake;
		// audit 2026-10-16 (with stateMutex): Transact's pre-wait, ended by
		// BeginShutdown/Close for abortable commands. Separate from `wake`, so
		// a notify_one meant for the worker is never taken by a pre-wait.
		std::condition_variable gapWake;
		std::thread worker;
		bool stopping = false;
		bool opened = false;
		int pendingLight = -1;
		bool pendingJump = false;
		int commands = 0;
		int timeouts = 0;
		int lastLight = -1;
		int lightSends = 0;
		int lastSentLight = -1;
		int pendingContrast = -1;
		int contrastSends = 0;
		int lastSentContrast = -1;
		// led_landing_ms (2026-10-17): when the newest LPW1 / CNTR was written.
		std::chrono::steady_clock::time_point lastLightSentAt{};
		std::chrono::steady_clock::time_point lastContrastSentAt{};
		bool transmitting = false;
		// audit 2026-10-16 (guarded by stateMutex): a value the worker has
		// taken is sent only if its generation is unchanged once it holds the
		// port - SetContrastAsync(-1) (a hold), SetLightNow and BeginShutdown
		// cancel one waiting for the port.
		int contrastGeneration = 0;
		int lightGeneration = 0;
		bool contrastInFlight = false;
		// Barco's 2 s after a missing reply, kept by the script and sequence
		// sends that otherwise skip the command gap.
		std::chrono::steady_clock::time_point replyBackoffUntil;
		// Guarded by portMutex: the last Transact's write succeeded, write
		// failures in a row, the port's reopen back-off and the rate-limited
		// failure log.
		bool lastWriteOk = true;
		int writeFailures = 0;
		int reopenBackoffMs = 0;
		std::chrono::steady_clock::time_point reopenAt;
		std::chrono::steady_clock::time_point lastWriteFailureLog;
		bool writeFailureLogged = false;
		int unloggedWriteFailures = 0;
		std::chrono::steady_clock::time_point lastReopenLog;
		// audit 2026-10-16 follow-up (guarded by stateMutex): the port was
		// reopened; the worker repeats the handshake's ECHO and re-sends the
		// light and CNTR the model believes (a power-cycled projector is at
		// its own defaults).
		bool resyncAfterReopen = false;
		// audit 2026-10-16 (guarded by stateMutex): the last LPW1 / CNTR value
		// SendScript was asked for, recorded even when its write failed; -1
		// once the driver's own targets (SetLight, SetContrastAsync, ...) take
		// over. The resync's target after script sends.
		int scriptLight = -1;
		int scriptContrast = -1;
		// audit 2026-10-16 (guarded by portMutex): the exit restore's one
		// Open attempt ahead of the reopen back-off was made.
		bool shutdownOpenTried = false;
		int reopensUnlogged = 0;
		bool shuttingDown = false;
		// Experimental custom dim sequence (guarded by stateMutex).
		std::deque<std::pair<std::string, int>> sequence;
		int sequenceStepMs = 100;
		bool sequenceInFlight = false;
		int sequencesDone = 0;
		std::chrono::steady_clock::time_point sequenceNextAllowed;
		int sinceBurst = 0;
		// Guarded by stateMutex.
		std::chrono::steady_clock::time_point nextSendAllowed;
	};
}
