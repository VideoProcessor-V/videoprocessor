// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>

#include <libplacebo/shaders/custom.h>

#include <algorithm>

namespace VpLaneProducts
{
	bool UploadTapRows(pl_gpu gpu, pl_tex tap, const VpSkeleton::SourceFrameView& view, uint32_t y0, uint32_t y1,
		std::vector<uint16_t>& scratch)
	{
		if (!gpu || !tap || !view.Valid() || y1 <= y0 || y1 > view.height || int(view.width) != tap->params.w ||
			int(view.height) != tap->params.h)
			return false;
		const size_t rows = size_t(y1 - y0);
		const size_t w = view.width;
		scratch.resize(rows * w);
		const unsigned shift = view.msbShift;
		for (size_t r = 0; r < rows; ++r)
		{
			const uint16_t* src = view.luma + (size_t(y0) + r) * view.strideSamples;
			uint16_t* dst = scratch.data() + r * w;
			for (size_t x = 0; x < w; ++x)
				dst[x] = uint16_t(uint32_t(src[x]) << shift);
		}
		pl_tex_transfer_params t{};
		t.tex = tap;
		t.rc.x0 = 0;
		t.rc.x1 = int(w);
		t.rc.y0 = int(y0);
		t.rc.y1 = int(y1);
		t.rc.z0 = 0;
		t.rc.z1 = 1;
		t.row_pitch = w * sizeof(uint16_t);
		t.ptr = scratch.data();
		return pl_tex_upload(gpu, &t);
	}

	const char* ExtendBodyGlsl()
	{
		return
			"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"ivec2 sz = textureSize(ex_tap, 0);\n"
			"if (p.x >= sz.x || p.y >= sz.y) return;\n"
			"ivec2 q = ivec2(ex_reflect(p.x, ex_l, ex_r), ex_reflect(p.y, ex_t, ex_b));\n"
			"imageStore(ex_out, p, vec4(texelFetch(ex_tap, q, 0).x, 0.0, 0.0, 1.0));\n";
	}

	namespace
	{
		const char* ExtendHeaderGlsl()
		{
			// RegionEvidence::ReflectInto, integer for integer.
			return
				"int ex_reflect(int i, int lo, int hi) {\n"
				"  if (i < lo) i = lo + (lo - 1 - i);\n"
				"  else if (i >= hi) i = hi - 1 - (i - hi);\n"
				"  return i < lo ? lo : (i > hi - 1 ? hi - 1 : i);\n"
				"}\n";
		}
	}

	bool ExtendTap(pl_dispatch dispatch, pl_tex tap, pl_tex extended, const RegionEvidence::Rect& picture, pl_timer timer)
	{
		if (!dispatch || !tap || !extended || tap->params.w != extended->params.w || tap->params.h != extended->params.h)
			return false;
		const RegionEvidence::Rect r = RegionEvidence::PictureOrFrame(picture, tap->params.w, tap->params.h);
		const int l = r.left, t = r.top, rr = r.right, b = r.bottom;
		pl_shader_desc desc[2]{};
		desc[0].desc.name = "ex_tap";
		desc[0].desc.type = PL_DESC_SAMPLED_TEX;
		desc[0].binding.object = tap;
		desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		desc[1].desc.name = "ex_out";
		desc[1].desc.type = PL_DESC_STORAGE_IMG;
		desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
		desc[1].binding.object = extended;
		const pl_shader_var vars[4] = {
			{ pl_var_int("ex_l"), &l, true }, { pl_var_int("ex_t"), &t, true },
			{ pl_var_int("ex_r"), &rr, true }, { pl_var_int("ex_b"), &b, true },
		};
		pl_custom_shader custom{};
		custom.description = "VP-V lane tap extend";
		custom.header = ExtendHeaderGlsl();
		custom.body = ExtendBodyGlsl();
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = 2;
		custom.variables = vars;
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
		compute.dispatch_size[0] = (tap->params.w + 15) / 16;
		compute.dispatch_size[1] = (tap->params.h + 15) / 16;
		compute.dispatch_size[2] = 1;
		compute.timer = timer;
		return pl_dispatch_compute(dispatch, &compute);
	}
}
