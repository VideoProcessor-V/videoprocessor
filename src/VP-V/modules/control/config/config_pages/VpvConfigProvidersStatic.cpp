// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S4A A4 (refs/112 pattern P4, static registration): compiled into the VideoProcessor-Config executable only
// (src/VP-V/build/VideoProcessor-Config.items.props), so AppMain.cpp carries no include and no call for it.
// RegisterConfigProviders is idempotent, its page instances are function-local statics and the registry is a
// function-local static, so running it during static initialization is safe and happens before wWinMain
// creates the first editor, as the call at the top of the start-up path did. ConfigTests does not compile this
// file and keeps its explicit call.
#include <VP-V/modules/control/config/config_pages/VpvConfigProviders.h>

namespace
{
	struct ConfigProvidersRegistration
	{
		ConfigProvidersRegistration() { Vpv::RegisterConfigProviders(); }
	};
	const ConfigProvidersRegistration g_configProvidersRegistration;
}
