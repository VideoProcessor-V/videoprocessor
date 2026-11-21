// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The pathway this PROCESS runs (CC W4: B1, B2, GEM-221 / MUST-8). Latched once (RunningPathwayLatch.h), from the first
// configuration the profile runtime builds a snapshot from (Vpv::ProfileQueueAdvice::BillQueueOnOriginal); a later
// [vpv] pipeline_original_pathway change takes effect at the next start, as the renderer reads it. Unknown (nothing
// latched: before the first snapshot, the Config editor, the renderer DLL) behaves as VP-V, i.e. as before the latch.
// Process-wide, one per module (an inline function's static). Header-only, C++14. This header is the reader only
// (it compiles into Bill's translation units through its consumers); the writers are in RunningPathwayLatch.h.

#include <atomic>

namespace Vpv
{
	namespace RunningPathway
	{
		enum State : int { kUnknown = 0, kVpv = 1, kOriginal = 2 };
		inline std::atomic<int>& Value() { static std::atomic<int> value{ kUnknown }; return value; }
		inline bool Original() { return Value().load(std::memory_order_relaxed) == kOriginal; }
	}
}
