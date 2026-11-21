// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The writers of the running-pathway latch (RunningPathway.h): the profile runtime's VP-V unit latches it; tests set it.
// Not for Bill's translation units. Declarations only (RunningPathway.cpp). C++14.

#include <VP-V/core/RunningPathway.h>

#include <atomic>
#include <string>

namespace Vpv
{
	namespace RunningPathway
	{
		// The latched state (State); process-wide, one per module.
		std::atomic<int>& Value();
		// The first call wins; returns whether the latched pathway is Original.
		bool Latch(bool original);
		// Latches from a configuration file's pathway ([vpv] then [vprenderer], as the renderer reads it) when nothing is
		// latched yet; a file that does not load latches nothing. Returns Original().
		bool LatchFromConfigurationFile(const std::string& path);
		// Tests only: set (or reset to kUnknown) the latched state; also re-arms Original()'s one start-up file read.
		void SetForTesting(int state);
	}
}
