// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Q2-B3: load-time migration of cnn_residual_stabilise -> sharp_residual_stabilise across
// [vprenderer] and [vprenderer.*] sections. Preserves an explicit sharp_residual_stabilise
// value if already set, removes cnn_residual_stabilise, and returns true iff any change was made.
// Header-only pure C++14 helper plus ConfigDocument declaration (defined in Migrations.cpp).
#include <cctype>
#include <map>
#include <string>

namespace ConfigEditorCore { struct ConfigDocument; }

namespace Vpv
{
	bool MigrateSharpResidualStabilise(ConfigEditorCore::ConfigDocument& document);

	// Pure section-map form (unit tests without Qt / ConfigEditorCore): applies the exact same
	// precedence rule (existing sharp_residual_stabilise wins over legacy cnn_residual_stabilise).
	inline bool MigrateSharpResidualStabiliseSection(std::map<std::string, std::string>& entries)
	{
		auto norm = [](const std::string& s) {
			std::string out;
			out.reserve(s.size());
			for (char c : s)
				out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			return out;
		};
		std::string legacyKey;
		std::string legacyValue;
		bool hasExplicitSharp = false;
		for (const auto& kv : entries)
		{
			const std::string nk = norm(kv.first);
			if (nk == "cnn_residual_stabilise")
			{
				legacyKey = kv.first;
				legacyValue = kv.second;
			}
			else if (nk == "sharp_residual_stabilise" && !kv.second.empty())
			{
				hasExplicitSharp = true;
			}
		}
		if (legacyKey.empty())
			return false;
		if (!hasExplicitSharp && !legacyValue.empty())
			entries["sharp_residual_stabilise"] = legacyValue;
		for (auto it = entries.begin(); it != entries.end(); )
		{
			if (norm(it->first) == "cnn_residual_stabilise")
				it = entries.erase(it);
			else
				++it;
		}
		return true;
	}
}
