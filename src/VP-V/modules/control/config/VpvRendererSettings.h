// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VP-V renderer settings, moved out of vprenderer/LibplaceboVideoRenderer.cpp (M2 P3). Pure functions
// over VpvRenderer::Fields (the VP-V members of the host's settings struct, VpvRendererFields.h); the host
// reaches them only through the generic settings hooks (vprenderer/RendererSettingsExtension.h,
// RendererExtension::SettingsLoad), which the settings module in VpvSettingsModule.cpp serves.
// No state, no locks: callers are the GUI thread (candidate load), the render thread under its
// render mutex (live apply) and the test exports.

#include "VpvRendererFields.h"
#include <VP-V/modules/motion/estimation/MatcherSettings.h>

#include <string>
#include <vector>

class ConfigFile;

namespace VpvRendererSettings
{
	// One applied rule's VP-V profile keys, after the host's own reads of that rule section.
	void ReadProfile(const ConfigFile& config, const std::string& ruleSection, const std::string& ruleName,
		bool ruleContentGated, VpvRenderer::Fields& settings);

	// Values VP-V decides for settings the HOST owns (either/or): the host applies them after PostLoad.
	struct Overrides
	{
		bool presentEveryVsync = false;          // the host's present_every_vsync after the presentation-owner decision
		const char* chromaUpscaler = nullptr;    // Original pathway: the host's chroma_upscaler (nullptr = keep)
	};

	// The VP-V keys the host reads before any profile rule (flat / [display] level).
	void ReadBase(const ConfigFile& config, VpvRenderer::Fields& settings);

	// The global pipeline keys ([vpv] first, then [vprenderer]).
	void ReadGlobal(const ConfigFile& config, VpvRenderer::Fields& settings);

	// The Original pathway overlay, the slot switches and the presentation owner; presentEveryVsyncRequested
	// is the host's own present_every_vsync before the decision.
	Overrides PostLoad(VpvRenderer::Fields& settings, bool presentEveryVsyncRequested);

	// deband_vp_exclusive: the host's automatic deband yields to VP-V's debander when that runs (deband_vp
	// auto / on) with its grain guard; an explicit deband_strength or deband stays. `S` is the host settings.
	template <class S> inline bool DebandYieldsToVp(const S& settings)
	{
		return settings.debandVpExclusive && settings.debandVp != "off" && settings.debandVpGrainGuard &&
			settings.debandStrength == "auto" && settings.deband == decltype(settings.deband)::AUTO;
	}

	// The VP-V terms of the effective-settings fingerprint, appended after the host's own (begins with '|').
	std::string Fingerprint(const VpvRenderer::Fields& settings, bool includeLive);

	// Names of the VP-V settings that differ between two settings (the host's live-versus-restart diff).
	void AppendDelta(const VpvRenderer::Fields& current, const VpvRenderer::Fields& next, std::vector<const char*>& fields);

	// The live-applied VP-V settings: currentTransport takes next's.
	void CopyLive(VpvRenderer::Fields& currentTransport, const VpvRenderer::Fields& next);

	// Settings test export body: key=value lines (see VideoProcessorLibplaceboDescribeSettingsForTest).
	std::string Describe(const VpvRenderer::Fields& s, const std::string& motionFingerprint, const Dejudder::Settings& m,
		const std::string& rule);

	// @@HEADER-END
}
