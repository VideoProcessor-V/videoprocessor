// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/signal/DvTunnelGlsl.h>

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
		for (size_t r = 0; r < rows; ++r)
			view.Row16(uint32_t(size_t(y0) + r), 0, uint32_t(w), scratch.data() + r * w);
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
		// Max DV tunnel shift in DvTunnelGlsl::kHeader (`dv_intensity` shifts by `4u - s`; ContainerShift max = 2u + 2u = 4u).
		constexpr uint32_t kMaxDvBitShift = 4u;
		// Quantization bit width of R16 unorm (PL_FMT_UNORM, 1 component, 16-bit); valid MsbAlign shifts are 0..15.
		constexpr uint32_t kR16BitDepth = 16u;
		// 2D compute workgroup tile edge shared with ExtendTap and SourceUnpackGpu (16x16 = 256 invocations).
		constexpr int kTapGroupSize = 16;

		bool Is2dTexture(pl_tex tex)
		{
			return tex != nullptr && tex->params.w > 0 && tex->params.h > 0 && tex->params.d == 0;
		}

		bool IsRgba8Unorm(pl_tex tex)
		{
			const pl_fmt fmt = tex ? tex->params.format : nullptr;
			// 4-byte 32-bit DV tunnel container word packed as 4 x 8-bit unorm channels (DvTunnelGlsl.h).
			return fmt != nullptr && fmt->type == PL_FMT_UNORM && fmt->num_components == 4 &&
				fmt->component_depth[0] == 8 && fmt->component_depth[1] == 8 &&
				fmt->component_depth[2] == 8 && fmt->component_depth[3] == 8;
		}

		bool IsR16Unorm(pl_tex tex)
		{
			const pl_fmt fmt = tex ? tex->params.format : nullptr;
			return fmt != nullptr && fmt->type == PL_FMT_UNORM && fmt->num_components == 1 &&
				fmt->component_depth[0] == int(kR16BitDepth);
		}

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

	const char* DvTapFromP13RawBodyGlsl()
	{
		return
			"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"ivec2 sz = textureSize(raw, 0);\n"
			"if (p.x >= sz.x || p.y >= sz.y) return;\n"
			"uint i16 = dv_intensity(dv_word(p.x, p.y), uint(dv_shift));\n"
			"imageStore(tap_out, p, vec4(dv_u16(i16), 0.0, 0.0, 1.0));\n";
	}

	const char* TapFromP13LumaBodyGlsl()
	{
		return
			"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"ivec2 sz = textureSize(p13_luma, 0);\n"
			"if (p.x >= sz.x || p.y >= sz.y) return;\n"
			"float s = texelFetch(p13_luma, p, 0).x;\n"
			"if (p13_msb_shift != 0) {\n"
			"  uint raw16 = uint(round(s * 65535.0));\n"
			"  uint aligned = (raw16 << uint(p13_msb_shift)) & 0xFFFFu;\n"
			"  s = float(aligned) / 65535.0;\n"
			"}\n"
			"imageStore(tap_out, p, vec4(s, 0.0, 0.0, 1.0));\n";
	}

	bool BuildDvTapFromP13Raw(pl_dispatch dispatch, pl_tex p13RawRgba8, pl_tex tapR16, bool bigEndian, uint32_t bitShift,
		pl_timer timer)
	{
		if (!dispatch || !Is2dTexture(p13RawRgba8) || !Is2dTexture(tapR16) || p13RawRgba8 == tapR16 ||
			p13RawRgba8->params.w != tapR16->params.w || p13RawRgba8->params.h != tapR16->params.h ||
			!p13RawRgba8->params.sampleable || !tapR16->params.storable ||
			!IsRgba8Unorm(p13RawRgba8) || !IsR16Unorm(tapR16) || bitShift > kMaxDvBitShift)
			return false;
		const int bigEndianInt = bigEndian ? 1 : 0;
		const int dvShiftInt = int(bitShift);
		pl_shader_desc desc[2]{};
		desc[0].desc.name = "raw";
		desc[0].desc.type = PL_DESC_SAMPLED_TEX;
		desc[0].binding.object = p13RawRgba8;
		desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		desc[1].desc.name = "tap_out";
		desc[1].desc.type = PL_DESC_STORAGE_IMG;
		desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
		desc[1].binding.object = tapR16;
		const pl_shader_var vars[2] = {
			{ pl_var_int("big_endian"), &bigEndianInt, true },
			{ pl_var_int("dv_shift"), &dvShiftInt, true },
		};
		pl_custom_shader custom{};
		custom.description = "VP-V lane DV tap from P13 raw";
		custom.header = DvTunnelGlsl::kHeader;
		custom.body = DvTapFromP13RawBodyGlsl();
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = 2;
		custom.variables = vars;
		custom.num_variables = 2;
		custom.compute = true;
		custom.compute_group_size[0] = kTapGroupSize;
		custom.compute_group_size[1] = kTapGroupSize;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!sh)
			return false;
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (tapR16->params.w + kTapGroupSize - 1) / kTapGroupSize;
		compute.dispatch_size[1] = (tapR16->params.h + kTapGroupSize - 1) / kTapGroupSize;
		compute.dispatch_size[2] = 1;
		compute.timer = timer;
		return pl_dispatch_compute(dispatch, &compute);
	}

	bool BuildTapFromP13Luma(pl_dispatch dispatch, pl_tex p13LumaR16, pl_tex tapR16, uint32_t msbShift, pl_timer timer)
	{
		if (!dispatch || !Is2dTexture(p13LumaR16) || !Is2dTexture(tapR16) || p13LumaR16 == tapR16 ||
			p13LumaR16->params.w != tapR16->params.w || p13LumaR16->params.h != tapR16->params.h ||
			!p13LumaR16->params.sampleable || !tapR16->params.storable ||
			!IsR16Unorm(p13LumaR16) || !IsR16Unorm(tapR16) || msbShift >= kR16BitDepth)
			return false;
		const int msbShiftInt = int(msbShift);
		pl_shader_desc desc[2]{};
		desc[0].desc.name = "p13_luma";
		desc[0].desc.type = PL_DESC_SAMPLED_TEX;
		desc[0].binding.object = p13LumaR16;
		desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		desc[1].desc.name = "tap_out";
		desc[1].desc.type = PL_DESC_STORAGE_IMG;
		desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
		desc[1].binding.object = tapR16;
		const pl_shader_var vars[1] = {
			{ pl_var_int("p13_msb_shift"), &msbShiftInt, true },
		};
		pl_custom_shader custom{};
		custom.description = "VP-V lane tap from P13 luma";
		custom.body = TapFromP13LumaBodyGlsl();
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = desc;
		custom.num_descriptors = 2;
		custom.variables = vars;
		custom.num_variables = 1;
		custom.compute = true;
		custom.compute_group_size[0] = kTapGroupSize;
		custom.compute_group_size[1] = kTapGroupSize;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!sh)
			return false;
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (tapR16->params.w + kTapGroupSize - 1) / kTapGroupSize;
		compute.dispatch_size[1] = (tapR16->params.h + kTapGroupSize - 1) / kTapGroupSize;
		compute.dispatch_size[2] = 1;
		compute.timer = timer;
		return pl_dispatch_compute(dispatch, &compute);
	}
}
