// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "VpvProfileQueueAdvice.h"

#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <ConfigFile.h>
#include <RendererProfileConfig.h>
#include <ShortcutDefaults.h>   // OriginalPathwayForConfig: the pathway read as the renderer reads it

#include <cstdlib>

namespace Vpv
{
	namespace ProfileQueueAdvice
	{
		bool BillQueueOnOriginal(const ConfigFile* config, RendererProfileConfig::ResolvedQueue& queue)
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

		Advice Compute(const Setting& setting, double sourceHz, bool originalPathway)
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
