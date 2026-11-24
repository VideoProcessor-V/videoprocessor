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
		double spatial = 0;          // incoherence between neighbouring disagreeing blocks (always computed, no share blind spot)
		double spatialCurvature = 0; // whole-field share of trusted blocks with |nabla^2 v| > absSigmas * sigma_C
		double rhoTemp = 0;          // whole-field camera-advected lag-1 residual autocorrelation in [-1, 1] (0 when unavailable)
		bool haveTemporal = false;
		bool haveRhoTemp = false;
	};

	// The previous pair's residual field (measured minus model per block, source px per pair), for the temporal term.
	struct DynTextureBuffers
	{
		std::vector<float> rx, ry;     // residuals (measured minus model)
		std::vector<float> vx, vy;     // raw vectors
		std::vector<float> thr;        // per-block agreement threshold (scratch)
		std::vector<uint8_t> valid, dis;
	};

	struct DynTextureState
	{
		bool have = false;
		int gridW = 0, gridH = 0;
		Rect picture;
		double seconds = 0;
		DynTextureBuffers prev;        // the previous pair (valid when have)
		DynTextureBuffers cur;         // scratch, swapped with prev after each computed pair (no per-pair allocation once sized)
		void Reset() { have = false; seconds = 0; }   // keeps the buffers
	};

	// Evaluates one pair against `model` and the previous residuals in `state`, then stores this pair's residuals in
	// `state` (state is updated for every call with computed == true, so call it once per fitted pair, in order).
	DynTextureResult DetectDynamicTexture(const DynTextureConfig& cfg, const PairInput& in, const Params& model, DynTextureState& state,
		double resolvedPrecPx = 0);

	// CE-WATER-C1 (default OFF in Config::waterBlockExclude): per-block model-free discrete vector curvature
	// exclusion from the GlobalModelFit sample / refinement set (NEVER stamped into BlockData::excluded / Label::Overlay).
	// For any linear camera motion (Translation, Similarity, Affine), the symmetric discrete curvature
	//   C(b) = v(b) - avg(symmetric orthogonal neighbours)
	// vanishes identically (100% invariant to pan, zoom, and rotation speed). Under i.i.d. vector noise sigma = precPx:
	//   - 4-point symmetric stencil (L, R, U, D): std(C_x) = sqrt(1 + 4/16) * precPx = (sqrt(5)/2) * precPx
	//   - 2-point symmetric pair (L+R or U+D on borders): std(C_x) = sqrt(1 + 2/4) * precPx = sqrt(3/2) * precPx
	//   - temporal difference v_t(b) - k * v_{t-1}(pb) (k = dt_t / dt_{t-1}): std(dv_x) = sqrt(1 + k^2) * precPx
	// Thresholds derive strictly from the fitter's 3-sigma contract (Config::absThresholdSigmas = 3.0).
	struct DynTextureExcludeConfig
	{
		double curvatureSigmas = 3.0;            // 3-sigma contract matching Config::absThresholdSigmas (no arbitrary rel * |v|)
		// Remaining-rigid-support guard (CE-WATER-C1): when waterExcludedShare exceeds this elementary 1-quadrant ratio
		// (1/4 of trusted blocks), remaining rigid support must still cover >= 3 quadrants (Config::quadrantMinShare) AND
		// retain border-ring support >= Config::borderVetoRatio, else demote cleanly to Reason::Ambiguous | kVerdictDynamicTexture.
		double supportGuardMinWaterShare = 0.25; // 1/4 = 1 quadrant of the 4-quadrant picture
	};

	struct WaterExcludeStats
	{
		int evaluated = 0;
		int excluded = 0;
	};

	// Writes per-block mask (size gridW * gridH; 1 = incoherent dynamic-texture / water block, 0 = keep).
	// Pure, deterministic, O(blocks). Does not mutate `prevState`.
	WaterExcludeStats ComputeWaterExclusionMask(const DynTextureExcludeConfig& cfg, const PairInput& in,
		const DynTextureState& prevState, double precPx, std::vector<uint8_t>& maskOut);

	// Stores this pair's raw forward vector field into `state.prev` for the next pair's temporal water-exclusion check.
	void RecordRawFieldForWaterExclusion(const PairInput& in, DynTextureState& state);
}
