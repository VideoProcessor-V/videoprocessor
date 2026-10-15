#pragma once

#include <led/ProjectorDriver.h>

#include <functional>
#include <string>
#include <vector>

// RS-232 connection test (LED drive page): LED to 60, read back, wait 5 s,
// LED to 255 (or the projector's reported maximum when lower), read back,
// pass/fail. Always on the LED module's own driver and serial code (the
// barco_ascii driver with the saved projector settings, read-back on), on a
// connection opened for the test: by Config when no VP holds the port, or by
// VP, which pauses its LED drive for the test and resumes it after.
namespace LedDrive
{
	struct ConnectionTestResult
	{
		bool pass = false;
		bool refused = false;          // not run (e.g. video is playing)
		std::string summary;           // one line for the Config page
		std::vector<std::string> lines; // the steps, for the log and a details view
	};

	struct ConnectionTestPlan
	{
		int lowLevel = 60;
		int highLevel = 255;
		int holdMs = 5000;
		int commandTimeoutMs = 3000;
	};

	// The steps on an open driver. sleepMs waits (tests pass a stub).
	ConnectionTestResult RunConnectionTest(IProjectorDriver& driver,
		const std::function<void(int)>& sleepMs, const ConnectionTestPlan& plan = ConnectionTestPlan());

	// Opens a driver from the saved projector settings (protocol barco_ascii),
	// runs the test and closes it. Fails cleanly when the port cannot be
	// opened (for example because a running VP's LED drive holds it).
	ConnectionTestResult RunConnectionTestOnNewConnection(const ProjectorConfig& projector,
		const SerialPortFactory& portFactory, const ConnectionTestPlan& plan = ConnectionTestPlan());
}
