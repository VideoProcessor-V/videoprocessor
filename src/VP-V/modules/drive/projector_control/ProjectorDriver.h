#pragma once

#include "LedClock.h"
#include <led/LedDriveConfig.h>
#include "SerialPort.h"

#include <chrono>
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
		// led_landing_ms (2026-10-17): when the newest LPW1 / CNTR was written
		// to the port (the driver's clock - the steady clock in production -
		// taken at the write); the epoch (a
		// default time_point) when unknown. The module lands each send
		// led_latency_ms / cntr_latency_ms after this.
		std::chrono::steady_clock::time_point lastLightSentAt{};
		std::chrono::steady_clock::time_point lastContrastSentAt{};
		// Bug hunt 2 (2026-10-17): the earliest the driver's thread may write its
		// next queued command (min_command_gap_ms, burst pause, Barco's 2 s after a
		// missing reply); the epoch when unknown. The module lands a queued LPW1
		// from there.
		std::chrono::steady_clock::time_point nextSendAt{};
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
		// Bug hunt 2026-10-17: drops a waiting LPW1 target and the rest of a
		// split dimming drop (compensate's queue when the mode changes).
		virtual void CancelLight() {}
		// written (optional, post-merge audit 2026-10-17): whether the write itself
		// happened, apart from the reply - a missing reply is not a failed write
		// (the caller resends only when nothing was written).
		virtual bool SendScript(const std::string& mnemonic, int value, bool* written = nullptr)
		{
			(void)mnemonic;
			(void)value;
			if (written) *written = false;
			return false;
		}
		// The startup correction to 50 and, after mode: script, the exit
		// restore. mode: compensate never drives CNTR.
		virtual bool SetContrast(int value) = 0;
		// audit 2026-10-16: the spacing the projector needs between commands
		// (min_command_gap_ms), for the exit restore's budget; 0 = none.
		virtual int CommandGapMs() const { return 0; }
		virtual void Close() = 0;
		virtual DriverStatus Status() const = 0;
	};

	using SerialPortFactory = std::function<std::unique_ptr<ISerialPort>()>;

	// protocol: "barco_ascii" or "none" (see EffectiveProtocol). The port
	// factory is only called for a protocol that needs a port.
	// clock: the drivers' time source (command gaps, write times); the steady
	// clock in production.
	std::unique_ptr<IProjectorDriver> CreateProjectorDriver(const std::string& protocol,
		const ProjectorConfig& config, const SerialPortFactory& portFactory, ClockPtr clock = SteadyClock());

	// Startup: logs what the projector reports, then sets the light to the
	// ceiling and CNTR to 50 when it is not already 50 (a previous run of the
	// old script may have left 74). Returns a one-line summary.
	// resetDimMode: also set the LED dim mode back to standard (LDMM 0), so a
	// crash cannot leave the projector in custom mode (audit 2026-10-16: the
	// module asks for it at every start on a driven projector, not only while
	// the experimental custom dim is configured).
	// alwaysSetContrast: send CNTR 50 even when the projector reports 50, as
	// the script did at its start (mode: script, step 7b).
	std::string BringToKnownState(IProjectorDriver& driver, int ledCeiling, bool resetDimMode = false,
		bool alwaysSetContrast = false);

	// Clean exit, disable or switch to log_only: light back to the ceiling.
	// restoreContrast: CNTR back to 50 as well, after the light (mode: script
	// drove it; skipped when 50 was the last CNTR sent and none is pending).
	// audit 2026-10-16: bounded by one second for a single command; with
	// LDMM 0 and/or CNTR as well, by enough to keep the projector's command
	// gap between them (CommandGapMs + 50 ms for each later one; 1.7 s for
	// three at Barco's 500 ms), so no restore command is dropped.
	// Returns whether the light was confirmed; `outcome` (optional, bug hunt 2
	// 2026-10-17) also reports the LDMM and CNTR sends (true when not needed).
	struct RestoreOutcome
	{
		bool light = true;
		bool dimMode = true;
		bool contrast = true;
		bool Confirmed() const { return light && dimMode && contrast; }
	};
	bool RestoreFullLight(IProjectorDriver& driver, int ledCeiling, bool resetDimMode = false,
		bool restoreContrast = false, RestoreOutcome* outcome = nullptr);
}
