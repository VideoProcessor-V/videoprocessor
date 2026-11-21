// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the kEdgeMap DISPATCH helpers (the edge-map law, BusyEdgeGlsl.h body, on a dispatch the caller
// hands in). Moved unchanged out of EdgeMapFill.h (CB train, PrivateTextureScanTests.OwnerFillsTakeNoPrivateGpu, C-T4 rule 2: a
// file that defines the owner fill takes no pl_dispatch parameter). The owner fill (EdgeMapFill.h) hands the context's dispatch
// straight in, `static_cast<pl_dispatch>(req.gpu->Dispatch())` (rule 4: a helper is handed the cast itself), as composite5's
// P1GpuFill DispatchPass does; X5's visible no-memo fallback (AdaptiveDownscale.cpp) hands its own. One header, one body, one law
// for both callers (rule 20). This header names no owner-fill request type, so it stays outside that scan. Header-only, C++14.
#include <VP-V/modules/input/source_analysis/regions/BusyEdgeGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomain.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomainGlsl.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <string>

namespace Vpv
{
	namespace Regions
	{
		// One dispatch of the edge-map law (BusyEdgeGlsl.h body) over a w x h output: both variants share it.
		inline bool DispatchEdgeMap(pl_dispatch dispatch, const std::string& header, const pl_shader_desc* desc, int numDesc,
			const pl_shader_var* vars, int numVars, int w, int h, std::string& why)
		{
			const std::string body = RegionEvidence::EdgeMapBodyGlsl();
			pl_custom_shader custom{};
			custom.description = "VP-V edge map";
			custom.header = header.c_str();
			custom.body = body.c_str();
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc;
			custom.num_descriptors = numDesc;
			custom.variables = vars;
			custom.num_variables = numVars;
			custom.compute = true;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				why = "edge_map: shader rejected";
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (w + 15) / 16;
			compute.dispatch_size[1] = (h + 15) / 16;
			compute.dispatch_size[2] = 1;
			if (!pl_dispatch_compute(dispatch, &compute))
			{
				why = "edge_map: dispatch failed";
				return false;
			}
			return true;
		}

		// The STAGE variant's law on one picture (CB slice 18 S3): RG32F `dst` at the picture's size, R = Sobel |G| and G = the
		// measure luma of `picture` in the measure domain of its colour facts (VpMeasure::MeasureDomainFor), taps clamped to the
		// whole texture. The owner fill (EdgeMapFill.h FillEdgeMapStage) and X5's visible no-memo fallback (AdaptiveDownscale.cpp:
		// no skeleton memo on this thread, i.e. the Original pathway or a test host) both dispatch THIS function: one header, one
		// body, one law (rule 20), never a second copy. False = `why` names the failure.
		inline bool DispatchEdgeMapStage(pl_dispatch dispatch, pl_tex picture, const pl_color_space& color, pl_color_system system,
			pl_color_primaries primaries, pl_tex dst, std::string& why)
		{
			if (dispatch == nullptr || picture == nullptr || dst == nullptr)
			{
				why = "edge_map: no dispatch, picture or output texture";
				return false;
			}
			const VpMeasure::MeasureDomain domain = VpMeasure::MeasureDomainFor(color, system, primaries);
			pl_shader_desc desc[2]{};
			desc[0].desc.name = "src";
			desc[0].desc.type = PL_DESC_SAMPLED_TEX;
			desc[0].binding.object = picture;
			desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			desc[1].desc.name = "em_out";
			desc[1].desc.type = PL_DESC_STORAGE_IMG;
			desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc[1].binding.object = dst;
			const float linearIn = domain.mode > 0.5f ? 1.0f : 0.0f;
			const bool hdrDecode = domain.mode > 1.5f;
			const pl_shader_var vars[5] = {
				{ pl_var_float("linear_in"), &linearIn, true },
				{ pl_var_float("ad_trc"), &domain.transfer.mode, true },
				{ pl_var_float("ad_a"), &domain.transfer.a, true },
				{ pl_var_float("ad_b"), &domain.transfer.b, true },
				{ pl_var_float("ad_g"), &domain.transfer.g, true } };
			const std::string header = "#define AD_KW " + VpMeasure::LumaWeightsGlsl(domain) + "\n" + (hdrDecode ? "#define AD_HDR 1\n" : "")
				+ VpMeasure::MeasureLumaGlsl() + RegionEvidence::EdgeMapStageHeaderGlsl();
			return DispatchEdgeMap(dispatch, header, desc, 2, vars, hdrDecode ? 5 : 1, picture->params.w, picture->params.h, why);
		}
	}
}
