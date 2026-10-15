#include <pch.h>
#include "GpuRgbUnpack.h"

#include <libplacebo/shaders/custom.h>

namespace
{
	// The raw frame is uploaded as rgba8: one texel per 4 source bytes, unorm8
	// exact for 0..255, so the bytes come back exactly.
	const char* const RGB_UNPACK_HEADER = R"GLSL(
uint ru_byte(int o, int y)
{
    vec4 t = texelFetch(raw, ivec2(o >> 2, y), 0);
    int c = o & 3;
    float v = c == 0 ? t.x : c == 1 ? t.y : c == 2 ? t.z : t.w;
    return uint(round(v * 255.0));
}
float ru_u16(uint v) { return float(v) / 65535.0; }
// CDeckLinkRGBToP010's RoundQ20 (symmetric round half away from zero).
int ru_round_q20(int v) { return v >= 0 ? (v + 524288) >> 20 : -(((-v) + 524288) >> 20); }
int ru_clamp10(int v) { return clamp(v, 0, 1023); }
)GLSL";

	// R12B / R12L: one invocation per block of 8 pixels (4 pairs, 36 bytes =
	// 9 texels, read once; rows are whole blocks since widths are multiples of
	// 8). R12L's stream is the bytes in order; R12B reverses every 32-bit word,
	// so stream byte k of a block is its byte k ^ 3 (as CDeckLinkRGBToP010's
	// ReadPixelPair). Then each pair's nine bytes as SMPTE 268M C4.
	const char* const R12_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < pairs / 4 && g.y < frame_height) {
    uint blk[36];
    for (int t = 0; t < 9; t++) {
        uvec4 c = uvec4(round(texelFetch(raw, ivec2(g.x * 9 + t, g.y), 0) * 255.0));
        blk[4 * t] = c.x;
        blk[4 * t + 1] = c.y;
        blk[4 * t + 2] = c.z;
        blk[4 * t + 3] = c.w;
    }
    for (int j = 0; j < 4; j++) {
        uint b[9];
        if (big_endian != 0) {
            for (int k = 0; k < 9; k++)
                b[k] = blk[(9 * j + k) ^ 3];
        } else {
            for (int k = 0; k < 9; k++)
                b[k] = blk[9 * j + k];
        }
        uvec3 p0 = uvec3(b[0] | ((b[1] & 15u) << 8), (b[1] >> 4) | (b[2] << 4), b[3] | ((b[4] & 15u) << 8));
        uvec3 p1 = uvec3((b[4] >> 4) | (b[5] << 4), b[6] | ((b[7] & 15u) << 8), (b[7] >> 4) | (b[8] << 4));
        for (int k = 0; k < 2; k++) {
            uvec3 p = k == 0 ? p0 : p1;
            ivec2 at = ivec2(8 * g.x + 2 * j + k, g.y);
    #ifndef LUMA_ONLY
            imageStore(dst_rgb, at, vec4(ru_u16(p.x << 4), ru_u16(p.y << 4), ru_u16(p.z << 4), 1.0));
#endif
            // The matcher's luma: the CPU formatter's 12-to-10 scale and Q16
            // full-range Y (10-bit code << 6).
            ivec3 t = ivec3((p * 1023u + 2047u) / 4095u);
            int y10 = ru_clamp10((cy.x * t.x + cy.y * t.y + cy.z * t.z + 32768) >> 16);
            imageStore(dst_luma, at, vec4(ru_u16(uint(y10) << 6), 0.0, 0.0, 1.0));
        }
    }
}
)GLSL";

	// R10b / R10l: one invocation per pixel ([R10 G10 B10 00]; big_endian 1 =
	// R10b), levels 64-940 as libplacebo's limited 10-bit: the value << 6.
	const char* const R10_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uint b0 = ru_byte(4 * g.x, g.y), b1 = ru_byte(4 * g.x + 1, g.y), b2 = ru_byte(4 * g.x + 2, g.y), b3 = ru_byte(4 * g.x + 3, g.y);
    uint w = big_endian != 0 ? (b0 << 24) | (b1 << 16) | (b2 << 8) | b3 : (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
    ivec3 v = ivec3(int((w >> 22) & 1023u), int((w >> 12) & 1023u), int((w >> 2) & 1023u));
#ifndef LUMA_ONLY
    imageStore(dst_rgb, g, vec4(ru_u16(uint(v.x) << 6), ru_u16(uint(v.y) << 6), ru_u16(uint(v.z) << 6), 1.0));
#endif
    ivec3 o = v - 64;
    int y10 = ru_clamp10(64 + ru_round_q20(cy.x * o.x + cy.y * o.y + cy.z * o.z));
    imageStore(dst_luma, g, vec4(ru_u16(uint(y10) << 6), 0.0, 0.0, 1.0));
}
)GLSL";

	// ARGB / BGRA (big_endian 1 = ARGB's [A R G B] bytes, 0 = BGRA's
	// [B G R A]): the 8-bit value << 8; the luma as CARGBtoP010's (8 to 10
	// bits, both endpoints exact, then the Q16 full-range Y).
	const char* const RGB8_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uint b0 = ru_byte(4 * g.x, g.y), b1 = ru_byte(4 * g.x + 1, g.y), b2 = ru_byte(4 * g.x + 2, g.y), b3 = ru_byte(4 * g.x + 3, g.y);
    uvec3 v = big_endian != 0 ? uvec3(b1, b2, b3) : uvec3(b2, b1, b0);
#ifndef LUMA_ONLY
    imageStore(dst_rgb, g, vec4(ru_u16(v.x << 8), ru_u16(v.y << 8), ru_u16(v.z << 8), 1.0));
#endif
    ivec3 t = ivec3((v * 1023u + 127u) / 255u);
    int y10 = ru_clamp10((cy.x * t.x + cy.y * t.y + cy.z * t.z + 32768) >> 16);
    imageStore(dst_luma, g, vec4(ru_u16(uint(y10) << 6), 0.0, 0.0, 1.0));
}
)GLSL";

	// R210: one invocation per pixel ([00 R10 G10 B10], big-endian word).
	const char* const R210_BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < frame_width && g.y < frame_height) {
    uint w = (ru_byte(4 * g.x, g.y) << 24) | (ru_byte(4 * g.x + 1, g.y) << 16) |
        (ru_byte(4 * g.x + 2, g.y) << 8) | ru_byte(4 * g.x + 3, g.y);
    ivec3 v = ivec3(int((w >> 20) & 1023u), int((w >> 10) & 1023u), int(w & 1023u));
    // 64-960 to 16-bit limited 4096-60160: 4096 + (v - 64) x 438/7, rounded
    // (never a tie: n/7 has no half), below-black and above-white kept.
    ivec3 n = 4096 * 7 + (v - 64) * 438;
    uvec3 q = uvec3((n + 3) / 7);
#ifndef LUMA_ONLY
    imageStore(dst_rgb, g, vec4(ru_u16(q.x), ru_u16(q.y), ru_u16(q.z), 1.0));
#endif
    // The matcher's luma: the CPU formatter's Q20 limited matrix.
    ivec3 o = v - 64;
    int y10 = ru_clamp10(64 + ru_round_q20(cy.x * o.x + cy.y * o.y + cy.z * o.z));
    imageStore(dst_luma, g, vec4(ru_u16(uint(y10) << 6), 0.0, 0.0, 1.0));
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

GpuRgbUnpack::GpuRgbUnpack(pl_log log, pl_gpu gpuIn)
	: gpu(gpuIn)
{
	dispatch = gpu ? pl_dispatch_create(log, gpu) : nullptr;
}

GpuRgbUnpack::~GpuRgbUnpack()
{
	if (gpu)
		pl_tex_destroy(gpu, &raw);
	pl_dispatch_destroy(&dispatch);
}

bool GpuRgbUnpack::Supports(VideoFrameEncoding encoding)
{
	return encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L ||
		encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b ||
		encoding == VideoFrameEncoding::R10l || Is8Bit(encoding);
}

bool GpuRgbUnpack::Unpack(VideoFrameEncoding encoding, bool bt2020, const void* data, size_t rowBytes, int width, int height,
	pl_tex& rgb, pl_tex& luma, bool lumaOnly)
{
	error.clear();
	if (!gpu || !dispatch) { error = "no GPU"; return false; }
	if (!Supports(encoding)) { error = "unsupported encoding"; return false; }
	const bool r12 = encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L;
	const bool r10 = encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l;
	const bool rgb8 = Is8Bit(encoding);
	if (!data || width <= 0 || height <= 0 || (width & 1) || (r12 && width % 8 != 0) || rowBytes % 4 != 0)
	{
		error = "bad frame geometry";
		return false;
	}
	const size_t needed = r12 ? static_cast<size_t>(width / 2) * 9 : static_cast<size_t>(width) * 4;
	if (rowBytes < needed) { error = "row shorter than the frame"; return false; }
	pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
	const enum pl_fmt_caps planeCaps = static_cast<enum pl_fmt_caps>(
		PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
	pl_fmt rgba16 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, planeCaps);
	pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, planeCaps);
	if (!rgba8 || !rgba16 || !r16) { error = "no rgba8 / storable rgba16 / r16 format"; return false; }
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
	auto plane = [this](pl_tex* tex, pl_fmt fmt, int w, int h) -> bool
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
	if ((!lumaOnly && !plane(&rgb, rgba16, width, height)) || !plane(&luma, r16, width, height))
	{
		error = "cannot create the output planes";
		return false;
	}
	// The CPU formatter's luma coefficients (CDeckLinkRGBToP010VideoFrameFormatter.Common.h):
	// Q16 full range for R12, Q20 limited (r210's 64-960 span) for R210.
	int cy[3];
	if (r12 || rgb8)
	{
		const int bt709[3] = { 13933, 46871, 4732 }, rec2020[3] = { 17218, 44444, 3886 };
		for (int k = 0; k < 3; ++k) cy[k] = bt2020 ? rec2020[k] : bt709[k];
	}
	else if (r10)
	{
		// BT709_R10 / BT2020_R10 (64-940 input).
		const int bt709[3] = { 222927, 749942, 75707 }, rec2020[3] = { 275461, 710935, 62181 };
		for (int k = 0; k < 3; ++k) cy[k] = bt2020 ? rec2020[k] : bt709[k];
	}
	else
	{
		const int bt709[3] = { 217951, 733202, 74017 }, rec2020[3] = { 269312, 695065, 60793 };
		for (int k = 0; k < 3; ++k) cy[k] = bt2020 ? rec2020[k] : bt709[k];
	}
	const int pairs = width / 2;
	const int bigEndian = encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R10b ||
		encoding == VideoFrameEncoding::ARGB_8BIT ? 1 : 0;
	const pl_shader_desc descriptors[3] = { Sampled("raw", raw), Storage("dst_luma", luma), Storage("dst_rgb", rgb) };
	pl_shader_var variables[5] = {
		{ pl_var_int("pairs"), &pairs, false },
		{ pl_var_int("frame_width"), &width, false },
		{ pl_var_int("frame_height"), &height, false },
		{ pl_var_int("big_endian"), &bigEndian, false },
		{ pl_var_ivec3("cy"), cy, false } };
	pl_custom_shader custom{};
	custom.description = r12 ? "VideoProcessor capture: R12 to RGB (GPU)" : r10 ? "VideoProcessor capture: R10 to RGB (GPU)" :
		rgb8 ? "VideoProcessor capture: 8-bit RGB (GPU)" : "VideoProcessor capture: r210 to RGB (GPU)";
	static const std::string lumaOnlyHeader = std::string("#define LUMA_ONLY 1\n") + RGB_UNPACK_HEADER;
	custom.header = lumaOnly ? lumaOnlyHeader.c_str() : RGB_UNPACK_HEADER;
	custom.body = r12 ? R12_BODY : r10 ? R10_BODY : rgb8 ? RGB8_BODY : R210_BODY;
	custom.input = PL_SHADER_SIG_NONE;
	custom.output = PL_SHADER_SIG_NONE;
	custom.descriptors = descriptors;
	custom.num_descriptors = lumaOnly ? 2 : 3;
	custom.variables = variables;
	custom.num_variables = 5;
	custom.compute = true;
	custom.compute_group_size[0] = 16;
	custom.compute_group_size[1] = 16;
	pl_shader sh = pl_dispatch_begin(dispatch);
	if (!pl_shader_custom(sh, &custom))
	{
		pl_dispatch_abort(dispatch, &sh);
		error = "RGB unpack shader rejected";
		return false;
	}
	const int columns = r12 ? pairs / 4 : width;
	pl_dispatch_compute_params compute{};
	compute.shader = &sh;
	compute.dispatch_size[0] = (columns + 15) / 16;
	compute.dispatch_size[1] = (height + 15) / 16;
	compute.dispatch_size[2] = 1;
	compute.timer = timer;
	if (!pl_dispatch_compute(dispatch, &compute))
	{
		error = "RGB unpack dispatch failed";
		return false;
	}
	return true;
}
