// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The de-judder matcher's settings from a renderer settings profile (M2 P3-6; moved from
// vprenderer/LibplaceboVideoRenderer.cpp). Pure: `Profile` is any settings object that has the
// de-judder fields below (the host's RendererSettings, which carries VpvRenderer::Fields).

#include <vprenderer/DejudderWarp.h>

namespace MatcherSettingsFromProfile
{
	// The matcher's settings from a profile (refinements included): what
	// RunDejudderEstimate gives the matcher, and what the settings test
	// checks against (VideoProcessorLibplaceboDescribeSettingsForTest).
	template <class Profile>
	inline Dejudder::Settings MatcherSettings(const Profile& profile, Dejudder::Settings settings)
	{
		settings.matchTolerance = static_cast<float>(profile.dejudderMatchTolerance);
		settings.minTrustedShare = static_cast<float>(profile.dejudderFrameTrust);
		settings.globalPrior = profile.dejudderGlobalPrior;
		settings.vectorSmooth = profile.dejudderVectorSmooth;
		settings.motionScaleLo = static_cast<float>(profile.dejudderMotionScaleLo);
		settings.motionScaleHi = static_cast<float>(profile.dejudderMotionScaleHi);
		settings.trustScaleFull = static_cast<float>(profile.dejudderTrustScale);
		// D5 is source-warp only (planner 2026-10-12): with the rendered
		// warp it is ignored (logged once) and its pass not run.
		settings.occlusionFill = profile.dejudderOcclusionFill && !profile.dejudderWarpRendered;
		settings.estCandidates = profile.dejudderEstCandidates;
		settings.historyFixes = profile.motionHistoryFixes;   // Hunt 2 04 M1 / L2
		// docs/VP-nvof-ahead.md (owner 2026-10-16): the optical-flow engine is the
		// established default motion source; optical_flow: off = the block searches only.
		// Without the engine (not NVIDIA, a failure) the searches run as before.
		settings.opticalFlow = profile.opticalFlow;
		settings.opticalFlowGlobal = profile.opticalFlowGlobal;
		settings.opticalFlowGlobalFallback = profile.opticalFlowGlobalFallback;
		settings.opticalFlowCostTrust = profile.opticalFlowCostTrust;
		settings.opticalFlowFbCheck = profile.opticalFlowFbCheck;
		settings.estLambda = static_cast<float>(profile.dejudderEstLambda);
		settings.estQpel = profile.dejudderEstQpel;
		settings.estStillZero = profile.dejudderEstStillZero;
		settings.estGate = profile.dejudderEstGate;
		settings.estSplit = profile.dejudderEstSplit;
		settings.estSplitSad = static_cast<float>(profile.dejudderEstSplitSad);
		settings.motionRolloff = profile.dejudderMotionRolloff;
		settings.rollLo = static_cast<float>(profile.dejudderRollLo);
		settings.rollHi = static_cast<float>(profile.dejudderRollHi);
		settings.rollFloor = static_cast<float>(profile.dejudderRollFloor);
		settings.objectStrength = static_cast<float>(profile.dejudderObjectStrength);
		settings.textureTolerance = static_cast<float>(profile.dejudderTextureTolerance);
		settings.globalTwo = profile.dejudderGlobalTwo;
		settings.regionalHold = profile.dejudderRegionalHold;
		settings.cutHistogram = static_cast<float>(profile.dejudderCutHistogram);
		settings.coarseRange = profile.dejudderSearchRange;
		settings.consistencyPixels = static_cast<float>(profile.dejudderConsistencyPx);
		settings.fadeMatch = profile.dejudderFadeMatch;
		settings.fadeFloor = static_cast<float>(profile.dejudderFadeFloor);
		settings.verifyPixels = profile.dejudderVerify;
		settings.verifyShare = static_cast<float>(profile.dejudderVerifyShare);
		settings.pixelTrust = profile.dejudderPixelTrust;
		settings.pixelTrustLo = static_cast<float>(profile.dejudderPixelTrustLo);
		settings.pixelTrustHi = static_cast<float>(profile.dejudderPixelTrustHi);
		settings.landingCheck = profile.dejudderLandingCheck ? 1.5f : 0.0f;
		settings.sourceWarpCatmull = profile.dejudderSourceWarpCatmull;
		settings.pictureArea = true;   // dejudder_picture_area retired 2026-10-17: always the picture only
		// General (owner 2026-10-14): pixel values for a 2160-line picture,
		// per-frame values at 23.976p; the matcher scales them to the source.
		settings.referenceHeight = 2160.0f;
		settings.referenceRateHz = 24000.0 / 1001.0;
		settings.pixelTrustedVectors = true;
		settings.lowRes = profile.dejudderLowRes != "off";
		settings.lowResAll = profile.dejudderLowRes == "all";
		settings.holdFeatherPx = static_cast<float>(profile.dejudderHoldFeatherPx);
		settings.holdHysteresis = profile.dejudderHoldHysteresis;
		settings.blockTaper = profile.dejudderBlockTaper;
		settings.taperSad = static_cast<float>(profile.dejudderTaperSad);
		return settings;
	}
}
