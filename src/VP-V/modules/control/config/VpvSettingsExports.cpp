// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>

#include <algorithm>
#include <cstring>
#include <string>

// Tests only (M2 P3-0 identity kit): defined in LibplaceboVideoRenderer.cpp, which alone can see the
// renderer's settings struct.
std::string LibplaceboRendererSettingsDumpForTest(const char* configPath);

// The settings dump of a configuration file (every field of RendererSettings, then both effective
// fingerprints) for RendererSettingsIdentityTests; returns its length (0 = not loaded), copying at
// most size - 1 characters.
extern "C" __declspec(dllexport) int __cdecl
VideoProcessorLibplaceboSettingsDumpForTest(const char* configPath, char* out, int size)
{
	try
	{
		const std::string dump = LibplaceboRendererSettingsDumpForTest(configPath);
		if (out && size > 0)
		{
			const size_t n = (std::min)(dump.size(), static_cast<size_t>(size - 1));
			memcpy(out, dump.data(), n);
			out[n] = 0;
		}
		return static_cast<int>(dump.size());
	}
	catch (...)
	{
		if (out && size > 0)
			out[0] = 0;
		return 0;
	}
}
