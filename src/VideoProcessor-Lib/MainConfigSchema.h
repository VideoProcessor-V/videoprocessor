#pragma once

#include "ConfigSchema.h"
#include "QueueConfiguration.h"
#include "MainSchemaHooks.h" // VPV-HOOK

#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <vector>

// Startup-only schema for VideoProcessor.cfg.  It shares the strict validation
// engine with renderer configuration without making application settings
// dynamically selectable by renderer profile rules.
namespace MainConfigSchema
{
	inline bool IsRendererAliasName(const std::string& name)
	{
		if (name.empty() || name.size() > 64 || name == "vprenderer" ||
			!std::isalpha(static_cast<unsigned char>(name.front())))
			return false;
		return std::all_of(name.begin() + 1, name.end(), [](unsigned char c)
			{ return std::isalnum(c) || c == '_' || c == '-'; });
	}

	inline bool OwnsSection(const std::string& section)
	{
		return section == "command_line" ||
			section == "general" ||
			section == "renderer_alias" ||
			section == "queue" ||
			section.rfind("queue.", 0) == 0 ||
			section == "directshow" ||
			section == "vprenderer.input_processing" ||
			section == "directshow.conversion" ||
			section == "directshow.ppm" ||
			section == "queue_recovery" ||
			section == "lldv" ||
			section.rfind("lldv.", 0) == 0 ||
			section == "logging" ||
			section == "decklink" ||
			section == "shortcuts" ||
			section == "p010_conversion" ||
			section == "ppm_correction" ||
			section == "display_refresh_rate_override" ||
			section == "shaders" ||
			section.rfind("shaders.", 0) == 0 ||
			section.rfind("shader.", 0) == 0 ||
			SchemaHooks::OwnsSection(section);   // VPV-HOOK
	}

	inline bool BoundedInvalidCaptureRecoveryEnabled(const ConfigFile& config)
	{
		bool enabled = true;
		config.TryGetBool("general", "bounded_invalid_capture_recovery", enabled);
		return enabled;
	}

	// The [command_line] / [general] application keys (also the Config coverage test's
	// list: every key here has a Config control or is on its allow-list).
	inline std::vector<ConfigSchema::KeyRule> GeneralRules()
	{
		std::vector<ConfigSchema::KeyRule> rules = {
			ConfigSchema::Boolean("fullscreen"),
			ConfigSchema::Boolean("windowedfullscreenmode"),
			ConfigSchema::Boolean("windowed_fullscreen_mode"),
			ConfigSchema::Any("fullscreen_monitor_name"),
			ConfigSchema::Choice("fullscreen_monitor_session_mode",
				{ "existing", "target-only" }),
			ConfigSchema::Any("renderer"),
			ConfigSchema::Any("hide_legacy_renderers"),
			ConfigSchema::Integer("queue_size", 1, INT_MAX),
			ConfigSchema::Any("capture_device"),
			{
				"frame_offset",
				[](const std::string& value)
				{
					if (ConfigFile::NormalizeName(value) == "auto") return true;
					return ConfigSchema::Integer("value", 0, INT_MAX).validator(value);
				},
				"AUTO or a non-negative integer"
			},
			ConfigSchema::Any("video_conversion"),
			ConfigSchema::Any("container_colorspace"),
			ConfigSchema::Any("hdr_colorspace"),
			ConfigSchema::Any("hdr_luminance"),
			ConfigSchema::Any("renderer_start_stop_time_method"),
			ConfigSchema::Any("renderer_nominal_range"),
			ConfigSchema::Any("renderer_transfer_function"),
			ConfigSchema::Any("renderer_transfer_matrix"),
			ConfigSchema::Any("renderer_primaries"),
			ConfigSchema::Boolean("scene_detect"),
			ConfigSchema::Boolean("scene"),
			ConfigSchema::Boolean("disable_detection_features"),
			ConfigSchema::Any("scene_correction_mode"),
			ConfigSchema::Boolean("scene_correction_basic"),
			ConfigSchema::Choice("subtitle_reposition",
				{ "true", "false", "basic", "advanced", "on", "off", "1", "0" }),
			ConfigSchema::Boolean("dolby_vision_tunnel"),
			ConfigSchema::Boolean("newlldv"),
			ConfigSchema::Boolean("new_lldv"),
			// Interface resolution deliberately validates this value non-fatally:
			// malformed persisted values log and fall back to Classic rather than
			// making capture startup fail.
			ConfigSchema::Any("interface"),
			ConfigSchema::Boolean("noui"),
			ConfigSchema::Boolean("no_ui"),
			ConfigSchema::Boolean("startminimized"),
			ConfigSchema::Boolean("start_minimized"),
			ConfigSchema::Any("capture_input"),
			ConfigSchema::Choice("switch_refresh_rate",
				{ "true", "false", "never", "fullscreen_only", "full_screen_only", "always" }),
			ConfigSchema::Boolean("persist_profile_selection"),
			ConfigSchema::Integer("profile_change_display_seconds", 0, 60),
			ConfigSchema::Boolean("scale_osd"),
            ConfigSchema::Number("stats_osd_size_percent", 37.5, 375.0),
            // Append startup fields: backend input rules below reuse fixed indices.
            ConfigSchema::Boolean("present_every_vsync"),
            ConfigSchema::Integer("presentation_queue_depth", 1, 8)
		};
		SchemaHooks::AppendRules("general", rules);   // VPV-HOOK: VP-V keys, appended after the positional list
		return rules;
	}

	// The rule lists below are exposed (hunt 2, 2026-10-17) so the Config coverage
	// test checks every validated key; Validate uses the same lists.
	inline ConfigSchema::KeyRule BoundedInvalidCaptureRecoveryRule()
	{
		return ConfigSchema::Boolean("bounded_invalid_capture_recovery");
	}

	// [queue] / [queue.<name>]: whole frames, capped; old names still accepted.
	inline std::vector<ConfigSchema::KeyRule> QueueRules()
	{
		std::vector<ConfigSchema::KeyRule> rules = {
			ConfigSchema::Any("when"),
			ConfigSchema::Any("shortcut"),
			ConfigSchema::Any("cycle_shortcut"),
			ConfigSchema::Integer("queue_size", 1, INT_MAX),
			ConfigSchema::Integer("lead_frames", 0, 16),
			ConfigSchema::Integer("target_frames", 0, 16),
			ConfigSchema::Integer("active_picture_lookahead_frames", 0, 8),
			ConfigSchema::Integer("startup_preroll_frames", 0, 16),
			ConfigSchema::Integer("steady_reserve_frames", 0, 16),
			ConfigSchema::Integer("reset_after_render_restart_seconds", 1, INT_MAX),
			ConfigSchema::Integer("reset_queue_too_large_percent", 1, 200)
		};
		SchemaHooks::AppendRules("queue", rules);   // VPV-HOOK
		return rules;
	}

	// [directshow]: presentation lead plus the input-policy overrides of [general].
	inline std::vector<ConfigSchema::KeyRule> DirectShowRules()
	{
		const std::vector<ConfigSchema::KeyRule> general = GeneralRules();
		return {
			ConfigSchema::Integer("presentation_lead_frames", 0, 16),
			general[9],  // frame_offset
			general[10], // video_conversion
			general[11], // container_colorspace
			general[12], // hdr_colorspace
			general[13], // hdr_luminance
			general[14], // renderer_start_stop_time_method
			general[15], // renderer_nominal_range
			general[16], // renderer_transfer_function
			general[17], // renderer_transfer_matrix
			general[18]  // renderer_primaries
		};
	}

	// [vprenderer.input_processing]: the VP renderer's input-policy overrides.
	inline std::vector<ConfigSchema::KeyRule> VpRendererInputRules()
	{
		const std::vector<ConfigSchema::KeyRule> general = GeneralRules();
		return {
			general[10], // video_conversion
			general[11], // container_colorspace
			general[12], // hdr_colorspace
			general[13]  // hdr_luminance
		};
	}

	// [shortcuts] Boolean switches (the shortcut bindings themselves are free-form).
	inline std::vector<ConfigSchema::KeyRule> ShortcutSwitchRules()
	{
		std::vector<ConfigSchema::KeyRule> rules = { ConfigSchema::Boolean("foreground_only") };
		SchemaHooks::AppendRules("shortcuts", rules);   // VPV-HOOK
		return rules;
	}

	// [decklink] capture packing.
	inline std::vector<ConfigSchema::KeyRule> DeckLinkRules()
	{
		std::vector<ConfigSchema::KeyRule> rules = {
			ConfigSchema::Choice("rgb_8bit_packing", { "AUTO", "ARGB", "BGRA" }),
			ConfigSchema::Choice("rgb_10bit_packing", { "AUTO", "R210", "R10B", "R10L" }),
			ConfigSchema::Choice("rgb_12bit_packing", { "AUTO", "R12B", "R12L" })
		};
		SchemaHooks::AppendRules("decklink", rules);   // VPV-HOOK
		return rules;
	}

	// [directshow.conversion]: the P010 CPU conversion.
	inline std::vector<ConfigSchema::KeyRule> ConversionRules()
	{
		return {
			ConfigSchema::Choice("conversion_method",
				{ "auto", "simd", "optimized", "standard" }),
			ConfigSchema::Integer("min_core_count", 1, INT_MAX),
			ConfigSchema::Integer("max_core_count", 1, INT_MAX),
			ConfigSchema::Choice("chroma_downsampling", { "AVERAGE", "LEGACY", "ADVANCED" })
		};
	}

	// [directshow.ppm]: clock PPM correction.
	inline std::vector<ConfigSchema::KeyRule> PpmRules()
	{
		return {
			{
				"ppm",
				[](const std::string& value)
				{
					const std::string normalized = ConfigFile::NormalizeName(value);
					if (normalized == "auto") return true;
					try
					{
						size_t consumed = 0;
						const long long parsed = std::stoll(
							ConfigFile::Trim(value), &consumed);
						return consumed == ConfigFile::Trim(value).size() &&
							parsed >= -1000000 && parsed <= 1000000;
					}
					catch (const std::exception&) { return false; }
				},
				"AUTO or an integer from -1000000 to 1000000"
			}
		};
	}

	// [queue_recovery]: legacy location of the queue reset keys (Config moves them to [queue.*]).
	inline std::vector<ConfigSchema::KeyRule> RecoveryRules()
	{
		return {
			ConfigSchema::Integer("reset_after_render_restart_seconds", 1, INT_MAX),
			ConfigSchema::Integer("reset_queue_too_large_percent", 1, 200)
		};
	}

	// [lldv] / [lldv.<name>].
	inline std::vector<ConfigSchema::KeyRule> LldvRules()
	{
		return {
			ConfigSchema::Boolean("dolby_vision_tunnel"),
			ConfigSchema::Any("when"),
			ConfigSchema::Any("shortcut"),
			ConfigSchema::Any("cycle_shortcut"),
			ConfigSchema::NumberAtLeast("max_cll", 0.0),
			ConfigSchema::NumberAtLeast("max_fall", 0.0),
			ConfigSchema::NumberAtLeast("mastering_min_luminance", 0.0),
			ConfigSchema::NumberAtLeast("mastering_max_luminance", 0.0, false)
		};
	}

	// [logging]: retention defaults safely; resolution validates 1-200 before logger startup.
	inline std::vector<ConfigSchema::KeyRule> LoggingRules()
	{
		std::vector<ConfigSchema::KeyRule> rules = {
			ConfigSchema::Boolean("enabled"),
			ConfigSchema::Boolean("debug"),
			ConfigSchema::Any("debug_log_retention")
		};
		SchemaHooks::AppendRules("logging", rules);   // VPV-HOOK
		return rules;
	}

	inline bool Validate(const ConfigFile& config, std::string& error)
	{
		error.clear();
		const auto recoveryRule = BoundedInvalidCaptureRecoveryRule();
		std::string recoveryValue;
		if (config.TryGetString("general", recoveryRule.key, recoveryValue) &&
			!recoveryRule.validator(recoveryValue))
		{
			error = "[general] key 'bounded_invalid_capture_recovery' must be a Boolean";
			return false;
		}
		const std::vector<ConfigSchema::KeyRule> commandLineRules = GeneralRules();
		if (!ConfigSchema::ValidateSection(
			config, "command_line", commandLineRules, error))
			return false;
		// [general] was renderer-profile state in the old layout. It becomes
		// application startup configuration only when the VP-0079 renderer root
		// is present, so retain validation compatibility for old profile files.
		bool hasUnifiedRenderer = config.HasSection("vprenderer");
		if (!hasUnifiedRenderer)
			for (const std::string& section : config.GetSectionNames())
				if (section.rfind("vprenderer.", 0) == 0)
				{
					hasUnifiedRenderer = true;
					break;
				}
		auto generalRules = commandLineRules;
		generalRules.push_back(recoveryRule);
		if (hasUnifiedRenderer &&
			!ConfigSchema::ValidateSection(
				config, "general", generalRules, error))
			return false;

		// Queue policy is deliberately expressed in whole frames and capped so
		// a config typo cannot create an impractically deep live queue. The old
		// names remain accepted for configuration-file compatibility.
		const std::vector<ConfigSchema::KeyRule> queueRules = QueueRules();
		std::string defaultQueueSection;
		const bool hasDefaultQueue = QueueConfiguration::ResolveDefaultSection(
			config, defaultQueueSection);
		for (const std::string& section : config.GetSectionNames())
			if (section == "queue" ||
				QueueConfiguration::IsDirectNamedQueueSection(section))
			{
				if (!ConfigSchema::ValidateSection(config, section, queueRules, error))
					return false;
			}

		const std::vector<ConfigSchema::KeyRule> directShowRules = DirectShowRules();
		if (!ConfigSchema::ValidateSection(
			config, "directshow", directShowRules, error))
			return false;
		const std::vector<ConfigSchema::KeyRule> vpRendererInputRules = VpRendererInputRules();
		if (!ConfigSchema::ValidateSection(
			config, "vprenderer.input_processing", vpRendererInputRules, error))
			return false;
		// [general] is the shared input-policy default. [directshow] and
		// [vprenderer.input_processing] may independently override any of its fields, so overlap
		// is deliberate and resolved by the runtime rather than rejected here.

		const std::vector<ConfigSchema::KeyRule> deckLinkRules = DeckLinkRules();
		if (!ConfigSchema::ValidateSection(
			config, "decklink", deckLinkRules, error))
			return false;
		std::string foregroundOnly;
		if (config.TryGetString("shortcuts", "foreground_only", foregroundOnly) &&
			!ConfigSchema::Boolean("foreground_only").validator(foregroundOnly))
		{
			error = "[shortcuts] key 'foreground_only' must be a Boolean";
			return false;
		}
		const std::vector<ConfigSchema::KeyRule> conversionRules = ConversionRules();
		if (!ConfigSchema::ValidateSection(
			config, "directshow.conversion", conversionRules, error))
			return false;
		const std::vector<ConfigSchema::KeyRule> ppmRules = PpmRules();
		if (!ConfigSchema::ValidateSection(
			config, "directshow.ppm", ppmRules, error))
			return false;
		if (!SchemaHooks::Validate(config, error))   // VPV-HOOK: the VP-V sections and keys
			return false;
		if (const auto* aliases = config.GetSectionValues("renderer_alias"))
			for (const auto& alias : *aliases)
			{
				if (!IsRendererAliasName(alias.first))
				{
					error = "[renderer_alias] alias '" + alias.first +
						"' must be an identifier other than vprenderer";
					return false;
				}
				if (!ConfigSchema::Integer("alias", 1, INT_MAX).validator(
					alias.second))
				{
					error = "[renderer_alias] alias '" + alias.first +
						"' must be a positive renderer-selector index";
					return false;
				}
			}

		std::string value;
		std::string legacyValue;
		for (const std::string& section : config.GetSectionNames())
			if ((section == "queue" ||
				QueueConfiguration::IsDirectNamedQueueSection(section)) &&
				config.TryGetString(section, "target_frames", value) &&
				config.TryGetString(section, "steady_reserve_frames", legacyValue))
			{
				error = "cannot specify both [" + section +
					"] target_frames and legacy steady_reserve_frames";
				return false;
			}
		if (hasDefaultQueue && config.TryGetString(
			defaultQueueSection, "lead_frames", value) &&
			config.TryGetString(
				"directshow", "presentation_lead_frames", legacyValue))
		{
			error = "cannot specify both [" + defaultQueueSection +
				"] lead_frames and legacy "
				"[directshow] presentation_lead_frames";
			return false;
		}

		const std::vector<ConfigSchema::KeyRule> recoveryRules = RecoveryRules();
		if (!ConfigSchema::ValidateSection(
			config, "queue_recovery", recoveryRules, error))
			return false;
		for (const char* key : { "reset_after_render_restart_seconds",
			"reset_queue_too_large_percent" })
		{
			std::string canonicalValue;
			std::string legacyValue;
			if (hasDefaultQueue && config.TryGetString(
				defaultQueueSection, key, canonicalValue) &&
				config.TryGetString("queue_recovery", key, legacyValue))
			{
				error = "cannot specify both [" + defaultQueueSection + "] " +
					key + " and legacy [queue_recovery] " + key;
				return false;
			}
		}

		const std::vector<ConfigSchema::KeyRule> lldvRules = LldvRules();
		for (const std::string& section : config.GetSectionNames())
			if (section == "lldv" ||
				(section.rfind("lldv.", 0) == 0 &&
				 section.size() > sizeof("lldv.") - 1 &&
				 section.find('.', sizeof("lldv.") - 1) == std::string::npos))
				if (!ConfigSchema::ValidateSection(config, section, lldvRules,
					error))
					return false;

		// Debug-log retention deliberately defaults safely instead of making an
		// otherwise usable configuration fatal. Resolution validates 1-200
		// before logger startup; the schema still rejects unknown logging keys.
		const std::vector<ConfigSchema::KeyRule> loggingRules = LoggingRules();
		return ConfigSchema::ValidateSection(
			config, "logging", loggingRules, error);
	}
}
