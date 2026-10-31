// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19): search-hints data API for the matcher (phase 2). CPU/data only;
// nothing is wired to a matcher yet.
#pragma once

#include <cstdint>
#include <vector>

#include "CameraMotion.h"

namespace CameraModel
{
	// SkipAcrossCut: the matcher must not search across this pair. SeedFromNextPair: seed the new
	// scene from the first post-cut pair in the queue. BridgeFlash: outlier frame, use the bridged
	// model. FadeNormalised: zero-mean (gain/offset-normalised) matching cost for this pair.
	enum class SceneAction : uint8_t { Normal, SkipAcrossCut, SeedFromNextPair, BridgeFlash, FadeNormalised };

	enum class BlockHint : uint8_t { FullSearch = 0, NarrowAroundPrediction = 1, FinerGrid = 2, Excluded = 3 };

	struct HintConfig
	{
		double minConfidence = 0.7;
		double agreeSigmas = 2.0;
		double narrowRadiusSigmas = 4.0;
		bool temporalSeed = true;
		bool tinyMoverRoi = true;
		bool fadeNormalised = true;
		bool cutSkip = true;
		bool flashBridge = true;
	};

	struct SearchHints
	{
		SceneAction scene = SceneAction::Normal;
		double fadeGain = 1, fadeOffset = 0;
		int gridW = 0, gridH = 0;
		std::vector<uint8_t> hint;            // BlockHint per block
		std::vector<float> seedX, seedY;
		double narrowRadiusPx = 0;
		struct Roi { int bx0, by0, bx1, by1; };   // block units, bx1/by1 exclusive
		std::vector<Roi> rois;
		uint32_t narrowBlocks = 0, fullBlocks = 0, finerBlocks = 0;
	};

	// prevFwdX/Y: the previous pair's forward vectors per block (may be null); nvofX/Y: the engine's
	// vectors per block (may be null). precisionPx: the model's measured precision (sigma) in px.
	// BuildHints never returns SeedFromNextPair: the caller sets it.
	// Escalation: phase 2 re-runs a narrow block that fails verification as FullSearch.
	SearchHints BuildHints(const HintConfig& cfg, const CameraMotion* prev, const float* prevFwdX, const float* prevFwdY,
		const float* nvofX, const float* nvofY, bool cut, bool fade, bool flash, double fadeGain, double fadeOffset,
		double precisionPx);
}
