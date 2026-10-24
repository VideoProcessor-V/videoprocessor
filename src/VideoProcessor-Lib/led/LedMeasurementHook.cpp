#include <pch.h>
#include "LedMeasurementHook.h"

#include "../DebugLog.h"

#include <libplacebo/dispatch.h>

#include <d3d11.h>
#include <libplacebo/d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

namespace LedDrive
{
	namespace
	{
		// "VPLEDMSR"
		constexpr uint64_t HOOK_SIGNATURE = 0x56504C45444D5352ull;

		// Hunt 2 (owner 2026-10-17, led_picture_only): up to MAX_MASK_RECTS
		// rectangles of the hooked texture (pixels, right/bottom exclusive)
		// left out of every statistic; an unused one is all zero.
		const char* const MASK_HEADER = R"GLSL(
bool in_rect(vec2 p, vec4 m)
{
    return p.x >= m.x && p.x < m.z && p.y >= m.y && p.y < m.w;
}
bool masked(vec2 p)
{
    return in_rect(p, mask0) || in_rect(p, mask1) || in_rect(p, mask2) ||
           in_rect(p, mask3) || in_rect(p, mask4) || in_rect(p, mask5);
}
vec2 block_centre(ivec2 b)
{
    return vec2(ivec2(rect_px.xy + vec2(b) * block_step)) + vec2(1.5);
}
)GLSL";

		// R2 (GRAIN): the 3x3 mean of one block, and the grain amount at it.
		const char* const GRAIN_HEADER = R"GLSL(
float block_mean(ivec2 b, ivec2 lo, ivec2 hi)
{
    ivec2 bb = clamp(b, ivec2(0), ivec2(blocks) - ivec2(1));
    ivec2 origin = ivec2(rect_px.xy + vec2(bb) * block_step);
    float acc = 0.0;
    for (int dy = 0; dy < 3; dy++)
        for (int dx = 0; dx < 3; dx++) {
            ivec2 p = clamp(origin + ivec2(dx, dy), lo, hi);
            acc += dot(pow(clamp(texelFetch(src, p, 0).rgb, vec3(0.0), vec3(1.0)), vec3(boost)), weights);
        }
    return acc / 9.0;
}
float grain_at(vec2 picture_px)
{
    vec2 s = src_crop.xy + (picture_px - pic_rect.xy) * src_crop.zw / pic_rect.zw;
    ivec2 gs = textureSize(grain_map, 0);
    vec4 m = texelFetch(grain_map, clamp(ivec2(floor(s / 16.0)), ivec2(0), gs - ivec2(1)), 0);
    return m.g > 0.5 ? clamp((1.0 - m.r) / max(1.0 - grain_floor, 1e-3), 0.0, 1.0) : 0.0;
}
)GLSL";

		const char* const BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < int(blocks.x) && b.y < int(blocks.y) && !masked(block_centre(b))) {
    ivec2 lo = ivec2(rect_px.xy);
    ivec2 hi = ivec2(rect_px.zw) - ivec2(1);
    ivec2 origin = ivec2(rect_px.xy + vec2(b) * block_step);
    float acc = 0.0;
    for (int dy = 0; dy < 3; dy++)
        for (int dx = 0; dx < 3; dx++) {
            ivec2 p = clamp(origin + ivec2(dx, dy), lo, hi);
            acc += dot(pow(clamp(texelFetch(src, p, 0).rgb, vec3(0.0), vec3(1.0)), vec3(boost)), weights);
        }
    float code = 16.0 + 219.0 * clamp(acc / 9.0, 0.0, 1.0);
    uint scaled = uint(code * 16.0 + 0.5);
    float hist_code = code;
#ifdef GRAIN
    // Grain averages out over the 3x3 blocks around; a real highlight spans
    // them. Weighted by the grain amount, not darkened by it.
    float g = grain_at(vec2(origin) + vec2(1.5));
    if (g > 0.0) {
        // Masked neighbours (a subtitle) are left out (hunt 2).
        float wide = 0.0, around = 0.0;
        for (int ny = -1; ny <= 1; ny++)
            for (int nx = -1; nx <= 1; nx++) {
                ivec2 nb = clamp(b + ivec2(nx, ny), ivec2(0), ivec2(blocks) - ivec2(1));
                if (!masked(block_centre(nb))) {
                    wide += block_mean(nb, lo, hi);
                    around += 1.0;
                }
            }
        float wide_mean = around > 0.0 ? wide / around : acc / 9.0;
        hist_code = 16.0 + 219.0 * clamp(mix(acc / 9.0, wide_mean, g), 0.0, 1.0);
    }
#endif
    atomicAdd(hist[min(int(hist_code), 255)], 1u);
    atomicAdd(total_sum, scaled);
    atomicAdd(total_blocks, 1u);
#ifndef FULL_PEAK
    atomicMax(max_block, scaled);
#endif
    int cell = min(int(float(b.y) * 18.0 / blocks.y), 17) * 32 +
               min(int(float(b.x) * 32.0 / blocks.x), 31);
    atomicAdd(cell_sum[cell], scaled);
    atomicAdd(cell_count[cell], 1u);
}
)GLSL";

		// Hunt 2 (owner 2026-10-17, led_peak_full_coverage): one invocation
		// per box of the AreaPeakGrid tiling rect_px; the box's area mean
		// (fractional pixels at its edges weighted by their share, as cv2
		// INTER_AREA does) of the unmasked pixels, the brightest into
		// max_block. A box left with under half its area is skipped.
		const char* const PEAK_BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < int(peak_boxes.x) && b.y < int(peak_boxes.y)) {
    vec2 lo = rect_px.xy + vec2(b) * peak_box;
    vec2 hi = min(lo + peak_box, rect_px.zw);
    ivec2 p0 = ivec2(floor(lo));
    ivec2 p1 = ivec2(ceil(hi));
    ivec2 last = ivec2(rect_px.zw) - ivec2(1);
    float sum = 0.0;
    float area = 0.0;
    for (int y = p0.y; y < p1.y; y++) {
        float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));
        if (wy <= 0.0)
            continue;
        for (int x = p0.x; x < p1.x; x++) {
            float wx = min(hi.x, float(x + 1)) - max(lo.x, float(x));
            if (wx <= 0.0 || masked(vec2(float(x), float(y)) + vec2(0.5)))
                continue;
            ivec2 p = clamp(ivec2(x, y), ivec2(rect_px.xy), last);
            float v = dot(pow(clamp(texelFetch(src, p, 0).rgb, vec3(0.0), vec3(1.0)), vec3(boost)), weights);
            sum += wx * wy * v;
            area += wx * wy;
        }
    }
    if (area > 0.0 && area >= 0.5 * peak_box.x * peak_box.y - 1e-4) {
        float code = 16.0 + 219.0 * clamp(sum / area, 0.0, 1.0);
        uint scaled = uint(code * 16.0 + 0.5);
        if (scaled > max_block)
            atomicMax(max_block, scaled);
    }
}
)GLSL";

		pl_var UintArray(const char* name, int count)
		{
			pl_var var = pl_var_uint(name);
			var.dim_a = count;
			return var;
		}
	}

	namespace
	{
		constexpr int READBACK_WORDS = static_cast<int>((sizeof(RawMeasurement) + 3) / 4);
		const char* const COPY_BODY = R"GLSL(
int i = int(gl_GlobalInvocationID.x);
if (i < READBACK_WORDS) {
    // libplacebo declares storage images as float image2D only, so each word goes out as its
    // four bytes in an RGBA8 UNORM texel (k / 255 stores exactly k): the texel's bytes in memory
    // are the word, little-endian, as the CPU reads it back.
    uint w = words[i];
    imageStore(readback_img, ivec2(i, 0),
               vec4(float(w & 0xFFu), float((w >> 8) & 0xFFu), float((w >> 16) & 0xFFu), float(w >> 24)) / 255.0);
}
)GLSL";
	}

	MeasurementHook::MeasurementHook()
	{
		hook.stages = PL_HOOK_PRE_OUTPUT;
		hook.input = PL_HOOK_SIG_TEX;
		hook.priv = this;
		hook.hook = &MeasurementHook::Run;
		hook.signature = HOOK_SIGNATURE;
	}

	MeasurementHook::~MeasurementHook()
	{
		Release();
	}

	void MeasurementHook::SetFrame(const MeasurementRect& rectIn, const double weightsIn[3],
		double boostIn, uint64_t sequenceIn)
	{
		rect = rectIn;
		sequence = sequenceIn;
		boost = static_cast<float>(boostIn);
		for (int i = 0; i < 3; ++i) weights[i] = static_cast<float>(weightsIn[i]);
	}

	pl_hook_res MeasurementHook::Run(void* priv, const struct pl_hook_params* params)
	{
		return static_cast<MeasurementHook*>(priv)->Measure(params);
	}

	pl_hook_res MeasurementHook::Measure(const struct pl_hook_params* params)
	{
		pl_hook_res none{};
		none.output = PL_HOOK_SIG_NONE;
		if (!params->tex) return none;
		if (gpu && gpu != params->gpu) Release();
		gpu = params->gpu;
		if (!nativeTried)
			EnsureNativeReadback();
		const int slot = schedule.Claim(++frames);
		if (slot < 0)
		{
			// Every slot still holds an unread result: skip this frame
			// rather than wait for the GPU.
			++dropped;
			return none;
		}
		// A pass that does not get queued gives its slot back (every early return below).
		struct SlotGuard
		{
			ReadbackRing& owner;
			int slot;
			bool queued = false;
			~SlotGuard() { if (!queued) owner.Release(slot); }
		} guard{ schedule, slot };
		if (!ring[slot])
		{
			pl_buf_params bufParams{};
			bufParams.size = sizeof(RawMeasurement);
			bufParams.storable = true;
			bufParams.host_readable = true;
			bufParams.host_writable = true;
			ring[slot] = pl_buf_create(gpu, &bufParams);
			if (!ring[slot])
			{
				DebugLog::Log("Dynamic Drive: cannot create the measurement buffer");
				pl_hook_res failed{};
				failed.failed = true;
				return failed;
			}
		}
		static const RawMeasurement zero{};
		pl_buf_write(gpu, ring[slot], 0, &zero, sizeof(zero));

		const pl_rect2df& r = params->rect;
		const float rw = r.x1 - r.x0, rh = r.y1 - r.y0;
		const float rectPx[4] = {
			static_cast<float>(std::floor(r.x0 + rect.x0 * rw)),
			static_cast<float>(std::floor(r.y0 + rect.y0 * rh)),
			static_cast<float>(std::ceil(r.x0 + rect.x1 * rw)),
			static_cast<float>(std::ceil(r.y0 + rect.y1 * rh)) };
		const float width = rectPx[2] - rectPx[0], height = rectPx[3] - rectPx[1];
		if (width < 3.0f || height < 3.0f) return none;
		const float blocks[2] = {
			std::min<float>(MAX_BLOCKS_X, std::floor(width / 3.0f)),
			std::min<float>(MAX_BLOCKS_Y, std::floor(height / 3.0f)) };
		const float step[2] = { width / blocks[0], height / blocks[1] };

		pl_buffer_var bufferVars[6] = {
			{ UintArray("hist", HISTOGRAM_BINS), {} },
			{ UintArray("cell_sum", GRID_W * GRID_H), {} },
			{ UintArray("cell_count", GRID_W * GRID_H), {} },
			{ pl_var_uint("total_sum"), {} },
			{ pl_var_uint("total_blocks"), {} },
			{ pl_var_uint("max_block"), {} } };
		size_t offset = 0;
		for (pl_buffer_var& var : bufferVars)
		{
			var.layout = pl_std430_layout(offset, &var.var);
			offset = var.layout.offset + var.layout.size;
		}
		if (offset != sizeof(RawMeasurement))
		{
			DebugLog::Log("Dynamic Drive: measurement buffer layout mismatch (%zu bytes)", offset);
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}

		pl_shader_desc descriptors[3] = {};
		descriptors[0].desc.name = "src";
		descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
		descriptors[0].binding.object = params->tex;
		descriptors[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		descriptors[1].desc.name = "stats";
		descriptors[1].desc.type = PL_DESC_BUF_STORAGE;
		descriptors[1].desc.access = PL_DESC_ACCESS_READWRITE;
		descriptors[1].binding.object = ring[slot];
		descriptors[1].buffer_vars = bufferVars;
		descriptors[1].num_buffer_vars = 6;
		descriptors[1].memory = PL_MEMORY_COHERENT;
		// R2: the grain map, when handed over and the source crop is known.
		const bool useGrain = grain && crop.x1 > crop.x0 && crop.y1 > crop.y0 && rw > 0.0f && rh > 0.0f;
		if (useGrain)
		{
			descriptors[2].desc.name = "grain_map";
			descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
			descriptors[2].binding.object = grain;
			descriptors[2].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		}
		const float srcCrop[4] = { crop.x0, crop.y0, crop.x1 - crop.x0, crop.y1 - crop.y0 };
		const float picRect[4] = { r.x0, r.y0, rw, rh };
		// Hunt 2: the masks in hooked-texture pixels (all zero = unused).
		static_assert(MAX_MASK_RECTS == 6, "MASK_HEADER names six masks");
		float maskPx[MAX_MASK_RECTS][4] = {};
		for (int i = 0; i < maskCount; ++i)
		{
			maskPx[i][0] = r.x0 + static_cast<float>(masks[i].x0) * rw;
			maskPx[i][1] = r.y0 + static_cast<float>(masks[i].y0) * rh;
			maskPx[i][2] = r.x0 + static_cast<float>(masks[i].x1) * rw;
			maskPx[i][3] = r.y0 + static_cast<float>(masks[i].y1) * rh;
		}
		// Hunt 2: the full-coverage peak's boxes, the script's share of the screen.
		const PeakGrid peakGrid = AreaPeakGrid(width, height,
			screenSize[0] > 0.0f ? screenSize[0] : static_cast<float>(params->tex->params.w),
			screenSize[1] > 0.0f ? screenSize[1] : static_cast<float>(params->tex->params.h));
		const float peakBoxes[2] = { static_cast<float>(peakGrid.boxesX), static_cast<float>(peakGrid.boxesY) };
		const float peakBox[2] = { static_cast<float>(peakGrid.boxW), static_cast<float>(peakGrid.boxH) };

		const pl_shader_var variables[14] = {
			{ pl_var_vec4("rect_px"), rectPx, true },
			{ pl_var_vec2("blocks"), blocks, true },
			{ pl_var_vec2("block_step"), step, true },
			{ pl_var_vec3("weights"), weights, true },
			{ pl_var_float("boost"), &boost, true },
			{ pl_var_vec4("mask0"), maskPx[0], true },
			{ pl_var_vec4("mask1"), maskPx[1], true },
			{ pl_var_vec4("mask2"), maskPx[2], true },
			{ pl_var_vec4("mask3"), maskPx[3], true },
			{ pl_var_vec4("mask4"), maskPx[4], true },
			{ pl_var_vec4("mask5"), maskPx[5], true },
			{ pl_var_vec4("src_crop"), srcCrop, true },
			{ pl_var_vec4("pic_rect"), picRect, true },
			{ pl_var_float("grain_floor"), &grainFloorValue, true } };

		const bool fullPeak = peakFullCoverage;
		static const std::string headers[4] = {
			std::string(MASK_HEADER),
			std::string("#define GRAIN 1\n") + MASK_HEADER + GRAIN_HEADER,
			std::string("#define FULL_PEAK 1\n") + MASK_HEADER,
			std::string("#define FULL_PEAK 1\n#define GRAIN 1\n") + MASK_HEADER + GRAIN_HEADER };
		pl_custom_shader custom{};
		custom.description = useGrain ? "VideoProcessor Dynamic Drive measurement (grain-suppressed highlights)" :
			"VideoProcessor Dynamic Drive measurement";
		custom.header = headers[(fullPeak ? 2 : 0) + (useGrain ? 1 : 0)].c_str();
		custom.body = BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = useGrain ? 3 : 2;
		custom.variables = variables;
		custom.num_variables = useGrain ? 14 : 11;
		custom.compute = true;
		custom.compute_group_size[0] = 16;
		custom.compute_group_size[1] = 16;

		pl_shader sh = pl_dispatch_begin(params->dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (static_cast<int>(blocks[0]) + 15) / 16;
		compute.dispatch_size[1] = (static_cast<int>(blocks[1]) + 15) / 16;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(params->dispatch, &compute))
		{
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}
		if (fullPeak)
		{
			// Hunt 2: every pixel of the rectangle counts towards the peak.
			const pl_shader_var peakVariables[12] = {
				{ pl_var_vec4("rect_px"), rectPx, true },
				{ pl_var_vec2("block_step"), step, true },
				{ pl_var_vec3("weights"), weights, true },
				{ pl_var_float("boost"), &boost, true },
				{ pl_var_vec4("mask0"), maskPx[0], true },
				{ pl_var_vec4("mask1"), maskPx[1], true },
				{ pl_var_vec4("mask2"), maskPx[2], true },
				{ pl_var_vec4("mask3"), maskPx[3], true },
				{ pl_var_vec4("mask4"), maskPx[4], true },
				{ pl_var_vec4("mask5"), maskPx[5], true },
				{ pl_var_vec2("peak_boxes"), peakBoxes, true },
				{ pl_var_vec2("peak_box"), peakBox, true } };
			pl_custom_shader peak{};
			peak.description = "VideoProcessor Dynamic Drive peak (full coverage)";
			peak.header = MASK_HEADER;
			peak.body = PEAK_BODY;
			peak.input = PL_SHADER_SIG_NONE;
			peak.output = PL_SHADER_SIG_NONE;
			peak.descriptors = descriptors;
			peak.num_descriptors = 2;
			peak.variables = peakVariables;
			peak.num_variables = 12;
			peak.compute = true;
			peak.compute_group_size[0] = 16;
			peak.compute_group_size[1] = 16;
			pl_shader peakShader = pl_dispatch_begin(params->dispatch);
			if (!pl_shader_custom(peakShader, &peak))
			{
				pl_dispatch_abort(params->dispatch, &peakShader);
				pl_hook_res failed{};
				failed.failed = true;
				return failed;
			}
			pl_dispatch_compute_params peakCompute{};
			peakCompute.shader = &peakShader;
			peakCompute.dispatch_size[0] = (peakGrid.boxesX + 15) / 16;
			peakCompute.dispatch_size[1] = (peakGrid.boxesY + 15) / 16;
			peakCompute.dispatch_size[2] = 1;
			if (!pl_dispatch_compute(params->dispatch, &peakCompute))
			{
				pl_hook_res failed{};
				failed.failed = true;
				return failed;
			}
		}
		if (nativeReadback && !QueueNativeCopy(params, slot))
		{
			// The non-blocking path failed: back to libplacebo's readback for the session (visible
			// through LateNote only if results then run late).
			DebugLog::Log("Dynamic Drive: the non-blocking measurement readback failed; libplacebo's readback for the session");
			ReleaseNative();
			nativeTried = true;
		}
		slotSequence[slot] = sequence;
		guard.queued = true;
		++measured;
		return none;
	}

	bool MeasurementHook::EnsureNativeReadback()
	{
		nativeTried = true;
		const pl_d3d11 d3d = gpu ? pl_d3d11_get(gpu) : nullptr;
		if (!d3d || !d3d->device)
			return false;   // not D3D11: pl_buf_poll / pl_buf_read do not block there
		d3d->device->GetImmediateContext(&immediate);
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(READBACK_WORDS);
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // one 32-bit word per texel (see COPY_BODY)
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		D3D11_TEXTURE2D_DESC staging = desc;
		staging.Usage = D3D11_USAGE_STAGING;
		staging.BindFlags = 0;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		D3D11_QUERY_DESC query{};
		query.Query = D3D11_QUERY_EVENT;
		bool ok = immediate != nullptr;
		for (int i = 0; ok && i < RING; ++i)
		{
			ok = SUCCEEDED(d3d->device->CreateTexture2D(&desc, nullptr, &readbackTex[i])) &&
				SUCCEEDED(d3d->device->CreateTexture2D(&staging, nullptr, &stagingTex[i])) &&
				SUCCEEDED(d3d->device->CreateQuery(&query, &copyDone[i]));
			if (ok)
			{
				struct pl_d3d11_wrap_params wrap{};
				wrap.tex = readbackTex[i];
				readbackWrapped[i] = pl_d3d11_wrap(gpu, &wrap);
				ok = readbackWrapped[i] && readbackWrapped[i]->params.storable;
			}
		}
		if (!ok)
		{
			DebugLog::Log("Dynamic Drive: the non-blocking measurement readback is unavailable; libplacebo's readback");
			ReleaseNative();
			nativeTried = true;
			return false;
		}
		nativeReadback = true;
		DebugLog::Log("Dynamic Drive: measurement read back without waiting (D3D11 staging ring of %d)", RING);
		return true;
	}

	bool MeasurementHook::QueueNativeCopy(const struct pl_hook_params* params, int slot)
	{
		pl_buffer_var words[1] = { { UintArray("words", READBACK_WORDS), {} } };
		words[0].layout = pl_std430_layout(0, &words[0].var);
		pl_shader_desc descriptors[2] = {};
		descriptors[0].desc.name = "stats_in";
		descriptors[0].desc.type = PL_DESC_BUF_STORAGE;
		descriptors[0].desc.access = PL_DESC_ACCESS_READONLY;
		descriptors[0].binding.object = ring[slot];
		descriptors[0].buffer_vars = words;
		descriptors[0].num_buffer_vars = 1;
		descriptors[0].memory = PL_MEMORY_COHERENT;
		descriptors[1].desc.name = "readback_img";
		descriptors[1].desc.type = PL_DESC_STORAGE_IMG;
		descriptors[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
		descriptors[1].binding.object = readbackWrapped[slot];
		static const std::string header = "#define READBACK_WORDS " + std::to_string(READBACK_WORDS) + "\n";
		pl_custom_shader copy{};
		copy.description = "VideoProcessor Dynamic Drive measurement readback copy";
		copy.header = header.c_str();
		copy.body = COPY_BODY;
		copy.input = PL_SHADER_SIG_NONE;
		copy.output = PL_SHADER_SIG_NONE;
		copy.descriptors = descriptors;
		copy.num_descriptors = 2;
		copy.compute = true;
		copy.compute_group_size[0] = 64;
		copy.compute_group_size[1] = 1;
		pl_shader sh = pl_dispatch_begin(params->dispatch);
		if (!pl_shader_custom(sh, &copy))
		{
			pl_dispatch_abort(params->dispatch, &sh);
			return false;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = (READBACK_WORDS + 63) / 64;
		compute.dispatch_size[1] = 1;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(params->dispatch, &compute))
			return false;
		// libplacebo issues its D3D11 work on the immediate context as it goes, so these follow
		// the copy pass on the GPU timeline; the query marks when the staging copy is done.
		immediate->CopyResource(stagingTex[slot], readbackTex[slot]);
		immediate->End(copyDone[slot]);
		return true;
	}

	bool MeasurementHook::NativeReady(int slot)
	{
		return immediate && copyDone[slot] &&
			immediate->GetData(copyDone[slot], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
	}

	bool MeasurementHook::NativeRead(int slot, RawMeasurement& raw)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(immediate->Map(stagingTex[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			return false;   // DXGI_ERROR_WAS_STILL_DRAWING: try again at the next Collect
		std::memcpy(&raw, mapped.pData, sizeof(raw));
		immediate->Unmap(stagingTex[slot], 0);
		return true;
	}

	void MeasurementHook::ReleaseNative()
	{
		for (int i = 0; i < RING; ++i)
		{
			if (gpu && readbackWrapped[i]) pl_tex_destroy(gpu, &readbackWrapped[i]);
			readbackWrapped[i] = nullptr;
			if (readbackTex[i]) { readbackTex[i]->Release(); readbackTex[i] = nullptr; }
			if (stagingTex[i]) { stagingTex[i]->Release(); stagingTex[i] = nullptr; }
			if (copyDone[i]) { copyDone[i]->Release(); copyDone[i] = nullptr; }
		}
		if (immediate) { immediate->Release(); immediate = nullptr; }
		nativeReadback = false;
		nativeTried = false;
	}

	int MeasurementHook::Collect(std::vector<RawMeasurement>& out, int waitMs,
		std::vector<uint64_t>* sequences)
	{
		int collected = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
		while (gpu && schedule.Pending() > 0)
		{
			// Never waits: a slot is read only when its copy is done (D3D11: the event query;
			// elsewhere libplacebo's poll), oldest first.
			const std::vector<int> slots = schedule.Collect(frames, [this](int slot)
			{
				return nativeReadback ? NativeReady(slot) : !pl_buf_poll(gpu, ring[slot], 0);
			});
			for (int slot : slots)
			{
				RawMeasurement raw{};
				const bool read = nativeReadback ? NativeRead(slot, raw) : pl_buf_read(gpu, ring[slot], 0, &raw, sizeof(raw));
				if (!read && nativeReadback)
					break;   // still drawing after all: the next Collect takes it
				if (read)
				{
					out.push_back(raw);
					if (sequences) sequences->push_back(slotSequence[slot]);
					++collected;
				}
				schedule.Release(slot);
			}
			if (schedule.Pending() == 0 || waitMs <= 0 || std::chrono::steady_clock::now() >= deadline)
				break;
			pl_gpu_flush(gpu);   // tests only (waitMs > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return collected;
	}

	void MeasurementHook::Release()
	{
		ReleaseNative();
		if (gpu)
			for (pl_buf& buf : ring)
				pl_buf_destroy(gpu, &buf);
		schedule.Reset();
		gpu = nullptr;
	}
}
