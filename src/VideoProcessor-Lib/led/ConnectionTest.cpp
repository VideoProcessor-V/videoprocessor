#include <pch.h>
#include "ConnectionTest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace LedDrive
{
	namespace
	{
		std::string Line(const char* format, int a, int b = 0)
		{
			char text[160];
			std::snprintf(text, sizeof(text), format, a, b);
			return text;
		}

		// One step: send the level, then ask the projector what it has.
		bool Step(IProjectorDriver& driver, int level, int timeoutMs, ConnectionTestResult& result)
		{
			if (!driver.SetLightNow(level, timeoutMs))
			{
				result.lines.push_back(Line("LED %d: not sent (no acknowledgement within %d ms)", level, timeoutMs));
				return false;
			}
			const ProjectorState state = driver.ReadState();
			if (!state.lightKnown)
			{
				result.lines.push_back(Line("LED %d: sent; no reply to the read-back (LPW1 ?)", level));
				return false;
			}
			if (state.light != level)
			{
				result.lines.push_back(Line("LED %d: projector reports %d", level, state.light));
				return false;
			}
			result.lines.push_back(Line("LED %d: confirmed %d", level, state.light));
			return true;
		}
	}

	ConnectionTestResult RunConnectionTest(IProjectorDriver& driver, const std::function<void(int)>& sleepMs,
		const ConnectionTestPlan& plan)
	{
		ConnectionTestResult result;
		const LightRange range = driver.QueryLightRange();
		const int high = range.known && range.maximum < plan.highLevel ? range.maximum : plan.highLevel;
		const int low = range.known ? std::max(range.minimum, plan.lowLevel) : plan.lowLevel;
		const bool lowOk = Step(driver, low, plan.commandTimeoutMs, result);
		if (lowOk && sleepMs)
			sleepMs(plan.holdMs);
		// Always try to put the light back, even after a failed first step.
		const bool highOk = Step(driver, high, plan.commandTimeoutMs, result);
		result.pass = lowOk && highOk;
		if (result.pass)
			result.summary = Line("PASS: the projector confirmed LED %d and LED %d", low, high);
		else
			result.summary = "FAIL: " + (lowOk ? result.lines.back() : result.lines.front());
		return result;
	}

	ConnectionTestResult RunConnectionTestOnNewConnection(const ProjectorConfig& projectorIn,
		const SerialPortFactory& portFactory, const ConnectionTestPlan& plan)
	{
		// Read-back is what the test checks: the test connection always asks
		// the projector (the saved read_back setting is left as it is).
		ProjectorConfig projector = projectorIn;
		projector.protocol = "barco_ascii";
		projector.readBack = true;
		ConnectionTestResult result;
		// Device profiles: never test with a refused profile, and a device without
		// read-back cannot confirm anything (the user confirms visually).
		if (!projector.deviceProfileError.empty())
		{
			result.summary = "FAIL: device profile refused: " + projector.deviceProfileError;
			result.lines.push_back(result.summary);
			return result;
		}
		if (!projector.device.readBackSupported)
		{
			result.summary = "UNVERIFIED: the device profile " + projector.device.DisplayName() +
				" has no read-back, so the projector cannot confirm commands and nothing was sent; check the light changes by eye with Dynamic Drive running";
			result.lines.push_back(result.summary);
			return result;
		}
		std::unique_ptr<IProjectorDriver> driver = CreateProjectorDriver("barco_ascii", projector, portFactory);
		std::string error;
		if (!driver || !driver->Open(error))
		{
			result.summary = "FAIL: could not open " + projector.port + (error.empty() ? std::string() : ": " + error);
			result.lines.push_back(result.summary);
			return result;
		}
		const DriverStatus status = driver->Status();
		if (!status.acknowledged)
		{
			result.summary = "FAIL: " + projector.port + " opened, but the projector did not answer (check the cable, "
				"baud rate and that the projector is on)";
			result.lines.push_back(result.summary);
			driver->Close();
			return result;
		}
		result = RunConnectionTest(*driver,
			[](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }, plan);
		driver->Close();
		return result;
	}
}
