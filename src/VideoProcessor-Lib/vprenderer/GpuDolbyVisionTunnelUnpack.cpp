#include <pch.h>
#include "GpuDolbyVisionTunnelUnpack.h"
#include "DolbyVisionTunnelUnpackReference.h"

#include <libplacebo/shaders/custom.h>

#include <cstdlib>

namespace
{
	// The raw frame is uploaded as rgba8: one texel per pixel (its 32-bit
	// word's bytes in x, y, z, w; big endian for r210 / R10b, little for
	// R10l). unorm8 is exact for 0..255, so the bytes come back exactly.
	const char* const DV_HEADER = R"GLSL(
uint dv_word(int i, int y)
{
    uvec4 b = uvec4(round(texelFetch(raw, ivec2(i, y), 0) * 255.0));
    return big_endian != 0 ? (b.x << 24) | (b.y << 16) | (b.z << 8) | b.w :
        (b.w << 24) | (b.z << 16) | (b.y << 8) | b.x;
}
// HDMI 1.4a 6.5.4: G8 = I[11:4], R8 = C[11:4], B8 = C[3:0] I[3:0]; s = 2 when
// RGB8 sits in the high eight bits of the 10-bit channels, else 0, plus 2 for
// R10b / R10l (their word is the r210 word << 2). The 12-bit codes land in the
// high bits of the 16-bit sample (code << 4).
uint dv_intensity(uint p, uint s) { return ((p >> (2u + s)) & 0xFF00u) | ((p << (4u - s)) & 0xF0u); }
uint dv_chroma(uint p, uint s) { return ((p >> (12u + s)) & 0xFF00u) | ((p >> s) & 0xF0u); }
float dv_u16(uint v) { return float(v) / 65535.0; }
)GLSL";

	// One invocation per pixel pair: both intensity samples and the pair's
	// (C0, C1) chroma texel, as CR210DolbyVisionTunnelFormatter's planes.
	// Keep in lockstep with DolbyVisionTunnelUnpackReference.h.
	const char* const DV_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < pairs && g.y < frame_height) {
    uint s = uint(shift);
    uint p0 = dv_word(2 * g.x, g.y);
    uint p1 = dv_word(2 * g.x + 1, g.y);
    imageStore(dst_y, ivec2(2 * g.x, g.y), vec4(dv_u16(dv_intensity(p0, s)), 0.0, 0.0, 1.0));
    imageStore(dst_y, ivec2(2 * g.x + 1, g.y), vec4(dv_u16(dv_intensity(p1, s)), 0.0, 0.0, 1.0));
    imageStore(dst_uv, g, vec4(dv_u16(dv_chroma(p0, s)), dv_u16(dv_chroma(p1, s)), 0.0, 1.0));
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

GpuDolbyVisionTunnelUnpack::GpuDolbyVisionTunnelUnpack(pl_log log, pl_gpu gpuIn)
	: gpu(gpuIn)
{
	dispatch = gpu ? pl_dispatch_create(log, gpu) : nullptr;
	// Tests: VP_TEST_DV_UNPACK_FAIL=1, read once per unpack object (one per
	// renderer device), not per frame.
	char* v = nullptr;
	size_t n = 0;
	forceFail = _dupenv_s(&v, &n, "VP_TEST_DV_UNPACK_FAIL") == 0 && v && v[0] == '1';
	std::free(v);
}

GpuDolbyVisionTunnelUnpack::~GpuDolbyVisionTunnelUnpack()
{
	if (gpu)
		pl_tex_destroy(gpu, &raw);
	pl_dispatch_destroy(&dispatch);
}

bool GpuDolbyVisionTunnelUnpack::Supports(VideoFrameEncoding encoding)
{
	return DolbyVisionTunnelUnpackReference::SupportsContainer(encoding);
}

bool GpuDolbyVisionTunnelUnpack::Unpack(VideoFrameEncoding encoding, const void* data, size_t rowBytes, int width, int height, bool highAligned,
	pl_tex (&planes)[2], pl_tex uploaded)
{
	error.clear();
	// Tests: VP_TEST_DV_UNPACK_FAIL=1 forces the CPU fallback (renderer harness).
	if (forceFail) { error = "forced by VP_TEST_DV_UNPACK_FAIL"; return false; }
	if (!gpu || !dispatch) { error = "no GPU"; return false; }
	if (!Supports(encoding)) { error = "unsupported tunnel container"; return false; }
	if ((!data && !uploaded) || width <= 0 || height <= 0 || (width & 1) || rowBytes % 4 != 0)
	{
		error = "bad frame geometry";
		return false;
	}
	if (rowBytes < static_cast<size_t>(width) * 4) { error = "row shorter than the frame"; return false; }
	pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
	const enum pl_fmt_caps planeCaps = static_cast<enum pl_fmt_caps>(
		PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
	pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, planeCaps);
	pl_fmt rg16 = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, planeCaps);
	if (!rgba8 || !r16 || !rg16) { error = "no rgba8 / storable r16 / rg16 format"; return false; }
	if (!uploaded)
	{
		pl_tex_params rawParams{};
		rawParams.w = static_cast<int>(rowBytes / 4);
		rawParams.h = height;
		rawParams.format = rgba8;
		rawParams.sampleable = true;
		rawParams.host_writable = true;
		if (!pl_tex_recreate(gpu, &raw, &rawParams)) { error = "cannot create the raw texture"; return false; }
		pl_tex_transfer_params upload{};
		upload.tex = raw;
		upload.ptr = const_cast<void*>(data);
		upload.row_pitch = rowBytes;
		if (!pl_tex_upload(gpu, &upload)) { error = "raw upload failed"; return false; }
	}
	else if (uploaded->params.w != static_cast<int>(rowBytes / 4) || uploaded->params.h != height)
	{
		error = "the uploaded frame's size differs";
		return false;
	}
	const pl_tex source = uploaded ? uploaded : raw;
	auto plane = [this](pl_tex* tex, int w, int h, pl_fmt fmt) -> bool
	{
		pl_tex_params params{};
		params.w = w;
		params.h = h;
		params.format = fmt;
		params.sampleable = true;
		params.storable = true;
		params.host_readable = hostReadable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
		return pl_tex_recreate(gpu, tex, &params);
	};
	const int pairs = width / 2;
	if (!plane(&planes[0], width, height, r16) || !plane(&planes[1], pairs, height, rg16))
	{
		error = "cannot create the planes";
		return false;
	}
	const int shift = static_cast<int>(DolbyVisionTunnelUnpackReference::ContainerShift(encoding, highAligned));
	const int bigEndian = DolbyVisionTunnelUnpackReference::ContainerBigEndian(encoding) ? 1 : 0;
	const pl_shader_desc descriptors[3] = { Sampled("raw", source), Storage("dst_y", planes[0]), Storage("dst_uv", planes[1]) };
	const pl_shader_var variables[4] = {
		{ pl_var_int("pairs"), &pairs, false },
		{ pl_var_int("frame_height"), &height, false },
		{ pl_var_int("shift"), &shift, false },
		{ pl_var_int("big_endian"), &bigEndian, false } };
	pl_custom_shader custom{};
	custom.description = "VideoProcessor capture: Dolby Vision tunnel to 12-bit 4:2:2 (GPU)";
	custom.header = DV_HEADER;
	custom.body = DV_BODY;
	custom.input = PL_SHADER_SIG_NONE;
	custom.output = PL_SHADER_SIG_NONE;
	custom.descriptors = descriptors;
	custom.num_descriptors = 3;
	custom.variables = variables;
	custom.num_variables = 4;
	custom.compute = true;
	custom.compute_group_size[0] = 16;
	custom.compute_group_size[1] = 16;
	pl_shader sh = pl_dispatch_begin(dispatch);
	if (!pl_shader_custom(sh, &custom))
	{
		pl_dispatch_abort(dispatch, &sh);
		error = "DV tunnel unpack shader rejected";
		return false;
	}
	pl_dispatch_compute_params compute{};
	compute.shader = &sh;
	compute.dispatch_size[0] = (pairs + 15) / 16;
	compute.dispatch_size[1] = (height + 15) / 16;
	compute.dispatch_size[2] = 1;
	compute.timer = timer;
	if (!pl_dispatch_compute(dispatch, &compute))
	{
		error = "DV tunnel unpack dispatch failed";
		return false;
	}
	return true;
}
