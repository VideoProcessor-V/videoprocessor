#include <pch.h>
#include "SourceRebase.h"

#include <VP-V/core/ChromaSiting.h>   // VP-V: the plane shifts after the resample

#include <KernelInverse.h>
#include <libplacebo/shaders/custom.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace SourceRebase
{
	int ClassHeight(const std::string& setting)
	{
		// General (owner's standing rule): any height, not a fixed list -
		// 1440, 1080, 900, 720, 576 ... native and auto (until step B) are 0.
		// "WIDTHxHEIGHT": the height after the x.
		const size_t x = setting.find('x');
		if (x != std::string::npos)
			return ClassWidth(setting) > 0 ? ClassHeight(setting.substr(x + 1)) : 0;
		if (setting.empty() || setting.find_first_not_of("0123456789") != std::string::npos) return 0;
		const long value = std::strtol(setting.c_str(), nullptr, 10);
		return value >= kMinimumHeight && value <= kMaximumHeight ? static_cast<int>(value) : 0;
	}

	double Factor(const std::string& setting, int nativeHeight)
	{
		const int target = ClassHeight(setting);
		if (target <= 0 || nativeHeight <= 0 || target >= nativeHeight) return 1.0;
		return static_cast<double>(target) / nativeHeight;
	}

	int ClassWidth(const std::string& setting)
	{
		const size_t x = setting.find('x');
		if (x == std::string::npos || x == 0) return 0;
		const std::string w = setting.substr(0, x), h = setting.substr(x + 1);
		if (w.find_first_not_of("0123456789") != std::string::npos || h.empty() ||
			h.find_first_not_of("0123456789") != std::string::npos) return 0;
		const long value = std::strtol(w.c_str(), nullptr, 10);
		return value >= kMinimumHeight && value <= 2 * kMaximumHeight ? static_cast<int>(value) : 0;
	}

	double FactorX(const std::string& setting, int nativeWidth, int nativeHeight)
	{
		const int width = ClassWidth(setting);
		if (width <= 0) return Factor(setting, nativeHeight);
		if (nativeWidth <= 0 || width >= nativeWidth) return 1.0;
		return static_cast<double>(width) / nativeWidth;
	}

	int Scaled(int size, double factor)
	{
		return (std::max)(1, static_cast<int>(std::lround(size * factor)));
	}

	namespace
	{
		// One separable pass of the kernel's inverse: each output sample from up to 80
		// input samples along the axis (2i + first + t), clamped at the edges.
		const char* const INVERSE_BODY = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    ivec2 last = textureSize(src, 0) - ivec2(1);
    vec4 acc = vec4(0.0);
    for (int t = 0; t < taps; t++) {
        int j = 2 * (horizontal != 0 ? p.x : p.y) + first + t;
        ivec2 q = horizontal != 0 ? ivec2(clamp(j, 0, last.x), p.y) : ivec2(p.x, clamp(j, 0, last.y));
        acc += weights[t] * texelFetch(src, q, 0);
    }
    imageStore(dst, p, acc);
}
)GLSL";
	}

	bool Module::InversePlane(pl_gpu gpu, pl_dispatch dispatch, pl_tex in, int w, int h, bool acrossInverse, bool downInverse,
		pl_tex out, int slot)
	{
		const auto& list = KernelInverse::Candidates();
		if (kernel < 0 || kernel >= static_cast<int>(list.size()) || (!acrossInverse && !downInverse))
		{
			error = "no kernel";
			return false;
		}
		const KernelInverse::Candidate& c = list[static_cast<size_t>(kernel)];
		float weights[80] = {};
		const int taps = static_cast<int>((std::min)(c.weights.size(), size_t{ 80 }));
		for (int t = 0; t < taps; ++t) weights[t] = c.weights[static_cast<size_t>(t)];
		const int first = c.first;
		auto pass = [&](pl_tex src, pl_tex dst, int horizontal, int ow, int oh) {
			pl_shader_desc d[2] = {};
			d[0].desc.name = "src"; d[0].desc.type = PL_DESC_SAMPLED_TEX;
			d[0].binding.object = src; d[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP; d[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			d[1].desc.name = "dst"; d[1].desc.type = PL_DESC_STORAGE_IMG; d[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			d[1].binding.object = dst;
			pl_var weightVar = pl_var_float("weights");
			weightVar.dim_a = 80;
			const pl_shader_var v[4] = {
				{ weightVar, weights, false },
				{ pl_var_int("taps"), &taps, false },
				{ pl_var_int("first"), &first, false },
				{ pl_var_int("horizontal"), &horizontal, false } };
			pl_custom_shader custom{};
			custom.description = horizontal ? "VideoProcessor re-base: kernel inverse across" : "VideoProcessor re-base: kernel inverse down";
			custom.body = INVERSE_BODY;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = d;
			custom.num_descriptors = 2;
			custom.variables = v;
			custom.num_variables = 4;
			custom.compute = true;
			custom.compute_group_size[0] = 16;
			custom.compute_group_size[1] = 16;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (ow + 15) / 16;
			compute.dispatch_size[1] = (oh + 15) / 16;
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
		};
		if (acrossInverse && downInverse)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = in->params.h;
			tp.format = out->params.format;
			tp.sampleable = true;
			tp.storable = true;
			if (!pl_tex_recreate(gpu, &inverseTemp[slot], &tp) ||
				!pass(in, inverseTemp[slot], 1, w, in->params.h) || !pass(inverseTemp[slot], out, 0, w, h))
			{
				error = "kernel inverse pass failed";
				return false;
			}
			return true;
		}
		if (!pass(in, out, acrossInverse ? 1 : 0, w, h))
		{
			error = "kernel inverse pass failed";
			return false;
		}
		return true;
	}

	Module::~Module()
	{
		// Textures belong to a device that may already be gone; the renderer
		// calls Release while it exists.
	}

	void Module::Release(pl_gpu gpu)
	{
		for (int slot = 0; slot < kSlots; ++slot)
		{
			if (inverseTemp[slot] && gpu) pl_tex_destroy(gpu, &inverseTemp[slot]);
			inverseTemp[slot] = nullptr;
		}
		for (int slot = 0; slot < kSlots; ++slot)
			for (int plane = 0; plane < PL_MAX_PLANES; ++plane)
			{
				if (textures[slot][plane] && gpu) pl_tex_destroy(gpu, &textures[slot][plane]);
				textures[slot][plane] = nullptr;
				if (alternate[slot][plane] && gpu) pl_tex_destroy(gpu, &alternate[slot][plane]);
				alternate[slot][plane] = nullptr;
				pl_shader_obj_destroy(&luts[slot][plane]);
				if (separableTemp[slot][plane] && gpu) pl_tex_destroy(gpu, &separableTemp[slot][plane]);
				separableTemp[slot][plane] = nullptr;
				pl_shader_obj_destroy(&lutsDown[slot][plane]);
			}
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, double factorX, double factorY, pl_frame& out, int slot)
	{
		error.clear();
		kernelUsed = false;
		factorX = factorX > 0.0 && factorX < 1.0 ? factorX : 1.0;
		factorY = factorY > 0.0 && factorY < 1.0 ? factorY : 1.0;
		if (!gpu || !dispatch || source.num_planes <= 0 || (factorX >= 1.0 && factorY >= 1.0))
		{
			error = "nothing to re-base";
			return false;
		}
		const bool oneAxis = factorX >= 1.0 || factorY >= 1.0;
		slot = (std::max)(0, (std::min)(slot, kSlots - 1));
		{   // DEBUG (VP_DEBUG_REBASE_PINGPONG=1): alternate between two output texture sets per slot
			char flag[4] = {};
			if (GetEnvironmentVariableA("VP_DEBUG_REBASE_PINGPONG", flag, sizeof(flag)) > 0 && flag[0] == '1')
				for (int plane = 0; plane < PL_MAX_PLANES; ++plane)
					std::swap(textures[slot][plane], alternate[slot][plane]);
		}
		pl_frame result = source;
		for (int plane = 0; plane < source.num_planes; ++plane)
		{
			const pl_tex in = source.planes[plane].texture;
			if (!in || !in->params.sampleable)
			{
				error = "plane " + std::to_string(plane) + " is not sampleable";
				++failures;
				return false;
			}
			const int w = Scaled(in->params.w, factorX), h = Scaled(in->params.h, factorY);
			// A storable (or renderable) format of the plane's own kind: the
			// shader writes the plane's components, its mapping is unchanged.
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = in->params.format;
			// Never below 16 bits: an 8-bit (or 10-bit packed) source plane would requantise the
			// downscale and lose what the 2:1 average gains (audit 2026-10-16).
			if (params.format->component_depth[0] < 16)
				if (pl_fmt wide = pl_find_fmt(gpu, PL_FMT_UNORM, params.format->num_components, 16, 16,
						static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR)))
					params.format = wide;
			params.sampleable = true;
			params.renderable = true;
			params.blit_src = true;   // the smooth-motion history copies source planes
			params.storable = plane == 0 && kernel >= 0 && (params.format->caps & PL_FMT_CAP_STORABLE);   // the inverse passes write it
			if (!(params.format->caps & PL_FMT_CAP_RENDERABLE))
			{
				// Fall back to a float format with the same component count: 32-bit first
				// (C19 / hunt 2 07-L4: fp16 holds 11 bits, about half a 10-bit code near
				// white, below the 16 bits asked for), half float only when 32-bit is not
				// renderable. Logged by the renderer (FormatNote).
				const pl_fmt wanted = params.format;
				const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR);
				params.format = pl_find_fmt(gpu, PL_FMT_FLOAT, wanted->num_components, 32, 32, caps);
				const bool half = !params.format;
				if (half)
					params.format = pl_find_fmt(gpu, PL_FMT_FLOAT, wanted->num_components, 16, 16, caps);
				if (!params.format)
				{
					error = "no renderable format for plane " + std::to_string(plane);
					++failures;
					return false;
				}
				params.storable = plane == 0 && kernel >= 0 && (params.format->caps & PL_FMT_CAP_STORABLE);
				formatNote = "plane " + std::to_string(plane) + ": " + (wanted->name ? wanted->name : "?") +
					" not renderable, " + (params.format->name ? params.format->name : "float") +
					(half ? " (half float: 32-bit float not renderable)" : "");
			}
			const pl_tex beforeRecreate = textures[slot][plane];
			if (pl_tex_recreate(gpu, &textures[slot][plane], &params) && textures[slot][plane] != beforeRecreate)
				++debugRecreations;
			if (!textures[slot][plane])
			{
				error = "texture allocation failed";
				++failures;
				return false;
			}
			// Kernel-matched (luma, exactly 2:1 on each re-based axis, a kernel identified).
			const bool acrossHalf = factorX < 1.0 && w * 2 == in->params.w;
			const bool downHalf = factorY < 1.0 && h * 2 == in->params.h;
			if (plane == 0 && kernel >= 0 && (factorX >= 1.0 || acrossHalf) && (factorY >= 1.0 || downHalf) &&
				(params.format->caps & PL_FMT_CAP_STORABLE))
			{
				if (!InversePlane(gpu, dispatch, in, w, h, acrossHalf, downHalf, textures[slot][plane], slot))
				{
					++failures;
					return false;
				}
				kernelUsed = true;
				result.planes[plane].texture = textures[slot][plane];
				continue;
			}
			if (!oneAxis && separable && plane > 0)   // chroma only: a 1D Lanczos rings into black bars (luma stays polar)
			{
				// Two 1D Lanczos passes: across into a temporary, then down.
				pl_tex_params tp = params;
				tp.w = w;
				tp.h = in->params.h;
				if (!pl_tex_recreate(gpu, &separableTemp[slot][plane], &tp))
				{
					error = "texture allocation failed";
					++failures;
					return false;
				}
				auto ortho = [&](pl_tex from, pl_tex to, int nw, int nh, pl_shader_obj* lut) {
					pl_shader s1 = pl_dispatch_begin(dispatch);
					pl_sample_src s{};
					s.tex = from;
					s.rect = { 0.0f, 0.0f, static_cast<float>(from->params.w), static_cast<float>(from->params.h) };
					s.new_w = nw;
					s.new_h = nh;
					s.components = from->params.format->num_components;
					pl_sample_filter_params f{};
					f.filter = pl_filter_lanczos;
					f.antiring = antiring;
					f.lut = lut;
					pl_dispatch_params d{};
					d.shader = &s1;
					d.target = to;
					if (!pl_shader_sample_ortho2(s1, &s, &f) || !pl_dispatch_finish(dispatch, &d))
					{
						if (s1) pl_dispatch_abort(dispatch, &s1);
						return false;
					}
					return true;
				};
				if (!ortho(in, separableTemp[slot][plane], w, in->params.h, &luts[slot][plane]) ||
					!ortho(separableTemp[slot][plane], textures[slot][plane], w, h, &lutsDown[slot][plane]))
				{
					error = "downscale pass failed";
					++failures;
					return false;
				}
				result.planes[plane].texture = textures[slot][plane];
				continue;
			}
			pl_shader sh = pl_dispatch_begin(dispatch);
			pl_sample_src src{};
			src.tex = in;
			src.rect = { 0.0f, 0.0f, static_cast<float>(in->params.w), static_cast<float>(in->params.h) };
			src.new_w = w;
			src.new_h = h;
			src.components = in->params.format->num_components;
			pl_sample_filter_params filter{};
			filter.filter = oneAxis ? pl_filter_lanczos : pl_filter_ewa_lanczossharp;
			filter.antiring = antiring;
			filter.lut = &luts[slot][plane];
			{   // DEBUG (VP_DEBUG_REBASE_NOCOMPUTE=1): the polar filter as a fragment pass
				char flag[4] = {};
				filter.no_compute = GetEnvironmentVariableA("VP_DEBUG_REBASE_NOCOMPUTE", flag, sizeof(flag)) > 0 && flag[0] == '1';
			}
			pl_dispatch_params dp{};
			dp.shader = &sh;
			dp.target = textures[slot][plane];
			const bool sampled = oneAxis ? pl_shader_sample_ortho2(sh, &src, &filter) : pl_shader_sample_polar(sh, &src, &filter);
			if (plane == 0) lastLumaKind = sampled ? (pl_shader_is_compute(sh) ? 1 : 2) : 0;
			if (!sampled || !pl_dispatch_finish(dispatch, &dp))
			{
				if (sh) pl_dispatch_abort(dispatch, &sh);
				error = "downscale pass failed";
				++failures;
				return false;
			}
			result.planes[plane].texture = textures[slot][plane];
			{   // DEBUG (VP_DEBUG_SYNC & 64): GPU finish after the polar pass
				char syncText[8] = {};
				if (GetEnvironmentVariableA("VP_DEBUG_SYNC", syncText, sizeof(syncText)) > 0 && (atoi(syncText) & 64)) pl_gpu_finish(gpu);
			}
		}
		// Chroma siting (research/chroma 2026-10-24): the planes were resampled edge-aligned by the
		// factors, so their shifts scale with them; the old -0.5 moved colour by 1/4 of a re-based
		// luma pixel at 2:1.
		for (int plane = 0; plane < result.num_planes; ++plane)
		{
			result.planes[plane].shift_x = ChromaSiting::ShiftAfterResample(source.planes[plane].shift_x, factorX);
			result.planes[plane].shift_y = ChromaSiting::ShiftAfterResample(source.planes[plane].shift_y, factorY);
		}
		// The crop in the luma plane's (now re-based) pixels.
		result.crop.x0 = static_cast<float>(source.crop.x0 * factorX);
		result.crop.y0 = static_cast<float>(source.crop.y0 * factorY);
		result.crop.x1 = static_cast<float>(source.crop.x1 * factorX);
		result.crop.y1 = static_cast<float>(source.crop.y1 * factorY);
		out = result;
		return true;
	}
}
