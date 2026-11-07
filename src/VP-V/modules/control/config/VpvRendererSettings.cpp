// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "VpvRendererSettings.h"

#include <ConfigFile.h>
#include <DebugLog.h>
#include <DisplayRuleExpression.h>
#include <IntentDescriptor.h>
#include <MotionAhead.h>
#include <vprenderer/DejudderWarp.h>
#include <RendererConfigView.h>
#include <RetiredSettings.h>
#include <VP-V/core/SlotSwitchSettings.h>
#include <VP-V/modules/output/presentation/PresentationOwner.h>
#include <vprenderer/DetailPulse.h>
#include <vprenderer/EdgeBlanking.h>
#include <vprenderer/InterpolationPolicy.h>
#include <vprenderer/SourceRebase.h>
#include <vprenderer/TextureRestore.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	// Copies of the host's ParseDouble / ParseInteger (LibplaceboVideoRenderer.cpp), same behaviour.
	bool ParseDouble(const std::string& value, double& parsed)
	{
		try
		{
			size_t consumed = 0;
			parsed = std::stod(ConfigFile::Trim(value), &consumed);
			return consumed == ConfigFile::Trim(value).size() && std::isfinite(parsed);
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	bool ParseInteger(const std::string& value, int minimum, int maximum, int& parsed)
	{
		try
		{
			const std::string trimmed = ConfigFile::Trim(value);
			size_t consumed = 0;
			const int candidate = std::stoi(trimmed, &consumed);
			if (consumed != trimmed.size() || candidate < minimum || candidate > maximum)
				return false;
			parsed = candidate;
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	// Copies of the host's TryGetDisplayString / ReadChoice (LibplaceboVideoRenderer.cpp), same behaviour and log text.
	bool TryGetDisplayString(const ConfigFile& config, const char* key, std::string& value)
	{
		return RendererConfigView(config).TryGetDisplayString(key, value);
	}

	std::string ReadChoice(const ConfigFile& config, const char* key, const char* defaultValue,
		std::initializer_list<const char*> choices)
	{
		std::string rawValue;
		if (!TryGetDisplayString(config, key, rawValue))
			return defaultValue;

		const std::string value = ConfigFile::NormalizeName(rawValue);
		for (const char* choice : choices)
		{
			if (value == choice)
				return value;
		}

		DebugLog::Log(
			"libplacebo: invalid %s value '%s'; using %s",
			key,
			rawValue.c_str(),
			defaultValue);
		return defaultValue;
	}

	// Pruning 2026-10-17: one log line per retired key or value a profile section still holds
	// (once per section and key for the process); the file loads and the setting is ignored or
	// migrated (RetiredSettings.h).
	void LogRetiredSettings(const std::string& section, const std::map<std::string, std::string>& values)
	{
		static std::mutex lock;
		static std::set<std::string> logged;
		for (const auto& value : values)
		{
			std::string note;
			if (const RetiredSettings::Key* key = RetiredSettings::FindKey(value.first))
				note = RetiredSettings::KeyNote(section, *key);
			else if (const RetiredSettings::Value* retired = RetiredSettings::FindValue(value.first, value.second))
				note = RetiredSettings::ValueNote(section, *retired);
			if (note.empty()) continue;
			std::lock_guard<std::mutex> guard(lock);
			if (logged.insert(note).second)
				DebugLog::Log("profile settings: %s", note.c_str());
		}
	}
}

namespace VpvRendererSettings
{
	void ReadProfile(const ConfigFile& config, const std::string& ruleSection, const std::string& ruleName,
		bool ruleContentGated, VpvRenderer::Fields& settings)
	{
		// The host's DisplayRule fields this reads, kept under the old name so the moved text is verbatim.
		const struct { const std::string& section; const std::string& name; bool contentGated; } rule{ ruleSection, ruleName, ruleContentGated };
		// Hunk B1 (profile-start block: content-gate guard, ignored keys, retired-key log, raw values)
		// Hunt 2 (cadence_class_fixes, 02-dv L6): a profile selected by `content` that set the
		// classifier's own inputs (intent_content, source_class, dejudder_pulldown,
		// cadence_class_fixes) would drop itself and loop with the hold time: those keys are
		// ignored there (the configuration check warns). Restored when this rule is done.
		struct ContentInputsGuard
		{
			VpvRenderer::Fields& settings;
			bool active;
			InterpolationPolicy::SourceClass sourceClass;
			bool intentContent, dejudderPulldown, cadenceClassFixes;
			~ContentInputsGuard()
			{
				if (!active)
					return;
				settings.sourceClass = sourceClass;
				settings.intentContent = intentContent;
				settings.dejudderPulldown = dejudderPulldown;
				settings.cadenceClassFixes = cadenceClassFixes;
			}
		} contentInputsGuard{ settings, rule.contentGated && settings.cadenceClassFixes, settings.sourceClass,
			settings.intentContent, settings.dejudderPulldown, settings.cadenceClassFixes };
		if (contentInputsGuard.active)
			for (const char* key : { "intent_content", "source_class", "dejudder_pulldown", "cadence_class_fixes" })
			{
				std::string ignored;
				if (config.TryGetString(rule.section, key, ignored))
					DebugLog::Log("profile '%s': %s ignored - its when: rule reads content (cadence_class_fixes)",
						rule.section.c_str(), key);
			}
		if (const auto* values = config.GetSectionValues(rule.section))
		{
			LogRetiredSettings(rule.section, *values);
			settings.profileValues += rule.section + "{";
			for (const auto& value : *values)
				settings.profileValues += value.first + "=" + value.second + ";";
			settings.profileValues += "}";
		}
		// Hunk B2 (tryChoice) and the readChoice helper copied from the host (identical log text)
		auto readChoice = [&config, &rule](const char* key, std::string& target,
			std::initializer_list<const char*> choices)
		{
			std::string raw;
			if (!config.TryGetString(rule.section, key, raw)) return;
			const std::string value = ConfigFile::NormalizeName(raw);
			for (const char* choice : choices)
				if (value == choice) { target = value; return; }
			DebugLog::Log("display rule '%s': invalid %s value '%s'; retaining base setting", rule.name.c_str(), key, raw.c_str());
		};
		// The raw on/off-style compares below, case-insensitive like the validation (audit 2026-10-16:
		// a hand-written "On" passed Apply and was then ignored).
		auto tryChoice = [&config, &rule](const char* key, std::string& out)
		{
			if (!config.TryGetString(rule.section, key, out)) return false;
			out = ConfigFile::NormalizeName(out);
			return true;
		};
		const auto readViewportString = [&](const char* genericKey,
			std::string& value)
		{
			return config.TryGetString(rule.section, genericKey, value);
		};
		std::string raw;
		// Hunk B3 (capture, source resolution, deband_vp, ...)
		readChoice("dejudder_low_res", settings.dejudderLowRes, { "off", "search", "all" });
		readChoice("capture_path", settings.capturePath, { "standard", "gpu" });
		readChoice("capture_upload", settings.captureUpload, { "auto", "render", "early" });
		readChoice("dv_tone_mapping", settings.dvToneMapping, { "libplacebo", "dolby_style" });   // feat/dv-l8
		config.TryGetString(rule.section, "dv_compare_capture_folder", settings.dvCompareCaptureFolder);   // feat/dv-l8 WIP
		readChoice("source_range", settings.sourceRange, { "auto", "full", "limited" });
		readChoice("deband_vp", settings.debandVp, { "off", "auto", "on" });
		readChoice("low_bitrate_handling", settings.lowBitrateHandling, { "auto", "off", "on" });
		{
			// native | auto | any height in lines (general, owner's standing rule).
			std::string value;
			if (config.TryGetString(rule.section, "source_resolution", value))
			{
				value = ConfigFile::NormalizeName(value);
				if (value == "native" || value == "auto" || SourceRebase::ClassHeight(value) > 0)
					settings.sourceResolution = value;
				else
					DebugLog::Log("display rule '%s': invalid source_resolution '%s'; retaining base setting",
						rule.name.c_str(), value.c_str());
			}
			if (config.TryGetString(rule.section, "source_resolution_auto_min_confidence", value))
			{
				char* end = nullptr;
				const double v = std::strtod(value.c_str(), &end);
				if (end != value.c_str() && *end == '\0' && v >= 0.0 && v <= 1.0)
					settings.sourceResolutionAutoMinConfidence = v;
				else
					DebugLog::Log("display rule '%s': invalid source_resolution_auto_min_confidence '%s'; retaining base setting",
						rule.name.c_str(), value.c_str());
			}
			// Latch review 2026-10-17: the switch timing (seconds).
			auto readSeconds = [&](const char* key, double lo, double hi, double& out) {
				if (!config.TryGetString(rule.section, key, value)) return;
				char* end = nullptr;
				const double v = std::strtod(value.c_str(), &end);
				if (end != value.c_str() && *end == '\0' && v >= lo && v <= hi)
					out = v;
				else
					DebugLog::Log("display rule '%s': invalid %s '%s'; retaining base setting", rule.name.c_str(), key, value.c_str());
			};
			readSeconds("source_resolution_engage_max_wait_s", 0.0, 600.0, settings.sourceResolutionEngageMaxWait);
			readSeconds("source_resolution_crossfade_s", 0.0, 10.0, settings.sourceResolutionCrossfade);
			readSeconds("source_resolution_release_fade_s", 0.0, 0.25, settings.sourceResolutionReleaseFade);
			if (config.TryGetString(rule.section, "source_resolution_evidence_shots", value))
			{
				char* end = nullptr;
				const long v = std::strtol(value.c_str(), &end, 10);
				if (end != value.c_str() && *end == '\0' && v >= 1 && v <= 10)
					settings.sourceResolutionEvidenceShots = static_cast<int>(v);
				else
					DebugLog::Log("display rule '%s': invalid source_resolution_evidence_shots '%s'; retaining base setting",
						rule.name.c_str(), value.c_str());
			}
			if (config.TryGetString(rule.section, "source_resolution_evidence_seconds", value))
			{
				char* end = nullptr;
				const double v = std::strtod(value.c_str(), &end);
				if (end != value.c_str() && *end == '\0' && v >= 2.0 && v <= 120.0)
					settings.sourceResolutionEvidenceSeconds = v;
				else
					DebugLog::Log("display rule '%s': invalid source_resolution_evidence_seconds '%s'; retaining base setting",
						rule.name.c_str(), value.c_str());
			}
		}
		readChoice("downscale_light", settings.downscaleLight, { "linear", "gamma" });
		readChoice("upscale_light", settings.upscaleLight, { "linear", "gamma" });
		// Hunk B4 (saturation, de-judder, repair, intent, ... )
		if (config.TryGetString(rule.section, "saturation", raw))
		{
			int parsedSaturation = 0;
			if (ParseInteger(raw, -100, 100, parsedSaturation))
				settings.saturation = parsedSaturation;
			else
				DebugLog::Log("profile '%s': invalid saturation '%s'; using 0",
					rule.section.c_str(), raw.c_str());
		}
		if (tryChoice("dejudder_even_rates", raw))
		{
			// General (owner 2026-10-14): whole-number source rates, or also the
			// 1000/1001 rates; "30" / "30_and_29.97" are retired names (RetiredSettings).
			raw = RetiredSettings::Migrate("dejudder_even_rates", raw);
			if (raw == "whole")
				settings.dejudderEvenIncludes1001 = false;
			else if (raw == "whole_and_1001")
				settings.dejudderEvenIncludes1001 = true;
			else
				DebugLog::Log("profile '%s': invalid dejudder_even_rates '%s'; keeping %s", rule.section.c_str(),
					raw.c_str(), settings.dejudderEvenIncludes1001 ? "whole_and_1001" : "whole");
		}
		for (const auto& numeric : { std::make_tuple("dejudder_strength", 0.0, 1.0, &settings.dejudderStrength),
			std::make_tuple("dejudder_strength_even", 0.0, 1.0, &settings.dejudderStrengthEven),
			std::make_tuple("dejudder_match_tolerance", 0.5, 16.0, &settings.dejudderMatchTolerance),
			std::make_tuple("dejudder_frame_trust", 0.0, 1.0, &settings.dejudderFrameTrust),
			std::make_tuple("dejudder_motion_scale_lo", 0.0, 16.0, &settings.dejudderMotionScaleLo),
			std::make_tuple("dejudder_motion_scale_hi", 0.0, 32.0, &settings.dejudderMotionScaleHi),
			std::make_tuple("dejudder_trust_scale", 0.0, 1.0, &settings.dejudderTrustScale),
			std::make_tuple("dejudder_est_lambda", 0.0, 8.0, &settings.dejudderEstLambda),
			std::make_tuple("dejudder_est_split_sad", 0.0, 16.0, &settings.dejudderEstSplitSad),
			std::make_tuple("dejudder_roll_lo", 0.0, 200.0, &settings.dejudderRollLo),
			std::make_tuple("dejudder_roll_hi", 0.0, 400.0, &settings.dejudderRollHi),
			std::make_tuple("dejudder_roll_floor", 0.0, 1.0, &settings.dejudderRollFloor),
			std::make_tuple("dejudder_object_strength", 0.0, 1.0, &settings.dejudderObjectStrength),
			std::make_tuple("dejudder_texture_tolerance", 0.0, 4.0, &settings.dejudderTextureTolerance),
			std::make_tuple("dejudder_cut_histogram", 0.0, 2.0, &settings.dejudderCutHistogram),
			std::make_tuple("dejudder_consistency_px", 0.5, 8.0, &settings.dejudderConsistencyPx),
			std::make_tuple("dejudder_fade_floor", 0.0, 1.0, &settings.dejudderFadeFloor),
			std::make_tuple("dejudder_verify_share", 0.02, 1.0, &settings.dejudderVerifyShare),
			std::make_tuple("dejudder_pixel_trust_lo", 0.25, 16.0, &settings.dejudderPixelTrustLo),
			std::make_tuple("dejudder_pixel_trust_hi", 0.5, 32.0, &settings.dejudderPixelTrustHi),
			std::make_tuple("dejudder_hold_feather_px", 0.0, 16.0, &settings.dejudderHoldFeatherPx),
			std::make_tuple("dejudder_taper_sad", 0.0, 32.0, &settings.dejudderTaperSad),
			std::make_tuple("dither_grain_tolerance_scale", 1.0, 8.0, &settings.ditherGrainToleranceScale),
			std::make_tuple("intent_content_hold_seconds", 1.0, 60.0, &settings.intentContentHoldSeconds),
			std::make_tuple("detail_pulse_cap", DetailPulse::kCapMin, DetailPulse::kCapMax, &settings.detailPulseCap),
			std::make_tuple("texture_restore_cap", TextureRestore::kCapMin, TextureRestore::kCapMax, &settings.textureRestoreCap),
			std::make_tuple("intent_motion_min", 0.0, 1.0, &settings.intentMotionMin),
			std::make_tuple("intent_motion_min_shot_s", 0.5, 30.0, &settings.intentMotionMinShotSeconds),
			std::make_tuple("intent_ease_s", 0.0, 5.0, &settings.intentEaseSeconds),
			std::make_tuple("ambient_adapt_strength", 0.0, 1.0, &settings.ambientAdaptStrength),
			std::make_tuple("ambient_screen_reflect", 0.0, 1.0, &settings.ambientScreenReflect),
			std::make_tuple("ambient_max_black_nits", 0.0, 10.0, &settings.ambientMaxBlackNits),
			std::make_tuple("ambient_tau_up_s", 1.0, 600.0, &settings.ambientTauUpSeconds),
			std::make_tuple("ambient_tau_down_s", 1.0, 1800.0, &settings.ambientTauDownSeconds) })
		{
			if (!config.TryGetString(rule.section, std::get<0>(numeric), raw))
				continue;
			double parsed = 0.0;
			if (DisplayRuleExpression::ParseNumber(raw, parsed) &&
				parsed >= std::get<1>(numeric) && parsed <= std::get<2>(numeric))
				*std::get<3>(numeric) = parsed;
			else
				DebugLog::Log("profile '%s': invalid %s '%s'; keeping %g",
					rule.section.c_str(), std::get<0>(numeric), raw.c_str(), *std::get<3>(numeric));
		}
		if (config.TryGetString(rule.section, "dejudder_search_range", raw))
		{
			int parsed = 0;
			if (ParseInteger(raw, 1, 16, parsed))
				settings.dejudderSearchRange = parsed;
			else
				DebugLog::Log("profile '%s': invalid dejudder_search_range '%s'; keeping %d",
					rule.section.c_str(), raw.c_str(), settings.dejudderSearchRange);
		}
		if (config.TryGetString(rule.section, "dejudder_hold_hysteresis", raw))
		{
			int parsed = 0;
			if (ParseInteger(raw, 1, 8, parsed))
				settings.dejudderHoldHysteresis = parsed;
			else
				DebugLog::Log("profile '%s': invalid dejudder_hold_hysteresis '%s'; keeping %d",
					rule.section.c_str(), raw.c_str(), settings.dejudderHoldHysteresis);
		}
		if (tryChoice("dither_vectors", raw))
		{
			if (raw == "auto" || raw == "own")
				settings.ditherVectorsOwn = raw == "own";
			else
				DebugLog::Log("profile '%s': invalid dither_vectors '%s'; keeping %s", rule.section.c_str(), raw.c_str(),
					settings.ditherVectorsOwn ? "own" : "auto");
		}
		if (tryChoice("fbo_float32", raw))
		{
			if (raw == "auto" || raw == "on" || raw == "off")
				settings.fboFloat32 = raw == "auto" ? -1 : (raw == "on" ? 1 : 0);
			else
				DebugLog::Log("profile '%s': invalid fbo_float32 '%s'; keeping %s", rule.section.c_str(), raw.c_str(),
					settings.fboFloat32 < 0 ? "auto" : (settings.fboFloat32 ? "on" : "off"));
		}
		for (const auto& toggle : { std::make_pair("occlusion_mask", &settings.occlusionMask),
			std::make_pair("repeat_render_reuse", &settings.repeatRenderReuse),
			std::make_pair("dejudder_global_prior", &settings.dejudderGlobalPrior),
			std::make_pair("dejudder_vector_smooth", &settings.dejudderVectorSmooth),
			std::make_pair("dejudder_occlusion_fill", &settings.dejudderOcclusionFill),
			std::make_pair("dejudder_est_candidates", &settings.dejudderEstCandidates),
			std::make_pair("dejudder_est_qpel", &settings.dejudderEstQpel),
			std::make_pair("dejudder_est_still_zero", &settings.dejudderEstStillZero),
			std::make_pair("dejudder_est_gate", &settings.dejudderEstGate),
			std::make_pair("dejudder_est_split", &settings.dejudderEstSplit),
			std::make_pair("dejudder_block_taper", &settings.dejudderBlockTaper),
			std::make_pair("dejudder_estimate_ahead", &settings.dejudderEstimateAhead),
			std::make_pair("dejudder_motion_rolloff", &settings.dejudderMotionRolloff),
			std::make_pair("dejudder_global_two", &settings.dejudderGlobalTwo),
			std::make_pair("dejudder_regional_hold", &settings.dejudderRegionalHold),
			std::make_pair("dejudder_fade_match", &settings.dejudderFadeMatch),
			std::make_pair("dejudder_verify", &settings.dejudderVerify),
			std::make_pair("dejudder_pixel_trust", &settings.dejudderPixelTrust),
			std::make_pair("dejudder_landing_check", &settings.dejudderLandingCheck),
			std::make_pair("dejudder_pulldown", &settings.dejudderPulldown),
			std::make_pair("native_rgb_ingest", &settings.nativeRgbIngest),
			std::make_pair("dv_tunnel_gpu_unpack", &settings.dvTunnelGpuUnpack),
			std::make_pair("dv_trims", &settings.dvTrims),
			std::make_pair("dv_tunnel_fixes", &settings.dvTunnelFixes),
			// Stage 12 §2c / S8-1: the residual held along the vectors (on = when they exist).
			std::make_pair("cnn_residual_stabilise", &settings.adaptive.cnnResidualStabilise),
			std::make_pair("ssim_residual_stabilise", &settings.adaptive.ssimResidualStabilise),
			std::make_pair("ssim_bitrate_aware", &settings.ssimBitrateAware),
			// Film grain (owner 2026-10-17): the faithful defaults on / deband_grain off.
			std::make_pair("residual_stabilise_grain_guard", &settings.adaptive.residualGrainGuard),
			std::make_pair("deband_grain", &settings.debandGrain),
			std::make_pair("deband_vp_exclusive", &settings.debandVpExclusive),
			std::make_pair("detail_pulse_stabilise", &settings.detailPulseStabilise),
			std::make_pair("detail_pulse_fast", &settings.detailPulseFast) })
		{
			if (!tryChoice(toggle.first, raw))
				continue;
			if (raw == "on" || raw == "off")
				*toggle.second = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid %s '%s'; keeping %s",
					rule.section.c_str(), toggle.first, raw.c_str(), *toggle.second ? "on" : "off");
		}
		// Stage 8-2: the adaptive downscale (rendering profile).
		for (const auto& numeric : { std::make_tuple("adaptive_w_base", 0.0, 1.0, &settings.adaptive.wBase),
			std::make_tuple("adaptive_edge_lo", 0.0, 255.0, &settings.adaptive.edgeLo),
			std::make_tuple("adaptive_edge_hi", 0.0, 255.0, &settings.adaptive.edgeHi),
			std::make_tuple("adaptive_grain_lo", 0.0, 64.0, &settings.adaptive.grainLo),
			std::make_tuple("adaptive_grain_hi", 0.0, 64.0, &settings.adaptive.grainHi),
			std::make_tuple("adaptive_grain_floor", 0.0, 1.0, &settings.adaptive.grainFloor),
			std::make_tuple("adaptive_grain_smooth_ms", 0.0, 10000.0, &settings.adaptive.grainSmoothMs),
			std::make_tuple("adaptive_grain_map_smooth_ms", 0.0, 10000.0, &settings.adaptive.grainMapSmoothMs),
			std::make_tuple("adaptive_grain_shadow_bias", 0.0, 1.0, &settings.adaptive.shadowBias),
			// The motion-adapted blend (VP-V scaling.output; owner 2026-10-31).
			std::make_tuple("adaptive_motion_speed_lo", 0.0, 10.0, &settings.adaptive.motionSpeedLo),
			std::make_tuple("adaptive_motion_speed_hi", 0.0, 10.0, &settings.adaptive.motionSpeedHi),
			std::make_tuple("adaptive_motion_floor", 0.0, 1.0, &settings.adaptive.motionFloor),
			std::make_tuple("adaptive_motion_smooth_ms", 0.0, 10000.0, &settings.adaptive.motionSmoothMs) })
		{
			if (!config.TryGetString(rule.section, std::get<0>(numeric), raw))
				continue;
			double parsed = 0.0;
			if (DisplayRuleExpression::ParseNumber(raw, parsed) &&
				parsed >= std::get<1>(numeric) && parsed <= std::get<2>(numeric))
				*std::get<3>(numeric) = static_cast<float>(parsed);
			else
				DebugLog::Log("profile '%s': invalid %s '%s'; keeping %g",
					rule.section.c_str(), std::get<0>(numeric), raw.c_str(), *std::get<3>(numeric));
		}
		if (settings.adaptive.edgeHi <= settings.adaptive.edgeLo)
			settings.adaptive.edgeHi = settings.adaptive.edgeLo + 1.0f;
		if (settings.adaptive.grainHi <= settings.adaptive.grainLo)
			settings.adaptive.grainHi = settings.adaptive.grainLo + 0.5f;
		if (settings.adaptive.motionSpeedHi <= settings.adaptive.motionSpeedLo)
			settings.adaptive.motionSpeedHi = settings.adaptive.motionSpeedLo + 0.05f;
		if (tryChoice("adaptive_faithful", raw))
		{
			// "polar" retired 2026-10-17 (owner: Auto or 1D): runs as auto (RetiredSettings).
			raw = RetiredSettings::Migrate("adaptive_faithful", raw);
			if (raw == "ortho" || raw == "auto")
			{
				settings.adaptive.faithfulOrtho = raw == "ortho";
				settings.adaptive.faithfulAuto = raw == "auto";
			}
			else
				DebugLog::Log("profile '%s': invalid adaptive_faithful '%s'; keeping %s", rule.section.c_str(), raw.c_str(),
					settings.adaptive.faithfulAuto ? "auto" : settings.adaptive.faithfulOrtho ? "ortho" : "polar");
		}
		if (tryChoice("rebase_kernel_match", raw))
		{
			if (raw == "on" || raw == "off")
				settings.rebaseKernelMatch = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid rebase_kernel_match '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.rebaseKernelMatch ? "on" : "off");
		}
		if (tryChoice("source_resolution_mixed_guard", raw))
		{
			if (raw == "on" || raw == "off")
				settings.sourceResolutionMixedGuard = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid source_resolution_mixed_guard '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.sourceResolutionMixedGuard ? "on" : "off");
		}
		if (tryChoice("source_coding_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.sourceCodingFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid source_coding_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.sourceCodingFixes ? "on" : "off");
		}
		if (tryChoice("source_resolution_latch_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.sourceResolutionLatchFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid source_resolution_latch_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.sourceResolutionLatchFixes ? "on" : "off");
		}
		if (tryChoice("active_picture_startup_hold", raw))
		{
			if (raw == "on" || raw == "off")
				settings.activePictureStartupHold = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid active_picture_startup_hold '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.activePictureStartupHold ? "on" : "off");
		}
		if (tryChoice("source_resolution_cut_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.sourceResolutionCutFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid source_resolution_cut_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.sourceResolutionCutFixes ? "on" : "off");
		}
		if (tryChoice("timing_continuity_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.timingContinuityFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid timing_continuity_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.timingContinuityFixes ? "on" : "off");
		}
		if (tryChoice("cadence_class_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.cadenceClassFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid cadence_class_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.cadenceClassFixes ? "on" : "off");
		}
		if (tryChoice("motion_history_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.motionHistoryFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid motion_history_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.motionHistoryFixes ? "on" : "off");
		}
		if (tryChoice("vector_demand", raw))
		{
			if (raw == "on" || raw == "off")
				settings.vectorDemand = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid vector_demand '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.vectorDemand ? "on" : "off");
		}
		if (tryChoice("queue_timing_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.queueTimingFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid queue_timing_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.queueTimingFixes ? "on" : "off");
		}
		if (tryChoice("general_rate_fixes", raw))   // fixed-constant batch 2026-10-18
		{
			if (raw == "on" || raw == "off")
				settings.generalRateFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid general_rate_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.generalRateFixes ? "on" : "off");
		}
		if (tryChoice("efficiency_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.efficiencyFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid efficiency_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.efficiencyFixes ? "on" : "off");
		}
		// Render-ahead (docs/VP-render-ahead-design.md).
		for (const auto& toggle : { std::make_pair("render_ahead", &settings.renderAhead),
				std::make_pair("render_ahead_queue_credit", &settings.renderAheadQueueCredit) })
			if (tryChoice(toggle.first, raw))
			{
				if (raw == "on" || raw == "off")
					*toggle.second = raw == "on";
				else
					DebugLog::Log("profile '%s': invalid %s '%s'; keeping %s",
						rule.section.c_str(), toggle.first, raw.c_str(), *toggle.second ? "on" : "off");
			}
		if (tryChoice("dejudder_gate", raw))
		{
			if (raw == "auto" || raw == "period" || raw == "refresh")
				settings.dejudderGate = raw;
			else
				DebugLog::Log("profile '%s': invalid dejudder_gate '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.dejudderGate.c_str());
		}
		if (config.TryGetString(rule.section, "render_ahead_utilisation", raw))
		{
			double parsed = 0.0;
			if (DisplayRuleExpression::ParseNumber(raw, parsed) && parsed >= 0.5 && parsed <= 1.0)
				settings.renderAheadUtilisation = parsed;
			else
				DebugLog::Log("profile '%s': invalid render_ahead_utilisation '%s'; keeping %g",
					rule.section.c_str(), raw.c_str(), settings.renderAheadUtilisation);
		}
		if (tryChoice("pulldown_fixes", raw))
		{
			if (raw == "on" || raw == "off")
				settings.pulldownFixes = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid pulldown_fixes '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.pulldownFixes ? "on" : "off");
		}
		if (tryChoice("source_resolution_text_guard", raw))
		{
			if (raw == "on" || raw == "off")
				settings.sourceResolutionTextGuard = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid source_resolution_text_guard '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.sourceResolutionTextGuard ? "on" : "off");
		}
		if (tryChoice("interpolation", raw))
		{
			if (raw == "off" || raw == "auto" || raw == "always")
				settings.interpolation = InterpolationPolicy::ParseMode(raw);
			else
				DebugLog::Log("profile '%s': invalid interpolation '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), InterpolationPolicy::Name(settings.interpolation));
		}
		if (tryChoice("interpolation_fallback", raw))
		{
			if (raw == "hold" || raw == "blend")
				settings.interpolationFallback = InterpolationPolicy::ParseFallback(raw);
			else
				DebugLog::Log("profile '%s': invalid interpolation_fallback '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), InterpolationPolicy::Name(settings.interpolationFallback));
		}
		if (tryChoice("interpolation_grain", raw))
		{
			if (raw == "preserve" || raw == "blend")
				settings.interpolationGrain = InterpolationPolicy::ParseGrain(raw);
			else
				DebugLog::Log("profile '%s': invalid interpolation_grain '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), InterpolationPolicy::Name(settings.interpolationGrain));
		}
		// deband_vp_grain_guard: on (the grain split) | veto (the 2026-10-17 spatial-share veto) | off.
		if (tryChoice("deband_vp_grain_guard", raw))
		{
			if (raw == "on" || raw == "veto" || raw == "off")
			{
				settings.debandVpGrainGuard = raw != "off";
				settings.debandVpGrainSplit = raw == "on";
			}
			else
				DebugLog::Log("profile '%s': invalid deband_vp_grain_guard '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), !settings.debandVpGrainGuard ? "off" : settings.debandVpGrainSplit ? "on" : "veto");
		}
		// texture_restore: off | statistical (docs/VP-detail-restore.md).
		if (tryChoice("texture_restore", raw))
		{
			if (raw == "off" || raw == "statistical")
				settings.textureRestore = raw == "statistical";
			else
				DebugLog::Log("profile '%s': invalid texture_restore '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.textureRestore ? "statistical" : "off");
		}
		if (tryChoice("dejudder_source_warp_filter", raw))
		{
			if (raw == "catmull" || raw == "bilinear")
				settings.dejudderSourceWarpCatmull = raw == "catmull";
			else
				DebugLog::Log("profile '%s': invalid dejudder_source_warp_filter '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.dejudderSourceWarpCatmull ? "catmull" : "bilinear");
		}
		if (tryChoice("nls_sampling", raw))
		{
			if (raw == "catmull" || raw == "bilinear")
				settings.nlsSamplingCatmull = raw == "catmull";
			else
				DebugLog::Log("profile '%s': invalid nls_sampling '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.nlsSamplingCatmull ? "catmull" : "bilinear");
		}
		if (tryChoice("interpolation_fill_gaps", raw))
		{
			if (raw == "on" || raw == "off")
				settings.interpolationFillGaps = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid interpolation_fill_gaps '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.interpolationFillGaps ? "on" : "off");
		}
		if (tryChoice("source_class", raw))
		{
			if (raw == "legacy" || raw == "measured")
				settings.sourceClass = InterpolationPolicy::ParseSourceClass(raw);
			else
				DebugLog::Log("profile '%s': invalid source_class '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), InterpolationPolicy::Name(settings.sourceClass));
		}
		if (tryChoice("intent_content", raw))
		{
			if (raw == "on" || raw == "off")
				settings.intentContent = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid intent_content '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.intentContent ? "on" : "off");
		}
		// Intent interpreter items 2-5 (2026-10-17).
		for (const auto& toggle : { std::make_pair("intent_motion", &settings.intentMotion),
			std::make_pair("ambient_adapt", &settings.ambientAdapt) })
		{
			if (!tryChoice(toggle.first, raw))
				continue;
			if (raw == "on" || raw == "off")
				*toggle.second = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid %s '%s'; keeping %s",
					rule.section.c_str(), toggle.first, raw.c_str(), *toggle.second ? "on" : "off");
		}
		if (tryChoice("intent_motion_policy", raw))
		{
			MotionIntentPolicy policy = settings.intentMotionPolicy;
			if (ParseMotionIntentPolicy(raw, policy))
				settings.intentMotionPolicy = policy;
			else
				DebugLog::Log("profile '%s': invalid intent_motion_policy '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), ToString(settings.intentMotionPolicy));
		}
		if (tryChoice("intent_black", raw))
		{
			BlackIntentMode mode = settings.intentBlack;
			if (ParseBlackIntentMode(raw, mode))
				settings.intentBlack = mode;
			else
				DebugLog::Log("profile '%s': invalid intent_black '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), ToString(settings.intentBlack));
		}
		if (tryChoice("intent_display", raw))
		{
			if (raw == "report" || raw == "off")
				settings.intentDisplay = raw == "report";
			else
				DebugLog::Log("profile '%s': invalid intent_display '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.intentDisplay ? "report" : "off");
		}
		if (tryChoice("optical_flow", raw))
		{
			if (raw == "on" || raw == "off")
				settings.opticalFlow = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid optical_flow '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.opticalFlow ? "on" : "off");
		}
		if (config.TryGetString(rule.section, "temporal_dither", raw))
		{
			const std::string v = ConfigFile::NormalizeName(raw);
			if (v == "auto" || v == "off" || v == "libplacebo" || v == "sigma_delta")
				settings.temporalDither = v;
			else
				DebugLog::Log("profile '%s': invalid temporal_dither '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.temporalDither.c_str());
		}
		if (tryChoice("optical_flow_global", raw))
		{
			if (raw == "on" || raw == "off")
				settings.opticalFlowGlobal = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid optical_flow_global '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.opticalFlowGlobal ? "on" : "off");
		}
		// The engine's extras (2026-10-17): on | off each.
		for (const auto& extra : { std::make_pair("optical_flow_global_fallback", &settings.opticalFlowGlobalFallback),
				std::make_pair("optical_flow_cost_trust", &settings.opticalFlowCostTrust),
				std::make_pair("optical_flow_fb_check", &settings.opticalFlowFbCheck) })
			if (tryChoice(extra.first, raw))
			{
				if (raw == "on" || raw == "off")
					*extra.second = raw == "on";
				else
					DebugLog::Log("profile '%s': invalid %s '%s'; keeping %s",
						rule.section.c_str(), extra.first, raw.c_str(), *extra.second ? "on" : "off");
			}
		if (config.TryGetString(rule.section, "motion_ahead_frames", raw))
		{
			double parsed = 0.0;
			// Owner 2026-10-16: on or off - the look-ahead works only with 4 frames ahead, so any
			// value above 0 (older files may say 1-3) is 4.
			if (DisplayRuleExpression::ParseNumber(raw, parsed) && parsed >= 0.0 && parsed <= MotionAhead::kMaxDepth && parsed == static_cast<int>(parsed))
				settings.motionAheadFrames = parsed > 0.0 ? MotionAhead::kDefaultDepth : 0;
			else
				DebugLog::Log("profile '%s': invalid motion_ahead_frames '%s'; keeping %d",
					rule.section.c_str(), raw.c_str(), settings.motionAheadFrames);
		}
		if (config.TryGetString(rule.section, "ssim_bitrate_floor", raw))
		{
			double parsed = 0.0;
			if (DisplayRuleExpression::ParseNumber(raw, parsed) && parsed >= 0.0 && parsed <= 1.0)
				settings.ssimBitrateFloor = parsed;
			else
				DebugLog::Log("profile '%s': invalid ssim_bitrate_floor '%s'; keeping %g",
					rule.section.c_str(), raw.c_str(), settings.ssimBitrateFloor);
		}
		if (config.TryGetString(rule.section, "adaptive_upscale_w_base", raw))
		{
			double parsed = 0.0;
			if (DisplayRuleExpression::ParseNumber(raw, parsed) && parsed >= 0.0 && parsed <= 1.0)
				settings.adaptiveUpscaleWBase = parsed;
			else
				DebugLog::Log("profile '%s': invalid adaptive_upscale_w_base '%s'; keeping %g",
					rule.section.c_str(), raw.c_str(), settings.adaptiveUpscaleWBase);
		}
		for (const auto& toggle : { std::make_pair("adaptive_downscale", &settings.adaptiveDownscale),
			std::make_pair("adaptive_chroma_ssim", &settings.adaptive.chromaSsim),
			std::make_pair("adaptive_grain_map", &settings.adaptive.grainMap),
			std::make_pair("adaptive_upscale", &settings.adaptiveUpscale),
			std::make_pair("adaptive_upscale_superres_scale", &settings.adaptiveUpscaleSuperresScale),
			std::make_pair("adaptive_motion", &settings.adaptive.motionAdaptive) })
		{
			if (!tryChoice(toggle.first, raw))
				continue;
			if (raw == "on" || raw == "off")
				*toggle.second = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid %s '%s'; keeping %s",
					rule.section.c_str(), toggle.first, raw.c_str(), *toggle.second ? "on" : "off");
		}
		if (tryChoice("dejudder_warp_stage", raw))
		{
			if (raw == "rendered" || raw == "source")
				settings.dejudderWarpRendered = raw == "rendered";
			else
				DebugLog::Log("profile '%s': invalid dejudder_warp_stage '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.dejudderWarpRendered ? "rendered" : "source");
		}
		if (tryChoice("extended_range_more_cases", raw))
		{
			if (raw == "on" || raw == "off")
				settings.extendedRangeMoreCases = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid extended_range_more_cases '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.extendedRangeMoreCases ? "on" : "off");
		}
		if (tryChoice("preserve_extended_range", raw))
		{
			if (raw == "on" || raw == "off")
				settings.preserveExtendedRange = raw == "on";
			else
				DebugLog::Log("profile '%s': invalid preserve_extended_range '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.preserveExtendedRange ? "on" : "off");
		}
		// Delta 13: the SDR tone shape and headroom.
		if (tryChoice("sdr_tone_shape", raw))
		{
			if (raw == "off" || raw == "knee")
				settings.sdrTone.knee = raw == "knee";
			else
				DebugLog::Log("profile '%s': invalid sdr_tone_shape '%s'; keeping %s",
					rule.section.c_str(), raw.c_str(), settings.sdrTone.knee ? "knee" : "off");
		}
		{
			const auto readTone = [&](const char* key, double minimum, double maximum, double& target)
			{
				std::string text;
				if (!config.TryGetString(rule.section, key, text))
					return;
				double value = 0.0;
				if (ParseDouble(text, value) && value >= minimum && value <= maximum)
					target = value;
				else
					DebugLog::Log("profile '%s': invalid %s '%s'; keeping %.3f", rule.section.c_str(), key, text.c_str(), target);
			};
			readTone("sdr_white_headroom", 0.0, 0.5, settings.sdrTone.headroom);
			readTone("sdr_knee_start", 0.5, 0.99, settings.sdrTone.kneeStart);
			readTone("sdr_knee_strength", 0.0, 1.0, settings.sdrTone.kneeStrength);
		}
		if (config.TryGetString(rule.section, "sdr_contrast_recovery", raw))
		{
			const std::string normalized = ConfigFile::NormalizeName(raw);
			double value = 0.0;
			if (normalized == "auto")
				settings.sdrTone.recovery = -1.0;
			else if (normalized == "off")
				settings.sdrTone.recovery = 0.0;
			else if (ParseDouble(raw, value) && value >= 0.0 && value <= 2.0)
				settings.sdrTone.recovery = value;
			else
				DebugLog::Log("profile '%s': invalid sdr_contrast_recovery '%s'; keeping it", rule.section.c_str(), raw.c_str());
		}
		// Hunk B6 (edge blanking)
		for (const auto& blank : {
			std::make_pair("blank_left", &settings.edgeBlanking.left),
			std::make_pair("blank_top", &settings.edgeBlanking.top),
			std::make_pair("blank_right", &settings.edgeBlanking.right),
			std::make_pair("blank_bottom", &settings.edgeBlanking.bottom) })
		{
			int parsed = 0;
			if (readViewportString(blank.first, raw) &&
				ParseInteger(raw, 0, EdgeBlanking::MAX_INSET_PIXELS, parsed))
				*blank.second = parsed;
		}
	}

	// ---- ReadBase: the VP-V keys the host reads before any profile rule (flat / [display] level) ----
	void ReadBase(const ConfigFile& config, VpvRenderer::Fields& settings)
	{
		std::string rawValue;
		settings.downscaleLight = ReadChoice(config, "downscale_light", "linear", { "linear", "gamma" });
		settings.upscaleLight = ReadChoice(config, "upscale_light", "linear", { "linear", "gamma" });
		if (TryGetDisplayString(config, "deband_grain", rawValue))
		{
			const std::string value = ConfigFile::NormalizeName(rawValue);
			if (value == "on" || value == "off")
				settings.debandGrain = value == "on";
			else
				DebugLog::Log("libplacebo: deband_grain must be on or off; retaining %s", settings.debandGrain ? "on" : "off");
		}
	}

	// ---- ReadGlobal: the global pipeline keys ([vpv] first, then [vprenderer]) ----
	void ReadGlobal(const ConfigFile& config, VpvRenderer::Fields& settings)
	{
		std::string raw;
		// ADR-0023: the VP-V Config section stores these global keys in [vpv] (renderer profiles are
		// renamed and reordered by Config, the [vpv] section never is); [vprenderer] stays readable.
		const auto TryGetPipelineString = [](const ConfigFile& file, const char* key, std::string& value)
		{
			return file.TryGetString("vpv", key, value) || TryGetDisplayString(file, key, value);
		};
		if (TryGetPipelineString(config, "pipeline_original_pathway", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "on" || value == "off")
				settings.pipelineOriginalPathway = value == "on";
			else
				settings.pipelineSettingsProblems += "pipeline_original_pathway '" + raw + "' (on|off); ";
		}
		if (TryGetPipelineString(config, "pipeline_order", raw))
			settings.pipelineOrder = ConfigFile::Trim(raw);
		if (TryGetPipelineString(config, "pipeline_slots_off", raw))
			settings.pipelineSlotsOff = ConfigFile::Trim(raw);
		if (TryGetPipelineString(config, "calibration_lut_stage", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "libplacebo" || value == "vp")
				settings.calibrationLutStage = value;
			else
				settings.pipelineSettingsProblems += "calibration_lut_stage '" + raw + "' (libplacebo|vp); ";
		}
		if (TryGetPipelineString(config, "output_segments_merged", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "on" || value == "off")
				settings.outputSegmentsMerged = value == "on";
			else
				settings.pipelineSettingsProblems += "output_segments_merged '" + raw + "' (on|off); ";
		}
		if (TryGetPipelineString(config, "hdr_inference", raw))   // F-8 (HdrInferenceChoice.h)
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "vp" || value == "original")
				settings.hdrInference = value;
			else
				settings.pipelineSettingsProblems += "hdr_inference '" + raw + "' (vp|original); ";
		}
		if (TryGetPipelineString(config, "pipeline_entrance_once", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "on" || value == "off")
				settings.pipelineEntranceOnce = value == "on";
			else
				settings.pipelineSettingsProblems += "pipeline_entrance_once '" + raw + "' (on|off); ";
		}
		if (TryGetPipelineString(config, "frame_analysis_store", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "on" || value == "off")
				settings.frameAnalysisStore = value == "on";
			else
				settings.pipelineSettingsProblems += "frame_analysis_store '" + raw + "' (on|off); ";
		}
		if (TryGetPipelineString(config, "resync_event", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "off" || value == "shadow" || value == "on")
				settings.resyncEvent = value;
			else
				settings.pipelineSettingsProblems += "resync_event '" + raw + "' (off|shadow|on); ";
		}
		if (TryGetPipelineString(config, "entrance_gpu_drain", raw))
		{
			const std::string value = ConfigFile::NormalizeName(raw);
			if (value == "off" || value == "flush" || value == "finish")
				settings.entranceGpuDrain = value;
			else
				settings.pipelineSettingsProblems += "entrance_gpu_drain '" + raw + "' (off|flush|finish); ";
		}
	}

	// ---- PostLoad: the Original pathway overlay, the slot switches, the presentation owner ----
	void ApplyOriginal(VpvRenderer::Fields& settings);   // below

	Overrides PostLoad(VpvRenderer::Fields& settings, bool presentEveryVsyncRequested)
	{
		Overrides overrides;
		if (settings.pipelineOriginalPathway)
		{
			ApplyOriginal(settings);
			overrides.chromaUpscaler = "bicubic";                 // key=chroma_upscaler (a host field, so returned)
		}
		// Motion + queue/timing slots switched off run in their upstream form (owner 2026-10-25).
		SlotSwitchSettings::Apply(SlotSwitchSettings::FromText(settings.pipelineSlotsOff), settings);
		// Bill merge (owner decision 2): Bill's presentation vs VP-V's render-ahead, either/or on
		// render_ahead (off on the Original pathway = Bill's presentation exactly).
		const PresentationOwner::Decision presentation =
			PresentationOwner::Decide(settings.renderAhead, presentEveryVsyncRequested);
		overrides.presentEveryVsync = presentation.presentEveryVsync;
		settings.presentEveryVsyncWithheld = presentation.presentEveryVsyncWithheld;
		return overrides;
	}

	// ADR-0019: the Original pathway baseline. Every VP addition is switched off and every slot
	// that exists upstream runs in its upstream form, so the renderer reproduces Bill's original
	// pathway. The list is src/VP-V/core/OriginalPathway.h (verified against bill-v1.4.02); each
	// line here names its key, and the drift check fails when the two disagree.
	void ApplyOriginal(VpvRenderer::Fields& settings)
	{
		settings.capturePath = "standard";                 // key=capture_path
		settings.captureUpload = "render";                 // key=capture_upload
		settings.nativeRgbIngest = false;                  // key=native_rgb_ingest
		settings.dvTunnelGpuUnpack = false;                // key=dv_tunnel_gpu_unpack
		settings.dvTunnelFixes = false;                    // key=dv_tunnel_fixes
		settings.dvCompareCaptureFolder.clear();           // key=dv_compare_capture_folder
		settings.dvTrims = false;                          // key=dv_trims
		settings.dvToneMapping = "libplacebo";             // key=dv_tone_mapping
		settings.queueTimingFixes = false;                 // key=queue_timing_fixes
		settings.pulldownFixes = false;                    // key=pulldown_fixes
		settings.timingContinuityFixes = false;            // key=timing_continuity_fixes
		settings.cadenceClassFixes = false;                // key=cadence_class_fixes
		settings.generalRateFixes = false;                 // key=general_rate_fixes
		settings.efficiencyFixes = false;                  // key=efficiency_fixes
		settings.sourceResolution = "native";              // key=source_resolution
		settings.rebaseKernelMatch = false;                // key=rebase_kernel_match
		settings.sourceCodingFixes = false;                // key=source_coding_fixes
		settings.sourceResolutionLatchFixes = false;       // key=source_resolution_latch_fixes
		settings.sourceResolutionCutFixes = false;         // key=source_resolution_cut_fixes
		settings.debandVp = "off";                         // key=deband_vp
		settings.debandVpExclusive = false;                // key=deband_vp_exclusive
		settings.debandGrain = true;                       // key=deband_grain
		settings.sourceRange = "auto";                     // key=source_range
		settings.upscaleLight = "linear";                  // key=upscale_light
		settings.detailPulseStabilise = false;             // key=detail_pulse_stabilise
		settings.textureRestore = false;                   // key=texture_restore
		settings.dejudderStrength = 0.0;                   // key=dejudder_strength
		settings.dejudderStrengthEven = 0.0;               // key=dejudder_strength_even
		settings.interpolation = InterpolationPolicy::Mode::Off;   // key=interpolation
		settings.interpolationFillGaps = false;            // key=interpolation_fill_gaps
		settings.occlusionMask = false;                    // key=occlusion_mask
		settings.repeatRenderReuse = false;                // key=repeat_render_reuse
		settings.dejudderEstStillZero = false;             // key=dejudder_est_still_zero
		settings.opticalFlow = false;                      // key=optical_flow
		settings.motionAheadFrames = 0;                    // key=motion_ahead_frames
		settings.motionHistoryFixes = false;               // key=motion_history_fixes
		settings.vectorDemand = false;                     // key=vector_demand
		settings.dejudderSourceWarpCatmull = false;        // key=dejudder_source_warp_filter
		settings.nlsSamplingCatmull = false;               // key=nls_sampling
		settings.adaptiveDownscale = false;                // key=adaptive_downscale
		settings.adaptiveUpscale = false;                  // key=adaptive_upscale
		settings.ssimBitrateAware = false;                 // key=ssim_bitrate_aware
		settings.downscaleLight = "linear";                // key=downscale_light
		settings.fboFloat32 = 0;                           // key=fbo_float32
		settings.preserveExtendedRange = false;            // key=preserve_extended_range
		settings.extendedRangeMoreCases = false;           // key=extended_range_more_cases
		settings.sdrTone.knee = false;                     // key=sdr_tone_shape
		settings.intentContent = false;                    // key=intent_content
		settings.intentMotion = false;                     // key=intent_motion
		settings.intentDisplay = false;                    // key=intent_display
		settings.intentBlack = BlackIntentMode::Off;       // key=intent_black
		settings.ambientAdapt = false;                     // key=ambient_adapt
		settings.temporalDither = "libplacebo";            // key=temporal_dither
		settings.outputSegmentsMerged = false;             // key=output_segments_merged
		settings.renderAhead = false;                      // key=render_ahead
		settings.calibrationLutStage = "libplacebo";       // key=calibration_lut_stage
		settings.hdrInference = "original";                // key=hdr_inference
		// Settings audit 2026-10-24: VP additions the first table missed (absent from Bill's v1.4.02-beta).
		settings.activePictureStartupHold = false;         // key=active_picture_startup_hold
		settings.lowBitrateHandling = "off";               // key=low_bitrate_handling
		settings.adaptive.ssimResidualStabilise = false;   // key=ssim_residual_stabilise
		settings.adaptive.cnnResidualStabilise = false;    // key=cnn_residual_stabilise
		settings.adaptive.residualGrainGuard = false;      // key=residual_stabilise_grain_guard
		settings.adaptive.motionAdaptive = false;          // key=adaptive_motion
		settings.dejudderEstimateAhead = false;            // key=dejudder_estimate_ahead
		settings.interpolationFallback = InterpolationPolicy::Fallback::Hold; // key=interpolation_fallback
		settings.pipelineOrder.clear();                    // the designed order
		settings.pipelineEntranceOnce = false;             // B3: per-picture stages, as upstream
		settings.frameAnalysisStore = false;               // key=frame_analysis_store (WP-23: upstream's own decodes only)
		settings.resyncEvent = "off";                      // key=resync_event
		settings.entranceGpuDrain = "off";                 // key=entrance_gpu_drain
	}

	// deband_vp_exclusive: see LibplaceboDebandYieldsToVp in the host (settings = the host's settings struct).

	// Fingerprint terms of the VP-V settings, appended after the host's own (VpvRendererSettings::AppendFingerprint).
	// Every value is separated by '|', so the string stays unambiguous; its ORDER differs from the old
	// interleaved one, the equality partition over configurations is the same (RendererSettingsIdentityTests).
	std::string Fingerprint(const VpvRenderer::Fields& settings, bool includeLive)
	{
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream.precision(17);
		stream << '|'
			<< settings.saturation << '|'
			<< settings.dejudderStrength << '|'
			<< settings.dejudderStrengthEven << '|'
			<< settings.dejudderEvenIncludes1001 << '|'
			<< settings.dejudderMatchTolerance << '|'
			<< settings.dejudderFrameTrust << '|'
			<< settings.occlusionMask << '|'
			<< settings.dejudderGlobalPrior << '|'
			<< settings.dejudderMotionScaleLo << '|'
			<< settings.dejudderMotionScaleHi << '|'
			<< settings.dejudderVectorSmooth << '|'
			<< settings.dejudderTrustScale << '|'
			<< settings.dejudderOcclusionFill << '|'
			<< settings.dejudderEstCandidates << '|'
			<< settings.dejudderEstLambda << '|'
			<< settings.dejudderEstQpel << '|'
			<< settings.dejudderEstStillZero << '|'
			<< settings.dejudderEstGate << '|'
			<< settings.dejudderEstSplit << '|'
			<< settings.dejudderEstSplitSad << '|'
			<< settings.dejudderLowRes << '|'
			<< settings.dejudderEstimateAhead << '|'
			<< settings.capturePath << '|'
			<< settings.dejudderMotionRolloff << '|'
			<< settings.dejudderRollLo << '|'
			<< settings.dejudderRollHi << '|'
			<< settings.dejudderRollFloor << '|'
			<< settings.dejudderObjectStrength << '|'
			<< settings.dejudderTextureTolerance << '|'
			<< settings.dejudderGlobalTwo << '|'
			<< settings.dejudderRegionalHold << '|'
			<< settings.dejudderCutHistogram << '|'
			<< settings.dejudderSearchRange << '|'
			<< settings.dejudderConsistencyPx << '|'
			<< settings.dejudderFadeMatch << '|'
			<< settings.dejudderFadeFloor << '|'
			<< settings.dejudderVerify << '|'
			<< settings.dejudderVerifyShare << '|'
			<< settings.dejudderPixelTrust << '|'
			<< settings.dejudderPixelTrustLo << '|'
			<< settings.dejudderPixelTrustHi << '|'
			<< settings.dejudderLandingCheck << '|'
			<< settings.dejudderPulldown << '|'
			<< settings.nativeRgbIngest << '|'
			<< settings.dvTunnelGpuUnpack << '|'
			<< settings.dvTrims << '|'
			<< settings.dvTunnelFixes << '|'
			<< settings.dejudderHoldFeatherPx << '|'
			<< settings.dejudderHoldHysteresis << '|'
			<< settings.dejudderBlockTaper << '|'
			<< settings.dejudderTaperSad << '|'
			<< settings.ditherGrainToleranceScale << '|'
			<< settings.ditherVectorsOwn << '|'
			<< settings.fboFloat32 << '|'
			<< settings.preserveExtendedRange << '|'
			<< settings.extendedRangeMoreCases << '|'
			<< settings.dejudderWarpRendered << '|'
			<< settings.outputSegmentsMerged << '|'
			<< settings.captureUpload << '|'
			<< settings.sourceRange << '|'
			<< settings.sourceResolution << '|'
			<< settings.dvToneMapping << '|'
			<< settings.dvCompareCaptureFolder << '|'
			<< settings.sourceResolutionAutoMinConfidence << '|'
			<< settings.sourceResolutionEvidenceShots << '|'
			<< settings.sourceResolutionEvidenceSeconds << '|'
			<< settings.debandVp << '|'
			<< settings.lowBitrateHandling << '|'
			<< settings.pipelineOriginalPathway << '|'
			<< settings.pipelineOrder << '|'
			<< settings.pipelineSlotsOff << '|'
			<< settings.calibrationLutStage << '|'
			<< settings.pipelineEntranceOnce << '|'
			<< settings.frameAnalysisStore << '|'
			<< settings.resyncEvent << '|'
			<< settings.entranceGpuDrain << '|'
			<< settings.hdrInference << '|'
			<< settings.adaptive.cnnResidualStabilise << '|'
			<< settings.adaptive.ssimResidualStabilise << '|'
			<< settings.ssimBitrateAware << '|'
			<< settings.ssimBitrateFloor << '|'
			<< settings.motionAheadFrames << '|'
			<< settings.opticalFlow << '|'
			<< settings.opticalFlowGlobal << '|'
			<< settings.opticalFlowGlobalFallback << '|'
			<< settings.opticalFlowCostTrust << '|'
			<< settings.opticalFlowFbCheck << '|'
			<< static_cast<int>(settings.interpolationGrain) << '|'
			<< settings.dejudderSourceWarpCatmull << '|'
			<< settings.nlsSamplingCatmull << '|'
			<< settings.sourceCodingFixes << '|'
			<< settings.debandVpGrainGuard << '|'
			<< settings.debandVpGrainSplit << '|'
			<< settings.debandVpExclusive << '|'
			<< settings.detailPulseStabilise << '|'
			<< settings.detailPulseCap << '|'
			<< settings.detailPulseFast << '|'
			<< settings.textureRestore << '|'
			<< settings.textureRestoreCap << '|'
			<< settings.debandGrain << '|'
			<< settings.adaptive.residualGrainGuard << '|'
			<< static_cast<int>(settings.interpolation) << '|'
			<< static_cast<int>(settings.interpolationFallback) << '|'
			<< settings.interpolationFillGaps << '|'
			<< static_cast<int>(settings.sourceClass) << '|'
			<< settings.intentContent << '|'
			<< settings.intentContentHoldSeconds << '|'
			<< settings.intentMotion << '|'
			<< static_cast<int>(settings.intentMotionPolicy) << '|'
			<< settings.intentMotionMin << '|'
			<< settings.intentMotionMinShotSeconds << '|'
			<< settings.intentEaseSeconds << '|'
			<< settings.ambientAdapt << '|'
			<< settings.ambientAdaptStrength << '|'
			<< settings.ambientScreenReflect << '|'
			<< settings.ambientMaxBlackNits << '|'
			<< settings.ambientTauUpSeconds << '|'
			<< settings.ambientTauDownSeconds << '|'
			<< static_cast<int>(settings.intentBlack) << '|'
			<< settings.intentDisplay << '|'
			<< settings.temporalDither << '|'
			<< settings.rebaseKernelMatch << '|'
			<< settings.sourceResolutionTextGuard << '|'
			<< settings.sourceResolutionMixedGuard << '|'
			<< settings.sourceResolutionLatchFixes << '|'
			<< settings.sourceResolutionCutFixes << '|'
			<< settings.activePictureStartupHold << '|'
			<< settings.sourceResolutionEngageMaxWait << '|'
			<< settings.sourceResolutionCrossfade << '|'
			<< settings.sourceResolutionReleaseFade << '|'
			<< settings.pulldownFixes << '|'
			<< settings.timingContinuityFixes << '|'
			<< settings.cadenceClassFixes << '|'
			<< settings.motionHistoryFixes << '|'
			<< settings.vectorDemand << '|'
			<< settings.queueTimingFixes << '|'
			<< settings.generalRateFixes << '|'
			<< settings.efficiencyFixes << '|'
			<< settings.renderAhead << '|'
			<< settings.dejudderGate << '|'
			<< settings.renderAheadUtilisation << '|'
			<< settings.renderAheadQueueCredit << '|'
			<< settings.sdrTone.knee << '|'
			<< settings.sdrTone.headroom << '|'
			<< settings.sdrTone.kneeStart << '|'
			<< settings.sdrTone.kneeStrength << '|'
			<< settings.sdrTone.recovery << '|'
			<< settings.downscaleLight << '|'
			<< settings.adaptiveUpscale << '|'
			<< settings.adaptiveUpscaleWBase << '|'
			<< settings.adaptiveUpscaleSuperresScale << '|'
			<< settings.upscaleLight << '|'
			<< settings.adaptiveDownscale << '|'
			<< settings.adaptive.wBase << '|'
			<< settings.adaptive.edgeLo << '|'
			<< settings.adaptive.edgeHi << '|'
			<< settings.adaptive.grainLo << '|'
			<< settings.adaptive.grainHi << '|'
			<< settings.adaptive.grainFloor << '|'
			<< settings.adaptive.grainSmoothMs << '|'
			<< settings.adaptive.shadowBias << '|'
			<< settings.adaptive.chromaSsim << '|'
			<< settings.adaptive.faithfulOrtho << '|'
			<< settings.adaptive.faithfulAuto << '|'
			<< settings.adaptive.grainMap << '|'
			<< settings.adaptive.grainMapSmoothMs << '|'
			<< settings.adaptive.motionAdaptive << '|'
			<< settings.adaptive.motionSpeedLo << '|'
			<< settings.adaptive.motionSpeedHi << '|'
			<< settings.adaptive.motionFloor << '|'
			<< settings.adaptive.motionSmoothMs << '|';
		if (includeLive)
		{
			// Planner Q4: the applied profiles' raw values (structural: any key, present or
			// future). Not in the rebuild / transport fingerprint (includeLive off),
			// so they never force a restart by themselves.
			stream << settings.profileValues << '|';
		}
		return stream.str();
	}

	// The live-applied settings: dst takes src's (the host's transport copy of the active settings).
	void CopyLive(VpvRenderer::Fields& currentTransport, const VpvRenderer::Fields& next)
	{
		currentTransport.saturation = next.saturation;
		currentTransport.dejudderStrength = next.dejudderStrength;
		currentTransport.dejudderStrengthEven = next.dejudderStrengthEven;
		currentTransport.dejudderEvenIncludes1001 = next.dejudderEvenIncludes1001;
		currentTransport.dejudderMatchTolerance = next.dejudderMatchTolerance;
		currentTransport.dejudderFrameTrust = next.dejudderFrameTrust;
		currentTransport.occlusionMask = next.occlusionMask;
		currentTransport.dejudderGlobalPrior = next.dejudderGlobalPrior;
		currentTransport.dejudderMotionScaleLo = next.dejudderMotionScaleLo;
		currentTransport.dejudderMotionScaleHi = next.dejudderMotionScaleHi;
		currentTransport.dejudderVectorSmooth = next.dejudderVectorSmooth;
		currentTransport.dejudderTrustScale = next.dejudderTrustScale;
		currentTransport.dejudderOcclusionFill = next.dejudderOcclusionFill;
		currentTransport.dejudderEstCandidates = next.dejudderEstCandidates;
		currentTransport.dejudderEstLambda = next.dejudderEstLambda;
		currentTransport.dejudderEstQpel = next.dejudderEstQpel;
		currentTransport.dejudderEstStillZero = next.dejudderEstStillZero;
		currentTransport.dejudderEstGate = next.dejudderEstGate;
		currentTransport.dejudderEstSplit = next.dejudderEstSplit;
		currentTransport.dejudderEstSplitSad = next.dejudderEstSplitSad;
		currentTransport.dejudderMotionRolloff = next.dejudderMotionRolloff;
		currentTransport.dejudderRollLo = next.dejudderRollLo;
		currentTransport.dejudderRollHi = next.dejudderRollHi;
		currentTransport.dejudderRollFloor = next.dejudderRollFloor;
		currentTransport.dejudderObjectStrength = next.dejudderObjectStrength;
		currentTransport.dejudderTextureTolerance = next.dejudderTextureTolerance;
		currentTransport.dejudderGlobalTwo = next.dejudderGlobalTwo;
		currentTransport.dejudderRegionalHold = next.dejudderRegionalHold;
		currentTransport.dejudderCutHistogram = next.dejudderCutHistogram;
		currentTransport.dejudderSearchRange = next.dejudderSearchRange;
		currentTransport.dejudderConsistencyPx = next.dejudderConsistencyPx;
		currentTransport.dejudderFadeMatch = next.dejudderFadeMatch;
		currentTransport.dejudderFadeFloor = next.dejudderFadeFloor;
		currentTransport.dejudderVerify = next.dejudderVerify;
		currentTransport.dejudderVerifyShare = next.dejudderVerifyShare;
		currentTransport.dejudderPixelTrust = next.dejudderPixelTrust;
		currentTransport.dejudderPixelTrustLo = next.dejudderPixelTrustLo;
		currentTransport.dejudderPixelTrustHi = next.dejudderPixelTrustHi;
		currentTransport.dejudderLowRes = next.dejudderLowRes;
		currentTransport.dejudderEstimateAhead = next.dejudderEstimateAhead;
		currentTransport.capturePath = next.capturePath;
		currentTransport.captureUpload = next.captureUpload;
		currentTransport.dejudderHoldFeatherPx = next.dejudderHoldFeatherPx;
		currentTransport.dejudderHoldHysteresis = next.dejudderHoldHysteresis;
		currentTransport.dejudderBlockTaper = next.dejudderBlockTaper;
		currentTransport.dejudderTaperSad = next.dejudderTaperSad;
		currentTransport.ditherGrainToleranceScale = next.ditherGrainToleranceScale;
		currentTransport.ditherVectorsOwn = next.ditherVectorsOwn;
		currentTransport.fboFloat32 = next.fboFloat32;
		currentTransport.preserveExtendedRange = next.preserveExtendedRange;
		currentTransport.extendedRangeMoreCases = next.extendedRangeMoreCases;
		currentTransport.sdrTone = next.sdrTone;
		currentTransport.dejudderWarpRendered = next.dejudderWarpRendered;
		currentTransport.downscaleLight = next.downscaleLight;
		currentTransport.adaptiveDownscale = next.adaptiveDownscale;
		currentTransport.adaptiveUpscale = next.adaptiveUpscale;
		currentTransport.adaptiveUpscaleWBase = next.adaptiveUpscaleWBase;
		currentTransport.adaptiveUpscaleSuperresScale = next.adaptiveUpscaleSuperresScale;
		currentTransport.upscaleLight = next.upscaleLight;
		currentTransport.adaptive = next.adaptive;
		currentTransport.debandGrain = next.debandGrain;
		currentTransport.debandVpExclusive = next.debandVpExclusive;
	}

	// Names of the VP-V settings that differ (appended after the host's own; the order differs from the old interleaved one).
	void AppendDelta(const VpvRenderer::Fields& current, const VpvRenderer::Fields& next, std::vector<const char*>& fields)
	{
		auto changed = [&fields](bool differs, const char* name)
		{
			if (differs)
				fields.push_back(name);
		};
		changed(current.saturation != next.saturation, "saturation");
		changed(current.captureUpload != next.captureUpload, "capture_upload");
		changed(current.sourceRange != next.sourceRange, "source_range");
		changed(current.sourceResolution != next.sourceResolution ||
			current.sourceResolutionAutoMinConfidence != next.sourceResolutionAutoMinConfidence ||
			current.sourceResolutionEvidenceShots != next.sourceResolutionEvidenceShots ||
			current.sourceResolutionEvidenceSeconds != next.sourceResolutionEvidenceSeconds, "source_resolution");
		changed(current.debandVp != next.debandVp || current.lowBitrateHandling != next.lowBitrateHandling, "deband_vp");
		changed(current.pipelineOriginalPathway != next.pipelineOriginalPathway, "pipeline_original_pathway");
		changed(current.pipelineOrder != next.pipelineOrder, "pipeline_order");
		changed(current.pipelineSlotsOff != next.pipelineSlotsOff, "pipeline_slots_off");
		changed(current.calibrationLutStage != next.calibrationLutStage, "calibration_lut_stage");
		changed(current.hdrInference != next.hdrInference, "hdr_inference");
		changed(current.pipelineEntranceOnce != next.pipelineEntranceOnce, "pipeline_entrance_once");
		changed(current.outputSegmentsMerged != next.outputSegmentsMerged, "output_segments_merged");
		changed(current.entranceGpuDrain != next.entranceGpuDrain, "entrance_gpu_drain");
		changed(current.adaptive.cnnResidualStabilise != next.adaptive.cnnResidualStabilise ||
			current.adaptive.ssimResidualStabilise != next.adaptive.ssimResidualStabilise, "residual_stabilise");
		changed(current.ssimBitrateAware != next.ssimBitrateAware || current.ssimBitrateFloor != next.ssimBitrateFloor, "ssim_bitrate_aware");
		changed(current.motionAheadFrames != next.motionAheadFrames, "motion_ahead_frames");
		changed(current.opticalFlow != next.opticalFlow, "optical_flow");
		changed(current.opticalFlowGlobal != next.opticalFlowGlobal, "optical_flow_global");
		changed(current.opticalFlowGlobalFallback != next.opticalFlowGlobalFallback, "optical_flow_global_fallback");
		changed(current.opticalFlowCostTrust != next.opticalFlowCostTrust, "optical_flow_cost_trust");
		changed(current.opticalFlowFbCheck != next.opticalFlowFbCheck, "optical_flow_fb_check");
		changed(current.interpolationGrain != next.interpolationGrain, "interpolation_grain");
		changed(current.dejudderSourceWarpCatmull != next.dejudderSourceWarpCatmull, "dejudder_source_warp_filter");
		changed(current.nlsSamplingCatmull != next.nlsSamplingCatmull, "nls_sampling");
		changed(current.sourceCodingFixes != next.sourceCodingFixes, "source_coding_fixes");
		changed(current.debandVpGrainGuard != next.debandVpGrainGuard || current.debandVpGrainSplit != next.debandVpGrainSplit, "deband_vp_grain_guard");
		changed(current.debandGrain != next.debandGrain, "deband_grain");
		changed(current.debandVpExclusive != next.debandVpExclusive, "deband_vp_exclusive");
		changed(current.detailPulseStabilise != next.detailPulseStabilise, "detail_pulse_stabilise");
		changed(current.detailPulseCap != next.detailPulseCap, "detail_pulse_cap");
		changed(current.detailPulseFast != next.detailPulseFast, "detail_pulse_fast");
		changed(current.textureRestore != next.textureRestore, "texture_restore");
		changed(current.textureRestoreCap != next.textureRestoreCap, "texture_restore_cap");
		changed(current.adaptive.residualGrainGuard != next.adaptive.residualGrainGuard, "residual_stabilise_grain_guard");
		changed(current.adaptive.motionAdaptive != next.adaptive.motionAdaptive, "adaptive_motion");
		changed(current.interpolation != next.interpolation, "interpolation");
		changed(current.interpolationFallback != next.interpolationFallback, "interpolation_fallback");
		changed(current.interpolationFillGaps != next.interpolationFillGaps, "interpolation_fill_gaps");
		changed(current.sourceClass != next.sourceClass, "source_class");
		changed(current.intentContent != next.intentContent ||
			current.intentContentHoldSeconds != next.intentContentHoldSeconds, "intent_content");
		changed(current.intentMotion != next.intentMotion || current.intentMotionPolicy != next.intentMotionPolicy ||
			current.intentMotionMin != next.intentMotionMin ||
			current.intentMotionMinShotSeconds != next.intentMotionMinShotSeconds ||
			current.intentEaseSeconds != next.intentEaseSeconds, "intent_motion");
		changed(current.ambientAdapt != next.ambientAdapt || current.ambientAdaptStrength != next.ambientAdaptStrength ||
			current.ambientScreenReflect != next.ambientScreenReflect || current.ambientMaxBlackNits != next.ambientMaxBlackNits ||
			current.ambientTauUpSeconds != next.ambientTauUpSeconds ||
			current.ambientTauDownSeconds != next.ambientTauDownSeconds, "ambient_adapt");
		changed(current.intentBlack != next.intentBlack, "intent_black");
		changed(current.intentDisplay != next.intentDisplay, "intent_display");
		changed(current.temporalDither != next.temporalDither, "temporal_dither");
		changed(current.rebaseKernelMatch != next.rebaseKernelMatch, "rebase_kernel_match");
		changed(current.sourceResolutionTextGuard != next.sourceResolutionTextGuard, "source_resolution_text_guard");
		changed(current.sourceResolutionMixedGuard != next.sourceResolutionMixedGuard, "source_resolution_mixed_guard");
		changed(current.sourceResolutionLatchFixes != next.sourceResolutionLatchFixes, "source_resolution_latch_fixes");
		changed(current.sourceResolutionCutFixes != next.sourceResolutionCutFixes, "source_resolution_cut_fixes");
		changed(current.activePictureStartupHold != next.activePictureStartupHold, "active_picture_startup_hold");
		changed(current.sourceResolutionEngageMaxWait != next.sourceResolutionEngageMaxWait ||
			current.sourceResolutionCrossfade != next.sourceResolutionCrossfade ||
			current.sourceResolutionReleaseFade != next.sourceResolutionReleaseFade, "source_resolution_crossfade_s");
		changed(current.pulldownFixes != next.pulldownFixes, "pulldown_fixes");
		changed(current.timingContinuityFixes != next.timingContinuityFixes, "timing_continuity_fixes");
		changed(current.cadenceClassFixes != next.cadenceClassFixes, "cadence_class_fixes");
		changed(current.motionHistoryFixes != next.motionHistoryFixes, "motion_history_fixes");
		changed(current.vectorDemand != next.vectorDemand, "vector_demand");
		changed(current.queueTimingFixes != next.queueTimingFixes, "queue_timing_fixes");
		changed(current.generalRateFixes != next.generalRateFixes, "general_rate_fixes");
		changed(current.efficiencyFixes != next.efficiencyFixes, "efficiency_fixes");
		changed(current.renderAhead != next.renderAhead, "render_ahead");
		changed(current.dejudderGate != next.dejudderGate, "dejudder_gate");
		changed(current.renderAheadUtilisation != next.renderAheadUtilisation, "render_ahead_utilisation");
		changed(current.renderAheadQueueCredit != next.renderAheadQueueCredit, "render_ahead_queue_credit");
		changed(current.dejudderStrength != next.dejudderStrength, "dejudder_strength");
		changed(current.dejudderStrengthEven != next.dejudderStrengthEven ||
			current.dejudderEvenIncludes1001 != next.dejudderEvenIncludes1001, "dejudder_strength_even");
		changed(current.dejudderMatchTolerance != next.dejudderMatchTolerance, "dejudder_match_tolerance");
		changed(current.dejudderFrameTrust != next.dejudderFrameTrust, "dejudder_frame_trust");
		changed(current.occlusionMask != next.occlusionMask, "occlusion_mask");
		changed(current.dejudderGlobalPrior != next.dejudderGlobalPrior, "dejudder_global_prior");
		changed(current.dejudderMotionScaleLo != next.dejudderMotionScaleLo, "dejudder_motion_scale_lo");
		changed(current.dejudderMotionScaleHi != next.dejudderMotionScaleHi, "dejudder_motion_scale_hi");
		changed(current.dejudderVectorSmooth != next.dejudderVectorSmooth, "dejudder_vector_smooth");
		changed(current.dejudderTrustScale != next.dejudderTrustScale, "dejudder_trust_scale");
		changed(current.dejudderOcclusionFill != next.dejudderOcclusionFill, "dejudder_occlusion_fill");
		changed(current.dejudderEstCandidates != next.dejudderEstCandidates ||
			current.dejudderEstLambda != next.dejudderEstLambda || current.dejudderEstQpel != next.dejudderEstQpel ||
			current.dejudderEstStillZero != next.dejudderEstStillZero ||
			current.dejudderEstGate != next.dejudderEstGate || current.dejudderEstSplit != next.dejudderEstSplit ||
			current.dejudderEstSplitSad != next.dejudderEstSplitSad, "dejudder_est");
		changed(current.dejudderLowRes != next.dejudderLowRes, "dejudder_low_res");
		changed(current.dejudderEstimateAhead != next.dejudderEstimateAhead, "dejudder_estimate_ahead");
		changed(current.capturePath != next.capturePath, "capture_path");
		changed(current.captureUpload != next.captureUpload, "capture_upload");
		changed(current.dejudderMotionRolloff != next.dejudderMotionRolloff || current.dejudderRollLo != next.dejudderRollLo ||
			current.dejudderRollHi != next.dejudderRollHi || current.dejudderRollFloor != next.dejudderRollFloor,
			"dejudder_motion_rolloff");
		changed(current.dejudderObjectStrength != next.dejudderObjectStrength, "dejudder_object_strength");
		changed(current.dejudderTextureTolerance != next.dejudderTextureTolerance || current.dejudderGlobalTwo != next.dejudderGlobalTwo ||
			current.dejudderRegionalHold != next.dejudderRegionalHold || current.dejudderCutHistogram != next.dejudderCutHistogram ||
			current.dejudderSearchRange != next.dejudderSearchRange || current.dejudderConsistencyPx != next.dejudderConsistencyPx ||
			current.dejudderFadeMatch != next.dejudderFadeMatch || current.dejudderFadeFloor != next.dejudderFadeFloor ||
			current.dejudderVerify != next.dejudderVerify || current.dejudderVerifyShare != next.dejudderVerifyShare ||
			current.dejudderPixelTrust != next.dejudderPixelTrust || current.dejudderPixelTrustLo != next.dejudderPixelTrustLo ||
			current.dejudderPixelTrustHi != next.dejudderPixelTrustHi ||
			current.dejudderLandingCheck != next.dejudderLandingCheck,
			"dejudder_motion_fixes");
		changed(current.dejudderPulldown != next.dejudderPulldown, "dejudder_pulldown");
		changed(current.nativeRgbIngest != next.nativeRgbIngest, "native_rgb_ingest");
		changed(current.dvTunnelGpuUnpack != next.dvTunnelGpuUnpack, "dv_tunnel_gpu_unpack");
		changed(current.dvTrims != next.dvTrims, "dv_trims");
		changed(current.dvToneMapping != next.dvToneMapping, "dv_tone_mapping");
		changed(current.dvCompareCaptureFolder != next.dvCompareCaptureFolder, "dv_compare_capture_folder");
		changed(current.dvTunnelFixes != next.dvTunnelFixes, "dv_tunnel_fixes");
		changed(current.dejudderHoldFeatherPx != next.dejudderHoldFeatherPx ||
			current.dejudderHoldHysteresis != next.dejudderHoldHysteresis ||
			current.dejudderBlockTaper != next.dejudderBlockTaper || current.dejudderTaperSad != next.dejudderTaperSad,
			"dejudder_hold");
		changed(current.ditherGrainToleranceScale != next.ditherGrainToleranceScale, "dither_grain_tolerance_scale");
		changed(current.ditherVectorsOwn != next.ditherVectorsOwn, "dither_vectors");
		changed(current.fboFloat32 != next.fboFloat32, "fbo_float32");
		changed(current.preserveExtendedRange != next.preserveExtendedRange, "preserve_extended_range");
		changed(current.extendedRangeMoreCases != next.extendedRangeMoreCases, "extended_range_more_cases");
		changed(current.sdrTone.knee != next.sdrTone.knee || current.sdrTone.headroom != next.sdrTone.headroom ||
			current.sdrTone.kneeStart != next.sdrTone.kneeStart || current.sdrTone.kneeStrength != next.sdrTone.kneeStrength ||
			current.sdrTone.recovery != next.sdrTone.recovery, "sdr_tone_shape");
		changed(current.dejudderWarpRendered != next.dejudderWarpRendered, "dejudder_warp_stage");
		changed(current.downscaleLight != next.downscaleLight, "downscale_light");
		changed(current.upscaleLight != next.upscaleLight, "upscale_light");
		changed(current.adaptiveUpscale != next.adaptiveUpscale ||
			current.adaptiveUpscaleWBase != next.adaptiveUpscaleWBase ||
			current.adaptiveUpscaleSuperresScale != next.adaptiveUpscaleSuperresScale, "adaptive_upscale");
		changed(current.adaptiveDownscale != next.adaptiveDownscale ||
			!(current.adaptive == next.adaptive), "adaptive_downscale");
		// The host's "deband" entry covers deband_strength / deband; deband_grain is a VP-V setting of the same entry.
		if (current.debandGrain != next.debandGrain &&
			std::none_of(fields.begin(), fields.end(), [](const char* name) { return std::strcmp(name, "deband") == 0; }))
			fields.push_back("deband");
	}

	// The VP-V part of the settings test export (VideoProcessorLibplaceboDescribeSettingsForTest): key=value lines of the
	// settings a configuration activates and of the matcher's settings; `motionFingerprint` is the host's motion setting.
	std::string Describe(const VpvRenderer::Fields& s, const std::string& motionFingerprint, const Dejudder::Settings& m,
		const std::string& rule)
	{
		std::ostringstream o;
	o << "rule=" << rule << "\n"
		<< "capture_path=" << s.capturePath << "\n"
		<< "capture_upload=" << s.captureUpload << "\n"
		<< "source_range=" << s.sourceRange << "\n"
		<< "source_resolution=" << s.sourceResolution << "\n"
		<< "source_resolution_auto_min_confidence=" << s.sourceResolutionAutoMinConfidence << "\n"
		<< "source_resolution_evidence_shots=" << s.sourceResolutionEvidenceShots << "\n"
		<< "source_resolution_evidence_seconds=" << s.sourceResolutionEvidenceSeconds << "\n"
		<< "deband_vp=" << s.debandVp << "\n"
		<< "low_bitrate_handling=" << s.lowBitrateHandling << "\n"
		<< "cnn_residual_stabilise=" << (s.adaptive.cnnResidualStabilise ? "on" : "off") << "\n"
		<< "ssim_residual_stabilise=" << (s.adaptive.ssimResidualStabilise ? "on" : "off") << "\n"
		<< "ssim_bitrate_aware=" << (s.ssimBitrateAware ? "on" : "off") << "\n"
		<< "ssim_bitrate_floor=" << s.ssimBitrateFloor << "\n"
		<< "motion_ahead_frames=" << s.motionAheadFrames << "\n"
		<< "optical_flow=" << (s.opticalFlow ? "on" : "off") << "\n"
		<< "optical_flow_global=" << (s.opticalFlowGlobal ? "on" : "off") << "\n"
		<< "optical_flow_global_fallback=" << (s.opticalFlowGlobalFallback ? "on" : "off") << "\n"
		<< "optical_flow_cost_trust=" << (s.opticalFlowCostTrust ? "on" : "off") << "\n"
		<< "optical_flow_fb_check=" << (s.opticalFlowFbCheck ? "on" : "off") << "\n"
		<< "motion=" << motionFingerprint << "\n"
		<< "interpolation_grain=" << InterpolationPolicy::Name(s.interpolationGrain) << "\n"
		<< "dejudder_source_warp_filter=" << (s.dejudderSourceWarpCatmull ? "catmull" : "bilinear") << "\n"
		<< "nls_sampling=" << (s.nlsSamplingCatmull ? "catmull" : "bilinear") << "\n"
		<< "source_coding_fixes=" << (s.sourceCodingFixes ? "on" : "off") << "\n"
		<< "deband_vp_grain_guard=" << (!s.debandVpGrainGuard ? "off" : s.debandVpGrainSplit ? "on" : "veto") << "\n"
		<< "deband_vp_exclusive=" << (s.debandVpExclusive ? "on" : "off") << "\n"
		<< "detail_pulse_stabilise=" << (s.detailPulseStabilise ? "on" : "off") << "\n"
		<< "detail_pulse_cap=" << s.detailPulseCap << "\n"
		<< "detail_pulse_fast=" << (s.detailPulseFast ? "on" : "off") << "\n"
		<< "texture_restore=" << (s.textureRestore ? "statistical" : "off") << "\n"
		<< "texture_restore_cap=" << s.textureRestoreCap << "\n"
		<< "deband_grain=" << (s.debandGrain ? "on" : "off") << "\n"
		<< "residual_stabilise_grain_guard=" << (s.adaptive.residualGrainGuard ? "on" : "off") << "\n"
		<< "adaptive_motion=" << (s.adaptive.motionAdaptive ? "on" : "off") << "\n"
		<< "adaptive_motion_speed_lo=" << s.adaptive.motionSpeedLo << "\n"
		<< "adaptive_motion_speed_hi=" << s.adaptive.motionSpeedHi << "\n"
		<< "adaptive_motion_floor=" << s.adaptive.motionFloor << "\n"
		<< "adaptive_motion_smooth_ms=" << s.adaptive.motionSmoothMs << "\n"
		<< "interpolation=" << InterpolationPolicy::Name(s.interpolation) << "\n"
		<< "interpolation_fallback=" << InterpolationPolicy::Name(s.interpolationFallback) << "\n"
		<< "interpolation_fill_gaps=" << (s.interpolationFillGaps ? "on" : "off") << "\n"
		<< "source_class=" << InterpolationPolicy::Name(s.sourceClass) << "\n"
		<< "intent_content=" << (s.intentContent ? "on" : "off") << "\n"
		<< "intent_content_hold_seconds=" << s.intentContentHoldSeconds << "\n"
		<< "intent_motion=" << (s.intentMotion ? "on" : "off") << "\n"
		<< "intent_motion_policy=" << ToString(s.intentMotionPolicy) << "\n"
		<< "intent_motion_min=" << s.intentMotionMin << "\n"
		<< "intent_motion_min_shot_s=" << s.intentMotionMinShotSeconds << "\n"
		<< "intent_ease_s=" << s.intentEaseSeconds << "\n"
		<< "ambient_adapt=" << (s.ambientAdapt ? "on" : "off") << "\n"
		<< "ambient_adapt_strength=" << s.ambientAdaptStrength << "\n"
		<< "ambient_screen_reflect=" << s.ambientScreenReflect << "\n"
		<< "ambient_max_black_nits=" << s.ambientMaxBlackNits << "\n"
		<< "ambient_tau_up_s=" << s.ambientTauUpSeconds << "\n"
		<< "ambient_tau_down_s=" << s.ambientTauDownSeconds << "\n"
		<< "intent_black=" << ToString(s.intentBlack) << "\n"
		<< "intent_display=" << (s.intentDisplay ? "report" : "off") << "\n"
		<< "temporal_dither=" << s.temporalDither << "\n"
		<< "rebase_kernel_match=" << (s.rebaseKernelMatch ? "on" : "off") << "\n"
		<< "source_resolution_text_guard=" << (s.sourceResolutionTextGuard ? "on" : "off") << "\n"
		<< "source_resolution_mixed_guard=" << (s.sourceResolutionMixedGuard ? "on" : "off") << "\n"
		<< "source_resolution_latch_fixes=" << (s.sourceResolutionLatchFixes ? "on" : "off") << "\n"
		<< "source_resolution_cut_fixes=" << (s.sourceResolutionCutFixes ? "on" : "off") << "\n"
		<< "active_picture_startup_hold=" << (s.activePictureStartupHold ? "on" : "off") << "\n"
		<< "source_resolution_engage_max_wait_s=" << s.sourceResolutionEngageMaxWait << "\n"
		<< "source_resolution_crossfade_s=" << s.sourceResolutionCrossfade << "\n"
		<< "source_resolution_release_fade_s=" << s.sourceResolutionReleaseFade << "\n"
		<< "pulldown_fixes=" << (s.pulldownFixes ? "on" : "off") << "\n"
		<< "timing_continuity_fixes=" << (s.timingContinuityFixes ? "on" : "off") << "\n"
		<< "cadence_class_fixes=" << (s.cadenceClassFixes ? "on" : "off") << "\n"
		<< "motion_history_fixes=" << (s.motionHistoryFixes ? "on" : "off") << "\n"
		<< "queue_timing_fixes=" << (s.queueTimingFixes ? "on" : "off") << "\n"
		<< "general_rate_fixes=" << (s.generalRateFixes ? "on" : "off") << "\n"
		<< "efficiency_fixes=" << (s.efficiencyFixes ? "on" : "off") << "\n"
		<< "render_ahead=" << (s.renderAhead ? "on" : "off") << "\n"
		<< "dejudder_gate=" << s.dejudderGate << "\n"
		<< "render_ahead_utilisation=" << s.renderAheadUtilisation << "\n"
		<< "render_ahead_queue_credit=" << (s.renderAheadQueueCredit ? "on" : "off") << "\n"
		<< "dejudder_strength=" << s.dejudderStrength << "\n"
		<< "dejudder_strength_even=" << s.dejudderStrengthEven << "\n"
		<< "dejudder_even_includes_1001=" << s.dejudderEvenIncludes1001 << "\n"
		<< "dejudder_warp_rendered=" << s.dejudderWarpRendered << "\n"
		<< "dejudder_estimate_ahead=" << s.dejudderEstimateAhead << "\n"
		<< "dejudder_pulldown=" << s.dejudderPulldown << "\n"
		<< "native_rgb_ingest=" << s.nativeRgbIngest << "\n"
		<< "dv_tunnel_gpu_unpack=" << s.dvTunnelGpuUnpack << "\n"
		<< "dv_trims=" << s.dvTrims << "\n"
		<< "dv_tone_mapping=" << s.dvToneMapping << "\n"
		<< "dv_compare_capture_folder=" << s.dvCompareCaptureFolder << "\n"
		<< "dv_tunnel_fixes=" << (s.dvTunnelFixes ? "on" : "off") << "\n"
		<< "hdr_inference=" << s.hdrInference << "\n"
		<< "occlusion_mask=" << s.occlusionMask << "\n"
		<< "dither_vectors_own=" << s.ditherVectorsOwn << "\n"
		<< "dither_grain_tolerance_scale=" << s.ditherGrainToleranceScale << "\n"
		<< "sdr_knee=" << s.sdrTone.knee << "\n"
		<< "sdr_headroom=" << s.sdrTone.headroom << "\n"
		<< "sdr_knee_start=" << s.sdrTone.kneeStart << "\n"
		<< "sdr_knee_strength=" << s.sdrTone.kneeStrength << "\n"
		<< "sdr_recovery=" << s.sdrTone.recovery << "\n"
		<< "adaptive_downscale=" << s.adaptiveDownscale << "\n"
		<< "adaptive_upscale=" << s.adaptiveUpscale << "\n"
		<< "upscale_light=" << s.upscaleLight << "\n"
		<< "fbo_float32=" << s.fboFloat32 << "\n"
		<< "matcher.matchTolerance=" << m.matchTolerance << "\n"
		<< "matcher.minTrustedShare=" << m.minTrustedShare << "\n"
		<< "matcher.globalPrior=" << m.globalPrior << "\n"
		<< "matcher.vectorSmooth=" << m.vectorSmooth << "\n"
		<< "matcher.motionScaleLo=" << m.motionScaleLo << "\n"
		<< "matcher.motionScaleHi=" << m.motionScaleHi << "\n"
		<< "matcher.trustScaleFull=" << m.trustScaleFull << "\n"
		<< "matcher.occlusionFill=" << m.occlusionFill << "\n"
		<< "matcher.estCandidates=" << m.estCandidates << "\n"
		<< "matcher.opticalFlow=" << m.opticalFlow << "\n"
		<< "matcher.estLambda=" << m.estLambda << "\n"
		<< "matcher.estQpel=" << m.estQpel << "\n"
		<< "dejudder_est_still_zero=" << m.estStillZero << "\n"
		<< "entrance_gpu_drain=" << s.entranceGpuDrain << "\n"
		<< "matcher.estGate=" << m.estGate << "\n"
		<< "matcher.estSplit=" << m.estSplit << "\n"
		<< "matcher.estSplitSad=" << m.estSplitSad << "\n"
		<< "matcher.motionRolloff=" << m.motionRolloff << "\n"
		<< "matcher.rollLo=" << m.rollLo << "\n"
		<< "matcher.rollHi=" << m.rollHi << "\n"
		<< "matcher.rollFloor=" << m.rollFloor << "\n"
		<< "matcher.objectStrength=" << m.objectStrength << "\n"
		<< "matcher.textureTolerance=" << m.textureTolerance << "\n"
		<< "matcher.globalTwo=" << m.globalTwo << "\n"
		<< "matcher.regionalHold=" << m.regionalHold << "\n"
		<< "matcher.cutHistogram=" << m.cutHistogram << "\n"
		<< "matcher.coarseRange=" << m.coarseRange << "\n"
		<< "matcher.consistencyPixels=" << m.consistencyPixels << "\n"
		<< "matcher.fadeMatch=" << m.fadeMatch << "\n"
		<< "matcher.fadeFloor=" << m.fadeFloor << "\n"
		<< "matcher.verifyPixels=" << m.verifyPixels << "\n"
		<< "matcher.verifyShare=" << m.verifyShare << "\n"
		<< "matcher.pixelTrust=" << m.pixelTrust << "\n"
		<< "matcher.pixelTrustLo=" << m.pixelTrustLo << "\n"
		<< "matcher.pixelTrustHi=" << m.pixelTrustHi << "\n"
		<< "matcher.landingCheck=" << m.landingCheck << "\n"
		<< "matcher.pictureArea=" << m.pictureArea << "\n"
		<< "matcher.lowRes=" << m.lowRes << "\n"
		<< "matcher.lowResAll=" << m.lowResAll << "\n"
		<< "matcher.holdFeatherPx=" << m.holdFeatherPx << "\n"
		<< "matcher.holdHysteresis=" << m.holdHysteresis << "\n"
		<< "matcher.blockTaper=" << m.blockTaper << "\n"
		<< "matcher.taperSad=" << m.taperSad << "\n";
		return o.str();
	}

	// @@CPP-END
}
