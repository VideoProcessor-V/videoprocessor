// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#pragma detect_mismatch("vpv_schema_hooks", "2")   // 2: host-only pass (HostOnlyScope), WithVpKeys, WithExpressionVariables, AppendLegacyDisplayRules

// The VP-V keys, sections and constants of the startup schema (MainConfigSchema.h), moved out of the host
// header (M2 P3-7). MainSchemaHooks.h includes this header when the build defines
// MAIN_SCHEMA_EXTENSION_HEADER (Directory.Build.targets); a build without it gets inert hooks. It is a
// header, not a registry: the schema is used by the GUI, the Config editor, the renderer DLLs and the
// tests, and a compile-time seam gives every one of them the same rules with no start-up registration.
//
// What the hooks do, in the order the host calls them:
//   OwnsSection(section)     the sections the startup schema owns beyond the host's ([ambient], [led_drive...],
//                            [calibration...]),
//   OwnsRendererSection      [vpv] and [vpv.*], owned by the renderer profile reader,
//   AppendRules(list, rules) the VP-V keys of a rule list, appended to a COPY the host validates with
//                            (MainConfigSchema::WithVpKeys); the host's own list functions (GeneralRules,
//                            QueueRules, ...) hold the host's keys only, in the host's order,
//   Validate(config, error)  the VP-V sections and the [shortcuts] VP-V keys, called once after ALL of the
//                            host's checks,
//   ProfileFault             a slot RendererProfileConfig.h fills: the host's first profile fault of a file,
//   HostOnlyScope            while one exists on the thread the VP-V keys are accepted unchecked
//                            (AppendRules strips their validators), so a pass under it can report only the host's
//                            faults.
// Validation order (owner decision 2026-11-07, Bill-identical): MainConfigSchema::Validate runs the host's
// checks and then Validate() above; if anything fails it re-runs the host's checks under HostOnlyScope and
// reports the host's first fault when there is one. A file with several faults therefore reports the same
// first fault as the host's own build would for its host faults; a file with only VP-V faults reports the
// VP-V fault (the same text as before the move: src/VP-V/tests/golden/schema-validation.golden.txt).

#include "ConfigSchema.h"
#include "AmbientConfig.h"
#include "led/LedDriveConfig.h"
#include "calibration/CalibrationConfig.h"
#include "ShortcutDefaults.h"

#include <climits>
#include <string>
#include <utility>
#include <vector>

namespace MainConfigSchema
{
	// VP Renderer filter keys ([general]); defaults and ranges shared with
	// FileHost and the Config editor.
	static const int FileAvOffsetDefaultMs = 0;
	static const int FileAvOffsetMinMs = -2000;
	static const int FileAvOffsetMaxMs = 2000;
	static const int FileLedHoldoverDefaultMs = 3000;
	static const int FileLedHoldoverMinMs = 0;
	static const int FileLedHoldoverMaxMs = 30000;

	// Bug hunt 3 (SH6): every item of [shortcuts] global_actions names an
	// action of ShortcutDefaults.h. `unknown` receives the first that does not.
	inline bool ShortcutGlobalActionsValid(const std::string& list, std::string& unknown)
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
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return true;
	}

	namespace SchemaHooks
	{
		// True while a HostOnlyScope exists on this thread.
		inline bool& HostOnlyFlag()
		{
			static thread_local bool flag = false;
			return flag;
		}

		// Set by RendererProfileConfig.h once Read is defined: true with the host's first profile fault in `error`
		// (host-only pass). Lets Validate report a host profile fault before a VP-V startup-group fault on the
		// paths that run Validate and then Read (Config Save/Apply, legacy editor, dialog reload), as Bill's build does.
		// A function-local static, not an inline variable (the Lib project compiles as C++14).
		inline bool (*&ProfileFault())(const ConfigFile&, std::string&)
		{
			static bool (*slot)(const ConfigFile&, std::string&) = nullptr;
			return slot;
		}

		struct HostOnlyScope
		{
			HostOnlyScope() : previous_(HostOnlyFlag()) { HostOnlyFlag() = true; }
			~HostOnlyScope() { HostOnlyFlag() = previous_; }
			HostOnlyScope(const HostOnlyScope&) = delete;
			HostOnlyScope& operator=(const HostOnlyScope&) = delete;
		private:
			bool previous_;
		};

		inline bool OwnsSection(const std::string& section)
		{
			return section == "ambient" ||
				LedDrive::OwnsSection(section) ||
				Calibration::OwnsSection(section);
		}

		// The renderer profile reader's unknown-section check (RendererProfileConfig.h).
		inline bool OwnsRendererSection(const std::string& section)
		{
			// The VP-V section (ADR-0023): [vpv] / [vpv.*] hold VP-V's own namespaced keys.
			return section == "vpv" || section.rfind("vpv.", 0) == 0;
		}

		// The VP-V keys of one startup rule list ("general", "queue", "shortcuts", "decklink", "logging"),
		// with their validators (Config coverage and the schema goldens read them here).
		inline std::vector<ConfigSchema::KeyRule> VpRules(const char* list)
		{
			std::vector<ConfigSchema::KeyRule> rules;
			const std::string name = list;
			if (name == "general")
			{
				rules.push_back(ConfigSchema::Boolean("newlldv_sdr_veto"));
				rules.push_back(ConfigSchema::Boolean("dv_tunnel_handoff"));   // DV input fixes 2026-10-18
				rules.insert(rules.end(), {
					// Delta 6: saved startup state for what keys / window controls
					// change (settings first).
					ConfigSchema::Boolean("stats_overlay"),
					ConfigSchema::Boolean("use_frame_queue"),
					ConfigSchema::Boolean("queue_auto_reset"),
					// Bug hunt 2026-10-17: keep fullscreen intent and retry the
					// renderer after a failure (false = old behaviour).
					ConfigSchema::Boolean("renderer_failure_keep_fullscreen"),
					// Restore switch (bug hunt 2026-10-17, default on): Magewell
					// recoverable faults restart the device internally (false = end
					// capture, as before).
					ConfigSchema::Boolean("magewell_recover_faults"),
					ConfigSchema::Boolean("capture_display_fixes"),   // restore switch, post-merge audit 2026-10-17
					// VP Renderer DirectShow filter (2026-10-17), read by its FileHost only:
					// audio/video offset for file playback (positive = picture later) and how
					// long the filter process keeps the LED drive between graphs (0 = stop at
					// once). Appended after the host's positional list (DirectShowRules indexes it).
					ConfigSchema::Integer("file_av_offset_ms", FileAvOffsetMinMs, FileAvOffsetMaxMs),
					ConfigSchema::Integer("file_led_holdover_ms", FileLedHoldoverMinMs, FileLedHoldoverMaxMs)
				});
			}
			else if (name == "queue")
			{
				rules.push_back(ConfigSchema::Integer("target_latency_ms", 0, 1000));
				// Owner 2026-10-16: VP's low-latency path as a queue property.
				rules.push_back(ConfigSchema::Choice("low_latency_path", { "on", "off" }));
			}
			else if (name == "shortcuts")
			{
				rules.push_back(ConfigSchema::Boolean("global_hotkeys"));
			}
			else if (name == "decklink")
			{
				rules.push_back(ConfigSchema::Choice("ycbcr_12bit_capture", { "rgb12", "v210", "off" }));
			}
			else if (name == "logging")
			{
				// Report-a-problem (StateReportSettings.h), both default off.
				rules.push_back(ConfigSchema::Boolean("auto_report"));
				rules.push_back(ConfigSchema::Boolean("report_thumbnail"));
			}
			return rules;
		}

		// Appends the VP-V keys of `list` to `rules` (the host's keys stay first and unchanged). Under a
		// HostOnlyScope the keys are accepted without a value check.
		inline void AppendRules(const char* list, std::vector<ConfigSchema::KeyRule>& rules)
		{
			for (ConfigSchema::KeyRule& rule : VpRules(list))
			{
				if (HostOnlyFlag()) rule.validator = nullptr;
				rules.push_back(std::move(rule));
			}
		}

		inline bool Validate(const ConfigFile& config, std::string& error)
		{
			// Stage 9: room lights.
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
			// Bug hunt 3 (SH6): the actions that act while another application
			// has focus - a comma-separated list of [shortcuts] action keys.
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
			// Automatic calibration: the [calibration] section.
			if (!Calibration::ValidateConfig(config, error))
				return false;
			return true;
		}
	}
}
