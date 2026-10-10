#pragma once

#include "LedDriveConfig.h"
#include "SerialPort.h"

#include <functional>
#include <memory>
#include <string>

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
		// A light target is waiting or being transmitted.
		bool busy = false;
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
		// Synchronous, bounded by timeoutMs (used at startup and exit).
		// timeoutMs > 0 bounds the whole call (exit); 0 waits as long as the
		// command gap and pause require (startup).
		virtual bool SetLightNow(int level, int timeoutMs) = 0;
		// Before the exit restore: drop queued targets and stop the driver's
		// own thread from sending anything more, so the restore is last.
		virtual void BeginShutdown() {}
		// Only the one-off startup correction to 50; CNTR is never driven.
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
	std::string BringToKnownState(IProjectorDriver& driver, int ledCeiling);

	// Clean exit, disable or switch to log_only: light back to the ceiling,
	// never blocking for more than a second.
	bool RestoreFullLight(IProjectorDriver& driver, int ledCeiling);
}
