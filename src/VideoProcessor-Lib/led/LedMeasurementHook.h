#pragma once

#include "LedMeasurement.h"

#include <libplacebo/gpu.h>
#include <libplacebo/shaders/custom.h>

#include <deque>
#include <vector>

// Layer 1 of the LED drive module, GPU side: a native libplacebo hook at
// PL_HOOK_PRE_OUTPUT (after tone mapping, gamut mapping and the normalized
// target LUT; before the limited-range encode, the compensation hook's
// successors and the OUTPUT-stage sigma-delta). A GLSL user shader cannot
// hand results back to the host, so this is C++: one compute pass writes the
// block statistics into a storage buffer, read back a few frames later
// without ever waiting on the GPU. It never modifies the picture.
namespace LedDrive
{
	class MeasurementHook
	{
	public:
		static constexpr int RING = 4;
		static constexpr int MAX_BLOCKS_X = 320;
		static constexpr int MAX_BLOCKS_Y = 180;

		MeasurementHook();
		~MeasurementHook();
		MeasurementHook(const MeasurementHook&) = delete;
		MeasurementHook& operator=(const MeasurementHook&) = delete;

		const pl_hook* Hook() const { return &hook; }
		// Before rendering: which part of the picture to measure, the R, G, B
		// weights for the APL, and the gamma boost exponent (the statistics
		// describe the boosted picture, as the script's screenshots did), and
		// the frame's source sequence, handed back with its measurement.
		void SetFrame(const MeasurementRect& rect, const double weights[3], double boost = 1.0,
			uint64_t sequence = 0);
		// After rendering: finished measurements in frame order, and their
		// sequences when asked. wait > 0 blocks up to that many milliseconds
		// for the oldest (tests only).
		int Collect(std::vector<RawMeasurement>& out, int waitMs = 0,
			std::vector<uint64_t>* sequences = nullptr);
		// Destroys the GPU buffers; call before the GPU is destroyed.
		void Release();
		int Dropped() const { return dropped; }
		int Measured() const { return measured; }

	private:
		static pl_hook_res Run(void* priv, const struct pl_hook_params* params);
		pl_hook_res Measure(const struct pl_hook_params* params);

		pl_hook hook{};
		pl_gpu gpu = nullptr;
		pl_buf ring[RING] = {};
		bool busy[RING] = {};
		uint64_t slotSequence[RING] = {};
		uint64_t sequence = 0;
		std::deque<int> submitted;
		int next = 0;
		MeasurementRect rect;
		float weights[3] = { 0.114f, 0.587f, 0.299f };
		float boost = 1.0f;
		int dropped = 0;
		int measured = 0;
	};
}
