#include <pch.h>
#include "SourceRebase.h"

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

	Module::~Module()
	{
		// Textures belong to a device that may already be gone; the renderer
		// calls Release while it exists.
	}

	void Module::Release(pl_gpu gpu)
	{
		for (int slot = 0; slot < kSlots; ++slot)
			for (int plane = 0; plane < PL_MAX_PLANES; ++plane)
			{
				if (textures[slot][plane] && gpu) pl_tex_destroy(gpu, &textures[slot][plane]);
				textures[slot][plane] = nullptr;
				pl_shader_obj_destroy(&luts[slot][plane]);
			}
	}

	bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, double factorX, double factorY, pl_frame& out, int slot)
	{
		error.clear();
		factorX = factorX > 0.0 && factorX < 1.0 ? factorX : 1.0;
		factorY = factorY > 0.0 && factorY < 1.0 ? factorY : 1.0;
		if (!gpu || !dispatch || source.num_planes <= 0 || (factorX >= 1.0 && factorY >= 1.0))
		{
			error = "nothing to re-base";
			return false;
		}
		const bool oneAxis = factorX >= 1.0 || factorY >= 1.0;
		slot = (std::max)(0, (std::min)(slot, kSlots - 1));
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
			params.sampleable = true;
			params.renderable = true;
			params.blit_src = true;   // the smooth-motion history copies source planes
			if (!(params.format->caps & PL_FMT_CAP_RENDERABLE))
			{
				// Fall back to a float format with the same component count.
				params.format = pl_find_fmt(gpu, PL_FMT_FLOAT, params.format->num_components, 16, 16,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR));
				if (!params.format)
				{
					error = "no renderable format for plane " + std::to_string(plane);
					++failures;
					return false;
				}
			}
			if (!pl_tex_recreate(gpu, &textures[slot][plane], &params))
			{
				error = "texture allocation failed";
				++failures;
				return false;
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
			pl_dispatch_params dp{};
			dp.shader = &sh;
			dp.target = textures[slot][plane];
			const bool sampled = oneAxis ? pl_shader_sample_ortho2(sh, &src, &filter) : pl_shader_sample_polar(sh, &src, &filter);
			if (!sampled || !pl_dispatch_finish(dispatch, &dp))
			{
				if (sh) pl_dispatch_abort(dispatch, &sh);
				error = "downscale pass failed";
				++failures;
				return false;
			}
			result.planes[plane].texture = textures[slot][plane];
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
