#pragma once

#include "ProjectorDriver.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Barco F(L)35 ASCII protocol (601-0381-00) over RS232: ":MNEM value" with a
// carriage return; replies "%001 MNEM 000nnn" while ECHO is on. Commands are
// paced on replies: send, wait for the reply or ack_timeout_ms, then honour
// min_command_gap_ms.
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
		bool Transact(const std::string& command, const char* mnemonic, int* value,
			int timeoutMs, int maxGapWaitMs);
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
		std::mutex portMutex;
		std::condition_variable wake;
		std::thread worker;
		bool stopping = false;
		bool opened = false;
		int pendingLight = -1;
		int commands = 0;
		int timeouts = 0;
		int lastLight = -1;
		std::chrono::steady_clock::time_point nextSendAllowed;
	};
}
