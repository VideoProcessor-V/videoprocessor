#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Fixed-constant batch B7 (2026-10-18, sweep #22; owner 2026-10-17: "unify the refresh-key
// convention, with cfg migration + backup").
//
// Display-refresh keys were read three ways: the VP renderer's refresh-rate commands floored with
// a 1e-6 allowance, the madVR shader rules floored with 1e-4, and [ppm_correction] /
// [display_refresh_rate_override] truncated (then took the nearest key within 0.5 Hz). All of
// them meant "the integer band": 23 = 23.976, 59 = 59.94, 60 = 60. (${source_rate} is a SOURCE
// key and rounds; it is not a refresh key and is unchanged.)
//
// One convention now:
//  - a key is a rate: an integer (60), a decimal (59.94, 23.976) or a ratio (60000/1001);
//  - a key matches a refresh rate exactly when within kExactTolerance (400 ppm: 59.94 and 60 are
//    1000 ppm apart, so they never meet), and an exact key wins;
//  - an integer key that matches nothing exactly still matches its legacy band (floor), so a file
//    the migration has not touched keeps working; then the nearest key within 0.5 Hz, as before.
//  - LegacyKey is the one implementation of the band for lookups that stay integer (the refresh
//    commands' ranges, the madVR shader rules' nominal rate).
// The migration rewrites a saved integer key that stands for a 1000/1001 rate (23, 29, 47, 59, 71,
// 89, 119, 143, 239 ...: K where K + 1 is a whole multiple of the NTSC 24 or 30 Hz bases) as that
// rate (23.976, 29.97, 47.952, 59.94, ...): MigratedKey. ConfigFile applies it in memory at load;
// the Config editor applies it to the file (with the migration backup).
namespace RefreshKey
{
	constexpr double kExactTolerance = 4e-4;
	constexpr double kNearestToleranceHz = 0.5;   // the established fallback of both keyed sections
	constexpr double kBandAllowanceHz = 1e-4;     // a rate computed a hair below a whole number

	// The integer band key (23.976 -> 23, 59.94 -> 59, 60 -> 60); 0 for no rate.
	inline int LegacyKey(double hz)
	{
		if (!std::isfinite(hz) || hz <= 0.0)
			return 0;
		return static_cast<int>(std::floor(hz + kBandAllowanceHz));
	}

	// "60", "59.94", "60000/1001" -> the rate; false for anything else (or <= 0).
	inline bool Parse(const std::string& text, double& hz)
	{
		hz = 0.0;
		std::string t;
		for (char c : text)
			if (c != ' ' && c != '\t')
				t.push_back(c);
		if (t.empty())
			return false;
		const size_t slash = t.find('/');
		auto number = [](const std::string& s, double& value) {
			if (s.empty()) return false;
			char* end = nullptr;
			value = std::strtod(s.c_str(), &end);
			return end && *end == '\0' && std::isfinite(value);
		};
		double value = 0.0;
		if (slash == std::string::npos)
		{
			if (!number(t, value)) return false;
		}
		else
		{
			double n = 0.0, d = 0.0;
			if (!number(t.substr(0, slash), n) || !number(t.substr(slash + 1), d) || !(d > 0.0)) return false;
			value = n / d;
		}
		if (!(value > 0.0)) return false;
		hz = value;
		return true;
	}

	inline bool IsInteger(double key) { return std::fabs(key - std::round(key)) < 1e-9; }
	inline bool Exact(double key, double hz) { return key > 0.0 && hz > 0.0 && std::fabs(hz - key) <= key * kExactTolerance; }

	// The index of the key for `hz` (-1 = none). general: exact, then an integer key's band, then
	// the nearest within 0.5 Hz. Legacy (general_rate_fixes off): the truncated integer key, then the
	// nearest within 0.5 Hz (the [ppm_correction] / override rule).
	inline int Find(const std::vector<double>& keys, double hz, bool general = true)
	{
		if (!std::isfinite(hz) || hz <= 0.0)
			return -1;
		int found = -1;
		if (general)
		{
			double best = 0.0;
			for (size_t i = 0; i < keys.size(); ++i)
				if (Exact(keys[i], hz) && (found < 0 || std::fabs(hz - keys[i]) < best))
				{
					found = static_cast<int>(i);
					best = std::fabs(hz - keys[i]);
				}
			if (found >= 0)
				return found;
			const int band = LegacyKey(hz);
			for (size_t i = 0; i < keys.size(); ++i)
				if (IsInteger(keys[i]) && static_cast<int>(std::lround(keys[i])) == band)
					return static_cast<int>(i);
		}
		else
		{
			const int truncated = static_cast<int>(hz);
			for (size_t i = 0; i < keys.size(); ++i)
				if (IsInteger(keys[i]) && static_cast<int>(std::lround(keys[i])) == truncated)
					return static_cast<int>(i);
		}
		double best = kNearestToleranceHz;
		for (size_t i = 0; i < keys.size(); ++i)
		{
			const double distance = std::fabs(hz - keys[i]);
			if (distance <= kNearestToleranceHz && (found < 0 || distance < best))
			{
				found = static_cast<int>(i);
				best = distance;
			}
		}
		return found;
	}

	// The 1000/1001 family's bases (NTSC film and video; their whole multiples carry the family:
	// 48, 60, 72, 90, 96, 120, 144, 240 ...). Standard constants, not a list of supported rates.
	inline bool IsNtscFamilyWhole(long long whole)
	{
		return whole > 0 && (whole % 24 == 0 || whole % 30 == 0);
	}

	// The exact spelling of a 1000/1001 rate: 23.976, 29.97, 59.94, 119.88, 143.856, 239.76.
	inline std::string RateText(double hz)
	{
		char text[32];
		snprintf(text, sizeof(text), "%.3f", hz);
		std::string s(text);
		while (!s.empty() && s.back() == '0') s.pop_back();
		if (!s.empty() && s.back() == '.') s.pop_back();
		return s;
	}

	// The migration of one saved key: an integer K (written in the band convention) whose band holds
	// the 1000/1001 rate of K + 1 -> that rate's exact spelling. False: the key stays (an integer
	// rate such as 24, 25, 50, 60, 100, 120, a decimal or ratio already, or not a rate).
	inline bool MigratedKey(const std::string& key, std::string& migrated)
	{
		double hz = 0.0;
		if (!Parse(key, hz) || !IsInteger(hz) || key.find_first_of("./") != std::string::npos)
			return false;
		const long long k = std::llround(hz);
		if (k <= 0 || k >= 1000 || !IsNtscFamilyWhole(k + 1))
			return false;
		migrated = RateText(static_cast<double>(k + 1) * 1000.0 / 1001.0);
		return true;
	}

	// The sections keyed by refresh rate.
	inline bool IsKeyedSection(const std::string& normalizedSection)
	{
		return normalizedSection == "ppm_correction" || normalizedSection == "display_refresh_rate_override";
	}

	// The renames for one keyed section's settings (old key -> new key), in the section's order. A
	// key whose migrated spelling is already present is dropped (the explicit entry wins).
	struct Rename { std::string from, to; bool drop = false; };
	inline std::vector<Rename> PlanSection(const std::vector<std::string>& keys)
	{
		std::vector<Rename> plan;
		for (const auto& key : keys)
		{
			std::string migrated;
			if (!MigratedKey(key, migrated))
				continue;
			bool present = false;
			for (const auto& other : keys)
				if (other == migrated)
					present = true;
			plan.push_back({ key, migrated, present });
		}
		return plan;
	}

	// In memory, at load (ConfigFile): the keyed sections' saved band keys read as their exact rates.
	// Returns the number of keys rewritten. No disk writes here.
	inline size_t Apply(std::map<std::string, std::map<std::string, std::string>>& sections)
	{
		size_t changed = 0;
		for (auto& section : sections)
		{
			if (!IsKeyedSection(section.first))
				continue;
			std::vector<std::string> keys;
			for (const auto& setting : section.second)
				keys.push_back(setting.first);
			for (const auto& rename : PlanSection(keys))
			{
				const auto found = section.second.find(rename.from);
				if (found == section.second.end())
					continue;
				if (!rename.drop)
					section.second[rename.to] = found->second;
				section.second.erase(rename.from);
				++changed;
			}
		}
		return changed;
	}
}
