// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <vprenderer/NlsPipelinePreparation.h>
#include <VP-V/modules/scaling/rebase/RebaseWarmup.h>
#include <VP-V/modules/scaling/rebase/SourceRebase.h>

#include <libplacebo/dispatch.h>

#include <cmath>
#include <memory>
#include <sstream>
#include <string>

// Re-base shader warm-up job (scaling.rebase; fix FX-RB). Runs on the generic preparation worker
// (NlsPipelinePreparation: one background thread, generation-aware, no lock held while compiling).
// Only CPU-owned snapshots cross the thread boundary: the worker owns a PRIVATE D3D11 device, the
// re-base module, its dispatch, a renderer and every texture it makes; the only shared object is
// libplacebo's pl_cache, which is documented thread-safe and content-addressed. The renderer's own
// first use of the same shader then finds the translated SPIR-V / HLSL / DXBC in that cache and
// skips the seconds-long translation.
//
// What is warmed, from the real contracts rather than guesses: (1) the re-base passes themselves
// (SourceRebase::Module::Apply on a frame with the live plane sizes, the candidate factors and the
// identified kernel, for the main slot and the crossfade slot); (2) the main libplacebo render of the
// RE-BASED planes to the live target with the live render parameters (VP-owned hooks are private to
// the renderer's device and are not part of this; see README "Not warmed").
namespace VpRebaseWarmup
{
	// A stable identity for one candidate: the revision (device / settings generation), the source
	// frame's colour and plane contract, the candidate factors (as the exact scaled plane sizes, so
	// nothing is rounded twice or hard-coded) and the identified kernel.
	inline std::string MakeKey(unsigned long long revision, const pl_frame& source, const pl_frame& target,
		double factorX, double factorY, int kernel)
	{
		std::ostringstream key;
		key << "rebase|" << revision << '|' << kernel;
		for (const pl_frame* frame : { &source, &target })
		{
			key << '|' << frame->num_planes << '|' << frame->repr.sys << '|' << frame->repr.levels << '|'
				<< frame->repr.alpha << '|' << frame->repr.bits.sample_depth << ',' << frame->repr.bits.color_depth
				<< ',' << frame->repr.bits.bit_shift << '|' << frame->color.primaries << '|' << frame->color.transfer
				<< '|' << frame->rotation << '|' << frame->field << '|' << (frame->lut ? frame->lut->signature : 0)
				<< '|' << frame->lut_type << '|' << bool(frame->icc) << '|' << frame->film_grain.type;
			for (int i = 0; i < frame->num_planes; ++i)
			{
				const pl_plane& plane = frame->planes[i];
				if (!plane.texture) return std::string();
				const pl_tex_params& p = plane.texture->params;
				key << '|' << p.w << 'x' << p.h << ':' << (p.format ? p.format->name : "?") << ':' << plane.components
					<< ':' << plane.flipped << ':' << plane.shift_x << ':' << plane.shift_y;
				for (int mapping : plane.component_mapping) key << ':' << mapping;
			}
		}
		const pl_tex luma = source.num_planes > 0 ? source.planes[0].texture : nullptr;
		if (!luma) return std::string();
		key << "|to:" << SourceRebase::Scaled(luma->params.w, factorX) << 'x' << SourceRebase::Scaled(luma->params.h, factorY);
		return key.str();
	}

	inline std::string Label(const pl_frame& source, double factorX, double factorY)
	{
		const pl_tex luma = source.num_planes > 0 ? source.planes[0].texture : nullptr;
		if (!luma) return "?";
		return std::to_string(SourceRebase::Scaled(luma->params.w, factorX)) + "x" +
			std::to_string(SourceRebase::Scaled(luma->params.h, factorY));
	}

	// One request: CPU snapshots of the live source / target frames and the live render parameters
	// (hooks, callbacks and the info sink are dropped by the copy), the candidate factors and kernel.
	struct Job
	{
		NlsPipelinePreparation::Request base;
		double factorX = 1.0, factorY = 1.0;
		int kernel = -1;
		LUID adapter{};
		int featureLevel = 0;
		bool noCompute = false;

		// Throws (std::runtime_error / bad_alloc) when a frame carries state the snapshot refuses
		// (external ICC objects, acquire/release callbacks, H.274 grain): the caller then lets the
		// re-base through on demand, visibly, instead of holding it forever.
		Job(const pl_frame& source, const pl_frame& target, const pl_render_params& params)
			: base(source, target, params, std::string()) {}

		bool Compile(pl_cache cache)
		{
			struct Owned
			{
				pl_d3d11 device = nullptr;
				pl_renderer renderer = nullptr;
				pl_dispatch dispatch = nullptr;
				SourceRebase::Module module;
				Job& job;
				explicit Owned(Job& j) : job(j) {}
				~Owned()
				{
					if (!device) return;
					pl_gpu gpu = device->gpu;
					module.Release(gpu);
					job.base.source.Destroy(gpu);
					job.base.target.Destroy(gpu);
					if (dispatch) pl_dispatch_destroy(&dispatch);
					pl_renderer_destroy(&renderer);
					pl_d3d11_destroy(&device);
				}
			} owned(*this);

			pl_d3d11_params p{};
			p.adapter_luid = adapter;
			p.no_compute = noCompute;
			p.min_feature_level = p.max_feature_level = featureLevel;
			owned.device = pl_d3d11_create(nullptr, &p);
			if (!owned.device) return false;
			pl_gpu gpu = owned.device->gpu;
			pl_gpu_set_cache(gpu, cache);
			owned.renderer = pl_renderer_create(nullptr, gpu);
			owned.dispatch = pl_dispatch_create(nullptr, gpu);
			if (!owned.renderer || !owned.dispatch || !base.source.Create(gpu) || !base.target.Create(gpu)) return false;

			owned.module.SetKernel(kernel);
			// The main slot, then the crossfade slot (same passes; its own textures).
			pl_frame rebased{};
			if (!owned.module.Apply(gpu, owned.dispatch, base.source.value, factorX, factorY, rebased, 0)) return false;
			pl_frame crossfade{};
			if (!owned.module.Apply(gpu, owned.dispatch, base.source.value, factorX, factorY, crossfade,
				SourceRebase::Module::kSlotCrossfade)) return false;
			// The main render of the re-based planes (hooks were dropped by the snapshot).
			rebased.prev = rebased.next = nullptr;
			if (!pl_render_image(owned.renderer, &rebased, &base.target.value, &base.options->params)) return false;
			pl_gpu_finish(gpu);
			return true;
		}
	};
}
