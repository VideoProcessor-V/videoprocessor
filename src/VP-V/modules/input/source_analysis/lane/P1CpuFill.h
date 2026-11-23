// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

struct AnalysisLumaSource;

namespace Vpv
{
	namespace FrameAnalysis
	{
		// D3 / FIX3-D6 (102b CO-DEFAULT-MOTIONAHEAD-U3): zero-wait no-lane CPU owner fill for kP1HalfLuma.
		// Registers a module-wide (kP1HalfLuma, AcquireForm::Cpu) OwnerFill under the catalog owner unit
		// "input/source_analysis/lane" (renderThreadOnly = false, frameScoped = false, keepFrames = 2,
		// aheadDepth = kAheadCeiling while demand is active). Shadowed 100% by laneOwners_ / lane_.copyPublished
		// whenever analysis_lane is ON.
		void RegisterP1CpuModule();
		void NoteP1CpuDemand(const void* host, bool active);
		void OfferP1CpuSource(const void* host, uint64_t seq, const AnalysisLumaSource& source);
	}
}
