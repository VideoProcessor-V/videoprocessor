#pragma once

#include "AmbientState.h"
#include "ConfigFile.h"
#include "ConfigSchema.h"
#include "DisplayRuleExpression.h"
#include "led/LedDriveConfig.h"

#include <cstdlib>
#include <string>
#include <vector>

// Stage 9a: the [ambient] section (docs/VP-stage9-ambient.md). A plain
// section, not a profile: mode, the manual room, and the detector's
// thresholds (used once the detector's driver exists, 9b).
namespace Ambient
{
	inline const std::vector<ConfigSchema::KeyRule>& SchemaRules()
	{
		static const std::vector<ConfigSchema::KeyRule> rules = {
			ConfigSchema::Choice("mode", { "off", "manual", "sensor" }),
			ConfigSchema::Choice("room", { "dark", "lit" }),
			ConfigSchema::Number("lux_lit", 0.0, 100000.0),
			ConfigSchema::Number("lux_dark", 0.0, 100000.0),
			ConfigSchema::Number("hold_s", 0.0, 600.0),
			ConfigSchema::KeyRule{ "port", [](const std::string& value)
				{
					const std::string port = ConfigFile::NormalizeName(value);
					return port.size() > 3 && port.compare(0, 3, "com") == 0 &&
						port.find_first_not_of("0123456789", 3) == std::string::npos &&
						std::atoi(port.c_str() + 3) >= 1 && std::atoi(port.c_str() + 3) <= 256;
				}, "COM1 through COM256" },
			ConfigSchema::Integer("baud", 1200, 921600)
		};
		return rules;
	}

	// The sensor needs its own port: a COM port opens for one user at a time,
	// so the projector's (the LED drive, the RS-232 test) cannot be shared.
	inline bool PortClashesWithProjector(const ConfigFile& config, std::string& error)
	{
		std::string mode, ambientPort, projectorPort;
		if (!config.TryGetString("ambient", "mode", mode) || ConfigFile::NormalizeName(mode) != "sensor" ||
			!config.TryGetString("ambient", "port", ambientPort) ||
			!config.TryGetString(LedDrive::PROJECTOR_SECTION, "port", projectorPort))
			return false;
		if (ConfigFile::NormalizeName(ambientPort) != ConfigFile::NormalizeName(projectorPort))
			return false;
		error = "ambient.port " + ConfigFile::Trim(ambientPort) +
			" is the projector's RS-232 port; the light sensor needs its own (its USB port)";
		return true;
	}

	// Defaults for absent keys; false (with error) for an invalid value, the
	// settings then keep their defaults for that key.
	inline bool Read(const ConfigFile& config, Settings& settings, std::string& error)
	{
		settings = Settings();
		bool ok = true;
		std::string raw;
		if (config.TryGetString("ambient", "mode", raw) && !ParseMode(ConfigFile::Trim(raw), settings.mode))
		{
			error = "unsupported value for ambient.mode: " + raw;
			ok = false;
		}
		if (config.TryGetString("ambient", "room", raw) && !ParseRoom(ConfigFile::Trim(raw), settings.room))
		{
			error = "unsupported value for ambient.room: " + raw;
			ok = false;
		}
		if (config.TryGetString("ambient", "port", raw))
			settings.port = ConfigFile::Trim(raw);
		if (config.TryGetString("ambient", "baud", raw))
		{
			const int baud = std::atoi(ConfigFile::Trim(raw).c_str());
			if (baud >= 1200 && baud <= 921600)
				settings.baud = baud;
			else
			{
				error = "unsupported value for ambient.baud: " + raw;
				ok = false;
			}
		}
		for (const auto& number : { std::make_pair("lux_lit", &settings.luxLit),
			std::make_pair("lux_dark", &settings.luxDark), std::make_pair("hold_s", &settings.holdSeconds) })
		{
			if (!config.TryGetString("ambient", number.first, raw))
				continue;
			double parsed = 0.0;
			if (DisplayRuleExpression::ParseNumber(ConfigFile::Trim(raw), parsed) && parsed >= 0.0)
				*number.second = parsed;
			else
			{
				error = std::string("unsupported value for ambient.") + number.first + ": " + raw;
				ok = false;
			}
		}
		// The hysteresis needs dark below lit.
		if (settings.luxDark > settings.luxLit)
			settings.luxDark = settings.luxLit;
		return ok;
	}
}
