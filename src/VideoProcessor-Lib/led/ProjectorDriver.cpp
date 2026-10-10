#include <pch.h>
#include "ProjectorDriver.h"
#include "BarcoAsciiDriver.h"

#include "../DebugLog.h"

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
			explicit NoneDriver(const ProjectorConfig& configIn) : config(configIn) {}

			const char* Protocol() const override { return "none"; }

			bool Open(std::string&) override
			{
				DebugLog::Log("LED drive: projector dry run - commands are logged, nothing is sent");
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

			bool SetContrast(int value) override
			{
				DebugLog::Log("LED drive: dry run, would set CNTR %d", value);
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
				DebugLog::Log("LED drive: dry run, would set LPW1 %d", level);
				std::lock_guard<std::mutex> guard(mutex);
				++status.commands;
				status.lastLight = level;
			}

			ProjectorConfig config;
			mutable std::mutex mutex;
			DriverStatus status{ "dry-run", false, 0, 0, -1 };
		};
	}

	std::unique_ptr<IProjectorDriver> CreateProjectorDriver(const std::string& protocol,
		const ProjectorConfig& config, const SerialPortFactory& portFactory)
	{
		if (protocol == "barco_ascii")
			return std::make_unique<BarcoAsciiDriver>(config,
				portFactory ? portFactory() : nullptr);
		return std::make_unique<NoneDriver>(config);
	}

	std::string BringToKnownState(IProjectorDriver& driver, int ledCeiling)
	{
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
		if (!found.contrastKnown || found.contrast != 50)
		{
			const bool contrast = driver.SetContrast(50);
			summary << "; contrast set to 50" << (contrast ? "" : " (not confirmed)");
		}
		DebugLog::Log("LED drive: %s", summary.str().c_str());
		return summary.str();
	}

	bool RestoreFullLight(IProjectorDriver& driver, int ledCeiling)
	{
		const bool ok = driver.SetLightNow(ledCeiling, 1000);
		DebugLog::Log("LED drive: light restored to %d%s", ledCeiling, ok ? "" : " (not confirmed)");
		return ok;
	}
}
