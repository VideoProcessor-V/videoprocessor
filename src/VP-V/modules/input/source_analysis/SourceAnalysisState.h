// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P11a-f (refs/110 s2.6): the source-resolution latch state of the renderer (Stage 12 step B), moved out of
// LibplaceboVideoRenderer.cpp's Impl as `Vpv::HostState::SourceAnalysis` (core/hub/HostState.h), reached as
// `vpv.sourceAnalysis.x`. Data only; types and initial values unchanged. Effective resolution, bitrate quality and
// bit depth of every source, read on the native frame (EntrancePlanner.inl feeds it; look-ahead walks and the vector
// engine host read it). The monitor is the module's own class (SourceResolutionMonitor.h).
// The layout reserve for the future P15/P15c members is already declared by QualityPatternRecord.h (r2/p15-decl-b).
// C++14 only.

#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <cstdint>

namespace Vpv
{
	struct SourceAnalysisState
	{
		SourceResolutionMonitor::Monitor sourceMonitor;
		uint64_t sourceMonitorSequence = 0;
		bool sourceMonitorHistorySet = false;
		bool sourceMonitorDvLogged = false;
	};
}
