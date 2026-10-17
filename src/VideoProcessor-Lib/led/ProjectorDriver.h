#pragma once

#include "LedDriveConfig.h"
#include "SerialPort.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Layer 3 of the LED drive module: projector protocols. The controller posts
// light targets; a driver never sees the renderer and the controller never
// sees a port. See docs/VP-led-drive-module.md section 5.
namespace LedDrive
{
	struct LightRange
	{
		bool known = false;
		int minimum = 0;
		int maximum = 255;
	};

	struct ProjectorState
	{
		bool lightKnown = false;
		int light = 0;
		bool contrastKnown = false;
		int contrast = 0;
	};

	struct DriverStatus
	{
		// How commands are framed ("cr", "literal_cr", "script", "dry-run").
		std::string framing;
		// True when the projector answers commands (ECHO on).
		bool acknowledged = false;
		int commands = 0;
		int timeouts = 0;
		// Last level the projector confirmed (or, without acknowledgements,
		// the last level sent); -1 before any.
		int lastLight = -1;
		// LPW1 levels written to the port: how many, and the newest (counted
		// when written, before any reply).
		int lightSends = 0;
		int lastSentLight = -1;
		// The same for CNTR values (7c-1).
		int contrastSends = 0;
		int lastSentContrast = -1;
		// A light target is waiting or being transmitted.
		bool busy = false;
		// A CNTR value (SetContrastAsync) is waiting or being transmitted
		// (audit 2026-10-16).
		bool contrastBusy = false;
		// A command sequence (experimental custom dim) is waiting or being
		// sent, and how many sequences have finished.
		bool sequenceBusy = false;
		int sequencesDone = 0;
	};

	class IProjectorDriver
	{
	public:
		virtual ~IProjectorDriver() = default;
		virtual const char* Protocol() const = 0;
		// Opens the transport and performs the protocol handshake. A driver
		// whose Open failed never sends anything.
		virtual bool Open(std::string& error) = 0;
		virtual LightRange QueryLightRange() = 0;
		virtual ProjectorState ReadState() = 0;
		// Asynchronous and coalesced: if a newer target arrives before the
		// previous command completes, only the newest is sent.
		virtual void SetLight(int level) = 0;
		// The same, sent whole (a cut jump): not split by max_levels_per_command.
		virtual void SetLightJump(int level) { SetLight(level); }
		// Synchronous, bounded by timeoutMs (used at startup and exit).
		// timeoutMs > 0 bounds the whole call (exit); 0 waits as long as the
		// command gap and pause require (startup).
		virtual bool SetLightNow(int level, int timeoutMs) = 0;
		// Before the exit restore: drop queued targets and stop the driver's
		// own thread from sending anything more, so the restore is last.
		virtual void BeginShutdown() {}
		// Experimental custom LED dim: commands (mnemonic, value) sent stepMs
		// apart by the driver's own thread, ahead of light targets; a new
		// sequence replaces the unsent rest of the previous one. False when
		// the driver cannot send.
		virtual bool QueueSequence(const std::vector<std::pair<std::string, int>>& commands, int stepMs)
		{
			(void)commands;
			(void)stepMs;
			return false;
		}
		// The same, without waiting for the step interval since the last
		// sequence command (leaving the custom dim as light returns).
		virtual bool QueueSequenceNow(const std::vector<std::pair<std::string, int>>& commands, int stepMs)
		{
			return QueueSequence(commands, stepMs);
		}
		// One command now (startup, exit); timeoutMs > 0 bounds the call.
		virtual bool SendNow(const std::string& mnemonic, int value, int timeoutMs)
		{
			(void)mnemonic;
			(void)value;
			(void)timeoutMs;
			return false;
		}
		// mode: script (step 7b): one LPW1 or CNTR command, synchronous, in the
		// script's own timing: no min_command_gap_ms or burst pause (the
		// script's sleeps space its commands), not coalesced or split. Returns
		// once the command is written and, with read_back on, its reply came
		// (or ack_timeout_ms passed). False once shutdown has begun.
		// mode: compensate's CNTR assist (7c-1): asynchronous like SetLight,
		// keeping only the newest value, and taking the next command slot
		// ahead of a waiting LPW1 (CNTR commands are rare: only on integer
		// crossings). A negative value drops a waiting one.
		virtual void SetContrastAsync(int value) { (void)value; }
		virtual bool SendScript(const std::string& mnemonic, int value)
		{
			(void)mnemonic;
			(void)value;
			return false;
		}
		// The startup correction to 50 and, after mode: script, the exit
		// restore. mode: compensate never drives CNTR.
		virtual bool SetContrast(int value) = 0;
		virtual void Close() = 0;
		virtual DriverStatus Status() const = 0;
	};

	using SerialPortFactory = std::function<std::unique_ptr<ISerialPort>()>;

	// protocol: "barco_ascii" or "none" (see EffectiveProtocol). The port
	// factory is only called for a protocol that needs a port.
	std::unique_ptr<IProjectorDriver> CreateProjectorDriver(const std::string& protocol,
		const ProjectorConfig& config, const SerialPortFactory& portFactory);

	// Startup: logs what the projector reports, then sets the light to the
	// ceiling and CNTR to 50 when it is not already 50 (a previous run of the
	// old script may have left 74). Returns a one-line summary.
	// resetDimMode: also set the LED dim mode back to standard (LDMM 0) - used
	// when the experimental custom dim is configured, so a crash cannot leave
	// the projector in custom mode.
	// alwaysSetContrast: send CNTR 50 even when the projector reports 50, as
	// the script did at its start (mode: script, step 7b).
	std::string BringToKnownState(IProjectorDriver& driver, int ledCeiling, bool resetDimMode = false,
		bool alwaysSetContrast = false);

	// Clean exit, disable or switch to log_only: light back to the ceiling,
	// never blocking for more than a second. restoreContrast: CNTR back to 50
	// as well, after the light (mode: script drove it).
	bool RestoreFullLight(IProjectorDriver& driver, int ledCeiling, bool resetDimMode = false,
		bool restoreContrast = false);
}
