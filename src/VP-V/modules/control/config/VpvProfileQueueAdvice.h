// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P14 (LIB-MISC moves): the VP-V part of UnifiedProfileRuntime's snapshot builder, moved out of
// UnifiedProfileRuntime.cpp. It answers one question from the active display/input profile
// values: how many frames deep must the renderer queue be for the optical-flow look-ahead
// (owner 2026-10-16: on or off; on needs the engine depth ahead), and is general_rate_fixes on.
// C++14. The host keeps the profile resolution (the `setting` accessor: the input profile's value
// wins over the display profile's) and the cadence parse; this unit holds only VP-V keys and the
// MotionAhead depth rule. It starts no motion work and classifies nothing (rule 21: the depth comes
// from MotionAhead::DepthAt / QueueFramesFor).
// Declarations only (CC W4 item 4b): the bodies compile in VpvProfileQueueAdvice.cpp, so Bill's
// UnifiedProfileRuntime.cpp translation unit carries no VP-V code from this header.

#include <cstddef>
#include <functional>
#include <string>

class ConfigFile;
namespace RendererProfileConfig { struct ResolvedQueue; }

namespace Vpv
{
	namespace ProfileQueueAdvice
	{
		struct Advice
		{
			size_t lookAheadQueueFrames = 0;
			bool generalRateFixes = true;
		};

		// setting(key, fallback) returns the normalized profile value of a key (the fallback when no profile sets it).
		using Setting = std::function<std::string(const char* key, const std::string& fallback)>;

		// GEM-032 (config-schema MUST-2) and GEM-034: the Original pathway is Bill's queue. Returns whether it is
		// in force (config: the running configuration; nullptr = not). When it is, the resolved queue profile's
		// VP-only keys are made inert before the snapshot carries them, so the host's existing queue code
		// computes Bill's values (Gemini's fix routed through the existing snapshot hook block, no new Bill line):
		//   - target_latency_ms: an explicit 0 (also switches a time target inherited from the default queue off),
		//     so the target is Bill's target_frames (own, else the default) and the delay budget is 0;
		//   - low_latency_path: off (the renderer's m_lowLatencyQueue follows the snapshot);
		//   - an unset active_picture_lookahead_frames: Bill's 0 instead of VP-V's Auto.
		// Bill's own keys (target_frames, lead, pre-roll, size, reset, a set look-ahead) are left as resolved.
		bool BillQueueOnOriginal(const ConfigFile* config, RendererProfileConfig::ResolvedQueue& queue);

		// sourceHz is the measured source rate (0 = unknown). originalPathway: the Original pathway is in force
		// (GEM-034: Bill's queue, so no look-ahead depth and no general-rate fixes).
		Advice Compute(const Setting& setting, double sourceHz, bool originalPathway = false);
	}
}
