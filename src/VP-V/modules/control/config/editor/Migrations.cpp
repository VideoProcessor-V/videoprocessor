// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <ConfigEditorCore.h>
#include <ConfigFile.h>
#include <RendererProfileConfig.h>

#include <map>
#include <set>
#include <string>

namespace Vpv
{
	bool FoldLegacyInputPolicy(ConfigEditorCore::ConfigDocument& document)
	{
		// The input policy keys move from [vprenderer.input] / [vprenderer] to
		// [vprenderer.input_processing], and the emptied headers are removed. Run
		// by every load path (hunt 3, CFG7: a remote Discard skipped it).
		bool migratedLegacyInputPolicy = false;
		for (const char* inputKey : { "video_conversion", "container_colorspace",
			"hdr_colorspace", "hdr_luminance" })
		{
			const std::string vpRendererInput =
				document.Get("vprenderer.input_processing", inputKey);
			const std::string interimVpRendererInput =
				document.Get("vprenderer.input", inputKey);
			const std::string legacyVpRenderer =
				document.Get("vprenderer", inputKey);
			if (!interimVpRendererInput.empty())
			{
				if (vpRendererInput.empty())
				{
					document.AddSection("vprenderer.input_processing");
					document.SetKnown("vprenderer.input_processing", inputKey,
						interimVpRendererInput);
				}
				document.RemoveKnown("vprenderer.input", inputKey);
				migratedLegacyInputPolicy = true;
			}
			if (!legacyVpRenderer.empty())
			{
				if (vpRendererInput.empty() && interimVpRendererInput.empty())
				{
					document.AddSection("vprenderer.input_processing");
					document.SetKnown("vprenderer.input_processing", inputKey,
						legacyVpRenderer);
				}
				document.RemoveKnown("vprenderer", inputKey);
				migratedLegacyInputPolicy = true;
			}
		}
		// ADR-0023 global [vpv] keys stored in legacy [vprenderer] must migrate to [vpv]
		// BEFORE createProfilePage("vprenderer") renames [vprenderer] -> [vprenderer.profile_1].
		// Preserve [vpv]-over-[vprenderer] precedence and leave non-global keys untouched.
		std::map<std::string, std::string> initialVpvKeys;
		for (const auto& entry : document.SectionSettings("vpv"))
			initialVpvKeys[ConfigFile::NormalizeName(entry.first)] = entry.second;
		std::set<std::string> handledKeys;
		for (const auto& setting : document.SectionSettings("vprenderer"))
		{
			const std::string normalizedKey = ConfigFile::NormalizeName(setting.first);
			if (!RendererProfileConfig::ProfileSchemaHooks::IsKnownGlobalKey("vpv", normalizedKey))
				continue;
			if (!handledKeys.insert(normalizedKey).second)
				continue;
			const auto existingIt = initialVpvKeys.find(normalizedKey);
			const bool hasExplicitVpvKey = (existingIt != initialVpvKeys.end());
			if (!hasExplicitVpvKey)
			{
				// Last-wins across duplicate legacy [vprenderer] entries, matching ConfigFile::Load.
				std::string legacyValue;
				for (const auto& candidate : document.SectionSettings("vprenderer"))
					if (ConfigFile::NormalizeName(candidate.first) == normalizedKey)
						legacyValue = candidate.second;
				if (!legacyValue.empty())
				{
					document.AddSection("vpv");
					document.SetKnown("vpv", normalizedKey.c_str(), legacyValue);
				}
			}
			else if (existingIt->second.empty() &&
				!RendererProfileConfig::ValidateBaseSetting(normalizedKey, existingIt->second))
			{
				// An explicit empty [vpv] choice key already overrode the legacy [vprenderer]
				// value above; clear all empty invalid entries so Apply saves a schema-valid file.
				while (document.RemoveKnown("vpv", normalizedKey.c_str())) {}
			}
			while (document.RemoveKnown("vprenderer", normalizedKey.c_str())) {}
			migratedLegacyInputPolicy = true;
		}
		// Removing an empty header is a visible migration (hunt 2, L9): the
		// presence of [vprenderer] alone selects the [general] validation.
		for (const char* emptied : { "vprenderer.input", "vprenderer" })
			if (document.SectionSettings(emptied).empty() &&
				document.RemoveSection(emptied))
				migratedLegacyInputPolicy = true;
		return migratedLegacyInputPolicy;
	}
}
