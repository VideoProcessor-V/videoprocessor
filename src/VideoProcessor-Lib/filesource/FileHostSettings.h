#pragma once

#include <ConfigFile.h>
#include <MainConfigSchema.h>

#include <cstdint>
#include <string>
#include <vector>

// VP Renderer DirectShow filter (2026-10-17): the [general] keys only the
// filter reads. Out-of-range or malformed values keep the default and are
// reported (Config refuses them; a hand edit is not fatal to playback).
namespace FileSource
{
	struct FileHostSettings
	{
		// Picture offset against the audio, ms (positive = the picture later).
		int avOffsetMs = MainConfigSchema::FileAvOffsetDefaultMs;
		// How long the filter process keeps the LED drive between graphs (0 = stop at once).
		uint32_t ledHoldoverMs = static_cast<uint32_t>(MainConfigSchema::FileLedHoldoverDefaultMs);
	};

	inline bool ParseBoundedInteger(const std::string& text, int minimum, int maximum, int& value)
	{
		const std::string trimmed = ConfigFile::Trim(text);
		if (trimmed.empty())
			return false;
		try
		{
			size_t consumed = 0;
			const long long parsed = std::stoll(trimmed, &consumed);
			if (consumed != trimmed.size() || parsed < minimum || parsed > maximum)
				return false;
			value = static_cast<int>(parsed);
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	inline FileHostSettings ReadFileHostSettings(const ConfigFile& config,
		std::vector<std::string>* problems = nullptr)
	{
		FileHostSettings settings;
		std::string text;
		int value = 0;
		if (config.TryGetString("general", "file_av_offset_ms", text))
		{
			if (ParseBoundedInteger(text, MainConfigSchema::FileAvOffsetMinMs,
				MainConfigSchema::FileAvOffsetMaxMs, value))
				settings.avOffsetMs = value;
			else if (problems)
				problems->push_back("[general] file_av_offset_ms '" + text + "' is not an integer from " +
					std::to_string(MainConfigSchema::FileAvOffsetMinMs) + " to " +
					std::to_string(MainConfigSchema::FileAvOffsetMaxMs) + "; using " +
					std::to_string(settings.avOffsetMs));
		}
		if (config.TryGetString("general", "file_led_holdover_ms", text))
		{
			if (ParseBoundedInteger(text, MainConfigSchema::FileLedHoldoverMinMs,
				MainConfigSchema::FileLedHoldoverMaxMs, value))
				settings.ledHoldoverMs = static_cast<uint32_t>(value);
			else if (problems)
				problems->push_back("[general] file_led_holdover_ms '" + text + "' is not an integer from " +
					std::to_string(MainConfigSchema::FileLedHoldoverMinMs) + " to " +
					std::to_string(MainConfigSchema::FileLedHoldoverMaxMs) + "; using " +
					std::to_string(settings.ledHoldoverMs));
		}
		return settings;
	}
}
