// VPV-HOOK-FILE
/*
 * Copyright(C) 2025 Bill Slack <bslack@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

#include "ConfigSchema.h"

#include <string>
#include <vector>

// Startup-schema extension point (H2). A build that defines MAIN_SCHEMA_EXTENSION_HEADER (the VP-V
// Directory.Build.targets) includes that header, which defines MainConfigSchema::SchemaHooks (extra
// sections, keys appended after the host's positional rule lists, extra validation). Without the define
// the hooks below own and add nothing, so every result is exactly the host's.
#ifdef MAIN_SCHEMA_EXTENSION_HEADER
#include MAIN_SCHEMA_EXTENSION_HEADER
#else
#pragma detect_mismatch("vpv_schema_hooks", "0")
namespace MainConfigSchema
{
	namespace SchemaHooks
	{
		inline bool OwnsSection(const std::string&) { return false; }
		inline bool OwnsRendererSection(const std::string&) { return false; }
		inline void AppendRules(const char*, std::vector<ConfigSchema::KeyRule>&) {}
		inline bool Validate(const ConfigFile&, std::string&) { return true; }
	}
}
#endif
