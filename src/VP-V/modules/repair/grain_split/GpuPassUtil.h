// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Small libplacebo compute helpers shared by the grain-split readers (repair.grain_split, repair.temporal_detail,
// scaling.chroma_upscale). Same conventions as texture_restore (TextureRestore.cpp): every pass is a
// pl_custom_shader compute dispatch with named descriptors, nearest sampling, write-only storage images, and
// shader constants emitted as #defines (geometry and fixed law) or dynamic variables (per-frame values).
#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace VpvGpu
{
	inline std::string Num(double v)
	{
		char s[48];
		std::snprintf(s, sizeof(s), "%.9g", v);
		std::string r = s;
		if (r.find_first_of(".eEn") == std::string::npos) r += ".0";
		return r;
	}

	inline std::string Define(const char* name, double v) { return std::string("#define ") + name + " " + Num(v) + "\n"; }
	inline std::string DefineInt(const char* name, long long v) { return std::string("#define ") + name + " " + std::to_string(v) + "\n"; }

	// const float NAME[n] = float[n](...);
	template <typename F>
	std::string ConstArray(const char* name, int n, F value)
	{
		std::string s = "const float " + std::string(name) + "[" + std::to_string(n) + "] = float[" + std::to_string(n) + "](";
		for (int i = 0; i < n; ++i)
		{
			s += Num(value(i));
			if (i + 1 < n) s += ", ";
		}
		return s + ");\n";
	}

	inline pl_fmt FloatFormat(pl_gpu gpu, int components, int bits = 32)
	{
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
		for (int n = (std::max)(1, components); n <= 4; ++n)
			if (pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, n, bits, bits, caps))
				return f;
		return nullptr;
	}

	inline pl_fmt Unorm16(pl_gpu gpu, int components)
	{
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
		return pl_find_fmt(gpu, PL_FMT_UNORM, components, 16, 16, caps);
	}

	// A storable texture like `like` (at least 16 bits: a restored value is finer than an 8-bit code).
	inline pl_fmt StorableLike(pl_gpu gpu, pl_fmt like, int components = 0)
	{
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
		const int n = components > 0 ? components : (like ? like->num_components : 1);
		if (like && like->num_components == n && (like->caps & caps) == caps && like->component_depth[0] >= 16) return like;
		pl_fmt f = pl_find_fmt(gpu, PL_FMT_UNORM, n, 16, 16, caps);
		return f ? f : pl_find_fmt(gpu, PL_FMT_FLOAT, n, 16, 16, caps);
	}

	// A sampleable + storable 32-bit unsigned-integer format (exact counters / ids); null if the GPU has none.
	inline pl_fmt UintFormat(pl_gpu gpu, int components)
	{
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE);
		return pl_find_fmt(gpu, PL_FMT_UINT, components, 32, 32, caps);
	}

	inline pl_shader_desc Sampled(const char* name, pl_tex tex)
	{
		pl_shader_desc d{};
		d.desc.name = name;
		d.desc.type = PL_DESC_SAMPLED_TEX;
		d.binding.object = tex;
		d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		return d;
	}

	inline pl_shader_desc Storage(const char* name, pl_tex tex)
	{
		pl_shader_desc d{};
		d.desc.name = name;
		d.desc.type = PL_DESC_STORAGE_IMG;
		d.desc.access = PL_DESC_ACCESS_WRITEONLY;
		d.binding.object = tex;
		return d;
	}

	inline pl_shader_var Var(const char* name, const float* value) { return { pl_var_float(name), value, true }; }

	// One compute dispatch. group: the workgroup edge (gx x gy threads), groups: the grid.
	inline bool Pass(pl_dispatch dispatch, const char* description, const std::string& header, const char* body,
		const pl_shader_desc* desc, int numDesc, const pl_shader_var* vars, int numVars, int groupsX, int groupsY,
		size_t shmem, int groupW = 16, int groupH = 16)
	{
		pl_custom_shader custom{};
		custom.description = description;
		custom.header = header.c_str();
		custom.body = body;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = numDesc;
		custom.variables = vars;
		custom.num_variables = numVars;
		custom.compute = true;
		custom.compute_shmem = shmem;
		custom.compute_group_size[0] = groupW;
		custom.compute_group_size[1] = groupH;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (std::max)(1, groupsX);
		compute.dispatch_size[1] = (std::max)(1, groupsY);
		compute.dispatch_size[2] = 1;
		return pl_dispatch_compute(dispatch, &compute);
	}

	inline bool Ensure(pl_gpu gpu, pl_tex& tex, int w, int h, pl_fmt fmt, bool blitSrc = false)
	{
		pl_tex_params tp{};
		tp.w = w;
		tp.h = h;
		tp.format = fmt;
		tp.sampleable = true;
		tp.storable = true;
		tp.blit_src = blitSrc;
		return fmt && pl_tex_recreate(gpu, &tex, &tp);
	}

	// One code of the signal in the plane texture's normalised units (as texture_restore / VpDebander).
	inline double CodeUnit(pl_tex tex, const pl_bit_encoding& bits, int colorDepth)
	{
		if (!tex) return 1.0;
		const int sampleDepth = bits.sample_depth > 0 ? bits.sample_depth : tex->params.format->component_depth[0];
		if (tex->params.format->type == PL_FMT_FLOAT)
		{
			double m = 1.0;
			for (int i = 0; i < colorDepth; ++i) m *= 2.0;
			return 1.0 / (m - 1.0);
		}
		double num = 1.0, den = 1.0;
		for (int i = 0; i < bits.bit_shift; ++i) num *= 2.0;
		for (int i = 0; i < sampleDepth; ++i) den *= 2.0;
		return num / (den - 1.0);
	}
}
