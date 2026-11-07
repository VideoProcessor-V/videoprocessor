// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Test support for the Config coverage test (ConfigEditorWindowTests.cpp): the profile keys the VP-V schema
// header validates (M2 P3-7 moved them out of RendererProfileConfig.h into VpvSchemaKeys.h; its whole text is
// validator code, so every `key == "x"` counts). Kept here so the host-side test file stays small.

#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

namespace SchemaKeyScan
{
	// `srcRoot` ends with a path separator (the repository's src folder). Empty when the header is not found.
	inline std::set<std::string> VpvProfileKeys(const std::string& srcRoot)
	{
		std::ifstream file(srcRoot + "VP-V/modules/control/config/VpvSchemaKeys.h", std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::set<std::string> keys;
		static const std::regex marker(R"re(\bkey\s*==\s*"([^"]+)")re");
		for (std::sregex_iterator it(text.begin(), text.end(), marker), last; it != last; ++it)
			keys.insert((*it)[1].str());
		return keys;
	}
}
