// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include <VP-V/modules/input/capture/SourceUnpackGpu.h>
#include <VP-V/modules/input/signal/DolbyVisionTunnelUnpackReference.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/signal/DvTunnelGlsl.h>
#include <VP-V/modules/input/capture/CaptureUnpackGlsl.h>

#include <libplacebo/shaders/custom.h>

namespace
{
	// The raw frame is an rgba8 texture: one texel per four source bytes (a little-endian 32-bit word in x, y, z, w).
	// The helpers are the shared texts: DvTunnelGlsl::kHeader (dv_word / dv_intensity / dv_u16, one copy with the DV
	// kernel) and CaptureUnpackGlsl::kHeader (cu_bytes / cu_word, one copy with GpuCaptureUnpack).
	const std::string& UnpackHeader()
	{
		static const std::string header = std::string(DvTunnelGlsl::kHeader) + CaptureUnpackGlsl::kHeader;
		return header;
	}

	// P010 / P210 luma: word16 >> 6 (the ten-bit code), back to the high bits.
	const char* const PLANAR16_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uint w = cu_word(g.x >> 1, g.y);
    uint s16 = (g.x & 1) != 0 ? (w >> 16) : (w & 0xFFFFu);
    imageStore(dst, g, vec4(dv_u16((s16 >> 6) << 6), 0.0, 0.0, 1.0));
}
)GLSL";

	// v210: pixel x is group x / 6, slot k = x % 6; its Y field is word (2k + 1) / 3 at bit 10, 0 or 20 (k % 3).
	const char* const V210_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    int group = g.x / 6;
    int k = g.x - group * 6;
    int wi = (k * 2 + 1) / 3;
    int ph = k % 3;
    uint sh = ph == 0 ? 10u : (ph == 1 ? 0u : 20u);
    uint code = (cu_word(group * 4 + wi, g.y) >> sh) & 1023u;
    imageStore(dst, g, vec4(dv_u16(code << 6), 0.0, 0.0, 1.0));
}
)GLSL";

	// UYVY / HDYC: U Y0 V Y1 per 4 bytes, 8-bit luma << 8.
	const char* const UYVY_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uvec4 b = cu_bytes(g.x >> 1, g.y);
    uint y8 = (g.x & 1) != 0 ? b.w : b.y;
    imageStore(dst, g, vec4(dv_u16(y8 << 8), 0.0, 0.0, 1.0));
}
)GLSL";

	// DV tunnel, limited: tap12 = I (12 bit), out = tap12 << 4 (the intensity is already in the high bits of 16).
	const char* const DV_LIMITED_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uint i16 = dv_intensity(dv_word(g.x, g.y), uint(shift)) & 0xFFF0u;
    imageStore(dst, g, vec4(dv_u16(i16), 0.0, 0.0, 1.0));
}
)GLSL";

	// DV tunnel, full range: the 12-bit code goes through the CPU-built table (entry = FullLumaToLimited12 << 4).
	const char* const DV_FULL_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uint code12 = dv_intensity(dv_word(g.x, g.y), uint(shift)) >> 4;
    uint t16 = uint(round(texelFetch(range_tab, ivec2(int(code12), 0), 0).r * 65535.0));
    imageStore(dst, g, vec4(dv_u16(t16), 0.0, 0.0, 1.0));
}
)GLSL";

	pl_shader_desc Sampled(const char* name, pl_tex tex)
	{
		pl_shader_desc desc{};
		desc.desc.name = name;
		desc.desc.type = PL_DESC_SAMPLED_TEX;
		desc.binding.object = tex;
		desc.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		desc.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		return desc;
	}

	pl_shader_desc Storage(const char* name, pl_tex tex)
	{
		pl_shader_desc desc{};
		desc.desc.name = name;
		desc.desc.type = PL_DESC_STORAGE_IMG;
		desc.desc.access = PL_DESC_ACCESS_WRITEONLY;
		desc.binding.object = tex;
		return desc;
	}
}

namespace SourceUnpack
{
	const char* CauseName(Cause cause)
	{
		switch (cause)
		{
		case Cause::None: return "none";
		case Cause::NoGpu: return "no gpu";
		case Cause::BadGeometry: return "geometry";
		case Cause::DvContainer: return "dv container";
		case Cause::DvAlignment: return "dv alignment";
		case Cause::NoFormat: return "no format";
		case Cause::RawUploadFailed: return "raw upload";
		case Cause::UploadedSizeDiffers: return "uploaded size";
		case Cause::OutputCreateFailed: return "output plane";
		case Cause::RangeTableFailed: return "dv range table";
		case Cause::ShaderRejected: return "shader";
		case Cause::DispatchFailed: return "dispatch";
		default: return "unknown";
		}
	}

	size_t MinRowBytes(Packing packing, int width)
	{
		const size_t w = width > 0 ? static_cast<size_t>(width) : 0;
		switch (packing)
		{
		case Packing::V210: return ((w + 5u) / 6u) * 16u;
		case Packing::DvTunnel: return w * 4u;
		case Packing::Uyvy8: return ((w + 1u) / 2u) * 4u;
		default: return w * 2u;
		}
	}

	Cause Validate(const Request& request)
	{
		if (request.width <= 0 || request.height <= 0 || request.rowBytes % 4 != 0 ||
			request.rowBytes < MinRowBytes(request.packing, request.width))
			return Cause::BadGeometry;
		if (request.packing == Packing::DvTunnel)
		{
			if (!DolbyVisionTunnelUnpackReference::SupportsContainer(request.container))
				return Cause::DvContainer;
			if (!DvTunnelTapLaw::SupportsAlignment(request.rgbBitAlignment))
				return Cause::DvAlignment;
			if ((request.width & 1) != 0)
				return Cause::BadGeometry;
		}
		return Cause::None;
	}

	SourceUnpackGpu::SourceUnpackGpu(pl_log log, pl_gpu gpuIn)
		: gpu(gpuIn)
	{
		dispatch = gpu ? pl_dispatch_create(log, gpu) : nullptr;
	}

	SourceUnpackGpu::~SourceUnpackGpu()
	{
		if (gpu)
		{
			pl_tex_destroy(gpu, &raw);
			pl_tex_destroy(gpu, &rangeTable);
		}
		pl_dispatch_destroy(&dispatch);
	}

	bool SourceUnpackGpu::Fail(Cause c, const char* message)
	{
		cause = c;
		error = message;
		return false;
	}

	bool SourceUnpackGpu::EnsureRangeTable()
	{
		if (rangeTable)
			return true;
		const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE);
		pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, caps);
		if (!r16)
			return false;
		uint16_t table[DvTunnelTapLaw::kLevels];
		DvTunnelTapLaw::BuildRangeTable(table);
		for (int i = 0; i < DvTunnelTapLaw::kLevels; ++i)
			table[i] = static_cast<uint16_t>(table[i] << 4);   // the tap's 16-bit form
		pl_tex_params params{};
		params.w = DvTunnelTapLaw::kLevels;
		params.h = 1;
		params.format = r16;
		params.sampleable = true;
		params.host_writable = true;
		params.initial_data = table;
		rangeTable = pl_tex_create(gpu, &params);
		return rangeTable != nullptr;
	}

	bool SourceUnpackGpu::Unpack(const Request& request, const void* data, pl_tex uploaded, pl_tex* out)
	{
		cause = Cause::None;
		error.clear();
		if (!gpu || !dispatch || !out)
			return Fail(Cause::NoGpu, "no GPU");
		const Cause invalid = Validate(request);
		if (invalid != Cause::None)
			return Fail(invalid, CauseName(invalid));
		if (!data && !uploaded)
			return Fail(Cause::BadGeometry, "no raw bytes and no uploaded texture");
		pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
		const enum pl_fmt_caps planeCaps = static_cast<enum pl_fmt_caps>(
			PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
		pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, planeCaps);
		if (!rgba8 || !r16)
			return Fail(Cause::NoFormat, "no rgba8 / storable r16 format");
		const int texelsPerRow = static_cast<int>(request.rowBytes / 4);
		if (!uploaded)
		{
			pl_tex_params rawParams{};
			rawParams.w = texelsPerRow;
			rawParams.h = request.height;
			rawParams.format = rgba8;
			rawParams.sampleable = true;
			rawParams.host_writable = true;
			if (!pl_tex_recreate(gpu, &raw, &rawParams))
				return Fail(Cause::RawUploadFailed, "cannot create the raw texture");
			pl_tex_transfer_params upload{};
			upload.tex = raw;
			upload.ptr = const_cast<void*>(data);
			upload.row_pitch = request.rowBytes;
			if (!pl_tex_upload(gpu, &upload))
				return Fail(Cause::RawUploadFailed, "raw upload failed");
		}
		else if (uploaded->params.w != texelsPerRow || uploaded->params.h < request.height)
			return Fail(Cause::UploadedSizeDiffers, "the uploaded frame's size differs");
		const pl_tex source = uploaded ? uploaded : raw;
		const bool dvFull = request.packing == Packing::DvTunnel && request.fullRange;
		if (dvFull && !EnsureRangeTable())
			return Fail(Cause::RangeTableFailed, "cannot create the DV range table");
		pl_tex_params outParams{};
		outParams.w = request.width;
		outParams.h = request.height;
		outParams.format = r16;
		outParams.sampleable = true;
		outParams.storable = true;
		outParams.host_readable = hostReadable && (r16->caps & PL_FMT_CAP_HOST_READABLE);
		if (!pl_tex_recreate(gpu, out, &outParams))
			return Fail(Cause::OutputCreateFailed, "cannot create the P13 plane");

		const char* body = PLANAR16_BODY;
		switch (request.packing)
		{
		case Packing::V210: body = V210_BODY; break;
		case Packing::Uyvy8: body = UYVY_BODY; break;
		case Packing::DvTunnel: body = dvFull ? DV_FULL_BODY : DV_LIMITED_BODY; break;
		default: break;
		}
		const int shift = request.packing == Packing::DvTunnel ?
			static_cast<int>(DolbyVisionTunnelUnpackReference::ContainerShift(request.container, request.rgbBitAlignment == 1)) : 0;
		const int bigEndian = request.packing == Packing::DvTunnel &&
			DolbyVisionTunnelUnpackReference::ContainerBigEndian(request.container) ? 1 : 0;
		pl_shader_desc descriptors[3] = { Sampled("raw", source), Storage("dst", *out), {} };
		int descriptorCount = 2;
		if (dvFull)
			descriptors[descriptorCount++] = Sampled("range_tab", rangeTable);
		const pl_shader_var variables[4] = {
			{ pl_var_int("frame_width"), &request.width, false },
			{ pl_var_int("frame_height"), &request.height, false },
			{ pl_var_int("shift"), &shift, false },
			{ pl_var_int("big_endian"), &bigEndian, false } };
		pl_custom_shader custom{};
		custom.description = "VideoProcessor capture: P13 luma plane unpack (GPU)";
		custom.header = UnpackHeader().c_str();
		custom.body = body;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = descriptorCount;
		custom.variables = variables;
		custom.num_variables = 4;
		custom.compute = true;
		custom.compute_group_size[0] = 16;
		custom.compute_group_size[1] = 16;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			return Fail(Cause::ShaderRejected, "P13 unpack shader rejected");
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (request.width + 15) / 16;
		compute.dispatch_size[1] = (request.height + 15) / 16;
		compute.dispatch_size[2] = 1;
		compute.timer = timer;
		if (!pl_dispatch_compute(dispatch, &compute))
			return Fail(Cause::DispatchFailed, "P13 unpack dispatch failed");
		return true;
	}
}
