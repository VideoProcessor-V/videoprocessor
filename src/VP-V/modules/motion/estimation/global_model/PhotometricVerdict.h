// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the verdict on a photometric refinement (plan lead20 W3). The fit calls PhotometricVerdict() ONCE at the
// point where the R3 result is applied to the chosen model. Today it returns the R3 result unchanged (empty stub, agreed with
// lane B, which fills it: R3's own rule, then a photometric-disagreement demotion). Pure C++14, no Windows headers.
#pragma once

#include "PhotometricRefine.h"

namespace CameraModel
{
	// vectorModel: the vector fit's chosen model before the refine; refined: R3's answer; precisionPx: the pair's final vector
	// precision (source px). Returns the result the fit must apply (status Refined = use result.model).
	inline RefineResult PhotometricVerdict(const Params& vectorModel, const RefineResult& refined, double precisionPx)
	{
		(void)vectorModel;
		(void)precisionPx;
		return refined;
	}
}
