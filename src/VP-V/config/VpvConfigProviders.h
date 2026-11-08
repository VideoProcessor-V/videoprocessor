// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Config page providers VP-V registers with the generic Config page host
// (VideoProcessor-Config/ConfigPageHost.h). One call at process start, before the first editor is
// created, registers them in the order their pages take in the editor: a sub-page appears in the
// stack in registration order, ahead of the master VP-V page (VpvConfigPages.h).
// Idempotent. Nothing here runs unless a process calls it, so an editor without the call is Bill's.

#include <VP-V/modules/output/calibration_lut/config_ui/CalibrationPage.h>

namespace Vpv
{
	inline void RegisterConfigProviders() noexcept
	{
		if (!ConfigPageHost::Find<CalibrationPage>())
			ConfigPageHost::Register(&CalibrationPageInstance());
	}
}
