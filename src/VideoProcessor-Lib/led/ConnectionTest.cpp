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
		if (ConnectionTestCancelled(plan))
		{
			result.summary = "FAIL: cancelled (VideoProcessor is closing)";
			result.lines.push_back(result.summary);
			return result;
		}
		const LightRange range = driver.QueryLightRange();
		const int high = range.known && range.maximum < plan.highLevel ? range.maximum : plan.highLevel;
		const int low = range.known ? std::max(range.minimum, plan.lowLevel) : plan.lowLevel;
		const bool lowOk = Step(driver, low, plan.commandTimeoutMs, result);
		if (lowOk && sleepMs && !ConnectionTestCancelled(plan))
			sleepMs(plan.holdMs);
		// Always try to put the light back, even after a failed first step or
		// a cancel during the hold (bug hunt 3, SH5).
		const bool highOk = Step(driver, high, plan.commandTimeoutMs, result);
		const bool cancelled = ConnectionTestCancelled(plan);
		if (cancelled)
			result.lines.push_back(Line("cancelled during the hold (VideoProcessor is closing); LED put back to %d", high));
		result.pass = lowOk && highOk && !cancelled;
		if (cancelled)
		{
			result.summary = "FAIL: cancelled (VideoProcessor is closing)";
			return result;
		}
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
		// The hold wakes early on a cancel (bug hunt 3, SH5).
		const auto cancel = plan.cancel;
		result = RunConnectionTest(*driver,
			[cancel](int ms)
			{
				const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
				while (std::chrono::steady_clock::now() < until && !(cancel && cancel->load()))
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}, plan);
		driver->Close();
		return result;
	}
}
