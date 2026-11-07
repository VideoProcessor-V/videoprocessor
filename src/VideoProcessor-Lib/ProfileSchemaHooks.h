// VPV-HOOK-FILE
/*
 * Copyright(C) 2025 Bill Slack <bslack@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

// Renderer-profile schema extension point (H2), included by RendererProfileConfig.h inside namespace
// RendererProfileConfig after the Is*/Parse* helpers. A build that defines PROFILE_SCHEMA_EXTENSION_HEADER
// (the VP-V Directory.Build.targets) includes that header, which defines ProfileSchemaHooks: the setting
// keys it owns per profile group and the [vpv] section check. Without the define nothing is claimed and
// the host's validators answer alone.
#ifdef PROFILE_SCHEMA_EXTENSION_HEADER
#include PROFILE_SCHEMA_EXTENSION_HEADER
#else
#pragma detect_mismatch("vpv_schema_hooks", "0")
namespace ProfileSchemaHooks
{
	enum class Verdict { NotMine, Valid, Invalid };
	inline Verdict Validate(const std::string&, const std::string&, const std::string&) { return Verdict::NotMine; }
	inline bool ValidateSections(const ConfigFile&, std::string&) { return true; }
}
#endif
