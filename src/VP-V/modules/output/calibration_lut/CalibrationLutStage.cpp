#include <pch.h>
#include "CalibrationLutStage.h"

#include <VP-V/modules/colour/extended_range/ExtendedRange.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>

#include <algorithm>
#include <vector>

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
		if (gpu && slopeTex)
			pl_tex_destroy(gpu, &slopeTex);
		slopeTex = nullptr;
		slopeLut = nullptr;
		slopeData = nullptr;
		slopeSignature = 0;
		gpu = nullptr;
	}

	bool Hook::EnsureSlopeTexture(pl_gpu g)
	{
		if (gpu && gpu != g && slopeTex)
			pl_tex_destroy(gpu, &slopeTex);
		gpu = g;
		// The LUT's own copy for the edge slope (pl_custom_lut keeps its texture private): as
		// ExtendedRange's late LUT restore, rgba32f, only for a LUT it can carry.
		if (!gpu || !ExtendedRange::LutCarryable(lut))
			return false;
		if (slopeTex && slopeLut == lut && slopeData == lut->data && slopeSignature == lut->signature &&
			slopeTex->params.w == lut->size[0] && slopeTex->params.h == lut->size[1] && slopeTex->params.d == lut->size[2])
			return true;
		if (slopeTex)
			pl_tex_destroy(gpu, &slopeTex);
		slopeLut = nullptr;
		const int maxDim = (std::max)({ lut->size[0], lut->size[1], lut->size[2] });
		if (gpu->limits.max_tex_3d_dim < static_cast<uint32_t>(maxDim))
			return false;
		const pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
		if (!fmt)
			return false;
		const size_t count = static_cast<size_t>(lut->size[0]) * lut->size[1] * lut->size[2];
		std::vector<float> rgba(count * 4);
		for (size_t i = 0; i < count; ++i)
		{
			rgba[i * 4 + 0] = lut->data[i * 3 + 0];
			rgba[i * 4 + 1] = lut->data[i * 3 + 1];
			rgba[i * 4 + 2] = lut->data[i * 3 + 2];
			rgba[i * 4 + 3] = 1.0f;
		}
		pl_tex_params tp{};
		tp.w = lut->size[0];
		tp.h = lut->size[1];
		tp.d = lut->size[2];
		tp.format = fmt;
		tp.sampleable = true;
		tp.initial_data = rgba.data();
		slopeTex = pl_tex_create(gpu, &tp);
		if (!slopeTex)
			return false;
		slopeLut = lut;
		slopeData = lut->data;
		slopeSignature = lut->signature;
		return true;
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
		// BTB/WTW: the part outside 0..1 is kept aside (shader-globals, declared in the
		// prelude) and added back after the LUT, which clamps its input to 0..1. Bug hunt
		// 2026-10-31 F2: added through the LUT's edge slope at the clamped point, per axis, as a
		// libplacebo-stage LUT's is (ExtendedRange's late LUT restore, LutExtrapolate), so the
		// out-of-range part gets the LUT's gain and cross-channel correction too. Slope 1 (the
		// excess unchanged) only when the LUT cannot be carried (shaper matrices, a side over 256,
		// no 3D texture): counted in SlopeFallbacks().
		const bool slope = EnsureSlopeTexture(params->gpu);
		if (slope) ++slopeApplied; else ++slopeFallbacks;
		pl_custom_shader keep{};
		keep.prelude = "vec3 vp_calibration_excess;\nvec3 vp_calibration_clamped;";
		keep.body = "vp_calibration_clamped = clamp(color.rgb, 0.0, 1.0);\n"
			"vp_calibration_excess = color.rgb - vp_calibration_clamped;\n"
			"color.rgb = vp_calibration_clamped;";
		keep.input = PL_SHADER_SIG_COLOR;
		keep.output = PL_SHADER_SIG_COLOR;
		if (!pl_shader_custom(sh, &keep))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return failed();
		}
		pl_shader_custom_lut(sh, lut, &state);
		pl_custom_shader restore{};
		const float lutMax[3] = { static_cast<float>(lut->size[0] - 1), static_cast<float>(lut->size[1] - 1),
			static_cast<float>(lut->size[2] - 1) };
		const pl_shader_var slopeVars[1] = { { pl_var_vec3("er_lut_max"), lutMax, true } };
		pl_shader_desc slopeDesc[1]{};
		if (slope)
		{
			slopeDesc[0].desc.name = "er_lut";
			slopeDesc[0].desc.type = PL_DESC_SAMPLED_TEX;
			slopeDesc[0].binding.object = slopeTex;
			slopeDesc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			slopeDesc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			restore.header = ExtendedRange::LutSlopeGlsl();
			restore.descriptors = slopeDesc;
			restore.num_descriptors = 1;
			restore.variables = slopeVars;
			restore.num_variables = 1;
			restore.body =
				"vec3 vp_d = vp_calibration_excess;\n"
				"vec3 vp_c = vp_calibration_clamped;\n"
				"if (vp_d.r != 0.0)\n"
				"    color.rgb += vp_d.r * er_lut_slope(vp_c, vec3(1.0, 0.0, 0.0), vp_d.r);\n"
				"if (vp_d.g != 0.0)\n"
				"    color.rgb += vp_d.g * er_lut_slope(vp_c, vec3(0.0, 1.0, 0.0), vp_d.g);\n"
				"if (vp_d.b != 0.0)\n"
				"    color.rgb += vp_d.b * er_lut_slope(vp_c, vec3(0.0, 0.0, 1.0), vp_d.b);";
		}
		else
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
