// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <ConfigurationApplyPolicy.h>
#include <RendererProfileConfig.h>
#include <MainConfigSchema.h>
#include <AmbientConfig.h>
#include <led/LedDriveConfig.h>
#include <calibration/CalibrationConfig.h>
#include <ShortcutDefaults.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Schema
	{
		bool ShortcutGlobalActionsValid(const std::string& list, std::string& unknown)
		{
			size_t start = 0;
			while (start <= list.size())
			{
				const size_t end = list.find(',', start);
				const std::string item = ConfigFile::NormalizeName(ConfigFile::Trim(
					list.substr(start, end == std::string::npos ? std::string::npos : end - start)));
				if (!item.empty() && !FindShortcutDefault(item.c_str()))
				{
					unknown = item;
					return false;
				}
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return true;
		}

		bool ValidateExtraSections(const ConfigFile& config, std::string& error)
		{
			if (const auto* camValues = config.GetSectionValues("vpv.camera"))
			{
				static const char* const kCamBools[] = {
					"camera_model", "camera_labels_gpu",
					"camera_meas_model_global", "camera_meas_guided_candidate", "camera_meas_narrow_window",
					"camera_meas_e1_skip", "camera_meas_static_gate", "camera_meas_cut_skip",
					"camera_meas_fade_normalised", "camera_meas_flash_bridge", "camera_meas_temporal_seed",
					"camera_meas_tiny_mover_roi",
					"camera_use_grain", "camera_use_detail", "camera_use_dither",
					"camera_use_deband", "camera_use_filler", "camera_use_dejudder",
					"camera_ahead_fit"
				};
				for (const auto& value : *camValues)
				{
					const std::string norm = ConfigFile::NormalizeName(value.second);
					if (value.first == "camera_model_overlay")
					{
						if (norm != "off" && norm != "labels" && norm != "residual" && norm != "vectors")
						{
							error = "[vpv.camera] invalid camera_model_overlay '" + value.second + "'";
							return false;
						}
						continue;
					}
					bool isBoolKey = false;
					for (const char* k : kCamBools)
						isBoolKey = isBoolKey || value.first == k;
					if (!isBoolKey)
					{
						error = "[vpv.camera] unknown key '" + value.first + "'";
						return false;
					}
					if (norm != "on" && norm != "off")
					{
						error = "[vpv.camera] invalid " + value.first + " '" + value.second + "'";
						return false;
					}
				}
			}
			if (const auto* skelValues = config.GetSectionValues("vpv.skeleton"))
			{
				auto parseIntRange = [](const std::string& raw, int lo, int hi) -> bool {
					const std::string t = ConfigFile::NormalizeName(raw);
					if (t.empty())
						return false;
					char* end = nullptr;
					const long v = std::strtol(t.c_str(), &end, 10);
					return end != t.c_str() && *end == '\0' && v >= lo && v <= hi;
				};
				for (const auto& value : *skelValues)
				{
					const std::string& k = value.first;
					const std::string norm = ConfigFile::NormalizeName(value.second);
					if (k == "analysis_lane")
					{
						if (norm != "off" && norm != "shadow" && norm != "on")
						{
							error = "[vpv.skeleton] invalid analysis_lane '" + value.second + "'";
							return false;
						}
					}
					else if (k == "analysis_lane_chunk_us" || k == "analysis_lane_test_wait_ms")
					{
						if (!parseIntRange(value.second, 0, 4000000))
						{
							error = "[vpv.skeleton] invalid " + k + " '" + value.second + "'";
							return false;
						}
					}
					else if (k == "analysis_lane_deterministic")
					{
						if (norm != "auto" && norm != "on" && norm != "off")
						{
							error = "[vpv.skeleton] invalid analysis_lane_deterministic '" + value.second + "'";
							return false;
						}
					}
					else if (k == "skeleton_gpu_gap")
					{
						if (norm != "off" && norm != "after_render")
						{
							error = "[vpv.skeleton] invalid skeleton_gpu_gap '" + value.second + "'";
							return false;
						}
					}
					else if (k == "grain_split_storage")
					{
						if (norm != "full" && norm != "compact")
						{
							error = "[vpv.skeleton] invalid grain_split_storage '" + value.second + "'";
							return false;
						}
					}
					else if (k == "lane_tap_source")
					{
						if (norm != "auto" && norm != "cpu")
						{
							error = "[vpv.skeleton] invalid lane_tap_source '" + value.second + "'";
							return false;
						}
					}
					else if (k == "camera_link")
					{
						if (norm != "off" && norm != "report")
						{
							error = "[vpv.skeleton] invalid camera_link '" + value.second + "'";
							return false;
						}
					}
					else
					{
						error = "[vpv.skeleton] unknown key '" + k + "'";
						return false;
					}
				}
			}
			return true;
		}

		bool ValidateStartupSections(const ConfigFile& config, std::string& error, bool validateVpv)
		{
			if (!ConfigSchema::ValidateSection(config, "ambient", Ambient::SchemaRules(), error) ||
				Ambient::PortClashesWithProjector(config, error))
				return false;
			std::string globalHotkeys;
			if (config.TryGetString("shortcuts", "global_hotkeys", globalHotkeys) &&
				!ConfigSchema::Boolean("global_hotkeys").validator(globalHotkeys))
			{
				error = "[shortcuts] key 'global_hotkeys' must be a Boolean";
				return false;
			}
			std::string globalActions;
			if (config.TryGetString("shortcuts", "global_actions", globalActions))
			{
				std::string unknown;
				if (!ShortcutGlobalActionsValid(globalActions, unknown))
				{
					error = "[shortcuts] global_actions: '" + unknown +
						"' is not a shortcut action (use keys such as config_editor, toggle_noui)";
					return false;
				}
			}
			if (!LedDrive::Validate(config, error))
				return false;
			if (!Calibration::ValidateConfig(config, error))
				return false;
			if (validateVpv && !RendererProfileConfig::ProfileSchemaHooks::ValidateSections(config, error))
				return false;
			return true;
		}
	}

	namespace ApplyPolicy
	{
		namespace
		{
			bool IsVpvLiveSection(const std::string& normalized)
			{
				return normalized == "vpv" || normalized == "vpv.camera" || normalized == "vpv.skeleton" ||
					normalized == "ambient" ||
					normalized == "led_drive" || ConfigurationApplyPolicy::HasPrefix(normalized, "led_drive") ||
					normalized == "dynamic_drive" || ConfigurationApplyPolicy::HasPrefix(normalized, "dynamic_drive") ||
					normalized == "projector" || ConfigurationApplyPolicy::HasPrefix(normalized, "projector");
			}

			bool IsVpvRestartKey(const std::string& section, const std::string& key)
			{
				if (section != "vpv")
					return false;
				return key == "pipeline_original_pathway" || key == "pipeline_order" ||
					key == "pipeline_entrance_once" || key == "output_segments_merged" ||
					key == "frame_analysis_store" || key == "resync_event" ||
					key == "entrance_gpu_drain";
			}

			ConfigurationApplyPolicy::Action ClassifyOne(const ConfigurationApplyPolicy::Change& change,
				bool directShowActive)
			{
				const std::string section = ConfigurationApplyPolicy::NormalizeSection(change.section);
				const std::string key = ConfigurationApplyPolicy::NormalizeSection(change.key);
				if (IsVpvRestartKey(section, key))
					return ConfigurationApplyPolicy::Action::RestartRenderer;
				if (IsVpvLiveSection(section))
					return ConfigurationApplyPolicy::Action::ApplyProfiles;
				return ConfigurationApplyPolicy::ClassifyChange(change, directShowActive);
			}
		}

		ConfigurationApplyPolicy::Action ClassifyChanges(
			const std::vector<ConfigurationApplyPolicy::Change>& changes, bool directShowActive)
		{
			ConfigurationApplyPolicy::Action action = ConfigurationApplyPolicy::Action::SaveOnly;
			for (const ConfigurationApplyPolicy::Change& change : changes)
				action = (std::max)(action, ClassifyOne(change, directShowActive));
			return action;
		}

		const char* EffectCategory(const std::string& normalized)
		{
			if (normalized == "vpv" || ConfigurationApplyPolicy::HasPrefix(normalized, "vpv"))
				return "VP-V";
			if (normalized == "led_drive" || ConfigurationApplyPolicy::HasPrefix(normalized, "led_drive") ||
				normalized == "dynamic_drive" || ConfigurationApplyPolicy::HasPrefix(normalized, "dynamic_drive") ||
				normalized == "projector" || ConfigurationApplyPolicy::HasPrefix(normalized, "projector"))
				return "Dynamic Drive";
			if (normalized == "ambient")
				return "Ambient";
			return nullptr;
		}
	}
}
