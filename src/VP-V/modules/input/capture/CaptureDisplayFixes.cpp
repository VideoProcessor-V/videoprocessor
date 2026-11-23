// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// See CaptureDisplayFixes.h (VideoProcessor-Lib).
#include <pch.h>
#include <CaptureDisplayFixes.h>

#include <VP-V/core/RunningPathway.h>

namespace CaptureDisplayFixes
{
	// GEM-221 / MUST-8 (owner: Original = Bill): off on the Original pathway the process runs, whatever the switch says.
	// Original() latches from the start-up configuration when nothing is latched yet, so the start-up target-only
	// display session (before the profile runtime's first snapshot) is Bill's too.
	bool Enabled()
	{
		return Flag().load(std::memory_order_relaxed) && !Vpv::RunningPathway::Original();
	}
}
