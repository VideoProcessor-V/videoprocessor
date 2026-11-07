// VPV-HOOK-FILE
/*
 * Copyright(C) 2025 Bill Slack <bslack@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

// Settings extension point (H2). The renderer settings struct derives from RendererSettingsExtension,
// so a build that defines RENDERER_SETTINGS_EXTENSION_HEADER (the VP-V props) adds that header's members
// to every settings object; without the define the base is empty and the struct is exactly the host's.
#ifdef RENDERER_SETTINGS_EXTENSION_HEADER
#include RENDERER_SETTINGS_EXTENSION_HEADER
#else
struct RendererSettingsExtension {};
#endif

#include <vprenderer/RendererExtension.h>

#include <string>
#include <vector>

// The renderer's settings-load hook calls, one line each at the host's own positions. Every call goes
// through RendererExtension::SettingsLoad, so with nothing registered each is a no-op (and ApplyGlobal
// changes nothing), and the host's behaviour is exactly its own.
namespace RendererSettingsHooks
{
	namespace RE = RendererExtension;

	// The keys read before any profile rule (flat / display level).
	inline void ReadBase(const ConfigFile& config, RendererSettingsExtension& settings) noexcept
	{
		RE::SettingsLoadContext c; c.phase = RE::SettingsPhase::ReadBase; c.config = &config; c.settings = &settings;
		RE::SettingsLoad(c);
	}

	// One applied rule's keys, after the host's own reads of that rule section.
	inline void ReadProfile(const ConfigFile& config, const std::string& section, const std::string& rule, bool contentGated,
		RendererSettingsExtension& settings) noexcept
	{
		RE::SettingsLoadContext c; c.phase = RE::SettingsPhase::ReadProfile; c.config = &config; c.section = &section; c.rule = &rule;
		c.settings = &settings; c.flag = contentGated;
		RE::SettingsLoad(c);
	}

	// The global keys, then the extension's overlay; `S` is the host settings (derived from
	// RendererSettingsExtension) with presentEveryVsync and chromaUpscaler, which an extension may decide.
	template <class S> inline void ApplyGlobal(const ConfigFile& config, S& settings) noexcept
	{
		RendererSettingsExtension& extension = settings;
		RE::SettingsLoadContext c; c.phase = RE::SettingsPhase::ReadGlobal; c.config = &config; c.settings = &extension;
		RE::SettingsLoad(c);
		RE::SettingsOverrides overrides;
		c.phase = RE::SettingsPhase::PostLoad; c.flag = settings.presentEveryVsync; c.result = &overrides;
		RE::SettingsLoad(c);
		if (overrides.hasPresentEveryVsync) settings.presentEveryVsync = overrides.presentEveryVsync;
		if (overrides.chromaUpscaler) settings.chromaUpscaler = overrides.chromaUpscaler;
	}

	// The extension's terms of the effective-settings fingerprint, appended after the host's own.
	inline std::string Fingerprint(const RendererSettingsExtension& settings, bool includeLive) noexcept
	{
		std::string out;
		RE::SettingsLoadContext c; c.phase = RE::SettingsPhase::Fingerprint; c.other = &settings; c.out = &out; c.includeLive = includeLive;
		RE::SettingsLoad(c);
		return out;
	}

	// Names of the extension's settings that differ (appended to the host's own list); the extension only reads.
	inline void AppendDelta(const RendererSettingsExtension& current, const RendererSettingsExtension& next,
		std::vector<const char*>& changed) noexcept
	{
		RE::SettingsLoadContext c; c.phase = RE::SettingsPhase::Delta; c.settings = const_cast<RendererSettingsExtension*>(&current);
		c.other = &next; c.changed = &changed;
		RE::SettingsLoad(c);
	}

	// The live-applied settings: dst takes src's.
	inline void CopyLive(RendererSettingsExtension& dst, const RendererSettingsExtension& src) noexcept
	{
		RE::SettingsLoadContext c; c.phase = RE::SettingsPhase::CopyLive; c.settings = &dst; c.other = &src;
		RE::SettingsLoad(c);
	}
}
