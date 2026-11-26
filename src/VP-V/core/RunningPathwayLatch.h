// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The writers of the running-pathway state (RunningPathway.h): Owner Option A live in-process switch —
// updated on configuration commit / renderer rebuild via Store(); tests set it via SetForTesting().
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
		// Owner Option A: atomically switch the live in-process pathway on configuration commit / renderer rebuild
		// so queue, lookahead, capture/display fixes, ingress policy, Dynamic Drive, and renderer switch together.
		// Serialized with cold-startup file probing; returns `original`.
		bool Store(bool original);
		using HostPathwaySink = void (*)(const void* host, bool original);
		void SetHostPathwaySink(HostPathwaySink sink);
		void NotifyHostPathway(const void* host, bool original);
		// Latches from a configuration file's pathway ([vpv] then [vprenderer], as the renderer reads it) when nothing is
		// latched yet; a file that does not load latches nothing. Returns Original().
		bool LatchFromConfigurationFile(const std::string& path);
		// Tests only: set (or reset to kUnknown) the latched state; also re-arms Original()'s one start-up file read.
		void SetForTesting(int state);
	}
}
