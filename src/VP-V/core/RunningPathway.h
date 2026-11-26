// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The pathway this PROCESS currently runs (Owner Option A live in-process switch, GEM-221 / MUST-8).
// Initialised on first Original() read from the start-up configuration file (VideoProcessor.cfg, --config honoured)
// when no snapshot/renderer has stored a pathway yet, and updated atomically via RunningPathway::Store(bool) when
// UnifiedProfileRuntime commits a new configuration or LibplaceboVideoRenderer rebuilds for a pathway change, so
// BillQueueOnOriginal, QueueLookahead, CaptureDisplayFixes, IngressPolicy, DynamicDriveHost, and the renderer all
// switch cleanly together between VP-V and Original in-process with zero split-brain. Unknown behaves as VP-V.
// Declarations only: the bodies compile in RunningPathway.cpp (VideoProcessor-Lib), so a Bill translation unit that
// reaches this header carries no VP-V code from it. C++14.

namespace Vpv
{
	namespace RunningPathway
	{
		enum State : int { kUnknown = 0, kVpv = 1, kOriginal = 2 };
		// The active pathway is Original. When kUnknown, latches once from the start-up configuration file.
		bool Original();
	}
}
