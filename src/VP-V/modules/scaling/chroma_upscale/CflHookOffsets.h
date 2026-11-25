// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale: per-frame chroma-siting offset propagation for the bundled CfL_Prediction.glsl
// hook (GEM-066). Declaration-only header (0 inline bodies); implementations live in CflHookOffsets.cpp.
#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <microsoft_directshow/MadVRShaderLoader.h>

#include <vector>

namespace ChromaUpscale
{
	// Chroma siting offsets in luma-pixel units, matching libplacebo's pl_plane::shift_x / shift_y and
	// CfL_Prediction.glsl's chroma_offset_x / chroma_offset_y. Defaults (-0.5, 0.0) are PL_CHROMA_LEFT
	// (even-column co-sited horizontal, vertically centred), matching pl_chroma_location_offset(PL_CHROMA_LEFT).
	struct CflOffsets
	{
		bool valid = false;
		double x = -0.5;
		double y = 0.0;
	};

	CflOffsets CflOffsetsFromFrame(const pl_frame& frame);
	bool RuleSetsCflParam(const std::vector<ConfiguredShaderRule>& rules, const char* paramName);
	void ApplyCflHookOffsets(const pl_frame& frame, const std::vector<const pl_hook*>& hooks, const std::vector<ConfiguredShaderRule>& rules, bool originalPathway);
}
