// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a: the vector engine behind the renderer. It owns the estimation-side state
// (VectorEngineState) and the logic that used to be member functions of the renderer's Impl:
//   P5a-3  the optical-flow producer's lifecycle and the look-ahead depth (PlanProducer);
//   P5a-4  the vector-demand decision, its statistics service and the invalidation requests
//          (UpdateVectorEngine, ServeStatistics, RequestInvalidate, ApplyInvalidate).
// The renderer holds one member of this type (`vectorEngine`) and calls it at the sites the code used
// to sit at. What the engine needs from the renderer comes in as plain values (ProducerInputs, the
// VectorDemand::Input) or through the small IVectorEngineHost interface the renderer implements with
// an adapter; the engine never sees the renderer's Impl.
// C++14: included by VideoProcessor-Lib code.
#pragma once

#include <VP-V/modules/motion/estimation/ProducerPlan.h>
#include <VP-V/modules/motion/estimation/VectorEngineState.h>

#include <libplacebo/gpu.h>

#include <atomic>
#include <cstddef>

namespace VpMotion
{
	class VectorEngine : public VectorEngineState
	{
	public:
		// What the producer plan reads from the renderer, as plain values. The two atomics are read
		// at the points the old inline block read them (low latency first, the queue depth after the
		// producer was acted on).
		struct ProducerInputs
		{
			const std::atomic<bool>* lowLatency = nullptr;
			const std::atomic<size_t>* queuedFutureFrames = nullptr;
			bool opticalFlow = true;            // the optical_flow setting
			int motionAheadFrames = 0;          // the motion_ahead_frames setting
			bool generalRateFixes = true;       // the general_rate_fixes setting
			bool dejudderPresent = false;       // the de-judder's matcher exists
			bool dejudderEstimating = false;    // its reason is Active / NoNextFrame / NextFrameDiffers
			double sourceHz = 0.0;              // the measured content rate (0 = unmeasured)
			pl_gpu gpu = nullptr;               // the device the engine needs; null = none
		};

		// Render thread, renderMutex held, once per rendered source frame: starts, keeps or stops the
		// optical-flow producer and publishes the look-ahead depth the render loop submits to
		// (motionAheadDepth / Backward / Configured), with the one "Motion ahead:" log line.
		void PlanProducer(const ProducerInputs& in);
	};
}
