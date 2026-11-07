// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The settings module (M2 P3): serves the host's settings-load hooks (RendererExtension::SettingsLoad,
// vprenderer/RendererSettingsExtension.h) with the VP-V renderer settings (VpvRendererSettings.h).
// Registered first with the skeleton hub, so every later module sees settings that are already complete.

namespace Vpv
{
	namespace Settings
	{
		// Idempotent; called by the hub bootstrap at module load, before any renderer thread exists.
		void RegisterSettingsModule();
	}
}
