// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance (CF-1 CAM-4): the one status sentence the Config card shows for the appearance setting, built from the
// setting values alone (pure, no Windows, no clock). The OFF state is shown (never silent); the degraded states (shader
// missing, sensor lost, L11 reference mode, ambient_adapt replaced) are FallbackCatalog rows (Ctrl+I "Fallbacks engaged").
//
// General, not fixed: nothing here names a display, a rate or a resolution.

#include <string>

namespace AppearanceStatus
{
	inline std::string ConfigLine(bool on, bool screenWhiteMeasured, bool ambientAdaptRequested)
	{
		if (!on)
			return "appearance: off";
		std::string line = "appearance on: replaces the gamma boost and saturation steps";
		if (!screenWhiteMeasured)
			line += " (screen white not measured: the configured value is used)";
		if (ambientAdaptRequested)
			line += "; room-light adaptation (ambient_adapt) is replaced by appearance";
		return line;
	}
}
