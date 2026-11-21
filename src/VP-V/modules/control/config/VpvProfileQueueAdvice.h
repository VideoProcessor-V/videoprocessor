// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P14 (LIB-MISC moves): the VP-V part of UnifiedProfileRuntime's snapshot builder, moved out of
// UnifiedProfileRuntime.cpp. It answers one question from the active display/input profile
// values: how many frames deep must the renderer queue be for the optical-flow look-ahead
// (owner 2026-10-16: on or off; on needs the engine depth ahead), and is general_rate_fixes on.
// Header-only, C++14. The host keeps the profile resolution (the `setting` accessor: the input
// profile's value wins over the display profile's) and the cadence parse; this header holds only
// VP-V keys and the MotionAhead depth rule. It starts no motion work and classifies nothing
// (rule 21: the depth comes from MotionAhead::DepthAt / QueueFramesFor).

#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <ShortcutDefaults.h>   // OriginalPathwayForConfig: the pathway read as the renderer reads it

#include <cstddef>
#include <cstdlib>
#include <string>

namespace Vpv
{
	namespace ProfileQueueAdvice
	{
		struct Advice
		{
			size_t lookAheadQueueFrames = 0;
			bool generalRateFixes = true;
		};

		// GEM-032 (config-schema MUST-2) and GEM-034: the Original pathway is Bill's queue. Returns whether it is
		// in force (config: the running configuration; nullptr = not). When it is, the resolved queue profile's
		// VP-only keys are made inert before the snapshot carries them, so the host's existing queue code
		// computes Bill's values (Gemini's fix routed through the existing snapshot hook block, no new Bill line):
		//   - target_latency_ms: an explicit 0 (also switches a time target inherited from the default queue off),
		//     so the target is Bill's target_frames (own, else the default) and the delay budget is 0;
		//   - low_latency_path: off (the renderer's m_lowLatencyQueue follows the snapshot);
		//   - an unset active_picture_lookahead_frames: Bill's 0 instead of VP-V's Auto.
		// Bill's own keys (target_frames, lead, pre-roll, size, reset, a set look-ahead) are left as resolved.
		template <class Queue>
		inline bool BillQueueOnOriginal(const ConfigFile* config, Queue& queue)
		{
			const bool original = config && OriginalPathwayForConfig(
				[config](const char* section, const char* key, std::string& out) { return config->TryGetString(section, key, out); });
			if (!original)
				return false;
			queue.hasTargetLatencyMs = true;
			queue.targetLatencyMs = 0;
			queue.hasLowLatencyPath = true;
			queue.lowLatencyPath = false;
			if (!queue.hasActivePictureLookaheadFrames)
			{
				queue.hasActivePictureLookaheadFrames = true;
				queue.activePictureLookaheadFrames = 0;
			}
			return true;
		}

		// setting(key, fallback) returns the normalized profile value of a key (the fallback when
		// no profile sets it). sourceHz is the measured source rate (0 = unknown). originalPathway: the
		// Original pathway is in force (GEM-034: Bill's queue, so no look-ahead depth and no general-rate fixes).
		template <typename SettingFn>
		inline Advice Compute(SettingFn&& setting, double sourceHz, bool originalPathway = false)
		{
			Advice advice;
			if (originalPathway)
			{
				advice.generalRateFixes = false;
				advice.lookAheadQueueFrames = 0;
				return advice;
			}
			// GEM-034: vector_demand off (Batch B: the motion engine switched off) computes nothing ahead.
			const bool engine = setting("vector_demand", "on") != "off";
			const bool flow = setting("optical_flow", "on") != "off";
			const bool ahead = std::atof(setting("motion_ahead_frames", "4").c_str()) > 0.0;
			const bool dejudder = std::atof(setting("dejudder_strength", "0").c_str()) > 0.0 ||
				std::atof(setting("dejudder_strength_even", "0").c_str()) > 0.0;
			// The interpolator takes its pairs from the producer too (plan step 5).
			const bool interpolation = setting("interpolation", "off") != "off";
			// Fixed-constant batch B3 (2026-10-18, general_rate_fixes): the depth at the source rate
			// (MotionAhead::DepthAt: 4 at 60 Hz and below, the same time above).
			advice.generalRateFixes = setting("general_rate_fixes", "on") != "off";
			if (engine && flow && ahead && (dejudder || interpolation))
				// audit 2026-10-16: derived from the engine depth, not a literal 5 (4 ahead + the frame shown).
				advice.lookAheadQueueFrames = MotionAhead::QueueFramesFor(
					MotionAhead::DepthAt(MotionAhead::kDefaultDepth, sourceHz, advice.generalRateFixes));
			return advice;
		}
	}
}
