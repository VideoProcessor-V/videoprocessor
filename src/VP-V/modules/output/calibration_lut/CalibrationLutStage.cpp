#include <pch.h>
#include "CalibrationLutStage.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>

namespace CalibrationLutStage
{
	Hook::Hook()
	{
		hook.stages = PL_HOOK_PRE_OUTPUT;
		hook.input = PL_HOOK_SIG_TEX;
		hook.priv = this;
		hook.hook = &Hook::Run;
		hook.signature = 0x5650434C4C555400ull;   // "VPCLLUT"
	}

	Hook::~Hook()
	{
		Release();
	}

	void Hook::Release()
	{
		pl_shader_obj_destroy(&state);
	}

	pl_hook_res Hook::Run(void* priv, const pl_hook_params* params)
	{
		return static_cast<Hook*>(priv)->Apply(params);
	}

	pl_hook_res Hook::Apply(const pl_hook_params* params)
	{
		pl_hook_res none{};
		none.output = PL_HOOK_SIG_NONE;
		if (!params->tex || !lut)
			return none;
		auto failed = [this]()
		{
			++failures;
			pl_hook_res res{};
			res.failed = true;
			return res;
		};

		pl_shader sh = pl_dispatch_begin(params->dispatch);
		pl_sample_src src{};
		src.tex = params->tex;
		src.components = params->components;
		if (!pl_shader_sample_nearest(sh, &src))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return failed();
		}
		// BTB/WTW: the part outside 0..1 is kept aside (a shader-global, declared in the
		// prelude) and added back after the LUT, which clamps its input to 0..1.
		pl_custom_shader keep{};
		keep.prelude = "vec3 vp_calibration_excess;";
		keep.body = "vp_calibration_excess = color.rgb - clamp(color.rgb, 0.0, 1.0);\n"
			"color.rgb = clamp(color.rgb, 0.0, 1.0);";
		keep.input = PL_SHADER_SIG_COLOR;
		keep.output = PL_SHADER_SIG_COLOR;
		if (!pl_shader_custom(sh, &keep))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return failed();
		}
		pl_shader_custom_lut(sh, lut, &state);
		pl_custom_shader restore{};
		restore.body = "color.rgb += vp_calibration_excess;";
		restore.input = PL_SHADER_SIG_COLOR;
		restore.output = PL_SHADER_SIG_COLOR;
		if (!pl_shader_custom(sh, &restore))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return failed();
		}
		pl_tex out = params->get_tex(params->priv, params->tex->params.w, params->tex->params.h);
		if (!out)
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return failed();
		}
		pl_dispatch_params dispatch{};
		dispatch.shader = &sh;
		dispatch.target = out;
		if (!pl_dispatch_finish(params->dispatch, &dispatch))
			return failed();
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
