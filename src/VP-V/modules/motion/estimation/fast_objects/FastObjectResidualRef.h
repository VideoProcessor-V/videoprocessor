// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage A CPU reference: camera-compensated 3-frame same-polarity symmetric residual,
// physics-derived local noise & gradient floor (with MAD fallback), optional half-res Cb/Cr
// 3-DOF chi^2 residual, 4x4 cell Neyman-Pearson matched filter, bounded active-cell overflow
// guard, hysteresis connected components, and exact second-moment streak deconvolution.
// Pure C++14/17, deterministic, caller-owned workspace (zero steady-state heap allocations).
#pragma once

#include <cstdint>
#include <vector>

#include "FastObjectTypes.h"

namespace FastObjects
{
	struct ResidualInput
	{
		// Half-resolution (or full-res with sourceScale = 1.0f) float luma triplet (width * height).
		// Bidirectional mode: prev = Y_{t-1}, cur = Y_t, next = Y_{t+1}.
		// Causal fallback (causalTwoBack = true): prev = Y_{t-1}, cur = Y_t, next = Y_{t-2}.
		const float* lumaPrev = nullptr;
		const float* lumaCur = nullptr;
		const float* lumaNext = nullptr;
		int width = 0;
		int height = 0;
		float sourceScale = 2.0f; // Source pixels per input sample (2.0 for half-res, 1.0 for full-res)

		// Optional native half-res chroma triplets (same width * height).
		const float* cbPrev = nullptr;
		const float* cbCur = nullptr;
		const float* cbNext = nullptr;
		const float* crPrev = nullptr;
		const float* crCur = nullptr;
		const float* crNext = nullptr;

		// Camera homographies in picture-centred source px (CameraMotion.h convention).
		// Bidirectional: camPrevToCur = H_{t-1 -> t}, camCurToNext = H_{t -> t+1}.
		// Causal fallback: camPrevToCur = H_{t-1 -> t}, camCurToNext = H_{t-2 -> t-1}.
		CameraModel::Params camPrevToCur;
		CameraModel::Params camCurToNext;
		bool camPrevUsable = false;
		bool camNextUsable = false;
		double camSigmaPx = 0.0; // sigma_H = absThresholdPx / 3.0 in source px

		// P4 active picture rectangle in source px (empty -> full frame [0, width*sourceScale) x [0, height*sourceScale)).
		CameraModel::Rect picture;
		bool sceneDiscontinuity = false; // P4 cut || flash
		bool causalTwoBack = false;      // True when laneLead == 0 (triplet t-2, t-1, t)

		// Optional P4 fade normalization: (sample - offset) / gain before differencing.
		bool fadePrev = false;
		double fadeGainPrev = 1.0;
		double fadeOffsetPrev = 0.0;
		bool fadeNext = false;
		double fadeGainNext = 1.0;
		double fadeOffsetNext = 0.0;

		// Optional P10g per-block grain variance N_b = sigma_g^2 (in input luma units^2).
		const float* grainVarMap = nullptr;
		int grainGridW = 0;
		int grainGridH = 0;
		double grainBlockSrcPx = 16.0;
		// Optional scalar grain std (in input luma units); < 0 falls back to robust MAD when grainVarMap == nullptr.
		double uniformGrainSigma = -1.0;
		// Optional scalar chroma grain std; < 0 reuses luma grain or computes chroma MAD.
		double uniformChromaGrainSigma = -1.0;

		// Optional precomputed gradient magnitude |grad Y_t| (width * height, units per input sample).
		// When nullptr, Stage A computes inline 3x3 Sobel gradient magnitude (normalized by 1/8).
		const float* edgeMagMap = nullptr;

		// Optional P8 block labels & CE-WATER guard maps (grid guardGridW x guardGridH, block size guardBlockSrcPx).
		const uint8_t* blockLabels = nullptr;   // CameraModel::Label values
		const uint8_t* blockExcluded = nullptr; // Nonzero = bar / text-guard excluded
		const float* waterProb = nullptr;       // CE-WATER water probability in [0, 1]
		int guardGridW = 0;
		int guardGridH = 0;
		double guardBlockSrcPx = 16.0;
	};

	struct CellDescriptor
	{
		float maxAbsZ = 0.0f;      // Peak |z_Y| in 4x4 cell
		float zCell = 0.0f;        // Signed Neyman-Pearson matched-filter z-score sum(z_Y) / sqrt(N_act)
		float maxChi2 = 0.0f;      // Peak z_Y^2 + z_Cb^2 + z_Cr^2 in cell (when chroma active)
		float cellChi2 = 0.0f;     // Matched-filter 3-DOF chi^2 in cell (when chroma active)
		uint16_t growCount = 0;    // Samples with |z| >= kGrowSigma (or chroma grow)
		uint16_t activeCount = 0;  // Valid unmasked samples in cell
		uint8_t flags = 0;         // Bit 0: active cell, Bit 1: matched-filter streak trigger, Bit 2: guard ok
	};

	struct ResidualOutput
	{
		BlobCandidate blobs[kMaxBlobs];
		uint32_t blobCount = 0;
		uint32_t droppedBlobs = 0;
		uint32_t activeCells = 0;
		uint32_t abstainReason = AbstainReason::None;
		bool clutterOverflow = false;
		bool chromaUsed = false;
		bool causalMode = false;
		float noiseSigmaEst = 0.0f; // Estimated baseline diff noise floor (sqrt(2*sigma_g^2 + 2*sigma_q^2))

		void Clear()
		{
			blobCount = 0;
			droppedBlobs = 0;
			activeCells = 0;
			abstainReason = AbstainReason::None;
			clutterOverflow = false;
			chromaUsed = false;
			causalMode = false;
			noiseSigmaEst = 0.0f;
		}
	};

	// Caller-owned scratch workspace so steady-state frame processing performs zero heap allocations.
	struct ResidualWorkspace
	{
		std::vector<float> diffMinus;
		std::vector<float> signedResY;
		std::vector<float> zMapY;
		std::vector<float> sigmaDiffY;
		std::vector<float> bilinErrY;
		std::vector<float> signedResCb;
		std::vector<float> signedResCr;
		std::vector<float> bilinErrCb;
		std::vector<float> bilinErrCr;
		std::vector<float> chi2Map;
		std::vector<float> bgLuma;
		std::vector<uint8_t> sampleMask;       // Bit 0: valid, Bit 1: seed, Bit 2: grow, Bit 3: overlay, Bit 4: water, Bit 5: nearLargeObj
		std::vector<uint8_t> visited;
		std::vector<CellDescriptor> cells;
		std::vector<uint32_t> activeCellIndices;
		std::vector<uint8_t> largeObjectBlocks;
		std::vector<uint8_t> nearLargeObjectBlocks;
		std::vector<int> bfsQueue;
		std::vector<int> componentIndices;
		std::vector<float> madScratch;
		std::vector<float> madScratchCb;
		std::vector<float> madScratchCr;
		std::vector<BlobCandidate> candidateScratch;
		std::vector<uint8_t> cellPopulated;    // Sparse GPU extraction: 1 if 4x4 cell was populated on-demand this frame
		std::vector<uint32_t> populatedCells;  // Sparse GPU extraction: list of populated cells to clean at frame end
		bool gpuSparseClean = false;           // True only when sampleMask/visited/cellPopulated are all-zero outside active cells

		void Reserve(int width, int height, int guardGridW = 0, int guardGridH = 0);
	};

	// Runs Stage A on a 3-frame triplet. Deterministic, fixed scan order, zero static state.
	// Returns true if Stage A executed (even when 0 blobs found); returns false on hard abstain
	// (invalid input, scene discontinuity, unusable camera, or clutter overflow).
	bool DetectFastObjectBlobs(
		const Config& cfg,
		const ResidualInput& in,
		ResidualWorkspace& ws,
		ResidualOutput& out);
}
