#include <pch.h>
#include "OutputEncodingRepair.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/colorspace.h>
#include <libplacebo/shaders/sampling.h>

namespace OutputEncodingRepair
{
	bool Needed(const std::vector<const pl_hook*>& hooks, const pl_render_errors* errors,
		enum pl_lut_type targetLut)
	{
		if (targetLut == PL_LUT_CONVERSION)
			return false;
		for (const pl_hook* hook : hooks)
		{
			if (!hook || !(hook->stages & PL_HOOK_PRE_OUTPUT) || hook->input != PL_HOOK_SIG_TEX)
				continue;
			bool disabled = false;
			if (errors)
				for (int i = 0; i < errors->num_disabled_hooks; ++i)
					disabled = disabled || errors->disabled_hooks[i] == hook->signature;
			if (!disabled)
				return true;
		}
		return false;
	}

	Hook::Hook()
	{
		hook.stages = PL_HOOK_OUTPUT;
		hook.input = PL_HOOK_SIG_TEX;
		hook.priv = this;
		hook.hook = &Hook::Run;
		hook.signature = 0x56504F5554524550ull;   // "VPOUTREP"
	}

	pl_hook_res Hook::Run(void* priv, const pl_hook_params* params)
	{
		return static_cast<Hook*>(priv)->Repair(params);
	}

	pl_hook_res Hook::Repair(const pl_hook_params* params)
	{
		pl_hook_res none{};
		none.output = PL_HOOK_SIG_NONE;
		if (!params->tex) return none;

		// What pass_output_target() would have done between PRE_OUTPUT and
		// OUTPUT: encode to the normalised target representation, with the
		// alpha mode left alone for images of fewer than 4 components.
		pl_color_repr repr = target;
		pl_color_repr_normalize(&repr);
		if (params->repr.alpha == repr.alpha || params->components < 4)
			repr.alpha = PL_ALPHA_NONE;

		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = params->components;
		if (!pl_shader_sample_nearest(sh, &src))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}
		pl_shader_encode_color(sh, &repr);
		pl_tex out = params->get_tex(params->priv, params->tex->params.w, params->tex->params.h);
		if (!out)
		{
			pl_dispatch_abort(params->dispatch, &sh);
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}
		pl_dispatch_params dispatch{};
		dispatch.shader = &sh;
		dispatch.target = out;
		if (!pl_dispatch_finish(params->dispatch, &dispatch))
		{
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}
		++applied;
		pl_hook_res res{};
		res.output = PL_HOOK_SIG_TEX;
		res.tex = out;
		res.repr = params->repr;
		res.color = params->color;
		res.components = params->components;
		res.rect = params->rect;
		return res;
	}
}
