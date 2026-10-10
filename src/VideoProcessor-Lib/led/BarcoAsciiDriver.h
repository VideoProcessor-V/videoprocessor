#pragma once

#include "ProjectorDriver.h"

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
	class BarcoAsciiDriver : public IProjectorDriver
	{
	public:
		BarcoAsciiDriver(const ProjectorConfig& config, std::unique_ptr<ISerialPort> port);
		~BarcoAsciiDriver() override;

		const char* Protocol() const override { return "barco_ascii"; }
		bool Open(std::string& error) override;
		LightRange QueryLightRange() override;
		ProjectorState ReadState() override;
		void SetLight(int level) override;
		bool SetLightNow(int level, int timeoutMs) override;
		void BeginShutdown() override;
		bool QueueSequence(const std::vector<std::pair<std::string, int>>& commands, int stepMs) override;
		bool SendNow(const std::string& mnemonic, int value, int timeoutMs) override;
		bool SetContrast(int value) override;
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
		bool Transact(const std::string& command, const char* mnemonic, int* value,
			int timeoutMs, int maxGapWaitMs, int sentLight = -1, bool abortable = false);
		bool QueryLocked(const char* mnemonic, const std::string& argument, int& value);
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
		std::thread worker;
		bool stopping = false;
		bool opened = false;
		int pendingLight = -1;
		int commands = 0;
		int timeouts = 0;
		int lastLight = -1;
		int lightSends = 0;
		int lastSentLight = -1;
		bool transmitting = false;
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
