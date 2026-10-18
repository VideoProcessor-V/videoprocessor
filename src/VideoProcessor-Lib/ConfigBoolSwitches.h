#pragma once

#include <string>
#include <vector>

// Bug hunt 3 (CFG1): the [general] / [command_line] booleans VP turns into
// command-line switches at start-up. A switch alone means true; the parser
// (ReadBooleanOption) also takes an explicit value after it. The old helper
// emitted only true, so a key whose default is ON (newlldv_sdr_veto) could not
// be turned off from the configuration. Every such default-on key is listed
// here with defaultOn = true and gets "<switch> false" when configured false.
//
// Audit (2026-10-18), the bools that go through the helper and their defaults:
//   fullscreen               off  (StartFullScreen)
//   windowedfullscreenmode   off  (m_windowedFullScreenMode)
//   scene_detect             off  (SceneDetect)
//   scene_correction_basic   off  (the advanced correction is the default; a
//                                  false must not override scene_correction_mode)
//   newlldv                  off  (EnableNewLldvHeuristic)
//   newlldv_sdr_veto         ON   (HdrInferenceOptions::sdrVeto, m_hdrInferenceSdrVeto)
//   dolby_vision_tunnel      off  (m_enableDolbyVisionTunnel)
//   noui                     off  (m_hideUI)
//   startminimized           off  (m_startMinimized)
// A new default-on bool (dv_tunnel_handoff when merged) must be added with
// defaultOn = true; ConfigBoolSwitchesTests checks the list.
namespace ConfigBoolSwitches
{
	struct Option
	{
		// The key and its alias (nullptr when none), in lookup order.
		const char* keys[2];
		const wchar_t* commandLineSwitch;
		bool defaultOn;
	};

	inline const std::vector<Option>& Options()
	{
		static const std::vector<Option> options = {
			{ { "fullscreen", nullptr }, L"/fullscreen", false },
			{ { "windowedfullscreenmode", "windowed_fullscreen_mode" }, L"/windowedfullscreenmode", false },
			{ { "scene_detect", "scene" }, L"/scene_detect", false },
			{ { "scene_correction_basic", nullptr }, L"/scene_correction_basic", false },
			{ { "newlldv", "new_lldv" }, L"/newlldv", false },
			{ { "newlldv_sdr_veto", nullptr }, L"/newlldv_sdr_veto", true },
			{ { "dolby_vision_tunnel", nullptr }, L"/dolby_vision_tunnel", false },
			{ { "noui", "no_ui" }, L"/noui", false },
			{ { "startminimized", "start_minimized" }, L"/startminimized", false },
		};
		return options;
	}

	inline const Option* Find(const char* key)
	{
		for (const Option& option : Options())
			for (const char* name : option.keys)
				if (name && std::string(name) == key) return &option;
		return nullptr;
	}

	// The tokens a configured value adds: true -> the switch; false -> the
	// switch and "false" for a default-on key (nothing for a default-off one,
	// which is already off and must not override an older alias).
	inline std::vector<std::wstring> Tokens(const Option& option, bool configured, bool value)
	{
		std::vector<std::wstring> tokens;
		if (!configured) return tokens;
		if (value)
			tokens.emplace_back(option.commandLineSwitch);
		else if (option.defaultOn)
		{
			tokens.emplace_back(option.commandLineSwitch);
			tokens.emplace_back(L"false");
		}
		return tokens;
	}
}
