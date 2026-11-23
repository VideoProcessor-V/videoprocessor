// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the wiring of the direct photometric refinement (R3, PhotometricRefine.h) into the per-pair fit.
// One pair's luma pair goes in through PairInput::photometric; the outcome comes back out of the same struct, so the link
// (CameraFitProducer) can count it and show it (Ctrl+I, notice, Config, dump). Pure C++14, no Windows headers.
// See README.md "Photometric refinement (R3)".
#pragma once

#include <cstdint>

#include "PhotometricRefine.h"

namespace CameraModel
{
	// What the wiring did with one pair. The counters of the link are one per value (no silent fallback).
	enum class PhotometricOutcome : uint8_t
	{
		NotAttempted,      // no PhotometricInput on the pair, or the fit never reached the refine (invalid / cut / flash)
		Refined,           // the refined model replaced the vector fit's model
		FellBack,          // the refine verified against the record and returned the input model (reason in result.status)
		SkippedNoLuma,     // no usable luma pair arrived with the field
		SkippedResolution, // the luma form is too coarse for the accuracy target at this frame size (measured rule below)
		SkippedOther       // too few pixels / no model / a degenerate input model
	};
	inline const char* PhotometricOutcomeText(PhotometricOutcome o)
	{
		switch (o)
		{
		case PhotometricOutcome::Refined: return "refined";
		case PhotometricOutcome::FellBack: return "fell back";
		case PhotometricOutcome::SkippedNoLuma: return "skipped (no luma)";
		case PhotometricOutcome::SkippedResolution: return "skipped (resolution)";
		case PhotometricOutcome::SkippedOther: return "skipped (other)";
		default: return "not attempted";
		}
	}

	// RESOLUTION RULE (CAMR3-WIRING-GUARDS 2026-11-12, README R3). The refine's accuracy target is 0.02 source px at 1080 and
	// grows linearly with the source height (0.04 at 4K). The measured model error of the luma form is proportional to its
	// sampling step: kFormErrorPerSourcePx x s source px (s = sourcePxPerPixel; form Q, s = 4, measures 0.0205 at 1080,
	// 0.005125 per unit of s). A form is used only when its error is within the target at the frame's height, so 1080 takes
	// the half-size form (s = 2: 0.01025) and 4K may take the quarter-size one (s = 4: 0.0205 <= 0.04). No resolution literal.
	constexpr double kFormErrorPerSourcePx = 0.005125;    // CAMR3-WIRING-GUARDS 2026-11-12: 0.0205 / 4
	constexpr double kAccuracyTargetAt1080Px = 0.02;      // README R3 accuracy target at 1080 source rows
	constexpr double kAccuracyReferenceRows = 1080.0;
	inline bool PhotometricFormPasses(double sourcePxPerPixel, double frameRows)
	{
		if (!(sourcePxPerPixel > 0) || !(frameRows > 0))
			return false;
		return kFormErrorPerSourcePx * sourcePxPerPixel <= kAccuracyTargetAt1080Px * frameRows / kAccuracyReferenceRows;
	}

	// The refine's request and answer for one pair. luma is read-only input; everything after it is written by the fit.
	// The planes must stay valid for the whole Fit() call (the caller owns them).
	struct PhotometricInput
	{
		LumaPairView luma;
		PhotometricOutcome outcome = PhotometricOutcome::NotAttempted;
		RefineResult result;                         // the refine's own answer (status, correctionPx, residuals) when attempted
	};
}
