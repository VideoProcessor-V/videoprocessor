// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// scaling.output (X5): where the measure pass gets its edge map (CB slice 18 S3; the SHOULD-1 seam of review-cb-s18-r1, pure
// extraction of AdaptiveDownscale.cpp's measure step 2, unchanged behaviour). With a skeleton memo for the frame, the picture X5
// measures is offered to P13 as the stage after scaling.geometry and kEdgeMap is acquired under (seq, that stage, the picture's
// size, the per-seq picture ordinal): the owner's product, computed once per (seq, picture), a second ask is a memo hit. Without a
// memo (the Original pathway, a hook test host), the module computes the map with the owner's own dispatch
// (Regions::DispatchEdgeMapStage, passed in as `computeLocal` so this header stays CPU-testable). Every outcome is visible in
// `status` (the module's status line): cleared when the memo served it, "edge map unavailable (<cause>)" when it did not (the
// caller then takes the measure-failure path), "edge map computed in the module (no skeleton memo)" [+ " - <why>"] without a memo.
// Render thread only (X5's hook). Header-only, C++14.

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/VariantKey.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>
#include <VP-V/modules/scaling/output/EdgeMapStageOrdinal.h>

#include <cstdint>
#include <functional>
#include <string>

namespace AdaptiveDownscale
{
	// The stage X5's picture is offered and its edge map keyed under: after scaling.geometry.
	inline uint8_t MeasureEdgeMapStage()
	{
		return VpSkeleton::VariantKey::StageAfter(static_cast<uint8_t>(PipelineSlots::Id::ScalingGeometry));
	}

	// The edge map the measure reads, or null (`status` says why; the measure fails visibly). `fm`: the frame's memo
	// (Vpv::Skeleton::CurrentFrameMemo()); `ordinals`: X5's per-seq picture ordinal; `base`: the picture measured, with its colour
	// facts; `computeLocal(why)`: the no-memo dispatch into the module's own texture (null + why on failure).
	inline pl_tex AcquireMeasureEdgeMap(const Vpv::Skeleton::FrameMemo& fm, EdgeMapStageOrdinal& ordinals, pl_tex base,
		const pl_color_space& color, pl_color_system system, pl_color_primaries primaries,
		const std::function<pl_tex(std::string& why)>& computeLocal, std::string& status)
	{
		if (fm.Valid())
		{
			const uint16_t ordinal = ordinals.Next(fm.seq);
			const uint8_t stage = MeasureEdgeMapStage();
			Vpv::Capture::StagePicture pic;
			pic.seq = fm.seq;
			pic.stage = stage;
			pic.ordinal = ordinal;
			pic.tex = base;
			pic.color = color;
			pic.system = system;
			pic.primaries = primaries;
			pic.origin = "scaling.output (X5)";
			const bool offered = Vpv::Capture::SourcePlanes::OfferStage(fm.host, pic);
			const uint32_t w = base != nullptr ? static_cast<uint32_t>(base->params.w) : 0u;
			const uint32_t h = base != nullptr ? static_cast<uint32_t>(base->params.h) : 0u;
			const VpSkeleton::AcquireResult r = fm.memo->Acquire(VpSkeleton::kEdgeMap, fm.seq, VpSkeleton::AcquireForm::Gpu,
				Vpv::Regions::EdgeMapStageKey(stage, w, h, ordinal));
			if (!r.Ok() || !r.gpu.Valid() || r.gpu.resource == nullptr)
			{
				// Plan line 69: a genuine Unavailable is visible (the status line) and takes the measure-failure path.
				const std::string cause = fm.memo->LastCause(VpSkeleton::kEdgeMap);
				status = "Adaptive downscale: edge map unavailable (" +
					(cause.empty() ? std::string(offered ? "not served" : "stage picture not offered") : cause) + ")";
				return nullptr;
			}
			status.clear();
			return static_cast<pl_tex>(r.gpu.resource);
		}
		// No skeleton memo on this thread (the Original pathway, a test host): visible, and the module dispatches THE shared
		// fill (Regions::DispatchEdgeMapStage, the owner fill's own function: one law, rule 20) into its own texture.
		status = "Adaptive downscale: edge map computed in the module (no skeleton memo)";
		std::string why;
		const pl_tex local = computeLocal ? computeLocal(why) : nullptr;
		if (local == nullptr && !why.empty())
			status += " - " + why;
		return local;
	}
}
