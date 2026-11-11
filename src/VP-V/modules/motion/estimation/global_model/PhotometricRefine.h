// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: DIRECT PHOTOMETRIC REFINEMENT of the chosen camera model (doc 103 s3 fix R3).
// Inverse-compositional Gauss-Newton on the producer's luma pair: the vector fit's model (any type) is polished
// to sub-pixel accuracy on the PIXELS of the pair (the only information independent of the NVOF field), over the
// fit's camera-inlier blocks only (movers / overlays / bars masked), with Huber weights and optional fade
// gain/offset terms. Verified against the record: when the photometric residual does not fall (or the step
// leaves the basin, the fade terms go out of range, or the system is degenerate) the ORIGINAL model is returned
// and the status says why. CPU reference (the GPU version is a later item). Pure C++14, deterministic (fixed
// evaluation order, double accumulation, single thread, no clocks), no Windows headers. See README.md
// "Photometric refinement (R3)".
#pragma once

#include <cstdint>
#include <vector>

#include "CameraMotion.h"

namespace CameraModel
{
	// One luma plane at engine scale: exactly one of data8 (the producer's 8-bit HalfLuma8 / quarter buffer) or
	// data16 (R16 half luma, core/Products.h P1) is set.
	struct LumaPlane
	{
		const uint8_t* data8 = nullptr;
		const uint16_t* data16 = nullptr;
		int width = 0, height = 0;
		int stride = 0;                             // elements per row (>= width)
		bool Valid() const
		{
			return ((data8 != nullptr) != (data16 != nullptr)) && width > 0 && height > 0 && stride >= width;
		}
	};

	// The pair: `from` = frame pairFrom (N-1, the template), `to` = frame pairTo (N). Both planes cover the WHOLE
	// frame (not only the active picture) at the same size. Luma pixel (i, j) is the box of source pixels
	// [s*i, s*(i+1)) x [s*j, s*(j+1)), centre s*(i + 0.5) in frame px (s = sourcePxPerPixel: 2 for the half-size
	// search, 4 for the quarter-size 4K search; the producer's HalfLuma8 2x2 box and its quarter 2x2 box of that).
	struct LumaPairView
	{
		LumaPlane from, to;
		double sourcePxPerPixel = 0;
		double codeScale = 1;                       // codes x codeScale = 8-bit-equivalent units (1 for 8-bit; 255/65535 for R16 unorm)
	};

	// The fit's per-block labels (BlockData of the record, frame N-1 coordinates, grid anchored at the frame's top-left).
	// A luma pixel is used only when its block is Label::Camera and not excluded. label == nullptr = no mask (every
	// picture pixel is a candidate; Huber weights only): the result says maskUsed = false.
	struct InlierMask
	{
		const uint8_t* label = nullptr;             // Label per block, gridW * gridH
		const uint8_t* excluded = nullptr;          // optional, 1 = bar / text-guard block
		int gridW = 0, gridH = 0;
		double blockSize = 0;                       // source px per block
	};
	// The record's labels (empty arrays, e.g. labelsOnGpu records, give an empty mask).
	InlierMask MaskOf(const BlockData& blocks);

	enum class RefineStatus : uint8_t
	{
		Refined,                 // the refined model is returned
		FellBackNoImprovement,   // photometric residual did not fall: the input model is returned
		FellBackTooFar,          // the correction left the sub-pixel basin (> maxCorrectionLumaPx): input model returned
		FellBackPhotometric,     // photometric gain outside [minGain, maxGain] (e.g. a cut to a flat picture): input model returned
		FellBackDegenerate,      // singular normal equations / degenerate model / too few pixels after warping
		SkippedNoLuma,           // no (or mismatched) luma pair: nothing done
		SkippedFewPixels,        // fewer than minPixels usable gradient pixels in the inlier blocks
		SkippedModel             // model type None or a degenerate input model
	};
	const char* RefineStatusText(RefineStatus status);
	inline bool RefineFellBack(RefineStatus s)
	{
		return s == RefineStatus::FellBackNoImprovement || s == RefineStatus::FellBackTooFar || s == RefineStatus::FellBackPhotometric
			|| s == RefineStatus::FellBackDegenerate;
	}

	enum class RefineInterp : uint8_t { Bilinear, Bicubic };

	struct RefineOptions
	{
		int targetPixels = 50000;                   // strongest-gradient inlier pixels kept (deterministic: gradient-bin threshold, raster order)
		int maxIterations = 4;                      // Gauss-Newton steps (2-4 typical; stops earlier on convergence)
		int minPixels = 2000;
		bool fadeTerms = true;                      // solve gain/offset with the geometry (I(W(x)) = gain * T(x) + offset)
		bool translationOnly = false;               // false: Translation models refine 2 parameters, every other type a 4-parameter similarity correction
		double huberK = 1.345;                      // Huber threshold in robust sigmas (1.4826 x median |r|)
		double minScaleCodes = 0.5;                 // floor of the robust sigma (8-bit-equivalent codes)
		double minGradientCodes = 4;                // |T(x+1)-T(x-1)| + |T(y+1)-T(y-1)| below this: not a gradient pixel
		double borderLumaPx = 3;                    // margin inside the active picture
		double maxCorrectionLumaPx = 1.0;           // largest accepted correction at the picture corners / centre, in luma px
		double convergeLumaPx = 0.001;              // stop when a step moves no corner by more than this (luma px)
		double minGain = 0.5, maxGain = 2.0;
		double minImprovement = 1e-6;               // accept only when after < before * (1 - minImprovement) (rounding-level gains are not improvements)
		RefineInterp interp = RefineInterp::Bicubic;// sampling of frame N (Keys a = -0.5); Bilinear is phase-biased (tests)
	};

	struct RefineResult
	{
		RefineStatus status = RefineStatus::SkippedNoLuma;
		Params model;                               // the refined model (Refined) or the input model unchanged; type always the input type
		double gain = 1, offset = 0;                // fade terms (1 / 0 unless Refined with fadeTerms)
		double residualBefore = 0, residualAfter = 0;   // mean Huber cost (codes^2) of the input / returned-candidate model, same pixels and sigma
		double robustSigma = 0;                     // codes, the final iteration's
		double correctionPx = 0;                    // largest corner / centre displacement change of the candidate vs the input, source px
		int iterations = 0;                         // Gauss-Newton steps taken
		double lastStepPx = 0;                      // largest corner move of the last Gauss-Newton step, source px (convergence check)
		uint32_t candidates = 0;                    // gradient pixels in the inlier blocks
		uint32_t pixels = 0;                        // pixels used (<= targetPixels)
		bool maskUsed = false;
	};

	// Visible counters (doc 103 R3): refined / fell back / skipped (no luma) / skipped (other).
	struct RefineCounters
	{
		uint64_t refined = 0, fellBack = 0, skippedNoLuma = 0, skippedOther = 0;
		void Add(RefineStatus s)
		{
			if (s == RefineStatus::Refined) ++refined;
			else if (RefineFellBack(s)) ++fellBack;
			else if (s == RefineStatus::SkippedNoLuma) ++skippedNoLuma;
			else ++skippedOther;
		}
	};

	// Holds the scratch buffers between calls (cost only; never part of the output) and the counters. NOT thread-safe:
	// one per fit worker, like Fitter.
	class PhotometricRefiner
	{
	public:
		explicit PhotometricRefiner(const RefineOptions& options = RefineOptions()) : m_options(options) {}
		// model: the vector fit's chosen model (picture-centred source px, frame N-1 -> N, CameraMotion.h conventions).
		// picture: the active picture in frame px. Never throws.
		RefineResult Refine(const Params& model, const LumaPairView& luma, const Rect& picture, const InlierMask& mask);
		const RefineCounters& Counters() const { return m_counters; }
		const RefineOptions& Options() const { return m_options; }
		void ResetCounters() { m_counters = RefineCounters(); }

	private:
		struct Impl;
		friend struct Impl;
		RefineOptions m_options;
		RefineCounters m_counters;
		std::vector<uint16_t> m_bin;
		std::vector<double> m_x, m_y, m_t, m_gx, m_gy, m_r0, m_r, m_abs;
		std::vector<uint8_t> m_ok0, m_ok;
	};

	// One-shot form (allocates its own scratch).
	RefineResult PhotometricRefine(const Params& model, const LumaPairView& luma, const Rect& picture, const InlierMask& mask,
		const RefineOptions& options = RefineOptions());
}
