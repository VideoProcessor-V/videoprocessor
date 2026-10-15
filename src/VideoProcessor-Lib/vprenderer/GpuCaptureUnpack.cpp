#include <pch.h>
#include "GpuCaptureUnpack.h"

#include <libplacebo/shaders/custom.h>

#include <cstdlib>

namespace
{
	// The raw frame is uploaded as rgba8: one texel per 4 source bytes
	// (V210: one 32-bit little-endian word; UYVY: U, Y0, V, Y1). unorm8 is
	// exact for 0..255, so the bytes come back exactly.
	const char* const UNPACK_HEADER = R"GLSL(
uvec4 cu_bytes(int i, int y) { return uvec4(round(texelFetch(raw, ivec2(i, y), 0) * 255.0)); }
uint cu_word(int i, int y) { uvec4 b = cu_bytes(i, y); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }
// A 16-bit sample (the value already in the high bits) for a unorm16 store.
float cu_u16(uint v) { return float(v) / 65535.0; }
)GLSL";

	// One invocation per pixel pair: its two luma samples and its CbCr.
	// V210: 6 pixels per 4 words. Pair k (0..2) of a pack takes
	//   k 0: Cb w0[0], Y w0[1], Cr w0[2], Y w1[0]
	//   k 1: Cb w1[1], Y w1[2], Cr w2[0], Y w2[1]
	//   k 2: Cb w2[2], Y w3[0], Cr w3[1], Y w3[2]
	// (10-bit fields at bits 0, 10, 20), each << 6.
	const char* const V210_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < pairs && g.y < frame_height) {
    int p = g.x;
    int pack = p / 3;
    int k = p - pack * 3;
    int w = pack * 4;
    uint cb, y0, cr, y1;
    if (k == 0) {
        uint a = cu_word(w, g.y), b = cu_word(w + 1, g.y);
        cb = a & 1023u; y0 = (a >> 10) & 1023u; cr = (a >> 20) & 1023u; y1 = b & 1023u;
    } else if (k == 1) {
        uint b = cu_word(w + 1, g.y), c = cu_word(w + 2, g.y);
        cb = (b >> 10) & 1023u; y0 = (b >> 20) & 1023u; cr = c & 1023u; y1 = (c >> 10) & 1023u;
    } else {
        uint c = cu_word(w + 2, g.y), d = cu_word(w + 3, g.y);
        cb = (c >> 20) & 1023u; y0 = d & 1023u; cr = (d >> 10) & 1023u; y1 = (d >> 20) & 1023u;
    }
    imageStore(dst_y, ivec2(2 * p, g.y), vec4(cu_u16(y0 << 6), 0.0, 0.0, 1.0));
    if (2 * p + 1 < frame_width)
        imageStore(dst_y, ivec2(2 * p + 1, g.y), vec4(cu_u16(y1 << 6), 0.0, 0.0, 1.0));
    imageStore(dst_uv, ivec2(p, g.y), vec4(cu_u16(cb << 6), cu_u16(cr << 6), 0.0, 1.0));
}
)GLSL";

	// UYVY / HDYC: U, Y0, V, Y1 per pair, each << 8.
	const char* const UYVY_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < pairs && g.y < frame_height) {
    uvec4 b = cu_bytes(g.x, g.y);
    imageStore(dst_y, ivec2(2 * g.x, g.y), vec4(cu_u16(b.y << 8), 0.0, 0.0, 1.0));
    if (2 * g.x + 1 < frame_width)
        imageStore(dst_y, ivec2(2 * g.x + 1, g.y), vec4(cu_u16(b.w << 8), 0.0, 0.0, 1.0));
    imageStore(dst_uv, ivec2(g.x, g.y), vec4(cu_u16(b.x << 8), cu_u16(b.z << 8), 0.0, 1.0));
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

GpuCaptureUnpack::GpuCaptureUnpack(pl_log log, pl_gpu gpuIn)
	: gpu(gpuIn)
{
	dispatch = gpu ? pl_dispatch_create(log, gpu) : nullptr;
}

GpuCaptureUnpack::~GpuCaptureUnpack()
{
	if (gpu)
		pl_tex_destroy(gpu, &raw);
	pl_dispatch_destroy(&dispatch);
}

bool GpuCaptureUnpack::Supports(VideoFrameEncoding encoding)
{
	return encoding == VideoFrameEncoding::V210 || encoding == VideoFrameEncoding::UYVY ||
		encoding == VideoFrameEncoding::HDYC;
}

VideoFrameFormatterOutputContract GpuCaptureUnpack::Contract(VideoFrameEncoding encoding)
{
	// As CV210toP210 ({limited, 10, 6}) and CUYVYtoP210 ({limited, 8, 8}).
	VideoFrameFormatterOutputContract contract;
	contract.sampleRange = VideoFrameSampleRange::LIMITED;
	contract.colorDepth = encoding == VideoFrameEncoding::V210 ? 10 : 8;
	contract.bitShift = encoding == VideoFrameEncoding::V210 ? 6 : 8;
	return contract;
}

bool GpuCaptureUnpack::Unpack(VideoFrameEncoding encoding, const void* data, size_t rowBytes, int width, int height,
	pl_tex (&planes)[2], pl_tex uploaded)
{
	error.clear();
	// Tests: VP_TEST_CAPTURE_UNPACK_FAIL=1 forces the fallback (renderer harness).
	const bool forceFail = [] { char* v = nullptr; size_t n = 0; const bool on = _dupenv_s(&v, &n,
		"VP_TEST_CAPTURE_UNPACK_FAIL") == 0 && v && v[0] == '1'; std::free(v); return on; }();
	if (forceFail) { error = "forced by VP_TEST_CAPTURE_UNPACK_FAIL"; return false; }
	if (!gpu || !dispatch) { error = "no GPU"; return false; }
	if (!Supports(encoding)) { error = "unsupported encoding"; return false; }
	if ((!data && !uploaded) || width <= 0 || height <= 0 || (width & 1) || rowBytes % 4 != 0)
	{
		error = "bad frame geometry";
		return false;
	}
	const size_t needed = encoding == VideoFrameEncoding::V210 ?
		static_cast<size_t>((width + 5) / 6) * 16 : static_cast<size_t>(width) * 2;
	if (rowBytes < needed) { error = "row shorter than the frame"; return false; }
	// The raw bytes, as they are.
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
	const pl_shader_desc descriptors[3] = { Sampled("raw", source), Storage("dst_y", planes[0]), Storage("dst_uv", planes[1]) };
	const pl_shader_var variables[3] = {
		{ pl_var_int("pairs"), &pairs, false },
		{ pl_var_int("frame_width"), &width, false },
		{ pl_var_int("frame_height"), &height, false } };
	pl_custom_shader custom{};
	custom.description = encoding == VideoFrameEncoding::V210 ? "VideoProcessor capture: V210 to P210 (GPU)" :
		"VideoProcessor capture: UYVY to P210 (GPU)";
	custom.header = UNPACK_HEADER;
	custom.body = encoding == VideoFrameEncoding::V210 ? V210_BODY : UYVY_BODY;
	custom.input = PL_SHADER_SIG_NONE;
	custom.output = PL_SHADER_SIG_NONE;
	custom.descriptors = descriptors;
	custom.num_descriptors = 3;
	custom.variables = variables;
	custom.num_variables = 3;
	custom.compute = true;
	custom.compute_group_size[0] = 16;
	custom.compute_group_size[1] = 16;
	pl_shader sh = pl_dispatch_begin(dispatch);
	if (!pl_shader_custom(sh, &custom))
	{
		pl_dispatch_abort(dispatch, &sh);
		error = "unpack shader rejected";
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
		error = "unpack dispatch failed";
		return false;
	}
	return true;
}
