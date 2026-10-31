// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Dithering choice sets for the Config editor's Scaling page (owner decision, v1.4.02 merge:
// keep BOTH).
//   Original pathway (pipeline_original_pathway: on): Bill's control exactly - blue_noise,
//     ordered_fixed, white_noise, off - with his display mapping of auto/on/true.
//   VP-V mode (default): VP-V's full list, exactly as before the merge, including Auto,
//     ordered_lut and the error_diffusion_* modes. A VP-V value is never rewritten, flagged
//     as retired or marked dirty by Bill's retired-value display rule.
// Pure C++17 (no Qt) so the sets are tested without a window.

#include <cctype>
#include <string>
#include <vector>

namespace Vpv
{
	struct DitherChoice
	{
		const char* data;   // the value written to the file
		const char* label;  // VP-V mode label
	};

	// VP-V's list (order as in the editor before the merge).
	inline const std::vector<DitherChoice>& VpvDitherChoices()
	{
		static const std::vector<DitherChoice> choices = {
			{ "AUTO", "Auto" },
			{ "blue_noise", "Blue noise" },
			{ "ordered_lut", "Ordered (LUT)" },
			{ "ordered_fixed", "Ordered (fixed)" },
			{ "white_noise", "White noise" },
			{ "error_diffusion_simple", "Error diffusion: Simple" },
			{ "error_diffusion_false_fs", "Error diffusion: False Floyd-Steinberg" },
			{ "error_diffusion_sierra_lite", "Error diffusion: Sierra Lite" },
			{ "error_diffusion_floyd_steinberg", "Error diffusion: Floyd-Steinberg" },
			{ "error_diffusion_atkinson", "Error diffusion: Atkinson" },
			{ "error_diffusion_jarvis_judice_ninke", "Error diffusion: Jarvis-Judice-Ninke" },
			{ "error_diffusion_stucki", "Error diffusion: Stucki" },
			{ "error_diffusion_burkes", "Error diffusion: Burkes" },
			{ "error_diffusion_sierra2", "Error diffusion: Sierra 2" },
			{ "error_diffusion_sierra3", "Error diffusion: Sierra 3" },
			{ "off", "Off" } };
		return choices;
	}

	// Bill's Scaling-page set (labels come from the editor's own choice labels).
	inline const std::vector<const char*>& OriginalDitherChoices()
	{
		static const std::vector<const char*> choices = { "blue_noise", "ordered_fixed", "white_noise", "off" };
		return choices;
	}

	inline std::string LowerAscii(std::string text)
	{
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	inline bool IsOriginalDither(const std::string& value)
	{
		const std::string v = LowerAscii(value);
		for (const char* choice : OriginalDitherChoices()) if (v == choice) return true;
		return false;
	}

	// Bill's display mapping (Original pathway only): the retired spellings read as blue noise.
	// Display only: the stored value is never rewritten from here.
	inline bool IsOriginalRetiredAlias(const std::string& value)
	{
		const std::string v = LowerAscii(value);
		return v == "auto" || v == "on" || v == "true";
	}
}
