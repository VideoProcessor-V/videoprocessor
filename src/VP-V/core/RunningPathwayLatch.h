// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The writers of the running-pathway latch (RunningPathway.h): the profile runtime's VP-V unit latches it; tests set it.
// Not for Bill's translation units. Header-only, C++14.

#include <VP-V/core/RunningPathway.h>

namespace Vpv
{
	namespace RunningPathway
	{
		// The first call wins; returns whether the latched pathway is Original.
		inline bool Latch(bool original)
		{
			int unknown = kUnknown;
			Value().compare_exchange_strong(unknown, original ? kOriginal : kVpv);
			return Value().load() == kOriginal;
		}
		// Tests only: set (or reset to kUnknown) the latched state.
		inline void SetForTesting(int state) { Value().store(state); }
	}
}
