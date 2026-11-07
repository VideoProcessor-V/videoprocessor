// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: dynamic-texture (water-like) detector. A whole-frame dynamic texture (water, foliage in wind,
// smoke) gives block vectors that follow neither the camera nor any object: after the camera model is removed, most
// trusted blocks still disagree, the disagreement is not coherent with the previous pair's residual field, and
// neighbouring residuals do not agree with each other. Such a pair has no camera motion a consumer may use, so the
// fitter reports it Ambiguous (a fit that merely locks onto a texture is never "confident"). Pure C++14,
// deterministic, CPU, O(blocks) (one pass over the block field plus one pass over the previous residuals).
#pragma once

#include <cstdint>
#include <vector>

#include "CameraMotion.h"

namespace CameraModel
{
	struct PairInput;

	struct DynTextureConfig
	{
		// Agreement threshold of a block with the model: max(absSigmas * precision, rel * |model|), source px (the fit's own).
		double absSigmas = 3.0;
		double rel = 0.05;
		// Minimum trusted blocks, and minimum blocks in the temporal / spatial comparison sets, for the statistic to count.
		int minBlocks = 64;
		int minCompared = 48;
		// The pair is a dynamic texture when the share of trusted blocks that disagree with the model is at least minShare AND
		// the temporal incoherence of the disagreeing field is at least minTemporal (when a previous field exists) AND the
		// spatial incoherence is at least minSpatial. The defaults are chosen so that no real pair of the camera_eval clips
		// (crowd, tos, sol, boat; every tier) that the fitter calls usable is turned Ambiguous: the largest spatial
		// incoherence among them is 0.43 (boat), the largest disagreeing share 0.59 (sol, spatial 0.4).
		double minShare = 0.5;
		double minTemporal = 0.6;
		double minSpatial = 0.5;
		// Two residuals agree when they differ by at most max(threshold, ratio * the larger magnitude).
		double agreeRatio = 0.5;
	};

	struct DynTextureResult
	{
		bool computed = false;       // enough trusted blocks
		bool detected = false;       // the verdict
		double share = 0;            // trusted blocks that disagree with the model
		double temporal = 0;         // incoherence vs the previous pair's residuals (0 when none / too few compared)
		double field = 0;            // incoherence of the RAW vectors vs the previous pair's (camera + residual)
		double spatial = 0;          // incoherence between neighbouring disagreeing blocks
		bool haveTemporal = false;
	};

	// The previous pair's residual field (measured minus model per block, source px per pair), for the temporal term.
	struct DynTextureState
	{
		bool have = false;
		int gridW = 0, gridH = 0;
		Rect picture;
		double seconds = 0;
		std::vector<float> rx, ry;     // residuals
		std::vector<float> vx, vy;     // raw vectors
		std::vector<uint8_t> valid;
		void Reset() { *this = DynTextureState(); }
	};

	// Evaluates one pair against `model` and the previous residuals in `state`, then stores this pair's residuals in
	// `state` (state is updated for every call with computed == true, so call it once per fitted pair, in order).
	DynTextureResult DetectDynamicTexture(const DynTextureConfig& cfg, const PairInput& in, const Params& model, DynTextureState& state);
}
