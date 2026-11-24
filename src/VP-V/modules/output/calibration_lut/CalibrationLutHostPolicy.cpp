// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "CalibrationLutHostPolicy.h"

namespace CalibrationLutHost
{
	std::string ResolveStage(const std::string& configured, bool dmStepsRun)
	{
		if (configured == "auto")
			return dmStepsRun ? "vp" : "libplacebo";
		return configured == "vp" ? "vp" : "libplacebo";
	}

	bool ContainsSlotToken(const std::string& slotsOff, const char* targetLower)
	{
		if (!targetLower || !*targetLower)
			return false;
		const auto isSep = [](char c) { return c == ',' || c == ' ' || c == ';' || c == '\t'; };
		size_t pos = 0;
		const size_t n = slotsOff.size();
		while (pos < n)
		{
			while (pos < n && isSep(slotsOff[pos]))
				++pos;
			const size_t start = pos;
			while (pos < n && !isSep(slotsOff[pos]))
				++pos;
			if (pos == start)
				continue;
			size_t i = start;
			const char* t = targetLower;
			bool match = true;
			while (i < pos && *t)
			{
				char c = slotsOff[i];
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
				if (c != *t)
				{
					match = false;
					break;
				}
				++i;
				++t;
			}
			if (match && i == pos && *t == '\0')
				return true;
		}
		return false;
	}

	bool DisplayMappingStepsRun(bool originalPathway, const std::string& dvDisplayMapping, bool dvTrims,
		bool appearance, const std::string& slotsOff)
	{
		if (originalPathway)
			return false;
		const bool trimsRun = dvDisplayMapping != "original" && dvTrims &&
			!ContainsSlotToken(slotsOff, "colour.dm_trims");
		const bool appearanceRuns = appearance && !ContainsSlotToken(slotsOff, "colour.appearance");
		return trimsRun || appearanceRuns;
	}

	bool VpStageStatus(std::string& status, const std::string& fileName)
	{
		if (StartsWith(status, kVpReloadRejectedPrefix))
			return false;
		if (StartsWith(status, kReloadRejectedPrefix))
		{
			status = kVpReloadRejectedPrefix + status.substr(std::char_traits<char>::length(kReloadRejectedPrefix));
			return true;
		}
		const std::string::size_type prefix = std::char_traits<char>::length(kVpStagePrefix);
		if (status.size() == prefix + fileName.size() && StartsWith(status, kVpStagePrefix) &&
			status.compare(prefix, std::string::npos, fileName) == 0)
			return false;
		status = kVpStagePrefix + fileName;
		return true;
	}

	bool LibplaceboStageStatus(std::string& status)
	{
		if (!StartsWith(status, kVpReloadRejectedPrefix))
			return false;
		status = kReloadRejectedPrefix + status.substr(std::char_traits<char>::length(kVpReloadRejectedPrefix));
		return true;
	}

	FallbackCatalog::Decision SlopeFallbackDecision(unsigned long long slopeFallbacks)
	{
		return slopeFallbacks > 0
			? FallbackCatalog::Engaged("calibration LUT BTB/WTW",
				"BTB/WTW carried at slope 1 (edge-slope extrapolation unavailable for this LUT)")
			: FallbackCatalog::Released("calibration LUT BTB/WTW");
	}
}
