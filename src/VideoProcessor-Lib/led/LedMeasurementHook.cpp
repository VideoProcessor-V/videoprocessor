#include <pch.h>
#include "LedMeasurementHook.h"

#include "../DebugLog.h"

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace LedDrive
{
	namespace
	{
		// "VPLEDMSR"
		constexpr uint64_t HOOK_SIGNATURE = 0x56504C45444D5352ull;

		const char* const BODY = R"GLSL(
ivec2 b = ivec2(gl_GlobalInvocationID.xy);
if (b.x < int(blocks.x) && b.y < int(blocks.y)) {
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
    atomicAdd(hist[min(int(code), 255)], 1u);
    atomicAdd(total_sum, scaled);
    atomicAdd(total_blocks, 1u);
    atomicMax(max_block, scaled);
    int cell = min(int(float(b.y) * 18.0 / blocks.y), 17) * 32 +
               min(int(float(b.x) * 32.0 / blocks.x), 31);
    atomicAdd(cell_sum[cell], scaled);
    atomicAdd(cell_count[cell], 1u);
}
)GLSL";

		pl_var UintArray(const char* name, int count)
		{
			pl_var var = pl_var_uint(name);
			var.dim_a = count;
			return var;
		}
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
		const int slot = next;
		if (busy[slot])
		{
			// The oldest readback has not been collected: skip this frame
			// rather than wait for the GPU.
			++dropped;
			return none;
		}
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
				DebugLog::Log("LED drive: cannot create the measurement buffer");
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
			DebugLog::Log("LED drive: measurement buffer layout mismatch (%zu bytes)", offset);
			pl_hook_res failed{};
			failed.failed = true;
			return failed;
		}

		pl_shader_desc descriptors[2] = {};
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

		const pl_shader_var variables[5] = {
			{ pl_var_vec4("rect_px"), rectPx, true },
			{ pl_var_vec2("blocks"), blocks, true },
			{ pl_var_vec2("block_step"), step, true },
			{ pl_var_vec3("weights"), weights, true },
			{ pl_var_float("boost"), &boost, true } };

		pl_custom_shader custom{};
		custom.description = "VideoProcessor LED drive measurement";
		custom.body = BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = 2;
		custom.variables = variables;
		custom.num_variables = 5;
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
		busy[slot] = true;
		slotSequence[slot] = sequence;
		submitted.push_back(slot);
		next = (next + 1) % RING;
		++measured;
		return none;
	}

	int MeasurementHook::Collect(std::vector<RawMeasurement>& out, int waitMs,
		std::vector<uint64_t>* sequences)
	{
		int collected = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
		while (gpu && !submitted.empty())
		{
			const int slot = submitted.front();
			if (pl_buf_poll(gpu, ring[slot], 0))
			{
				if (std::chrono::steady_clock::now() >= deadline) break;
				pl_gpu_flush(gpu);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			RawMeasurement raw{};
			if (pl_buf_read(gpu, ring[slot], 0, &raw, sizeof(raw)))
			{
				out.push_back(raw);
				if (sequences) sequences->push_back(slotSequence[slot]);
				++collected;
			}
			busy[slot] = false;
			submitted.pop_front();
		}
		return collected;
	}

	void MeasurementHook::Release()
	{
		if (gpu)
			for (pl_buf& buf : ring)
				pl_buf_destroy(gpu, &buf);
		for (bool& b : busy) b = false;
		submitted.clear();
		next = 0;
		gpu = nullptr;
	}
}
