#pragma once

#include "ConfigFile.h"

#include <string>

// The first queue section supplies the startup policy before runtime profile
// selection exists. Later named profiles can replace every queue-policy value
// dynamically; this helper only resolves the initial ordered baseline and
// read-compatible legacy recovery locations.
namespace QueueConfiguration
{
	inline bool IsDirectNamedQueueSection(const std::string& section)
	{
		static constexpr const char prefix[] = "queue.";
		return section.rfind(prefix, 0) == 0 &&
			section.size() > sizeof(prefix) - 1 &&
			section.find('.', sizeof(prefix) - 1) == std::string::npos;
	}

	inline bool ResolveDefaultSection(const ConfigFile& config,
		std::string& section)
	{
		if (config.HasSection("queue"))
		{
			section = "queue";
			return true;
		}
		for (const std::string& candidate : config.GetSectionNames())
			if (IsDirectNamedQueueSection(candidate))
			{
				section = candidate;
				return true;
			}
		section.clear();
		return false;
	}

	inline bool TryGetDefaultString(const ConfigFile& config,
		const std::string& key, std::string& value,
		std::string* resolvedSection = nullptr)
	{
		std::string section;
		if (!ResolveDefaultSection(config, section) ||
			!config.TryGetString(section, key, value))
			return false;
		if (resolvedSection) *resolvedSection = section;
		return true;
	}

	// Hunt 3 (CFG9): the renderer queue's hard capacity at start-up - the
	// default queue profile's queue_size, else the legacy application keys
	// ([command_line], then [general]; both validated by MainConfigSchema,
	// [general] was never read before). `source` names where it came from.
	inline bool TryGetHardCapacityString(const ConfigFile& config,
		std::string& value, std::string& source)
	{
		std::string section;
		if (TryGetDefaultString(config, "queue_size", value, &section))
		{
			source = section;
			return true;
		}
		for (const char* legacy : { "command_line", "general" })
			if (config.TryGetString(legacy, "queue_size", value))
			{
				source = legacy;
				return true;
			}
		return false;
	}

	// [queue_recovery] remains a read-compatible legacy source.  Callers rely
	// on MainConfigSchema to reject a same-key canonical/legacy duplicate
	// before startup; canonical queue values deliberately win here as a safe
	// fallback for diagnostic paths that only load the file.
	inline bool TryGetRecoveryString(const ConfigFile& config,
		const std::string& key, std::string& value,
		std::string* resolvedSection = nullptr)
	{
		if (TryGetDefaultString(config, key, value, resolvedSection))
			return true;
		if (!config.TryGetString("queue_recovery", key, value))
			return false;
		if (resolvedSection) *resolvedSection = "queue_recovery";
		return true;
	}
}
