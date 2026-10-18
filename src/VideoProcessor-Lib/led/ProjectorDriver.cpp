#include <pch.h>
#include "ProjectorDriver.h"
#include "BarcoAsciiDriver.h"

#include "../DebugLog.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>

namespace LedDrive
{
	namespace
	{
		// Dry run: logs what would be sent and never opens a port. Used for
		// protocol: none, log_only mode and a disabled module.
		class NoneDriver : public IProjectorDriver
		{
		public:
			NoneDriver(const ProjectorConfig& configIn, ClockPtr clockIn) : config(configIn), clock(std::move(clockIn)) {}

			const char* Protocol() const override { return "none"; }

			bool Open(std::string&) override
			{
				DebugLog::Log("Dynamic Drive: projector dry run - commands are logged, nothing is sent");
				return true;
			}

			LightRange QueryLightRange() override
			{
				LightRange range;
				if (config.lightMin >= 0 && config.lightMax >= 0)
				{
					range.known = true;
					range.minimum = config.lightMin;
					range.maximum = config.lightMax;
				}
				return range;
			}

			ProjectorState ReadState() override { return {}; }

			void SetLight(int level) override { Record(level); }

			bool SetLightNow(int level, int) override
			{
				Record(level);
				return true;
			}

			void SetContrastAsync(int value) override
			{
				if (value < 0) return;
				DebugLog::Log("Dynamic Drive: dry run, would set CNTR %d", value);
				std::lock_guard<std::mutex> guard(mutex);
				++status.commands;
				++status.contrastSends;
				status.lastSentContrast = value;
				status.lastContrastSentAt = clock->Now();
			}

			bool SendScript(const std::string& mnemonic, int value, bool* written = nullptr) override
			{
				if (written) *written = true;
				if (mnemonic == "LPW1")
					Record(value);
				else
				{
					DebugLog::Log("Dynamic Drive: dry run, would set %s %d", mnemonic.c_str(), value);
					std::lock_guard<std::mutex> guard(mutex);
					++status.commands;
					if (mnemonic == "CNTR")
					{
						++status.contrastSends;
						status.lastSentContrast = value;
						status.lastContrastSentAt = clock->Now();
					}
				}
				return true;
			}

			bool SetContrast(int value) override
			{
				DebugLog::Log("Dynamic Drive: dry run, would set CNTR %d", value);
				std::lock_guard<std::mutex> guard(mutex);
				++status.commands;
				return true;
			}

			void Close() override {}

			DriverStatus Status() const override
			{
				std::lock_guard<std::mutex> guard(mutex);
				return status;
			}

		private:
			void Record(int level)
			{
				DebugLog::Log("Dynamic Drive: dry run, would set LPW1 %d", level);
				std::lock_guard<std::mutex> guard(mutex);
				++status.commands;
				status.lastLight = level;
			}

			ProjectorConfig config;
			ClockPtr clock;
			mutable std::mutex mutex;
			DriverStatus status{ "dry-run", false, 0, 0, -1 };
		};
	}

	std::unique_ptr<IProjectorDriver> CreateProjectorDriver(const std::string& protocol,
		const ProjectorConfig& config, const SerialPortFactory& portFactory, ClockPtr clock)
	{
		if (!clock) clock = SteadyClock();
		if (protocol == "barco_ascii")
			return std::make_unique<BarcoAsciiDriver>(config,
				portFactory ? portFactory() : nullptr, clock);
		return std::make_unique<NoneDriver>(config, clock);
	}

	std::string BringToKnownState(IProjectorDriver& driver, int ledCeiling, bool resetDimMode,
		bool alwaysSetContrast)
	{
		if (resetDimMode)
			DebugLog::Log("Dynamic Drive: LED dim mode set to standard%s",
				driver.SendNow("LDMM", 0, 0) ? "" : " (not confirmed)");
		const LightRange range = driver.QueryLightRange();
		const ProjectorState found = driver.ReadState();
		std::ostringstream summary;
		summary << "projector found: light="
			<< (found.lightKnown ? std::to_string(found.light) : std::string("unknown"))
			<< " contrast="
			<< (found.contrastKnown ? std::to_string(found.contrast) : std::string("unknown"))
			<< " range="
			<< (range.known ? std::to_string(range.minimum) + "-" + std::to_string(range.maximum) :
				std::string("unknown"));
		// Startup is not time-critical: no bound, so the command gap is kept.
		const bool light = driver.SetLightNow(ledCeiling, 0);
		summary << "; light set to " << ledCeiling << (light ? "" : " (not confirmed)");
		if (alwaysSetContrast || !found.contrastKnown || found.contrast != 50)
		{
			const bool contrast = driver.SetContrast(50);
			summary << "; contrast set to 50" << (contrast ? "" : " (not confirmed)");
		}
		DebugLog::Log("Dynamic Drive: %s", summary.str().c_str());
		return summary.str();
	}

	bool RestoreFullLight(IProjectorDriver& driver, int ledCeiling, bool resetDimMode, bool restoreContrast,
		RestoreOutcome* outcome)
	{
		RestoreOutcome result;
		using namespace std::chrono;
		driver.BeginShutdown();
		// Standard dim mode first, then (after mode: script or the CNTR
		// assist) contrast back to 50, then full light - only the commands needed.
		// audit 2026-10-16: CNTR is skipped when 50 was the last value sent
		// and none is waiting or in flight.
		if (restoreContrast)
		{
			const DriverStatus status = driver.Status();
			if (status.lastSentContrast == 50 && !status.contrastBusy)
				restoreContrast = false;
		}
		// audit 2026-10-16: the old fixed budgets (LDMM 300 ms, LPW1 250, CNTR
		// 350) could not keep Barco's 500 ms command gap, so a restore command
		// could be dropped and the projector stay dim after exit. One shared
		// deadline: each later command keeps the gap plus 50 ms for its reply;
		// time the earlier ones did not use carries over.
		const int count = 1 + (resetDimMode ? 1 : 0) + (restoreContrast ? 1 : 0);
		const int perLater = std::max(0, driver.CommandGapMs()) + 50;
		// The first command keeps 600 ms (the port wait, any gap left, its
		// reply): 1 s alone, 1.15 s for two, 1.7 s for three at a 500 ms gap.
		const auto deadline = steady_clock::now() + milliseconds(std::max(1000, (count - 1) * perLater + 600));
		int remaining = count;
		auto budget = [&]()
		{
			--remaining;
			const int left = static_cast<int>(duration_cast<milliseconds>(deadline - steady_clock::now()).count());
			return std::max(1, left - remaining * perLater);
		};
		if (resetDimMode)
		{
			result.dimMode = driver.SendNow("LDMM", 0, budget());
			DebugLog::Log("Dynamic Drive: LED dim mode restored to standard%s", result.dimMode ? "" : " (not confirmed)");
		}
		// Hunt 3 (LED3): CNTR before LPW1, the bright-cut order, so CNTR is
		// never the command left at the end of the bounded budget (the one
		// an overrun drops). The budget still reserves the gap plus its reply
		// for every later command, so LPW1 keeps its own share.
		if (restoreContrast)
		{
			result.contrast = driver.SendNow("CNTR", 50, budget());
			DebugLog::Log("Dynamic Drive: contrast restored to 50%s", result.contrast ? "" : " (not confirmed)");
		}
		const bool ok = driver.SetLightNow(ledCeiling, budget());
		result.light = ok;
		DebugLog::Log("Dynamic Drive: light restored to %d%s", ledCeiling, ok ? "" : " (not confirmed)");
		if (outcome) *outcome = result;
		return ok;
	}
}
