// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2): the measurement-side plan for the vector engine's matcher.
// From the PREVIOUS pair's causal record (never the queued smoother: no feedback loop through the
// search), the scene flags and the switches below, BuildMeasurementPlan says per frame and per block
// what the matcher may skip or narrow. Every saving has an escalation: a narrowed or skipped block
// that fails verification is searched with today's full search in the same frame, so no object is
// missed. Pure C++14, deterministic, no Windows headers, no threads.
//
// Switch off (or record absent / invalid / not usable): the plan is EMPTY (Active() == false) and the
// matcher runs exactly today's search (byte-identical). Every automatic downgrade is counted in
// MeasurementCounters and named in fallbackReason (shown on the status line), never only logged.
//
// Units: every vector in this header is in SOURCE pixels over the pair ABOUT TO BE SEARCHED (seeds and
// predictions are already time-scaled by nextPairSeconds / prev->pairSeconds). The matcher converts to
// its own working pixels (half size: x 0.5; a low-resolution inner: x 0.25).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CameraMotion.h"
#include "GlobalModelSearchHints.h"

namespace CameraModel
{
	// One internal test switch per optimisation (diagnostics section; not on the owner's pages).
	// All default off: off = today's search exactly.
	struct MeasurementSwitches
	{
		bool modelGlobal = false;      // vector_model_global: the model as D1's global candidate (D1 search skipped)
		bool guidedCandidate = false;  // vector_guided_candidate: the model's per-block prediction as a candidate
		bool narrowWindow = false;     // vector_narrow_window: camera blocks searched +-r around the prediction
		bool e1Skip = false;           // vector_e1_skip: second sweep skipped where the first agrees with the model
		bool staticGate = false;       // vector_static_gate: E4 also from the model (zero motion, high confidence)
		bool cutSkip = false;          // vector_cut_skip: no matcher search across a cut
		bool fadeNormalised = false;   // vector_fade_normalised: zero-mean (gain/offset) cost in a fade
		bool flashBridge = false;      // vector_flash_bridge: the flash pair is not searched; bridged model
		bool temporalSeed = false;     // vector_temporal_seed: previous vector as the seed when it agrees with NVOF
		bool tinyMoverRoi = false;     // vector_tiny_mover_roi: tiny movers -> finer grid / ROI, never narrowed
		bool objects = false;          // camera_model_objects (search side): object-model seeds for object blocks
		bool Any() const
		{
			return modelGlobal || guidedCandidate || narrowWindow || e1Skip || staticGate || cutSkip || fadeNormalised ||
				flashBridge || temporalSeed || tinyMoverRoi || objects;
		}
	};

	struct MeasurementConfig
	{
		double minConfidence = 0.7;      // record confidence needed for any model-driven saving
		double staticConfidence = 0.85;  // E4 from the model: stricter
		double staticSigmas = 1.0;       // E4: |model displacement| at every picture corner <= staticSigmas * precision
		double agreeSigmas = 2.0;        // seed agreement (temporal seed vs NVOF / model) in precision sigmas
		double narrowRadiusSigmas = 4.0; // narrow window radius = narrowRadiusSigmas * precision (source px)
		double narrowMinRadiusPx = 1.0;  // never narrower than this many source px (relative floor: one matcher px at half size is 2)
		double e1AgreePx = 0.5;          // E1 skip: first-sweep winner within this many source px of the prediction
		double roiBudgetShare = 0.05;    // tiny-mover ROIs: at most this share of the grid's blocks per frame
	};

	// What the caller knows about the pair about to be searched.
	struct MeasurementInput
	{
		const CameraMotion* prev = nullptr;      // the PREVIOUS pair's causal record (Fitter::Fit output), or null
		const float* prevFwdX = nullptr;         // the previous pair's verified forward vectors (source px), or null
		const float* prevFwdY = nullptr;         //   (null after a flash or a cut: never seed from a junk pair)
		const float* nvofX = nullptr;            // the engine's seed vectors for THIS pair (source px), or null
		const float* nvofY = nullptr;
		bool cut = false, fade = false, flash = false;   // this pair's scene flags (frame analysis store)
		double fadeGain = 1, fadeOffset = 0;
		double precisionPx = 0;                  // <= 0 -> 0.25 * blockSize / 16 (as the fitter)
		double nextPairSeconds = 0;              // duration of this pair; <= 0 = no time scaling
		bool haveNextPair = false;               // a queued post-cut pair exists (cut: seed from it)
	};

	// Visible counters (status line / Ctrl+Alt+D). Accumulated per plan; MeasurementDiagnostics sums them.
	struct MeasurementCounters
	{
		uint32_t narrowBlocks = 0;        // blocks planned NarrowAroundPrediction
		uint32_t fullBlocks = 0;          // blocks planned FullSearch
		uint32_t finerBlocks = 0;         // tiny-mover blocks (finer grid / ROI)
		uint32_t excludedBlocks = 0;      // bars / text guard
		uint32_t objectSeeded = 0;        // narrow blocks seeded from an object model
		uint32_t temporalSeeded = 0;      // narrow blocks seeded from the previous vector
		uint32_t roiOverBudget = 0;       // FinerGrid blocks turned FullSearch (budget exceeded, raster order)
		uint32_t escalated = 0;           // narrowed / skipped blocks that failed verification -> full search
		bool modelGlobalUsed = false, staticFromModel = false, matcherSkipped = false, fadeNormalisedUsed = false;
	};

	struct MeasurementPlan
	{
		bool active = false;              // false: today's search exactly (nothing below is used)
		SceneAction scene = SceneAction::Normal;
		bool skipMatcher = false;         // cut (cutSkip) or flash (flashBridge): no search; the field holds
		bool seedFromNext = false;        // cut with a queued pair: the record is seeded from it (fitter side)
		// D1 from the model: forward and backward global translation (displacement at the picture centre).
		bool useModelGlobal = false;
		double globalX = 0, globalY = 0, globalBwdX = 0, globalBwdY = 0;
		bool staticScene = false;         // E4 from the model
		bool fadeNormalised = false;      // zero-mean / gain-offset cost (narrow pass and E1 skip check)
		double fadeGain = 1, fadeOffset = 0;
		bool guidedCandidate = false;     // predX/Y is an extra candidate for eligible blocks
		bool e1Skip = false;              // E1 skip check enabled
		// Per block (gridW * gridH, the record's grid; empty when no per-block guidance).
		int gridW = 0, gridH = 0;
		double blockSize = 0;
		std::vector<uint8_t> hint;        // BlockHint
		std::vector<uint8_t> eligible;    // 1 = camera- (or object-model-) consistent block with a usable prediction
		std::vector<float> seedX, seedY;  // narrow centre (source px)
		std::vector<float> predX, predY;  // model (or object-model) prediction (source px)
		double narrowRadiusPx = 0;        // source px
		double e1AgreePx = 0;             // source px
		std::vector<SearchHints::Roi> rois;
		MeasurementCounters counters;
		const char* fallbackReason = "";  // non-empty when a switched-on saving was not applied (visible)
		bool Active() const { return active; }
	};

	MeasurementPlan BuildMeasurementPlan(const MeasurementSwitches& sw, const MeasurementConfig& cfg, const MeasurementInput& in);

	// After the matcher's guided pass: accepted[i] = 1 where the narrowed / skipped block passed
	// verification. Every planned narrow (or E1-skip eligible) block with accepted == 0 is escalated:
	// its hint becomes FullSearch and counters.escalated counts it. Returns the number escalated.
	uint32_t ApplyVerification(MeasurementPlan& plan, const uint8_t* accepted);

	// Running totals for the status line (never log-only).
	struct MeasurementDiagnostics
	{
		uint64_t frames = 0, narrow = 0, full = 0, finer = 0, escalated = 0, roiOverBudget = 0;
		uint64_t modelGlobal = 0, staticFromModel = 0, matcherSkipped = 0, fadeNormalised = 0;
		std::string lastFallback;
		void Add(const MeasurementPlan& plan);
		std::string StatusLine() const;   // e.g. "camera guidance: narrow 71% (escalated 0.4%), D1 model 120/130, ..."
	};

	// GPU guidance texels (layout shared with the matcher's guided pass, CameraGuidanceGlsl.h):
	// texture `camera_guide` RGBA32F per record block = (seedX, seedY, predX, predY) source px;
	// texture `camera_code` R32F per record block = hint + 4 * eligible (exact small integers in float).
	// Both empty when !plan.active or the plan has no per-block data.
	void PackGuideTexels(const MeasurementPlan& plan, std::vector<float>& guideRgba, std::vector<float>& code);
}
