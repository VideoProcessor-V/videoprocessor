#pragma once
#include "ConfigFile.h"

#include <string>

// Pruning pass 2026-10-17 (docs/VP-prune-inventory-2026-10-17.md; owner: "why keep
// inferior options"). Renderer profile keys and values that a newer default fully
// supersedes. Old configuration files must still load: a retired key is accepted with
// any value and ignored, a retired value runs as its replacement, and the renderer
// logs one line for each it meets. Never reject a file for these.
namespace RetiredSettings
{
	struct Key
	{
		const char* key;
		const char* replacement;   // what runs instead (for the log line)
	};

	// Retired keys: accepted with any value, ignored at runtime.
	inline const Key* FindKey(const std::string& key)
	{
		static const Key keys[] = {
			// Fixes under test since 2026-10-14; off restored a documented defect.
			{ "fix_scene_cooldown", "the 2 s scene-cut cooldown at the content rate always applies" },
			{ "dejudder_picture_area", "the matcher's statistics always cover the picture only (black bars held)" },
			{ "dither_vectors_new_frame", "a new frame's dither always takes the de-judder's motion when it is valid "
				"(its own search otherwise)" },
		};
		for (const Key& entry : keys)
			if (key == entry.key) return &entry;
		return nullptr;
	}

	struct Value
	{
		const char* key;
		const char* value;
		const char* replacement;   // the value that runs instead
	};

	// Retired values: still valid in a file, migrated to the replacement at load.
	// `value` is compared after ConfigFile::NormalizeName.
	inline const Value* FindValue(const std::string& key, const std::string& value)
	{
		static const Value values[] = {
			// Owner 2026-10-16: Auto (polar where both axes shrink alike, 1D otherwise) or 1D.
			{ "adaptive_faithful", "polar", "auto" },
			// Owner 2026-10-14 (general rates): the earlier names of whole / whole_and_1001.
			{ "dejudder_even_rates", "30", "whole" },
			{ "dejudder_even_rates", "30_and_29.97", "whole_and_1001" },
		};
		const std::string normalized = ConfigFile::NormalizeName(value);
		for (const Value& entry : values)
			if (key == entry.key && normalized == entry.value) return &entry;
		return nullptr;
	}

	// The replacement for a retired value, or the value itself (normalized).
	inline std::string Migrate(const std::string& key, const std::string& value)
	{
		const Value* retired = FindValue(key, value);
		return retired ? std::string(retired->replacement) : ConfigFile::NormalizeName(value);
	}

	inline std::string KeyNote(const std::string& section, const Key& retired)
	{
		return "[" + section + "] " + retired.key + " is retired and ignored: " + retired.replacement;
	}

	inline std::string ValueNote(const std::string& section, const Value& retired)
	{
		return "[" + section + "] " + retired.key + ": " + retired.value + " is retired; runs as " + retired.replacement;
	}
}
