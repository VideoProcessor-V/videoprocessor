// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The pathway this PROCESS runs (CC W4: B1, B2, GEM-221 / MUST-8). Latched once, first call wins:
// - by the profile runtime's first snapshot (Vpv::ProfileQueueAdvice::BillQueueOnOriginal), or earlier,
// - by the first Original() read while nothing is latched: it reads the configured pathway from the start-up
//   configuration file (VideoProcessor.cfg, as VideoProcessorApp / the dialog load it; --config honoured), so the
//   start-up target-only display session, and a process whose profile runtime failed to start, are Bill's on Original.
// A later [vpv] pipeline_original_pathway change takes effect at the next start, as the renderer reads it. Unknown
// (no file read yet or none that loads; the Config editor and the renderer DLL never read it) behaves as VP-V.
// Declarations only: the bodies compile in RunningPathway.cpp (VideoProcessor-Lib), so a Bill translation unit that
// reaches this header carries no VP-V code from it. C++14.

namespace Vpv
{
	namespace RunningPathway
	{
		enum State : int { kUnknown = 0, kVpv = 1, kOriginal = 2 };
		// The latched pathway is Original. Nothing latched yet: latches first from the start-up configuration file (once).
		bool Original();
	}
}
