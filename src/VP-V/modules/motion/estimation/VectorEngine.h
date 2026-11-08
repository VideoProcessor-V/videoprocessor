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

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/ProducerPlan.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/estimation/VectorEngineState.h>
#include <IntentInterpreterObserver.h>

#include <libplacebo/gpu.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace VpMotion
{
	// What the engine needs from the renderer besides plain values: the renderer's fallback registry, its stats
	// overlay, the source monitor's pan input and the intent interpreter's vector input. Implemented for the
	// renderer by VectorEngineImplHost (VectorEngineHost.h); tests use a fake.
	class IVectorEngineHost
	{
	public:
		virtual ~IVectorEngineHost() {}
		virtual void SyncFallback(const FallbackCatalog::Decision& decision) = 0;
		virtual void PublishStatus(bool force) = 0;
		virtual bool DitherVectorsSupported() const = 0;
		virtual void NoteSourceMonitorMotion(double sourcePixelsPerFrame, uint64_t sequence) = 0;
		virtual bool IntentActive() const = 0;
		virtual void NoteIntentVectors(const IntentInterpreter::VectorSample& sample) = 0;
	};

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

		// ---- P5a-4: the vector-demand decision and its statistics service ------------------------------------
		struct UpdateFacts
		{
			bool vectorDemandSetting = false;   // the vector_demand setting
			bool sigmaDeltaRanLast = false;     // the sigma-delta quantiser ran in the last frame's output pass
			bool ditherVectorsOwn = false;      // the dither_vectors_own setting
		};
		// Decides for this source frame (from the renderer's facts, VectorDemand::Input), then keeps the engine's state
		// visible: fallback entry, status line, Ctrl+Alt+D record and the logs (on change, once per mode, every 5 s
		// while on). Render thread.
		void UpdateVectorEngine(const VectorDemand::Input& input, const UpdateFacts& facts, IVectorEngineHost& host);

		struct ServeFacts
		{
			bool legacyConsumer = false;          // a de-judder / interpolator estimate takes this frame's pair
			bool lowResSearch = true;             // dejudder_low_res != "off"
			bool vectorDeliveryRuns = true;       // the motion.vector_delivery slot runs
			int pictureHeight = 0;                // the trusted picture's height (the raster's when none)
		};
		// Backward pairs (no frame ahead): once per new source frame the statistics consumers read the pair (n-1, n)
		// if the producer has it, else the frame-old (n-2, n-1); statistics only, never a field. Forward without a
		// render-time estimate: statistics consumers only. Render thread.
		void ServeStatistics(uint64_t sequence, uint64_t nextSequence, int width, int height, const ServeFacts& facts,
			IVectorEngineHost& host);

		// Whole-frame motion for the statistics consumers: a producer pair (hit) or none (miss). A miss run is visible
		// (never log-only): hysteresis, so a single missed pair does not flap the entry.
		void NoteGlobalPair(bool hit);
		// Once per mode: the first vectors the engine delivered and where they came from.
		void NoteVectorsDelivered(bool backward, const char* source);
		// Engine size the producer works at for a picture of this size, as the matcher would use it (halvings of the
		// search input), and the halvings themselves for the pixel scaling.
		static void SizeFor(bool lowResSearch, int width, int height, int& engineW, int& engineH, int& halvings);
		// S0 shadow step: the segment's answer against the host's own predicate; nothing is delivered from it.
		void MotionShadowCheck(bool deliveryRuns, uint64_t sequence, uint64_t nextSequence, bool hostLive);

		// ---- P5a-4 (item d): the apply point of the one vectors-valid state, split at the renderer's own consumers ----
		// Render thread, before an estimate: consumes the epoch requests made from any thread since the last look and
		// counts them. False = nothing changed (the caller returns). The caller then makes every consumer of older
		// vectors forget (they are the renderer's: matcher, ahead state, adaptive / deband / detail histories) and
		// calls LogInvalidation.
		bool TakeInvalidation(MotionValidity::Pending& pending);
		// The first 50 invalidations are all logged; then at most one line per 5 s, but a new cause is always logged.
		void LogInvalidation(const MotionValidity::Pending& pending);
	};
}
